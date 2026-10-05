/*
 * zinit — the first process of Zurbax.
 *
 * It mounts the kernel filesystems, finds the user's data partition (or
 * falls back to a RAM-backed live session), starts the system services and
 * keeps them running. It also performs shutdown, restart and sleep when asked
 * through /run/zinit.sock.
 */
#include "zr.h"

#define DATA_LABEL "ZURBAX-DATA"
#define CONTROL_SOCKET "/run/zinit.sock"

typedef struct {
	const char *name;
	const char *path;
	char *const *argv;
	bool essential;   /* restart forever */
	int pid;
	u64 started_ms;
	int restarts;
	u32 restart_timer;
} Service;

static char *const display_argv[] = { "zdisplay", NULL };
static char *const net_argv[] = { "znet", NULL };
static char *const audio_argv[] = { "zaudio", NULL };

static Service services[] = {
	{ "display", "/system/bin/zdisplay", display_argv, true },
	{ "network", "/system/bin/znet", net_argv, true },
	{ "audio", "/system/bin/zaudio", audio_argv, true },
};

static Loop *loop;
static bool shutting_down;
static bool live_session;

/* ---- Logging ------------------------------------------------------------------ */

static int kmsg = -1;

static void log_msg(const char *f, ...) PRINTF_LIKE(1, 2);
static void log_msg(const char *f, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, f);
	int n = fmt(line, sizeof line, "<6>zinit: ");
	n += fmt_v(line + n, sizeof line - (usize)n, f, ap);
	va_end(ap);
	if (kmsg >= 0)
		sys_write(kmsg, line, (usize)MIN(n, (int)sizeof line - 1));
}

/* ---- Filesystems ---------------------------------------------------------------- */

static void mount_fs(const char *src, const char *dst, const char *type, unsigned long flags, const char *opts)
{
	dir_make(dst, 0755);
	long r = sys_mount(src, dst, type, flags, opts);
	if (r < 0 && r != -EBUSY)
		log_msg("mount %s on %s failed: %s", type, dst, err_str(r));
}

