/*
 * Wallpapers: the pictures that ship with Zurbax and the procedural
 * landscapes, rendered at any size. Used by the desktop and by Settings
 * for previews.
 */
#include "zui.h"

const char *const WALLPAPER_NAMES[] = { "silk", "horizon", "daybreak", "golden-rain", "lake", "ridgeline", "glacier", "nightfall", "ember",
					"meadow" };
const char *const WALLPAPER_TITLES[] = { "Silk", "Horizon", "Daybreak", "Golden Rain", "Lake at Dusk", "Ridgeline", "Glacier", "Nightfall",
					 "Ember", "Meadow" };
const int WALLPAPER_COUNT = 10;

static f32 hash1(int i, u32 seed)
{
	u32 x = (u32)i * 0x9e3779b1u ^ seed * 0x85ebca6bu;
	x ^= x >> 15;
	x *= 0x2c1b3c6du;
	x ^= x >> 12;
	return (f32)(x & 0xffffff) / (f32)0xffffff;
}

/* Smooth 1-D value noise with several octaves: natural ridgelines. */
static f32 ridge(f32 x, u32 seed, int octaves)
{
	f32 sum = 0, amp = 1, freq = 1, norm = 0;
	for (int o = 0; o < octaves; o++) {
		f32 p = x * freq;
		int i = (int)floorf(p);
		f32 t = p - (f32)i;
		t = t * t * (3 - 2 * t);
		f32 a = hash1(i, seed + (u32)o * 977), b = hash1(i + 1, seed + (u32)o * 977);
		sum += (a + (b - a) * t) * amp;
		norm += amp;
		amp *= 0.5f;
		freq *= 2.1f;
	}
	return sum / norm;
}

static void add_grain(Canvas *c, u32 seed, f32 strength)
{
	u32 state = seed | 1;
	for (int y = 0; y < c->height; y++) {
		u32 *row = canvas_row(c, y);
		for (int x = 0; x < c->width; x++) {
			state ^= state << 13, state ^= state >> 17, state ^= state << 5;
			int n = (int)((f32)((int)(state & 0xff) - 128) * strength);
			u32 p = row[x];
			int r = CLAMP((int)((p >> 16) & 255) + n, 0, 255), g = CLAMP((int)((p >> 8) & 255) + n, 0, 255),
			    b = CLAMP((int)(p & 255) + n, 0, 255);
			row[x] = 0xff000000u | (u32)r << 16 | (u32)g << 8 | (u32)b;
		}
	}
}

typedef struct {
	const char *name;
	const char *sky_top, *sky_mid, *sky_bottom, *sun;
	const char *layers[5];
	f32 sun_x, sun_y;
	bool stars;
} Landscape;

static const Landscape LANDSCAPES[] = {
	{ "ridgeline", "#1b2a4a", "#5b6d9a", "#f2b58c", "#ffe2b8",
	  { "#8d8fb3", "#6c6f99", "#4d5180", "#343865", "#1f2347" }, 0.68f, 0.58f, false },
	{ "glacier", "#5f8fc4", "#a9c9e6", "#eef4f8", "#ffffff",
	  { "#b9cde0", "#93b0cc", "#6d8fb0", "#4b6d90", "#2d4a6b" }, 0.3f, 0.35f, false },
	{ "nightfall", "#05070f", "#141c38", "#3b3570", "#c9b8ff",
	  { "#2b2b55", "#22224a", "#1a1a3c", "#12122e", "#0a0a1e" }, 0.75f, 0.3f, true },
	{ "ember", "#2a0f2e", "#8a2f4a", "#ff9a5a", "#fff0c2",
	  { "#b9566a", "#8f3c5a", "#672a4a", "#45193a", "#26091f" }, 0.5f, 0.62f, false },
	{ "meadow", "#7fb5d8", "#c3dde9", "#f4efd8", "#fffbe8",
	  { "#9fc1a0", "#7aa67d", "#5a8c60", "#3f7046", "#28532f" }, 0.25f, 0.4f, false },
};

/* Scales a picture to cover the screen. focus_y picks which part of the
 * picture stays when the top or bottom has to be cropped (0 top, 1 bottom). */
