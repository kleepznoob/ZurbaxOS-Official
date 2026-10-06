/* harborctl: installs and removes apps for Harbor. It runs with root's rights
 * (set-user-ID) because apps live in /data/apps for everyone, so it trusts
 * nothing it is handed: the index must come from the system's own source or
 * carry a signature from a trusted key, and the package must match the
 * index's size and SHA-256. Packages are read once into memory, checked,
 * and unpacked from that copy.
 *
 *   harborctl install system - ID          from the source built into /system
 *   harborctl install INDEX PACKAGE ID     from a downloaded (signed) index
 *   harborctl remove ID
 *
 * Prints "done", or "error <why>". */
#include "harbor_repo.h"

#define SYSTEM_SOURCE "/system/share/harbor-repo/"
#define KEYS "/system/share/harbor/keys"

static bool privileged;

static NORETURN void fail(const char *why)
{
	print("error %s\n", why);
	exit(1);
}

static const char *data_dir(void)
{
	/* A set-user-ID program must not take paths from the environment. */
	const char *d = privileged ? NULL : env_get("ZURBAX_DATA");
	return d ? d : "/data";
}

static const char *system_source(void)
{
	static char buf[512];
	const char *root = privileged ? NULL : env_get("ZURBAX_ROOT");
	if (!root)
		return SYSTEM_SOURCE;
	fmt(buf, sizeof buf, "%s/share/harbor-repo/", root);
	return buf;
}

static const char *keys_dir(void)
{
	static char buf[512];
	const char *root = privileged ? NULL : env_get("ZURBAX_ROOT");
	if (!root)
		return KEYS;
	fmt(buf, sizeof buf, "%s/share/harbor/keys", root);
	return buf;
}

static void remove_tree(const char *path)
{
	DirList l = { 0 };
	if (dir_list(path, &l, false)) {
		vec_each(&l, e)
		{
			char *p = path_join(path, e->name);
			if (e->type == DT_DIR)
				remove_tree(p);
			else
				sys_unlink(p);
			mem_free(p);
		}
		dir_list_free(&l);
	}
	sys_rmdir(path);
}

static u32 hex8(const char *s)
{
	u32 v = 0;
	for (int i = 0; i < 8; i++) {
		char c = s[i];
		v = v * 16 + (u32)(is_digit(c) ? c - '0' : to_lower(c) - 'a' + 10);
	}
	return v;
}

/* Unpacks a newc cpio archive into dir. Only plain files and folders with
 * ordinary permissions, at relative paths that stay inside. */
static bool unpack(const u8 *d, usize n, const char *dir, char *why, usize cap)
{
	usize p = 0;
	bool have_conf = false;
	while (p + 110 <= n) {
		if (memcmp(d + p, "070701", 6) != 0) {
			str_copy(why, "the package is damaged", cap);
			return false;
		}
		const char *h = (const char *)d + p;
		u32 mode = hex8(h + 14), size = hex8(h + 54), namesize = hex8(h + 94);
		usize name_at = p + 110;
		if (namesize == 0 || name_at + namesize > n) {
			str_copy(why, "the package is damaged", cap);
			return false;
		}
		char name[256];
		if (namesize > sizeof name) {
			str_copy(why, "a file name in the package is too long", cap);
			return false;
		}
		memcpy(name, d + name_at, namesize);
		name[namesize - 1] = 0;
		usize data_at = ALIGN_UP(name_at + namesize, (usize)4);
		if (data_at + size > n) {
			str_copy(why, "the package is damaged", cap);
			return false;
		}
		if (str_eq(name, "TRAILER!!!"))
			return have_conf || (str_copy(why, "the package has no app.conf", cap), false);
		const char *rel = name;
		while (rel[0] == '.' && rel[1] == '/')
			rel += 2;
		if (rel[0] == '/' || str_find(rel, "..") || !*rel || str_eq(rel, ".")) {
			p = ALIGN_UP(data_at + size, (usize)4);
			if (str_eq(rel, ".") || !*rel)
				continue;
			str_copy(why, "the package tries to write outside its folder", cap);
			return false;
		}
		char *path = path_join(dir, rel);
		u32 type = mode & 0170000;
		if (type == 0040000) {
			dir_make(path, 0755);
		} else if (type == 0100000) {
			char *parent = path_dir(path);
			dir_make(parent, 0755);
			mem_free(parent);
			if (!file_write(path, d + data_at, size)) {
				mem_free(path);
				str_copy(why, "the app couldn't be written to disk", cap);
				return false;
			}
			/* executable or not; never set-user-ID */
			sys_chmod(path, (mode & 0111) ? 0755 : 0644);
			if (str_eq(rel, "app.conf"))
				have_conf = true;
		} else {
			mem_free(path);
			str_copy(why, "the package contains something that isn't a file", cap);
			return false;
		}
		mem_free(path);
		p = ALIGN_UP(data_at + size, (usize)4);
	}
	str_copy(why, "the package is damaged", cap);
	return false;
}

