/*
 * zsession: Zurbax's session manager for GNOME.
 *
 *     zsession [--headless WIDTHxHEIGHT]
 *
 * It runs as the person, inside the Linux layer (layerrun starts it), and
 * does what gnome-session does under systemd elsewhere: it answers as
 * org.gnome.SessionManager on the session bus, starts the sound server
 * (PipeWire), GNOME Shell (the compositor and desktop), then the
 * settings daemons and every autostart program in their phases, keeps
 * them running, notices when the person is idle, and ends the session
 * (log out, restart, power off) through GNOME Shell's dialog.
 *
 * --headless runs GNOME Shell without a screen, on a virtual monitor of
 * that size, in unsafe mode (for tests: screenshots and Eval over D-Bus).
 */
#include "zr.h"

#define SM "org.gnome.SessionManager"
#define SM_PATH "/org/gnome/SessionManager"
#define PRESENCE_PATH SM_PATH "/Presence"
#define PRESENCE_IF SM ".Presence"
#define CLIENT_IF SM ".Client"
#define CLIENT_PRIVATE_IF SM ".ClientPrivate"
#define INHIBITOR_IF SM ".Inhibitor"
#define DIALOG_PATH SM_PATH "/EndSessionDialog"
#define DIALOG_IF SM ".EndSessionDialog"

enum { PHASE_INIT, PHASE_WM, PHASE_PANEL, PHASE_DESKTOP, PHASE_APPS, NPHASES };
enum { INHIBIT_LOGOUT = 1, INHIBIT_SWITCH = 2, INHIBIT_SUSPEND = 4, INHIBIT_IDLE = 8, INHIBIT_AUTOMOUNT = 16 };
enum { PRESENCE_AVAILABLE, PRESENCE_INVISIBLE, PRESENCE_BUSY, PRESENCE_IDLE };
enum { END_LOGOUT, END_SHUTDOWN, END_REBOOT };

static Loop *loop;
static DBus *bus;
static const char *headless;
static bool running, ending;
static int end_action = -1;

static char **env;
static usize nenv;

static void setenv_(const char *name, const char *value)
{
	usize n = strlen(name);
	char *kv = value ? str_fmt("%s=%s", name, value) : NULL;
	for (usize i = 0; i < nenv; i++)
		if (str_starts(env[i], name) && env[i][n] == '=') {
			if (kv) {
				env[i] = kv;
			} else {
				env[i] = env[--nenv];
				env[nenv] = NULL;
			}
			return;
		}
	if (!kv)
		return;
	env = mem_realloc(env, sizeof(char *) * (nenv + 2));
	env[nenv++] = kv;
	env[nenv] = NULL;
}

static const char *getenv_(const char *name)
{
	usize n = strlen(name);
	for (usize i = 0; i < nenv; i++)
		if (str_starts(env[i], name) && env[i][n] == '=')
			return env[i] + n + 1;
	return NULL;
}

/* ---- Programs --------------------------------------------------------------------------- */

typedef struct {
	char id[128];               /* the desktop file's name, or a name of ours */
	char **argv;
	int pid;
	bool restart;
	int restarts;
	u64 started_ms;
	char startup_id[48];
} Proc;

static Proc procs[128];
static int nprocs;
static Proc *shell;

static char *find_exe(const char *name)
{
	if (str_chr(name, '/'))
		return file_exists(name) ? str_dup(name) : NULL;
	static const char *const dirs[] = { "/usr/local/bin", "/usr/bin", "/usr/libexec", "/usr/lib/gnome-settings-daemon", "/bin" };
	for (usize i = 0; i < ARRAY_LEN(dirs); i++) {
		char *p = str_fmt("%s/%s", dirs[i], name);
		if (file_exists(p))
			return p;
		mem_free(p);
	}
	return NULL;
}

static void proc_start(Proc *p)
{
	char *exe = find_exe(p->argv[0]);
	if (!exe) {
		eprint("zsession: %s: no %s\n", p->id, p->argv[0]);
		p->pid = 0;
		return;
	}
	u8 r[8];
	rand_bytes(r, sizeof r);
	fmt(p->startup_id, sizeof p->startup_id, "zurbax-%02x%02x%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
	setenv_("DESKTOP_AUTOSTART_ID", p->startup_id);
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1 };
	p->pid = proc_spawn(exe, p->argv, env, &o);
	setenv_("DESKTOP_AUTOSTART_ID", NULL);
	p->started_ms = time_mono_ms();
	if (p->pid < 0) {
		eprint("zsession: can't start %s: %s\n", p->id, err_str(p->pid));
		p->pid = 0;
	}
	mem_free(exe);
}

static Proc *run(const char *id, char **argv, bool restart)
{
	if (nprocs == (int)ARRAY_LEN(procs))
		return NULL;
	Proc *p = &procs[nprocs++];
	*p = (Proc){ .restart = restart };
	str_copy(p->id, id, sizeof p->id);
	p->argv = argv;
	proc_start(p);
	return p;
}

static char **argv_of(const char *a, ...)
{
	char **v = mem_alloc(sizeof(char *) * 8);
	usize n = 0;
	va_list ap;
	va_start(ap, a);
	for (const char *s = a; s && n < 7; s = va_arg(ap, const char *))
		v[n++] = str_dup(s);
	va_end(ap);
	v[n] = NULL;
	return v;
}

/* ---- Autostart (desktop entries) ------------------------------------------------------ */

typedef struct {
	char name[128];
	char **argv;
	int phase;
	bool restart;
	u32 delay_ms;
} Autostart;

