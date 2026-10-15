/* Shapes, shadows, blur and image blits built on the rasterizer. */
#include "zg.h"
/* SSE2 intrinsics without the C library: their header would pull in malloc.h */
#define _MM_MALLOC_H_INCLUDED
#include <emmintrin.h>

static Rect clip_of(const Canvas *c)
{
	return rect_intersect(c->clip, rect(0, 0, (f32)c->width, (f32)c->height));
}

void fill_rect_paint(Canvas *c, Rect r, const Paint *paint)
{
	Path p = { 0 };
	path_rect(&p, r);
	fill_path(c, &p, mat_identity(), paint, FILL_NONZERO);
	path_free(&p);
}

static void solid_round_rect(Canvas *c, Rect r, f32 radius, f32 ring, u32 src);

/* Solid rectangles, rounded or not, and their outlines skip the rasterizer:
 * coverage comes from the shape's distance, the inside is blended four
 * pixels at a time (solid_round_rect, below). */
void fill_rect(Canvas *c, Rect r, Color color)
{
	solid_round_rect(c, r, 0, 0, color_premul(color));
}

void fill_round_rect_paint(Canvas *c, Rect r, f32 radius, const Paint *paint)
{
	Path p = { 0 };
	path_round_rect(&p, r, radius);
	fill_path(c, &p, mat_identity(), paint, FILL_NONZERO);
	path_free(&p);
}

void fill_round_rect(Canvas *c, Rect r, f32 radius, Color color)
{
	solid_round_rect(c, r, maxf(radius, 0), 0, color_premul(color));
}

/* Appends every contour of `src` to `dst`. */
static void path_append(Path *dst, const Path *src)
{
	usize pi = 0;
	const Vec2 *q = src->pts.data;
	for (usize i = 0; i < src->verbs.len; i++) {
		switch (src->verbs.data[i]) {
		case PATH_MOVE: path_move(dst, q[pi].x, q[pi].y); pi++; break;
		case PATH_LINE: path_line(dst, q[pi].x, q[pi].y); pi++; break;
		case PATH_QUAD: path_quad(dst, q[pi].x, q[pi].y, q[pi + 1].x, q[pi + 1].y); pi += 2; break;
		case PATH_CUBIC:
			path_cubic(dst, q[pi].x, q[pi].y, q[pi + 1].x, q[pi + 1].y, q[pi + 2].x, q[pi + 2].y);
			pi += 3;
			break;
		case PATH_CLOSE: path_close(dst); break;
		}
	}
}

void stroke_round_rect(Canvas *c, Rect r, f32 radius, f32 width, Color color)
{
	/* The ring between two rounded rectangles: crisp corners, even width. */
	if (width > 0 && width < minf(r.w, r.h) / 2) {
		solid_round_rect(c, r, maxf(radius, 0), width, color_premul(color));
		return;
	}
	Paint paint = paint_solid(color);
	Path ring = { 0 };
	path_round_rect(&ring, r, radius);
	Rect in = rect_inset(r, width, width);
	if (!rect_empty(in)) {
		Path inner = { 0 };
		path_round_rect(&inner, in, maxf(0, radius - width));
		path_append(&ring, &inner);
		path_free(&inner);
	}
	fill_path(c, &ring, mat_identity(), &paint, FILL_EVENODD);
	path_free(&ring);
}

void fill_circle(Canvas *c, f32 cx, f32 cy, f32 r, Color color)
{
	Path p = { 0 };
	path_circle(&p, cx, cy, r);
	Paint paint = paint_solid(color);
	fill_path(c, &p, mat_identity(), &paint, FILL_NONZERO);
	path_free(&p);
}

void stroke_circle(Canvas *c, f32 cx, f32 cy, f32 r, f32 width, Color color)
{
	Path p = { 0 };
	path_circle(&p, cx, cy, r + width / 2);
	if (r - width / 2 > 0)
		path_circle(&p, cx, cy, r - width / 2);
	Paint paint = paint_solid(color);
	fill_path(c, &p, mat_identity(), &paint, FILL_EVENODD);
	path_free(&p);
}

