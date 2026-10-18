/*
 * mkimage — builds what Zurbax boots from.
 *
 *   mkimage cpio OUT.cpio [HOOK...]  the system archive the kernel embeds (HOOKs:
 *                                    test helpers for test kernels only, in
 *                                    /system/test):
 *                                    build/bin, build/apps + apps/<app> files,
 *                                    data/ as /system/share, build/firmware as
 *                                    /lib/firmware
 *   mkimage disk OUT.img EFI-FILE    a GPT disk image with one EFI system
 *                                    partition holding EFI/BOOT/BOOTX64.EFI;
 *                                    write it to a USB stick and boot it
 *   mkimage iso OUT.iso EFI-FILE     the same as an ISO: it boots from a DVD
 *                                    (El Torito, UEFI) and, written to a USB
 *                                    stick as is, from the stick (GPT)
 *   mkimage iso --skeleton OUT.iso EFI-FILE LAYER...
 *                                    the ISO without its layers' contents
 *                                    (holes where they go) and OUT.iso.map,
 *                                    "BYTE SIZE PATH" for each: together, the
 *                                    whole image, for tools/publish-ia.py to
 *                                    send without it ever being on disk
 *   mkimage ext4 OUT.img MIB         an empty ext4 image (for tests)
 *   mkimage repo OUTDIR              Harbor's built-in source: a package per
 *                                    store/<app>, its icon and screenshot,
 *                                    and the index
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
		/* a link within the tree stays a link (firmware files many GPUs share) */
		struct stat ls;
		char target[256] = { 0 };
		if (sys_lstat(s, &ls) == 0 && S_ISLNK(ls.st_mode) && sys_readlink(s, target, sizeof target - 1) > 0 && target[0] != '/') {
			check(cpio_symlink(&cw, d, target), d);
		} else if (S_ISDIR(e->mode)) {
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

static int make_cpio(const char *out, char **hooks, int nhooks)
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
		/* sudo checks the password itself; zinstall works only in the live
		 * session; harborctl installs only signed or built-in apps. */
		const char *b = bins.data[i].name;
		add_host_file(s, d, (str_eq(b, "sudo") || str_eq(b, "zinstall") || str_eq(b, "harborctl")) ? 04755 : 0755);
		mem_free(s);
		mem_free(d);
	}
	dir_list_free(&bins);
	if (nhooks)
		check(cpio_dir(&cw, "system/test", 0755), "system/test");
	for (int i = 0; i < nhooks; i++) {
		char *d = path_join("system/test", path_base(hooks[i]));
		add_host_file(hooks[i], d, 0755);
		mem_free(d);
	}

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
	/* Wi-Fi and GPU firmware and the regulatory database, where the kernel looks (tools/fetch-firmware.py) */
	if (dir_exists("build/firmware")) {
		check(cpio_dir(&cw, "lib", 0755), "lib");
		check(cpio_dir(&cw, "lib/firmware", 0755), "lib/firmware");
		add_tree("build/firmware", "lib/firmware", NULL);
	}
	/* Harbor's own apps. The folder comes first: the kernel drops files
	 * whose folder the archive hasn't made yet. */
	if (dir_exists("build/repo")) {
		check(cpio_dir(&cw, "system/share/harbor-repo", 0755), "system/share/harbor-repo");
		add_tree("build/repo", "system/share/harbor-repo", NULL);
	}

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

/* On every install medium, for people who open it on another computer. The
 * installer also looks for it to recognize a stick made by copying files. */
static const char README[] =
	OS_PRETTY_NAME "\r\n"
	"\r\n"
	"This is the " OS_NAME " installer. To start " OS_NAME ", restart the computer and\r\n"
	"choose this USB stick or disc in its boot menu (F8 or Esc on most ASUS\r\n"
	"computers, F12 on many others). " OS_NAME " starts in UEFI mode.\r\n"
	"\r\n"
	"You can try everything first. Nothing on the computer changes until you\r\n"
	"choose Install " OS_NAME ".\r\n";

/* The Linux layer's images, when they were built (the layer itself, the
 * firmware of every device): on the medium as LAYERS/NAME.SFS, where a live
 * session uses them and the installer copies them from. FAT32 and ISO 9660
 * keep files under 4 GiB, so a big layer comes as several images. */
#define LAYER_DIR "/LAYERS"
#define MAX_LAYERS 8

typedef struct {
	const char *path;
	char name[40];              /* /LAYERS/STEAM.SFS */
	i64 size;
	u64 byte;                   /* where it starts in the image (ISO) */
} Layer;

