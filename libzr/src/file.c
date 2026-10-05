/* Files, directories, paths, line reading and configuration files. */
#include "zr.h"

bool write_all(int fd, const void *data, usize len)
{
	const u8 *p = data;
	while (len) {
		long n = sys_write(fd, p, len);
		if (n == -EINTR || n == -EAGAIN)
			continue;
		if (n <= 0)
			return false;
		p += n;
		len -= (usize)n;
	}
	return true;
}

char *file_read(const char *path, usize *len_out)
{
	int fd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return NULL;
	struct stat st;
	usize cap = (sys_fstat(fd, &st) == 0 && st.st_size > 0) ? (usize)st.st_size + 1 : 4096;
	char *buf = mem_alloc(cap);
	usize len = 0;
	for (;;) {
		if (len + 1 >= cap) {
			cap *= 2;
			buf = mem_realloc(buf, cap);
		}
		long n = sys_read(fd, buf + len, cap - len - 1);
		if (n == -EINTR)
			continue;
		if (n < 0) {
			mem_free(buf);
			sys_close(fd);
			return NULL;
		}
		if (n == 0)
			break;
		len += (usize)n;
	}
	sys_close(fd);
	buf[len] = 0;
	if (len_out)
		*len_out = len;
	return buf;
}

static bool write_file(const char *path, const void *data, usize len, int flags)
{
	int fd = (int)sys_open(path, O_WRONLY | O_CREAT | O_CLOEXEC | flags, 0644);
	if (fd < 0)
		return false;
	bool ok = write_all(fd, data, len);
	return sys_close(fd) == 0 && ok;
}

bool file_write(const char *path, const void *data, usize len)
{
	return write_file(path, data, len, O_TRUNC);
}

bool file_append(const char *path, const void *data, usize len)
{
	return write_file(path, data, len, O_APPEND);
}

bool file_write_atomic(const char *path, const void *data, usize len)
{
	char *tmp = str_fmt("%s.tmp%ld", path, sys_getpid());
	int fd = (int)sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	bool ok = fd >= 0 && write_all(fd, data, len) && sys_fsync(fd) == 0;
	if (fd >= 0)
		ok = sys_close(fd) == 0 && ok;
	ok = ok && sys_rename(tmp, path) == 0;
	if (!ok)
		sys_unlink(tmp);
	mem_free(tmp);
	return ok;
}

bool file_write_str(const char *path, const char *s)
{
	return file_write(path, s, strlen(s));
}

char *file_read_line(const char *path)
{
	char *s = file_read(path, NULL);
	if (!s)
		return NULL;
	char *nl = (char *)str_chr(s, '\n');
	if (nl)
		*nl = 0;
	char *t = str_trim(s);
	char *r = str_dup(t);
	mem_free(s);
	return r;
}

i64 file_read_int(const char *path, i64 fallback)
{
	char buf[64];
	int fd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return fallback;
	long n = sys_read(fd, buf, sizeof buf - 1);
	sys_close(fd);
	if (n <= 0)
		return fallback;
	buf[n] = 0;
	const char *end;
	i64 v = str_to_int(buf, &end);
	return end == buf ? fallback : v;
}

bool file_exists(const char *path)
{
	struct stat st;
	return sys_stat(path, &st) == 0;
}

bool dir_exists(const char *path)
{
	struct stat st;
	return sys_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

i64 file_size(const char *path)
{
	struct stat st;
	return sys_stat(path, &st) == 0 ? st.st_size : -1;
}

i64 file_mtime(const char *path)
{
	struct stat st;
	return sys_stat(path, &st) == 0 ? st.st_mtime : 0;
}

bool file_copy(const char *from, const char *to)
{
	int in = (int)sys_open(from, O_RDONLY | O_CLOEXEC, 0);
	if (in < 0)
		return false;
	struct stat st;
	sys_fstat(in, &st);
	int out = (int)sys_open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (int)(st.st_mode & 0777));
	if (out < 0) {
		sys_close(in);
		return false;
	}
	bool ok = true;
	for (;;) {
		long n = sys_copy_file_range(in, out, 1 << 30);
		if (n == 0)
			break;
		if (n < 0) {
			/* Not supported across these filesystems: fall back to read/write. */
			static char buf[65536];
			for (;;) {
				long r = sys_read(in, buf, sizeof buf);
				if (r <= 0) {
					ok = r == 0;
					break;
				}
				if (!write_all(out, buf, (usize)r)) {
					ok = false;
					break;
				}
			}
			break;
		}
	}
	sys_close(in);
	return sys_close(out) == 0 && ok;
}

