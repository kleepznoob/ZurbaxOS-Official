/*
 * JPEG decoder: baseline and progressive Huffman-coded images, any chroma
 * subsampling, restart intervals, EXIF orientation, and fast reduced-size
 * decoding for thumbnails.
 *
 * All scans decode into per-component coefficient buffers; the inverse DCT,
 * upsampling and color conversion run once at the end. That one path handles
 * both baseline and progressive files.
 */
#include "zg.h"

static const u8 zigzag[64 + 16] = {
	0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
	/* padding so a corrupt run cannot index past the table */
	63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

typedef struct {
	u8 fast[512];      /* 9-bit lookup: symbol index + 1, or 0 */
	u8 fast_len[512];
	u16 code[256];
	u8 size[256];
	u8 values[256];
	i32 maxcode[18];
	i32 delta[17];
	bool present;
} Huff;

typedef struct {
	int id, h, v, tq;
	int bw, bh;        /* blocks per row / column, padded to whole MCUs */
	i16 *coef;         /* bw*bh blocks of 64 coefficients (natural order) */
	int dc_tbl, ac_tbl;
	int dc_pred;
	u8 *plane;         /* decoded samples */
	int pw, ph;        /* plane size */
} Comp;

typedef struct {
	const u8 *p, *end;
	u32 bits;
	int nbits;
	bool marker_hit;
	bool error;

	int width, height;
	int ncomp;
	Comp comp[4];
	int hmax, vmax, mcux, mcuy;
	bool progressive;
	u16 qt[4][64];
	Huff dc[4], ac[4];
	int restart_interval;
	int eobrun;
	int ss, se, ah, al;
	int scan_comp[4], scan_n;
	int orientation;
	bool adobe;
	int adobe_transform;
} Jpeg;

/* ---- Bit reading ---------------------------------------------------------------- */

static void fill_bits(Jpeg *j)
{
	while (j->nbits <= 24) {
		u32 b = 0;
		if (!j->marker_hit && j->p < j->end) {
			b = *j->p++;
			if (b == 0xff) {
				u8 next = j->p < j->end ? *j->p : 0;
				if (next == 0) {
					j->p++;
				} else {
					/* A marker: stop consuming, feed zeros from here on. */
					j->p--;
					j->marker_hit = true;
					b = 0;
				}
			}
		}
		j->bits |= b << (24 - j->nbits);
		j->nbits += 8;
	}
}

static u32 get_bits(Jpeg *j, int n)
{
	if (n == 0)
		return 0;
	fill_bits(j);
	u32 v = j->bits >> (32 - n);
	j->bits <<= n;
	j->nbits -= n;
	return v;
}

static int get_bit(Jpeg *j)
{
	return (int)get_bits(j, 1);
}

static int extend(u32 v, int s)
{
	return s == 0 ? 0 : (v < (1u << (s - 1)) ? (int)v - (1 << s) + 1 : (int)v);
}

static void build_huff(Huff *h, const u8 counts[16], const u8 *values)
{
	int k = 0, code = 0;
	for (int len = 1; len <= 16; len++) {
		h->delta[len] = k - code;
		for (int i = 0; i < counts[len - 1]; i++) {
			h->size[k] = (u8)len;
			h->code[k] = (u16)code++;
			k++;
		}
		h->maxcode[len] = code << (16 - len); /* exclusive, left-aligned */
		code <<= 1;
	}
	h->maxcode[17] = 0x7fffffff;
	memcpy(h->values, values, (usize)k);
	memset(h->fast, 0, sizeof h->fast);
	for (int i = 0; i < k; i++) {
		if (h->size[i] <= 9) {
			int shift = 9 - h->size[i];
			int base = h->code[i] << shift;
			for (int m = 0; m < (1 << shift); m++) {
				h->fast[base + m] = (u8)(i + 1);
				h->fast_len[base + m] = h->size[i];
			}
		}
	}
	h->present = true;
}

static int huff_decode(Jpeg *j, const Huff *h)
{
	fill_bits(j);
	u32 top = j->bits >> 23; /* 9 bits */
	int f = h->fast[top];
	if (f) {
		int len = h->fast_len[top];
		j->bits <<= len;
		j->nbits -= len;
		return h->values[f - 1];
	}
	u32 code16 = j->bits >> 16;
	int len;
	for (len = 10; len <= 16; len++)
		if ((i32)code16 < h->maxcode[len])
			break;
	if (len > 16) {
		j->error = true;
		return 0;
	}
	int idx = (int)(code16 >> (16 - len)) + h->delta[len];
	j->bits <<= len;
	j->nbits -= len;
	if (idx < 0 || idx > 255) {
		j->error = true;
		return 0;
	}
	return h->values[idx];
}

/* ---- Block decoding ---------------------------------------------------------------- */

static void decode_block_baseline(Jpeg *j, Comp *c, i16 *blk)
{
	int s = huff_decode(j, &j->dc[c->dc_tbl]);
	int diff = extend(get_bits(j, s), s);
	c->dc_pred += diff;
	blk[0] = (i16)(c->dc_pred);
	const Huff *ac = &j->ac[c->ac_tbl];
	for (int k = 1; k < 64;) {
		int rs = huff_decode(j, ac);
		int r = rs >> 4;
		s = rs & 15;
		if (s == 0) {
			if (r != 15)
				break;
			k += 16;
			continue;
		}
		k += r;
		if (k > 63)
			break;
		blk[zigzag[k++]] = (i16)extend(get_bits(j, s), s);
	}
}

static void decode_dc_progressive(Jpeg *j, Comp *c, i16 *blk)
{
	if (j->ah == 0) {
		int s = huff_decode(j, &j->dc[c->dc_tbl]);
		c->dc_pred += extend(get_bits(j, s), s);
		blk[0] = (i16)(c->dc_pred * (1 << j->al));
	} else if (get_bit(j)) {
		blk[0] = (i16)(blk[0] | (1 << j->al));
	}
}

static void decode_ac_first(Jpeg *j, Comp *c, i16 *blk)
{
	if (j->eobrun) {
		j->eobrun--;
		return;
	}
	const Huff *ac = &j->ac[c->ac_tbl];
	for (int k = j->ss; k <= j->se;) {
		int rs = huff_decode(j, ac);
		int r = rs >> 4, s = rs & 15;
		if (s == 0) {
			if (r < 15) {
				j->eobrun = (1 << r) - 1;
				if (r)
					j->eobrun += (int)get_bits(j, r);
				break;
			}
			k += 16;
		} else {
			k += r;
			if (k > 63)
				break;
			blk[zigzag[k++]] = (i16)(extend(get_bits(j, s), s) * (1 << j->al));
		}
	}
}

static void refine_coef(Jpeg *j, i16 *p, int bit)
{
	if (get_bit(j) && (*p & bit) == 0)
		*p = (i16)(*p > 0 ? *p + bit : *p - bit);
}

static void decode_ac_refine(Jpeg *j, Comp *c, i16 *blk)
{
	int bit = 1 << j->al;
	if (j->eobrun) {
		j->eobrun--;
		for (int k = j->ss; k <= j->se; k++)
			if (blk[zigzag[k]])
				refine_coef(j, &blk[zigzag[k]], bit);
		return;
	}
	const Huff *ac = &j->ac[c->ac_tbl];
	int k = j->ss;
	while (k <= j->se) {
		int rs = huff_decode(j, ac);
		int r = rs >> 4, s = rs & 15;
		if (s == 0) {
			if (r < 15) {
				j->eobrun = (1 << r) - 1;
				if (r)
					j->eobrun += (int)get_bits(j, r);
				r = 64; /* refine everything left in this block */
			}
		} else {
			s = get_bit(j) ? bit : -bit;
		}
		/* Skip r zero coefficients, refining the non-zero ones passed on the way. */
		while (k <= j->se) {
			i16 *p = &blk[zigzag[k++]];
			if (*p) {
				refine_coef(j, p, bit);
			} else {
				if (r == 0) {
					*p = (i16)s;
					break;
				}
				r--;
			}
		}
		if (j->error)
			return;
	}
}

/* ---- Markers ------------------------------------------------------------------------- */

static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

static void parse_exif(Jpeg *j, const u8 *d, usize len)
{
	if (len < 14 || memcmp(d, "Exif\0\0", 6) != 0)
		return;
	const u8 *t = d + 6;
	usize tl = len - 6;
	bool le = t[0] == 'I';
	#define R16(o) ((u16)(le ? (t[o] | t[(o) + 1] << 8) : (t[o] << 8 | t[(o) + 1])))
	#define R32(o) ((u32)(le ? (t[o] | t[(o) + 1] << 8 | t[(o) + 2] << 16 | (u32)t[(o) + 3] << 24) \
				 : ((u32)t[o] << 24 | t[(o) + 1] << 16 | t[(o) + 2] << 8 | t[(o) + 3])))
	u32 ifd = R32(4);
	if (ifd + 2 > tl)
		return;
	u16 n = R16(ifd);
	for (u16 i = 0; i < n; i++) {
		u32 e = ifd + 2 + i * 12u;
		if (e + 12 > tl)
			break;
		if (R16(e) == 0x0112) {
			j->orientation = R16(e + 8);
			break;
		}
	}
	#undef R16
	#undef R32
}