static Canvas *cover(Canvas *img, int w, int h, f32 focus_y)
{
	Canvas *c = canvas_new(w, h);
	f32 s = maxf((f32)w / (f32)img->width, (f32)h / (f32)img->height);
	f32 sw = (f32)w / s, sh = (f32)h / s;
	Rect src = rect(((f32)img->width - sw) / 2, ((f32)img->height - sh) * focus_y, sw, sh);
	blit_scaled(c, img, src, rect(0, 0, (f32)w, (f32)h), 1); /* box-filters large downscales */
	return c;
}

/* Pictures that ship with Zurbax, in /system/share/wallpapers. */
static const struct {
	const char *name, *file;
	f32 focus_y;
} BUILTIN_PHOTOS[] = {
	{ "lake", "lake.png", 0.72f },
};

/* A streak of light that fades out toward its tail. */
static void light_streak(Canvas *c, f32 x0, f32 y0, f32 x1, f32 y1, f32 width, Color col, f32 alpha)
{
	Path p = { 0 };
	path_move(&p, x0, y0);
	path_line(&p, x1, y1);
	Paint paint = paint_linear(x0, y0, x1, y1, color_scale_alpha(col, 0), color_scale_alpha(col, alpha));
	StrokeStyle st = { .width = width, .cap = CAP_ROUND, .join = JOIN_ROUND, .miter_limit = 4 };
	stroke_path(c, &p, mat_identity(), &paint, &st);
	path_free(&p);
}

/* Golden Rain, the picture of Zurbax 2.0: a lake at sundown under a shower of
 * golden light, with ripples where the drops land. */
