/* D-Bus, from the specification: the wire format, the EXTERNAL login, the
 * bus's own methods, and dispatching calls, replies and signals.
 *
 * A message is a fixed 12-byte start (byte order, type, flags, version,
 * body length, serial), an array of header fields (path, interface,
 * member, destination, signature...), padding to 8 bytes and the body.
 * Every value sits at an offset that is a multiple of its own alignment,
 * counted from the start of the message; the body starts on a multiple of
 * 8, so alignments within it can be counted from its own start. */
#include "zr.h"
#include "zr/dbus.h"

#define MAX_DEPTH 32
#define MAX_MESSAGE (128u << 20)

/* ---- Signatures ------------------------------------------------------------------------- */

static usize align_of(char t)
{
	switch (t) {
	case 'n': case 'q': return 2;
	case 'b': case 'i': case 'u': case 's': case 'o': case 'a': case 'h': return 4;
	case 'x': case 't': case 'd': case '(': case '{': return 8;
	default: return 1; /* y g v */
	}
}

/* Length of the one complete type at the start of s (0 if malformed). */
static usize sig_one(const char *s)
{
	if (!*s)
		return 0;
	if (*s == 'a') {
		usize n = sig_one(s + 1);
		return n ? n + 1 : 0;
	}
	if (*s == '(' || *s == '{') {
		char close = *s == '(' ? ')' : '}';
		usize i = 1;
		while (s[i] && s[i] != close) {
			usize n = sig_one(s + i);
			if (!n)
				return 0;
			i += n;
		}
		return s[i] == close ? i + 1 : 0;
	}
	return str_chr("ybnqiuxtdsoghv", *s) ? 1 : 0;
}

/* ---- Values ------------------------------------------------------------------------------- */

static void val_free(DBusVal *v)
{
	if (!v)
		return;
	for (usize i = 0; i < v->count; i++)
		val_free(v->items[i]);
	mem_free(v->items);
	mem_free(v->s);
	mem_free(v->sig);
	mem_free(v);
}

const DBusVal *dbus_unvariant(const DBusVal *v)
{
	while (v && v->type == 'v')
		v = v->count ? v->items[0] : NULL;
	return v;
}

const char *dbus_str(const DBusVal *v, const char *fallback)
{
	v = dbus_unvariant(v);
	return v && v->s && (v->type == 's' || v->type == 'o' || v->type == 'g') ? v->s : fallback;
}

static bool is_int(char t) { return t && str_chr("ybnqiuxth", t); }

i64 dbus_int(const DBusVal *v, i64 fallback)
{
	v = dbus_unvariant(v);
	if (!v || !is_int(v->type))
		return fallback;
	switch (v->type) {
	case 'n': return (i16)v->n;
	case 'i': return (i32)v->n;
	default: return (i64)v->n;
	}
}

u64 dbus_uint(const DBusVal *v, u64 fallback)
{
	v = dbus_unvariant(v);
	return v && is_int(v->type) ? v->n : fallback;
}

bool dbus_bool(const DBusVal *v, bool fallback)
{
	v = dbus_unvariant(v);
	return v && is_int(v->type) ? v->n != 0 : fallback;
}

f64 dbus_double(const DBusVal *v, f64 fallback)
{
	v = dbus_unvariant(v);
	if (v && v->type == 'd')
		return v->d;
	return v && is_int(v->type) ? (f64)dbus_int(v, 0) : fallback;
}

const DBusVal *dbus_dict_get(const DBusVal *dict, const char *key)
{
	dict = dbus_unvariant(dict);
	if (!dict || dict->type != 'a')
		return NULL;
	for (usize i = 0; i < dict->count; i++) {
		const DBusVal *e = dict->items[i];
		if (e->type == '{' && e->count == 2 && e->items[0]->s && str_eq(e->items[0]->s, key))
			return dbus_unvariant(e->items[1]);
	}
	return NULL;
}

const char **dbus_strv(const DBusVal *v, usize *count)
{
	v = dbus_unvariant(v);
	usize n = v && v->type == 'a' ? v->count : 0;
	const char **out = mem_zalloc(sizeof(char *) * (n + 1));
	usize k = 0;
	for (usize i = 0; i < n; i++)
		if (v->items[i]->s)
			out[k++] = v->items[i]->s;
	if (count)
		*count = k;
	return out;
}

/* ---- Messages ------------------------------------------------------------------------------ */

typedef struct {
	char type;
	usize len_pos, start;
} Open;

struct DBusMsg {
	int refs;
	u8 type, flags;
	bool big;                 /* received in big-endian order */
	u32 serial, reply_serial;
	char *path, *iface, *member, *error, *sender, *dest;
	StrBuf sig;
	u8 *body;
	usize len, cap;
	VEC(int) fds;
	Open open[MAX_DEPTH];
	int depth;
	DBusVal **args;
	usize nargs;
	bool parsed;
	u32 want_fds;             /* received: descriptors the header announced */
};

static DBusMsg *msg_new(int type)
{
	DBusMsg *m = NEW(DBusMsg);
	m->refs = 1;
	m->type = (u8)type;
	return m;
}

DBusMsg *dbus_new_call(const char *dest, const char *path, const char *iface, const char *member)
{
	DBusMsg *m = msg_new(DBUS_CALL);
	m->dest = dest ? str_dup(dest) : NULL;
	m->path = str_dup(path);
	m->iface = iface ? str_dup(iface) : NULL;
	m->member = str_dup(member);
	return m;
}

DBusMsg *dbus_new_signal(const char *path, const char *iface, const char *member)
{
	DBusMsg *m = msg_new(DBUS_SIGNAL);
	m->path = str_dup(path);
	m->iface = str_dup(iface);
	m->member = str_dup(member);
	return m;
}

