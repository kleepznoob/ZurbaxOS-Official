/* Flint's built-in commands. */
#include "flint.h"
#include "zui/proto.h"
#include "zui.h"

int job_foreground(int index);
int job_background(int index);
void job_list(void);

/* ---- Helpers ------------------------------------------------------------------------- */

static bool has_flag(const char *flags, char f)
{
	return str_chr(flags, f) != NULL;
}

/* Collects single-letter flags ("-la" -> "la") and returns the index of the first operand. */
static int parse_flags(int argc, char **argv, char *flags, usize cap)
{
	usize n = 0;
	int i = 1;
	for (; i < argc; i++) {
		if (str_eq(argv[i], "--")) {
			i++;
			break;
		}
		if (argv[i][0] != '-' || !argv[i][1])
			break;
		for (const char *p = argv[i] + 1; *p && n < cap - 1; p++)
			flags[n++] = *p;
	}
	flags[n] = 0;
	return i;
}

static void human(u64 bytes, char *out_s, usize cap)
{
	fmt_bytes(out_s, cap, bytes);
}

static bool color(void)
{
	static int c = -1;
	if (c < 0)
		c = out_is_tty();
	return c;
}

static const char *paint(const char *code)
{
	return color() ? code : "";
}

/* Reads all of a file or standard input. */
static char *slurp(const char *path, usize *len)
{
	if (path && !str_eq(path, "-"))
		return file_read(path, len);
	StrBuf sb = { 0 };
	char buf[8192];
	for (;;) {
		long n = sys_read(0, buf, sizeof buf);
		if (n == -EINTR)
			continue;
		if (n <= 0)
			break;
		sb_add(&sb, buf, (usize)n);
	}
	if (len)
		*len = sb.len;
	return sb.data ? sb_take(&sb) : str_dup("");
}

/* ---- Shell commands -------------------------------------------------------------------- */

static int cmd_cd(int argc, char **argv)
{
	static char prev[1024];
	const char *dest = argc > 1 ? argv[1] : home_dir();
	char cwd[1024] = "";
	sys_getcwd(cwd, sizeof cwd);
	if (str_eq(dest, "-")) {
		if (!prev[0]) {
			errf("cd: no previous folder\n");
			return 1;
		}
		dest = prev;
		outf("%s\n", dest);
	}
	char target[1024];
	str_copy(target, dest, sizeof target);
	long r = sys_chdir(target);
	if (r < 0) {
		errf("cd: %s: %s\n", dest, r == -ENOENT ? "no such folder" : r == -ENOTDIR ? "not a folder" : err_str(r));
		return 1;
	}
	str_copy(prev, cwd, sizeof prev);
	char now[1024];
	if (sys_getcwd(now, sizeof now) >= 0)
		env_set("PWD", now);
	return 0;
}

static int cmd_pwd(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	char cwd[1024];
	if (sys_getcwd(cwd, sizeof cwd) < 0)
		return 1;
	outf("%s\n", cwd);
	return 0;
}

static int cmd_echo(int argc, char **argv)
{
	bool newline = true;
	int i = 1;
	if (i < argc && str_eq(argv[i], "-n")) {
		newline = false;
		i++;
	}
	for (; i < argc; i++) {
		out(argv[i]);
		if (i + 1 < argc)
			out(" ");
	}
	if (newline)
		out("\n");
	return 0;
}

static int cmd_export(int argc, char **argv)
{
	if (argc == 1) {
		for (char **e = environ; *e; e++)
			outf("%s\n", *e);
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		char *eq = (char *)str_chr(argv[i], '=');
		if (!eq) {
			continue;
		}
		*eq = 0;
		env_set(argv[i], eq + 1);
		*eq = '=';
	}
	return 0;
}

static int cmd_unset(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		usize n = strlen(argv[i]);
		char **w = environ;
		for (char **e = environ; *e; e++)
			if (!(memcmp(*e, argv[i], n) == 0 && (*e)[n] == '='))
				*w++ = *e;
		*w = NULL;
	}
	return 0;
}

static int cmd_env(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	for (char **e = environ; *e; e++)
		outf("%s\n", *e);
	return 0;
}

static int cmd_exit(int argc, char **argv)
{
	request_exit(argc > 1 ? (int)str_to_int(argv[1], NULL) : last_status);
	return 0;
}

static int cmd_alias(int argc, char **argv)
{
	if (argc == 1) {
		vec_each(&aliases, a) outf("alias %s='%s'\n", a->name, a->value);
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		char *eq = (char *)str_chr(argv[i], '=');
		if (!eq) {
			const char *v = alias_get(argv[i]);
			if (v)
				outf("alias %s='%s'\n", argv[i], v);
			continue;
		}
		*eq = 0;
		alias_set(argv[i], eq + 1);
	}
	return 0;
}

static int cmd_unalias(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
		alias_unset(argv[i]);
	return 0;
}

static int cmd_source(int argc, char **argv)
{
	if (argc < 2) {
		errf("source: which file?\n");
		return 2;
	}
	int st = source_file(argv[1]);
	return st;
}

static int cmd_history(int argc, char **argv)
{
	if (argc > 1 && str_eq(argv[1], "-c")) {
		history.len = 0;
		return 0;
	}
	usize first = history.len > 50 && !(argc > 1 && str_eq(argv[1], "-a")) ? history.len - 50 : 0;
	for (usize i = first; i < history.len; i++)
		outf("%s%5zu%s  %s\n", paint(C_GRAY), i + 1, paint(C_RESET), history.data[i]);
	return 0;
}

static int cmd_fg(int argc, char **argv)
{
	return job_foreground(argc > 1 ? (int)str_to_int(argv[1] + (argv[1][0] == '%'), NULL) : 0);
}

static int cmd_bg(int argc, char **argv)
{
	return job_background(argc > 1 ? (int)str_to_int(argv[1] + (argv[1][0] == '%'), NULL) : 0);
}

static int cmd_jobs(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	job_list();
	return 0;
}

static int cmd_clear(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	out("\x1b[H\x1b[2J\x1b[3J");
	return 0;
}

static int cmd_true(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	return 0;
}

static int cmd_false(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	return 1;
}

static int cmd_type(int argc, char **argv)
{
	int st = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = alias_get(argv[i]);
		const char *p;
		if (a)
			outf("%s is an alias for '%s'\n", argv[i], a);
		else if (find_command(argv[i]))
			outf("%s is built into Flint\n", argv[i]);
		else if ((p = find_in_path(argv[i])))
			outf("%s is %s\n", argv[i], p);
		else {
			errf("%s: not found\n", argv[i]);
			st = 1;
		}
	}
	return st;
}

static int cmd_which(int argc, char **argv)
{
	int st = 0;
	for (int i = 1; i < argc; i++) {
		const char *p = find_in_path(argv[i]);
		if (p)
			outf("%s\n", p);
		else if (find_command(argv[i]))
			outf("%s: built into Flint\n", argv[i]);
		else
			st = 1;
	}
	return st;
}

static int cmd_sleep(int argc, char **argv)
{
	if (argc < 2)
		return 1;
	f64 s = str_to_float(argv[1], NULL);
	sleep_ms((u32)(s * 1000));
	return 0;
}

/* ---- Files ---------------------------------------------------------------------------- */

typedef struct {
	char *name;
	struct stat st;
	bool ok;
} LsEntry;