static Canvas *golden_rain(int w, int h)
{
	Canvas *c = canvas_new(w, h);
	f32 fw = (f32)w, fh = (f32)h, k = fw / 1920;
	f32 horizon = fh * 0.62f, sx = fw * 0.5f, sy = horizon - fh * 0.035f;
	/* the sky, from night above to gold at the water */
	Paint sky = paint_linear(0, 0, 0, horizon, color_hex("#1d1640"), color_hex("#ffc56e"));
	paint_add_stop(&sky, 0.38f, color_hex("#5a326a"));
	paint_add_stop(&sky, 0.68f, color_hex("#c9636c"));
	paint_add_stop(&sky, 0.88f, color_hex("#f3955f"));
	fill_rect_paint(c, rect(0, 0, fw, horizon + 2), &sky);
	for (int i = 0; i < w * h / 9000; i++) {
		f32 x = hash1(i, 501) * fw, y = hash1(i, 503) * horizon * 0.42f;
		fill_circle(c, x, y, (0.4f + hash1(i, 505) * 0.9f) * k, color_scale_alpha(COLOR_WHITE, 0.15f + hash1(i, 507) * 0.5f * (1 - y / (horizon * 0.42f))));
	}
	/* the sun sinking into the lake, and its glow */
	Paint glow = paint_radial(sx, sy, fw * 0.55f, color_scale_alpha(color_hex("#ffd27a"), 0.7f), color_scale_alpha(color_hex("#ff9a5a"), 0));
	fill_rect_paint(c, rect(0, 0, fw, horizon), &glow);
	Paint core = paint_radial(sx, sy, fw * 0.09f, color_scale_alpha(color_hex("#fff4d6"), 0.95f), color_scale_alpha(color_hex("#ffd27a"), 0));
	fill_rect_paint(c, rect(sx - fw * 0.1f, sy - fw * 0.1f, fw * 0.2f, fw * 0.2f), &core);
	fill_circle(c, sx, sy, fw * 0.034f, color_hex("#fff3d4"));
	/* distant hills along the shore, hazy with light */
	static const char *const HILLS[] = { "#c47a7c", "#8f5272", "#5c3557" };
	for (int layer = 0; layer < 3; layer++) {
		f32 amp = fh * (0.085f - 0.02f * (f32)layer), scale = 2.5f + 1.7f * (f32)layer;
		Path p = { 0 };
		path_move(&p, 0, horizon + 1);
		for (int x = 0; x <= w; x += MAX(2, w / 640)) {
			f32 fx = (f32)x / fw;
			f32 edge = minf(1, minf(fx, 1 - fx) * (3.2f - (f32)layer * 0.6f)); /* low by the sun, higher at the sides */
			f32 n = ridge(fx * scale, 900 + (u32)layer * 17, 6);
			path_line(&p, (f32)x, horizon - amp * (0.25f + n * 0.9f) * (1.2f - edge * 0.85f));
		}
		path_line(&p, fw, horizon + 1);
		path_close(&p);
		Color col = color_hex(HILLS[layer]);
		Paint fill = paint_linear(0, horizon - amp * 1.2f, 0, horizon, col, color_mix(col, color_hex("#ffb26b"), 0.35f));
		fill_path(c, &p, mat_identity(), &fill, FILL_NONZERO);
		path_free(&p);
	}
	/* the lake: the sky again, darker, and a road of light under the sun */
	Paint lake = paint_linear(0, horizon, 0, fh, color_hex("#f0a46a"), color_hex("#1a1233"));
	paint_add_stop(&lake, 0.22f, color_hex("#b45c6c"));
	paint_add_stop(&lake, 0.55f, color_hex("#4c2a58"));
	fill_rect_paint(c, rect(0, horizon, fw, fh - horizon), &lake);
	for (int i = 0; i < 900; i++) {
		f32 t = hash1(i, 601);
		f32 y = horizon + 2 + t * t * (fh - horizon);
		f32 spread = (0.012f + t * 0.16f) * fw;
		f32 x = sx + (hash1(i, 603) * 2 - 1) * spread * (0.4f + hash1(i, 607) * 0.6f);
		f32 len = (6 + hash1(i, 605) * 30) * k * (0.5f + t);
		f32 a = (0.85f - t * 0.6f) * (1 - fabsf(x - sx) / (spread + 1)) * (0.5f + hash1(i, 609) * 0.5f);
		fill_rect(c, rect(x - len / 2, y, len, maxf(1, (1 + t * 2) * k)), color_scale_alpha(color_hex("#ffe1a0"), clampf(a, 0, 1)));
	}
	/* ripples where the rain meets the water */
	for (int i = 0; i < 70; i++) {
		f32 t = hash1(i, 701);
		f32 y = horizon + fh * 0.02f + t * (fh - horizon) * 0.95f;
		f32 x = hash1(i, 703) * fw, r = (8 + hash1(i, 705) * 34) * k * (0.4f + t * 1.2f);
		Path p = { 0 };
		path_ellipse(&p, x, y, r, r * 0.22f);
		StrokeStyle st = { .width = maxf(0.8f, 1.3f * k), .cap = CAP_ROUND, .join = JOIN_ROUND, .miter_limit = 4 };
		Paint ring = paint_solid(color_scale_alpha(color_hex("#ffd9a0"), 0.1f + 0.25f * (1 - t)));
		stroke_path(c, &p, mat_identity(), &ring, &st);
		path_free(&p);
	}
	/* a dark shore in the corners to frame it */
	for (int side = 0; side < 2; side++) {
		Path p = { 0 };
		f32 x0 = side ? fw : 0, dir = side ? -1 : 1;
		path_move(&p, x0, fh * (0.80f + 0.03f * (f32)side));
		path_cubic(&p, x0 + dir * fw * 0.12f, fh * 0.80f, x0 + dir * fw * 0.2f, fh * 0.9f, x0 + dir * fw * (0.34f - 0.06f * (f32)side), fh);
		path_line(&p, x0, fh);
		path_close(&p);
		Paint shore = paint_linear(0, fh * 0.8f, 0, fh, color_hex("#20152f"), color_hex("#0d0918"));
		fill_path(c, &p, mat_identity(), &shore, FILL_NONZERO);
		path_free(&p);
	}
	/* the golden rain: streaks of light falling slightly aslant, thicker near */
	f32 slant = 0.16f;
	int drops = w * h / 5200;
	for (int i = 0; i < drops; i++) {
		f32 x = hash1(i, 801) * (fw + fh * slant) - fh * slant, y = hash1(i, 803) * fh * 1.05f - fh * 0.05f;
		f32 depth = hash1(i, 805);                      /* 0 far .. 1 near */
		f32 len = (18 + depth * depth * 90) * k;
		f32 width = (0.6f + depth * depth * 2.2f) * k;
		f32 near_sun = 1 - minf(1, sqrtf((x - sx) * (x - sx) + (y - sy) * (y - sy)) / (fw * 0.6f));
		f32 a = (0.22f + depth * 0.5f) * (0.6f + near_sun * 0.6f);
		Color col = color_mix(color_hex("#ffbe4d"), color_hex("#fff0c0"), near_sun * near_sun);
		light_streak(c, x - len * slant, y - len, x, y, width, col, clampf(a, 0, 1));
	}
	/* drops caught in the light: soft glowing beads */
	for (int i = 0; i < 46; i++) {
		f32 x = hash1(i, 901) * fw, y = hash1(i, 903) * fh * 0.85f;
		f32 r = (4 + hash1(i, 905) * hash1(i, 907) * 26) * k;
		Paint bead = paint_radial(x, y, r, color_scale_alpha(color_hex("#ffe3a3"), 0.35f + hash1(i, 909) * 0.25f), color_scale_alpha(color_hex("#ffcf70"), 0));
		fill_rect_paint(c, rect(x - r, y - r, 2 * r, 2 * r), &bead);
	}
	add_grain(c, 4242, 0.02f);
	return c;
}

