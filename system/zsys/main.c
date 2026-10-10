/*
 * zsys: the system services of a GNOME session on Zurbax.
 *
 *     zsys [--bus ADDRESS] [--session ID]
 *
 * The desktop starts it, as root, when someone signs in to GNOME. It starts
 * the session's system bus (the layer's message bus program, with Zurbax's
 * own configuration), joins it and answers there for logind, UPower and
 * power profiles, AccountsService, hostnamed, localed, timedated, polkit,
 * NetworkManager, udisks and the display manager's unlock channel until the
 * session ends (SIGTERM) or the bus goes away.
 * The account is the one in /data/system/users.conf.
 */
#include "zsys.h"

#define DEFAULT_BUS_PATH "/run/dbus/system_bus_socket"

Sys Z;

static const char *data_dir(void)
{
	const char *d = env_get("ZURBAX_DATA");
	return d && *d ? d : "/data";
}

char *data_path(const char *name)
{
	return str_fmt("%s/system/%s", data_dir(), name);
}

static void load_account(void)
{
	char *path = data_path("users.conf");
	Conf *u = conf_load(path);
	mem_free(path);
	str_copy(Z.user, conf_get(u, "user", "user"), sizeof Z.user);
	str_copy(Z.full_name, conf_get(u, "full_name", "Zurbax User"), sizeof Z.full_name);
	Z.uid = (u32)conf_get_int(u, "uid", 1000);
	Z.gid = (u32)conf_get_int(u, "gid", 1000);
	const char *home = env_get("ZURBAX_HOME");
	if (home && *home)
		str_copy(Z.home, home, sizeof Z.home);
	else
		fmt(Z.home, sizeof Z.home, "%s/home/%s", data_dir(), Z.user);
	fmt(Z.runtime, sizeof Z.runtime, "/run/user/%u", Z.uid);
	conf_free(u);
}

const char *layer_root(void)
{
	const char *l = env_get("ZURBAX_LAYER");
	return l && *l ? l : "/data/layers/steam";
}

void account_reload(void)
{
	load_account();
}

void power_action(const char *what)
{
	int fd = unix_connect("/run/zinit.sock", SOCK_SEQPACKET);
	if (fd < 0) {
		eprint("zsys: can't reach zinit to %s\n", what);
		return;
	}
	sys_write(fd, what, strlen(what));
	sys_close(fd);
}

/* ---- The system bus ------------------------------------------------------------------- */

static int bus_pid;

/* The layer's dbus-daemon, in a namespace of its own with the layer as its
 * root (layerrun), listening where both Zurbax and the session see it. */
static int start_bus(const char *path)
{
	sys_unlink(path);
	char *dir = path_dir(path);
	dir_make(dir, 0755);
	mem_free(dir);
	char *addr = str_fmt("--address=unix:path=%s", path);
	char *argv[] = { "layerrun", "--isolated", "/usr/bin/dbus-daemon", "--config-file=/usr/share/zurbax/system-bus.conf",
			 "--nofork", "--nopidfile", "--nosyslog", addr, NULL };
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1 };
	const char *layerrun = file_exists("/system/bin/layerrun") ? "/system/bin/layerrun" : find_program("layerrun");
	int pid = layerrun ? proc_spawn(layerrun, argv, NULL, &o) : -1;
	mem_free(addr);
	if (pid < 0)
		return 0;
	for (int tries = 0; tries < 100 && !file_exists(path); tries++)
		sleep_ms(50);
	return pid;
}

static DBus *connect_bus(const char *address)
{
	for (int tries = 0; tries < 100; tries++) {
		DBus *b = dbus_open_address(address);
		if (b)
			return b;
		sleep_ms(100);
	}
	return NULL;
}

/* ---- Running ------------------------------------------------------------------------------ */

static void on_disconnect(DBus *bus, void *data)
{
	(void)bus, (void)data;
	Z.lost = true;
	loop_quit(Z.loop);
}

static volatile int stop_requested;

static void on_term(int sig)
{
	(void)sig;
	stop_requested = 1;
	if (Z.loop)
		loop_wake(Z.loop);
}

static void check_stop(Loop *loop, void *data)
{
	(void)data;
	if (stop_requested)
		loop_quit(loop);
}

int main(int argc, char **argv)
{
	const char *address = NULL;
	str_copy(Z.session, "1", sizeof Z.session);
	str_copy(Z.seat, "seat0", sizeof Z.seat);
	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "--bus") && i + 1 < argc)
			address = argv[++i];
		else if (str_eq(argv[i], "--session") && i + 1 < argc)
			str_copy(Z.session, argv[++i], sizeof Z.session);
		else {
			eprint("usage: zsys [--bus ADDRESS] [--session ID]\n");
			return 2;
		}
	}
	load_account();
	Z.started_us = time_unix_ms() * 1000;
	Z.started_mono_us = (i64)(time_mono_ns() / 1000);
	sig_ignore(SIGPIPE);
	sig_handle(SIGTERM, on_term);
	sig_handle(SIGINT, on_term);

	if (!address) {
		bus_pid = start_bus(DEFAULT_BUS_PATH);
		if (!bus_pid) {
			eprint("zsys: the layer's message bus didn't start\n");
			return 1;
		}
		address = "unix:path=" DEFAULT_BUS_PATH;
	}
	Z.bus = connect_bus(address);
	if (!Z.bus) {
		eprint("zsys: can't join the system bus at %s\n", address);
		return 1;
	}
	Z.loop = loop_new();
	loop_on_wake(Z.loop, check_stop, NULL);
	dbus_on_disconnect(Z.bus, on_disconnect, NULL);
	dbus_attach(Z.bus, Z.loop);

	login1_start(Z.bus);
	upower_start(Z.bus);
	accounts_start(Z.bus);
	hostname_start(Z.bus);
	polkit_start(Z.bus);
	network_start(Z.bus);
	gdm_start(Z.bus);
	udisks_start(Z.bus);
	print("zsys: serving the session of %s (uid %u) on %s\n", Z.user, Z.uid, address);

	while (!stop_requested && !Z.lost)
		loop_once(Z.loop, 1000);

	login1_stop();
	dbus_close_bus(Z.bus);
	if (bus_pid > 0) {
		sys_kill(bus_pid, SIGTERM);
		sys_wait4(bus_pid, NULL, 0, NULL);
		sys_unlink(DEFAULT_BUS_PATH);
	}
	return 0;
}
