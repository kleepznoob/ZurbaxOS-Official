/*
 * winusb — makes a USB stick that installs Windows, from a Windows ISO.
 *
 *   sudo winusb WINDOWS.iso          shows the USB drives (and offers the
 *                                    only one, when there is one)
 *   sudo winusb WINDOWS.iso sdb      erases sdb and puts Windows on it
 *
 * The stick gets one FAT32 partition with the ISO's files, which is what a
 * PC's UEFI firmware boots (EFI/BOOT/BOOTX64.EFI, signed by Microsoft, so
 * Secure Boot can stay on). Writing the ISO byte for byte doesn't work for
 * Windows: its ISOs are made for DVDs only. Only USB drives are offered, so
 * the computer's own disk can't be erased by a typo.
 */
#include "zr.h"
#include "zd.h"

#define BLKRRPART 0x125F
#define BLKSSZGET 0x1268
#define BLKGETSIZE64 0x80081272
#define LOOP_CTL_GET_FREE 0x4C82
#define LOOP_CONFIGURE 0x4C0A
#define LO_FLAGS_READ_ONLY 1
#define LO_FLAGS_AUTOCLEAR 4

struct loop_info64 {
	u64 device, inode, rdevice, offset, sizelimit;
	u32 number, encrypt_type, encrypt_key_size, flags;
	u8 file_name[64], crypt_name[64], encrypt_key[32];
	u64 init[2];
};

struct loop_config {
	u32 fd, block_size;
	struct loop_info64 info;
	u64 reserved[8];
};

#define WORK "/run/winusb"
#define ISO_DIR WORK "/iso"
#define USB_DIR WORK "/usb"
#define FAT_MAX (4ull << 30) /* FAT32 keeps files under 4 GiB */
#define PART_MAX (32ull << 30) /* the biggest FAT32 partition Windows itself makes */

static bool iso_mounted, usb_mounted;

static void cleanup(void)
{
	if (usb_mounted && sys_umount(USB_DIR, 0) < 0)
		sys_umount(USB_DIR, MNT_DETACH);
	if (iso_mounted)
		sys_umount(ISO_DIR, MNT_DETACH);
	usb_mounted = iso_mounted = false;
}

static void fail(const char *f, ...) PRINTF_LIKE(1, 2);
static void fail(const char *f, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, f);
	fmt_v(msg, sizeof msg, f, ap);
	va_end(ap);
	eprint("\nwinusb: %s\n", msg);
	cleanup();
	exit(1);
}

static void size_text(u64 bytes, char *out, usize n)
{
	if (bytes >= 1000000000ull)
		fmt(out, n, "%llu.%llu GB", (unsigned long long)(bytes / 1000000000ull), (unsigned long long)(bytes / 100000000ull % 10));
	else
		fmt(out, n, "%llu MB", (unsigned long long)(bytes / 1000000ull));
}

/* ---- USB drives ---------------------------------------------------------------- */

typedef struct {
	char name[32];
	char label[128];
	u64 bytes;
} Drive;

static char *sys_line(const char *dev, const char *file)
{
	char path[160];
	fmt(path, sizeof path, "/sys/block/%s/%s", dev, file);
	char *s = file_read_line(path);
	return s ? s : str_dup("");
}

#ifdef WINUSB_TEST
/* tools/winusb-test.sh has a loop device stand in for a stick */
static bool test_disk(const char *name)
{
	const char *t = env_get("WINUSB_TEST_DISK");
	return t && str_eq(name, t);
}
#else
#define test_disk(name) false
#endif

