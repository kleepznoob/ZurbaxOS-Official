/*
 * unarchive — unpacks an archive of any common kind for Vault: zip, 7z, rar,
 * tar (plain, .gz, .xz, .zst, .lzma, .lz4), cab, iso, cpio, ar, xar, lha,
 * and single compressed files (.gz, .xz, .zst, .lzma, .Z).
 *
 * It runs in the Linux layer (layerrun --isolated) and reads through the
 * layer's libarchive, opened when it starts, so nothing about it is fixed
 * at build time but the C library.
 *
 *   unarchive [-p] ARCHIVE FOLDER
 *
 * Unpacks into a new folder in FOLDER named after the archive, or, when
 * everything in the archive is one folder or one file, puts that in FOLDER
 * itself (a free name either way). With -p, the password is the first line
 * of the standard input.
 *
 * On the standard output: "progress DONE TOTAL" lines (bytes of the archive
 * read), then "done PATH" with what was made. On the standard error: what
 * went wrong, in a sentence. Exit status: 0, 1 when it failed, 3 when the
 * archive needs a password (or the one given is wrong).
 *
 * Nothing lands outside the new folder: absolute names, ".." and links that
 * point out are refused, as libarchive's secure extraction options do.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct archive;
struct archive_entry;

#define ARCHIVE_EOF 1
#define ARCHIVE_OK 0
#define ARCHIVE_WARN (-20)
#define ARCHIVE_FATAL (-30)

#define EXTRACT_PERM 0x0002
#define EXTRACT_TIME 0x0004
#define EXTRACT_SECURE_SYMLINKS 0x0100
#define EXTRACT_SECURE_NODOTDOT 0x0200
#define EXTRACT_SECURE_NOABSOLUTEPATHS 0x10000

static struct {
	struct archive *(*read_new)(void);
	int (*support_filter_all)(struct archive *);
	int (*support_format_all)(struct archive *);
	int (*support_format_raw)(struct archive *);
	int (*support_format_empty)(struct archive *);
	int (*add_passphrase)(struct archive *, const char *);
	int (*open_filename)(struct archive *, const char *, size_t);
	int (*next_header)(struct archive *, struct archive_entry **);
	int (*data_block)(struct archive *, const void **, size_t *, long long *);
	int (*read_free)(struct archive *);
	int (*filter_count)(struct archive *);
	long long (*filter_bytes)(struct archive *, int);
	const char *(*error_string)(struct archive *);
	int (*has_encrypted)(struct archive *);
	struct archive *(*disk_new)(void);
	int (*disk_set_options)(struct archive *, int);
	int (*write_header)(struct archive *, struct archive_entry *);
	long (*write_data_block)(struct archive *, const void *, size_t, long long);
	int (*finish_entry)(struct archive *);
	int (*write_free)(struct archive *);
	const char *(*entry_pathname)(struct archive_entry *);
	void (*entry_set_pathname)(struct archive_entry *, const char *);
	const char *(*entry_hardlink)(struct archive_entry *);
	void (*entry_set_hardlink)(struct archive_entry *, const char *);
	int (*entry_is_encrypted)(struct archive_entry *);
} A;

static char *work;      /* the hidden folder being unpacked into */
static bool finished;
static volatile sig_atomic_t stopped; /* Vault's Stop button */

static void on_stop(int sig)
{
	(void)sig;
	stopped = 1;
}

static void remove_tree(const char *path);

/* a failure leaves nothing half unpacked behind */
static void clean_up(void)
{
	if (work && !finished && chdir("/") == 0)
		remove_tree(work);
}

static void fail(int status, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	vfprintf(stderr, f, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(status);
}

static void load(void)
{
	void *h = dlopen("libarchive.so.13", RTLD_NOW);
	if (!h)
		fail(1, "The Linux layer has no libarchive, which unpacking needs.");
	static const struct { void *slot; const char *name; } syms[] = {
		{ &A.read_new, "archive_read_new" },
		{ &A.support_filter_all, "archive_read_support_filter_all" },
		{ &A.support_format_all, "archive_read_support_format_all" },
		{ &A.support_format_raw, "archive_read_support_format_raw" },
		{ &A.support_format_empty, "archive_read_support_format_empty" },
		{ &A.add_passphrase, "archive_read_add_passphrase" },
		{ &A.open_filename, "archive_read_open_filename" },
		{ &A.next_header, "archive_read_next_header" },
		{ &A.data_block, "archive_read_data_block" },
		{ &A.read_free, "archive_read_free" },
		{ &A.filter_count, "archive_filter_count" },
		{ &A.filter_bytes, "archive_filter_bytes" },
		{ &A.error_string, "archive_error_string" },
		{ &A.has_encrypted, "archive_read_has_encrypted_entries" },
		{ &A.disk_new, "archive_write_disk_new" },
		{ &A.disk_set_options, "archive_write_disk_set_options" },
		{ &A.write_header, "archive_write_header" },
		{ &A.write_data_block, "archive_write_data_block" },
		{ &A.finish_entry, "archive_write_finish_entry" },
		{ &A.write_free, "archive_write_free" },
		{ &A.entry_pathname, "archive_entry_pathname" },
		{ &A.entry_set_pathname, "archive_entry_set_pathname" },
		{ &A.entry_hardlink, "archive_entry_hardlink" },
		{ &A.entry_set_hardlink, "archive_entry_set_hardlink" },
		{ &A.entry_is_encrypted, "archive_entry_is_encrypted" },
	};
	for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) {
		void *f = dlsym(h, syms[i].name);
		if (!f)
			fail(1, "The Linux layer's libarchive is too old (it has no %s).", syms[i].name);
		memcpy(syms[i].slot, &f, sizeof f);
	}
}

