/* Mail: reads and sends email over IMAP and SMTP.
 *
 * A worker thread talks to the servers (one connection per account) so the
 * window never waits on the network; what it learns comes back as results
 * the window picks up each frame. Message lists and messages are cached on
 * disk, so mail read once opens instantly, even offline. */
#include "mail.h"

#define MAX_ACCTS 6
#define MAX_FOLDERS 80
#define LIST_MAX 200
#define POLL_SECONDS 120
#define UNDO_SECONDS 5

/* ---- State ------------------------------------------------------------------------------------- */

enum { AS_OFFLINE, AS_CONNECTING, AS_ONLINE, AS_FAILED };

typedef struct {
	int id;                  /* stable while the app runs, for the worker */
	Account a;
	Folder folders[MAX_FOLDERS];
	int nfolders;
	int state;
	char error[200];
	u32 inbox_top;           /* newest inbox UID seen, for new-mail notices */
	int inbox_unseen;
	char cache[512];
} Acct;

typedef struct {
	char name[160];
	char type[80];
	u8 *data;
	usize len;
} Att;

typedef struct {
	int acct;
	StrBuf to, cc, bcc, subject, body;
	bool show_cc;
	char in_reply_to[200], references[1100];
	Att atts[16];
	int natts;
	u32 draft_uid;           /* the saved draft this one replaces */
	u32 answers;             /* the message this replies to, flagged \Answered once sent */
	char answers_folder[256];
	char title[32];
	bool sending;
	char error[256];
	u64 start_hash;          /* to tell whether anything changed */
	int frames;
	bool focus_body;
} Compose;

typedef struct {
	bool open;
	int edit;                /* account being changed, or -1 */
	StrBuf name, email, password, imap, imap_port, smtp, smtp_port;
	bool advanced, servers_edited, busy;
	char error[256];
	char guessed_for[128];
} Setup;

enum { PEND_NONE, PEND_MOVE, PEND_DELETE };

typedef struct {
	int kind;
	int acct_id;
	char folder[256], to[256];
	Head head;
	int index;
	f64 deadline;
} Pending;

/* Work for the worker, and what it found. */
enum { J_CONNECT, J_HEADS, J_SEARCH, J_BODY, J_FLAG, J_MOVE, J_DELETE, J_SEND, J_DRAFT, J_TEST, J_CHECK };

typedef struct Job Job;
struct Job {
	int kind;
	int acct_id;
	Account account;
	u64 gen;
	char folder[256], to[256];
	u32 uid;
	char flag[24];
	bool on, quiet;
	char *data;
	usize len;
	char **rcpt;
	int nrcpt;
	u32 old_draft;
	char drafts[256], sent[256];
	char query[256];
	char cache[512];
	Job *next;
};

typedef struct Result Result;
struct Result {
	int job;                 /* J_* it answers */
	int acct_id;
	u64 gen;
	bool ok, quiet;
	char error[256];
	char folder[256];
	Folder *folders;
	int nfolders;
	Head *heads;
	int nheads;
	u32 exists;
	int unseen;
	u32 uid;
	char *raw;
	usize len;
	Account account;
	Result *next;
};

static struct {
	Ui *ui;
	Acct accts[MAX_ACCTS];
	int nacct, next_id;
	int cur;                 /* account shown */
	char folder[256];        /* raw name of the folder shown */
	Head *heads;
	int nheads;
	u32 exists;
	bool loading;
	u64 heads_gen;
	char list_error[200];
	StrBuf search;
	bool searching;          /* showing server search results */
	char search_q[256];
	u32 sel;                 /* UID of the open message */
	Msg *msg;
	bool msg_loading;
	u64 msg_gen;
	char msg_error[200];
	f32 scroll, content_h;
	bool details;
	int save_att;            /* attachment waiting for the save dialog */
	Compose *compose;
	Setup setup;
	Pending pend;
	char toast[200];
	f64 toast_until;
	bool toast_undo;
	bool sidebar_open;
	f64 last_poll;
	bool h24;
	u64 gen;
	int confirm_remove;
	Map trusted;             /* senders whose pictures always load */
	/* sel-text */
	struct {
		bool on, dragging;
		int a, ao, b, bo;
		f32 px, py;
		Page *page;
	} sel_text;
	/* worker */
	Mutex qlock;
	Job *qhead, *qtail;
	int qwake;
	int busy;                /* atomic: jobs not yet finished */
	Mutex rlock;
	Result *rhead, *rtail;
} M;

/* ---- Paths and saving -------------------------------------------------------------------------- */

static char *config_file(const char *name)
{
	char *dir = path_join(home_dir(), ".config/zurbax/mail");
	dir_make(dir, 0700);
	char *p = path_join(dir, name);
	mem_free(dir);
	return p;
}

static void acct_cache_dir(Acct *a)
{
	/* the address names the folder, so it may only hold harmless characters */
	char name[128];
	str_copy(name, a->a.email, sizeof name);
	for (char *p = name; *p; p++)
		if (!(is_alnum(*p) || *p == '@' || *p == '_' || *p == '+' || *p == '-' || (*p == '.' && p > name)))
			*p = '_';
	if (!name[0] || str_eq(name, "."))
		str_copy(name, "account", sizeof name);
	char *base = path_join(home_dir(), ".cache/zurbax/mail");
	char *dir = path_join(base, name);
	dir_make(dir, 0700);
	str_copy(a->cache, dir, sizeof a->cache);
	mem_free(dir);
	mem_free(base);
}

static void folder_key(const char *folder, char *out)
{
	u8 h[32];
	sha256(folder, strlen(folder), h);
	hex_encode(h, 8, out);
}

static char *heads_file(const char *cache, const char *folder)
{
	char key[20];
	folder_key(folder, key);
	return str_fmt("%s/%s.heads", cache, key);
}

static char *message_file(const char *cache, const char *folder, u32 uid)
{
	char key[20];
	folder_key(folder, key);
	return str_fmt("%s/%s-%u.eml", cache, key, uid);
}

/* Written with owner-only permissions: the file holds passwords. */
static bool write_private(const char *path, const char *data, usize len)
{
	char *tmp = str_fmt("%s.tmp", path);
	int fd = (int)sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	bool ok = fd >= 0 && write_all(fd, data, len);
	if (fd >= 0)
		ok = sys_close(fd) == 0 && ok;
	ok = ok && sys_rename(tmp, path) == 0;
	if (!ok)
		sys_unlink(tmp);
	mem_free(tmp);
	return ok;
}

static void save_accounts(void)
{
	StrBuf sb = { 0 };
	sb_fmt(&sb, "# Mail accounts. Readable only by you.\ncount = %d\n", M.nacct);
	for (int i = 0; i < M.nacct; i++) {
		Account *a = &M.accts[i].a;
		sb_fmt(&sb, "%d.name = %s\n%d.email = %s\n%d.password = %s\n%d.imap = %s\n%d.imap_port = %u\n%d.smtp = %s\n%d.smtp_port = %u\n", i,
		       a->name, i, a->email, i, a->password, i, a->imap_host, i, a->imap_port, i, a->smtp_host, i, a->smtp_port);
	}
	char *path = config_file("accounts.conf");
	write_private(path, sb_str(&sb), sb.len);
	mem_free(path);
	sb_free(&sb);
}

static void load_accounts(void)
{
	char *path = config_file("accounts.conf");
	Conf *c = conf_load(path);
	mem_free(path);
	int n = (int)conf_get_int(c, "count", 0);
	for (int i = 0; i < n && M.nacct < MAX_ACCTS; i++) {
		char k[32];
		Acct *x = &M.accts[M.nacct];
		memset(x, 0, sizeof *x);
		Account *a = &x->a;
#define GET(field, key) fmt(k, sizeof k, "%d." key, i), str_copy(a->field, conf_get(c, k, ""), sizeof a->field)
		GET(name, "name");
		GET(email, "email");
		GET(password, "password");
		GET(imap_host, "imap");
		GET(smtp_host, "smtp");
#undef GET
		fmt(k, sizeof k, "%d.imap_port", i);
		a->imap_port = (u16)conf_get_int(c, k, 993);
		fmt(k, sizeof k, "%d.smtp_port", i);
		a->smtp_port = (u16)conf_get_int(c, k, 465);
		if (!a->email[0] || !a->imap_host[0])
			continue;
		x->id = ++M.next_id;
		acct_cache_dir(x);
		M.nacct++;
	}
	conf_free(c);
	path = config_file("trusted.conf");
	c = conf_load(path);
	mem_free(path);
	for (usize i = 0; i < c->keys.len; i++)
		map_set(&M.trusted, c->keys.data[i], (void *)1);
	conf_free(c);
}

static void trust_sender(const char *addr)
{
	char *path = config_file("trusted.conf");
	Conf *c = conf_load(path);
	char low[128];
	str_copy(low, addr, sizeof low);
	str_lower(low);
	conf_set(c, low, "yes");
	conf_save(c);
	conf_free(c);
	mem_free(path);
	map_set(&M.trusted, low, (void *)1);
}

static bool sender_trusted(const char *addr)
{
	char low[128];
	str_copy(low, addr, sizeof low);
	str_lower(low);
	return map_get(&M.trusted, low) != NULL;
}

static bool use_24h(void)
{
	char *path = path_join(home_dir(), ".config/zurbax/settings.conf");
	Conf *c = conf_load(path);
	mem_free(path);
	bool v = conf_get_bool(c, "clock_24h", true);
	conf_free(c);
	return v;
}

/* Heads cache: a count and the records as they are in memory. */
static void save_heads(const char *cache, const char *folder, const Head *h, int n)
{
	char *path = heads_file(cache, folder);
	StrBuf sb = { 0 };
	u32 hdr[2] = { 0x5a4d4831, (u32)n };
	sb_add(&sb, hdr, sizeof hdr);
	sb_add(&sb, h, sizeof(Head) * (usize)n);
	file_write_atomic(path, sb.data, sb.len);
	sb_free(&sb);
	mem_free(path);
}

static Head *load_heads(const char *cache, const char *folder, int *n)
{
	*n = 0;
	char *path = heads_file(cache, folder);
	usize len;
	char *data = file_read(path, &len);
	mem_free(path);
	if (!data)
		return NULL;
	u32 hdr[2];
	Head *out = NULL;
	if (len >= sizeof hdr) {
		memcpy(hdr, data, sizeof hdr);
		if (hdr[0] == 0x5a4d4831 && len == sizeof hdr + sizeof(Head) * hdr[1] && hdr[1] <= LIST_MAX) {
			out = mem_alloc(sizeof(Head) * MAX(1, hdr[1]));
			memcpy(out, data + sizeof hdr, sizeof(Head) * hdr[1]);
			*n = (int)hdr[1];
			/* a damaged file must not leave strings unterminated */
			for (int i = 0; i < *n; i++) {
				Head *h = &out[i];
				h->from_name[sizeof h->from_name - 1] = h->from_addr[sizeof h->from_addr - 1] = 0;
				h->to[sizeof h->to - 1] = h->subject[sizeof h->subject - 1] = 0;
				h->snippet[sizeof h->snippet - 1] = h->message_id[sizeof h->message_id - 1] = 0;
			}
		}
	}
	mem_free(data);
	return out;
}

/* ---- The worker ------------------------------------------------------------------------------------ */

static struct {
	int id;
	Imap *m;
	u64 used;                /* ms */
} conns[MAX_ACCTS * 2];

static void post(Result *r)
{
	mutex_lock(&M.rlock);
	if (M.rtail)
		M.rtail->next = r;
	else
		M.rhead = r;
	M.rtail = r;
	mutex_unlock(&M.rlock);
	ui_wake(M.ui);
}

static Result *result_for(Job *j, bool ok)
{
	Result *r = NEW(Result);
	r->job = j->kind;
	r->acct_id = j->acct_id;
	r->gen = j->gen;
	r->ok = ok;
	r->quiet = j->quiet;
	r->uid = j->uid;
	str_copy(r->folder, j->folder, sizeof r->folder);
	return r;
}

static Imap **conn_slot(int id)
{
	for (int i = 0; i < (int)ARRAY_LEN(conns); i++)
		if (conns[i].id == id && conns[i].m)
			return &conns[i].m;
	for (int i = 0; i < (int)ARRAY_LEN(conns); i++)
		if (!conns[i].m) {
			conns[i].id = id;
			return &conns[i].m;
		}
	return NULL;
}

static void conn_drop(int id)
{
	for (int i = 0; i < (int)ARRAY_LEN(conns); i++)
		if (conns[i].id == id && conns[i].m) {
			imap_close(conns[i].m);
			conns[i].m = NULL;
		}
}

/* The open connection for a job's account, opening one if needed. */
static Imap *conn_get(Job *j, char *err, usize cap)
{
	Imap **slot = conn_slot(j->acct_id);
	if (!slot) {
		str_copy(err, "too many connections", cap);
		return NULL;
	}
	int idx = (int)(((char *)slot - (char *)&conns[0].m) / (isize)sizeof conns[0]);
	u64 now = time_mono_ms();
	/* connections left alone for long are often dropped silently by routers */
	if (*slot && (imap_dead(*slot) || now - conns[idx].used > 10 * 60 * 1000)) {
		imap_close(*slot);
		*slot = NULL;
	}
	if (!*slot)
		*slot = imap_open(&j->account, err, cap);
	conns[idx].used = now;
	return *slot;
}

/* Runs an IMAP step; if the connection had died, reconnects and tries once more. */
#define WITH_IMAP(j, r, call)                                                     \
	do {                                                                      \
		for (int try_ = 0; try_ < 2; try_++) {                            \
			Imap *m = conn_get(j, r->error, sizeof r->error);         \
			if (!m)                                                   \
				break;                                            \
			r->error[0] = 0;                                          \
			call;                                                     \
			if (r->ok || !imap_dead(m)) {                             \
				if (!r->ok && !r->error[0])                       \
					str_copy(r->error, imap_error(m), sizeof r->error); \
				break;                                            \
			}                                                         \
			conn_drop(j->acct_id);                                    \
			str_copy(r->error, "The connection to the mail server broke.", sizeof r->error); \
		}                                                                 \
	} while (0)

