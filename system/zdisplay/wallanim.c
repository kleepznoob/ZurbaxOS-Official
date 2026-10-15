/*
 * Wallanim: Hive's pixel-art wallpapers come alive.
 *
 * Over a "bit-" wallpaper a few sprites move on the same grid of big pixels
 * as the picture: stars twinkle and now and then one shoots across, snow
 * falls on Nord, fireflies wander the forest and the cat's hill, petals
 * drift over Kanagawa and Rosé Pine, birds cross the desert, the water
 * glitters under the city and the moon, and lines run down Matte Black's
 * grid towards you. Eight times a second the sprites are worked out again
 * (from the time alone: nothing is kept but what was drawn), and those that
 * changed have their old and new squares marked to be drawn again, so only
 * they are; the compositor draws them over the wallpaper, under the
 * windows. With the pixel style off, reduce motion on, the screen locked or
 * something full screen, nothing moves and nothing is woken.
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

static void snow(int base, int n, Color c)
{
	for (int i = 0; i < n; i++) {
		begin(base + i);
		f32 speed = 2.5f + 4 * h1(i, 31);
		f32 y = fmodf(h1(i, 32) * (f32)gh + t * speed, (f32)gh + 4) - 2;
		f32 x = h1(i, 33) * (f32)gw + sinf(t * (0.5f + h1(i, 34)) + (f32)i) * 2.5f;
		dot(x, y, c, 0.9f);
		if (h1(i, 35) > 0.82f) /* the big flakes */
			dot(x - 1, y, c, 0.45f), dot(x + 1, y, c, 0.45f), dot(x, y - 1, c, 0.45f), dot(x, y + 1, c, 0.45f);
		end();
	}
}

static void fireflies(int base, int n, f32 y0, f32 y1, Color core, Color glow)
{
	for (int i = 0; i < n; i++) {
		begin(base + i);
		f32 x = h1(i, 41) * (f32)gw + sinf(t * (0.25f + 0.35f * h1(i, 42)) + (f32)i * 1.7f) * 9;
		f32 y = (y0 + (y1 - y0) * h1(i, 43)) * (f32)gh + sinf(t * (0.4f + 0.5f * h1(i, 44)) + (f32)i) * 3;
		f32 v = 0.5f + 0.5f * sinf(t * (1.2f + 1.8f * h1(i, 45)) + (f32)i * 2.3f);
		v *= v;
		dot(x, y, core, v);
		dot(x - 1, y, glow, v * 0.4f), dot(x + 1, y, glow, v * 0.4f), dot(x, y - 1, glow, v * 0.4f), dot(x, y + 1, glow, v * 0.4f);
		end();
	}
}

static void petals(int base, int n, const Color *cols, int ncols)
{
	for (int i = 0; i < n; i++) {
		begin(base + i);
		f32 vx = 2.5f + 3.5f * h1(i, 51), vy = 2 + 3 * h1(i, 52);
		f32 x = fmodf(h1(i, 53) * (f32)gw + t * vx, (f32)gw + 10) - 5 + sinf(t * 1.3f + (f32)i) * 1.5f;
		f32 y = fmodf(h1(i, 54) * (f32)gh + t * vy, (f32)gh + 10) - 5;
		Color c = cols[i % ncols];
		dot(x, y, c, 0.9f);
		if (h1(i, 55) > 0.45f) /* tumbling: its other half above or beside */
			dot(x + 1, y + (((int)(t * 3) + i) & 1 ? 0 : -1), c, 0.7f);
		end();
	}
}

static void birds(int base, int n, Color c)
{
	static const char *const FLAP[2][3] = { { "X...X", ".X.X.", "..X.." }, { ".....", "XX.XX", "..X.." } };
	for (int i = 0; i < n; i++) {
		begin(base + i);
		f32 x = fmodf(h1(i, 61) * (f32)gw + t * (4.5f + 2 * h1(i, 62)), (f32)gw + 14) - 7;
		f32 y = (0.2f + 0.12f * h1(i, 63)) * (f32)gh + sinf(t * 0.8f + (f32)i);
		int f = ((int)(t * 5) + i * 2) & 1;
		for (int r = 0; r < 3; r++)
			for (int k = 0; k < 5; k++)
				if (FLAP[f][r][k] == 'X')
					dot(x + (f32)k, y + (f32)r, c, 1);
		end();
	}
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
	const char *id = S.set.wallpaper + 4;
	for (int i = 0; i < HIVE_THEME_COUNT; i++)
		if (str_eq(HIVE_THEMES[i].id, id))
			return i;
	return -1;
}

static void picture(const char *id)
{
	if (str_eq(id, "tokyo-night")) {
		const Color w[] = { color_hex("#7aa2f7"), color_hex("#bb9af7"), color_hex("#e0af68"), color_hex("#7dcfff") };
		twinkles(0, 26, 0.6f, color_hex("#c0caf5"), 7);
		shooting(30, color_hex("#c0caf5"));
		glitter(31, 16, 0, 1, 0.72f, 1, w, 4);
	} else if (str_eq(id, "catppuccin")) {
		twinkles(0, 26, 0.55f, color_hex("#b4befe"), 11);
		shooting(30, color_hex("#f5e0dc"));
		fireflies(31, 7, 0.88f, 0.98f, color_hex("#f9e2af"), color_hex("#a6e3a1"));
	} else if (str_eq(id, "gruvbox")) {
		twinkles(0, 10, 0.3f, color_hex("#ebdbb2"), 19);
		birds(12, 3, color_hex("#1d2021"));
	} else if (str_eq(id, "nord")) {
		twinkles(0, 14, 0.45f, color_hex("#e5e9f0"), 23);
		shooting(16, color_hex("#eceff4"));
		snow(17, 44, color_hex("#eceff4"));
	} else if (str_eq(id, "everforest")) {
		twinkles(0, 8, 0.35f, color_hex("#d3c6aa"), 29);
		fireflies(10, 18, 0.66f, 0.98f, color_hex("#dbbc7f"), color_hex("#a7c080"));
	} else if (str_eq(id, "kanagawa")) {
		const Color p[] = { color_hex("#d27e99"), color_hex("#e6a5b8") };
		twinkles(0, 10, 0.45f, color_hex("#dcd7ba"), 37);
		petals(12, 24, p, 2);
	} else if (str_eq(id, "rose-pine")) {
		const Color p[] = { color_hex("#ebbcba"), color_hex("#eb6f92") }, g[] = { color_hex("#f6c177") };
		twinkles(0, 22, 0.55f, color_hex("#e0def4"), 41);
		petals(24, 14, p, 2);
		glitter(40, 12, 0.6f, 0.68f, 0.64f, 1, g, 1);
	} else if (str_eq(id, "matte-black")) {
		twinkles(0, 12, 0.5f, color_hex("#5c5c5c"), 43);
		floor_lines(14, 0.64f, color_hex("#e68e0d"));
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