void draw_line(Canvas *c, f32 x0, f32 y0, f32 x1, f32 y1, f32 width, Color color)
{
	Path p = { 0 };
	path_move(&p, x0, y0);
	path_line(&p, x1, y1);
	Paint paint = paint_solid(color);
	StrokeStyle st = { width, CAP_ROUND, JOIN_ROUND, 4 };
	stroke_path(c, &p, mat_identity(), &paint, &st);
	path_free(&p);
}

/* ---- Blur ------------------------------------------------------------------------- */

static void box_blur_h(u32 *dst, const u32 *src, int w, int h, int stride_d, int stride_s, int r)
{
	if (r <= 0) {
		for (int y = 0; y < h; y++)
			memcpy(dst + (usize)y * stride_d, src + (usize)y * stride_s, (usize)w * 4);
		return;
	}
	u32 div = (u32)(2 * r + 1);
	u32 inv = (1u << 24) / div;
	for (int y = 0; y < h; y++) {
		const u32 *s = src + (usize)y * stride_s;
		u32 *d = dst + (usize)y * stride_d;
		u32 sum[4] = { 0 };
		for (int k = -r; k <= r; k++) {
			u32 p = s[CLAMP(k, 0, w - 1)];
			sum[0] += p & 0xff, sum[1] += (p >> 8) & 0xff, sum[2] += (p >> 16) & 0xff, sum[3] += p >> 24;
		}
		for (int x = 0; x < w; x++) {
			d[x] = ((sum[0] * inv) >> 24) | (((sum[1] * inv) >> 24) << 8) | (((sum[2] * inv) >> 24) << 16) |
			       (((sum[3] * inv) >> 24) << 24);
			u32 add = s[MIN(x + r + 1, w - 1)], sub = s[MAX(x - r, 0)];
			sum[0] += (add & 0xff) - (sub & 0xff);
			sum[1] += ((add >> 8) & 0xff) - ((sub >> 8) & 0xff);
			sum[2] += ((add >> 16) & 0xff) - ((sub >> 16) & 0xff);
			sum[3] += (add >> 24) - (sub >> 24);
		}
	}
}

static void transpose(u32 *dst, const u32 *src, int w, int h)
{
	/* src is w x h (stride w) -> dst is h x w (stride h) */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			dst[(usize)x * h + y] = src[(usize)y * w + x];
}

void blur_region(Canvas *c, Rect r, f32 radius)
{
	Rect cr = rect_intersect(r, rect(0, 0, (f32)c->width, (f32)c->height));
	int x0 = (int)floorf(cr.x), y0 = (int)floorf(cr.y);
	int w = (int)ceilf(cr.x + cr.w) - x0, h = (int)ceilf(cr.y + cr.h) - y0;
	if (w <= 0 || h <= 0 || radius < 0.5f)
		return;
	/* Three box passes approximate a Gaussian with sigma ~ radius/2. */
	int boxes[3];
	f32 sigma = radius / 2;
	f32 wIdeal = sqrtf(12 * sigma * sigma / 3 + 1);
	int wl = (int)floorf(wIdeal);
	if (wl % 2 == 0)
		wl--;
	int wu = wl + 2;
	f32 mIdeal = (12 * sigma * sigma - 3 * (f32)(wl * wl) - 12 * (f32)wl - 9) / (-4 * (f32)wl - 4);
	int m = (int)roundf(mIdeal);
	for (int i = 0; i < 3; i++)
		boxes[i] = ((i < m ? wl : wu) - 1) / 2;

	usize n = (usize)w * (usize)h;
	u32 *a = mem_alloc(n * 4), *b = mem_alloc(n * 4);
	u32 *base = canvas_row(c, y0) + x0;
	box_blur_h(a, base, w, h, w, c->stride, boxes[0]);
	box_blur_h(b, a, w, h, w, w, boxes[1]);
	box_blur_h(a, b, w, h, w, w, boxes[2]);
	transpose(b, a, w, h);
	box_blur_h(a, b, h, w, h, h, boxes[0]);
	box_blur_h(b, a, h, w, h, h, boxes[1]);
	box_blur_h(a, b, h, w, h, h, boxes[2]);
	transpose(b, a, h, w);
	for (int y = 0; y < h; y++)
		memcpy(base + (usize)y * c->stride, b + (usize)y * w, (usize)w * 4);
	mem_free(a);
	mem_free(b);
}