DBusMsg *dbus_new_reply(const DBusMsg *call)
{
	DBusMsg *m = msg_new(DBUS_RETURN);
	m->reply_serial = call->serial;
	m->dest = call->sender ? str_dup(call->sender) : NULL;
	m->flags = DBUS_NO_REPLY;
	return m;
}

DBusMsg *dbus_new_error(const DBusMsg *call, const char *name, const char *text)
{
	DBusMsg *m = msg_new(DBUS_ERROR);
	m->reply_serial = call->serial;
	m->dest = call->sender ? str_dup(call->sender) : NULL;
	m->error = str_dup(name);
	m->flags = DBUS_NO_REPLY;
	if (text)
		dbus_add(m, "s", text);
	return m;
}

void dbus_set_destination(DBusMsg *m, const char *dest)
{
	mem_free(m->dest);
	m->dest = dest ? str_dup(dest) : NULL;
}

void dbus_set_flags(DBusMsg *m, u8 flags) { m->flags = flags; }

DBusMsg *dbus_msg_ref(DBusMsg *m)
{
	m->refs++;
	return m;
}

void dbus_msg_free(DBusMsg *m)
{
	if (!m || --m->refs > 0)
		return;
	mem_free(m->path);
	mem_free(m->iface);
	mem_free(m->member);
	mem_free(m->error);
	mem_free(m->sender);
	mem_free(m->dest);
	sb_free(&m->sig);
	mem_free(m->body);
	vec_each(&m->fds, fd) if (*fd >= 0) sys_close(*fd);
	vec_free(&m->fds);
	for (usize i = 0; i < m->nargs; i++)
		val_free(m->args[i]);
	mem_free(m->args);
	mem_free(m);
}

int dbus_type(const DBusMsg *m) { return m->type; }
u8 dbus_flags(const DBusMsg *m) { return m->flags; }
u32 dbus_serial(const DBusMsg *m) { return m->serial; }
u32 dbus_reply_serial(const DBusMsg *m) { return m->reply_serial; }
const char *dbus_path(const DBusMsg *m) { return m->path; }
const char *dbus_interface(const DBusMsg *m) { return m->iface; }
const char *dbus_member(const DBusMsg *m) { return m->member; }
const char *dbus_error_name(const DBusMsg *m) { return m->error; }
const char *dbus_sender(const DBusMsg *m) { return m->sender; }
const char *dbus_destination(const DBusMsg *m) { return m->dest; }
const char *dbus_signature(const DBusMsg *m) { return sb_str(&m->sig); }

bool dbus_is_call(const DBusMsg *m, const char *iface, const char *member)
{
	return m->type == DBUS_CALL && m->member && str_eq(m->member, member) &&
	       (!m->iface || !iface || str_eq(m->iface, iface));
}

/* ---- Writing -------------------------------------------------------------------------------- */

static void put(DBusMsg *m, const void *data, usize n)
{
	if (m->len + n > m->cap) {
		m->cap = (m->len + n) * 2 + 64;
		m->body = mem_realloc(m->body, m->cap);
	}
	memcpy(m->body + m->len, data, n);
	m->len += n;
}

static void pad(DBusMsg *m, usize a)
{
	static const u8 zero[8];
	usize p = (a - m->len % a) % a;
	put(m, zero, p);
}

static void put_u32(DBusMsg *m, u32 v)
{
	pad(m, 4);
	put(m, &v, 4);
}

static void put_str(DBusMsg *m, const char *s)
{
	u32 n = (u32)strlen(s);
	put_u32(m, n);
	put(m, s, n + 1);
}

static void put_sig(DBusMsg *m, const char *s)
{
	u8 n = (u8)strlen(s);
	put(m, &n, 1);
	put(m, s, (usize)n + 1);
}

static void note_sig(DBusMsg *m, const char *sig, usize n)
{
	if (m->depth == 0)
		sb_add(&m->sig, sig, n);
}

static void add_one(DBusMsg *m, char t, va_list *ap)
{
	switch (t) {
	case 'y': { u8 v = (u8)va_arg(*ap, int); put(m, &v, 1); break; }
	case 'b': put_u32(m, va_arg(*ap, int) ? 1 : 0); break;
	case 'n': case 'q': { u16 v = (u16)va_arg(*ap, int); pad(m, 2); put(m, &v, 2); break; }
	case 'i': case 'u': put_u32(m, (u32)va_arg(*ap, int)); break;
	case 'x': case 't': { u64 v = va_arg(*ap, u64); pad(m, 8); put(m, &v, 8); break; }
	case 'd': { f64 v = va_arg(*ap, f64); pad(m, 8); put(m, &v, 8); break; }
	case 's': case 'o': { const char *s = va_arg(*ap, const char *); put_str(m, s ? s : ""); break; }
	case 'g': { const char *s = va_arg(*ap, const char *); put_sig(m, s ? s : ""); break; }
	case 'h': {
		int fd = va_arg(*ap, int);
		int copy = fd >= 0 ? (int)sys_fcntl(fd, F_DUPFD_CLOEXEC, 3) : -1;
		vec_push(&m->fds, copy);
		put_u32(m, (u32)(m->fds.len - 1));
		break;
	}
	default: break;
	}
}

void dbus_add(DBusMsg *m, const char *sig, ...)
{
	va_list ap;
	va_start(ap, sig);
	for (const char *p = sig; *p; p++) {
		note_sig(m, p, 1);
		add_one(m, *p, &ap);
	}
	va_end(ap);
}

void dbus_open(DBusMsg *m, const char *sig)
{
	if (m->depth >= MAX_DEPTH)
		return;
	note_sig(m, sig, sig_one(sig));
	Open *o = &m->open[m->depth++];
	o->type = sig[0];
	if (sig[0] == 'a') {
		put_u32(m, 0);
		o->len_pos = m->len - 4;
		pad(m, align_of(sig[1]));
		o->start = m->len;
	} else {
		pad(m, 8);
	}
}

