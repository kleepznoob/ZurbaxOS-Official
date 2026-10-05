/* SHA-256 (FIPS 180-4), HMAC (RFC 2104) and PBKDF2 (RFC 8018). */
#include "zr.h"
#include "zr/crypto.h"

static const u32 K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void compress(u32 h[8], const u8 *block)
{
	u32 w[64];
	for (int i = 0; i < 16; i++)
		w[i] = (u32)block[i * 4] << 24 | (u32)block[i * 4 + 1] << 16 | (u32)block[i * 4 + 2] << 8 | block[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		u32 ch = (e & f) ^ (~e & g);
		u32 t1 = hh + S1 + ch + K[i] + w[i];
		u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		u32 maj = (a & b) ^ (a & c) ^ (b & c);
		u32 t2 = S0 + maj;
		hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
	}
	h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void sha256_init(Sha256 *s)
{
	static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
				   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	memcpy(s->h, iv, sizeof iv);
	s->len = 0;
	s->fill = 0;
}

void sha256_update(Sha256 *s, const void *data, usize n)
{
	const u8 *p = data;
	s->len += n;
	while (n) {
		usize take = MIN(n, 64 - s->fill);
		memcpy(s->buf + s->fill, p, take);
		s->fill += take, p += take, n -= take;
		if (s->fill == 64) {
			compress(s->h, s->buf);
			s->fill = 0;
		}
	}
}

void sha256_final(Sha256 *s, u8 out[32])
{
	u64 bits = s->len * 8;
	u8 pad = 0x80;
	sha256_update(s, &pad, 1);
	u8 zero = 0;
	while (s->fill != 56)
		sha256_update(s, &zero, 1);
	u8 lenb[8];
	for (int i = 0; i < 8; i++)
		lenb[i] = (u8)(bits >> (56 - 8 * i));
	sha256_update(s, lenb, 8);
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (u8)(s->h[i] >> 24);
		out[i * 4 + 1] = (u8)(s->h[i] >> 16);
		out[i * 4 + 2] = (u8)(s->h[i] >> 8);
		out[i * 4 + 3] = (u8)s->h[i];
	}
}

void sha256(const void *data, usize n, u8 out[32])
{
	Sha256 s;
	sha256_init(&s);
	sha256_update(&s, data, n);
	sha256_final(&s, out);
}

void hmac_sha256(const void *key, usize klen, const void *msg, usize mlen, u8 out[32])
{
	u8 k[64] = { 0 }, ipad[64], opad[64], inner[32];
	if (klen > 64)
		sha256(key, klen, k);
	else
		memcpy(k, key, klen);
	for (int i = 0; i < 64; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	Sha256 s;
	sha256_init(&s);
	sha256_update(&s, ipad, 64);
	sha256_update(&s, msg, mlen);
	sha256_final(&s, inner);
	sha256_init(&s);
	sha256_update(&s, opad, 64);
	sha256_update(&s, inner, 32);
	sha256_final(&s, out);
}

void pbkdf2_sha256(const void *pw, usize plen, const void *salt, usize slen, u32 iterations, u8 *out, usize olen)
{
	u8 *block = mem_alloc(slen + 4);
	memcpy(block, salt, slen);
	for (u32 i = 1; olen; i++) {
		block[slen] = (u8)(i >> 24), block[slen + 1] = (u8)(i >> 16);
		block[slen + 2] = (u8)(i >> 8), block[slen + 3] = (u8)i;
		u8 u[32], t[32];
		hmac_sha256(pw, plen, block, slen + 4, u);
		memcpy(t, u, 32);
		for (u32 j = 1; j < iterations; j++) {
			hmac_sha256(pw, plen, u, 32, u);
			for (int k = 0; k < 32; k++)
				t[k] ^= u[k];
		}
		usize take = MIN(olen, 32u);
		memcpy(out, t, take);
		out += take, olen -= take;
	}
	mem_free(block);
}

bool mem_equal_ct(const void *a, const void *b, usize n)
{
	const u8 *x = a, *y = b;
	u8 diff = 0;
	for (usize i = 0; i < n; i++)
		diff |= x[i] ^ y[i];
	return diff == 0;
}

void hex_encode(const u8 *data, usize n, char *out)
{
	static const char digits[] = "0123456789abcdef";
	for (usize i = 0; i < n; i++) {
		out[i * 2] = digits[data[i] >> 4];
		out[i * 2 + 1] = digits[data[i] & 15];
	}
	out[n * 2] = 0;
}

bool hex_decode(const char *s, u8 *out, usize n)
{
	for (usize i = 0; i < n; i++) {
		int hi = s[i * 2], lo = hi ? s[i * 2 + 1] : 0;
		if (!is_xdigit(hi) || !is_xdigit(lo))
			return false;
		hi = is_digit(hi) ? hi - '0' : to_lower(hi) - 'a' + 10;
		lo = is_digit(lo) ? lo - '0' : to_lower(lo) - 'a' + 10;
		out[i] = (u8)(hi << 4 | lo);
	}
	return true;
}

#define PW_ITERATIONS 60000

char *password_hash(const char *password)
{
	u8 salt[16], hash[32];
	rand_bytes(salt, sizeof salt);
	pbkdf2_sha256(password, strlen(password), salt, sizeof salt, PW_ITERATIONS, hash, sizeof hash);
	char sh[33], hh[65];
	hex_encode(salt, sizeof salt, sh);
	hex_encode(hash, sizeof hash, hh);
	return str_fmt("pbkdf2-sha256$%d$%s$%s", PW_ITERATIONS, sh, hh);
}

bool password_check(const char *password, const char *record)
{
	if (!record || !str_starts(record, "pbkdf2-sha256$"))
		return false;
	const char *p = record + 14;
	i64 iter = str_to_int(p, &p);
	if (*p != '$' || iter < 1 || iter > 10000000)
		return false;
	p++;
	u8 salt[16], want[32], got[32];
	if (strlen(p) < 32 + 1 + 64 || p[32] != '$' || !hex_decode(p, salt, 16) || !hex_decode(p + 33, want, 32))
		return false;
	pbkdf2_sha256(password, strlen(password), salt, 16, (u32)iter, got, 32);
	return mem_equal_ct(got, want, 32);
}
