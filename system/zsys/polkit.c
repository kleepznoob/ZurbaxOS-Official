/*
 * org.freedesktop.PolicyKit1: who may do what. GNOME's Settings, Software
 * and others ask before changing the system ("may this program set the
 * time?"), and the answer comes from the actions the layer's programs
 * describe in /usr/share/polkit-1/actions.
 *
 * Zurbax's rule is short: the account's owner is its administrator. What an
 * action allows the active session outright is allowed; what needs a
 * password asks for the account's own (through the session's
 * authentication agent, GNOME Shell's dialog), unless the account has none,
 * in which case there is nothing to ask. Root may do anything.
 *
 * The agent checks the password with the layer's polkit-agent-helper-1,
 * which on Zurbax is a small program of its own (system/polkit-helper) that
 * hands the password to org.zurbax.PolicyKit.Authenticate here.
 */
#include "zsys.h"

#define POLKIT "org.freedesktop.PolicyKit1"
#define AUTHORITY_PATH "/org/freedesktop/PolicyKit1/Authority"
#define AUTHORITY_IF "org.freedesktop.PolicyKit1.Authority"
#define AGENT_IF "org.freedesktop.PolicyKit1.AuthenticationAgent"
#define ZURBAX_IF "org.zurbax.PolicyKit"

enum { IMPLICIT_NO, IMPLICIT_AUTH_SELF, IMPLICIT_AUTH_ADMIN, IMPLICIT_AUTH_SELF_KEEP, IMPLICIT_AUTH_ADMIN_KEEP, IMPLICIT_YES };
enum { CHECK_ALLOW_INTERACTION = 1 };

/* ---- Actions ------------------------------------------------------------------------------- */

typedef struct {
	char *id, *description, *message, *icon, *vendor, *vendor_url;
	int any, inactive, active;
	char *imply;     /* org.freedesktop.policykit.imply: actions this one also grants */
} Action;

static VEC(Action) actions;

static int implicit_from(const char *s)
{
	if (str_eq(s, "yes"))
		return IMPLICIT_YES;
	if (str_eq(s, "auth_self"))
		return IMPLICIT_AUTH_SELF;
	if (str_eq(s, "auth_admin"))
		return IMPLICIT_AUTH_ADMIN;
	if (str_eq(s, "auth_self_keep"))
		return IMPLICIT_AUTH_SELF_KEEP;
	if (str_eq(s, "auth_admin_keep"))
		return IMPLICIT_AUTH_ADMIN_KEEP;
	return IMPLICIT_NO;
}

