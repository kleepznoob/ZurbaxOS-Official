/* Text rendering: glyph parsing, rasterisation, caching and layout. */
#include "font_internal.h"

#define REGULAR_NIB 84.0f
#define MONO_ADVANCE 600.0f

typedef struct {
	f32 x, y, r;
} Dot;

typedef struct {
	u32 cp;
	bool parsed, valid;
	f32 advance;
	Path strokes;     /* font units, y up */
	VEC(Dot) dots;
	f32 ink_x0, ink_x1, ink_y1;
	const char *def;
	u32 comp_base, comp_mark;
} Glyph;

static Glyph *glyphs;
static usize nglyphs;

static int glyph_cmp(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	u32 x = ((const Glyph *)a)->cp, y = ((const Glyph *)b)->cp;
	return x < y ? -1 : x > y;
}

static void init_table(void)
{
	if (glyphs)
		return;
	usize n = 0, m = 0;
	while (ZURBAX_SANS[n].def)
		n++;
	while (ZURBAX_COMPOSITES[m].cp)
		m++;
	glyphs = NEW_ARRAY(Glyph, n + m);
	for (usize i = 0; i < n; i++) {
		glyphs[i].cp = ZURBAX_SANS[i].cp;
		glyphs[i].def = ZURBAX_SANS[i].def;
	}
	for (usize i = 0; i < m; i++) {
		Glyph *g = &glyphs[n + i];
		g->cp = ZURBAX_COMPOSITES[i].cp;
		g->comp_base = ZURBAX_COMPOSITES[i].base;
		g->comp_mark = ZURBAX_COMPOSITES[i].mark;
	}
	nglyphs = n + m;
	sort(glyphs, nglyphs, sizeof(Glyph), glyph_cmp, NULL);
}