/* Horizon, the picture of Zurbax: night lifting off a calm world. Soft hills
 * sweep across the screen in layers, each with light running along its
 * crest, under a sky that goes from deep ink to iris to the first gold of
 * the day. Daybreak is the same scene a little later, in pale light. */
typedef struct {
	const char *sky[5];          /* top .. horizon */
	const char *glow, *rim;
	const char *hills[4][2];     /* far .. near: crest color, foot color */
	f32 rim_alpha;
} Dawn;

static const Dawn HORIZON = {
	{ "#06071a", "#111342", "#29287c", "#6250c8", "#f4b47c" },
	"#ffc98a", "#ffd9a6",
	{ { "#2f2a7c", "#1d1955" }, { "#231e66", "#13103b" }, { "#17134a", "#0b0927" }, { "#0e0b30", "#05040e" } },
	0.85f,
};

static const Dawn DAYBREAK = {
	{ "#7f8fd6", "#a7aee6", "#cfc8ef", "#f3d9e4", "#ffe6c4" },
	"#fff1d6", "#ffffff",
	{ { "#b9b4ea", "#a59fe0" }, { "#9c96dc", "#837ccf" }, { "#7d76c8", "#615aae" }, { "#5d55aa", "#3f3884" } },
	0.7f,
};

/* One hill: a smooth crest from the left edge to the right, as a path. */
static void hill_crest(Path *p, f32 fw, f32 y, f32 amp, f32 freq, f32 phase, f32 tilt)
{
	int steps = 128;
	for (int i = 0; i <= steps; i++) {
		f32 u = (f32)i / (f32)steps, x = u * fw;
		f32 yy = y + tilt * (u - 0.5f) - amp * (0.8f * sinf((u * freq + phase) * PI_F) + 0.2f * sinf((u * freq * 2.2f + phase * 1.7f) * PI_F));
		if (i == 0)
			path_move(p, x, yy);
		else
			path_line(p, x, yy);
	}
}

