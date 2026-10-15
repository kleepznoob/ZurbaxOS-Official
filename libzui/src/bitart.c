/*
 * Bit art: Hive's themes, and a pixel-art wallpaper for each.
 *
 * A picture is painted at a low resolution, about two hundred rows whatever
 * the screen, in its theme's colours, with gradients ordered-dithered (a 4×4
 * Bayer pattern) instead of smooth, then blown up with every pixel kept
 * square and sharp. Carbon, the first and the default, is a sun going down
 * striped over a grid in black and orange; Midnight is a city by a river at
 * night, its windows lit, the moon over it.
 */
#include "zui.h"

const HiveTheme HIVE_THEMES[] = {
	{ "carbon", "Carbon", "#121212", "#bebebe", "#e68e0d", "#d35f5f", "#2a2a2a", "#5c5c5c" },
	{ "midnight", "Midnight", "#1a1b26", "#c0caf5", "#7aa2f7", "#bb9af7", "#3b4261", "#565f89" },
};
const int HIVE_THEME_COUNT = (int)ARRAY_LEN(HIVE_THEMES);

/* The theme of that id, the first (Carbon) if there's none; the names the
 * two had before they were renamed still find them. */
const HiveTheme *hive_theme_find(const char *id)
{
	if (id && str_eq(id, "matte-black"))
		id = "carbon";
	if (id && str_eq(id, "tokyo-night"))
		id = "midnight";
	for (int i = 0; i < HIVE_THEME_COUNT; i++)
		if (id && str_eq(HIVE_THEMES[i].id, id))
			return &HIVE_THEMES[i];
	return &HIVE_THEMES[0];
}

bool look_hive(const Conf *c)
{
	return conf_get_bool(c, "hive", HIVE_DEFAULT);
}

bool look_dark(const Conf *c)
{
	return conf_get_bool(c, "dark", look_hive(c));
}

const char *look_accent(const Conf *c)
{
	return conf_get(c, "accent", look_hive(c) ? hive_theme_find(conf_get(c, "hive_theme", NULL))->accent : THEME_ACCENT_DEFAULT);
}

const char *look_wallpaper(const Conf *c)
{
	static char hive_wall[64];
	if (!look_hive(c))
		return conf_get(c, "wallpaper", WALLPAPER_DEFAULT);
	fmt(hive_wall, sizeof hive_wall, "bit-%s", hive_theme_find(conf_get(c, "hive_theme", NULL))->id);
	return conf_get(c, "wallpaper", hive_wall);
}

/* ---- Pixels ------------------------------------------------------------------------------ */

#define W (c->width)
#define H (c->height)

static const u8 BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static inline f32 bayer(int x, int y)
{
	return ((f32)BAYER[y & 3][x & 3] + 0.5f) / 16;
}

/* t in [0, 1] rounded to steps, up or down as the pattern says: a dithered level */
static inline f32 dither(f32 t, int x, int y, int steps)
{
	f32 q = floorf(t * (f32)steps + bayer(x, y)) / (f32)steps;
	return q < 0 ? 0 : q > 1 ? 1 : q;
}

static f32 hashf(int i, u32 seed)
{
	u32 x = (u32)i * 0x9e3779b1u ^ (seed + 0x7f4a7c15u) * 0x85ebca6bu;
	x ^= x >> 15;
	x *= 0x2c1b3c6du;
	x ^= x >> 12;
	x *= 0x297a2d39u;
	x ^= x >> 15;
	return (f32)(x & 0xffffff) / (f32)0x1000000;
}

/* smooth 1-D value noise in [0, 1] */
static f32 noise(f32 x, u32 seed, int octaves)
{
	f32 sum = 0, amp = 1, norm = 0;
	for (int o = 0; o < octaves; o++) {
		int i = (int)floorf(x);
		f32 t = x - (f32)i;
		t = t * t * (3 - 2 * t);
		f32 a = hashf(i, seed + (u32)o * 977), b = hashf(i + 1, seed + (u32)o * 977);
		sum += (a + (b - a) * t) * amp;
		norm += amp;
		amp *= 0.5f;
		x *= 2.03f;
	}
	return sum / norm;
}

static inline f32 dist(f32 x, f32 y)
{
	return sqrtf(x * x + y * y);
}