static void run_job(Job *j)
{
	Result *r = result_for(j, false);
	switch (j->kind) {
	case J_TEST: {
		Imap *m = imap_open(&j->account, r->error, sizeof r->error);
		r->ok = m != NULL;
		imap_close(m);
		r->account = j->account;
		break;
	}
	case J_CONNECT: {
		conn_drop(j->acct_id);
		Folder *f = mem_alloc(sizeof(Folder) * MAX_FOLDERS);
		WITH_IMAP(j, r, {
			r->nfolders = imap_folders(m, f, MAX_FOLDERS);
			r->ok = r->nfolders > 0;
			for (int i = 0; i < r->nfolders; i++)
				if (str_eq(f[i].role, "inbox"))
					f[i].unseen = imap_unseen(m, f[i].raw);
		});
		r->folders = f;
		break;
	}
	case J_HEADS:
	case J_SEARCH: {
		Head *h = mem_alloc(sizeof(Head) * LIST_MAX);
		WITH_IMAP(j, r, {
			int n = j->kind == J_HEADS ? imap_heads(m, j->folder, h, LIST_MAX, &r->exists) : imap_search(m, j->folder, j->query, h, LIST_MAX);
			r->ok = n >= 0;
			r->nheads = MAX(0, n);
			r->unseen = r->ok && j->kind == J_HEADS ? imap_unseen(m, j->folder) : -1;
		});
		r->heads = h;
		if (r->ok && j->kind == J_HEADS)
			save_heads(j->cache, j->folder, h, r->nheads);
		break;
	}
	case J_CHECK:
		WITH_IMAP(j, r, {
			r->unseen = imap_unseen(m, "INBOX");
			r->ok = r->unseen >= 0;
		});
		break;
	case J_BODY:
		WITH_IMAP(j, r, {
			r->raw = imap_message(m, j->folder, j->uid, &r->len);
			r->ok = r->raw != NULL;
		});
		if (r->ok) {
			char *path = message_file(j->cache, j->folder, j->uid);
			file_write_atomic(path, r->raw, r->len);
			mem_free(path);
		}
		break;
	case J_FLAG:
		WITH_IMAP(j, r, r->ok = imap_flag(m, j->folder, j->uid, j->flag, j->on));
		break;
	case J_MOVE:
		WITH_IMAP(j, r, r->ok = imap_move(m, j->folder, j->uid, j->to));
		break;
	case J_DELETE:
		WITH_IMAP(j, r, r->ok = imap_delete(m, j->folder, j->uid));
		break;
	case J_SEND:
		r->ok = smtp_send(&j->account, (const char *const *)j->rcpt, j->nrcpt, j->data, j->len, r->error, sizeof r->error);
		if (r->ok) {
			/* filing it in Sent and dropping the draft are nice to have; sending was the point */
			Result scratch = { 0 };
			Result *s = &scratch;
			Imap *probe = conn_get(j, s->error, sizeof s->error);
			if (probe) {
				if (j->sent[0] && !imap_is_gmail(probe))
					WITH_IMAP(j, s, s->ok = imap_append(m, j->sent, "\\Seen", j->data, j->len));
				if (j->old_draft && j->drafts[0])
					WITH_IMAP(j, s, s->ok = imap_delete(m, j->drafts, j->old_draft));
				if (j->uid && j->folder[0])
					WITH_IMAP(j, s, s->ok = imap_flag(m, j->folder, j->uid, "\\Answered", true));
			}
		}
		break;
	case J_DRAFT:
		WITH_IMAP(j, r, {
			r->ok = imap_append(m, j->drafts, "\\Seen \\Draft", j->data, j->len);
			if (r->ok && j->old_draft)
				imap_delete(m, j->drafts, j->old_draft);
		});
		break;
	}
	post(r);
}

static void job_free(Job *j)
{
	mem_free(j->data);
	for (int i = 0; i < j->nrcpt; i++)
		mem_free(j->rcpt[i]);
	mem_free(j->rcpt);
	mem_free(j);
}

static void worker(void *arg)
{
	for (;;) {
		int seen = atomic_load(&M.qwake);
		mutex_lock(&M.qlock);
		Job *j = M.qhead;
		if (j) {
			M.qhead = j->next;
			if (!M.qhead)
				M.qtail = NULL;
		}
		mutex_unlock(&M.qlock);
		if (!j) {
			futex_wait(&M.qwake, seen, -1);
			continue;
		}
		run_job(j);
		job_free(j);
		atomic_add(&M.busy, -1);
		ui_wake(M.ui);
	}
}

static Job *job_new(int kind, Acct *a)
{
	Job *j = NEW(Job);
	j->kind = kind;
	if (a) {
		j->acct_id = a->id;
		j->account = a->a;
		str_copy(j->cache, a->cache, sizeof j->cache);
	}
	return j;
}

static void submit(Job *j)
{
	atomic_add(&M.busy, 1);
	mutex_lock(&M.qlock);
	if (M.qtail)
		M.qtail->next = j;
	else
		M.qhead = j;
	M.qtail = j;
	mutex_unlock(&M.qlock);
	atomic_add(&M.qwake, 1);
	futex_wake(&M.qwake, 1);
}

static void result_free(Result *r)
{
	mem_free(r->folders);
	mem_free(r->heads);
	mem_free(r->raw);
	mem_free(r);
}

/* ---- Small helpers ------------------------------------------------------------------------------- */

static Acct *cur_acct(void)
{
	return M.nacct ? &M.accts[M.cur] : NULL;
}

static int acct_index(int id)
{
	for (int i = 0; i < M.nacct; i++)
		if (M.accts[i].id == id)
			return i;
	return -1;
}

static void toast(const char *text, bool undo)
{
	str_copy(M.toast, text, sizeof M.toast);
	M.toast_until = M.ui->now + (undo ? UNDO_SECONDS : 4);
	M.toast_undo = undo;
}

static Folder *folder_by_role(Acct *a, const char *role)
{
	if (!a)
		return NULL;
	for (int i = 0; i < a->nfolders; i++)
		if (str_eq(a->folders[i].role, role))
			return &a->folders[i];
	return NULL;
}

static Folder *folder_by_raw(Acct *a, const char *raw)
{
	if (!a)
		return NULL;
	for (int i = 0; i < a->nfolders; i++)
		if (str_eq(a->folders[i].raw, raw))
			return &a->folders[i];
	return NULL;
}

static const char *cur_role(void)
{
	Folder *f = folder_by_raw(cur_acct(), M.folder);
	if (f)
		return f->role;
	return str_eq_nocase(M.folder, "INBOX") ? "inbox" : "";
}

static bool is_gmail(Acct *a)
{
	return a && str_find_nocase(a->a.imap_host, "gmail") != NULL;
}

static const char *folder_title(Acct *a, const Folder *f)
{
	const char *r = f->role;
	if (str_eq(r, "inbox")) return "Inbox";
	if (str_eq(r, "flagged")) return "Starred";
	if (str_eq(r, "drafts")) return "Drafts";
	if (str_eq(r, "sent")) return "Sent";
	if (str_eq(r, "archive")) return is_gmail(a) ? "All Mail" : "Archive";
	if (str_eq(r, "junk")) return is_gmail(a) ? "Spam" : "Junk";
	if (str_eq(r, "trash")) return "Trash";
	const char *n = f->name;
	if (str_starts(n, "[Gmail]/") || str_starts(n, "[Google Mail]/"))
		n = str_chr(n, '/') + 1;
	else if (str_starts(n, "INBOX.") || str_starts(n, "INBOX/"))
		n += 6;
	return n;
}

static const char *folder_icon(const Folder *f)
{
	const char *r = f->role;
	if (str_eq(r, "inbox")) return "inbox";
	if (str_eq(r, "flagged")) return "starred";
	if (str_eq(r, "drafts")) return "mail-drafts";
	if (str_eq(r, "sent")) return "mail-send";
	if (str_eq(r, "archive")) return "mail-archive";
	if (str_eq(r, "junk")) return "mail-junk";
	if (str_eq(r, "trash")) return "trash";
	if (str_find_nocase(f->name, "Important")) return "bookmark";
	return "folder";
}

static int role_rank(const char *r)
{
	static const char *const order[] = { "inbox", "flagged", "drafts", "sent", "archive", "junk", "trash" };
	for (int i = 0; i < (int)ARRAY_LEN(order); i++)
		if (str_eq(r, order[i]))
			return i;
	return 100;
}

static void sort_folders(Acct *a)
{
	for (int i = 1; i < a->nfolders; i++) {
		Folder t = a->folders[i];
		int j = i - 1;
		while (j >= 0) {
			int ra = role_rank(a->folders[j].role), rb = role_rank(t.role);
			bool after = ra > rb || (ra == rb && ra == 100 && str_cmp_nocase(a->folders[j].name, t.name) > 0);
			if (!after)
				break;
			a->folders[j + 1] = a->folders[j];
			j--;
		}
		a->folders[j + 1] = t;
	}
}

static void fmt_hm(char *out, usize cap, const Civil *c)
{
	if (M.h24)
		fmt(out, cap, "%02d:%02d", c->hour, c->min);
	else
		fmt(out, cap, "%d:%02d %s", c->hour % 12 ? c->hour % 12 : 12, c->min, c->hour < 12 ? "AM" : "PM");
}

static void mon3(char *out, int month)
{
	str_copy(out, MONTH_NAMES[(month - 1) % 12], 4);
}

static void short_date(i64 t, char *out, usize cap)
{
	if (t <= 0) {
		out[0] = 0;
		return;
	}
	Civil c, now;
	civil_local(t, &c);
	civil_local(time_unix(), &now);
	char m[4];
	mon3(m, c.month);
	if (c.year == now.year && c.yday == now.yday)
		fmt_hm(out, cap, &c);
	else if (c.year == now.year)
		fmt(out, cap, "%s %d", m, c.day);
	else
		fmt(out, cap, "%s %d, %d", m, c.day, c.year);
}

static void long_date_ex(i64 t, char *out, usize cap, bool relative);

static void long_date(i64 t, char *out, usize cap)
{
	long_date_ex(t, out, cap, true);
}

static void long_date_ex(i64 t, char *out, usize cap, bool relative)
{
	if (t <= 0) {
		out[0] = 0;
		return;
	}
	Civil c;
	civil_local(t, &c);
	char hm[16], m[4], ago[48] = "";
	fmt_hm(hm, sizeof hm, &c);
	mon3(m, c.month);
	i64 d = relative ? time_unix() - t : -1;
	if (d >= 0 && d < 60)
		str_copy(ago, " (just now)", sizeof ago);
	else if (d >= 0 && d < 3600)
		fmt(ago, sizeof ago, " (%d minute%s ago)", (int)(d / 60), d / 60 == 1 ? "" : "s");
	else if (d >= 0 && d < 86400)
		fmt(ago, sizeof ago, " (%d hour%s ago)", (int)(d / 3600), d / 3600 == 1 ? "" : "s");
	else if (d >= 0 && d < 7 * 86400)
		fmt(ago, sizeof ago, " (%d day%s ago)", (int)(d / 86400), d / 86400 == 1 ? "" : "s");
	fmt(out, cap, "%.3s, %s %d, %d, %s%s", WEEKDAY_NAMES[c.wday], m, c.day, c.year, hm, ago);
}

/* RFC 5322 date in local time: "Tue, 06 Oct 2026 10:12:33 +0200". */
static void mail_date(char *out, usize cap)
{
	i64 now = time_unix();
	i32 off = tz_offset(now);
	Civil c;
	civil_from_unix(now + off, &c);
	char m[4];
	mon3(m, c.month);
	int ao = off < 0 ? -off : off;
	fmt(out, cap, "%.3s, %02d %s %d %02d:%02d:%02d %c%02d%02d", WEEKDAY_NAMES[c.wday], c.day, m, c.year, c.hour, c.min, c.sec,
	    off < 0 ? '-' : '+', ao / 3600, (ao % 3600) / 60);
}

static const char *display_name(const char *name, const char *addr)
{
	return name && name[0] ? name : addr && addr[0] ? addr : "(unknown)";
}

/* Splits "A <a@x>, b@y; "C, D" <c@z>" into single addresses. */
static int split_addresses(const char *s, char out[][256], int max)
{
	int n = 0;
	const char *p = s;
	while (*p && n < max) {
		const char *start = p;
		bool q = false;
		int angle = 0;
		while (*p && (q || angle || (*p != ',' && *p != ';'))) {
			if (*p == '"')
				q = !q;
			else if (*p == '<' && !q)
				angle++;
			else if (*p == '>' && !q && angle)
				angle--;
			p++;
		}
		usize len = (usize)(p - start);
		char item[256];
		str_copy(item, start, MIN(len + 1, sizeof item));
		char *t = str_trim(item);
		if (*t)
			str_copy(out[n++], t, 256);
		if (*p)
			p++;
	}
	return n;
}

/* A trimmed copy (text in a StrBuf must not be trimmed in place). */
static char *trim_copy(const char *s, char *out, usize cap)
{
	while (is_space(*s))
		s++;
	str_copy(out, s, cap);
	usize n = strlen(out);
	while (n && is_space(out[n - 1]))
		out[--n] = 0;
	return out;
}

static bool blank(const char *s)
{
	while (is_space(*s))
		s++;
	return !*s;
}

static bool valid_addr(const char *a)
{
	const char *at = str_chr(a, '@');
	for (const char *p = a; *p; p++)
		if (is_space(*p) || *p == '/' || *p == '\\' || *p == '<' || *p == '>' || *p == ',' || *p == '"' || (u8)*p < 0x20)
			return false;
	return at && at > a && at[1] && str_chr(at, '.') && !str_chr(at + 1, '@');
}

static u64 compose_hash(Compose *c)
{
	u64 h = 1469598103934665603ull;
	StrBuf *parts[] = { &c->to, &c->cc, &c->bcc, &c->subject, &c->body };
	for (int i = 0; i < 5; i++)
		h = h * 31 + hash_bytes(sb_str(parts[i]), parts[i]->len);
	return h * 31 + (u64)c->natts;
}

static bool compose_dirty(Compose *c)
{
	return compose_hash(c) != c->start_hash;
}

