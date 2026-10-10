/*
 * Documents: how a file the user picked gets into an app's sandbox, and no
 * other file does. Each document is one file (or folder) of the user's,
 * given an id, with permissions for the apps it was handed to. They are
 * served as a file system of their own (FUSE), mounted in the Linux session
 * at $XDG_RUNTIME_DIR/doc:
 *
 *     doc/ID/NAME                    every document, for programs outside sandboxes
 *     doc/by-app/APP/ID/NAME         the documents APP may use
 *
 * Flatpak puts by-app/APP at doc/ in the app's sandbox, so a path like
 * /run/user/1000/doc/3f2a91c0/Report.odt means the same inside and out.
 * Reads and writes go straight to the user's real file; an app with write
 * permission can also save the way editors do, through a temporary file
 * renamed over the document.
 *
 * The FUSE protocol is the kernel's (include/uapi/linux/fuse.h): requests
 * read from /dev/fuse, each answered with a reply written back. The session
 * (layerrun) mounts it for us and hands over the descriptor.
 */
#include "zportal.h"

/* ---- The kernel's FUSE messages ---------------------------------------------------------------- */

enum {
	FUSE_LOOKUP = 1, FUSE_FORGET = 2, FUSE_GETATTR = 3, FUSE_SETATTR = 4, FUSE_MKDIR = 9, FUSE_UNLINK = 10,
	FUSE_RMDIR = 11, FUSE_RENAME = 12, FUSE_OPEN = 14, FUSE_READ = 15, FUSE_WRITE = 16, FUSE_STATFS = 17,
	FUSE_RELEASE = 18, FUSE_FSYNC = 20, FUSE_FLUSH = 25, FUSE_INIT = 26, FUSE_OPENDIR = 27, FUSE_READDIR = 28,
	FUSE_RELEASEDIR = 29, FUSE_FSYNCDIR = 30, FUSE_ACCESS = 34, FUSE_CREATE = 35, FUSE_INTERRUPT = 36,
	FUSE_DESTROY = 38, FUSE_BATCH_FORGET = 42, FUSE_RENAME2 = 45,
};
enum { FATTR_MODE = 1, FATTR_SIZE = 8, FATTR_ATIME = 16, FATTR_MTIME = 32, FATTR_FH = 64, FATTR_ATIME_NOW = 128,
       FATTR_MTIME_NOW = 256 };

typedef struct {
	u32 len, opcode;
	u64 unique, nodeid;
	u32 uid, gid, pid;
	u16 total_extlen, padding;
} FuseIn;

typedef struct {
	u32 len;
	i32 error;
	u64 unique;
} FuseOut;

typedef struct {
	u64 ino, size, blocks, atime, mtime, ctime;
	u32 atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev, blksize, flags;
} FuseAttr;

typedef struct {
	u64 nodeid, generation, entry_valid, attr_valid;
	u32 entry_valid_nsec, attr_valid_nsec;
	FuseAttr attr;
} FuseEntryOut;

typedef struct {
	u64 attr_valid;
	u32 attr_valid_nsec, dummy;
	FuseAttr attr;
} FuseAttrOut;

typedef struct {
	u64 fh;
	u32 open_flags, padding;
} FuseOpenOut;

typedef struct {
	u32 major, minor, max_readahead, flags;
	u16 max_background, congestion_threshold;
	u32 max_write, time_gran;
	u16 max_pages, map_alignment;
	u32 flags2, max_stack_depth;
	u32 unused[6];
} FuseInitOut;

typedef struct {
	u32 valid, padding;
	u64 fh, size, lock_owner, atime, mtime, ctime;
	u32 atimensec, mtimensec, ctimensec, mode, unused4, uid, gid, unused5;
} FuseSetattrIn;

typedef struct {
	u64 fh, offset;
	u32 size, read_flags;
	u64 lock_owner;
	u32 flags, padding;
} FuseReadIn;

typedef struct {
	u32 flags, mode, umask, open_flags;
} FuseCreateIn;

/* ---- Documents ------------------------------------------------------------------------------------ */

enum { PERM_READ = 1, PERM_WRITE = 2, PERM_GRANT = 4, PERM_DELETE = 8 };

typedef struct {
	char app[128];
	u32 perms;
} Grant;

typedef struct {
	char id[12];
	char *dir, *name;       /* the real file: dir/name */
	bool persistent;
	Grant grants[16];
	int ngrants;
} Doc;

static VEC(Doc *) docs;
static int fuse_fd = -1;
static bool mounted;
static char mountpoint[256];

static char *docs_db(void)
{
	return str_fmt("%s/.local/share/zos/documents", P.home);
}

/* id dir name persistent app:perms ... (tab-separated, a document a line) */
static void docs_save(void)
{
	StrBuf sb = { 0 };
	vec_each(&docs, d) {
		if (!(*d)->persistent)
			continue;
		sb_fmt(&sb, "%s\t%s\t%s", (*d)->id, (*d)->dir, (*d)->name);
		for (int g = 0; g < (*d)->ngrants; g++)
			sb_fmt(&sb, "\t%s:%u", (*d)->grants[g].app, (*d)->grants[g].perms);
		sb_addc(&sb, '\n');
	}
	char *path = docs_db(), *dir = path_dir(path);
	dir_make(dir, 0700);
	file_write_atomic(path, sb_str(&sb), sb.len);
	mem_free(dir);
	mem_free(path);
	sb_free(&sb);
}