bool dir_make(const char *path, int mode)
{
	if (dir_exists(path))
		return true;
	char *p = str_dup(path);
	for (char *s = p + 1; *s; s++) {
		if (*s == '/') {
			*s = 0;
			sys_mkdir(p, mode);
			*s = '/';
		}
	}
	long r = sys_mkdir(p, mode);
	mem_free(p);
	return r == 0 || r == -EEXIST;
}

bool path_remove_all(const char *path)
{
	struct stat st;
	if (sys_lstat(path, &st) != 0)
		return true;
	if (S_ISDIR(st.st_mode)) {
		DirList list = { 0 };
		if (dir_list(path, &list, false)) {
			vec_each(&list, e)
			{
				char *child = path_join(path, e->name);
				path_remove_all(child);
				mem_free(child);
			}
			dir_list_free(&list);
		}
		return sys_rmdir(path) == 0;
	}
	return sys_unlink(path) == 0;
}

/* ---- Directory listing ------------------------------------------------------- */

static int dir_entry_cmp(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	const DirEntry *x = a, *y = b;
	bool dx = x->type == DT_DIR, dy = y->type == DT_DIR;
	if (dx != dy)
		return dx ? -1 : 1;
	return str_cmp_natural(x->name, y->name);
}

bool dir_list(const char *path, DirList *out, bool with_stat)
{
	int fd = (int)sys_open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (fd < 0)
		return false;
	static char buf[16384];
	for (;;) {
		long n = sys_getdents64(fd, buf, sizeof buf);
		if (n <= 0)
			break;
		for (long off = 0; off < n;) {
			struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
			off += d->d_reclen;
			if (str_eq(d->d_name, ".") || str_eq(d->d_name, ".."))
				continue;
			DirEntry e = { .name = str_dup(d->d_name), .type = d->d_type };
			if (with_stat || e.type == DT_UNKNOWN || e.type == DT_LNK) {
				char *full = path_join(path, d->d_name);
				struct stat st;
				if (sys_stat(full, &st) == 0) {
					e.size = st.st_size;
					e.mtime = st.st_mtime;
					e.mode = st.st_mode;
					if (S_ISDIR(st.st_mode))
						e.type = DT_DIR;
					else if (S_ISREG(st.st_mode))
						e.type = DT_REG;
				}
				mem_free(full);
			}
			vec_push(out, e);
		}
	}
	sys_close(fd);
	sort(out->data, out->len, sizeof(DirEntry), dir_entry_cmp, NULL);
	return true;
}

void dir_list_free(DirList *list)
{
	vec_each(list, e) mem_free(e->name);
	vec_free(list);
}

/* ---- Paths ---------------------------------------------------------------------- */

char *path_join(const char *a, const char *b)
{
	if (!a || !*a || b[0] == '/')
		return str_dup(b);
	usize n = strlen(a);
	return str_fmt(a[n - 1] == '/' ? "%s%s" : "%s/%s", a, b);
}

const char *path_base(const char *path)
{
	usize n = strlen(path);
	while (n > 1 && path[n - 1] == '/')
		n--;
	const char *p = path + n;
	while (p > path && p[-1] != '/')
		p--;
	return p;
}

char *path_dir(const char *path)
{
	const char *slash = str_rchr(path, '/');
	if (!slash)
		return str_dup(".");
	if (slash == path)
		return str_dup("/");
	return str_ndup(path, (usize)(slash - path));
}

const char *path_ext(const char *path)
{
	const char *base = path_base(path);
	const char *dot = str_rchr(base, '.');
	return (dot && dot != base) ? dot + 1 : "";
}

char *path_normalize(const char *path)
{
	usize count;
	char **parts = str_split(path, '/', &count);
	VEC(char *) out = { 0 };
	for (usize i = 0; i < count; i++) {
		if (!parts[i][0] || str_eq(parts[i], "."))
			continue;
		if (str_eq(parts[i], "..")) {
			if (out.len)
				out.len--;
			continue;
		}
		vec_push(&out, parts[i]);
	}
	StrBuf sb = { 0 };
	if (path[0] == '/' || !out.len)
		sb_addc(&sb, path[0] == '/' ? '/' : '.');
	for (usize i = 0; i < out.len; i++) {
		if (i)
			sb_addc(&sb, '/');
		sb_adds(&sb, out.data[i]);
	}
	vec_free(&out);
	str_split_free(parts, count);
	return sb_take(&sb);
}

const char *home_dir(void)
{
	const char *h = env_get("HOME");
	return h && *h ? h : "/data/home";
}

/* ---- Line reader ------------------------------------------------------------------ */

void line_reader_init(LineReader *r, int fd)
{
	memset(r, 0, sizeof *r);
	r->fd = fd;
}

