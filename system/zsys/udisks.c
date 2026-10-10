/*
 * org.freedesktop.UDisks2: the disks, their partitions and file systems,
 * for GNOME's Files (GVfs lists drives through it and mounts USB sticks
 * with it) and Settings (the About page's storage).
 *
 * Drives and partitions come from the kernel (/sys/block); what each
 * holds (FAT, exFAT, NTFS, ext4, btrfs, XFS, ISO 9660...) and its label
 * from the first bytes on it, read here; mounts from /proc's mount table.
 * Mounting is done here as root, under /run/media/USER, with the options
 * that give the person their files; zinit shares mounts with the Linux
 * session, so they appear there at once. Zurbax's own partitions are
 * marked to be left alone.
 */
#include "zsys.h"

#define UD "org.freedesktop.UDisks2"
#define UD_PATH "/org/freedesktop/UDisks2"
#define MANAGER_PATH UD_PATH "/Manager"
#define MANAGER_IF UD ".Manager"
#define DRIVE_IF UD ".Drive"
#define BLOCK_IF UD ".Block"
#define FS_IF UD ".Filesystem"
#define PART_IF UD ".Partition"
#define TABLE_IF UD ".PartitionTable"
#define OM_IF "org.freedesktop.DBus.ObjectManager"

#define CDROMEJECT 0x5309

typedef struct {
	bool used, seen;
	char name[32], id[160];
	char vendor[64], model[128], serial[64], revision[32], bus[16];
	bool removable, ejectable, optical;
	u64 size, detected;
	u64 hash;
} Drive;

typedef struct {
	bool used, seen;
	char name[32], disk[32];     /* sda1, and its disk sda */
	bool partition;
	u32 major, minor, number;
	u64 size, offset;
	bool ro;
	char usage[16], type[24], version[16], label[128], uuid[64];
	char part_type[40], part_name[80], part_uuid[40];
	char table[8];               /* the partition table on a whole disk: gpt or dos */
	char mounts[4][256];
	int nmounts;
	u64 hash[4];
	bool exported[4];
} Block;

static Drive drives[32];
static Block blocks[128];

/* ---- Reading the kernel and the disks -------------------------------------------------------- */

static char *sysattr(const char *dir, const char *name)
{
	char *p = str_fmt("%s/%s", dir, name);
	char *v = file_read_line(p);
	mem_free(p);
	return v;
}

static void attr_copy(const char *dir, const char *name, char *out, usize cap)
{
	char *v = sysattr(dir, name);
	str_copy(out, v ? str_trim(v) : "", cap);
	mem_free(v);
}

static u64 attr_u64(const char *dir, const char *name)
{
	char *v = sysattr(dir, name);
	u64 r = v ? (u64)str_to_int(v, NULL) : 0;
	mem_free(v);
	return r;
}

static void trim_label(char *s)
{
	usize n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == 0))
		s[--n] = 0;
}