/* ---- Names ---------------------------------------------------------------------------- */

/* "photos.tar.gz" -> "photos", "notes.zip" -> "notes", "data.gz" -> "data" */
static char *stem(const char *path)
{
	const char *base = strrchr(path, '/');
	char *s = strdup(base ? base + 1 : path);
	static const char *const exts[] = { ".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2", ".tar.lzma", ".tar.lz4", ".tar.Z",
					    ".tgz", ".txz", ".tbz2", ".tbz", ".tzst", ".zip", ".7z", ".rar", ".tar", ".gz",
					    ".xz", ".zst", ".bz2", ".lzma", ".lz4", ".Z", ".cab", ".iso", ".cpio", ".ar",
					    ".deb", ".xar", ".lha", ".lzh", ".jar", ".apk" };
	size_t n = strlen(s);
	for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
		size_t e = strlen(exts[i]);
		if (n > e && strcasecmp(s + n - e, exts[i]) == 0) {
			s[n - e] = 0;
			break;
		}
	}
	if (!*s || s[0] == '.') {
		free(s);
		s = strdup("Archive");
	}
	return s;
}

static bool exists(const char *p)
{
	struct stat st;
	return lstat(p, &st) == 0;
}

/* dir/name, or "name 2", "name 3"... when that's taken (before the extension for files) */
static char *free_name(const char *dir, const char *name)
{
	char *p;
	if (asprintf(&p, "%s/%s", dir, name) < 0)
		exit(1);
	const char *dot = strrchr(name, '.');
	int base = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
	for (int k = 2; exists(p); k++) {
		free(p);
		if (asprintf(&p, "%s/%.*s %d%s", dir, base, name, k, name + base) < 0)
			exit(1);
	}
	return p;
}

static void remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char *c;
			if (asprintf(&c, "%s/%s", path, e->d_name) >= 0) {
				remove_tree(c);
				free(c);
			}
		}
		if (d)
			closedir(d);
		rmdir(path);
	} else {
		unlink(path);
	}
}

/* ---- Unpacking ------------------------------------------------------------------------- */

static long long total;
static time_t last_shown;

static void progress(struct archive *a, bool force)
{
	time_t now = time(NULL);
	if (!force && now == last_shown)
		return;
	last_shown = now;
	long long done = A.filter_bytes(a, -1);
	if (done > total)
		done = total;
	printf("progress %lld %lld\n", done < 0 ? 0 : done, total);
	fflush(stdout);
}

/* NULL when it isn't an archive of a kind libarchive knows */
static struct archive *open_archive(const char *path, const char *password, bool raw)
{
	struct archive *a = A.read_new();
	A.support_filter_all(a);
	if (raw) {
		A.support_format_raw(a);
	} else {
		A.support_format_all(a);
		A.support_format_empty(a);
	}
	if (password)
		A.add_passphrase(a, password);
	if (A.open_filename(a, path, 1 << 16) != ARCHIVE_OK) {
		if (!raw)
			return NULL;
		fail(1, "The archive is damaged or of a kind that can't be opened (%s).", A.error_string(a));
	}
	/* read as one file, it has to be a compressed one ("none" is a filter too) */
	if (raw && A.filter_count(a) < 2)
		fail(1, "This isn't an archive, or it's of a kind that can't be opened.");
	return a;
}

static bool needs_password(struct archive *a, struct archive_entry *e)
{
	const char *err = A.error_string(a);
	return (e && A.entry_is_encrypted(e)) || (err && (strcasestr(err, "passphrase") || strcasestr(err, "password") ||
							  strcasestr(err, "encrypt")));
}

/* Unpacks everything into dest (an empty folder). Returns how many entries
 * there were, and the first one's top-level name in first. */