void dbus_open_variant(DBusMsg *m, const char *contained)
{
	if (m->depth >= MAX_DEPTH)
		return;
	note_sig(m, "v", 1);
	put_sig(m, contained);
	m->open[m->depth++].type = 'v';
}

void dbus_close(DBusMsg *m)
{
	if (m->depth <= 0)
		return;
	Open *o = &m->open[--m->depth];
	if (o->type == 'a') {
		u32 n = (u32)(m->len - o->start);
		memcpy(m->body + o->len_pos, &n, 4);
	}
}

void dbus_dict_add(DBusMsg *m, const char *key, const char *sig, ...)
{
	dbus_open(m, "{sv}");
	dbus_add(m, "s", key);
	dbus_open_variant(m, sig);
	va_list ap;
	va_start(ap, sig);
	for (const char *p = sig; *p; p++)
		add_one(m, *p, &ap);
	va_end(ap);
	dbus_close(m);
	dbus_close(m);
}

void dbus_add_val(DBusMsg *m, const DBusVal *v)
{
	if (!v)
		return;
	switch (v->type) {
	case 'a': case '(': case '{':
		dbus_open(m, v->sig);
		for (usize i = 0; i < v->count; i++)
			dbus_add_val(m, v->items[i]);
		dbus_close(m);
		break;
	case 'v':
		dbus_open_variant(m, v->count ? v->items[0]->sig : "s");
		if (v->count)
			dbus_add_val(m, v->items[0]);
		else
			put_str(m, "");
		dbus_close(m);
		break;
	case 'd': dbus_add(m, "d", v->d); break;
	case 's': case 'o': case 'g': {
		char t[2] = { v->type, 0 };
		dbus_add(m, t, v->s);
		break;
	}
	case 'x': case 't': {
		char t[2] = { v->type, 0 };
		dbus_add(m, t, v->n);
		break;
	}
	case 'h': dbus_add(m, "h", -1); break; /* descriptors aren't copied this way */
	default: {
		char t[2] = { v->type, 0 };
		dbus_add(m, t, (int)v->n);
		break;
	}
	}
}

/* ---- Reading --------------------------------------------------------------------------------- */

typedef struct {
	const u8 *data;
	usize len, pos;
	bool big, bad;
	int depth;
} Reader;

static bool r_align(Reader *r, usize a)
{
	usize p = (r->pos + a - 1) / a * a;
	if (p > r->len) {
		r->bad = true;
		return false;
	}
	r->pos = p;
	return true;
}

static u64 r_num(Reader *r, usize n)
{
	if (!r_align(r, n) || r->pos + n > r->len) {
		r->bad = true;
		return 0;
	}
	u64 v = 0;
	for (usize i = 0; i < n; i++) {
		u8 b = r->data[r->pos + (r->big ? i : n - 1 - i)];
		v = (v << 8) | b;
	}
	r->pos += n;
	return v;
}

static char *r_str(Reader *r, bool sig)
{
	u64 n = sig ? r_num(r, 1) : r_num(r, 4);
	if (r->bad || r->pos + n + 1 > r->len || r->data[r->pos + n] != 0) {
		r->bad = true;
		return NULL;
	}
	char *s = str_ndup((const char *)r->data + r->pos, (usize)n);
	r->pos += (usize)n + 1;
	return s;
}

static DBusVal *r_value(Reader *r, const char *sig)
{
	usize n = sig_one(sig);
	if (!n || r->bad || ++r->depth > MAX_DEPTH) {
		r->bad = true;
		return NULL;
	}
	DBusVal *v = NEW(DBusVal);
	v->type = sig[0];
	v->sig = str_ndup(sig, n);
	switch (sig[0]) {
	case 'y': v->n = r_num(r, 1); break;
	case 'b': case 'i': case 'u': case 'h': v->n = r_num(r, 4); break;
	case 'n': case 'q': v->n = r_num(r, 2); break;
	case 'x': case 't': v->n = r_num(r, 8); break;
	case 'd': {
		u64 bits = r_num(r, 8);
		memcpy(&v->d, &bits, 8);
		break;
	}
	case 's': case 'o': v->s = r_str(r, false); break;
	case 'g': v->s = r_str(r, true); break;
	case 'v': {
		char *inner = r_str(r, true);
		if (inner && sig_one(inner) == strlen(inner)) {
			v->items = NEW_ARRAY(DBusVal *, 1);
			v->items[0] = r_value(r, inner);
			v->count = v->items[0] ? 1 : 0;
		} else {
			r->bad = true;
		}
		mem_free(inner);
		break;
	}
	case 'a': {
		u64 bytes = r_num(r, 4);
		r_align(r, align_of(sig[1]));
		usize end = r->pos + (usize)bytes;
		if (bytes > (64u << 20) || end > r->len) {
			r->bad = true;
			break;
		}
		usize cap = 0;
		while (r->pos < end && !r->bad) {
			if (v->count == cap) {
				cap = cap ? cap * 2 : 8;
				v->items = mem_realloc(v->items, sizeof(DBusVal *) * cap);
			}
			DBusVal *e = r_value(r, sig + 1);
			if (!e)
				break;
			v->items[v->count++] = e;
		}
		if (r->pos != end)
			r->bad = true;
		break;
	}
	case '(': case '{': {
		r_align(r, 8);
		usize i = 1, cap = 0;
		while (i < n - 1 && !r->bad) {
			if (v->count == cap) {
				cap = cap ? cap * 2 : 4;
				v->items = mem_realloc(v->items, sizeof(DBusVal *) * cap);
			}
			DBusVal *e = r_value(r, sig + i);
			if (!e)
				break;
			v->items[v->count++] = e;
			i += sig_one(sig + i);
		}
		break;
	}
	default: r->bad = true; break;
	}
	r->depth--;
	if (r->bad) {
		val_free(v);
		return NULL;
	}
	return v;
}