static const char *ls_color(const LsEntry *e)
{
	if (!color())
		return "";
	if (S_ISDIR(e->st.st_mode))
		return C_BBLUE;
	if (S_ISLNK(e->st.st_mode))
		return C_BCYAN;
	if (e->st.st_mode & 0111)
		return C_BGREEN;
	const char *ext = path_ext(e->name);
	if (str_eq_nocase(ext, "png") || str_eq_nocase(ext, "jpg") || str_eq_nocase(ext, "jpeg") || str_eq_nocase(ext, "bmp"))
		return C_MAGENTA;
	if (str_eq_nocase(ext, "mp3") || str_eq_nocase(ext, "flac") || str_eq_nocase(ext, "wav") || str_eq_nocase(ext, "ogg"))
		return C_CYAN;
	if (str_eq_nocase(ext, "zip") || str_eq_nocase(ext, "tar") || str_eq_nocase(ext, "gz") || str_eq_nocase(ext, "xz"))
		return C_RED;
	return "";
}

static int cmp_ls(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	return str_cmp_natural(((const LsEntry *)a)->name, ((const LsEntry *)b)->name);
}

static void ls_long(LsEntry *e, int n)
{
	for (int i = 0; i < n; i++) {
		char mode[11] = "----------";
		u32 m = e[i].st.st_mode;
		mode[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-';
		static const char rwx[] = "rwxrwxrwx";
		for (int k = 0; k < 9; k++)
			if (m & (0400u >> k))
				mode[k + 1] = rwx[k];
		char size[32];
		if (S_ISDIR(m))
			str_copy(size, "-", sizeof size);
		else
			human((u64)e[i].st.st_size, size, sizeof size);
		Civil c;
		civil_local(e[i].st.st_mtime, &c);
		static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
		outf("%s%s%s %9s  %s%2d %s %02d:%02d%s  %s%s%s\n", paint(C_GRAY), mode, paint(C_RESET), size, paint(C_GRAY), c.day,
		     mon[c.month - 1], c.hour, c.min, paint(C_RESET), ls_color(&e[i]), e[i].name, paint(C_RESET));
	}
}

static void ls_columns(LsEntry *e, int n)
{
	if (!color()) {
		for (int i = 0; i < n; i++)
			outf("%s\n", e[i].name);
		return;
	}
	struct winsize ws;
	int cols = sys_ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col ? ws.ws_col : 80;
	usize maxw = 1;
	for (int i = 0; i < n; i++)
		maxw = MAX(maxw, utf8_count(e[i].name, strlen(e[i].name)));
	int per_row = MAX(1, cols / (int)(maxw + 2));
	int rows = (n + per_row - 1) / per_row;
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < per_row; c++) {
			int i = c * rows + r;
			if (i >= n)
				break;
			usize w = utf8_count(e[i].name, strlen(e[i].name));
			outf("%s%s%s", ls_color(&e[i]), e[i].name, paint(C_RESET));
			if (c + 1 < per_row && (c + 1) * rows + r < n)
				for (usize k = w; k < maxw + 2; k++)
					out(" ");
		}
		out("\n");
	}
}

static int list_one(const char *path, const char *flags, bool header)
{
	struct stat st;
	if (sys_stat(path, &st) < 0) {
		errf("ls: %s: no such file or folder\n", path);
		return 1;
	}
	if (!S_ISDIR(st.st_mode)) {
		LsEntry e = { (char *)path, st, true };
		if (has_flag(flags, 'l'))
			ls_long(&e, 1);
		else
			outf("%s%s%s\n", ls_color(&e), path, paint(C_RESET));
		return 0;
	}
	DirList list = { 0 };
	if (!dir_list(path, &list, false)) {
		errf("ls: %s: can't open\n", path);
		return 1;
	}
	LsEntry *e = NEW_ARRAY(LsEntry, list.len + 1);
	int n = 0;
	vec_each(&list, d) {
		if (d->name[0] == '.' && !has_flag(flags, 'a'))
			continue;
		e[n].name = d->name;
		char *full = path_join(path, d->name);
		e[n].ok = sys_lstat(full, &e[n].st) == 0;
		mem_free(full);
		n++;
	}
	sort(e, (usize)n, sizeof(LsEntry), cmp_ls, NULL);
	if (header)
		outf("%s%s:%s\n", paint(C_BOLD), path, paint(C_RESET));
	if (has_flag(flags, 'l') || has_flag(flags, '1')) {
		if (has_flag(flags, 'l'))
			ls_long(e, n);
		else
			for (int i = 0; i < n; i++)
				outf("%s%s%s\n", ls_color(&e[i]), e[i].name, paint(C_RESET));
	} else {
		ls_columns(e, n);
	}
	mem_free(e);
	dir_list_free(&list);
	return 0;
}

static int cmd_ls(int argc, char **argv)
{
	char flags[32];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	if (i >= argc)
		return list_one(".", flags, false);
	int st = 0;
	for (int k = i; k < argc; k++) {
		st |= list_one(argv[k], flags, argc - i > 1);
		if (k + 1 < argc && argc - i > 1)
			out("\n");
	}
	return st;
}

static int cmd_cat(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	bool number = has_flag(flags, 'n');
	int st = 0;
	int line = 1;
	for (int k = i; k < argc || (k == i && i == argc); k++) {
		usize len;
		char *d = slurp(k < argc ? argv[k] : NULL, &len);
		if (!d) {
			errf("cat: %s: can't read\n", argv[k]);
			st = 1;
			continue;
		}
		if (!number) {
			outn(d, len);
		} else {
			bool bol = true;
			for (usize j = 0; j < len; j++) {
				if (bol)
					outf("%s%6d%s  ", paint(C_GRAY), line++, paint(C_RESET));
				outn(d + j, 1);
				bol = d[j] == '\n';
			}
		}
		mem_free(d);
		if (i == argc)
			break;
	}
	return st;
}

static bool copy_any(const char *from, const char *to, bool recursive)
{
	struct stat st;
	if (sys_stat(from, &st) < 0)
		return false;
	if (S_ISDIR(st.st_mode)) {
		if (!recursive) {
			errf("cp: %s is a folder (use cp -r)\n", from);
			return false;
		}
		dir_make(to, 0755);
		DirList l = { 0 };
		if (!dir_list(from, &l, false))
			return false;
		bool ok = true;
		vec_each(&l, e) {
			char *a = path_join(from, e->name), *b = path_join(to, e->name);
			ok = copy_any(a, b, true) && ok;
			mem_free(a);
			mem_free(b);
		}
		dir_list_free(&l);
		return ok;
	}
	return file_copy(from, to);
}

/* Destination for cp/mv: inside a folder, or the given name. */
static char *dest_for(const char *src, const char *dst)
{
	if (dir_exists(dst))
		return path_join(dst, path_base(src));
	return str_dup(dst);
}

static int cmd_cp(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	if (argc - i < 2) {
		errf("usage: cp [-r] SOURCE... DESTINATION\n");
		return 2;
	}
	int st = 0;
	for (int k = i; k < argc - 1; k++) {
		char *d = dest_for(argv[k], argv[argc - 1]);
		if (!copy_any(argv[k], d, has_flag(flags, 'r') || has_flag(flags, 'R'))) {
			errf("cp: couldn't copy %s\n", argv[k]);
			st = 1;
		}
		mem_free(d);
	}
	return st;
}

static int cmd_mv(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	if (argc - i < 2) {
		errf("usage: mv SOURCE... DESTINATION\n");
		return 2;
	}
	int st = 0;
	for (int k = i; k < argc - 1; k++) {
		char *d = dest_for(argv[k], argv[argc - 1]);
		long r = sys_rename(argv[k], d);
		if (r == -EXDEV) {
			r = copy_any(argv[k], d, true) && path_remove_all(argv[k]) ? 0 : -1;
		}
		if (r < 0) {
			errf("mv: couldn't move %s\n", argv[k]);
			st = 1;
		}
		mem_free(d);
	}
	return st;
}

