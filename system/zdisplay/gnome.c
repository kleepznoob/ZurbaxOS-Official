/*
 * GNOME, the second desktop.
 *
 * The lock screen offers a choice of desktops: Zurbax's own, or GNOME from
 * the Linux layer. Unlocking into GNOME hands the screen and the input
 * devices over: zdisplay gives up the right to set modes and closes its
 * keyboards and mice, starts zsys (the system services, as root) and then,
 * as the person, the GNOME session (layerrun --desktop gnome, which runs
 * zsession). GNOME Shell takes the devices through zsys. When the session
 * ends, zdisplay takes everything back and shows the lock screen again.
 */
#include "zdisplay.h"

#define LAYER "/data/layers/steam"
#define BUS_SOCKET "/run/dbus/system_bus_socket"

static int sys_pid, session_pid;
static f64 started;

bool gnome_available(void)
{
	static int known = -1;
	static f64 checked;
	/* the layer can be mounted after the desktop starts: look again now and then */
	if (known < 0 || S.now - checked > 30) {
		checked = S.now;
		char *zsys = zurbax_path("bin/zsys");
		known = file_exists(LAYER "/usr/bin/gnome-shell") && file_exists(LAYER "/usr/libexec/zurbax/zsession") && file_exists(zsys);
		mem_free(zsys);
	}
	return known == 1;
}

bool gnome_running(void)
{
	return session_pid > 0;
}

static int start_zsys(void)
{
	char *path = zurbax_path("bin/zsys");
	char *argv[] = { "zsys", NULL };
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .new_session = true };
	int pid = proc_spawn(path, argv, NULL, &o);
	mem_free(path);
	/* GNOME Shell needs the system bus from its first moment */
	for (int i = 0; pid > 0 && i < 100 && !file_exists(BUS_SOCKET); i++)
		sleep_ms(50);
	return pid > 0 ? pid : 0;
}

bool gnome_start(void)
{
	if (session_pid || !gnome_available())
		return false;
	shell_stop_portal();
	if (!S.headless) {
		if (S.out->pause)
			S.out->pause(S.out, true);
		input_pause(true);
	}
	sys_pid = start_zsys();
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[] = { "layerrun", "--desktop", "gnome", NULL };
	int pid = shell_user_spawn(layerrun, argv);
	mem_free(layerrun);
	if (pid <= 0) {
		gnome_ended(0, -1);
		shell_notify("system", "error", "GNOME couldn't start", "The Zurbax desktop is back.");
		return false;
	}
	session_pid = pid;
	started = S.now;
	eprint("zdisplay: GNOME session %d started\n", pid);
	return true;
}

/* A child of the desktop ended: the GNOME session, or zsys under it. */
void gnome_reaped(int pid, int status)
{
	if (pid == sys_pid && session_pid) {
		/* the system services went away: start them again, the session needs them */
		sys_pid = start_zsys();
	} else if (pid == session_pid) {
		gnome_ended(pid, status);
	}
}

void gnome_ended(int pid, int status)
{
	bool early = S.now - started < 15;
	session_pid = 0;
	if (sys_pid > 0) {
		sys_kill(sys_pid, SIGTERM);
		for (int i = 0; i < 40 && sys_kill(sys_pid, 0) == 0; i++) {
			sys_wait4(sys_pid, NULL, WNOHANG, NULL);
			sleep_ms(50);
		}
		sys_pid = 0;
	}
	/* the Linux session goes too: the next Linux program gets the Zurbax kind */
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[] = { "layerrun", "--stop", NULL };
	shell_user_spawn(layerrun, argv);
	mem_free(layerrun);
	if (!S.headless) {
		if (S.out->pause)
			S.out->pause(S.out, false);
		input_pause(false);
	}
	S.locked = true;
	S.lock_k = 1;
	S.last_input = S.now;
	damage_all();
	shell_mark_dirty();
	eprint("zdisplay: GNOME session %d ended (%d)\n", pid, status);
	if (pid && early && status != 0)
		shell_notify("system", "error", "GNOME stopped right after starting", "The Zurbax desktop is here instead.");
}