static void parse_args(DBusMsg *m)
{
	if (m->parsed)
		return;
	m->parsed = true;
	Reader r = { .data = m->body, .len = m->len, .big = m->big };
	const char *s = sb_str(&m->sig);
	usize cap = 0;
	while (*s && !r.bad) {
		DBusVal *v = r_value(&r, s);
		if (!v)
			break;
		if (m->nargs == cap) {
			cap = cap ? cap * 2 : 4;
			m->args = mem_realloc(m->args, sizeof(DBusVal *) * cap);
		}
		m->args[m->nargs++] = v;
		s += sig_one(s);
	}
}

const DBusVal *dbus_arg(DBusMsg *m, usize i)
{
	parse_args(m);
	return i < m->nargs ? m->args[i] : NULL;
}

usize dbus_nargs(DBusMsg *m)
{
	parse_args(m);
	return m->nargs;
}

int dbus_take_fd(DBusMsg *m, const DBusVal *h)
{
	h = dbus_unvariant(h);
	if (!h || h->type != 'h' || h->n >= m->fds.len)
		return -1;
	int fd = m->fds.data[h->n];
	m->fds.data[h->n] = -1;
	return fd;
}

/* ---- The wire: whole messages ------------------------------------------------------------------- */

static void field_str(DBusMsg *h, u8 code, char type, const char *s)
{
	if (!s)
		return;
	pad(h, 8);
	put(h, &code, 1);
	char sig[2] = { type, 0 };
	put_sig(h, sig);
	if (type == 'g')
		put_sig(h, s);
	else
		put_str(h, s);
}

static void field_u32(DBusMsg *h, u8 code, u32 v)
{
	pad(h, 8);
	put(h, &code, 1);
	put_sig(h, "u");
	put_u32(h, v);
}

/* The header and body of m, ready to send, in one buffer. */
static u8 *serialize(DBusMsg *m, usize *out_len)
{
	DBusMsg h = { 0 };
	u8 start[4] = { 'l', m->type, m->flags, 1 };
	put(&h, start, 4);
	u32 blen = (u32)m->len;
	put(&h, &blen, 4);
	put(&h, &m->serial, 4);
	put_u32(&h, 0); /* fields array length, filled in below */
	usize fstart = h.len;
	field_str(&h, 1, 'o', m->path);
	field_str(&h, 2, 's', m->iface);
	field_str(&h, 3, 's', m->member);
	field_str(&h, 4, 's', m->error);
	if (m->reply_serial)
		field_u32(&h, 5, m->reply_serial);
	field_str(&h, 6, 's', m->dest);
	if (m->sig.len)
		field_str(&h, 8, 'g', m->sig.data);
	if (m->fds.len)
		field_u32(&h, 9, (u32)m->fds.len);
	u32 flen = (u32)(h.len - fstart);
	memcpy(h.body + 12, &flen, 4);
	pad(&h, 8);
	put(&h, m->body, m->len);
	*out_len = h.len;
	return h.body;
}

/* Parses one whole message from data (its total length known). */
static DBusMsg *deserialize(const u8 *data, usize len)
{
	Reader r = { .data = data, .len = len, .big = data[0] == 'B' };
	DBusMsg *m = msg_new(data[1]);
	m->flags = data[2];
	m->big = r.big;
	r.pos = 4;
	u32 blen = (u32)r_num(&r, 4);
	m->serial = (u32)r_num(&r, 4);
	DBusVal *fields = r_value(&r, "a(yv)");
	if (!fields) {
		dbus_msg_free(m);
		return NULL;
	}
	for (usize i = 0; i < fields->count; i++) {
		DBusVal *f = fields->items[i];
		if (f->count != 2)
			continue;
		const DBusVal *v = dbus_unvariant(f->items[1]);
		switch (f->items[0]->n) {
		case 1: m->path = str_dup(dbus_str(v, "")); break;
		case 2: m->iface = str_dup(dbus_str(v, "")); break;
		case 3: m->member = str_dup(dbus_str(v, "")); break;
		case 4: m->error = str_dup(dbus_str(v, "")); break;
		case 5: m->reply_serial = (u32)dbus_uint(v, 0); break;
		case 6: m->dest = str_dup(dbus_str(v, "")); break;
		case 7: m->sender = str_dup(dbus_str(v, "")); break;
		case 8: sb_adds(&m->sig, dbus_str(v, "")); break;
		case 9: m->want_fds = (u32)dbus_uint(v, 0); break;
		default: break;
		}
	}
	val_free(fields);
	r_align(&r, 8);
	if (r.bad || r.pos + blen != len) {
		dbus_msg_free(m);
		return NULL;
	}
	m->body = mem_dup(data + r.pos, blen);
	m->len = m->cap = blen;
	return m;
}

u8 *dbus_msg_encode(DBusMsg *m, u32 serial, usize *len)
{
	while (m->depth > 0)
		dbus_close(m);
	m->serial = serial;
	return serialize(m, len);
}

DBusMsg *dbus_msg_decode(const u8 *data, usize len)
{
	return len >= 16 ? deserialize(data, len) : NULL;
}

/* Total length of the message at the start of data, 0 if not known yet. */
static usize message_length(const u8 *data, usize have)
{
	if (have < 16)
		return 0;
	bool big = data[0] == 'B';
	u32 blen, flen;
	if (big) {
		blen = (u32)data[4] << 24 | (u32)data[5] << 16 | (u32)data[6] << 8 | data[7];
		flen = (u32)data[12] << 24 | (u32)data[13] << 16 | (u32)data[14] << 8 | data[15];
	} else {
		memcpy(&blen, data + 4, 4);
		memcpy(&flen, data + 12, 4);
	}
	usize head = (16 + (usize)flen + 7) / 8 * 8;
	return head + blen;
}

