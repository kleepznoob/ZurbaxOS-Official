/*
 * org.freedesktop.Accounts: the person who uses this computer, for GNOME's
 * Settings, its top bar menu and lock screen.
 *
 * Zurbax has one account, in /data/system/users.conf (name, full name,
 * ids) with the password apart in the root-only shadow file. What GNOME
 * keeps besides (language, formats, e-mail, picture, keyboard layouts) goes
 * to /data/system/profile.conf, and the picture to /data/system/icons.
 * org.zurbax.Accounts.ChangePassword lets the account's own programs (the
 * layer's passwd) change the password with the current one.
 */
#include "zsys.h"

#define ACCOUNTS "org.freedesktop.Accounts"
#define ACCOUNTS_PATH "/org/freedesktop/Accounts"
#define USER_IF "org.freedesktop.Accounts.User"
#define ZURBAX_IF "org.zurbax.Accounts"

static char user_path[96];

static Conf *profile(void)
{
	char *p = data_path("profile.conf");
	Conf *c = conf_load(p);
	mem_free(p);
	return c;
}

static const char *pget(const char *key, const char *fallback, char *buf, usize cap)
{
	Conf *c = profile();
	str_copy(buf, conf_get(c, key, fallback), cap);
	conf_free(c);
	return buf;
}

static void pset(const char *key, const char *value)
{
	Conf *c = profile();
	conf_set(c, key, value);
	char *dir = data_path("");
	dir_make(dir, 0755);
	mem_free(dir);
	conf_save(c);
	conf_free(c);
}

