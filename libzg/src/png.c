/* PNG decoding (all color types, bit depths and interlacing) and encoding. */
#include "zg.h"

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static void put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v;
}

static int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = ABS(p - a), pb = ABS(p - b), pc = ABS(p - c);
	if (pa <= pb && pa <= pc)
		return a;
	return pb <= pc ? b : c;
}

/* Undoes the per-row filters in place. `bpp` is bytes per complete pixel (>= 1). */
static bool unfilter(u8 *data, usize rows, usize rowbytes, usize bpp, u8 *out)
{
	u8 *prev = NULL;
	for (usize y = 0; y < rows; y++) {
		u8 ft = data[y * (rowbytes + 1)];
		const u8 *src = data + y * (rowbytes + 1) + 1;
		u8 *cur = out + y * rowbytes;
		for (usize x = 0; x < rowbytes; x++) {
			int a = x >= bpp ? cur[x - bpp] : 0;
			int b = prev ? prev[x] : 0;
			int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
			int v = src[x];
			switch (ft) {
			case 0: break;
			case 1: v += a; break;
			case 2: v += b; break;
			case 3: v += (a + b) / 2; break;
			case 4: v += paeth(a, b, c); break;
			default: return false;
			}
			cur[x] = (u8)v;
		}
		prev = cur;
	}
	return true;
}

typedef struct {
	int color_type, depth, channels;
	u8 palette[256][4];
	int npalette;
	bool has_trns;
	u16 trns[3];
} PngInfo;

static u16 sample(const u8 *row, usize index, int depth)
{
	switch (depth) {
	case 16: return (u16)(row[index * 2] << 8 | row[index * 2 + 1]);
	case 8: return row[index];
	default: {
		usize bit = index * (usize)depth;
		u8 byte = row[bit / 8];
		int shift = 8 - depth - (int)(bit % 8);
		return (byte >> shift) & ((1 << depth) - 1);
	}
	}
}

static u32 png_pixel(const PngInfo *in, const u8 *row, usize x)
{
	int d = in->depth;
	usize ch = (usize)in->channels;
	u32 maxv = (1u << d) - 1;
	u32 r, g, b, a = 255;
#define S8(v) (d == 16 ? (u32)(v) >> 8 : ((u32)(v) * 255 + maxv / 2) / maxv)
	switch (in->color_type) {
	case 0: {
		u16 v = sample(row, x, d);
		r = g = b = S8(v);
		if (in->has_trns && v == in->trns[0])
			a = 0;
		break;
	}
	case 2: {
		u16 rv = sample(row, x * 3, d), gv = sample(row, x * 3 + 1, d), bv = sample(row, x * 3 + 2, d);
		r = S8(rv), g = S8(gv), b = S8(bv);
		if (in->has_trns && rv == in->trns[0] && gv == in->trns[1] && bv == in->trns[2])
			a = 0;
		break;
	}
	case 3: {
		u16 i = sample(row, x, d);
		const u8 *p = in->palette[i < in->npalette ? i : 0];
		r = p[0], g = p[1], b = p[2], a = p[3];
		break;
	}
	case 4:
		r = g = b = S8(sample(row, x * 2, d));
		a = S8(sample(row, x * 2 + 1, d));
		break;
	default: /* 6 */
		r = S8(sample(row, x * ch, d));
		g = S8(sample(row, x * ch + 1, d));
		b = S8(sample(row, x * ch + 2, d));
		a = S8(sample(row, x * ch + 3, d));
		break;
	}
#undef S8
	return color_premul(RGBA(r, g, b, a));
}