/* ---- Connections ---------------------------------------------------------------------------------- */

typedef struct {
	char *path, *iface, *xml;
	bool subtree;
	DBusMethodFn fn;
	void *data;
} Export;

typedef struct {
	char *sender, *path, *iface, *member;
	DBusSignalFn fn;
	void *data;
} Sub;

typedef struct {
	u32 serial;
	DBusReplyFn fn;
	void *data;
} Pending;

struct DBus {
	int fd;
	bool dead, fds_ok;
	u32 serial;
	char *name;
	u8 *in;
	usize in_len, in_cap;
	VEC(int) in_fds;
	VEC(DBusMsg *) queue;
	VEC(Export) exports;
	VEC(Sub) subs;
	VEC(Pending) pending;
	Loop *loop;
	void (*on_disc)(DBus *, void *);
	void *disc_data;
};

static void lost(DBus *bus)
{
	if (bus->dead)
		return;
	bus->dead = true;
	if (bus->loop)
		loop_del_fd(bus->loop, bus->fd);
	/* calls still waiting get their failure */
	while (bus->pending.len) {
		Pending p = bus->pending.data[0];
		vec_remove(&bus->pending, 0);
		if (p.fn)
			p.fn(bus, NULL, p.data);
	}
	if (bus->on_disc)
		bus->on_disc(bus, bus->disc_data);
}

/* Writes everything, waiting while the socket is full. */
static bool send_all(DBus *bus, const u8 *data, usize len, const int *fds, int nfds)
{
	while (len) {
		long n = unix_send(bus->fd, data, len, fds, nfds);
		if (n == -EAGAIN) {
			struct pollfd p = { bus->fd, POLLOUT, 0 };
			sys_poll(&p, 1, 1000);
			continue;
		}
		if (n <= 0)
			return false;
		data += n;
		len -= (usize)n;
		nfds = 0; /* descriptors go with the first bytes */
	}
	return true;
}

u32 dbus_send(DBus *bus, DBusMsg *m)
{
	if (!bus || bus->dead) {
		dbus_msg_free(m);
		return 0;
	}
	while (m->depth > 0)
		dbus_close(m);
	m->serial = ++bus->serial;
	if (!m->serial)
		m->serial = ++bus->serial;
	usize len;
	u8 *wire = serialize(m, &len);
	int nfds = bus->fds_ok ? (int)m->fds.len : 0;
	bool ok = send_all(bus, wire, len, m->fds.data, nfds);
	mem_free(wire);
	u32 serial = m->serial;
	dbus_msg_free(m);
	if (!ok) {
		lost(bus);
		return 0;
	}
	return serial;
}

/* Reads what the socket has (without waiting). False when the bus is gone. */
static bool read_some(DBus *bus)
{
	for (;;) {
		if (bus->in_cap - bus->in_len < 4096) {
			bus->in_cap = bus->in_cap * 2 + 8192;
			bus->in = mem_realloc(bus->in, bus->in_cap);
		}
		int fds[8], nfds = 0;
		long n = unix_recv(bus->fd, bus->in + bus->in_len, bus->in_cap - bus->in_len, fds, 8, &nfds);
		for (int i = 0; i < nfds; i++)
			vec_push(&bus->in_fds, fds[i]);
		if (n == -EAGAIN)
			return true;
		if (n <= 0)
			return false;
		bus->in_len += (usize)n;
	}
}

/* The next whole message in the buffer, or NULL. */
static DBusMsg *take_message(DBus *bus)
{
	usize total = message_length(bus->in, bus->in_len);
	if (!total || bus->in_len < total)
		return NULL;
	if (total > MAX_MESSAGE) {
		lost(bus);
		return NULL;
	}
	DBusMsg *m = deserialize(bus->in, total);
	memmove(bus->in, bus->in + total, bus->in_len - total);
	bus->in_len -= total;
	if (!m)
		return NULL;
	/* its descriptors: as many as the header said, from those received */
	for (u32 i = 0; i < m->want_fds && bus->in_fds.len; i++) {
		vec_push(&m->fds, bus->in_fds.data[0]);
		vec_remove(&bus->in_fds, 0);
	}
	return m;
}

static bool path_matches(const Export *e, const char *path)
{
	if (!path)
		return false;
	if (!e->subtree)
		return str_eq(e->path, path);
	usize n = strlen(e->path);
	return str_eq(e->path, path) || (str_starts(path, e->path) && (path[n] == '/' || (n == 1 && e->path[0] == '/')));
}

static void reply_introspect(DBus *bus, DBusMsg *call)
{
	StrBuf x = { 0 };
	sb_adds(&x, "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
		    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n"
		    "<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\">"
		    "<arg type=\"s\" direction=\"out\"/></method></interface>\n"
		    "<interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/><method name=\"GetMachineId\">"
		    "<arg type=\"s\" direction=\"out\"/></method></interface>\n");
	const char *path = call->path;
	usize plen = strlen(path);
	VEC(char *) kids = { 0 };
	vec_each(&bus->exports, e) {
		if (path_matches(e, path) && e->xml) {
			sb_adds(&x, e->xml);
			sb_addc(&x, '\n');
		}
		/* child nodes: the next path element of exports below this one */
		if (!e->subtree && str_starts(e->path, path) && strlen(e->path) > plen) {
			const char *rest = e->path + plen;
			if (plen > 1) {
				if (*rest != '/')
					continue;
				rest++;
			} else if (*rest == '/') {
				rest++;
			}
			const char *slash = str_chr(rest, '/');
			char *kid = slash ? str_ndup(rest, (usize)(slash - rest)) : str_dup(rest);
			bool have = false;
			vec_each(&kids, k) if (str_eq(*k, kid)) have = true;
			if (have || !*kid)
				mem_free(kid);
			else
				vec_push(&kids, kid);
		}
	}
	vec_each(&kids, k) {
		sb_fmt(&x, "<node name=\"%s\"/>\n", *k);
		mem_free(*k);
	}
	vec_free(&kids);
	sb_adds(&x, "</node>\n");
	DBusMsg *r = dbus_new_reply(call);
	dbus_add(r, "s", x.data);
	dbus_send(bus, r);
	sb_free(&x);
}