static VEC(Autostart) autostarts;

/* A desktop entry's Exec line as arguments: quotes and escapes undone,
 * the field codes (%f %u and the rest) dropped. */
static char **parse_exec(const char *exec)
{
	VEC(char *) args = { 0 };
	StrBuf cur = { 0 };
	bool in_quote = false, have = false;
	for (const char *c = exec; ; c++) {
		if (!*c || (!in_quote && (*c == ' ' || *c == '\t'))) {
			if (have) {
				char *a = sb_take(&cur);
				if (a[0] == '%' && a[1] && !a[2])
					mem_free(a);
				else
					vec_push(&args, a);
				cur = (StrBuf){ 0 };
				have = false;
			}
			if (!*c)
				break;
			continue;
		}
		have = true;
		if (*c == '"') {
			in_quote = !in_quote;
		} else if (*c == '\\' && c[1]) {
			sb_addc(&cur, *++c);
		} else if (*c == '%' && c[1] == '%') {
			sb_addc(&cur, '%');
			c++;
		} else {
			sb_addc(&cur, *c);
		}
	}
	sb_free(&cur);
	vec_push(&args, NULL);
	return args.data;
}

static bool desktop_listed(const char *list, const char *desktop)
{
	usize n = 0;
	char **parts = str_split(list, ';', &n);
	bool found = false;
	for (usize i = 0; i < n; i++)
		found |= str_eq(parts[i], desktop);
	str_split_free(parts, n);
	return found;
}

/* "GSettings schema key": the key's value, through the layer's gsettings. */
static bool gsettings_true(const char *schema, const char *key)
{
	char *exe = find_exe("gsettings");
	if (!exe)
		return false;
	char *argv[] = { exe, "get", (char *)schema, (char *)key, NULL };
	int status = 0;
	char *out = proc_run_capture(argv, &status);
	bool on = out && str_starts(str_trim(out), "true");
	mem_free(out);
	mem_free(exe);
	return on;
}

static bool condition_holds(const char *cond)
{
	if (!*cond)
		return true;
	usize n = 0;
	char **w = str_split(cond, ' ', &n);
	bool ok = true;
	if (n >= 3 && str_eq(w[0], "GSettings"))
		ok = gsettings_true(w[1], w[2]);
	else if (n >= 2 && (str_eq(w[0], "if-exists") || str_eq(w[0], "unless-exists"))) {
		const char *cfg = getenv_("XDG_CONFIG_HOME");
		char *p = cfg ? str_fmt("%s/%s", cfg, w[1]) : str_fmt("%s/.config/%s", getenv_("HOME"), w[1]);
		ok = file_exists(p) == str_eq(w[0], "if-exists");
		mem_free(p);
	}
	str_split_free(w, n);
	return ok;
}

static int phase_of(const char *s)
{
	if (str_eq(s, "EarlyInitialization") || str_eq(s, "PreDisplayServer") || str_eq(s, "Initialization"))
		return PHASE_INIT;
	if (str_eq(s, "WindowManager"))
		return PHASE_WM;
	if (str_eq(s, "Panel"))
		return PHASE_PANEL;
	if (str_eq(s, "Desktop"))
		return PHASE_DESKTOP;
	return PHASE_APPS;
}

/* Reads one entry; the person's own (same name, read first) wins. */
static void load_entry(const char *dir, const char *name)
{
	for (usize i = 0; i < autostarts.len; i++)
		if (str_eq(autostarts.data[i].name, name))
			return;
	char *path = path_join(dir, name);
	char *text = file_read(path, NULL);
	mem_free(path);
	if (!text)
		return;
	char exec[1024] = "", tryexec[256] = "", only[256] = "", not[256] = "", phase[48] = "", cond[256] = "";
	bool hidden = false, enabled = true, restart = false, in_entry = false;
	u32 delay = 0;
	usize n = 0;
	char **lines = str_split(text, '\n', &n);
	for (usize i = 0; i < n; i++) {
		char *l = str_trim(lines[i]);
		if (l[0] == '[') {
			in_entry = str_eq(l, "[Desktop Entry]");
			continue;
		}
		char *eq = (char *)str_chr(l, '=');
		if (!in_entry || !eq)
			continue;
		*eq = 0;
		char *k = str_trim(l), *v = str_trim(eq + 1);
		if (str_eq(k, "Exec"))
			str_copy(exec, v, sizeof exec);
		else if (str_eq(k, "TryExec"))
			str_copy(tryexec, v, sizeof tryexec);
		else if (str_eq(k, "OnlyShowIn"))
			str_copy(only, v, sizeof only);
		else if (str_eq(k, "NotShowIn"))
			str_copy(not, v, sizeof not);
		else if (str_eq(k, "Hidden"))
			hidden = str_eq(v, "true");
		else if (str_eq(k, "X-GNOME-Autostart-enabled"))
			enabled = !str_eq(v, "false");
		else if (str_eq(k, "X-GNOME-Autostart-Phase"))
			str_copy(phase, v, sizeof phase);
		else if (str_eq(k, "X-GNOME-AutoRestart"))
			restart = str_eq(v, "true");
		else if (str_eq(k, "X-GNOME-Autostart-Delay"))
			delay = (u32)str_to_int(v, NULL) * 1000;
		else if (str_eq(k, "AutostartCondition"))
			str_copy(cond, v, sizeof cond);
	}
	str_split_free(lines, n);
	mem_free(text);
	Autostart a = { 0 };
	str_copy(a.name, name, sizeof a.name);
	/* a hidden or disabled entry still counts: it hides the system's one of the same name */
	bool wanted = !hidden && enabled && exec[0] && (!only[0] || desktop_listed(only, "GNOME")) && !desktop_listed(not, "GNOME");
	if (wanted && tryexec[0]) {
		char *t = find_exe(tryexec);
		wanted = t != NULL;
		mem_free(t);
	}
	/* GNOME Shell is started on its own, first */
	if (str_starts(name, "org.gnome.Shell"))
		wanted = false;
	if (wanted && !condition_holds(cond))
		wanted = false;
	if (wanted) {
		a.argv = parse_exec(exec);
		a.phase = phase_of(phase);
		a.restart = restart;
		a.delay_ms = delay;
	}
	vec_push(&autostarts, a);
}

