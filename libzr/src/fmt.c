/* printf-style formatting and output. */
#include "zr.h"

typedef struct {
	char *buf;
	usize cap;
	usize len; /* total length that would have been written */
} Out;

static void put(Out *o, char c)
{
	if (o->len + 1 < o->cap)
		o->buf[o->len] = c;
	o->len++;
}

static void put_n(Out *o, const char *s, usize n)
{
	for (usize i = 0; i < n; i++)
		put(o, s[i]);
}

static void pad(Out *o, char c, int n)
{
	while (n-- > 0)
		put(o, c);
}

enum { F_LEFT = 1, F_PLUS = 2, F_SPACE = 4, F_ZERO = 8, F_ALT = 16 };

static void emit_number(Out *o, const char *digits, usize ndig, const char *prefix, int flags, int width, int prec)
{
	usize plen = strlen(prefix);
	int zeros = prec > (int)ndig ? prec - (int)ndig : 0;
	int total = (int)(plen + ndig) + zeros;
	if (!(flags & F_LEFT) && !(flags & F_ZERO))
		pad(o, ' ', width - total);
	put_n(o, prefix, plen);
	if (!(flags & F_LEFT) && (flags & F_ZERO))
		pad(o, '0', width - total);
	pad(o, '0', zeros);
	put_n(o, digits, ndig);
	if (flags & F_LEFT)
		pad(o, ' ', width - total);
}

