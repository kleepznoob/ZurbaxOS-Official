/* X25519 key agreement (RFC 7748): a Montgomery ladder over GF(2^255 - 19)
 * with five 51-bit limbs. Runs in constant time. */
#include "zn.h"

typedef unsigned __int128 u128;
typedef u64 fe[5];

#define MASK51 0x7ffffffffffffull

static u64 le64(const u8 *p)
{
	u64 v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static void fe_frombytes(fe h, const u8 s[32])
{
	h[0] = le64(s) & MASK51;
	h[1] = (le64(s + 6) >> 3) & MASK51;
	h[2] = (le64(s + 12) >> 6) & MASK51;
	h[3] = (le64(s + 19) >> 1) & MASK51;
	h[4] = (le64(s + 24) >> 12) & MASK51;
}

static void fe_carry(fe h)
{
	u64 c;
	c = h[0] >> 51, h[0] &= MASK51, h[1] += c;
	c = h[1] >> 51, h[1] &= MASK51, h[2] += c;
	c = h[2] >> 51, h[2] &= MASK51, h[3] += c;
	c = h[3] >> 51, h[3] &= MASK51, h[4] += c;
	c = h[4] >> 51, h[4] &= MASK51, h[0] += c * 19;
}

static void fe_tobytes(u8 s[32], const fe in)
{
	fe t;
	memcpy(t, in, sizeof t);
	fe_carry(t);
	fe_carry(t);
	/* Subtract p if t >= p: add 19 and see whether it carries out of bit 255. */
	u64 q = (t[0] + 19) >> 51;
	q = (t[1] + q) >> 51;
	q = (t[2] + q) >> 51;
	q = (t[3] + q) >> 51;
	q = (t[4] + q) >> 51;
	t[0] += 19 * q;
	u64 c;
	c = t[0] >> 51, t[0] &= MASK51, t[1] += c;
	c = t[1] >> 51, t[1] &= MASK51, t[2] += c;
	c = t[2] >> 51, t[2] &= MASK51, t[3] += c;
	c = t[3] >> 51, t[3] &= MASK51, t[4] += c;
	t[4] &= MASK51;
	u64 w[4] = { t[0] | t[1] << 51, t[1] >> 13 | t[2] << 38, t[2] >> 26 | t[3] << 25, t[3] >> 39 | t[4] << 12 };
	for (int i = 0; i < 4; i++)
		for (int k = 0; k < 8; k++)
			s[i * 8 + k] = (u8)(w[i] >> (8 * k));
}

static void fe_add(fe r, const fe a, const fe b)
{
	for (int i = 0; i < 5; i++)
		r[i] = a[i] + b[i];
}

static void fe_sub(fe r, const fe a, const fe b)
{
	/* Add 2p so nothing goes negative. */
	r[0] = a[0] + 0xfffffffffffdaull - b[0];
	r[1] = a[1] + 0xffffffffffffeull - b[1];
	r[2] = a[2] + 0xffffffffffffeull - b[2];
	r[3] = a[3] + 0xffffffffffffeull - b[3];
	r[4] = a[4] + 0xffffffffffffeull - b[4];
	fe_carry(r);
}

static void fe_mul(fe r, const fe a, const fe b)
{
	u64 b1 = b[1] * 19, b2 = b[2] * 19, b3 = b[3] * 19, b4 = b[4] * 19;
	u128 t0 = (u128)a[0] * b[0] + (u128)a[1] * b4 + (u128)a[2] * b3 + (u128)a[3] * b2 + (u128)a[4] * b1;
	u128 t1 = (u128)a[0] * b[1] + (u128)a[1] * b[0] + (u128)a[2] * b4 + (u128)a[3] * b3 + (u128)a[4] * b2;
	u128 t2 = (u128)a[0] * b[2] + (u128)a[1] * b[1] + (u128)a[2] * b[0] + (u128)a[3] * b4 + (u128)a[4] * b3;
	u128 t3 = (u128)a[0] * b[3] + (u128)a[1] * b[2] + (u128)a[2] * b[1] + (u128)a[3] * b[0] + (u128)a[4] * b4;
	u128 t4 = (u128)a[0] * b[4] + (u128)a[1] * b[3] + (u128)a[2] * b[2] + (u128)a[3] * b[1] + (u128)a[4] * b[0];
	u64 c;
	r[0] = (u64)t0 & MASK51, c = (u64)(t0 >> 51);
	t1 += c, r[1] = (u64)t1 & MASK51, c = (u64)(t1 >> 51);
	t2 += c, r[2] = (u64)t2 & MASK51, c = (u64)(t2 >> 51);
	t3 += c, r[3] = (u64)t3 & MASK51, c = (u64)(t3 >> 51);
	t4 += c, r[4] = (u64)t4 & MASK51, c = (u64)(t4 >> 51);
	r[0] += c * 19;
	c = r[0] >> 51, r[0] &= MASK51, r[1] += c;
}

static void fe_sq(fe r, const fe a)
{
	fe_mul(r, a, a);
}

static void fe_mul_small(fe r, const fe a, u64 k)
{
	u128 c = 0;
	for (int i = 0; i < 5; i++) {
		c += (u128)a[i] * k;
		r[i] = (u64)c & MASK51;
		c >>= 51;
	}
	r[0] += (u64)c * 19;
	u64 cc = r[0] >> 51;
	r[0] &= MASK51, r[1] += cc;
}

static void fe_cswap(fe a, fe b, u64 swap)
{
	u64 m = 0 - swap;
	for (int i = 0; i < 5; i++) {
		u64 t = m & (a[i] ^ b[i]);
		a[i] ^= t, b[i] ^= t;
	}
}

/* z^(p-2), the inverse, by the usual addition chain. */
static void fe_invert(fe out, const fe z)
{
	fe z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;
	fe_sq(z2, z);
	fe_sq(t, z2);
	fe_sq(t, t);
	fe_mul(z9, t, z);
	fe_mul(z11, z9, z2);
	fe_sq(t, z11);
	fe_mul(z2_5_0, t, z9);
	fe_sq(t, z2_5_0);
	for (int i = 1; i < 5; i++)
		fe_sq(t, t);
	fe_mul(z2_10_0, t, z2_5_0);
	fe_sq(t, z2_10_0);
	for (int i = 1; i < 10; i++)
		fe_sq(t, t);
	fe_mul(z2_20_0, t, z2_10_0);
	fe_sq(t, z2_20_0);
	for (int i = 1; i < 20; i++)
		fe_sq(t, t);
	fe_mul(t, t, z2_20_0);
	fe_sq(t, t);
	for (int i = 1; i < 10; i++)
		fe_sq(t, t);
	fe_mul(z2_50_0, t, z2_10_0);
	fe_sq(t, z2_50_0);
	for (int i = 1; i < 50; i++)
		fe_sq(t, t);
	fe_mul(z2_100_0, t, z2_50_0);
	fe_sq(t, z2_100_0);
	for (int i = 1; i < 100; i++)
		fe_sq(t, t);
	fe_mul(t, t, z2_100_0);
	fe_sq(t, t);
	for (int i = 1; i < 50; i++)
		fe_sq(t, t);
	fe_mul(t, t, z2_50_0);
	for (int i = 0; i < 5; i++)
		fe_sq(t, t);
	fe_mul(out, t, z11);
}

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32])
{
	u8 k[32];
	memcpy(k, scalar, 32);
	k[0] &= 248;
	k[31] &= 127;
	k[31] |= 64;
	u8 u[32];
	memcpy(u, point, 32);
	u[31] &= 127;
	fe x1, x2 = { 1 }, z2 = { 0 }, x3, z3 = { 1 }, a, aa, b, bb, e, c, d, da, cb, t;
	fe_frombytes(x1, u);
	memcpy(x3, x1, sizeof x3);
	u64 swap = 0;
	for (int i = 254; i >= 0; i--) {
		u64 bit = (k[i / 8] >> (i % 8)) & 1;
		swap ^= bit;
		fe_cswap(x2, x3, swap);
		fe_cswap(z2, z3, swap);
		swap = bit;
		fe_add(a, x2, z2);
		fe_sq(aa, a);
		fe_sub(b, x2, z2);
		fe_sq(bb, b);
		fe_sub(e, aa, bb);
		fe_add(c, x3, z3);
		fe_sub(d, x3, z3);
		fe_mul(da, d, a);
		fe_mul(cb, c, b);
		fe_add(t, da, cb);
		fe_sq(x3, t);
		fe_sub(t, da, cb);
		fe_sq(t, t);
		fe_mul(z3, x1, t);
		fe_mul(x2, aa, bb);
		fe_mul_small(t, e, 121665);
		fe_add(t, aa, t);
		fe_mul(z2, e, t);
	}
	fe_cswap(x2, x3, swap);
	fe_cswap(z2, z3, swap);
	fe_invert(z2, z2);
	fe_mul(x2, x2, z2);
	fe_tobytes(out, x2);
}

void x25519_base(u8 out[32], const u8 scalar[32])
{
	static const u8 nine[32] = { 9 };
	x25519(out, scalar, nine);
}