static void load_autostart(void)
{
	char *user = str_fmt("%s/.config/autostart", getenv_("HOME"));
	const char *dirs[] = { user, "/etc/xdg/autostart", "/usr/share/gnome/autostart" };
	for (usize d = 0; d < ARRAY_LEN(dirs); d++) {
		DirList l = { 0 };
		if (!dir_list(dirs[d], &l, false))
			continue;
		for (usize i = 0; i < l.len; i++)
			if (str_ends(l.data[i].name, ".desktop"))
				load_entry(dirs[d], l.data[i].name);
		dir_list_free(&l);
	}
	mem_free(user);
}

/* The settings daemons: their own autostart entries when installed, or each gsd-* program. */
static void settings_daemons(void)
{
	bool have_entries = false;
	for (usize i = 0; i < autostarts.len; i++)
		have_entries |= str_starts(autostarts.data[i].name, "org.gnome.SettingsDaemon.");
	if (have_entries)
		return;
	static const char *const dirs[] = { "/usr/libexec", "/usr/lib/gnome-settings-daemon" };
	static const char *const helpers[] = { "gsd-backlight-helper", "gsd-wacom-oled-helper", "gsd-printer", "gsd-test", "gsd-list-wacom" };
	for (usize d = 0; d < ARRAY_LEN(dirs); d++) {
		DirList l = { 0 };
		if (!dir_list(dirs[d], &l, false))
			continue;
		for (usize i = 0; i < l.len; i++) {
			const char *n = l.data[i].name;
			bool helper = false;
			for (usize h = 0; h < ARRAY_LEN(helpers); h++)
				helper |= str_starts(n, helpers[h]);
			if (!str_starts(n, "gsd-") || helper)
				continue;
			Autostart a = { .phase = PHASE_INIT, .restart = true };
			fmt(a.name, sizeof a.name, "%s.daemon", n);
			char *p = path_join(dirs[d], n);
			a.argv = argv_of(p, NULL);
			mem_free(p);
			vec_push(&autostarts, a);
		}
		dir_list_free(&l);
	}
}

static void start_phase(int phase)
{
	for (usize i = 0; i < autostarts.len; i++) {
		Autostart *a = &autostarts.data[i];
		if (a->argv && a->phase == phase && !a->delay_ms)
			run(a->name, a->argv, a->restart);
	}
}

static void start_delayed(Loop *l, void *data)
{
	(void)l;
	Autostart *a = data;
	if (!ending)
		run(a->name, a->argv, a->restart);
}

/* ---- Clients (programs that register) ------------------------------------------------------- */

typedef struct {
	bool used;
	u32 num;
	char app_id[128], startup_id[64], owner[64];
} Client;

static Client clients[64];
static u32 next_client = 1;

static char *client_path(const Client *c)
{
	return str_fmt(SM_PATH "/Client%u", c->num);
}

static bool client_method(DBus *b, DBusMsg *call, void *data)
{
	Client *c = data;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "GetAppId") || str_eq(mbr, "GetStartupId")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "s", str_eq(mbr, "GetAppId") ? c->app_id : c->startup_id);
		dbus_send(b, m);
	} else if (str_eq(mbr, "GetRestartStyleHint") || str_eq(mbr, "GetStatus")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "u", str_eq(mbr, "GetStatus") ? 1u : 0u);
		dbus_send(b, m);
	} else if (str_eq(mbr, "GetUnixProcessId")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "u", dbus_peer_pid(b, c->owner));
		dbus_send(b, m);
	} else if (str_eq(mbr, "Stop") || str_eq(mbr, "EndSessionResponse")) {
		dbus_reply_empty(b, call);
	} else {
		return false;
	}
	return true;
}

static void client_signal(Client *c, const char *member, bool with_flags)
{
	char *p = client_path(c);
	DBusMsg *m = dbus_new_signal(p, CLIENT_PRIVATE_IF, member);
	if (with_flags)
		dbus_add(m, "u", 0u);
	dbus_set_destination(m, c->owner);
	dbus_send(bus, m);
	mem_free(p);
}

static void client_drop(Client *c)
{
	char *p = client_path(c);
	dbus_unexport(bus, p, CLIENT_IF);
	dbus_unexport(bus, p, CLIENT_PRIVATE_IF);
	DBusMsg *m = dbus_new_signal(SM_PATH, SM, "ClientRemoved");
	dbus_add(m, "o", p);
	dbus_send(bus, m);
	mem_free(p);
	c->used = false;
}

/* ---- Inhibitors ------------------------------------------------------------------------------ */

typedef struct {
	bool used;
	u32 cookie, flags;
	char app_id[128], reason[256], owner[64];
} Inhibitor;

static Inhibitor inhibitors[64];
static u32 next_cookie = 1;