static usize utoa(u64 v, unsigned base, bool upper, char *out)
{
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24];
	usize n = 0;
	do {
		tmp[n++] = dig[v % base];
		v /= base;
	} while (v);
	for (usize i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

/* Writes |v| with `prec` digits after the point into out (no sign). */
static usize ftoa_fixed(f64 v, int prec, char *out, usize cap)
{
	static const f64 pow10[] = { 1, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
				     1e12, 1e13, 1e14, 1e15, 1e16, 1e17 };
	if (prec > 17)
		prec = 17;
	usize n = 0;
	if (v >= 1e18) {
		/* Too large for an integer split: emit digits by repeated division. */
		int e = 0;
		while (v >= 10) {
			v /= 10;
			e++;
		}
		for (int i = 0; i <= e && n < cap - 1; i++) {
			int d = (int)v;
			out[n++] = (char)('0' + d);
			v = (v - d) * 10;
		}
		if (prec > 0 && n < cap - 1)
			out[n++] = '.';
		for (int i = 0; i < prec && n < cap - 1; i++)
			out[n++] = '0';
		return n;
	}
	f64 scaled = v * pow10[prec] + 0.5;
	u64 ip, fp;
	if (scaled < 1.8e19) {
		u64 all = (u64)scaled;
		u64 div = (u64)pow10[prec];
		ip = all / div;
		fp = all % div;
	} else {
		ip = (u64)v;
		fp = (u64)((v - (f64)ip) * pow10[prec] + 0.5);
	}
	n = utoa(ip, 10, false, out);
	if (prec > 0) {
		out[n++] = '.';
		char frac[24];
		usize fl = utoa(fp, 10, false, frac);
		for (int i = (int)fl; i < prec; i++)
			out[n++] = '0';
		memcpy(out + n, frac, fl);
		n += fl;
	}
	return n;
}

static int decimal_exponent(f64 v)
{
	int e = 0;
	if (v == 0)
		return 0;
	while (v >= 10) {
		v /= 10;
		e++;
	}
	while (v < 1) {
		v *= 10;
		e--;
	}
	return e;
}

static usize ftoa_exp(f64 v, int prec, bool upper, char *out)
{
	int e = decimal_exponent(v);
	f64 m = v;
	if (e > 0)
		for (int i = 0; i < e; i++)
			m /= 10;
	else
		for (int i = 0; i < -e; i++)
			m *= 10;
	usize n = ftoa_fixed(m, prec, out, 40);
	if (out[0] == '1' && out[1] == '0' && (n == 2 || out[2] == '.')) {
		/* Rounding carried into a new digit, e.g. 9.99 -> 10.0 */
		e++;
		n = ftoa_fixed(m / 10, prec, out, 40);
	}
	out[n++] = upper ? 'E' : 'e';
	out[n++] = e < 0 ? '-' : '+';
	if (e < 0)
		e = -e;
	if (e < 10)
		out[n++] = '0';
	n += utoa((u64)e, 10, false, out + n);
	return n;
}

static usize strip_zeros(char *s, usize n)
{
	char *dot = NULL, *exp = NULL;
	for (usize i = 0; i < n; i++) {
		if (s[i] == '.')
			dot = s + i;
		if (s[i] == 'e' || s[i] == 'E')
			exp = s + i;
	}
	if (!dot)
		return n;
	char *end = exp ? exp : s + n;
	char *p = end;
	while (p > dot + 1 && p[-1] == '0')
		p--;
	if (p == dot + 1)
		p = dot;
	usize tail = (usize)(s + n - end);
	memmove(p, end, tail);
	return (usize)(p - s) + tail;
}

int fmt_v(char *buf, usize cap, const char *f, va_list ap)
{
	Out o = { buf, cap, 0 };
	for (; *f; f++) {
		if (*f != '%') {
			put(&o, *f);
			continue;
		}
		f++;
		int flags = 0;
		for (;; f++) {
			if (*f == '-') flags |= F_LEFT;
			else if (*f == '+') flags |= F_PLUS;
			else if (*f == ' ') flags |= F_SPACE;
			else if (*f == '0') flags |= F_ZERO;
			else if (*f == '#') flags |= F_ALT;
			else break;
		}
		int width = 0, prec = -1;
		if (*f == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				flags |= F_LEFT;
				width = -width;
			}
			f++;
		} else {
			while (is_digit(*f))
				width = width * 10 + (*f++ - '0');
		}
		if (*f == '.') {
			f++;
			prec = 0;
			if (*f == '*') {
				prec = va_arg(ap, int);
				f++;
			} else {
				while (is_digit(*f))
					prec = prec * 10 + (*f++ - '0');
			}
		}
		int lng = 0;
		while (*f == 'l' || *f == 'z' || *f == 'h' || *f == 'j' || *f == 't') {
			if (*f == 'l' || *f == 'z' || *f == 'j' || *f == 't')
				lng++;
			f++;
		}
		char digits[64];
		usize nd;
		switch (*f) {
		case 'd':
		case 'i': {
			i64 v = lng ? va_arg(ap, i64) : va_arg(ap, int);
			u64 u = v < 0 ? -(u64)v : (u64)v;
			nd = (prec == 0 && u == 0) ? 0 : utoa(u, 10, false, digits);
			const char *prefix = v < 0 ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
			emit_number(&o, digits, nd, prefix, prec >= 0 ? flags & ~F_ZERO : flags, width, prec);
			break;
		}
		case 'u':
		case 'x':
		case 'X':
		case 'o': {
			u64 u = lng ? va_arg(ap, u64) : va_arg(ap, unsigned);
			unsigned base = *f == 'o' ? 8 : *f == 'u' ? 10 : 16;
			nd = (prec == 0 && u == 0) ? 0 : utoa(u, base, *f == 'X', digits);
			const char *prefix = "";
			if ((flags & F_ALT) && u && base == 16)
				prefix = *f == 'X' ? "0X" : "0x";
			emit_number(&o, digits, nd, prefix, prec >= 0 ? flags & ~F_ZERO : flags, width, prec);
			break;
		}
		case 'p': {
			u64 u = (u64)(uintptr_t)va_arg(ap, void *);
			nd = utoa(u, 16, false, digits);
			emit_number(&o, digits, nd, "0x", flags, width, -1);
			break;
		}
		case 'c': {
			char c = (char)va_arg(ap, int);
			if (!(flags & F_LEFT))
				pad(&o, ' ', width - 1);
			put(&o, c);
			if (flags & F_LEFT)
				pad(&o, ' ', width - 1);
			break;
		}
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			usize n = 0;
			while (s[n] && (prec < 0 || (int)n < prec))
				n++;
			if (!(flags & F_LEFT))
				pad(&o, ' ', width - (int)n);
			put_n(&o, s, n);
			if (flags & F_LEFT)
				pad(&o, ' ', width - (int)n);
			break;
		}
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G': {
			f64 v = va_arg(ap, f64);
			const char *prefix = "";
			if (v < 0 || (v == 0 && 1 / v < 0)) {
				prefix = "-";
				v = -v;
			} else if (flags & F_PLUS) {
				prefix = "+";
			} else if (flags & F_SPACE) {
				prefix = " ";
			}
			if (v != v) {
				nd = 3, memcpy(digits, "nan", 3);
			} else if (v > 1.7976931348623157e308) {
				nd = 3, memcpy(digits, "inf", 3);
			} else {
				if (prec < 0)
					prec = 6;
				char c = (char)(*f | 32);
				if (c == 'f') {
					nd = ftoa_fixed(v, prec, digits, sizeof digits);
				} else if (c == 'e') {
					nd = ftoa_exp(v, prec, *f == 'E', digits);
				} else {
					int p = prec == 0 ? 1 : prec;
					/* Format in exponent form first: its exponent already
					 * reflects rounding (9.9999 -> 1.0e+01). */
					nd = ftoa_exp(v, p - 1, *f == 'G', digits);
					digits[nd] = 0;
					usize ei = nd;
					while (ei > 0 && digits[ei - 1] != 'e' && digits[ei - 1] != 'E')
						ei--;
					int e = (int)str_to_int(digits + ei, NULL);
					if (e >= -4 && e < p)
						nd = ftoa_fixed(v, p - 1 - e, digits, sizeof digits);
					if (!(flags & F_ALT))
						nd = strip_zeros(digits, nd);
				}
			}
			emit_number(&o, digits, nd, prefix, flags, width, -1);
			break;
		}
		case '%':
			put(&o, '%');
			break;
		default:
			put(&o, '%');
			if (*f)
				put(&o, *f);
			else
				f--;
			break;
		}
	}
	if (cap)
		buf[o.len < cap ? o.len : cap - 1] = 0;
	return (int)o.len;
}