static Canvas *dawn(const Dawn *d, int w, int h)
{
	Canvas *c = canvas_new(w, h);
	f32 fw = (f32)w, fh = (f32)h, k = fh / 1080;
	f32 horizon = fh * 0.66f, gx = fw * 0.6f;
	Paint sky = paint_linear(0, 0, 0, horizon, color_hex(d->sky[0]), color_hex(d->sky[4]));
	paint_add_stop(&sky, 0.3f, color_hex(d->sky[1]));
	paint_add_stop(&sky, 0.62f, color_hex(d->sky[2]));
	paint_add_stop(&sky, 0.86f, color_hex(d->sky[3]));
	fill_rect_paint(c, rect(0, 0, fw, horizon + fh * 0.1f), &sky);
	/* the light before the sun, low over the hills */
	Paint glow = paint_radial(gx, horizon, fw * 0.4f, color_scale_alpha(color_hex(d->glow), 0.42f), color_scale_alpha(color_hex(d->glow), 0));
	paint_add_stop(&glow, 0.45f, color_scale_alpha(color_hex(d->glow), 0.12f));
	fill_rect_paint(c, rect(0, 0, fw, fh), &glow);
	Paint core = paint_radial(gx, horizon, fw * 0.14f, color_scale_alpha(color_hex(d->glow), 0.7f), color_scale_alpha(color_hex(d->glow), 0));
	fill_rect_paint(c, rect(gx - fw * 0.2f, horizon - fw * 0.2f, fw * 0.4f, fw * 0.4f), &core);
	blur_region(c, rect(0, 0, fw, fh), 24 * k);
	/* the hills, far to near, each crest catching the light */
	static const f32 Y[] = { 0.67f, 0.76f, 0.85f, 0.95f }, AMP[] = { 0.06f, 0.075f, 0.07f, 0.06f }, FREQ[] = { 0.9f, 1.25f, 1.05f, 1.5f },
			 PHASE[] = { 0.1f, 0.85f, 0.35f, 1.3f }, TILT[] = { -0.03f, 0.05f, -0.06f, 0.04f };
	for (int i = 0; i < 4; i++) {
		f32 y = fh * Y[i];
		Path p = { 0 };
		hill_crest(&p, fw, y, fh * AMP[i], FREQ[i], PHASE[i], fh * TILT[i]);
		path_line(&p, fw, fh);
		path_line(&p, 0, fh);
		path_close(&p);
		Paint fill = paint_linear(0, y - fh * 0.06f, 0, fh, color_hex(d->hills[i][0]), color_hex(d->hills[i][1]));
		fill_path(c, &p, mat_identity(), &fill, FILL_NONZERO);
		path_free(&p);
		/* the crest: bright under the glow, fading toward the edges */
		Path crest = { 0 };
		hill_crest(&crest, fw, y, fh * AMP[i], FREQ[i], PHASE[i], fh * TILT[i]);
		Paint line = paint_linear(0, 0, fw, 0, color_scale_alpha(color_hex(d->rim), 0), color_scale_alpha(color_hex(d->rim), 0));
		f32 a = d->rim_alpha * (1 - 0.15f * (f32)i);
		paint_add_stop(&line, clampf(gx / fw - 0.32f, 0.02f, 0.5f), color_scale_alpha(color_hex(d->rim), a * 0.15f));
		paint_add_stop(&line, gx / fw, color_scale_alpha(color_hex(d->rim), a));
		paint_add_stop(&line, clampf(gx / fw + 0.3f, 0.5f, 0.98f), color_scale_alpha(color_hex(d->rim), a * 0.15f));
		StrokeStyle st = { .width = maxf(1, 1.6f * k), .cap = CAP_ROUND, .join = JOIN_ROUND, .miter_limit = 4 };
		stroke_path(c, &crest, mat_identity(), &line, &st);
		/* and a haze of the same light just above it */
		StrokeStyle haze = { .width = 14 * k, .cap = CAP_ROUND, .join = JOIN_ROUND, .miter_limit = 4 };
		Paint soft = line;
		for (int s = 0; s < soft.nstops; s++)
			soft.stops[s].color = color_scale_alpha(soft.stops[s].color, 0.12f);
		stroke_path(c, &crest, mat_identity(), &soft, &haze);
		path_free(&crest);
	}
	add_grain(c, 2020, 0.018f);
	return c;
}

/* ---- Silk ----------------------------------------------------------------------------- */
/* Silk, Zurbax 2.0's picture: folds of warm light, coral to amber, drifting
 * up across deep aubergine like a length of silk caught in the evening sun.
 * Each fold is a ribbon that narrows where it turns; light runs along its
 * upper edge, the far ones softened by distance. */

