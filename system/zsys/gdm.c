/*
 * org.gnome.DisplayManager: unlocking the screen.
 *
 * GNOME Shell's lock screen checks the password through its display
 * manager: it asks for a reauthentication channel (a private D-Bus
 * connection to a socket the display manager names), then holds a
 * conversation on it: begin verifying the user, answer the password
 * question, wait for "complete" or "failed". Zurbax has no GDM; zsys
 * opens the channel itself and checks the password against the account,
 * the way the Zurbax lock screen does.
 *
 * The channel is peer to peer (no bus in between): after the SASL
 * handshake the two ends exchange D-Bus messages directly.
 */
#include "zsys.h"

#define GDM "org.gnome.DisplayManager"
#define MANAGER_PATH "/org/gnome/DisplayManager/Manager"
#define MANAGER_IF "org.gnome.DisplayManager.Manager"
#define SESSION_PATH "/org/gnome/DisplayManager/Session"
#define VERIFIER_IF "org.gnome.DisplayManager.UserVerifier"
#define PASSWORD_SERVICE "gdm-password"

typedef struct {
	int fd;
	bool authed, began;         /* SASL done; BEGIN received */
	u8 *buf;
	usize len, cap;
	u32 serial;
	bool verifying;             /* the password question is out */
} Peer;

static Peer *peers[4];
static u64 last_failure_ms;

static void peer_close(Peer *p)
{
	for (usize i = 0; i < ARRAY_LEN(peers); i++)
		if (peers[i] == p)
			peers[i] = NULL;
	loop_del_fd(Z.loop, p->fd);
	sys_close(p->fd);
	mem_free(p->buf);
	mem_free(p);
}

static void peer_send(Peer *p, DBusMsg *m)
{
	usize len = 0;
	u8 *bytes = dbus_msg_encode(m, ++p->serial, &len);
	if (bytes) {
		write_all(p->fd, bytes, len);
		mem_free(bytes);
	}
	dbus_msg_free(m);
}

static void emit(Peer *p, const char *member, const char *service, const char *text)
{
	DBusMsg *m = dbus_new_signal(SESSION_PATH, VERIFIER_IF, member);
	if (service)
		dbus_add(m, "s", service);
	if (text)
		dbus_add(m, "s", text);
	peer_send(p, m);
}

static void reply_empty(Peer *p, DBusMsg *call)
{
	if (!(dbus_flags(call) & DBUS_NO_REPLY))
		peer_send(p, dbus_new_reply(call));
}

static void reply_error(Peer *p, DBusMsg *call, const char *name, const char *text)
{
	peer_send(p, dbus_new_error(call, name, text));
}

static bool account_has_password(void)
{
	char *path = data_path("shadow");
	Conf *sh = conf_load(path);
	bool has = conf_get(sh, "password", "")[0] != 0;
	conf_free(sh);
	mem_free(path);
	return has;
}

/* Starts the password conversation (or finishes it at once without a password). */
static void begin(Peer *p, DBusMsg *call, const char *service, const char *user)
{
	if (!str_eq(service, PASSWORD_SERVICE)) {
		/* fingerprints and smart cards: not on Zurbax yet */
		reply_error(p, call, "org.gnome.DisplayManager.SessionWorker.Error.ServiceUnavailable", "Not available");
		return;
	}
	if (*user && !str_eq(user, Z.user)) {
		reply_error(p, call, "org.gnome.DisplayManager.SessionWorker.Error.Generic", "Only the account's owner can unlock");
		return;
	}
	reply_empty(p, call);
	emit(p, "ConversationStarted", service, NULL);
	if (!account_has_password()) {
		emit(p, "VerificationComplete", service, NULL);
		return;
	}
	p->verifying = true;
	emit(p, "SecretInfoQuery", service, "Password");
}

static void answer(Peer *p, DBusMsg *call, const char *service, const char *text)
{
	reply_empty(p, call);
	if (!p->verifying || !str_eq(service, PASSWORD_SERVICE))
		return;
	p->verifying = false;
	/* a wrong answer slows the next one down */
	u64 now = time_mono_ms();
	bool ok = now - last_failure_ms >= 1000 && accounts_check_password(text);
	if (ok) {
		emit(p, "VerificationComplete", service, NULL);
		return;
	}
	last_failure_ms = now;
	emit(p, "Problem", service, "Sorry, that didn’t work. Please try again.");
	emit(p, "VerificationFailed", service, NULL);
	emit(p, "ConversationStopped", service, NULL);
}

