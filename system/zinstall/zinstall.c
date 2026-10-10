/*
 * zinstall — puts Zurbax on a disk. The Installer app drives it.
 *
 *   zinstall disks                   lists the disks, one "disk" line each,
 *                                    followed by "part" lines
 *   zinstall install DISK erase      the whole disk becomes Zurbax
 *   zinstall install DISK alongside  Zurbax goes into the disk's free space
 *                                    and boots from its EFI system partition
 *
 * While installing it prints "step FRACTION TEXT" lines, then "done" or
 * "error TEXT". It is setuid root but works only in the live session (booted
 * from the install stick), where the person at the keyboard owns the machine.
 *
 * Layout: an EFI system partition holding \EFI\ZURBAX\ZURBAX.EFI (the kernel
 * with the whole system inside) and an ext4 partition named ZURBAX-DATA for
 * accounts, settings and files. A firmware boot entry points at the EFI file,
 * so there is no boot loader.
 */
#include "zd.h"

#define BLKRRPART 0x125f
#define BLKSSZGET 0x1268
#define BLKGETSIZE64 0x80081272
#define FS_IOC_GETFLAGS 0x80086601
#define FS_IOC_SETFLAGS 0x40086602
#define FS_IMMUTABLE_FL 0x10

#define ESP_MIB 512
#define MIN_DATA_GIB 16
#define EFI_PATH "\\EFI\\ZURBAX\\ZURBAX.EFI"
#define EFIVARS "/sys/firmware/efi/efivars"
#define GLOBAL_GUID "8be4df61-93ca-11d2-aa0d-00e098032b8c"
#define WORK "/run/zinstall"

/* ---- Output ---------------------------------------------------------------------------- */

static void say(const char *f, ...) PRINTF_LIKE(1, 2);
static void say(const char *f, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, f);
	int n = fmt_v(line, sizeof line - 1, f, ap);
	va_end(ap);
	line[n++] = '\n';
	write_all(1, line, (usize)n);
}

static void step(f32 fraction, const char *text)
{
	say("step %.2f %s", (double)fraction, text);
}

NORETURN static void fail(const char *text)
{
	say("error %s", text);
	exit(1);
}

/* ---- Disks ---------------------------------------------------------------------------- */

typedef struct {
	char name[32];       /* "sda1", "nvme0n1p2" */
	u32 number;
	u64 start, sectors;
	char partname[40];
	char fs[16];
} Part;

typedef struct {
	char name[32];
	char model[64];
	u64 bytes;
	bool removable, usb, boot_medium;
	Part parts[32];
	int nparts;
} Disk;

static char *sys_str(const char *path)
{
	char *s = file_read_line(path);
	return s ? s : str_dup("");
}

static bool valid_name(const char *n)
{
	if (!*n || strlen(n) >= 32)
		return false;
	for (const char *p = n; *p; p++)
		if (!is_alnum(*p))
			return false;
	return true;
}

static const char *uevent_value(const char *uevent, const char *key, char *out, usize cap)
{
	out[0] = 0;
	usize kl = strlen(key);
	for (const char *p = uevent; p && *p;) {
		if (str_starts(p, key) && p[kl] == '=') {
			const char *v = p + kl + 1, *e = str_chr(v, '\n');
			usize n = e ? (usize)(e - v) : strlen(v);
			str_copy(out, v, MIN(cap, n + 1));
			return out;
		}
		p = str_chr(p, '\n');
		if (p)
			p++;
	}
	return out;
}