static void install(const char *index_path, const char *pkg_path, const char *id)
{
	/* Trust: the system's own source (read from /system here, never from a
	 * path we were handed), or an index with a valid signature. */
	bool system = str_eq(index_path, "system");
	char *sys_index = str_fmt("%sindex", system_source());
	usize ilen;
	char *index = file_read(system ? sys_index : index_path, &ilen);
	mem_free(sys_index);
	if (!index)
		fail("the app list couldn't be read");
	if (!system) {
		char *sig_path = str_fmt("%s.sig", index_path);
		usize slen;
		u8 *sig = (u8 *)file_read(sig_path, &slen);
		mem_free(sig_path);
		if (!sig || !harbor_verify(index, ilen, sig, slen, keys_dir()))
			fail("this source isn't signed by a trusted publisher");
		mem_free(sig);
	}
	HarborIndex ix;
	harbor_parse(index, &ix);
	const HarborApp *app = harbor_find(&ix, id);
	if (!app)
		fail("the app isn't in this source");
	usize len;
	char *sys_pkg = str_fmt("%s%s", system_source(), app->file);
	u8 *pkg = (u8 *)file_read(system ? sys_pkg : pkg_path, &len);
	mem_free(sys_pkg);
	if (!pkg)
		fail("the download is missing");
	u8 digest[32];
	sha256(pkg, len, digest);
	if ((app->size && len != app->size) || !mem_equal_ct(digest, app->sha256, 32))
		fail("the download doesn't match what the source promised");
	char *apps = path_join(data_dir(), "apps");
	dir_make(apps, 0755);
	char *tmp = str_fmt("%s/.%s.new", apps, id), *dest = str_fmt("%s/%s", apps, id), *old = str_fmt("%s/.%s.old", apps, id);
	remove_tree(tmp);
	dir_make(tmp, 0755);
	char why[160];
	if (!unpack(pkg, len, tmp, why, sizeof why)) {
		remove_tree(tmp);
		fail(why);
	}
	char *marker = str_fmt("%s/.harbor", tmp);
	char *info = str_fmt("version = %s\nsource = %s\n", app->version, ix.name[0] ? ix.name : "Zurbax");
	file_write_str(marker, info);
	mem_free(marker);
	mem_free(info);
	/* Swap the new folder in. */
	remove_tree(old);
	if (dir_exists(dest) && sys_rename(dest, old) < 0) {
		remove_tree(tmp);
		fail("the old version couldn't be replaced");
	}
	if (sys_rename(tmp, dest) < 0) {
		sys_rename(old, dest);
		remove_tree(tmp);
		fail("the app couldn't be put in place");
	}
	remove_tree(old);
	print("done\n");
}

static void remove_app(const char *id)
{
	for (const char *p = id; *p; p++)
		if (!is_alnum(*p) && *p != '-' && *p != '_' && *p != '.')
			fail("that isn't an app name");
	if (str_find(id, ".."))
		fail("that isn't an app name");
	char *dir = str_fmt("%s/apps/%s", data_dir(), id);
	char *marker = str_fmt("%s/.harbor", dir);
	/* Only what Harbor installed can be removed this way. */
	if (!file_exists(marker))
		fail("that app wasn't installed from Harbor");
	remove_tree(dir);
	print("done\n");
}

int main(int argc, char **argv)
{
	privileged = sys_getuid() != sys_geteuid();
	sys_umask(022);
	if (argc == 5 && str_eq(argv[1], "install"))
		install(argv[2], argv[3], argv[4]);
	else if (argc == 3 && str_eq(argv[1], "remove"))
		remove_app(argv[2]);
	else
		fail("usage: harborctl install INDEX PACKAGE ID | remove ID");
	return 0;
}