const char *line_reader_next(LineReader *r)
{
	sb_clear(&r->line);
	for (;;) {
		for (usize i = r->start; i < r->end; i++) {
			if (r->buf[i] == '\n') {
				sb_add(&r->line, r->buf + r->start, i - r->start);
				r->start = i + 1;
				if (r->line.len && r->line.data[r->line.len - 1] == '\r')
					r->line.data[--r->line.len] = 0;
				return sb_str(&r->line);
			}
		}
		sb_add(&r->line, r->buf + r->start, r->end - r->start);
		r->start = r->end = 0;
		if (r->eof)
			return r->line.len ? sb_str(&r->line) : NULL;
		long n = sys_read(r->fd, r->buf, sizeof r->buf);
		if (n == -EINTR)
			continue;
		if (n <= 0) {
			r->eof = true;
			if (!r->line.len)
				return NULL;
			return sb_str(&r->line);
		}
		r->end = (usize)n;
	}
}

void line_reader_free(LineReader *r)
{
	sb_free(&r->line);
}

/* ---- Configuration files ------------------------------------------------------------ */

Conf *conf_load(const char *path)
{
	Conf *c = NEW(Conf);
	c->path = str_dup(path);
	char *text = file_read(path, NULL);
	if (!text)
		return c;
	usize n;
	char **lines = str_split(text, '\n', &n);
	for (usize i = 0; i < n; i++) {
		char *line = str_trim(lines[i]);
		if (!*line || *line == '#')
			continue;
		char *eq = (char *)str_chr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		vec_push(&c->keys, str_dup(str_trim(line)));
		vec_push(&c->values, str_dup(str_trim(eq + 1)));
	}
	str_split_free(lines, n);
	mem_free(text);
	return c;
}

static isize conf_index(const Conf *c, const char *key)
{
	for (usize i = 0; i < c->keys.len; i++)
		if (str_eq(c->keys.data[i], key))
			return (isize)i;
	return -1;
}

const char *conf_get(const Conf *c, const char *key, const char *fallback)
{
	isize i = conf_index(c, key);
	return i >= 0 ? c->values.data[i] : fallback;
}

i64 conf_get_int(const Conf *c, const char *key, i64 fallback)
{
	const char *v = conf_get(c, key, NULL);
	return v ? str_to_int(v, NULL) : fallback;
}

f64 conf_get_float(const Conf *c, const char *key, f64 fallback)
{
	const char *v = conf_get(c, key, NULL);
	return v ? str_to_float(v, NULL) : fallback;
}

bool conf_get_bool(const Conf *c, const char *key, bool fallback)
{
	const char *v = conf_get(c, key, NULL);
	if (!v)
		return fallback;
	return str_eq(v, "true") || str_eq(v, "yes") || str_eq(v, "1") || str_eq(v, "on");
}

void conf_set(Conf *c, const char *key, const char *value)
{
	isize i = conf_index(c, key);
	if (i >= 0) {
		mem_free(c->values.data[i]);
		c->values.data[i] = str_dup(value);
		return;
	}
	vec_push(&c->keys, str_dup(key));
	vec_push(&c->values, str_dup(value));
}

void conf_set_int(Conf *c, const char *key, i64 value)
{
	char buf[32];
	fmt(buf, sizeof buf, "%lld", (long long)value);
	conf_set(c, key, buf);
}

void conf_set_bool(Conf *c, const char *key, bool value)
{
	conf_set(c, key, value ? "true" : "false");
}

void conf_unset(Conf *c, const char *key)
{
	isize i = conf_index(c, key);
	if (i < 0)
		return;
	mem_free(c->keys.data[i]);
	mem_free(c->values.data[i]);
	vec_remove(&c->keys, (usize)i);
	vec_remove(&c->values, (usize)i);
}

bool conf_save(const Conf *c)
{
	StrBuf sb = { 0 };
	for (usize i = 0; i < c->keys.len; i++)
		sb_fmt(&sb, "%s = %s\n", c->keys.data[i], c->values.data[i]);
	char *dir = path_dir(c->path);
	dir_make(dir, 0755);
	mem_free(dir);
	bool ok = file_write_atomic(c->path, sb_str(&sb), sb.len);
	sb_free(&sb);
	return ok;
}

void conf_free(Conf *c)
{
	if (!c)
		return;
	for (usize i = 0; i < c->keys.len; i++) {
		mem_free(c->keys.data[i]);
		mem_free(c->values.data[i]);
	}
	vec_free(&c->keys);
	vec_free(&c->values);
	mem_free(c->path);
	mem_free(c);
}