static int layers_open(char **paths, int n, Layer *out)
{
	if (n > MAX_LAYERS) {
		eprint("mkimage: %d layer images at most\n", MAX_LAYERS);
		return -1;
	}
	for (int i = 0; i < n; i++) {
		Layer *l = &out[i];
		*l = (Layer){ .path = paths[i] };
		l->size = file_size(paths[i]);
		if (l->size <= 0 || l->size >= (4ll << 30)) {
			eprint("mkimage: %s: %s\n", paths[i], l->size <= 0 ? "cannot read it" : "4 GiB or more (split the layer)");
			return -1;
		}
		char base[32];
		str_copy(base, path_base(paths[i]), sizeof base);
		for (char *c = base; *c; c++)
			*c = (char)to_upper((u8)*c);
		fmt(l->name, sizeof l->name, LAYER_DIR "/%s", base);
	}
	return n;
}

static u64 layers_bytes(const Layer *l, int n)
{
	u64 sum = 0;
	for (int i = 0; i < n; i++)
		sum += (u64)l[i].size;
	return sum;
}

static bool skeleton; /* the layers' room only, their contents left out */

static void add_layers(FatWriter *w, Layer *l, int n)
{
	if (!n)
		return;
	check(fat_mkdir(w, LAYER_DIR), "LAYERS folder");
	for (int i = 0; i < n; i++)
		check(skeleton ? fat_add_file_space(w, l[i].name, (u64)l[i].size) : fat_add_file_from(w, l[i].name, l[i].path), l[i].name);
}

static int make_disk(const char *out, const char *efi, char **layer_paths, int nlayers)
{
	Layer layers[MAX_LAYERS];
	i64 efi_size = file_size(efi);
	if (efi_size <= 0) {
		eprint("mkimage: cannot read %s\n", efi);
		return 1;
	}
	if ((nlayers = layers_open(layer_paths, nlayers, layers)) < 0)
		return 1;
	/* Room for the kernel, an update of it, the layers and FAT32's minimum cluster count. */
	u64 mib = MAX(64ull, ((u64)efi_size * 2 + layers_bytes(layers, nlayers) * 129 / 128 + (16u << 20) * (u64)(1 + nlayers)) >> 20);
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
	FatWriter *w = fat_format(fd, esp.first_lba, esp.last_lba - esp.first_lba + 1, "Zurbax"); /* the name Windows shows for the stick */
	check(w != NULL, "FAT32 format");
	if (w) {
		check(fat_mkdir(w, "/EFI"), "EFI folder");
		check(fat_mkdir(w, "/EFI/BOOT"), "EFI/BOOT folder");
		check(fat_add_file_from(w, "/EFI/BOOT/BOOTX64.EFI", efi), "BOOTX64.EFI");
		check(fat_add_file(w, "/ZURBAX.TXT", README, sizeof README - 1), "ZURBAX.TXT");
		add_layers(w, layers, nlayers);
		check(fat_finish(w), "FAT32 tables");
	}
	sys_fsync(fd);
	sys_close(fd);
	if (failed)
		return 1;
	print("%s: %llu MiB, EFI system partition with %s and %d layer image%s\n", out, (unsigned long long)mib, path_base(efi),
	      nlayers, nlayers == 1 ? "" : "s");
	return 0;
}

/*
 * The ISO holds one FAT32 image with the kernel, which serves three ways:
 *   - from a DVD, the El Torito catalog points UEFI firmware at it;
 *   - written to a USB stick, a GPT in the first 32 KiB names it as the
 *     EFI system partition (called ZURBAX-INSTALL, like the disk image's);
 *   - read as a disc (or copied file by file to a stick), the ISO 9660
 *     tree's EFI/BOOT/BOOTX64.EFI points at the kernel inside it, so the
 *     kernel is stored once.
 */