static int cmd_rm(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	bool rec = has_flag(flags, 'r') || has_flag(flags, 'R'), force = has_flag(flags, 'f');
	int st = 0;
	for (int k = i; k < argc; k++) {
		struct stat s;
		if (sys_lstat(argv[k], &s) < 0) {
			if (!force) {
				errf("rm: %s: no such file\n", argv[k]);
				st = 1;
			}
			continue;
		}
		if (str_eq(argv[k], "/") || str_eq(argv[k], home_dir())) {
			errf("rm: refusing to remove %s\n", argv[k]);
			st = 1;
			continue;
		}
		if (S_ISDIR(s.st_mode)) {
			if (!rec) {
				errf("rm: %s is a folder (use rm -r)\n", argv[k]);
				st = 1;
				continue;
			}
			if (!path_remove_all(argv[k])) {
				errf("rm: couldn't remove %s\n", argv[k]);
				st = 1;
			}
		} else if (sys_unlink(argv[k]) < 0) {
			errf("rm: couldn't remove %s\n", argv[k]);
			st = 1;
		}
	}
	return st;
}

static int cmd_mkdir(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	int st = 0;
	for (int k = i; k < argc; k++) {
		long r = has_flag(flags, 'p') ? (dir_make(argv[k], 0755) ? 0 : -1) : sys_mkdir(argv[k], 0755);
		if (r < 0) {
			errf("mkdir: couldn't create %s%s\n", argv[k], r == -EEXIST ? " (it already exists)" : "");
			st = 1;
		}
	}
	return st;
}

static int cmd_rmdir(int argc, char **argv)
{
	int st = 0;
	for (int k = 1; k < argc; k++)
		if (sys_rmdir(argv[k]) < 0) {
			errf("rmdir: couldn't remove %s (is it empty?)\n", argv[k]);
			st = 1;
		}
	return st;
}

static int cmd_touch(int argc, char **argv)
{
	int st = 0;
	for (int k = 1; k < argc; k++) {
		int fd = (int)sys_open(argv[k], O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd < 0) {
			errf("touch: %s: %s\n", argv[k], err_str(fd));
			st = 1;
			continue;
		}
		sys_close(fd);
		sys_utimensat(AT_FDCWD, argv[k], NULL, 0);
	}
	return st;
}

static int head_tail(int argc, char **argv, bool tail)
{
	int lines = 10, i = 1;
	if (i < argc && argv[i][0] == '-' && argv[i][1] == 'n' && i + 1 < argc) {
		lines = (int)str_to_int(argv[i + 1], NULL);
		i += 2;
	} else if (i < argc && argv[i][0] == '-' && is_digit((u8)argv[i][1])) {
		lines = (int)str_to_int(argv[i] + 1, NULL);
		i++;
	}
	usize len;
	char *d = slurp(i < argc ? argv[i] : NULL, &len);
	if (!d) {
		errf("%s: can't read %s\n", tail ? "tail" : "head", argv[i]);
		return 1;
	}
	if (!tail) {
		usize p = 0;
		for (int n = 0; p < len && n < lines; p++)
			if (d[p] == '\n')
				n++;
		outn(d, p);
	} else {
		usize p = len;
		if (p && d[p - 1] == '\n')
			p--;
		for (int n = 0; p > 0; p--)
			if (d[p - 1] == '\n' && ++n == lines)
				break;
		outn(d + p, len - p);
	}
	mem_free(d);
	return 0;
}

static int cmd_head(int argc, char **argv)
{
	return head_tail(argc, argv, false);
}

static int cmd_tail(int argc, char **argv)
{
	return head_tail(argc, argv, true);
}

static int cmd_wc(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	usize len;
	char *d = slurp(i < argc ? argv[i] : NULL, &len);
	if (!d)
		return 1;
	usize lines = 0, words = 0, chars = 0;
	bool in = false;
	for (usize k = 0; k < len; k++) {
		u8 c = (u8)d[k];
		if (c == '\n')
			lines++;
		if ((c & 0xC0) != 0x80)
			chars++;
		bool sp = c == ' ' || c == '\n' || c == '\t' || c == '\r';
		if (!sp && !in)
			words++;
		in = !sp;
	}
	mem_free(d);
	if (has_flag(flags, 'l'))
		outf("%zu\n", lines);
	else if (has_flag(flags, 'w'))
		outf("%zu\n", words);
	else if (has_flag(flags, 'c') || has_flag(flags, 'm'))
		outf("%zu\n", chars);
	else
		outf("%7zu lines %7zu words %7zu characters\n", lines, words, chars);
	return 0;
}

static bool line_matches(const char *line, usize n, const char *pat, bool icase, usize *at)
{
	usize pl = strlen(pat);
	for (usize i = 0; i + pl <= n; i++) {
		bool ok = icase ? str_eq_nocase_n(line + i, pat, pl) : memcmp(line + i, pat, pl) == 0;
		if (ok) {
			if (at)
				*at = i;
			return true;
		}
	}
	return false;
}

static int grep_text(const char *name, const char *d, usize len, const char *pat, const char *flags, bool show_name)
{
	bool icase = has_flag(flags, 'i'), invert = has_flag(flags, 'v'), num = has_flag(flags, 'n'), count = has_flag(flags, 'c');
	int found = 0, lineno = 0;
	for (usize p = 0; p < len;) {
		const char *l = d + p;
		const char *e = memchr(l, '\n', len - p);
		usize n = e ? (usize)(e - l) : len - p;
		lineno++;
		usize at = 0;
		bool m = line_matches(l, n, pat, icase, &at);
		if (m != invert) {
			found++;
			if (!count) {
				if (show_name)
					outf("%s%s%s:", paint(C_MAGENTA), name, paint(C_RESET));
				if (num)
					outf("%s%d%s:", paint(C_GREEN), lineno, paint(C_RESET));
				if (m && !invert && color()) {
					/* Highlight every match on the line. */
					usize q = 0;
					usize pl = strlen(pat);
					while (q < n) {
						usize rel;
						if (line_matches(l + q, n - q, pat, icase, &rel)) {
							outn(l + q, rel);
							out("\x1b[1;31m");
							outn(l + q + rel, pl);
							out(C_RESET);
							q += rel + pl;
						} else {
							outn(l + q, n - q);
							break;
						}
					}
					out("\n");
				} else {
					outn(l, n);
					out("\n");
				}
			}
		}
		p += n + 1;
	}
	if (count) {
		if (show_name)
			outf("%s:", name);
		outf("%d\n", found);
	}
	return found;
}

static int grep_tree(const char *path, const char *pat, const char *flags)
{
	struct stat st;
	if (sys_stat(path, &st) < 0)
		return 0;
	if (S_ISDIR(st.st_mode)) {
		DirList l = { 0 };
		int found = 0;
		if (dir_list(path, &l, false)) {
			vec_each(&l, e) {
				if (e->name[0] == '.')
					continue;
				char *p = path_join(path, e->name);
				found += grep_tree(p, pat, flags);
				mem_free(p);
			}
			dir_list_free(&l);
		}
		return found;
	}
	if (st.st_size > 16 * 1024 * 1024)
		return 0;
	usize len;
	char *d = file_read(path, &len);
	if (!d)
		return 0;
	/* Skip binary files. */
	int found = memchr(d, 0, MIN(len, 8000)) ? 0 : grep_text(path, d, len, pat, flags, true);
	mem_free(d);
	return found;
}

