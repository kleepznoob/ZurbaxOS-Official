/* Clocks, calendar arithmetic and time zones (TZif files + POSIX TZ rules). */
#include "zr.h"

const char *const MONTH_NAMES[12] = { "January", "February", "March", "April", "May", "June", "July",
				      "August", "September", "October", "November", "December" };
const char *const WEEKDAY_NAMES[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

u64 time_mono_ns(void)
{
	struct timespec ts;
	sys_clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec;
}

i64 time_unix(void)
{
	struct timespec ts;
	sys_clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec;
}

i64 time_unix_ms(void)
{
	struct timespec ts;
	sys_clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(u32 ms)
{
	struct timespec ts = { ms / 1000, (i64)(ms % 1000) * 1000000 };
	while (sys_nanosleep(&ts, &ts) == -EINTR)
		;
}

/* ---- Calendar ----------------------------------------------------------------
 * Day counting uses the proleptic Gregorian calendar with years shifted to
 * start in March, so the leap day falls at the end of the shifted year. */

static i64 days_from_ymd(i64 y, int m, int d)
{
	y -= m <= 2;
	i64 era = (y >= 0 ? y : y - 399) / 400;
	i64 yoe = y - era * 400;                                   /* 0..399 */
	i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  /* 0..365 */
	i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* 0..146096 */
	return era * 146097 + doe - 719468;
}

static void ymd_from_days(i64 z, i64 *y, int *m, int *d)
{
	z += 719468;
	i64 era = (z >= 0 ? z : z - 146096) / 146097;
	i64 doe = z - era * 146097;
	i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	i64 mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = yoe + era * 400 + (*m <= 2);
}

static bool is_leap(int y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int days_in_month(int year, int month)
{
	static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	return month == 2 && is_leap(year) ? 29 : dm[(month - 1) % 12];
}

void civil_from_unix(i64 t, Civil *c)
{
	i64 days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
	i64 secs = t - days * 86400;
	i64 y;
	ymd_from_days(days, &y, &c->month, &c->day);
	c->year = (int)y;
	c->hour = (int)(secs / 3600);
	c->min = (int)(secs / 60 % 60);
	c->sec = (int)(secs % 60);
	c->wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */
	c->yday = (int)(days - days_from_ymd(y, 1, 1));
}

i64 unix_from_civil(const Civil *c)
{
	return days_from_ymd(c->year, c->month, c->day) * 86400 + c->hour * 3600 + c->min * 60 + c->sec;
}

/* ---- Time zones ------------------------------------------------------------------ */

typedef struct {
	/* POSIX rule: std offset, optional dst with start/end rules */
	i32 std_off, dst_off;        /* seconds east of UTC */
	bool has_dst;
	struct {
		char kind;               /* 'M' month/week/day, 'J' julian (no leap), 'D' zero-based day */
		int m, w, d;             /* for M: month, week (1..5), weekday; for J/D: day in d */
		i32 time;                /* seconds after local midnight */
	} start, end;
} TzRule;

static struct {
	char name[64];
	i64 *times;
	u8 *idx;
	i32 *offsets;
	usize ntimes, ntypes;
	TzRule rule;
	bool has_rule;
	bool loaded;
} tz;

static const char *parse_tz_offset(const char *s, i32 *out)
{
	/* [+-]hh[:mm[:ss]]; POSIX offsets are west-positive, we store east-positive */
	int sign = 1;
	if (*s == '+' || *s == '-')
		sign = *s++ == '-' ? -1 : 1;
	i32 v = (i32)str_to_int(s, &s) * 3600;
	if (*s == ':') {
		v += (i32)str_to_int(s + 1, &s) * 60;
		if (*s == ':')
			v += (i32)str_to_int(s + 1, &s);
	}
	*out = -sign * v;
	return s;
}

static const char *skip_tz_name(const char *s)
{
	if (*s == '<') {
		while (*s && *s != '>')
			s++;
		return *s ? s + 1 : s;
	}
	while (is_alpha(*s))
		s++;
	return s;
}

static const char *parse_rule_date(const char *s, char *kind, int *m, int *w, int *d, i32 *time)
{
	*time = 7200;
	if (*s == 'M') {
		*kind = 'M';
		*m = (int)str_to_int(s + 1, &s);
		if (*s == '.')
			*w = (int)str_to_int(s + 1, &s);
		if (*s == '.')
			*d = (int)str_to_int(s + 1, &s);
	} else if (*s == 'J') {
		*kind = 'J';
		*d = (int)str_to_int(s + 1, &s);
	} else {
		*kind = 'D';
		*d = (int)str_to_int(s, &s);
	}
	if (*s == '/') {
		i32 t;
		s = parse_tz_offset(s + 1, &t);
		*time = -t; /* parse_tz_offset negates; times are plain positive */
	}
	return s;
}

static bool parse_posix_rule(const char *s, TzRule *r)
{
	memset(r, 0, sizeof *r);
	s = skip_tz_name(s);
	if (!*s)
		return false;
	s = parse_tz_offset(s, &r->std_off);
	if (!*s)
		return true;
	s = skip_tz_name(s);
	r->has_dst = true;
	r->dst_off = r->std_off + 3600;
	if (*s && *s != ',')
		s = parse_tz_offset(s, &r->dst_off);
	if (*s != ',')
		return true;
	s = parse_rule_date(s + 1, &r->start.kind, &r->start.m, &r->start.w, &r->start.d, &r->start.time);
	if (*s != ',')
		return false;
	parse_rule_date(s + 1, &r->end.kind, &r->end.m, &r->end.w, &r->end.d, &r->end.time);
	return true;
}

/* Local-midnight day number (days since epoch) for a rule date in `year`. */
static i64 rule_day(int year, char kind, int m, int w, int d)
{
	if (kind == 'J') {
		i64 day = days_from_ymd(year, 1, 1) + d - 1;
		if (is_leap(year) && d >= 60)
			day++;
		return day;
	}
	if (kind == 'D')
		return days_from_ymd(year, 1, 1) + d;
	/* Mm.w.d: day d (0=Sun) of week w (5 = last) of month m */
	i64 first = days_from_ymd(year, m, 1);
	int first_wday = (int)((first % 7 + 11) % 7);
	int offset = (d - first_wday + 7) % 7;
	i64 day = first + offset + (w - 1) * 7;
	while (day >= first + days_in_month(year, m))
		day -= 7;
	return day;
}

static i32 rule_offset(const TzRule *r, i64 utc)
{
	if (!r->has_dst)
		return r->std_off;
	Civil c;
	civil_from_unix(utc + r->std_off, &c);
	/* Transition instants in UTC: start uses std time, end uses dst time. */
	i64 start = rule_day(c.year, r->start.kind, r->start.m, r->start.w, r->start.d) * 86400 + r->start.time - r->std_off;
	i64 end = rule_day(c.year, r->end.kind, r->end.m, r->end.w, r->end.d) * 86400 + r->end.time - r->dst_off;
	bool dst = start < end ? (utc >= start && utc < end) : !(utc >= end && utc < start);
	return dst ? r->dst_off : r->std_off;
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u64 be64(const u8 *p) { return (u64)be32(p) << 32 | be32(p + 4); }

static usize tzif_block(const u8 *h, usize tsize)
{
	u32 isut = be32(h + 20), isstd = be32(h + 24), leap = be32(h + 28);
	u32 ntime = be32(h + 32), ntype = be32(h + 36), nchar = be32(h + 40);
	return ntime * tsize + ntime + ntype * 6 + nchar + leap * (tsize + 4) + isstd + isut;
}

bool tz_set(const char *zone)
{
	char *path = str_fmt("/system/share/zoneinfo/%s", zone);
	usize len;
	u8 *d = (u8 *)file_read(path, &len);
	mem_free(path);
	if (!d || len < 44 || memcmp(d, "TZif", 4) != 0) {
		mem_free(d);
		/* No compiled zone file: use the built-in rule for that city. */
		const ZoneInfo *zi = tz_find(zone);
		if (!zi)
			return false;
		mem_free(tz.times);
		mem_free(tz.idx);
		mem_free(tz.offsets);
		memset(&tz, 0, sizeof tz);
		tz.has_rule = parse_posix_rule(zi->posix, &tz.rule);
		tz.loaded = tz.has_rule;
		str_copy(tz.name, zone, sizeof tz.name);
		return tz.loaded;
	}
	mem_free(tz.times);
	mem_free(tz.idx);
	mem_free(tz.offsets);
	memset(&tz, 0, sizeof tz);

	/* Version 2+ files repeat the data with 64-bit times after the
	 * 32-bit block; prefer that copy. */
	const u8 *h = d, *end = d + len;
	usize tsize = 4;
	usize block = tzif_block(h, tsize);
	if (d[4] >= '2' && 44 + block + 44 <= len) {
		h = d + 44 + block;
		tsize = 8;
		block = tzif_block(h, tsize);
	}
	if ((usize)(end - h) < 44 + block) {
		mem_free(d);
		return false;
	}
	u32 ntime = be32(h + 32), ntype = be32(h + 36);
	const u8 *q = h + 44;
	tz.ntimes = ntime;
	tz.ntypes = ntype;
	tz.times = NEW_ARRAY(i64, ntime + 1);
	tz.idx = NEW_ARRAY(u8, ntime + 1);
	tz.offsets = NEW_ARRAY(i32, ntype + 1);
	for (u32 i = 0; i < ntime; i++)
		tz.times[i] = tsize == 8 ? (i64)be64(q + i * 8) : (i32)be32(q + i * 4);
	q += ntime * tsize;
	for (u32 i = 0; i < ntime; i++)
		tz.idx[i] = q[i] < ntype ? q[i] : 0;
	q += ntime;
	for (u32 i = 0; i < ntype; i++)
		tz.offsets[i] = (i32)be32(q + i * 6);
	tz.loaded = true;
	/* Footer "\n<POSIX rule>\n" covers instants after the last transition. */
	const u8 *f = h + 44 + block;
	if (tsize == 8 && f < end && *f == '\n') {
		const u8 *fe = f + 1;
		while (fe < end && *fe != '\n')
			fe++;
		char *rule = str_ndup((const char *)f + 1, (usize)(fe - f - 1));
		tz.has_rule = parse_posix_rule(rule, &tz.rule);
		mem_free(rule);
	}
	mem_free(d);
	str_copy(tz.name, zone, sizeof tz.name);
	return tz.loaded;
}

const char *tz_name(void)
{
	return tz.loaded ? tz.name : "UTC";
}

i32 tz_offset(i64 utc)
{
	if (!tz.loaded)
		return 0;
	if (tz.ntimes == 0 || utc >= tz.times[tz.ntimes - 1]) {
		if (tz.has_rule)
			return rule_offset(&tz.rule, utc);
		if (tz.ntimes == 0)
			return tz.ntypes ? tz.offsets[0] : 0;
	}
	if (utc < tz.times[0])
		return tz.offsets[0];
	usize lo = 0, hi = tz.ntimes;
	while (hi - lo > 1) {
		usize mid = (lo + hi) / 2;
		if (tz.times[mid] <= utc)
			lo = mid;
		else
			hi = mid;
	}
	return tz.offsets[tz.idx[lo]];
}

i32 tz_offset_in(const char *zone, i64 utc)
{
	const ZoneInfo *zi = tz_find(zone);
	TzRule r;
	if (!zi || !parse_posix_rule(zi->posix, &r))
		return 0;
	return rule_offset(&r, utc);
}

void civil_local(i64 utc, Civil *out)
{
	civil_from_unix(utc + tz_offset(utc), out);
}