static void docs_load(void)
{
	if (docs.len)
		return;
	char *path = docs_db();
	char *text = file_read(path, NULL);
	mem_free(path);
	usize n = 0;
	char **lines = str_split(text ? text : "", '\n', &n);
	for (usize i = 0; i < n; i++) {
		usize nf = 0;
		char **f = str_split(lines[i], '\t', &nf);
		if (nf >= 3) {
			Doc *d = NEW(Doc);
			str_copy(d->id, f[0], sizeof d->id);
			d->dir = str_dup(f[1]);
			d->name = str_dup(f[2]);
			d->persistent = true;
			for (usize k = 3; k < nf && d->ngrants < 16; k++) {
				const char *colon = str_rchr(f[k], ':');
				if (!colon)
					continue;
				Grant *g = &d->grants[d->ngrants++];
				str_copy(g->app, f[k], MIN(sizeof g->app, (usize)(colon - f[k]) + 1));
				g->perms = (u32)str_to_int(colon + 1, NULL);
			}
			vec_push(&docs, d);
		}
		str_split_free(f, nf);
	}
	str_split_free(lines, n);
	mem_free(text);
}

static Doc *doc_by_id(const char *id)
{
	vec_each(&docs, d) if (str_eq((*d)->id, id)) return *d;
	return NULL;
}

static Doc *doc_by_path(const char *dir, const char *name)
{
	vec_each(&docs, d) if (str_eq((*d)->dir, dir) && str_eq((*d)->name, name)) return *d;
	return NULL;
}

static u32 doc_perms(const Doc *d, const char *app)
{
	if (!app)
		return PERM_READ | PERM_WRITE | PERM_GRANT | PERM_DELETE; /* outside sandboxes: everything */
	for (int g = 0; g < d->ngrants; g++)
		if (str_eq(d->grants[g].app, app))
			return d->grants[g].perms;
	return 0;
}

static void doc_grant(Doc *d, const char *app, u32 perms)
{
	if (!app || !*app)
		return;
	for (int g = 0; g < d->ngrants; g++)
		if (str_eq(d->grants[g].app, app)) {
			d->grants[g].perms |= perms;
			return;
		}
	if (d->ngrants < 16) {
		str_copy(d->grants[d->ngrants].app, app, sizeof d->grants[0].app);
		d->grants[d->ngrants++].perms = perms;
	}
}

/* The document for dir/name, made if it's new. */
static Doc *doc_add(const char *dir, const char *name, bool reuse, bool persistent)
{
	docs_load();
	Doc *d = reuse ? doc_by_path(dir, name) : NULL;
	if (d) {
		d->persistent |= persistent;
		return d;
	}
	d = NEW(Doc);
	do {
		u8 raw[4];
		rand_bytes(raw, 4);
		fmt(d->id, sizeof d->id, "%02x%02x%02x%02x", raw[0], raw[1], raw[2], raw[3]);
	} while (doc_by_id(d->id));
	d->dir = str_dup(dir);
	d->name = str_dup(name);
	d->persistent = persistent;
	vec_push(&docs, d);
	return d;
}

static u32 perms_from(const DBusVal *list)
{
	u32 p = 0;
	usize n = 0;
	const char **v = dbus_strv(list, &n);
	for (usize i = 0; i < n; i++)
		p |= str_eq(v[i], "read") ? PERM_READ : str_eq(v[i], "write") ? PERM_WRITE
			: str_eq(v[i], "grant-permissions") ? PERM_GRANT : str_eq(v[i], "delete") ? PERM_DELETE : 0;
	mem_free(v);
	return p;
}

char *document_export(const char *path, const char *app_id, bool writable)
{
	if (!mounted || !path || path[0] != '/')
		return NULL;
	char *dir = path_dir(path);
	Doc *d = doc_add(dir, path_base(path), true, true);
	mem_free(dir);
	doc_grant(d, app_id, PERM_READ | (writable ? PERM_WRITE : 0));
	docs_save();
	return str_fmt("%s/%s/%s", mountpoint, d->id, d->name);
}

/* ---- The file system's nodes ------------------------------------------------------------------------ */

enum { N_ROOT, N_BYAPP, N_APP, N_DOC, N_ENTRY };

typedef struct {
	int kind;
	char *app;      /* by-app's app, NULL at the top level */
	Doc *doc;
	char *rel;      /* N_ENTRY: the path inside the document's folder (its name, or deeper) */
} Node;

static VEC(Node *) nodes;  /* node id = index + 1; 1 is the root, 2 by-app */

static u64 node_id(int kind, const char *app, Doc *doc, const char *rel)
{
	for (usize i = 0; i < nodes.len; i++) {
		Node *n = nodes.data[i];
		if (n->kind == kind && n->doc == doc && ((!n->app && !app) || (n->app && app && str_eq(n->app, app))) &&
		    ((!n->rel && !rel) || (n->rel && rel && str_eq(n->rel, rel))))
			return i + 1;
	}
	Node *n = NEW(Node);
	n->kind = kind;
	n->app = app ? str_dup(app) : NULL;
	n->doc = doc;
	n->rel = rel ? str_dup(rel) : NULL;
	vec_push(&nodes, n);
	return nodes.len;
}