static const char *guess_type(const char *name)
{
	static const char *const map[][2] = {
		{ "pdf", "application/pdf" }, { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
		{ "gif", "image/gif" }, { "webp", "image/webp" }, { "svg", "image/svg+xml" }, { "txt", "text/plain" },
		{ "html", "text/html" }, { "csv", "text/csv" }, { "json", "application/json" }, { "zip", "application/zip" },
		{ "mp3", "audio/mpeg" }, { "ogg", "audio/ogg" }, { "flac", "audio/flac" }, { "wav", "audio/wav" },
		{ "mp4", "video/mp4" }, { "doc", "application/msword" }, { "xls", "application/vnd.ms-excel" },
		{ "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
		{ "xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
		{ "pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
		{ "odt", "application/vnd.oasis.opendocument.text" }, { "ics", "text/calendar" },
	};
	const char *ext = path_ext(name);
	for (int i = 0; i < (int)ARRAY_LEN(map); i++)
		if (str_eq_nocase(ext, map[i][0]))
			return map[i][1];
	return "application/octet-stream";
}

/* ---- Writing messages ---------------------------------------------------------------------------- */

/* Header text on one line: no breaks that could start a header of their own. */
static void one_line(char *s)
{
	for (; *s; s++)
		if ((u8)*s < 0x20 || *s == 0x7f)
			*s = ' ';
}

/* "Name <addr>" with the name quoted or encoded as needed. */
static void add_address(StrBuf *sb, const char *name, const char *addr)
{
	if (!name[0]) {
		sb_adds(sb, addr);
		return;
	}
	bool ascii = true, special = false;
	for (const char *p = name; *p; p++) {
		if ((u8)*p >= 0x80)
			ascii = false;
		if (str_chr("()<>[]:;@\\,.\"", *p))
			special = true;
	}
	if (!ascii) {
		char *e = mime_encode_header(name);
		sb_adds(sb, e);
		mem_free(e);
	} else if (special) {
		sb_addc(sb, '"');
		for (const char *p = name; *p; p++) {
			if (*p == '"' || *p == '\\')
				sb_addc(sb, '\\');
			sb_addc(sb, *p);
		}
		sb_addc(sb, '"');
	} else {
		sb_adds(sb, name);
	}
	sb_fmt(sb, " <%s>", addr);
}

static void add_address_header(StrBuf *sb, const char *header, const char *list, char ***rcpt, int *nrcpt)
{
	char items[64][256];
	int n = split_addresses(list, items, 64);
	if (!n)
		return;
	if (header)
		sb_fmt(sb, "%s: ", header);
	for (int i = 0; i < n; i++) {
		char name[96], addr[128];
		one_line(items[i]);
		mime_address(items[i], name, sizeof name, addr, sizeof addr);
		if (header) {
			if (i)
				sb_adds(sb, ",\r\n ");
			add_address(sb, name, addr);
		}
		if (rcpt) {
			*rcpt = mem_realloc(*rcpt, sizeof(char *) * (usize)(*nrcpt + 1));
			(*rcpt)[(*nrcpt)++] = str_dup(addr);
		}
	}
	if (header)
		sb_adds(sb, "\r\n");
}

static void add_base64(StrBuf *sb, const u8 *data, usize len)
{
	char *b = base64_encode(data, len);
	usize n = strlen(b);
	for (usize i = 0; i < n; i += 76) {
		sb_add(sb, b + i, MIN(76, n - i));
		sb_adds(sb, "\r\n");
	}
	mem_free(b);
}

/* Text with every line ending as CRLF. */
static char *crlf(const char *s)
{
	StrBuf sb = { 0 };
	for (; *s; s++) {
		if (*s == '\r')
			continue;
		if (*s == '\n')
			sb_addc(&sb, '\r');
		sb_addc(&sb, *s);
	}
	return sb_take(&sb);
}

static char *build_message(Compose *c, const Account *a, char ***rcpt, int *nrcpt, usize *len)
{
	one_line(c->in_reply_to);
	one_line(c->references);
	char subject[1024];
	str_copy(subject, sb_str(&c->subject), sizeof subject);
	one_line(subject);
	StrBuf sb = { 0 };
	char date[64];
	mail_date(date, sizeof date);
	u8 rnd[12];
	rand_bytes(rnd, sizeof rnd);
	char hex[32];
	hex_encode(rnd, sizeof rnd, hex);
	const char *domain = str_chr(a->email, '@') ? str_chr(a->email, '@') + 1 : "zurbax.local";
	sb_adds(&sb, "From: ");
	add_address(&sb, a->name, a->email);
	sb_adds(&sb, "\r\n");
	add_address_header(&sb, "To", sb_str(&c->to), rcpt, nrcpt);
	add_address_header(&sb, "Cc", sb_str(&c->cc), rcpt, nrcpt);
	add_address_header(&sb, NULL, sb_str(&c->bcc), rcpt, nrcpt);
	char *subj = mime_encode_header(subject);
	sb_fmt(&sb, "Subject: %s\r\nDate: %s\r\nMessage-ID: <%s.%lld@%s>\r\n", subj, date, hex, (long long)time_unix(), domain);
	mem_free(subj);
	if (c->in_reply_to[0])
		sb_fmt(&sb, "In-Reply-To: %s\r\n", c->in_reply_to);
	if (c->references[0])
		sb_fmt(&sb, "References: %s\r\n", c->references);
	sb_adds(&sb, "MIME-Version: 1.0\r\nUser-Agent: Zurbax Mail\r\n");
	char *text = crlf(sb_str(&c->body));
	char *qp = mime_quoted_printable(text, strlen(text));
	mem_free(text);
	const char *text_headers = "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n";
	if (!c->natts) {
		sb_fmt(&sb, "%s\r\n%s\r\n", text_headers, qp);
	} else {
		char boundary[48];
		fmt(boundary, sizeof boundary, "=_zurbax_%s", hex);
		sb_fmt(&sb, "Content-Type: multipart/mixed; boundary=\"%s\"\r\n\r\nThis is a message with attachments.\r\n", boundary);
		sb_fmt(&sb, "--%s\r\n%s\r\n%s\r\n", boundary, text_headers, qp);
		for (int i = 0; i < c->natts; i++) {
			Att *t = &c->atts[i];
			char *fname = mime_encode_header(t->name);
			sb_fmt(&sb, "--%s\r\nContent-Type: %s; name=\"%s\"\r\nContent-Disposition: attachment; filename=\"%s\"\r\n"
			       "Content-Transfer-Encoding: base64\r\n\r\n",
			       boundary, t->type, fname, fname);
			mem_free(fname);
			add_base64(&sb, t->data, t->len);
		}
		sb_fmt(&sb, "--%s--\r\n", boundary);
	}
	mem_free(qp);
	*len = sb.len;
	return sb_take(&sb);
}

/* ---- Opening folders and messages ------------------------------------------------------------------ */

static void close_message(void)
{
	if (M.msg) {
		atomic_store(&M.msg->doc.cancelled, 1);
		msg_release(M.msg);
	}
	M.msg = NULL;
	M.sel = 0;
	M.msg_loading = false;
	M.msg_error[0] = 0;
	M.scroll = 0;
	M.details = false;
	M.sel_text.on = false;
	M.sel_text.page = NULL;
}

static void set_heads(Head *h, int n)
{
	mem_free(M.heads);
	M.heads = h;
	M.nheads = n;
}

static void fetch_heads(bool quiet)
{
	Acct *a = cur_acct();
	if (!a || !M.folder[0])
		return;
	Job *j = job_new(J_HEADS, a);
	str_copy(j->folder, M.folder, sizeof j->folder);
	j->gen = M.heads_gen = ++M.gen;
	j->quiet = quiet;
	submit(j);
	if (!quiet)
		M.loading = true;
}

static void open_folder(const char *raw)
{
	Acct *a = cur_acct();
	if (!a)
		return;
	close_message();
	str_copy(M.folder, raw, sizeof M.folder);
	M.searching = false;
	sb_clear(&M.search);
	M.list_error[0] = 0;
	int n;
	Head *h = load_heads(a->cache, raw, &n);
	set_heads(h, n);
	M.exists = (u32)n;
	ui_scroll_to(M.ui, "list", 0);
	fetch_heads(false);
	M.sidebar_open = false;
}

static void switch_account(int i)
{
	if (i < 0 || i >= M.nacct)
		return;
	M.cur = i;
	Folder *inbox = folder_by_role(&M.accts[i], "inbox");
	open_folder(inbox ? inbox->raw : "INBOX");
}

static void connect_account(Acct *a)
{
	a->state = AS_CONNECTING;
	submit(job_new(J_CONNECT, a));
}

static MsgColors msg_colors(void)
{
	Theme *t = &M.ui->theme;
	return (MsgColors){ .text = t->text, .link = t->accent, .background = t->view, .quote = t->text_dim };
}

static Head *head_by_uid(u32 uid, int *index)
{
	for (int i = 0; i < M.nheads; i++)
		if (M.heads[i].uid == uid) {
			if (index)
				*index = i;
			return &M.heads[i];
		}
	return NULL;
}

static void set_flag(Head *h, const char *flag, bool on)
{
	Acct *a = cur_acct();
	Job *j = job_new(J_FLAG, a);
	str_copy(j->folder, M.folder, sizeof j->folder);
	str_copy(j->flag, flag, sizeof j->flag);
	j->uid = h->uid;
	j->on = on;
	submit(j);
	Folder *f = folder_by_raw(a, M.folder);
	if (str_eq(flag, "\\Seen")) {
		if (h->seen != on && f)
			f->unseen = MAX(0, f->unseen + (on ? -1 : 1));
		h->seen = on;
	} else if (str_eq(flag, "\\Flagged")) {
		h->flagged = on;
	}
}

static void compose_from_message(Msg *m, u32 uid);
static void show_message(Msg *m);

static void open_message(u32 uid)
{
	Acct *a = cur_acct();
	Head *h = head_by_uid(uid, NULL);
	if (!a || !h)
		return;
	close_message();
	M.sel = uid;
	M.sel_text.on = false;
	if (!h->seen && !str_eq(cur_role(), "drafts"))
		set_flag(h, "\\Seen", true);
	char *path = message_file(a->cache, M.folder, uid);
	usize len;
	char *raw = file_read(path, &len);
	mem_free(path);
	if (raw) {
		MsgColors c = msg_colors();
		show_message(msg_new(raw, len, &c));
		return;
	}
	Job *j = job_new(J_BODY, a);
	str_copy(j->folder, M.folder, sizeof j->folder);
	j->uid = uid;
	j->gen = M.msg_gen = ++M.gen;
	submit(j);
	M.msg_loading = true;
}

static void show_message(Msg *m)
{
	M.msg_loading = false;
	if (str_eq(cur_role(), "drafts")) {
		/* drafts open where they can be finished */
		compose_from_message(m, M.sel);
		msg_release(m);
		close_message();
		return;
	}
	M.msg = m;
	if (sender_trusted(m->from_addr))
		m->allow_remote = true;
}

/* Moves (or deletes) the open or given message after a moment, so it can be undone. */
static void commit_pending(void)
{
	Pending *p = &M.pend;
	if (p->kind == PEND_NONE)
		return;
	int ai = acct_index(p->acct_id);
	if (ai >= 0) {
		Job *j = job_new(p->kind == PEND_MOVE ? J_MOVE : J_DELETE, &M.accts[ai]);
		str_copy(j->folder, p->folder, sizeof j->folder);
		str_copy(j->to, p->to, sizeof j->to);
		j->uid = p->head.uid;
		submit(j);
	}
	p->kind = PEND_NONE;
}

static void undo_pending(void)
{
	Pending *p = &M.pend;
	if (p->kind == PEND_NONE)
		return;
	Acct *a = cur_acct();
	if (a && a->id == p->acct_id && str_eq(M.folder, p->folder) && !head_by_uid(p->head.uid, NULL)) {
		M.heads = mem_realloc(M.heads, sizeof(Head) * (usize)(M.nheads + 1));
		int at = MIN(p->index, M.nheads);
		memmove(&M.heads[at + 1], &M.heads[at], sizeof(Head) * (usize)(M.nheads - at));
		M.heads[at] = p->head;
		M.nheads++;
		Folder *f = folder_by_raw(a, M.folder);
		if (f && !p->head.seen)
			f->unseen++;
	}
	p->kind = PEND_NONE;
	M.toast_until = 0;
}

static void move_message(u32 uid, const char *to, const char *done)
{
	Acct *a = cur_acct();
	int idx;
	Head *h = head_by_uid(uid, &idx);
	if (!a || !h)
		return;
	commit_pending();
	Pending *p = &M.pend;
	p->kind = to ? PEND_MOVE : PEND_DELETE;
	p->acct_id = a->id;
	str_copy(p->folder, M.folder, sizeof p->folder);
	str_copy(p->to, to ? to : "", sizeof p->to);
	p->head = *h;
	p->index = idx;
	p->deadline = M.ui->now + UNDO_SECONDS;
	Folder *f = folder_by_raw(a, M.folder);
	if (f && !h->seen)
		f->unseen = MAX(0, f->unseen - 1);
	/* the next message takes its place, as in Gmail */
	u32 next = idx + 1 < M.nheads ? M.heads[idx + 1].uid : idx > 0 ? M.heads[idx - 1].uid : 0;
	bool was_open = M.sel == uid;
	memmove(&M.heads[idx], &M.heads[idx + 1], sizeof(Head) * (usize)(M.nheads - idx - 1));
	M.nheads--;
	if (was_open) {
		close_message();
		if (next && M.ui->width >= 1120)
			open_message(next);
	}
	toast(done, true);
}

static void archive_message(u32 uid)
{
	Folder *f = folder_by_role(cur_acct(), "archive");
	if (f && !str_eq(f->raw, M.folder))
		move_message(uid, f->raw, "Conversation archived.");
}

static void delete_message(u32 uid)
{
	Folder *trash = folder_by_role(cur_acct(), "trash");
	if (trash && !str_eq(trash->raw, M.folder))
		move_message(uid, trash->raw, "Moved to Trash.");
	else
		move_message(uid, NULL, "Deleted forever.");
}

static void junk_message(u32 uid)
{
	Acct *a = cur_acct();
	if (str_eq(cur_role(), "junk")) {
		Folder *inbox = folder_by_role(a, "inbox");
		move_message(uid, inbox ? inbox->raw : "INBOX", "Moved to Inbox.");
		return;
	}
	Folder *f = folder_by_role(a, "junk");
	if (f)
		move_message(uid, f->raw, is_gmail(a) ? "Marked as spam." : "Moved to Junk.");
}

/* ---- Composing --------------------------------------------------------------------------------------- */

static void compose_free(Compose *c)
{
	if (!c)
		return;
	sb_free(&c->to);
	sb_free(&c->cc);
	sb_free(&c->bcc);
	sb_free(&c->subject);
	sb_free(&c->body);
	for (int i = 0; i < c->natts; i++)
		mem_free(c->atts[i].data);
	mem_free(c);
}

static Compose *compose_new(const char *title)
{
	if (M.compose)
		compose_free(M.compose);
	Compose *c = NEW(Compose);
	c->acct = M.cur;
	str_copy(c->title, title, sizeof c->title);
	M.compose = c;
	M.sidebar_open = false;
	ui_request_frame(M.ui);
	return c;
}

static void compose_ready(Compose *c)
{
	c->start_hash = compose_hash(c);
	c->frames = 0;
}

static void add_att(Compose *c, const char *name, const char *type, u8 *data, usize len)
{
	if (c->natts >= (int)ARRAY_LEN(c->atts)) {
		mem_free(data);
		return;
	}
	Att *t = &c->atts[c->natts++];
	str_copy(t->name, name, sizeof t->name);
	str_copy(t->type, type && type[0] ? type : guess_type(name), sizeof t->type);
	t->data = data;
	t->len = len;
}

static void compose_to(const char *addr)
{
	Compose *c = compose_new("New message");
	sb_adds(&c->to, addr);
	c->focus_body = addr[0] != 0;
	compose_ready(c);
}

static bool same_addr(const char *a, const char *b)
{
	return str_eq_nocase(a, b);
}

static void quote_into(StrBuf *sb, const char *text)
{
	const char *p = text ? text : "";
	while (*p) {
		const char *eol = str_chr(p, '\n');
		usize n = eol ? (usize)(eol - p) : strlen(p);
		sb_adds(sb, n && p[0] == '>' ? ">" : "> ");
		sb_add(sb, p, n);
		sb_addc(sb, '\n');
		p += n;
		if (*p)
			p++;
	}
}

static void subject_with(StrBuf *out, const char *prefix, const char *subject)
{
	usize pl = strlen(prefix);
	if (!str_eq_nocase_n(subject, prefix, pl))
		sb_fmt(out, "%s ", prefix);
	sb_adds(out, subject);
}

enum { REPLY, REPLY_ALL, FORWARD };

static void reply(Msg *m, int kind)
{
	Acct *a = cur_acct();
	if (!m || !a)
		return;
	Compose *c = compose_new(kind == FORWARD ? "Forward" : kind == REPLY_ALL ? "Reply all" : "Reply");
	char who[256];
	fmt(who, sizeof who, "%s <%s>", display_name(m->from_name, m->from_addr), m->from_addr);
	if (kind != FORWARD) {
		const char *to = m->reply_to[0] ? m->reply_to : who;
		/* replying to your own sent mail goes to its recipients */
		if (same_addr(m->from_addr, a->a.email) && m->to[0])
			to = m->to;
		sb_adds(&c->to, to);
		if (kind == REPLY_ALL) {
			char items[64][256];
			StrBuf all = { 0 };
			sb_fmt(&all, "%s, %s", m->to, m->cc);
			int n = split_addresses(sb_str(&all), items, 64);
			sb_free(&all);
			char first[96], first_addr[128];
			mime_address(to, first, sizeof first, first_addr, sizeof first_addr);
			for (int i = 0; i < n; i++) {
				char nm[96], ad[128];
				mime_address(items[i], nm, sizeof nm, ad, sizeof ad);
				if (!ad[0] || same_addr(ad, a->a.email) || same_addr(ad, first_addr) || str_find_nocase(sb_str(&c->cc), ad))
					continue;
				if (c->cc.len)
					sb_adds(&c->cc, ", ");
				sb_adds(&c->cc, items[i]);
			}
			c->show_cc = c->cc.len > 0;
		}
		subject_with(&c->subject, "Re:", m->subject);
		str_copy(c->in_reply_to, m->message_id, sizeof c->in_reply_to);
		c->answers = M.sel;
		str_copy(c->answers_folder, M.folder, sizeof c->answers_folder);
		fmt(c->references, sizeof c->references, "%s%s%s", m->references, m->references[0] ? " " : "", m->message_id);
		char when[96] = "";
		if (m->date > 0) {
			Civil cv;
			civil_local(m->date, &cv);
			char hm[16], mo[4];
			fmt_hm(hm, sizeof hm, &cv);
			mon3(mo, cv.month);
			fmt(when, sizeof when, "On %.3s, %s %d, %d at %s, ", WEEKDAY_NAMES[cv.wday], mo, cv.day, cv.year, hm);
		}
		sb_fmt(&c->body, "\n\n%s%s wrote:\n", when, who);
		quote_into(&c->body, m->text);
		c->focus_body = true;
	} else {
		subject_with(&c->subject, "Fwd:", m->subject);
		char date[96];
		long_date_ex(m->date, date, sizeof date, false);
		sb_fmt(&c->body, "\n\n---------- Forwarded message ---------\nFrom: %s\nDate: %s\nSubject: %s\nTo: %s\n", who, date, m->subject, m->to);
		if (m->cc[0])
			sb_fmt(&c->body, "Cc: %s\n", m->cc);
		sb_fmt(&c->body, "\n%s\n", m->text ? m->text : "");
		for (int i = 0; i < m->natts; i++) {
			usize len;
			char *data = mime_body(m->atts[i], &len);
			add_att(c, m->atts[i]->filename[0] ? m->atts[i]->filename : "attachment", m->atts[i]->type, (u8 *)data, len);
		}
	}
	compose_ready(c);
}

static void compose_from_message(Msg *m, u32 uid)
{
	Compose *c = compose_new("Draft");
	sb_adds(&c->to, m->to);
	sb_adds(&c->cc, m->cc);
	c->show_cc = m->cc[0] != 0;
	sb_adds(&c->subject, m->subject);
	sb_adds(&c->body, m->text ? m->text : "");
	str_copy(c->references, m->references, sizeof c->references);
	char *irt = mime_header(m->mime->headers, m->mime->headers_len, "In-Reply-To");
	if (irt) {
		str_copy(c->in_reply_to, str_trim(irt), sizeof c->in_reply_to);
		mem_free(irt);
	}
	for (int i = 0; i < m->natts; i++) {
		usize len;
		char *data = mime_body(m->atts[i], &len);
		add_att(c, m->atts[i]->filename[0] ? m->atts[i]->filename : "attachment", m->atts[i]->type, (u8 *)data, len);
	}
	c->draft_uid = uid;
	compose_ready(c);
}

static Acct *compose_acct(Compose *c)
{
	return c->acct >= 0 && c->acct < M.nacct ? &M.accts[c->acct] : cur_acct();
}

static void save_draft(Compose *c)
{
	Acct *a = compose_acct(c);
	Folder *d = folder_by_role(a, "drafts");
	if (!a || !d)
		return;
	Job *j = job_new(J_DRAFT, a);
	j->data = build_message(c, &a->a, NULL, NULL, &j->len);
	str_copy(j->drafts, d->raw, sizeof j->drafts);
	j->old_draft = c->draft_uid;
	submit(j);
}

/* A draft that was sent, replaced or discarded leaves the list at once. */
static void drop_draft_head(Compose *c)
{
	int idx;
	if (c->draft_uid && str_eq(cur_role(), "drafts") && head_by_uid(c->draft_uid, &idx)) {
		memmove(&M.heads[idx], &M.heads[idx + 1], sizeof(Head) * (usize)(M.nheads - idx - 1));
		M.nheads--;
	}
}

static void close_compose(bool keep)
{
	Compose *c = M.compose;
	if (!c)
		return;
	bool empty = !c->to.len && !c->subject.len && blank(sb_str(&c->body)) && !c->natts;
	if (keep && compose_dirty(c) && !empty) {
		drop_draft_head(c);
		save_draft(c);
		toast(folder_by_role(compose_acct(c), "drafts") ? "Draft saved." : "Draft discarded: this account has no Drafts folder.", false);
	} else if (!keep && c->draft_uid) {
		drop_draft_head(c);
		Acct *a = compose_acct(c);
		Folder *d = folder_by_role(a, "drafts");
		if (d) {
			Job *j = job_new(J_DELETE, a);
			str_copy(j->folder, d->raw, sizeof j->folder);
			j->uid = c->draft_uid;
			submit(j);
		}
		toast("Draft discarded.", false);
	}
	compose_free(c);
	M.compose = NULL;
}

static void send_compose(void)
{
	Compose *c = M.compose;
	Acct *a = compose_acct(c);
	if (!c || !a || c->sending)
		return;
	c->error[0] = 0;
	char items[64][256];
	StrBuf all = { 0 };
	sb_fmt(&all, "%s,%s,%s", sb_str(&c->to), sb_str(&c->cc), sb_str(&c->bcc));
	int n = split_addresses(sb_str(&all), items, 64);
	sb_free(&all);
	if (!n) {
		str_copy(c->error, "Add at least one recipient.", sizeof c->error);
		return;
	}
	for (int i = 0; i < n; i++) {
		char nm[96], ad[128];
		mime_address(items[i], nm, sizeof nm, ad, sizeof ad);
		if (!valid_addr(ad)) {
			fmt(c->error, sizeof c->error, "“%s” doesn't look like an email address.", items[i]);
			return;
		}
	}
	Job *j = job_new(J_SEND, a);
	j->data = build_message(c, &a->a, &j->rcpt, &j->nrcpt, &j->len);
	Folder *sent = folder_by_role(a, "sent");
	if (sent)
		str_copy(j->sent, sent->raw, sizeof j->sent);
	Folder *drafts = folder_by_role(a, "drafts");
	if (drafts && c->draft_uid) {
		str_copy(j->drafts, drafts->raw, sizeof j->drafts);
		j->old_draft = c->draft_uid;
	}
	j->uid = c->answers;
	str_copy(j->folder, c->answers_folder, sizeof j->folder);
	j->gen = ++M.gen;
	submit(j);
	c->sending = true;
	drop_draft_head(c);
}

/* ---- Results from the worker --------------------------------------------------------------------------- */

static void notify_new(Acct *a, Head *h, int n)
{
	int fresh = 0;
	Head *last = NULL;
	for (int i = 0; i < n; i++)
		if (h[i].uid > a->inbox_top && !h[i].seen)
			fresh++, last = last ? last : &h[i];
	if (!fresh || !a->inbox_top)
		return;
	if (fresh == 1) {
		ui_notify(M.ui, display_name(last->from_name, last->from_addr), last->subject[0] ? last->subject : "(no subject)", "mail");
	} else {
		char title[64];
		fmt(title, sizeof title, "%d new messages", fresh);
		ui_notify(M.ui, title, a->a.email, "mail");
	}
}

static void take_results(void)
{
	mutex_lock(&M.rlock);
	Result *r = M.rhead;
	M.rhead = M.rtail = NULL;
	mutex_unlock(&M.rlock);
	while (r) {
		Result *next = r->next;
		int ai = acct_index(r->acct_id);
		Acct *a = ai >= 0 ? &M.accts[ai] : NULL;
		bool current = a && ai == M.cur;
		switch (r->job) {
		case J_TEST: {
			Setup *s = &M.setup;
			s->busy = false;
			if (!r->ok) {
				fmt(s->error, sizeof s->error, "Couldn't sign in: %s", r->error);
				break;
			}
			s->open = false;
			int i = s->edit >= 0 && s->edit < M.nacct ? s->edit : M.nacct;
			if (i >= MAX_ACCTS)
				break;
			Acct *x = &M.accts[i];
			if (i == M.nacct) {
				memset(x, 0, sizeof *x);
				x->id = ++M.next_id;
				M.nacct++;
			}
			/* the connect job below reconnects with the new settings (connections are the worker's) */
			x->a = r->account;
			acct_cache_dir(x);
			save_accounts();
			connect_account(x);
			M.cur = i;
			open_folder("INBOX");
			break;
		}
		case J_CONNECT:
			if (!a)
				break;
			if (!r->ok) {
				a->state = AS_FAILED;
				str_copy(a->error, r->error[0] ? r->error : "The server sent no folders.", sizeof a->error);
				if (current)
					M.loading = false;
				break;
			}
			a->state = AS_ONLINE;
			a->error[0] = 0;
			a->nfolders = MIN(r->nfolders, MAX_FOLDERS);
			memcpy(a->folders, r->folders, sizeof(Folder) * (usize)a->nfolders);
			sort_folders(a);
			if (current) {
				/* "INBOX" was a guess before the folders were known */
				Folder *inbox = folder_by_role(a, "inbox");
				if (inbox && str_eq_nocase(M.folder, "INBOX") && !str_eq(M.folder, inbox->raw))
					str_copy(M.folder, inbox->raw, sizeof M.folder);
			}
			break;
		case J_HEADS:
		case J_SEARCH:
			if (!a)
				break;
			if (r->job == J_HEADS && str_eq_nocase(r->folder, "INBOX") && r->ok) {
				notify_new(a, r->heads, r->nheads);
				for (int i = 0; i < r->nheads; i++)
					a->inbox_top = MAX(a->inbox_top, r->heads[i].uid);
			}
			if (!current || r->gen != M.heads_gen || !str_eq(r->folder, M.folder))
				break;
			M.loading = false;
			if (!r->ok) {
				if (!r->quiet)
					str_copy(M.list_error, r->error[0] ? r->error : "Couldn't get the messages.", sizeof M.list_error);
				a->state = AS_FAILED;
				str_copy(a->error, M.list_error, sizeof a->error);
				break;
			}
			if (a->state != AS_ONLINE && a->nfolders)
				a->state = AS_ONLINE;
			M.list_error[0] = 0;
			/* a message waiting to be moved stays out of sight */
			if (M.pend.kind && M.pend.acct_id == a->id && str_eq(M.pend.folder, M.folder))
				for (int i = 0; i < r->nheads; i++)
					if (r->heads[i].uid == M.pend.head.uid) {
						memmove(&r->heads[i], &r->heads[i + 1], sizeof(Head) * (usize)(r->nheads - i - 1));
						r->nheads--;
						break;
					}
			set_heads(r->heads, r->nheads);
			r->heads = NULL;
			M.exists = r->exists;
			M.searching = r->job == J_SEARCH;
			if (r->unseen >= 0) {
				Folder *f = folder_by_raw(a, M.folder);
				if (f)
					f->unseen = r->unseen;
			}
			break;
		case J_CHECK:
			if (a && r->ok) {
				Folder *inbox = folder_by_role(a, "inbox");
				if (inbox && !(current && str_eq(M.folder, inbox->raw)) && r->unseen > inbox->unseen) {
					int fresh = r->unseen - inbox->unseen;
					char title[64];
					fmt(title, sizeof title, fresh == 1 ? "1 new message" : "%d new messages", fresh);
					ui_notify(M.ui, title, a->a.email, "mail");
				}
				if (inbox)
					inbox->unseen = r->unseen;
			}
			break;
		case J_BODY:
			if (!current || r->gen != M.msg_gen || r->uid != M.sel)
				break;
			if (!r->ok) {
				M.msg_loading = false;
				str_copy(M.msg_error, r->error[0] ? r->error : "Couldn't open this message.", sizeof M.msg_error);
				break;
			}
			{
				MsgColors c = msg_colors();
				show_message(msg_new(r->raw, r->len, &c));
				r->raw = NULL;
			}
			break;
		case J_SEND:
			if (M.compose && M.compose->sending) {
				M.compose->sending = false;
				if (r->ok) {
					Head *h = current && str_eq(r->folder, M.folder) && r->uid ? head_by_uid(r->uid, NULL) : NULL;
					if (h)
						h->answered = true;
					compose_free(M.compose);
					M.compose = NULL;
					toast("Message sent.", false);
					if (current && (str_eq(cur_role(), "sent") || str_eq(cur_role(), "drafts")))
						fetch_heads(true);
				} else {
					fmt(M.compose->error, sizeof M.compose->error, "Couldn't send: %s", r->error);
				}
			} else if (!r->ok) {
				toast("A message couldn't be sent.", false);
			}
			break;
		case J_DRAFT:
			if (!r->ok)
				toast("The draft couldn't be saved.", false);
			else if (current && str_eq(cur_role(), "drafts"))
				fetch_heads(true);
			break;
		case J_FLAG:
		case J_MOVE:
		case J_DELETE:
			if (!r->ok) {
				char t[200];
				fmt(t, sizeof t, "That didn't work: %s", r->error[0] ? r->error : "the server said no");
				toast(t, false);
				if (current)
					fetch_heads(true);
			}
			break;
		}
		result_free(r);
		r = next;
	}
}

/* ---- Account setup ------------------------------------------------------------------------------------- */

static void setup_open(int edit)
{
	Setup *s = &M.setup;
	StrBuf *fields[] = { &s->name, &s->email, &s->password, &s->imap, &s->imap_port, &s->smtp, &s->smtp_port };
	for (int i = 0; i < 7; i++)
		sb_clear(fields[i]);
	s->open = true;
	s->edit = edit;
	s->advanced = false;
	s->servers_edited = edit >= 0;
	s->busy = false;
	s->error[0] = 0;
	s->guessed_for[0] = 0;
	if (edit >= 0 && edit < M.nacct) {
		Account *a = &M.accts[edit].a;
		sb_adds(&s->name, a->name);
		sb_adds(&s->email, a->email);
		sb_adds(&s->password, a->password);
		sb_adds(&s->imap, a->imap_host);
		sb_fmt(&s->imap_port, "%u", a->imap_port);
		sb_adds(&s->smtp, a->smtp_host);
		sb_fmt(&s->smtp_port, "%u", a->smtp_port);
	} else {
		const char *who = ui_user_name(M.ui);
		if (who && *who)
			sb_adds(&s->name, who);
	}
}

static void setup_guess(void)
{
	Setup *s = &M.setup;
	if (s->servers_edited || str_eq(s->guessed_for, sb_str(&s->email)))
		return;
	str_copy(s->guessed_for, sb_str(&s->email), sizeof s->guessed_for);
	Account a = { 0 };
	str_copy(a.email, sb_str(&s->email), sizeof a.email);
	account_guess(&a);
	sb_clear(&s->imap);
	sb_clear(&s->smtp);
	sb_clear(&s->imap_port);
	sb_clear(&s->smtp_port);
	if (str_chr(a.email, '@')) {
		sb_adds(&s->imap, a.imap_host);
		sb_adds(&s->smtp, a.smtp_host);
		sb_fmt(&s->imap_port, "%u", a.imap_port);
		sb_fmt(&s->smtp_port, "%u", a.smtp_port);
	}
}

static const char *provider_hint(const char *email)
{
	const char *at = str_chr(email, '@');
	if (!at)
		return NULL;
	at++;
	if (str_eq_nocase(at, "gmail.com") || str_eq_nocase(at, "googlemail.com"))
		return "Gmail needs an app password: turn on 2-Step Verification in your Google Account, then make one at "
		       "myaccount.google.com/apppasswords and type it here.";
	if (str_find_nocase(at, "outlook.") || str_find_nocase(at, "hotmail.") || str_find_nocase(at, "live."))
		return "If your Microsoft account uses two-step verification, make an app password in its security settings.";
	if (str_eq_nocase(at, "icloud.com") || str_eq_nocase(at, "me.com") || str_eq_nocase(at, "mac.com"))
		return "iCloud Mail needs an app-specific password from account.apple.com.";
	if (str_find_nocase(at, "yahoo."))
		return "Yahoo Mail needs an app password from your Yahoo account's security page.";
	return NULL;
}

static void setup_submit(void)
{
	Setup *s = &M.setup;
	if (s->busy)
		return;
	setup_guess();
	s->error[0] = 0;
	char email[128];
	trim_copy(sb_str(&s->email), email, sizeof email);
	if (!valid_addr(email)) {
		str_copy(s->error, "Type your full email address, like name@example.com.", sizeof s->error);
		return;
	}
	if (!s->password.len) {
		str_copy(s->error, "Type your password.", sizeof s->error);
		return;
	}
	if (!s->imap.len || !s->smtp.len) {
		str_copy(s->error, "Mail doesn't know the servers for this address. Fill them in under Server Settings.", sizeof s->error);
		s->advanced = true;
		return;
	}
	for (int i = 0; i < M.nacct; i++)
		if (i != s->edit && str_eq_nocase(M.accts[i].a.email, email)) {
			str_copy(s->error, "This account is already set up.", sizeof s->error);
			return;
		}
	Job *j = job_new(J_TEST, NULL);
	Account *a = &j->account;
	trim_copy(sb_str(&s->name), a->name, sizeof a->name);
	str_copy(a->email, email, sizeof a->email);
	str_copy(a->password, sb_str(&s->password), sizeof a->password);
	trim_copy(sb_str(&s->imap), a->imap_host, sizeof a->imap_host);
	trim_copy(sb_str(&s->smtp), a->smtp_host, sizeof a->smtp_host);
	a->imap_port = (u16)str_to_int(sb_str(&s->imap_port), NULL);
	a->smtp_port = (u16)str_to_int(sb_str(&s->smtp_port), NULL);
	if (!a->imap_port)
		a->imap_port = 993;
	if (!a->smtp_port)
		a->smtp_port = 465;
	submit(j);
	s->busy = true;
}

static void remove_account(int i)
{
	if (i < 0 || i >= M.nacct)
		return;
	Acct *a = &M.accts[i];
	path_remove_all(a->cache);
	memmove(&M.accts[i], &M.accts[i + 1], sizeof(Acct) * (usize)(M.nacct - i - 1));
	M.nacct--;
	save_accounts();
	close_message();
	set_heads(NULL, 0);
	M.folder[0] = 0;
	if (M.nacct)
		switch_account(MIN(M.cur, M.nacct - 1));
	else
		setup_open(-1);
}

static void field(Ui *ui, Rect *col, const char *label, const char *id, StrBuf *text, const char *placeholder, u32 flags, int *ev)
{
	Theme *t = &ui->theme;
	ui_text(ui, ui_font(ui, 12, 600), col->x, col->y + 12, label, t->text_dim);
	int e = ui_entry(ui, rect(col->x, col->y + 20, col->w, 36), id, text, placeholder, flags);
	if (ev)
		*ev = e;
	col->y += 20 + 36 + 14;
	col->h -= 70;
}

static void setup_page(Ui *ui, Rect area)
{
	Theme *t = &ui->theme;
	Setup *s = &M.setup;
	ui_fill(ui, area, t->view);
	f32 y = ui_scroll_begin(ui, area, "setup");
	f32 w = minf(440, area.w - 48);
	Rect col = rect(area.x + (area.w - w) / 2, y + maxf(24, (area.h - 620) / 3), w, 2000);
	ui_app_icon(ui, rect(col.x + (w - 72) / 2, col.y, 72, 72), "mail");
	col.y += 88;
	ui_text_in(ui, ui_font(ui, 22, 700), rect(col.x, col.y, w, 30), s->edit >= 0 ? "Account Settings" : "Add Your Email Account", t->text, ALIGN_CENTER);
	col.y += 36;
	col.y += ui_text_wrapped(ui, ui_font(ui, 13, 400), rect(col.x, col.y, w, 60),
				 "Works with Gmail, Outlook, iCloud, Yahoo and any email that offers IMAP. Your password stays on this computer.",
				 t->text_dim, ALIGN_CENTER) + 20;
	int ev = 0;
	field(ui, &col, "Your Name", "s-name", &s->name, "Shown to the people you write to", 0, NULL);
	field(ui, &col, "Email Address", "s-email", &s->email, "name@example.com", s->edit < 0 && !s->email.len ? ENTRY_AUTOFOCUS : 0, &ev);
	if (ev & ENTRY_CHANGED)
		setup_guess();
	field(ui, &col, "Password", "s-pass", &s->password, "Password or app password", ENTRY_PASSWORD, &ev);
	if (ev & ENTRY_SUBMIT)
		setup_submit();
	const char *hint = provider_hint(sb_str(&s->email));
	if (hint) {
		f32 hh = ui_text_height(ui, ui_font(ui, 12, 400), hint, w - 44) + 20;
		Rect hr = rect(col.x, col.y - 4, w, hh);
		ui_round(ui, hr, 8, color_with_alpha(t->accent, 0x1c));
		ui_icon(ui, rect(hr.x + 12, hr.y + 11, 16, 16), "info", t->accent);
		ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(hr.x + 36, hr.y + 10, w - 44, hh), hint, t->text, ALIGN_START);
		col.y += hh + 10;
	}
	/* server settings, filled in from the address */
	Rect adv = rect(col.x - 6, col.y, 200, 30);
	if (ui_button_icon_label(ui, adv, s->advanced ? "pan-down" : "pan-end", "Server Settings"))
		s->advanced = !s->advanced;
	col.y += 40;
	if (s->advanced) {
		f32 pw = 84;
		Rect row = rect(col.x, col.y, w, 70);
		Rect host = rect(row.x, row.y, w - pw - 10, 70), port = rect(row.x + w - pw, row.y, pw, 70);
		int e1 = 0, e2 = 0, e3 = 0, e4 = 0;
		field(ui, &host, "Incoming Mail (IMAP)", "s-imap", &s->imap, "imap.example.com", 0, &e1);
		field(ui, &port, "Port", "s-imap-port", &s->imap_port, "993", 0, &e2);
		col.y += 70;
		host = rect(row.x, col.y, w - pw - 10, 70), port = rect(row.x + w - pw, col.y, pw, 70);
		field(ui, &host, "Outgoing Mail (SMTP)", "s-smtp", &s->smtp, "smtp.example.com", 0, &e3);
		field(ui, &port, "Port", "s-smtp-port", &s->smtp_port, "465", 0, &e4);
		col.y += 70;
		if ((e1 | e2 | e3 | e4) & ENTRY_CHANGED)
			s->servers_edited = true;
		ui_text_wrapped(ui, ui_font(ui, 11, 400), rect(col.x, col.y - 6, w, 40),
				"Ports 993 and 465 use TLS from the start; 143 and 587 switch to it with STARTTLS.", t->text_faint, ALIGN_START);
		col.y += 30;
	}
	if (s->error[0]) {
		f32 eh = ui_text_height(ui, ui_font(ui, 13, 500), s->error, w - 46) + 20;
		Rect er = rect(col.x, col.y, w, eh);
		ui_round(ui, er, 8, color_with_alpha(t->danger, 0x22));
		ui_icon(ui, rect(er.x + 12, er.y + 11, 16, 16), "error", t->danger);
		ui_text_wrapped(ui, ui_font(ui, 13, 500), rect(er.x + 38, er.y + 10, w - 46, eh), s->error, t->text, ALIGN_START);
		col.y += eh + 14;
	}
	Rect btns = rect(col.x, col.y + 4, w, 38);
	if (s->busy) {
		ui_spinner(ui, rect(btns.x + w - 28, btns.y + 9, 20, 20));
		ui_text_in(ui, ui_font(ui, 13, 500), rect(btns.x, btns.y, w - 40, 38), "Signing in…", t->text_dim, ALIGN_END);
	} else {
		if (ui_button_suggested(ui, rect(btns.x + w - 120, btns.y, 120, 38), s->edit >= 0 ? "Save" : "Sign In"))
			setup_submit();
		if (M.nacct && ui_button(ui, rect(btns.x + w - 232, btns.y, 100, 38), "Cancel"))
			s->open = false;
	}
	col.y += 60;
	ui_scroll_end(ui, col.y - y + 24);
	if (M.nacct && !s->busy && ui_key_pressed(ui, KEY_ESC, 0))
		s->open = false;
}

/* ---- Sidebar -------------------------------------------------------------------------------------------- */

static void accounts_popover(Ui *ui, Rect anchor)
{
	Theme *t = &ui->theme;
	f32 h = 8 + 52 * (f32)M.nacct + 9 + 36 * 3 + 8;
	Rect c;
	if (!ui_popover_begin(ui, "accounts", anchor, 300, h, &c))
		return;
	f32 y = c.y + 4;
	for (int i = 0; i < M.nacct; i++) {
		Acct *a = &M.accts[i];
		Rect r = rect(c.x + 4, y, c.w - 8, 50);
		ui_push_id(ui, i);
		UiInteract in = ui_row(ui, r, ui_id(ui, "acct"), false);
		ui_pop_id(ui);
		ui_avatar(ui, rect(r.x + 8, r.y + 9, 32, 32), display_name(a->a.name, a->a.email));
		ui_text(ui, ui_font(ui, 13, 600), r.x + 50, r.y + 22, display_name(a->a.name, a->a.email), t->text);
		ui_text(ui, ui_font(ui, 12, 400), r.x + 50, r.y + 40, a->a.email, t->text_dim);
		if (i == M.cur)
			ui_icon(ui, rect(r.x + r.w - 28, r.y + 16, 18, 18), "check", t->accent);
		if (in.clicked) {
			ui_popover_close(ui);
			commit_pending();
			switch_account(i);
		}
		y += 52;
	}
	ui_separator(ui, c.x + 8, y + 4, c.x + c.w - 8);
	y += 9;
	if (ui_menu_item(ui, rect(c.x + 4, y, c.w - 8, 34), "add", "Add Account…", NULL)) {
		ui_popover_close(ui);
		setup_open(-1);
	}
	y += 36;
	if (ui_menu_item(ui, rect(c.x + 4, y, c.w - 8, 34), "settings", "Account Settings…", NULL)) {
		ui_popover_close(ui);
		setup_open(M.cur);
	}
	y += 36;
	if (ui_menu_item(ui, rect(c.x + 4, y, c.w - 8, 34), "remove", "Remove This Account…", NULL)) {
		ui_popover_close(ui);
		M.confirm_remove = M.cur;
		ui_dialog_open(ui, "remove");
	}
	ui_popover_end(ui);
}

static void sidebar(Ui *ui, Rect area)
{
	Theme *t = &ui->theme;
	Acct *a = cur_acct();
	ui_sidebar_background(ui, area);
	Rect r = pad2(area, 10, 10);
	/* Compose, the big friendly button */
	Rect cb = cut_top(&r, 46);
	cb.w = minf(cb.w, 150);
	UiId cid = ui_id(ui, "compose-big");
	UiInteract in = ui_interact(ui, cid, cb);
	f32 hv = ui_fade(ui, cid, in.hovered, 0.12f);
	ui_shadow(ui, cb, 14, 8 + 6 * hv, 2, color_scale_alpha(t->shadow, 0.5f + 0.5f * hv));
	ui_round(ui, cb, 14, color_mix(t->card, color_mix(t->card, t->accent, 0.12f), hv));
	ui_icon(ui, rect(cb.x + 16, cb.y + 14, 18, 18), "edit", t->accent);
	ui_text(ui, ui_font(ui, 14, 600), cb.x + 44, cb.y + 28, "Compose", t->text);
	if (in.clicked)
		compose_to("");
	cut_top(&r, 14);
	Rect bottom = cut_bottom(&r, 52);
	f32 y = ui_scroll_begin(ui, r, "folders");
	Rect row = rect(r.x, y, r.w, 34);
	bool others = false;
	for (int i = 0; a && i < a->nfolders; i++) {
		Folder *f = &a->folders[i];
		if (!f->role[0] && !others) {
			others = true;
			row.y += 8;
			ui_sidebar_heading(ui, rect(row.x, row.y, row.w, 24), "FOLDERS");
			row.y += 26;
		}
		bool selected = str_eq(f->raw, M.folder);
		ui_push_id(ui, i);
		if (ui_sidebar_item(ui, rect(row.x, row.y, row.w, 34), folder_icon(f), folder_title(a, f), selected)) {
			commit_pending();
			open_folder(f->raw);
		}
		ui_pop_id(ui);
		if (f->unseen > 0 && !str_eq(f->role, "sent") && !str_eq(f->role, "trash")) {
			char n[16];
			fmt(n, sizeof n, "%d", f->unseen);
			ui_text_in(ui, ui_font(ui, 12, 700), rect(row.x, row.y, row.w - 10, 34), n, selected ? t->on_accent : t->text, ALIGN_END);
		}
		row.y += 36;
	}
	if (a && !a->nfolders) {
		/* folders aren't known yet: the inbox still works */
		if (ui_sidebar_item(ui, rect(row.x, row.y, row.w, 34), "inbox", "Inbox", str_eq_nocase(M.folder, "INBOX")))
			open_folder("INBOX");
		row.y += 36;
		if (a->state == AS_CONNECTING) {
			ui_spinner(ui, rect(row.x + 12, row.y + 8, 16, 16));
			ui_text(ui, ui_font(ui, 12, 500), row.x + 38, row.y + 21, "Connecting…", t->text_dim);
		}
	}
	ui_scroll_end(ui, row.y - y + 8);
	/* the account, and a menu to switch or add one */
	if (a) {
		ui_separator(ui, bottom.x, bottom.y + 2, bottom.x + bottom.w);
		Rect ar = rect(bottom.x, bottom.y + 8, bottom.w, 44);
		UiInteract ai = ui_row(ui, ar, ui_id(ui, "acct-row"), ui_popover_is_open(ui, "accounts"));
		ui_avatar(ui, rect(ar.x + 6, ar.y + 8, 28, 28), display_name(a->a.name, a->a.email));
		ui_text_in(ui, ui_font(ui, 12, 600), rect(ar.x + 42, ar.y + 4, ar.w - 66, 20), display_name(a->a.name, a->a.email), t->text, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 11, 400), rect(ar.x + 42, ar.y + 22, ar.w - 66, 18), a->a.email, t->text_dim, ALIGN_START);
		ui_icon(ui, rect(ar.x + ar.w - 22, ar.y + 15, 14, 14), "pan-up", t->text_dim);
		if (ai.clicked)
			ui_popover_toggle(ui, "accounts");
		accounts_popover(ui, ar);
	}
}

/* ---- Message list ---------------------------------------------------------------------------------------- */

static bool matches_filter(const Head *h, const char *q)
{
	if (!q[0] || M.searching)
		return true;
	return str_find_nocase(h->subject, q) || str_find_nocase(h->from_name, q) || str_find_nocase(h->from_addr, q) ||
	       str_find_nocase(h->snippet, q) || str_find_nocase(h->to, q);
}

static void message_list(Ui *ui, Rect area)
{
	Theme *t = &ui->theme;
	Acct *a = cur_acct();
	ui_fill(ui, area, t->view);
	Rect r = area;
	/* title */
	Rect head = cut_top(&r, 56);
	Folder *f = folder_by_raw(a, M.folder);
	char title[300], sub[96] = "";
	if (M.searching)
		fmt(title, sizeof title, "Results for “%s”", M.search_q);
	else
		str_copy(title, f ? folder_title(a, f) : str_eq_nocase(M.folder, "INBOX") ? "Inbox" : M.folder, sizeof title);
	if (M.searching)
		fmt(sub, sizeof sub, M.nheads == 1 ? "1 message" : "%d messages", M.nheads);
	else if (f && f->unseen > 0)
		fmt(sub, sizeof sub, "%d unread", f->unseen);
	else if (M.exists)
		fmt(sub, sizeof sub, M.exists == 1 ? "1 message" : "%u messages", M.exists);
	Rect hr = pad2(head, 18, 8);
	Rect btn = cut_right(&hr, 32);
	if (M.searching) {
		if (ui_icon_button(ui, center_in(btn, 32, 32), "window-close", "Close Search")) {
			M.searching = false;
			sb_clear(&M.search);
			open_folder(M.folder);
		}
	} else if (M.loading) {
		ui_spinner(ui, center_in(btn, 18, 18));
	} else if (ui_icon_button(ui, center_in(btn, 32, 32), "refresh", "Refresh")) {
		if (a && a->state == AS_FAILED)
			connect_account(a);
		fetch_heads(false);
	}
	ui_text_in(ui, ui_font(ui, 17, 700), rect(hr.x, hr.y, hr.w - 8, 22), title, t->text, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 12, 500), rect(hr.x, hr.y + 22, hr.w - 8, 18), sub, t->text_dim, ALIGN_START);
	ui_fill(ui, rect(r.x, r.y, r.w, 1), t->border);
	/* trouble reaching the server */
	const char *err = M.list_error[0] ? M.list_error : a && a->state == AS_FAILED ? a->error : NULL;
	if (err) {
		Font ef = ui_font(ui, 12, 500);
		f32 eh = maxf(48, ui_text_height(ui, ef, err, r.w - 170) + 20);
		Rect er = cut_top(&r, eh);
		ui_fill(ui, er, color_with_alpha(t->warning, 0x26));
		ui_icon(ui, rect(er.x + 14, er.y + (eh - 16) / 2, 16, 16), "warning", t->warning);
		ui_text_wrapped(ui, ef, rect(er.x + 40, er.y + 10, er.w - 170, eh), err, t->text, ALIGN_START);
		if (ui_button(ui, rect(er.x + er.w - 84, er.y + (eh - 30) / 2, 72, 30), "Retry")) {
			M.list_error[0] = 0;
			connect_account(a);
			fetch_heads(false);
		}
		/* a refused password is fixed in the account's settings */
		if (str_find_nocase(err, "password") && ui_icon_button(ui, rect(er.x + er.w - 122, er.y + (eh - 30) / 2, 30, 30), "settings", "Account Settings"))
			setup_open(M.cur);
	}
	const char *q = sb_str(&M.search);
	int shown = 0;
	for (int i = 0; i < M.nheads; i++)
		shown += matches_filter(&M.heads[i], q);
	if (!shown) {
		if (M.loading && !M.nheads)
			ui_spinner(ui, center_in(r, 28, 28));
		else if (q[0] || M.searching)
			ui_placeholder(ui, r, "search", "No messages found", M.searching ? "Try other words." : "Press Enter to search the whole folder.");
		else
			ui_placeholder(ui, r, str_eq(cur_role(), "inbox") ? "inbox" : "mail", "Nothing here", str_eq(cur_role(), "inbox") ? "You're all caught up." : NULL);
		return;
	}
	bool sent = str_eq(cur_role(), "sent") || str_eq(cur_role(), "drafts");
	const f32 rh = 82;
	f32 oy0 = ui->oy;
	f32 y = ui_scroll_begin(ui, r, "list");
	f32 off = oy0 - ui->oy;   /* how far the list is scrolled */
	f32 cy = y;
	for (int i = 0; i < M.nheads; i++) {
		Head *h = &M.heads[i];
		if (!matches_filter(h, q))
			continue;
		Rect row = rect(r.x + 6, cy + 2, r.w - 12, rh - 4);
		cy += rh;
		if (row.y + row.h - off < r.y || row.y - off > r.y + r.h)
			continue;   /* out of sight */
		ui_push_id(ui, (i64)h->uid);
		bool selected = M.sel == h->uid || (M.compose && M.compose->draft_uid == h->uid);
		UiInteract in = ui_row(ui, row, ui_id(ui, "row"), selected);
		Rect starr = rect(row.x + row.w - 34, row.y + row.h - 32, 26, 26);
		UiId sid = ui_id(ui, "star");
		UiInteract sin = ui_interact(ui, sid, starr);
		ui_pop_id(ui);
		bool unread = !h->seen;
		Color fg = t->text;
		if (unread)
			ui_circle(ui, row.x + 7, row.y + 30, 3.5f, t->accent);
		const char *who = sent ? (h->to[0] ? h->to : "(no recipient)") : display_name(h->from_name, h->from_addr);
		ui_avatar(ui, rect(row.x + 16, row.y + 12, 38, 38), who);
		f32 x = row.x + 66, w = row.w - 66 - 10;
		char date[40];
		short_date(h->date, date, sizeof date);
		Font df = ui_font(ui, 12, unread ? 700 : 500);
		f32 dw = ui_text_width(ui, df, date);
		ui_text(ui, df, row.x + row.w - 10 - dw, row.y + 24, date, unread ? t->accent : t->text_dim);
		char whobuf[300];
		fmt(whobuf, sizeof whobuf, "%s%s", sent ? "To: " : "", who);
		ui_text_in(ui, ui_font(ui, 14, unread ? 700 : 500), rect(x, row.y + 8, w - dw - 10, 22), whobuf, fg, ALIGN_START);
		f32 sx = x;
		if (h->answered) {
			ui_icon(ui, rect(x, row.y + 33, 14, 14), "mail-reply", t->text_dim);
			sx += 18;
		}
		ui_text_in(ui, ui_font(ui, 13, unread ? 650 : 400), rect(sx, row.y + 30, w - (sx - x), 20), h->subject[0] ? h->subject : "(no subject)", fg, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 12, 400), rect(x, row.y + 50, w - 30, 20), h->snippet, t->text_dim, ALIGN_START);
		/* the star */
		f32 sh = ui_fade(ui, sid, sin.hovered, 0.1f);
		if (sh > 0)
			ui_round(ui, starr, 13, color_scale_alpha(t->hover, sh));
		ui_icon(ui, pad(starr, 5), h->flagged ? "starred" : "star", h->flagged ? 0xfff4b400 : color_mix(t->text_faint, t->text_dim, sh));
		if (sin.clicked)
			set_flag(h, "\\Flagged", !h->flagged);
		else if (in.clicked) {
			commit_pending();
			if (M.compose && !M.compose->sending)
				close_compose(true);
			open_message(h->uid);
		}
	}
	if (!M.searching && M.exists > (u32)M.nheads && !q[0]) {
		char more[96];
		fmt(more, sizeof more, "Showing the newest %d of %u", M.nheads, M.exists);
		ui_text_in(ui, ui_font(ui, 12, 500), rect(r.x, cy + 8, r.w, 24), more, t->text_faint, ALIGN_CENTER);
		cy += 40;
	}
	ui_scroll_end(ui, cy - y + 8);
}

