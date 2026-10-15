/*
 * Glass: the shell's material for bars, panels and menus.
 *
 * What's behind (the softened wallpaper) shows through, a little brighter
 * and more colourful, under a thin tint of the theme. Towards the rim the
 * picture bends outward, as through the edge of a thick lens, and the rim
 * itself catches light, most on its upper side, as from a lamp above.
 *
 * It's drawn on the CPU, so it's made cheap: the tinted wallpaper is worked
 * out once per wallpaper and style; inside the rim band a pixel is that plus
 * the sheen of its row; only the band does the lens and the light. And a
 * panel's glass is kept: the taskbar, an open menu or panel, drawn again
 * the same, costs a blit (fading ones too: the fade is applied as it's
 * blended).
 */
#include "zdisplay.h"

static inline f32 clamp01(f32 v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* ---- The wallpaper as glass shows it ----------------------------------------------------- */

static struct {
	u32 gen;
	Canvas *tinted[2]; /* [dark] */
} bd;

static Canvas *tinted_backdrop(bool dark)
{
	const Canvas *b = S.wallpaper_blur;
	if (bd.gen != S.wallpaper_gen) {
		for (int i = 0; i < 2; i++)
			if (bd.tinted[i])
				canvas_free(bd.tinted[i]), bd.tinted[i] = NULL;
		bd.gen = S.wallpaper_gen;
	}
	if (bd.tinted[dark])
		return bd.tinted[dark];
	/* the tint: a cool near-black in the dark style, milky white in the light one */
	f32 tr = dark ? 22 : 250, tg = dark ? 24 : 250, tb = dark ? 30 : 252;
	f32 ta = dark ? 0.46f : 0.42f;
	f32 lift = dark ? 0.92f : 1.06f; /* overall brightness */
	f32 sat = 1.28f;                 /* and colour */
	Canvas *t = canvas_new(b->width, b->height);
	for (int y = 0; y < b->height; y++) {
		const u32 *s = canvas_row(b, y);
		u32 *d = canvas_row(t, y);
		for (int x = 0; x < b->width; x++) {
			f32 cr = (f32)((s[x] >> 16) & 0xff), cg = (f32)((s[x] >> 8) & 0xff), cb = (f32)(s[x] & 0xff);
			f32 lum = 0.2126f * cr + 0.7152f * cg + 0.0722f * cb;
			cr = ((lum + (cr - lum) * sat) * lift) * (1 - ta) + tr * ta;
			cg = ((lum + (cg - lum) * sat) * lift) * (1 - ta) + tg * ta;
			cb = ((lum + (cb - lum) * sat) * lift) * (1 - ta) + tb * ta;
			d[x] = 0xff000000u | (u32)clampf(cr, 0, 255) << 16 | (u32)clampf(cg, 0, 255) << 8 | (u32)clampf(cb, 0, 255);
		}
	}
	bd.tinted[dark] = t;
	return t;
}

/* The tinted wallpaper at a device position, with bilinear filtering. */
static inline void backdrop(const Canvas *b, f32 x, f32 y, f32 *r, f32 *g, f32 *bl)
{
	x = clampf(x, 0, (f32)b->width - 1.001f);
	y = clampf(y, 0, (f32)b->height - 1.001f);
	int x0 = (int)x, y0 = (int)y;
	f32 fx = x - (f32)x0, fy = y - (f32)y0;
	const u32 *r0 = canvas_row(b, y0), *r1 = canvas_row(b, y0 + 1);
	u32 p[4] = { r0[x0], r0[x0 + 1], r1[x0], r1[x0 + 1] };
	f32 w[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
	f32 cr = 0, cg = 0, cb = 0;
	for (int i = 0; i < 4; i++) {
		cr += (f32)((p[i] >> 16) & 0xff) * w[i];
		cg += (f32)((p[i] >> 8) & 0xff) * w[i];
		cb += (f32)(p[i] & 0xff) * w[i];
	}
	*r = cr, *g = cg, *bl = cb;
}

/* ---- One panel's glass ------------------------------------------------------------------- */

/* Into img (premultiplied, its corner at the device pixel (ox, oy)): the glass of
 * the rounded rectangle d, radius rad, in device pixels. */
static void render(Canvas *img, int ox, int oy, Rect d, f32 rad, bool dark)
{
	const Canvas *tb = tinted_backdrop(dark);
	f32 sc = S.scale;
	f32 cx = d.x + d.w / 2, cy = d.y + d.h / 2, hw = d.w / 2 - rad, hh = d.h / 2 - rad;
	f32 band = minf(14 * sc, minf(d.w, d.h) / 2); /* how far in the rim bends the picture */
	f32 in_w = d.w / 2 - band, in_h = d.h / 2 - band;
	f32 sheen_k = dark ? 0.05f : 0.07f;
	for (int iy = 0; iy < img->height; iy++) {
		int y = oy + iy;
		u32 *row = canvas_row(img, iy);
		f32 py = (f32)y + 0.5f - cy;
		f32 qy = fabsf(py) - hh;
		f32 sheen = clamp01(1.0f - (py + d.h / 2) / (d.h * 0.55f)) * sheen_k;
		u32 sheen_add = (u32)(sheen * 255 + 0.5f);
		/* inside the band: the tinted wallpaper and the row's sheen, nothing else */
		int xa = 1, xb = 0;
		if (fabsf(py) <= in_h && in_w > 0) {
			xa = (int)ceilf(cx - in_w - 0.5f) - ox;
			xb = (int)floorf(cx + in_w - 0.5f) - ox;
		}
		const u32 *trow = y >= 0 && y < tb->height ? canvas_row(tb, y) : NULL;
		for (int ix = 0; ix < img->width; ix++) {
			int x = ox + ix;
			if (ix >= xa && ix <= xb && trow && x >= 0 && x < tb->width) {
				u32 p = trow[x];
				u32 r = MIN(((p >> 16) & 0xff) + sheen_add, 255u), g = MIN(((p >> 8) & 0xff) + sheen_add, 255u),
				    b = MIN((p & 0xff) + sheen_add, 255u);
				row[ix] = 0xff000000u | r << 16 | g << 8 | b;
				continue;
			}
			f32 px = (f32)x + 0.5f - cx;
			f32 qx = fabsf(px) - hw;
			/* signed distance to the rounded rectangle: negative inside */
			f32 ox_ = maxf(qx, 0), oy_ = maxf(qy, 0);
			f32 sdf = sqrtf(ox_ * ox_ + oy_ * oy_) + minf(maxf(qx, qy), 0) - rad;
			f32 cov = clamp01(0.5f - sdf);
			if (cov <= 0) {
				row[ix] = 0;
				continue;
			}
			f32 depth = -sdf;
			/* the outward direction at this point of the rim */
			f32 nx, ny;
			if (ox_ > 0 && oy_ > 0) {
				f32 l = sqrtf(ox_ * ox_ + oy_ * oy_);
				nx = ox_ / l * (px < 0 ? -1 : 1), ny = oy_ / l * (py < 0 ? -1 : 1);
			} else if (qx > qy) {
				nx = px < 0 ? -1 : 1, ny = 0;
			} else {
				nx = 0, ny = py < 0 ? -1 : 1;
			}
			/* the lens: near the rim, what's seen comes from further in */
			f32 sx = (f32)x + 0.5f, sy = (f32)y + 0.5f;
			if (depth < band) {
				f32 k = 1 - depth / band;
				f32 disp = k * k * band * 0.85f;
				sx -= nx * disp, sy -= ny * disp;
			}
			f32 cr, cg, cb;
			backdrop(tb, sx - 0.5f, sy - 0.5f, &cr, &cg, &cb);
			/* light on the rim, brightest facing up, a second thin line of it
			 * just inside, a glow under the top edge and the sheen; the lower
			 * rim a little darker */
			f32 facing = clamp01(-ny * 0.85f - nx * 0.25f);
			f32 rim = clamp01(1.0f - depth / (1.6f * sc)) * (0.30f + 0.62f * facing);
			f32 e = (depth - 3.2f * sc) / (1.1f * sc);
			f32 inner = e > -4 && e < 4 ? expf(-e * e) * 0.10f * (0.3f + facing) : 0;
			f32 glow = clamp01(1.0f - depth / (10 * sc)) * 0.10f * facing;
			f32 shade = clamp01(1.0f - depth / (1.6f * sc)) * clamp01(ny) * (dark ? 0.30f : 0.10f);
			f32 add = (rim + inner + glow + sheen) * 255;
			cr = cr * (1 - shade) + add;
			cg = cg * (1 - shade) + add;
			cb = cb * (1 - shade) + add;
			u32 R = (u32)clampf(cr, 0, 255), G = (u32)clampf(cg, 0, 255), B = (u32)clampf(cb, 0, 255);
			row[ix] = blend_premul(0, 0xff000000u | R << 16 | G << 8 | B, (u32)(cov * 256));
		}
	}
}

/* ---- Kept panels ------------------------------------------------------------------------- */

typedef struct {
	int x, y, w, h, rad;
	bool dark;
	u32 gen;
	Canvas *img;
	u64 used;
} Kept;

static Kept kept[12];
static u64 kept_clock;

static Kept *glass_for(Rect d, f32 rad, bool dark)
{
	int x = (int)floorf(d.x), y = (int)floorf(d.y);
	int w = (int)ceilf(d.x + d.w) - x, h = (int)ceilf(d.y + d.h) - y;
	int r = (int)roundf(rad * 4); /* quarter pixels */
	Kept *slot = &kept[0];
	for (usize i = 0; i < ARRAY_LEN(kept); i++) {
		Kept *k = &kept[i];
		if (k->img && k->x == x && k->y == y && k->w == w && k->h == h && k->rad == r && k->dark == dark && k->gen == S.wallpaper_gen) {
			k->used = ++kept_clock;
			return k;
		}
		if (!k->img || k->used < slot->used)
			slot = k;
	}
	if (slot->img)
		canvas_free(slot->img);
	Canvas *img = canvas_new(w, h);
	render(img, x, y, d, rad, dark);
	*slot = (Kept){ x, y, w, h, r, dark, S.wallpaper_gen, img, ++kept_clock };
	return slot;
}

/* ---- Under windows' glass ---------------------------------------------------------------- */

void glass_backdrop(Canvas *dst, Rect d, Rect shape, f32 rad, f32 opacity)
{
	const Canvas *b = S.wallpaper_blur;
	if (!b || opacity <= 0.004f)
		return;
	Rect c = rect_intersect(rect_intersect(d, shape), dst->clip);
	int x0 = MAX((int)roundf(c.x), 0), y0 = MAX((int)roundf(c.y), 0);
	int x1 = MIN((int)roundf(c.x + c.w), MIN(b->width, dst->width)), y1 = MIN((int)roundf(c.y + c.h), MIN(b->height, dst->height));
	f32 cx = shape.x + shape.w / 2, cy = shape.y + shape.h / 2, hw = shape.w / 2 - rad, hh = shape.h / 2 - rad;
	u32 k = (u32)(clampf(opacity, 0, 1) * 256);
	for (int y = y0; y < y1; y++) {
		const u32 *s = canvas_row(b, y);
		u32 *o = canvas_row(dst, y);
		f32 py = (f32)y + 0.5f, qy = fabsf(py - cy) - hh;
		for (int x = x0; x < x1; x++) {
			u32 a = k;
			if (rad > 0 && qy > 0) { /* by a rounded corner: only what's inside it */
				f32 qx = fabsf((f32)x + 0.5f - cx) - hw;
				if (qx > 0) {
					f32 cov = clamp01(rad + 0.5f - sqrtf(qx * qx + qy * qy));
					if (cov <= 0)
						continue;
					a = (u32)(cov * (f32)k);
				}
			}
			o[x] = a >= 256 ? (s[x] | 0xff000000u) : blend_premul(o[x], s[x] | 0xff000000u, a);
		}
	}
}

/* dark: the dark style's tint, whatever the theme (the taskbar can differ) */
void glass_panel_style(Ui *ui, Rect r, f32 radius, f32 alpha, bool dark)
{
	if (alpha <= 0.004f)
		return;
	if (!S.set.glass || !S.wallpaper_blur) { /* transparency effects off: a solid pane */
		ui_round(ui, r, radius, color_scale_alpha(color_hex(dark ? "#1d1e23f4" : "#f6f6f8f4"), alpha));
		return;
	}
	Rect d = ui_to_device(ui, r);
	if (d.w < 1 || d.h < 1)
		return;
	f32 rad = minf(radius * S.scale, minf(d.w, d.h) / 2);
	Kept *k = glass_for(d, rad, dark);
	Canvas *dst = ui->canvas;
	Rect saved = dst->clip;
	canvas_clip(dst, rect_intersect(saved, ui_to_device(ui, ui->clip)));
	blit(dst, k->img, k->x, k->y, alpha);
	dst->clip = saved;
}

void glass_panel(Ui *ui, Rect r, f32 radius, f32 alpha)
{
	glass_panel_style(ui, r, radius, alpha, ui->theme.dark);
}

/* A floating glass panel: a soft shadow beneath, then the glass. */
void glass_card_style(Ui *ui, Rect r, f32 radius, f32 alpha, bool dark)
{
	ui_shadow(ui, r, radius, 28, 10, color_scale_alpha(color_hex(dark ? "#00000070" : "#0000002e"), alpha));
	glass_panel_style(ui, r, radius, alpha, dark);
}

void glass_card(Ui *ui, Rect r, f32 radius, f32 alpha)
{
	glass_card_style(ui, r, radius, alpha, ui->theme.dark);
}
