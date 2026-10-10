/*
 * polkit-helper: the password check behind GNOME's authentication dialogs.
 *
 * GNOME Shell's polkit agent runs /usr/lib/polkit-1/polkit-agent-helper-1
 * (in the Linux layer, this program) when something asks for the
 * administrator's password, and talks to it line by line:
 *
 *     agent -> helper   the authentication's cookie (first line of input)
 *     helper -> agent   PAM_PROMPT_ECHO_OFF Password:
 *     agent -> helper   what the person typed
 *     helper -> agent   SUCCESS or FAILURE
 *
 * The password goes to zsys, the session's polkit authority, which checks
 * it against the account and completes the authentication it handed out.
 * Nothing here needs privileges.
 */
#include "zr.h"

static void say(const char *line)
{
	write_all(1, line, strlen(line));
	write_all(1, "\n", 1);
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

int main(int argc, char **argv)
{
	sig_ignore(SIGPIPE);
	if (argc < 2) {
		eprint("polkit-agent-helper-1: this program is run by the authentication agent\n");
		return 1;
	}
	char cookie[128], password[512];
	if (!read_line(cookie, sizeof cookie)) {
		say("FAILURE");
		return 1;
	}
	say("PAM_PROMPT_ECHO_OFF Password: ");
	if (!read_line(password, sizeof password)) {
		say("FAILURE");
		return 1;
	}
	const char *addr = env_get("DBUS_SYSTEM_BUS_ADDRESS");
	DBus *bus = dbus_open_address(addr && *addr ? addr : "unix:path=/run/dbus/system_bus_socket");
	bool ok = false;
	if (bus) {
		DBusMsg *m = dbus_new_call("org.freedesktop.PolicyKit1", "/org/freedesktop/PolicyKit1/Authority", "org.zurbax.PolicyKit",
					   "Authenticate");
		dbus_add(m, "ss", cookie, password);
		DBusMsg *reply = dbus_call(bus, m, 15000);
		ok = reply && dbus_type(reply) == DBUS_RETURN;
		if (reply)
			dbus_msg_free(reply);
		dbus_close_bus(bus);
	}
	memset(password, 0, sizeof password);
	say(ok ? "SUCCESS" : "FAILURE");
	return ok ? 0 : 1;
}