static bool parse_sof(Jpeg *j, const u8 *d, usize len)
{
	if (len < 6 || d[0] != 8)
		return false;
	j->height = be16(d + 1);
	j->width = be16(d + 3);
	j->ncomp = d[5];
	if (!j->width || !j->height || j->width > 16384 || j->height > 16384 ||
	    (j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) || len < 6 + 3 * (usize)j->ncomp)
		return false;
	j->hmax = j->vmax = 1;
	for (int i = 0; i < j->ncomp; i++) {
		Comp *c = &j->comp[i];
		c->id = d[6 + i * 3];
		c->h = d[7 + i * 3] >> 4;
		c->v = d[7 + i * 3] & 15;
		c->tq = d[8 + i * 3] & 3;
		if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
			return false;
		j->hmax = MAX(j->hmax, c->h);
		j->vmax = MAX(j->vmax, c->v);
	}
	j->mcux = (j->width + 8 * j->hmax - 1) / (8 * j->hmax);
	j->mcuy = (j->height + 8 * j->vmax - 1) / (8 * j->vmax);
	for (int i = 0; i < j->ncomp; i++) {
		Comp *c = &j->comp[i];
		c->bw = j->mcux * c->h;
		c->bh = j->mcuy * c->v;
		c->coef = mem_zalloc((usize)c->bw * (usize)c->bh * 64 * sizeof(i16));
	}
	return true;
}