static void hexid(const u8 *b, int n, bool dashes, char *out, usize cap)
{
	usize o = 0;
	for (int i = 0; i < n && o + 3 < cap; i++) {
		if (dashes && (i == 4 || i == 6 || i == 8 || i == 10))
			out[o++] = '-';
		fmt(out + o, cap - o, "%02x", b[i]);
		o += 2;
	}
	out[o] = 0;
}

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* What a block device holds, from its first 128 KiB (the way blkid looks). */
static void probe(Block *b)
{
	b->usage[0] = b->type[0] = b->version[0] = b->label[0] = b->uuid[0] = b->table[0] = 0;
	char dev[48];
	fmt(dev, sizeof dev, "/dev/%s", b->name);
	int fd = (int)sys_open(dev, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
	if (fd < 0)
		return;
	enum { N = 128 * 1024 };
	u8 *d = mem_alloc(N);
	memset(d, 0, N);
	long got = sys_pread(fd, d, N, 0);
	sys_close(fd);
	if (got < 4096) {
		mem_free(d);
		return;
	}
#define FS(t, u) (str_copy(b->type, t, sizeof b->type), str_copy(b->usage, u, sizeof b->usage))
	if (!memcmp(d, "LUKS\xba\xbe", 6)) {
		FS("crypto_LUKS", "crypto");
		str_copy(b->uuid, (const char *)d + 168, MIN(sizeof b->uuid, 41));
	} else if (!memcmp(d + 3, "EXFAT   ", 8)) {
		FS("exfat", "filesystem");
		fmt(b->uuid, sizeof b->uuid, "%04X-%04X", le16(d + 0x66), le16(d + 0x64));
	} else if (!memcmp(d + 3, "NTFS    ", 8)) {
		FS("ntfs", "filesystem");
		u8 id[8];
		for (int i = 0; i < 8; i++)
			id[i] = d[0x48 + 7 - i];
		hexid(id, 8, false, b->uuid, sizeof b->uuid);
		for (char *c = b->uuid; *c; c++)
			*c = (char)to_upper((u8)*c);
	} else if (d[510] == 0x55 && d[511] == 0xaa && (!memcmp(d + 0x52, "FAT32   ", 8) || !memcmp(d + 0x36, "FAT1", 4))) {
		bool fat32 = !memcmp(d + 0x52, "FAT32   ", 8);
		FS("vfat", "filesystem");
		str_copy(b->version, fat32 ? "FAT32" : "FAT16", sizeof b->version);
		const u8 *lab = d + (fat32 ? 0x47 : 0x2b), *ser = d + (fat32 ? 0x43 : 0x27);
		char l[12] = { 0 };
		memcpy(l, lab, 11);
		trim_label(l);
		if (!str_eq(l, "NO NAME"))
			str_copy(b->label, l, sizeof b->label);
		fmt(b->uuid, sizeof b->uuid, "%04X-%04X", le16(ser + 2), le16(ser));
	} else if (le16(d + 1024 + 56) == 0xEF53) {
		const u8 *sb = d + 1024;
		u32 compat = le32(sb + 92), incompat = le32(sb + 96);
		FS(incompat & 0x40 ? "ext4" : compat & 0x4 ? "ext3" : "ext2", "filesystem");
		char l[17] = { 0 };
		memcpy(l, sb + 120, 16);
		str_copy(b->label, l, sizeof b->label);
		hexid(sb + 104, 16, true, b->uuid, sizeof b->uuid);
	} else if (!memcmp(d + 65536 + 64, "_BHRfS_M", 8)) {
		const u8 *sb = d + 65536;
		FS("btrfs", "filesystem");
		char l[257] = { 0 };
		memcpy(l, sb + 299, 256);
		str_copy(b->label, l, sizeof b->label);
		hexid(sb + 32, 16, true, b->uuid, sizeof b->uuid);
	} else if (!memcmp(d, "XFSB", 4)) {
		FS("xfs", "filesystem");
		char l[13] = { 0 };
		memcpy(l, d + 108, 12);
		str_copy(b->label, l, sizeof b->label);
		hexid(d + 32, 16, true, b->uuid, sizeof b->uuid);
	} else if (le32(d + 1024) == 0xF2F52010) {
		FS("f2fs", "filesystem");
		hexid(d + 1024 + 0x6c, 16, true, b->uuid, sizeof b->uuid);
	} else if (!memcmp(d + 32769, "CD001", 5)) {
		FS("iso9660", "filesystem");
		char l[33] = { 0 };
		memcpy(l, d + 32808, 32);
		trim_label(l);
		str_copy(b->label, l, sizeof b->label);
		/* the volume's creation time stands in for an id, as blkid does */
		const char *t = (const char *)d + 32768 + 813;
		fmt(b->uuid, sizeof b->uuid, "%.4s-%.2s-%.2s-%.2s-%.2s-%.2s-%.2s", t, t + 4, t + 6, t + 8, t + 10, t + 12, t + 14);
	} else if (!memcmp(d + 4096 - 10, "SWAPSPACE2", 10)) {
		FS("swap", "other");
		hexid(d + 1024 + 12, 16, true, b->uuid, sizeof b->uuid);
	} else if (!b->partition && !memcmp(d + 512, "EFI PART", 8)) {
		str_copy(b->table, "gpt", sizeof b->table);
	} else if (!b->partition && d[510] == 0x55 && d[511] == 0xaa && (d[446 + 4] || d[462 + 4])) {
		str_copy(b->table, "dos", sizeof b->table);
	}
#undef FS
	mem_free(d);
}

/* A partition's type and name from the disk's partition table. */
static void partition_info(Block *p, const char *table)
{
	p->part_type[0] = p->part_name[0] = p->part_uuid[0] = 0;
	char dev[48];
	fmt(dev, sizeof dev, "/dev/%s", p->disk);
	int fd = (int)sys_open(dev, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
	if (fd < 0)
		return;
	if (str_eq(table, "gpt")) {
		u8 h[512];
		if (sys_pread(fd, h, 512, 512) == 512 && !memcmp(h, "EFI PART", 8)) {
			u64 lba = (u64)le32(h + 72) | (u64)le32(h + 76) << 32;
			u32 count = le32(h + 80), esize = le32(h + 84);
			if (p->number >= 1 && p->number <= count && esize >= 128 && esize <= 512) {
				u8 e[512];
				if (sys_pread(fd, e, esize, (i64)(lba * 512 + (u64)(p->number - 1) * esize)) == (long)esize) {
					/* the GUID's first three parts are little-endian */
					u8 g[16] = { e[3], e[2], e[1], e[0], e[5], e[4], e[7], e[6] };
					memcpy(g + 8, e + 8, 8);
					hexid(g, 16, true, p->part_type, sizeof p->part_type);
					u8 u[16] = { e[19], e[18], e[17], e[16], e[21], e[20], e[23], e[22] };
					memcpy(u + 8, e + 24, 8);
					hexid(u, 16, true, p->part_uuid, sizeof p->part_uuid);
					usize o = 0;
					for (int i = 0; i < 36 && o + 1 < sizeof p->part_name; i++) {
						u16 ch = le16(e + 56 + i * 2);
						if (!ch)
							break;
						p->part_name[o++] = ch < 128 ? (char)ch : '?';
					}
					p->part_name[o] = 0;
				}
			}
		}
	} else if (str_eq(table, "dos") && p->number >= 1 && p->number <= 4) {
		u8 m[512];
		if (sys_pread(fd, m, 512, 0) == 512)
			fmt(p->part_type, sizeof p->part_type, "0x%02x", m[446 + (p->number - 1) * 16 + 4]);
	}
	sys_close(fd);
}

static Block *find_block(const char *name)
{
	for (usize i = 0; i < ARRAY_LEN(blocks); i++)
		if (blocks[i].used && str_eq(blocks[i].name, name))
			return &blocks[i];
	return NULL;
}

static Block *new_block(const char *name)
{
	Block *b = find_block(name);
	if (b)
		return b;
	for (usize i = 0; i < ARRAY_LEN(blocks); i++)
		if (!blocks[i].used) {
			blocks[i] = (Block){ .used = true };
			str_copy(blocks[i].name, name, sizeof blocks[i].name);
			return &blocks[i];
		}
	return NULL;
}

static Drive *drive_of(const Block *b)
{
	const char *disk = b->partition ? b->disk : b->name;
	for (usize i = 0; i < ARRAY_LEN(drives); i++)
		if (drives[i].used && str_eq(drives[i].name, disk))
			return &drives[i];
	return NULL;
}

/* The serial number: the device's own, or the USB device's above it. */
static void find_serial(const char *sysdir, char *out, usize cap)
{
	char *dev = str_fmt("%s/device", sysdir);
	attr_copy(dev, "serial", out, cap);
	char path[512];
	long n = sys_readlink(dev, path, sizeof path - 1);
	mem_free(dev);
	if (out[0] || n <= 0)
		return;
	/* walk up from the SCSI device to the USB device, which has "serial" */
	char real[600];
	fmt(real, sizeof real, "%s/%.*s", sysdir, (int)n, path);
	char *norm = path_normalize(real);
	for (int up = 0; up < 8 && !out[0]; up++) {
		char *s = str_fmt("%s/serial", norm);
		char *v = file_read_line(s);
		if (v && *v && up > 1)
			str_copy(out, str_trim(v), cap);
		mem_free(v);
		mem_free(s);
		char *parent = path_dir(norm);
		mem_free(norm);
		norm = parent;
	}
	mem_free(norm);
}

static void read_mounts(void)
{
	for (usize i = 0; i < ARRAY_LEN(blocks); i++)
		blocks[i].nmounts = 0;
	char *t = file_read("/proc/self/mountinfo", NULL);
	usize n = 0;
	char **lines = str_split(t ? t : "", '\n', &n);
	for (usize i = 0; i < n; i++) {
		/* id parent major:minor root mountpoint ... */
		usize nf = 0;
		char **f = str_split(lines[i], ' ', &nf);
		if (nf >= 5) {
			const char *colon = str_chr(f[2], ':');
			u32 maj = (u32)str_to_int(f[2], NULL), min = colon ? (u32)str_to_int(colon + 1, NULL) : 0;
			for (usize k = 0; k < ARRAY_LEN(blocks); k++) {
				Block *b = &blocks[k];
				if (b->used && b->major == maj && b->minor == min && b->nmounts < 4 && maj != 0) {
					/* mountinfo writes spaces in paths as \040 */
					StrBuf sb = { 0 };
					for (const char *c = f[4]; *c; c++) {
						if (c[0] == '\\' && is_digit((u8)c[1]) && is_digit((u8)c[2]) && is_digit((u8)c[3])) {
							sb_addc(&sb, (char)((c[1] - '0') * 64 + (c[2] - '0') * 8 + (c[3] - '0')));
							c += 3;
						} else {
							sb_addc(&sb, *c);
						}
					}
					str_copy(b->mounts[b->nmounts++], sb_str(&sb), sizeof b->mounts[0]);
					sb_free(&sb);
				}
			}
		}
		str_split_free(f, nf);
	}
	str_split_free(lines, n);
	mem_free(t);
}

static bool interesting(const char *name)
{
	static const char *const skip[] = { "loop", "ram", "zram", "dm-", "md", "nbd", "fd" };
	for (usize i = 0; i < ARRAY_LEN(skip); i++)
		if (str_starts(name, skip[i]))
			return false;
	return true;
}

static void scan(void)
{
	for (usize i = 0; i < ARRAY_LEN(drives); i++)
		drives[i].seen = false;
	for (usize i = 0; i < ARRAY_LEN(blocks); i++)
		blocks[i].seen = false;
	DirList l = { 0 };
	if (!dir_list("/sys/block", &l, false))
		return;
	for (usize k = 0; k < l.len; k++) {
		const char *name = l.data[k].name;
		if (!interesting(name))
			continue;
		char *sys = str_fmt("/sys/block/%s", name);
		Drive *d = NULL;
		for (usize i = 0; i < ARRAY_LEN(drives) && !d; i++)
			if (drives[i].used && str_eq(drives[i].name, name))
				d = &drives[i];
		if (!d)
			for (usize i = 0; i < ARRAY_LEN(drives) && !d; i++)
				if (!drives[i].used) {
					d = &drives[i];
					*d = (Drive){ .used = true, .detected = (u64)time_unix_ms() * 1000 };
					str_copy(d->name, name, sizeof d->name);
				}
		if (!d) {
			mem_free(sys);
			continue;
		}
		d->seen = true;
		char *dev = str_fmt("%s/device", sys);
		attr_copy(dev, "vendor", d->vendor, sizeof d->vendor);
		attr_copy(dev, "model", d->model, sizeof d->model);
		attr_copy(dev, "rev", d->revision, sizeof d->revision);
		if (!d->revision[0])
			attr_copy(dev, "firmware_rev", d->revision, sizeof d->revision);
		if (!d->model[0])
			attr_copy(dev, "name", d->model, sizeof d->model);   /* SD cards */
		mem_free(dev);
		find_serial(sys, d->serial, sizeof d->serial);
		char real[512] = { 0 };
		sys_readlink(sys, real, sizeof real - 1);
		str_copy(d->bus, str_find(real, "/usb") ? "usb" : str_starts(name, "nvme") ? "nvme" : str_find(real, "/mmc") ? "sdio"
					: str_find(real, "/ata") ? "ata" : "", sizeof d->bus);
		d->optical = str_starts(name, "sr");
		d->removable = attr_u64(sys, "removable") || str_eq(d->bus, "usb") || str_eq(d->bus, "sdio");
		d->ejectable = d->optical || str_eq(d->bus, "usb") || str_eq(d->bus, "sdio");
		d->size = attr_u64(sys, "size") * 512;
		char id[160];
		fmt(id, sizeof id, "%s-%s-%s", d->vendor[0] ? d->vendor : "drive", d->model[0] ? d->model : name, d->serial[0] ? d->serial : name);
		usize o = 0;
		for (char *c = id; *c && o + 1 < sizeof d->id; c++)
			d->id[o++] = is_alnum((u8)*c) ? *c : '_';
		d->id[o] = 0;

		/* the whole disk as a block device, then its partitions */
		Block *whole = new_block(name);
		if (whole) {
			whole->seen = true;
			whole->partition = false;
			char *devnum = sysattr(sys, "dev");
			if (devnum) {
				whole->major = (u32)str_to_int(devnum, NULL);
				const char *c = str_chr(devnum, ':');
				whole->minor = c ? (u32)str_to_int(c + 1, NULL) : 0;
			}
			mem_free(devnum);
			u64 size = d->size;
			bool changed = size != whole->size;
			whole->size = size;
			whole->ro = attr_u64(sys, "ro") != 0;
			if (changed || !whole->hash[0])
				probe(whole);
		}
		DirList pl = { 0 };
		if (dir_list(sys, &pl, false)) {
			for (usize i = 0; i < pl.len; i++) {
				const char *pn = pl.data[i].name;
				char *pdir = str_fmt("%s/%s", sys, pn);
				char *pf = str_fmt("%s/partition", pdir);
				if (file_exists(pf)) {
					Block *p = new_block(pn);
					if (p) {
						p->seen = true;
						p->partition = true;
						str_copy(p->disk, name, sizeof p->disk);
						p->number = (u32)attr_u64(pdir, "partition");
						u64 size = attr_u64(pdir, "size") * 512;
						p->offset = attr_u64(pdir, "start") * 512;
						p->ro = attr_u64(pdir, "ro") != 0;
						char *devnum = sysattr(pdir, "dev");
						if (devnum) {
							p->major = (u32)str_to_int(devnum, NULL);
							const char *c = str_chr(devnum, ':');
							p->minor = c ? (u32)str_to_int(c + 1, NULL) : 0;
						}
						mem_free(devnum);
						bool changed = size != p->size;
						p->size = size;
						if (changed || !p->hash[0]) {
							probe(p);
							partition_info(p, whole ? whole->table : "");
						}
					}
				}
				mem_free(pf);
				mem_free(pdir);
			}
			dir_list_free(&pl);
		}
		mem_free(sys);
	}
	dir_list_free(&l);
	read_mounts();
}

/* ---- Hints: what GNOME shows ---------------------------------------------------------------- */

static bool hidden_partition(const Block *b)
{
	static const char *const types[] = {
		"c12a7328-f81f-11d2-ba4b-00a0c93ec93b",   /* EFI system */
		"e3c9e316-0b5c-4db8-817d-f92df00215ae",   /* Microsoft reserved */
		"de94bba4-06d1-4d40-a16a-bfd50179d6ac",   /* Windows recovery */
		"21686148-6449-6e6f-744e-656564454649",   /* BIOS boot */
		"0657fd6d-a4ab-43c4-84e5-0933c84b4f4f",   /* Linux swap */
	};
	for (usize i = 0; i < ARRAY_LEN(types); i++)
		if (str_eq(b->part_type, types[i]))
			return true;
	return str_starts(b->part_name, "ZURBAX-") || str_eq(b->usage, "other") || str_eq(b->part_type, "0xef") ||
	       str_eq(b->part_type, "0x27");
}

/* ---- Properties ------------------------------------------------------------------------------ */

static char *drive_path(const Drive *d)
{
	return str_fmt(UD_PATH "/drives/%s", d->id);
}

static char *block_path(const Block *b)
{
	char *e = path_escape(b->name);
	char *p = str_fmt(UD_PATH "/block_devices/%s", e);
	mem_free(e);
	return p;
}

static void add_bytes(DBusMsg *m, const char *s)
{
	dbus_open(m, "ay");
	for (const char *c = s; *c; c++)
		dbus_add(m, "y", (u8)*c);
	dbus_add(m, "y", 0);
	dbus_close(m);
}

#define GET(name) static void name(DBusMsg *m, void *obj)
#define D ((Drive *)obj)
#define B ((Block *)obj)

GET(d_vendor) { dbus_add(m, "s", D->vendor); }
GET(d_model) { dbus_add(m, "s", D->model); }
GET(d_revision) { dbus_add(m, "s", D->revision); }
GET(d_serial) { dbus_add(m, "s", D->serial); }
GET(d_empty) { (void)obj; dbus_add(m, "s", ""); }
GET(d_id) { dbus_add(m, "s", D->id); }
GET(d_config) { (void)obj; dbus_open(m, "a{sv}"); dbus_close(m); }
GET(d_media) { dbus_add(m, "s", D->optical ? "optical_cd" : D->removable && str_eq(D->bus, "sdio") ? "flash_sd" : ""); }
GET(d_compat) { dbus_open(m, "as"); if (D->optical) dbus_add(m, "s", "optical_cd"); dbus_close(m); }
GET(d_media_removable) { dbus_add(m, "b", D->optical || str_eq(D->bus, "sdio")); }
GET(d_media_available) { dbus_add(m, "b", D->size > 0); }
GET(d_true) { (void)obj; dbus_add(m, "b", true); }
GET(d_false) { (void)obj; dbus_add(m, "b", false); }
GET(d_size) { dbus_add(m, "t", D->size); }
GET(d_detected) { dbus_add(m, "t", D->detected); }
GET(d_optical) { dbus_add(m, "b", D->optical); }
GET(d_zero_u) { (void)obj; dbus_add(m, "u", 0u); }
GET(d_rotation) { char p[64]; fmt(p, sizeof p, "/sys/block/%s/queue/rotational", D->name); char *v = file_read_line(p); dbus_add(m, "i", v && v[0] == '1' ? -1 : 0); mem_free(v); }
GET(d_bus) { dbus_add(m, "s", D->bus); }
GET(d_seat) { (void)obj; dbus_add(m, "s", "seat0"); }
GET(d_removable) { dbus_add(m, "b", D->removable); }
GET(d_ejectable) { dbus_add(m, "b", D->ejectable); }
GET(d_sort) { char s[200]; fmt(s, sizeof s, "%s%s", D->removable ? "01hotplug/" : "00coldplug/", D->name); dbus_add(m, "s", s); }
GET(d_power_off) { dbus_add(m, "b", str_eq(D->bus, "usb")); }

static const Prop DRIVE_PROPS[] = {
	{ "Vendor", "s", d_vendor, NULL },
	{ "Model", "s", d_model, NULL },
	{ "Revision", "s", d_revision, NULL },
	{ "Serial", "s", d_serial, NULL },
	{ "WWN", "s", d_empty, NULL },
	{ "Id", "s", d_id, NULL },
	{ "Configuration", "a{sv}", d_config, NULL },
	{ "Media", "s", d_media, NULL },
	{ "MediaCompatibility", "as", d_compat, NULL },
	{ "MediaRemovable", "b", d_media_removable, NULL },
	{ "MediaAvailable", "b", d_media_available, NULL },
	{ "MediaChangeDetected", "b", d_true, NULL },
	{ "Size", "t", d_size, NULL },
	{ "TimeDetected", "t", d_detected, NULL },
	{ "TimeMediaDetected", "t", d_detected, NULL },
	{ "Optical", "b", d_optical, NULL },
	{ "OpticalBlank", "b", d_false, NULL },
	{ "OpticalNumTracks", "u", d_zero_u, NULL },
	{ "OpticalNumAudioTracks", "u", d_zero_u, NULL },
	{ "OpticalNumDataTracks", "u", d_zero_u, NULL },
	{ "OpticalNumSessions", "u", d_zero_u, NULL },
	{ "RotationRate", "i", d_rotation, NULL },
	{ "ConnectionBus", "s", d_bus, NULL },
	{ "Seat", "s", d_seat, NULL },
	{ "Removable", "b", d_removable, NULL },
	{ "Ejectable", "b", d_ejectable, NULL },
	{ "SortKey", "s", d_sort, NULL },
	{ "CanPowerOff", "b", d_power_off, NULL },
	{ "SiblingId", "s", d_empty, NULL },
};

GET(b_device) { char p[48]; fmt(p, sizeof p, "/dev/%s", B->name); add_bytes(m, p); }
GET(b_symlinks)
{
	dbus_open(m, "aay");
	char p[160];
	if (B->uuid[0]) {
		fmt(p, sizeof p, "/dev/disk/by-uuid/%s", B->uuid);
		add_bytes(m, p);
	}
	if (B->label[0]) {
		fmt(p, sizeof p, "/dev/disk/by-label/%s", B->label);
		add_bytes(m, p);
	}
	dbus_close(m);
}
GET(b_devnum) { dbus_add(m, "t", (u64)B->major << 8 | B->minor); }
GET(b_id) { char s[200]; fmt(s, sizeof s, "by-uuid-%s", B->uuid[0] ? B->uuid : B->name); dbus_add(m, "s", s); }
GET(b_size) { dbus_add(m, "t", B->size); }
GET(b_ro) { dbus_add(m, "b", B->ro); }
GET(b_drive) { Drive *d = drive_of(B); if (d) { char *p = drive_path(d); dbus_add(m, "o", p); mem_free(p); } else dbus_add(m, "o", "/"); }
GET(b_none_o) { (void)obj; dbus_add(m, "o", "/"); }
GET(b_usage) { dbus_add(m, "s", B->usage); }
GET(b_type) { dbus_add(m, "s", B->type); }
GET(b_version) { dbus_add(m, "s", B->version); }
GET(b_label) { dbus_add(m, "s", B->label); }
GET(b_uuid) { dbus_add(m, "s", B->uuid); }
GET(b_config) { (void)obj; dbus_open(m, "a(sa{sv})"); dbus_close(m); }
GET(b_partitionable) { dbus_add(m, "b", !B->partition); }
GET(b_system) { Drive *d = drive_of(B); dbus_add(m, "b", !d || !d->removable); }
GET(b_ignore) { dbus_add(m, "b", hidden_partition(B)); }
GET(b_auto) { Drive *d = drive_of(B); dbus_add(m, "b", d && d->removable); }
GET(b_hint_name) { dbus_add(m, "s", B->label); }
GET(b_hint_icon) { (void)obj; dbus_add(m, "s", ""); }
GET(b_empty_as) { (void)obj; dbus_open(m, "as"); dbus_close(m); }

static const Prop BLOCK_PROPS[] = {
	{ "Device", "ay", b_device, NULL },
	{ "PreferredDevice", "ay", b_device, NULL },
	{ "Symlinks", "aay", b_symlinks, NULL },
	{ "DeviceNumber", "t", b_devnum, NULL },
	{ "Id", "s", b_id, NULL },
	{ "Size", "t", b_size, NULL },
	{ "ReadOnly", "b", b_ro, NULL },
	{ "Drive", "o", b_drive, NULL },
	{ "MDRaid", "o", b_none_o, NULL },
	{ "MDRaidMember", "o", b_none_o, NULL },
	{ "IdUsage", "s", b_usage, NULL },
	{ "IdType", "s", b_type, NULL },
	{ "IdVersion", "s", b_version, NULL },
	{ "IdLabel", "s", b_label, NULL },
	{ "IdUUID", "s", b_uuid, NULL },
	{ "Configuration", "a(sa{sv})", b_config, NULL },
	{ "CryptoBackingDevice", "o", b_none_o, NULL },
	{ "HintPartitionable", "b", b_partitionable, NULL },
	{ "HintSystem", "b", b_system, NULL },
	{ "HintIgnore", "b", b_ignore, NULL },
	{ "HintAuto", "b", b_auto, NULL },
	{ "HintName", "s", b_hint_name, NULL },
	{ "HintIconName", "s", b_hint_icon, NULL },
	{ "HintSymbolicIconName", "s", b_hint_icon, NULL },
	{ "UserspaceMountOptions", "as", b_empty_as, NULL },
};

GET(f_mounts)
{
	dbus_open(m, "aay");
	for (int i = 0; i < B->nmounts; i++)
		add_bytes(m, B->mounts[i]);
	dbus_close(m);
}

static const Prop FS_PROPS[] = {
	{ "MountPoints", "aay", f_mounts, NULL },
	{ "Size", "t", b_size, NULL },
};

GET(p_number) { dbus_add(m, "u", B->number); }
GET(p_type) { dbus_add(m, "s", B->part_type); }
GET(p_flags) { (void)obj; dbus_add(m, "t", (u64)0); }
GET(p_offset) { dbus_add(m, "t", B->offset); }
GET(p_name) { dbus_add(m, "s", B->part_name); }
GET(p_uuid) { dbus_add(m, "s", B->part_uuid); }
GET(p_table) { Block *disk = find_block(B->disk); if (disk) { char *p = block_path(disk); dbus_add(m, "o", p); mem_free(p); } else dbus_add(m, "o", "/"); }

static const Prop PART_PROPS[] = {
	{ "Number", "u", p_number, NULL },
	{ "Type", "s", p_type, NULL },
	{ "Flags", "t", p_flags, NULL },
	{ "Offset", "t", p_offset, NULL },
	{ "Size", "t", b_size, NULL },
	{ "Name", "s", p_name, NULL },
	{ "UUID", "s", p_uuid, NULL },
	{ "Table", "o", p_table, NULL },
	{ "IsContainer", "b", d_false, NULL },
	{ "IsContained", "b", d_false, NULL },
};

GET(t_partitions)
{
	dbus_open(m, "ao");
	for (usize i = 0; i < ARRAY_LEN(blocks); i++)
		if (blocks[i].used && blocks[i].partition && str_eq(blocks[i].disk, B->name)) {
			char *p = block_path(&blocks[i]);
			dbus_add(m, "o", p);
			mem_free(p);
		}
	dbus_close(m);
}
GET(t_type) { dbus_add(m, "s", B->table); }

static const Prop TABLE_PROPS[] = {
	{ "Partitions", "ao", t_partitions, NULL },
	{ "Type", "s", t_type, NULL },
};

GET(m_version) { (void)obj; dbus_add(m, "s", "2.11.1"); }
GET(m_fs)
{
	(void)obj;
	static const char *const fs[] = { "vfat", "exfat", "ntfs", "ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "iso9660", "udf" };
	dbus_open(m, "as");
	for (usize i = 0; i < ARRAY_LEN(fs); i++)
		dbus_add(m, "s", fs[i]);
	dbus_close(m);
}
GET(m_luks) { (void)obj; dbus_open(m, "as"); dbus_add(m, "s", "luks2"); dbus_close(m); }
GET(m_luks_default) { (void)obj; dbus_add(m, "s", "luks2"); }

static const Prop MANAGER_PROPS[] = {
	{ "Version", "s", m_version, NULL },
	{ "SupportedFilesystems", "as", m_fs, NULL },
	{ "SupportedEncryptionTypes", "as", m_luks, NULL },
	{ "DefaultEncryptionType", "s", m_luks_default, NULL },
};

#undef D
#undef B

static const Iface I_DRIVE = IFACE(DRIVE_IF, DRIVE_PROPS), I_BLOCK = IFACE(BLOCK_IF, BLOCK_PROPS), I_FS = IFACE(FS_IF, FS_PROPS),
		   I_PART = IFACE(PART_IF, PART_PROPS), I_TABLE = IFACE(TABLE_IF, TABLE_PROPS), I_MANAGER = IFACE(MANAGER_IF, MANAGER_PROPS);

/* The interfaces a block device has: always Block; Filesystem when it
 * holds one; Partition and PartitionTable as the case may be. */
static const Iface *const *block_ifaces(const Block *b, int *n)
{
	static const Iface *list[4];
	int k = 0;
	list[k++] = &I_BLOCK;
	if (str_eq(b->usage, "filesystem"))
		list[k++] = &I_FS;
	if (b->partition)
		list[k++] = &I_PART;
	if (b->table[0])
		list[k++] = &I_TABLE;
	*n = k;
	return list;
}

/* ---- Methods ------------------------------------------------------------------------------------- */

static u32 sender_uid(DBus *bus, DBusMsg *call)
{
	u32 pid = dbus_peer_pid(bus, dbus_sender(call));
	if (!pid)
		return (u32)-1;
	char path[64];
	fmt(path, sizeof path, "/proc/%u/status", pid);
	char *t = file_read(path, NULL);
	const char *u = t ? str_find(t, "\nUid:") : NULL;
	u32 uid = (u32)-1;
	if (u) {
		u += 5;
		while (*u == ' ' || *u == '\t')
			u++;
		uid = (u32)str_to_int(u, NULL);
	}
	mem_free(t);
	return uid;
}

static bool allowed(DBus *bus, DBusMsg *call)
{
	u32 uid = sender_uid(bus, call);
	if (uid == Z.uid || uid == 0)
		return true;
	dbus_reply_error(bus, call, "org.freedesktop.UDisks2.Error.NotAuthorized", "Not allowed");
	return false;
}

static void refresh(void);

/* A folder of its own under /run/media/USER, named for the volume. */
static void mount_dir(const Block *b, char *out, usize cap)
{
	char base[160];
	str_copy(base, b->label[0] ? b->label : b->uuid[0] ? b->uuid : b->name, sizeof base);
	for (char *c = base; *c; c++)
		if (*c == '/' || (u8)*c < 32)
			*c = '_';
	char *user = str_fmt("/run/media/%s", Z.user);
	dir_make("/run/media", 0755);
	dir_make(user, 0750);
	sys_chown(user, 0, Z.gid);
	struct stat st;
	fmt(out, cap, "%s/%s", user, base);
	for (int i = 1; sys_lstat(out, &st) == 0 && i < 100; i++)
		fmt(out, cap, "%s/%s%d", user, base, i);
	mem_free(user);
}

static bool fs_method(DBus *bus, DBusMsg *call, void *data)
{
	Block *b = data;
	if (props_call(bus, call, FS_IF, FS_PROPS, ARRAY_LEN(FS_PROPS), b))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "Mount")) {
		if (!allowed(bus, call))
			return true;
		if (b->nmounts) {
			dbus_reply_error(bus, call, "org.freedesktop.UDisks2.Error.AlreadyMounted", "It's mounted already");
			return true;
		}
		const char *type = str_eq(b->type, "ntfs") ? "ntfs3" : str_starts(b->type, "ext") ? "ext4" : b->type;
		char opts[200] = "";
		bool owned = str_eq(b->type, "vfat") || str_eq(b->type, "exfat") || str_eq(b->type, "ntfs") || str_eq(b->type, "iso9660") ||
			     str_eq(b->type, "udf");
		if (str_eq(b->type, "vfat"))
			fmt(opts, sizeof opts, "uid=%u,gid=%u,dmask=0077,fmask=0177,utf8=1,shortname=mixed,flush,errors=remount-ro", Z.uid, Z.gid);
		else if (str_eq(b->type, "exfat"))
			fmt(opts, sizeof opts, "uid=%u,gid=%u,dmask=0077,fmask=0177,iocharset=utf8,errors=remount-ro", Z.uid, Z.gid);
		else if (str_eq(b->type, "ntfs"))
			fmt(opts, sizeof opts, "uid=%u,gid=%u,windows_names,iocharset=utf8", Z.uid, Z.gid);
		else if (owned)
			fmt(opts, sizeof opts, "uid=%u,gid=%u,iocharset=utf8", Z.uid, Z.gid);
		char dir[300], dev[48];
		mount_dir(b, dir, sizeof dir);
		fmt(dev, sizeof dev, "/dev/%s", b->name);
		dir_make(dir, 0700);
		unsigned long flags = MS_NOSUID | MS_NODEV | MS_RELATIME | (b->ro || str_eq(b->type, "iso9660") ? MS_RDONLY : 0);
		long r = sys_mount(dev, dir, type, flags, opts[0] ? opts : NULL);
		if (r < 0 && !(flags & MS_RDONLY))
			r = sys_mount(dev, dir, type, flags | MS_RDONLY, opts[0] ? opts : NULL);   /* a locked card */
		if (r < 0) {
			sys_rmdir(dir);
			char msg[160];
			fmt(msg, sizeof msg, "Can't mount it: %s", err_str(r));
			dbus_reply_error(bus, call, "org.freedesktop.UDisks2.Error.Failed", msg);
			return true;
		}
		/* a Linux file system made elsewhere: its top folder becomes the person's if it's root's and empty */
		if (!owned) {
			struct stat st;
			if (sys_stat(dir, &st) == 0 && st.st_uid == 0) {
				DirList l = { 0 };
				bool empty = !dir_list(dir, &l, false) || l.len == 0 || (l.len == 1 && str_eq(l.data[0].name, "lost+found"));
				dir_list_free(&l);
				if (empty)
					sys_chown(dir, Z.uid, Z.gid);
			}
		}
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "s", dir);
		dbus_send(bus, m);
		refresh();
	} else if (str_eq(mbr, "Unmount")) {
		if (!allowed(bus, call))
			return true;
		bool force = dbus_bool(dbus_dict_get(dbus_arg(call, 0), "force"), false);
		for (int i = b->nmounts - 1; i >= 0; i--) {
			long r = sys_umount(b->mounts[i], force ? MNT_DETACH : 0);
			if (r < 0) {
				dbus_reply_error(bus, call, "org.freedesktop.UDisks2.Error.DeviceBusy", "Something is still using it");
				return true;
			}
			if (str_starts(b->mounts[i], "/run/media/"))
				sys_rmdir(b->mounts[i]);
		}
		dbus_reply_empty(bus, call);
		refresh();
	} else if (str_eq(mbr, "SetLabel") || str_eq(mbr, "Resize") || str_eq(mbr, "Check") || str_eq(mbr, "Repair") ||
		   str_eq(mbr, "TakeOwnership")) {
		dbus_reply_error(bus, call, "org.freedesktop.UDisks2.Error.NotSupported", "Not on Zurbax yet");
	} else {
		return false;
	}
	return true;
}

