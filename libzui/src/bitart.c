/*
 * Bit art: Hive's themes, and a pixel-art wallpaper for each.
 *
 * A picture is painted at a low resolution, about two hundred rows whatever
 * the screen, in its theme's colours, with gradients ordered-dithered (a 4×4
 * Bayer pattern) instead of smooth, then blown up with every pixel kept
 * square and sharp. Tokyo Night is a city by a river at night, Catppuccin a
 * cat on a hill watching the moon, Gruvbox a desert at sundown, Nord an
 * aurora over snow, Everforest a forest at dusk with fireflies, Kanagawa
 * Fuji over a sea of waves, Rosé Pine a moonlit lake, Matte Black a sun
 * going down over a grid.
 */
#include "zui.h"

const HiveTheme HIVE_THEMES[] = {
	{ "tokyo-night", "Tokyo Night", "#1a1b26", "#c0caf5", "#7aa2f7", "#bb9af7", "#3b4261", "#565f89" },
	{ "catppuccin", "Catppuccin", "#1e1e2e", "#cdd6f4", "#cba6f7", "#89b4fa", "#45475a", "#6c7086" },
	{ "gruvbox", "Gruvbox", "#282828", "#ebdbb2", "#fe8019", "#fabd2f", "#504945", "#928374" },
	{ "nord", "Nord", "#2e3440", "#d8dee9", "#88c0d0", "#b48ead", "#434c5e", "#616e88" },
	{ "everforest", "Everforest", "#2d353b", "#d3c6aa", "#a7c080", "#83c092", "#475258", "#859289" },
	{ "kanagawa", "Kanagawa", "#1f1f28", "#dcd7ba", "#7e9cd8", "#d27e99", "#363646", "#727169" },
	{ "rose-pine", "Rosé Pine", "#191724", "#e0def4", "#ebbcba", "#c4a7e7", "#2a273f", "#6e6a86" },
	{ "matte-black", "Matte Black", "#121212", "#bebebe", "#e68e0d", "#d35f5f", "#2a2a2a", "#5c5c5c" },
};
const int HIVE_THEME_COUNT = (int)ARRAY_LEN(HIVE_THEMES);