static u32 C(const char *hex)
{
	return color_hex(hex) | 0xff000000u;
}

static inline u32 mix(u32 a, u32 b, f32 t)
{
	if (t <= 0)
		return a;
	if (t >= 1)
		return b;
	u32 k = (u32)(t * 256), j = 256 - k;
	u32 r = (((a >> 16) & 255) * j + ((b >> 16) & 255) * k) >> 8;
	u32 g = (((a >> 8) & 255) * j + ((b >> 8) & 255) * k) >> 8;
	u32 bl = ((a & 255) * j + (b & 255) * k) >> 8;
	return 0xff000000u | r << 16 | g << 8 | bl;
}

static inline void put(Canvas *c, int x, int y, u32 v)
{
	if (x >= 0 && y >= 0 && x < W && y < H)
		canvas_row(c, y)[x] = v;
}

static inline u32 get(const Canvas *c, int x, int y)
{
	x = CLAMP(x, 0, W - 1), y = CLAMP(y, 0, H - 1);
	return canvas_row(c, y)[x];
}

/* a colour laid over what's there, dithered to quarters */
static inline void tint(Canvas *c, int x, int y, u32 col, f32 a)
{
	f32 q = dither(a, x, y, 4);
	if (q > 0)
		put(c, x, y, mix(get(c, x, y), col, q));
}

static void fill(Canvas *c, int x, int y, int w, int h, u32 col)
{
	for (int j = MAX(y, 0); j < MIN(y + h, H); j++)
		for (int i = MAX(x, 0); i < MIN(x + w, W); i++)
			canvas_row(c, j)[i] = col;
}

/* A gradient down the rows y0..y1 through n colours, dithered. */
static void gradient(Canvas *c, int y0, int y1, const u32 *stops, int n)
{
	int levels = (n - 1) * 4;
	for (int y = MAX(y0, 0); y < MIN(y1, H); y++) {
		f32 p = (f32)(y - y0) / (f32)MAX(1, y1 - y0 - 1) * (f32)levels;
		u32 *row = canvas_row(c, y);
		for (int x = 0; x < W; x++) {
			int q = CLAMP((int)floorf(p + bayer(x, y) - 0.5f), 0, levels);
			int seg = MIN(q / 4, n - 2);
			row[x] = mix(stops[seg], stops[seg + 1], (f32)(q - seg * 4) / 4);
		}
	}
}

/* light falling off around a point, in dithered rings */
static void glow(Canvas *c, f32 cx, f32 cy, f32 r, u32 col, f32 amount)
{
	for (int y = MAX(0, (int)(cy - r)); y < MIN(H, (int)(cy + r) + 1); y++)
		for (int x = MAX(0, (int)(cx - r)); x < MIN(W, (int)(cx + r) + 1); x++) {
			f32 d = dist((f32)x + 0.5f - cx, (f32)y + 0.5f - cy);
			if (d >= r)
				continue;
			f32 t = 1 - d / r;
			tint(c, x, y, col, t * t * amount);
		}
}

static inline bool in_disc(f32 cx, f32 cy, f32 r, int x, int y)
{
	f32 dx = (f32)x + 0.5f - cx, dy = (f32)y + 0.5f - cy;
	return dx * dx + dy * dy <= r * r;
}

/* a disc, no lower than max_y */
static void disc(Canvas *c, f32 cx, f32 cy, f32 r, u32 col, int max_y)
{
	for (int y = MAX(0, (int)(cy - r) - 1); y <= MIN(MIN(H - 1, max_y), (int)(cy + r) + 1); y++)
		for (int x = MAX(0, (int)(cx - r) - 1); x <= MIN(W - 1, (int)(cx + r) + 1); x++)
			if (in_disc(cx, cy, r, x, y))
				canvas_row(c, y)[x] = col;
}