static Node *node(u64 id)
{
	return id >= 1 && id <= nodes.len ? nodes.data[id - 1] : NULL;
}

static char *real_path(const Node *n)
{
	return str_fmt("%s/%s", n->doc->dir, n->rel);
}

static u32 node_perms(const Node *n)
{
	return n->doc ? doc_perms(n->doc, n->app) : PERM_READ;
}

static void fill_attr(FuseAttr *a, u64 id, const Node *n)
{
	memset(a, 0, sizeof *a);
	a->ino = id;
	a->uid = P.uid, a->gid = P.gid;
	a->blksize = 4096;
	if (n->kind == N_ENTRY) {
		struct stat st;
		char *p = real_path(n);
		long r = sys_lstat(p, &st);
		mem_free(p);
		if (r == 0) {
			a->size = (u64)st.st_size;
			a->blocks = (u64)st.st_blocks;
			a->atime = (u64)st.st_atime, a->atimensec = (u32)st.st_atime_nsec;
			a->mtime = (u64)st.st_mtime, a->mtimensec = (u32)st.st_mtime_nsec;
			a->ctime = (u64)st.st_ctime, a->ctimensec = (u32)st.st_ctime_nsec;
			a->mode = st.st_mode;
			if (!(node_perms(n) & PERM_WRITE))
				a->mode &= ~0222u;
			a->nlink = (u32)st.st_nlink;
			return;
		}
	}
	u64 now = (u64)time_unix();
	a->atime = a->mtime = a->ctime = now;
	a->mode = S_IFDIR | (n->kind == N_DOC && (node_perms(n) & PERM_WRITE) ? 0700 : 0500);
	a->nlink = 2;
}

/* ---- Answering the kernel ---------------------------------------------------------------------------- */

static u8 *req;

static void reply(const FuseIn *in, i32 error, const void *data, usize len)
{
	FuseOut out = { (u32)(sizeof out + len), error, in->unique };
	struct iovec iov[2] = { { &out, sizeof out }, { (void *)data, len } };
	sys_writev(fuse_fd, iov, len ? 2 : 1);
}

static void reply_entry(const FuseIn *in, u64 id)
{
	FuseEntryOut e = { 0 };
	e.nodeid = id;
	e.entry_valid = 1, e.attr_valid = 1;
	fill_attr(&e.attr, id, node(id));
	reply(in, 0, &e, sizeof e);
}

static bool exists(const Node *n)
{
	if (n->kind != N_ENTRY)
		return true;
	struct stat st;
	char *p = real_path(n);
	bool ok = sys_lstat(p, &st) == 0;
	mem_free(p);
	return ok;
}

static void do_lookup(const FuseIn *in, Node *parent, const char *name)
{
	u64 id = 0;
	switch (parent->kind) {
	case N_ROOT:
		if (str_eq(name, "by-app"))
			id = 2;
		else if (doc_by_id(name))
			id = node_id(N_DOC, NULL, doc_by_id(name), NULL);
		break;
	case N_BYAPP:
		id = node_id(N_APP, name, NULL, NULL);
		break;
	case N_APP: {
		Doc *d = doc_by_id(name);
		if (d && (doc_perms(d, parent->app) & PERM_READ))
			id = node_id(N_DOC, parent->app, d, NULL);
		break;
	}
	case N_DOC:
		/* the document, or a file being saved next to it (with write permission) */
		if (str_eq(name, parent->doc->name) || (node_perms(parent) & PERM_WRITE))
			id = node_id(N_ENTRY, parent->app, parent->doc, name);
		break;
	case N_ENTRY: {
		char *rel = str_fmt("%s/%s", parent->rel, name);
		id = node_id(N_ENTRY, parent->app, parent->doc, rel);
		mem_free(rel);
		break;
	}
	}
	if (!id || !exists(node(id))) {
		reply(in, -ENOENT, NULL, 0);
		return;
	}
	reply_entry(in, id);
}

/* A directory listing: "." and "..", then the entries, from offset on. */
typedef struct {
	u8 buf[65536];
	usize len, cap;
	u64 index, from;
} Dirents;

static void dirent(Dirents *d, u64 ino, const char *name, u32 type)
{
	d->index++;
	if (d->index <= d->from)
		return;
	usize nl = strlen(name), size = (24 + nl + 7) & ~(usize)7;
	if (d->len + size > d->cap)
		return;
	u8 *p = d->buf + d->len;
	memset(p, 0, size);
	memcpy(p, &ino, 8);
	memcpy(p + 8, &d->index, 8);
	u32 n32 = (u32)nl;
	memcpy(p + 16, &n32, 4);
	memcpy(p + 20, &type, 4);
	memcpy(p + 24, name, nl);
	d->len += size;
}

