/*
 * passwd: changes the account's password from the Linux layer.
 *
 * GNOME's Settings changes the person's own password by running
 * /usr/bin/passwd (in the layer, this program) and answering its prompts,
 * which it recognises by their words (run-passwd.c in GNOME's Settings):
 *
 *     Current password:          (only when the account has one)
 *     New password:              (only once the current one was right)
 *     Retype new password:
 *     passwd: password updated successfully
 *
 * A wrong current password ends with "Authentication token manipulation
 * error", the answer Settings shows as a failed authentication. The
 * passwords go to zsys (org.zurbax.Accounts on the session's system bus),
 * which checks and keeps them; nothing here needs privileges. It works the
 * same typed into a terminal.
 */
#include "zr.h"

static DBus *bus;

static void say(const char *s)
{
	write_all(1, s, strlen(s));
}

/* One line of input, without its newline; NULL at the end. */
static char *read_line(char *buf, usize cap)
{
	usize n = 0;
	for (;;) {
		char c;
		long r = sys_read(0, &c, 1);
		if (r == -EINTR)
			continue;
		if (r <= 0)
			return n ? (buf[n] = 0, buf) : NULL;
		if (c == '\n')
			break;
		if (n + 1 < cap)
			buf[n++] = c;
	}
	buf[n] = 0;
	return buf;
}

/* Asks with the terminal's echo off when there is a terminal. */
static char *ask(const char *prompt, char *buf, usize cap)
{
	say(prompt);
	struct termios t, quiet;
	bool tty = sys_ioctl(0, TCGETS, &t) == 0;
	if (tty) {
		quiet = t;
		quiet.c_lflag &= ~(u32)ECHO;
		sys_ioctl(0, TCSETS, &quiet);
	}
	char *r = read_line(buf, cap);
	if (tty) {
		sys_ioctl(0, TCSETS, &t);
		say("\n");
	}
	return r;
}

/* org.zurbax.Accounts METHOD(args): NULL on success, else the error's name. */
static const char *call(const char *method, const char *a, const char *b)
{
	static char err[128];
	DBusMsg *m = dbus_new_call("org.freedesktop.Accounts", "/org/freedesktop/Accounts", "org.zurbax.Accounts", method);
	if (b)
		dbus_add(m, "ss", a, b);
	else
		dbus_add(m, "s", a);
	DBusMsg *reply = dbus_call(bus, m, 15000);
	const char *res = "org.zurbax.Accounts.Error.NoReply";
	if (reply && dbus_type(reply) == DBUS_RETURN) {
		res = NULL;
	} else if (reply) {
		str_copy(err, dbus_error_name(reply), sizeof err);
		res = err;
	}
	if (reply)
		dbus_msg_free(reply);
	return res;
}

static int fail(const char *why)
{
	say(why);
	say("passwd: Authentication token manipulation error\npasswd: password unchanged\n");
	return 1;
}

int main(int argc, char **argv)
{
	(void)argv;
	sig_ignore(SIGPIPE);
	if (argc > 1) {
		eprint("usage: passwd\n(Zurbax has one account; this changes its password)\n");
		return 2;
	}
	const char *addr = env_get("DBUS_SYSTEM_BUS_ADDRESS");
	bus = dbus_open_address(addr && *addr ? addr : "unix:path=/run/dbus/system_bus_socket");
	if (!bus) {
		eprint("passwd: the account service isn't running; change your password in Zurbax's Settings\n");
		return 1;
	}
	char cur[512] = "", next[512], again[512];
	/* an empty current password is right only for an account without one */
	if (call("CheckPassword", "", NULL)) {
		if (!ask("Current password: ", cur, sizeof cur))
			return fail("");
		const char *e = call("CheckPassword", cur, NULL);
		if (e)
			return fail(str_ends(e, "TryLater") ? "passwd: too soon after a wrong password; wait a moment\n" : "");
	}
	if (!ask("New password: ", next, sizeof next) || !ask("Retype new password: ", again, sizeof again))
		return fail("");
	if (!str_eq(next, again))
		return fail("Sorry, passwords do not match.\n");
	if (!next[0])
		return fail("No password has been supplied.\n");
	const char *e = call("ChangePassword", cur, next);
	memset(cur, 0, sizeof cur), memset(next, 0, sizeof next), memset(again, 0, sizeof again);
	dbus_close_bus(bus);
	if (e)
		return fail("");
	say("passwd: password updated successfully\n");
	return 0;
}