typedef struct {
	f32 y0, y1;                  /* where the fold enters (left) and leaves (right), in heights */
	f32 lift;                    /* how far its middle rises */
	f32 width;                   /* thickest, in heights */
	f32 turn;                    /* where along it the fold twists (0..1) */
	const char *from, *to;       /* its colours, left to right */
	f32 alpha, sheen;
} Fold;

static void fold_point(const Fold *f, f32 fw, f32 fh, f32 u, f32 *x, f32 *y)
{
	/* a cubic sweep from the lower left up to the right */
	f32 x0 = -0.12f * fw, x3 = 1.12f * fw;
	f32 y0 = f->y0 * fh, y3 = f->y1 * fh;
	f32 x1 = 0.34f * fw, y1 = y0 - f->lift * fh, x2 = 0.58f * fw, y2 = y3 + f->lift * 0.7f * fh;
	f32 a = 1 - u;
	*x = a * a * a * x0 + 3 * a * a * u * x1 + 3 * a * u * u * x2 + u * u * u * x3;
	*y = a * a * a * y0 + 3 * a * a * u * y1 + 3 * a * u * u * y2 + u * u * u * y3;
}

static void draw_fold(Canvas *c, const Fold *f, f32 fw, f32 fh)
{
	enum { N = 180 };
	f32 top[N + 1][2], bot[N + 1][2];
	for (int i = 0; i <= N; i++) {
		f32 u = (f32)i / N, x, y, xn, yn;
		fold_point(f, fw, fh, u, &x, &y);
		fold_point(f, fw, fh, minf(1, u + 0.002f), &xn, &yn);
		if (i == N)
			fold_point(f, fw, fh, u - 0.002f, &xn, &yn), xn = 2 * x - xn, yn = 2 * y - yn;
		f32 dx = xn - x, dy = yn - y, len = sqrtf(dx * dx + dy * dy);
		f32 nx = -dy / len, ny = dx / len;
		/* thick in the open, thin where the silk turns over (smoothly: no crease) */
		f32 t = (u - f->turn) * 3.2f;
		f32 th = f->width * fh * (0.06f + 0.94f * (1 - expf(-t * t))) * (0.5f + 0.5f * sinf(u * PI_F));
		top[i][0] = x + nx * th * 0.5f, top[i][1] = y + ny * th * 0.5f;
		bot[i][0] = x - nx * th * 0.5f, bot[i][1] = y - ny * th * 0.5f;
	}
	Path p = { 0 };
	path_move(&p, top[0][0], top[0][1]);
	for (int i = 1; i <= N; i++)
		path_line(&p, top[i][0], top[i][1]);
	for (int i = N; i >= 0; i--)
		path_line(&p, bot[i][0], bot[i][1]);
	path_close(&p);
	Paint body = paint_linear(0, 0, fw, 0, color_scale_alpha(color_hex(f->from), f->alpha), color_scale_alpha(color_hex(f->to), f->alpha));
	paint_add_stop(&body, f->turn, color_scale_alpha(color_mix(color_hex(f->from), color_hex(f->to), 0.5f), f->alpha * 0.7f));
	fill_path(c, &p, mat_identity(), &body, FILL_NONZERO);
	/* the shadow inside the fold: darker toward its lower edge */
	Paint shade = paint_linear(0, fh * (f->y0 - f->lift), 0, fh * (f->y0 + f->width), color_hex("#00000000"), color_hex("#2a061f55"));
	fill_path(c, &p, mat_identity(), &shade, FILL_NONZERO);
	path_free(&p);
	/* light along the upper edge, brightest in the open */
	Path edge = { 0 };
	path_move(&edge, top[0][0], top[0][1]);
	for (int i = 1; i <= N; i++)
		path_line(&edge, top[i][0], top[i][1]);
	Color light = color_hex("#ffd9b0");
	Paint sheen = paint_linear(0, 0, fw, 0, color_scale_alpha(light, 0), color_scale_alpha(light, 0));
	paint_add_stop(&sheen, clampf(f->turn - 0.3f, 0.02f, 0.9f), color_scale_alpha(light, f->sheen));
	paint_add_stop(&sheen, clampf(f->turn - 0.04f, 0.05f, 0.95f), color_scale_alpha(light, f->sheen * 0.2f));
	paint_add_stop(&sheen, clampf(f->turn + 0.25f, 0.1f, 0.98f), color_scale_alpha(light, f->sheen * 0.8f));
	f32 k = fh / 1080;
	StrokeStyle thin = { .width = maxf(1, 1.5f * k), .cap = CAP_ROUND, .join = JOIN_ROUND, .miter_limit = 4 };
	stroke_path(c, &edge, mat_identity(), &sheen, &thin);
	Paint glow = sheen;
	for (int s = 0; s < glow.nstops; s++)
		glow.stops[s].color = color_scale_alpha(glow.stops[s].color, 0.18f);
	StrokeStyle wide = { .width = 18 * k, .cap = CAP_ROUND, .join = JOIN_ROUND, .miter_limit = 4 };
	stroke_path(c, &edge, mat_identity(), &glow, &wide);
	path_free(&edge);
}

