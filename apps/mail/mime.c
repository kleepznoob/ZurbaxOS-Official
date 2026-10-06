/* MIME: message structure, transfer encodings, charsets and header words. */
#include "mail.h"

/* ---- Small helpers ----------------------------------------------------------------------------- */

static int hexv(int c)
{
	return is_digit(c) ? c - '0' : (c | 32) - 'a' + 10;
}

static const char *find_n(const char *s, usize n, const char *needle)
{
	usize k = strlen(needle);
	for (usize i = 0; i + k <= n; i++)
		if (memcmp(s + i, needle, k) == 0)
			return s + i;
	return NULL;
}

/* End of the header block: the first empty line. */
static usize header_end(const char *msg, usize len, usize *body_at)
{
	for (usize i = 0; i < len; i++) {
		if (msg[i] != '\n')
			continue;
		if (i + 1 < len && msg[i + 1] == '\n') {
			*body_at = i + 2;
			return i + 1;
		}
		if (i + 2 < len && msg[i + 1] == '\r' && msg[i + 2] == '\n') {
			*body_at = i + 3;
			return i + 1;
		}
	}
	*body_at = len;
	return len;
}

char *mime_header(const char *h, usize len, const char *name)
{
	usize nl = strlen(name);
	for (usize i = 0; i < len;) {
		usize e = i;
		while (e < len && h[e] != '\n')
			e++;
		if (e - i > nl && h[i + nl] == ':' && str_eq_nocase_n(h + i, name, nl)) {
			/* the value, with continuation lines joined */
			StrBuf sb = { 0 };
			usize p = i + nl + 1;
			for (;;) {
				usize q = p;
				while (q < len && h[q] != '\n')
					q++;
				usize end = q;
				if (end > p && h[end - 1] == '\r')
					end--;
				if (sb.len)
					sb_addc(&sb, ' ');
				sb_add(&sb, h + p, end - p);
				if (q + 1 < len && (h[q + 1] == ' ' || h[q + 1] == '\t'))
					p = q + 1;
				else
					break;
			}
			char *v = sb_take(&sb);
			char *t = str_trim(v);
			char *r = str_dup(t);
			mem_free(v);
			return r;
		}
		i = e + 1;
	}
	return NULL;
}

/* A parameter of a structured header: boundary="x"; charset=utf-8 */
static bool param(const char *v, const char *name, char *out, usize cap)
{
	usize nl = strlen(name);
	for (const char *p = v; (p = str_chr(p, ';')); p++) {
		const char *s = p + 1;
		while (is_space(*s))
			s++;
		if (!str_eq_nocase_n(s, name, nl))
			continue;
		const char *e = s + nl;
		if (*e == '*')
			e++;   /* RFC 2231 name*=utf-8''... : take the value as-is below */
		while (is_space(*e))
			e++;
		if (*e != '=')
			continue;
		e++;
		while (is_space(*e))
			e++;
		usize n = 0;
		if (*e == '"') {
			e++;
			while (*e && *e != '"' && n < cap - 1) {
				if (*e == '\\' && e[1])
					e++;
				out[n++] = *e++;
			}
		} else {
			while (*e && *e != ';' && !is_space(*e) && n < cap - 1)
				out[n++] = *e++;
		}
		out[n] = 0;
		/* utf-8''Name%20here */
		char *q = (char *)str_find(out, "''");
		if (q) {
			char tmp[512];
			usize w = 0;
			for (const char *r = q + 2; *r && w < sizeof tmp - 1; r++) {
				if (*r == '%' && is_xdigit(r[1]) && is_xdigit(r[2])) {
					tmp[w++] = (char)(hexv(r[1]) * 16 + hexv(r[2]));
					r += 2;
				} else {
					tmp[w++] = *r;
				}
			}
			tmp[w] = 0;
			str_copy(out, tmp, cap);
		}
		return true;
	}
	return false;
}

/* ---- Charsets and encodings --------------------------------------------------------------------- */