static bool block_method(DBus *bus, DBusMsg *call, void *data)
{
	Block *b = data;
	if (props_call(bus, call, BLOCK_IF, BLOCK_PROPS, ARRAY_LEN(BLOCK_PROPS), b) ||
	    props_call(bus, call, PART_IF, PART_PROPS, ARRAY_LEN(PART_PROPS), b) ||
	    props_call(bus, call, TABLE_IF, TABLE_PROPS, ARRAY_LEN(TABLE_PROPS), b))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "OpenForBackup") || str_eq(mbr, "OpenForRestore") || str_eq(mbr, "OpenForBenchmark") || str_eq(mbr, "OpenDevice")) {
		if (!allowed(bus, call))
			return true;
		char dev[48];
		fmt(dev, sizeof dev, "/dev/%s", b->name);
		bool write = str_eq(mbr, "OpenForRestore") || (str_eq(mbr, "OpenDevice") && str_eq(dbus_str(dbus_arg(call, 0), "r"), "rw"));
		int fd = (int)sys_open(dev, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC, 0);
		if (fd < 0) {
			dbus_reply_error(bus, call, "org.freedesktop.UDisks2.Error.Failed", "Can't open the device");
			return true;
		}
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "h", fd);
		dbus_send(bus, m);
		sys_close(fd);
	} else if (str_eq(mbr, "Rescan")) {
		dbus_reply_empty(bus, call);
		refresh();
	} else if (str_eq(mbr, "Format") || str_eq(mbr, "AddConfigurationItem") || str_eq(mbr, "RemoveConfigurationItem") ||
		   str_eq(mbr, "UpdateConfigurationItem") || str_eq(mbr, "GetSecretConfiguration")) {
		dbus_reply_error(bus, call, "org.freedesktop.UDisks2.Error.NotSupported", "Not on Zurbax yet");
	} else {
		return false;
	}
	return true;
}