static char *icon_path(void)
{
	char *dir = data_path("icons");
	char *p = str_fmt("%s/%s", dir, Z.user);
	mem_free(dir);
	return p;
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

/* The uid of whoever sent a call (through its process). */
static u32 sender_uid(DBus *bus, DBusMsg *call)
{
	u32 pid = dbus_peer_pid(bus, dbus_sender(call));
	if (!pid)
		return (u32)-1;
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

static bool allowed(DBus *bus, DBusMsg *call)
{
	u32 uid = sender_uid(bus, call);
	if (uid == 0 || uid == Z.uid)
		return true;
	dbus_reply_error(bus, call, "org.freedesktop.Accounts.Error.PermissionDenied", "Not your account");
	return false;
}

/* ---- Properties -------------------------------------------------------------------------- */

#define GET(name) static void name(DBusMsg *m, void *obj)
#define PSTR(name, key, fallback) GET(name) { (void)obj; char b[512]; dbus_add(m, "s", pget(key, fallback, b, sizeof b)); }

GET(a_version) { (void)obj; dbus_add(m, "s", "23.13.9"); }
GET(a_false) { (void)obj; dbus_add(m, "b", false); }
GET(a_true) { (void)obj; dbus_add(m, "b", true); }
GET(a_autologin)
{
	(void)obj;
	dbus_open(m, "ao");
	char b[8];
	if (str_eq(pget("automatic_login", "false", b, sizeof b), "true"))
		dbus_add(m, "o", user_path);
	dbus_close(m);
}

static const Prop ACCOUNTS_PROPS[] = {
	{ "DaemonVersion", "s", a_version, NULL },
	{ "HasNoUsers", "b", a_false, NULL },
	{ "HasMultipleUsers", "b", a_false, NULL },
	{ "AutomaticLoginUsers", "ao", a_autologin, NULL },
};

GET(u_uid) { (void)obj; dbus_add(m, "t", (u64)Z.uid); }
GET(u_name) { (void)obj; dbus_add(m, "s", Z.user); }
GET(u_real) { (void)obj; dbus_add(m, "s", Z.full_name); }
GET(u_type) { (void)obj; dbus_add(m, "i", 1); }      /* the one account is the administrator */
GET(u_home) { (void)obj; dbus_add(m, "s", Z.home); }
GET(u_shell) { (void)obj; dbus_add(m, "s", "/bin/bash"); }
PSTR(u_email, "email", "")
PSTR(u_language, "language", "")
PSTR(u_formats, "formats_locale", "")
PSTR(u_session, "session", "gnome")
PSTR(u_session_type, "session_type", "wayland")
PSTR(u_location, "location", "")
PSTR(u_hint, "password_hint", "")
PSTR(u_xsession, "session", "")
GET(u_icon)
{
	(void)obj;
	char *p = icon_path();
	dbus_add(m, "s", file_exists(p) ? p : "");
	mem_free(p);
}
GET(u_background) { (void)obj; dbus_add(m, "s", ""); }
GET(u_input_sources)
{
	(void)obj;
	char b[256];
	pget("input_sources", "", b, sizeof b);
	dbus_open(m, "aa{ss}");
	usize n = 0;
	char **parts = str_split(b, ',', &n);
	for (usize i = 0; i < n; i++) {
		if (!parts[i][0])
			continue;
		dbus_open(m, "a{ss}");
		dbus_open(m, "{ss}");
		dbus_add(m, "ss", "xkb", parts[i]);
		dbus_close(m);
		dbus_close(m);
	}
	str_split_free(parts, n);
	dbus_close(m);
}
GET(u_layouts)
{
	(void)obj;
	char b[256];
	pget("input_sources", "", b, sizeof b);
	dbus_open(m, "as");
	usize n = 0;
	char **parts = str_split(b, ',', &n);
	for (usize i = 0; i < n; i++)
		if (parts[i][0])
			dbus_add(m, "s", parts[i]);
	str_split_free(parts, n);
	dbus_close(m);
}
GET(u_login_frequency) { (void)obj; dbus_add(m, "t", (u64)1); }
GET(u_login_time) { (void)obj; dbus_add(m, "x", Z.started_us / 1000000); }
GET(u_login_history) { (void)obj; dbus_open(m, "a(xxa{sv})"); dbus_close(m); }
GET(u_password_mode) { (void)obj; dbus_add(m, "i", has_password() ? 0 : 2); }   /* REGULAR or NONE */
GET(u_autologin)
{
	(void)obj;
	char b[8];
	dbus_add(m, "b", str_eq(pget("automatic_login", "false", b, sizeof b), "true"));
}

static const Prop USER_PROPS[] = {
	{ "Uid", "t", u_uid, NULL },
	{ "UserName", "s", u_name, NULL },
	{ "RealName", "s", u_real, NULL },
	{ "AccountType", "i", u_type, NULL },
	{ "HomeDirectory", "s", u_home, NULL },
	{ "Shell", "s", u_shell, NULL },
	{ "Email", "s", u_email, NULL },
	{ "Language", "s", u_language, NULL },
	{ "FormatsLocale", "s", u_formats, NULL },
	{ "Session", "s", u_session, NULL },
	{ "SessionType", "s", u_session_type, NULL },
	{ "InputSources", "aa{ss}", u_input_sources, NULL },
	{ "XSession", "s", u_xsession, NULL },
	{ "Location", "s", u_location, NULL },
	{ "LoginFrequency", "t", u_login_frequency, NULL },
	{ "LoginTime", "x", u_login_time, NULL },
	{ "LoginHistory", "a(xxa{sv})", u_login_history, NULL },
	{ "XHasMessages", "b", a_false, NULL },
	{ "XKeyboardLayouts", "as", u_layouts, NULL },
	{ "BackgroundFile", "s", u_background, NULL },
	{ "IconFile", "s", u_icon, NULL },
	{ "Saved", "b", a_true, NULL },
	{ "Locked", "b", a_false, NULL },
	{ "PasswordMode", "i", u_password_mode, NULL },
	{ "PasswordHint", "s", u_hint, NULL },
	{ "AutomaticLogin", "b", u_autologin, NULL },
	{ "SystemAccount", "b", a_false, NULL },
	{ "LocalAccount", "b", a_true, NULL },
};

/* Every property changed: AccountsService says so with Changed, and the
 * standard signal for the rest. */
static void user_changed(const char *prop)
{
	dbus_send(Z.bus, dbus_new_signal(user_path, USER_IF, "Changed"));
	const char *names[] = { prop, NULL };
	props_changed(Z.bus, user_path, USER_IF, USER_PROPS, ARRAY_LEN(USER_PROPS), NULL, names);
}

/* ---- Changes ------------------------------------------------------------------------------- */

static bool set_real_name(const char *name)
{
	char clean[96];
	str_copy(clean, name, sizeof clean);
	char *t = str_trim(clean);
	if (!*t || str_chr(t, '\n'))
		return false;
	char *p = data_path("users.conf");
	Conf *c = conf_load(p);
	conf_set(c, "full_name", t);
	bool ok = conf_save(c);
	conf_free(c);
	mem_free(p);
	if (ok)
		account_reload();
	return ok;
}

/* The picture: a copy of a file the person can read themselves. */
static bool set_icon(const char *file)
{
	char *dst = icon_path();
	bool ok = false;
	if (!*file) {
		sys_unlink(dst);
		ok = true;
	} else {
		int fd = (int)sys_open(file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0);
		struct stat st;
		if (fd >= 0 && sys_fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 4 * 1024 * 1024 &&
		    (st.st_uid == Z.uid || (st.st_mode & 0004))) {
			char *buf = mem_alloc((usize)st.st_size);
			long n = sys_read(fd, buf, (usize)st.st_size);
			if (n == st.st_size) {
				char *dir = path_dir(dst);
				dir_make(dir, 0755);
				mem_free(dir);
				ok = file_write_atomic(dst, buf, (usize)n);
				sys_chmod(dst, 0644);
			}
			mem_free(buf);
		}
		if (fd >= 0)
			sys_close(fd);
	}
	mem_free(dst);
	return ok;
}

static bool write_password(const char *record)
{
	char *p = data_path("shadow");
	Conf *sh = conf_load(p);
	conf_set(sh, "password", record);
	bool ok = conf_save(sh);
	sys_chmod(p, 0600);
	conf_free(sh);
	mem_free(p);
	/* the desktop's lock screen reads the account again */
	char *u = data_path("users.conf");
	Conf *c = conf_load(u);
	conf_set_bool(c, "has_password", record[0] != 0);
	conf_save(c);
	conf_free(c);
	mem_free(u);
	return ok;
}

static bool check_password(const char *pw)
{
	char *p = data_path("shadow");
	Conf *sh = conf_load(p);
	const char *rec = conf_get(sh, "password", "");
	bool ok = !rec[0] || password_check(pw, rec);
	conf_free(sh);
	mem_free(p);
	return ok;
}

bool accounts_check_password(const char *pw)
{
	return check_password(pw);
}

static u64 last_failure_ms;

static bool user_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, USER_IF, USER_PROPS, ARRAY_LEN(USER_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	const char *arg = dbus_str(dbus_arg(call, 0), "");
	if (str_eq(mbr, "GetPasswordExpirationPolicy")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "xxxxxx", (i64)0, (i64)0, (i64)99999, (i64)7, (i64)-1, (i64)-1);
		dbus_send(bus, m);
		return true;
	}
	if (!str_starts(mbr, "Set"))
		return false;
	if (!allowed(bus, call))
		return true;
	const char *prop = NULL;
	bool ok = true;
	if (str_eq(mbr, "SetRealName")) {
		ok = set_real_name(arg), prop = "RealName";
	} else if (str_eq(mbr, "SetEmail")) {
		pset("email", arg), prop = "Email";
	} else if (str_eq(mbr, "SetLanguage")) {
		pset("language", arg), prop = "Language";
	} else if (str_eq(mbr, "SetFormatsLocale")) {
		pset("formats_locale", arg), prop = "FormatsLocale";
	} else if (str_eq(mbr, "SetSession") || str_eq(mbr, "SetXSession")) {
		pset("session", arg), prop = "Session";
	} else if (str_eq(mbr, "SetSessionType")) {
		pset("session_type", arg), prop = "SessionType";
	} else if (str_eq(mbr, "SetLocation")) {
		pset("location", arg), prop = "Location";
	} else if (str_eq(mbr, "SetPasswordHint")) {
		pset("password_hint", arg), prop = "PasswordHint";
	} else if (str_eq(mbr, "SetIconFile")) {
		ok = set_icon(arg), prop = "IconFile";
	} else if (str_eq(mbr, "SetAutomaticLogin")) {
		pset("automatic_login", dbus_bool(dbus_arg(call, 0), false) ? "true" : "false"), prop = "AutomaticLogin";
	} else if (str_eq(mbr, "SetInputSources") || str_eq(mbr, "SetXKeyboardLayouts")) {
		/* aa{ss} of {"xkb": "us"} or as of layouts */
		StrBuf sb = { 0 };
		const DBusVal *v = dbus_arg(call, 0);
		for (usize i = 0; v && i < v->count; i++) {
			const DBusVal *e = v->items[i];
			const char *layout = e->type == 's' ? e->s : dbus_str(dbus_dict_get(e, "xkb"), "");
			if (*layout && !str_chr(layout, ','))
				sb_fmt(&sb, "%s%s", sb.len ? "," : "", layout);
		}
		pset("input_sources", sb_str(&sb));
		sb_free(&sb);
		prop = "InputSources";
	} else if (str_eq(mbr, "SetPasswordMode")) {
		/* 2 (none): signing in without a password */
		if (dbus_int(dbus_arg(call, 0), 0) == 2)
			ok = write_password("");
		prop = "PasswordMode";
	} else if (str_eq(mbr, "SetPassword")) {
		/* a crypt(3) record made by the caller; Zurbax's check knows SHA-512 crypt */
		const char *rec = arg;
		if (!str_starts(rec, "$6$")) {
			dbus_reply_error(bus, call, "org.freedesktop.Accounts.Error.Failed", "Use SHA-512 crypt for the password");
			return true;
		}
		ok = write_password(rec);
		pset("password_hint", dbus_str(dbus_arg(call, 1), ""));
		prop = "PasswordMode";
	} else if (str_eq(mbr, "SetAccountType") || str_eq(mbr, "SetLocked") || str_eq(mbr, "SetShell") ||
		   str_eq(mbr, "SetHomeDirectory") || str_eq(mbr, "SetUserName") || str_eq(mbr, "SetXHasMessages") ||
		   str_eq(mbr, "SetBackgroundFile")) {
		/* fixed on Zurbax: one administrator account, its folder and its shell */
		dbus_reply_empty(bus, call);
		return true;
	} else {
		return false;
	}
	if (!ok) {
		dbus_reply_error(bus, call, "org.freedesktop.Accounts.Error.Failed", "Couldn't save that");
		return true;
	}
	dbus_reply_empty(bus, call);
	user_changed(prop);
	return true;
}

/* org.zurbax.Accounts, for the passwd program: CheckPassword(current)
 * answers whether it's right (an empty one is right only for an account
 * without a password), ChangePassword(current, new) changes it. */
static bool zurbax_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	const char *mbr = dbus_member(call);
	bool check = str_eq(mbr, "CheckPassword");
	if (!check && !str_eq(mbr, "ChangePassword"))
		return false;
	if (!allowed(bus, call))
		return true;
	const char *cur = dbus_str(dbus_arg(call, 0), ""), *next = dbus_str(dbus_arg(call, 1), "");
	/* a pause after a wrong answer keeps guessing slow (asking whether
	 * there is a password at all isn't a guess) */
	if (cur[0] && time_mono_ms() - last_failure_ms < 2000) {
		dbus_reply_error(bus, call, "org.zurbax.Accounts.Error.TryLater", "Wait a moment and try again");
		return true;
	}
	if (!check_password(cur)) {
		if (cur[0])
			last_failure_ms = time_mono_ms();
		dbus_reply_error(bus, call, "org.zurbax.Accounts.Error.WrongPassword", "The current password isn't right");
		return true;
	}
	if (check) {
		dbus_reply_empty(bus, call);
		return true;
	}
	char *rec = next[0] ? password_hash(next) : str_dup("");
	bool ok = write_password(rec);
	mem_free(rec);
	if (!ok) {
		dbus_reply_error(bus, call, "org.freedesktop.Accounts.Error.Failed", "Couldn't save the password");
		return true;
	}
	dbus_reply_empty(bus, call);
	user_changed("PasswordMode");
	return true;
}

