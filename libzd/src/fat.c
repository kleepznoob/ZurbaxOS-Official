/*
 * FAT32: the format UEFI firmware reads. The writer formats a partition,
 * stores files in contiguous clusters as they are added, and writes the
 * directories and both FAT copies at the end. Names are 8.3, which is all
 * an EFI system partition needs.
 */
#include "zd.h"

#define RESERVED 32
#define ATTR_DIR 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_VOLUME 0x08

typedef struct FatNode {
	char name[11];          /* 8.3, space padded */
	bool dir;
	u32 cluster;
	u32 size;
	struct FatNode *child, *next, *parent;
} FatNode;

struct FatWriter {
	int fd;
	u64 start;              /* partition start, sectors */
	u64 sectors;
	u32 spc;                /* sectors per cluster */
	u32 fat_sectors;
	u64 data_start;         /* absolute sector of cluster 2 */
	u32 clusters;
	u32 *fat;
	u32 next_free;
	char label[11];
	FatNode root;
	bool failed;
};

static void put16(u8 *p, u16 v)
{
	p[0] = (u8)v, p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (u8)(v >> (i * 8));
}

static bool write_at(int fd, u64 byte, const void *data, usize n)
{
	const u8 *p = data;
	while (n) {
		long w = sys_pwrite(fd, p, n, (i64)byte);
		if (w == -EINTR)
			continue;
		if (w <= 0)
			return false;
		p += w;
		byte += (u64)w;
		n -= (usize)w;
	}
	return true;
}

static u64 cluster_byte(const FatWriter *w, u32 c)
{
	return (w->data_start + (u64)(c - 2) * w->spc) * SECTOR;
}

FatWriter *fat_format(int fd, u64 start_lba, u64 sectors, const char *label)
{
	FatWriter *w = NEW(FatWriter);
	w->fd = fd;
	w->start = start_lba;
	w->sectors = sectors;
	/* 4 KiB clusters, smaller when needed to reach FAT32's minimum cluster count. */
	w->spc = 8;
	while (w->spc > 1 && sectors / w->spc < 70000)
		w->spc /= 2;
	u32 fat = 1;
	for (int i = 0; i < 8; i++) {
		u64 clusters = (sectors - RESERVED - 2ull * fat) / w->spc;
		fat = (u32)(((clusters + 2) * 4 + SECTOR - 1) / SECTOR);
	}
	w->fat_sectors = fat;
	w->data_start = start_lba + RESERVED + 2ull * fat;
	w->clusters = (u32)((sectors - RESERVED - 2ull * fat) / w->spc);
	if (w->clusters < 65525) {
		mem_free(w);
		return NULL;
	}
	w->fat = NEW_ARRAY(u32, (usize)w->clusters + 2);
	w->fat[0] = 0x0ffffff8;
	w->fat[1] = 0x0fffffff;
	w->next_free = 2;
	memset(w->label, ' ', 11);
	for (int i = 0; i < 11 && label && label[i]; i++)
		w->label[i] = (char)(label[i] >= 'a' && label[i] <= 'z' ? label[i] - 32 : label[i]);
	w->root.dir = true;
	return w;
}

