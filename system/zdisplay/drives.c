/*
 * USB drives and memory cards: mounted under /media as they're plugged in,
 * for the person at the desktop (on FAT, exFAT and NTFS their files are
 * theirs to write), announced with a notice that opens them in Vault, and
 * unmounted when Vault's eject button asks (MsgLaunch "zurbax:eject") or the
 * drive is pulled out. Looked for every two seconds, with the status icons.
 *
 * The system's own disk is left alone: anything with a partition already
 * mounted (the data partition, the stick a live session runs from) is.
 */
#include "zdisplay.h"

#define MEDIA "/media"

typedef struct {
	char dev[32];           /* "sdb1" */
	char disk[32];          /* "sdb" */
	char path[96];          /* "/media/KINGSTON" */
	bool ejected;           /* unmounted on request: not again until it's unplugged */
} Drive;

static Drive drives[16];
static int ndrives;

static Drive *find_dev(const char *dev)
{
	for (int i = 0; i < ndrives; i++)
		if (str_eq(drives[i].dev, dev))
			return &drives[i];
	return NULL;
}

static void forget(int i)
{
	drives[i] = drives[--ndrives];
}

/* Disks a person plugs in: on a USB port, or an SD card (not built-in eMMC). */
static bool removable(const char *disk)
{
	char path[128], link[512] = { 0 };
	fmt(path, sizeof path, "/sys/block/%s", disk);
	if (sys_readlink(path, link, sizeof link - 1) > 0 && str_find(link, "/usb"))
		return true;
	if (str_starts(disk, "mmcblk")) {
		fmt(path, sizeof path, "/sys/block/%s/device/type", disk);
		char *t = file_read_line(path);
		bool sd = t && str_eq(str_trim(t), "SD");
		mem_free(t);
		return sd;
	}
	return false;
}

/* What's mounted, as /dev/... names, to leave alone. */
static char *mounts_text(void)
{
	char *m = file_read("/proc/self/mounts", NULL);
	return m ? m : str_dup("");
}

static bool is_mounted(const char *mounts, const char *dev)
{
	char want[48];
	fmt(want, sizeof want, "/dev/%s ", dev);
	for (const char *p = mounts; p && *p;) {
		if (str_starts(p, want))
			return true;
		const char *nl = str_chr(p, '\n');
		p = nl ? nl + 1 : NULL;
	}
	return false;
}

static void label_from(char *out, usize cap, const u8 *s, usize n)
{
	usize k = 0;
	for (usize i = 0; i < n && k + 1 < cap && s[i]; i++)
		out[k++] = (s[i] >= 0x20 && s[i] < 0x7f && s[i] != '/') ? (char)s[i] : '_';
	out[k] = 0;
	str_copy(out, str_trim(out), cap);
	if (str_eq(out, "NO NAME"))
		out[0] = 0;
}

/* The file system on a partition, by its signature, and its name when it's easy to read. */
static const char *probe(const char *dev, char *label, usize cap)
{
	label[0] = 0;
	char path[48];
	fmt(path, sizeof path, "/dev/%s", dev);
	int fd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return NULL;
	static u8 b[34816];
	memset(b, 0, sizeof b);
	long n = sys_pread(fd, b, sizeof b, 0);
	sys_close(fd);
	if (n < 4096)
		return NULL;
	if (b[1080] == 0x53 && b[1081] == 0xef) {
		label_from(label, cap, b + 1024 + 120, 16);
		return "ext4";
	}
	if (memcmp(b + 3, "EXFAT   ", 8) == 0)
		return "exfat";
	if (memcmp(b + 3, "NTFS    ", 8) == 0)
		return "ntfs3";
	if (b[510] == 0x55 && b[511] == 0xaa && memcmp(b + 82, "FAT32   ", 8) == 0) {
		label_from(label, cap, b + 71, 11);
		return "vfat";
	}
	if (b[510] == 0x55 && b[511] == 0xaa && memcmp(b + 54, "FAT1", 4) == 0) {
		label_from(label, cap, b + 43, 11);
		return "vfat";
	}
	if (n >= 32768 + 72 && memcmp(b + 32769, "CD001", 5) == 0) {
		label_from(label, cap, b + 32768 + 40, 32);
		return "iso9660";
	}
	return NULL;
}

/* /media/NAME, or NAME 2... when it's taken */
static void mount_point(const char *name, char *out, usize cap)
{
	fmt(out, cap, "%s/%s", MEDIA, name);
	for (int k = 2; dir_exists(out) || file_exists(out); k++)
		fmt(out, cap, "%s/%s %d", MEDIA, name, k);
}