static int unpack(const char *path, const char *dest, const char *password, const char *single, char *first, size_t cap)
{
	struct archive *a = open_archive(path, password, single != NULL);
	if (!a)
		return -1; /* maybe a compressed file, not an archive: unpacked again as one file */
	struct archive *out = A.disk_new();
	A.disk_set_options(out, EXTRACT_PERM | EXTRACT_TIME | EXTRACT_SECURE_SYMLINKS | EXTRACT_SECURE_NODOTDOT |
				    EXTRACT_SECURE_NOABSOLUTEPATHS);
	if (chdir(dest) < 0)
		fail(1, "The folder to unpack into couldn't be made.");
	int n = 0;
	first[0] = 0;
	for (;;) {
		struct archive_entry *e;
		int r = A.next_header(a, &e);
		if (r == ARCHIVE_EOF)
			break;
		if (r < ARCHIVE_WARN) {
			if (needs_password(a, NULL))
				fail(3, "This archive is locked with a password.");
			if (n == 0 && !single && A.filter_count(a) > 1)
				return -1; /* a compressed file, not an archive: unpacked again as one file */
			fail(1, "The archive is damaged or of a kind that can't be opened (%s).", A.error_string(a));
		}
		if (single) {
			A.entry_set_pathname(e, single);
		} else {
			/* names stay inside the folder: a leading "/" or "./" goes */
			const char *name = A.entry_pathname(e);
			while (name && (*name == '/' || (name[0] == '.' && name[1] == '/')))
				name += name[0] == '.' ? 2 : 1;
			if (!name || !*name || !strcmp(name, "."))
				continue;
			char *copy = strdup(name);
			A.entry_set_pathname(e, copy);
			free(copy);
			const char *link = A.entry_hardlink(e);
			while (link && *link == '/')
				link++;
			if (link) {
				copy = strdup(link);
				A.entry_set_hardlink(e, copy);
				free(copy);
			}
		}
		const char *name = A.entry_pathname(e);
		size_t top = strcspn(name, "/");
		if (n == 0)
			snprintf(first, cap, "%.*s", (int)top, name);
		else if (first[0] && (strlen(first) != top || strncmp(first, name, top) != 0))
			first[0] = 0; /* more than one thing at the top */
		n++;
		r = A.write_header(out, e);
		if (r < ARCHIVE_WARN)
			fail(1, "%s couldn't be unpacked: %s", name, A.error_string(out));
		for (;;) {
			const void *buf;
			size_t len;
			long long off;
			r = A.data_block(a, &buf, &len, &off);
			if (r == ARCHIVE_EOF)
				break;
			if (r < ARCHIVE_WARN) {
				if (needs_password(a, e))
					fail(3, password ? "That password isn't the right one." : "This archive is locked with a password.");
				fail(1, "%s is damaged in the archive (%s).", name, A.error_string(a));
			}
			if (A.write_data_block(out, buf, len, off) < ARCHIVE_WARN)
				fail(1, "%s couldn't be written: %s", name, A.error_string(out));
			if (stopped)
				fail(1, "Stopped.");
			progress(a, false);
		}
		if (A.finish_entry(out) < ARCHIVE_WARN)
			fail(1, "%s couldn't be written: %s", name, A.error_string(out));
		if (stopped)
			fail(1, "Stopped.");
	}
	progress(a, true);
	A.read_free(a);
	A.write_free(out);
	return n;
}

int main(int argc, char **argv)
{
	bool ask = argc > 1 && !strcmp(argv[1], "-p");
	if (argc - ask != 3)
		fail(2, "usage: unarchive [-p] ARCHIVE FOLDER");
	/* whole paths: unpacking happens from inside the new folder */
	char *path = realpath(argv[1 + ask], NULL), *dir = realpath(argv[2 + ask], NULL);
	if (!path)
		fail(1, "The archive isn't there any more.");
	if (!dir)
		fail(1, "The folder to unpack into isn't there.");
	char password[1024] = { 0 };
	if (ask && fgets(password, sizeof password, stdin))
		password[strcspn(password, "\r\n")] = 0;
	struct stat st;
	if (stat(path, &st) < 0)
		fail(1, "The archive isn't there any more.");
	total = st.st_size;
	load();

	/* a hidden folder to unpack into first, so a failure leaves nothing half done */
	char *name = stem(path);
	if (asprintf(&work, "%s/.%s.unpacking-%d", dir, name, getpid()) < 0 || mkdir(work, 0755) < 0)
		fail(1, "There's no room to unpack in %s (or it's read-only).", dir);
	atexit(clean_up);
	signal(SIGTERM, on_stop);
	signal(SIGINT, on_stop);
	char first[1024];
	int n = unpack(path, work, ask ? password : NULL, NULL, first, sizeof first);
	if (n < 0) {
		if (chdir("/") < 0)
			exit(1);
		remove_tree(work);
		mkdir(work, 0755);
		n = unpack(path, work, ask ? password : NULL, name, first, sizeof first);
	}
	if (chdir("/") < 0)
		exit(1);
	char *made;
	if (n == 0) {
		remove_tree(work);
		fail(1, "The archive is empty.");
	} else if (first[0]) {
		/* one folder or file at the top: it comes out as it is */
		char *inner;
		if (asprintf(&inner, "%s/%s", work, first) < 0)
			exit(1);
		made = free_name(dir, first);
		if (rename(inner, made) < 0)
			fail(1, "The unpacked files couldn't be moved into place.");
		rmdir(work);
	} else {
		made = free_name(dir, name);
		if (rename(work, made) < 0)
			fail(1, "The unpacked files couldn't be moved into place.");
	}
	finished = true;
	printf("done %s\n", made);
	return 0;
}
