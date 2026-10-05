/* C string helpers and UTF-8 handling. */
#include "zr.h"

bool str_eq(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

bool str_eq_nocase(const char *a, const char *b)
{
	return str_cmp_nocase(a, b) == 0;
}

bool str_eq_nocase_n(const char *a, const char *b, usize n)
{
	for (usize i = 0; i < n; i++) {
		u8 x = (u8)a[i], y = (u8)b[i];
		if (x >= 'A' && x <= 'Z')
			x += 32;
		if (y >= 'A' && y <= 'Z')
			y += 32;
		if (x != y)
			return false;
	}
	return true;
}

int str_cmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (u8)*a - (u8)*b;
}

int str_cmp_nocase(const char *a, const char *b)
{
	while (*a && to_lower((u8)*a) == to_lower((u8)*b))
		a++, b++;
	return to_lower((u8)*a) - to_lower((u8)*b);
}

int str_cmp_natural(const char *a, const char *b)
{
	while (*a && *b) {
		if (is_digit(*a) && is_digit(*b)) {
			while (*a == '0')
				a++;
			while (*b == '0')
				b++;
			const char *da = a, *db = b;
			while (is_digit(*da))
				da++;
			while (is_digit(*db))
				db++;
			if (da - a != db - b)
				return (int)((da - a) - (db - b));
			for (; a < da; a++, b++)
				if (*a != *b)
					return *a - *b;
			continue;
		}
		int ca = to_lower((u8)*a), cb = to_lower((u8)*b);
		if (ca != cb)
			return ca - cb;
		a++, b++;
	}
	return (u8)*a - (u8)*b;
}

bool str_starts(const char *s, const char *prefix)
{
	while (*prefix)
		if (*s++ != *prefix++)
			return false;
	return true;
}

bool str_ends(const char *s, const char *suffix)
{
	usize n = strlen(s), m = strlen(suffix);
	return m <= n && memcmp(s + n - m, suffix, m) == 0;
}

char *str_dup(const char *s)
{
	return s ? mem_dup(s, strlen(s) + 1) : NULL;
}

char *str_ndup(const char *s, usize n)
{
	usize len = 0;
	while (len < n && s[len])
		len++;
	char *d = mem_alloc(len + 1);
	memcpy(d, s, len);
	d[len] = 0;
	return d;
}

const char *str_chr(const char *s, int c)
{
	for (; *s; s++)
		if (*s == (char)c)
			return s;
	return c == 0 ? s : NULL;
}

const char *str_rchr(const char *s, int c)
{
	const char *last = NULL;
	for (; *s; s++)
		if (*s == (char)c)
			last = s;
	return c == 0 ? s : last;
}

const char *str_find(const char *hay, const char *needle)
{
	usize n = strlen(needle);
	if (n == 0)
		return hay;
	for (; *hay; hay++)
		if (*hay == *needle && memcmp(hay, needle, n) == 0)
			return hay;
	return NULL;
}

const char *str_find_nocase(const char *hay, const char *needle)
{
	usize n = strlen(needle);
	if (n == 0)
		return hay;
	for (; *hay; hay++) {
		usize i = 0;
		while (i < n && hay[i] && to_lower((u8)hay[i]) == to_lower((u8)needle[i]))
			i++;
		if (i == n)
			return hay;
	}
	return NULL;
}

usize str_copy(char *dst, const char *src, usize cap)
{
	usize n = strlen(src);
	if (cap) {
		usize m = MIN(n, cap - 1);
		memcpy(dst, src, m);
		dst[m] = 0;
	}
	return n;
}

char *str_trim(char *s)
{
	while (is_space((u8)*s))
		s++;
	usize n = strlen(s);
	while (n && is_space((u8)s[n - 1]))
		s[--n] = 0;
	return s;
}

char *str_lower(char *s)
{
	for (char *p = s; *p; p++)
		*p = (char)to_lower((u8)*p);
	return s;
}

i64 str_to_int(const char *s, const char **end)
{
	while (is_space((u8)*s))
		s++;
	bool neg = false;
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	u64 v = 0;
	int base = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && is_xdigit((u8)s[2])) {
		base = 16;
		s += 2;
	}
	for (;; s++) {
		int d;
		if (is_digit(*s))
			d = *s - '0';
		else if (base == 16 && is_xdigit((u8)*s))
			d = to_lower((u8)*s) - 'a' + 10;
		else
			break;
		v = v * (u64)base + (u64)d;
	}
	if (end)
		*end = s;
	return neg ? -(i64)v : (i64)v;
}