static void unmount_drive(const Drive *d)
{
	for (usize i = 0; i < ARRAY_LEN(blocks); i++) {
		Block *b = &blocks[i];
		if (!b->used || drive_of(b) != d)
			continue;
		for (int k = b->nmounts - 1; k >= 0; k--) {
			sys_umount(b->mounts[k], MNT_DETACH);
			if (str_starts(b->mounts[k], "/run/media/"))
				sys_rmdir(b->mounts[k]);
		}
	}
	sys_sync();
}

static bool drive_method(DBus *bus, DBusMsg *call, void *data)
{
	Drive *d = data;
	if (props_call(bus, call, DRIVE_IF, DRIVE_PROPS, ARRAY_LEN(DRIVE_PROPS), d))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "Eject") || str_eq(mbr, "PowerOff")) {
		if (!allowed(bus, call))
			return true;
		unmount_drive(d);
		if (d->optical) {
			char dev[48];
			fmt(dev, sizeof dev, "/dev/%s", d->name);
			int fd = (int)sys_open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
			if (fd >= 0) {
				sys_ioctl(fd, CDROMEJECT, NULL);
				sys_close(fd);
			}
		} else if (str_eq(mbr, "PowerOff") || str_eq(d->bus, "usb")) {
			/* the kernel lets go of it: safe to pull out */
			char *del = str_fmt("/sys/block/%s/device/delete", d->name);
			file_write_str(del, "1");
			mem_free(del);
		}
		dbus_reply_empty(bus, call);
		refresh();
	} else if (str_eq(mbr, "SetConfiguration")) {
		dbus_reply_empty(bus, call);
	} else {
		return false;
	}
	return true;
}