/* Text between <tag> and </tag> (the first without xml:lang), entities decoded. */
static char *element(const char *from, const char *to, const char *tag)
{
	char open[48], close[48];
	fmt(open, sizeof open, "<%s", tag);
	fmt(close, sizeof close, "</%s>", tag);
	const char *p = from;
	while ((p = str_find(p, open)) && p < to) {
		const char *gt = str_chr(p, '>');
		if (!gt || gt > to)
			return NULL;
		char c = p[strlen(open)];
		if (c != '>' && c != ' ' && c != '\t' && c != '\n') {
			p++;
			continue;
		}
		bool translated = false;
		for (const char *a = p; a < gt; a++)
			if (str_starts(a, "xml:lang"))
				translated = true;
		if (translated) {
			p = gt;
			continue;
		}
		const char *end = str_find(gt, close);
		if (!end || end > to)
			return NULL;
		StrBuf sb = { 0 };
		for (const char *s = gt + 1; s < end; s++) {
			if (*s == '&') {
				static const char *const ent[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" } };
				bool done = false;
				for (usize e = 0; e < ARRAY_LEN(ent) && !done; e++)
					if (str_starts(s, ent[e][0])) {
						sb_adds(&sb, ent[e][1]);
						s += strlen(ent[e][0]) - 1;
						done = true;
					}
				if (done)
					continue;
			}
			sb_addc(&sb, *s);
		}
		char *t = sb_take(&sb);
		char *trimmed = str_dup(str_trim(t));
		mem_free(t);
		return trimmed;
	}
	return NULL;
}

static char *or_empty(char *s)
{
	return s ? s : str_dup("");
}

static void load_policy(const char *path)
{
	char *text = file_read(path, NULL);
	if (!text)
		return;
	const char *first_action = str_find(text, "<action ");
	char *vendor = first_action ? element(text, first_action, "vendor") : NULL;
	char *vendor_url = first_action ? element(text, first_action, "vendor_url") : NULL;
	char *icon = first_action ? element(text, first_action, "icon_name") : NULL;
	for (const char *p = first_action; p; p = str_find(p + 1, "<action ")) {
		const char *end = str_find(p, "</action>");
		const char *idq = str_find(p, "id=\"");
		if (!end || !idq || idq > end)
			break;
		idq += 4;
		const char *idend = str_chr(idq, '"');
		if (!idend)
			break;
		Action a = { 0 };
		a.id = str_ndup(idq, (usize)(idend - idq));
		a.description = or_empty(element(p, end, "description"));
		a.message = or_empty(element(p, end, "message"));
		char *own_icon = element(p, end, "icon_name");
		a.icon = own_icon ? own_icon : str_dup(icon ? icon : "");
		char *v = element(p, end, "vendor"), *vu = element(p, end, "vendor_url");
		a.vendor = v ? v : str_dup(vendor ? vendor : "");
		a.vendor_url = vu ? vu : str_dup(vendor_url ? vendor_url : "");
		const char *defs = str_find(p, "<defaults>");
		char *any = NULL, *inactive = NULL, *active = NULL;
		if (defs && defs < end) {
			any = element(defs, end, "allow_any");
			inactive = element(defs, end, "allow_inactive");
			active = element(defs, end, "allow_active");
		}
		a.any = implicit_from(any ? any : "no");
		a.inactive = implicit_from(inactive ? inactive : "no");
		a.active = implicit_from(active ? active : "no");
		mem_free(any);
		mem_free(inactive);
		mem_free(active);
		const char *imp = str_find(p, "key=\"org.freedesktop.policykit.imply\"");
		if (imp && imp < end) {
			const char *gt = str_chr(imp, '>'), *lt = gt ? str_chr(gt, '<') : NULL;
			if (gt && lt && lt < end)
				a.imply = str_ndup(gt + 1, (usize)(lt - gt - 1));
		}
		vec_push(&actions, a);
		p = end;
	}
	mem_free(vendor);
	mem_free(vendor_url);
	mem_free(icon);
	mem_free(text);
}

static void load_actions(void)
{
	char *dir = str_fmt("%s/usr/share/polkit-1/actions", layer_root());
	DirList l = { 0 };
	if (dir_list(dir, &l, false)) {
		for (usize i = 0; i < l.len; i++) {
			if (!str_ends(l.data[i].name, ".policy"))
				continue;
			char *p = path_join(dir, l.data[i].name);
			load_policy(p);
			mem_free(p);
		}
		dir_list_free(&l);
	}
	mem_free(dir);
}

static Action *find_action(const char *id)
{
	for (usize i = 0; i < actions.len; i++)
		if (str_eq(actions.data[i].id, id))
			return &actions.data[i];
	return NULL;
}

/* ---- Subjects ------------------------------------------------------------------------------- */

static u32 pid_uid(u32 pid)
{
	char path[64];
	fmt(path, sizeof path, "/proc/%u/status", pid);
	char *t = file_read(path, NULL);
	u32 uid = (u32)-1;
	const char *u = t ? str_find(t, "\nUid:") : NULL;
	if (u) {
		u += 5;
		while (*u == ' ' || *u == '\t')
			u++;
		uid = (u32)str_to_int(u, NULL);
	}
	mem_free(t);
	return uid;
}

/* The uid behind a subject (sa{sv}): a process, a bus name or a session. */
static u32 subject_uid(DBus *bus, const DBusVal *subject)
{
	if (!subject || subject->count < 2)
		return (u32)-1;
	const char *kind = dbus_str(subject->items[0], "");
	const DBusVal *d = subject->items[1];
	if (str_eq(kind, "unix-process")) {
		const DBusVal *uid = dbus_dict_get(d, "uid");
		u32 pid = (u32)dbus_uint(dbus_dict_get(d, "pid"), 0);
		u32 from_pid = pid ? pid_uid(pid) : (u32)-1;
		if (uid && from_pid != (u32)-1 && (u32)dbus_int(uid, -1) != from_pid)
			return (u32)-1;     /* a uid that doesn't match the process: someone lying */
		return from_pid;
	}
	if (str_eq(kind, "system-bus-name")) {
		u32 pid = dbus_peer_pid(bus, dbus_str(dbus_dict_get(d, "name"), ""));
		return pid ? pid_uid(pid) : (u32)-1;
	}
	if (str_eq(kind, "unix-session"))
		return str_eq(dbus_str(dbus_dict_get(d, "session-id"), ""), Z.session) ? Z.uid : (u32)-1;
	return (u32)-1;
}

static bool has_password(void)
{
	char *p = data_path("shadow");
	Conf *sh = conf_load(p);
	bool has = conf_get(sh, "password", "")[0] != 0;
	conf_free(sh);
	mem_free(p);
	return has;
}

/* ---- Temporary authorizations (the "_keep" kinds, five minutes) ----------------------------- */

typedef struct {
	char id[24], action[128];
	u32 uid;
	u64 obtained_ms, expires_ms;
} Temp;

static Temp temps[32];
static int ntemps;

static bool temp_valid(const char *action, u32 uid)
{
	u64 now = time_mono_ms();
	for (int i = ntemps - 1; i >= 0; i--) {
		if (temps[i].expires_ms <= now) {
			temps[i] = temps[--ntemps];
			continue;
		}
		if (temps[i].uid == uid && str_eq(temps[i].action, action))
			return true;
	}
	return false;
}

static void temp_add(const char *action, u32 uid)
{
	if (ntemps == (int)ARRAY_LEN(temps))
		temps[0] = temps[--ntemps];
	Temp *t = &temps[ntemps++];
	u8 r[6];
	rand_bytes(r, sizeof r);
	fmt(t->id, sizeof t->id, "tmpauth%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5]);
	str_copy(t->action, action, sizeof t->action);
	t->uid = uid;
	t->obtained_ms = time_mono_ms();
	t->expires_ms = t->obtained_ms + 5 * 60 * 1000;
}

/* ---- Agents and pending checks ---------------------------------------------------------------- */

static char agent_owner[64], agent_path[160];

typedef struct {
	bool used;
	DBusMsg *call;           /* the CheckAuthorization waiting for its answer */
	char cookie[40], action[128], cancel_id[96];
	u32 uid;
	bool keep, authenticated;
} Pending;

static Pending pending[16];

static void reply_result(DBus *bus, const DBusMsg *call, bool authorized, bool challenge, const char *detail)
{
	DBusMsg *m = dbus_new_reply(call);
	dbus_open(m, "(bba{ss})");
	dbus_add(m, "bb", authorized, challenge);
	dbus_open(m, "a{ss}");
	if (detail) {
		dbus_open(m, "{ss}");
		dbus_add(m, "ss", detail, "true");
		dbus_close(m);
	}
	dbus_close(m);
	dbus_close(m);
	dbus_send(bus, m);
}

static void finish(Pending *p, DBus *bus, bool dismissed)
{
	if (!p->used)
		return;
	if (p->authenticated && p->keep)
		temp_add(p->action, p->uid);
	reply_result(bus, p->call, p->authenticated, false, !p->authenticated && dismissed ? "polkit.dismissed" : NULL);
	dbus_msg_free(p->call);
	*p = (Pending){ 0 };
}

static void on_agent_reply(DBus *bus, DBusMsg *reply, void *data)
{
	Pending *p = data;
	finish(p, bus, !reply || dbus_type(reply) == DBUS_ERROR);
}

static Pending *find_cookie(const char *cookie)
{
	for (usize i = 0; i < ARRAY_LEN(pending); i++)
		if (pending[i].used && str_eq(pending[i].cookie, cookie))
			return &pending[i];
	return NULL;
}

/* Asks the agent to authenticate; the check answers once it is done. */
static bool begin_authentication(DBus *bus, DBusMsg *call, const Action *a, const DBusVal *details, u32 uid, bool keep)
{
	if (!agent_owner[0])
		return false;
	Pending *p = NULL;
	for (usize i = 0; i < ARRAY_LEN(pending) && !p; i++)
		if (!pending[i].used)
			p = &pending[i];
	if (!p)
		return false;
	*p = (Pending){ .used = true, .uid = uid, .keep = keep };
	p->call = dbus_msg_ref(call);
	u8 r[16];
	rand_bytes(r, sizeof r);
	for (int i = 0; i < 16; i++)
		fmt(p->cookie + i * 2, 3, "%02x", r[i]);
	str_copy(p->action, a->id, sizeof p->action);
	str_copy(p->cancel_id, dbus_str(dbus_arg(call, 4), ""), sizeof p->cancel_id);

	DBusMsg *m = dbus_new_call(agent_owner, agent_path, AGENT_IF, "BeginAuthentication");
	dbus_add(m, "sss", a->id, a->message[0] ? a->message : "Authentication is needed to change the system", a->icon);
	dbus_open(m, "a{ss}");
	for (usize i = 0; details && i < details->count; i++) {
		const DBusVal *e = details->items[i];
		if (e->count == 2) {
			dbus_open(m, "{ss}");
			dbus_add(m, "ss", dbus_str(e->items[0], ""), dbus_str(e->items[1], ""));
			dbus_close(m);
		}
	}
	dbus_close(m);
	dbus_add(m, "s", p->cookie);
	dbus_open(m, "a(sa{sv})");
	dbus_open(m, "(sa{sv})");
	dbus_add(m, "s", "unix-user");
	dbus_open(m, "a{sv}");
	dbus_dict_add(m, "uid", "u", Z.uid);
	dbus_close(m);
	dbus_close(m);
	dbus_close(m);
	dbus_call_async(bus, m, on_agent_reply, p);
	return true;
}

/* ---- The authority ----------------------------------------------------------------------- */

static void check_authorization(DBus *bus, DBusMsg *call)
{
	u32 uid = subject_uid(bus, dbus_arg(call, 0));
	const char *action_id = dbus_str(dbus_arg(call, 1), "");
	u32 flags = (u32)dbus_uint(dbus_arg(call, 3), 0);
	if (uid == 0) {
		reply_result(bus, call, true, false, NULL);
		return;
	}
	Action *a = find_action(action_id);
	if (!a || uid != Z.uid) {
		reply_result(bus, call, false, false, NULL);
		return;
	}
	/* the session is always the active one */
	int implicit = a->active;
	if (implicit == IMPLICIT_YES || (implicit != IMPLICIT_NO && (!has_password() || temp_valid(action_id, uid)))) {
		reply_result(bus, call, true, false, NULL);
		return;
	}
	if (implicit == IMPLICIT_NO) {
		reply_result(bus, call, false, false, NULL);
		return;
	}
	bool keep = implicit == IMPLICIT_AUTH_SELF_KEEP || implicit == IMPLICIT_AUTH_ADMIN_KEEP;
	if (!(flags & CHECK_ALLOW_INTERACTION) || !begin_authentication(bus, call, a, dbus_arg(call, 2), uid, keep))
		reply_result(bus, call, false, true, NULL);
}

static void add_action(DBusMsg *m, const Action *a)
{
	dbus_open(m, "(ssssssuuua{ss})");
	dbus_add(m, "ssssssuuu", a->id, a->description, a->message, a->vendor, a->vendor_url, a->icon, a->any, a->inactive, a->active);
	dbus_open(m, "a{ss}");
	if (a->imply) {
		dbus_open(m, "{ss}");
		dbus_add(m, "ss", "org.freedesktop.policykit.imply", a->imply);
		dbus_close(m);
	}
	dbus_close(m);
	dbus_close(m);
}

static void p_backend(DBusMsg *m, void *obj) { (void)obj; dbus_add(m, "s", "zurbax"); }
static void p_version(DBusMsg *m, void *obj) { (void)obj; dbus_add(m, "s", OS_VERSION); }
static void p_features(DBusMsg *m, void *obj) { (void)obj; dbus_add(m, "u", 1u); }

static const Prop AUTHORITY_PROPS[] = {
	{ "BackendName", "s", p_backend, NULL },
	{ "BackendVersion", "s", p_version, NULL },
	{ "BackendFeatures", "u", p_features, NULL },
};

static bool authority_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, AUTHORITY_IF, AUTHORITY_PROPS, ARRAY_LEN(AUTHORITY_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "CheckAuthorization")) {
		check_authorization(bus, call);
	} else if (str_eq(mbr, "EnumerateActions")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "a(ssssssuuua{ss})");
		for (usize i = 0; i < actions.len; i++)
			add_action(m, &actions.data[i]);
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "RegisterAuthenticationAgent") || str_eq(mbr, "RegisterAuthenticationAgentWithOptions")) {
		u32 uid = subject_uid(bus, dbus_arg(call, 0));
		u32 caller = dbus_peer_pid(bus, dbus_sender(call));
		if (uid != Z.uid && uid != 0) {
			dbus_reply_error(bus, call, "org.freedesktop.PolicyKit1.Error.Failed", "Only the session's own agent");
			return true;
		}
		if (caller && pid_uid(caller) != uid && pid_uid(caller) != 0) {
			dbus_reply_error(bus, call, "org.freedesktop.PolicyKit1.Error.NotAuthorized", "An agent registers itself");
			return true;
		}
		str_copy(agent_owner, dbus_sender(call), sizeof agent_owner);
		str_copy(agent_path, dbus_str(dbus_arg(call, 2), ""), sizeof agent_path);
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "UnregisterAuthenticationAgent")) {
		if (str_eq(agent_owner, dbus_sender(call)))
			agent_owner[0] = 0;
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "AuthenticationAgentResponse") || str_eq(mbr, "AuthenticationAgentResponse2")) {
		/* polkit's own helper reports success here as root; Zurbax's uses Authenticate */
		u32 caller = dbus_peer_pid(bus, dbus_sender(call));
		Pending *p = find_cookie(dbus_str(dbus_arg(call, str_eq(mbr, "AuthenticationAgentResponse2") ? 1 : 0), ""));
		if (!p || !caller || pid_uid(caller) != 0) {
			dbus_reply_error(bus, call, "org.freedesktop.PolicyKit1.Error.NotAuthorized", "Not allowed");
			return true;
		}
		p->authenticated = true;
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "CancelCheckAuthorization")) {
		const char *id = dbus_str(dbus_arg(call, 0), "");
		for (usize i = 0; i < ARRAY_LEN(pending); i++)
			if (pending[i].used && *id && str_eq(pending[i].cancel_id, id)) {
				DBusMsg *c = dbus_new_call(agent_owner, agent_path, AGENT_IF, "CancelAuthentication");
				dbus_add(c, "s", pending[i].cookie);
				dbus_set_flags(c, DBUS_NO_REPLY);
				dbus_send(bus, c);
				finish(&pending[i], bus, true);
			}
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "EnumerateTemporaryAuthorizations")) {
		u32 uid = subject_uid(bus, dbus_arg(call, 0));
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "a(ss(sa{sv})tt)");
		temp_valid("", 0);   /* drops the expired ones */
		u64 now_ms = (u64)time_unix_ms(), mono = time_mono_ms();
		for (int i = 0; i < ntemps; i++) {
			if (temps[i].uid != uid)
				continue;
			dbus_open(m, "(ss(sa{sv})tt)");
			dbus_add(m, "ss", temps[i].id, temps[i].action);
			dbus_open(m, "(sa{sv})");
			dbus_add(m, "s", "unix-session");
			dbus_open(m, "a{sv}");
			dbus_dict_add(m, "session-id", "s", Z.session);
			dbus_close(m);
			dbus_close(m);
			dbus_add(m, "tt", (now_ms - (mono - temps[i].obtained_ms)) / 1000, (now_ms + (temps[i].expires_ms - mono)) / 1000);
			dbus_close(m);
		}
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "RevokeTemporaryAuthorizations")) {
		ntemps = 0;
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "RevokeTemporaryAuthorizationById")) {
		const char *id = dbus_str(dbus_arg(call, 0), "");
		for (int i = ntemps - 1; i >= 0; i--)
			if (str_eq(temps[i].id, id))
				temps[i] = temps[--ntemps];
		dbus_reply_empty(bus, call);
	} else {
		return false;
	}
	return true;
}

