/*
 * ISO 9660 with El Torito: what CD and DVD images are made of.
 *
 * iso_write lays down the volume descriptors, both path tables, the folder
 * records and a boot catalog that names a FAT image for UEFI firmware
 * ("no emulation", platform EFI). File data is already in the image; the
 * records only point at it, so a file can share its bytes with the FAT
 * image the firmware boots from. The first 32 KiB (the "system area") is
 * left alone for a partition table, which is what lets the same image
 * boot from a USB stick.
 *
 * Names are upper-case 8.3 (ISO 9660 level 1), which every system reads.
 */
#include "zd.h"

#define BLOCK 2048u
#define FIRST_SECTOR 16  /* after the system area */
#define MAX_DIRS 32

typedef struct {
	char name[13];        /* "EFI", or "" for the root */
	int parent;           /* index, the root is its own parent */
	u32 sector, size;     /* where its records go */
} IsoDir;

static void both16(u8 *p, u16 v)
{
	p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 8), p[3] = (u8)v;
}

static void both32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (u8)(v >> (i * 8)), p[7 - i] = (u8)(v >> (i * 8));
}

static void le32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (u8)(v >> (i * 8));
}

static void be32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[3 - i] = (u8)(v >> (i * 8));
}

static void text(u8 *p, usize n, const char *s)
{
	memset(p, ' ', n);
	for (usize i = 0; i < n && s && s[i]; i++)
		p[i] = (u8)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
}

/* 1 January 2026, noon, like the FAT writer: images build the same each time. */
static void date7(u8 *p)
{
	p[0] = 126, p[1] = 1, p[2] = 1, p[3] = 12, p[4] = 0, p[5] = 0, p[6] = 0;
}

static void date17(u8 *p)
{
	memcpy(p, "2026010112000000", 16);
	p[16] = 0;
}

/* One folder record; returns its length. */
static usize record(u8 *p, const char *name, usize name_len, u32 sector, u32 size, bool dir)
{
	usize len = 33 + name_len + (name_len % 2 == 0);
	memset(p, 0, len);
	p[0] = (u8)len;
	both32(p + 2, sector);
	both32(p + 10, size);
	date7(p + 18);
	p[25] = dir ? 2 : 0;
	both16(p + 28, 1);
	p[32] = (u8)name_len;
	memcpy(p + 33, name, name_len);
	return len;
}

static int find_dir(const IsoDir *dirs, int n, int parent, const char *name)
{
	for (int i = 1; i < n; i++)
		if (dirs[i].parent == parent && str_eq(dirs[i].name, name))
			return i;
	return -1;
}

static bool valid_name(const char *s, bool file)
{
	usize n = strlen(s), base = n;
	const char *dot = str_chr(s, '.');
	if (dot) {
		if (!file || str_chr(dot + 1, '.') || strlen(dot + 1) > 3)
			return false;
		base = (usize)(dot - s);
	}
	if (base == 0 || base > 8)
		return false;
	for (usize i = 0; i < n; i++)
		if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '.'))
			return false;
	return true;
}

/* The records of one folder, packed so none crosses a sector. */
typedef struct {
	u8 *buf;
	usize cap, at;
} RecBuf;

static void rec_add(RecBuf *b, const char *name, usize name_len, u32 sector, u32 size, bool dir)
{
	usize len = 33 + name_len + (name_len % 2 == 0);
	if (b->at / BLOCK != (b->at + len - 1) / BLOCK)
		b->at = (b->at / BLOCK + 1) * BLOCK;
	if (b->at + len > b->cap) {
		usize ncap = MAX(b->cap * 2, b->at + len + BLOCK);
		b->buf = mem_realloc(b->buf, ncap);
		memset(b->buf + b->cap, 0, ncap - b->cap);
		b->cap = ncap;
	}
	b->at += record(b->buf + b->at, name, name_len, sector, size, dir);
}