char *mime_to_utf8(const char *s, usize n, const char *charset, usize *out_len)
{
	static const u16 cp1252[32] = { 0x20ac, 0, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17d, 0,
					0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0, 0x17e, 0x178 };
	bool latin = charset && (str_find_nocase(charset, "8859") || str_find_nocase(charset, "1252") || str_find_nocase(charset, "latin") ||
				 str_eq_nocase(charset, "us-ascii") || str_eq_nocase(charset, "ascii"));
	StrBuf sb = { 0 };
	sb_reserve(&sb, n + 16);
	if (!latin) {
		/* UTF-8 (or unknown): keep valid sequences, replace broken bytes */
		for (usize i = 0; i < n;) {
			u32 cp;
			usize k = utf8_decode(s + i, n - i, &cp);
			if (cp == 0xfffd && !(k == 3 && (u8)s[i] == 0xef))
				sb_addcp(&sb, (u8)s[i] < 0x80 ? (u8)s[i] : 0xfffd), k = 1;
			else
				sb_add(&sb, s + i, k);
			i += k;
		}
	} else {
		for (usize i = 0; i < n; i++) {
			u8 c = (u8)s[i];
			if (c < 0x80)
				sb_addc(&sb, (char)c);
			else if (c < 0xa0 && cp1252[c - 0x80])
				sb_addcp(&sb, cp1252[c - 0x80]);
			else
				sb_addcp(&sb, c);
		}
	}
	if (out_len)
		*out_len = sb.len;
	return sb.data ? sb_take(&sb) : str_dup("");
}

static char *qp_decode(const char *s, usize n, usize *out_len, bool header)
{
	char *out = mem_alloc(n + 1);
	usize w = 0;
	for (usize i = 0; i < n; i++) {
		char c = s[i];
		if (c == '=' && i + 2 < n + 1 && i + 1 < n) {
			if (s[i + 1] == '\r' && i + 2 < n && s[i + 2] == '\n') {
				i += 2;   /* soft line break */
				continue;
			}
			if (s[i + 1] == '\n') {
				i += 1;
				continue;
			}
			if (i + 2 < n && is_xdigit(s[i + 1]) && is_xdigit(s[i + 2])) {
				out[w++] = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
				i += 2;
				continue;
			}
		}
		out[w++] = header && c == '_' ? ' ' : c;
	}
	out[w] = 0;
	*out_len = w;
	return out;
}

char *mime_words(const char *s)
{
	StrBuf sb = { 0 };
	const char *p = s;
	bool last_word = false;
	while (*p) {
		const char *st = str_find(p, "=?");
		if (!st) {
			sb_adds(&sb, p);
			break;
		}
		/* white space between two encoded words disappears */
		const char *gap = p;
		bool only_space = true;
		for (const char *g = gap; g < st; g++)
			if (!is_space(*g))
				only_space = false;
		if (!(last_word && only_space))
			sb_add(&sb, p, (usize)(st - p));
		const char *q1 = str_chr(st + 2, '?');
		const char *q2 = q1 ? str_chr(q1 + 1, '?') : NULL;
		const char *end = q2 ? str_find(q2 + 1, "?=") : NULL;
		if (!q1 || !q2 || !end || q2 - q1 != 2) {
			sb_add(&sb, st, 2);
			p = st + 2;
			last_word = false;
			continue;
		}
		char charset[48];
		str_copy(charset, st + 2, MIN(sizeof charset, (usize)(q1 - st - 2) + 1));
		char *star = (char *)str_chr(charset, '*');
		if (star)
			*star = 0;
		char enc = (char)to_upper(q1[1]);
		const char *data = q2 + 1;
		usize dn = (usize)(end - data);
		u8 *raw = NULL;
		usize rn = 0;
		if (enc == 'B')
			raw = base64_decode(data, dn, &rn);
		else
			raw = (u8 *)qp_decode(data, dn, &rn, true);
		if (raw) {
			usize un;
			char *u = mime_to_utf8((char *)raw, rn, charset, &un);
			sb_add(&sb, u, un);
			mem_free(u);
			mem_free(raw);
		}
		p = end + 2;
		last_word = true;
	}
	return sb.data ? sb_take(&sb) : str_dup("");
}

/* ---- Structure ------------------------------------------------------------------------------------ */