/* Disks connected over USB: their sysfs path goes through a USB port. */
static int usb_drives(Drive *out, int max)
{
	DirList l = { 0 };
	if (!dir_list("/sys/block", &l, false))
		return 0;
	int n = 0;
	vec_each(&l, e) {
		char path[160], link[512] = { 0 };
		fmt(path, sizeof path, "/sys/block/%s", e->name);
		if (sys_readlink(path, link, sizeof link - 1) <= 0 || !(str_find(link, "/usb") || test_disk(e->name)))
			continue;
		char *size = sys_line(e->name, "size");
		u64 bytes = (u64)str_to_int(size, NULL) * 512;
		mem_free(size);
		if (!bytes || n >= max)
			continue; /* a card reader with no card in it */
		Drive *d = &out[n++];
		str_copy(d->name, e->name, sizeof d->name);
		char *vendor = sys_line(e->name, "device/vendor"), *model = sys_line(e->name, "device/model");
		char *v = str_trim(vendor), *m = str_trim(model);
		fmt(d->label, sizeof d->label, "%s%s%s", v, *v && *m ? " " : "", m);
		if (!d->label[0])
			str_copy(d->label, "USB drive", sizeof d->label);
		mem_free(vendor);
		mem_free(model);
		d->bytes = bytes;
	}
	dir_list_free(&l);
	return n;
}

static void show_drive(const Drive *d)
{
	char sz[32];
	size_text(d->bytes, sz, sizeof sz);
	print("  %-6s %-9s %s\n", d->name, sz, d->label);
}

/* "sdb1" for sdb, "mmcblk0p1" for mmcblk0 */
static void part_name(const char *disk, char *out, usize n)
{
	usize len = strlen(disk);
	fmt(out, n, "%s%s1", disk, len && is_digit((u8)disk[len - 1]) ? "p" : "");
}

/* Whether a file lives on the disk (any of its partitions). */
static bool file_on_disk(const char *file, const char *disk)
{
	struct stat st;
	if (sys_stat(file, &st) < 0)
		return false;
	char want[32];
	fmt(want, sizeof want, "%u:%u", (unsigned)((st.st_dev >> 8) & 0xfff), (unsigned)((st.st_dev & 0xff) | ((st.st_dev >> 12) & 0xfff00)));
	DirList l = { 0 };
	char path[160];
	fmt(path, sizeof path, "/sys/block/%s", disk);
	bool found = false;
	if (dir_list(path, &l, false)) {
		vec_each(&l, e) {
			if (!str_starts(e->name, disk))
				continue;
			char rel[96];
			fmt(rel, sizeof rel, "%s/dev", e->name);
			char *dev = sys_line(disk, rel);
			found = found || str_eq(dev, want);
			mem_free(dev);
		}
		dir_list_free(&l);
	}
	char *dev = sys_line(disk, "dev");
	found = found || str_eq(dev, want);
	mem_free(dev);
	return found;
}

/* Unmounts whatever the system mounted from the stick. */
static void unmount_disk(const char *disk)
{
	char prefix[48];
	fmt(prefix, sizeof prefix, "/dev/%s", disk);
	for (int round = 0; round < 3; round++) {
		char *m = file_read("/proc/self/mounts", NULL);
		bool any = false;
		for (char *line = m, *next; line && *line; line = next) {
			next = (char *)str_chr(line, '\n');
			if (next)
				*next++ = 0;
			char *sp = (char *)str_chr(line, ' ');
			if (!sp)
				continue;
			*sp = 0;
			if (!str_starts(line, prefix))
				continue;
			char *dir = sp + 1, *end = (char *)str_chr(dir, ' ');
			if (end)
				*end = 0;
			any = true;
			if (sys_umount(dir, 0) < 0)
				sys_umount(dir, MNT_DETACH);
		}
		mem_free(m);
		if (!any)
			return;
	}
}

static bool ask_yes(void)
{
	char buf[64];
	usize n = 0;
	while (n + 1 < sizeof buf) {
		long r = sys_read(0, buf + n, 1);
		if (r <= 0 || buf[n] == '\n')
			break;
		n++;
	}
	buf[n] = 0;
	return str_eq(str_trim(buf), "yes") || str_eq(str_trim(buf), "y");
}

/* ---- The ISO --------------------------------------------------------------------- */

