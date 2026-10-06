/* GIF decoding: the first frame, composited on a transparent canvas the
 * size of the logical screen. Animations show their first picture. */
#include "zg.h"

typedef struct {
	const u8 *d;
	usize n, pos;            /* byte position */
	u32 bits;
	int nbits;
} BitReader;

static int read_code(BitReader *br, int size)
{
	while (br->nbits < size) {
		if (br->pos >= br->n)
			return -1;
		br->bits |= (u32)br->d[br->pos++] << br->nbits;
		br->nbits += 8;
	}
	int code = (int)(br->bits & ((1u << size) - 1));
	br->bits >>= size;
	br->nbits -= size;
	return code;
}

/* LZW: decodes up to `count` color indices into out. Returns how many. */
static usize lzw_decode(const u8 *data, usize n, int min_size, u8 *out, usize count)
{
	if (min_size < 2 || min_size > 11)
		return 0;
	static const int MAXC = 4096;
	u16 *prefix = mem_alloc(sizeof(u16) * MAXC);
	u8 *suffix = mem_alloc(MAXC), *stack = mem_alloc(MAXC + 1);
	int clear = 1 << min_size, eoi = clear + 1;
	int size = min_size + 1, next = clear + 2, prev = -1;
	u8 first = 0;
	for (int i = 0; i < clear; i++) {
		prefix[i] = 0xffff;
		suffix[i] = (u8)i;
	}
	BitReader br = { data, n, 0, 0, 0 };
	usize w = 0;
	while (w < count) {
		int code = read_code(&br, size);
		if (code < 0 || code == eoi)
			break;
		if (code == clear) {
			size = min_size + 1;
			next = clear + 2;
			prev = -1;
			continue;
		}
		int sp = 0, c = code;
		if (prev < 0) {
			if (code >= clear)
				break;
			out[w++] = (u8)code;
			first = (u8)code;
			prev = code;
			continue;
		}
		if (code >= next) {
			if (code > next)
				break;   /* damaged */
			stack[sp++] = first;
			c = prev;
		}
		while (c >= clear && sp < MAXC) {
			stack[sp++] = suffix[c];
			c = prefix[c];
		}
		stack[sp++] = (u8)c;
		first = (u8)c;
		while (sp && w < count)
			out[w++] = stack[--sp];
		if (next < MAXC) {
			prefix[next] = (u16)prev;
			suffix[next] = first;
			next++;
			if (next == (1 << size) && size < 12)
				size++;
		}
		prev = code;
	}
	mem_free(prefix);
	mem_free(suffix);
	mem_free(stack);
	return w;
}

static void read_palette(const u8 *d, int n, u32 *pal)
{
	for (int i = 0; i < n; i++)
		pal[i] = 0xff000000u | (u32)d[i * 3] << 16 | (u32)d[i * 3 + 1] << 8 | d[i * 3 + 2];
}

Canvas *gif_decode(const u8 *d, usize len)
{
	if (len < 13 || memcmp(d, "GIF8", 4) != 0)
		return NULL;
	int W = d[6] | d[7] << 8, H = d[8] | d[9] << 8;
	u8 flags = d[10];
	usize p = 13;
	u32 gpal[256] = { 0 };
	if (flags & 0x80) {
		int n = 2 << (flags & 7);
		if (p + (usize)n * 3 > len)
			return NULL;
		read_palette(d + p, n, gpal);
		p += (usize)n * 3;
	}
	int transparent = -1;
	while (p < len) {
		u8 b = d[p++];
		if (b == 0x21) {
			if (p >= len)
				break;
			u8 label = d[p++];
			if (label == 0xf9 && p + 5 <= len && d[p] >= 4 && (d[p + 1] & 1))
				transparent = d[p + 4];
			while (p < len && d[p])
				p += (usize)d[p] + 1;
			p++;
			continue;
		}
		if (b != 0x2c)
			break;
		if (p + 9 > len)
			return NULL;
		int ix = d[p] | d[p + 1] << 8, iy = d[p + 2] | d[p + 3] << 8;
		int iw = d[p + 4] | d[p + 5] << 8, ih = d[p + 6] | d[p + 7] << 8;
		u8 f = d[p + 8];
		p += 9;
		u32 lpal[256];
		const u32 *pal = gpal;
		if (f & 0x80) {
			int n = 2 << (f & 7);
			if (p + (usize)n * 3 > len)
				return NULL;
			read_palette(d + p, n, lpal);
			pal = lpal;
			p += (usize)n * 3;
		}
		if (W <= 0 || H <= 0) {
			W = ix + iw;
			H = iy + ih;
		}
		if (W <= 0 || H <= 0 || W > 16384 || H > 16384 || iw <= 0 || ih <= 0 || p >= len)
			return NULL;
		int min_size = d[p++];
		/* Join the data sub-blocks. */
		StrBuf data = { 0 };
		while (p < len && d[p]) {
			usize k = d[p];
			if (p + 1 + k > len)
				break;
			sb_add(&data, d + p + 1, k);
			p += k + 1;
		}
		usize count = (usize)iw * (usize)ih;
		u8 *idx = mem_zalloc(count);
		lzw_decode((const u8 *)data.data, data.len, min_size, idx, count);
		sb_free(&data);
		Canvas *c = canvas_new(W, H);
		canvas_clear(c, COLOR_CLEAR);
		/* Interlaced images store rows in four passes. */
		static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
		int row = 0, pass = 0;
		for (int y = 0; y < ih; y++) {
			int dy;
			if (f & 0x40) {
				while (pass < 4 && start[pass] + row * step[pass] >= ih) {
					pass++;
					row = 0;
				}
				if (pass >= 4)
					break;
				dy = start[pass] + row * step[pass];
				row++;
			} else {
				dy = y;
			}
			int ty = iy + dy;
			if (ty < 0 || ty >= H)
				continue;
			u32 *out = canvas_row(c, ty);
			const u8 *src = idx + (usize)y * (usize)iw;
			for (int x = 0; x < iw; x++) {
				int tx = ix + x;
				if (tx < 0 || tx >= W || src[x] == transparent)
					continue;
				out[tx] = pal[src[x]];
			}
		}
		mem_free(idx);
		return c;
	}
	return NULL;
}