static void detect_fs(Part *p)
{
	str_copy(p->fs, "", sizeof p->fs);
	char *dev = str_fmt("/dev/%s", p->name);
	int fd = (int)sys_open(dev, O_RDONLY | O_CLOEXEC, 0);
	mem_free(dev);
	if (fd < 0)
		return;
	u8 b[4096];
	if (sys_pread(fd, b, sizeof b, 0) == (long)sizeof b) {
		if (memcmp(b + 3, "NTFS    ", 8) == 0)
			str_copy(p->fs, "ntfs", sizeof p->fs);
		else if (memcmp(b + 3, "-FVE-FS-", 8) == 0)
			str_copy(p->fs, "bitlocker", sizeof p->fs);
		else if (memcmp(b + 3, "EXFAT   ", 8) == 0)
			str_copy(p->fs, "exfat", sizeof p->fs);
		else if (memcmp(b + 82, "FAT32   ", 8) == 0 || memcmp(b + 54, "FAT1", 4) == 0)
			str_copy(p->fs, "fat", sizeof p->fs);
		else if (b[1080] == 0x53 && b[1081] == 0xef)
			str_copy(p->fs, "ext4", sizeof p->fs);
		else if (memcmp(b + 4086, "SWAPSPACE2", 10) == 0)
			str_copy(p->fs, "swap", sizeof p->fs);
	}
	sys_close(fd);
}

static bool load_disk(const char *name, Disk *d)
{
	memset(d, 0, sizeof *d);
	if (!valid_name(name))
		return false;
	char *base = str_fmt("/sys/block/%s", name);
	bool ok = dir_exists(base);
	if (ok) {
		str_copy(d->name, name, sizeof d->name);
		char *p = path_join(base, "size");
		d->bytes = (u64)file_read_int(p, 0) * 512;
		mem_free(p);
		p = path_join(base, "removable");
		d->removable = file_read_int(p, 0) == 1;
		mem_free(p);
		p = path_join(base, "device/model");
		char *model = sys_str(p);
		mem_free(p);
		str_copy(d->model, str_trim(model), sizeof d->model);
		mem_free(model);
		char link[512];
		long n = sys_readlink(base, link, sizeof link - 1);
		if (n > 0) {
			link[n] = 0;
			d->usb = str_find(link, "/usb") != NULL;
		}
		DirList list = { 0 };
		dir_list(base, &list, false);
		for (usize i = 0; i < list.len && d->nparts < (int)ARRAY_LEN(d->parts); i++) {
			const char *e = list.data[i].name;
			char *pp = str_fmt("%s/%s/partition", base, e);
			bool is_part = str_starts(e, name) && file_exists(pp);
			mem_free(pp);
			if (!is_part)
				continue;
			Part *part = &d->parts[d->nparts++];
			str_copy(part->name, e, sizeof part->name);
			char *f = str_fmt("%s/%s/start", base, e);
			part->start = (u64)file_read_int(f, 0);
			mem_free(f);
			f = str_fmt("%s/%s/size", base, e);
			part->sectors = (u64)file_read_int(f, 0);
			mem_free(f);
			f = str_fmt("%s/%s/uevent", base, e);
			char *ue = file_read(f, NULL);
			mem_free(f);
			char num[16];
			uevent_value(ue, "PARTN", num, sizeof num);
			part->number = (u32)str_to_int(num, NULL);
			uevent_value(ue, "PARTNAME", part->partname, sizeof part->partname);
			mem_free(ue);
			if (str_eq(part->partname, "ZURBAX-INSTALL"))
				d->boot_medium = true;
			detect_fs(part);
		}
		dir_list_free(&list);
	}
	mem_free(base);
	return ok;
}

static bool skip_device(const char *name)
{
	static const char *const prefixes[] = { "loop", "ram", "zram", "sr", "dm-", "md", "nbd", "fd" };
	for (usize i = 0; i < ARRAY_LEN(prefixes); i++)
		if (str_starts(name, prefixes[i]))
			return true;
	return false;
}

static int open_disk(const Disk *d, int flags)
{
	char *dev = str_fmt("/dev/%s", d->name);
	int fd = (int)sys_open(dev, flags | O_CLOEXEC, 0);
	mem_free(dev);
	return fd;
}

