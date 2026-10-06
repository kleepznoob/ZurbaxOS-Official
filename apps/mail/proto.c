/* IMAP (reading) and SMTP (sending), over TLS or upgraded with STARTTLS. */
#include "mail.h"

/* ---- Connections ------------------------------------------------------------------------------ */

typedef struct {
	int fd;
	Tls *tls;
	u8 buf[16384];
	usize len, pos;
	bool dead;
} Conn;

/* Ports that start in plain text and switch to TLS with STARTTLS: 143, 587 and 25,
 * and the same endings used by local bridges (1143, 1025). Others use TLS throughout. */
static bool starttls_port(u16 port)
{
	return port % 1000 == 143 || port % 1000 == 587 || port % 1000 == 25 || port == 2525;
}

static bool conn_open(Conn *c, const char *host, u16 port, bool tls, char *err, usize cap)
{
	memset(c, 0, sizeof *c);
	c->fd = tcp_connect(host, port, 20000, err, cap);
	if (c->fd < 0)
		return false;
	/* a server that goes quiet must not hang the worker forever */
	struct { long sec, usec; } tv = { 90, 0 };
	sys_setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	sys_setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (tls) {
		c->tls = tls_connect(c->fd, host, err, cap);
		if (!c->tls) {
			sys_close(c->fd);
			c->fd = -1;
			return false;
		}
	}
	return true;
}

static bool conn_starttls(Conn *c, const char *host, char *err, usize cap)
{
	c->len = c->pos = 0;   /* nothing read before the switch may be trusted */
	c->tls = tls_connect(c->fd, host, err, cap);
	if (!c->tls) {
		sys_close(c->fd);
		c->fd = -1;
		c->dead = true;
	}
	return c->tls != NULL;
}

static void conn_close(Conn *c)
{
	if (c->tls)
		tls_close(c->tls);
	else if (c->fd >= 0)
		sys_close(c->fd);
	c->tls = NULL;
	c->fd = -1;
}

static bool conn_write(Conn *c, const void *d, usize n)
{
	if (c->dead)
		return false;
	bool ok = c->tls ? tls_write(c->tls, d, n) : write_all(c->fd, d, n);
	if (!ok)
		c->dead = true;
	return ok;
}

static bool conn_fill(Conn *c)
{
	if (c->dead)
		return false;
	long n = c->tls ? tls_read(c->tls, c->buf, sizeof c->buf) : sys_read(c->fd, c->buf, sizeof c->buf);
	if (n <= 0) {
		c->dead = true;
		return false;
	}
	c->len = (usize)n;
	c->pos = 0;
	return true;
}

static int conn_peek(Conn *c)
{
	if (c->pos >= c->len && !conn_fill(c))
		return -1;
	return c->buf[c->pos];
}

static int conn_get(Conn *c)
{
	int ch = conn_peek(c);
	if (ch >= 0)
		c->pos++;
	return ch;
}

static bool conn_line(Conn *c, StrBuf *out)
{
	sb_clear(out);
	for (;;) {
		int ch = conn_get(c);
		if (ch < 0)
			return false;
		if (ch == '\n')
			break;
		if (ch != '\r')
			sb_addc(out, (char)ch);
	}
	return true;
}

/* ---- IMAP values ------------------------------------------------------------------------------- */

enum { IV_NIL, IV_ATOM, IV_STR, IV_LIST };

typedef struct IVal IVal;
struct IVal {
	u8 kind;
	char *s;
	usize len;
	IVal *v;
	int n;
};

static void iv_free(IVal *x)
{
	mem_free(x->s);
	for (int i = 0; i < x->n; i++)
		iv_free(&x->v[i]);
	mem_free(x->v);
	memset(x, 0, sizeof *x);
}

static void iv_push(IVal *list, IVal item)
{
	if (list->n % 16 == 0)
		list->v = mem_realloc(list->v, sizeof(IVal) * (usize)(list->n + 16));
	list->v[list->n++] = item;
}

static bool iv_parse(Conn *c, IVal *out, int depth);

static bool iv_list_until(Conn *c, IVal *list, int close, int depth)
{
	list->kind = IV_LIST;
	for (;;) {
		int ch = conn_peek(c);
		if (ch < 0)
			return false;
		if (ch == ' ') {
			conn_get(c);
			continue;
		}
		if (close == ')' && ch == ')') {
			conn_get(c);
			return true;
		}
		if (close == '\n' && (ch == '\r' || ch == '\n')) {
			conn_get(c);
			if (ch == '\r' && conn_peek(c) == '\n')
				conn_get(c);
			return true;
		}
		IVal item = { 0 };
		if (!iv_parse(c, &item, depth + 1))
			return false;
		iv_push(list, item);
	}
}

