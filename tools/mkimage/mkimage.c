/*
 * mkimage — builds what Zurbax boots from.
 *
 *   mkimage cpio OUT.cpio            the system archive the kernel embeds:
 *                                    build/bin, build/apps + apps/<app> files,
 *                                    data/ as /system/share
 *   mkimage disk OUT.img EFI-FILE    a GPT disk image with one EFI system
 *                                    partition holding EFI/BOOT/BOOTX64.EFI;
 *                                    write it to a USB stick and boot it
 *   mkimage ext4 OUT.img MIB         an empty ext4 image (for tests)
 *
 * Paths are relative to the source tree (run from the top of the repo).
 */
#include "zd.h"

static CpioWriter cw;
static bool failed;

static void check(bool ok, const char *what)
{
	if (!ok && !failed) {
		eprint("mkimage: %s failed\n", what);
		failed = true;
	}
}

static void add_host_file(const char *src, const char *dst, u32 mode)
{
	usize len;
	char *data = file_read(src, &len);
	if (!data) {
		eprint("mkimage: cannot read %s\n", src);
		failed = true;
		return;
	}
	check(cpio_file(&cw, dst, mode, data, len), dst);
	mem_free(data);
}

/* Copies a host folder tree into the archive under `dst`. */
static void add_tree(const char *src, const char *dst, bool (*skip)(const char *name))
{
	DirList list = { 0 };
	if (!dir_list(src, &list, true)) {
		eprint("mkimage: cannot list %s\n", src);
		failed = true;
		return;
	}
	for (usize i = 0; i < list.len; i++) {
		DirEntry *e = &list.data[i];
		if (e->name[0] == '.' || (skip && skip(e->name)))
			continue;
		char *s = path_join(src, e->name), *d = path_join(dst, e->name);
		if (S_ISDIR(e->mode)) {
			check(cpio_dir(&cw, d, 0755), d);
			add_tree(s, d, skip);
		} else {
			add_host_file(s, d, (e->mode & 0111) ? 0755 : 0644);
		}
		mem_free(s);
		mem_free(d);
	}
	dir_list_free(&list);
}

static bool skip_sources(const char *name)
{
	return str_ends(name, ".c") || str_ends(name, ".h");
}

static int make_cpio(const char *out)
{
	int fd = (int)sys_open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		eprint("mkimage: cannot create %s: %s\n", out, err_str(fd));
		return 1;
	}
	cpio_begin(&cw, fd);
	static const char *const dirs[] = { "dev", "proc", "sys", "run", "tmp", "data", "mnt", "system", "system/bin", "system/apps", "system/share" };
	for (usize i = 0; i < ARRAY_LEN(dirs); i++)
		check(cpio_dir(&cw, dirs[i], str_eq(dirs[i], "tmp") ? 01777 : 0755), dirs[i]);
	/* The kernel opens /dev/console for process 1 before devtmpfs is mounted. */
	check(cpio_device(&cw, "dev/console", S_IFCHR | 0600, 5, 1), "dev/console");
	check(cpio_symlink(&cw, "init", "system/bin/zinit"), "init");

	DirList bins = { 0 };
	if (!dir_list("build/bin", &bins, false)) {
		eprint("mkimage: build/bin is missing; run make first\n");
		return 1;
	}
	for (usize i = 0; i < bins.len; i++) {
		char *s = path_join("build/bin", bins.data[i].name), *d = path_join("system/bin", bins.data[i].name);
		/* sudo checks the password itself; zinstall works only in the live session. */
		add_host_file(s, d, (str_eq(bins.data[i].name, "sudo") || str_eq(bins.data[i].name, "zinstall")) ? 04755 : 0755);
		mem_free(s);
		mem_free(d);
	}
	dir_list_free(&bins);

	DirList apps = { 0 };
	dir_list("apps", &apps, false);
	for (usize i = 0; i < apps.len; i++) {
		const char *a = apps.data[i].name;
		char *dir = str_fmt("system/apps/%s", a), *bin = str_fmt("build/apps/%s/%s", a, a), *src = str_fmt("apps/%s", a);
		char *dst = str_fmt("%s/%s", dir, a);
		check(cpio_dir(&cw, dir, 0755), dir);
		add_host_file(bin, dst, 0755);
		add_tree(src, dir, skip_sources);
		mem_free(dir), mem_free(bin), mem_free(src), mem_free(dst);
	}
	dir_list_free(&apps);
	add_tree("data", "system/share", NULL);

	check(cpio_end(&cw), "archive trailer");
	u64 size = cw.written;
	sys_close(fd);
	if (failed)
		return 1;
	print("%s: %u entries, %llu KiB\n", out, cw.ino - 1, (unsigned long long)(size / 1024));
	return 0;
}

