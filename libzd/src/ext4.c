/*
 * ext4: the format of the data partition.
 *
 * 4 KiB blocks, 256-byte inodes, extents, a journal and uninit_bg, so that
 * formatting a large disk only writes a few megabytes: the kernel zeroes the
 * remaining inode tables in the background after the first mount. Groups
 * keep their bitmaps and inode table at their start (no flex_bg), which
 * keeps the layout easy to reason about.
 */
#include "zd.h"

#define BS 4096u
#define BPG 32768u                     /* blocks per group: one bitmap block */
#define INODE_SIZE 256u
#define INODES_PER_BLOCK (BS / INODE_SIZE)
#define ROOT_INO 2
#define JOURNAL_INO 8
#define LPF_INO 11
#define FIRST_INO 11

#define COMPAT_HAS_JOURNAL 0x0004
#define COMPAT_EXT_ATTR 0x0008
#define COMPAT_DIR_INDEX 0x0020
#define INCOMPAT_FILETYPE 0x0002
#define INCOMPAT_EXTENTS 0x0040
#define RO_SPARSE_SUPER 0x0001
#define RO_LARGE_FILE 0x0002
#define RO_HUGE_FILE 0x0008
#define RO_GDT_CSUM 0x0010
#define RO_DIR_NLINK 0x0020
#define RO_EXTRA_ISIZE 0x0040

#define BG_INODE_UNINIT 0x1
#define BG_BLOCK_UNINIT 0x2
#define BG_ITABLE_ZEROED 0x4

#define EXTENTS_FL 0x80000

static void put16(u8 *p, u16 v)
{
	p[0] = (u8)v, p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (u8)(v >> (i * 8));
}

static void put32be(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (u8)(v >> ((3 - i) * 8));
}

static u16 crc16(u16 crc, const void *data, usize n)
{
	const u8 *p = data;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc & 1) ? (u16)((crc >> 1) ^ 0xa001) : (u16)(crc >> 1);
	}
	return crc;
}

typedef struct {
	int fd;
	u64 base;              /* byte offset of block 0 */
	u32 blocks;
	u32 groups;
	u32 ipg;               /* inodes per group */
	u32 itable_blocks;     /* per group */
	u32 gdt_blocks;
	u8 uuid[16];
} Fs;

static bool put_blocks(Fs *fs, u64 block, const void *data, usize n)
{
	const u8 *p = data;
	u64 off = fs->base + block * BS;
	while (n) {
		long w = sys_pwrite(fs->fd, p, n, (i64)off);
		if (w == -EINTR)
			continue;
		if (w <= 0)
			return false;
		p += w;
		off += (u64)w;
		n -= (usize)w;
	}
	return true;
}

static bool zero_blocks(Fs *fs, u64 block, u32 count)
{
	static u8 zero[64 * 1024];
	while (count) {
		u32 n = MIN(count, (u32)(sizeof zero / BS));
		if (!put_blocks(fs, block, zero, (usize)n * BS))
			return false;
		block += n;
		count -= n;
	}
	return true;
}

static bool is_power_of(u32 n, u32 base)
{
	while (n > 1 && n % base == 0)
		n /= base;
	return n == 1;
}

/* sparse_super: copies of the superblock live in groups 0, 1 and powers of 3, 5 and 7. */
static bool group_has_super(u32 g)
{
	return g <= 1 || is_power_of(g, 3) || is_power_of(g, 5) || is_power_of(g, 7);
}

static u32 group_start(u32 g)
{
	return g * BPG;
}

static u32 group_blocks(const Fs *fs, u32 g)
{
	return g + 1 < fs->groups ? BPG : fs->blocks - group_start(g);
}

/* The first block after the superblock copy and descriptor table of a group. */
static u32 group_meta(const Fs *fs, u32 g)
{
	return group_start(g) + (group_has_super(g) ? 1 + fs->gdt_blocks : 0);
}

static u32 group_overhead(const Fs *fs, u32 g)
{
	return (group_has_super(g) ? 1 + fs->gdt_blocks : 0) + 2 + fs->itable_blocks;
}

static void set_bit(u8 *bitmap, u32 i)
{
	bitmap[i / 8] |= (u8)(1u << (i % 8));
}