static u64 free_space(const Disk *d, bool *has_esp)
{
	*has_esp = false;
	int fd = open_disk(d, O_RDONLY);
	if (fd < 0)
		return 0;
	GptPart parts[128];
	int n = gpt_read(fd, d->bytes / SECTOR, parts, 128, NULL);
	sys_close(fd);
	if (n < 0)
		return 0;
	for (int i = 0; i < n; i++)
		if (memcmp(parts[i].type, GPT_TYPE_ESP, 16) == 0)
			*has_esp = true;
	u64 a, b;
	return gpt_largest_gap(d->bytes / SECTOR, parts, n, &a, &b) * SECTOR;
}

static int cmd_disks(void)
{
	DirList list = { 0 };
	dir_list("/sys/block", &list, false);
	for (usize i = 0; i < list.len; i++) {
		const char *name = list.data[i].name;
		Disk d;
		if (skip_device(name) || !load_disk(name, &d) || d.bytes < (8ull << 30))
			continue;
		bool esp;
		u64 gap = free_space(&d, &esp);
		say("disk %s %llu %d %d %d %llu %d %s", d.name, (unsigned long long)d.bytes, d.removable || d.usb, d.usb, d.boot_medium,
		    (unsigned long long)gap, esp, d.model[0] ? d.model : "-");
		for (int k = 0; k < d.nparts; k++) {
			Part *p = &d.parts[k];
			say("part %s %llu %s %s", p->name, (unsigned long long)(p->sectors * 512), p->fs[0] ? p->fs : "-",
			    p->partname[0] ? p->partname : "-");
		}
	}
	dir_list_free(&list);
	return 0;
}

/* ---- The system image ------------------------------------------------------------------- */

/* Mounts dev read-only and reads the first of paths that exists; with a
 * marker, only from a file system that has it. */