/* ---- Reading pane -------------------------------------------------------------------------------------------- */

static void sel_range(int *a, int *ao, int *b, int *bo)
{
	*a = M.sel_text.a, *ao = M.sel_text.ao, *b = M.sel_text.b, *bo = M.sel_text.bo;
	if (*a > *b || (*a == *b && *ao > *bo)) {
		int t = *a, to = *ao;
		*a = *b, *ao = *bo, *b = t, *bo = to;
	}
}

static void follow_link(Ui *ui, Node *link)
{
	const char *href = node_attr(link, "href");
	if (!href || !*href || href[0] == '#')
		return;
	if (str_eq_nocase_n(href, "mailto:", 7)) {
		char addr[256];
		str_copy(addr, href + 7, sizeof addr);
		char *qm = (char *)str_chr(addr, '?');
		if (qm)
			*qm = 0;
		compose_to(addr);
		return;
	}
	if (str_eq_nocase_n(href, "http://", 7) || str_eq_nocase_n(href, "https://", 8))
		ui_launch(ui, "", href); /* the web browser */
}

static void save_attachment(Ui *ui, int i)
{
	if (!M.msg || i < 0 || i >= M.msg->natts)
		return;
	M.save_att = i;
	char *dl = path_join(home_dir(), "Downloads");
	const Part *p = M.msg->atts[i];
	ui_file_dialog_open(ui, "save-att", FILE_SAVE, dl, NULL, p->filename[0] ? p->filename : "attachment");
	mem_free(dl);
}

