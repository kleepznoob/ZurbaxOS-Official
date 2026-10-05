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
	dir_list_free(