static u8 *read_from(const char *dev, const char *fs, const char *marker, const char *const *paths, usize *len)
{
	dir_make(WORK "/medium", 0700);
	long r = sys_mount(dev, WORK "/medium", fs, MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	if (r < 0)
		return NULL;
	u8 *data = NULL;
	char *m = marker ? path_join(WORK "/medium", marker) : NULL;
	if (!m || file_exists(m))
		for (int i = 0; paths[i] && !data; i++) {
			char *p = path_join(WORK "/medium", paths[i]);
			data = (u8 *)file_read(p, len);
			mem_free(p);
		}
	mem_free(m);
	sys_umount(WORK "/medium", MNT_DETACH);
	return data;
}

/* Reads the bootable EFI file from what Zurbax started from: the install stick
 * (or the ISO written to one), a disc, or a stick the ISO's files were
 * copied onto. */
static u8 *read_system(usize *len)
{
	static const char *const efi[] = { "EFI/BOOT/BOOTX64.EFI", NULL };
	/* a disc shows its ISO 9660 names in lower case */
	static const char *const disc[] = { "efi/boot/bootx64.efi", NULL };
	DirList list = { 0 };
	dir_list("/sys/class/block", &list, false);
	u8 *data = NULL;
	for (int pass = 0; pass < 3 && !data; pass++)
		for (usize i = 0; i < list.len && !data; i++) {
			const char *name = list.data[i].name;
			char *f = str_fmt("/sys/class/block/%s/uevent", name);
			char *ue = file_read(f, NULL);
			mem_free(f);
			char part[40] = "", type[16] = "";
			uevent_value(ue, "PARTNAME", part, sizeof part);
			uevent_value(ue, "DEVTYPE", type, sizeof type);
			mem_free(ue);
			char *dev = str_fmt("/dev/%s", name);
			if (pass == 0 && str_eq(part, "ZURBAX-INSTALL"))
				data = read_from(dev, "vfat", NULL, efi, len);
			else if (pass == 1 && str_starts(name, "sr"))
				data = read_from(dev, "iso9660", "zos.txt", disc, len); /* not some other system's disc */
			else if (pass == 2 && str_eq(type, "partition"))
				data = read_from(dev, "vfat", "ZURBAX.TXT", efi, len);
			mem_free(dev);
		}
	dir_list_free(&list);
	return data;
}

/* ---- Firmware boot entry ---------------------------------------------------------------- */

static void put16(u8 *p, u16 v)
{
	p[0] = (u8)v, p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (u8)(v >> (i * 8));
}

static void put64(u8 *p, u64 v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (u8)(v >> (i * 8));
}

static usize put_utf16(u8 *p, const char *s)
{
	usize n = 0;
	for (; *s; s++, n += 2)
		put16(p + n, (u8)*s);
	put16(p + n, 0);
	return n + 2;
}

static bool efivar_mutable(const char *path)
{
	int fd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return true; /* does not exist yet */
	long flags = 0;
	if (sys_ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0 && (flags & FS_IMMUTABLE_FL)) {
		flags &= ~FS_IMMUTABLE_FL;
		sys_ioctl(fd, FS_IOC_SETFLAGS, &flags);
	}
	sys_close(fd);
	return true;
}

static bool efivar_write(const char *name, const u8 *data, usize len)
{
	char *path = str_fmt("%s/%s-%s", EFIVARS, name, GLOBAL_GUID);
	efivar_mutable(path);
	u8 *buf = mem_alloc(len + 4);
	put32(buf, 0x7); /* non-volatile, boot service and runtime access */
	memcpy(buf + 4, data, len);
	int fd = (int)sys_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	bool ok = fd >= 0 && sys_write(fd, buf, len + 4) == (long)(len + 4);
	if (fd >= 0)
		sys_close(fd);
	mem_free(buf);
	mem_free(path);
	return ok;
}

/* Returns the variable's data without the attribute word. */
static u8 *efivar_read(const char *name, usize *len)
{
	char *path = str_fmt("%s/%s-%s", EFIVARS, name, GLOBAL_GUID);
	usize n;
	u8 *raw = (u8 *)file_read(path, &n);
	mem_free(path);
	if (!raw || n < 4) {
		mem_free(raw);
		return NULL;
	}
	memmove(raw, raw + 4, n - 4);
	*len = n - 4;
	return raw;
}

/* Whether a boot option's UTF-16 description is exactly name. */
static bool option_named(const u8 *opt, usize len, const char *name)
{
	/* Attributes (4), path list length (2), then the description. */
	usize n = strlen(name);
	if (len < 6 + (n + 1) * 2)
		return false;
	for (usize i = 0; i <= n; i++) {
		u16 c = (u16)(opt[6 + i * 2] | opt[7 + i * 2] << 8);
		if (c != (u16)(i < n ? name[i] : 0))
			return false;
	}
	return true;
}

/* Ours: named for the system, or for its name before 2.0. */
static bool option_is_ours(const u8 *opt, usize len)
{
	return option_named(opt, len, OS_NAME) || option_named(opt, len, "Zurbax");
}

static bool add_boot_entry(u32 part_number, u64 start, u64 sectors, const u8 part_guid[16])
{
	if (!dir_exists(EFIVARS))
		return false;
	/* Reuse an older entry of ours, else take the first free number. */
	int number = -1, first_free = -1;
	for (int i = 0; i < 0x100; i++) {
		char name[16];
		fmt(name, sizeof name, "Boot%04X", i);
		usize len;
		u8 *opt = efivar_read(name, &len);
		if (!opt) {
			if (first_free < 0)
				first_free = i;
			continue;
		}
		bool ours = option_is_ours(opt, len);
		mem_free(opt);
		if (ours) {
			number = i;
			break;
		}
	}
	if (number < 0)
		number = first_free;
	if (number < 0)
		return false;

	u8 opt[512] = { 0 };
	usize n = 6;
	put32(opt, 1); /* active */
	n += put_utf16(opt + n, OS_NAME);
	usize path_start = n;
	/* Hard drive media path: which partition. */
	u8 *hd = opt + n;
	hd[0] = 4, hd[1] = 1;
	put16(hd + 2, 42);
	put32(hd + 4, part_number);
	put64(hd + 8, start);
	put64(hd + 16, sectors);
	memcpy(hd + 24, part_guid, 16);
	hd[40] = 2; /* GPT */
	hd[41] = 2; /* GUID signature */
	n += 42;
	/* File path on it. */
	u8 *fp = opt + n;
	fp[0] = 4, fp[1] = 4;
	usize name_bytes = put_utf16(fp + 4, EFI_PATH);
	put16(fp + 2, (u16)(4 + name_bytes));
	n += 4 + name_bytes;
	/* End of path. */
	opt[n] = 0x7f, opt[n + 1] = 0xff;
	put16(opt + n + 2, 4);
	n += 4;
	put16(opt + 4, (u16)(n - path_start));
	char name[16];
	fmt(name, sizeof name, "Boot%04X", number);
	if (!efivar_write(name, opt, n))
		return false;

	/* Zurbax first in the boot order; everything else keeps its place. */
	usize len = 0;
	u8 *order = efivar_read("BootOrder", &len);
	u8 *next = mem_alloc(len + 2);
	put16(next, (u16)number);
	usize k = 2;
	for (usize i = 0; i + 1 < len; i += 2) {
		u16 v = (u16)(order[i] | order[i + 1] << 8);
		if (v != number) {
			put16(next + k, v);
			k += 2;
		}
	}
	bool ok = efivar_write("BootOrder", next, k);
	mem_free(order);
	mem_free(next);
	return ok;
}

/* ---- Installing ------------------------------------------------------------------------- */

static bool write_zeros(int fd, u64 byte, u64 len)
{
	static u8 zero[1 << 16];
	while (len) {
		usize n = (usize)MIN(len, (u64)sizeof zero);
		if (sys_pwrite(fd, zero, n, (i64)byte) != (long)n)
			return false;
		byte += n;
		len -= n;
	}
	return true;
}

static void install_erase(Disk *d, int fd, const u8 *sys, usize sys_len)
{
	u64 sectors = d->bytes / SECTOR;
	step(0.08f, "Preparing the disk");
	/* Remove what identified the old contents: both partition tables and
	 * the start of the disk, where file system signatures live. */
	if (!write_zeros(fd, 0, 1 << 20) || !write_zeros(fd, (sectors - 2048) * SECTOR, 2048 * SECTOR))
		fail("The disk couldn't be written to.");
	GptPart parts[2] = { 0 };
	memcpy(parts[0].type, GPT_TYPE_ESP, 16);
	guid_random(parts[0].guid);
	parts[0].first_lba = 2048;
	parts[0].last_lba = 2048 + (ESP_MIB << 11) - 1;
	str_copy(parts[0].name, "EFI system partition", sizeof parts[0].name);
	memcpy(parts[1].type, GPT_TYPE_LINUX, 16);
	guid_random(parts[1].guid);
	parts[1].first_lba = parts[0].last_lba + 1;
	parts[1].last_lba = gpt_last_usable(sectors) / 2048 * 2048 - 1;
	str_copy(parts[1].name, "ZURBAX-DATA", sizeof parts[1].name);
	u8 disk_guid[16];
	guid_random(disk_guid);
	step(0.15f, "Creating partitions");
	if (!gpt_write(fd, sectors, parts, 2, disk_guid))
		fail("The partition table couldn't be written.");

	step(0.25f, "Copying " OS_NAME);
	FatWriter *w = fat_format(fd, parts[0].first_lba, parts[0].last_lba - parts[0].first_lba + 1, "Zurbax");
	if (!w)
		fail("The startup partition couldn't be formatted.");
	bool ok = fat_mkdir(w, "/EFI/ZURBAX") && fat_add_file(w, "/EFI/ZURBAX/ZURBAX.EFI", sys, sys_len) &&
		  /* The fallback path: firmware boots it even with no boot entry. */
		  fat_add_file(w, "/EFI/BOOT/BOOTX64.EFI", sys, sys_len);
	if (!fat_finish(w) || !ok)
		fail("Zurbax couldn't be copied to the disk.");

	step(0.55f, "Preparing storage for your files");
	u8 uuid[16];
	guid_random(uuid);
	if (!ext4_format(fd, parts[1].first_lba * SECTOR, (parts[1].last_lba - parts[1].first_lba + 1) * SECTOR, "ZURBAX-DATA", uuid))
		fail("The storage partition couldn't be formatted.");
	sys_fsync(fd);
	sys_ioctl(fd, BLKRRPART, NULL);

	step(0.85f, "Setting up startup");
	add_boot_entry(1, parts[0].first_lba, parts[0].last_lba - parts[0].first_lba + 1, parts[0].guid);
}

/* Puts the system in the disk's existing EFI system partition (shared with
 * Windows, often only 100 MB). Checked before anything on the disk changes:
 * the space free plus an older Zurbax being replaced must hold the new one. */
static void copy_to_esp(Disk *d, u32 slot, const u8 *sys, usize sys_len)
{
	char espdev[48] = { 0 };
	for (int i = 0; i < d->nparts; i++)
		if (d->parts[i].number == slot)
			fmt(espdev, sizeof espdev, "/dev/%s", d->parts[i].name);
	if (!espdev[0])
		fail("The EFI system partition couldn't be found.");
	dir_make(WORK "/esp", 0700);
	if (sys_mount(espdev, WORK "/esp", "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
		fail("The EFI system partition couldn't be opened.");
	struct statfs fs;
	u64 room = sys_statfs(WORK "/esp", &fs) == 0 ? fs.f_bavail * (u64)fs.f_bsize : 0;
	struct stat old;
	u64 old_size = sys_stat(WORK "/esp/EFI/ZURBAX/ZURBAX.EFI", &old) == 0 ? (u64)old.st_size : 0;
	u64 need = (u64)sys_len + (1u << 20);
	if (room + old_size < need) {
		sys_umount(WORK "/esp", 0);
		char msg[256];
		fmt(msg, sizeof msg,
		    "The EFI system partition has %llu MB free, and Zurbax needs %llu MB there. "
		    "Nothing on the disk was changed. Installing by erasing the disk gives Zurbax its own.",
		    (unsigned long long)((room + old_size) >> 20), (unsigned long long)(need >> 20));
		fail(msg);
	}
	/* short of room for both copies at once: the old one goes first */
	if (room < need)
		sys_unlink(WORK "/esp/EFI/ZURBAX/ZURBAX.EFI");
	dir_make(WORK "/esp/EFI/ZURBAX", 0755);
	bool ok = file_write(WORK "/esp/EFI/ZURBAX/ZURBAX.EFI.new", sys, sys_len) &&
		  sys_rename(WORK "/esp/EFI/ZURBAX/ZURBAX.EFI.new", WORK "/esp/EFI/ZURBAX/ZURBAX.EFI") == 0;
	sys_sync();
	sys_umount(WORK "/esp", 0);
	if (!ok)
		fail("There isn't enough room in the EFI system partition.");
}

static void install_alongside(Disk *d, int fd, const u8 *sys, usize sys_len)
{
	u64 sectors = d->bytes / SECTOR;
	step(0.08f, "Reading the disk");
	GptPart parts[129];
	u8 disk_guid[16];
	int n = gpt_read(fd, sectors, parts, 128, disk_guid);
	if (n < 0)
		fail("This disk has no GPT partition table, so Zurbax can only be installed by erasing it.");
	int esp = -1, data = -1;
	for (int i = 0; i < n; i++) {
		if (memcmp(parts[i].type, GPT_TYPE_ESP, 16) == 0 && esp < 0)
			esp = i;
		if (str_eq(parts[i].name, "ZURBAX-DATA"))
			data = i;
	}
	if (esp < 0)
		fail("This disk has no EFI system partition to start Zurbax from.");
	step(0.12f, "Copying " OS_NAME);
	copy_to_esp(d, parts[esp].slot, sys, sys_len);
	if (data < 0) {
		/* A new partition in the largest free space. */
		u64 a, b;
		u64 gap = gpt_largest_gap(sectors, parts, n, &a, &b);
		if (gap * SECTOR < ((u64)MIN_DATA_GIB << 30))
			fail("There isn't enough free space on this disk.");
		GptPart *p = &parts[n];
		memset(p, 0, sizeof *p);
		memcpy(p->type, GPT_TYPE_LINUX, 16);
		guid_random(p->guid);
		p->first_lba = a;
		p->last_lba = (b + 1) / 2048 * 2048 - 1;
		str_copy(p->name, "ZURBAX-DATA", sizeof p->name);
		data = n++;
		step(0.45f, "Creating a partition");
		if (!gpt_write(fd, sectors, parts, n, disk_guid))
			fail("The partition table couldn't be written.");
		sys_fsync(fd);
		/* The kernel learns about the new partition; the others aren't in use. */
		sys_ioctl(fd, BLKRRPART, NULL);
	}

	step(0.55f, "Preparing storage for your files");
	u8 uuid[16];
	guid_random(uuid);
	GptPart *p = &parts[data];
	if (!ext4_format(fd, p->first_lba * SECTOR, (p->last_lba - p->first_lba + 1) * SECTOR, "ZURBAX-DATA", uuid))
		fail("The storage partition couldn't be formatted.");
	sys_fsync(fd);

	step(0.85f, "Setting up startup");
	add_boot_entry(parts[esp].slot, parts[esp].first_lba, parts[esp].last_lba - parts[esp].first_lba + 1, parts[esp].guid);
}

/* The Linux layer's images (the layer, the firmware) that the live session
 * uses from the install medium go onto the new data partition, where zinit
 * finds them from then on. Not essential: without them Zurbax works and
 * Steam and GNOME say what's missing. */
static bool copy_file(const char *src, const char *dst, u64 *done, u64 total)
{
	char *part = str_fmt("%s.part", dst);
	int in = (int)sys_open(src, O_RDONLY | O_CLOEXEC, 0);
	int out = (int)sys_open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	bool ok = in >= 0 && out >= 0;
	usize chunk = 4u << 20;
	u8 *buf = ok ? mem_alloc(chunk) : NULL;
	static f32 shown;
	for (;;) {
		if (!ok)
			break;
		long n = sys_read(in, buf, chunk);
		if (n == -EINTR)
			continue;
		if (n == 0)
			break;
		ok = n > 0 && write_all(out, buf, (usize)n);
		*done += ok ? (u64)n : 0;
		f32 f = 0.88f + 0.1f * (f32)((f64)*done / (f64)MAX(total, 1));
		if (f - shown >= 0.01f) {
			step(f, "Copying the Linux layer");
			shown = f;
		}
	}
	mem_free(buf);
	ok = ok && sys_fsync(out) == 0;
	if (in >= 0)
		sys_close(in);
	if (out >= 0)
		sys_close(out);
	if (ok)
		ok = sys_rename(part, dst) == 0;
	else
		sys_unlink(part);
	mem_free(part);
	return ok;
}

static void copy_layers(const char *disk)
{
	static const char *const dirs[] = { "/run/zurbax/medium/LAYERS", "/run/zurbax/medium/layers" };
	const char *src = NULL;
	DirList l = { 0 };
	for (usize i = 0; i < ARRAY_LEN(dirs) && !src; i++)
		if (dir_list(dirs[i], &l, true))
			src = dirs[i];
	if (!src)
		return;
	u64 total = 0;
	for (usize i = 0; i < l.len; i++)
		total += (u64)l.data[i].size;
	step(0.88f, "Copying the Linux layer");
	/* the new partition's device appears once the kernel has reread the table */
	char dev[48] = { 0 };
	for (int tries = 0; tries < 50 && !dev[0]; tries++) {
		Disk d;
		if (load_disk(disk, &d))
			for (int i = 0; i < d.nparts; i++)
				if (str_eq(d.parts[i].partname, "ZURBAX-DATA"))
					fmt(dev, sizeof dev, "/dev/%s", d.parts[i].name);
		if (!dev[0] || !file_exists(dev))
			dev[0] = 0, sleep_ms(100);
	}
	dir_make(WORK "/data", 0700);
	if (!dev[0] || sys_mount(dev, WORK "/data", "ext4", MS_NOSUID | MS_NODEV, NULL) < 0) {
		say("note the Linux layer wasn't copied: the new storage partition couldn't be opened");
		dir_list_free(&l);
		return;
	}
	dir_make(WORK "/data/layers", 0755);
	u64 done = 0;
	bool ok = true;
	for (usize i = 0; i < l.len; i++) {
		char name[64];
		str_copy(name, l.data[i].name, sizeof name);
		for (char *c = name; *c; c++)
			*c = (char)to_lower((u8)*c);
		if (!str_ends(name, ".sfs"))
			continue;
		char *from = path_join(src, l.data[i].name), *to = str_fmt(WORK "/data/layers/%s", name);
		ok &= copy_file(from, to, &done, total);
		mem_free(from);
		mem_free(to);
	}
	dir_list_free(&l);
	sys_sync();
	sys_umount(WORK "/data", 0);
	if (!ok)
		say("note part of the Linux layer couldn't be copied; Zurbax works without it");
}

static int cmd_install(const char *name, const char *mode)
{
	bool erase = str_eq(mode, "erase");
	if (!erase && !str_eq(mode, "alongside"))
		fail("Unknown installation type.");
	Disk d;
	if (!load_disk(name, &d))
		fail("That disk isn't connected any more.");
	if (d.boot_medium)
		fail("Zurbax can't be installed on the stick it's running from.");
	step(0.02f, "Reading the install media");
	usize sys_len;
	u8 *sys = read_system(&sys_len);
	if (!sys || sys_len < (1u << 20))
		fail("The USB stick or disc " OS_NAME " started from couldn't be read. Make sure it's still there.");
	/* Exclusive: fails if anything on the disk is in use. */
	int fd = open_disk(&d, O_RDWR | O_EXCL);
	if (fd < 0)
		fail("The disk is in use. Close anything that has it open and try again.");
	int ssz = 0;
	if (sys_ioctl(fd, BLKSSZGET, &ssz) == 0 && ssz != (int)SECTOR)
		fail("Disks with 4K sectors aren't supported yet.");
	u64 bytes = 0;
	if (sys_ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes)
		d.bytes = bytes;
	if (erase)
		install_erase(&d, fd, sys, sys_len);
	else
		install_alongside(&d, fd, sys, sys_len);
	sys_fsync(fd);
	sys_close(fd);
	sys_sync();
	mem_free(sys);
	copy_layers(name);
	step(1, "Finished");
	say("done");
	return 0;
}

int main(int argc, char **argv)
{
	/* Only in the live session, which only process 1 can mark. */
	struct stat st;
	if (sys_stat("/run/zurbax-live", &st) < 0 || st.st_uid != 0) {
		say("error Zurbax can only be installed while running from the install media.");
		return 1;
	}
	if (sys_setresgid(0, 0, 0) < 0 || sys_setresuid(0, 0, 0) < 0) {
		say("error The installer needs administrator rights.");
		return 1;
	}
	sys_umask(022);
	dir_make(WORK, 0700);
	if (argc == 2 && str_eq(argv[1], "disks"))
		return cmd_disks();
	if (argc == 4 && str_eq(argv[1], "install"))
		return cmd_install(argv[2], argv[3]);
	eprint("usage: zinstall disks | install DISK erase|alongside\n");
	return 2;
}
