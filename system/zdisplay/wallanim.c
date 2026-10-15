/*
 * Wallanim: Hive's pixel-art wallpapers come alive.
 *
 * Over a "bit-" wallpaper a few sprites move on the same grid of big pixels
 * as the picture: on Midnight stars twinkle, now and then one shoots across,
 * and the water glitters under the city; on Carbon the stars are fainter and
 * lines run down the grid towards you. Eight times a second the sprites are
 * worked out again (from the time alone: nothing is kept but what was
 * drawn), and those that changed have their old and new squares marked to
 * be drawn again, so only they are; the compositor draws them over the
 * wallpaper, under the windows. With the pixel style off, reduce motion on,
 * the screen locked or something full screen, nothing moves.
 */
#include "zdisplay.h"

#define SPRITES 64
#define SPANS 1024

typedef struct {
	i16 x, y, w; /* in cells of the picture's grid; w cells across */
	u32 c;       /* premultiplied */
} Span;

typedef struct {
	bool on;
	Rect box;  /* device pixels */
	u32 hash;  /* of what it drew, to tell whether it changed */
} Sprite;

static Span spans[SPANS];
static int nspans;
static Sprite cur[SPRITES], last[SPRITES];
static int slot, first, bx0, by0, bx1, by1;
static int P = 1, gw, gh; /* a cell's size in device pixels; the grid's size in cells */
static f32 t;

