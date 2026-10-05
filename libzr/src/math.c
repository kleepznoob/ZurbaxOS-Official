/*
 * Elementary functions.
 *
 * Each function reduces its argument to a small interval where a short
 * polynomial is accurate, then rebuilds the result. Accuracy is close to
 * full double precision for ordinary inputs, which is plenty for graphics,
 * the calculator and unit conversions.
 */
#include "zr.h"

typedef union {
	f64 f;
	u64 u;
} Bits;

f64 fabs(f64 x)
{
	return __builtin_fabs(x);
}

f64 sqrt(f64 x)
{
	return __builtin_sqrt(x);
}

bool is_nan(f64 x)
{
	return x != x;
}

bool is_inf(f64 x)
{
	return x == INF || x == -INF;
}

f64 trunc(f64 x)
{
	if (!(fabs(x) < 4503599627370496.0)) /* >= 2^52, NaN or inf: already integral */
		return x;
	return (f64)(i64)x;
}

f64 floor(f64 x)
{
	f64 t = trunc(x);
	return t > x ? t - 1 : t;
}

f64 ceil(f64 x)
{
	f64 t = trunc(x);
	return t < x ? t + 1 : t;
}

f64 round(f64 x)
{
	return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);
}

f64 fmod(f64 x, f64 y)
{
	if (y == 0 || is_nan(x) || is_nan(y) || is_inf(x))
		return __builtin_nan("");
	f64 ay = fabs(y), r = fabs(x);
	/* Subtract the largest power-of-two multiple of y that fits, repeatedly:
	 * exact, unlike x - trunc(x/y)*y. */
	while (r >= ay) {
		f64 m = ay;
		while (m * 2 <= r)
			m *= 2;
		r -= m;
	}
	return x < 0 ? -r : r;
}

f64 ldexp(f64 x, int e)
{
	while (e > 1023) {
		x *= 8.98846567431158e307; /* 2^1023 */
		e -= 1023;
	}
	while (e < -1022) {
		x *= 2.2250738585072014e-308; /* 2^-1022 */
		e += 1022;
	}
	Bits b = { .u = (u64)(e + 1023) << 52 };
	return x * b.f;
}

f64 frexp(f64 x, int *e)
{
	Bits b = { x };
	int ex = (int)((b.u >> 52) & 0x7ff);
	if (ex == 0) {
		if (x == 0) {
			*e = 0;
			return x;
		}
		x *= 18014398509481984.0; /* 2^54: normalise subnormals */
		b.f = x;
		ex = (int)((b.u >> 52) & 0x7ff) - 54;
	} else if (ex == 0x7ff) {
		*e = 0;
		return x;
	}
	*e = ex - 1022;
	b.u = (b.u & ~(0x7ffull << 52)) | (1022ull << 52);
	return b.f;
}

/* ---- exp / log ----------------------------------------------------------------- */

#define LN2_HI 6.93147180369123816490e-01
#define LN2_LO 1.90821492927058770002e-10
#define INV_LN2 1.44269504088896338700e+00

f64 exp(f64 x)
{
	if (is_nan(x))
		return x;
	if (x > 709.78)
		return INF;
	if (x < -745.2)
		return 0;
	f64 k = round(x * INV_LN2);
	f64 r = (x - k * LN2_HI) - k * LN2_LO; /* |r| <= ln2/2 */
	/* Taylor series to r^13: error < 1e-17 on this interval */
	f64 sum = 1, term = 1;
	for (int i = 1; i <= 13; i++) {
		term *= r / i;
		sum += term;
	}
	return ldexp(sum, (int)k);
}

f64 log(f64 x)
{
	if (x < 0 || is_nan(x))
		return __builtin_nan("");
	if (x == 0)
		return -INF;
	if (is_inf(x))
		return x;
	int e;
	f64 m = frexp(x, &e); /* m in [0.5, 1) */
	if (m < 0.70710678118654752440) {
		m *= 2;
		e--;
	}
	/* log(m) = 2 atanh(s), s = (m-1)/(m+1), |s| <= 0.172 */
	f64 s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
	for (int i = 1; i <= 27; i += 2) {
		sum += term / i;
		term *= s2;
	}
	return 2 * sum + e * LN2_HI + e * LN2_LO;
}

f64 log2(f64 x)
{
	return log(x) * 1.44269504088896340736;
}

f64 log10(f64 x)
{
	return log(x) * 0.43429448190325182765;
}

f64 pow(f64 x, f64 y)
{
	if (y == 0)
		return 1;
	if (is_nan(x) || is_nan(y))
		return __builtin_nan("");
	bool y_int = trunc(y) == y && fabs(y) < 9007199254740992.0;
	if (y_int && fabs(y) <= 64) {
		/* Exact repeated squaring for small integer powers. */
		i64 n = (i64)fabs(y);
		f64 r = 1, b = x;
		while (n) {
			if (n & 1)
				r *= b;
			b *= b;
			n >>= 1;
		}
		return y < 0 ? 1 / r : r;
	}
	if (x == 0)
		return y > 0 ? 0 : INF;
	if (x < 0) {
		if (!y_int)
			return __builtin_nan("");
		f64 r = exp(y * log(-x));
		return fmod(y, 2) != 0 ? -r : r;
	}
	return exp(y * log(x));
}

f64 cbrt(f64 x)
{
	if (x == 0 || is_nan(x) || is_inf(x))
		return x;
	f64 a = fabs(x), r = exp(log(a) / 3);
	r = r - (r * r * r - a) / (3 * r * r); /* one Newton step polishes the last bits */
	return x < 0 ? -r : r;
}

