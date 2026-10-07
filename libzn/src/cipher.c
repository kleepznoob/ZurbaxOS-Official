/* ChaCha20-Poly1305 and AES-GCM, the two AEADs TLS 1.3 uses, and AES key
 * unwrap for Wi-Fi. GHASH and Poly1305 are written to run in constant time. */
#include "zn.h"

typedef unsigned __int128 u128;

static u32 le32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 le64(const u8 *p)
{
	return (u64)le32(p) | (u64)le32(p + 4) << 32;
}

static void put_le32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (u8)(v >> (8 * i));
}

static void put_le64(u8 *p, u64 v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (u8)(v >> (8 * i));
}

static u64 be64(const u8 *p)
{
	u64 v = 0;
	for (int i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}

static void put_be64(u8 *p, u64 v)
{
	for (int i = 7; i >= 0; i--, v >>= 8)
		p[i] = (u8)v;
}

/* ---- ChaCha20 ------------------------------------------------------------------------------ */

#define QR(a, b, c, d) \
	a += b, d ^= a, d = d << 16 | d >> 16, c += d, b ^= c, b = b << 12 | b >> 20, \
	a += b, d ^= a, d = d << 8 | d >> 24, c += d, b ^= c, b = b << 7 | b >> 25

static void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64])
{
	u32 s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };
	for (int i = 0; i < 8; i++)
		s[4 + i] = le32(key + i * 4);
	s[12] = counter;
	for (int i = 0; i < 3; i++)
		s[13 + i] = le32(nonce + i * 4);
	u32 x[16];
	memcpy(x, s, sizeof x);
	for (int i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8], x[12]);
		QR(x[1], x[5], x[9], x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8], x[13]);
		QR(x[3], x[4], x[9], x[14]);
	}
	for (int i = 0; i < 16; i++)
		put_le32(out + i * 4, x[i] + s[i]);
}

static void chacha20_xor(const u8 key[32], u32 counter, const u8 nonce[12], const u8 *in, u8 *out, usize n)
{
	u8 ks[64];
	while (n) {
		chacha20_block(key, counter++, nonce, ks);
		usize k = MIN(n, (usize)64);
		for (usize i = 0; i < k; i++)
			out[i] = in[i] ^ ks[i];
		in += k, out += k, n -= k;
	}
}

/* ---- Poly1305 ------------------------------------------------------------------------------ */

typedef struct {
	u64 r[3], h[3], pad[2];
	u8 buf[16];
	usize fill;
} Poly;

static void poly_init(Poly *p, const u8 key[32])
{
	memset(p, 0, sizeof *p);
	u64 t0 = le64(key), t1 = le64(key + 8);
	p->r[0] = t0 & 0xffc0fffffff;
	p->r[1] = ((t0 >> 44) | (t1 << 20)) & 0xfffffc0ffff;
	p->r[2] = (t1 >> 24) & 0x00ffffffc0f;
	p->pad[0] = le64(key + 16);
	p->pad[1] = le64(key + 24);
}

static void poly_block(Poly *p, const u8 *m, u64 hibit)
{
	u64 r0 = p->r[0], r1 = p->r[1], r2 = p->r[2];
	u64 s1 = r1 * (5 << 2), s2 = r2 * (5 << 2);
	u64 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2];
	u64 t0 = le64(m), t1 = le64(m + 8);
	h0 += t0 & 0xfffffffffff;
	h1 += ((t0 >> 44) | (t1 << 20)) & 0xfffffffffff;
	h2 += ((t1 >> 24) & 0x3ffffffffff) | hibit;
	u128 d0 = (u128)h0 * r0 + (u128)h1 * s2 + (u128)h2 * s1;
	u128 d1 = (u128)h0 * r1 + (u128)h1 * r0 + (u128)h2 * s2;
	u128 d2 = (u128)h0 * r2 + (u128)h1 * r1 + (u128)h2 * r0;
	u64 c = (u64)(d0 >> 44);
	h0 = (u64)d0 & 0xfffffffffff;
	d1 += c;
	c = (u64)(d1 >> 44);
	h1 = (u64)d1 & 0xfffffffffff;
	d2 += c;
	c = (u64)(d2 >> 42);
	h2 = (u64)d2 & 0x3ffffffffff;
	h0 += c * 5;
	c = h0 >> 44;
	h0 &= 0xfffffffffff;
	h1 += c;
	p->h[0] = h0, p->h[1] = h1, p->h[2] = h2;
}