/* ---- Shadows -------------------------------------------------------------------------- */

static void blend_row(u32 *d, const u32 *s, int n, u32 k);

/*
 * A blurred rounded rectangle is its four corners, its four edges (the same
 * profile all along each one) and a solid middle. So one small picture is
 * blurred, once, for each radius, blur and colour, and any shadow of those
 * is pieced together from it: the corners copied, the edges' middle row and
 * column repeated, the middle filled. A shape too thin for that in one
 * direction (a taskbar: the blur reaches across it) gets a picture of its
 * exact size that way, and only the other direction is pieced. Shadows cost
 * a few blits, whatever their size, instead of a blur every frame. The
 * colour's alpha is applied as the pieces are blended, so a shadow fading
 * in doesn't make new pictures.
 */
typedef struct {
	int radius, blur, tw, th; /* tw, th: an exact width or height, 0 when pieced */
	u32 rgb;
	Canvas *tile;
	u64 used;
} ShadowTile;

static ShadowTile shadow_tiles[32];
static u64 shadow_clock;

static ShadowTile *shadow_tile(int radius, int blur, u32 rgb, int tw, int th, int side)
{
	ShadowTile *slot = &shadow_tiles[0];
	for (usize i = 0; i < ARRAY_LEN(shadow_tiles); i++) {
		ShadowTile *t = &shadow_tiles[i];
		if (t->tile && t->radius == radius && t->blur == blur && t->rgb == rgb && t->tw == tw && t->th == th) {
			t->used = ++shadow_clock;
			return t;
		}
		if (!t->tile || t->used < slot->used)
			slot = &shadow_tiles[i];
	}
	if (slot->tile)
		canvas_free(slot->tile);
	int margin = (int)ceilf((f32)blur * 1.5f) + 2;
	int w = tw ? tw : side, h = th ? th : side;
	Canvas *tile = canvas_new(w, h);
	fill_round_rect(tile, rect((f32)margin, (f32)margin, (f32)(w - 2 * margin), (f32)(h - 2 * margin)), (f32)radius, 0xff000000u | rgb);
	blur_region(tile, rect(0, 0, (f32)w, (f32)h), (f32)blur);
	*slot = (ShadowTile){ radius, blur, tw, th, rgb, tile, ++shadow_clock };
	return slot;
}

/* Part of src (sx, sy, w, h) blended at (dx, dy), at coverage k. */
static void blit_part(Canvas *c, const Canvas *src, int sx, int sy, int w, int h, int dx, int dy, u32 k)
{
	Rect clip = clip_of(c);
	int x0 = MAX(dx, (int)clip.x), y0 = MAX(dy, (int)clip.y);
	int x1 = MIN(dx + w, (int)(clip.x + clip.w)), y1 = MIN(dy + h, (int)(clip.y + clip.h));
	if (x1 <= x0)
		return;
	for (int y = y0; y < y1; y++)
		blend_row(canvas_row(c, y) + x0, canvas_row(src, sy + y - dy) + sx + x0 - dx, x1 - x0, k);
}