static bool iv_parse(Conn *c, IVal *out, int depth)
{
	memset(out, 0, sizeof *out);
	if (depth > 64)
		return false;
	int ch = conn_peek(c);
	if (ch < 0)
		return false;
	if (ch == '(') {
		conn_get(c);
		return iv_list_until(c, out, ')', depth);
	}
	StrBuf sb = { 0 };
	if (ch == '"') {
		conn_get(c);
		for (;;) {
			int k = conn_get(c);
			if (k < 0)
				return false;
			if (k == '"')
				break;
			if (k == '\\')
				k = conn_get(c);
			sb_addc(&sb, (char)k);
		}
		out->kind = IV_STR;
	} else if (ch == '{') {
		conn_get(c);
		usize n = 0;
		int k;
		while ((k = conn_get(c)) >= 0 && k != '}')
			if (is_digit(k))
				n = n * 10 + (usize)(k - '0');
		while ((k = conn_get(c)) >= 0 && k != '\n')
			;
		sb_reserve(&sb, n + 1);
		for (usize i = 0; i < n; i++) {
			int b = conn_get(c);
			if (b < 0)
				return false;
			sb.data[sb.len++] = (char)b;
		}
		if (sb.data)
			sb.data[sb.len] = 0;
		out->kind = IV_STR;
	} else {
		int bracket = 0;
		for (;;) {
			int k = conn_peek(c);
			if (k < 0)
				break;
			if (!bracket && (k == ' ' || k == '(' || k == ')' || k == '\r' || k == '\n'))
				break;
			if (k == '[')
				bracket++;
			else if (k == ']' && bracket)
				bracket--;
			sb_addc(&sb, (char)conn_get(c));
		}
		out->kind = str_eq_nocase(sb_str(&sb), "NIL") ? IV_NIL : IV_ATOM;
	}
	out->len = sb.len;
	out->s = sb.data ? sb_take(&sb) : str_dup("");
	return true;
}

static const char *iv_str(const IVal *x)
{
	return x && (x->kind == IV_STR || x->kind == IV_ATOM) ? x->s : "";
}

/* ---- IMAP commands ------------------------------------------------------------------------------- */

struct Imap {
	Conn c;
	int tag;
	char err[200];
	char caps[1024];
	char selected[256];
	VEC(IVal) untagged;
	bool gmail;
};

const char *imap_error(Imap *m)
{
	return m->err;
}

static void clear_untagged(Imap *m)
{
	vec_each(&m->untagged, u) iv_free(u);
	vec_clear(&m->untagged);
}

/* Reads responses until our tag comes back. */
static bool imap_finish(Imap *m, const char *tag)
{
	for (;;) {
		IVal line = { 0 };
		if (!iv_list_until(&m->c, &line, '\n', 0)) {
			iv_free(&line);
			str_copy(m->err, "the mail server stopped answering", sizeof m->err);
			return false;
		}
		const char *first = line.n ? iv_str(&line.v[0]) : "";
		if (str_eq(first, tag)) {
			const char *status = line.n > 1 ? iv_str(&line.v[1]) : "";
			bool ok = str_eq_nocase(status, "OK");
			if (!ok) {
				StrBuf sb = { 0 };
				for (int i = 2; i < line.n; i++) {
					if (sb.len)
						sb_addc(&sb, ' ');
					sb_adds(&sb, iv_str(&line.v[i]));
				}
				str_copy(m->err, sb.len ? sb_str(&sb) : "the server refused", sizeof m->err);
				sb_free(&sb);
			}
			iv_free(&line);
			return ok;
		}
		if (str_eq(first, "*")) {
			if (line.n > 1 && str_eq_nocase(iv_str(&line.v[1]), "CAPABILITY")) {
				StrBuf sb = { 0 };
				for (int i = 2; i < line.n; i++) {
					sb_addc(&sb, ' ');
					sb_adds(&sb, iv_str(&line.v[i]));
				}
				sb_addc(&sb, ' ');
				str_copy(m->caps, sb_str(&sb), sizeof m->caps);
				sb_free(&sb);
			}
			vec_push(&m->untagged, line);
		} else {
			iv_free(&line);   /* continuation or junk */
		}
	}
}

static bool imap_cmd(Imap *m, const char *f, ...) PRINTF_LIKE(2, 3);
static bool imap_cmd(Imap *m, const char *f, ...)
{
	clear_untagged(m);
	char tag[16];
	fmt(tag, sizeof tag, "Z%03d", ++m->tag);
	va_list ap;
	va_start(ap, f);
	char *cmd = str_fmt_v(f, ap);
	va_end(ap);
	char *line = str_fmt("%s %s\r\n", tag, cmd);
	bool ok = conn_write(&m->c, line, strlen(line));
	mem_free(line);
	mem_free(cmd);
	if (!ok) {
		str_copy(m->err, "the connection to the mail server broke", sizeof m->err);
		return false;
	}
	return imap_finish(m, tag);
}

static bool has_cap(Imap *m, const char *cap)
{
	char k[64];
	fmt(k, sizeof k, " %s ", cap);
	return str_find_nocase(m->caps, k) != NULL;
}