static void do_readdir(const FuseIn *in, Node *n, const FuseReadIn *r)
{
	static Dirents d;
	d.len = 0, d.index = 0, d.from = r->offset, d.cap = MIN((usize)r->size, sizeof d.buf);
	dirent(&d, in->nodeid, ".", 4);
	dirent(&d, 1, "..", 4);
	if (n->kind == N_ROOT) {
		dirent(&d, 2, "by-app", 4);
		vec_each(&docs, x) dirent(&d, 3, (*x)->id, 4);
	} else if (n->kind == N_BYAPP) {
		/* the apps that have documents */
		VEC(const char *) seen = { 0 };
		vec_each(&docs, x) for (int g = 0; g < (*x)->ngrants; g++) {
			bool dup = false;
			vec_each(&seen, s) dup |= str_eq(*s, (*x)->grants[g].app);
			if (!dup) {
				vec_push(&seen, (*x)->grants[g].app);
				dirent(&d, 3, (*x)->grants[g].app, 4);
			}
		}
		vec_free(&seen);
	} else if (n->kind == N_APP) {
		vec_each(&docs, x) if (doc_perms(*x, n->app) & PERM_READ) dirent(&d, 3, (*x)->id, 4);
	} else if (n->kind == N_DOC) {
		char *p = str_fmt("%s/%s", n->doc->dir, n->doc->name);
		struct stat st;
		if (sys_lstat(p, &st) == 0)
			dirent(&d, 3, n->doc->name, S_ISDIR(st.st_mode) ? 4 : 8);
		mem_free(p);
	} else {
		char *p = real_path(n);
		DirList l = { 0 };
		if (dir_list(p, &l, false))
			vec_each(&l, e) dirent(&d, 3, e->name, e->type);
		dir_list_free(&l);
		mem_free(p);
	}
	reply(in, 0, d.buf, d.len);
}

/* Open files: the real file's descriptor is the handle. */
static void do_open(const FuseIn *in, Node *n, u32 flags)
{
	if (n->kind != N_ENTRY) {
		FuseOpenOut o = { 0 };
		reply(in, 0, &o, sizeof o);
		return;
	}
	bool writes = (flags & 3) != O_RDONLY || (flags & O_TRUNC);
	if (writes && !(node_perms(n) & PERM_WRITE)) {
		reply(in, -EACCES, NULL, 0);
		return;
	}
	char *p = real_path(n);
	int fd = (int)sys_open(p, (int)(flags & ~(u32)(O_CREAT | O_EXCL | O_NOCTTY)) | O_CLOEXEC, 0);
	mem_free(p);
	if (fd < 0) {
		reply(in, fd, NULL, 0);
		return;
	}
	FuseOpenOut o = { (u64)fd, 0, 0 };
	reply(in, 0, &o, sizeof o);
}

static void do_create(const FuseIn *in, Node *parent, const FuseCreateIn *c, const char *name)
{
	if ((parent->kind != N_DOC && parent->kind != N_ENTRY) || !(node_perms(parent) & PERM_WRITE)) {
		reply(in, -EACCES, NULL, 0);
		return;
	}
	char *rel = parent->kind == N_DOC ? str_dup(name) : str_fmt("%s/%s", parent->rel, name);
	u64 id = node_id(N_ENTRY, parent->app, parent->doc, rel);
	mem_free(rel);
	char *p = real_path(node(id));
	int fd = (int)sys_open(p, (int)(c->flags | O_CREAT) | O_CLOEXEC, (int)(c->mode & ~c->umask & 07777));
	mem_free(p);
	if (fd < 0) {
		reply(in, fd, NULL, 0);
		return;
	}
	struct {
		FuseEntryOut e;
		FuseOpenOut o;
	} out = { 0 };
	out.e.nodeid = id;
	out.e.entry_valid = out.e.attr_valid = 1;
	fill_attr(&out.e.attr, id, node(id));
	out.o.fh = (u64)fd;
	reply(in, 0, &out, sizeof out);
}

/* The real path of name in a document folder node (NULL if not one). */
static char *child_path(Node *parent, const char *name)
{
	if (parent->kind == N_DOC)
		return str_fmt("%s/%s", parent->doc->dir, name);
	if (parent->kind == N_ENTRY)
		return str_fmt("%s/%s/%s", parent->doc->dir, parent->rel, name);
	return NULL;
}

static void do_setattr(const FuseIn *in, Node *n, const FuseSetattrIn *s)
{
	if (n->kind != N_ENTRY) {
		FuseAttrOut a = { 1, 0, 0, { 0 } };
		fill_attr(&a.attr, in->nodeid, n);
		reply(in, 0, &a, sizeof a);
		return;
	}
	if (!(node_perms(n) & PERM_WRITE)) {
		reply(in, -EACCES, NULL, 0);
		return;
	}
	char *p = real_path(n);
	long r = 0;
	if (s->valid & FATTR_SIZE)
		r = (s->valid & FATTR_FH) ? sys_ftruncate((int)s->fh, (i64)s->size) : sys_truncate(p, (i64)s->size);
	if (r == 0 && (s->valid & FATTR_MODE))
		r = sys_chmod(p, s->mode & 07777);
	if (r == 0 && (s->valid & (FATTR_ATIME | FATTR_MTIME))) {
		struct timespec t[2] = { { (i64)s->atime, s->atimensec }, { (i64)s->mtime, s->mtimensec } };
		if (!(s->valid & FATTR_ATIME))
			t[0].tv_nsec = UTIME_OMIT;
		else if (s->valid & FATTR_ATIME_NOW)
			t[0].tv_nsec = UTIME_NOW;
		if (!(s->valid & FATTR_MTIME))
			t[1].tv_nsec = UTIME_OMIT;
		else if (s->valid & FATTR_MTIME_NOW)
			t[1].tv_nsec = UTIME_NOW;
		r = sys_utimensat(AT_FDCWD, p, t, 0);
	}
	mem_free(p);
	if (r < 0) {
		reply(in, (i32)r, NULL, 0);
		return;
	}
	FuseAttrOut a = { 1, 0, 0, { 0 } };
	fill_attr(&a.attr, in->nodeid, n);
	reply(in, 0, &a, sizeof a);
}