static void poly_update(Poly *p, const u8 *m, usize n)
{
	while (n) {
		if (p->fill == 0 && n >= 16) {
			poly_block(p, m, 1ull << 40);
			m += 16, n -= 16;
			continue;
		}
		usize k = MIN(n, 16 - p->fill);
		memcpy(p->buf + p->fill, m, k);
		p->fill += k, m += k, n -= k;
		if (p->fill == 16) {
			poly_block(p, p->buf, 1ull << 40);
			p->fill = 0;
		}
	}
}

/* Pads to a 16-byte boundary with zeros (the AEAD's layout). */
static void poly_pad(Poly *p)
{
	if (p->fill) {
		memset(p->buf + p->fill, 0, 16 - p->fill);
		poly_block(p, p->buf, 1ull << 40);
		p->fill = 0;
	}
}

static void poly_finish(Poly *p, u8 mac[16])
{
	if (p->fill) {
		p->buf[p->fill] = 1;
		memset(p->buf + p->fill + 1, 0, 15 - p->fill);
		poly_block(p, p->buf, 0);
	}
	const u64 m44 = 0xfffffffffff, m42 = 0x3ffffffffff;
	u64 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], c;
	c = h1 >> 44, h1 &= m44;
	h2 += c, c = h2 >> 42, h2 &= m42;
	h0 += c * 5, c = h0 >> 44, h0 &= m44;
	h1 += c, c = h1 >> 44, h1 &= m44;
	h2 += c, c = h2 >> 42, h2 &= m42;
	h0 += c * 5, c = h0 >> 44, h0 &= m44;
	h1 += c;
	u64 g0 = h0 + 5;
	c = g0 >> 44, g0 &= m44;
	u64 g1 = h1 + c;
	c = g1 >> 44, g1 &= m44;
	u64 g2 = h2 + c - (1ull << 42);
	c = (g2 >> 63) - 1;
	g0 &= c, g1 &= c, g2 &= c;
	c = ~c;
	h0 = (h0 & c) | g0, h1 = (h1 & c) | g1, h2 = (h2 & c) | g2;
	u64 t0 = p->pad[0], t1 = p->pad[1];
	h0 += t0 & m44, c = h0 >> 44, h0 &= m44;
	h1 += (((t0 >> 44) | (t1 << 20)) & m44) + c, c = h1 >> 44, h1 &= m44;
	h2 += ((t1 >> 24) & m42) + c, h2 &= m42;
	put_le64(mac, h0 | (h1 << 44));
	put_le64(mac + 8, (h1 >> 20) | (h2 << 24));
}

static void aead_tag(const u8 key[32], const u8 nonce[12], const u8 *aad, usize alen, const u8 *ct, usize n, u8 tag[16])
{
	u8 block[64];
	chacha20_block(key, 0, nonce, block);
	Poly p;
	poly_init(&p, block);
	poly_update(&p, aad, alen);
	poly_pad(&p);
	poly_update(&p, ct, n);
	poly_pad(&p);
	u8 lens[16];
	put_le64(lens, alen);
	put_le64(lens + 8, n);
	poly_update(&p, lens, 16);
	poly_finish(&p, tag);
}

void chacha20_poly1305_seal(const u8 key[32], const u8 nonce[12], const u8 *aad, usize alen, const u8 *in, usize n, u8 *out)
{
	chacha20_xor(key, 1, nonce, in, out, n);
	aead_tag(key, nonce, aad, alen, out, n, out + n);
}

bool chacha20_poly1305_open(const u8 key[32], const u8 nonce[12], const u8 *aad, usize alen, const u8 *in, usize n, u8 *out)
{
	if (n < 16)
		return false;
	n -= 16;
	u8 tag[16];
	aead_tag(key, nonce, aad, alen, in, n, tag);
	if (!mem_equal_ct(tag, in + n, 16))
		return false;
	chacha20_xor(key, 1, nonce, in, out, n);
	return true;
}

/* ---- AES ------------------------------------------------------------------------------------ */

static u8 sbox[256], inv_sbox[256];

static u8 xtime(u8 x)
{
	return (u8)((x << 1) ^ ((x >> 7) * 0x1b));
}