static u32 inhibited_actions(void)
{
	u32 f = 0;
	for (usize i = 0; i < ARRAY_LEN(inhibitors); i++)
		if (inhibitors[i].used)
			f |= inhibitors[i].flags;
	return f;
}

static char *inhibitor_path(const Inhibitor *in)
{
	return str_fmt(SM_PATH "/Inhibitor%u", in->cookie);
}

static void sm_changed(const char *prop);

static bool inhibitor_method(DBus *b, DBusMsg *call, void *data)
{
	Inhibitor *in = data;
	const char *mbr = dbus_member(call);
	DBusMsg *m = dbus_new_reply(call);
	if (str_eq(mbr, "GetAppId"))
		dbus_add(m, "s", in->app_id);
	else if (str_eq(mbr, "GetReason"))
		dbus_add(m, "s", in->reason);
	else if (str_eq(mbr, "GetFlags"))
		dbus_add(m, "u", in->flags);
	else if (str_eq(mbr, "GetToplevelXid"))
		dbus_add(m, "u", 0u);
	else if (str_eq(mbr, "GetClientId"))
		dbus_add(m, "o", "/");
	else {
		dbus_msg_free(m);
		return false;
	}
	dbus_send(b, m);
	return true;
}

static void inhibitor_drop(Inhibitor *in)
{
	char *p = inhibitor_path(in);
	dbus_unexport(bus, p, INHIBITOR_IF);
	DBusMsg *m = dbus_new_signal(SM_PATH, SM, "InhibitorRemoved");
	dbus_add(m, "o", p);
	dbus_send(bus, m);
	mem_free(p);
	in->used = false;
	sm_changed("InhibitedActions");
}

/* ---- Presence (idle, busy) -------------------------------------------------------------------- */

static u32 presence = PRESENCE_AVAILABLE, chosen = PRESENCE_AVAILABLE;
static char status_text[256];
static u32 idle_watch, active_watch;

