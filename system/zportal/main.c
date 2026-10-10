/*
 * zportal: the desktop services standard Linux programs ask the desktop
 * for over D-Bus, answered by ZOS. Flatpak apps can't reach most of the
 * system from their sandbox; these are how they open a file the user picks,
 * show a notification, follow the dark style, open a link in the browser,
 * write an email, save a secret, keep the screen on, and so on.
 *
 *   org.freedesktop.portal.Desktop      the portals (portals.c): Settings,
 *                                       FileChooser, OpenURI, Email,
 *                                       Notification, Inhibit, Account,
 *                                       Trash, Secret, Wallpaper, Screenshot,
 *                                       Background, NetworkMonitor,
 *                                       ProxyResolver, GameMode, monitors
 *   org.freedesktop.portal.Documents    files handed into sandboxes (documents.c)
 *   org.freedesktop.Notifications       notifications of programs outside
 *                                       Flatpak (notify.c)
 *   org.freedesktop.ScreenSaver         keeping the screen on (notify.c)
 *
 * zdisplay starts it for the user. It waits for the session bus of the
 * user's Linux session (layerrun), which comes and goes, and talks to
 * zdisplay over its app socket like an app without a window. `zportal pick`
 * is the file chooser window it opens (picker.c).
 */
#include "zportal.h"

Portal P;

/* ---- zdisplay ------------------------------------------------------------------------------ */

static void display_send(void *msg, u16 type, usize size)
{
	if (P.display < 0)
		return;
	MsgHeader *h = msg;
	h->type = type;
	h->size = (u16)size;
	h->win = 0;
	if (unix_send(P.display, msg, size, NULL, 0) < 0) {
		loop_del_fd(P.loop, P.display);
		sys_close(P.display);
		P.display = -1;
	}
}

void display_notify(const char *icon, const char *title, const char *body)
{
	MsgNotify m = { 0 };
	str_copy(m.icon, icon && *icon ? icon : "zportal", sizeof m.icon);
	str_copy(m.title, title ? title : "", sizeof m.title);
	str_copy(m.body, body ? body : "", sizeof m.body);
	display_send(&m, MSG_NOTIFY, sizeof m);
}

void display_launch(const char *app_id, const char *arg)
{
	MsgLaunch m = { 0 };
	str_copy(m.app_id, app_id ? app_id : "", sizeof m.app_id);
	str_copy(m.arg, arg ? arg : "", sizeof m.arg);
	display_send(&m, MSG_LAUNCH, sizeof m);
}

void display_settings_changed(void)
{
	MsgHeader m = { 0 };
	display_send(&m, MSG_SETTINGS_CHANGED, sizeof m);
}

static void on_display(Loop *loop, int fd, u32 events, void *data)
{
	(void)loop;
	(void)data;
	u8 buf[ZUI_MAX_MSG];
	int fds[4], nfds = 0;
	long n = unix_recv(fd, buf, sizeof buf, fds, 4, &nfds);
	for (int i = 0; i < nfds; i++)
		sys_close(fds[i]);
	if (n <= 0 || (events & (EPOLLHUP | EPOLLERR))) {
		/* the desktop ended: zportal goes with it (the next one starts its own) */
		eprint("zportal: the desktop went away\n");
		exit(0);
	}
	const MsgHeader *h = (const MsgHeader *)buf;
	if (h->type == MSG_WELCOME && (usize)n >= sizeof(MsgWelcome)) {
		const MsgWelcome *w = (const MsgWelcome *)buf;
		str_copy(P.full_name, w->user_name, sizeof P.full_name);
	} else if (h->type == MSG_THEME) {
		/* the look changed: tell the programs that follow it */
		settings_poll();
	}
}

static void display_connect(void)
{
	if (P.display >= 0)
		return;
	const char *path = env_get("ZUI_SOCKET");
	int fd = unix_connect(path && *path ? path : ZUI_SOCKET_PATH, SOCK_SEQPACKET);
	if (fd < 0)
		return;
	P.display = fd;
	MsgHello hello = { .version = ZUI_PROTOCOL_VERSION, .pid = (u32)sys_getpid() };
	str_copy(hello.app_id, "zportal", sizeof hello.app_id);
	display_send(&hello, MSG_HELLO, sizeof hello);
	if (P.display >= 0)
		loop_add_fd(P.loop, fd, EPOLLIN, on_display, NULL);
}

/* ---- Callers --------------------------------------------------------------------------------- */

typedef struct {
	char sender[64];
	char app[128];
	bool sandboxed;
} Caller;

static Caller callers[64];
static int ncallers;

/* Flatpak puts a description of the sandbox at its root, .flatpak-info:
 * "[Application]\nname=org.mozilla.firefox\n..." */
