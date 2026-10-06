/* SHA-1, SHA-384/512, a common interface over all hashes, HMAC, HKDF and
 * PBKDF2-SHA1 (Wi-Fi passphrases). SHA-256 lives in libzr. */
#include "zn.h"

static u32 rol32(u32 x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static u64 ror64(u64 x, int n)
{
	return (x >> n) | (x << (64 - n));
}

static u32 be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static u64 be64(const u8 *p)
{
	return (u64)be32(p) << 32 | be32(p + 4);
}

static void put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v;
}

static void put_be64(u8 *p, u64 v)
{
	put_be32(p, (u32)(v >> 32));
	put_be32(p + 4, (u32)v);
}

/* ---- SHA-1 ------------------------------------------------------------------------------- */

static void sha1_block(Sha1 *s, const u8 *p)
{
	u32 w[80];
	for (int i = 0; i < 16; i++)
		w[i] = be32(p + i * 4);
	for (int i = 16; i < 80; i++)
		w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
	for (int i = 0; i < 80; i++) {
		u32 f, k;
		if (i < 20)
			f = (b & c) | (~b & d), k = 0x5a827999;
		else if (i < 40)
			f = b ^ c ^ d, k = 0x6ed9eba1;
		else if (i < 60)
			f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
		else
			f = b ^ c ^ d, k = 0xca62c1d6;
		u32 t = rol32(a, 5) + f + e + k + w[i];
		e = d, d = c, c = rol32(b, 30), b = a, a = t;
	}
	s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e;
}

void sha1_init(Sha1 *s)
{
	static const u32 iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
	memset(s, 0, sizeof *s);
	memcpy(s->h, iv, sizeof iv);
}

void sha1_update(Sha1 *s, const void *data, usize n)
{
	const u8 *p = data;
	s->len += n;
	while (n) {
		usize k = MIN(n, 64 - s->fill);
		memcpy(s->buf + s->fill, p, k);
		s->fill += k, p += k, n -= k;
		if (s->fill == 64) {
			sha1_block(s, s->buf);
			s->fill = 0;
		}
	}
}

void sha1_final(Sha1 *s, u8 out[20])
{
	u64 bits = s->len * 8;
	u8 pad = 0x80;
	sha1_update(s, &pad, 1);
	u8 zero = 0;
	while (s->fill != 56)
		sha1_update(s, &zero, 1);
	u8 len[8];
	put_be64(len, bits);
	sha1_update(s, len, 8);
	for (int i = 0; i < 5; i++)
		put_be32(out + i * 4, s->h[i]);
}

/* ---- SHA-512 / SHA-384 ------------------------------------------------------------------- */