int fmt(char *buf, usize cap, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int n = fmt_v(buf, cap, f, ap);
	va_end(ap);
	return n;
}

char *str_fmt_v(const char *f, va_list ap)
{
	va_list ap2;
	va_copy(ap2, ap);
	char small[256];
	int n = fmt_v(small, sizeof small, f, ap);
	char *s = mem_alloc((usize)n + 1);
	if ((usize)n < sizeof small)
		memcpy(s, small, (usize)n + 1);
	else
		fmt_v(s, (usize)n + 1, f, ap2);
	va_end(ap2);
	return s;
}

char *str_fmt(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	char *s = str_fmt_v(f, ap);
	va_end(ap);
	return s;
}

static void fd_vprint(int fd, const char *f, va_list ap)
{
	char *s = str_fmt_v(f, ap);
	write_all(fd, s, strlen(s));
	mem_free(s);
}

void print(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	fd_vprint(1, f, ap);
	va_end(ap);
}

void eprint(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	fd_vprint(2, f, ap);
	va_end(ap);
}

void fd_print(int fd, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	fd_vprint(fd, f, ap);
	va_end(ap);
}

void fmt_bytes(char *buf, usize cap, u64 bytes)
{
	static const char *units[] = { "bytes", "KB", "MB", "GB", "TB" };
	if (bytes < 1000) {
		fmt(buf, cap, "%llu %s", (unsigned long long)bytes, bytes == 1 ? "byte" : "bytes");
		return;
	}
	f64 v = (f64)bytes;
	int u = 0;
	while (v >= 1000 && u < 4) {
		v /= 1000;
		u++;
	}
	fmt(buf, cap, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
}

/* ---- StrBuf ------------------------------------------------------------------ */

void sb_reserve(StrBuf *sb, usize extra)
{
	usize need = sb->len + extra + 1;
	if (need <= sb->cap)
		return;
	usize cap = sb->cap ? sb->cap : 32;
	while (cap < need)
		cap *= 2;
	sb->data = mem_realloc(sb->data, cap);
	sb->cap = cap;
}

void sb_add(StrBuf *sb, const void *data, usize n)
{
	sb_reserve(sb, n);
	memcpy(sb->data + sb->len, data, n);
	sb->len += n;
	sb->data[sb->len] = 0;
}

void sb_adds(StrBuf *sb, const char *s)
{
	sb_add(sb, s, strlen(s));
}

void sb_addc(StrBuf *sb, char c)
{
	sb_reserve(sb, 1);
	sb->data[sb->len++] = c;
	sb->data[sb->len] = 0;
}

void sb_addcp(StrBuf *sb, u32 cp)
{
	char tmp[4];
	sb_add(sb, tmp, utf8_encode(cp, tmp));
}

void sb_fmt(StrBuf *sb, const char *f, ...)
{
	va_list ap, ap2;
	va_start(ap, f);
	va_copy(ap2, ap);
	sb_reserve(sb, 64);
	usize room = sb->cap - sb->len;
	int n = fmt_v(sb->data + sb->len, room, f, ap);
	if ((usize)n >= room) {
		sb_reserve(sb, (usize)n);
		fmt_v(sb->data + sb->len, sb->cap - sb->len, f, ap2);
	}
	sb->len += (usize)n;
	va_end(ap2);
	va_end(ap);
}

void sb_insert(StrBuf *sb, usize pos, const char *s, usize n)
{
	if (pos > sb->len)
		pos = sb->len;
	sb_reserve(sb, n);
	memmove(sb->data + pos + n, sb->data + pos, sb->len - pos + 1);
	memcpy(sb->data + pos, s, n);
	sb->len += n;
}

void sb_remove(StrBuf *sb, usize pos, usize n)
{
	if (pos >= sb->len)
		return;
	if (pos + n > sb->len)
		n = sb->len - pos;
	memmove(sb->data + pos, sb->data + pos + n, sb->len - pos - n + 1);
	sb->len -= n;
}

void sb_clear(StrBuf *sb)
{
	sb->len = 0;
	if (sb->data)
		sb->data[0] = 0;
}

char *sb_take(StrBuf *sb)
{
	char *s = sb->data ? sb->data : str_dup("");
	sb->data = NULL;
	sb->len = sb->cap = 0;
	return s;
}

void sb_free(StrBuf *sb)
{
	mem_free(sb->data);
	sb->data = NULL;
	sb->len = sb->cap = 0;
}