const char *caller_app(const char *sender, bool *sandboxed)
{
	if (!sender || !P.bus) {
		if (sandboxed)
			*sandboxed = false;
		return "";
	}
	for (int i = 0; i < ncallers; i++)
		if (str_eq(callers[i].sender, sender)) {
			if (sandboxed)
				*sandboxed = callers[i].sandboxed;
			return callers[i].app;
		}
	Caller *c = &callers[ncallers < (int)ARRAY_LEN(callers) ? ncallers++ : (int)(rand_u32() % ARRAY_LEN(callers))];
	memset(c, 0, sizeof *c);
	str_copy(c->sender, sender, sizeof c->sender);
	u32 pid = dbus_peer_pid(P.bus, sender);
	if (pid) {
		char path[64];
		fmt(path, sizeof path, "/proc/%u/root/.flatpak-info", pid);
		char *info = file_read(path, NULL);
		if (info) {
			c->sandboxed = true;
			bool app = false;
			usize n = 0;
			char **lines = str_split(info, '\n', &n);
			for (usize i = 0; i < n; i++) {
				char *l = str_trim(lines[i]);
				if (l[0] == '[')
					app = str_eq(l, "[Application]") || str_eq(l, "[Runtime]");
				else if (app && str_starts(l, "name="))
					str_copy(c->app, l + 5, sizeof c->app);
			}
			str_split_free(lines, n);
			mem_free(info);
		}
	}
	if (sandboxed)
		*sandboxed = c->sandboxed;
	return c->app;
}

/* ---- Requests -------------------------------------------------------------------------------- */

static const char REQUEST_XML[] = "<interface name=\"org.freedesktop.portal.Request\"><method name=\"Close\"/>"
				  "<signal name=\"Response\"><arg type=\"u\" name=\"response\"/>"
				  "<arg type=\"a{sv}\" name=\"results\"/></signal></interface>";

static void request_free(Request *r)
{
	if (P.bus)
		dbus_unexport(P.bus, r->path, "org.freedesktop.portal.Request");
	if (r->out_fd >= 0) {
		loop_del_fd(P.loop, r->out_fd);
		sys_close(r->out_fd);
	}
	sb_free(&r->out);
	mem_free(r->path);
	mem_free(r->sender);
	mem_free(r);
}

static bool request_method(DBus *bus, DBusMsg *call, void *data)
{
	Request *r = data;
	if (!dbus_is_call(call, "org.freedesktop.portal.Request", "Close"))
		return false;
	dbus_reply_empty(bus, call);
	if (r->closed)
		r->closed(r);
	if (r->pid > 0)
		sys_kill(r->pid, SIGTERM);
	request_free(r);
	return true;
}

/* The caller can know the path in advance (to subscribe to Response before
 * calling): /org/freedesktop/portal/desktop/request/SENDER/TOKEN, the
 * sender without its colon and with dots as underscores. */
Request *request_new(DBusMsg *call, const DBusVal *options)
{
	Request *r = NEW(Request);
	r->out_fd = -1;
	const char *sender = dbus_sender(call) ? dbus_sender(call) : ":0.0";
	r->sender = str_dup(sender);
	char token[64];
	const char *t = dbus_str(dbus_dict_get(options, "handle_token"), NULL);
	if (t)
		str_copy(token, t, sizeof token);
	else
		fmt(token, sizeof token, "zos%u", ++P.next_token);
	StrBuf p = { 0 };
	sb_adds(&p, "/org/freedesktop/portal/desktop/request/");
	for (const char *c = sender + (sender[0] == ':'); *c; c++)
		sb_addc(&p, *c == '.' ? '_' : *c);
	sb_addc(&p, '/');
	for (const char *c = token; *c; c++)
		sb_addc(&p, is_alnum(*c) || *c == '_' ? *c : '_');
	r->path = sb_take(&p);
	dbus_export(P.bus, r->path, "org.freedesktop.portal.Request", request_method, r, REQUEST_XML);
	return r;
}

void request_reply_handle(Request *r, DBusMsg *call)
{
	DBusMsg *m = dbus_new_reply(call);
	dbus_add(m, "o", r->path);
	dbus_send(P.bus, m);
}

DBusMsg *request_response(Request *r, u32 code)
{
	DBusMsg *m = dbus_new_signal(r->path, "org.freedesktop.portal.Request", "Response");
	dbus_set_destination(m, r->sender);
	dbus_add(m, "u", code);
	dbus_open(m, "a{sv}");
	return m;
}

void request_finish(Request *r, DBusMsg *response)
{
	dbus_close(response);
	if (P.bus)
		dbus_send(P.bus, response);
	else
		dbus_msg_free(response);
	request_free(r);
}