static void reset_restart(Jpeg *j)
{
	j->bits = 0;
	j->nbits = 0;
	j->marker_hit = false;
	j->eobrun = 0;
	for (int i = 0; i < j->ncomp; i++)
		j->comp[i].dc_pred = 0;
}

/* Called between restart intervals: skips the RSTn marker. */
static void handle_restart(Jpeg *j)
{
	/* Find the marker the bit reader stopped at. */
	while (j->p + 1 < j->end && !(j->p[0] == 0xff && j->p[1] >= 0xd0 && j->p[1] <= 0xd7)) {
		if (j->p[0] == 0xff && j->p[1] != 0 && j->p[1] != 0xff)
			break; /* some other marker: let the main loop handle it */
		j->p++;
	}
	if (j->p + 1 < j->end && j->p[0] == 0xff && j->p[1] >= 0xd0 && j->p[1] <= 0xd7)
		j->p += 2;
	reset_restart(j);
}

static void decode_scan(Jpeg *j)
{
	reset_restart(j);
	int restarts_left = j->restart_interval;
	void (*fn)(Jpeg *, Comp *, i16 *);
	if (!j->progressive)
		fn = decode_block_baseline;
	else if (j->ss == 0)
		fn = decode_dc_progressive;
	else
		fn = j->ah == 0 ? decode_ac_first : decode_ac_refine;

	if (j->scan_n == 1) {
		/* Non-interleaved: blocks in raster order over the component's own size. */
		Comp *c = &j->comp[j->scan_comp[0]];
		int cw = (j->width * c->h + j->hmax - 1) / j->hmax;
		int ch = (j->height * c->v + j->vmax - 1) / j->vmax;
		int nbx = (cw + 7) / 8, nby = (ch + 7) / 8;
		for (int by = 0; by < nby; by++) {
			for (int bx = 0; bx < nbx; bx++) {
				if (j->restart_interval && restarts_left-- == 0) {
					handle_restart(j);
					restarts_left = j->restart_interval - 1;
				}
				fn(j, c, c->coef + ((usize)by * c->bw + bx) * 64);
				if (j->error)
					return;
			}
		}
		return;
	}
	for (int my = 0; my < j->mcuy; my++) {
		for (int mx = 0; mx < j->mcux; mx++) {
			if (j->restart_interval && restarts_left-- == 0) {
				handle_restart(j);
				restarts_left = j->restart_interval - 1;
			}
			for (int s = 0; s < j->scan_n; s++) {
				Comp *c = &j->comp[j->scan_comp[s]];
				for (int v = 0; v < c->v; v++)
					for (int h = 0; h < c->h; h++) {
						int bx = mx * c->h + h, by = my * c->v + v;
						fn(j, c, c->coef + ((usize)by * c->bw + bx) * 64);
					}
			}
			if (j->error)
				return;
		}
	}
}

