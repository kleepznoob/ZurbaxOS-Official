/*
 * DEFLATE (RFC 1951) and the zlib wrapper (RFC 1950).
 *
 * The decoder handles stored, fixed and dynamic Huffman blocks. The encoder
 * uses LZ77 with hash chains and the fixed Huffman code, which compresses
 * screenshots and UI images well while staying short.
 */
#include "zg.h"

/* ---- Checksums ---------------------------------------------------------------- */

static u32 crc_table[256];

u32 crc32_update(u32 crc, const void *data, usize len)
{
	if (!crc_table[1]) {
		for (u32 n = 0; n < 256; n++) {
			u32 c = n;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			crc_table[n] = c;
		}
	}
	const u8 *p = data;
	crc = ~crc;
	for (usize i = 0; i < len; i++)
		crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

u32 adler32_update(u32 adler, const void *data, usize len)
{
	const u8 *p = data;
	u32 a = adler & 0xffff, b = adler >> 16;
	while (len) {
		usize n = MIN(len, 5552u);
		len -= n;
		while (n--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return b << 16 | a;
}

/* ---- Decoder --------------------------------------------------------------------- */

typedef struct {
	const u8 *src, *end;
	u32 bits;
	int nbits;
	bool error;
	u8 *out;
	usize len, cap;
} Inflate;

typedef struct {
	u16 count[16];   /* number of codes of each length */
	u16 symbol[320]; /* symbols ordered by code */
} Huffman;

static u32 getbits(Inflate *s, int n)
{
	while (s->nbits < n) {
		if (s->src >= s->end) {
			s->error = true;
			return 0;
		}
		s->bits |= (u32)*s->src++ << s->nbits;
		s->nbits += 8;
	}
	u32 v = s->bits & ((1u << n) - 1);
	s->bits >>= n;
	s->nbits -= n;
	return v;
}

static void put_byte(Inflate *s, u8 b)
{
	if (s->len == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 65536;
		s->out = mem_realloc(s->out, s->cap);
	}
	s->out[s->len++] = b;
}

static bool build_huffman(Huffman *h, const u8 *lengths, int n)
{
	memset(h->count, 0, sizeof h->count);
	for (int i = 0; i < n; i++)
		h->count[lengths[i]]++;
	h->count[0] = 0;
	u16 offs[16];
	offs[1] = 0;
	for (int i = 1; i < 15; i++)
		offs[i + 1] = offs[i] + h->count[i];
	for (int i = 0; i < n; i++)
		if (lengths[i])
			h->symbol[offs[lengths[i]]++] = (u16)i;
	return true;
}

/* Canonical Huffman decode, one bit at a time (codes are stored MSB-first). */
static int decode_sym(Inflate *s, const Huffman *h)
{
	int code = 0, first = 0, index = 0;
	for (int len = 1; len < 16; len++) {
		code |= (int)getbits(s, 1);
		int count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
		if (s->error)
			return -1;
	}
	s->error = true;
	return -1;
}

static const u16 len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
				  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
				   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool inflate_codes(Inflate *s, const Huffman *lit, const Huffman *dist)
{
	for (;;) {
		int sym = decode_sym(s, lit);
		if (sym < 0)
			return false;
		if (sym < 256) {
			put_byte(s, (u8)sym);
			continue;
		}
		if (sym == 256)
			return true;
		sym -= 257;
		if (sym >= 29)
			return false;
		usize len = len_base[sym] + getbits(s, len_extra[sym]);
		int ds = decode_sym(s, dist);
		if (ds < 0 || ds >= 30)
			return false;
		usize d = dist_base[ds] + getbits(s, dist_extra[ds]);
		if (s->error || d > s->len)
			return false;
		if (s->len + len > s->cap) {
			while (s->len + len > s->cap)
				s->cap = s->cap ? s->cap * 2 : 65536;
			s->out = mem_realloc(s->out, s->cap);
		}
		u8 *o = s->out + s->len;
		const u8 *from = o - d;
		for (usize i = 0; i < len; i++)
			o[i] = from[i]; /* byte by byte: the copy may overlap itself */
		s->len += len;
	}
}

static bool inflate_block_dynamic(Inflate *s)
{
	static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	int nlen = (int)getbits(s, 5) + 257, ndist = (int)getbits(s, 5) + 1, ncode = (int)getbits(s, 4) + 4;
	if (nlen > 286 || ndist > 30)
		return false;
	u8 lengths[320] = { 0 };
	for (int i = 0; i < ncode; i++)
		lengths[order[i]] = (u8)getbits(s, 3);
	Huffman lencode;
	build_huffman(&lencode, lengths, 19);
	memset(lengths, 0, sizeof lengths);
	int idx = 0;
	while (idx < nlen + ndist) {
		int sym = decode_sym(s, &lencode);
		if (sym < 0)
			return false;
		if (sym < 16) {
			lengths[idx++] = (u8)sym;
			continue;
		}
		u8 prev = 0;
		int rep;
		if (sym == 16) {
			if (idx == 0)
				return false;
			prev = lengths[idx - 1];
			rep = 3 + (int)getbits(s, 2);
		} else if (sym == 17) {
			rep = 3 + (int)getbits(s, 3);
		} else {
			rep = 11 + (int)getbits(s, 7);
		}
		if (idx + rep > nlen + ndist)
			return false;
		while (rep--)
			lengths[idx++] = prev;
	}
	Huffman lit, dist;
	build_huffman(&lit, lengths, nlen);
	build_huffman(&dist, lengths + nlen, ndist);
	return inflate_codes(s, &lit, &dist);
}

static void fixed_tables(Huffman *lit, Huffman *dist)
{
	u8 l[288];
	int i = 0;
	for (; i < 144; i++) l[i] = 8;
	for (; i < 256; i++) l[i] = 9;
	for (; i < 280; i++) l[i] = 7;
	for (; i < 288; i++) l[i] = 8;
	build_huffman(lit, l, 288);
	u8 d[30];
	for (i = 0; i < 30; i++)
		d[i] = 5;
	build_huffman(dist, d, 30);
}

u8 *zlib_inflate(const u8 *data, usize len, usize *out_len, bool has_header)
{
	Inflate s = { data, data + len, 0, 0, false, NULL, 0, 0 };
	if (has_header) {
		if (len < 2 || (data[0] & 0x0f) != 8 || ((data[0] << 8) | data[1]) % 31 != 0)
			return NULL;
		s.src += 2;
	}
	bool last = false;
	while (!last && !s.error) {
		last = getbits(&s, 1);
		u32 type = getbits(&s, 2);
		bool ok;
		if (type == 0) {
			s.bits = 0;
			s.nbits = 0;
			if (s.end - s.src < 4) {
				ok = false;
			} else {
				u16 n = (u16)(s.src[0] | s.src[1] << 8), nn = (u16)(s.src[2] | s.src[3] << 8);
				s.src += 4;
				ok = (u16)~nn == n && s.end - s.src >= n;
				for (u16 i = 0; ok && i < n; i++)
					put_byte(&s, *s.src++);
			}
		} else if (type == 1) {
			Huffman lit, dist;
			fixed_tables(&lit, &dist);
			ok = inflate_codes(&s, &lit, &dist);
		} else if (type == 2) {
			ok = inflate_block_dynamic(&s);
		} else {
			ok = false;
		}
		if (!ok)
			s.error = true;
	}
	if (s.error) {
		mem_free(s.out);
		return NULL;
	}
	if (out_len)
		*out_len = s.len;
	return s.out ? s.out : mem_zalloc(1);
}

/* ---- Encoder ------------------------------------------------------------------------- */

typedef struct {
	u8 *out;
	usize len, cap;
	u32 bits;
	int nbits;
} BitWriter;

static void bw_byte(BitWriter *w, u8 b)
{
	if (w->len == w->cap) {
		w->cap = w->cap ? w->cap * 2 : 65536;
		w->out = mem_realloc(w->out, w->cap);
	}
	w->out[w->len++] = b;
}

static void bw_bits(BitWriter *w, u32 v, int n)
{
	w->bits |= v << w->nbits;
	w->nbits += n;
	while (w->nbits >= 8) {
		bw_byte(w, (u8)w->bits);
		w->bits >>= 8;
		w->nbits -= 8;
	}
}

/* Huffman codes are written MSB-first, so reverse them into the LSB-first stream. */
static void bw_code(BitWriter *w, u32 code, int len)
{
	u32 r = 0;
	for (int i = 0; i < len; i++)
		r |= ((code >> i) & 1) << (len - 1 - i);
	bw_bits(w, r, len);
}

static void emit_literal(BitWriter *w, int lit)
{
	if (lit < 144)
		bw_code(w, 0x30 + (u32)lit, 8);
	else if (lit < 256)
		bw_code(w, 0x190 + (u32)(lit - 144), 9);
	else if (lit < 280)
		bw_code(w, (u32)(lit - 256), 7);
	else
		bw_code(w, 0xc0 + (u32)(lit - 280), 8);
}

static void emit_match(BitWriter *w, int len, int dist)
{
	int li = 28;
	while (li > 0 && len_base[li] > len)
		li--;
	emit_literal(w, 257 + li);
	bw_bits(w, (u32)(len - len_base[li]), len_extra[li]);
	int di = 29;
	while (di > 0 && dist_base[di] > dist)
		di--;
	bw_code(w, (u32)di, 5);
	bw_bits(w, (u32)(dist - dist_base[di]), dist_extra[di]);
}

#define HASH_BITS 15
#define WINDOW 32768

u8 *zlib_deflate(const u8 *data, usize len, usize *out_len)
{
	BitWriter w = { 0 };
	bw_byte(&w, 0x78);
	bw_byte(&w, 0x9c);
	bw_bits(&w, 1, 1); /* final block */
	bw_bits(&w, 1, 2); /* fixed Huffman */
	i32 *head = mem_alloc(sizeof(i32) << HASH_BITS);
	for (usize i = 0; i < (1u << HASH_BITS); i++)
		head[i] = -1;
	i32 *prev = mem_alloc(sizeof(i32) * WINDOW);
	usize i = 0;
	while (i < len) {
		int best_len = 0, best_dist = 0;
		if (i + 3 <= len) {
			u32 h = ((u32)data[i] << 10 ^ (u32)data[i + 1] << 5 ^ data[i + 2]) & ((1u << HASH_BITS) - 1);
			i32 cand = head[h];
			int chain = 48;
			usize maxlen = MIN(258u, len - i);
			while (cand >= 0 && i - (usize)cand <= WINDOW && chain--) {
				const u8 *a = data + cand, *b = data + i;
				usize l = 0;
				while (l < maxlen && a[l] == b[l])
					l++;
				if ((int)l > best_len) {
					best_len = (int)l;
					best_dist = (int)(i - (usize)cand);
					if (l == maxlen)
						break;
				}
				i32 nxt = prev[cand % WINDOW];
				if (nxt >= cand)
					break;
				cand = nxt;
			}
			prev[i % WINDOW] = head[h];
			head[h] = (i32)i;
		}
		if (best_len >= 3) {
			emit_match(&w, best_len, best_dist);
			/* Index the skipped positions so later matches can find them. */
			for (usize k = i + 1; k < i + (usize)best_len && k + 3 <= len; k++) {
				u32 h = ((u32)data[k] << 10 ^ (u32)data[k + 1] << 5 ^ data[k + 2]) & ((1u << HASH_BITS) - 1);
				prev[k % WINDOW] = head[h];
				head[h] = (i32)k;
			}
			i += (usize)best_len;
		} else {
			emit_literal(&w, data[i]);
			i++;
		}
	}
	emit_literal(&w, 256);
	if (w.nbits)
		bw_bits(&w, 0, 8 - w.nbits);
	u32 ad = adler32_update(1, data, len);
	bw_byte(&w, (u8)(ad >> 24));
	bw_byte(&w, (u8)(ad >> 16));
	bw_byte(&w, (u8)(ad >> 8));
	bw_byte(&w, (u8)ad);
	mem_free(head);
	mem_free(prev);
	*out_len = w.len;
	return w.out;
}
