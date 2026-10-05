/*
 * Wallpapers: the pictures that ship with Zurbax and the procedural
 * landscapes, rendered at any size. Used by the desktop and by Settings
 * for previews.
 */
#include "zui.h"

const char *const WALLPAPER_NAMES[] = { "lake", "ridgeline", "glacier", "nightfall", "ember", "meadow" };
const char *const WALLPAPER_TITLES[] = { "Lake at Dusk", "Ridgeline", "Glacier", "Nightfall", "Ember", "Meadow" };
const int WALLPAPER_COUNT = 6;

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

Canvas *wallpaper_render(const char *name, int w, int h)
{
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