/* ---- Inverse DCT (AAN, floating point) ------------------------------------------------- */

static const f32 aan[8] = { 1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
			    1.0f, 0.785694958f, 0.541196100f, 0.275899379f };

static void idct_block(const i16 *in, const f32 *q, u8 *out, int out_stride)
{
	f32 ws[64];
	for (int col = 0; col < 8; col++) {
		const i16 *c = in + col;
		const f32 *qq = q + col;
		if (!c[8] && !c[16] && !c[24] && !c[32] && !c[40] && !c[48] && !c[56]) {
			f32 dc = c[0] * qq[0];
			for (int r = 0; r < 8; r++)
				ws[r * 8 + col] = dc;
			continue;
		}
		f32 t0 = c[0] * qq[0], t1 = c[16] * qq[16], t2 = c[32] * qq[32], t3 = c[48] * qq[48];
		f32 t10 = t0 + t2, t11 = t0 - t2, t13 = t1 + t3, t12 = (t1 - t3) * 1.414213562f - t13;
		t0 = t10 + t13, t3 = t10 - t13, t1 = t11 + t12, t2 = t11 - t12;
		f32 t4 = c[8] * qq[8], t5 = c[24] * qq[24], t6 = c[40] * qq[40], t7 = c[56] * qq[56];
		f32 z13 = t6 + t5, z10 = t6 - t5, z11 = t4 + t7, z12 = t4 - t7;
		t7 = z11 + z13;
		t11 = (z11 - z13) * 1.414213562f;
		f32 z5 = (z10 + z12) * 1.847759065f;
		t10 = 1.082392200f * z12 - z5;
		t12 = -2.613125930f * z10 + z5;
		t6 = t12 - t7, t5 = t11 - t6, t4 = t10 + t5;
		ws[0 * 8 + col] = t0 + t7, ws[7 * 8 + col] = t0 - t7;
		ws[1 * 8 + col] = t1 + t6, ws[6 * 8 + col] = t1 - t6;
		ws[2 * 8 + col] = t2 + t5, ws[5 * 8 + col] = t2 - t5;
		ws[4 * 8 + col] = t3 + t4, ws[3 * 8 + col] = t3 - t4;
	}
	for (int row = 0; row < 8; row++) {
		f32 *w = ws + row * 8;
		u8 *o = out + row * out_stride;
		f32 t10 = w[0] + w[4], t11 = w[0] - w[4];
		f32 t13 = w[2] + w[6], t12 = (w[2] - w[6]) * 1.414213562f - t13;
		f32 t0 = t10 + t13, t3 = t10 - t13, t1 = t11 + t12, t2 = t11 - t12;
		f32 z13 = w[5] + w[3], z10 = w[5] - w[3], z11 = w[1] + w[7], z12 = w[1] - w[7];
		f32 t7 = z11 + z13, t11b = (z11 - z13) * 1.414213562f;
		f32 z5 = (z10 + z12) * 1.847759065f;
		f32 t10b = 1.082392200f * z12 - z5, t12b = -2.613125930f * z10 + z5;
		f32 t6 = t12b - t7, t5 = t11b - t6, t4 = t10b + t5;
		f32 v[8] = { t0 + t7, t1 + t6, t2 + t5, t3 - t4, t3 + t4, t2 - t5, t1 - t6, t0 - t7 };
		for (int i = 0; i < 8; i++) {
			int x = (int)(v[i] / 8 + 128.5f);
			o[i] = (u8)CLAMP(x, 0, 255);
		}
	}
}