static void parse_part(Part *p, const char *msg, usize len, int depth)
{
	usize body_at;
	usize hl = header_end(msg, len, &body_at);
	p->headers = msg;
	p->headers_len = hl;
	p->body = msg + body_at;
	p->body_len = len - body_at;
	char *ct = mime_header(msg, hl, "Content-Type");
	if (ct) {
		usize n = 0;
		while (ct[n] && ct[n] != ';' && !is_space(ct[n]) && n < sizeof p->type - 1) {
			p->type[n] = (char)to_lower(ct[n]);
			n++;
		}
		p->type[n] = 0;
		param(ct, "charset", p->charset, sizeof p->charset);
		param(ct, "name", p->filename, sizeof p->filename);
	} else {
		str_copy(p->type, "text/plain", sizeof p->type);
	}
	char *te = mime_header(msg, hl, "Content-Transfer-Encoding");
	if (te) {
		str_copy(p->encoding, str_trim(te), sizeof p->encoding);
		str_lower(p->encoding);
	}
	char *cd = mime_header(msg, hl, "Content-Disposition");
	if (cd) {
		p->attachment = str_eq_nocase_n(str_trim(cd), "attachment", 10);
		char fname[160];
		if (param(cd, "filename", fname, sizeof fname))
			str_copy(p->filename, fname, sizeof p->filename);
	}
	char *cid = mime_header(msg, hl, "Content-ID");
	if (cid) {
		char *s = str_trim(cid);
		if (*s == '<')
			s++;
		str_copy(p->cid, s, sizeof p->cid);
		char *gt = (char *)str_chr(p->cid, '>');
		if (gt)
			*gt = 0;
	}
	if (p->filename[0]) {
		char *dec = mime_words(p->filename);
		str_copy(p->filename, dec, sizeof p->filename);
		mem_free(dec);
	}
	char boundary[200];
	if (str_starts(p->type, "multipart/") && ct && param(ct, "boundary", boundary, sizeof boundary) && depth < 12) {
		char delim[220];
		fmt(delim, sizeof delim, "--%s", boundary);
		usize dl = strlen(delim);
		const char *b = p->body, *end = p->body + p->body_len;
		const char *cur = find_n(b, (usize)(end - b), delim);
		VEC(Part) kids = { 0 };
		while (cur) {
			const char *after = cur + dl;
			if (after + 2 <= end && after[0] == '-' && after[1] == '-')
				break;   /* closing delimiter */
			while (after < end && *after != '\n')
				after++;
			if (after < end)
				after++;
			const char *next = find_n(after, (usize)(end - after), delim);
			const char *stop = next ? next : end;
			/* the line break before the delimiter belongs to it */
			if (stop > after && stop[-1] == '\n')
				stop--;
			if (stop > after && stop[-1] == '\r')
				stop--;
			Part kid = { 0 };
			parse_part(&kid, after, (usize)(stop - after), depth + 1);
			vec_push(&kids, kid);
			cur = next;
		}
		p->kids = kids.data;
		p->nkids = (int)kids.len;
	} else if (str_eq(p->type, "message/rfc822") && depth < 12) {
		p->kids = NEW(Part);
		p->nkids = 1;
		parse_part(p->kids, p->body, p->body_len, depth + 1);
	}
	mem_free(ct);
	mem_free(te);
	mem_free(cd);
	mem_free(cid);
}

Part *mime_parse(const char *msg, usize len)
{
	Part *p = NEW(Part);
	parse_part(p, msg, len, 0);
	return p;
}

static void free_kids(Part *p)
{
	for (int i = 0; i < p->nkids; i++)
		free_kids(&p->kids[i]);
	mem_free(p->kids);
}

void mime_free(Part *p)
{
	if (!p)
		return;
	free_kids(p);
	mem_free(p);
}

char *mime_body(const Part *p, usize *len)
{
	u8 *raw;
	usize n;
	if (str_eq(p->encoding, "base64")) {
		raw = base64_decode(p->body, p->body_len, &n);
		if (!raw)
			raw = (u8 *)str_dup(""), n = 0;
	} else if (str_eq(p->encoding, "quoted-printable")) {
		raw = (u8 *)qp_decode(p->body, p->body_len, &n, false);
	} else {
		raw = (u8 *)str_ndup(p->body, p->body_len);
		n = p->body_len;
	}
	if (str_starts(p->type, "text/")) {
		char *u = mime_to_utf8((char *)raw, n, p->charset, len);
		mem_free(raw);
		return u;
	}
	*len = n;
	return (char *)raw;
}

const Part *mime_best_text(const Part *p, bool *is_html)
{
	if (str_eq(p->type, "text/html") && !p->attachment) {
		*is_html = true;
		return p;
	}
	if (str_eq(p->type, "text/plain") && !p->attachment) {
		*is_html = false;
		return p;
	}
	if (str_eq(p->type, "multipart/alternative")) {
		/* the richest alternative we can show: the last one we know */
		for (int i = p->nkids - 1; i >= 0; i--) {
			bool h;
			const Part *t = mime_best_text(&p->kids[i], &h);
			if (t) {
				*is_html = h;
				return t;
			}
		}
		return NULL;
	}
	for (int i = 0; i < p->nkids; i++) {
		bool h;
		const Part *t = mime_best_text(&p->kids[i], &h);
		if (t) {
			*is_html = h;
			return t;
		}
	}
	return NULL;
}

void mime_attachments(const Part *p, const Part **out, int *n, int max)
{
	if (*n >= max)
		return;
	if (!p->nkids && (p->attachment || (p->filename[0] && !p->cid[0])) && !str_starts(p->type, "multipart/")) {
		out[(*n)++] = p;
		return;
	}
	for (int i = 0; i < p->nkids; i++)
		mime_attachments(&p->kids[i], out, n, max);
}