static int make_iso(const char *out, const char *efi, char **layer_paths, int nlayers)
{
	Layer layers[MAX_LAYERS];
	i64 efi_size = file_size(efi);
	if (efi_size <= 0) {
		eprint("mkimage: cannot read %s\n", efi);
		return 1;
	}
	if ((nlayers = layers_open(layer_paths, nlayers, layers)) < 0)
		return 1;
	u64 lsize = layers_bytes(layers, nlayers) + (1u << 20) * (u64)nlayers;
	const u64 readme_byte = 48 * ISO_BLOCK, fat_byte = 128 * 1024;
	/* FAT32 needs 65525 clusters, so the image is at least about 34 MiB;
	 * the two FATs take 8 bytes per cluster, 1/512 of the data at 4 KiB
	 * clusters (room for 1/128, and more for the files' last clusters) */
	u64 fat_bytes = MAX(40ull, ((u64)efi_size + lsize + lsize / 128 + (16u << 20) + (1u << 20) - 1) >> 20) << 20;
	u64 volume = fat_byte + fat_bytes;
	/* room for the backup GPT after the file system, in whole disc sectors */
	u64 total = (volume + 64 * SECTOR + ISO_BLOCK - 1) / ISO_BLOCK * ISO_BLOCK;
	int fd = open_image(out, total);
	if (fd < 0)
		return 1;
	GptPart esp = { 0 };
	memcpy(esp.type, GPT_TYPE_ESP, 16);
	guid_random(esp.guid);
	esp.first_lba = fat_byte / SECTOR;
	esp.last_lba = (fat_byte + fat_bytes) / SECTOR - 1;
	str_copy(esp.name, "ZURBAX-INSTALL", sizeof esp.name);
	check(esp.last_lba <= gpt_last_usable(total / SECTOR), "layout");
	u8 disk[16];
	guid_random(disk);
	check(gpt_write(fd, total / SECTOR, &esp, 1, disk), "partition table");
	u64 kernel_byte = 0;
	FatWriter *w = fat_format(fd, esp.first_lba, fat_bytes / SECTOR, "Zurbax");
	check(w != NULL, "FAT32 format");
	if (w) {
		check(fat_mkdir(w, "/EFI"), "EFI folder");
		check(fat_mkdir(w, "/EFI/BOOT"), "EFI/BOOT folder");
		check(fat_add_file_from(w, "/EFI/BOOT/BOOTX64.EFI", efi), "BOOTX64.EFI");
		check(fat_add_file(w, "/ZURBAX.TXT", README, sizeof README - 1), "ZURBAX.TXT");
		kernel_byte = fat_file_byte(w, "/EFI/BOOT/BOOTX64.EFI");
		check(kernel_byte && kernel_byte % ISO_BLOCK == 0, "kernel alignment");
		add_layers(w, layers, nlayers);
		for (int i = 0; i < nlayers; i++) {
			layers[i].byte = fat_file_byte(w, layers[i].name);
			check(layers[i].byte && layers[i].byte % ISO_BLOCK == 0, "layer alignment");
		}
		check(fat_finish(w), "FAT32 tables");
	}
	check(sys_pwrite(fd, README, sizeof README - 1, (i64)readme_byte) == (long)sizeof README - 1, "readme");
	/* the layers too are stored once, seen from both file systems */
	IsoFile files[2 + MAX_LAYERS] = {
		{ "/EFI/BOOT/BOOTX64.EFI", kernel_byte, (u64)efi_size },
		{ "/ZURBAX.TXT", readme_byte, sizeof README - 1 },
	};
	for (int i = 0; i < nlayers; i++)
		files[2 + i] = (IsoFile){ layers[i].name, layers[i].byte, (u64)layers[i].size };
	IsoVolume v = { .system_id = OS_NAME, .volume_id = "ZURBAX_2_1", .publisher = OS_NAME, .app_id = OS_PRETTY_NAME,
			.boot_byte = fat_byte, .boot_size = fat_bytes, .volume_bytes = volume };
	check(!failed && iso_write(fd, &v, files, 2 + nlayers), "ISO 9660 records");
	sys_fsync(fd);
	sys_close(fd);
	if (skeleton && !failed) {
		StrBuf map = { 0 };
		for (int i = 0; i < nlayers; i++)
			sb_fmt(&map, "%llu %lld %s\n", (unsigned long long)layers[i].byte, (long long)layers[i].size, layers[i].path);
		char *path = str_fmt("%s.map", out);
		check(file_write(path, map.data, map.len), "map");
		mem_free(path);
		sb_free(&map);
	}
	if (failed)
		return 1;
	print("%s: %llu MiB, boots from a disc or a USB stick\n", out, (unsigned long long)(total >> 20));
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

/* ---- Harbor's built-in source ------------------------------------------------------------ */

static void index_line(StrBuf *ix, const char *key, const char *value)
{
	if (value && *value)
		sb_fmt(ix, "%s = %s\n", key, value);
}

static int make_repo(const char *out)
{
	dir_make(out, 0755);
	DirList store = { 0 };
	if (!dir_list("store", &store, false)) {
		eprint("mkimage: no store/ folder\n");
		return 1;
	}
	StrBuf ix = { 0 };
	sb_adds(&ix, "# Apps built for " OS_NAME ", offered by Harbor.\nsource = " OS_NAME "\n");
	for (usize i = 0; i < store.len; i++) {
		const char *a = store.data[i].name;
		char *conf_path = str_fmt("store/%s/app.conf", a), *meta_path = str_fmt("store/%s/store.conf", a);
		Conf *app = conf_load(conf_path), *meta = conf_load(meta_path);
		const char *version = conf_get(meta, "version", "1.0");
		char *pkg = str_fmt("%s-%s.zpk", a, version);
		char *pkg_path = path_join(out, pkg);
		int fd = (int)sys_open(pkg_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd < 0) {
			eprint("mkimage: cannot create %s\n", pkg_path);
			return 1;
		}
		cpio_begin(&cw, fd);
		char *bin = str_fmt("build/store/%s/%s", a, a), *src = str_fmt("store/%s", a);
		add_host_file(bin, a, 0755);
		/* everything in the app's folder except sources and the store text */
		DirList files = { 0 };
		dir_list(src, &files, false);
		vec_each(&files, f)
		{
			if (str_ends(f->name, ".c") || str_ends(f->name, ".h") || str_eq(f->name, "store.conf") || f->type == DT_DIR)
				continue;
			char *s2 = path_join(src, f->name);
			add_host_file(s2, f->name, 0644);
			mem_free(s2);
		}
		dir_list_free(&files);
		check(cpio_end(&cw), pkg);
		sys_close(fd);
		usize len;
		char *data = file_read(pkg_path, &len);
		u8 digest[32];
		char hex[65];
		sha256(data, len, digest);
		hex_encode(digest, 32, hex);
		mem_free(data);
		/* icon and screenshot, for showing the app before it's installed */
		char *icon_src = str_fmt("store/%s/icon.svg", a), *icon_dst = str_fmt("%s/%s.svg", out, a);
		char *shot_src = str_fmt("build/store/%s/screenshot.png", a), *shot_dst = str_fmt("%s/%s-1.png", out, a);
		usize n;
		char *icon = file_read(icon_src, &n);
		if (icon)
			file_write(icon_dst, icon, n);
		mem_free(icon);
		char *shot = file_read(shot_src, &n);
		if (shot)
			file_write(shot_dst, shot, n);
		bool have_shot = shot != NULL;
		mem_free(shot);
		sb_fmt(&ix, "\n[%s]\n", a);
		index_line(&ix, "name", conf_get(app, "name", a));
		index_line(&ix, "version", version);
		index_line(&ix, "category", conf_get(meta, "category", conf_get(app, "category", "Other")));
		index_line(&ix, "developer", conf_get(meta, "developer", OS_NAME));
		index_line(&ix, "summary", conf_get(meta, "summary", ""));
		index_line(&ix, "description", conf_get(meta, "description", ""));
		index_line(&ix, "file", pkg);
		char size[32];
		fmt(size, sizeof size, "%llu", (unsigned long long)len);
		index_line(&ix, "size", size);
		index_line(&ix, "sha256", hex);
		char icon_name[96], shot_name[96];
		fmt(icon_name, sizeof icon_name, "%s.svg", a);
		fmt(shot_name, sizeof shot_name, "%s-1.png", a);
		index_line(&ix, "icon", icon_name);
		if (have_shot)
			index_line(&ix, "screenshot", shot_name);
		print("  %s %s (%llu KiB)\n", a, version, (unsigned long long)(len / 1024));
		conf_free(app), conf_free(meta);
		mem_free(conf_path), mem_free(meta_path), mem_free(pkg), mem_free(pkg_path), mem_free(bin), mem_free(src);
		mem_free(icon_src), mem_free(icon_dst), mem_free(shot_src), mem_free(shot_dst);
	}
	dir_list_free(&store);
	char *index_path = path_join(out, "index");
	bool ok = file_write(index_path, ix.data, ix.len);
	mem_free(index_path);
	sb_free(&ix);
	return ok && !failed ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && str_eq(argv[1], "cpio"))
		return make_cpio(argv[2], argv + 3, argc - 3);
	if (argc >= 4 && str_eq(argv[1], "disk"))
		return make_disk(argv[2], argv[3], argv + 4, argc - 4);
	if (argc >= 5 && str_eq(argv[1], "iso") && str_eq(argv[2], "--skeleton")) {
		skeleton = true;
		return make_iso(argv[3], argv[4], argv + 5, argc - 5);
	}
	if (argc >= 4 && str_eq(argv[1], "iso"))
		return make_iso(argv[2], argv[3], argv + 4, argc - 4);
	if (argc == 4 && str_eq(argv[1], "ext4"))
		return make_ext4(argv[2], argv[3]);
	if (argc == 3 && str_eq(argv[1], "repo"))
		return make_repo(argv[2]);
	eprint("usage: mkimage cpio OUT.cpio | disk OUT.img EFI-FILE [LAYER...] | iso OUT.iso EFI-FILE [LAYER...] | ext4 OUT.img MIB\n");
	return 2;
}