static Glyph *find_glyph(u32 cp)
{
	usize lo = 0, hi = nglyphs;
	while (lo < hi) {
		usize mid = (lo + hi) / 2;
		if (glyphs[mid].cp == cp)
			return &glyphs[mid];
		if (glyphs[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

static Glyph *get_glyph(u32 cp);

/* Appends src's strokes and dots to dst, shifted. */
static void merge_glyph(Glyph *dst, const Glyph *src, f32 dx, f32 dy)
{
	usize pi = 0;
	const Vec2 *q = src->strokes.pts.data;
	for (usize i = 0; i < src->strokes.verbs.len; i++) {
		switch (src->strokes.verbs.data[i]) {
		case PATH_MOVE: path_move(&dst->strokes, q[pi].x + dx, q[pi].y + dy); pi++; break;
		case PATH_LINE: path_line(&dst->strokes, q[pi].x + dx, q[pi].y + dy); pi++; break;
		case PATH_QUAD:
			path_quad(&dst->strokes, q[pi].x + dx, q[pi].y + dy, q[pi + 1].x + dx, q[pi + 1].y + dy);
			pi += 2;
			break;
		case PATH_CUBIC:
			path_cubic(&dst->strokes, q[pi].x + dx, q[pi].y + dy, q[pi + 1].x + dx, q[pi + 1].y + dy,
				   q[pi + 2].x + dx, q[pi + 2].y + dy);
			pi += 3;
			break;
		case PATH_CLOSE: path_close(&dst->strokes); break;
		}
	}
	vec_each(&src->dots, d) vec_push(&dst->dots, ((Dot){ d->x + dx, d->y + dy, d->r }));
}

static void compute_ink(Glyph *g)
{
	f32 x0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	vec_each(&g->strokes.pts, p)
	{
		x0 = minf(x0, p->x), x1 = maxf(x1, p->x), y1 = maxf(y1, p->y);
	}
	vec_each(&g->dots, d)
	{
		x0 = minf(x0, d->x - d->r), x1 = maxf(x1, d->x + d->r), y1 = maxf(y1, d->y + d->r);
	}
	if (x0 > x1)
		x0 = x1 = g->advance / 2, y1 = 0;
	g->ink_x0 = x0, g->ink_x1 = x1, g->ink_y1 = y1;
}

static void parse_def(Glyph *g, const char *s)
{
	const char *p = s;
	f32 a[6];
	for (;;) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		char cmd = *p++;
		int nargs = 0;
		switch (cmd) {
		case 'w': case '@': nargs = cmd == 'w' ? 1 : 3; break;
		case 'M': case 'L': nargs = 2; break;
		case 'Q': nargs = 4; break;
		case 'C': case 'O': case 'A': nargs = 6; break;
		case 'D': nargs = 3; break;
		default: return; /* malformed definition: stop */
		}
		for (int i = 0; i < nargs; i++)
			a[i] = (f32)str_to_float(p, &p);
		switch (cmd) {
		case 'w': g->advance = a[0]; break;
		case 'M': path_move(&g->strokes, a[0], a[1]); break;
		case 'L': path_line(&g->strokes, a[0], a[1]); break;
		case 'Q': path_quad(&g->strokes, a[0], a[1], a[2], a[3]); break;
		case 'C': path_cubic(&g->strokes, a[0], a[1], a[2], a[3], a[4], a[5]); break;
		case 'O':
			path_move(&g->strokes, a[0] + a[2] * cosf(a[4] * PI_F / 180), a[1] + a[3] * sinf(a[4] * PI_F / 180));
			path_arc(&g->strokes, a[0], a[1], a[2], a[3], a[4] * PI_F / 180, a[5] * PI_F / 180);
			break;
		case 'A': path_arc(&g->strokes, a[0], a[1], a[2], a[3], a[4] * PI_F / 180, a[5] * PI_F / 180); break;
		case 'D': vec_push(&g->dots, ((Dot){ a[0], a[1], a[2] })); break;
		case '@': {
			Glyph *inc = get_glyph((u32)a[0]);
			if (inc)
				merge_glyph(g, inc, a[1], a[2]);
			break;
		}
		}
	}
}

static Glyph *get_glyph(u32 cp)
{
	init_table();
	Glyph *g = find_glyph(cp);
	if (!g)
		return NULL;
	if (g->parsed)
		return g->valid ? g : NULL;
	g->parsed = true;
	if (g->def) {
		parse_def(g, g->def);
	} else {
		Glyph *base = get_glyph(g->comp_base), *mark = get_glyph(g->comp_mark);
		if (!base || !mark)
			return NULL;
		g->advance = base->advance;
		merge_glyph(g, base, 0, 0);
		/* Marks sit over the base's ink centre; raise them for capitals. */
		f32 cx = (base->ink_x0 + base->ink_x1) / 2;
		bool above = mark->ink_y1 > 300;
		f32 dy = (above && base->ink_y1 > 600) ? 180 : 0;
		if (g->comp_base == 0x131 || g->comp_base == 'I')
			cx = (base->ink_x0 + base->ink_x1) / 2;
		merge_glyph(g, mark, cx, dy);
	}
	compute_ink(g);
	g->valid = true;
	return g;
}

/* ---- Metrics ---------------------------------------------------------------------- */

static f32 nib_units(int weight)
{
	return 92 + (f32)(weight - 400) * 0.16f;
}

f32 font_ascent(Font f) { return f.size * 0.95f; }
f32 font_descent(Font f) { return f.size * 0.27f; }
f32 font_line_height(Font f) { return roundf(f.size * 1.32f); }
f32 font_cap_height(Font f) { return f.size * 0.70f; }
f32 font_x_height(Font f) { return f.size * 0.52f; }

static f32 kern(u32 prev, u32 cp)
{
	if (!prev || prev > 127 || cp > 127)
		return 0;
	for (const GlyphKern *k = ZURBAX_KERNING; k->left; k++)
		if (k->left == prev && k->right == cp)
			return (f32)k->adjust;
	return 0;
}

/* Advance in font units for a glyph at this weight. */
static f32 advance_units(Font f, Glyph *g)
{
	if (f.mono)
		return MONO_ADVANCE;
	f32 adv = g ? g->advance : 600;
	if (adv > 0)
		adv += (nib_units(f.weight) - REGULAR_NIB) * 0.55f;
	return adv;
}

f32 glyph_advance(Font f, u32 cp, u32 prev)
{
	Glyph *g = get_glyph(cp);
	if (!g && cp >= 0x20)
		g = get_glyph('?');
	f32 adv = advance_units(f, g);
	if (!f.mono)
		adv += kern(prev, cp);
	return adv * f.size / 1000;
}

f32 text_width(Font f, const char *s, usize n)
{
	f32 w = 0;
	u32 prev = 0;
	for (usize i = 0; i < n;) {
		u32 cp;
		i += utf8_decode(s + i, n - i, &cp);
		if (cp == '\n')
			break;
		w += glyph_advance(f, cp, prev);
		prev = cp;
	}
	return w;
}

/* ---- Glyph cache --------------------------------------------------------------------- */

#define CACHE_SIZE 4096
#define SUBPIXEL 4

typedef struct {
	u64 key;
	Mask mask;   /* mask.x / mask.y are offsets from the pen position */
} CacheEntry;

static CacheEntry *cache;
static usize cache_used;

void font_cache_clear(void)
{
	if (!cache)
		return;
	for (usize i = 0; i < CACHE_SIZE; i++)
		if (cache[i].key)
			mask_free(&cache[i].mask);
	memset(cache, 0, sizeof(CacheEntry) * CACHE_SIZE);
	cache_used = 0;
}

static u64 cache_key(Font f, u32 cp, int phase)
{
	u64 size_q = (u64)(f.size * 4 + 0.5f) & 0xffff;
	u64 w = (u64)(f.weight / 10) & 0xff;
	return ((u64)cp << 32) | (size_q << 16) | (w << 8) | ((u64)f.mono << 7) | (u64)phase | (1ull << 6);
}

static void render_glyph(Font f, Glyph *g, int phase, Mask *out)
{
	/* Light vertical hinting: land the x-height on a whole pixel so small
	 * text keeps clear, even lines (horizontal positions stay exact). */
	f32 s = f.size / 1000;
	f32 sy = s;
	if (f.size <= 22) {
		f32 xh = roundf(520 * s);
		if (xh >= 4)
			sy = xh / 520;
	}
	f32 nib = nib_units(f.weight);
	f32 extra = (nib - REGULAR_NIB) * 0.275f; /* half of the added advance, keeps glyphs centred */
	f32 ox = (f32)phase / SUBPIXEL;
	f32 sx = s, shift = extra;
	if (f.mono) {
		f32 adv = g->advance;
		f32 squeeze = adv > 640 ? 600 / adv : 1;
		sx = s * squeeze;
		shift = (MONO_ADVANCE - adv * squeeze) / 2 / squeeze;
	}
	Mat m = { sx, 0, 0, -sy, ox + shift * sx, 0 };
	/* Keep hairlines from fading at very small sizes. */
	f32 px_width = maxf(nib * s, 1.0f);
	/* Flat terminals and sharp corners give a grotesque, UI-friendly texture. */
	StrokeStyle st = { px_width / mat_scale_factor(m), CAP_SQUARE, JOIN_MITER, 2.2f };
	Path outline = { 0 };
	path_stroke_outline(&g->strokes, m, &st, &outline);
	f32 dot_scale = maxf(nib / REGULAR_NIB, 0.8f);
	vec_each(&g->dots, d)
	{
		Vec2 c = mat_apply(m, (Vec2){ d->x, d->y });
		f32 r = maxf(d->r * dot_scale * s, px_width * 0.6f);
		path_circle(&outline, c.x, c.y, r);
	}
	Rect clip = rect(-4096, -4096, 8192, 8192);
	if (!mask_from_path(out, &outline, mat_identity(), FILL_NONZERO, clip))
		memset(out, 0, sizeof *out);
	path_free(&outline);
}

static const Mask *glyph_mask(Font f, u32 cp, int phase)
{
	if (!cache)
		cache = NEW_ARRAY(CacheEntry, CACHE_SIZE);
	u64 key = cache_key(f, cp, phase);
	usize i = (usize)(hash_bytes(&key, sizeof key) & (CACHE_SIZE - 1));
	while (cache[i].key) {
		if (cache[i].key == key)
			return &cache[i].mask;
		i = (i + 1) & (CACHE_SIZE - 1);
	}
	if (cache_used * 4 >= CACHE_SIZE * 3) {
		font_cache_clear();
		i = (usize)(hash_bytes(&key, sizeof key) & (CACHE_SIZE - 1));
	}
	Glyph *g = get_glyph(cp);
	if (!g)
		g = get_glyph('?');
	cache[i].key = key;
	if (g)
		render_glyph(f, g, phase, &cache[i].mask);
	cache_used++;
	return &cache[i].mask;
}

/* ---- Drawing ----------------------------------------------------------------------------- */

f32 draw_text(Canvas *c, Font f, f32 x, f32 y, const char *s, usize n, Color color)
{
	Paint paint = paint_solid(color);
	int baseline = (int)roundf(y);
	u32 prev = 0;
	for (usize i = 0; i < n;) {
		u32 cp;
		i += utf8_decode(s + i, n - i, &cp);
		if (cp == '\n')
			break;
		if (!f.mono)
			x += kern(prev, cp) * f.size / 1000;
		if (cp != ' ' && cp != '\t' && cp != 0xa0) {
			f32 fl = floorf(x);
			int phase = (int)((x - fl) * SUBPIXEL);
			if (phase >= SUBPIXEL)
				phase = SUBPIXEL - 1;
			const Mask *m = glyph_mask(f, cp, phase);
			if (m->data)
				fill_mask(c, m, (int)fl, baseline, &paint);
		}
		x += glyph_advance(f, cp, 0);
		prev = cp;
	}
	return x;
}

void draw_text_in(Canvas *c, Font f, Rect r, const char *s, Color color, Align align)
{
	usize n = strlen(s);
	f32 w = text_width(f, s, n);
	f32 baseline = r.y + (r.h + font_cap_height(f)) / 2;
	if (w <= r.w) {
		f32 x = align == ALIGN_CENTER ? r.x + (r.w - w) / 2 : align == ALIGN_END ? r.x + r.w - w : r.x;
		draw_text(c, f, x, baseline, s, n, color);
		return;
	}
	const char *ell = "\xe2\x80\xa6";
	f32 ew = text_width(f, ell, 3);
	usize fit = text_fit(f, s, n, maxf(0, r.w - ew));
	while (fit > 0 && s[fit - 1] == ' ')
		fit--;
	f32 x = draw_text(c, f, r.x, baseline, s, fit, color);
	draw_text(c, f, x, baseline, ell, 3, color);
}

usize text_hit(Font f, const char *s, usize n, f32 x)
{
	f32 pen = 0;
	u32 prev = 0;
	for (usize i = 0; i < n;) {
		u32 cp;
		usize len = utf8_decode(s + i, n - i, &cp);
		f32 adv = glyph_advance(f, cp, prev);
		if (x < pen + adv / 2)
			return i;
		pen += adv;
		prev = cp;
		i += len;
	}
	return n;
}

usize text_fit(Font f, const char *s, usize n, f32 max_width)
{
	f32 pen = 0;
	u32 prev = 0;
	for (usize i = 0; i < n;) {
		u32 cp;
		usize len = utf8_decode(s + i, n - i, &cp);
		pen += glyph_advance(f, cp, prev);
		if (pen > max_width)
			return i;
		prev = cp;
		i += len;
	}
	return n;
}

int text_wrap(Font f, const char *s, usize n, f32 max_width, TextLine *lines, int max_lines)
{
	int count = 0;
	usize line_start = 0;
	for (;;) {
		/* Find the longest run that fits, preferring to break after a space. */
		usize i = line_start, last_break = 0;
		f32 pen = 0, width_at_break = 0;
		bool has_break = false;
		u32 prev = 0;
		while (i < n && s[i] != '\n') {
			u32 cp;
			usize len = utf8_decode(s + i, n - i, &cp);
			f32 adv = glyph_advance(f, cp, prev);
			if (pen + adv > max_width && i > line_start) {
				if (cp == ' ') { /* a space at the edge: break here, drop it */
					has_break = true;
					last_break = i;
					width_at_break = pen;
				}
				break;
			}
			pen += adv;
			if (cp == ' ' || cp == '-' || cp == '/') {
				has_break = true;
				last_break = cp == ' ' ? i : i + len;
				width_at_break = cp == ' ' ? pen - adv : pen;
			}
			prev = cp;
			i += len;
		}
		usize end, next;
		f32 width;
		if (i >= n || s[i] == '\n') {
			end = i;
			next = i + 1;
			width = pen;
		} else if (has_break) {
			end = last_break;
			next = last_break;
			while (next < n && s[next] == ' ')
				next++;
			width = width_at_break;
		} else {
			end = next = i; /* a single word longer than the line: hard break */
			width = pen;
		}
		if (count < max_lines && lines)
			lines[count] = (TextLine){ line_start, end - line_start, width };
		count++;
		if (i >= n)
			break; /* reached the end of the text */
		line_start = next;
	}
	return count;
}

f32 text_wrap_height(Font f, const char *s, f32 max_width, f32 line_spacing)
{
	int n = text_wrap(f, s, strlen(s), max_width, NULL, 0);
	return (f32)n * font_line_height(f) * (line_spacing > 0 ? line_spacing : 1);
}

f32 draw_text_wrapped(Canvas *c, Font f, Rect r, const char *s, Color color, Align align, f32 line_spacing)
{
	usize n = strlen(s);
	int count = text_wrap(f, s, n, r.w, NULL, 0);
	TextLine stack[64];
	TextLine *lines = count <= 64 ? stack : mem_alloc(sizeof(TextLine) * (usize)count);
	text_wrap(f, s, n, r.w, lines, count);
	f32 lh = font_line_height(f) * (line_spacing > 0 ? line_spacing : 1);
	f32 y = r.y + (lh + font_cap_height(f)) / 2;
	for (int i = 0; i < count; i++) {
		if (y - font_cap_height(f) > rect_bottom(r))
			break;
		f32 x = r.x;
		if (align == ALIGN_CENTER)
			x += (r.w - lines[i].width) / 2;
		else if (align == ALIGN_END)
			x += r.w - lines[i].width;
		draw_text(c, f, x, y, s + lines[i].start, lines[i].len, color);
		y += lh;
	}
	if (lines != stack)
		mem_free(lines);
	return (f32)count * lh;
}
