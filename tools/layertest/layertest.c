/* layertest: checks the Linux layer inside ZOS, as the desktop's user would
 * use it, and reports to the kernel log:
 *
 *     layertest: mounted /data/layers/steam
 *     layertest: check | yes the C library (64-bit) ...
 *     layertest: gpu | opengl softpipe|4.5 (Compatibility Profile) Mesa 26.2.4
 *     layertest: x11 | 61234 /tmp/keymap.xkb
 *     layertest: done
 *
 * zinit mounts the layer (from /data/layers or the install medium); this
 * runs layerrun --check, zos-gpucheck in the layer, and an X11 program on
 * the layer's Xwayland (xkbcomp reading the server's keymap back), each as
 * user 1000 like the desktop's apps. Test builds start it from /system/test
 * when the kernel is told "zurbax.test"; it is never part of a release.
 *
 * With "zurbax.layertest=install" it instead installs ZOS on the virtual
 * disk vda (erasing it) and logs the installer's steps, including the copy
 * of the layer; the next boot, from that disk, then checks the layer the
 * installed system mounts from /data/layers/steam.sfs. */
#include "zr.h"

static void say(const char *f, ...) PRINTF_LIKE(1, 2);
static void say(const char *f, ...)
{
	char msg[512] = "layertest: ";
	va_list ap;
	va_start(ap, f);
	fmt_v(msg + 11, sizeof msg - 12, f, ap);
	va_end(ap);
	str_copy(msg + strlen(msg), "\n", 2);
	eprint("%s", msg);
	int k = (int)sys_open("/dev/kmsg", O_WRONLY | O_CLOEXEC, 0);
	if (k >= 0) {
		write_all(k, msg, strlen(msg));
		sys_close(k);
	}
}

static char *env[16];

/* Runs layerrun with ARGS as user 1000 and logs its output, line by line. */
static void run(const char *tag, char **args)
{
	char *argv[16] = { "/system/bin/layerrun" };
	int n = 1;
	for (; args[n - 1] && n < 15; n++)
		argv[n] = args[n - 1];
	argv[n] = NULL;
	int fds[2];
	if (sys_pipe2(fds, O_CLOEXEC) < 0)
		return;
	static const u32 groups[] = { GID_AUDIO, GID_VIDEO, GID_INPUT };
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = fds[1], .stderr_fd = fds[1], .cwd = "/data/home/test",
			.drop_privileges = true, .uid = 1000, .gid = 1000, .groups = groups, .ngroups = 3 };
	int pid = proc_spawn(argv[0], argv, env, &o);
	sys_close(fds[1]);
	if (pid < 0) {
		say("%s | cannot start layerrun: %s", tag, err_str(pid));
		sys_close(fds[0]);
		return;
	}
	StrBuf out = { 0 };
	char buf[1024];
	long got;
	while ((got = sys_read(fds[0], buf, sizeof buf)) > 0 || got == -EINTR)
		if (got > 0)
			sb_add(&out, buf, (usize)got);
	sys_close(fds[0]);
	int status = 0;
	proc_wait(pid, &status, false);
	usize nl = 0;
	char **lines = str_split(sb_str(&out), '\n', &nl);
	for (usize i = 0; i < nl; i++)
		if (lines[i][0])
			say("%s | %s", tag, lines[i]);
	str_split_free(lines, nl);
	sb_free(&out);
	say("%s exited with %d", tag, (status >> 8) & 0xff);
}

static void install(void)
{
	char *argv[] = { "/system/bin/zinstall", "install", "vda", "erase", NULL };
	char *out = proc_run_capture(argv, NULL);
	usize n = 0;
	char **lines = str_split(out ? out : "", '\n', &n);
	for (usize i = 0; i < n; i++)
		if (lines[i][0])
			say("install | %s", lines[i]);
	str_split_free(lines, n);
	mem_free(out);
	say("done");
}

int main(void)
{
	char *cmdline = file_read("/proc/cmdline", NULL);
	bool installing = cmdline && str_find(cmdline, "zurbax.layertest=install");
	mem_free(cmdline);
	if (installing) {
		sleep_ms(3000);
		install();
		return 0;
	}
	/* the desktop first: Xwayland needs its Wayland server */
	sleep_ms(8000);
	char *mounts = file_read("/proc/self/mountinfo", NULL);
	say("%s /data/layers/steam", mounts && str_find(mounts, " /data/layers/steam ") ? "mounted" : "NOT mounted");
	mem_free(mounts);
	dir_make("/data/home", 0755);
	dir_make("/data/home/test", 0755);
	sys_chown("/data/home/test", 1000, 1000);
	int e = 0;
	env[e++] = "HOME=/data/home/test";
	env[e++] = "USER=test";
	env[e++] = "LANG=en_US.UTF-8";
	env[e++] = "PATH=/system/bin";
	env[e++] = "ZOS_SCREEN=1280x800";
	env[e++] = "WAYLAND_DISPLAY=/run/zurbax/wayland-0";
	env[e] = NULL;
	run("check", (char *[]){ "--check", NULL });
	run("gpu", (char *[]){ "zos-gpucheck", NULL });
	run("x11", (char *[]){ "--x11", "--app", "layertest", "bash", "-c",
			       "xkbcomp -xkb \"$DISPLAY\" /tmp/keymap.xkb && wc -c /tmp/keymap.xkb && zos-gpucheck --opengl", NULL });
	say("done");
	return 0;
}