void request_respond_empty(Request *r, u32 code)
{
	request_finish(r, request_response(r, code));
}

bool reply_version(DBus *bus, DBusMsg *call, u32 version)
{
	if (dbus_is_call(call, "org.freedesktop.DBus.Properties", "Get")) {
		if (!str_eq(dbus_str(dbus_arg(call, 1), ""), "version"))
			return false;
		dbus_reply_property(bus, call, "u", version);
		return true;
	}
	if (dbus_is_call(call, "org.freedesktop.DBus.Properties", "GetAll")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "a{sv}");
		dbus_dict_add(m, "version", "u", version);
		dbus_close(m);
		dbus_send(bus, m);
		return true;
	}
	return false;
}

/* ---- URIs ------------------------------------------------------------------------------------------- */

char *path_to_uri(const char *path)
{
	StrBuf sb = { 0 };
	sb_adds(&sb, "file://");
	for (const u8 *c = (const u8 *)path; *c; c++) {
		if (is_alnum(*c) || str_chr("/-._~!$&'()*+,;=:@", *c))
			sb_addc(&sb, (char)*c);
		else
			sb_fmt(&sb, "%%%02X", *c);
	}
	return sb_take(&sb);
}

char *uri_to_path(const char *uri)
{
	if (!str_starts(uri, "file://"))
		return NULL;
	const char *p = uri + 7;
	if (*p != '/') {
		p = str_chr(p, '/'); /* file://host/path */
		if (!p)
			return NULL;
	}
	StrBuf sb = { 0 };
	for (; *p; p++) {
		if (*p == '%' && is_xdigit(p[1]) && is_xdigit(p[2])) {
			int v = 0;
			for (int i = 1; i <= 2; i++)
				v = v * 16 + (is_digit(p[i]) ? p[i] - '0' : to_lower(p[i]) - 'a' + 10);
			sb_addc(&sb, (char)v);
			p += 2;
		} else {
			sb_addc(&sb, *p);
		}
	}
	return sb_take(&sb);
}

/* ---- The bus ----------------------------------------------------------------------------------------- */

static void on_bus_lost(DBus *bus, void *data)
{
	(void)bus;
	(void)data;
	P.bus_lost = true;
}

static void bus_check(Loop *loop, void *data)
{
	(void)loop;
	(void)data;
	if (P.bus_lost) {
		dbus_close_bus(P.bus);
		P.bus = NULL;
		P.bus_lost = false;
		ncallers = 0;
		eprint("zportal: the session bus went away\n");
	}
	display_connect();
	if (!P.bus) {
		char *path = str_fmt("%s/bus", P.rt);
		bool there = file_exists(path);
		char *addr = str_fmt("unix:path=%s", path);
		mem_free(path);
		if (there) {
			P.bus = dbus_open_address(addr);
			if (P.bus) {
				dbus_on_disconnect(P.bus, on_bus_lost, NULL);
				dbus_attach(P.bus, P.loop);
				/* the Desktop name last: programs (and the session, which
				 * waits for it) find everything else ready once it's there */
				documents_start(P.bus);
				notifications_start(P.bus);
				portals_start(P.bus);
				eprint("zportal: serving the session bus as %s\n", dbus_unique_name(P.bus));
			}
		}
		mem_free(addr);
	}
	if (P.bus && !P.bus_lost)
		documents_tick(P.bus);
	settings_poll();
	network_poll();
}

/* Helpers (the file chooser) that end: their output answers the request (portals.c). */
void helpers_reap(void);

static void reap_timer(Loop *loop, void *data)
{
	(void)loop;
	(void)data;
	helpers_reap();
}

int main(int argc, char **argv)
{
	if (argc > 1 && str_eq(argv[1], "pick"))
		return picker_main(argc, argv);
	sig_ignore(SIGPIPE);
	P.uid = (u32)sys_getuid();
	P.gid = (u32)sys_getgid();
	P.display = -1;
	const char *home = env_get("HOME"), *user = env_get("USER");
	str_copy(P.home, home && *home ? home : "/", sizeof P.home);
	str_copy(P.user, user && *user ? user : "user", sizeof P.user);
	str_copy(P.full_name, P.user, sizeof P.full_name);
	const char *x = env_get("XDG_RUNTIME_DIR");
	if (x && *x && !str_eq(x, "/run/zurbax"))
		str_copy(P.rt, x, sizeof P.rt);
	else
		fmt(P.rt, sizeof P.rt, "/run/user/%u", P.uid);
	P.loop = loop_new();
	loop_timer(P.loop, 1, 400, bus_check, NULL);
	loop_timer(P.loop, 200, 200, reap_timer, NULL);
	loop_run(P.loop);
	return 0;
}