static Canvas *silk(int w, int h)
{
	Canvas *c = canvas_new(w, h);
	f32 fw = (f32)w, fh = (f32)h, k = fh / 1080;
	/* deep aubergine, warming toward the lower right */
	Paint base = paint_linear(0, 0, fw, fh, color_hex("#140414"), color_hex("#5c1534"));
	paint_add_stop(&base, 0.45f, color_hex("#2c0a2b"));
	paint_add_stop(&base, 0.78f, color_hex("#47112f"));
	fill_rect_paint(c, rect(0, 0, fw, fh), &base);
	Paint warm = paint_radial(fw * 0.86f, fh * 1.02f, fw * 0.75f, color_hex("#e0582a8c"), color_hex("#e0582a00"));
	paint_add_stop(&warm, 0.4f, color_hex("#b13a3a40"));
	fill_rect_paint(c, rect(0, 0, fw, fh), &warm);
	/* far folds, softened by distance: a wide veil high up, two more below */
	static const Fold FAR[] = {
		{ 0.46f, -0.02f, 0.16f, 0.30f, 0.70f, "#4a1240", "#a8344a", 0.5f, 0.18f },
		{ 0.74f, 0.20f, 0.24f, 0.22f, 0.58f, "#6e1d4f", "#c9493e", 0.55f, 0.3f },
		{ 0.96f, 0.42f, 0.20f, 0.17f, 0.36f, "#7d2150", "#dc6238", 0.5f, 0.24f },
	};
	for (usize i = 0; i < ARRAY_LEN(FAR); i++)
		draw_fold(c, &FAR[i], fw, fh);
	blur_region(c, rect(0, 0, fw, fh), 28 * k);
	/* near folds, crisp */
	static const Fold NEAR[] = {
		{ 1.04f, 0.28f, 0.30f, 0.15f, 0.56f, "#8a2652", "#f07a3a", 0.82f, 0.75f },
		{ 1.14f, 0.56f, 0.22f, 0.13f, 0.33f, "#a3304f", "#f6a04d", 0.88f, 0.9f },
		{ 1.26f, 0.80f, 0.12f, 0.11f, 0.70f, "#b33c4c", "#ffb85c", 0.78f, 0.7f },
	};
	for (usize i = 0; i < ARRAY_LEN(NEAR); i++)
		draw_fold(c, &NEAR[i], fw, fh);
	/* a soft vignette keeps the corners quiet behind the bars */
	Paint vig = paint_radial(fw * 0.5f, fh * 0.45f, fw * 0.8f, color_hex("#00000000"), color_hex("#0a020c80"));
	paint_add_stop(&vig, 0.6f, color_hex("#00000000"));
	fill_rect_paint(c, rect(0, 0, fw, fh), &vig);
	add_grain(c, 4242, 0.014f);
	return c;
}