/* One premultiplied pixel blended over a rectangle, at coverage k. */
static void blend_fill(Canvas *c, int x, int y, int w, int h, u32 px, u32 k)
{
	Rect clip = clip_of(c);
	int x0 = MAX(x, (int)clip.x), y0 = MAX(y, (int)clip.y);
	int x1 = MIN(x + w, (int)(clip.x + clip.w)), y1 = MIN(y + h, (int)(clip.y + clip.h));
	if (x1 <= x0 || !(px >> 24))
		return;
	for (int yy = y0; yy < y1; yy++) {
		u32 *d = canvas_row(c, yy);
		for (int xx = x0; xx < x1; xx++)
			d[xx] = blend_premul(d[xx], px, k);
	}
}

/* How a dimension is pieced: up to three spans of the target, each either a
 * copy of the tile's (src..src+len) or a repeat of its column/row src. */
typedef struct {
	int dst, len, src;
	bool repeat;
} Span;

static int spans(Span *out, int total, int e, bool exact)
{
	if (exact) {
		out[0] = (Span){ 0, total, 0, false };
		return 1;
	}
	out[0] = (Span){ 0, e, 0, false };
	out[1] = (Span){ e, total - 2 * e, e, true };
	out[2] = (Span){ total - e, e, e + 1, false };
	return 3;
}

void draw_shadow(Canvas *c, Rect r, f32 radius, f32 blur, f32 dx, f32 dy, Color color)
{
	u32 alpha = COLOR_A(color);
	if (!alpha || r.w <= 0 || r.h <= 0)
		return;
	int rad = (int)roundf(radius), bl = (int)roundf(blur);
	int margin = (int)ceilf((f32)bl * 1.5f) + 2;
	int x = (int)floorf(r.x + dx) - margin, y = (int)floorf(r.y + dy) - margin;
	int w = (int)ceilf(r.w) + margin * 2, h = (int)ceilf(r.h) + margin * 2;
	/* from the picture's side to its middle: past the corner and the blur's reach */
	int e = margin + rad + margin, side = 2 * e + 1;
	bool exact_w = w < side, exact_h = h < side;
	ShadowTile *t = shadow_tile(rad, bl, color & 0xffffffu, exact_w ? w : 0, exact_h ? h : 0, side);
	const Canvas *s = t->tile;
	u32 k = (alpha * 256 + 127) / 255;
	Span xs[3], ys[3];
	int nx = spans(xs, w, e, exact_w), ny = spans(ys, h, e, exact_h);
	for (int j = 0; j < ny; j++) {
		for (int i = 0; i < nx; i++) {
			Span a = xs[i], b = ys[j];
			if (a.len <= 0 || b.len <= 0)
				continue;
			if (!a.repeat && !b.repeat) {
				blit_part(c, s, a.src, b.src, a.len, b.len, x + a.dst, y + b.dst, k);
			} else if (a.repeat && !b.repeat) {
				for (int row = 0; row < b.len; row++)
					blend_fill(c, x + a.dst, y + b.dst + row, a.len, 1, canvas_row(s, b.src + row)[a.src], k);
			} else if (!a.repeat && b.repeat) {
				for (int col = 0; col < a.len; col++)
					blend_fill(c, x + a.dst + col, y + b.dst, 1, b.len, canvas_row(s, b.src)[a.src + col], k);
			} else {
				blend_fill(c, x + a.dst, y + b.dst, a.len, b.len, canvas_row(s, b.src)[a.src], k);
			}
		}
	}
}

/* ---- Blits -------------------------------------------------------------------------------- */

/* Four premultiplied pixels over four, at coverage k (0..256), with SSE2
 * (every x86-64 has it): what moving and fading windows spend their time on. */
static inline __m128i blend4(__m128i d, __m128i s, __m128i k16)
{
	__m128i z = _mm_setzero_si128(), c255 = _mm_set1_epi16(255), c128 = _mm_set1_epi16(128);
	__m128i sl = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(s, z), k16), 8);
	__m128i sh = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(s, z), k16), 8);
	__m128i al = _mm_shufflehi_epi16(_mm_shufflelo_epi16(sl, 0xff), 0xff);
	__m128i ah = _mm_shufflehi_epi16(_mm_shufflelo_epi16(sh, 0xff), 0xff);
	__m128i tl = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(d, z), _mm_sub_epi16(c255, al)), c128);
	__m128i th = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(d, z), _mm_sub_epi16(c255, ah)), c128);
	tl = _mm_srli_epi16(_mm_add_epi16(tl, _mm_srli_epi16(tl, 8)), 8);
	th = _mm_srli_epi16(_mm_add_epi16(th, _mm_srli_epi16(th, 8)), 8);
	return _mm_packus_epi16(_mm_add_epi16(sl, tl), _mm_add_epi16(sh, th));
}