static int cmd_grep(int argc, char **argv)
{
	char flags[16];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	if (i >= argc) {
		errf("usage: grep [-i -n -v -c -r] TEXT [FILE...]\n");
		return 2;
	}
	const char *pat = argv[i++];
	int found = 0;
	if (i >= argc) {
		if (has_flag(flags, 'r'))
			found = grep_tree(".", pat, flags);
		else {
			usize len;
			char *d = slurp(NULL, &len);
			found = grep_text("-", d, len, pat, flags, false);
			mem_free(d);
		}
	} else {
		for (int k = i; k < argc; k++) {
			if (has_flag(flags, 'r')) {
				found += grep_tree(argv[k], pat, flags);
				continue;
			}
			usize len;
			char *d = file_read(argv[k], &len);
			if (!d) {
				errf("grep: %s: can't read\n", argv[k]);
				continue;
			}
			found += grep_text(argv[k], d, len, pat, flags, argc - i > 1);
			mem_free(d);
		}
	}
	return found ? 0 : 1;
}

static bool glob_match(const char *pat, const char *s)
{
	while (*pat) {
		if (*pat == '*') {
			while (*pat == '*')
				pat++;
			if (!*pat)
				return true;
			for (; *s; s++)
				if (glob_match(pat, s))
					return true;
			return false;
		}
		if (!*s)
			return false;
		char a = *pat, b = *s;
		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (b >= 'A' && b <= 'Z')
			b += 32;
		if (*pat != '?' && a != b)
			return false;
		pat++, s++;
	}
	return !*s;
}

static void find_walk(const char *path, const char *name, char type, int depth)
{
	DirList l = { 0 };
	if (depth > 32 || !dir_list(path, &l, false))
		return;
	vec_each(&l, e) {
		char *p = str_eq(path, ".") ? str_fmt("./%s", e->name) : path_join(path, e->name);
		bool is_dir = e->type == DT_DIR;
		bool ok = (!name || glob_match(name, e->name)) && (!type || (type == 'd') == is_dir);
		if (ok)
			outf("%s%s%s\n", is_dir ? paint(C_BBLUE) : "", p, is_dir ? paint(C_RESET) : "");
		if (is_dir)
			find_walk(p, name, type, depth + 1);
		mem_free(p);
	}
	dir_list_free(&l);
}

static int cmd_find(int argc, char **argv)
{
	const char *dir = ".", *name = NULL;
	char type = 0;
	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "-name") && i + 1 < argc)
			name = argv[++i];
		else if (str_eq(argv[i], "-type") && i + 1 < argc)
			type = argv[++i][0];
		else
			dir = argv[i];
	}
	find_walk(dir, name, type, 0);
	return 0;
}

static void tree_walk(const char *path, const char *prefix, int depth, int *dirs, int *files, bool all)
{
	DirList l = { 0 };
	if (depth > 12 || !dir_list(path, &l, false))
		return;
	int n = 0;
	for (usize i = 0; i < l.len; i++)
		n += all || l.data[i].name[0] != '.';
	int k = 0;
	vec_each(&l, e) {
		if (!all && e->name[0] == '.')
			continue;
		bool last = ++k == n;
		bool is_dir = e->type == DT_DIR;
		outf("%s%s%s%s%s\n", prefix, last ? "└── " : "├── ", is_dir ? paint(C_BBLUE) : "", e->name, is_dir ? paint(C_RESET) : "");
		if (is_dir) {
			(*dirs)++;
			char *p = path_join(path, e->name);
			char *np = str_fmt("%s%s", prefix, last ? "    " : "│   ");
			tree_walk(p, np, depth + 1, dirs, files, all);
			mem_free(p);
			mem_free(np);
		} else {
			(*files)++;
		}
	}
	dir_list_free(&l);
}

static int cmd_tree(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	const char *dir = i < argc ? argv[i] : ".";
	int dirs = 0, files = 0;
	outf("%s%s%s\n", paint(C_BBLUE), dir, paint(C_RESET));
	tree_walk(dir, "", 0, &dirs, &files, has_flag(flags, 'a'));
	outf("\n%d folder%s, %d file%s\n", dirs, dirs == 1 ? "" : "s", files, files == 1 ? "" : "s");
	return 0;
}

static u64 du_walk(const char *path)
{
	struct stat st;
	if (sys_lstat(path, &st) < 0)
		return 0;
	if (!S_ISDIR(st.st_mode))
		return (u64)st.st_size;
	u64 total = 0;
	DirList l = { 0 };
	if (dir_list(path, &l, false)) {
		vec_each(&l, e) {
			char *p = path_join(path, e->name);
			total += du_walk(p);
			mem_free(p);
		}
		dir_list_free(&l);
	}
	return total;
}

static int cmd_du(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	if (i >= argc && !has_flag(flags, 's')) {
		/* Sizes of what's in the current folder. */
		DirList l = { 0 };
		if (dir_list(".", &l, false)) {
			vec_each(&l, e) {
				if (e->name[0] == '.')
					continue;
				char b[32];
				human(du_walk(e->name), b, sizeof b);
				outf("%10s  %s%s%s\n", b, e->type == DT_DIR ? paint(C_BBLUE) : "", e->name, paint(C_RESET));
			}
			dir_list_free(&l);
		}
		return 0;
	}
	for (int k = i; k < argc || k == i; k++) {
		const char *p = k < argc ? argv[k] : ".";
		char b[32];
		human(du_walk(p), b, sizeof b);
		outf("%10s  %s\n", b, p);
		if (k >= argc)
			break;
	}
	return 0;
}

static int cmd_df(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	outf("%s%-14s %10s %10s %10s %5s%s\n", paint(C_BOLD), "Where", "Size", "Used", "Free", "Use", paint(C_RESET));
	const char *places[] = { "/", "/data", "/tmp" };
	const char *names[] = { "System", "Your files", "Temporary" };
	for (int i = 0; i < 3; i++) {
		struct statfs st;
		if (sys_statfs(places[i], &st) < 0 || !st.f_blocks)
			continue;
		u64 total = st.f_blocks * (u64)st.f_bsize, avail = st.f_bavail * (u64)st.f_bsize;
		u64 used = total - st.f_bfree * (u64)st.f_bsize;
		char a[32], b[32], c[32];
		human(total, a, sizeof a);
		human(used, b, sizeof b);
		human(avail, c, sizeof c);
		outf("%-14s %10s %10s %10s %4d%%\n", names[i], a, b, c, (int)(used * 100 / (total ? total : 1)));
	}
	return 0;
}

static u64 meminfo(const char *text, const char *key)
{
	const char *p = str_find(text, key);
	return p ? (u64)str_to_int(p + strlen(key), NULL) * 1024 : 0;
}

static int cmd_free(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	char *m = file_read("/proc/meminfo", NULL);
	if (!m)
		return 1;
	u64 total = meminfo(m, "MemTotal:"), avail = meminfo(m, "MemAvailable:"), swap = meminfo(m, "SwapTotal:"),
	    swapfree = meminfo(m, "SwapFree:");
	mem_free(m);
	char a[32], b[32], c[32];
	human(total, a, sizeof a);
	human(total - avail, b, sizeof b);
	human(avail, c, sizeof c);
	outf("%s%-8s %10s %10s %10s%s\n", paint(C_BOLD), "", "Total", "Used", "Available", paint(C_RESET));
	outf("%-8s %10s %10s %10s\n", "Memory", a, b, c);
	if (swap) {
		human(swap, a, sizeof a);
		human(swap - swapfree, b, sizeof b);
		human(swapfree, c, sizeof c);
		outf("%-8s %10s %10s %10s\n", "Swap", a, b, c);
	}
	return 0;
}