static void stars(Canvas *c, int y1, int count, u32 col, u32 seed)
{
	for (int i = 0; i < count; i++) {
		int x = (int)(hashf(i, seed) * (f32)W);
		f32 v = hashf(i, seed + 1);
		int y = (int)(v * v * (f32)y1);
		f32 b = hashf(i, seed + 2);
		u32 bg = get(c, x, y);
		put(c, x, y, mix(bg, col, 0.3f + b * 0.7f));
		if (b > 0.94f) { /* a few bright ones, with points */
			put(c, x - 1, y, mix(get(c, x - 1, y), col, 0.45f));
			put(c, x + 1, y, mix(get(c, x + 1, y), col, 0.45f));
			put(c, x, y - 1, mix(get(c, x, y - 1), col, 0.45f));
			put(c, x, y + 1, mix(get(c, x, y + 1), col, 0.45f));
		}
	}
}

/* A range of hills or mountains: the ridge at base - amp × noise, filled
 * below. The light comes from the left: under a ridge falling to the right
 * the slope is in shade, fading out lower down. Above snow_y, snow. ridge
 * (W entries, or NULL) gets the top row of each column. */
typedef struct {
	f32 base, amp, scale;
	u32 seed;
	int octaves;
	f32 peaky;    /* 0 rounded hills .. 1 sharp peaks */
	u32 lit, shade;
	int snow_y;   /* -1: none */
	u32 snow, snow_shade;
	f32 shade_depth;
} Range;

static void range(Canvas *c, const Range *r, int *ridge)
{
	enum { K = 3 }; /* the slope is taken over 2K + 1 columns */
	int *top = mem_alloc(sizeof(int) * (usize)(W + 2 * K));
	f32 *v = mem_alloc(sizeof(f32) * (usize)(W + 2 * K)), lo = 1e9f, hi = -1e9f;
	for (int x = -K; x < W + K; x++) {
		f32 u = (f32)x / (f32)W * r->scale;
		f32 n = noise(u, r->seed, r->octaves);
		f32 p = 1 - fabsf(noise(u * 0.8f, r->seed + 51, 3) * 2 - 1);
		v[x + K] = n * (1 - r->peaky) + p * p * r->peaky;
		lo = minf(lo, v[x + K]), hi = maxf(hi, v[x + K]);
	}
	/* the noise stays near its middle: stretched, the range uses all its height */
	for (int x = 0; x < W + 2 * K; x++)
		top[x] = (int)(r->base - r->amp * (v[x] - lo) / maxf(hi - lo, 1e-3f));
	mem_free(v);
	for (int x = 0; x < W; x++) {
		int t = top[x + K];
		if (ridge)
			ridge[x] = t;
		bool falling = top[x + 2 * K] - top[x] >= K;
		int snow_line = r->snow_y >= 0 && t < r->snow_y ? t + (int)((f32)(r->snow_y - t) * 0.7f) + (int)(hashf(x, r->seed + 9) * 3) : -1;
		for (int y = MAX(t, 0); y < H; y++) {
			f32 depth = (f32)(y - t) / maxf(r->shade_depth, 1);
			bool shaded = falling && bayer(x, y) > depth;
			bool snowy = y < snow_line;
			u32 col = snowy ? (shaded ? r->snow_shade : r->snow) : shaded ? r->shade : r->lit;
			canvas_row(c, y)[x] = col;
		}
	}
	mem_free(top);
}

/* A skyline: blocks with windows, some lit. */
static void city(Canvas *c, int base, int hmin, int hmax, u32 body, const u32 *lights, int nlights, f32 lit, u32 tip, u32 seed)
{
	for (int i = 0, x = -2; x < W; i++) {
		int bw = 6 + (int)(hashf(i, seed) * 11);
		f32 hv = hashf(i, seed + 1);
		int top = base - (hmin + (int)(hv * hv * (f32)(hmax - hmin)));
		fill(c, x, top, bw, H - top, body);
		if (hashf(i, seed + 2) < 0.3f && bw > 7)
			fill(c, x + 2, top - 3, bw - 4, 3, body);
		if (tip && hashf(i, seed + 3) < 0.22f) {
			int ax = x + bw / 2, ah = 4 + (int)(hashf(i, seed + 4) * 6);
			fill(c, ax, top - ah, 1, ah, body);
			put(c, ax, top - ah - 1, tip);
		}
		f32 p = lit * (0.3f + hashf(i, seed + 5) * 1.4f);
		for (int wy = top + 2; wy < base - 1; wy += 2) {
			f32 floor_p = p * (0.4f + hashf(wy, seed + (u32)i * 7) * 1.2f);
			for (int wx = x + 1; wx < x + bw - 1; wx += 2)
				if (hashf(wx * 131 + wy, seed + 6) < floor_p)
					put(c, wx, wy, lights[(int)(hashf(wx + wy * 7, seed + 8) * (f32)nlights) % nlights]);
		}
		x += bw + (hashf(i, seed + 7) < 0.3f ? 1 : 0);
	}
}