/* A row of src over dst at coverage k: four at a time, opaque ones copied, clear ones skipped. */
static void blend_row(u32 *d, const u32 *s, int n, u32 k)
{
	__m128i z = _mm_setzero_si128(), opaque = _mm_set1_epi32(255), k16 = _mm_set1_epi16((short)k);
	int x = 0;
	for (; x + 4 <= n; x += 4) {
		__m128i sv = _mm_loadu_si128((const __m128i *)(s + x));
		if (_mm_movemask_epi8(_mm_cmpeq_epi32(sv, z)) == 0xffff)
			continue;
		if (k == 256 && _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_srli_epi32(sv, 24), opaque)) == 0xffff) {
			_mm_storeu_si128((__m128i *)(d + x), sv);
			continue;
		}
		_mm_storeu_si128((__m128i *)(d + x), blend4(_mm_loadu_si128((const __m128i *)(d + x)), sv, k16));
	}
	for (; x < n; x++) {
		u32 p = s[x];
		if (!p)
			continue;
		d[x] = k == 256 && (p >> 24) == 255 ? p : blend_premul(d[x], p, k);
	}
}

void blit(Canvas *c, const Canvas *src, int dx, int dy, f32 opacity)
{
	Rect clip = clip_of(c);
	int x0 = MAX(dx, (int)clip.x), y0 = MAX(dy, (int)clip.y);
	int x1 = MIN(dx + src->width, (int)(clip.x + clip.w)), y1 = MIN(dy + src->height, (int)(clip.y + clip.h));
	u32 k = (u32)(clampf(opacity, 0, 1) * 256 + 0.5f);
	if (!k || x1 <= x0)
		return;
	for (int y = y0; y < y1; y++)
		blend_row(canvas_row(c, y) + x0, canvas_row(src, y - dy) - dx + x0, x1 - x0, k);
}

/* Two premultiplied pixels mixed, w/256 of the second (both pairs of channels at once). */
static inline u32 lerp_px(u32 a, u32 b, u32 w)
{
	u32 iw = 256 - w;
	u32 rb = ((((a & 0xff00ffu) * iw + (b & 0xff00ffu) * w)) >> 8) & 0xff00ffu;
	u32 ag = (((a >> 8) & 0xff00ffu) * iw + ((b >> 8) & 0xff00ffu) * w) & 0xff00ff00u;
	return rb | ag;
}

/* A picture stretched into an axis-aligned rectangle, smoothly (bilinear),
 * without the general rasterizer: windows growing, shrinking and flying to
 * the taskbar in their animations. Edges land on whole pixels. */