static void mount_iso(const char *iso)
{
	int ctl = (int)sys_open("/dev/loop-control", O_RDWR | O_CLOEXEC, 0);
	long n = ctl >= 0 ? sys_ioctl(ctl, LOOP_CTL_GET_FREE, NULL) : -1;
	if (ctl >= 0)
		sys_close(ctl);
	if (n < 0)
		fail("no loop device to open the ISO with");
	char dev[32];
	fmt(dev, sizeof dev, "/dev/loop%ld", n);
	int file = (int)sys_open(iso, O_RDONLY | O_CLOEXEC, 0);
	if (file < 0)
		fail("can't open %s", iso);
	int lo = -1;
	for (int tries = 0; tries < 50 && lo < 0; tries++) {
		lo = (int)sys_open(dev, O_RDONLY | O_CLOEXEC, 0);
		if (lo < 0)
			sleep_ms(20);
	}
	if (lo < 0)
		fail("can't open %s", dev);
	/* the loop device goes away by itself once the ISO is unmounted */
	struct loop_config c = { .fd = (u32)file, .info.flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR };
	str_copy((char *)c.info.file_name, iso, sizeof c.info.file_name);
	long r = sys_ioctl(lo, LOOP_CONFIGURE, &c);
	if (r < 0)
		fail("can't open the ISO: %s", err_str(r));
	dir_make(ISO_DIR, 0700);
	/* Windows ISOs keep their files in UDF; the ISO 9660 side only has a note */
	r = sys_mount(dev, ISO_DIR, "udf", MS_RDONLY | MS_NOSUID | MS_NODEV, NULL);
	if (r < 0)
		r = sys_mount(dev, ISO_DIR, "iso9660", MS_RDONLY | MS_NOSUID | MS_NODEV, NULL);
	sys_close(lo);
	sys_close(file);
	if (r < 0)
		fail("%s doesn't look like a disc image (%s)", iso, err_str(r));
	iso_mounted = true;
}

typedef struct {
	u64 bytes, files, biggest;
	char biggest_name[256];
} Tally;

static void tally(const char *dir, const char *rel, Tally *t)
{
	DirList l = { 0 };
	if (!dir_list(dir, &l, true))
		fail("can't read %s on the ISO", rel[0] ? rel : "/");
	vec_each(&l, e) {
		char *p = path_join(dir, e->name), *r = str_fmt("%s/%s", rel, e->name);
		if (S_ISDIR(e->mode)) {
			tally(p, r, t);
		} else {
			t->bytes += (u64)e->size;
			t->files++;
			if ((u64)e->size > t->biggest) {
				t->biggest = (u64)e->size;
				str_copy(t->biggest_name, r, sizeof t->biggest_name);
			}
		}
		mem_free(p);
		mem_free(r);
	}
	dir_list_free(&l);
}

/* ---- Copying ----------------------------------------------------------------------- */

static u8 *buf;
#define BUF_SIZE (4u << 20)
static u64 copied, total;
static u32 shown = 1000;

static void show_progress(void)
{
	u32 pct = total ? (u32)(copied * 100 / total) : 100;
	if (pct == shown)
		return;
	shown = pct;
	char a[32], b[32];
	size_text(copied, a, sizeof a);
	size_text(total, b, sizeof b);
	print("\r  Copying Windows: %3u%%  (%s of %s)   ", pct, a, b);
}

static void copy_file(const char *from, const char *to, const char *rel)
{
	int in = (int)sys_open(from, O_RDONLY | O_CLOEXEC, 0);
	if (in < 0)
		fail("can't read %s on the ISO", rel);
	int out = (int)sys_open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0)
		fail("can't write %s on the stick", rel);
	for (;;) {
		long n = sys_read(in, buf, BUF_SIZE);
		if (n == -EINTR)
			continue;
		if (n < 0)
			fail("can't read %s on the ISO (%s); the download may be damaged", rel, err_str(n));
		if (n == 0)
			break;
		if (!write_all(out, buf, (usize)n))
			fail("writing %s to the stick failed; is it full or unplugged?", rel);
		copied += (u64)n;
		show_progress();
	}
	sys_close(in);
	if (sys_close(out) < 0)
		fail("writing %s to the stick failed", rel);
}