/* ---- Reconstruction ------------------------------------------------------------------- */

static void build_planes(Jpeg *j, int scale)
{
	for (int i = 0; i < j->ncomp; i++) {
		Comp *c = &j->comp[i];
		int bs = 8 / scale; /* output pixels per block side */
		c->pw = c->bw * bs;
		c->ph = c->bh * bs;
		c->plane = mem_alloc((usize)c->pw * (usize)c->ph);
		f32 q[64];
		for (int k = 0; k < 64; k++) {
			int r = k / 8, col = k % 8;
			q[k] = (f32)j->qt[c->tq][k] * aan[r] * aan[col];
		}
		u8 tmp[64];
		for (int by = 0; by < c->bh; by++) {
			for (int bx = 0; bx < c->bw; bx++) {
				const i16 *blk = c->coef + ((usize)by * c->bw + bx) * 64;
				u8 *dst = c->plane + (usize)by * bs * c->pw + (usize)bx * bs;
				if (scale == 8) {
					int v = (int)(blk[0] * (f32)j->qt[c->tq][0] / 8 + 128.5f);
					*dst = (u8)CLAMP(v, 0, 255);
					continue;
				}
				if (scale == 1) {
					idct_block(blk, q, dst, c->pw);
					continue;
				}
				idct_block(blk, q, tmp, 8);
				for (int y = 0; y < bs; y++)
					for (int x = 0; x < bs; x++) {
						int sum = 0;
						for (int yy = 0; yy < scale; yy++)
							for (int xx = 0; xx < scale; xx++)
								sum += tmp[(y * scale + yy) * 8 + x * scale + xx];
						dst[y * c->pw + x] = (u8)(sum / (scale * scale));
					}
			}
		}
		mem_free(c->coef);
		c->coef = NULL;
	}
}

/* Samples a component plane at output pixel (x, y) with bilinear upsampling. */
static inline int sample_plane(const Comp *c, int x, int y, int hmax, int vmax)
{
	if (c->h == hmax && c->v == vmax)
		return c->plane[(usize)y * c->pw + x];
	f32 fx = ((f32)x + 0.5f) * (f32)c->h / (f32)hmax - 0.5f;
	f32 fy = ((f32)y + 0.5f) * (f32)c->v / (f32)vmax - 0.5f;
	int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
	f32 ax = fx - (f32)x0, ay = fy - (f32)y0;
	int x1 = MIN(x0 + 1, c->pw - 1), y1 = MIN(y0 + 1, c->ph - 1);
	x0 = MAX(x0, 0), y0 = MAX(y0, 0);
	f32 top = c->plane[(usize)y0 * c->pw + x0] * (1 - ax) + c->plane[(usize)y0 * c->pw + x1] * ax;
	f32 bot = c->plane[(usize)y1 * c->pw + x0] * (1 - ax) + c->plane[(usize)y1 * c->pw + x1] * ax;
	return (int)(top * (1 - ay) + bot * ay + 0.5f);
}