/* "folder name" with quotes and backslashes escaped. */
static char *quoted(const char *s)
{
	StrBuf sb = { 0 };
	sb_addc(&sb, '"');
	for (; *s; s++) {
		if (*s == '\r' || *s == '\n')
			continue;   /* a line break would end the command early */
		if (*s == '"' || *s == '\\')
			sb_addc(&sb, '\\');
		sb_addc(&sb, *s);
	}
	sb_addc(&sb, '"');
	return sb_take(&sb);
}

Imap *imap_open(const Account *a, char *err, usize errcap)
{
	Imap *m = NEW(Imap);
	bool tls = !starttls_port(a->imap_port);
	if (!conn_open(&m->c, a->imap_host, a->imap_port, tls, err, errcap)) {
		mem_free(m);
		return NULL;
	}
	m->gmail = str_find_nocase(a->imap_host, "gmail") != NULL;
	StrBuf greet = { 0 };
	if (!conn_line(&m->c, &greet) || !str_starts(sb_str(&greet), "* OK")) {
		str_copy(err, "the mail server didn't say hello", errcap);
		sb_free(&greet);
		imap_close(m);
		return NULL;
	}
	sb_free(&greet);
	imap_cmd(m, "CAPABILITY");
	if (!tls) {
		/* never send a password over a connection that stayed in plain text */
		if (!has_cap(m, "STARTTLS")) {
			str_copy(err, "The mail server doesn't offer encryption (STARTTLS), so Mail won't send your password to it.", errcap);
			imap_close(m);
			return NULL;
		}
		if (!imap_cmd(m, "STARTTLS") || !conn_starttls(&m->c, a->imap_host, err, errcap)) {
			imap_close(m);
			return NULL;
		}
		imap_cmd(m, "CAPABILITY");
	}
	/* AUTHENTICATE PLAIN keeps odd characters in passwords safe */
	StrBuf cred = { 0 };
	sb_addc(&cred, 0);
	sb_adds(&cred, a->email);
	sb_addc(&cred, 0);
	sb_adds(&cred, a->password);
	char *b64 = base64_encode(cred.data, cred.len);
	sb_free(&cred);
	bool ok;
	if (has_cap(m, "AUTH=PLAIN") && has_cap(m, "SASL-IR")) {
		ok = imap_cmd(m, "AUTHENTICATE PLAIN %s", b64);
	} else {
		char *u = quoted(a->email), *p = quoted(a->password);
		ok = imap_cmd(m, "LOGIN %s %s", u, p);
		mem_free(u);
		mem_free(p);
	}
	mem_free(b64);
	if (!ok) {
		fmt(err, errcap, "The server didn't accept your address and password (%s).", m->err);
		imap_close(m);
		return NULL;
	}
	imap_cmd(m, "CAPABILITY");
	return m;
}

void imap_close(Imap *m)
{
	if (!m)
		return;
	if (!m->c.dead && m->c.fd >= 0) {
		const char *bye = "ZBYE LOGOUT\r\n";
		conn_write(&m->c, bye, strlen(bye));
	}
	conn_close(&m->c);
	clear_untagged(m);
	vec_free(&m->untagged);
	mem_free(m);
}

/* Modified UTF-7 folder names ("&AOk-t&AOk-") to UTF-8. */
static void mutf7(const char *s, char *out, usize cap)
{
	StrBuf sb = { 0 };
	for (const char *p = s; *p; p++) {
		if (*p != '&') {
			sb_addc(&sb, *p);
			continue;
		}
		const char *e = str_chr(p, '-');
		if (!e)
			break;
		if (e == p + 1) {
			sb_addc(&sb, '&');
			p = e;
			continue;
		}
		char b64[256];
		usize n = 0;
		for (const char *q = p + 1; q < e && n < sizeof b64 - 4; q++)
			b64[n++] = *q == ',' ? '/' : *q;
		while (n % 4)
			b64[n++] = '=';
		b64[n] = 0;
		usize bl;
		u8 *bytes = base64_decode(b64, n, &bl);
		for (usize i = 0; bytes && i + 1 < bl; i += 2) {
			u32 cp = (u32)bytes[i] << 8 | bytes[i + 1];
			if (cp >= 0xd800 && cp < 0xdc00 && i + 3 < bl) {
				u32 lo = (u32)bytes[i + 2] << 8 | bytes[i + 3];
				cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				i += 2;
			}
			sb_addcp(&sb, cp);
		}
		mem_free(bytes);
		p = e;
	}
	str_copy(out, sb_str(&sb), cap);
	sb_free(&sb);
}