/* Below the horizon, what's above it, mirrored and rippling, under water. */
static void water(Canvas *c, int hz, u32 col, f32 amount, u32 ripple, u32 seed)
{
	for (int y = hz; y < H; y++) {
		f32 depth = (f32)(y - hz) / (f32)MAX(1, H - hz);
		int wob = (int)roundf(sinf((f32)y * 1.7f + (f32)seed) * (0.4f + depth * 1.6f));
		int sy = hz - 1 - (y - hz);
		for (int x = 0; x < W; x++)
			canvas_row(c, y)[x] = mix(get(c, x + wob, sy), col, amount + depth * 0.25f);
	}
	int n = W * (H - hz) / 90;
	for (int i = 0; i < n; i++) {
		int y = hz + 1 + (int)(hashf(i, seed + 1) * (f32)(H - hz));
		int x = (int)(hashf(i, seed + 2) * (f32)W), len = 2 + (int)(hashf(i, seed + 3) * 5);
		for (int k = 0; k < len; k++)
			put(c, x + k, y, mix(get(c, x + k, y), ripple, 0.28f));
	}
}

/* ---- The pictures ------------------------------------------------------------------------ */

static void midnight(Canvas *c)
{
	int hz = (int)((f32)H * 0.70f);
	const u32 sky[] = { C("#0d0d14"), C("#16161e"), C("#1f2335"), C("#2f3354"), C("#5b4a80") };
	gradient(c, 0, hz, sky, 5);
	stars(c, (int)((f32)hz * 0.8f), W * H / 170, C("#c0caf5"), 7);
	f32 mx = (f32)W * 0.78f, my = (f32)H * 0.19f, mr = (f32)H * 0.075f;
	glow(c, mx, my, (f32)H * 0.32f, C("#7aa2f7"), 0.32f);
	disc(c, mx, my, mr, C("#c0caf5"), H);
	disc(c, mx - mr * 0.3f, my - mr * 0.2f, mr * 0.22f, C("#a9b1d6"), H);
	disc(c, mx + mr * 0.35f, my + mr * 0.3f, mr * 0.16f, C("#a9b1d6"), H);
	disc(c, mx + mr * 0.1f, my - mr * 0.55f, mr * 0.1f, C("#a9b1d6"), H);
	glow(c, (f32)W * 0.5f, (f32)hz, (f32)W * 0.45f, C("#bb9af7"), 0.22f);
	const u32 far_lights[] = { mix(C("#2a2e48"), C("#7aa2f7"), 0.35f), mix(C("#2a2e48"), C("#bb9af7"), 0.35f) };
	city(c, hz, (int)((f32)H * 0.10f), (int)((f32)H * 0.34f), C("#2a2e48"), far_lights, 2, 0.10f, 0, 31);
	const u32 lights[] = { C("#7aa2f7"), C("#bb9af7"), C("#e0af68"), C("#7dcfff"), C("#e0af68"), C("#f7768e") };
	city(c, hz, (int)((f32)H * 0.05f), (int)((f32)H * 0.24f), C("#101018"), lights, 6, 0.20f, C("#f7768e"), 77);
	water(c, hz, C("#121220"), 0.45f, C("#7aa2f7"), 3);
	fill(c, 0, hz, W, 1, C("#292e42"));
}