static void dispatch_call(DBus *bus, DBusMsg *m)
{
	const char *iface = m->iface;
	if (iface && str_eq(iface, "org.freedesktop.DBus.Peer")) {
		DBusMsg *r = dbus_new_reply(m);
		if (str_eq(m->member, "GetMachineId")) {
			char *id = file_read_line("/etc/machine-id");
			dbus_add(r, "s", id ? id : "00000000000000000000000000000000");
			mem_free(id);
		}
		dbus_send(bus, r);
		return;
	}
	if (iface && str_eq(iface, "org.freedesktop.DBus.Introspectable") && str_eq(m->member, "Introspect")) {
		reply_introspect(bus, m);
		return;
	}
	/* Properties calls go to the interface they're about */
	const char *target = iface;
	if (iface && str_eq(iface, "org.freedesktop.DBus.Properties"))
		target = dbus_str(dbus_arg(m, 0), NULL);
	bool known_path = false;
	for (usize i = 0; i < bus->exports.len; i++) {
		Export e = bus->exports.data[i];
		if (!path_matches(&e, m->path))
			continue;
		known_path = true;
		if (target && !str_eq(e.iface, target))
			continue;
		if (e.fn(bus, m, e.data))
			return;
	}
	if (m->flags & DBUS_NO_REPLY)
		return;
	if (!known_path)
		dbus_reply_error(bus, m, "org.freedesktop.DBus.Error.UnknownObject", "No such object");
	else if (iface && str_eq(iface, "org.freedesktop.DBus.Properties"))
		dbus_reply_error(bus, m, "org.freedesktop.DBus.Error.UnknownProperty", "No such property");
	else
		dbus_reply_error(bus, m, "org.freedesktop.DBus.Error.UnknownMethod", "No such method");
}

static bool opt_eq(const char *want, const char *have)
{
	return !want || (have && str_eq(want, have));
}

static void dispatch(DBus *bus, DBusMsg *m)
{
	switch (m->type) {
	case DBUS_RETURN:
	case DBUS_ERROR:
		for (usize i = 0; i < bus->pending.len; i++)
			if (bus->pending.data[i].serial == m->reply_serial) {
				Pending p = bus->pending.data[i];
				vec_remove(&bus->pending, i);
				if (p.fn)
					p.fn(bus, m, p.data);
				break;
			}
		break;
	case DBUS_SIGNAL:
		for (usize i = 0; i < bus->subs.len; i++) {
			Sub s = bus->subs.data[i];
			if (opt_eq(s.sender, m->sender) && opt_eq(s.path, m->path) && opt_eq(s.iface, m->iface) &&
			    opt_eq(s.member, m->member))
				s.fn(bus, m, s.data);
		}
		break;
	case DBUS_CALL:
		dispatch_call(bus, m);
		break;
	default:
		break;
	}
}

bool dbus_dispatch(DBus *bus)
{
	if (!bus || bus->dead)
		return false;
	if (!read_some(bus)) {
		/* what arrived before the end still counts */
		DBusMsg *m;
		while ((m = take_message(bus)))
			vec_push(&bus->queue, m);
		while (bus->queue.len) {
			m = bus->queue.data[0];
			vec_remove(&bus->queue, 0);
			dispatch(bus, m);
			dbus_msg_free(m);
		}
		lost(bus);
		return false;
	}
	DBusMsg *m;
	while ((m = take_message(bus)))
		vec_push(&bus->queue, m);
	while (bus->queue.len && !bus->dead) {
		m = bus->queue.data[0];
		vec_remove(&bus->queue, 0);
		dispatch(bus, m);
		dbus_msg_free(m);
	}
	return !bus->dead;
}

DBusMsg *dbus_call(DBus *bus, DBusMsg *call, int timeout_ms)
{
	u32 serial = dbus_send(bus, call);
	if (!serial)
		return NULL;
	u64 until = time_mono_ms() + (u64)(timeout_ms < 0 ? 25000 : timeout_ms);
	for (;;) {
		DBusMsg *m;
		while ((m = take_message(bus))) {
			if ((m->type == DBUS_RETURN || m->type == DBUS_ERROR) && m->reply_serial == serial)
				return m;
			vec_push(&bus->queue, m);
		}
		u64 now = time_mono_ms();
		if (now >= until || bus->dead)
			return NULL;
		struct pollfd p = { bus->fd, POLLIN, 0 };
		long r = sys_poll(&p, 1, (int)(until - now));
		if (r < 0 && r != -EINTR)
			return NULL;
		if (r > 0 && !read_some(bus)) {
			while ((m = take_message(bus))) {
				if ((m->type == DBUS_RETURN || m->type == DBUS_ERROR) && m->reply_serial == serial)
					return m;
				vec_push(&bus->queue, m);
			}
			lost(bus);
			return NULL;
		}
	}
}