static void blit_scaled_fast(Canvas *c, const Canvas *src, Rect sr, Rect dst, f32 opacity)
{
	Rect clip = clip_of(c);
	int x0 = MAX((int)floorf(dst.x + 0.5f), (int)clip.x), x1 = MIN((int)floorf(dst.x + dst.w + 0.5f), (int)(clip.x + clip.w));
	int y0 = MAX((int)floorf(dst.y + 0.5f), (int)clip.y), y1 = MIN((int)floorf(dst.y + dst.h + 0.5f), (int)(clip.y + clip.h));
	u32 k = (u32)(clampf(opacity, 0, 1) * 256 + 0.5f);
	if (x1 <= x0 || y1 <= y0 || !k)
		return;
	f32 fx = sr.w / dst.w, fy = sr.h / dst.h;
	int minx = CLAMP((int)sr.x, 0, src->width - 1), maxx = CLAMP((int)ceilf(sr.x + sr.w) - 1, 0, src->width - 1);
	int miny = CLAMP((int)sr.y, 0, src->height - 1), maxy = CLAMP((int)ceilf(sr.y + sr.h) - 1, 0, src->height - 1);
	/* the columns are the same on every row: worked out once */
	int n = x1 - x0;
	int *xa = mem_alloc(sizeof(int) * (usize)n * 2);
	int *xb = xa + n;
	u8 *wx = mem_alloc((usize)n);
	u32 *line = mem_alloc(sizeof(u32) * (usize)n);
	for (int i = 0; i < n; i++) {
		f32 u = ((f32)(x0 + i) + 0.5f - dst.x) * fx + sr.x - 0.5f, uf = floorf(u);
		xa[i] = CLAMP((int)uf, minx, maxx);
		xb[i] = CLAMP((int)uf + 1, minx, maxx);
		wx[i] = (u8)((u - uf) * 256);
	}
	for (int y = y0; y < y1; y++) {
		f32 v = ((f32)y + 0.5f - dst.y) * fy + sr.y - 0.5f, vf = floorf(v);
		u32 wy = (u32)((v - vf) * 256);
		const u32 *ra = canvas_row(src, CLAMP((int)vf, miny, maxy)), *rb = canvas_row(src, CLAMP((int)vf + 1, miny, maxy));
		if (wy == 0)
			for (int i = 0; i < n; i++)
				line[i] = lerp_px(ra[xa[i]], ra[xb[i]], wx[i]);
		else
			for (int i = 0; i < n; i++)
				line[i] = lerp_px(lerp_px(ra[xa[i]], ra[xb[i]], wx[i]), lerp_px(rb[xa[i]], rb[xb[i]], wx[i]), wy);
		blend_row(canvas_row(c, y) + x0, line, n, k);
	}
	mem_free(xa);
	mem_free(wx);
	mem_free(line);
}

void copy_pixels(Canvas *c, const Canvas *src, int sx, int sy, int dx, int dy, int w, int h)
{
	for (int y = 0; y < h; y++) {
		int ty = dy + y, fy = sy + y;
		if (ty < 0 || ty >= c->height || fy < 0 || fy >= src->height)
			continue;
		int x0 = MAX(0, MAX(dx, dx - sx)), x1 = MIN(c->width, MIN(dx + w, dx + src->width - sx));
		if (x1 > x0)
			memcpy(canvas_row(c, ty) + x0, canvas_row(src, fy) + sx + (x0 - dx), (usize)(x1 - x0) * 4);
	}
}

static void blit_scaled_impl(Canvas *c, const Canvas *src, Rect sr, Rect dst, f32 radius, f32 opacity)
{
	if (rect_empty(dst) || rect_empty(sr))
		return;
	Mat m = mat_mul(mat_translate(dst.x, dst.y), mat_mul(mat_scale(dst.w / sr.w, dst.h / sr.h), mat_translate(-sr.x, -sr.y)));
	/* Downscaling by a lot: pre-shrink with a box filter to avoid shimmering. */
	Canvas *tmp = NULL;
	const Canvas *img = src;
	f32 sx = dst.w / sr.w, sy = dst.h / sr.h;
	if (sx < 0.5f || sy < 0.5f) {
		int tw = MAX(1, (int)ceilf((f32)src->width * sx)), th = MAX(1, (int)ceilf((f32)src->height * sy));
		tmp = canvas_resize(src, tw, th);
		img = tmp;
		f32 kx = (f32)tw / (f32)src->width, ky = (f32)th / (f32)src->height;
		m = mat_mul(m, mat_scale(1 / kx, 1 / ky));
	}
	Paint p = paint_image(img, m);
	p.opacity = opacity;
	Path path = { 0 };
	if (radius > 0)
		path_round_rect(&path, dst, radius);
	else
		path_rect(&path, dst);
	fill_path(c, &path, mat_identity(), &p, FILL_NONZERO);
	path_free(&path);
	canvas_free(tmp);
}

