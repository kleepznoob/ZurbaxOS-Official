/*
 * Signature verification: RSA (PKCS #1 v1.5 and PSS) and ECDSA on P-256 and
 * P-384. Only public-key operations, so variable time is fine. Everything
 * uses Montgomery multiplication on 32-bit limbs; no division is needed.
 */
#include "internal.h"

#define LIMBS 264 /* up to 8448-bit numbers */

typedef struct {
	u32 m[LIMBS];
	int n;          /* limbs in use */
	u32 inv;        /* -m^-1 mod 2^32 */
	u32 rr[LIMBS];  /* R^2 mod m */
	u32 one[LIMBS]; /* R mod m: 1 in Montgomery form */
} Mod;

static void bn_from_bytes(u32 *r, int n, const u8 *b, usize len)
{
	memset(r, 0, sizeof(u32) * (usize)n);
	for (usize i = 0; i < len && i / 4 < (usize)n; i++)
		r[i / 4] |= (u32)b[len - 1 - i] << (8 * (i % 4));
}

static void bn_to_bytes(const u32 *a, int n, u8 *b, usize len)
{
	for (usize i = 0; i < len; i++)
		b[len - 1 - i] = i / 4 < (usize)n ? (u8)(a[i / 4] >> (8 * (i % 4))) : 0;
}

static int bn_cmp(const u32 *a, const u32 *b, int n)
{
	for (int i = n - 1; i >= 0; i--)
		if (a[i] != b[i])
			return a[i] < b[i] ? -1 : 1;
	return 0;
}

static bool bn_is_zero(const u32 *a, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i])
			return false;
	return true;
}

static u32 bn_sub(u32 *r, const u32 *a, const u32 *b, int n)
{
	u64 borrow = 0;
	for (int i = 0; i < n; i++) {
		u64 d = (u64)a[i] - b[i] - borrow;
		r[i] = (u32)d;
		borrow = (d >> 32) & 1;
	}
	return (u32)borrow;
}

static u32 bn_add(u32 *r, const u32 *a, const u32 *b, int n)
{
	u64 c = 0;
	for (int i = 0; i < n; i++) {
		c += (u64)a[i] + b[i];
		r[i] = (u32)c;
		c >>= 32;
	}
	return (u32)c;
}

static void mod_mul(const Mod *md, u32 *r, const u32 *a, const u32 *b)
{
	int n = md->n;
	u32 t[LIMBS + 2] = { 0 };
	for (int i = 0; i < n; i++) {
		u64 c = 0;
		for (int j = 0; j < n; j++) {
			c += (u64)t[j] + (u64)a[j] * b[i];
			t[j] = (u32)c;
			c >>= 32;
		}
		c += t[n];
		t[n] = (u32)c;
		t[n + 1] = (u32)(c >> 32);
		u32 m = t[0] * md->inv;
		c = ((u64)t[0] + (u64)m * md->m[0]) >> 32;
		for (int j = 1; j < n; j++) {
			c += (u64)t[j] + (u64)m * md->m[j];
			t[j - 1] = (u32)c;
			c >>= 32;
		}
		c += t[n];
		t[n - 1] = (u32)c;
		t[n] = t[n + 1] + (u32)(c >> 32);
	}
	if (t[n] || bn_cmp(t, md->m, n) >= 0)
		bn_sub(t, t, md->m, n);
	memcpy(r, t, sizeof(u32) * (usize)n);
}

static bool mod_init(Mod *md, const u8 *m, usize len)
{
	while (len && !*m)
		m++, len--;
	if (!len || len > LIMBS * 4 || !(m[len - 1] & 1))
		return false;
	md->n = (int)((len + 3) / 4);
	bn_from_bytes(md->m, md->n, m, len);
	/* -m^-1 mod 2^32 by Newton's iteration. */
	u32 x = md->m[0];
	for (int i = 0; i < 5; i++)
		x *= 2 - md->m[0] * x;
	md->inv = 0 - x;
	/* R^2 mod m by doubling 1 2*32*n times. */
	u32 *r = md->rr;
	memset(r, 0, sizeof(u32) * (usize)md->n);
	r[0] = 1;
	for (int i = 0; i < 64 * md->n; i++) {
		u32 top = r[md->n - 1] >> 31;
		for (int k = md->n - 1; k > 0; k--)
			r[k] = r[k] << 1 | r[k - 1] >> 31;
		r[0] <<= 1;
		if (top || bn_cmp(r, md->m, md->n) >= 0)
			bn_sub(r, r, md->m, md->n);
		if (i == 32 * md->n - 1)
			memcpy(md->one, r, sizeof(u32) * (usize)md->n);
	}
	return true;
}