/* An inode whose data is one run of blocks, described by a single extent. */
static void extent_inode(u8 *ino, u16 mode, u16 links, u64 size, u32 first_block, u32 count, u32 now)
{
	memset(ino, 0, INODE_SIZE);
	put16(ino + 0, mode);
	put32(ino + 4, (u32)size);
	put32(ino + 8, now);
	put32(ino + 12, now);
	put32(ino + 16, now);
	put16(ino + 26, links);
	put32(ino + 28, count * (BS / 512));
	put32(ino + 32, EXTENTS_FL);
	u8 *e = ino + 40;
	put16(e + 0, 0xf30a);   /* extent header magic */
	put16(e + 2, 1);        /* entries */
	put16(e + 4, 4);        /* capacity */
	put16(e + 6, 0);        /* depth: leaves */
	put32(e + 12, 0);       /* first logical block */
	put16(e + 16, (u16)count);
	put16(e + 18, 0);       /* physical start, high bits */
	put32(e + 20, first_block);
	put32(ino + 108, (u32)(size >> 32));
	put16(ino + 128, 32);   /* extra inode size */
	put32(ino + 144, now);  /* creation time */
}

static u8 *dirent(u8 *p, u32 ino, u16 rec_len, const char *name, u8 type)
{
	usize n = strlen(name);
	put32(p, ino);
	put16(p + 4, rec_len);
	p[6] = (u8)n;
	p[7] = type;
	memcpy(p + 8, name, n);
	return p + rec_len;
}

static u32 journal_blocks_for(u32 blocks)
{
	if (blocks < 2 * BPG)
		return 1024;
	if (blocks < 256 * 1024)
		return 4096;
	return 16384;
}