static bool manager_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, MANAGER_IF, MANAGER_PROPS, ARRAY_LEN(MANAGER_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "GetBlockDevices") || str_eq(mbr, "ResolveDevice")) {
		const DBusVal *spec = str_eq(mbr, "ResolveDevice") ? dbus_arg(call, 0) : NULL;
		const char *want_path = dbus_str(dbus_dict_get(spec, "path"), NULL), *want_label = dbus_str(dbus_dict_get(spec, "label"), NULL),
			   *want_uuid = dbus_str(dbus_dict_get(spec, "uuid"), NULL);
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "ao");
		for (usize i = 0; i < ARRAY_LEN(blocks); i++) {
			Block *b = &blocks[i];
			if (!b->used)
				continue;
			char dev[48];
			fmt(dev, sizeof dev, "/dev/%s", b->name);
			if (spec && !((want_path && str_eq(want_path, dev)) || (want_label && str_eq(want_label, b->label)) ||
				      (want_uuid && str_eq(want_uuid, b->uuid))))
				continue;
			char *p = block_path(b);
			dbus_add(m, "o", p);
			mem_free(p);
		}
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_starts(mbr, "Can")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "(bs)");
		dbus_add(m, "bs", false, "");
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "EnableModule") || str_eq(mbr, "EnableModules")) {
		dbus_reply_empty(bus, call);
	} else {
		return false;
	}
	return true;
}