static void handle(Peer *p, DBusMsg *m)
{
	if (dbus_type(m) != DBUS_CALL)
		return;
	const char *iface = dbus_interface(m) ? dbus_interface(m) : "", *mbr = dbus_member(m);
	if (str_eq(iface, "org.freedesktop.DBus.Properties")) {
		/* the proxies read the interfaces' properties: there are none */
		DBusMsg *r = dbus_new_reply(m);
		if (str_eq(mbr, "GetAll")) {
			dbus_open(r, "a{sv}");
			dbus_close(r);
			peer_send(p, r);
		} else {
			dbus_msg_free(r);
			reply_error(p, m, "org.freedesktop.DBus.Error.UnknownProperty", "No such property");
		}
	} else if (str_eq(iface, "org.freedesktop.DBus")) {
		/* a client that takes the channel for a bus (gdbus) says hello first */
		DBusMsg *r = dbus_new_reply(m);
		if (str_eq(mbr, "Hello"))
			dbus_add(r, "s", ":1.1");
		peer_send(p, r);
	} else if (str_eq(iface, "org.freedesktop.DBus.Peer")) {
		DBusMsg *r = dbus_new_reply(m);
		if (str_eq(mbr, "GetMachineId"))
			dbus_add(r, "s", "00000000000000000000000000000000");
		peer_send(p, r);
	} else if (str_eq(mbr, "EnableExtensions")) {
		reply_empty(p, m);
	} else if (str_eq(mbr, "BeginVerification")) {
		begin(p, m, dbus_str(dbus_arg(m, 0), ""), "");
	} else if (str_eq(mbr, "BeginVerificationForUser")) {
		begin(p, m, dbus_str(dbus_arg(m, 0), ""), dbus_str(dbus_arg(m, 1), ""));
	} else if (str_eq(mbr, "AnswerQuery")) {
		answer(p, m, dbus_str(dbus_arg(m, 0), ""), dbus_str(dbus_arg(m, 1), ""));
	} else if (str_eq(mbr, "Cancel")) {
		p->verifying = false;
		reply_empty(p, m);
	} else {
		reply_error(p, m, "org.freedesktop.DBus.Error.UnknownMethod", "No such method");
	}
}

/* ---- The handshake (SASL), then messages ----------------------------------------------- */

static void sasl_line(Peer *p, const char *line)
{
	static const char guid[] = "5a75726261782d7a7379732d67646d31";
	char out[96];
	if (str_starts(line, "AUTH EXTERNAL")) {
		const char *arg = line + 13;
		while (*arg == ' ')
			arg++;
		if (!*arg) {
			write_all(p->fd, "DATA\r\n", 6);
			return;
		}
		p->authed = true;
		fmt(out, sizeof out, "OK %s\r\n", guid);
		write_all(p->fd, out, strlen(out));
	} else if (str_starts(line, "DATA")) {
		p->authed = true;
		fmt(out, sizeof out, "OK %s\r\n", guid);
		write_all(p->fd, out, strlen(out));
	} else if (str_starts(line, "NEGOTIATE_UNIX_FD")) {
		write_all(p->fd, "AGREE_UNIX_FD\r\n", 15);
	} else if (str_starts(line, "BEGIN")) {
		p->began = p->authed;
	} else if (str_starts(line, "CANCEL") || str_starts(line, "ERROR")) {
		write_all(p->fd, "REJECTED EXTERNAL\r\n", 19);
	} else {
		write_all(p->fd, "REJECTED EXTERNAL\r\n", 19);
	}
}

static u32 rd32(const u8 *b, bool big)
{
	return big ? (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3] : (u32)b[3] << 24 | (u32)b[2] << 16 | (u32)b[1] << 8 | b[0];
}

static void consume(Peer *p, usize n)
{
	memmove(p->buf, p->buf + n, p->len - n);
	p->len -= n;
}

static void on_peer(Loop *loop, int fd, u32 events, void *data)
{
	(void)loop, (void)fd;
	Peer *p = data;
	if (p->cap - p->len < 4096) {
		p->cap = p->cap ? p->cap * 2 : 8192;
		if (p->cap > 1 << 20) {
			peer_close(p);
			return;
		}
		p->buf = mem_realloc(p->buf, p->cap);
	}
	long n = sys_read(p->fd, p->buf + p->len, p->cap - p->len);
	if (n <= 0 && n != -EAGAIN && n != -EINTR) {
		peer_close(p);
		return;
	}
	if (n > 0)
		p->len += (usize)n;
	(void)events;
	/* the handshake: a NUL byte, then lines */
	while (!p->began && p->len) {
		if (p->buf[0] == 0) {
			consume(p, 1);
			continue;
		}
		u8 *nl = memchr(p->buf, '\n', p->len);
		if (!nl)
			return;
		usize ll = (usize)(nl - p->buf) + 1;
		char line[512];
		usize copy = MIN(ll, sizeof line - 1);
		memcpy(line, p->buf, copy);
		line[copy] = 0;
		for (char *c = line; *c; c++)
			if (*c == '\r' || *c == '\n')
				*c = 0;
		consume(p, ll);
		sasl_line(p, line);
	}
	/* messages: a 16-byte fixed header says how long each is */
	while (p->began && p->len >= 16) {
		bool big = p->buf[0] == 'B';
		u32 body = rd32(p->buf + 4, big), fields = rd32(p->buf + 12, big);
		usize total = ((16 + (usize)fields + 7) & ~(usize)7) + body;
		if (total > (1u << 20)) {
			peer_close(p);
			return;
		}
		if (p->len < total)
			return;
		DBusMsg *m = dbus_msg_decode(p->buf, total);
		consume(p, total);
		if (m) {
			handle(p, m);
			dbus_msg_free(m);
		}
	}
}