static Canvas *apply_orientation(Canvas *c, int o)
{
	if (o < 2 || o > 8)
		return c;
	bool swap = o >= 5;
	int w = c->width, h = c->height;
	Canvas *r = canvas_new(swap ? h : w, swap ? w : h);
	for (int y = 0; y < h; y++) {
		const u32 *src = canvas_row(c, y);
		for (int x = 0; x < w; x++) {
			int nx, ny;
			switch (o) {
			case 2: nx = w - 1 - x, ny = y; break;
			case 3: nx = w - 1 - x, ny = h - 1 - y; break;
			case 4: nx = x, ny = h - 1 - y; break;
			case 5: nx = y, ny = x; break;
			case 6: nx = h - 1 - y, ny = x; break;
			case 7: nx = h - 1 - y, ny = w - 1 - x; break;
			default: nx = y, ny = w - 1 - x; break; /* 8 */
			}
			canvas_row(r, ny)[nx] = src[x];
		}
	}
	canvas_free(c);
	return r;
}

static void free_jpeg(Jpeg *j)
{
	for (int i = 0; i < 4; i++) {
		mem_free(j->comp[i].coef);
		mem_free(j->comp[i].plane);
	}
}

Canvas *jpeg_decode(const u8 *data, usize len, int scale)
{
	if (scale != 1 && scale != 2 && scale != 4 && scale != 8)
		scale = 1;
	if (len < 4 || data[0] != 0xff || data[1] != 0xd8)
		return NULL;
	Jpeg *j = NEW(Jpeg);
	j->end = data + len;
	const u8 *p = data + 2;
	bool have_frame = false, have_scan = false;
	while (p + 4 <= j->end && !j->error) {
		if (p[0] != 0xff) {
			p++;
			continue;
		}
		u8 m = p[1];
		if (m == 0xff) {
			p++;
			continue;
		}
		if (m == 0xd9)
			break;
		if (m >= 0xd0 && m <= 0xd7) {
			p += 2;
			continue;
		}
		usize seg = be16(p + 2);
		const u8 *d = p + 4;
		if (seg < 2 || p + 2 + seg > j->end)
			break;
		usize dl = seg - 2;
		switch (m) {
		case 0xc0:
		case 0xc1:
		case 0xc2:
			j->progressive = m == 0xc2;
			if (have_frame || !parse_sof(j, d, dl))
				j->error = true;
			have_frame = true;
			break;
		case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf:
			j->error = true; /* lossless / arithmetic coding: not supported */
			break;
		case 0xc4: {
			usize o = 0;
			while (o + 17 <= dl) {
				int tc = d[o] >> 4, th = d[o] & 3;
				const u8 *counts = d + o + 1;
				usize total = 0;
				for (int i = 0; i < 16; i++)
					total += counts[i];
				if (total > 256 || o + 17 + total > dl) {
					j->error = true;
					break;
				}
				build_huff(tc ? &j->ac[th] : &j->dc[th], counts, d + o + 17);
				o += 17 + total;
			}
			break;
		}
		case 0xdb: {
			usize o = 0;
			while (o < dl) {
				int pq = d[o] >> 4, tq = d[o] & 3;
				o++;
				for (int k = 0; k < 64; k++) {
					u16 v = pq ? be16(d + o + k * 2) : d[o + k];
					j->qt[tq][zigzag[k]] = v;
				}
				o += pq ? 128 : 64;
			}
			break;
		}
		case 0xdd:
			j->restart_interval = be16(d);
			break;
		case 0xe1:
			parse_exif(j, d, dl);
			break;
		case 0xee:
			if (dl >= 12 && memcmp(d, "Adobe", 5) == 0) {
				j->adobe = true;
				j->adobe_transform = d[11];
			}
			break;
		case 0xda: {
			if (!have_frame) {
				j->error = true;
				break;
			}
			j->scan_n = d[0];
			if (j->scan_n < 1 || j->scan_n > 4 || dl < 4 + 2 * (usize)j->scan_n) {
				j->error = true;
				break;
			}
			for (int i = 0; i < j->scan_n; i++) {
				int id = d[1 + i * 2], tbl = d[2 + i * 2];
				int idx = -1;
				for (int k = 0; k < j->ncomp; k++)
					if (j->comp[k].id == id)
						idx = k;
				if (idx < 0) {
					j->error = true;
					break;
				}
				j->scan_comp[i] = idx;
				j->comp[idx].dc_tbl = tbl >> 4 & 3;
				j->comp[idx].ac_tbl = tbl & 3;
			}
			const u8 *t = d + 1 + 2 * j->scan_n;
			j->ss = t[0], j->se = MIN(t[1], 63), j->ah = t[2] >> 4, j->al = t[2] & 15;
			if (!j->progressive)
				j->ss = 0, j->se = 63, j->ah = j->al = 0;
			j->p = p + 2 + seg;
			decode_scan(j);
			have_scan = true;
			/* Continue after the entropy-coded data. */
			p = j->p;
			while (p + 1 < j->end && !(p[0] == 0xff && p[1] != 0 && !(p[1] >= 0xd0 && p[1] <= 0xd7)))
				p++;
			continue;
		}
		default:
			break;
		}
		p += 2 + seg;
	}
	if (j->error || !have_frame || !have_scan) {
		free_jpeg(j);
		mem_free(j);
		return NULL;
	}
	build_planes(j, scale);
	int ow = (j->width + scale - 1) / scale, oh = (j->height + scale - 1) / scale;
	Canvas *c = canvas_new(ow, oh);
	bool ycc = j->ncomp == 3 && !(j->adobe && j->adobe_transform == 0);
	for (int y = 0; y < oh; y++) {
		u32 *row = canvas_row(c, y);
		for (int x = 0; x < ow; x++) {
			int r, g, b;
			if (j->ncomp == 1) {
				r = g = b = j->comp[0].plane[(usize)y * j->comp[0].pw + x];
			} else {
				int c0 = sample_plane(&j->comp[0], x, y, j->hmax, j->vmax);
				int c1 = sample_plane(&j->comp[1], x, y, j->hmax, j->vmax);
				int c2 = sample_plane(&j->comp[2], x, y, j->hmax, j->vmax);
				if (ycc || j->ncomp == 4) {
					int cb = c1 - 128, cr = c2 - 128;
					r = c0 + ((91881 * cr + 32768) >> 16);
					g = c0 - ((22554 * cb + 46802 * cr - 32768) >> 16);
					b = c0 + ((116130 * cb + 32768) >> 16);
				} else {
					r = c0, g = c1, b = c2;
				}
				if (j->ncomp == 4) {
					/* Adobe CMYK/YCCK, stored inverted. */
					int k = sample_plane(&j->comp[3], x, y, j->hmax, j->vmax);
					r = CLAMP(r, 0, 255) * k / 255;
					g = CLAMP(g, 0, 255) * k / 255;
					b = CLAMP(b, 0, 255) * k / 255;
				}
			}
			row[x] = RGB(CLAMP(r, 0, 255), CLAMP(g, 0, 255), CLAMP(b, 0, 255));
		}
	}
	int orientation = j->orientation;
	free_jpeg(j);
	mem_free(j);
	return apply_orientation(c, orientation);
}