void dbus_call_async(DBus *bus, DBusMsg *call, DBusReplyFn fn, void *data)
{
	u32 serial = dbus_send(bus, call);
	if (!serial) {
		if (fn)
			fn(bus, NULL, data);
		return;
	}
	vec_push(&bus->pending, (Pending){ serial, fn, data });
}

static void on_readable(Loop *loop, int fd, u32 events, void *data)
{
	(void)loop;
	(void)fd;
	(void)events;
	dbus_dispatch(data);
}

static void on_queued(Loop *loop, void *data)
{
	(void)loop;
	DBus *bus = data;
	if (bus->queue.len)
		dbus_dispatch(bus);
}

void dbus_attach(DBus *bus, Loop *loop)
{
	bus->loop = loop;
	loop_add_fd(loop, bus->fd, EPOLLIN, on_readable, bus);
	/* messages that arrived during synchronous calls wait in the queue
	 * without the socket becoming readable again: check now and then */
	loop_timer(loop, 50, 50, on_queued, bus);
}

/* ---- Connecting ------------------------------------------------------------------------------------- */

/* Reads one line of the login exchange (blocking). */
static bool auth_line(int fd, char *buf, usize cap)
{
	usize n = 0;
	while (n + 1 < cap) {
		char c;
		long r = sys_read(fd, &c, 1);
		if (r == -EINTR)
			continue;
		if (r <= 0)
			return false;
		if (c == '\n')
			break;
		if (c != '\r')
			buf[n++] = c;
	}
	buf[n] = 0;
	return true;
}

static int connect_unix(const char *path, bool abstract)
{
	if (!abstract)
		return unix_connect(path, SOCK_STREAM);
	struct sockaddr_un a;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	usize n = strlen(path);
	if (n + 1 > sizeof a.sun_path)
		return -ENAMETOOLONG;
	memcpy(a.sun_path + 1, path, n);
	int fd = (int)sys_socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return fd;
	long r = sys_connect(fd, &a, (u32)(offsetof(struct sockaddr_un, sun_path) + 1 + n));
	if (r < 0) {
		sys_close(fd);
		return (int)r;
	}
	return fd;
}

/* "unix:path=/run/user/1000/bus,guid=..." (several addresses separated by ;) */
static int connect_address(const char *address)
{
	usize na = 0;
	char **addrs = str_split(address, ';', &na);
	int fd = -ENOENT;
	for (usize i = 0; i < na && fd < 0; i++) {
		if (!str_starts(addrs[i], "unix:"))
			continue;
		usize nk = 0;
		char **kv = str_split(addrs[i] + 5, ',', &nk);
		for (usize k = 0; k < nk && fd < 0; k++) {
			bool abstract = str_starts(kv[k], "abstract=");
			if (!abstract && !str_starts(kv[k], "path="))
				continue;
			/* values may escape bytes as %xx */
			const char *v = str_chr(kv[k], '=') + 1;
			StrBuf p = { 0 };
			for (; *v; v++) {
				if (*v == '%' && is_xdigit(v[1]) && is_xdigit(v[2])) {
					int c = 0;
					for (int j = 1; j <= 2; j++)
						c = c * 16 + (is_digit(v[j]) ? v[j] - '0' : to_lower(v[j]) - 'a' + 10);
					sb_addc(&p, (char)c);
					v += 2;
				} else {
					sb_addc(&p, *v);
				}
			}
			fd = connect_unix(sb_str(&p), abstract);
			sb_free(&p);
		}
		str_split_free(kv, nk);
	}
	str_split_free(addrs, na);
	return fd;
}

DBus *dbus_open_address(const char *address)
{
	if (!address)
		return NULL;
	int fd = connect_address(address);
	if (fd < 0)
		return NULL;
	/* the login: who we are (EXTERNAL: the kernel vouches for our user id) */
	char uid[16], hex[40], line[256];
	fmt(uid, sizeof uid, "%u", (u32)sys_getuid());
	usize h = 0;
	for (const char *c = uid; *c && h + 2 < sizeof hex; c++)
		h += (usize)fmt(hex + h, sizeof hex - h, "%02x", (u8)*c);
	char hello[96];
	int n = fmt(hello, sizeof hello, "%cAUTH EXTERNAL %s\r\n", 0, hex);
	bool ok = write_all(fd, hello, (usize)n) && auth_line(fd, line, sizeof line) && str_starts(line, "OK ");
	bool fds_ok = false;
	if (ok) {
		ok = write_all(fd, "NEGOTIATE_UNIX_FD\r\n", 19) && auth_line(fd, line, sizeof line);
		fds_ok = ok && str_starts(line, "AGREE_UNIX_FD");
		ok = ok && write_all(fd, "BEGIN\r\n", 7);
	}
	if (!ok) {
		sys_close(fd);
		return NULL;
	}
	DBus *bus = NEW(DBus);
	bus->fd = fd;
	bus->fds_ok = fds_ok;
	fd_set_nonblock(fd, true);
	DBusMsg *r = dbus_call(bus, dbus_new_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello"),
			       10000);
	const char *name = r && r->type == DBUS_RETURN ? dbus_str(dbus_arg(r, 0), NULL) : NULL;
	if (!name) {
		dbus_msg_free(r);
		dbus_close_bus(bus);
		return NULL;
	}
	bus->name = str_dup(name);
	dbus_msg_free(r);
	return bus;
}

DBus *dbus_open_session(void)
{
	return dbus_open_address(env_get("DBUS_SESSION_BUS_ADDRESS"));
}

