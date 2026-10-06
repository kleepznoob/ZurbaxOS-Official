/* GUID Partition Tables: a protective MBR, the primary table at the start
 * of the disk and the backup at the end, each protected by CRC32. */
#include "zd.h"

#define ENTRIES 128
#define ENTRY_SIZE 128
#define ENTRY_SECTORS (ENTRIES * ENTRY_SIZE / SECTOR) /* 32 */

/* GUIDs are stored with their first three fields little-endian. */
const u8 GPT_TYPE_ESP[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
const u8 GPT_TYPE_LINUX[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };
const u8 GPT_TYPE_MS_DATA[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };

static u32 crc32(const void *data, usize n)
{
	static u32 table[256];
	if (!table[1])
		for (u32 i = 0; i < 256; i++) {
			u32 c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	u32 c = 0xffffffffu;
	const u8 *p = data;
	for (usize i = 0; i < n; i++)
		c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return ~c;
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

static u32 get32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 get64(const u8 *p)
{
	return (u64)get32(p) | (u64)get32(p + 4) << 32;
}

void guid_random(u8 out[16])
{
	rand_bytes(out, 16);
	out[7] = (u8)((out[7] & 0x0f) | 0x40); /* version 4 (byte order of the 3rd field) */
	out[8] = (u8)((out[8] & 0x3f) | 0x80); /* variant */
}

void guid_format(const u8 g[16], char out[37])
{
	fmt(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", g[3], g[2], g[1], g[0], g[5], g[4], g[7],
	    g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

u64 gpt_first_usable(void)
{
	return 2 + ENTRY_SECTORS; /* 34 */
}

u64 gpt_last_usable(u64 disk_sectors)
{
	return disk_sectors - 2 - ENTRY_SECTORS;
}

static bool write_at(int fd, u64 lba, const void *data, usize n)
{
	const u8 *p = data;
	i64 off = (i64)(lba * SECTOR);
	while (n) {
		long w = sys_pwrite(fd, p, n, off);
		if (w == -EINTR)
			continue;
		if (w <= 0)
			return false;
		p += w;
		off += w;
		n -= (usize)w;
	}
	return true;
}

static bool read_at(int fd, u64 lba, void *data, usize n)
{
	long r = sys_pread(fd, data, n, (i64)(lba * SECTOR));
	return r == (long)n;
}

static void header(u8 *h, u64 self, u64 backup, u64 entries_lba, u64 disk_sectors, const u8 disk_guid[16], u32 entries_crc)
{
	memset(h, 0, SECTOR);
	memcpy(h, "EFI PART", 8);
	put32(h + 8, 0x00010000);     /* revision 1.0 */
	put32(h + 12, 92);            /* header size */
	put64(h + 24, self);
	put64(h + 32, backup);
	put64(h + 40, gpt_first_usable());
	put64(h + 48, gpt_last_usable(disk_sectors));
	memcpy(h + 56, disk_guid, 16);
	put64(h + 72, entries_lba);
	put32(h + 80, ENTRIES);
	put32(h + 84, ENTRY_SIZE);
	put32(h + 88, entries_crc);
	put32(h + 16, crc32(h, 92));
}

bool gpt_write(int fd, u64 disk_sectors, const GptPart *parts, int n, const u8 disk_guid[16])
{
	if (n > ENTRIES || disk_sectors < 2 * (2 + ENTRY_SECTORS) + 64)
		return false;
	/* Protective MBR: one partition of type EE covering the disk. */
	u8 mbr[SECTOR] = { 0 };
	u8 *pe = mbr + 446;
	pe[1] = 0x00, pe[2] = 0x02, pe[3] = 0x00; /* CHS of LBA 1 */
	pe[4] = 0xee;
	pe[5] = 0xff, pe[6] = 0xff, pe[7] = 0xff;
	put32(pe + 8, 1);
	put32(pe + 12, disk_sectors - 1 > 0xffffffffu ? 0xffffffffu : (u32)(disk_sectors - 1));
	mbr[510] = 0x55, mbr[511] = 0xaa;
	/* Partition entries. */
	u8 *entries = mem_zalloc(ENTRIES * ENTRY_SIZE);
	for (int i = 0; i < n; i++) {
		u8 *e = entries + i * ENTRY_SIZE;
		memcpy(e, parts[i].type, 16);
		memcpy(e + 16, parts[i].guid, 16);
		put64(e + 32, parts[i].first_lba);
		put64(e + 40, parts[i].last_lba);
		put64(e + 48, parts[i].attrs);
		for (int k = 0; k < 36 && parts[i].name[k]; k++)
			e[56 + k * 2] = (u8)parts[i].name[k];
	}
	u32 ecrc = crc32(entries, ENTRIES * ENTRY_SIZE);
	u8 primary[SECTOR], backup[SECTOR];
	u64 last = disk_sectors - 1;
	header(primary, 1, last, 2, disk_sectors, disk_guid, ecrc);
	header(backup, last, 1, last - ENTRY_SECTORS, disk_sectors, disk_guid, ecrc);
	bool ok = write_at(fd, 0, mbr, SECTOR) && write_at(fd, 1, primary, SECTOR) && write_at(fd, 2, entries, ENTRIES * ENTRY_SIZE) &&
		  write_at(fd, last - ENTRY_SECTORS, entries, ENTRIES * ENTRY_SIZE) && write_at(fd, last, backup, SECTOR);
	mem_free(entries);
	return ok;
}

int gpt_read(int fd, u64 disk_sectors, GptPart *parts, int max, u8 disk_guid[16])
{
	u8 h[SECTOR];
	if (!read_at(fd, 1, h, SECTOR) || memcmp(h, "EFI PART", 8) != 0)
		return -1;
	u32 hsize = get32(h + 12);
	if (hsize < 92 || hsize > SECTOR)
		return -1;
	u32 want = get32(h + 16);
	put32(h + 16, 0);
	if (crc32(h, hsize) != want)
		return -1;
	if (disk_guid)
		memcpy(disk_guid, h + 56, 16);
	u64 elba = get64(h + 72);
	u32 count = get32(h + 80), esize = get32(h + 84);
	if (esize < 128 || count > 1024 || elba >= disk_sectors)
		return -1;
	usize bytes = (usize)count * esize;
	u8 *entries = mem_alloc(bytes);
	if (!read_at(fd, elba, entries, bytes)) {
		mem_free(entries);
		return -1;
	}
	int n = 0;
	for (u32 i = 0; i < count && n < max; i++) {
		const u8 *e = entries + (usize)i * esize;
		static const u8 zero[16];
		if (memcmp(e, zero, 16) == 0)
			continue;
		GptPart *p = &parts[n++];
		memcpy(p->type, e, 16);
		memcpy(p->guid, e + 16, 16);
		p->first_lba = get64(e + 32);
		p->last_lba = get64(e + 40);
		p->attrs = get64(e + 48);
		for (int k = 0; k < 36; k++)
			p->name[k] = (char)e[56 + k * 2];
		p->name[36] = 0;
	}
	mem_free(entries);
	return n;
}