static void copy_tree(const char *from, const char *to, const char *rel)
{
	DirList l = { 0 };
	if (!dir_list(from, &l, true))
		fail("can't read %s on the ISO", rel[0] ? rel : "/");
	vec_each(&l, e) {
		char *a = path_join(from, e->name), *b = path_join(to, e->name), *r = str_fmt("%s/%s", rel, e->name);
		if (S_ISDIR(e->mode)) {
			if (sys_mkdir(b, 0755) < 0 && !dir_exists(b))
				fail("can't make %s on the stick", r);
			copy_tree(a, b, r);
		} else {
			copy_file(a, b, r);
		}
		mem_free(a);
		mem_free(b);
		mem_free(r);
	}
	dir_list_free(&l);
}

/* ---- The stick --------------------------------------------------------------------- */

static void put32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (u8)(v >> (i * 8));
}

/* One FAT32 partition, starting at 1 MiB, the way Windows lays out a stick. */
static void partition_and_format(const char *disk)
{
	char dev[48];
	fmt(dev, sizeof dev, "/dev/%s", disk);
	/* O_EXCL: refused while anything still has the stick mounted */
	int fd = (int)sys_open(dev, O_RDWR | O_EXCL | O_CLOEXEC, 0);
	if (fd < 0)
		fail("can't open %s (%s); unplug it, plug it back in and try again", dev, err_str(fd));
	u64 bytes = 0;
	int ssz = 0;
	if (sys_ioctl(fd, BLKGETSIZE64, &bytes) < 0 || sys_ioctl(fd, BLKSSZGET, &ssz) < 0)
		fail("can't read the size of %s", dev);
	if (ssz != 512)
		fail("%s uses %d-byte sectors, which this tool doesn't handle", dev, ssz);
	u64 sectors = bytes / 512;
	if (sectors < 2048 + 131072)
		fail("the stick is too small");
	u64 part = MIN(sectors - 2048, PART_MAX / 512);
	part &= ~2047ull; /* whole MiBs */

	/* Wipe what was there: old partition tables at both ends (GPT keeps a
	 * copy at the end) and old filesystem signatures. */
	u8 *zero = mem_zalloc(1 << 20);
	bool ok = (u64)sys_pwrite(fd, zero, 1 << 20, 0) == 1u << 20 &&
		  (u64)sys_pwrite(fd, zero, 1 << 20, (i64)(bytes - (1 << 20))) == 1u << 20;
	mem_free(zero);
	if (!ok)
		fail("can't write to %s; is it write-protected?", dev);

	u8 mbr[512] = { 0 };
	u32 sig;
	rand_bytes(&sig, 4);
	put32(mbr + 440, sig);
	u8 *p = mbr + 446;
	p[0] = 0x80; /* active */
	p[1] = 0xfe, p[2] = 0xff, p[3] = 0xff;
	p[4] = 0x0c; /* FAT32, LBA */
	p[5] = 0xfe, p[6] = 0xff, p[7] = 0xff;
	put32(p + 8, 2048);
	put32(p + 12, (u32)part);
	mbr[510] = 0x55, mbr[511] = 0xaa;
	if (sys_pwrite(fd, mbr, sizeof mbr, 0) != (long)sizeof mbr)
		fail("can't write the partition table");

	FatWriter *w = fat_format(fd, 2048, part, "WINDOWS");
	if (!w || !fat_finish(w))
		fail("formatting the stick failed");
	if (sys_fsync(fd) < 0)
		fail("writing to the stick failed");
	/* have the kernel read the new partition table */
	long r = -1;
	for (int tries = 0; tries < 20 && r < 0; tries++) {
		r = sys_ioctl(fd, BLKRRPART, NULL);
		if (r < 0)
			sleep_ms(250);
	}
	sys_close(fd);
	if (r < 0)
		fail("the system didn't see the new partition (%s); unplug the stick, plug it back in and try again", err_str(r));
}