void blit_scaled(Canvas *c, const Canvas *src, Rect sr, Rect dst, f32 opacity)
{
	if (rect_empty(dst) || rect_empty(sr))
		return;
	/* not scaled at all, on whole pixels (a full-screen backdrop): a plain blend */
	if (sr.w == dst.w && sr.h == dst.h && sr.x == floorf(sr.x) && sr.y == floorf(sr.y) && dst.x == floorf(dst.x) &&
	    dst.y == floorf(dst.y) && sr.x >= 0 && sr.y >= 0 && sr.x + sr.w <= (f32)src->width && sr.y + sr.h <= (f32)src->height) {
		u32 k = (u32)(clampf(opacity, 0, 1) * 256 + 0.5f);
		if (k)
			blit_part(c, src, (int)sr.x, (int)sr.y, (int)sr.w, (int)sr.h, (int)dst.x, (int)dst.y, k);
		return;
	}
	/* shrinking a lot: a box-filtered copy first, so it doesn't shimmer */
	f32 sx = dst.w / sr.w, sy = dst.h / sr.h;
	if (sx < 0.5f || sy < 0.5f) {
		int tw = MAX(1, (int)ceilf((f32)src->width * sx)), th = MAX(1, (int)ceilf((f32)src->height * sy));
		Canvas *tmp = canvas_resize(src, tw, th);
		f32 kx = (f32)tw / (f32)src->width, ky = (f32)th / (f32)src->height;
		blit_scaled_fast(c, tmp, rect(sr.x * kx, sr.y * ky, sr.w * kx, sr.h * ky), dst, opacity);
		canvas_free(tmp);
		return;
	}
	blit_scaled_fast(c, src, sr, dst, opacity);
}

void blit_scaled_round(Canvas *c, const Canvas *src, Rect sr, Rect dst, f32 radius, f32 opacity)
{
	blit_scaled_impl(c, src, sr, dst, radius, opacity);
}

Canvas *canvas_resize(const Canvas *src, int w, int h)
{
	Canvas *out = canvas_new(w, h);
	f32 fx = (f32)src->width / (f32)w, fy = (f32)src->height / (f32)h;
	for (int y = 0; y < h; y++) {
		int sy0 = (int)((f32)y * fy), sy1 = MAX(sy0 + 1, (int)((f32)(y + 1) * fy));
		sy1 = MIN(sy1, src->height);
		u32 *d = canvas_row(out, y);
		for (int x = 0; x < w; x++) {
			int sx0 = (int)((f32)x * fx), sx1 = MAX(sx0 + 1, (int)((f32)(x + 1) * fx));
			sx1 = MIN(sx1, src->width);
			u32 sum[4] = { 0 }, n = 0;
			for (int yy = sy0; yy < sy1; yy++) {
				const u32 *s = canvas_row(src, yy);
				for (int xx = sx0; xx < sx1; xx++) {
					u32 p = s[xx];
					sum[0] += p & 0xff, sum[1] += (p >> 8) & 0xff, sum[2] += (p >> 16) & 0xff, sum[3] += p >> 24;
					n++;
				}
			}
			if (n)
				d[x] = (sum[0] / n) | ((sum[1] / n) << 8) | ((sum[2] / n) << 16) | ((sum[3] / n) << 24);
		}
	}
	return out;
}

/* ---- Solid rounded rectangles, without the rasterizer -------------------------------------- */

/* One premultiplied colour over n pixels, at coverage k: four at a time. */
static void blend_span(u32 *d, int n, u32 src, u32 k)
{
	if (k >= 256 && (src >> 24) == 255) {
		for (int x = 0; x < n; x++)
			d[x] = src;
		return;
	}
	__m128i s = _mm_set1_epi32((int)src), k16 = _mm_set1_epi16((short)k);
	int x = 0;
	for (; x + 4 <= n; x += 4)
		_mm_storeu_si128((__m128i *)(d + x), blend4(_mm_loadu_si128((const __m128i *)(d + x)), s, k16));
	for (; x < n; x++)
		d[x] = blend_premul(d[x], src, k);
}