/* org.zurbax.PolicyKit.Authenticate(cookie, password): from the agent's helper */
static u64 last_failure_ms;

static bool zurbax_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (!str_eq(dbus_member(call), "Authenticate"))
		return false;
	u32 caller = dbus_peer_pid(bus, dbus_sender(call));
	u32 uid = caller ? pid_uid(caller) : (u32)-1;
	Pending *p = find_cookie(dbus_str(dbus_arg(call, 0), ""));
	if (!p || (uid != Z.uid && uid != 0)) {
		dbus_reply_error(bus, call, "org.freedesktop.PolicyKit1.Error.NotAuthorized", "No such authentication");
		return true;
	}
	if (time_mono_ms() - last_failure_ms < 1500) {
		dbus_reply_error(bus, call, "org.zurbax.PolicyKit.Error.TryLater", "Wait a moment and try again");
		return true;
	}
	if (!accounts_check_password(dbus_str(dbus_arg(call, 1), ""))) {
		last_failure_ms = time_mono_ms();
		dbus_reply_error(bus, call, "org.zurbax.PolicyKit.Error.WrongPassword", "That password isn't right");
		return true;
	}
	p->authenticated = true;
	dbus_reply_empty(bus, call);
	return true;
}

/* The agent left: what it was asked for is dismissed. */
static void on_name_owner(DBus *bus, DBusMsg *sig, void *data)
{
	(void)data;
	const char *name = dbus_str(dbus_arg(sig, 0), ""), *now = dbus_str(dbus_arg(sig, 2), "");
	if (*now || !agent_owner[0] || !str_eq(name, agent_owner))
		return;
	agent_owner[0] = 0;
	for (usize i = 0; i < ARRAY_LEN(pending); i++)
		finish(&pending[i], bus, true);
}