static const u64 K512[80] = {
	0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019,
	0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
	0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3,
	0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
	0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
	0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
	0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
	0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
	0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb,
	0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
	0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207,
	0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
	0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a,
	0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

static void sha512_block(Sha512 *s, const u8 *p)
{
	u64 w[80];
	for (int i = 0; i < 16; i++)
		w[i] = be64(p + i * 8);
	for (int i = 16; i < 80; i++) {
		u64 s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
		u64 s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	u64 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
	for (int i = 0; i < 80; i++) {
		u64 t1 = h + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
		u64 t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
	}
	s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d;
	s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

void sha512_init(Sha512 *s)
{
	static const u64 iv[8] = { 0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
				   0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179 };
	memset(s, 0, sizeof *s);
	memcpy(s->h, iv, sizeof iv);
	s->out_len = 64;
}

void sha384_init(Sha512 *s)
{
	static const u64 iv[8] = { 0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
				   0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4 };
	memset(s, 0, sizeof *s);
	memcpy(s->h, iv, sizeof iv);
	s->out_len = 48;
}

void sha512_update(Sha512 *s, const void *data, usize n)
{
	const u8 *p = data;
	s->len += n;
	while (n) {
		usize k = MIN(n, 128 - s->fill);
		memcpy(s->buf + s->fill, p, k);
		s->fill += k, p += k, n -= k;
		if (s->fill == 128) {
			sha512_block(s, s->buf);
			s->fill = 0;
		}
	}
}

void sha512_final(Sha512 *s, u8 *out)
{
	u64 bits = s->len * 8;
	u8 pad = 0x80;
	sha512_update(s, &pad, 1);
	u8 zero = 0;
	while (s->fill != 112)
		sha512_update(s, &zero, 1);
	u8 len[16] = { 0 };
	put_be64(len + 8, bits);
	sha512_update(s, len, 16);
	u8 full[64];
	for (int i = 0; i < 8; i++)
		put_be64(full + i * 8, s->h[i]);
	memcpy(out, full, s->out_len);
}

/* ---- Any hash ------------------------------------------------------------------------------- */

usize hash_len(int kind)
{
	switch (kind) {
	case HASH_SHA1: return 20;
	case HASH_SHA256: return 32;
	case HASH_SHA384: return 48;
	case HASH_SHA512: return 64;
	}
	return 0;
}

static usize block_len(int kind)
{
	return kind == HASH_SHA384 || kind == HASH_SHA512 ? 128 : 64;
}

void hash_init(Hash *h, int kind)
{
	h->kind = kind;
	switch (kind) {
	case HASH_SHA1: sha1_init(&h->u.sha1); break;
	case HASH_SHA256: sha256_init(&h->u.sha256); break;
	case HASH_SHA384: sha384_init(&h->u.sha512); break;
	case HASH_SHA512: sha512_init(&h->u.sha512); break;
	}
}

void hash_update(Hash *h, const void *data, usize n)
{
	switch (h->kind) {
	case HASH_SHA1: sha1_update(&h->u.sha1, data, n); break;
	case HASH_SHA256: sha256_update(&h->u.sha256, data, n); break;
	default: sha512_update(&h->u.sha512, data, n); break;
	}
}

void hash_final(Hash *h, u8 *out)
{
	switch (h->kind) {
	case HASH_SHA1: sha1_final(&h->u.sha1, out); break;
	case HASH_SHA256: sha256_final(&h->u.sha256, out); break;
	default: sha512_final(&h->u.sha512, out); break;
	}
}

void hash_once(int kind, const void *data, usize n, u8 *out)
{
	Hash h;
	hash_init(&h, kind);
	hash_update(&h, data, n);
	hash_final(&h, out);
}

void hmac(int kind, const void *key, usize klen, const void *msg, usize mlen, u8 *out)
{
	usize bl = block_len(kind), hl = hash_len(kind);
	u8 k[128] = { 0 }, pad[128];
	if (klen > bl)
		hash_once(kind, key, klen, k);
	else
		memcpy(k, key, klen);
	Hash h;
	for (usize i = 0; i < bl; i++)
		pad[i] = k[i] ^ 0x36;
	hash_init(&h, kind);
	hash_update(&h, pad, bl);
	hash_update(&h, msg, mlen);
	u8 inner[HASH_MAX];
	hash_final(&h, inner);
	for (usize i = 0; i < bl; i++)
		pad[i] = k[i] ^ 0x5c;
	hash_init(&h, kind);
	hash_update(&h, pad, bl);
	hash_update(&h, inner, hl);
	hash_final(&h, out);
}

void hkdf_extract(int kind, const void *salt, usize slen, const void *ikm, usize ilen, u8 *prk)
{
	u8 zero[HASH_MAX] = { 0 };
	if (!salt || !slen) {
		salt = zero;
		slen = hash_len(kind);
	}
	hmac(kind, salt, slen, ikm, ilen, prk);
}

void hkdf_expand(int kind, const u8 *prk, const void *info, usize ilen, u8 *out, usize olen)
{
	usize hl = hash_len(kind);
	u8 t[HASH_MAX];
	usize tlen = 0;
	u8 *msg = mem_alloc(hl + ilen + 1);
	for (u8 counter = 1; olen; counter++) {
		memcpy(msg, t, tlen);
		memcpy(msg + tlen, info, ilen);
		msg[tlen + ilen] = counter;
		hmac(kind, prk, hl, msg, tlen + ilen + 1, t);
		tlen = hl;
		usize k = MIN(olen, hl);
		memcpy(out, t, k);
		out += k, olen -= k;
	}
	mem_free(msg);
}

void pbkdf2_sha1(const void *pw, usize plen, const void *salt, usize slen, u32 iterations, u8 *out, usize olen)
{
	u8 *msg = mem_alloc(slen + 4);
	memcpy(msg, salt, slen);
	for (u32 block = 1; olen; block++) {
		put_be32(msg + slen, block);
		u8 u[20], acc[20];
		hmac(HASH_SHA1, pw, plen, msg, slen + 4, u);
		memcpy(acc, u, 20);
		for (u32 i = 1; i < iterations; i++) {
			hmac(HASH_SHA1, pw, plen, u, 20, u);
			for (int k = 0; k < 20; k++)
				acc[k] ^= u[k];
		}
		usize k = MIN(olen, (usize)20);
		memcpy(out, acc, k);
		out += k, olen -= k;
	}
	mem_free(msg);
}