int imap_folders(Imap *m, Folder *out, int max)
{
	if (!imap_cmd(m, "LIST \"\" \"*\""))
		return 0;
	int n = 0;
	vec_each(&m->untagged, u)
	{
		if (u->n < 5 || !str_eq_nocase(iv_str(&u->v[1]), "LIST") || n >= max)
			continue;
		IVal *flags = &u->v[2];
		bool noselect = false;
		Folder f = { 0 };
		for (int i = 0; flags->kind == IV_LIST && i < flags->n; i++) {
			const char *fl = iv_str(&flags->v[i]);
			if (str_eq_nocase(fl, "\\Noselect") || str_eq_nocase(fl, "\\NonExistent"))
				noselect = true;
			else if (str_eq_nocase(fl, "\\Sent")) str_copy(f.role, "sent", sizeof f.role);
			else if (str_eq_nocase(fl, "\\Drafts")) str_copy(f.role, "drafts", sizeof f.role);
			else if (str_eq_nocase(fl, "\\Trash")) str_copy(f.role, "trash", sizeof f.role);
			else if (str_eq_nocase(fl, "\\Junk")) str_copy(f.role, "junk", sizeof f.role);
			else if (str_eq_nocase(fl, "\\Archive") || str_eq_nocase(fl, "\\All")) str_copy(f.role, "archive", sizeof f.role);
			else if (str_eq_nocase(fl, "\\Flagged")) str_copy(f.role, "flagged", sizeof f.role);
		}
		if (noselect)
			continue;
		str_copy(f.raw, iv_str(&u->v[4]), sizeof f.raw);
		mutf7(f.raw, f.name, sizeof f.name);
		if (str_eq_nocase(f.raw, "INBOX"))
			str_copy(f.role, "inbox", sizeof f.role);
		/* servers without special-use flags: guess from the name */
		if (!f.role[0]) {
			const char *base = str_rchr(f.name, '/') ? str_rchr(f.name, '/') + 1 : str_rchr(f.name, '.') ? str_rchr(f.name, '.') + 1 : f.name;
			if (str_eq_nocase(base, "Sent") || str_eq_nocase(base, "Sent Items") || str_eq_nocase(base, "Sent Mail")) str_copy(f.role, "sent", sizeof f.role);
			else if (str_eq_nocase(base, "Drafts")) str_copy(f.role, "drafts", sizeof f.role);
			else if (str_eq_nocase(base, "Trash") || str_eq_nocase(base, "Deleted Items") || str_eq_nocase(base, "Bin")) str_copy(f.role, "trash", sizeof f.role);
			else if (str_eq_nocase(base, "Spam") || str_eq_nocase(base, "Junk") || str_eq_nocase(base, "Junk E-mail")) str_copy(f.role, "junk", sizeof f.role);
			else if (str_eq_nocase(base, "Archive") || str_eq_nocase(base, "All Mail")) str_copy(f.role, "archive", sizeof f.role);
		}
		out[n++] = f;
	}
	return n;
}

static bool select_folder(Imap *m, const char *folder, u32 *exists)
{
	char *q = quoted(folder);
	bool ok = imap_cmd(m, "SELECT %s", q);
	mem_free(q);
	if (!ok)
		return false;
	str_copy(m->selected, folder, sizeof m->selected);
	if (exists) {
		*exists = 0;
		vec_each(&m->untagged, u) if (u->n >= 3 && str_eq_nocase(iv_str(&u->v[2]), "EXISTS")) *exists = (u32)str_to_int(iv_str(&u->v[1]), NULL);
	}
	return true;
}

static bool ensure_selected(Imap *m, const char *folder)
{
	if (str_eq(m->selected, folder))
		return true;
	return select_folder(m, folder, NULL);
}

static void address_of(IVal *list, char *name, usize ncap, char *addr, usize acap)
{
	name[0] = addr[0] = 0;
	if (!list || list->kind != IV_LIST || !list->n || list->v[0].kind != IV_LIST || list->v[0].n < 4)
		return;
	IVal *a = &list->v[0];
	char *n = mime_words(iv_str(&a->v[0]));
	str_copy(name, n, ncap);
	mem_free(n);
	fmt(addr, acap, "%s@%s", iv_str(&a->v[2]), iv_str(&a->v[3]));
}