static int open_image(const char *out, u64 bytes)
{
	int fd = (int)sys_open(out, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		eprint("mkimage: cannot create %s: %s\n", out, err_str(fd));
		return -1;
	}
	if (sys_ftruncate(fd, (i64)bytes) < 0) {
		eprint("mkimage: cannot size %s\n", out);
		sys_close(fd);
		return -1;
	}
	return fd;
}

static int make_disk(const char *out, const char *efi)
{
	i64 efi_size = file_size(efi);
	if (efi_size <= 0) {
		eprint("mkimage: cannot read %s\n", efi);
		return 1;
	}
	/* Room for the kernel, an update of it and FAT32's minimum cluster count. */
	u64 mib = MAX(64ull, ((u64)efi_size * 2 + (16u << 20)) >> 20);
	u64 sectors = (mib << 20) / SECTOR;
	int fd = open_image(out, sectors * SECTOR);
	if (fd < 0)
		return 1;
	GptPart esp = { 0 };
	memcpy(esp.type, GPT_TYPE_ESP, 16);
	guid_random(esp.guid);
	esp.first_lba = 2048;
	esp.last_lba = gpt_last_usable(sectors) / 2048 * 2048 - 1;
	str_copy(esp.name, "ZURBAX-INSTALL", sizeof esp.name);
	u8 disk[16];
	guid_random(disk);
	check(gpt_write(fd, sectors, &esp, 1, disk), "partition table");
	FatWriter *w = fat_format(fd, esp.first_lba, esp.last_lba - esp.first_lba + 1, "ZURBAX");
	check(w != NULL, "FAT32 format");
	if (w) {
		check(fat_mkdir(w, "/EFI"), "EFI folder");
		check(fat_mkdir(w, "/EFI/BOOT"), "EFI/BOOT folder");
		check(fat_add_file_from(w, "/EFI/BOOT/BOOTX64.EFI", efi), "BOOTX64.EFI");
		check(fat_finish(w), "FAT32 tables");
	}
	sys_fsync(fd);
	sys_close(fd);
	if (failed)
		return 1;
	print("%s: %llu MiB, EFI system partition with %s\n", out, (unsigned long long)mib, path_base(efi));
	return 0;
}

static int make_ext4(const char *out, const char *mib_text)
{
	u64 bytes = (u64)str_to_int(mib_text, NULL) << 20;
	int fd = open_image(out, bytes);
	if (fd < 0)
		return 1;
	u8 uuid[16];
	guid_random(uuid);
	bool ok = ext4_format(fd, 0, bytes, "ZURBAX-DATA", uuid);
	sys_close(fd);
	if (!ok) {
		eprint("mkimage: ext4 format failed\n");
		return 1;
	}
	print("%s: ext4, %s MiB\n", out, mib_text);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && str_eq(argv[1], "cpio"))
		return make_cpio(argv[2]);
	if (argc == 4 && str_eq(argv[1], "disk"))
		return make_disk(argv[2], argv[3]);
	if (argc == 4 && str_eq(argv[1], "ext4"))
		return make_ext4(argv[2], argv[3]);
	eprint("usage: mkimage cpio OUT.cpio | disk OUT.img EFI-FILE | ext4 OUT.img MIB\n");
	return 2;
}