static void handle(const FuseIn *in, const u8 *arg, usize arglen)
{
	Node *n = node(in->nodeid);
	switch (in->opcode) {
	case FUSE_INIT: {
		FuseInitOut o = { 0 };
		u32 minor = arglen >= 8 ? ((const u32 *)arg)[1] : 31;
		o.major = 7;
		o.minor = MIN(minor, 31u);
		o.max_readahead = 128 * 1024;
		o.max_background = 16;
		o.congestion_threshold = 12;
		o.max_write = 128 * 1024;
		o.time_gran = 1;
		reply(in, 0, &o, sizeof o);
		return;
	}
	case FUSE_FORGET:
	case FUSE_BATCH_FORGET:
	case FUSE_INTERRUPT:
		return; /* no answers to these */
	case FUSE_DESTROY:
		reply(in, 0, NULL, 0);
		return;
	}
	if (!n) {
		reply(in, -ENOENT, NULL, 0);
		return;
	}
	switch (in->opcode) {
	case FUSE_LOOKUP:
		do_lookup(in, n, (const char *)arg);
		break;
	case FUSE_GETATTR: {
		if (!exists(n)) {
			reply(in, -ENOENT, NULL, 0);
			break;
		}
		FuseAttrOut a = { 1, 0, 0, { 0 } };
		fill_attr(&a.attr, in->nodeid, n);
		reply(in, 0, &a, sizeof a);
		break;
	}
	case FUSE_SETATTR:
		do_setattr(in, n, (const FuseSetattrIn *)arg);
		break;
	case FUSE_OPEN:
	case FUSE_OPENDIR:
		do_open(in, in->opcode == FUSE_OPENDIR ? nodes.data[0] : n, ((const u32 *)arg)[0]);
		break;
	case FUSE_READ: {
		const FuseReadIn *r = (const FuseReadIn *)arg;
		static u8 data[128 * 1024];
		long got = sys_pread((int)r->fh, data, MIN((usize)r->size, sizeof data), (i64)r->offset);
		if (got < 0)
			reply(in, (i32)got, NULL, 0);
		else
			reply(in, 0, data, (usize)got);
		break;
	}
	case FUSE_WRITE: {
		const FuseReadIn *w = (const FuseReadIn *)arg; /* same layout up to the data */
		long put = sys_pwrite((int)w->fh, arg + 40, MIN((usize)w->size, arglen > 40 ? arglen - 40 : 0), (i64)w->offset);
		if (put < 0) {
			reply(in, (i32)put, NULL, 0);
		} else {
			u32 out[2] = { (u32)put, 0 };
			reply(in, 0, out, sizeof out);
		}
		break;
	}
	case FUSE_RELEASE:
		if (n->kind == N_ENTRY)
			sys_close((int)*(const u64 *)arg);
		reply(in, 0, NULL, 0);
		break;
	case FUSE_RELEASEDIR:
	case FUSE_FLUSH:
	case FUSE_FSYNCDIR:
		reply(in, 0, NULL, 0);
		break;
	case FUSE_FSYNC:
		reply(in, (i32)sys_fsync((int)*(const u64 *)arg), NULL, 0);
		break;
	case FUSE_READDIR:
		do_readdir(in, n, (const FuseReadIn *)arg);
		break;
	case FUSE_CREATE:
		do_create(in, n, (const FuseCreateIn *)arg, (const char *)arg + sizeof(FuseCreateIn));
		break;
	case FUSE_MKDIR:
	case FUSE_UNLINK:
	case FUSE_RMDIR: {
		const char *name = (const char *)arg + (in->opcode == FUSE_MKDIR ? 8 : 0);
		char *p = (node_perms(n) & PERM_WRITE) ? child_path(n, name) : NULL;
		/* removing the document itself takes delete permission */
		if (p && n->kind == N_DOC && str_eq(name, n->doc->name) && in->opcode != FUSE_MKDIR && !(node_perms(n) & PERM_DELETE)) {
			mem_free(p);
			p = NULL;
		}
		long r = -EACCES;
		if (p)
			r = in->opcode == FUSE_MKDIR ? sys_mkdir(p, ((const u32 *)arg)[0] & 07777)
						     : in->opcode == FUSE_UNLINK ? sys_unlink(p) : sys_rmdir(p);
		mem_free(p);
		if (r == 0 && in->opcode == FUSE_MKDIR)
			do_lookup(in, n, name);
		else
			reply(in, (i32)r, NULL, 0);
		break;
	}
	case FUSE_RENAME:
	case FUSE_RENAME2: {
		u64 newdir = *(const u64 *)arg;
		usize skip = in->opcode == FUSE_RENAME ? 8 : 16;
		u32 flags = in->opcode == FUSE_RENAME2 ? ((const u32 *)arg)[2] : 0;
		const char *from = (const char *)arg + skip, *to = from + strlen(from) + 1;
		Node *nd = node(newdir);
		char *a = (node_perms(n) & PERM_WRITE) ? child_path(n, from) : NULL;
		char *b = nd && nd->doc == n->doc && (node_perms(nd) & PERM_WRITE) ? child_path(nd, to) : NULL;
		long r = a && b ? sys_rename2(a, b, flags) : -EACCES;
		mem_free(a);
		mem_free(b);
		reply(in, (i32)r, NULL, 0);
		break;
	}
	case FUSE_ACCESS: {
		u32 mask = *(const u32 *)arg;
		bool ok = !(mask & 2) || (node_perms(n) & PERM_WRITE) || n->kind == N_ROOT;
		reply(in, ok ? 0 : -EACCES, NULL, 0);
		break;
	}
	case FUSE_STATFS: {
		struct statfs st;
		u64 out[5 + 10] = { 0 };
		if (sys_statfs(P.home, &st) == 0) {
			out[0] = st.f_blocks, out[1] = st.f_bfree, out[2] = st.f_bavail, out[3] = st.f_files, out[4] = st.f_ffree;
			u32 *w = (u32 *)&out[5];
			w[0] = (u32)st.f_bsize, w[1] = 255, w[2] = (u32)st.f_frsize;
		}
		reply(in, 0, out, 80);
		break;
	}
	default:
		reply(in, -ENOSYS, NULL, 0);
		break;
	}
}

