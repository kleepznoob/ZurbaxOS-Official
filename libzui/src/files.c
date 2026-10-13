/* File operations shared by Vault and the desktop's file helper (zfiles):
 * copying and moving folders and files, free names, and the Trash in the
 * freedesktop.org layout (~/.local/share/Trash/files and info), so items
 * deleted anywhere can be restored from Vault. */
#include "zui.h"

char *files_unique_path(const char *dir, const char *name)
{
	char *p = path_join(dir, name);
	struct stat st;
	if (sys_lstat(p, &st) < 0)
		return p;
	mem_free(p);
	const char *ext = path_ext(name);
	/* a folder's dots aren't an extension, nor is a leading one (".config") */
	usize stem = strlen(name) - (*ext && ext - 1 != name ? strlen(ext) + 1 : 0);
	if (stem == strlen(name))
		ext = "";
	for (int n = 2; n < 10000; n++) {
		char buf[512];
		if (*ext)
			fmt(buf, sizeof buf, "%.*s (%d).%s", (int)stem, name, n, ext);
		else
			fmt(buf, sizeof buf, "%s (%d)", name, n);
		p = path_join(dir, buf);
		if (sys_lstat(p, &st) < 0)
			return p;
		mem_free(p);
	}
	return path_join(dir, name);
}

bool files_copy_tree(const char *from, const char *to)
{
	struct stat st;
	if (sys_lstat(from, &st) < 0)
		return false;
	if (S_ISLNK(st.st_mode)) {
		char target[1024] = { 0 };
		long n = sys_readlink(from, target, sizeof target - 1);
		return n > 0 && sys_symlink(target, to) == 0;
	}
	if (S_ISDIR(st.st_mode)) {
		if (sys_mkdir(to, (int)(st.st_mode & 07777)) < 0 && !dir_exists(to))
			return false;
		DirList list = { 0 };
		if (!dir_list(from, &list, false))
			return false;
		bool ok = true;
		vec_each(&list, e) {
			char *a = path_join(from, e->name), *b = path_join(to, e->name);
			ok = files_copy_tree(a, b) && ok;
			mem_free(a);
			mem_free(b);
		}
		dir_list_free(&list);
		return ok;
	}
	return file_copy(from, to);
}

bool files_inside(const char *path, const char *dir)
{
	usize n = strlen(dir);
	return str_starts(path, dir) && (path[n] == '/' || path[n] == 0);
}

bool files_move(const char *from, const char *to)
{
	if (files_inside(to, from))
		return false; /* a folder into itself */
	long r = sys_rename(from, to);
	if (r == 0)
		return true;
	if (r != -EXDEV)
		return false;
	/* another drive: copy, then remove the original */
	if (!files_copy_tree(from, to))
		return false;
	return path_remove_all(from);
}

bool files_same_drive(const char *a, const char *b)
{
	struct stat x, y;
	char *da = NULL;
	if (sys_stat(a, &x) < 0) {
		da = path_dir(a);
		long r = sys_stat(da, &x);
		mem_free(da);
		if (r < 0)
			return false;
	}
	if (sys_stat(b, &y) < 0)
		return false;
	return x.st_dev == y.st_dev;
}

char *files_trash_dir(const char *which)
{
	return str_fmt("%s/.local/share/Trash/%s", home_dir(), which);
}

bool files_trash(const char *path)
{
	char *tf = files_trash_dir("files"), *ti = files_trash_dir("info");
	dir_make(tf, 0700);
	dir_make(ti, 0700);
	char *dst = files_unique_path(tf, path_base(path));
	/* where it came from, so Restore can put it back */
	Civil c;
	civil_local(time_unix(), &c);
	char *info = str_fmt("%s/%s.trashinfo", ti, path_base(dst));
	char *body = str_fmt("[Trash Info]\nPath=%s\nDeletionDate=%04d-%02d-%02dT%02d:%02d:%02d\n", path, c.year, c.month, c.day,
			     c.hour, c.min, c.sec);
	file_write(info, body, strlen(body));
	bool ok = files_move(path, dst);
	if (!ok)
		sys_unlink(info);
	mem_free(info);
	mem_free(body);
	mem_free(dst);
	mem_free(tf);
	mem_free(ti);
	return ok;
}

int files_paste(const char *list, const char *dir, bool move, char *last, usize cap)
{
	int failed = 0;
	usize n;
	char **lines = str_split(list, '\n', &n);
	for (usize i = 0; i < n; i++) {
		const char *src = str_trim(lines[i]);
		if (*src != '/')
			continue;
		char *parent = path_dir(src);
		bool here = str_eq(parent, dir);
		mem_free(parent);
		if (here && move)
			continue; /* already where it's going */
		if (files_inside(dir, src)) {
			failed++; /* a folder into itself */
			continue;
		}
		char *dst = files_unique_path(dir, path_base(src));
		bool ok = move ? files_move(src, dst) : files_copy_tree(src, dst);
		failed += !ok;
		if (ok && last)
			str_copy(last, dst, cap);
		mem_free(dst);
	}
	str_split_free(lines, n);
	return failed;
}