static void carbon(Canvas *c)
{
	int hz = (int)((f32)H * 0.64f);
	const u32 sky[] = { C("#0a0a0a"), C("#121212"), C("#1c1a18") };
	gradient(c, 0, hz, sky, 3);
	stars(c, (int)((f32)hz * 0.6f), W * H / 500, C("#5c5c5c"), 43);
	f32 sx = (f32)W * 0.5f, sy = (f32)hz - (f32)H * 0.1f, sr = (f32)H * 0.2f;
	glow(c, sx, sy, (f32)H * 0.5f, C("#e68e0d"), 0.16f);
	/* the sun, orange to red, cut by bars that widen as it sinks */
	for (int y = (int)(sy - sr); y < hz; y++)
		for (int x = (int)(sx - sr) - 1; x <= (int)(sx + sr) + 1; x++) {
			if (!in_disc(sx, sy, sr, x, y))
				continue;
			f32 t = ((f32)y - (sy - sr)) / (sr * 2);
			f32 below = (f32)y - (sy - sr * 0.2f);
			if (below > 0) {
				int period = MAX(4, (int)(sr * 0.18f));
				int gap = 1 + (int)(below / (sr * 1.2f) * (f32)(period - 1));
				if ((int)below % period < MIN(gap, period - 1))
					continue;
			}
			put(c, x, y, mix(C("#e68e0d"), C("#d35f5f"), dither(t, x, y, 4)));
		}
	int *ridge = mem_alloc(sizeof(int) * (usize)W);
	Range hills = { (f32)hz + 1, (f32)H * 0.07f, 2.8f, 211, 4, 0.8f, C("#0c0c0c"), C("#0c0c0c"), -1, 0, 0, 1 };
	range(c, &hills, ridge);
	for (int x = 0; x < W; x++)
		put(c, x, ridge[x], mix(C("#0c0c0c"), C("#e68e0d"), 0.45f));
	mem_free(ridge);
	/* the floor: a grid running to the horizon */
	fill(c, 0, hz, W, H - hz, C("#0c0c0c"));
	fill(c, 0, hz, W, 1, mix(C("#0c0c0c"), C("#e68e0d"), 0.7f));
	for (int k = 1, last = H + 3; k < 80; k++) {
		int y = hz + (int)((f32)(H - hz) / (0.3f * (f32)k));
		if (y >= H)
			continue;
		if (last - y < 3) /* where they'd run together, stop */
			break;
		last = y;
		f32 z = (f32)(y - hz) / (f32)(H - hz);
		for (int x = 0; x < W; x++)
			tint(c, x, y, C("#e68e0d"), 0.12f + z * 0.6f);
	}
	for (int y = hz + 1; y < H; y++) {
		f32 z = (f32)(y - hz) / (f32)(H - hz);
		f32 step = (f32)W * 0.09f * z;
		f32 fog = clampf((step - 2.5f) / 4, 0, 1); /* far off, where they'd crowd, they fade */
		for (int i = -40; i <= 40 && fog > 0; i++) {
			int x = (int)roundf(sx + (f32)i * step);
			tint(c, x, y, C("#e68e0d"), (0.2f + z * 0.55f) * fog);
		}
	}
}

static const struct {
	const char *id;
	void (*paint)(Canvas *c);
} PICTURES[] = {
	{ "carbon", carbon },
	{ "midnight", midnight },
};

/* The picture for a theme ("carbon", "midnight") at w × h. */
Canvas *bitart_render(const char *id, int w, int h)
{
	id = hive_theme_find(id)->id; /* the old names too */
	void (*paint)(Canvas *) = PICTURES[0].paint;
	for (usize i = 0; i < ARRAY_LEN(PICTURES); i++)
		if (str_eq(id, PICTURES[i].id))
			paint = PICTURES[i].paint;
	w = MAX(w, 1), h = MAX(h, 1);
	/* about two hundred rows of pixels, each p × p on the screen */
	int p = MAX(1, (int)roundf((f32)h / 200));
	int lw = (w + p - 1) / p, lh = (h + p - 1) / p;
	if (h < 200) /* a thumbnail: painted at full size, then made small */
		lh = 200, lw = MAX(1, (int)roundf((f32)w * 200 / (f32)h));
	Canvas *lo = canvas_new(lw, lh);
	canvas_clear(lo, 0xff000000u);
	paint(lo);
	Canvas *out = canvas_new(w, h);
	if (h < 200) {
		blit_scaled(out, lo, rect(0, 0, (f32)lw, (f32)lh), rect(0, 0, (f32)w, (f32)h), 1);
	} else {
		for (int y = 0; y < h; y++) {
			const u32 *s = canvas_row(lo, y / p);
			u32 *d = canvas_row(out, y);
			for (int x = 0; x < w; x++)
				d[x] = s[x / p];
		}
	}
	canvas_free(lo);
	return out;
}