static void mod_to(const Mod *md, u32 *r, const u32 *a)
{
	mod_mul(md, r, a, md->rr);
}

static void mod_from(const Mod *md, u32 *r, const u32 *a)
{
	u32 one[LIMBS] = { 1 };
	mod_mul(md, r, a, one);
}

/* r = a^e for a in Montgomery form; e as big-endian bytes. */
static void mod_pow(const Mod *md, u32 *r, const u32 *a, const u8 *e, usize elen)
{
	u32 acc[LIMBS];
	memcpy(acc, md->one, sizeof(u32) * (usize)md->n);
	for (usize i = 0; i < elen; i++)
		for (int b = 7; b >= 0; b--) {
			mod_mul(md, acc, acc, acc);
			if ((e[i] >> b) & 1)
				mod_mul(md, acc, acc, a);
		}
	memcpy(r, acc, sizeof(u32) * (usize)md->n);
}

static void mod_add(const Mod *md, u32 *r, const u32 *a, const u32 *b)
{
	u32 c = bn_add(r, a, b, md->n);
	if (c || bn_cmp(r, md->m, md->n) >= 0)
		bn_sub(r, r, md->m, md->n);
}

static void mod_sub(const Mod *md, u32 *r, const u32 *a, const u32 *b)
{
	if (bn_sub(r, a, b, md->n))
		bn_add(r, r, md->m, md->n);
}

/* ---- RSA ------------------------------------------------------------------------------------ */

/* m = sig^e mod n, as big-endian bytes the size of n. */
static u8 *rsa_public(const u8 *n, usize nlen, const u8 *e, usize elen, const u8 *sig, usize slen, usize *klen)
{
	static Mod md; /* large; verification is not reentrant per thread anyway */
	Mod *m = mem_alloc(sizeof *m);
	UNUSED(md);
	if (!mod_init(m, n, nlen)) {
		mem_free(m);
		return NULL;
	}
	while (nlen && !*n)
		n++, nlen--;
	if (slen > nlen + 1 || nlen < 128) { /* below 1024 bits isn't trusted */
		mem_free(m);
		return NULL;
	}
	u32 s[LIMBS], sm[LIMBS], r[LIMBS];
	bn_from_bytes(s, m->n, sig, slen);
	if (bn_cmp(s, m->m, m->n) >= 0) {
		mem_free(m);
		return NULL;
	}
	mod_to(m, sm, s);
	mod_pow(m, r, sm, e, elen);
	mod_from(m, r, r);
	u8 *out = mem_alloc(nlen);
	bn_to_bytes(r, m->n, out, nlen);
	*klen = nlen;
	mem_free(m);
	return out;
}

static const u8 *digest_info(int hash, usize *len)
{
	static const u8 sha1[] = { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14 };
	static const u8 sha256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
	static const u8 sha384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
	static const u8 sha512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
	switch (hash) {
	case HASH_SHA1: *len = sizeof sha1; return sha1;
	case HASH_SHA256: *len = sizeof sha256; return sha256;
	case HASH_SHA384: *len = sizeof sha384; return sha384;
	case HASH_SHA512: *len = sizeof sha512; return sha512;
	}
	return NULL;
}

bool rsa_verify_pkcs1(const u8 *n, usize nlen, const u8 *e, usize elen, int hash, const u8 *digest, const u8 *sig, usize slen)
{
	usize k, ilen, hlen = hash_len(hash);
	const u8 *info = digest_info(hash, &ilen);
	if (!info)
		return false;
	u8 *em = rsa_public(n, nlen, e, elen, sig, slen, &k);
	if (!em)
		return false;
	/* 00 01 FF..FF 00 DigestInfo digest */
	bool ok = k >= ilen + hlen + 11 && em[0] == 0 && em[1] == 1;
	usize ps = k - ilen - hlen - 3;
	for (usize i = 2; ok && i < 2 + ps; i++)
		ok = em[i] == 0xff;
	ok = ok && em[2 + ps] == 0 && memcmp(em + 3 + ps, info, ilen) == 0 && memcmp(em + 3 + ps + ilen, digest, hlen) == 0;
	mem_free(em);
	return ok;
}