static void format_uptime(i64 s, char *buf, usize cap)
{
	if (s >= 86400)
		fmt(buf, cap, "%lld days, %lld h %lld min", (long long)(s / 86400), (long long)(s % 86400 / 3600), (long long)(s % 3600 / 60));
	else if (s >= 3600)
		fmt(buf, cap, "%lld h %lld min", (long long)(s / 3600), (long long)(s % 3600 / 60));
	else
		fmt(buf, cap, "%lld min", (long long)(s / 60));
}

static int cmd_uptime(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	struct sysinfo si;
	if (sys_sysinfo(&si) < 0)
		return 1;
	char b[64];
	format_uptime(si.uptime, b, sizeof b);
	outf("Up %s · load %.2f %.2f %.2f\n", b, (double)si.loads[0] / 65536.0, (double)si.loads[1] / 65536.0,
	     (double)si.loads[2] / 65536.0);
	return 0;
}

static int cmd_date(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	Civil c;
	civil_local(time_unix(), &c);
	i32 off = tz_offset(time_unix());
	outf("%s %d %s %d, %02d:%02d:%02d (UTC%+d%s) %s\n", WEEKDAY_NAMES[c.wday], c.day, MONTH_NAMES[c.month - 1], c.year, c.hour,
	     c.min, c.sec, off / 3600, off % 3600 ? ":30" : "", tz_name());
	return 0;
}

static int cmd_cal(int argc, char **argv)
{
	Civil now;
	civil_local(time_unix(), &now);
	int year = now.year, month = now.month;
	if (argc == 3) {
		month = (int)str_to_int(argv[1], NULL);
		year = (int)str_to_int(argv[2], NULL);
	} else if (argc == 2) {
		month = (int)str_to_int(argv[1], NULL);
	}
	if (month < 1 || month > 12)
		return 2;
	char title[48];
	fmt(title, sizeof title, "%s %d", MONTH_NAMES[month - 1], year);
	int pad_l = (20 - (int)strlen(title)) / 2;
	outf("%*s%s%s%s\n", pad_l, "", paint(C_BOLD), title, paint(C_RESET));
	outf("%sMo Tu We Th Fr %sSa Su%s\n", paint(C_GRAY), paint(C_DIM), paint(C_RESET));
	Civil first = { .year = year, .month = month, .day = 1 };
	i64 d = unix_from_civil(&first) / 86400;
	int wd = (int)((d % 7 + 11) % 7);
	int col = (wd + 6) % 7;
	for (int i = 0; i < col; i++)
		out("   ");
	for (int day = 1; day <= days_in_month(year, month); day++) {
		bool today = day == now.day && month == now.month && year == now.year;
		if (today)
			outf("%s%2d%s", color() ? "\x1b[7m" : "", day, paint(C_RESET));
		else
			outf("%2d", day);
		col++;
		if (col == 7) {
			out("\n");
			col = 0;
		} else {
			out(" ");
		}
	}
	if (col)
		out("\n");
	return 0;
}

static int cmd_whoami(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	const char *u = sys_geteuid() == 0 ? "root" : env_get("USER");
	outf("%s\n", u ? u : "user");
	return 0;
}

static int cmd_id(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	outf("uid=%ld gid=%ld\n", sys_geteuid(), (long)syscall0(SYS_getegid));
	return 0;
}

static int cmd_hostname(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	struct utsname u;
	sys_uname(&u);
	outf("%s\n", u.nodename);
	return 0;
}

static int cmd_uname(int argc, char **argv)
{
	struct utsname u;
	sys_uname(&u);
	if (argc > 1 && str_eq(argv[1], "-a"))
		outf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
	else if (argc > 1 && str_eq(argv[1], "-r"))
		outf("%s\n", u.release);
	else
		outf("%s\n", u.sysname);
	return 0;
}

static int cmd_ps(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	DirList l = { 0 };
	if (!dir_list("/proc", &l, false))
		return 1;
	outf("%s%7s %-8s %s%s\n", paint(C_BOLD), "PID", "USER", "COMMAND", paint(C_RESET));
	vec_each(&l, e) {
		if (!is_digit((u8)e->name[0]))
			continue;
		char path[64];
		fmt(path, sizeof path, "/proc/%s/cmdline", e->name);
		usize len;
		char *cmd = file_read(path, &len);
		if (!cmd || !len) {
			mem_free(cmd);
			continue; /* kernel thread */
		}
		for (usize i = 0; i + 1 < len; i++)
			if (!cmd[i])
				cmd[i] = ' ';
		fmt(path, sizeof path, "/proc/%s/status", e->name);
		char *st = file_read(path, NULL);
		u32 uid = 0;
		const char *u = st ? str_find(st, "Uid:") : NULL;
		if (u)
			uid = (u32)str_to_int(u + 4, NULL);
		mem_free(st);
		outf("%7s %-8s %s\n", e->name, uid == 0 ? "system" : env_get("USER") ? env_get("USER") : "user", cmd);
		mem_free(cmd);
	}
	dir_list_free(&l);
	return 0;
}

static int cmd_kill(int argc, char **argv)
{
	int sig = SIGTERM, i = 1;
	if (i < argc && argv[i][0] == '-') {
		const char *s = argv[i] + 1;
		sig = str_eq(s, "9") || str_eq(s, "KILL") ? SIGKILL : str_eq(s, "STOP") ? SIGSTOP : str_eq(s, "CONT") ? SIGCONT :
		      (int)str_to_int(s, NULL);
		i++;
	}
	int st = 0;
	for (; i < argc; i++) {
		long r = sys_kill((int)str_to_int(argv[i], NULL), sig);
		if (r < 0) {
			errf("kill: %s: %s\n", argv[i], err_str(r));
			st = 1;
		}
	}
	return st;
}

static int cmp_lines(const void *a, const void *b, void *ctx)
{
	bool rev = ctx != NULL;
	int c = str_cmp_natural(*(char *const *)a, *(char *const *)b);
	return rev ? -c : c;
}

static int cmd_sort(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	usize len;
	char *d = slurp(i < argc ? argv[i] : NULL, &len);
	if (!d)
		return 1;
	usize n;
	char **lines = str_split(d, '\n', &n);
	if (n && !lines[n - 1][0])
		n--;
	sort(lines, n, sizeof(char *), cmp_lines, has_flag(flags, 'r') ? (void *)1 : NULL);
	for (usize k = 0; k < n; k++)
		outf("%s\n", lines[k]);
	str_split_free(lines, n);
	mem_free(d);
	return 0;
}

static int cmd_uniq(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	usize len;
	char *d = slurp(i < argc ? argv[i] : NULL, &len);
	if (!d)
		return 1;
	usize n;
	char **lines = str_split(d, '\n', &n);
	if (n && !lines[n - 1][0])
		n--;
	for (usize k = 0; k < n;) {
		usize j = k + 1;
		while (j < n && str_eq(lines[j], lines[k]))
			j++;
		if (has_flag(flags, 'c'))
			outf("%7zu %s\n", j - k, lines[k]);
		else
			outf("%s\n", lines[k]);
		k = j;
	}
	str_split_free(lines, n);
	mem_free(d);
	return 0;
}

static int cmd_basename(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	outf("%s\n", path_base(argv[1]));
	return 0;
}

static int cmd_dirname(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	char *d = path_dir(argv[1]);
	outf("%s\n", d);
	mem_free(d);
	return 0;
}

static int cmd_realpath(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		char cwd[1024] = "/";
		sys_getcwd(cwd, sizeof cwd);
		char *j = argv[i][0] == '/' ? str_dup(argv[i]) : path_join(cwd, argv[i]);
		char *n = path_normalize(j);
		outf("%s\n", n);
		mem_free(j);
		mem_free(n);
	}
	return 0;
}