/* Builds a folder's records; with sizes still unknown the lengths come out the same. */
static RecBuf dir_records(const IsoDir *dirs, int ndirs, int d, const IsoFile *files, int nfiles, char **leaf_of, int *dir_of)
{
	RecBuf b = { 0 };
	const IsoDir *me = &dirs[d], *up = &dirs[me->parent];
	rec_add(&b, "\0", 1, me->sector, me->size, true);
	rec_add(&b, "\1", 1, up->sector, up->size, true);
	/* children in name order: folders and files mixed, as ISO 9660 wants */
	const char *last = "";
	for (;;) {
		const char *best = NULL;
		int best_dir = -1, best_file = -1;
		for (int i = 1; i < ndirs; i++)
			if (dirs[i].parent == d && str_cmp(dirs[i].name, last) > 0 && (!best || str_cmp(dirs[i].name, best) < 0))
				best = dirs[i].name, best_dir = i, best_file = -1;
		for (int i = 0; i < nfiles; i++)
			if (dir_of[i] == d && str_cmp(leaf_of[i], last) > 0 && (!best || str_cmp(leaf_of[i], best) < 0))
				best = leaf_of[i], best_file = i, best_dir = -1;
		if (!best)
			break;
		if (best_dir >= 0) {
			rec_add(&b, dirs[best_dir].name, strlen(dirs[best_dir].name), dirs[best_dir].sector, dirs[best_dir].size, true);
		} else {
			char name[16];
			fmt(name, sizeof name, "%s;1", leaf_of[best_file]);
			rec_add(&b, name, strlen(name), (u32)(files[best_file].byte / BLOCK), (u32)files[best_file].size, false);
		}
		last = best;
	}
	return b;
}