static bool om_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (!str_eq(dbus_member(call), "GetManagedObjects"))
		return false;
	DBusMsg *m = dbus_new_reply(call);
	dbus_open(m, "a{oa{sa{sv}}}");
	dbus_open(m, "{oa{sa{sv}}}");
	dbus_add(m, "o", MANAGER_PATH);
	dbus_open(m, "a{sa{sv}}");
	iface_add(m, &I_MANAGER, NULL);
	dbus_close(m);
	dbus_close(m);
	for (usize i = 0; i < ARRAY_LEN(drives); i++) {
		if (!drives[i].used)
			continue;
		char *p = drive_path(&drives[i]);
		dbus_open(m, "{oa{sa{sv}}}");
		dbus_add(m, "o", p);
		dbus_open(m, "a{sa{sv}}");
		iface_add(m, &I_DRIVE, &drives[i]);
		dbus_close(m);
		dbus_close(m);
		mem_free(p);
	}
	for (usize i = 0; i < ARRAY_LEN(blocks); i++) {
		if (!blocks[i].used)
			continue;
		char *p = block_path(&blocks[i]);
		int n = 0;
		const Iface *const *list = block_ifaces(&blocks[i], &n);
		dbus_open(m, "{oa{sa{sv}}}");
		dbus_add(m, "o", p);
		dbus_open(m, "a{sa{sv}}");
		for (int k = 0; k < n; k++)
			iface_add(m, list[k], &blocks[i]);
		dbus_close(m);
		dbus_close(m);
		mem_free(p);
	}
	dbus_close(m);
	dbus_send(bus, m);
	return true;
}

