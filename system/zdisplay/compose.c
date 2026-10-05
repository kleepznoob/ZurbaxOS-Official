/* Compositing: wallpaper, window shadows and contents, shell layers, cursor. */
#include "zdisplay.h"

/* ---- Window shadows (nine-slice) ------------------------------------------------- */

typedef struct {
	Canvas *img;
	int margin;     /* pixels of shadow outside the window edge */
	int corner;     /* size of a corner slice */
	f32 radius, blur, dy, alpha;
} Shadow;

static Shadow shadow_focused, shadow_normal;

static void shadow_build(Shadow *s, f32 radius, f32 blur, f32 dy, f32 alpha)
{
	canvas_free(s->img);
	s->radius = radius, s->blur = blur, s->dy = dy, s->alpha = alpha;
	s->margin = (int)ceilf(blur * 1.6f + fabsf(dy)) + 2;
	s->corner = s->margin + (int)ceilf(radius) + 2;
	int size = s->corner * 2 + 1;
	s->img = canvas_new(size, size);
	Rect shape = rect((f32)s->margin, (f32)s->margin + dy, (f32)(size - 2 * s->margin), (f32)(size - 2 * s->margin));
	fill_round_rect(s->img, shape, radius, color_scale_alpha(COLOR_BLACK, alpha));
	blur_region(s->img, rect(0, 0, (f32)size, (f32)size), blur);
	/* Punch out the area the window will cover so translucent windows
	 * don't show a dark shadow through their own rounded corners. */
	Rect inside = rect((f32)s->margin, (f32)s->margin, (f32)(size - 2 * s->margin), (f32)(size - 2 * s->margin));
	Path p = { 0 };
	path_round_rect(&p, inside, radius);
	Mask m;
	if (mask_from_path(&m, &p, mat_identity(), FILL_NONZERO, rect(0, 0, (f32)size, (f32)size))) {
		for (int y = 0; y < m.h; y++)
			for (int x = 0; x < m.w; x++) {
				u32 k = m.data[(usize)y * m.w + x];
				if (!k)
					continue;
				u32 *px = &canvas_row(s->img, m.y + y)[m.x + x];
				u32 a = (*px >> 24) * (255 - k) / 255;
				*px = a << 24;
			}
		mask_free(&m);
	}
	path_free(&p);
}

/* Blits a region of the shadow image, stretched to fill dst (only along one axis). */
static void blit_stretch(Canvas *dst, const Canvas *src, Rect s, Rect d, f32 opacity)
{
	Rect clip = rect_intersect(dst->clip, rect(0, 0, (f32)dst->width, (f32)dst->height));
	int x0 = MAX((int)d.x, (int)clip.x), y0 = MAX((int)d.y, (int)clip.y);
	int x1 = MIN((int)(d.x + d.w), (int)(clip.x + clip.w)), y1 = MIN((int)(d.y + d.h), (int)(clip.y + clip.h));
	if (x1 <= x0 || y1 <= y0 || s.w <= 0 || s.h <= 0)
		return;
	u32 k = (u32)(clampf(opacity, 0, 1) * 256);
	for (int y = y0; y < y1; y++) {
		int sy = (int)s.y + (int)(((f32)(y - (int)d.y) + 0.5f) * s.h / d.h);
		sy = CLAMP(sy, (int)s.y, (int)(s.y + s.h) - 1);
		const u32 *srow = canvas_row(src, sy);
		u32 *drow = canvas_row(dst, y);
		for (int x = x0; x < x1; x++) {
			int sx = (int)s.x + (int)(((f32)(x - (int)d.x) + 0.5f) * s.w / d.w);
			sx = CLAMP(sx, (int)s.x, (int)(s.x + s.w) - 1);
			u32 p = srow[sx];
			if (p)
				drow[x] = blend_premul(drow[x], p, k);
		}
	}
}