bool ext4_format(int fd, u64 start_byte, u64 bytes, const char *label, const u8 uuid[16])
{
	Fs fs = { .fd = fd, .base = start_byte };
	memcpy(fs.uuid, uuid, 16);
	u64 total = bytes / BS;
	if (total < 4096 || total > 0xffffffffull)
		return false;
	fs.blocks = (u32)total;
	fs.groups = (fs.blocks + BPG - 1) / BPG;
	/* One inode per 16 KiB, rounded to whole inode table blocks. */
	fs.ipg = MIN(BPG * BS / 16384, ALIGN_UP(fs.blocks / 4, INODES_PER_BLOCK));
	fs.ipg = ALIGN_UP(fs.ipg, INODES_PER_BLOCK);
	fs.itable_blocks = fs.ipg / INODES_PER_BLOCK;
	fs.gdt_blocks = (fs.groups * 32 + BS - 1) / BS;
	/* A tail group too small to hold its own metadata is left unused. */
	u32 tail = group_blocks(&fs, fs.groups - 1);
	if (fs.groups > 1 && tail < group_overhead(&fs, fs.groups - 1) + 64) {
		fs.groups--;
		fs.blocks = fs.groups * BPG;
		fs.gdt_blocks = (fs.groups * 32 + BS - 1) / BS;
	}
	u32 now = (u32)time_unix();

	/* Group 0 holds everything that exists after formatting: the root
	 * folder, lost+found and the journal, in that order after the inode table. */
	u32 g0_data = group_meta(&fs, 0) + 2 + fs.itable_blocks;
	u32 root_block = g0_data;
	u32 lpf_block = root_block + 1, lpf_count = 4;
	u32 journal_block = lpf_block + lpf_count;
	u32 journal_count = journal_blocks_for(fs.blocks);
	u32 g0_used = journal_block + journal_count - group_start(0);
	if (g0_used + 16 > group_blocks(&fs, 0))
		return false;

	/* Group descriptors. */
	u8 *gdt = mem_zalloc((usize)fs.gdt_blocks * BS);
	u64 free_blocks = 0;
	for (u32 g = 0; g < fs.groups; g++) {
		u8 *d = gdt + g * 32;
		u32 meta = group_meta(&fs, g);
		u32 used = g == 0 ? g0_used : group_overhead(&fs, g);
		u32 free_count = group_blocks(&fs, g) - used;
		free_blocks += free_count;
		put32(d + 0, meta);
		put32(d + 4, meta + 1);
		put32(d + 8, meta + 2);
		put16(d + 12, (u16)free_count);
		put16(d + 14, (u16)(g == 0 ? fs.ipg - FIRST_INO : fs.ipg));
		put16(d + 16, g == 0 ? 2 : 0);
		u16 flags = g == 0 ? BG_ITABLE_ZEROED : BG_INODE_UNINIT;
		/* The kernel computes untouched block bitmaps itself, except the last group's. */
		if (g != 0 && g + 1 != fs.groups)
			flags |= BG_BLOCK_UNINIT;
		put16(d + 18, flags);
		put16(d + 28, (u16)(g == 0 ? fs.ipg - FIRST_INO : fs.ipg));
		u8 le[4];
		put32(le, g);
		u16 crc = crc16(0xffff, fs.uuid, 16);
		crc = crc16(crc, le, 4);
		crc = crc16(crc, d, 30);
		put16(d + 30, crc);
	}

	/* Journal inode block list, which the superblock keeps a copy of. */
	u8 jino[INODE_SIZE];
	extent_inode(jino, S_IFREG | 0600, 1, (u64)journal_count * BS, journal_block, journal_count, now);

	/* Superblock. */
	u8 sb[1024] = { 0 };
	u32 inodes = fs.groups * fs.ipg;
	u32 reserved = MIN(fs.blocks / 100, 262144u);
	put32(sb + 0, inodes);
	put32(sb + 4, fs.blocks);
	put32(sb + 8, reserved);
	put32(sb + 12, (u32)free_blocks);
	put32(sb + 16, inodes - FIRST_INO);
	put32(sb + 20, 0);           /* first data block */
	put32(sb + 24, 2);           /* log2(block size) - 10 */
	put32(sb + 28, 2);
	put32(sb + 32, BPG);
	put32(sb + 36, BPG);
	put32(sb + 40, fs.ipg);
	put32(sb + 48, now);
	put16(sb + 54, 0xffff);      /* no forced checks */
	put16(sb + 56, 0xef53);
	put16(sb + 58, 1);           /* clean */
	put16(sb + 60, 1);           /* on errors: continue */
	put32(sb + 64, now);
	put32(sb + 76, 1);           /* dynamic revision */
	put32(sb + 84, FIRST_INO);
	put16(sb + 88, INODE_SIZE);
	put32(sb + 92, COMPAT_HAS_JOURNAL | COMPAT_EXT_ATTR | COMPAT_DIR_INDEX);
	put32(sb + 96, INCOMPAT_FILETYPE | INCOMPAT_EXTENTS);
	put32(sb + 100, RO_SPARSE_SUPER | RO_LARGE_FILE | RO_HUGE_FILE | RO_GDT_CSUM | RO_DIR_NLINK | RO_EXTRA_ISIZE);
	memcpy(sb + 104, fs.uuid, 16);
	for (int i = 0; i < 16 && label && label[i]; i++)
		sb[120 + i] = (u8)label[i];
	put32(sb + 224, JOURNAL_INO);
	rand_bytes(sb + 236, 16);    /* directory hash seed */
	sb[252] = 1;                 /* half MD4 */
	sb[253] = 1;                 /* journal blocks backed up below */
	put32(sb + 256, 0x000c);     /* user xattrs and ACLs by default */
	put32(sb + 264, now);
	memcpy(sb + 268, jino + 40, 60);
	put32(sb + 268 + 15 * 4, (u32)(((u64)journal_count * BS) >> 32));
	put32(sb + 268 + 16 * 4, journal_count * BS);
	put16(sb + 348, 32);
	put16(sb + 350, 32);
	put32(sb + 352, 0x1);        /* signed directory hash (x86 char) */

	bool ok = true;
	/* Superblock copies and descriptor tables. */
	for (u32 g = 0; g < fs.groups && ok; g++) {
		if (!group_has_super(g))
			continue;
		u8 block[BS] = { 0 };
		put16(sb + 90, (u16)g);
		memcpy(block + (g == 0 ? 1024 : 0), sb, 1024);
		ok = put_blocks(&fs, group_start(g), block, BS) && put_blocks(&fs, group_start(g) + 1, gdt, (usize)fs.gdt_blocks * BS);
	}
	put16(sb + 90, 0);

	/* Group 0: bitmaps, a zeroed inode table and the first inodes. */
	u8 *bitmap = mem_zalloc(BS);
	for (u32 b = 0; b < g0_used; b++)
		set_bit(bitmap, b);
	for (u32 b = group_blocks(&fs, 0); b < BPG; b++)
		set_bit(bitmap, b);
	ok = ok && put_blocks(&fs, group_meta(&fs, 0), bitmap, BS);
	memset(bitmap, 0, BS);
	for (u32 i = 0; i < FIRST_INO; i++)
		set_bit(bitmap, i);
	for (u32 i = fs.ipg; i < BS * 8; i++)
		set_bit(bitmap, i);
	ok = ok && put_blocks(&fs, group_meta(&fs, 0) + 1, bitmap, BS);
	ok = ok && zero_blocks(&fs, group_meta(&fs, 0) + 2, fs.itable_blocks);

	u8 itable[BS] = { 0 };
	extent_inode(itable + (ROOT_INO - 1) * INODE_SIZE, S_IFDIR | 0755, 3, BS, root_block, 1, now);
	memcpy(itable + (JOURNAL_INO - 1) * INODE_SIZE, jino, INODE_SIZE);
	ok = ok && put_blocks(&fs, group_meta(&fs, 0) + 2, itable, BS);
	memset(itable, 0, BS);
	/* Inode 11 lands in the inode table's first block too (16 per block). */
	extent_inode(itable, S_IFDIR | 0700, 2, (u64)lpf_count * BS, lpf_block, lpf_count, now);
	u8 first[BS];
	ok = ok && sys_pread(fd, first, BS, (i64)(fs.base + (u64)(group_meta(&fs, 0) + 2) * BS)) == BS;
	memcpy(first + (LPF_INO - 1) * INODE_SIZE, itable, INODE_SIZE);
	ok = ok && put_blocks(&fs, group_meta(&fs, 0) + 2, first, BS);

	/* The root folder and lost+found. */
	u8 dir[BS] = { 0 };
	u8 *p = dirent(dir, ROOT_INO, 12, ".", 2);
	p = dirent(p, ROOT_INO, 12, "..", 2);
	dirent(p, LPF_INO, (u16)(BS - 24), "lost+found", 2);
	ok = ok && put_blocks(&fs, root_block, dir, BS);
	memset(dir, 0, BS);
	p = dirent(dir, LPF_INO, 12, ".", 2);
	dirent(p, ROOT_INO, (u16)(BS - 12), "..", 2);
	ok = ok && put_blocks(&fs, lpf_block, dir, BS);
	memset(dir, 0, BS);
	put16(dir + 4, BS); /* an empty block is one unused entry spanning it */
	for (u32 i = 1; i < lpf_count && ok; i++)
		ok = put_blocks(&fs, lpf_block + i, dir, BS);

	/* The last group's block bitmap, with the blocks past the end marked used. */
	if (fs.groups > 1) {
		u32 g = fs.groups - 1;
		memset(bitmap, 0, BS);
		for (u32 b = 0; b < group_overhead(&fs, g); b++)
			set_bit(bitmap, b);
		for (u32 b = group_blocks(&fs, g); b < BPG; b++)
			set_bit(bitmap, b);
		ok = ok && put_blocks(&fs, group_meta(&fs, g), bitmap, BS);
	}
	mem_free(bitmap);

	/* The journal: zeroed, so nothing stale is ever replayed, then its superblock. */
	ok = ok && zero_blocks(&fs, journal_block, journal_count);
	u8 jsb[BS] = { 0 };
	put32be(jsb + 0, 0xc03b3998);
	put32be(jsb + 4, 4);         /* superblock v2 */
	put32be(jsb + 12, BS);
	put32be(jsb + 16, journal_count);
	put32be(jsb + 20, 1);        /* first log block */
	put32be(jsb + 24, 1);        /* first expected transaction */
	memcpy(jsb + 48, fs.uuid, 16);
	put32be(jsb + 64, 1);        /* one file system uses it */
	ok = ok && put_blocks(&fs, journal_block, jsb, BS);

	mem_free(gdt);
	return ok && sys_fsync(fd) == 0;
}