static inline bool same_box(Rect a, Rect b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static f32 h1(int i, u32 seed)
{
	u32 x = (u32)i * 0x9e3779b1u ^ (seed + 0x7f4a7c15u) * 0x85ebca6bu;
	x ^= x >> 15;
	x *= 0x2c1b3c6du;
	x ^= x >> 12;
	x *= 0x297a2d39u;
	x ^= x >> 15;
	return (f32)(x & 0xffffff) / (f32)0x1000000;
}

static void begin(int s)
{
	slot = s;
	first = nspans;
	bx0 = by0 = 1 << 30;
	bx1 = by1 = -(1 << 30);
}

static void span(f32 fx, f32 fy, int w, Color c, f32 a)
{
	int x = (int)floorf(fx), y = (int)floorf(fy);
	if (nspans >= SPANS || a <= 0.02f || y < 0 || y >= gh || x + w <= 0 || x >= gw)
		return;
	spans[nspans++] = (Span){ (i16)x, (i16)y, (i16)w, color_premul(color_scale_alpha(c, minf(a, 1))) };
	bx0 = MIN(bx0, x), by0 = MIN(by0, y), bx1 = MAX(bx1, x + w - 1), by1 = MAX(by1, y);
}

static void dot(f32 x, f32 y, Color c, f32 a)
{
	span(x, y, 1, c, a);
}

static void end(void)
{
	if (slot < 0 || slot >= SPRITES)
		return;
	Sprite *s = &cur[slot];
	s->on = bx1 >= bx0;
	if (!s->on)
		return;
	s->box = rect((f32)(bx0 * P), (f32)(by0 * P), (f32)((bx1 - bx0 + 1) * P), (f32)((by1 - by0 + 1) * P));
	u32 h = 2166136261u;
	for (int i = first; i < nspans; i++)
		h = (h ^ ((u32)spans[i].x << 16 ^ (u32)spans[i].y ^ (u32)spans[i].w << 24 ^ spans[i].c)) * 16777619u;
	s->hash = h;
}

/* ---- What moves --------------------------------------------------------------------------- */

static void twinkles(int base, int n, f32 ymax, Color c, u32 seed)
{
	for (int i = 0; i < n; i++) {
		begin(base + i);
		f32 x = floorf(h1(i, seed) * (f32)gw), y = floorf(powf(h1(i, seed + 1), 1.4f) * ymax * (f32)gh);
		f32 v = 0.5f + 0.5f * sinf(t * (0.8f + 2.2f * h1(i, seed + 2)) + 6.28f * h1(i, seed + 3));
		if (v > 0.88f) { /* at its brightest it sparkles */
			dot(x, y, c, 1);
			dot(x - 1, y, c, 0.5f), dot(x + 1, y, c, 0.5f), dot(x, y - 1, c, 0.5f), dot(x, y + 1, c, 0.5f);
		} else {
			dot(x, y, c, v > 0.45f ? 0.85f : 0.3f);
		}
		end();
	}
}

/* every nine seconds a star crosses the top of the sky, its tail fading */
static void shooting(int s, Color c)
{
	const f32 period = 9;
	int k = (int)(t / period);
	f32 u = (t - (f32)k * period) / 1.2f;
	begin(s);
	if (u < 1) {
		f32 x0 = h1(k, 91) * (f32)gw * 0.7f, y0 = h1(k, 92) * (f32)gh * 0.22f, head = u * 30;
		for (int j = 0; j < 8 && head - (f32)j >= 0; j++) {
			f32 p = head - (f32)j;
			dot(x0 + p * 2, y0 + p, c, 1 - (f32)j / 8);
			dot(x0 + p * 2 + 1, y0 + p, c, (1 - (f32)j / 8) * 0.6f);
		}
	}
	end();
}

/* light catching ripples: short dashes that come and go in a band */
static void glitter(int base, int n, f32 x0, f32 x1, f32 y0, f32 y1, const Color *cols, int ncols)
{
	for (int i = 0; i < n; i++) {
		begin(base + i);
		f32 v = sinf(t * (1.5f + 2.5f * h1(i, 71)) + 6.28f * h1(i, 72));
		if (v > 0.7f) {
			f32 x = (x0 + (x1 - x0) * h1(i, 73)) * (f32)gw, y = (y0 + (y1 - y0) * h1(i, 74)) * (f32)gh;
			span(x, y, 1 + (int)(h1(i, 75) * 4), cols[i % ncols], (v - 0.7f) / 0.3f * 0.75f);
		}
		end();
	}
}

/* lines running down a floor towards you, from the horizon at hz */
static void floor_lines(int base, f32 hz, Color c)
{
	for (int i = 0; i < 2; i++) {
		begin(base + i);
		f32 z = fmodf(t * 0.28f + (f32)i * 0.5f, 1);
		f32 y = hz * (f32)gh + ((f32)gh - hz * (f32)gh) * z * z;
		span(0, y, gw, c, 0.12f + 0.55f * z);
		end();
	}
}

/* ---- Each picture's ------------------------------------------------------------------------ */

static int active(void)
{
	if (!hive_on() || !pixel_motion() || S.locked || !S.wallpaper || !str_starts(S.set.wallpaper, "bit-"))
		return -1;
	if (S.focus && (S.focus->states & STATE_FULLSCREEN) && !S.focus->minimized)
		return -1;
	/* the picture painted for that name, old names included */
	return (int)(hive_theme_find(S.set.wallpaper + 4) - HIVE_THEMES);
}

static void picture(const char *id)
{
	if (str_eq(id, "midnight")) {
		const Color w[] = { color_hex("#7aa2f7"), color_hex("#bb9af7"), color_hex("#e0af68"), color_hex("#7dcfff") };
		twinkles(0, 26, 0.6f, color_hex("#c0caf5"), 7);
		shooting(30, color_hex("#c0caf5"));
		glitter(31, 16, 0, 1, 0.72f, 1, w, 4);
	} else if (str_eq(id, "carbon")) {
		twinkles(0, 14, 0.5f, color_hex("#7a7a7a"), 43);
		shooting(16, color_hex("#bebebe"));
		floor_lines(17, 0.64f, color_hex("#e68e0d"));
	}
}

void wallanim_tick(void)
{
	int th = active();
	nspans = 0;
	memset(cur, 0, sizeof cur);
	if (th >= 0 && S.frame) {
		P = MAX(1, (int)roundf((f32)S.frame->height / 200));
		gw = (S.frame->width + P - 1) / P, gh = (S.frame->height + P - 1) / P;
		t = (f32)fmod(S.now, 3600.0);
		picture(HIVE_THEMES[th].id);
	}
	/* what changed is drawn again: where it was, and where it is */
	for (int i = 0; i < SPRITES; i++) {
		Sprite *a = &last[i], *b = &cur[i];
		if (a->on == b->on && (!a->on || (a->hash == b->hash && same_box(a->box, b->box))))
			continue;
		if (a->on && b->on)
			damage_physical(rect_union(a->box, b->box));
		else
			damage_physical(a->on ? a->box : b->box);
	}
	memcpy(last, cur, sizeof cur);
}

void wallanim_draw(Canvas *frame)
{
	Rect clip = frame->clip;
	for (int i = 0; i < nspans; i++) {
		const Span *s = &spans[i];
		Rect r = rect_intersect(rect((f32)(s->x * P), (f32)(s->y * P), (f32)(s->w * P), (f32)P), clip);
		if (rect_empty(r))
			continue;
		for (int y = (int)r.y; y < (int)(r.y + r.h); y++) {
			u32 *row = canvas_row(frame, y);
			for (int x = (int)r.x; x < (int)(r.x + r.w); x++)
				row[x] = blend_premul(row[x], s->c, 256);
		}
	}
}