static void shadow_draw(Canvas *dst, const Shadow *s, Rect r, f32 opacity)
{
	int m = s->margin, c = s->corner, size = s->img->width;
	f32 x0 = r.x - (f32)m, y0 = r.y - (f32)m;
	f32 x1 = r.x + r.w + (f32)m, y1 = r.y + r.h + (f32)m;
	f32 cw = (f32)c;
	if (x1 - x0 < 2 * cw || y1 - y0 < 2 * cw)
		return;
	f32 mid = (f32)c, sz = (f32)size;
	/* corners */
	blit_stretch(dst, s->img, rect(0, 0, mid, mid), rect(x0, y0, cw, cw), opacity);
	blit_stretch(dst, s->img, rect(sz - mid, 0, mid, mid), rect(x1 - cw, y0, cw, cw), opacity);
	blit_stretch(dst, s->img, rect(0, sz - mid, mid, mid), rect(x0, y1 - cw, cw, cw), opacity);
	blit_stretch(dst, s->img, rect(sz - mid, sz - mid, mid, mid), rect(x1 - cw, y1 - cw, cw, cw), opacity);
	/* edges: the middle column/row stretched */
	blit_stretch(dst, s->img, rect(mid, 0, 1, mid), rect(x0 + cw, y0, x1 - x0 - 2 * cw, cw), opacity);
	blit_stretch(dst, s->img, rect(mid, sz - mid, 1, mid), rect(x0 + cw, y1 - cw, x1 - x0 - 2 * cw, cw), opacity);
	blit_stretch(dst, s->img, rect(0, mid, mid, 1), rect(x0, y0 + cw, cw, y1 - y0 - 2 * cw), opacity);
	blit_stretch(dst, s->img, rect(sz - mid, mid, mid, 1), rect(x1 - cw, y0 + cw, cw, y1 - y0 - 2 * cw), opacity);
}

/* ---- Wallpapers --------------------------------------------------------------------- */

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

