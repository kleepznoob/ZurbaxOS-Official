/*
 * The first program of tools/flash-test.sh's virtual machine: once the two
 * USB sticks show up, runs flash with a Windows-shaped ISO onto the first
 * and a hybrid one (Zurbax's) onto the second, prints how it went on the
 * serial port and powers off.
 */
#include "zr.h"

static void run(char **argv)
{
	int pid = proc_spawn(argv[0], argv, NULL, NULL);
	int st = -1;
	if (pid > 0)
		proc_wait(pid, &st, false);
	print("flash-test: %s exited with %d\n", argv[0], st);
}

int main(void)
{
	dir_make("/proc", 0755);
	dir_make("/sys", 0755);
	dir_make("/dev", 0755);
	dir_make("/run", 0755);
	sys_mount("proc", "/proc", "proc", 0, NULL);
	sys_mount("sysfs", "/sys", "sysfs", 0, NULL);
	sys_mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	sys_mount("tmpfs", "/run", "tmpfs", 0, NULL);
	int con = (int)sys_open("/dev/console", O_RDWR, 0);
	for (int fd = 0; con >= 0 && fd < 3; fd++)
		sys_dup3(con, fd, 0);
	/* USB devices take a moment to appear */
	for (int i = 0; i < 300 && !(file_exists("/sys/block/sda") && file_exists("/sys/block/sdb")); i++)
		sleep_ms(100);
	print("flash-test: sda and sdb %s\n", file_exists("/sys/block/sdb") ? "are here" : "never showed up");
	/* a stick with a filesystem on it, mounted, as a desktop would leave it */
	if (file_exists("/dev/sda1")) {
		dir_make("/run/media/old", 0755);
		long r = sys_mount("/dev/sda1", "/run/media/old", "vfat", 0, NULL);
		print("flash-test: old stick mounted: %s\n", r == 0 ? "yes" : err_str(r));
	}
	char *refuse[] = { "/flash", "/win.iso", "vda", "--yes", NULL };
	run(refuse);
	char *windows[] = { "/flash", "/win.iso", "sda", "--yes", NULL };
	run(windows);
	char *hybrid[] = { "/flash", "/hybrid.iso", "sdb", "--yes", NULL };
	run(hybrid);
	sys_sync();
	sys_reboot(REBOOT_CMD_POWER_OFF);
	return 0;
}