static void presence_set(u32 s)
{
	if (s == presence)
		return;
	presence = s;
	DBusMsg *m = dbus_new_signal(PRESENCE_PATH, PRESENCE_IF, "StatusChanged");
	dbus_add(m, "u", s);
	dbus_send(bus, m);
	m = dbus_new_signal(PRESENCE_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged");
	dbus_add(m, "s", PRESENCE_IF);
	dbus_open(m, "a{sv}");
	dbus_dict_add(m, "status", "u", s);
	dbus_close(m);
	dbus_open(m, "as");
	dbus_close(m);
	dbus_send(bus, m);
}

static u32 idle_delay_s(void)
{
	char *exe = find_exe("gsettings");
	u32 s = 300;
	if (exe) {
		char *argv[] = { exe, "get", "org.gnome.desktop.session", "idle-delay", NULL };
		int st = 0;
		char *out = proc_run_capture(argv, &st);
		if (out) {
			const char *v = out;
			while (*v && !is_digit((u8)*v))
				v++;
			if (*v)
				s = (u32)str_to_int(v, NULL);
		}
		mem_free(out);
		mem_free(exe);
	}
	return s;
}

static void idle_watch_reply(DBus *b, DBusMsg *reply, void *data)
{
	(void)b;
	u32 *slot = data;
	if (reply && dbus_type(reply) == DBUS_RETURN)
		*slot = (u32)dbus_uint(dbus_arg(reply, 0), 0);
}

static void watch_idle(void)
{
	u32 delay = idle_delay_s();
	if (!delay)
		return;   /* "never" */
	DBusMsg *m = dbus_new_call("org.gnome.Mutter.IdleMonitor", "/org/gnome/Mutter/IdleMonitor/Core", "org.gnome.Mutter.IdleMonitor",
				   "AddIdleWatch");
	dbus_add(m, "t", (u64)delay * 1000);
	dbus_call_async(bus, m, idle_watch_reply, &idle_watch);
}

static void watch_active(void)
{
	DBusMsg *m = dbus_new_call("org.gnome.Mutter.IdleMonitor", "/org/gnome/Mutter/IdleMonitor/Core", "org.gnome.Mutter.IdleMonitor",
				   "AddUserActiveWatch");
	dbus_call_async(bus, m, idle_watch_reply, &active_watch);
}

static void on_watch_fired(DBus *b, DBusMsg *sig, void *data)
{
	(void)b, (void)data;
	u32 id = (u32)dbus_uint(dbus_arg(sig, 0), 0);
	if (id && id == idle_watch) {
		if (!(inhibited_actions() & INHIBIT_IDLE)) {
			presence_set(PRESENCE_IDLE);
			watch_active();
		}
	} else if (id && id == active_watch) {
		active_watch = 0;
		presence_set(chosen);
	}
}

static bool presence_method(DBus *b, DBusMsg *call, void *data)
{
	(void)data;
	const char *mbr = dbus_member(call);
	const char *P = "org.freedesktop.DBus.Properties";
	if (dbus_is_call(call, P, "Get") || dbus_is_call(call, P, "GetAll")) {
		DBusMsg *m = dbus_new_reply(call);
		const char *name = dbus_str(dbus_arg(call, 1), "");
		if (str_eq(mbr, "Get")) {
			if (str_eq(name, "status-text")) {
				dbus_open_variant(m, "s");
				dbus_add(m, "s", status_text);
			} else {
				dbus_open_variant(m, "u");
				dbus_add(m, "u", presence);
			}
			dbus_close(m);
		} else {
			dbus_open(m, "a{sv}");
			dbus_dict_add(m, "status", "u", presence);
			dbus_dict_add(m, "status-text", "s", status_text);
			dbus_close(m);
		}
		dbus_send(b, m);
	} else if (str_eq(mbr, "SetStatus") || dbus_is_call(call, P, "Set")) {
		u32 s = (u32)dbus_uint(dbus_arg(call, str_eq(mbr, "Set") ? 2 : 0), 0);
		chosen = s == PRESENCE_IDLE ? PRESENCE_AVAILABLE : s;
		dbus_reply_empty(b, call);
		presence_set(s);
	} else if (str_eq(mbr, "SetStatusText")) {
		str_copy(status_text, dbus_str(dbus_arg(call, 0), ""), sizeof status_text);
		dbus_reply_empty(b, call);
		DBusMsg *m = dbus_new_signal(PRESENCE_PATH, PRESENCE_IF, "StatusTextChanged");
		dbus_add(m, "s", status_text);
		dbus_send(b, m);
	} else {
		return false;
	}
	return true;
}

/* ---- Ending the session ------------------------------------------------------------------------ */

static void finish_session(void);

static u32 dialog_subscribed;

static void on_dialog(DBus *b, DBusMsg *sig, void *data)
{
	(void)b, (void)data;
	const char *mbr = dbus_member(sig);
	if (str_eq(mbr, "ConfirmedLogout"))
		end_action = END_LOGOUT;
	else if (str_eq(mbr, "ConfirmedShutdown"))
		end_action = END_SHUTDOWN;
	else if (str_eq(mbr, "ConfirmedReboot"))
		end_action = END_REBOOT;
	else
		return;   /* Canceled, Closed */
	finish_session();
}

/* GNOME Shell's dialog asks first (unless told not to). */
static void ask_end(int type, bool confirm)
{
	if (!confirm || !shell || !shell->pid) {
		end_action = type;
		finish_session();
		return;
	}
	if (!dialog_subscribed) {
		dialog_subscribed = 1;
		dbus_add_match(bus, "type='signal',interface='" DIALOG_IF "'");
		dbus_subscribe(bus, NULL, DIALOG_PATH, DIALOG_IF, NULL, on_dialog, NULL);
	}
	DBusMsg *m = dbus_new_call("org.gnome.Shell", DIALOG_PATH, DIALOG_IF, "Open");
	dbus_add(m, "uuu", (u32)type, (u32)time_unix(), 60u);
	dbus_open(m, "ao");
	for (usize i = 0; i < ARRAY_LEN(inhibitors); i++)
		if (inhibitors[i].used && (inhibitors[i].flags & INHIBIT_LOGOUT)) {
			char *p = inhibitor_path(&inhibitors[i]);
			dbus_add(m, "o", p);
			mem_free(p);
		}
	dbus_close(m);
	dbus_call_async(bus, m, NULL, NULL);
}

static void power(const char *method)
{
	const char *addr = getenv_("DBUS_SYSTEM_BUS_ADDRESS");
	DBus *sys = dbus_open_address(addr && *addr ? addr : "unix:path=/run/dbus/system_bus_socket");
	if (!sys)
		return;
	DBusMsg *m = dbus_new_call("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", method);
	dbus_add(m, "b", false);   /* not interactive */
	DBusMsg *r = dbus_call(sys, m, 5000);
	if (r)
		dbus_msg_free(r);
	dbus_close_bus(sys);
}

static void finish_session(void)
{
	if (ending)
		return;
	ending = true;
	running = false;
	dbus_send(bus, dbus_new_signal(SM_PATH, SM, "SessionOver"));
	for (usize i = 0; i < ARRAY_LEN(clients); i++)
		if (clients[i].used)
			client_signal(&clients[i], "EndSession", true);
	loop_quit(loop);
}

/* ---- The manager ------------------------------------------------------------------------------- */

static void sm_changed(const char *prop)
{
	DBusMsg *m = dbus_new_signal(SM_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged");
	dbus_add(m, "s", SM);
	dbus_open(m, "a{sv}");
	if (str_eq(prop, "InhibitedActions"))
		dbus_dict_add(m, "InhibitedActions", "u", inhibited_actions());
	dbus_close(m);
	dbus_open(m, "as");
	dbus_close(m);
	dbus_send(bus, m);
}

static void add_sm_props(DBusMsg *m, const char *only)
{
#define P(name, sig, ...)                                     \
	if (!only || str_eq(only, name)) {                    \
		if (!only)                                    \
			dbus_dict_add(m, name, sig, __VA_ARGS__); \
		else {                                        \
			dbus_open_variant(m, sig);            \
			dbus_add(m, sig, __VA_ARGS__);        \
			dbus_close(m);                        \
		}                                             \
	}
	P("SessionName", "s", "gnome");
	P("Renderer", "s", "");
	P("SessionIsActive", "b", true);
	P("SessionIsLocked", "b", false);
	P("SessionClass", "s", "user");
	P("InhibitedActions", "u", inhibited_actions());
	P("RestoreSupported", "b", false);
#undef P
}

/* What the session's programs should all see, for those the bus starts too. */
static void upload_environment(void)
{
	DBusMsg *m = dbus_new_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "UpdateActivationEnvironment");
	dbus_open(m, "a{ss}");
	for (usize i = 0; i < nenv; i++) {
		const char *eq = str_chr(env[i], '=');
		if (!eq || str_starts(env[i], "DESKTOP_AUTOSTART_ID="))
			continue;
		char *k = str_ndup(env[i], (usize)(eq - env[i]));
		dbus_open(m, "{ss}");
		dbus_add(m, "ss", k, eq + 1);
		dbus_close(m);
		mem_free(k);
	}
	dbus_close(m);
	dbus_call_async(bus, m, NULL, NULL);
}

static void session_ready(void);

static bool sm_method(DBus *b, DBusMsg *call, void *data)
{
	(void)data;
	const char *mbr = dbus_member(call), *iface = dbus_interface(call) ? dbus_interface(call) : SM;
	if (str_eq(iface, "org.freedesktop.DBus.Properties")) {
		DBusMsg *m = dbus_new_reply(call);
		if (str_eq(mbr, "GetAll")) {
			dbus_open(m, "a{sv}");
			add_sm_props(m, NULL);
			dbus_close(m);
		} else if (str_eq(mbr, "Get")) {
			add_sm_props(m, dbus_str(dbus_arg(call, 1), ""));
		} else {
			dbus_msg_free(m);
			dbus_reply_error(b, call, "org.freedesktop.DBus.Error.PropertyReadOnly", "Read-only");
			return true;
		}
		dbus_send(b, m);
		return true;
	}
	if (str_eq(mbr, "Setenv")) {
		const char *k = dbus_str(dbus_arg(call, 0), ""), *v = dbus_str(dbus_arg(call, 1), "");
		if (*k && !str_chr(k, '='))
			setenv_(k, v);
		dbus_reply_empty(b, call);
		/* the compositor's socket: GNOME Shell is up */
		if (str_eq(k, "WAYLAND_DISPLAY") && !running)
			session_ready();
		else if (running)
			upload_environment();
	} else if (str_eq(mbr, "GetLocale")) {
		const char *lang = getenv_("LANG");
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "s", lang ? lang : "en_US.UTF-8");
		dbus_send(b, m);
	} else if (str_eq(mbr, "InitializationError")) {
		eprint("zsession: %s reports: %s\n", dbus_sender(call), dbus_str(dbus_arg(call, 0), ""));
		dbus_reply_empty(b, call);
	} else if (str_eq(mbr, "Initialized")) {
		dbus_reply_empty(b, call);
	} else if (str_eq(mbr, "RegisterClient")) {
		Client *c = NULL;
		for (usize i = 0; i < ARRAY_LEN(clients) && !c; i++)
			if (!clients[i].used)
				c = &clients[i];
		if (!c) {
			dbus_reply_error(b, call, "org.gnome.SessionManager.NotInInitialization", "Too many clients");
			return true;
		}
		*c = (Client){ .used = true, .num = next_client++ };
		str_copy(c->app_id, dbus_str(dbus_arg(call, 0), ""), sizeof c->app_id);
		str_copy(c->startup_id, dbus_str(dbus_arg(call, 1), ""), sizeof c->startup_id);
		str_copy(c->owner, dbus_sender(call), sizeof c->owner);
		char *p = client_path(c);
		dbus_export(b, p, CLIENT_IF, client_method, c, NULL);
		dbus_export(b, p, CLIENT_PRIVATE_IF, client_method, c, NULL);
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "o", p);
		dbus_send(b, m);
		DBusMsg *s = dbus_new_signal(SM_PATH, SM, "ClientAdded");
		dbus_add(s, "o", p);
		dbus_send(b, s);
		mem_free(p);
	} else if (str_eq(mbr, "UnregisterClient")) {
		const char *path = dbus_str(dbus_arg(call, 0), "");
		for (usize i = 0; i < ARRAY_LEN(clients); i++) {
			if (!clients[i].used)
				continue;
			char *p = client_path(&clients[i]);
			if (str_eq(p, path))
				client_drop(&clients[i]);
			mem_free(p);
		}
		dbus_reply_empty(b, call);
	} else if (str_eq(mbr, "GetClients")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "ao");
		for (usize i = 0; i < ARRAY_LEN(clients); i++)
			if (clients[i].used) {
				char *p = client_path(&clients[i]);
				dbus_add(m, "o", p);
				mem_free(p);
			}
		dbus_close(m);
		dbus_send(b, m);
	} else if (str_eq(mbr, "RegisterRestore")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "us", 0u, "");
		dbus_open(m, "as");
		dbus_close(m);
		dbus_send(b, m);
	} else if (str_eq(mbr, "DeletedInstanceIds") || str_eq(mbr, "UnregisterRestore")) {
		dbus_reply_empty(b, call);
	} else if (str_eq(mbr, "Inhibit")) {
		Inhibitor *in = NULL;
		for (usize i = 0; i < ARRAY_LEN(inhibitors) && !in; i++)
			if (!inhibitors[i].used)
				in = &inhibitors[i];
		if (!in) {
			dbus_reply_error(b, call, "org.gnome.SessionManager.GeneralError", "Too many inhibitors");
			return true;
		}
		*in = (Inhibitor){ .used = true, .cookie = next_cookie++ };
		str_copy(in->app_id, dbus_str(dbus_arg(call, 0), ""), sizeof in->app_id);
		str_copy(in->reason, dbus_str(dbus_arg(call, 2), ""), sizeof in->reason);
		in->flags = (u32)dbus_uint(dbus_arg(call, 3), 0);
		str_copy(in->owner, dbus_sender(call), sizeof in->owner);
		char *p = inhibitor_path(in);
		dbus_export(b, p, INHIBITOR_IF, inhibitor_method, in, NULL);
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "u", in->cookie);
		dbus_send(b, m);
		DBusMsg *s = dbus_new_signal(SM_PATH, SM, "InhibitorAdded");
		dbus_add(s, "o", p);
		dbus_send(b, s);
		mem_free(p);
		sm_changed("InhibitedActions");
	} else if (str_eq(mbr, "Uninhibit")) {
		u32 cookie = (u32)dbus_uint(dbus_arg(call, 0), 0);
		for (usize i = 0; i < ARRAY_LEN(inhibitors); i++)
			if (inhibitors[i].used && inhibitors[i].cookie == cookie)
				inhibitor_drop(&inhibitors[i]);
		dbus_reply_empty(b, call);
	} else if (str_eq(mbr, "IsInhibited")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "b", (inhibited_actions() & (u32)dbus_uint(dbus_arg(call, 0), 0)) != 0);
		dbus_send(b, m);
	} else if (str_eq(mbr, "GetInhibitors")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "ao");
		for (usize i = 0; i < ARRAY_LEN(inhibitors); i++)
			if (inhibitors[i].used) {
				char *p = inhibitor_path(&inhibitors[i]);
				dbus_add(m, "o", p);
				mem_free(p);
			}
		dbus_close(m);
		dbus_send(b, m);
	} else if (str_eq(mbr, "CanShutdown") || str_eq(mbr, "CanReboot") || str_eq(mbr, "CanSuspend")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "u", 1u);
		dbus_send(b, m);
	} else if (str_eq(mbr, "CanRebootToFirmwareSetup")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "b", false);
		dbus_send(b, m);
	} else if (str_eq(mbr, "SetRebootToFirmwareSetup")) {
		dbus_reply_empty(b, call);
	} else if (str_eq(mbr, "Shutdown")) {
		dbus_reply_empty(b, call);
		ask_end(END_SHUTDOWN, true);
	} else if (str_eq(mbr, "Reboot")) {
		dbus_reply_empty(b, call);
		ask_end(END_REBOOT, true);
	} else if (str_eq(mbr, "Suspend")) {
		dbus_reply_empty(b, call);
		power("Suspend");
	} else if (str_eq(mbr, "Logout")) {
		/* 0: ask, 1: don't ask, 2: don't ask and don't wait */
		dbus_reply_empty(b, call);
		ask_end(END_LOGOUT, dbus_uint(dbus_arg(call, 0), 0) == 0);
	} else if (str_eq(mbr, "IsSessionRunning")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "b", running);
		dbus_send(b, m);
	} else {
		return false;
	}
	return true;
}