static void on_fuse(Loop *loop, int fd, u32 events, void *data)
{
	(void)data;
	(void)events;
	for (int i = 0; i < 64; i++) {
		long n = sys_read(fd, req, 128 * 1024 + 4096);
		if (n == -EAGAIN || n == -EINTR)
			return;
		if (n < (long)sizeof(FuseIn)) {
			/* unmounted (the session ended) */
			loop_del_fd(loop, fd);
			sys_close(fd);
			fuse_fd = -1;
			mounted = false;
			return;
		}
		const FuseIn *in = (const FuseIn *)req;
		handle(in, req + sizeof(FuseIn), (usize)n - sizeof(FuseIn));
	}
}

/* Asks the Linux session to mount our file system at the runtime folder's doc/. */
static int mount_documents(void)
{
	fmt(mountpoint, sizeof mountpoint, "%s/doc", P.rt);
	sys_mkdir(mountpoint, 0700);
	char *sock = str_fmt("%s/zos-linux/fuse.sock", P.rt);
	int s = unix_connect(sock, SOCK_SEQPACKET);
	mem_free(sock);
	if (s < 0) {
		eprint("zportal: no document store yet: the session's mount service isn't there\n");
		return -1;
	}
	char request[400];
	int len = fmt(request, sizeof request, "mount\n%s\nfsname=portal,subtype=portal", mountpoint);
	unix_send(s, request, (usize)len, NULL, 0);
	char answer[256] = { 0 };
	int fd = -1, nfds = 0;
	struct pollfd p = { s, POLLIN, 0 };
	if (sys_poll(&p, 1, 3000) > 0)
		unix_recv(s, answer, sizeof answer - 1, &fd, 1, &nfds);
	sys_close(s);
	if (!str_eq(answer, "ok") || nfds < 1) {
		eprint("zportal: no document store: %s\n", answer[0] ? answer : "the session didn't answer");
		if (nfds > 0)
			sys_close(fd);
		return -1;
	}
	return fd;
}

/* ---- org.freedesktop.portal.Documents ------------------------------------------------------------------- */

static void add_bytes(DBusMsg *m, const char *s)
{
	dbus_open(m, "ay");
	for (const char *c = s; *c; c++)
		dbus_add(m, "y", (u8)*c);
	dbus_add(m, "y", 0);
	dbus_close(m);
}

static char *bytes_of(const DBusVal *v)
{
	v = dbus_unvariant(v);
	if (!v || v->type != 'a')
		return str_dup("");
	char *s = mem_alloc(v->count + 1);
	usize n = 0;
	for (usize i = 0; i < v->count && v->items[i]->n; i++)
		s[n++] = (char)v->items[i]->n;
	s[n] = 0;
	return s;
}

static char *fd_target(DBusMsg *call, const DBusVal *h)
{
	int fd = dbus_take_fd(call, h);
	if (fd < 0)
		return NULL;
	char link[64], target[1024] = { 0 };
	fmt(link, sizeof link, "/proc/self/fd/%d", fd);
	long n = sys_readlink(link, target, sizeof target - 1);
	sys_close(fd);
	return n > 0 ? str_dup(target) : NULL;
}

