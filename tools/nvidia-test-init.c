/*
 * The first program of tools/nvidia-test.sh's virtual machine: which driver
 * modprobe picks for an NVIDIA GPU without NVIDIA's layer and with it, and
 * whether the kernel takes NVIDIA's modules (signed with Ember's key, built
 * against it). The machine has no NVIDIA GPU, so the module is expected to
 * load, find none and step back, and nouveau to take over.
 */
#include "zr.h"

/* GeForce RTX 4060 Laptop GPU (AD107), in an ASUS TUF A16 */
#define RTX4060 "pci:v000010DEd000028E0sv00001043sd000013A3bc03sc00i00"
/* GeForce GTX 1060 (Pascal): nouveau's alone */
#define GTX1060 "pci:v000010DEd00001C03sv00001043sd000085ACbc03sc00i00"

static void run(char **argv)
{
	int pid = proc_spawn(argv[0], argv, NULL, NULL);
	int st = -1;
	if (pid > 0)
		proc_wait(pid, &st, false);
	print("nvidia-test: %s %s exited with %d\n", argv[1], argv[2] ? argv[2] : "", st);
}

static void kernel_log(void)
{
	int fd = (int)sys_open("/dev/kmsg", O_RDONLY | O_NONBLOCK, 0);
	char buf[1024];
	long n;
	while (fd >= 0 && (n = sys_read(fd, buf, sizeof buf - 1)) > 0) {
		buf[n] = 0;
		const char *msg = str_chr(buf, ';');
		msg = msg ? msg + 1 : buf;
		if (str_find(msg, "NVRM") || str_find(msg, "nvidia") || str_find(msg, "nouveau") || str_find(msg, "odule") ||
		    str_find(msg, "Key"))
			print("kernel: %s", msg);
	}
	if (fd >= 0)
		sys_close(fd);
}

int main(void)
{
	static const char *const dirs[] = { "/proc", "/sys", "/dev", "/run", "/run/zurbax", "/data" };
	for (usize i = 0; i < ARRAY_LEN(dirs); i++)
		dir_make(dirs[i], 0755);
	sys_mount("proc", "/proc", "proc", 0, NULL);
	sys_mount("sysfs", "/sys", "sysfs", 0, NULL);
	sys_mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	sys_mount("tmpfs", "/run", "tmpfs", 0, NULL);
	dir_make("/run/zurbax", 0755);
	int con = (int)sys_open("/dev/console", O_RDWR, 0);
	for (int fd = 0; con >= 0 && fd < 3; fd++)
		sys_dup3(con, fd, 0);
	for (int i = 0; i < 100 && !file_exists("/dev/vdb"); i++)
		sleep_ms(100);
	dir_make("/data/layers", 0755);
	dir_make("/data/layers/drivers", 0755);
	long r = sys_mount("/dev/vda", "/data/layers/drivers", "squashfs", MS_RDONLY, NULL);
	print("nvidia-test: drivers.sfs %s\n", r == 0 ? "mounted" : err_str(r));

	print("nvidia-test: without NVIDIA's layer\n");
	char *a1[] = { "/modprobe", "--show", RTX4060, NULL };
	run(a1);

	dir_make("/data/layers/nvidia", 0755);
	r = sys_mount("/dev/vdb", "/data/layers/nvidia", "squashfs", MS_RDONLY, NULL);
	print("nvidia-test: nvidia.sfs %s\n", r == 0 ? "mounted" : err_str(r));
	print("nvidia-test: with it\n");
	run(a1);
	print("nvidia-test: a Pascal card with it\n");
	char *a2[] = { "/modprobe", "--show", GTX1060, NULL };
	run(a2);

	print("nvidia-test: loading for the RTX 4060\n");
	char *a3[] = { "/modprobe", "--device", RTX4060, NULL };
	run(a3);
	char *log = file_read("/run/zurbax/drivers.log", NULL);
	print("nvidia-test: drivers.log:\n%s", log ? log : "(none)\n");
	char *mods = file_read("/proc/modules", NULL);
	print("nvidia-test: loaded:\n%s", mods ? mods : "");
	kernel_log();
	sys_sync();
	sys_reboot(REBOOT_CMD_POWER_OFF);
	return 0;
}