/* A short preview from the start of the first part, whatever its encoding. */
static void make_snippet(const char *raw, usize n, char *out, usize cap)
{
	char *text = NULL;
	usize tl = 0;
	/* Part 1 can itself be a multipart (text and HTML): use its first part. */
	usize lead = 0;
	while (lead < n && is_space(raw[lead]))
		lead++;
	if (n - lead > 2 && raw[lead] == '-' && raw[lead + 1] == '-') {
		const char *end = raw + n, *p = raw + lead;
		const char *eol = memchr(p, '\n', (usize)(end - p));
		usize blen = eol ? (usize)(eol - p) : 0;
		while (blen && (p[blen - 1] == '\r' || p[blen - 1] == ' '))
			blen--;
		/* skip the part's headers */
		const char *body = NULL;
		for (const char *q = eol; q && q + 1 < end; q = memchr(q + 1, '\n', (usize)(end - q - 1))) {
			if (q[1] == '\n' || (q[1] == '\r' && q + 2 < end && q[2] == '\n')) {
				body = q + (q[1] == '\n' ? 2 : 3);
				break;
			}
		}
		if (body && blen) {
			const char *stop = end;
			for (const char *q = body; q + blen <= end; q++)
				if (*q == '-' && (q == body || q[-1] == '\n') && memcmp(q, p, blen) == 0) {
					stop = q;
					break;
				}
			raw = body;
			n = (usize)(stop - body);
		}
	}
	bool b64 = n > 20;
	for (usize i = 0; i < MIN(n, 76); i++)
		if (!(is_alnum(raw[i]) || raw[i] == '+' || raw[i] == '/' || raw[i] == '=' || raw[i] == '\r' || raw[i] == '\n'))
			b64 = false;
	if (b64) {
		usize cut = n - n % 4;
		u8 *d = base64_decode(raw, cut, &tl);
		text = d ? mime_to_utf8((char *)d, tl, "utf-8", &tl) : NULL;
		mem_free(d);
	}
	if (!text) {
		/* quoted-printable bits are harmless to undo on plain text */
		StrBuf sb = { 0 };
		for (usize i = 0; i < n; i++) {
			if (raw[i] == '=' && i + 2 < n && is_xdigit(raw[i + 1]) && is_xdigit(raw[i + 2])) {
				int v = (is_digit(raw[i + 1]) ? raw[i + 1] - '0' : (raw[i + 1] | 32) - 'a' + 10) * 16 +
					(is_digit(raw[i + 2]) ? raw[i + 2] - '0' : (raw[i + 2] | 32) - 'a' + 10);
				sb_addc(&sb, (char)v);
				i += 2;
			} else if (raw[i] == '=' && i + 1 < n && (raw[i + 1] == '\r' || raw[i + 1] == '\n')) {
				i++;
				if (i + 1 < n && raw[i] == '\r' && raw[i + 1] == '\n')
					i++;
			} else {
				sb_addc(&sb, raw[i]);
			}
		}
		text = mime_to_utf8(sb_str(&sb), sb.len, "utf-8", &tl);
		sb_free(&sb);
	}
	/* drop tags, style blocks and runs of white space */
	usize w = 0;
	bool tag = false, space = false;
	const char *p = text;
	while (*p && w + 4 < cap) {
		if (*p == '<') {
			if (str_eq_nocase_n(p, "<style", 6) || str_eq_nocase_n(p, "<script", 7)) {
				const char *e = str_find_nocase(p, "</st");
				if (!e)
					e = str_find_nocase(p, "</sc");
				p = e ? e : p + strlen(p);
				continue;
			}
			tag = true;
		} else if (*p == '>') {
			tag = false;
			p++;
			continue;
		}
		if (tag) {
			p++;
			continue;
		}
		if (*p == '&') {
			const char *semi = str_chr(p, ';');
			if (semi && semi - p < 8) {
				if (!space && w)
					out[w++] = ' ';
				space = true;
				p = semi + 1;
				continue;
			}
		}
		if (is_space(*p)) {
			if (!space && w)
				out[w++] = ' ';
			space = true;
		} else {
			out[w++] = *p;
			space = false;
		}
		p++;
	}
	while (w && out[w - 1] == ' ')
		w--;
	/* don't end in the middle of a character */
	while (w && ((u8)out[w - 1] & 0xc0) == 0x80)
		w--;
	if (w && (u8)out[w - 1] >= 0xc0)
		w--;
	out[w] = 0;
	mem_free(text);
}

/* Reads the FETCH replies of a heads request, newest first. */
static int collect_heads(Imap *m, Head *out, int max)
{
	int n = 0;
	vec_each(&m->untagged, u)
	{
		if (u->n < 4 || !str_eq_nocase(iv_str(&u->v[2]), "FETCH") || u->v[3].kind != IV_LIST || n >= max)
			continue;
		IVal *kv = &u->v[3];
		Head h = { 0 };
		i64 internal = 0;
		for (int i = 0; i + 1 < kv->n; i += 2) {
			const char *k = iv_str(&kv->v[i]);
			IVal *v = &kv->v[i + 1];
			if (str_eq_nocase(k, "UID")) {
				h.uid = (u32)str_to_int(iv_str(v), NULL);
			} else if (str_eq_nocase(k, "FLAGS")) {
				for (int f = 0; f < v->n; f++) {
					const char *fl = iv_str(&v->v[f]);
					if (str_eq_nocase(fl, "\\Seen")) h.seen = true;
					if (str_eq_nocase(fl, "\\Flagged")) h.flagged = true;
					if (str_eq_nocase(fl, "\\Answered")) h.answered = true;
				}
			} else if (str_eq_nocase(k, "INTERNALDATE")) {
				internal = mime_date(iv_str(v));
			} else if (str_eq_nocase(k, "RFC822.SIZE")) {
				h.size = (u32)str_to_int(iv_str(v), NULL);
			} else if (str_eq_nocase(k, "ENVELOPE") && v->kind == IV_LIST && v->n >= 10) {
				h.date = mime_date(iv_str(&v->v[0]));
				char *subj = mime_words(iv_str(&v->v[1]));
				str_copy(h.subject, subj, sizeof h.subject);
				mem_free(subj);
				address_of(&v->v[2], h.from_name, sizeof h.from_name, h.from_addr, sizeof h.from_addr);
				char tn[96], ta[128];
				address_of(&v->v[5], tn, sizeof tn, ta, sizeof ta);
				str_copy(h.to, tn[0] ? tn : ta, sizeof h.to);
				str_copy(h.message_id, iv_str(&v->v[9]), sizeof h.message_id);
			} else if (str_starts(k, "BODY[")) {
				make_snippet(v->s ? v->s : "", v->len, h.snippet, sizeof h.snippet);
			}
		}
		if (!h.date)
			h.date = internal;
		if (h.uid)
			out[n++] = h;
	}
	/* newest first, by UID (servers answer in their own order) */
	for (int i = 1; i < n; i++) {
		Head t = out[i];
		int j = i - 1;
		while (j >= 0 && out[j].uid < t.uid) {
			out[j + 1] = out[j];
			j--;
		}
		out[j + 1] = t;
	}
	return n;
}