/* A document for a file given by descriptor (or a folder's and a name). */
static Doc *doc_for(DBusMsg *call, const DBusVal *h, const char *name, bool reuse, bool persistent)
{
	char *target = fd_target(call, h);
	if (!target)
		return NULL;
	Doc *d;
	if (name) {
		d = doc_add(target, name, reuse, persistent);
	} else {
		char *dir = path_dir(target);
		d = doc_add(dir, path_base(target), reuse, persistent);
		mem_free(dir);
	}
	mem_free(target);
	return d;
}

static bool documents_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 5))
		return true;
	const char *m = dbus_member(call);
	if (!m || dbus_type(call) != DBUS_CALL)
		return false;
	bool sandboxed = false;
	const char *caller = caller_app(dbus_sender(call), &sandboxed);
	DBusMsg *r = NULL;
	if (str_eq(m, "GetMountPoint")) {
		r = dbus_new_reply(call);
		add_bytes(r, mountpoint);
	} else if (str_eq(m, "Add") || str_eq(m, "AddNamed")) {
		bool named = str_eq(m, "AddNamed");
		char *name = named ? bytes_of(dbus_arg(call, 1)) : NULL;
		Doc *d = doc_for(call, dbus_arg(call, 0), name, dbus_bool(dbus_arg(call, named ? 2 : 1), true),
				 dbus_bool(dbus_arg(call, named ? 3 : 2), true));
		mem_free(name);
		if (!d) {
			dbus_reply_error(bus, call, "org.freedesktop.portal.Error.InvalidArgument", "Not a file");
			return true;
		}
		if (sandboxed)
			doc_grant(d, caller, PERM_READ | PERM_WRITE | PERM_GRANT | PERM_DELETE);
		docs_save();
		r = dbus_new_reply(call);
		dbus_add(r, "s", d->id);
	} else if (str_eq(m, "AddFull") || str_eq(m, "AddNamedFull")) {
		bool named = str_eq(m, "AddNamedFull");
		u32 flags = (u32)dbus_uint(dbus_arg(call, named ? 2 : 1), 0);
		const char *app = dbus_str(dbus_arg(call, named ? 3 : 2), "");
		u32 perms = perms_from(dbus_arg(call, named ? 4 : 3));
		r = dbus_new_reply(call);
		if (named) {
			char *name = bytes_of(dbus_arg(call, 1));
			Doc *d = doc_for(call, dbus_arg(call, 0), name, flags & 1, flags & 2);
			mem_free(name);
			if (d)
				doc_grant(d, app, perms);
			dbus_add(r, "s", d ? d->id : "");
		} else {
			dbus_open(r, "as");
			const DBusVal *fds = dbus_arg(call, 0);
			for (usize i = 0; fds && i < fds->count; i++) {
				Doc *d = doc_for(call, fds->items[i], NULL, flags & 1, flags & 2);
				if (d)
					doc_grant(d, app, perms);
				dbus_add(r, "s", d ? d->id : "");
			}
			dbus_close(r);
		}
		dbus_open(r, "a{sv}");
		dbus_open(r, "{sv}");
		dbus_add(r, "s", "mountpoint");
		dbus_open_variant(r, "ay");
		add_bytes(r, mountpoint);
		dbus_close(r);
		dbus_close(r);
		dbus_close(r);
		docs_save();
	} else if (str_eq(m, "GrantPermissions") || str_eq(m, "RevokePermissions")) {
		Doc *d = doc_by_id(dbus_str(dbus_arg(call, 0), ""));
		const char *app = dbus_str(dbus_arg(call, 1), "");
		u32 perms = perms_from(dbus_arg(call, 2));
		if (!d || (sandboxed && !(doc_perms(d, caller) & PERM_GRANT))) {
			dbus_reply_error(bus, call, "org.freedesktop.portal.Error.NotAllowed", "Not allowed");
			return true;
		}
		if (str_eq(m, "GrantPermissions"))
			doc_grant(d, app, perms);
		else
			for (int g = 0; g < d->ngrants; g++)
				if (str_eq(d->grants[g].app, app))
					d->grants[g].perms &= ~perms;
		docs_save();
		r = dbus_new_reply(call);
	} else if (str_eq(m, "Delete")) {
		for (usize i = 0; i < docs.len; i++)
			if (str_eq(docs.data[i]->id, dbus_str(dbus_arg(call, 0), "")) && (!sandboxed ||
			    (doc_perms(docs.data[i], caller) & PERM_DELETE))) {
				/* nodes may still point at it: it stays in memory, out of the list */
				vec_remove(&docs, i);
				break;
			}
		docs_save();
		r = dbus_new_reply(call);
	} else if (str_eq(m, "Lookup")) {
		char *path = bytes_of(dbus_arg(call, 0));
		char *dir = path_dir(path);
		Doc *d = doc_by_path(dir, path_base(path));
		r = dbus_new_reply(call);
		dbus_add(r, "s", d ? d->id : "");
		mem_free(dir);
		mem_free(path);
	} else if (str_eq(m, "Info")) {
		Doc *d = doc_by_id(dbus_str(dbus_arg(call, 0), ""));
		if (!d) {
			dbus_reply_error(bus, call, "org.freedesktop.portal.Error.NotFound", "No such document");
			return true;
		}
		r = dbus_new_reply(call);
		char *path = str_fmt("%s/%s", d->dir, d->name);
		add_bytes(r, path);
		mem_free(path);
		dbus_open(r, "a{sas}");
		for (int g = 0; g < d->ngrants; g++) {
			dbus_open(r, "{sas}");
			dbus_add(r, "s", d->grants[g].app);
			dbus_open(r, "as");
			static const char *const names[] = { "read", "write", "grant-permissions", "delete" };
			for (int b = 0; b < 4; b++)
				if (d->grants[g].perms & (1u << b))
					dbus_add(r, "s", names[b]);
			dbus_close(r);
			dbus_close(r);
		}
		dbus_close(r);
	} else if (str_eq(m, "List") || str_eq(m, "GetHostPaths")) {
		bool host = str_eq(m, "GetHostPaths");
		const char *app = host ? NULL : dbus_str(dbus_arg(call, 0), "");
		usize nid = 0;
		const char **ids = host ? dbus_strv(dbus_arg(call, 0), &nid) : NULL;
		r = dbus_new_reply(call);
		dbus_open(r, "a{say}");
		vec_each(&docs, x) {
			bool want = false;
			if (host)
				for (usize i = 0; i < nid; i++)
					want |= str_eq(ids[i], (*x)->id);
			else
				want = !*app || (doc_perms(*x, app) & PERM_READ);
			if (!want)
				continue;
			dbus_open(r, "{say}");
			dbus_add(r, "s", (*x)->id);
			char *path = host ? str_fmt("%s/%s", (*x)->dir, (*x)->name) : str_fmt("%s/%s/%s", mountpoint, (*x)->id, (*x)->name);
			add_bytes(r, path);
			mem_free(path);
			dbus_close(r);
		}
		dbus_close(r);
		mem_free(ids);
	} else {
		return false;
	}
	dbus_send(bus, r);
	return true;
}