static void folder_menu(Ui *ui, Rect anchor, u32 uid)
{
	Acct *a = cur_acct();
	if (!a)
		return;
	int n = 0;
	for (int i = 0; i < a->nfolders; i++)
		n += !str_eq(a->folders[i].raw, M.folder) && !str_eq(a->folders[i].role, "drafts") && !str_eq(a->folders[i].role, "flagged");
	Rect c;
	if (!ui_popover_begin(ui, "move", anchor, 240, minf(420, 8 + 36 * (f32)MAX(1, n)), &c))
		return;
	f32 y = ui_scroll_begin(ui, c, "move-list");
	f32 cy = y + 4;
	for (int i = 0; i < a->nfolders; i++) {
		Folder *f = &a->folders[i];
		if (str_eq(f->raw, M.folder) || str_eq(f->role, "drafts") || str_eq(f->role, "flagged"))
			continue;
		ui_push_id(ui, i);
		if (ui_menu_item(ui, rect(c.x + 4, cy, c.w - 8, 34), folder_icon(f), folder_title(a, f), NULL)) {
			ui_popover_close(ui);
			char done[300];
			fmt(done, sizeof done, "Moved to %s.", folder_title(a, f));
			move_message(uid, f->raw, done);
		}
		ui_pop_id(ui);
		cy += 36;
	}
	ui_scroll_end(ui, cy - y + 4);
	ui_popover_end(ui);
}