#define HEAD_ITEMS "(UID FLAGS INTERNALDATE RFC822.SIZE ENVELOPE BODY.PEEK[1]<0.900>)"

int imap_heads(Imap *m, const char *folder, Head *out, int max, u32 *exists_out)
{
	u32 exists = 0;
	if (!select_folder(m, folder, &exists))
		return -1;
	if (exists_out)
		*exists_out = exists;
	if (!exists)
		return 0;
	u32 first = exists > (u32)max ? exists - (u32)max + 1 : 1;
	if (!imap_cmd(m, "FETCH %u:%u " HEAD_ITEMS, first, exists))
		return -1;
	return collect_heads(m, out, max);
}

int imap_search(Imap *m, const char *folder, const char *query, Head *out, int max)
{
	if (!ensure_selected(m, folder))
		return -1;
	/* each word must appear somewhere: TEXT a TEXT b */
	StrBuf cmd = { 0 };
	sb_adds(&cmd, "UID SEARCH CHARSET UTF-8");
	char word[128];
	const char *p = query;
	int words = 0;
	while (*p) {
		while (is_space(*p))
			p++;
		usize k = 0;
		while (*p && !is_space(*p) && k < sizeof word - 1)
			word[k++] = *p++;
		word[k] = 0;
		if (!k)
			continue;
		char *q = quoted(word);
		sb_fmt(&cmd, " TEXT %s", q);
		mem_free(q);
		words++;
	}
	if (!words) {
		sb_free(&cmd);
		return 0;
	}
	bool ok = imap_cmd(m, "%s", sb_str(&cmd));
	sb_free(&cmd);
	if (!ok)
		return -1;
	VEC(u32) uids = { 0 };
	vec_each(&m->untagged, u)
	{
		if (u->n < 2 || !str_eq_nocase(iv_str(&u->v[1]), "SEARCH"))
			continue;
		for (int i = 2; i < u->n; i++)
			vec_push(&uids, (u32)str_to_int(iv_str(&u->v[i]), NULL));
	}
	if (!uids.len) {
		vec_free(&uids);
		return 0;
	}
	/* the newest `max` */
	StrBuf set = { 0 };
	usize from = uids.len > (usize)max ? uids.len - (usize)max : 0;
	for (usize i = from; i < uids.len; i++)
		sb_fmt(&set, "%s%u", i > from ? "," : "", uids.data[i]);
	vec_free(&uids);
	ok = imap_cmd(m, "UID FETCH %s " HEAD_ITEMS, sb_str(&set));
	sb_free(&set);
	return ok ? collect_heads(m, out, max) : -1;
}

int imap_unseen(Imap *m, const char *folder)
{
	char *q = quoted(folder);
	bool ok = imap_cmd(m, "STATUS %s (UNSEEN)", q);
	mem_free(q);
	if (!ok)
		return -1;
	vec_each(&m->untagged, u)
	{
		if (u->n < 4 || !str_eq_nocase(iv_str(&u->v[1]), "STATUS") || u->v[3].kind != IV_LIST)
			continue;
		IVal *kv = &u->v[3];
		for (int i = 0; i + 1 < kv->n; i += 2)
			if (str_eq_nocase(iv_str(&kv->v[i]), "UNSEEN"))
				return (int)str_to_int(iv_str(&kv->v[i + 1]), NULL);
	}
	return -1;
}

bool imap_alive(Imap *m)
{
	return m && !m->c.dead && imap_cmd(m, "NOOP");
}

bool imap_dead(Imap *m)
{
	return m->c.dead;
}

bool imap_is_gmail(Imap *m)
{
	return m->gmail;
}

char *imap_message(Imap *m, const char *folder, u32 uid, usize *len)
{
	if (!ensure_selected(m, folder) || !imap_cmd(m, "UID FETCH %u (BODY.PEEK[])", uid))
		return NULL;
	vec_each(&m->untagged, u)
	{
		if (u->n < 4 || u->v[3].kind != IV_LIST)
			continue;
		IVal *kv = &u->v[3];
		for (int i = 0; i + 1 < kv->n; i += 2)
			if (str_eq_nocase(iv_str(&kv->v[i]), "BODY[]")) {
				IVal *v = &kv->v[i + 1];
				*len = v->len;
				char *r = v->s;
				v->s = NULL;
				return r ? r : str_dup("");
			}
	}
	str_copy(m->err, "the message wasn't found", sizeof m->err);
	return NULL;
}