/* A client or an inhibitor's program left the bus. */
static void on_name_owner(DBus *b, DBusMsg *sig, void *data)
{
	(void)b, (void)data;
	const char *name = dbus_str(dbus_arg(sig, 0), ""), *now = dbus_str(dbus_arg(sig, 2), "");
	if (*now || name[0] != ':')
		return;
	for (usize i = 0; i < ARRAY_LEN(clients); i++)
		if (clients[i].used && str_eq(clients[i].owner, name))
			client_drop(&clients[i]);
	for (usize i = 0; i < ARRAY_LEN(inhibitors); i++)
		if (inhibitors[i].used && str_eq(inhibitors[i].owner, name))
			inhibitor_drop(&inhibitors[i]);
}

/* ---- Starting up ---------------------------------------------------------------------------------- */

static void session_ready(void)
{
	if (running)
		return;
	running = true;
	upload_environment();
	for (int phase = PHASE_INIT; phase < NPHASES; phase++)
		start_phase(phase);
	for (usize i = 0; i < autostarts.len; i++)
		if (autostarts.data[i].argv && autostarts.data[i].delay_ms)
			loop_timer(loop, autostarts.data[i].delay_ms, 0, start_delayed, &autostarts.data[i]);
	dbus_add_match(bus, "type='signal',interface='org.gnome.Mutter.IdleMonitor',member='WatchFired'");
	dbus_subscribe(bus, NULL, "/org/gnome/Mutter/IdleMonitor/Core", "org.gnome.Mutter.IdleMonitor", "WatchFired", on_watch_fired, NULL);
	watch_idle();
	dbus_send(bus, dbus_new_signal(SM_PATH, SM, "SessionRunning"));
	print("zsession: the session is running\n");
}