/* "BOOTX64.EFI" -> "BOOTX64 EFI" */
static bool to_83(const char *name, char out[11])
{
	memset(out, ' ', 11);
	const char *dot = str_rchr(name, '.');
	usize base = dot ? (usize)(dot - name) : strlen(name);
	usize ext = dot ? strlen(dot + 1) : 0;
	if (base == 0 || base > 8 || ext > 3)
		return false;
	for (usize i = 0; i < base; i++)
		out[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
	for (usize i = 0; i < ext; i++)
		out[8 + i] = (char)(dot[1 + i] >= 'a' && dot[1 + i] <= 'z' ? dot[1 + i] - 32 : dot[1 + i]);
	return true;
}

static FatNode *find_child(FatNode *dir, const char n83[11])
{
	for (FatNode *c = dir->child; c; c = c->next)
		if (memcmp(c->name, n83, 11) == 0)
			return c;
	return NULL;
}

static FatNode *add_child(FatNode *dir, const char n83[11], bool is_dir)
{
	FatNode *n = NEW(FatNode);
	memcpy(n->name, n83, 11);
	n->dir = is_dir;
	n->parent = dir;
	/* Keep insertion order. */
	FatNode **p = &dir->child;
	while (*p)
		p = &(*p)->next;
	*p = n;
	return n;
}

/* Walks to the parent folder of `path`, creating folders on the way. */
static FatNode *walk(FatWriter *w, const char *path, char leaf[11])
{
	FatNode *dir = &w->root;
	usize n;
	char **parts = str_split(path[0] == '/' ? path + 1 : path, '/', &n);
	for (usize i = 0; i + 1 < n; i++) {
		char c[11];
		if (!to_83(parts[i], c)) {
			dir = NULL;
			break;
		}
		FatNode *next = find_child(dir, c);
		if (!next)
			next = add_child(dir, c, true);
		dir = next;
	}
	if (dir && (n == 0 || !to_83(parts[n - 1], leaf)))
		dir = NULL;
	str_split_free(parts, n);
	return dir;
}

static u32 alloc_chain(FatWriter *w, u32 count)
{
	if (count == 0)
		return 0;
	if (w->next_free + count > w->clusters + 2) {
		w->failed = true;
		return 0;
	}
	u32 first = w->next_free;
	for (u32 i = 0; i < count; i++)
		w->fat[first + i] = i + 1 < count ? first + i + 1 : 0x0fffffff;
	w->next_free += count;
	return first;
}

bool fat_mkdir(FatWriter *w, const char *path)
{
	char leaf[11];
	FatNode *dir = walk(w, path, leaf);
	if (!dir)
		return false;
	if (!find_child(dir, leaf))
		add_child(dir, leaf, true);
	return true;
}

bool fat_add_file(FatWriter *w, const char *path, const void *data, usize len)
{
	char leaf[11];
	FatNode *dir = walk(w, path, leaf);
	if (!dir || find_child(dir, leaf))
		return false;
	FatNode *f = add_child(dir, leaf, false);
	u32 csize = w->spc * SECTOR;
	f->size = (u32)len;
	f->cluster = alloc_chain(w, (u32)((len + csize - 1) / csize));
	if (len && !f->cluster)
		return false;
	return !len || write_at(w->fd, cluster_byte(w, f->cluster), data, len);
}

bool fat_add_file_from(FatWriter *w, const char *path, const char *source)
{
	usize len;
	char *data = file_read(source, &len);
	if (!data)
		return false;
	bool ok = fat_add_file(w, path, data, len);
	mem_free(data);
	return ok;
}

static u32 count_children(const FatNode *d)
{
	u32 n = 0;
	for (FatNode *c = d->child; c; c = c->next)
		n++;
	return n;
}

static void alloc_dirs(FatWriter *w, FatNode *d)
{
	u32 entries = count_children(d) + 2; /* volume label, or . and .. */
	u32 csize = w->spc * SECTOR;
	d->cluster = alloc_chain(w, MAX(1, (entries * 32 + csize - 1) / csize));
	for (FatNode *c = d->child; c; c = c->next)
		if (c->dir)
			alloc_dirs(w, c);
}

static void dir_entry(u8 *e, const char name[11], u8 attr, u32 cluster, u32 size)
{
	memset(e, 0, 32);
	memcpy(e, name, 11);
	e[11] = attr;
	/* 1 January 2026, noon. */
	u16 date = (u16)(((2026 - 1980) << 9) | (1 << 5) | 1), time = (u16)(12 << 11);
	put16(e + 14, time);
	put16(e + 16, date);
	put16(e + 18, date);
	put16(e + 20, (u16)(cluster >> 16));
	put16(e + 22, time);
	put16(e + 24, date);
	put16(e + 26, (u16)cluster);
	put32(e + 28, size);
}

static bool write_dirs(FatWriter *w, FatNode *d)
{
	u32 csize = w->spc * SECTOR;
	u32 entries = count_children(d) + 2;
	u32 bytes = MAX(csize, (entries * 32 + csize - 1) / csize * csize);
	u8 *buf = mem_zalloc(bytes);
	u8 *e = buf;
	if (d == &w->root) {
		dir_entry(e, w->label, ATTR_VOLUME, 0, 0);
		e += 32;
	} else {
		char dot[11], dotdot[11];
		memset(dot, ' ', 11), memset(dotdot, ' ', 11);
		dot[0] = '.';
		dotdot[0] = dotdot[1] = '.';
		dir_entry(e, dot, ATTR_DIR, d->cluster, 0);
		/* ".." of a folder in the root points at cluster 0 by convention. */
		dir_entry(e + 32, dotdot, ATTR_DIR, d->parent == &w->root ? 0 : d->parent->cluster, 0);
		e += 64;
	}
	for (FatNode *c = d->child; c; c = c->next, e += 32)
		dir_entry(e, c->name, c->dir ? ATTR_DIR : ATTR_ARCHIVE, c->cluster, c->dir ? 0 : c->size);
	bool ok = write_at(w->fd, cluster_byte(w, d->cluster), buf, bytes);
	mem_free(buf);
	for (FatNode *c = d->child; c && ok; c = c->next)
		if (c->dir)
			ok = write_dirs(w, c);
	return ok;
}

static void free_nodes(FatNode *d)
{
	FatNode *c = d->child;
	while (c) {
		FatNode *next = c->next;
		free_nodes(c);
		mem_free(c);
		c = next;
	}
}

bool fat_finish(FatWriter *w)
{
	alloc_dirs(w, &w->root);
	bool ok = !w->failed && write_dirs(w, &w->root);
	/* Boot sector. */
	u8 bs[SECTOR] = { 0 };
	bs[0] = 0xeb, bs[1] = 0x58, bs[2] = 0x90;
	memcpy(bs + 3, "ZURBAX  ", 8);
	put16(bs + 11, SECTOR);
	bs[13] = (u8)w->spc;
	put16(bs + 14, RESERVED);
	bs[16] = 2;
	bs[21] = 0xf8;
	put16(bs + 24, 63);
	put16(bs + 26, 255);
	put32(bs + 28, (u32)w->start);
	put32(bs + 32, (u32)w->sectors);
	put32(bs + 36, w->fat_sectors);
	put32(bs + 44, w->root.cluster);
	put16(bs + 48, 1); /* FSInfo */
	put16(bs + 50, 6); /* backup boot sector */
	bs[64] = 0x80;
	bs[66] = 0x29;
	u32 serial;
	rand_bytes(&serial, 4);
	put32(bs + 67, serial);
	memcpy(bs + 71, w->label, 11);
	memcpy(bs + 82, "FAT32   ", 8);
	bs[510] = 0x55, bs[511] = 0xaa;
	u8 fsinfo[SECTOR] = { 0 };
	put32(fsinfo, 0x41615252);
	put32(fsinfo + 484, 0x61417272);
	put32(fsinfo + 488, w->clusters + 2 - w->next_free);
	put32(fsinfo + 492, w->next_free);
	put32(fsinfo + 508, 0xaa550000);
	u64 base = w->start * SECTOR;
	/* Clear the reserved area first (old boot code, stale signatures). */
	u8 *zero = mem_zalloc(RESERVED * SECTOR);
	ok = ok && write_at(w->fd, base, zero, RESERVED * SECTOR);
	mem_free(zero);
	ok = ok && write_at(w->fd, base, bs, SECTOR) && write_at(w->fd, base + SECTOR, fsinfo, SECTOR) &&
	     write_at(w->fd, base + 6 * SECTOR, bs, SECTOR) && write_at(w->fd, base + 7 * SECTOR, fsinfo, SECTOR);
	/* Both FAT copies; entries past the last used cluster are zero (free). */
	usize fat_bytes = (usize)w->fat_sectors * SECTOR;
	u8 *fat = mem_zalloc(fat_bytes);
	for (u32 i = 0; i < w->clusters + 2 && (usize)i * 4 + 4 <= fat_bytes; i++)
		put32(fat + i * 4, w->fat[i]);
	for (int copy = 0; copy < 2 && ok; copy++)
		ok = write_at(w->fd, (w->start + RESERVED + (u64)copy * w->fat_sectors) * SECTOR, fat, fat_bytes);
	mem_free(fat);
	free_nodes(&w->root);
	mem_free(w->fat);
	mem_free(w);
	return ok;
}