static void mgf1(int hash, const u8 *seed, usize slen, u8 *mask, usize mlen)
{
	usize hl = hash_len(hash);
	u8 block[HASH_MAX];
	for (u32 counter = 0; mlen; counter++) {
		Hash h;
		hash_init(&h, hash);
		hash_update(&h, seed, slen);
		u8 c[4] = { (u8)(counter >> 24), (u8)(counter >> 16), (u8)(counter >> 8), (u8)counter };
		hash_update(&h, c, 4);
		hash_final(&h, block);
		usize k = MIN(mlen, hl);
		for (usize i = 0; i < k; i++)
			mask[i] ^= block[i];
		mask += k, mlen -= k;
	}
}

bool rsa_verify_pss(const u8 *n, usize nlen, const u8 *e, usize elen, int hash, const u8 *digest, const u8 *sig, usize slen)
{
	usize k, hl = hash_len(hash);
	u8 *m = rsa_public(n, nlen, e, elen, sig, slen, &k);
	if (!m)
		return false;
	/* modBits from the modulus' leading byte. */
	const u8 *nn = n;
	while (*nn == 0)
		nn++;
	int top = 0;
	for (int b = 7; b >= 0; b--)
		if ((nn[0] >> b) & 1) {
			top = b + 1;
			break;
		}
	usize mod_bits = (k - 1) * 8 + (usize)top, em_bits = mod_bits - 1, em_len = (em_bits + 7) / 8;
	const u8 *em = m + (k - em_len);
	bool ok = (k == em_len || m[0] == 0) && em_len >= hl + 2 && em[em_len - 1] == 0xbc;
	if (ok) {
		usize db_len = em_len - hl - 1;
		u8 *db = mem_dup(em, db_len);
		const u8 *h = em + db_len;
		u8 top_mask = (u8)(0xff >> (8 * em_len - em_bits));
		ok = (db[0] & ~top_mask) == 0;
		mgf1(hash, h, hl, db, db_len);
		db[0] &= top_mask;
		/* DB = zeros || 01 || salt */
		usize i = 0;
		while (i < db_len && db[i] == 0)
			i++;
		ok = ok && i < db_len && db[i] == 1;
		if (ok) {
			const u8 *salt = db + i + 1;
			usize salt_len = db_len - i - 1;
			Hash hh;
			u8 zeros[8] = { 0 }, check[HASH_MAX];
			hash_init(&hh, hash);
			hash_update(&hh, zeros, 8);
			hash_update(&hh, digest, hl);
			hash_update(&hh, salt, salt_len);
			hash_final(&hh, check);
			ok = memcmp(check, h, hl) == 0;
		}
		mem_free(db);
	}
	mem_free(m);
	return ok;
}

/* ---- ECDSA ----------------------------------------------------------------------------------- */

typedef struct {
	int bytes;
	const char *p, *b, *gx, *gy, *n;
} CurveDef;

static const CurveDef CURVES[] = {
	[CURVE_P256] = { 32, "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
			 "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
			 "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
			 "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
			 "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551" },
	[CURVE_P384] = { 48,
			 "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
			 "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
			 "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
			 "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",
			 "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973" },
};

#define ELIMBS 12

typedef struct {
	u32 x[ELIMBS], y[ELIMBS], z[ELIMBS]; /* Jacobian, Montgomery form; z = 0 is infinity */
} Point;

typedef struct {
	Mod p, n;
	u32 b[ELIMBS], three[ELIMBS];
	Point g;
	int bytes;
} Curve;

static void hex_limbs(u32 *r, int n, const char *hex)
{
	u8 b[48];
	usize len = strlen(hex) / 2;
	hex_decode(hex, b, len);
	bn_from_bytes(r, n, b, len);
}

static bool curve_load(Curve *c, int id)
{
	const CurveDef *d = &CURVES[id];
	u8 b[48];
	c->bytes = d->bytes;
	hex_decode(d->p, b, (usize)d->bytes);
	mod_init(&c->p, b, (usize)d->bytes);
	hex_decode(d->n, b, (usize)d->bytes);
	mod_init(&c->n, b, (usize)d->bytes);
	int n = c->p.n;
	u32 t[ELIMBS];
	hex_limbs(t, n, d->b);
	mod_to(&c->p, c->b, t);
	u32 three[ELIMBS] = { 3 };
	mod_to(&c->p, c->three, three);
	hex_limbs(t, n, d->gx);
	mod_to(&c->p, c->g.x, t);
	hex_limbs(t, n, d->gy);
	mod_to(&c->p, c->g.y, t);
	memcpy(c->g.z, c->p.one, sizeof(u32) * (usize)n);
	return true;
}