static void pane_toolbar(Ui *ui, Rect bar, bool stacked)
{
	Theme *t = &ui->theme;
	Acct *a = cur_acct();
	u32 uid = M.sel;
	Rect r = pad2(bar, 10, 8);
	if (stacked) {
		if (ui_icon_button(ui, cut_left(&r, 32), "go-back", "Back"))
			close_message();
		cut_left(&r, 6);
	}
	const char *role = cur_role();
	if (folder_by_role(a, "archive") && !str_eq(role, "archive") && ui_icon_button(ui, cut_left(&r, 32), "mail-archive", "Archive (E)"))
		archive_message(uid);
	if ((folder_by_role(a, "junk") || str_eq(role, "junk")) &&
	    ui_icon_button(ui, cut_left(&r, 32), "mail-junk", str_eq(role, "junk") ? "Not Spam" : "Report Spam"))
		junk_message(uid);
	if (ui_icon_button(ui, cut_left(&r, 32), "trash", str_eq(role, "trash") ? "Delete Forever" : "Delete (#)"))
		delete_message(uid);
	cut_left(&r, 6);
	ui_vseparator(ui, r.x, r.y + 6, r.y + r.h - 6);
	cut_left(&r, 6);
	if (ui_icon_button(ui, cut_left(&r, 32), "mail-unread", "Mark as Unread (Shift+U)")) {
		Head *h = head_by_uid(uid, NULL);
		if (h)
			set_flag(h, "\\Seen", false);
		close_message();
		return;
	}
	Rect mv = cut_left(&r, 32);
	if (ui_icon_button(ui, mv, "folder", "Move To…"))
		ui_popover_toggle(ui, "move");
	folder_menu(ui, mv, uid);
	/* position and next / previous */
	int idx = -1;
	head_by_uid(uid, &idx);
	if (idx >= 0 && r.w > 200) {
		Rect nx = cut_right(&r, 32), pv = cut_right(&r, 32);
		if (idx > 0 && ui_icon_button(ui, pv, "pan-up", "Newer (K)"))
			open_message(M.heads[idx - 1].uid);
		if (idx + 1 < M.nheads && ui_icon_button(ui, nx, "pan-down", "Older (J)"))
			open_message(M.heads[idx + 1].uid);
		char pos[48];
		fmt(pos, sizeof pos, "%d of %d", idx + 1, M.nheads);
		ui_text_in(ui, ui_font(ui, 12, 500), cut_right(&r, 90), pos, t->text_dim, ALIGN_END);
	}
}

static void attachment_chip(Ui *ui, Rect r, const Part *p, int i)
{
	Theme *t = &ui->theme;
	ui_push_id(ui, i);
	UiId id = ui_id(ui, "att");
	UiInteract in = ui_interact(ui, id, r);
	ui_pop_id(ui);
	f32 hv = ui_fade(ui, id, in.hovered, 0.12f);
	ui_round(ui, r, 10, color_mix(t->card, t->hover, hv * 0.6f));
	ui_border(ui, r, 10, 1, t->border);
	const char *name = p->filename[0] ? p->filename : "attachment";
	Rect ic = rect(r.x + 10, r.y + 10, 36, 36);
	ui_round(ui, ic, 8, color_with_alpha(t->accent, 0x22));
	ui_icon(ui, pad(ic, 8), ui_file_icon(name, false), t->accent);
	ui_text_in(ui, ui_font(ui, 13, 600), rect(r.x + 56, r.y + 8, r.w - 64, 20), name, t->text, ALIGN_START);
	char size[32];
	usize n = p->body_len;
	if (str_eq(p->encoding, "base64")) {
		/* decoded size: four letters make three bytes, minus the padding */
		usize letters = 0, pad = 0;
		for (usize k = 0; k < p->body_len; k++) {
			char ch = p->body[k];
			if (ch == '=')
				pad++;
			else if (is_alnum(ch) || ch == '+' || ch == '/')
				letters++;
		}
		n = (letters + pad) / 4 * 3 - MIN(pad, 2);
	}
	fmt_bytes(size, sizeof size, n);
	ui_text_in(ui, ui_font(ui, 11, 500), rect(r.x + 56, r.y + 28, r.w - 64, 18), size, t->text_dim, ALIGN_START);
	if (hv > 0)
		ui_icon(ui, rect(r.x + r.w - 26, r.y + 20, 16, 16), "download", color_scale_alpha(t->text_dim, hv));
	if (in.clicked)
		save_attachment(ui, i);
	ui_tooltip(ui, "Save");
}