f64 hypot(f64 x, f64 y)
{
	x = fabs(x), y = fabs(y);
	f64 m = MAX(x, y);
	if (m == 0 || is_inf(m))
		return m;
	x /= m, y /= m;
	return m * sqrt(x * x + y * y);
}

/* ---- Trigonometry ----------------------------------------------------------------- */

/* pi/2 split into three parts for accurate argument reduction */
#define PIO2_1 1.57079632673412561417e+00
#define PIO2_2 6.07710050650619224932e-11
#define PIO2_3 2.02226624879595063154e-21
#define INV_PIO2 6.36619772367581382433e-01

static f64 sin_poly(f64 x) /* |x| <= pi/4 */
{
	f64 x2 = x * x, term = x, sum = x;
	for (int i = 1; i <= 9; i++) {
		term *= -x2 / ((2 * i) * (2 * i + 1));
		sum += term;
	}
	return sum;
}

static f64 cos_poly(f64 x)
{
	f64 x2 = x * x, term = 1, sum = 1;
	for (int i = 1; i <= 9; i++) {
		term *= -x2 / ((2 * i - 1) * (2 * i));
		sum += term;
	}
	return sum;
}

static f64 reduce(f64 x, int *quadrant)
{
	f64 k = round(x * INV_PIO2);
	*quadrant = (int)((i64)k & 3);
	return ((x - k * PIO2_1) - k * PIO2_2) - k * PIO2_3;
}

f64 sin(f64 x)
{
	if (is_nan(x) || is_inf(x))
		return __builtin_nan("");
	int q;
	f64 r = reduce(x, &q);
	switch (q) {
	case 0: return sin_poly(r);
	case 1: return cos_poly(r);
	case 2: return -sin_poly(r);
	default: return -cos_poly(r);
	}
}

f64 cos(f64 x)
{
	if (is_nan(x) || is_inf(x))
		return __builtin_nan("");
	int q;
	f64 r = reduce(x, &q);
	switch (q) {
	case 0: return cos_poly(r);
	case 1: return -sin_poly(r);
	case 2: return -cos_poly(r);
	default: return sin_poly(r);
	}
}

f64 tan(f64 x)
{
	return sin(x) / cos(x);
}

f64 atan(f64 x)
{
	if (is_nan(x))
		return x;
	bool neg = x < 0;
	if (neg)
		x = -x;
	bool inv = x > 1;
	if (inv)
		x = 1 / x;
	/* Halve the angle twice: atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) */
	x = x / (1 + sqrt(1 + x * x));
	x = x / (1 + sqrt(1 + x * x));
	f64 x2 = x * x, term = x, sum = 0; /* |x| <= 0.199 now */
	for (int i = 1; i <= 41; i += 2) {
		sum += term / i;
		term *= -x2;
	}
	sum *= 4;
	if (inv)
		sum = PI / 2 - sum;
	return neg ? -sum : sum;
}

f64 atan2(f64 y, f64 x)
{
	if (is_nan(x) || is_nan(y))
		return __builtin_nan("");
	if (x == 0) {
		if (y == 0)
			return 0;
		return y > 0 ? PI / 2 : -PI / 2;
	}
	f64 a = atan(y / x);
	if (x > 0)
		return a;
	return y >= 0 ? a + PI : a - PI;
}

f64 asin(f64 x)
{
	if (x < -1 || x > 1)
		return __builtin_nan("");
	return atan2(x, sqrt((1 - x) * (1 + x)));
}

f64 acos(f64 x)
{
	if (x < -1 || x > 1)
		return __builtin_nan("");
	return atan2(sqrt((1 - x) * (1 + x)), x);
}

/* ---- Single precision ------------------------------------------------------------- */

f32 floorf(f32 x) { return (f32)floor(x); }
f32 ceilf(f32 x) { return (f32)ceil(x); }
f32 roundf(f32 x) { return (f32)round(x); }
f32 fmodf(f32 x, f32 y) { return (f32)fmod(x, y); }
f32 sinf(f32 x) { return (f32)sin(x); }
f32 cosf(f32 x) { return (f32)cos(x); }
f32 tanf(f32 x) { return (f32)tan(x); }
f32 atan2f(f32 y, f32 x) { return (f32)atan2(y, x); }
f32 expf(f32 x) { return (f32)exp(x); }
f32 logf(f32 x) { return (f32)log(x); }
f32 powf(f32 x, f32 y) { return (f32)pow(x, y); }

/* ---- Animation helpers ----------------------------------------------------------- */

f32 ease_out_cubic(f32 t)
{
	t = clampf(t, 0, 1) - 1;
	return t * t * t + 1;
}

f32 ease_in_out_cubic(f32 t)
{
	t = clampf(t, 0, 1);
	if (t < 0.5f)
		return 4 * t * t * t;
	t = 2 * t - 2;
	return 0.5f * t * t * t + 1;
}

f32 ease_out_back(f32 t)
{
	const f32 c1 = 1.4f, c3 = c1 + 1;
	t = clampf(t, 0, 1) - 1;
	return 1 + c3 * t * t * t + c1 * t * t;
}

void spring_step(f32 *pos, f32 *vel, f32 target, f32 stiffness, f32 dt)
{
	/* Semi-implicit Euler on a critically damped spring, sub-stepped for stability. */
	f32 damping = 2 * sqrtf(stiffness);
	int steps = (int)(dt / 0.004f) + 1;
	f32 h = dt / (f32)steps;
	for (int i = 0; i < steps; i++) {
		f32 force = stiffness * (target - *pos) - damping * *vel;
		*vel += force * h;
		*pos += *vel * h;
	}
}