/* How much of the pixel centred at (px, py) the rounded rectangle covers:
 * its signed distance, as a ramp one pixel wide. */
static inline f32 rr_cover(f32 px, f32 py, f32 cx, f32 cy, f32 hw, f32 hh, f32 rad)
{
	f32 qx = fabsf(px - cx) - hw + rad, qy = fabsf(py - cy) - hh + rad;
	f32 ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	f32 sdf = sqrtf(ox * ox + oy * oy) + minf(maxf(qx, qy), 0) - rad;
	f32 cov = 0.5f - sdf;
	return cov < 0 ? 0 : cov > 1 ? 1 : cov;
}

/* The rounded rectangle r filled with src, or with ring > 0 only its
 * outline that wide. Edge pixels are worked out from the distance; the rest
 * is a constant blend. */
static void solid_round_rect(Canvas *c, Rect r, f32 radius, f32 ring, u32 src)
{
	if (!(src >> 24) || r.w <= 0 || r.h <= 0)
		return;
	Rect clip = clip_of(c);
	int x0 = MAX((int)floorf(r.x), (int)clip.x), y0 = MAX((int)floorf(r.y), (int)clip.y);
	int x1 = MIN((int)ceilf(r.x + r.w), (int)(clip.x + clip.w)), y1 = MIN((int)ceilf(r.y + r.h), (int)(clip.y + clip.h));
	if (x1 <= x0 || y1 <= y0)
		return;
	f32 rad = minf(radius, minf(r.w, r.h) / 2);
	f32 cx = r.x + r.w / 2, cy = r.y + r.h / 2, hw = r.w / 2, hh = r.h / 2;
	f32 irad = maxf(rad - ring, 0), ihw = hw - ring, ihh = hh - ring;
	/* the band at the top and bottom where corners (and a ring's edges) are,
	 * and the columns at the sides; everything else is a full blend, or for
	 * a ring nothing */
	f32 edge = maxf(rad, ring) + 1;
	int band_top = (int)ceilf(r.y + edge), band_bottom = (int)floorf(r.y + r.h - edge);
	int col_left = (int)ceilf(r.x + (ring > 0 ? ring : 0) + 1), col_right = (int)floorf(r.x + r.w - (ring > 0 ? ring : 0) - 1);
	for (int y = y0; y < y1; y++) {
		u32 *row = canvas_row(c, y);
		f32 py = (f32)y + 0.5f;
		bool middle = y >= band_top && y < band_bottom;
		if (middle && ring <= 0) {
			/* edges of the straight sides, then the inside */
			int a = MAX(x0, col_left), b = MIN(x1, col_right);
			for (int x = x0; x < MIN(a, x1); x++)
				row[x] = blend_premul(row[x], src, (u32)(rr_cover((f32)x + 0.5f, py, cx, cy, hw, hh, rad) * 256));
			if (b > a)
				blend_span(row + a, b - a, src, 256);
			for (int x = MAX(b, x0); x < x1; x++)
				row[x] = blend_premul(row[x], src, (u32)(rr_cover((f32)x + 0.5f, py, cx, cy, hw, hh, rad) * 256));
			continue;
		}
		for (int x = x0; x < x1; x++) {
			if (middle && x >= col_left && x < col_right)
				x = col_right; /* a ring's hollow middle */
			if (x >= x1)
				break;
			f32 px = (f32)x + 0.5f;
			f32 cov = rr_cover(px, py, cx, cy, hw, hh, rad);
			if (ring > 0 && cov > 0 && ihw > 0 && ihh > 0)
				cov -= rr_cover(px, py, cx, cy, ihw, ihh, irad);
			if (cov > 0)
				row[x] = blend_premul(row[x], src, (u32)(cov * 256));
		}
	}
}