static int cmd_stat(int argc, char **argv)
{
	int st = 0;
	for (int i = 1; i < argc; i++) {
		struct stat s;
		if (sys_lstat(argv[i], &s) < 0) {
			errf("stat: %s: no such file\n", argv[i]);
			st = 1;
			continue;
		}
		char sz[32];
		human((u64)s.st_size, sz, sizeof sz);
		Civil c;
		civil_local(s.st_mtime, &c);
		outf("%s%s%s\n  Type: %s\n  Size: %s (%lld bytes)\n  Permissions: %o\n  Owner: %u\n  Modified: %d %s %d %02d:%02d\n",
		     paint(C_BOLD), argv[i], paint(C_RESET), S_ISDIR(s.st_mode) ? "folder" : S_ISLNK(s.st_mode) ? "link" : "file", sz,
		     (long long)s.st_size, s.st_mode & 07777, s.st_uid, c.day, MONTH_NAMES[c.month - 1], c.year, c.hour, c.min);
	}
	return st;
}

static int cmd_ln(int argc, char **argv)
{
	char flags[8];
	int i = parse_flags(argc, argv, flags, sizeof flags);
	if (argc - i < 2) {
		errf("usage: ln -s TARGET NAME\n");
		return 2;
	}
	long r = has_flag(flags, 's') ? sys_symlink(argv[i], argv[i + 1]) : syscall5(SYS_linkat, AT_FDCWD, (long)argv[i], AT_FDCWD, (long)argv[i + 1], 0);
	if (r < 0) {
		errf("ln: %s\n", err_str(r));
		return 1;
	}
	return 0;
}

static int cmd_chmod(int argc, char **argv)
{
	if (argc < 3) {
		errf("usage: chmod MODE FILE...  (e.g. chmod 755 script, chmod +x script)\n");
		return 2;
	}
	int st = 0;
	for (int i = 2; i < argc; i++) {
		struct stat s;
		if (sys_stat(argv[i], &s) < 0) {
			errf("chmod: %s: no such file\n", argv[i]);
			st = 1;
			continue;
		}
		u32 mode = s.st_mode & 07777;
		if (str_eq(argv[1], "+x"))
			mode |= 0111;
		else if (str_eq(argv[1], "-x"))
			mode &= ~0111u;
		else {
			mode = 0;
			for (const char *p = argv[1]; *p >= '0' && *p <= '7'; p++)
				mode = mode * 8 + (u32)(*p - '0');
		}
		if (sys_chmod(argv[i], (int)mode) < 0) {
			errf("chmod: can't change %s\n", argv[i]);
			st = 1;
		}
	}
	return st;
}

static int cmd_seq(int argc, char **argv)
{
	i64 a = 1, b = 1, step = 1;
	if (argc == 2)
		b = str_to_int(argv[1], NULL);
	else if (argc == 3)
		a = str_to_int(argv[1], NULL), b = str_to_int(argv[2], NULL);
	else if (argc >= 4)
		a = str_to_int(argv[1], NULL), step = str_to_int(argv[2], NULL), b = str_to_int(argv[3], NULL);
	if (step == 0)
		return 2;
	for (i64 i = a; step > 0 ? i <= b : i >= b; i += step)
		outf("%lld\n", (long long)i);
	return 0;
}

static int cmd_yes(int argc, char **argv)
{
	const char *s = argc > 1 ? argv[1] : "y";
	/* Ends when whatever reads it goes away (the write fails with SIGPIPE). */
	for (;;)
		outf("%s\n", s);
	return 0;
}

/* ---- Zurbax commands -------------------------------------------------------------------- */

static int desktop_connect(void)
{
	const char *path = env_get("ZUI_SOCKET");
	int fd = unix_connect(path && *path ? path : ZUI_SOCKET_PATH, SOCK_SEQPACKET);
	if (fd < 0)
		return -1;
	MsgHello h = { .h = { MSG_HELLO, sizeof h, 0 }, .version = ZUI_PROTOCOL_VERSION, .pid = (u32)sys_getpid() };
	str_copy(h.app_id, "forge", sizeof h.app_id);
	unix_send(fd, &h, sizeof h, NULL, 0);
	return fd;
}

bool desktop_launch(const char *app, const char *arg)
{
	int fd = desktop_connect();
	if (fd < 0)
		return false;
	MsgLaunch m = { .h = { MSG_LAUNCH, sizeof m, 0 } };
	str_copy(m.app_id, app ? app : "", sizeof m.app_id);
	str_copy(m.arg, arg ? arg : "", sizeof m.arg);
	unix_send(fd, &m, sizeof m, NULL, 0);
	sys_close(fd);
	return true;
}

bool desktop_notify(const char *title, const char *body)
{
	int fd = desktop_connect();
	if (fd < 0)
		return false;
	MsgNotify m = { .h = { MSG_NOTIFY, sizeof m, 0 } };
	str_copy(m.icon, "terminal", sizeof m.icon);
	str_copy(m.title, title, sizeof m.title);
	str_copy(m.body, body ? body : "", sizeof m.body);
	unix_send(fd, &m, sizeof m, NULL, 0);
	sys_close(fd);
	return true;
}

bool desktop_settings_changed(void)
{
	int fd = desktop_connect();
	if (fd < 0)
		return false;
	MsgHeader m = { MSG_SETTINGS_CHANGED, sizeof m, 0 };
	unix_send(fd, &m, sizeof m, NULL, 0);
	sys_close(fd);
	return true;
}

static int cmd_open(int argc, char **argv)
{
	if (argc < 2) {
		errf("usage: open FILE|FOLDER|URL   (opens it with the right app)\n");
		return 2;
	}
	int st = 0;
	for (int i = 1; i < argc; i++) {
		char cwd[1024] = "/", *p;
		sys_getcwd(cwd, sizeof cwd);
		if (argv[i][0] == '/' || str_find(argv[i], "://"))
			p = str_dup(argv[i]);
		else
			p = path_join(cwd, argv[i]);
		if (!desktop_launch("", p)) {
			errf("open: the desktop isn't running\n");
			st = 1;
		}
		mem_free(p);
	}
	return st;
}

static int cmd_notify(int argc, char **argv)
{
	if (argc < 2) {
		errf("usage: notify TITLE [MESSAGE]\n");
		return 2;
	}
	return desktop_notify(argv[1], argc > 2 ? argv[2] : "") ? 0 : 1;
}

static int cmd_theme(int argc, char **argv)
{
	char *path = str_fmt("%s/.config/zurbax/settings.conf", home_dir());
	Conf *c = conf_load(path);
	mem_free(path);
	if (argc < 2) {
		outf("Desktop: %s · Style: %s · Accent: %s · Wallpaper: %s\n", look_hive(c) ? "Hive" : "Classic", look_dark(c) ? "dark" : "light",
		     look_accent(c), look_wallpaper(c));
		out("Change it with: theme dark | theme light | theme accent NAME | theme wallpaper NAME\n");
		conf_free(c);
		return 0;
	}
	if (str_eq(argv[1], "dark") || str_eq(argv[1], "light"))
		conf_set_bool(c, "dark", str_eq(argv[1], "dark"));
	else if (str_eq(argv[1], "accent") && argc > 2)
		conf_set(c, "accent", argv[2]);
	else if (str_eq(argv[1], "wallpaper") && argc > 2)
		conf_set(c, "wallpaper", argv[2]);
	else {
		errf("theme: unknown option %s\n", argv[1]);
		conf_free(c);
		return 2;
	}
	conf_save(c);
	conf_free(c);
	desktop_settings_changed();
	return 0;
}

/* A small calculator: + - * / ^ % and parentheses. */
typedef struct {
	const char *p;
	bool err;
} Calc;

static f64 calc_expr(Calc *c);