static u8 gmul(u8 a, u8 b)
{
	u8 r = 0;
	while (b) {
		if (b & 1)
			r ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return r;
}

static void build_sbox(void)
{
	if (sbox[0])
		return;
	/* The S-box is the multiplicative inverse in GF(2^8) followed by an affine map. */
	u8 p = 1, q = 1;
	do {
		p = p ^ (u8)(p << 1) ^ (p & 0x80 ? 0x1b : 0);
		q ^= q << 1;
		q ^= q << 2;
		q ^= q << 4;
		if (q & 0x80)
			q ^= 0x09;
		u8 x = q ^ (u8)(q << 1 | q >> 7) ^ (u8)(q << 2 | q >> 6) ^ (u8)(q << 3 | q >> 5) ^ (u8)(q << 4 | q >> 4);
		sbox[p] = x ^ 0x63;
	} while (p != 1);
	sbox[0] = 0x63;
	for (int i = 0; i < 256; i++)
		inv_sbox[sbox[i]] = (u8)i;
}

void aes_init(AesGcm *a, const u8 *key, usize klen)
{
	build_sbox();
	int nk = (int)(klen / 4);
	a->rounds = nk + 6;
	int total = 4 * (a->rounds + 1);
	for (int i = 0; i < nk; i++)
		a->rk[i] = (u32)key[4 * i] << 24 | (u32)key[4 * i + 1] << 16 | (u32)key[4 * i + 2] << 8 | key[4 * i + 3];
	u8 rcon = 1;
	for (int i = nk; i < total; i++) {
		u32 t = a->rk[i - 1];
		if (i % nk == 0) {
			t = t << 8 | t >> 24;
			t = (u32)sbox[t >> 24] << 24 | (u32)sbox[(t >> 16) & 255] << 16 | (u32)sbox[(t >> 8) & 255] << 8 | sbox[t & 255];
			t ^= (u32)rcon << 24;
			rcon = xtime(rcon);
		} else if (nk > 6 && i % nk == 4) {
			t = (u32)sbox[t >> 24] << 24 | (u32)sbox[(t >> 16) & 255] << 16 | (u32)sbox[(t >> 8) & 255] << 8 | sbox[t & 255];
		}
		a->rk[i] = a->rk[i - nk] ^ t;
	}
	u8 zero[16] = { 0 };
	aes_encrypt_block(a, zero, a->h);
}

static void add_round_key(u8 s[16], const u32 *rk)
{
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			s[c * 4 + r] ^= (u8)(rk[c] >> (24 - 8 * r));
}

void aes_encrypt_block(const AesGcm *a, const u8 in[16], u8 out[16])
{
	u8 s[16];
	memcpy(s, in, 16);
	add_round_key(s, a->rk);
	for (int round = 1; round <= a->rounds; round++) {
		u8 t[16];
		/* SubBytes and ShiftRows together. */
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				t[c * 4 + r] = sbox[s[((c + r) % 4) * 4 + r]];
		if (round != a->rounds) {
			for (int c = 0; c < 4; c++) {
				u8 *col = t + c * 4;
				u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = a0 ^ a1 ^ a2 ^ a3;
				col[0] ^= all ^ xtime(a0 ^ a1);
				col[1] ^= all ^ xtime(a1 ^ a2);
				col[2] ^= all ^ xtime(a2 ^ a3);
				col[3] ^= all ^ xtime(a3 ^ a0);
			}
		}
		add_round_key(t, a->rk + round * 4);
		memcpy(s, t, 16);
	}
	memcpy(out, s, 16);
}

static void aes_decrypt_block(const AesGcm *a, const u8 in[16], u8 out[16])
{
	u8 s[16];
	memcpy(s, in, 16);
	add_round_key(s, a->rk + a->rounds * 4);
	for (int round = a->rounds - 1; round >= 0; round--) {
		u8 t[16];
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				t[((c + r) % 4) * 4 + r] = inv_sbox[s[c * 4 + r]];
		add_round_key(t, a->rk + round * 4);
		if (round != 0) {
			for (int c = 0; c < 4; c++) {
				u8 *col = t + c * 4;
				u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
				col[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
				col[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
				col[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
				col[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
			}
		}
		memcpy(s, t, 16);
	}
	memcpy(out, s, 16);
}

/* ---- GCM ----------------------------------------------------------------------------------- */

/* x = x * h in GF(2^128), bit by bit with masks instead of branches. */
static void gf_mul(u8 x[16], const u8 h[16])
{
	u64 zh = 0, zl = 0, vh = be64(h), vl = be64(h + 8);
	for (int i = 0; i < 128; i++) {
		u64 bit = (u64)((x[i / 8] >> (7 - i % 8)) & 1);
		u64 m = 0 - bit;
		zh ^= vh & m;
		zl ^= vl & m;
		u64 carry = 0 - (vl & 1);
		vl = (vl >> 1) | (vh << 63);
		vh = (vh >> 1) ^ (0xe100000000000000ull & carry);
	}
	put_be64(x, zh);
	put_be64(x + 8, zl);
}

static void ghash(const u8 h[16], u8 y[16], const u8 *data, usize n)
{
	while (n) {
		usize k = MIN(n, (usize)16);
		for (usize i = 0; i < k; i++)
			y[i] ^= data[i];
		gf_mul(y, h);
		data += k, n -= k;
	}
}

static void gcm_ctr(const AesGcm *a, u8 ctr[16], const u8 *in, u8 *out, usize n)
{
	u8 ks[16];
	while (n) {
		/* Increment the low 32 bits. */
		for (int i = 15; i >= 12; i--)
			if (++ctr[i])
				break;
		aes_encrypt_block(a, ctr, ks);
		usize k = MIN(n, (usize)16);
		for (usize i = 0; i < k; i++)
			out[i] = in[i] ^ ks[i];
		in += k, out += k, n -= k;
	}
}

static void gcm_tag(const AesGcm *a, const u8 nonce[12], const u8 *aad, usize alen, const u8 *ct, usize n, u8 tag[16])
{
	u8 y[16] = { 0 }, lens[16], j0[16];
	ghash(a->h, y, aad, alen);
	ghash(a->h, y, ct, n);
	put_be64(lens, (u64)alen * 8);
	put_be64(lens + 8, (u64)n * 8);
	ghash(a->h, y, lens, 16);
	memcpy(j0, nonce, 12);
	j0[12] = j0[13] = j0[14] = 0, j0[15] = 1;
	u8 ek[16];
	aes_encrypt_block(a, j0, ek);
	for (int i = 0; i < 16; i++)
		tag[i] = y[i] ^ ek[i];
}

void aes_gcm_seal(const AesGcm *a, const u8 nonce[12], const u8 *aad, usize alen, const u8 *in, usize n, u8 *out)
{
	u8 ctr[16];
	memcpy(ctr, nonce, 12);
	ctr[12] = ctr[13] = ctr[14] = 0, ctr[15] = 1;
	gcm_ctr(a, ctr, in, out, n);
	gcm_tag(a, nonce, aad, alen, out, n, out + n);
}

bool aes_gcm_open(const AesGcm *a, const u8 nonce[12], const u8 *aad, usize alen, const u8 *in, usize n, u8 *out)
{
	if (n < 16)
		return false;
	n -= 16;
	u8 tag[16];
	gcm_tag(a, nonce, aad, alen, in, n, tag);
	if (!mem_equal_ct(tag, in + n, 16))
		return false;
	u8 ctr[16];
	memcpy(ctr, nonce, 12);
	ctr[12] = ctr[13] = ctr[14] = 0, ctr[15] = 1;
	gcm_ctr(a, ctr, in, out, n);
	return true;
}

/* ---- Key unwrap (RFC 3394) -------------------------------------------------------------------- */

bool aes_key_unwrap(const u8 *kek, usize klen, const u8 *in, usize n, u8 *out)
{
	if (n < 24 || n % 8)
		return false;
	AesGcm a;
	aes_init(&a, kek, klen);
	usize blocks = n / 8 - 1;
	u8 A[8], b[16];
	memcpy(A, in, 8);
	memcpy(out, in + 8, n - 8);
	for (int j = 5; j >= 0; j--) {
		for (usize i = blocks; i >= 1; i--) {
			u64 t = (u64)blocks * (u64)j + i;
			for (int k = 0; k < 8; k++)
				b[k] = A[k] ^ (u8)(t >> (56 - 8 * k));
			memcpy(b + 8, out + (i - 1) * 8, 8);
			aes_decrypt_block(&a, b, b);
			memcpy(A, b, 8);
			memcpy(out + (i - 1) * 8, b + 8, 8);
		}
	}
	static const u8 iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };
	return mem_equal_ct(A, iv, 8);
}

/* RFC 3394 key wrap: n bytes (a multiple of 8, at least 16) become n + 8. */
void aes_key_wrap(const u8 *kek, usize klen, const u8 *in, usize n, u8 *out)
{
	AesGcm a;
	aes_init(&a, kek, klen);
	usize blocks = n / 8;
	u8 A[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 }, b[16];
	u8 *R = out + 8;
	memmove(R, in, n);
	for (int j = 0; j <= 5; j++) {
		for (usize i = 1; i <= blocks; i++) {
			memcpy(b, A, 8);
			memcpy(b + 8, R + (i - 1) * 8, 8);
			aes_encrypt_block(&a, b, b);
			u64 t = (u64)blocks * (u64)j + i;
			for (int k = 0; k < 8; k++)
				A[k] = b[k] ^ (u8)(t >> (56 - 8 * k));
			memcpy(R + (i - 1) * 8, b + 8, 8);
		}
	}
	memcpy(out, A, 8);
}