static bool on_curve(const Curve *c, const u32 *x, const u32 *y)
{
	/* y^2 = x^3 - 3x + b */
	const Mod *p = &c->p;
	u32 l[ELIMBS], r[ELIMBS], t[ELIMBS];
	mod_mul(p, l, y, y);
	mod_mul(p, r, x, x);
	mod_mul(p, r, r, x);
	mod_mul(p, t, c->three, x);
	mod_sub(p, r, r, t);
	mod_add(p, r, r, c->b);
	return bn_cmp(l, r, p->n) == 0;
}

static void point_double(const Curve *c, Point *r, const Point *a)
{
	const Mod *p = &c->p;
	int n = p->n;
	if (bn_is_zero(a->z, n)) {
		*r = *a;
		return;
	}
	u32 delta[ELIMBS], gamma[ELIMBS], beta[ELIMBS], alpha[ELIMBS], t1[ELIMBS], t2[ELIMBS];
	mod_mul(p, delta, a->z, a->z);
	mod_mul(p, gamma, a->y, a->y);
	mod_mul(p, beta, a->x, gamma);
	mod_sub(p, t1, a->x, delta);
	mod_add(p, t2, a->x, delta);
	mod_mul(p, alpha, t1, t2);
	mod_mul(p, alpha, alpha, c->three);
	Point o;
	/* X3 = alpha^2 - 8 beta */
	mod_mul(p, o.x, alpha, alpha);
	mod_add(p, t1, beta, beta);
	mod_add(p, t1, t1, t1);
	mod_add(p, t2, t1, t1);
	mod_sub(p, o.x, o.x, t2);
	/* Z3 = (Y + Z)^2 - gamma - delta */
	mod_add(p, o.z, a->y, a->z);
	mod_mul(p, o.z, o.z, o.z);
	mod_sub(p, o.z, o.z, gamma);
	mod_sub(p, o.z, o.z, delta);
	/* Y3 = alpha (4 beta - X3) - 8 gamma^2 */
	mod_sub(p, t1, t1, o.x);
	mod_mul(p, o.y, alpha, t1);
	mod_mul(p, t2, gamma, gamma);
	mod_add(p, t2, t2, t2);
	mod_add(p, t2, t2, t2);
	mod_add(p, t2, t2, t2);
	mod_sub(p, o.y, o.y, t2);
	*r = o;
}

static void point_add(const Curve *c, Point *r, const Point *a, const Point *b)
{
	const Mod *p = &c->p;
	int n = p->n;
	if (bn_is_zero(a->z, n)) {
		*r = *b;
		return;
	}
	if (bn_is_zero(b->z, n)) {
		*r = *a;
		return;
	}
	u32 z1z1[ELIMBS], z2z2[ELIMBS], u1[ELIMBS], u2[ELIMBS], s1[ELIMBS], s2[ELIMBS], h[ELIMBS], i[ELIMBS], j[ELIMBS], rr[ELIMBS],
		v[ELIMBS], t[ELIMBS];
	mod_mul(p, z1z1, a->z, a->z);
	mod_mul(p, z2z2, b->z, b->z);
	mod_mul(p, u1, a->x, z2z2);
	mod_mul(p, u2, b->x, z1z1);
	mod_mul(p, s1, a->y, b->z);
	mod_mul(p, s1, s1, z2z2);
	mod_mul(p, s2, b->y, a->z);
	mod_mul(p, s2, s2, z1z1);
	mod_sub(p, h, u2, u1);
	mod_sub(p, rr, s2, s1);
	if (bn_is_zero(h, n)) {
		if (bn_is_zero(rr, n)) {
			point_double(c, r, a);
		} else {
			memset(r, 0, sizeof *r);
		}
		return;
	}
	mod_add(p, rr, rr, rr);
	mod_add(p, i, h, h);
	mod_mul(p, i, i, i);
	mod_mul(p, j, h, i);
	mod_mul(p, v, u1, i);
	Point o;
	mod_mul(p, o.x, rr, rr);
	mod_sub(p, o.x, o.x, j);
	mod_sub(p, o.x, o.x, v);
	mod_sub(p, o.x, o.x, v);
	mod_sub(p, t, v, o.x);
	mod_mul(p, o.y, rr, t);
	mod_mul(p, t, s1, j);
	mod_add(p, t, t, t);
	mod_sub(p, o.y, o.y, t);
	mod_add(p, o.z, a->z, b->z);
	mod_mul(p, o.z, o.z, o.z);
	mod_sub(p, o.z, o.z, z1z1);
	mod_sub(p, o.z, o.z, z2z2);
	mod_mul(p, o.z, o.z, h);
	*r = o;
}