bool imap_flag(Imap *m, const char *folder, u32 uid, const char *flag, bool on)
{
	return ensure_selected(m, folder) && imap_cmd(m, "UID STORE %u %cFLAGS.SILENT (%s)", uid, on ? '+' : '-', flag);
}

bool imap_move(Imap *m, const char *folder, u32 uid, const char *to)
{
	if (!ensure_selected(m, folder))
		return false;
	char *q = quoted(to);
	bool ok;
	if (has_cap(m, "MOVE")) {
		ok = imap_cmd(m, "UID MOVE %u %s", uid, q);
	} else {
		ok = imap_cmd(m, "UID COPY %u %s", uid, q) && imap_cmd(m, "UID STORE %u +FLAGS.SILENT (\\Deleted)", uid) &&
		     (has_cap(m, "UIDPLUS") ? imap_cmd(m, "UID EXPUNGE %u", uid) : imap_cmd(m, "EXPUNGE"));
	}
	mem_free(q);
	return ok;
}

bool imap_delete(Imap *m, const char *folder, u32 uid)
{
	return ensure_selected(m, folder) && imap_cmd(m, "UID STORE %u +FLAGS.SILENT (\\Deleted)", uid) &&
	       (has_cap(m, "UIDPLUS") ? imap_cmd(m, "UID EXPUNGE %u", uid) : imap_cmd(m, "EXPUNGE"));
}

bool imap_append(Imap *m, const char *folder, const char *flags, const char *msg, usize len)
{
	clear_untagged(m);
	char tag[16];
	fmt(tag, sizeof tag, "Z%03d", ++m->tag);
	char *q = quoted(folder);
	char *line = str_fmt("%s APPEND %s (%s) {%llu}\r\n", tag, q, flags, (unsigned long long)len);
	mem_free(q);
	bool ok = conn_write(&m->c, line, strlen(line));
	mem_free(line);
	if (!ok)
		return false;
	/* wait for the go-ahead */
	StrBuf sb = { 0 };
	for (;;) {
		if (!conn_line(&m->c, &sb)) {
			sb_free(&sb);
			return false;
		}
		if (str_starts(sb_str(&sb), "+"))
			break;
		if (str_starts(sb_str(&sb), tag)) {
			str_copy(m->err, sb_str(&sb), sizeof m->err);
			sb_free(&sb);
			return false;
		}
	}
	sb_free(&sb);
	ok = conn_write(&m->c, msg, len) && conn_write(&m->c, "\r\n", 2);
	return ok && imap_finish(m, tag);
}

/* ---- SMTP -------------------------------------------------------------------------------------- */

/* Reads a (possibly multi-line) reply; returns its code. */
static int smtp_reply(Conn *c, StrBuf *text)
{
	StrBuf line = { 0 };
	sb_clear(text);
	int code = 0;
	for (;;) {
		if (!conn_line(c, &line)) {
			sb_free(&line);
			return 0;
		}
		code = (int)str_to_int(sb_str(&line), NULL);
		if (line.len > 4)
			sb_adds(text, sb_str(&line) + 4), sb_addc(text, '\n');
		if (line.len < 4 || line.data[3] != '-')
			break;
	}
	sb_free(&line);
	return code;
}

static int smtp_cmd(Conn *c, StrBuf *text, const char *f, ...) PRINTF_LIKE(3, 4);
static int smtp_cmd(Conn *c, StrBuf *text, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	char *cmd = str_fmt_v(f, ap);
	va_end(ap);
	char *line = str_fmt("%s\r\n", cmd);
	bool ok = conn_write(c, line, strlen(line));
	mem_free(line);
	mem_free(cmd);
	return ok ? smtp_reply(c, text) : 0;
}