static bool is_us(const DBusVal *v)
{
	return v && (v->type == 's' ? str_eq(v->s, Z.user) : dbus_uint(v, (u64)-1) == Z.uid);
}

static bool accounts_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, ACCOUNTS, ACCOUNTS_PROPS, ARRAY_LEN(ACCOUNTS_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "ListCachedUsers")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "ao");
		dbus_add(m, "o", user_path);
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "FindUserById") || str_eq(mbr, "FindUserByName") || str_eq(mbr, "CacheUser")) {
		if (is_us(dbus_arg(call, 0))) {
			DBusMsg *m = dbus_new_reply(call);
			dbus_add(m, "o", user_path);
			dbus_send(bus, m);
		} else {
			dbus_reply_error(bus, call, "org.freedesktop.Accounts.Error.Failed", "No such user");
		}
	} else if (str_eq(mbr, "UncacheUser")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "GetUsersLanguages")) {
		char b[64];
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "as");
		if (*pget("language", "", b, sizeof b))
			dbus_add(m, "s", b);
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "CreateUser") || str_eq(mbr, "DeleteUser")) {
		dbus_reply_error(bus, call, "org.freedesktop.Accounts.Error.NotSupported", "Zurbax has one account");
	} else {
		return false;
	}
	return true;
}

static const char ACCOUNTS_XML[] =
	"<interface name=\"org.freedesktop.Accounts\">"
	"<method name=\"ListCachedUsers\"><arg type=\"ao\" direction=\"out\"/></method>"
	"<method name=\"FindUserById\"><arg type=\"x\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"FindUserByName\"><arg type=\"s\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"CacheUser\"><arg type=\"s\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"UncacheUser\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"GetUsersLanguages\"><arg type=\"as\" direction=\"out\"/></method>"
	"<signal name=\"UserAdded\"><arg type=\"o\"/></signal>"
	"<signal name=\"UserDeleted\"><arg type=\"o\"/></signal>"
	"<property name=\"DaemonVersion\" type=\"s\" access=\"read\"/>"
	"<property name=\"HasNoUsers\" type=\"b\" access=\"read\"/>"
	"<property name=\"HasMultipleUsers\" type=\"b\" access=\"read\"/>"
	"<property name=\"AutomaticLoginUsers\" type=\"ao\" access=\"read\"/>"
	"</interface>";

