/*
 * zfiles — file operations the desktop asks for, run as the person.
 *
 * The display server works as root; when files are dropped on the desktop,
 * pasted there or deleted from it, it starts zfiles as the person instead,
 * so new files are theirs and their permissions apply.
 *
 *   zfiles move SRC DST [SRC DST]...   move (across drives too)
 *   zfiles copy SRC DST [SRC DST]...
 *   zfiles trash PATH...               to the Trash, restorable from Vault
 *   zfiles mkdir PATH                  a new folder
 *   zfiles touch PATH                  a new empty file
 *   zfiles rename FROM TO
 *
 * A destination taken in the meantime gets a free name next to it. The exit
 * status is the number of items that failed.
 */
#include "zui.h"

static char *free_name(const char *dst)
{
	char *dir = path_dir(dst);
	char *p = files_unique_path(dir, path_base(dst));
	mem_free(dir);
	return p;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		eprint("usage: zfiles move|copy SRC DST... | trash PATH... | mkdir PATH | touch PATH | rename FROM TO\n");
		return 2;
	}
	const char *op = argv[1];
	int failed = 0;
	if (str_eq(op, "move") || str_eq(op, "copy")) {
		for (int i = 2; i + 1 < argc; i += 2) {
			const char *src = argv[i];
			if (files_inside(argv[i + 1], src)) {
				eprint("zfiles: %s can't go inside itself\n", src);
				failed++;
				continue;
			}
			char *dst = free_name(argv[i + 1]);
			bool ok = str_eq(op, "move") ? files_move(src, dst) : files_copy_tree(src, dst);
			if (!ok) {
				eprint("zfiles: can't %s %s to %s\n", op, src, dst);
				failed++;
			}
			mem_free(dst);
		}
	} else if (str_eq(op, "trash")) {
		for (int i = 2; i < argc; i++)
			if (!files_trash(argv[i])) {
				eprint("zfiles: can't move %s to the Trash\n", argv[i]);
				failed++;
			}
	} else if (str_eq(op, "mkdir")) {
		char *p = free_name(argv[2]);
		failed = sys_mkdir(p, 0755) < 0;
		mem_free(p);
	} else if (str_eq(op, "touch")) {
		char *p = free_name(argv[2]);
		failed = !file_write(p, "", 0);
		mem_free(p);
	} else if (str_eq(op, "rename") && argc >= 4) {
		struct stat st;
		if (!str_eq(argv[2], argv[3]) && sys_lstat(argv[3], &st) == 0) {
			eprint("zfiles: %s already exists\n", argv[3]);
			failed = 1;
		} else {
			failed = sys_rename(argv[2], argv[3]) < 0;
		}
	} else {
		eprint("zfiles: unknown operation %s\n", op);
		return 2;
	}
	return MIN(failed, 100);
}