static void mount_one(const char *disk, const char *dev)
{
	char label[64];
	const char *fs = probe(dev, label, sizeof label);
	if (!fs || ndrives >= (int)ARRAY_LEN(drives))
		return;
	if (!label[0]) {
		char path[128];
		fmt(path, sizeof path, "/sys/block/%s/device/model", disk);
		char *model = file_read_line(path);
		label_from(label, sizeof label, (const u8 *)(model ? model : ""), model ? strlen(model) : 0);
		mem_free(model);
		if (!label[0])
			str_copy(label, str_starts(disk, "mmcblk") ? "Memory Card" : "USB Drive", sizeof label);
	}
	Drive *d = &drives[ndrives];
	memset(d, 0, sizeof *d);
	str_copy(d->dev, dev, sizeof d->dev);
	str_copy(d->disk, disk, sizeof d->disk);
	dir_make(MEDIA, 0755);
	mount_point(label, d->path, sizeof d->path);
	dir_make(d->path, 0755);
	char opts[128] = "", src[48];
	unsigned long flags = MS_NOSUID | MS_NODEV;
	u32 uid = S.set.uid, gid = S.set.gid;
	if (str_eq(fs, "vfat"))
		fmt(opts, sizeof opts, "uid=%u,gid=%u,fmask=0022,dmask=0022,utf8,shortname=mixed", uid, gid);
	else if (str_eq(fs, "exfat") || str_eq(fs, "ntfs3"))
		fmt(opts, sizeof opts, "uid=%u,gid=%u,fmask=0022,dmask=0022", uid, gid);
	else if (str_eq(fs, "iso9660"))
		fmt(opts, sizeof opts, "uid=%u,gid=%u", uid, gid), flags |= MS_RDONLY;
	fmt(src, sizeof src, "/dev/%s", dev);
	long r = sys_mount(src, d->path, fs, flags, opts[0] ? opts : NULL);
	if (r < 0 && !(flags & MS_RDONLY))
		r = sys_mount(src, d->path, fs, flags | MS_RDONLY, opts[0] ? opts : NULL); /* write-protected, or not cleanly removed */
	if (r < 0) {
		sys_rmdir(d->path);
		return;
	}
	ndrives++;
	Notice *n = shell_notify("vault", "usb", "USB drive connected", path_base(d->path));
	if (n)
		str_copy(n->arg, d->path, sizeof n->arg);
}

void drives_poll(void)
{
	char *mounts = mounts_text();
	/* drives pulled out: their mount goes */
	for (int i = ndrives - 1; i >= 0; i--) {
		char path[96];
		fmt(path, sizeof path, "/sys/class/block/%s", drives[i].dev);
		if (dir_exists(path))
			continue;
		if (!drives[i].ejected) {
			sys_umount(drives[i].path, MNT_DETACH);
			sys_rmdir(drives[i].path);
		}
		forget(i);
	}
	DirList l = { 0 };
	if (dir_list("/sys/block", &l, false)) {
		vec_each(&l, e) {
			if (!removable(e->name))
				continue;
			/* its partitions, or the whole disk when it has none */
			char dpath[96];
			fmt(dpath, sizeof dpath, "/sys/block/%s", e->name);
			DirList parts = { 0 };
			int found = 0;
			bool system_disk = is_mounted(mounts, e->name);
			char names[16][32];
			if (dir_list(dpath, &parts, false)) {
				vec_each(&parts, p) {
					if (str_starts(p->name, e->name) && found < 16) {
						str_copy(names[found++], p->name, sizeof names[0]);
						if (is_mounted(mounts, p->name) && !find_dev(p->name))
							system_disk = true;
					}
				}
				dir_list_free(&parts);
			}
			if (system_disk)
				continue;
			if (!found)
				str_copy(names[found++], e->name, sizeof names[0]);
			for (int k = 0; k < found; k++)
				if (!find_dev(names[k]) && !is_mounted(mounts, names[k]))
					mount_one(e->name, names[k]);
		}
		dir_list_free(&l);
	}
	mem_free(mounts);
}

/* Vault's eject button: only drives mounted here, after writing out what's pending. */
void drives_eject(const char *path)
{
	for (int i = 0; i < ndrives; i++) {
		Drive *d = &drives[i];
		if (d->ejected || !str_eq(d->path, path))
			continue;
		sys_sync();
		if (sys_umount(d->path, 0) < 0) {
			shell_notify("system", "usb", "The drive is still in use", "Close what's open from it, then eject it again.");
			return;
		}
		sys_rmdir(d->path);
		d->ejected = true;
		shell_notify("system", "usb", "Safe to unplug", path_base(path));
		return;
	}
}