const Part *mime_find_cid(const Part *p, const char *cid)
{
	if (p->cid[0] && str_eq(p->cid, cid))
		return p;
	for (int i = 0; i < p->nkids; i++) {
		const Part *f = mime_find_cid(&p->kids[i], cid);
		if (f)
			return f;
	}
	return NULL;
}

/* ---- Dates and addresses ---------------------------------------------------------------------------- */

i64 mime_date(const char *s)
{
	if (!s)
		return 0;
	const char *p = s;
	/* skip a weekday */
	while (*p && is_space(*p))
		p++;
	if (is_alpha(*p)) {
		while (*p && *p != ',' && !is_space(*p))
			p++;
		while (*p == ',' || is_space(*p))
			p++;
	}
	Civil c = { 0 };
	const char *e;
	c.day = (int)str_to_int(p, &e);
	p = e;
	while (*p == ' ' || *p == '-')
		p++;
	c.month = 0;
	for (int m = 0; m < 12; m++)
		if (str_eq_nocase_n(p, MONTH_NAMES[m], 3))
			c.month = m + 1;
	if (!c.month)
		return 0;
	while (*p && *p != ' ' && *p != '-')
		p++;
	while (*p == ' ' || *p == '-')
		p++;
	c.year = (int)str_to_int(p, &e);
	if (c.year < 100)
		c.year += c.year < 70 ? 2000 : 1900;
	p = e;
	while (*p == ' ')
		p++;
	c.hour = (int)str_to_int(p, &e);
	p = e;
	if (*p == ':')
		c.min = (int)str_to_int(p + 1, &e), p = e;
	if (*p == ':')
		c.sec = (int)str_to_int(p + 1, &e), p = e;
	while (*p == ' ')
		p++;
	int off = 0;
	if (*p == '+' || *p == '-') {
		int v = (int)str_to_int(p + 1, NULL);
		off = (v / 100) * 3600 + (v % 100) * 60;
		if (*p == '-')
			off = -off;
	}
	return unix_from_civil(&c) - off;
}

void mime_address(const char *s, char *name, usize ncap, char *addr, usize acap)
{
	name[0] = addr[0] = 0;
	if (!s)
		return;
	char *dec = mime_words(s);
	const char *lt = str_chr(dec, '<');
	if (lt) {
		const char *gt = str_chr(lt, '>');
		str_copy(addr, lt + 1, MIN(acap, (usize)((gt ? gt : lt + strlen(lt)) - lt - 1) + 1));
		char tmp[256];
		str_copy(tmp, dec, MIN(sizeof tmp, (usize)(lt - dec) + 1));
		char *t = str_trim(tmp);
		usize n = strlen(t);
		if (n >= 2 && t[0] == '"' && t[n - 1] == '"') {
			t[n - 1] = 0;
			t++;
		}
		str_copy(name, t, ncap);
	} else {
		str_copy(addr, str_trim(dec), acap);
	}
	mem_free(dec);
}

/* ---- Writing -------------------------------------------------------------------------------------- */

char *mime_encode_header(const char *s)
{
	bool ascii = true;
	for (const char *p = s; *p; p++)
		if ((u8)*p >= 0x80)
			ascii = false;
	if (ascii)
		return str_dup(s);
	char *b = base64_encode(s, strlen(s));
	char *r = str_fmt("=?UTF-8?B?%s?=", b);
	mem_free(b);
	return r;
}

char *mime_quoted_printable(const char *s, usize n)
{
	StrBuf sb = { 0 };
	int col = 0;
	for (usize i = 0; i < n; i++) {
		u8 c = (u8)s[i];
		if (c == '\n') {
			/* trailing white space must be encoded */
			if (sb.len && (sb.data[sb.len - 1] == ' ' || sb.data[sb.len - 1] == '\t')) {
				char last = sb.data[--sb.len];
				sb_fmt(&sb, "=%02X", (u8)last);
			}
			sb_adds(&sb, "\r\n");
			col = 0;
			continue;
		}
		if (c == '\r')
			continue;
		char enc[4];
		int w;
		if ((c >= 33 && c <= 126 && c != '=') || c == ' ' || c == '\t') {
			enc[0] = (char)c, enc[1] = 0, w = 1;
		} else {
			fmt(enc, sizeof enc, "=%02X", c);
			w = 3;
		}
		if (col + w > 75) {
			sb_adds(&sb, "=\r\n");
			col = 0;
		}
		sb_adds(&sb, enc);
		col += w;
	}
	return sb.data ? sb_take(&sb) : str_dup("");
}