static const char AUTHORITY_XML[] =
	"<interface name=\"org.freedesktop.PolicyKit1.Authority\">"
	"<method name=\"EnumerateActions\"><arg type=\"s\" direction=\"in\"/><arg type=\"a(ssssssuuua{ss})\" direction=\"out\"/></method>"
	"<method name=\"CheckAuthorization\"><arg type=\"(sa{sv})\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"a{ss}\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"(bba{ss})\" direction=\"out\"/></method>"
	"<method name=\"CancelCheckAuthorization\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"RegisterAuthenticationAgent\"><arg type=\"(sa{sv})\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"RegisterAuthenticationAgentWithOptions\"><arg type=\"(sa{sv})\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/></method>"
	"<method name=\"UnregisterAuthenticationAgent\"><arg type=\"(sa{sv})\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"AuthenticationAgentResponse\"><arg type=\"s\" direction=\"in\"/><arg type=\"(sa{sv})\" direction=\"in\"/></method>"
	"<method name=\"AuthenticationAgentResponse2\"><arg type=\"u\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"(sa{sv})\" direction=\"in\"/></method>"
	"<method name=\"EnumerateTemporaryAuthorizations\"><arg type=\"(sa{sv})\" direction=\"in\"/>"
	"<arg type=\"a(ss(sa{sv})tt)\" direction=\"out\"/></method>"
	"<method name=\"RevokeTemporaryAuthorizations\"><arg type=\"(sa{sv})\" direction=\"in\"/></method>"
	"<method name=\"RevokeTemporaryAuthorizationById\"><arg type=\"s\" direction=\"in\"/></method>"
	"<signal name=\"Changed\"/>"
	"<property name=\"BackendName\" type=\"s\" access=\"read\"/>"
	"<property name=\"BackendVersion\" type=\"s\" access=\"read\"/>"
	"<property name=\"BackendFeatures\" type=\"u\" access=\"read\"/>"
	"</interface>";

static const char ZURBAX_XML[] =
	"<interface name=\"org.zurbax.PolicyKit\">"
	"<method name=\"Authenticate\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
	"</interface>";

void polkit_start(DBus *bus)
{
	load_actions();
	dbus_export(bus, AUTHORITY_PATH, AUTHORITY_IF, authority_method, NULL, AUTHORITY_XML);
	dbus_export(bus, AUTHORITY_PATH, ZURBAX_IF, zurbax_method, NULL, ZURBAX_XML);
	dbus_subscribe(bus, "org.freedesktop.DBus", NULL, "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner, NULL);
	dbus_request_name(bus, POLKIT, DBUS_NAME_NO_QUEUE);
}