Canvas *wallpaper_render(const char *name, int w, int h)
{
	if (!name || !*name || str_eq(name, "silk"))
		return silk(w, h);
	if (str_eq(name, "horizon"))
		return dawn(&HORIZON, w, h);
	if (str_eq(name, "daybreak"))
		return dawn(&DAYBREAK, w, h);
	if (name && str_eq(name, "golden-rain"))
		return golden_rain(w, h);
	/* A user picture? */
	if (name && name[0] == '/') {
		Canvas *img = image_load(name);
		if (img) {
			Canvas *c = cover(img, w, h, 0.5f);
			canvas_free(img);
			return c;
		}
	}
	for (usize i = 0; i < ARRAY_LEN(BUILTIN_PHOTOS); i++) {
		if (!name || !str_eq(name, BUILTIN_PHOTOS[i].name))
			continue;
		char *rel = str_fmt("share/wallpapers/%s", BUILTIN_PHOTOS[i].file);
		char *path = zurbax_path(rel);
		Canvas *img = image_load(path);
		mem_free(rel);
		mem_free(path);
		if (img) {
			Canvas *c = cover(img, w, h, BUILTIN_PHOTOS[i].focus_y);
			canvas_free(img);
			return c;
		}
	}
	const Landscape *L = &LANDSCAPES[0];
	for (usize i = 0; i < ARRAY_LEN(LANDSCAPES); i++)
		if (name && str_eq(name, LANDSCAPES[i].name))
			L = &LANDSCAPES[i];
	Canvas *c = canvas_new(w, h);
	f32 fw = (f32)w, fh = (f32)h;
	Paint sky = paint_linear(0, 0, 0, fh * 0.72f, color_hex(L->sky_top), color_hex(L->sky_bottom));
	paint_add_stop(&sky, 0.55f, color_hex(L->sky_mid));
	fill_rect_paint(c, rect(0, 0, fw, fh), &sky);
	if (L->stars) {
		for (int i = 0; i < w * h / 2500; i++) {
			f32 x = hash1(i, 11) * fw, y = hash1(i, 23) * fh * 0.6f;
			f32 r = 0.4f + hash1(i, 37) * 1.1f * (fw / 1920);
			fill_circle(c, x, y, r, color_scale_alpha(COLOR_WHITE, 0.3f + hash1(i, 41) * 0.6f));
		}
	}
	f32 sx = fw * L->sun_x, sy = fh * L->sun_y;
	Paint glow = paint_radial(sx, sy, fw * 0.45f, color_scale_alpha(color_hex(L->sun), 0.55f), color_scale_alpha(color_hex(L->sun), 0));
	fill_rect_paint(c, rect(0, 0, fw, fh), &glow);
	fill_circle(c, sx, sy, fw * 0.022f, color_scale_alpha(color_hex(L->sun), 0.95f));
	/* Mountain ranges, far to near; haze lightens the distant ones. */
	for (int layer = 0; layer < 5; layer++) {
		f32 base = fh * (0.52f + 0.09f * (f32)layer);
		f32 amp = fh * (0.16f - 0.018f * (f32)layer);
		f32 scale = 2.2f + (f32)layer * 1.3f;
		Path p = { 0 };
		path_move(&p, 0, fh);
		for (int x = 0; x <= w; x += MAX(2, w / 640)) {
			f32 n = ridge((f32)x / fw * scale, 101 + (u32)layer * 31, 6);
			f32 sharp = 1 - fabsf(ridge((f32)x / fw * scale * 0.7f, 7 + (u32)layer, 3) * 2 - 1);
			path_line(&p, (f32)x, base - amp * (n * 0.75f + sharp * 0.35f));
		}
		path_line(&p, fw, fh);
		path_close(&p);
		Color col = color_hex(L->layers[layer]);
		Paint fill = paint_linear(0, base - amp, 0, fh, col, color_lighten(col, -0.25f));
		fill_path(c, &p, mat_identity(), &fill, FILL_NONZERO);
		path_free(&p);
		/* A band of mist in front of each range. */
		Paint mist = paint_linear(0, base - amp * 0.2f, 0, base + fh * 0.05f, color_scale_alpha(color_hex(L->sky_bottom), 0),
					  color_scale_alpha(color_hex(L->sky_bottom), 0.22f - 0.04f * (f32)layer));
		fill_rect_paint(c, rect(0, base - amp * 0.2f, fw, fh * 0.12f), &mist);
	}
	add_grain(c, 1234, 0.025f);
	return c;
}