static bool scalar_bit(const u32 *k, int i)
{
	return (k[i / 32] >> (i % 32)) & 1;
}

bool ecdsa_verify(int curve, const u8 *pub, usize plen, const u8 *digest, usize dlen, const u8 *sig, usize slen)
{
	if (curve != CURVE_P256 && curve != CURVE_P384)
		return false;
	Curve c;
	curve_load(&c, curve);
	int bytes = c.bytes, n = c.p.n;
	if (plen != (usize)(1 + 2 * bytes) || pub[0] != 4)
		return false;
	/* The signature: SEQUENCE { r INTEGER, s INTEGER } */
	Der d = { sig, slen }, seq, rv, sv;
	if (!der_expect(&d, DER_SEQUENCE, &seq) || !der_expect(&seq, DER_INTEGER, &rv) || !der_expect(&seq, DER_INTEGER, &sv))
		return false;
	der_strip(&rv);
	der_strip(&sv);
	if (rv.n > (usize)bytes || sv.n > (usize)bytes)
		return false;
	u32 r[ELIMBS], s[ELIMBS], e[ELIMBS];
	bn_from_bytes(r, n, rv.p, rv.n);
	bn_from_bytes(s, n, sv.p, sv.n);
	if (bn_is_zero(r, n) || bn_is_zero(s, n) || bn_cmp(r, c.n.m, n) >= 0 || bn_cmp(s, c.n.m, n) >= 0)
		return false;
	/* The digest, cut to the curve size. */
	bn_from_bytes(e, n, digest, MIN(dlen, (usize)bytes));
	if (bn_cmp(e, c.n.m, n) >= 0)
		bn_sub(e, e, c.n.m, n);
	/* The public key must be a point on the curve. */
	u32 qx[ELIMBS], qy[ELIMBS];
	bn_from_bytes(qx, n, pub + 1, (usize)bytes);
	bn_from_bytes(qy, n, pub + 1 + bytes, (usize)bytes);
	if (bn_cmp(qx, c.p.m, n) >= 0 || bn_cmp(qy, c.p.m, n) >= 0)
		return false;
	Point q;
	mod_to(&c.p, q.x, qx);
	mod_to(&c.p, q.y, qy);
	memcpy(q.z, c.p.one, sizeof(u32) * (usize)n);
	if (!on_curve(&c, q.x, q.y))
		return false;
	/* w = s^-1, u1 = e w, u2 = r w (mod n) */
	u8 nm2[48];
	u32 two[ELIMBS] = { 2 }, t[ELIMBS];
	bn_sub(t, c.n.m, two, n);
	bn_to_bytes(t, n, nm2, (usize)bytes);
	u32 sm[ELIMBS], w[ELIMBS], u1[ELIMBS], u2[ELIMBS];
	mod_to(&c.n, sm, s);
	mod_pow(&c.n, w, sm, nm2, (usize)bytes);
	mod_to(&c.n, t, e);
	mod_mul(&c.n, u1, t, w);
	mod_from(&c.n, u1, u1);
	mod_to(&c.n, t, r);
	mod_mul(&c.n, u2, t, w);
	mod_from(&c.n, u2, u2);
	/* R = u1 G + u2 Q, both at once. */
	Point gq, acc;
	point_add(&c, &gq, &c.g, &q);
	memset(&acc, 0, sizeof acc);
	for (int i = bytes * 8 - 1; i >= 0; i--) {
		point_double(&c, &acc, &acc);
		bool b1 = scalar_bit(u1, i), b2 = scalar_bit(u2, i);
		if (b1 && b2)
			point_add(&c, &acc, &acc, &gq);
		else if (b1)
			point_add(&c, &acc, &acc, &c.g);
		else if (b2)
			point_add(&c, &acc, &acc, &q);
	}
	if (bn_is_zero(acc.z, n))
		return false;
	/* x = X / Z^2, then compare with r mod n. */
	u8 pm2[48];
	bn_sub(t, c.p.m, two, n);
	bn_to_bytes(t, n, pm2, (usize)bytes);
	u32 zi[ELIMBS], x[ELIMBS];
	mod_mul(&c.p, zi, acc.z, acc.z);
	mod_pow(&c.p, zi, zi, pm2, (usize)bytes);
	mod_mul(&c.p, x, acc.x, zi);
	mod_from(&c.p, x, x);
	if (bn_cmp(x, c.n.m, n) >= 0)
		bn_sub(x, x, c.n.m, n);
	return bn_cmp(x, r, n) == 0;
}