bool iso_write(int fd, const IsoVolume *v, const IsoFile *files, int nfiles)
{
	if (v->boot_byte % BLOCK || v->volume_bytes % BLOCK)
		return false;
	/* folders from the file paths */
	IsoDir dirs[MAX_DIRS] = { 0 };
	int ndirs = 1;
	char **leaf_of = NEW_ARRAY(char *, (usize)MAX(nfiles, 1));
	int *dir_of = NEW_ARRAY(int, (usize)MAX(nfiles, 1));
	bool ok = true;
	for (int i = 0; i < nfiles && ok; i++) {
		if (files[i].byte % BLOCK || files[i].size > 0xffffffffu) {
			ok = false;
			break;
		}
		usize n;
		char **parts = str_split(files[i].path[0] == '/' ? files[i].path + 1 : files[i].path, '/', &n);
		int d = 0;
		for (usize k = 0; k + 1 < n && ok; k++) {
			int found = find_dir(dirs, ndirs, d, parts[k]);
			if (found < 0) {
				if (ndirs == MAX_DIRS || !valid_name(parts[k], false)) {
					ok = false;
					break;
				}
				str_copy(dirs[ndirs].name, parts[k], sizeof dirs[ndirs].name);
				dirs[ndirs].parent = d;
				found = ndirs++;
			}
			d = found;
		}
		if (ok && (n == 0 || !valid_name(parts[n - 1], true)))
			ok = false;
		if (ok) {
			leaf_of[i] = str_dup(parts[n - 1]);
			dir_of[i] = d;
		}
		str_split_free(parts, n);
	}
	/* path tables list folders level by level; that's the order they're numbered in */
	int order[MAX_DIRS], norder = 0;
	order[norder++] = 0;
	for (int k = 0; k < norder; k++)
		for (int i = 1; i < ndirs; i++)
			if (dirs[i].parent == order[k])
				order[norder++] = i;
	int number[MAX_DIRS];
	for (int k = 0; k < norder; k++)
		number[order[k]] = k + 1;
	usize pt_size = 0;
	for (int k = 0; k < norder; k++) {
		usize nl = k == 0 ? 1 : strlen(dirs[order[k]].name);
		pt_size += 8 + nl + (nl % 2);
	}
	/* layout: descriptors, path tables, folders, boot catalog */
	u32 pt_sectors = (u32)((pt_size + BLOCK - 1) / BLOCK);
	u32 sector = FIRST_SECTOR + 3; /* primary, boot record, terminator */
	u32 l_table = sector, m_table = sector + pt_sectors;
	sector += 2 * pt_sectors;
	for (int k = 0; k < norder && ok; k++) {
		int d = order[k];
		dirs[d].sector = sector;
		RecBuf b = dir_records(dirs, ndirs, d, files, nfiles, leaf_of, dir_of);
		dirs[d].size = (u32)((b.at + BLOCK - 1) / BLOCK * BLOCK);
		sector += dirs[d].size / BLOCK;
		mem_free(b.buf);
	}
	u32 catalog = sector++;
	u64 lowest = v->boot_byte;
	for (int i = 0; i < nfiles; i++)
		if (files[i].size)
			lowest = MIN(lowest, files[i].byte);
	if ((u64)sector * BLOCK > lowest)
		ok = false; /* the caller left too little room before the data */
	u8 *blk = mem_zalloc(BLOCK);
	/* primary volume descriptor */
	if (ok) {
		blk[0] = 1;
		memcpy(blk + 1, "CD001", 5);
		blk[6] = 1;
		text(blk + 8, 32, v->system_id);
		text(blk + 40, 32, v->volume_id);
		both32(blk + 80, (u32)(v->volume_bytes / BLOCK));
		both16(blk + 120, 1);
		both16(blk + 124, 1);
		both16(blk + 128, BLOCK);
		both32(blk + 132, (u32)pt_size);
		le32(blk + 140, l_table);
		be32(blk + 148, m_table);
		record(blk + 156, "\0", 1, dirs[0].sector, dirs[0].size, true);
		text(blk + 190, 128, v->volume_id);
		text(blk + 318, 128, v->publisher);
		text(blk + 446, 128, v->publisher);
		text(blk + 574, 128, v->app_id);
		text(blk + 702, 37 * 3, "");
		date17(blk + 813);
		date17(blk + 830);
		memcpy(blk + 847, "0000000000000000", 16);
		date17(blk + 864);
		blk[881] = 1;
		ok = sys_pwrite(fd, blk, BLOCK, (i64)FIRST_SECTOR * BLOCK) == BLOCK;
	}
	/* El Torito boot record: where the catalog is */
	if (ok) {
		memset(blk, 0, BLOCK);
		memcpy(blk + 1, "CD001", 5);
		blk[6] = 1;
		memcpy(blk + 7, "EL TORITO SPECIFICATION", 23);
		le32(blk + 71, catalog);
		ok = sys_pwrite(fd, blk, BLOCK, (i64)(FIRST_SECTOR + 1) * BLOCK) == BLOCK;
	}
	/* terminator */
	if (ok) {
		memset(blk, 0, BLOCK);
		blk[0] = 255;
		memcpy(blk + 1, "CD001", 5);
		blk[6] = 1;
		ok = sys_pwrite(fd, blk, BLOCK, (i64)(FIRST_SECTOR + 2) * BLOCK) == BLOCK;
	}
	/* the two path tables: little-endian, then big-endian */
	for (int big = 0; big < 2 && ok; big++) {
		u8 *pt = mem_zalloc((usize)pt_sectors * BLOCK);
		usize at = 0;
		for (int k = 0; k < norder; k++) {
			const IsoDir *d = &dirs[order[k]];
			usize nl = k == 0 ? 1 : strlen(d->name);
			pt[at] = (u8)nl;
			if (big)
				be32(pt + at + 2, d->sector), pt[at + 6] = (u8)(number[d->parent] >> 8), pt[at + 7] = (u8)number[d->parent];
			else
				le32(pt + at + 2, d->sector), pt[at + 6] = (u8)number[d->parent], pt[at + 7] = (u8)(number[d->parent] >> 8);
			if (k > 0)
				memcpy(pt + at + 8, d->name, nl);
			at += 8 + nl + (nl % 2);
		}
		ok = sys_pwrite(fd, pt, (usize)pt_sectors * BLOCK, (i64)(big ? m_table : l_table) * BLOCK) == (long)pt_sectors * BLOCK;
		mem_free(pt);
	}
	/* the folders, now that every one has its place */
	for (int k = 0; k < norder && ok; k++) {
		int d = order[k];
		RecBuf b = dir_records(dirs, ndirs, d, files, nfiles, leaf_of, dir_of);
		u8 *out = mem_zalloc(dirs[d].size);
		memcpy(out, b.buf, MIN(b.at, (usize)dirs[d].size));
		ok = sys_pwrite(fd, out, dirs[d].size, (i64)dirs[d].sector * BLOCK) == (long)dirs[d].size;
		mem_free(out);
		mem_free(b.buf);
	}
	/* boot catalog: a validation entry for EFI, then the boot image itself */
	if (ok) {
		memset(blk, 0, BLOCK);
		blk[0] = 1;
		blk[1] = 0xef; /* EFI */
		text(blk + 4, 24, v->publisher);
		blk[30] = 0x55, blk[31] = 0xaa;
		u16 sum = 0;
		for (int i = 0; i < 32; i += 2)
			sum = (u16)(sum + (blk[i] | blk[i + 1] << 8));
		u16 fix = (u16)(0x10000 - sum);
		blk[28] = (u8)fix, blk[29] = (u8)(fix >> 8);
		u8 *e = blk + 32;
		e[0] = 0x88;   /* bootable */
		e[1] = 0;      /* no emulation */
		/* in 512-byte units; 0 means "to the end of the volume", for images past 32 MiB */
		u64 count = v->boot_size / 512;
		e[6] = (u8)(count <= 0xffff ? count : 0), e[7] = (u8)(count <= 0xffff ? count >> 8 : 0);
		le32(e + 8, (u32)(v->boot_byte / BLOCK));
		ok = sys_pwrite(fd, blk, BLOCK, (i64)catalog * BLOCK) == BLOCK;
	}
	mem_free(blk);
	for (int i = 0; i < nfiles; i++)
		mem_free(leaf_of[i]);
	mem_free(leaf_of);
	mem_free(dir_of);
	return ok;
}