static void calc_ws(Calc *c)
{
	while (*c->p == ' ')
		c->p++;
}

static f64 calc_atom(Calc *c)
{
	calc_ws(c);
	if (*c->p == '(') {
		c->p++;
		f64 v = calc_expr(c);
		calc_ws(c);
		if (*c->p == ')')
			c->p++;
		return v;
	}
	if (*c->p == '-') {
		c->p++;
		return -calc_atom(c);
	}
	if (str_starts(c->p, "pi")) {
		c->p += 2;
		return PI;
	}
	if (str_starts(c->p, "sqrt")) {
		c->p += 4;
		return sqrt(calc_atom(c));
	}
	const char *end;
	f64 v = str_to_float(c->p, &end);
	if (end == c->p)
		c->err = true;
	c->p = end;
	return v;
}

static f64 calc_pow(Calc *c)
{
	f64 b = calc_atom(c);
	calc_ws(c);
	if (*c->p == '^') {
		c->p++;
		return pow(b, calc_pow(c));
	}
	return b;
}

static f64 calc_term(Calc *c)
{
	f64 v = calc_pow(c);
	for (;;) {
		calc_ws(c);
		if (*c->p == '*' || *c->p == 'x')
			c->p++, v *= calc_pow(c);
		else if (*c->p == '/') {
			c->p++;
			f64 d = calc_pow(c);
			if (d == 0)
				c->err = true;
			v /= d;
		} else if (*c->p == '%')
			c->p++, v = fmod(v, calc_pow(c));
		else
			return v;
	}
}

static f64 calc_expr(Calc *c)
{
	f64 v = calc_term(c);
	for (;;) {
		calc_ws(c);
		if (*c->p == '+')
			c->p++, v += calc_term(c);
		else if (*c->p == '-')
			c->p++, v -= calc_term(c);
		else
			return v;
	}
}

static int cmd_calc(int argc, char **argv)
{
	StrBuf e = { 0 };
	for (int i = 1; i < argc; i++) {
		sb_adds(&e, argv[i]);
		sb_addc(&e, ' ');
	}
	if (!e.len) {
		errf("usage: calc EXPRESSION   (e.g. calc \"(2+3)*4^2\")\n");
		return 2;
	}
	Calc c = { sb_str(&e), false };
	f64 v = calc_expr(&c);
	calc_ws(&c);
	if (c.err || *c.p) {
		errf("calc: I couldn't work that out\n");
		sb_free(&e);
		return 1;
	}
	outf("%s%.12g%s\n", paint(C_BOLD), v, paint(C_RESET));
	sb_free(&e);
	return 0;
}

static int cmd_colors(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	for (int row = 0; row < 2; row++) {
		for (int i = 0; i < 8; i++)
			outf("\x1b[%dm  %2d  " C_RESET, (row ? 100 : 40) + i, row * 8 + i);
		out("\n");
	}
	out("\n");
	for (int i = 16; i < 232; i++) {
		outf("\x1b[48;5;%dm  " C_RESET, i);
		if ((i - 15) % 36 == 0)
			out("\n");
	}
	for (int i = 232; i < 256; i++)
		outf("\x1b[48;5;%dm  " C_RESET, i);
	out("\n");
	return 0;
}

static int cmd_sysinfo(int argc, char **argv)
{
	UNUSED(argc), UNUSED(argv);
	/* The Zurbax mark. */
	static const char *const logo[] = {
		"▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄",
		"████████████████",
		"▀▀▀▀▀▀▀▀▀▀▀█████",
		"         ▄████▀ ",
		"       ▄████▀   ",
		"     ▄████▀     ",
		"   ▄████▀       ",
		" ▄████▀         ",
		"█████▄▄▄▄▄▄▄▄▄▄▄",
		"████████████████",
		"▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀",
		"                ",
	};
	char lines[12][160];
	int n = 0;
	struct utsname u;
	sys_uname(&u);
	struct sysinfo si;
	sys_sysinfo(&si);
	const char *user = sys_geteuid() == 0 ? "root" : env_get("USER") ? env_get("USER") : "user";
	fmt(lines[n++], 160, "%s%s%s@%s%s%s", paint(C_BGREEN), user, paint(C_RESET), paint(C_BGREEN), u.nodename, paint(C_RESET));
	fmt(lines[n++], 160, "%s", "──────────────────");
	fmt(lines[n++], 160, "%sSystem%s    " OS_PRETTY_NAME, paint(C_BCYAN), paint(C_RESET));
	fmt(lines[n++], 160, "%sKernel%s    Linux %s", paint(C_BCYAN), paint(C_RESET), u.release);
	char up[64];
	format_uptime(si.uptime, up, sizeof up);
	fmt(lines[n++], 160, "%sUptime%s    %s", paint(C_BCYAN), paint(C_RESET), up);
	fmt(lines[n++], 160, "%sShell%s     Flint", paint(C_BCYAN), paint(C_RESET));
	char cpu[96] = "Unknown";
	char *ci = file_read("/proc/cpuinfo", NULL);
	if (ci) {
		const char *m = str_find(ci, "model name");
		if (m && (m = str_chr(m, ':'))) {
			m += 2;
			const char *e = str_chr(m, '\n');
			fmt(cpu, sizeof cpu, "%.*s", (int)(e ? e - m : (long)strlen(m)), m);
		}
		mem_free(ci);
	}
	fmt(lines[n++], 160, "%sProcessor%s %s", paint(C_BCYAN), paint(C_RESET), cpu);
	char *m = file_read("/proc/meminfo", NULL);
	u64 total = m ? meminfo(m, "MemTotal:") : 0, avail = m ? meminfo(m, "MemAvailable:") : 0;
	mem_free(m);
	char a[32], b[32];
	human(total - avail, a, sizeof a);
	human(total, b, sizeof b);
	fmt(lines[n++], 160, "%sMemory%s    %s / %s", paint(C_BCYAN), paint(C_RESET), a, b);
	struct statfs sf;
	if (sys_statfs("/data", &sf) == 0 || sys_statfs("/", &sf) == 0) {
		human((sf.f_blocks - sf.f_bfree) * (u64)sf.f_bsize, a, sizeof a);
		human(sf.f_blocks * (u64)sf.f_bsize, b, sizeof b);
		fmt(lines[n++], 160, "%sStorage%s   %s / %s", paint(C_BCYAN), paint(C_RESET), a, b);
	}
	lines[n++][0] = 0;
	char swatch[160] = "";
	usize sl = 0;
	for (int i = 0; i < 8; i++)
		sl += (usize)fmt(swatch + sl, sizeof swatch - sl, "\x1b[4%dm   ", i);
	fmt(swatch + sl, sizeof swatch - sl, C_RESET);
	str_copy(lines[n++], color() ? swatch : "", 160);
	out("\n");
	/* Teal to violet, like the Zurbax icon. */
	static const char *const tint[] = { "\x1b[38;2;54;209;196m", "\x1b[38;2;56;190;210m", "\x1b[38;2;58;170;225m",
					    "\x1b[38;2;58;150;235m", "\x1b[38;2;58;130;240m", "\x1b[38;2;70;115;238m",
					    "\x1b[38;2;84;100;232m", "\x1b[38;2;95;85;225m", "\x1b[38;2;104;70;220m",
					    "\x1b[38;2;109;60;216m", "\x1b[38;2;109;53;216m", "" };
	for (int i = 0; i < 12; i++)
		outf("  %s%s%s   %s\n", color() ? tint[i] : "", logo[i], paint(C_RESET), i < n ? lines[i] : "");
	out("\n");
	return 0;
}