/* ---- Keeping up ------------------------------------------------------------------------------- */

static void refresh(void)
{
	scan();
	for (usize i = 0; i < ARRAY_LEN(blocks); i++) {
		Block *b = &blocks[i];
		if (!b->used)
			continue;
		char *p = block_path(b);
		if (!b->seen) {
			static const char *const all[] = { BLOCK_IF, FS_IF, PART_IF, TABLE_IF };
			for (usize k = 0; k < ARRAY_LEN(all); k++)
				dbus_unexport(Z.bus, p, all[k]);
			om_removed(Z.bus, UD_PATH, p, all, ARRAY_LEN(all));
			*b = (Block){ 0 };
			mem_free(p);
			continue;
		}
		int n = 0;
		const Iface *const *list = block_ifaces(b, &n);
		const Iface *added[4];
		int nadded = 0;
		for (int k = 0; k < n; k++) {
			int slot = list[k] == &I_BLOCK ? 0 : list[k] == &I_FS ? 1 : list[k] == &I_PART ? 2 : 3;
			if (!b->exported[slot]) {
				dbus_export(Z.bus, p, list[k]->iface, slot == 1 ? fs_method : block_method, b, NULL);
				b->exported[slot] = true;
				b->hash[slot] = iface_hash(list[k], b);
				added[nadded++] = list[k];
			} else {
				iface_sync(Z.bus, p, list[k], b, &b->hash[slot]);
			}
		}
		if (nadded)
			om_added(Z.bus, UD_PATH, p, added, (usize)nadded, b);
		mem_free(p);
	}
	for (usize i = 0; i < ARRAY_LEN(drives); i++) {
		Drive *d = &drives[i];
		if (!d->used)
			continue;
		char *p = drive_path(d);
		if (!d->seen) {
			dbus_unexport(Z.bus, p, DRIVE_IF);
			const char *names[] = { DRIVE_IF };
			om_removed(Z.bus, UD_PATH, p, names, 1);
			*d = (Drive){ 0 };
		} else if (!d->hash) {
			dbus_export(Z.bus, p, DRIVE_IF, drive_method, d, NULL);
			d->hash = iface_hash(&I_DRIVE, d);
			const Iface *list[] = { &I_DRIVE };
			om_added(Z.bus, UD_PATH, p, list, 1, d);
		} else {
			iface_sync(Z.bus, p, &I_DRIVE, d, &d->hash);
		}
		mem_free(p);
	}
}

static void tick(Loop *loop, void *data)
{
	(void)loop, (void)data;
	refresh();
}

void udisks_start(DBus *bus)
{
	dbus_export(bus, UD_PATH, OM_IF, om_method, NULL, NULL);
	dbus_export(bus, MANAGER_PATH, MANAGER_IF, manager_method, NULL, NULL);
	refresh();
	loop_timer(Z.loop, 2000, 2000, tick, NULL);
	dbus_request_name(bus, UD, DBUS_NAME_NO_QUEUE);
}