static void mount_kernel_filesystems(void)
{
	mount_fs("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	kmsg = (int)sys_open("/dev/kmsg", O_WRONLY | O_CLOEXEC, 0);
	mount_fs("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mount_fs("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mount_fs("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=0620,ptmxmode=0666");
	mount_fs("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
	mount_fs("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
	mount_fs("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
	if (dir_exists("/sys/firmware/efi"))
		mount_fs("efivarfs", "/sys/firmware/efi/efivars", "efivarfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	sys_symlink("/proc/self/fd", "/dev/fd");
	sys_symlink("/proc/self/fd/0", "/dev/stdin");
	sys_symlink("/proc/self/fd/1", "/dev/stdout");
	sys_symlink("/proc/self/fd/2", "/dev/stderr");
}

/* Finds a block device whose GPT partition name is `label`; returns "/dev/xxx". */
static char *find_partition(const char *label)
{
	DirList list = { 0 };
	char *found = NULL;
	if (!dir_list("/sys/class/block", &list, false))
		return NULL;
	vec_each(&list, e)
	{
		char *path = str_fmt("/sys/class/block/%s/uevent", e->name);
		char *uevent = file_read(path, NULL);
		mem_free(path);
		if (!uevent)
			continue;
		const char *p = str_find(uevent, "PARTNAME=");
		if (p) {
			p += 9;
			const char *end = str_chr(p, '\n');
			usize n = end ? (usize)(end - p) : strlen(p);
			if (n == strlen(label) && memcmp(p, label, n) == 0)
				found = str_fmt("/dev/%s", e->name);
		}
		mem_free(uevent);
		if (found)
			break;
	}
	dir_list_free(&list);
	return found;
}

/* Mounts the persistent data partition, or a RAM disk for a live session
 * (booted from the install USB stick, or before installation). */
static void mount_data(void)
{
	char *dev = NULL;
	/* Storage controllers finish probing quickly, USB sticks can take a moment. */
	for (int i = 0; i < 30 && !dev; i++) {
		dev = find_partition(DATA_LABEL);
		if (!dev)
			sleep_ms(100);
	}
	dir_make("/data", 0755);
	if (dev) {
		long r = sys_mount(dev, "/data", "ext4", MS_NOATIME, NULL);
		if (r < 0) {
			log_msg("cannot mount %s: %s; starting a live session", dev, err_str(r));
			dev = NULL;
		}
	}
	if (!dev) {
		live_session = true;
		mount_fs("tmpfs", "/data", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
	}
	dir_make("/data/system", 0755);
	dir_make("/data/home", 0755);
	dir_make("/data/apps", 0755);
	if (live_session)
		file_write_str("/run/zurbax-live", "1\n");
	mem_free(dev);
}

/* ---- Services ------------------------------------------------------------------------- */

static char *service_env[] = {
	"PATH=/system/bin:/data/apps/bin",
	"ZURBAX_ROOT=/system",
	"ZURBAX_DATA=/data",
	"XDG_RUNTIME_DIR=/run/zurbax",
	"LANG=en_US.UTF-8",
	NULL,
};

static void restart_timer(Loop *l, void *data);

static void service_start(Service *s)
{
	if (shutting_down || !file_exists(s->path))
		return;
	s->pid = proc_spawn(s->path, s->argv, service_env, NULL);
	s->started_ms = time_mono_ms();
	if (s->pid < 0) {
		log_msg("cannot start %s: %s", s->name, err_str(s->pid));
		s->pid = 0;
	}
}

static void restart_timer(Loop *l, void *data)
{
	UNUSED(l);
	Service *s = data;
	s->restart_timer = 0;
	service_start(s);
}

static void reap(Loop *l, void *data)
{
	UNUSED(l), UNUSED(data);
	for (;;) {
		int status = 0;
		int pid = (int)sys_wait4(-1, &status, WNOHANG, NULL);
		if (pid <= 0)
			break;
		for (usize i = 0; i < ARRAY_LEN(services); i++) {
			Service *s = &services[i];
			if (s->pid != pid)
				continue;
			s->pid = 0;
			if (shutting_down || !s->essential)
				break;
			/* Crash loops back off: immediately, then up to five seconds. */
			u64 ran = time_mono_ms() - s->started_ms;
			s->restarts = ran > 30000 ? 0 : s->restarts + 1;
			u32 delay = (u32)MIN(5000, s->restarts * s->restarts * 100);
			log_msg("%s exited (status %d), restarting in %u ms", s->name, status, delay);
			s->restart_timer = loop_timer(loop, delay, 0, restart_timer, s);
			break;
		}
	}
}

static void on_sigchld(int sig)
{
	UNUSED(sig);
	loop_wake(loop);
}

/* ---- Shutdown -------------------------------------------------------------------------- */

static void power_off(int cmd)
{
	shutting_down = true;
	log_msg("%s", cmd == REBOOT_CMD_RESTART ? "restarting" : "powering off");
	sys_kill(-1, SIGTERM);
	/* Give programs up to three seconds to save their work. */
	for (int i = 0; i < 30; i++) {
		int status;
		while (sys_wait4(-1, &status, WNOHANG, NULL) > 0)
			;
		if (sys_kill(-1, 0) == -ESRCH)
			break;
		sleep_ms(100);
	}
	sys_kill(-1, SIGKILL);
	sys_sync();
	if (!live_session) {
		sys_mount(NULL, "/data", NULL, MS_REMOUNT | MS_RDONLY, NULL);
		sys_umount("/data", 0);
	}
	sys_sync();
	sys_reboot(cmd);
	for (;;)
		sleep_ms(1000);
}

static void on_control(Loop *l, int fd, u32 events, void *data)
{
	UNUSED(l), UNUSED(events), UNUSED(data);
	int c = (int)sys_accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (c < 0)
		return;
	/* Only the display server (root) may power the machine off; apps ask it. */
	struct ucred cred = { 0 };
	u32 len = sizeof cred;
	char cmd[32] = { 0 };
	if (sys_getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) >= 0 && cred.uid == 0) {
		struct pollfd p = { c, POLLIN, 0 };
		if (sys_poll(&p, 1, 500) > 0)
			sys_recvfrom(c, cmd, sizeof cmd - 1, 0, NULL, NULL);
	}
	sys_close(c);
	if (str_eq(cmd, "poweroff"))
		power_off(REBOOT_CMD_POWER_OFF);
	else if (str_eq(cmd, "reboot"))
		power_off(REBOOT_CMD_RESTART);
}

/* ---- Main ------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	if (sys_getpid() != 1) {
		eprint("zinit: must run as process 1\n");
		return 1;
	}
	mount_kernel_filesystems();
	/* Keep kernel messages off the screen: the boot splash owns it. */
	file_write_str("/proc/sys/kernel/printk", "1 4 1 3");
	sys_sethostname("zurbax", 6);
	sys_reboot(REBOOT_CMD_CAD_OFF);
	dir_make("/run/zurbax", 0755);
	mount_data();
	sys_umask(022);

	loop = loop_new();
	loop_on_wake(loop, reap, NULL);
	sig_handle(SIGCHLD, on_sigchld);
	sig_ignore(SIGPIPE);
	sig_ignore(SIGTERM);
	sig_ignore(SIGINT);

	int ctl = unix_listen(CONTROL_SOCKET, SOCK_SEQPACKET);
	if (ctl >= 0)
		loop_add_fd(loop, ctl, EPOLLIN, on_control, NULL);

	for (usize i = 0; i < ARRAY_LEN(services); i++)
		service_start(&services[i]);
	log_msg("started%s", live_session ? " (live session)" : "");
	for (;;) {
		loop_once(loop, 1000);
		reap(loop, NULL);
	}
}