Canvas *png_decode(const u8 *data, usize len)
{
	static const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	if (len < 33 || memcmp(data, sig, 8) != 0)
		return NULL;
	PngInfo in = { 0 };
	u32 w = 0, h = 0;
	int interlace = 0;
	StrBuf idat = { 0 };
	for (usize p = 8; p + 12 <= len;) {
		u32 clen = be32(data + p);
		const u8 *type = data + p + 4, *body = data + p + 8;
		if (clen > len - p - 12)
			break;
		if (!memcmp(type, "IHDR", 4) && clen >= 13) {
			w = be32(body), h = be32(body + 4);
			in.depth = body[8], in.color_type = body[9], interlace = body[12];
		} else if (!memcmp(type, "PLTE", 4)) {
			in.npalette = (int)MIN(clen / 3, 256u);
			for (int i = 0; i < in.npalette; i++) {
				in.palette[i][0] = body[i * 3], in.palette[i][1] = body[i * 3 + 1];
				in.palette[i][2] = body[i * 3 + 2], in.palette[i][3] = 255;
			}
		} else if (!memcmp(type, "tRNS", 4)) {
			if (in.color_type == 3) {
				for (u32 i = 0; i < clen && i < 256; i++)
					in.palette[i][3] = body[i];
			} else {
				in.has_trns = true;
				for (u32 i = 0; i < 3 && i * 2 + 1 < clen; i++)
					in.trns[i] = (u16)(body[i * 2] << 8 | body[i * 2 + 1]);
			}
		} else if (!memcmp(type, "IDAT", 4)) {
			sb_add(&idat, body, clen);
		} else if (!memcmp(type, "IEND", 4)) {
			break;
		}
		p += 12 + clen;
	}
	static const int channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
	if (!w || !h || w > 16384 || h > 16384 || in.color_type > 6 || !channels[in.color_type] || !idat.len) {
		sb_free(&idat);
		return NULL;
	}
	in.channels = channels[in.color_type];
	usize raw_len;
	u8 *raw = zlib_inflate((u8 *)idat.data, idat.len, &raw_len, true);
	sb_free(&idat);
	if (!raw)
		return NULL;
	usize bits_pp = (usize)in.channels * (usize)in.depth;
	usize bpp = MAX(1u, bits_pp / 8);
	Canvas *c = canvas_new((int)w, (int)h);
	bool ok = true;
	if (!interlace) {
		usize rowbytes = (w * bits_pp + 7) / 8;
		if (raw_len < h * (rowbytes + 1)) {
			ok = false;
		} else {
			u8 *img = mem_alloc(h * rowbytes);
			ok = unfilter(raw, h, rowbytes, bpp, img);
			for (u32 y = 0; ok && y < h; y++)
				for (u32 x = 0; x < w; x++)
					canvas_row(c, (int)y)[x] = png_pixel(&in, img + y * rowbytes, x);
			mem_free(img);
		}
	} else {
		/* Adam7: seven passes over a sparse grid. */
		static const int sx[7] = { 0, 4, 0, 2, 0, 1, 0 }, sy[7] = { 0, 0, 4, 0, 2, 0, 1 };
		static const int dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
		usize off = 0;
		for (int pass = 0; ok && pass < 7; pass++) {
			u32 pw = (w - (u32)sx[pass] + (u32)dx[pass] - 1) / (u32)dx[pass];
			u32 ph = (h - (u32)sy[pass] + (u32)dy[pass] - 1) / (u32)dy[pass];
			if ((u32)sx[pass] >= w)
				pw = 0;
			if ((u32)sy[pass] >= h)
				ph = 0;
			if (!pw || !ph)
				continue;
			usize rowbytes = (pw * bits_pp + 7) / 8;
			if (off + ph * (rowbytes + 1) > raw_len) {
				ok = false;
				break;
			}
			u8 *img = mem_alloc(ph * rowbytes);
			ok = unfilter(raw + off, ph, rowbytes, bpp, img);
			for (u32 y = 0; ok && y < ph; y++)
				for (u32 x = 0; x < pw; x++)
					canvas_row(c, (int)(sy[pass] + y * (u32)dy[pass]))[sx[pass] + x * (u32)dx[pass]] =
						png_pixel(&in, img + y * rowbytes, x);
			mem_free(img);
			off += ph * (rowbytes + 1);
		}
	}
	mem_free(raw);
	if (!ok) {
		canvas_free(c);
		return NULL;
	}
	return c;
}

static void put_chunk(StrBuf *sb, const char *type, const u8 *data, u32 len)
{
	u8 hdr[8];
	put_be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	sb_add(sb, hdr, 8);
	if (len)
		sb_add(sb, data, len);
	u32 crc = crc32_update(0, type, 4);
	crc = crc32_update(crc, data, len);
	u8 c[4];
	put_be32(c, crc);
	sb_add(sb, c, 4);
}

u8 *png_encode(const Canvas *c, usize *out_len)
{
	int w = c->width, h = c->height;
	usize rowbytes = (usize)w * 4;
	u8 *raw = mem_alloc((rowbytes + 1) * (usize)h);
	u8 *prev = mem_zalloc(rowbytes), *cur = mem_alloc(rowbytes);
	for (int y = 0; y < h; y++) {
		const u32 *row = canvas_row(c, y);
		for (int x = 0; x < w; x++) {
			Color px = color_unpremul(row[x]);
			cur[x * 4] = (u8)COLOR_R(px), cur[x * 4 + 1] = (u8)COLOR_G(px);
			cur[x * 4 + 2] = (u8)COLOR_B(px), cur[x * 4 + 3] = (u8)COLOR_A(px);
		}
		/* "Up" filter: cheap and effective for UI images. */
		u8 *dst = raw + (usize)y * (rowbytes + 1);
		dst[0] = 2;
		for (usize i = 0; i < rowbytes; i++)
			dst[1 + i] = (u8)(cur[i] - prev[i]);
		u8 *t = prev;
		prev = cur;
		cur = t;
	}
	mem_free(prev);
	mem_free(cur);
	usize zlen;
	u8 *z = zlib_deflate(raw, (rowbytes + 1) * (usize)h, &zlen);
	mem_free(raw);
	StrBuf sb = { 0 };
	static const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	sb_add(&sb, sig, 8);
	u8 ihdr[13];
	put_be32(ihdr, (u32)w);
	put_be32(ihdr + 4, (u32)h);
	ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
	put_chunk(&sb, "IHDR", ihdr, 13);
	put_chunk(&sb, "IDAT", z, (u32)zlen);
	put_chunk(&sb, "IEND", NULL, 0);
	mem_free(z);
	*out_len = sb.len;
	return (u8 *)sb_take(&sb);
}

bool png_save(const Canvas *c, const char *path)
{
	usize len;
	u8 *data = png_encode(c, &len);
	bool ok = file_write_atomic(path, data, len);
	mem_free(data);
	return ok;
}