static void reading_pane(Ui *ui, Rect area, bool stacked)
{
	Theme *t = &ui->theme;
	ui_fill(ui, area, t->view);
	if (M.msg_loading || M.msg_error[0] || !M.msg) {
		if (M.sel)
			pane_toolbar(ui, cut_top(&area, 48), stacked);
		if (M.msg_error[0])
			ui_placeholder(ui, area, "error", "Couldn't open this message", M.msg_error);
		else if (M.msg_loading)
			ui_spinner(ui, center_in(area, 28, 28));
		else {
			Folder *f = folder_by_role(cur_acct(), "inbox");
			char sub[64] = "";
			if (f && f->unseen)
				fmt(sub, sizeof sub, f->unseen == 1 ? "1 unread message in your inbox" : "%d unread messages in your inbox", f->unseen);
			ui_placeholder(ui, area, "mail", "No message selected", sub[0] ? sub : NULL);
		}
		return;
	}
	Msg *m = M.msg;
	Rect bar = cut_top(&area, 48);
	pane_toolbar(ui, bar, stacked);
	if (!M.msg)
		return;   /* the toolbar closed it */
	ui_fill(ui, rect(area.x, area.y, area.w, 1), t->border);
	Rect view = area;
	f32 margin = view.w > 700 ? 32 : 18;
	f32 cw = view.w - margin * 2;
	/* the body's page, laid out on a helper thread */
	f32 body_w = m->html ? cw : minf(cw, 820);
	msg_layout(ui, m, body_w);
	msg_take_page(m);
	Page *p = m->doc.page;
	/* scrolling */
	f32 maxs = maxf(0, M.content_h - view.h);
	if (ui_mouse_in(ui, view) && ui->scroll_dy != 0 && !ui->in_overlay) {
		M.scroll = clampf(M.scroll + ui->scroll_dy, 0, maxs);
		ui->scroll_dy = 0;
	}
	if (!ui->focus && !ui_popover_is_open(ui, "move")) {
		if (ui_key_pressed(ui, KEY_SPACE, 0) || ui_key_pressed(ui, KEY_PAGEDOWN, 0))
			M.scroll += view.h - 60;
		if (ui_key_pressed(ui, KEY_SPACE, MOD_SHIFT) || ui_key_pressed(ui, KEY_PAGEUP, 0))
			M.scroll -= view.h - 60;
		if (ui_key_pressed(ui, KEY_DOWN, 0))
			M.scroll += 48;
		if (ui_key_pressed(ui, KEY_UP, 0))
			M.scroll -= 48;
		if (ui_key_pressed(ui, KEY_HOME, 0))
			M.scroll = 0;
		if (ui_key_pressed(ui, KEY_END, 0))
			M.scroll = maxs;
	}
	M.scroll = clampf(M.scroll, 0, maxs);
	ui_push_clip(ui, view);
	f32 x = view.x + margin;
	f32 y = view.y + 20 - M.scroll;
	f32 top = y;
	/* subject */
	Font sf = ui_font(ui, 22, 700);
	f32 sh = ui_text_wrapped(ui, sf, rect(x, y, cw, 200), m->subject[0] ? m->subject : "(no subject)", t->text, ALIGN_START);
	y += sh + 16;
	/* sender line */
	const char *who = display_name(m->from_name, m->from_addr);
	ui_avatar(ui, rect(x, y, 40, 40), who);
	f32 tx = x + 52;
	Rect acts = rect(x + cw - 108, y + 2, 108, 32);
	char date[128];
	long_date(m->date, date, sizeof date);
	Font nf = ui_font(ui, 14, 700), af = ui_font(ui, 12, 400);
	f32 nw = ui_text_width(ui, nf, who);
	f32 room = cw - 52 - 120;
	ui_text_in(ui, nf, rect(tx, y + 2, minf(nw, room), 20), who, t->text, ALIGN_START);
	if (m->from_name[0] && nw + 10 < room) {
		char ab[160];
		fmt(ab, sizeof ab, "<%s>", m->from_addr);
		ui_text_in(ui, af, rect(tx + nw + 6, y + 3, room - nw - 6, 20), ab, t->text_dim, ALIGN_START);
	}
	/* "to me" and the details toggle */
	Acct *acct = cur_acct();
	char to_line[300];
	{
		/* "to me, Alex Chen": names only, and you as "me" */
		char items[32][256];
		StrBuf all = { 0 };
		sb_fmt(&all, "%s, %s", m->to, m->cc);
		int n = split_addresses(sb_str(&all), items, 32);
		sb_free(&all);
		StrBuf sb = { 0 };
		sb_adds(&sb, "to ");
		for (int i = 0; i < n; i++) {
			char nm[96], ad[128];
			mime_address(items[i], nm, sizeof nm, ad, sizeof ad);
			if (i)
				sb_adds(&sb, ", ");
			sb_adds(&sb, acct && same_addr(ad, acct->a.email) ? "me" : display_name(nm, ad));
		}
		if (!n)
			sb_adds(&sb, "undisclosed recipients");
		str_copy(to_line, sb_str(&sb), sizeof to_line);
		sb_free(&sb);
	}
	Rect tl = rect(tx, y + 22, minf(room, ui_text_width(ui, af, to_line) + 22), 18);
	UiId did = ui_id(ui, "details");
	UiInteract din = ui_interact(ui, did, tl);
	ui_text_in(ui, af, rect(tl.x, tl.y, tl.w - 18, tl.h), to_line, din.hovered ? t->text : t->text_dim, ALIGN_START);
	ui_icon(ui, rect(tl.x + tl.w - 14, tl.y + 3, 12, 12), M.details ? "pan-up" : "pan-down", t->text_dim);
	if (din.clicked)
		M.details = !M.details;
	/* actions: star and reply */
	Head *h = head_by_uid(M.sel, NULL);
	if (h && ui_icon_button(ui, rect(acts.x + 4, acts.y, 32, 32), h->flagged ? "starred" : "star", h->flagged ? "Starred" : "Star (S)"))
		set_flag(h, "\\Flagged", !h->flagged);
	if (h && h->flagged)
		ui_icon(ui, rect(acts.x + 11, acts.y + 7, 18, 18), "starred", 0xfff4b400);
	if (ui_icon_button(ui, rect(acts.x + 40, acts.y, 32, 32), "mail-reply", "Reply (R)"))
		reply(m, REPLY);
	if (ui_icon_button(ui, rect(acts.x + 76, acts.y, 32, 32), "mail-forward", "Forward (F)"))
		reply(m, FORWARD);
	y += 48;
	{
		/* the date sits under the actions when there's room, else on its own line */
		f32 dw = ui_text_width(ui, af, date);
		if (dw < cw - 52 - 40 - minf(room, nw))
			ui_text(ui, af, x + cw - dw, top + sh + 16 + 48, date, t->text_faint);
		else
			ui_text(ui, af, tx, top + sh + 16 + 54, date, t->text_faint);
		y += 14;
	}
	if (M.details) {
		const char *labels[] = { "from:", "reply-to:", "to:", "cc:", "date:", "subject:" };
		char from[300];
		fmt(from, sizeof from, "%s <%s>", who, m->from_addr);
		char dfull[128];
		long_date(m->date, dfull, sizeof dfull);
		const char *values[] = { from, m->reply_to, m->to, m->cc, dfull, m->subject };
		f32 dy = y + 8;
		Rect box = rect(tx, dy, cw - 52, 0);
		f32 bh = 12;
		for (int i = 0; i < 6; i++)
			if (values[i][0])
				bh += ui_text_height(ui, af, values[i], box.w - 90) + 6;
		box.h = bh;
		ui_round(ui, box, 8, t->card);
		ui_border(ui, box, 8, 1, t->border);
		dy += 8;
		for (int i = 0; i < 6; i++) {
			if (!values[i][0])
				continue;
			ui_text(ui, af, box.x + 12, dy + 12, labels[i], t->text_dim);
			dy += ui_text_wrapped(ui, af, rect(box.x + 80, dy, box.w - 90, 400), values[i], t->text, ALIGN_START) + 6;
		}
		y += bh + 16;
	}
	y += 8;
	/* pictures waiting for permission */
	int held = atomic_load(&m->remote);
	if (held > 0 && !m->allow_remote) {
		Rect br = rect(x, y, cw, 48);
		ui_round(ui, br, 8, t->card);
		ui_border(ui, br, 8, 1, t->border);
		ui_icon(ui, rect(br.x + 14, br.y + 16, 16, 16), "image", t->text_dim);
		bool wide = cw > 760;
		ui_text_in(ui, ui_font(ui, 12, 500), rect(br.x + 40, br.y, br.w - (wide ? 330 : 200), 48),
			   wide ? "Pictures from the web are hidden so the sender can't tell you opened this." : "Pictures are hidden.",
			   t->text, ALIGN_START);
		Rect b2 = rect(br.x + br.w - (wide ? 290 : 160), br.y + 9, wide ? 120 : 150, 30);
		if (ui_button(ui, b2, "Show Pictures"))
			msg_allow_remote(m);
		if (wide && ui_button_flat(ui, rect(b2.x + 128, b2.y, 154, 30), "Always From Sender")) {
			trust_sender(m->from_addr);
			msg_allow_remote(m);
		}
		y += 48 + 12;
	}
	/* the body */
	f32 body_top = y;
	f32 ph = p ? p->height : 120;
	if (p) {
		f32 vx = m->html ? x : x + (cw - body_w) / 2 * 0;
		f32 v0 = maxf(view.y, body_top), v1 = minf(view.y + view.h, body_top + ph);
		if (m->html && v1 > v0)
			ui_round(ui, rect(vx - 4, body_top - 4, body_w + 8, ph + 8), 8, p->background ? p->background : 0xffffffff);
		if (v1 > v0) {
			Rect pv = rect(vx, v0, body_w, v1 - v0);
			f32 pscroll = v0 - body_top;
			/* links, and selecting text */
			UiId bid = ui_id(ui, "body");
			UiInteract in = ui_interact(ui, bid, pv);
			f32 px = ui->mx - vx, py = ui->my - body_top;
			const Item *hit = in.hovered ? page_hit(p, px, py) : NULL;
			Node *link = hit && hit->link && str_eq(hit->link->tag, "a") ? hit->link : NULL;
			if (link)
				ui_set_cursor(ui, CURSOR_POINTER);
			if (M.sel_text.page != p)
				M.sel_text.on = M.sel_text.dragging = false, M.sel_text.page = p;
			if (in.pressed_now) {
				M.sel_text.px = ui->mx, M.sel_text.py = ui->my;
				M.sel_text.dragging = false;
				M.sel_text.on = false;
				ui->focus = 0;
			}
			if (in.held && !M.sel_text.dragging && (fabsf(ui->mx - M.sel_text.px) > 4 || fabsf(ui->my - M.sel_text.py) > 4)) {
				int it, off;
				if (page_text_at(p, M.sel_text.px - vx, M.sel_text.py - body_top, &it, &off)) {
					M.sel_text.dragging = M.sel_text.on = true;
					M.sel_text.a = M.sel_text.b = it, M.sel_text.ao = M.sel_text.bo = off;
				}
			}
			if (M.sel_text.dragging && in.held) {
				int it, off;
				if (page_text_at(p, px, py, &it, &off))
					M.sel_text.b = it, M.sel_text.bo = off;
				if (ui->my > view.y + view.h - 20)
					M.scroll = minf(maxs, M.scroll + 12), ui->redraw = true;
				else if (ui->my < view.y + 20)
					M.scroll = maxf(0, M.scroll - 12), ui->redraw = true;
				ui_set_cursor(ui, CURSOR_TEXT);
			}
			bool dragged = M.sel_text.dragging;
			if (!in.held)
				M.sel_text.dragging = false;
			if (M.sel_text.on && ui_key_pressed(ui, KEY_C, MOD_CTRL)) {
				int a, ao, b, bo;
				sel_range(&a, &ao, &b, &bo);
				char *text = page_selection(p, a, ao, b, bo);
				if (*text)
					ui_clipboard_set(ui, text);
				mem_free(text);
			}
			PaintOpts o = { .hover_link = link, .zoom = 1, .sel_a = -1 };
			if (M.sel_text.on)
				sel_range(&o.sel_a, &o.sel_ao, &o.sel_b, &o.sel_bo);
			paint_page(ui, p, pv, pscroll, &o);
			if (link && in.clicked && !dragged)
				follow_link(ui, link);
			if (link && in.hovered) {
				const char *href = node_attr(link, "href");
				if (href && *href) {
					Font lf = ui_font(ui, 11, 500);
					f32 lw = minf(view.w - 24, ui_text_width(ui, lf, href) + 16);
					Rect lr = rect(view.x + 8, view.y + view.h - 30, lw, 24);
					ui_round(ui, lr, 6, 0xe0202124);
					ui_text_in(ui, lf, pad2(lr, 8, 0), href, COLOR_WHITE, ALIGN_START);
				}
			}
		}
	} else {
		ui_spinner(ui, rect(x + cw / 2 - 12, body_top + 40, 24, 24));
		ui->wake_at = ui->now + 0.1;
	}
	y = body_top + ph + 24;
	/* attachments */
	if (m->natts) {
		char label[48];
		fmt(label, sizeof label, m->natts == 1 ? "1 attachment" : "%d attachments", m->natts);
		ui_separator(ui, x, y, x + cw);
		ui_text(ui, ui_font(ui, 13, 700), x, y + 28, label, t->text);
		y += 42;
		f32 chip_w = 236, gap = 12;
		int per = MAX(1, (int)((cw + gap) / (chip_w + gap)));
		for (int i = 0; i < m->natts; i++) {
			int c = i % per, rrow = i / per;
			attachment_chip(ui, rect(x + (chip_w + gap) * (f32)c, y + 68 * (f32)rrow, chip_w, 56), m->atts[i], i);
		}
		y += 68 * (f32)((m->natts + per - 1) / per) + 8;
	}
	/* reply buttons, as at the end of a Gmail message */
	y += 8;
	f32 bx = x;
	if (ui_button_icon_label(ui, rect(bx, y, 112, 38), "mail-reply", "Reply"))
		reply(m, REPLY);
	bx += 122;
	bool many = str_chr(m->to, ',') || m->cc[0];
	if (many) {
		if (ui_button_icon_label(ui, rect(bx, y, 130, 38), "mail-reply-all", "Reply All"))
			reply(m, REPLY_ALL);
		bx += 140;
	}
	if (ui_button_icon_label(ui, rect(bx, y, 120, 38), "mail-forward", "Forward"))
		reply(m, FORWARD);
	y += 38 + 32;
	M.content_h = y - top + 20;
	ui_pop_clip(ui);
	/* scroll bar */
	if (M.content_h > view.h + 1) {
		f32 frac = view.h / M.content_h;
		f32 bh = maxf(36, view.h * frac);
		f32 by = view.y + (view.h - bh) * (maxs > 0 ? M.scroll / maxs : 0);
		ui_round(ui, rect(view.x + view.w - 8, by + 2, 5, bh - 4), 2.5f, color_with_alpha(t->text, 0x40));
	}
	if (M.save_att >= 0) {
		char path[1024];
		if (ui_file_dialog(ui, "save-att", path, sizeof path)) {
			if (M.save_att < m->natts) {
				usize len;
				char *data = mime_body(m->atts[M.save_att], &len);
				char msg[300];
				if (file_write(path, data, len))
					fmt(msg, sizeof msg, "Saved “%s”.", path_base(path));
				else
					fmt(msg, sizeof msg, "Couldn't save “%s”.", path_base(path));
				toast(msg, false);
				mem_free(data);
			}
			M.save_att = -1;
		}
	}
}

/* ---- Compose view ------------------------------------------------------------------------------------------------ */

static void compose_field(Ui *ui, Rect *r, const char *label, const char *id, StrBuf *text, const char *placeholder)
{
	Theme *t = &ui->theme;
	Rect row = cut_top(r, 44);
	ui_text_in(ui, ui_font(ui, 13, 500), rect(row.x, row.y, 70, row.h), label, t->text_dim, ALIGN_START);
	ui_entry(ui, rect(row.x + 70, row.y + 5, row.w - 70, 34), id, text, placeholder, ENTRY_FLAT);
	ui_separator(ui, row.x, row.y + row.h - 1, row.x + row.w);
}