const HiveTheme *hive_theme_find(const char *id)
{
	for (int i = 0; i < HIVE_THEME_COUNT; i++)
		if (id && str_eq(HIVE_THEMES[i].id, id))
			return &HIVE_THEMES[i];
	return &HIVE_THEMES[0];
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

/* A pine, tier on tier, on the ground at (x, base); snow (if not 0) on the
 * top of each tier. */
static void pine(Canvas *c, int x, int base, int h, u32 col, u32 snow)
{
	int trunk = MAX(1, h / 7), tier = MAX(3, (h - trunk) / 4);
	for (int i = 0; i < h - trunk; i++) {
		int y = base - h + i;
		int hw = (int)((f32)(i % tier) * 0.7f + (f32)(i / tier) * 1.1f);
		bool cap = snow && i % tier == 0 && i > 0;
		for (int k = -hw; k <= hw; k++)
			put(c, x + k, y, cap ? snow : col);
	}
	for (int y = base - trunk; y < base; y++)
		put(c, x, y, col);
}

/* A row of pines, the ground beneath filled. */
static void forest(Canvas *c, int base, int hmin, int hmax, int spacing, u32 col, u32 snow, u32 seed)
{
	fill(c, 0, base, W, H - base, col);
	for (int i = 0, x = -3; x < W + 4; i++) {
		int h = hmin + (int)(hashf(i, seed) * (f32)(hmax - hmin));
		pine(c, x, base + 1 + (int)(hashf(i, seed + 5) * 3), h, col, snow);
		x += MAX(2, spacing / 2 + (int)(hashf(i, seed + 9) * (f32)spacing));
	}
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

static void sprite(Canvas *c, int x, int y, const char *const *rows, int n, u32 col)
{
	for (int j = 0; j < n; j++)
		for (int i = 0; rows[j][i]; i++)
			if (rows[j][i] == 'X')
				put(c, x + i, y + j, col);
}

static void bird(Canvas *c, int x, int y, u32 col)
{
	static const char *const B[] = { "X...X", ".X.X.", "..X.." };
	sprite(c, x - 2, y, B, 3, col);
}

/* a cloud: puffs in a row, flat beneath, a lit rim on top */
static void cloud(Canvas *c, f32 x, f32 y, f32 s, u32 body, u32 rim, u32 seed)
{
	int puffs = 4 + (int)(hashf(1, seed) * 3);
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < puffs; i++) {
			f32 u = (f32)i / (f32)(puffs - 1) - 0.5f;
			f32 r = s * (0.55f - fabsf(u) * 0.5f) * (0.8f + hashf(i, seed) * 0.4f);
			f32 px = x + u * s * 2.2f;
			disc(c, px, y - r * 0.6f + (pass ? 1 : 0), r, pass ? body : rim, (int)(y + s * 0.15f));
		}
}

/* ---- The pictures ------------------------------------------------------------------------ */

static void tokyo_night(Canvas *c)
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

static void catppuccin(Canvas *c)
{
	int hz = (int)((f32)H * 0.64f);
	const u32 sky[] = { C("#11111b"), C("#1e1e2e"), C("#313244"), mix(C("#313244"), C("#cba6f7"), 0.38f),
			    mix(C("#45475a"), C("#f5c2e7"), 0.5f) };
	gradient(c, 0, hz, sky, 5);
	stars(c, (int)((f32)hz * 0.7f), W * H / 200, C("#b4befe"), 11);
	f32 mx = (f32)W * 0.3f, my = (f32)H * 0.27f, mr = (f32)H * 0.11f;
	glow(c, mx, my, (f32)H * 0.5f, C("#cba6f7"), 0.38f);
	disc(c, mx, my, mr, C("#f5e0dc"), H);
	disc(c, mx + mr * 0.3f, my - mr * 0.25f, mr * 0.2f, C("#f2cdcd"), H);
	disc(c, mx - mr * 0.35f, my + mr * 0.2f, mr * 0.26f, C("#f2cdcd"), H);
	disc(c, mx + mr * 0.25f, my + mr * 0.5f, mr * 0.12f, C("#f2cdcd"), H);
	cloud(c, (f32)W * 0.62f, (f32)H * 0.2f, (f32)H * 0.07f, C("#313244"), C("#585b70"), 5);
	cloud(c, (f32)W * 0.16f, (f32)H * 0.42f, (f32)H * 0.05f, C("#45475a"), C("#6c7086"), 9);
	cloud(c, (f32)W * 0.86f, (f32)H * 0.36f, (f32)H * 0.055f, C("#45475a"), C("#6c7086"), 13);
	Range far = { (f32)hz, (f32)H * 0.09f, 3.0f, 21, 3, 0, mix(C("#45475a"), C("#cba6f7"), 0.18f),
		      mix(C("#313244"), C("#cba6f7"), 0.12f), -1, 0, 0, (f32)H * 0.05f };
	range(c, &far, NULL);
	int *ridge = mem_alloc(sizeof(int) * (usize)W);
	Range mid = { (f32)H * 0.76f, (f32)H * 0.09f, 2.2f, 33, 2, 0, C("#313244"), C("#292a3c"), -1, 0, 0, (f32)H * 0.05f };
	range(c, &mid, ridge);
	/* two cottages on the hill, their windows lit */
	for (int k = 0; k < 2; k++) {
		int x = (int)((f32)W * (k ? 0.82f : 0.12f)), y = ridge[CLAMP(x + 3, 0, W - 1)];
		glow(c, (f32)x + 3, (f32)y - 2, 10, C("#f9e2af"), 0.35f);
		fill(c, x, y - 4, 7, 6, C("#181825"));
		for (int r = 0; r < 4; r++)
			fill(c, x - 1 + r, y - 5 - r, 9 - 2 * r, 1, C("#181825"));
		put(c, x + 2, y - 2, C("#f9e2af"));
		put(c, x + 4, y - 2, C("#f9e2af"));
	}
	Range near = { (f32)H * 0.88f, (f32)H * 0.06f, 1.6f, 47, 2, 0, C("#181825"), C("#151520"), -1, 0, 0, 1 };
	range(c, &near, ridge);
	/* the cat, sitting with its back to us, watching the moon */
	static const char *const CAT[] = { ".X...X..", ".XX.XX..", ".XXXXX..", ".XXXXX..", "..XXX...", ".XXXXX..",
					   "XXXXXXX.", "XXXXXXX.", "XXXXXXX.X", "XXXXXXX.X", ".XXXXXXXX" };
	int cx = (int)((f32)W * 0.6f), lowest = 0;
	for (int x = cx; x < cx + 9 && x < W; x++)
		lowest = MAX(lowest, ridge[x]);
	sprite(c, cx, lowest - 11 + 1, CAT, 11, C("#11111b"));
	mem_free(ridge);
	/* flowers in the grass */
	for (int i = 0; i < W / 10; i++) {
		int x = (int)(hashf(i, 61) * (f32)W), y = (int)((f32)H * (0.93f + hashf(i, 62) * 0.06f));
		const u32 F[] = { C("#f5c2e7"), C("#f9e2af"), C("#89b4fa"), C("#a6e3a1") };
		put(c, x, y, mix(C("#181825"), F[i % 4], 0.7f));
	}
}

static void gruvbox(Canvas *c)
{
	int hz = (int)((f32)H * 0.66f);
	const u32 sky[] = { C("#1d2021"), C("#282828"), C("#3c3836"), mix(C("#3c3836"), C("#d65d0e"), 0.5f), C("#d65d0e"),
			    C("#fabd2f") };
	gradient(c, 0, hz, sky, 6);
	stars(c, (int)((f32)hz * 0.32f), W * H / 600, C("#ebdbb2"), 19);
	f32 sx = (f32)W * 0.62f, sy = (f32)hz - (f32)H * 0.03f, sr = (f32)H * 0.12f;
	glow(c, sx, sy, (f32)H * 0.55f, C("#fe8019"), 0.5f);
	disc(c, sx, sy, sr, C("#fabd2f"), hz);
	disc(c, sx, sy, sr * 0.82f, mix(C("#fabd2f"), C("#ebdbb2"), 0.35f), hz);
	for (int i = 0; i < 4; i++)
		bird(c, (int)((f32)W * (0.26f + 0.05f * (f32)i)), (int)((f32)H * (0.28f + 0.04f * hashf(i, 3))) + (i % 2) * 4, C("#1d2021"));
	/* mesas: far and hazy, then near with their strata */
	for (int layer = 0; layer < 2; layer++) {
		u32 col = layer ? C("#3c3836") : mix(C("#504945"), C("#d65d0e"), 0.3f);
		u32 band = layer ? C("#32302f") : mix(C("#504945"), C("#d65d0e"), 0.2f);
		int n = layer ? 2 : 3;
		for (int k = 0; k < n; k++) {
			f32 cx = (f32)W * (layer ? (k ? 0.92f : 0.14f) : 0.3f + 0.28f * (f32)k + hashf(k, 5) * 0.1f);
			f32 hw = (f32)W * (layer ? 0.12f : 0.06f + hashf(k, 7) * 0.05f);
			int top = (int)((f32)H * (layer ? 0.44f + 0.05f * (f32)k : 0.53f + hashf(k, 9) * 0.04f));
			for (int y = top; y <= hz; y++) {
				f32 f = (f32)(y - top) / (f32)MAX(1, hz - top);
				f32 spread = f < 0.55f ? f * 4 : 2.2f + (f - 0.55f) * (f32)H * 0.25f;
				u32 v = (y - top) % 6 == 3 && bayer(0, y) > 0.3f ? band : col;
				for (int x = (int)(cx - hw - spread); x <= (int)(cx + hw + spread); x++)
					put(c, x, y, (y - top) % 6 == 3 ? (bayer(x, y) < 0.6f ? band : col) : v);
			}
		}
	}
	const u32 ground[] = { C("#504945"), C("#3c3836"), C("#282828"), C("#1d2021") };
	gradient(c, hz, H, ground, 4);
	for (int i = 0; i < W * (H - hz) / 140; i++) {
		int y = hz + 2 + (int)(hashf(i, 41) * (f32)(H - hz)), x = (int)(hashf(i, 42) * (f32)W);
		fill(c, x, y, 2 + (int)(hashf(i, 43) * 4), 1, mix(get(c, x, y), C("#665c54"), 0.5f));
	}
	struct {
		f32 x, y, h;
	} CACTI[] = { { 0.15f, 0.95f, 0.22f }, { 0.83f, 0.9f, 0.14f }, { 0.46f, 0.8f, 0.07f } };
	for (usize i = 0; i < ARRAY_LEN(CACTI); i++) {
		int x = (int)((f32)W * CACTI[i].x), base = (int)((f32)H * CACTI[i].y), h = (int)((f32)H * CACTI[i].h);
		int tw = MAX(2, h / 7);
		u32 col = C("#1d2021");
		fill(c, x, base - h + 1, tw, h, col);
		fill(c, x + 1, base - h, tw - 2, 1, col);
		int ay = base - h * 45 / 100, ah = h / 4, aw = MAX(2, tw - 1);
		fill(c, x - aw - 1, ay, aw + 1, aw, col);
		fill(c, x - aw - 1, ay - ah, aw, ah, col);
		int by = base - h * 62 / 100, bh = h / 5;
		fill(c, x + tw, by, aw + 1, aw, col);
		fill(c, x + tw + 1, by - bh, aw, bh, col);
	}
}

static void nord(Canvas *c)
{
	int hz = (int)((f32)H * 0.62f);
	const u32 sky[] = { C("#1c2028"), C("#242933"), C("#2e3440"), C("#3b4252"), C("#4c566a") };
	gradient(c, 0, hz, sky, 5);
	stars(c, hz, W * H / 160, C("#e5e9f0"), 23);
	/* the aurora: curtains hanging over the peaks, bright at the hem */
	for (int x = 0; x < W; x++) {
		f32 u = (f32)x / (f32)W;
		f32 hem = (f32)H * (0.27f + 0.09f * noise(u * 2.5f, 71, 2) + 0.025f * sinf(u * 14));
		f32 len = (f32)H * (0.12f + 0.14f * noise(u * 4, 73, 2));
		f32 in = clampf((noise(u * 3.5f, 79, 2) - 0.32f) / 0.3f, 0, 1);
		in = in * in * (3 - 2 * in) * (0.7f + 0.3f * hashf(x / 2, 83));
		for (int y = (int)(hem - len); y <= (int)hem + 2; y++) {
			f32 t = (hem - (f32)y) / len;
			f32 a = t < 0 ? 1 + t * len / 3 : powf(1 - t, 1.3f);
			u32 col = t < 0.45f ? mix(C("#a3be8c"), C("#8fbcbb"), maxf(t, 0) / 0.45f) : mix(C("#8fbcbb"), C("#b48ead"), (t - 0.45f) / 0.55f);
			tint(c, x, y, col, a * in);
		}
	}
	Range far = { (f32)hz, (f32)H * 0.26f, 3.4f, 89, 4, 0.7f, C("#4c566a"), C("#3b4252"), (int)((f32)hz - (f32)H * 0.13f),
		      C("#d8dee9"), mix(C("#d8dee9"), C("#4c566a"), 0.5f), (f32)H * 0.12f };
	range(c, &far, NULL);
	Range near = { (f32)H * 0.76f, (f32)H * 0.17f, 2.4f, 97, 4, 0.6f, C("#434c5e"), C("#363d4b"), (int)((f32)H * 0.66f), C("#e5e9f0"),
		       mix(C("#e5e9f0"), C("#434c5e"), 0.45f), (f32)H * 0.1f };
	range(c, &near, NULL);
	forest(c, (int)((f32)H * 0.8f), (int)((f32)H * 0.05f), (int)((f32)H * 0.11f), 7, C("#2e3440"), C("#d8dee9"), 101);
	Range snow = { (f32)H * 0.86f, (f32)H * 0.05f, 1.4f, 103, 2, 0, C("#d8dee9"), C("#d8dee9"), -1, 0, 0, 1 };
	range(c, &snow, NULL);
	const u32 drift[] = { C("#d8dee9"), C("#e5e9f0"), C("#eceff4") };
	gradient(c, (int)((f32)H * 0.92f), H, drift, 3);
	pine(c, (int)((f32)W * 0.07f), H, (int)((f32)H * 0.3f), C("#242933"), C("#eceff4"));
	pine(c, (int)((f32)W * 0.12f), H + 3, (int)((f32)H * 0.2f), C("#242933"), C("#eceff4"));
	pine(c, (int)((f32)W * 0.9f), H + 2, (int)((f32)H * 0.26f), C("#242933"), C("#eceff4"));
}

static void everforest(Canvas *c)
{
	int hz = (int)((f32)H * 0.56f);
	const u32 sky[] = { C("#1e2326"), C("#2d353b"), C("#3d484d"), mix(C("#475258"), C("#e69875"), 0.45f),
			    mix(C("#56635f"), C("#dbbc7f"), 0.55f) };
	gradient(c, 0, hz, sky, 5);
	stars(c, (int)((f32)hz * 0.4f), W * H / 500, C("#d3c6aa"), 29);
	f32 sx = (f32)W * 0.7f, sy = (f32)H * 0.5f;
	glow(c, sx, sy, (f32)H * 0.5f, C("#e69875"), 0.45f);
	disc(c, sx, sy, (f32)H * 0.06f, C("#dbbc7f"), H);
	for (int i = 0; i < 3; i++)
		bird(c, (int)((f32)W * (0.2f + 0.04f * (f32)i)), (int)((f32)H * 0.22f) + (i % 2) * 3, C("#2d353b"));
	static const char *const LAYER[] = { "#5f6d63", "#4b5851", "#3a4540", "#2a3330", "#1a201f" };
	u32 haze = mix(C("#56635f"), C("#dbbc7f"), 0.4f);
	for (int k = 0; k < 5; k++) {
		int base = (int)((f32)H * (0.56f + 0.095f * (f32)k));
		int hmin = (int)((f32)H * (0.05f + 0.025f * (f32)k)), hmax = (int)((f32)H * (0.10f + 0.05f * (f32)k));
		forest(c, base, hmin, hmax, 5 + k * 2, C(LAYER[k]), 0, 131 + (u32)k * 17);
		if (k < 4)
			for (int y = base - hmin; y < base + 4; y++)
				for (int x = 0; x < W; x++)
					tint(c, x, y, haze, 0.3f * (1 - fabsf((f32)(y - base) / (f32)(hmin + 4))));
	}
	for (int i = 0; i < 18; i++) {
		f32 x = hashf(i, 151) * (f32)W, y = (f32)H * (0.72f + hashf(i, 152) * 0.26f);
		glow(c, x, y, 4, C("#a7c080"), 0.7f);
		put(c, (int)x, (int)y, C("#dbbc7f"));
	}
}

static void kanagawa(Canvas *c)
{
	int hz = (int)((f32)H * 0.6f);
	const u32 sky[] = { C("#16161d"), C("#1f1f28"), C("#2a2a37"), C("#363646"), mix(C("#363646"), C("#c34043"), 0.3f) };
	gradient(c, 0, hz, sky, 5);
	stars(c, (int)((f32)hz * 0.5f), W * H / 400, C("#dcd7ba"), 37);
	f32 sx = (f32)W * 0.72f, sy = (f32)H * 0.24f;
	glow(c, sx, sy, (f32)H * 0.3f, C("#c34043"), 0.25f);
	disc(c, sx, sy, (f32)H * 0.09f, C("#c34043"), H);
	/* bands of cloud, one across the sun */
	struct {
		f32 x, y, w;
	} BANDS[] = { { 0.6f, 0.27f, 0.22f }, { 0.08f, 0.16f, 0.2f }, { 0.78f, 0.4f, 0.18f }, { 0.2f, 0.36f, 0.12f } };
	for (usize i = 0; i < ARRAY_LEN(BANDS); i++) {
		int x = (int)((f32)W * BANDS[i].x), y = (int)((f32)H * BANDS[i].y), w = (int)((f32)W * BANDS[i].w);
		int h = MAX(3, H / 60);
		fill(c, x + 1, y, w - 2, h, C("#2a2a37"));
		fill(c, x, y + 1, w, h - 2, C("#2a2a37"));
		fill(c, x + 2, y, w - 4, 1, C("#363646"));
	}
	/* Fuji */
	f32 fx = (f32)W * 0.36f;
	int peak = (int)((f32)H * 0.3f);
	for (int y = peak; y <= hz; y++) {
		f32 d = (f32)(y - peak);
		f32 hw = (f32)H * 0.035f + d * 0.6f + d * d * 0.006f;
		for (int x = (int)(fx - hw); x <= (int)(fx + hw); x++) {
			bool shaded = (f32)x > fx + d * 0.25f + hashf(y, 3) * 2;
			int teeth = (int)(fabsf(fmodf((f32)x * 0.7f + hashf(x / 3, 5) * 2, 4) - 2) * 3);
			bool snow = y < peak + (int)((f32)H * 0.07f) + teeth;
			put(c, x, y, snow ? (shaded ? C("#c8c093") : C("#dcd7ba")) : shaded ? C("#1a2638") : C("#223249"));
		}
	}
	/* the sea: rows of waves, seigaiha, smaller towards the horizon */
	const u32 rings[] = { C("#223249"), C("#2d4f67"), C("#223249"), C("#2d4f67") };
	fill(c, 0, hz, W, H - hz, mix(C("#223249"), C("#363646"), 0.4f));
	f32 y = (f32)hz - 2;
	for (int j = 0; y - 12 < (f32)H; j++) {
		f32 r = 4.0f + (f32)j * 1.3f;
		f32 cy = y + r;
		f32 fade = clampf(1 - (f32)j / 9, 0, 1) * 0.75f;
		for (f32 cx = (j % 2) ? r : 0; cx < (f32)W + r * 2; cx += r * 2)
			for (int py = MAX(0, (int)(cy - r)); py < MIN(H, (int)(cy + r) + 1); py++)
				for (int px = (int)(cx - r); px <= (int)(cx + r); px++) {
					f32 d = dist((f32)px + 0.5f - cx, (f32)py + 0.5f - cy);
					if (d > r || px < 0 || px >= W)
						continue;
					f32 band = r / 3.4f;
					int k = (int)(d / band);
					f32 edge = d - (f32)k * band;
					bool line = edge < 1 && (r >= 6 || k == 3 || d > r - 1);
					u32 col = line ? (k == 3 || d > r - 1 ? C("#dcd7ba") : C("#7e9cd8")) : rings[k & 3];
					col = mix(col, C("#363646"), fade);
					canvas_row(c, py)[px] = col;
				}
		y += r * 0.9f;
	}
	for (int i = 0; i < 34; i++) {
		int x = (int)(hashf(i, 171) * (f32)W), py = (int)(hashf(i, 172) * (f32)H);
		put(c, x, py, C("#d27e99"));
		if (hashf(i, 173) < 0.5f)
			put(c, x + 1, py + 1, C("#d27e99"));
	}
}

static void rose_pine(Canvas *c)
{
	int hz = (int)((f32)H * 0.62f);
	const u32 sky[] = { C("#100e17"), C("#191724"), C("#1f1d2e"), C("#26233a"), mix(C("#26233a"), C("#ebbcba"), 0.42f) };
	gradient(c, 0, hz, sky, 5);
	stars(c, (int)((f32)hz * 0.75f), W * H / 180, C("#e0def4"), 41);
	f32 mx = (f32)W * 0.64f, my = (f32)H * 0.22f, mr = (f32)H * 0.075f;
	glow(c, mx, my, (f32)H * 0.36f, C("#c4a7e7"), 0.3f);
	/* a crescent: the moon without the disc that hides it */
	for (int y = (int)(my - mr) - 1; y <= (int)(my + mr) + 1; y++)
		for (int x = (int)(mx - mr) - 1; x <= (int)(mx + mr) + 1; x++)
			if (in_disc(mx, my, mr, x, y) && !in_disc(mx + mr * 0.45f, my - mr * 0.25f, mr * 0.88f, x, y))
				put(c, x, y, C("#f6c177"));
	Range far = { (f32)hz + 1, (f32)H * 0.1f, 2.6f, 181, 3, 0.3f, C("#26233a"), C("#211f33"), -1, 0, 0, (f32)H * 0.05f };
	range(c, &far, NULL);
	Range near = { (f32)hz + 1, (f32)H * 0.05f, 1.8f, 187, 2, 0, C("#1f1d2e"), C("#1a1828"), -1, 0, 0, 1 };
	range(c, &near, NULL);
	water(c, hz, C("#191724"), 0.42f, C("#ebbcba"), 5);
	/* the moon's path on the water */
	for (int y = hz + 1; y < H; y++) {
		f32 t = (f32)(y - hz) / (f32)(H - hz);
		int half = (int)(2 + t * (f32)W * 0.04f);
		for (int x = (int)mx - half; x <= (int)mx + half; x++)
			if (hashf(x / 3 + y * 61, 191) < 0.45f - t * 0.25f)
				put(c, x, y, mix(get(c, x, y), C("#f6c177"), 0.6f - t * 0.3f));
	}
	pine(c, (int)((f32)W * 0.86f), hz + 3, (int)((f32)H * 0.36f), C("#12101a"), 0);
	pine(c, (int)((f32)W * 0.93f), hz + 4, (int)((f32)H * 0.26f), C("#12101a"), 0);
	pine(c, (int)((f32)W * 0.8f), hz + 3, (int)((f32)H * 0.18f), C("#12101a"), 0);
	fill(c, (int)((f32)W * 0.76f), hz + 1, W, 3, C("#12101a"));
	for (int i = 0; i < 28; i++) {
		int x = (int)(hashf(i, 197) * (f32)W), py = (int)(hashf(i, 198) * (f32)H);
		u32 col = i % 3 ? C("#ebbcba") : C("#eb6f92");
		put(c, x, py, col);
		if (hashf(i, 199) < 0.5f)
			put(c, x + 1, py, col);
	}
}

static void matte_black(Canvas *c)
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
	{ "tokyo-night", tokyo_night }, { "catppuccin", catppuccin }, { "gruvbox", gruvbox }, { "nord", nord },
	{ "everforest", everforest },   { "kanagawa", kanagawa },     { "rose-pine", rose_pine }, { "matte-black", matte_black },
};

/* The picture for a theme ("tokyo-night"...) at w × h. */
Canvas *bitart_render(const char *id, int w, int h)
{
	void (*paint)(Canvas *) = PICTURES[0].paint;
	for (usize i = 0; i < ARRAY_LEN(PICTURES); i++)
		if (id && str_eq(id, PICTURES[i].id))
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