/* Without word from GNOME Shell after a while, its socket says it's up. */
static void check_ready(Loop *l, void *data)
{
	(void)l, (void)data;
	if (running)
		return;
	char *sock = str_fmt("%s/wayland-0", getenv_("XDG_RUNTIME_DIR"));
	bool up = file_exists(sock);
	mem_free(sock);
	if (up) {
		setenv_("WAYLAND_DISPLAY", "wayland-0");
		session_ready();
	}
}

static volatile int got_term, got_child;

static void on_signal(int sig)
{
	if (sig == SIGCHLD)
		got_child = 1;
	else
		got_term = 1;
	if (loop)
		loop_wake(loop);
}

static void reap(Loop *l, void *data)
{
	(void)data;
	if (got_term) {
		end_action = END_LOGOUT;
		finish_session();
		return;
	}
	got_child = 0;
	int st;
	int pid;
	while ((pid = (int)sys_wait4(-1, &st, WNOHANG, NULL)) > 0) {
		for (int i = 0; i < nprocs; i++) {
			Proc *p = &procs[i];
			if (p->pid != pid)
				continue;
			p->pid = 0;
			if (ending)
				break;
			bool crashed = (st & 0x7f) != 0 || ((st >> 8) & 0xff) != 0;
			if (p == shell) {
				/* the compositor: a crash gets one more try a minute, a clean exit ends the session */
				if (crashed && (time_mono_ms() - p->started_ms > 60000 || p->restarts < 1)) {
					p->restarts = time_mono_ms() - p->started_ms > 60000 ? 1 : p->restarts + 1;
					eprint("zsession: GNOME Shell stopped (%d); starting it again\n", st);
					proc_start(p);
				} else {
					end_action = END_LOGOUT;
					finish_session();
				}
			} else if (p->restart && crashed) {
				if (time_mono_ms() - p->started_ms > 60000)
					p->restarts = 0;
				if (p->restarts++ < 3)
					proc_start(p);
			}
			break;
		}
	}
	(void)l;
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "--headless") && i + 1 < argc)
			headless = argv[++i];
		else {
			eprint("usage: zsession [--headless WIDTHxHEIGHT]\n");
			return 2;
		}
	}
	for (char **e = environ; *e; e++) {
		env = mem_realloc(env, sizeof(char *) * (nenv + 2));
		env[nenv++] = *e;
		env[nenv] = NULL;
	}
	/* what a GNOME session's programs expect to find */
	setenv_("XDG_CURRENT_DESKTOP", "GNOME");
	setenv_("XDG_SESSION_DESKTOP", "gnome");
	setenv_("DESKTOP_SESSION", "gnome");
	setenv_("GNOME_SHELL_SESSION_MODE", "user");
	setenv_("XDG_SESSION_TYPE", "wayland");
	setenv_("XDG_SESSION_CLASS", "user");
	if (!getenv_("XDG_SESSION_ID"))
		setenv_("XDG_SESSION_ID", "1");
	if (!getenv_("XDG_SEAT"))
		setenv_("XDG_SEAT", "seat0");
	if (!getenv_("XDG_VTNR"))
		setenv_("XDG_VTNR", "1");
	if (!getenv_("DBUS_SYSTEM_BUS_ADDRESS"))
		setenv_("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket");
	setenv_("QT_QPA_PLATFORM", "wayland;xcb");
	setenv_("WAYLAND_DISPLAY", NULL);
	setenv_("DISPLAY", NULL);
	const char *rt = getenv_("XDG_RUNTIME_DIR");
	if (!rt) {
		eprint("zsession: no XDG_RUNTIME_DIR\n");
		return 1;
	}
	/* the Zurbax desktop's socket name is GNOME Shell's now */
	char *wl = str_fmt("%s/wayland-0", rt);
	struct stat st;
	if (sys_lstat(wl, &st) == 0 && S_ISLNK(st.st_mode))
		sys_unlink(wl);
	mem_free(wl);

	bus = dbus_open_session();
	if (!bus) {
		eprint("zsession: no session bus\n");
		return 1;
	}
	if (dbus_request_name(bus, SM, DBUS_NAME_NO_QUEUE) != DBUS_NAME_OWNER) {
		eprint("zsession: another session manager is running\n");
		return 1;
	}
	loop = loop_new();
	dbus_attach(bus, loop);
	dbus_export(bus, SM_PATH, SM, sm_method, NULL, NULL);
	dbus_export(bus, PRESENCE_PATH, PRESENCE_IF, presence_method, NULL, NULL);
	dbus_add_match(bus, "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged'");
	dbus_subscribe(bus, "org.freedesktop.DBus", NULL, "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner, NULL);
	sig_ignore(SIGPIPE);
	sig_handle(SIGCHLD, on_signal);
	sig_handle(SIGTERM, on_signal);
	sig_handle(SIGINT, on_signal);
	loop_on_wake(loop, reap, NULL);

	/* sound first, so the desktop finds it */
	char *pw = str_fmt("%s/pipewire-0", rt);
	if (!file_exists(pw) && (find_exe("pipewire") != NULL)) {
		run("pipewire", argv_of("pipewire", NULL), true);
		run("wireplumber", argv_of("wireplumber", NULL), true);
		run("pipewire-pulse", argv_of("pipewire-pulse", NULL), true);
	}
	mem_free(pw);

	load_autostart();
	settings_daemons();
	/* a headless session is a test's: unsafe mode lets it take screenshots */
	char **sh = headless ? argv_of("gnome-shell", "--headless", "--unsafe-mode", "--virtual-monitor", headless, NULL)
			     : argv_of("gnome-shell", "--display-server", "--wayland", NULL);
	shell = run("gnome-shell", sh, false);
	if (!shell || !shell->pid)
		return 1;
	loop_timer(loop, 1000, 1000, check_ready, NULL);
	loop_run(loop);

	/* the session ends: everything stops, then the computer does what was asked */
	for (int i = nprocs - 1; i >= 0; i--)
		if (procs[i].pid > 0 && &procs[i] != shell)
			sys_kill(procs[i].pid, SIGTERM);
	for (int t = 0; t < 30; t++) {
		bool left = false;
		while (sys_wait4(-1, NULL, WNOHANG, NULL) > 0)
			;
		for (int i = 0; i < nprocs; i++)
			if (procs[i].pid > 0 && &procs[i] != shell && sys_kill(procs[i].pid, 0) == 0)
				left = true;
		if (!left)
			break;
		sleep_ms(100);
	}
	if (shell->pid > 0) {
		sys_kill(shell->pid, SIGTERM);
		for (int t = 0; t < 30 && sys_kill(shell->pid, 0) == 0; t++) {
			sys_wait4(shell->pid, NULL, WNOHANG, NULL);
			sleep_ms(100);
		}
	}
	for (int i = 0; i < nprocs; i++)
		if (procs[i].pid > 0)
			sys_kill(procs[i].pid, SIGKILL);
	if (end_action == END_SHUTDOWN)
		power("PowerOff");
	else if (end_action == END_REBOOT)
		power("Reboot");
	dbus_close_bus(bus);
	return 0;
}