static void mount_usb(const char *disk)
{
	char name[48], dev[64];
	part_name(disk, name, sizeof name);
	fmt(dev, sizeof dev, "/dev/%s", name);
	for (int tries = 0; tries < 100 && !file_exists(dev); tries++)
		sleep_ms(50);
	/* the desktop may mount the new partition by itself */
	sleep_ms(500);
	unmount_disk(disk);
	dir_make(USB_DIR, 0700);
	long r = sys_mount(dev, USB_DIR, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC, "shortname=mixed");
	if (r < 0)
		r = sys_mount(dev, USB_DIR, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	if (r < 0)
		fail("can't open the new partition %s (%s)", dev, err_str(r));
	usb_mounted = true;
}

int main(int argc, char **argv)
{
	const char *iso = NULL, *target = NULL;
	bool yes = false;
	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "-y") || str_eq(argv[i], "--yes"))
			yes = true;
		else if (!iso)
			iso = argv[i];
		else
			target = argv[i];
	}
	if (!iso) {
		eprint("usage: sudo winusb WINDOWS.iso [DRIVE]\n");
		return 2;
	}
	if (!file_exists(iso))
		fail("there's no file called %s (check the name: `ls ~/Downloads`)", iso);
	if (sys_geteuid() != 0)
		fail("this needs to erase a drive, so run it with sudo: sudo %s %s%s%s", argv[0], iso, target ? " " : "", target ? target : "");

	Drive drives[16];
	int nd = usb_drives(drives, (int)ARRAY_LEN(drives));
	if (target && str_starts(target, "/dev/"))
		target += 5;
	const Drive *d = NULL;
	for (int i = 0; target && i < nd; i++)
		if (str_eq(drives[i].name, target))
			d = &drives[i];
	if (target && !d) {
		eprint("winusb: %s isn't a USB drive. ", target);
		if (!nd)
			fail("No USB drive is plugged in.");
		eprint("The USB drives are:\n");
		for (int i = 0; i < nd; i++)
			show_drive(&drives[i]);
		cleanup();
		return 1;
	}
	if (!target) {
		if (!nd)
			fail("plug in a USB stick (8 GB or bigger) first");
		print("USB drives:\n");
		for (int i = 0; i < nd; i++)
			show_drive(&drives[i]);
		if (nd > 1) {
			print("\nRun it again with the one to use, e.g.: sudo %s %s %s\n", argv[0], iso, drives[0].name);
			return 0;
		}
		d = &drives[0];
	}
	if (file_on_disk(iso, d->name))
		fail("the ISO is on that USB drive itself; copy it to Downloads first, since the drive gets erased");

	/* Look at the ISO before touching the stick. */
	print("Opening %s...\n", iso);
	mount_iso(iso);
	Tally t = { 0 };
	tally(ISO_DIR, "", &t);
	char sz[32];
	if (!file_exists(ISO_DIR "/sources") && !file_exists(ISO_DIR "/SOURCES"))
		fail("that ISO doesn't look like Windows (it has no sources folder)");
	if (t.biggest >= FAT_MAX) {
		size_text(t.biggest, sz, sizeof sz);
		fail("%s is %s, too big for a FAT32 stick (the limit is 4 GB). Use a smaller Windows ISO.", t.biggest_name + 1, sz);
	}
	if (t.bytes + (64u << 20) > MIN(d->bytes, PART_MAX)) {
		size_text(t.bytes, sz, sizeof sz);
		fail("Windows needs %s and the stick is smaller than that", sz);
	}

	char dsz[32];
	size_text(d->bytes, dsz, sizeof dsz);
	print("\nEverything on %s (%s, %s) will be erased.\n", d->name, d->label, dsz);
	if (!yes) {
		print("Type yes and press Enter to continue: ");
		if (!ask_yes())
			fail("stopped; nothing was changed");
	}

	print("Erasing and formatting %s...\n", d->name);
	unmount_disk(d->name);
	partition_and_format(d->name);
	mount_usb(d->name);

	buf = mem_alloc(BUF_SIZE);
	total = t.bytes;
	copy_tree(ISO_DIR, USB_DIR, "");
	print("\n  Finishing (the stick may take a minute to catch up)...\n");
	sys_sync();
	if (sys_umount(USB_DIR, 0) < 0)
		fail("the stick is still busy; wait a moment, then unplug it");
	usb_mounted = false;
	cleanup();
	print("\nDone! The stick boots Windows Setup.\n"
	      "Restart, and as the logo shows press the boot menu key (F8 or Esc on ASUS,\n"
	      "F12 on most others), then pick the USB stick (the \"UEFI:\" entry).\n");
	return 0;
}