void dbus_close_bus(DBus *bus)
{
	if (!bus)
		return;
	if (bus->loop && !bus->dead)
		loop_del_fd(bus->loop, bus->fd);
	sys_close(bus->fd);
	mem_free(bus->name);
	mem_free(bus->in);
	vec_each(&bus->in_fds, fd) sys_close(*fd);
	vec_free(&bus->in_fds);
	vec_each(&bus->queue, m) dbus_msg_free(*m);
	vec_free(&bus->queue);
	vec_each(&bus->exports, e) {
		mem_free(e->path);
		mem_free(e->iface);
		mem_free(e->xml);
	}
	vec_free(&bus->exports);
	vec_each(&bus->subs, s) {
		mem_free(s->sender);
		mem_free(s->path);
		mem_free(s->iface);
		mem_free(s->member);
	}
	vec_free(&bus->subs);
	vec_free(&bus->pending);
	mem_free(bus);
}

const char *dbus_unique_name(const DBus *bus) { return bus->name; }
int dbus_fd(const DBus *bus) { return bus->fd; }
bool dbus_connected(const DBus *bus) { return bus && !bus->dead; }

void dbus_on_disconnect(DBus *bus, void (*fn)(DBus *bus, void *data), void *data)
{
	bus->on_disc = fn;
	bus->disc_data = data;
}

/* ---- The bus's own methods ------------------------------------------------------------------------- */

static DBusMsg *bus_call(const char *member)
{
	return dbus_new_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", member);
}

int dbus_request_name(DBus *bus, const char *name, u32 flags)
{
	DBusMsg *c = bus_call("RequestName");
	dbus_add(c, "su", name, flags);
	DBusMsg *r = dbus_call(bus, c, 10000);
	int code = r && r->type == DBUS_RETURN ? (int)dbus_uint(dbus_arg(r, 0), 0) : -1;
	dbus_msg_free(r);
	return code;
}

bool dbus_release_name(DBus *bus, const char *name)
{
	DBusMsg *c = bus_call("ReleaseName");
	dbus_add(c, "s", name);
	DBusMsg *r = dbus_call(bus, c, 10000);
	bool ok = r && r->type == DBUS_RETURN;
	dbus_msg_free(r);
	return ok;
}

bool dbus_add_match(DBus *bus, const char *rule)
{
	DBusMsg *c = bus_call("AddMatch");
	dbus_add(c, "s", rule);
	DBusMsg *r = dbus_call(bus, c, 10000);
	bool ok = r && r->type == DBUS_RETURN;
	dbus_msg_free(r);
	return ok;
}

bool dbus_name_has_owner(DBus *bus, const char *name)
{
	DBusMsg *c = bus_call("NameHasOwner");
	dbus_add(c, "s", name);
	DBusMsg *r = dbus_call(bus, c, 10000);
	bool has = r && r->type == DBUS_RETURN && dbus_bool(dbus_arg(r, 0), false);
	dbus_msg_free(r);
	return has;
}

u32 dbus_peer_pid(DBus *bus, const char *name)
{
	DBusMsg *c = bus_call("GetConnectionUnixProcessID");
	dbus_add(c, "s", name);
	DBusMsg *r = dbus_call(bus, c, 10000);
	u32 pid = r && r->type == DBUS_RETURN ? (u32)dbus_uint(dbus_arg(r, 0), 0) : 0;
	dbus_msg_free(r);
	return pid;
}

/* ---- Serving -------------------------------------------------------------------------------------- */

void dbus_export(DBus *bus, const char *path, const char *iface, DBusMethodFn fn, void *data, const char *xml)
{
	Export e = { 0 };
	usize n = strlen(path);
	e.subtree = n >= 2 && str_ends(path, "/*");
	e.path = e.subtree ? str_ndup(path, n > 2 ? n - 2 : 1) : str_dup(path);
	e.iface = str_dup(iface);
	e.xml = xml ? str_dup(xml) : NULL;
	e.fn = fn;
	e.data = data;
	vec_push(&bus->exports, e);
}

void dbus_unexport(DBus *bus, const char *path, const char *iface)
{
	for (usize i = 0; i < bus->exports.len; i++) {
		Export *e = &bus->exports.data[i];
		if (str_eq(e->path, path) && (!iface || str_eq(e->iface, iface))) {
			mem_free(e->path);
			mem_free(e->iface);
			mem_free(e->xml);
			vec_remove(&bus->exports, i);
			i--;
		}
	}
}

void dbus_subscribe(DBus *bus, const char *sender, const char *path, const char *iface, const char *member,
		    DBusSignalFn fn, void *data)
{
	Sub s = { sender ? str_dup(sender) : NULL, path ? str_dup(path) : NULL, iface ? str_dup(iface) : NULL,
		  member ? str_dup(member) : NULL, fn, data };
	vec_push(&bus->subs, s);
	StrBuf rule = { 0 };
	sb_adds(&rule, "type='signal'");
	if (sender)
		sb_fmt(&rule, ",sender='%s'", sender);
	if (path)
		sb_fmt(&rule, ",path='%s'", path);
	if (iface)
		sb_fmt(&rule, ",interface='%s'", iface);
	if (member)
		sb_fmt(&rule, ",member='%s'", member);
	dbus_add_match(bus, rule.data);
	sb_free(&rule);
}

void dbus_reply_empty(DBus *bus, const DBusMsg *call)
{
	if (!(call->flags & DBUS_NO_REPLY))
		dbus_send(bus, dbus_new_reply(call));
}

void dbus_reply_error(DBus *bus, const DBusMsg *call, const char *name, const char *text)
{
	if (!(call->flags & DBUS_NO_REPLY))
		dbus_send(bus, dbus_new_error(call, name, text));
}

void dbus_reply_property(DBus *bus, const DBusMsg *call, const char *sig, ...)
{
	DBusMsg *r = dbus_new_reply(call);
	dbus_open_variant(r, sig);
	va_list ap;
	va_start(ap, sig);
	for (const char *p = sig; *p; p++)
		add_one(r, *p, &ap);
	va_end(ap);
	dbus_close(r);
	dbus_send(bus, r);
}