static const char USER_XML[] =
	"<interface name=\"org.freedesktop.Accounts.User\">"
	"<method name=\"SetRealName\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetEmail\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetLanguage\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetFormatsLocale\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetInputSources\"><arg type=\"aa{ss}\" direction=\"in\"/></method>"
	"<method name=\"SetSession\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetSessionType\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetLocation\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetIconFile\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetPassword\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetPasswordHint\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"SetPasswordMode\"><arg type=\"i\" direction=\"in\"/></method>"
	"<method name=\"SetAutomaticLogin\"><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"GetPasswordExpirationPolicy\"><arg type=\"x\" direction=\"out\"/><arg type=\"x\" direction=\"out\"/>"
	"<arg type=\"x\" direction=\"out\"/><arg type=\"x\" direction=\"out\"/><arg type=\"x\" direction=\"out\"/><arg type=\"x\" direction=\"out\"/></method>"
	"<signal name=\"Changed\"/>"
	"</interface>";

static const char ZURBAX_XML[] =
	"<interface name=\"org.zurbax.Accounts\">"
	"<method name=\"CheckPassword\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"ChangePassword\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
	"</interface>";

void accounts_start(DBus *bus)
{
	fmt(user_path, sizeof user_path, "/org/freedesktop/Accounts/User%u", Z.uid);
	dbus_export(bus, ACCOUNTS_PATH, ACCOUNTS, accounts_method, NULL, ACCOUNTS_XML);
	dbus_export(bus, user_path, USER_IF, user_method, NULL, USER_XML);
	dbus_export(bus, ACCOUNTS_PATH, ZURBAX_IF, zurbax_method, NULL, ZURBAX_XML);
	dbus_request_name(bus, ACCOUNTS, DBUS_NAME_NO_QUEUE);
}