f64 str_to_float(const char *s, const char **end)
{
	static const f64 p10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
				   1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
				   1e20, 1e21, 1e22 };
	while (is_space((u8)*s))
		s++;
	const char *start = s;
	bool neg = false;
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	u64 mant = 0;
	int digits = 0, exp10 = 0;
	bool any = false;
	for (; is_digit(*s); s++, any = true) {
		if (digits < 19) {
			mant = mant * 10 + (u64)(*s - '0');
			if (mant)
				digits++;
		} else {
			exp10++;
		}
	}
	if (*s == '.') {
		s++;
		for (; is_digit(*s); s++, any = true) {
			if (digits < 19) {
				mant = mant * 10 + (u64)(*s - '0');
				if (mant)
					digits++;
				exp10--;
			}
		}
	}
	if (!any) {
		if (end)
			*end = start;
		return 0;
	}
	if (*s == 'e' || *s == 'E') {
		const char *e = s + 1;
		bool eneg = false;
		if (*e == '-' || *e == '+')
			eneg = *e++ == '-';
		if (is_digit(*e)) {
			int ev = 0;
			for (; is_digit(*e); e++)
				if (ev < 10000)
					ev = ev * 10 + (*e - '0');
			exp10 += eneg ? -ev : ev;
			s = e;
		}
	}
	if (end)
		*end = s;
	f64 v = (f64)mant;
	while (exp10 > 22) {
		v *= 1e22;
		exp10 -= 22;
	}
	while (exp10 < -22) {
		v /= 1e22;
		exp10 += 22;
	}
	v = exp10 >= 0 ? v * p10[exp10] : v / p10[-exp10];
	return neg ? -v : v;
}

char **str_split(const char *s, char sep, usize *count)
{
	usize n = 1;
	for (const char *p = s; *p; p++)
		n += *p == sep;
	char **parts = mem_alloc(sizeof(char *) * (n + 1));
	usize i = 0;
	const char *start = s;
	for (const char *p = s;; p++) {
		if (*p == sep || *p == 0) {
			parts[i++] = str_ndup(start, (usize)(p - start));
			start = p + 1;
			if (*p == 0)
				break;
		}
	}
	parts[i] = NULL;
	if (count)
		*count = i;
	return parts;
}

void str_split_free(char **parts, usize count)
{
	for (usize i = 0; i < count; i++)
		mem_free(parts[i]);
	mem_free(parts);
}

char *str_replace(const char *s, const char *from, const char *to)
{
	StrBuf sb = { 0 };
	usize flen = strlen(from);
	if (flen == 0)
		return str_dup(s);
	for (;;) {
		const char *hit = str_find(s, from);
		if (!hit)
			break;
		sb_add(&sb, s, (usize)(hit - s));
		sb_adds(&sb, to);
		s = hit + flen;
	}
	sb_adds(&sb, s);
	return sb_take(&sb);
}

/* ---- UTF-8 ------------------------------------------------------------------- */

usize utf8_decode(const char *str, usize len, u32 *cp)
{
	const u8 *s = (const u8 *)str;
	if (len == 0) {
		*cp = 0;
		return 0;
	}
	u8 c = s[0];
	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	usize need;
	u32 v, min;
	if ((c & 0xe0) == 0xc0) {
		need = 2, v = c & 0x1f, min = 0x80;
	} else if ((c & 0xf0) == 0xe0) {
		need = 3, v = c & 0x0f, min = 0x800;
	} else if ((c & 0xf8) == 0xf0) {
		need = 4, v = c & 0x07, min = 0x10000;
	} else {
		*cp = 0xfffd;
		return 1;
	}
	if (len < need) {
		*cp = 0xfffd;
		return 1;
	}
	for (usize i = 1; i < need; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*cp = 0xfffd;
			return i;
		}
		v = (v << 6) | (s[i] & 0x3f);
	}
	if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
		v = 0xfffd;
	*cp = v;
	return need;
}

usize utf8_encode(u32 cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

usize utf8_next(const char *s, usize len, usize pos)
{
	if (pos >= len)
		return len;
	u32 cp;
	return pos + utf8_decode(s + pos, len - pos, &cp);
}

usize utf8_prev(const char *s, usize pos)
{
	if (pos == 0)
		return 0;
	pos--;
	while (pos > 0 && ((u8)s[pos] & 0xc0) == 0x80)
		pos--;
	return pos;
}

usize utf8_count(const char *s, usize len)
{
	usize n = 0;
	for (usize i = 0; i < len; i++)
		n += ((u8)s[i] & 0xc0) != 0x80;
	return n;
}