/* ---- The channel ------------------------------------------------------------------------- */

typedef struct {
	int fd;
	char path[128];
	u32 timer;
} Listener;

static void on_accept(Loop *loop, int fd, u32 events, void *data)
{
	(void)events;
	Listener *l = data;
	int c = (int)sys_accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	/* one connection per channel: it closes once used */
	loop_del_fd(loop, fd);
	sys_close(fd);
	sys_unlink(l->path);
	if (l->timer)
		loop_cancel(loop, l->timer);
	mem_free(l);
	if (c < 0)
		return;
	struct ucred cred = { 0 };
	u32 clen = sizeof cred;
	if (sys_getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0 || (cred.uid != Z.uid && cred.uid != 0)) {
		sys_close(c);
		return;
	}
	Peer *p = mem_alloc(sizeof *p);
	memset(p, 0, sizeof *p);
	p->fd = c;
	usize slot = ARRAY_LEN(peers);
	for (usize i = 0; i < ARRAY_LEN(peers); i++)
		if (!peers[i]) {
			slot = i;
			break;
		}
	if (slot == ARRAY_LEN(peers)) {
		peer_close(peers[0]);
		slot = 0;
	}
	peers[slot] = p;
	loop_add_fd(loop, c, EPOLLIN, on_peer, p);
}

/* An unused channel goes away after a minute. */
static void expire(Loop *loop, void *data)
{
	Listener *l = data;
	loop_del_fd(loop, l->fd);
	sys_close(l->fd);
	sys_unlink(l->path);
	mem_free(l);
}

static u32 sender_uid(DBus *bus, DBusMsg *call)
{
	u32 pid = dbus_peer_pid(bus, dbus_sender(call));
	if (!pid)
		return (u32)-1;
	char path[64];
	fmt(path, sizeof path, "/proc/%u/status", pid);
	char *t = file_read(path, NULL);
	const char *u = t ? str_find(t, "\nUid:") : NULL;
	u32 uid = (u32)-1;
	if (u) {
		u += 5;
		while (*u == ' ' || *u == '\t')
			u++;
		uid = (u32)str_to_int(u, NULL);
	}
	mem_free(t);
	return uid;
}

static bool manager_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "OpenReauthenticationChannel")) {
		const char *user = dbus_str(dbus_arg(call, 0), "");
		u32 uid = sender_uid(bus, call);
		if ((uid != Z.uid && uid != 0) || (*user && !str_eq(user, Z.user))) {
			dbus_reply_error(bus, call, "org.gnome.DisplayManager.Manager.Error.AccessDenied", "Not your session");
			return true;
		}
		Listener *l = mem_alloc(sizeof *l);
		memset(l, 0, sizeof *l);
		u8 r[8];
		rand_bytes(r, sizeof r);
		dir_make("/run/zurbax/gdm", 0711);
		fmt(l->path, sizeof l->path, "/run/zurbax/gdm/reauth-%02x%02x%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5],
		    r[6], r[7]);
		l->fd = unix_listen(l->path, SOCK_STREAM);
		if (l->fd < 0) {
			mem_free(l);
			dbus_reply_error(bus, call, "org.gnome.DisplayManager.Manager.Error.Failed", "Couldn't open a channel");
			return true;
		}
		/* only the account's owner may connect */
		sys_chown(l->path, Z.uid, Z.gid);
		sys_chmod(l->path, 0600);
		loop_add_fd(Z.loop, l->fd, EPOLLIN, on_accept, l);
		l->timer = loop_timer(Z.loop, 60000, 0, expire, l);
		char addr[160];
		fmt(addr, sizeof addr, "unix:path=%s", l->path);
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "s", addr);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "RegisterDisplay") || str_eq(mbr, "RegisterSession")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "OpenSession")) {
		dbus_reply_error(bus, call, "org.gnome.DisplayManager.Manager.Error.NotSupported", "Zurbax signs in at its own screen");
	} else {
		return false;
	}
	return true;
}

static const char MANAGER_XML[] =
	"<interface name=\"org.gnome.DisplayManager.Manager\">"
	"<method name=\"RegisterDisplay\"><arg type=\"a{ss}\" direction=\"in\"/></method>"
	"<method name=\"RegisterSession\"><arg type=\"a{sv}\" direction=\"in\"/></method>"
	"<method name=\"OpenSession\"><arg type=\"s\" direction=\"out\"/></method>"
	"<method name=\"OpenReauthenticationChannel\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"out\"/></method>"
	"</interface>";

void gdm_start(DBus *bus)
{
	dbus_export(bus, MANAGER_PATH, MANAGER_IF, manager_method, NULL, MANAGER_XML);
	dbus_request_name(bus, GDM, DBUS_NAME_NO_QUEUE);
}