Canvas *wallpaper_render(const char *name, int w, int h)
{
	/* A user picture? */
	if (name && name[0] == '/') {
		Canvas *img = image_load(name);
		if (img) {
			Canvas *c = canvas_new(w, h);
			f32 s = maxf((f32)w / (f32)img->width, (f32)h / (f32)img->height);
			f32 sw = (f32)w / s, sh = (f32)h / s;
			blit_scaled(c, img, rect(((f32)img->width - sw) / 2, ((f32)img->height - sh) / 2, sw, sh), rect(0, 0, (f32)w, (f32)h), 1);
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

void wallpaper_reload(void)
{
	canvas_free(S.wallpaper);
	canvas_free(S.wallpaper_blur);
	S.wallpaper = wallpaper_render(S.set.wallpaper, S.frame->width, S.frame->height);
	/* A heavily blurred, darkened copy for the lock screen and the launcher. */
	int bw = MAX(1, S.frame->width / 4), bh = MAX(1, S.frame->height / 4);
	Canvas *small = canvas_resize(S.wallpaper, bw, bh);
	blur_region(small, rect(0, 0, (f32)bw, (f32)bh), 18);
	S.wallpaper_blur = canvas_new(S.frame->width, S.frame->height);
	blit_scaled(S.wallpaper_blur, small, rect(0, 0, (f32)bw, (f32)bh), rect(0, 0, (f32)S.frame->width, (f32)S.frame->height), 1);
	canvas_free(small);
	damage_all();
}

/* ---- Cursors -------------------------------------------------------------------------- */

static Canvas *make_cursor(u32 shape, f32 s, int *hx, int *hy)
{
	int size = (int)ceilf(32 * s);
	Canvas *c = canvas_new(size, size);
	Path p = { 0 };
	Mat m = mat_scale(s, s);
	*hx = *hy = 0;
	switch (shape) {
	case CURSOR_TEXT:
		path_move(&p, 12, 4); path_line(&p, 20, 4);
		path_move(&p, 16, 4); path_line(&p, 16, 26);
		path_move(&p, 12, 26); path_line(&p, 20, 26);
		*hx = (int)(16 * s), *hy = (int)(15 * s);
		{
			Paint outline = paint_solid(COLOR_WHITE), ink = paint_solid(color_hex("#1b1d22"));
			StrokeStyle w = { 4, CAP_ROUND, JOIN_ROUND, 4 }, n = { 1.8f, CAP_ROUND, JOIN_ROUND, 4 };
			stroke_path(c, &p, m, &outline, &w);
			stroke_path(c, &p, m, &ink, &n);
		}
		path_free(&p);
		return c;
	case CURSOR_POINTER: {
		/* A pointing hand, simplified. */
		path_move(&p, 11, 15); path_line(&p, 11, 5);
		path_cubic(&p, 11, 3, 14, 3, 14, 5); path_line(&p, 14, 12);
		path_line(&p, 14, 10); path_cubic(&p, 14, 8.5f, 17, 8.5f, 17, 10); path_line(&p, 17, 12.5f);
		path_line(&p, 17, 11); path_cubic(&p, 17, 9.5f, 20, 9.5f, 20, 11); path_line(&p, 20, 13);
		path_line(&p, 20, 12); path_cubic(&p, 20, 10.5f, 23, 10.5f, 23, 12); path_line(&p, 23, 20);
		path_cubic(&p, 23, 25, 20, 28, 16, 28); path_line(&p, 15, 28);
		path_cubic(&p, 12, 28, 10, 26, 8, 23); path_line(&p, 5, 18);
		path_cubic(&p, 4, 16, 6.5f, 14.5f, 8, 16); path_line(&p, 11, 19);
		path_close(&p);
		*hx = (int)(12 * s), *hy = (int)(4 * s);
		break;
	}
	case CURSOR_MOVE:
	case CURSOR_GRAB:
		path_circle(&p, 16, 16, 10);
		*hx = *hy = (int)(16 * s);
		break;
	case CURSOR_RESIZE_NS:
	case CURSOR_RESIZE_EW:
	case CURSOR_RESIZE_NWSE:
	case CURSOR_RESIZE_NESW: {
		/* Double-headed arrow, rotated to the axis. */
		f32 ang = shape == CURSOR_RESIZE_NS ? PI_F / 2 : shape == CURSOR_RESIZE_EW ? 0 : shape == CURSOR_RESIZE_NWSE ? PI_F / 4 : -PI_F / 4;
		Path a = { 0 };
		path_move(&a, -12, 0); path_line(&a, -6, -6); path_line(&a, -6, -2.5f); path_line(&a, 6, -2.5f);
		path_line(&a, 6, -6); path_line(&a, 12, 0); path_line(&a, 6, 6); path_line(&a, 6, 2.5f);
		path_line(&a, -6, 2.5f); path_line(&a, -6, 6); path_close(&a);
		Mat mm = mat_mul(mat_mul(m, mat_translate(16, 16)), mat_rotate(ang));
		Path o = { 0 };
		StrokeStyle st = { 3, CAP_ROUND, JOIN_ROUND, 4 };
		path_stroke_outline(&a, mm, &st, &o);
		Paint white = paint_solid(COLOR_WHITE), black = paint_solid(color_hex("#1b1d22"));
		fill_path(c, &o, mat_identity(), &white, FILL_NONZERO);
		fill_path(c, &a, mm, &black, FILL_NONZERO);
		path_free(&a);
		path_free(&o);
		*hx = *hy = (int)(16 * s);
		return c;
	}
	case CURSOR_CROSSHAIR:
		path_move(&p, 16, 4); path_line(&p, 16, 28);
		path_move(&p, 4, 16); path_line(&p, 28, 16);
		*hx = *hy = (int)(16 * s);
		{
			Paint outline = paint_solid(COLOR_WHITE), ink = paint_solid(color_hex("#1b1d22"));
			StrokeStyle w = { 3.5f, CAP_ROUND, JOIN_ROUND, 4 }, n = { 1.5f, CAP_ROUND, JOIN_ROUND, 4 };
			stroke_path(c, &p, m, &outline, &w);
			stroke_path(c, &p, m, &ink, &n);
		}
		path_free(&p);
		return c;
	default: /* arrow */
		path_move(&p, 5, 3); path_line(&p, 5, 24); path_line(&p, 10, 19.5f); path_line(&p, 13.5f, 27.5f);
		path_line(&p, 17, 26); path_line(&p, 13.5f, 18); path_line(&p, 20, 18); path_close(&p);
		*hx = (int)(5 * s), *hy = (int)(3 * s);
		break;
	}
	/* Soft drop shadow, white outline, dark body. */
	Mask sh;
	Mat ms = mat_mul(mat_translate(0.5f * s, 1.5f * s), m);
	if (mask_from_path(&sh, &p, ms, FILL_NONZERO, rect(0, 0, (f32)size, (f32)size))) {
		Canvas *tmp = canvas_new(size, size);
		fill_mask_color(tmp, &sh, 0, 0, color_hex("#00000070"));
		blur_region(tmp, rect(0, 0, (f32)size, (f32)size), 2.5f * s);
		blit(c, tmp, 0, 0, 1);
		canvas_free(tmp);
		mask_free(&sh);
	}
	Path o = { 0 };
	StrokeStyle st = { 2.6f, CAP_ROUND, JOIN_ROUND, 4 };
	path_stroke_outline(&p, m, &st, &o);
	Paint white = paint_solid(COLOR_WHITE);
	fill_path(c, &o, mat_identity(), &white, FILL_NONZERO);
	Paint body = paint_solid(color_hex("#1d1f24"));
	fill_path(c, &p, m, &body, FILL_NONZERO);
	path_free(&o);
	path_free(&p);
	return c;
}

static int cursor_hot[16][2];
static int cursor_drawn_x = -10000, cursor_drawn_y, cursor_drawn_shape = -1;

void cursor_update(void)
{
	u32 shape = S.cursor < 16 ? S.cursor : CURSOR_DEFAULT;
	if (shape == CURSOR_HIDDEN)
		shape = CURSOR_DEFAULT;
	int x = (int)(S.px * S.scale) - cursor_hot[shape][0], y = (int)(S.py * S.scale) - cursor_hot[shape][1];
	if (S.hw_cursor) {
		if ((int)shape != cursor_drawn_shape) {
			S.out->cursor_set(S.out, S.cursor_img[shape], cursor_hot[shape][0], cursor_hot[shape][1]);
			cursor_drawn_shape = (int)shape;
		}
		S.out->cursor_move(S.out, (int)(S.px * S.scale), (int)(S.py * S.scale));
		return;
	}
	/* Software cursor: repaint the old and new spots. */
	if ((int)shape != cursor_drawn_shape || x != cursor_drawn_x || y != cursor_drawn_y) {
		int size = S.cursor_img[0]->width;
		damage_physical(rect((f32)cursor_drawn_x, (f32)cursor_drawn_y, (f32)size, (f32)size));
		damage_physical(rect((f32)x, (f32)y, (f32)size, (f32)size));
	}
}

static void draw_software_cursor(Canvas *frame)
{
	u32 shape = S.cursor < 16 ? S.cursor : CURSOR_DEFAULT;
	if (S.cursor == CURSOR_HIDDEN || S.splash)
		return;
	int x = (int)(S.px * S.scale) - cursor_hot[shape][0], y = (int)(S.py * S.scale) - cursor_hot[shape][1];
	blit(frame, S.cursor_img[shape], x, y, 1);
	cursor_drawn_x = x, cursor_drawn_y = y, cursor_drawn_shape = (int)shape;
}

/* ---- Frame composition --------------------------------------------------------------------- */

void compose_init(void)
{
	f32 s = S.scale;
	shadow_build(&shadow_focused, 10 * s, 18 * s, 8 * s, 0.42f);
	shadow_build(&shadow_normal, 10 * s, 10 * s, 3 * s, 0.26f);
	for (u32 i = 0; i < 16; i++)
		S.cursor_img[i] = make_cursor(i, s, &cursor_hot[i][0], &cursor_hot[i][1]);
	S.hw_cursor = S.out->cursor_set && S.out->cursor_set(S.out, S.cursor_img[0], cursor_hot[0][0], cursor_hot[0][1]);
	if (S.hw_cursor)
		cursor_drawn_shape = 0;
	wallpaper_reload();
}

static void draw_window(Canvas *frame, Window *w)
{
	if (!w->mapped && !w->last_frame)
		return;
	f32 opacity = w->appear * (1 - w->vanish) * (1 - w->minimize_k);
	if (opacity <= 0.003f)
		return;
	Rect v = window_visual_rect(w);
	Rect pr = { v.x * S.scale, v.y * S.scale, v.w * S.scale, v.h * S.scale };
	pr.x = roundf(pr.x), pr.y = roundf(pr.y);
	if (!rect_empty(rect_intersect(pr, frame->clip)) || !(w->flags & WIN_NO_SHADOW)) {
		if (!(w->flags & (WIN_NO_SHADOW | WIN_POPUP)) && !(w->states & STATE_FULLSCREEN)) {
			bool focused = w == S.focus;
			const Shadow *sh = focused ? &shadow_focused : &shadow_normal;
			shadow_draw(frame, sh, pr, opacity);
		} else if (w->flags & WIN_POPUP) {
			shadow_draw(frame, &shadow_normal, pr, opacity * 0.8f);
		}
	}
	Canvas src;
	const Canvas *img;
	if (w->closing && w->last_frame) {
		img = w->last_frame;
	} else {
		if (w->front < 0 || !w->map)
			return;
		src = canvas_wrap(w->map + (usize)w->front * w->slot_bytes, (int)w->bw, (int)w->bh, (int)w->stride);
		img = &src;
	}
	bool exact = fabsf(pr.w - (f32)img->width) < 0.5f && fabsf(pr.h - (f32)img->height) < 0.5f;
	if (exact)
		blit(frame, img, (int)pr.x, (int)pr.y, opacity);
	else
		blit_scaled(frame, img, rect(0, 0, (f32)img->width, (f32)img->height), pr, opacity);
}

void compose_frame(void)
{
	Canvas *frame = S.frame;
	Rect full = rect(0, 0, (f32)frame->width, (f32)frame->height);
	shell_prepare();
	Rect rects[16];
	int n;
	if (S.full_damage) {
		rects[0] = full;
		n = 1;
	} else {
		n = S.ndamage;
		memcpy(rects, S.damage, sizeof(Rect) * (usize)n);
	}
	S.full_damage = false;
	S.ndamage = 0;
	if (n == 0)
		return;

	for (int i = 0; i < n; i++) {
		Rect d = rects[i];
		canvas_clip(frame, d);
		if (S.splash && S.splash_k >= 1) {
			splash_draw(frame);
			continue;
		}
		copy_pixels(frame, S.wallpaper, (int)d.x, (int)d.y, (int)d.x, (int)d.y, (int)d.w, (int)d.h);
		if (!S.locked || S.lock_k < 1) {
			for (int k = 0; k < S.nwin; k++) {
				Window *w = S.windows[k];
				if (w->minimized && w->minimize_k >= 1)
					continue;
				Rect v = window_visual_rect(w);
				Rect pr = { v.x * S.scale - 60, v.y * S.scale - 60, v.w * S.scale + 120, v.h * S.scale + 120 };
				if (!rect_empty(rect_intersect(pr, d)))
					draw_window(frame, w);
			}
		}
		shell_draw(frame);
		if (S.splash)
			splash_draw(frame);
		if (!S.hw_cursor)
			draw_software_cursor(frame);
	}
	canvas_clip(frame, full);
	S.out->present(S.out, frame, rects, n);
	server_frame_done();
}