static int cmd_help(int argc, char **argv)
{
	if (argc > 1) {
		const Command *c = find_command(argv[1]);
		if (!c) {
			errf("help: no command called %s\n", argv[1]);
			return 1;
		}
		outf("%s%s%s — %s\n", paint(C_BOLD), c->usage, paint(C_RESET), c->help);
		return 0;
	}
	outf("\n  %sFlint%s, the Zurbax shell. Built-in commands:\n\n", paint(C_BOLD), paint(C_RESET));
	for (int i = 0; i < COMMAND_COUNT; i++)
		outf("  %s%-10s%s %s\n", paint(C_BGREEN), COMMANDS[i].name, paint(C_RESET), COMMANDS[i].help);
	outf("\n  Combine commands with %s|%s, %s&&%s and %s||%s; save output with %s>%s; run in the background with %s&%s.\n",
	     paint(C_BOLD), paint(C_RESET), paint(C_BOLD), paint(C_RESET), paint(C_BOLD), paint(C_RESET), paint(C_BOLD), paint(C_RESET),
	     paint(C_BOLD), paint(C_RESET));
	outf("  Press %sTab%s to complete names, %s↑%s for history, and %s→%s to accept a grey suggestion.\n", paint(C_BOLD),
	     paint(C_RESET), paint(C_BOLD), paint(C_RESET), paint(C_BOLD), paint(C_RESET));
	outf("  Use %ssudo%s to run a command with full rights (it asks for your password).\n\n", paint(C_BOLD), paint(C_RESET));
	return 0;
}

/* ---- Table -------------------------------------------------------------------------------- */

const Command COMMANDS[] = {
	{ "help", cmd_help, "help [COMMAND]", "Lists commands, or explains one", false },
	{ "cd", cmd_cd, "cd [FOLDER|-]", "Changes the current folder", true },
	{ "pwd", cmd_pwd, "pwd", "Shows the current folder", false },
	{ "ls", cmd_ls, "ls [-l -a -1] [PATH...]", "Lists files", false },
	{ "cat", cmd_cat, "cat [-n] [FILE...]", "Shows the contents of files", false },
	{ "cp", cmd_cp, "cp [-r] SOURCE... DEST", "Copies files and folders", false },
	{ "mv", cmd_mv, "mv SOURCE... DEST", "Moves or renames", false },
	{ "rm", cmd_rm, "rm [-r -f] PATH...", "Deletes files (no Trash!)", false },
	{ "mkdir", cmd_mkdir, "mkdir [-p] FOLDER...", "Creates folders", false },
	{ "rmdir", cmd_rmdir, "rmdir FOLDER...", "Removes empty folders", false },
	{ "touch", cmd_touch, "touch FILE...", "Creates a file or updates its date", false },
	{ "ln", cmd_ln, "ln -s TARGET NAME", "Creates a link", false },
	{ "chmod", cmd_chmod, "chmod MODE FILE...", "Changes permissions", false },
	{ "stat", cmd_stat, "stat PATH...", "Shows details about files", false },
	{ "tree", cmd_tree, "tree [-a] [FOLDER]", "Shows folders as a tree", false },
	{ "find", cmd_find, "find [FOLDER] [-name PATTERN] [-type f|d]", "Finds files by name", false },
	{ "grep", cmd_grep, "grep [-i -n -v -c -r] TEXT [FILE...]", "Finds lines containing text", false },
	{ "head", cmd_head, "head [-n N] [FILE]", "Shows the first lines", false },
	{ "tail", cmd_tail, "tail [-n N] [FILE]", "Shows the last lines", false },
	{ "wc", cmd_wc, "wc [-l -w -c] [FILE]", "Counts lines, words and characters", false },
	{ "sort", cmd_sort, "sort [-r] [FILE]", "Sorts lines", false },
	{ "uniq", cmd_uniq, "uniq [-c] [FILE]", "Removes repeated lines", false },
	{ "du", cmd_du, "du [-s] [PATH...]", "Shows how much space things use", false },
	{ "df", cmd_df, "df", "Shows free disk space", false },
	{ "free", cmd_free, "free", "Shows memory use", false },
	{ "ps", cmd_ps, "ps", "Lists running programs", false },
	{ "kill", cmd_kill, "kill [-9] PID...", "Stops a program", false },
	{ "jobs", cmd_jobs, "jobs", "Lists background jobs", true },
	{ "fg", cmd_fg, "fg [N]", "Brings a job to the foreground", true },
	{ "bg", cmd_bg, "bg [N]", "Resumes a stopped job in the background", true },
	{ "uptime", cmd_uptime, "uptime", "Shows how long the computer has been on", false },
	{ "date", cmd_date, "date", "Shows the date and time", false },
	{ "cal", cmd_cal, "cal [MONTH [YEAR]]", "Shows a calendar", false },
	{ "whoami", cmd_whoami, "whoami", "Shows your user name", false },
	{ "id", cmd_id, "id", "Shows your user and group numbers", false },
	{ "hostname", cmd_hostname, "hostname", "Shows the computer's name", false },
	{ "uname", cmd_uname, "uname [-a -r]", "Shows system information", false },
	{ "sysinfo", cmd_sysinfo, "sysinfo", "A summary of this computer", false },
	{ "echo", cmd_echo, "echo [-n] TEXT...", "Prints text", false },
	{ "env", cmd_env, "env", "Lists environment variables", false },
	{ "export", cmd_export, "export NAME=VALUE", "Sets an environment variable", true },
	{ "unset", cmd_unset, "unset NAME", "Removes an environment variable", true },
	{ "alias", cmd_alias, "alias [NAME=COMMAND]", "Creates a shortcut for a command", true },
	{ "unalias", cmd_unalias, "unalias NAME", "Removes a shortcut", true },
	{ "history", cmd_history, "history [-a] [-c]", "Shows recent commands", true },
	{ "source", cmd_source, "source FILE", "Runs commands from a file", true },
	{ "type", cmd_type, "type NAME", "Tells what a command is", false },
	{ "which", cmd_which, "which NAME", "Shows where a program is", false },
	{ "basename", cmd_basename, "basename PATH", "The last part of a path", false },
	{ "dirname", cmd_dirname, "dirname PATH", "The folder part of a path", false },
	{ "realpath", cmd_realpath, "realpath PATH", "The full path", false },
	{ "seq", cmd_seq, "seq [FIRST [STEP]] LAST", "Prints a sequence of numbers", false },
	{ "sleep", cmd_sleep, "sleep SECONDS", "Waits", false },
	{ "calc", cmd_calc, "calc EXPRESSION", "Does arithmetic", false },
	{ "open", cmd_open, "open FILE|FOLDER|URL", "Opens with the right app", false },
	{ "notify", cmd_notify, "notify TITLE [MESSAGE]", "Shows a desktop notification", false },
	{ "theme", cmd_theme, "theme [dark|light|accent NAME|wallpaper NAME]", "Changes the desktop's look", false },
	{ "colors", cmd_colors, "colors", "Shows the terminal's colours", false },
	{ "clear", cmd_clear, "clear", "Clears the screen", false },
	{ "true", cmd_true, "true", "Does nothing, successfully", false },
	{ "false", cmd_false, "false", "Does nothing, unsuccessfully", false },
	{ "yes", cmd_yes, "yes [TEXT]", "Prints text forever", false },
	{ "exit", cmd_exit, "exit [STATUS]", "Closes the shell", true },
};

const int COMMAND_COUNT = (int)ARRAY_LEN(COMMANDS);

const Command *find_command(const char *name)
{
	if (str_eq(name, "."))
		name = "source";
	for (int i = 0; i < COMMAND_COUNT; i++)
		if (str_eq(COMMANDS[i].name, name))
			return &COMMANDS[i];
	return NULL;
}