static const char DOCUMENTS_XML[] =
	"<interface name=\"org.freedesktop.portal.Documents\">"
	"<method name=\"GetMountPoint\"><arg type=\"ay\" direction=\"out\"/></method>"
	"<method name=\"Add\"><arg type=\"h\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/>"
	"<arg type=\"s\" direction=\"out\"/></method>"
	"<method name=\"AddNamed\"><arg type=\"h\" direction=\"in\"/><arg type=\"ay\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/>"
	"<arg type=\"b\" direction=\"in\"/><arg type=\"s\" direction=\"out\"/></method>"
	"<method name=\"AddFull\"><arg type=\"ah\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"as\" direction=\"in\"/><arg type=\"as\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
	"<method name=\"AddNamedFull\"><arg type=\"h\" direction=\"in\"/><arg type=\"ay\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
	"<arg type=\"s\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/><arg type=\"s\" direction=\"out\"/>"
	"<arg type=\"a{sv}\" direction=\"out\"/></method>"
	"<method name=\"GrantPermissions\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/></method>"
	"<method name=\"RevokePermissions\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"as\" direction=\"in\"/></method>"
	"<method name=\"Delete\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"Lookup\"><arg type=\"ay\" direction=\"in\"/><arg type=\"s\" direction=\"out\"/></method>"
	"<method name=\"Info\"><arg type=\"s\" direction=\"in\"/><arg type=\"ay\" direction=\"out\"/><arg type=\"a{sas}\" direction=\"out\"/></method>"
	"<method name=\"List\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{say}\" direction=\"out\"/></method>"
	"<method name=\"GetHostPaths\"><arg type=\"as\" direction=\"in\"/><arg type=\"a{say}\" direction=\"out\"/></method>"
	"<property name=\"version\" type=\"u\" access=\"read\"/></interface>";

static bool exported;
static int attempts;

/* Retried until the session mounts the store (a few times). */
void documents_tick(DBus *bus)
{
	if (!bus || mounted || attempts >= 5)
		return;
	attempts++;
	int fd = mount_documents();
	if (fd < 0)
		return;
	fuse_fd = fd;
	mounted = true;
	fd_set_nonblock(fd, true);
	loop_add_fd(P.loop, fd, EPOLLIN, on_fuse, NULL);
	if (!exported) {
		exported = true;
		dbus_export(bus, "/org/freedesktop/portal/documents", "org.freedesktop.portal.Documents", documents_method, NULL,
			    DOCUMENTS_XML);
	}
	dbus_request_name(bus, "org.freedesktop.portal.Documents", DBUS_NAME_REPLACE);
	eprint("zportal: documents at %s\n", mountpoint);
}

void documents_start(DBus *bus)
{
	docs_load();
	if (!nodes.len) {
		node_id(N_ROOT, NULL, NULL, NULL);
		node_id(N_BYAPP, NULL, NULL, NULL);
	}
	if (!req)
		req = mem_alloc(128 * 1024 + 4096);
	if (fuse_fd >= 0) {
		loop_del_fd(P.loop, fuse_fd);
		sys_close(fuse_fd);
		fuse_fd = -1;
	}
	mounted = false;
	exported = false;
	attempts = 0;
	/* without the file system, Flatpak would bind a missing folder into its
	 * sandboxes and fail: the name is only taken with it */
	documents_tick(bus);
}
