/* Tests for the disk formats: GPT, FAT32 file placement and ISO 9660 with El Torito. */
#include "zd.h"

static int checks, failed;
#define CHECK(c)                                                         \
	do {                                                             \
		checks++;                                                \
		if (!(c)) {                                              \
			failed++;                                        \
			eprint("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		}                                                        \
	} while (0)

#define IMG "build/tests/zd_test.img"

static u32 le32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u32 be32(const u8 *p)
{
	return (u32)p[3] | (u32)p[2] << 8 | (u32)p[1] << 16 | (u32)p[0] << 24;
}

static bool read_at(int fd, u64 byte, void *buf, usize n)
{
	return sys_pread(fd, buf, n, (i64)byte) == (long)n;
}

static int open_image(u64 bytes)
{
	int fd = (int)sys_open(IMG, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0)
		sys_ftruncate(fd, (i64)bytes);
	return fd;
}

static void test_gpt(void)
{
	u64 sectors = (64ull << 20) / SECTOR;
	int fd = open_image(sectors * SECTOR);
	CHECK(fd >= 0);
	GptPart p = { 0 };
	memcpy(p.type, GPT_TYPE_ESP, 16);
	guid_random(p.guid);
	p.first_lba = 2048, p.last_lba = 40959;
	str_copy(p.name, "ZURBAX-INSTALL", sizeof p.name);
	u8 disk[16];
	guid_random(disk);
	CHECK(gpt_write(fd, sectors, &p, 1, disk));
	GptPart back[4];
	int n = gpt_read(fd, sectors, back, 4, NULL);
	CHECK(n == 1 && back[0].first_lba == 2048 && back[0].last_lba == 40959 && str_eq(back[0].name, "ZURBAX-INSTALL"));
	CHECK(n == 1 && memcmp(back[0].type, GPT_TYPE_ESP, 16) == 0);
	sys_close(fd);
}

static void test_fat(void)
{
	/* an awkward start sector: file data still lands on a 4 KiB boundary */
	u64 start = 2049, sectors = (40ull << 20) / SECTOR;
	int fd = open_image((start + sectors) * SECTOR);
	FatWriter *w = fat_format(fd, start, sectors, "ZOS");
	CHECK(w != NULL);
	if (!w)
		return;
	static u8 data[10000];
	for (usize i = 0; i < sizeof data; i++)
		data[i] = (u8)(i * 7);
	CHECK(fat_mkdir(w, "/EFI"));
	CHECK(fat_add_file(w, "/EFI/KERNEL.BIN", data, sizeof data));
	CHECK(fat_add_file(w, "/ZOS.TXT", "hello", 5));
	u64 at = fat_file_byte(w, "/EFI/KERNEL.BIN");
	CHECK(at && at % 4096 == 0);
	CHECK(fat_file_byte(w, "/MISSING.TXT") == 0 && fat_file_byte(w, "/EFI") == 0);
	CHECK(fat_finish(w));
	u8 back[sizeof data];
	CHECK(read_at(fd, at, back, sizeof back) && memcmp(back, data, sizeof data) == 0);
	/* the boot sector agrees about where things are */
	u8 bs[512];
	CHECK(read_at(fd, start * SECTOR, bs, sizeof bs) && bs[510] == 0x55 && bs[511] == 0xaa);
	u32 reserved = (u32)bs[14] | (u32)bs[15] << 8, fat = le32(bs + 36);
	CHECK(reserved >= 32 && (start + reserved + 2 * fat) % 8 == 0);
	CHECK(memcmp(bs + 71, "ZOS        ", 11) == 0);
	sys_close(fd);
}

/* Finds a name in a folder's records; returns the record's extent and size. */
static bool iso_find(int fd, u32 dir, u32 size, const char *name, u32 *extent, u32 *len, bool *is_dir)
{
	u8 *buf = mem_alloc(size);
	bool found = false;
	if (read_at(fd, (u64)dir * ISO_BLOCK, buf, size))
		for (u32 i = 0; i < size && !found;) {
			u8 n = buf[i];
			if (n == 0) {
				i = (i / ISO_BLOCK + 1) * ISO_BLOCK;
				continue;
			}
			const u8 *r = buf + i;
			if (r[32] == strlen(name) && memcmp(r + 33, name, r[32]) == 0 && le32(r + 2) == be32(r + 6)) {
				*extent = le32(r + 2), *len = le32(r + 10), *is_dir = r[25] & 2;
				found = true;
			}
			i += n;
		}
	mem_free(buf);
	return found;
}

static void test_iso(void)
{
	const u64 boot = 128 * 1024, boot_size = 1 << 20, readme = 48 * ISO_BLOCK;
	int fd = open_image(boot + boot_size);
	static u8 kernel[5000];
	for (usize i = 0; i < sizeof kernel; i++)
		kernel[i] = (u8)(i ^ 0x5a);
	CHECK(sys_pwrite(fd, kernel, sizeof kernel, (i64)(boot + 4096)) == (long)sizeof kernel);
	CHECK(sys_pwrite(fd, "read me", 7, (i64)readme) == 7);
	IsoFile files[] = { { "/EFI/BOOT/BOOTX64.EFI", boot + 4096, sizeof kernel }, { "/ZOS.TXT", readme, 7 } };
	IsoVolume v = { .system_id = "ZOS", .volume_id = "ZOS_TEST", .publisher = "ZOS", .app_id = "TEST", .boot_byte = boot,
			.boot_size = boot_size, .volume_bytes = boot + boot_size };
	CHECK(iso_write(fd, &v, files, 2));
	u8 pvd[ISO_BLOCK];
	CHECK(read_at(fd, 16 * ISO_BLOCK, pvd, sizeof pvd) && pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0);
	CHECK(le32(pvd + 80) == (boot + boot_size) / ISO_BLOCK && be32(pvd + 84) == le32(pvd + 80));
	CHECK(memcmp(pvd + 40, "ZOS_TEST ", 9) == 0);
	/* walk to the kernel through the folders */
	u32 ext = le32(pvd + 156 + 2), len = le32(pvd + 156 + 10), fext, flen;
	bool dir = false;
	CHECK(iso_find(fd, ext, len, "EFI", &fext, &flen, &dir) && dir);
	CHECK(iso_find(fd, fext, flen, "BOOT", &fext, &flen, &dir) && dir);
	CHECK(iso_find(fd, fext, flen, "BOOTX64.EFI;1", &fext, &flen, &dir) && !dir);
	CHECK((u64)fext * ISO_BLOCK == boot + 4096 && flen == sizeof kernel);
	CHECK(iso_find(fd, ext, len, "ZOS.TXT;1", &fext, &flen, &dir) && fext == 48 && flen == 7);
	/* both path tables list root, EFI, BOOT */
	u32 pt = le32(pvd + 132), l = le32(pvd + 140), m = be32(pvd + 148);
	u8 lt[64] = { 0 }, mt[64] = { 0 };
	CHECK(pt == 10 + 12 + 12 && read_at(fd, (u64)l * ISO_BLOCK, lt, pt) && read_at(fd, (u64)m * ISO_BLOCK, mt, pt));
	CHECK(lt[0] == 1 && le32(lt + 2) == ext && be32(mt + 2) == ext);
	CHECK(lt[10] == 3 && memcmp(lt + 18, "EFI", 3) == 0 && lt[16] == 1);
	CHECK(lt[22] == 4 && memcmp(lt + 30, "BOOT", 4) == 0 && lt[28] == 2 && mt[29] == 2);
	/* El Torito: the boot record, a valid catalog, a UEFI entry for the image */
	u8 br[ISO_BLOCK], cat[64];
	CHECK(read_at(fd, 17 * ISO_BLOCK, br, sizeof br) && memcmp(br + 7, "EL TORITO SPECIFICATION", 23) == 0);
	CHECK(read_at(fd, (u64)le32(br + 71) * ISO_BLOCK, cat, sizeof cat));
	u16 sum = 0;
	for (int i = 0; i < 32; i += 2)
		sum = (u16)(sum + (cat[i] | cat[i + 1] << 8));
	CHECK(cat[0] == 1 && cat[1] == 0xef && cat[30] == 0x55 && cat[31] == 0xaa && sum == 0);
	CHECK(cat[32] == 0x88 && cat[33] == 0 && (cat[38] | cat[39] << 8) == boot_size / 512 && le32(cat + 40) == boot / ISO_BLOCK);
	/* bad input is refused */
	IsoFile odd[] = { { "/A.TXT", readme + 100, 1 } };
	CHECK(!iso_write(fd, &v, odd, 1));
	IsoFile early[] = { { "/A.TXT", 18 * ISO_BLOCK, 1 } };
	CHECK(!iso_write(fd, &v, early, 1));
	IsoFile badname[] = { { "/a long name.txt", readme, 1 } };
	CHECK(!iso_write(fd, &v, badname, 1));
	sys_close(fd);
}

int main(void)
{
	test_gpt();
	test_fat();
	test_iso();
	sys_unlink(IMG);
	print("libzd: %d/%d checks passed\n", checks - failed, checks);
	return failed ? 1 : 0;
}