bool smtp_send(const Account *a, const char *const *rcpt, int nrcpt, const char *msg, usize len, char *err, usize errcap)
{
	Conn c;
	bool implicit = !starttls_port(a->smtp_port);
	if (!conn_open(&c, a->smtp_host, a->smtp_port, implicit, err, errcap))
		return false;
	StrBuf t = { 0 };
	bool ok = false;
	int code = smtp_reply(&c, &t);
	if (code != 220) {
		str_copy(err, "The mail server for sending didn't answer.", errcap);
		goto out;
	}
	code = smtp_cmd(&c, &t, "EHLO zurbax");
	if (code != 250)
		code = smtp_cmd(&c, &t, "HELO zurbax");
	if (!implicit) {
		/* never send a password over a connection that stayed in plain text */
		if (!str_find_nocase(sb_str(&t), "STARTTLS")) {
			str_copy(err, "The server for sending doesn't offer encryption (STARTTLS), so Mail won't send your password to it.", errcap);
			goto out;
		}
		if (smtp_cmd(&c, &t, "STARTTLS") != 220 || !conn_starttls(&c, a->smtp_host, err, errcap))
			goto out;
		smtp_cmd(&c, &t, "EHLO zurbax");
	}
	if (str_find_nocase(sb_str(&t), "AUTH")) {
		StrBuf cred = { 0 };
		sb_addc(&cred, 0);
		sb_adds(&cred, a->email);
		sb_addc(&cred, 0);
		sb_adds(&cred, a->password);
		char *b64 = base64_encode(cred.data, cred.len);
		sb_free(&cred);
		code = smtp_cmd(&c, &t, "AUTH PLAIN %s", b64);
		mem_free(b64);
		if (code != 235) {
			/* AUTH LOGIN, for older servers */
			char *u = base64_encode(a->email, strlen(a->email)), *p = base64_encode(a->password, strlen(a->password));
			if (smtp_cmd(&c, &t, "AUTH LOGIN") == 334 && smtp_cmd(&c, &t, "%s", u) == 334)
				code = smtp_cmd(&c, &t, "%s", p);
			mem_free(u);
			mem_free(p);
		}
		if (code != 235) {
			str_copy(err, "The server for sending didn't accept your address and password.", errcap);
			goto out;
		}
	}
	if (smtp_cmd(&c, &t, "MAIL FROM:<%s>", a->email) != 250) {
		fmt(err, errcap, "The server wouldn't send from this address: %s", str_trim(t.data ? t.data : (char *)""));
		goto out;
	}
	for (int i = 0; i < nrcpt; i++) {
		code = smtp_cmd(&c, &t, "RCPT TO:<%s>", rcpt[i]);
		if (code != 250 && code != 251) {
			fmt(err, errcap, "%s was refused: %s", rcpt[i], str_trim(t.data ? t.data : (char *)""));
			goto out;
		}
	}
	if (smtp_cmd(&c, &t, "DATA") != 354) {
		str_copy(err, "The server didn't take the message.", errcap);
		goto out;
	}
	/* CRLF line endings, and a dot at the start of a line doubled */
	StrBuf body = { 0 };
	bool bol = true;
	for (usize i = 0; i < len; i++) {
		char ch = msg[i];
		if (bol && ch == '.')
			sb_addc(&body, '.');
		if (ch == '\n' && (i == 0 || msg[i - 1] != '\r'))
			sb_addc(&body, '\r');
		sb_addc(&body, ch);
		bol = ch == '\n';
	}
	if (!bol)
		sb_adds(&body, "\r\n");
	sb_adds(&body, ".\r\n");
	bool sent = conn_write(&c, body.data, body.len);
	sb_free(&body);
	if (!sent || smtp_reply(&c, &t) != 250) {
		fmt(err, errcap, "The message wasn't accepted: %s", str_trim(t.data ? t.data : (char *)""));
		goto out;
	}
	smtp_cmd(&c, &t, "QUIT");
	ok = true;
out:
	sb_free(&t);
	conn_close(&c);
	return ok;
}

/* ---- Providers ----------------------------------------------------------------------------------- */

void account_guess(Account *a)
{
	const char *at = str_chr(a->email, '@');
	if (!at)
		return;
	const char *d = at + 1;
	static const struct { const char *domain, *imap, *smtp; u16 sport; } KNOWN[] = {
		{ "gmail.com", "imap.gmail.com", "smtp.gmail.com", 465 },
		{ "googlemail.com", "imap.gmail.com", "smtp.gmail.com", 465 },
		{ "outlook.com", "outlook.office365.com", "smtp.office365.com", 587 },
		{ "hotmail.com", "outlook.office365.com", "smtp.office365.com", 587 },
		{ "live.com", "outlook.office365.com", "smtp.office365.com", 587 },
		{ "yahoo.com", "imap.mail.yahoo.com", "smtp.mail.yahoo.com", 465 },
		{ "icloud.com", "imap.mail.me.com", "smtp.mail.me.com", 587 },
		{ "me.com", "imap.mail.me.com", "smtp.mail.me.com", 587 },
		{ "fastmail.com", "imap.fastmail.com", "smtp.fastmail.com", 465 },
		{ "proton.me", "127.0.0.1", "127.0.0.1", 1025 },
		{ "gmx.com", "imap.gmx.com", "mail.gmx.com", 465 },
		{ "yandex.com", "imap.yandex.com", "smtp.yandex.com", 465 },
		{ "zoho.com", "imap.zoho.com", "smtp.zoho.com", 465 },
	};
	for (usize i = 0; i < ARRAY_LEN(KNOWN); i++)
		if (str_eq_nocase(d, KNOWN[i].domain)) {
			str_copy(a->imap_host, KNOWN[i].imap, sizeof a->imap_host);
			str_copy(a->smtp_host, KNOWN[i].smtp, sizeof a->smtp_host);
			a->imap_port = 993;
			a->smtp_port = KNOWN[i].sport;
			return;
		}
	fmt(a->imap_host, sizeof a->imap_host, "imap.%s", d);
	fmt(a->smtp_host, sizeof a->smtp_host, "smtp.%s", d);
	a->imap_port = 993;
	a->smtp_port = 465;
}