static void compose_view(Ui *ui, Rect area, bool stacked)
{
	Theme *t = &ui->theme;
	Compose *c = M.compose;
	ui_fill(ui, area, t->view);
	Rect bar = cut_top(&area, 48);
	Rect br = pad2(bar, 10, 8);
	if (ui_icon_button(ui, cut_left(&br, 32), stacked ? "go-back" : "window-close", "Close (Esc) — keeps a draft"))
		close_compose(true);
	if (!M.compose)
		return;
	cut_left(&br, 8);
	ui_text_in(ui, ui_font(ui, 15, 700), br, c->title, t->text, ALIGN_START);
	/* focus first, so typing that follows straight away lands in a field */
	if (c->frames++ == 0)
		ui_focus(ui, c->focus_body ? "body" : "to");
	/* before the fields, which would take Enter for themselves */
	if (ui_key_pressed(ui, KEY_ENTER, MOD_CTRL)) {
		send_compose();
		if (!M.compose)
			return;
	}
	ui_fill(ui, rect(area.x, area.y, area.w, 1), t->border);
	Rect r = pad2(area, area.w > 700 ? 32 : 16, 8);
	/* From, when there's a choice */
	if (M.nacct > 1) {
		Rect row = cut_top(&r, 44);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(row.x, row.y, 70, row.h), "From", t->text_dim, ALIGN_START);
		const char *items[MAX_ACCTS];
		for (int i = 0; i < M.nacct; i++)
			items[i] = M.accts[i].a.email;
		ui_dropdown(ui, rect(row.x + 70, row.y + 6, minf(320, row.w - 70), 32), "from", items, M.nacct, &c->acct);
		ui_separator(ui, row.x, row.y + row.h - 1, row.x + row.w);
	}
	Rect to_row = r;
	compose_field(ui, &r, "To", "to", &c->to, "Recipients, separated by commas");
	if (!c->show_cc && ui_button_flat(ui, rect(to_row.x + to_row.w - 70, to_row.y + 7, 70, 30), "Cc Bcc"))
		c->show_cc = true;
	if (c->show_cc) {
		compose_field(ui, &r, "Cc", "cc", &c->cc, NULL);
		compose_field(ui, &r, "Bcc", "bcc", &c->bcc, "Hidden from the other recipients");
	}
	compose_field(ui, &r, "Subject", "subject", &c->subject, NULL);
	/* bottom: send, attach, discard */
	Rect bottom = cut_bottom(&r, 56);
	Rect atts = { 0 };
	if (c->natts)
		atts = cut_bottom(&r, 44 * (f32)((c->natts + 2) / 3));
	if (c->error[0]) {
		Rect er = cut_bottom(&r, 40);
		ui_round(ui, pad2(er, 0, 4), 8, color_with_alpha(t->danger, 0x22));
		ui_icon(ui, rect(er.x + 12, er.y + 12, 16, 16), "error", t->danger);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(er.x + 38, er.y, er.w - 46, er.h), c->error, t->text, ALIGN_START);
	}
	cut_top(&r, 8);
	ui_textarea(ui, r, "body", &c->body, 14.5f, TEXT_NO_FRAME);
	if (c->frames == 1 && c->focus_body)
		ui_textarea_select(ui, "body", 0, 0);   /* the caret above the quote */
	for (int i = 0; i < c->natts; i++) {
		int per = 3;
		f32 w = (atts.w - 16) / (f32)per;
		Rect ch = rect(atts.x + (w + 8) * (f32)(i % per), atts.y + 4 + 44 * (f32)(i / per), w, 36);
		ui_round(ui, ch, 8, t->card);
		ui_border(ui, ch, 8, 1, t->border);
		ui_icon(ui, rect(ch.x + 10, ch.y + 10, 16, 16), ui_file_icon(c->atts[i].name, false), t->accent);
		char size[32];
		fmt_bytes(size, sizeof size, c->atts[i].len);
		char label[220];
		fmt(label, sizeof label, "%s  (%s)", c->atts[i].name, size);
		ui_text_in(ui, ui_font(ui, 12, 500), rect(ch.x + 34, ch.y, ch.w - 70, ch.h), label, t->text, ALIGN_START);
		ui_push_id(ui, i);
		if (ui_icon_button(ui, rect(ch.x + ch.w - 32, ch.y + 4, 28, 28), "window-close", "Remove")) {
			mem_free(c->atts[i].data);
			memmove(&c->atts[i], &c->atts[i + 1], sizeof(Att) * (usize)(c->natts - i - 1));
			c->natts--;
		}
		ui_pop_id(ui);
	}
	ui_separator(ui, bottom.x, bottom.y, bottom.x + bottom.w);
	Rect b = pad2(bottom, 0, 10);
	if (c->sending) {
		ui_spinner(ui, rect(b.x + 8, b.y + 8, 20, 20));
		ui_text(ui, ui_font(ui, 13, 500), b.x + 38, b.y + 24, "Sending…", t->text_dim);
	} else {
		Rect send = cut_left(&b, 110);
		UiId sid = ui_id(ui, "send");
		UiInteract in = ui_interact(ui, sid, send);
		f32 hv = ui_fade(ui, sid, in.hovered, 0.1f);
		ui_round(ui, send, 18, color_mix(t->accent, t->accent_hover, hv));
		ui_icon(ui, rect(send.x + 18, send.y + 10, 16, 16), "mail-send", t->on_accent);
		ui_text(ui, ui_font(ui, 14, 600), send.x + 44, send.y + 23, "Send", t->on_accent);
		if (in.clicked)
			send_compose();
		ui_tooltip(ui, "Send (Ctrl+Enter)");
		cut_left(&b, 8);
		if (ui_icon_button(ui, cut_left(&b, 36), "mail-attachment", "Attach Files")) {
			char *home = str_dup(home_dir());
			ui_file_dialog_open(ui, "attach", FILE_OPEN, home, NULL, NULL);
			mem_free(home);
		}
		if (ui_icon_button(ui, cut_right(&b, 36), "trash", "Discard"))
			close_compose(false);
	}
	if (M.compose) {
		char path[1024];
		if (ui_file_dialog(ui, "attach", path, sizeof path)) {
			i64 size = file_size(path);
			if (size > 25 * 1024 * 1024) {
				str_copy(c->error, "Files over 25 MB are too big for most mail servers.", sizeof c->error);
			} else {
				usize len;
				char *data = file_read(path, &len);
				if (data)
					add_att(c, path_base(path), NULL, (u8 *)data, len);
				else
					fmt(c->error, sizeof c->error, "Couldn't read “%s”.", path_base(path));
			}
		}
		/* files dropped on the window */
		if (ui->drop_paths) {
			const char *p = ui->drop_paths;
			while (*p) {
				const char *e = str_chr(p, '\n');
				usize n = e ? (usize)(e - p) : strlen(p);
				char one[1024];
				str_copy(one, p, MIN(n + 1, sizeof one));
				usize len;
				char *data = file_read(one, &len);
				if (data && len <= 25 * 1024 * 1024)
					add_att(c, path_base(one), NULL, (u8 *)data, len);
				else
					mem_free(data);
				p += n;
				if (*p)
					p++;
			}
		}
		if (ui_key_pressed(ui, KEY_ESC, 0) && !ui_popover_is_open(ui, "from"))
			close_compose(true);
	}
}

/* ---- Window --------------------------------------------------------------------------------------------------- */

static void draw_toast(Ui *ui)
{
	Theme *t = &ui->theme;
	if (M.pend.kind && ui->now >= M.pend.deadline)
		commit_pending();
	if (!M.toast[0] || ui->now >= M.toast_until)
		return;
	Font f = ui_font(ui, 13, 500);
	bool undo = M.toast_undo && M.pend.kind;
	f32 w = ui_text_width(ui, f, M.toast) + 40 + (undo ? 76 : 0);
	w = minf(w, ui->width - 32);
	Rect r = rect((ui->width - w) / 2, ui->height - 64, w, 44);
	ui_shadow(ui, r, 10, 18, 4, 0x50000000);
	ui_round(ui, r, 10, 0xf0303034);
	ui_text_in(ui, f, rect(r.x + 20, r.y, r.w - 40 - (undo ? 76 : 0), r.h), M.toast, 0xffffffff, ALIGN_START);
	if (undo) {
		Rect ub = rect(r.x + r.w - 86, r.y + 7, 72, 30);
		UiId id = ui_id(ui, "undo");
		UiInteract in = ui_interact(ui, id, ub);
		if (in.hovered)
			ui_round(ui, ub, 6, 0x30ffffff);
		ui_text_in(ui, ui_font(ui, 13, 700), ub, "Undo", color_lighten(t->accent, 0.35f), ALIGN_CENTER);
		if (in.clicked || ui_key_pressed(ui, KEY_Z, MOD_CTRL))
			undo_pending();
	}
	f64 next = M.pend.kind ? minf((f32)M.toast_until, (f32)M.pend.deadline) : M.toast_until;
	ui->wake_at = ui->wake_at ? minf((f32)ui->wake_at, (f32)next) : next;
}

static void remove_dialog(Ui *ui)
{
	Theme *t = &ui->theme;
	Rect c;
	if (!ui_dialog_begin(ui, "remove", 420, 190, &c))
		return;
	int i = M.confirm_remove;
	const char *email = i >= 0 && i < M.nacct ? M.accts[i].a.email : "";
	ui_text_in(ui, ui_font(ui, 17, 700), rect(c.x, c.y, c.w, 26), "Remove this account?", t->text, ALIGN_START);
	char body[300];
	fmt(body, sizeof body, "%s will be removed from Mail along with the copies of its messages kept on this computer. "
			       "Nothing is deleted from the server.", email);
	ui_text_wrapped(ui, ui_font(ui, 13, 400), rect(c.x, c.y + 36, c.w, 80), body, t->text_dim, ALIGN_START);
	Rect b = rect(c.x, c.y + c.h - 38, c.w, 38);
	if (ui_button_destructive(ui, cut_right(&b, 110), "Remove")) {
		ui_dialog_close(ui);
		remove_account(i);
	}
	cut_right(&b, 10);
	if (ui_button(ui, cut_right(&b, 100), "Cancel"))
		ui_dialog_close(ui);
	ui_dialog_end(ui);
}

static void shortcuts(Ui *ui)
{
	if (ui->focus || M.compose || M.setup.open)
		return;
	int idx = -1;
	if (M.sel)
		head_by_uid(M.sel, &idx);
	if (ui_key_pressed(ui, KEY_C, 0) || ui_key_pressed(ui, KEY_N, MOD_CTRL))
		compose_to("");
	else if (ui_key_pressed(ui, KEY_SLASH, 0) || ui_key_pressed(ui, KEY_F, MOD_CTRL))
		ui_focus(ui, "search");
	else if (ui_key_pressed(ui, KEY_J, 0) && M.nheads)
		open_message(M.heads[idx + 1 < M.nheads ? idx + 1 : idx].uid);
	else if (ui_key_pressed(ui, KEY_K, 0) && M.nheads)
		open_message(M.heads[idx > 0 ? idx - 1 : 0].uid);
	else if (ui_key_pressed(ui, KEY_R, MOD_CTRL) || ui_key_pressed(ui, KEY_F5, 0))
		fetch_heads(false);
	else if (M.sel) {
		Head *h = head_by_uid(M.sel, NULL);
		if (ui_key_pressed(ui, KEY_E, 0))
			archive_message(M.sel);
		else if (ui_key_pressed(ui, KEY_DELETE, 0) || ui_key_pressed(ui, KEY_3, MOD_SHIFT))
			delete_message(M.sel);
		else if (ui_key_pressed(ui, KEY_R, 0) && M.msg)
			reply(M.msg, REPLY);
		else if (ui_key_pressed(ui, KEY_A, 0) && M.msg)
			reply(M.msg, REPLY_ALL);
		else if (ui_key_pressed(ui, KEY_F, 0) && M.msg)
			reply(M.msg, FORWARD);
		else if (ui_key_pressed(ui, KEY_S, 0) && h)
			set_flag(h, "\\Flagged", !h->flagged);
		else if (ui_key_pressed(ui, KEY_U, MOD_SHIFT) && h) {
			set_flag(h, "\\Seen", false);
			close_message();
		} else if (ui_key_pressed(ui, KEY_U, 0) || ui_key_pressed(ui, KEY_ESC, 0))
			close_message();
	}
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	M.ui = ui;
	take_results();
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 48);
	Rect hb = ui_headerbar(ui, header, NULL, HEADER_NO_TITLE);
	hb = pad2(hb, 4, 8);
	if (M.setup.open || !M.nacct) {
		if (!M.setup.open)
			setup_open(-1);
		ui_text_in(ui, ui_font(ui, 14, 700), hb, "Mail", t->text, ALIGN_CENTER);
		ui_fill(ui, rect(win.x, win.y, win.w, 1), t->border);
		setup_page(ui, win);
		return;
	}
	bool three = ui->width >= 1120;
	bool side_fixed = ui->width >= 820;
	bool open = M.compose || M.sel;
	bool stacked = !three;
	if (!side_fixed && ui_icon_button(ui, cut_left(&hb, 32), "sidebar", "Folders"))
		M.sidebar_open = !M.sidebar_open;
	/* search */
	f32 sw = minf(460, hb.w - 120);
	Rect search = center_in(hb, sw, 32);
	int ev = ui_entry(ui, search, "search", &M.search, "Search mail", ENTRY_SEARCH);
	if ((ev & ENTRY_SUBMIT) && !blank(sb_str(&M.search))) {
		Acct *a = cur_acct();
		Job *j = job_new(J_SEARCH, a);
		str_copy(j->folder, M.folder, sizeof j->folder);
		trim_copy(sb_str(&M.search), j->query, sizeof j->query);
		str_copy(M.search_q, j->query, sizeof M.search_q);
		j->gen = M.heads_gen = ++M.gen;
		submit(j);
		M.loading = true;
		close_message();
		ui->focus = 0;
	}
	if (ev & ENTRY_ESCAPE) {
		sb_clear(&M.search);
		if (M.searching)
			open_folder(M.folder);
		ui->focus = 0;
	}
	if (!side_fixed && ui_icon_button(ui, cut_right(&hb, 32), "edit", "Compose (C)"))
		compose_to("");
	ui_fill(ui, rect(win.x, win.y, win.w, 1), t->border);
	win.y += 1, win.h -= 1;
	Rect side = { 0 };
	if (side_fixed)
		side = cut_left(&win, three ? 232 : 210);
	if (three) {
		Rect list = cut_left(&win, clampf(win.w * 0.4f, 340, 440));
		message_list(ui, list);
		ui_vseparator(ui, list.x + list.w, list.y, list.y + list.h);
		if (M.compose)
			compose_view(ui, win, false);
		else
			reading_pane(ui, win, false);
	} else if (open) {
		if (M.compose)
			compose_view(ui, win, true);
		else
			reading_pane(ui, win, true);
	} else {
		message_list(ui, win);
	}
	if (side_fixed) {
		sidebar(ui, side);
		ui_vseparator(ui, side.x + side.w, side.y, side.y + side.h);
	} else if (M.sidebar_open) {
		Rect shade = win;
		UiInteract in = ui_interact(ui, ui_id(ui, "shade"), shade);
		ui_fill(ui, shade, 0x40000000);
		if (in.clicked)
			M.sidebar_open = false;
		Rect s = rect(0, win.y, minf(260, ui->width * 0.8f), win.h);
		ui_shadow(ui, s, 0, 24, 0, 0x50000000);
		sidebar(ui, s);
	}
	remove_dialog(ui);
	shortcuts(ui);
	if (stacked && open && ui_key_pressed(ui, KEY_LEFT, MOD_ALT)) {
		if (M.compose)
			close_compose(true);
		else
			close_message();
	}
	draw_toast(ui);
	if (atomic_load(&M.busy) || M.loading)
		ui->wake_at = ui->wake_at ? minf((f32)ui->wake_at, (f32)(ui->now + 0.25)) : ui->now + 0.25;
}

static void poll(void)
{
	M.last_poll = M.ui->now;
	for (int i = 0; i < M.nacct; i++) {
		Acct *a = &M.accts[i];
		if (a->state == AS_FAILED || (!a->nfolders && a->state != AS_CONNECTING))
			connect_account(a);
		else
			submit(job_new(J_CHECK, a));
	}
	if (!M.searching)
		fetch_heads(true);
}

static void tick(Ui *ui)
{
	M.ui = ui;
	if (M.nacct && ui->now - M.last_poll >= POLL_SECONDS)
		poll();
}

static bool on_close(Ui *ui)
{
	commit_pending();
	if (M.compose && !M.compose->sending)
		close_compose(true);
	/* give moves, flags and drafts a moment to reach the server */
	for (int i = 0; i < 100 && atomic_load(&M.busy) > 0; i++)
		sleep_ms(50);
	return true;
}

static char mailto_arg[256];

static void init(Ui *ui)
{
	M.ui = ui;
	M.save_att = -1;
	M.confirm_remove = -1;
	M.h24 = use_24h();
	view_start(ui);
	thread_spawn(worker, NULL);
	load_accounts();
	if (!M.nacct) {
		setup_open(-1);
		return;
	}
	M.last_poll = ui->now;
	for (int i = 0; i < M.nacct; i++)
		connect_account(&M.accts[i]);
	open_folder("INBOX");
	if (mailto_arg[0])
		compose_to(mailto_arg);
}

int main(int argc, char **argv)
{
	UiAppDesc d = { .id = "mail", .title = "Mail", .width = 1240, .height = 800, .min_width = 420, .min_height = 480,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame, .tick = tick, .tick_ms = 15000, .close = on_close };
	/* mailto: links from other apps */
	if (argc > 1 && str_eq_nocase_n(argv[1], "mailto:", 7)) {
		str_copy(mailto_arg, argv[1] + 7, sizeof mailto_arg);
		char *q = (char *)str_chr(mailto_arg, '?');
		if (q)
			*q = 0;
	}
	return ui_run(&d);
}
