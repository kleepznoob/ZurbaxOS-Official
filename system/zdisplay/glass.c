/*
 * Glass: the shell's material for bars, panels and menus.
 *
 * What's behind (the softened wallpaper) shows through, a little brighter
 * and more colourful, under a thin tint of the theme. Towards the rim the
 * picture bends outward, as through the edge of a thick lens, and the rim
 * itself catches light, most on its upper side, as from a lamp above. One
 * pass over the panel's pixels does all of it, straight into the canvas.
 */
#include "zdisplay.h"

static inline f32 clamp01(f32 v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* The blurred wallpaper at a device position, with bilinear filtering. */
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

/* dark: the dark style's tint, whatever the theme (the taskbar can differ) */
void glass_panel_style(Ui *ui, Rect r, f32 radius, f32 alpha, bool dark)
{
	if (alpha <= 0.004f || !S.wallpaper_blur)
		return;
	f32 sc = S.scale;
	Rect d = ui_to_device(ui, r);
	Rect clip = ui_to_device(ui, ui->clip);
	Canvas *dst = ui->canvas;
	int x0 = (int)floorf(maxf(maxf(d.x, clip.x), 0)), y0 = (int)floorf(maxf(maxf(d.y, clip.y), 0));
	int x1 = (int)ceilf(minf(minf(d.x + d.w, clip.x + clip.w), (f32)dst->width));
	int y1 = (int)ceilf(minf(minf(d.y + d.h, clip.y + clip.h), (f32)dst->height));
	if (x1 <= x0 || y1 <= y0)
		return;
	f32 rad = minf(radius * sc, minf(d.w, d.h) / 2);
	f32 cx = d.x + d.w / 2, cy = d.y + d.h / 2, hw = d.w / 2 - rad, hh = d.h / 2 - rad;
	f32 band = minf(14 * sc, minf(d.w, d.h) / 2); /* how far in the rim bends the picture */
	/* the tint: a cool near-black in the dark style, milky white in the light one */
	f32 tr = dark ? 22 : 250, tg = dark ? 24 : 250, tb = dark ? 30 : 252;
	f32 ta = dark ? 0.46f : 0.42f;
	f32 lift = dark ? 0.92f : 1.06f;          /* overall brightness */
	f32 sat = 1.28f;                           /* and colour */
	for (int y = y0; y < y1; y++) {
		u32 *row = canvas_row(dst, y);
		f32 py = (f32)y + 0.5f - cy;
		f32 qy = fabsf(py) - hh;
		for (int x = x0; x < x1; x++) {
			f32 px = (f32)x + 0.5f - cx;
			f32 qx = fabsf(px) - hw;
			/* signed distance to the rounded rectangle: negative inside */
			f32 ox = maxf(qx, 0), oy = maxf(qy, 0);
			f32 sdf = sqrtf(ox * ox + oy * oy) + minf(maxf(qx, qy), 0) - rad;
			f32 cov = clamp01(0.5f - sdf);
			if (cov <= 0)
				continue;
			f32 depth = -sdf;
			/* the outward direction at this point of the rim */
			f32 nx, ny;
			if (ox > 0 && oy > 0) {
				f32 l = sqrtf(ox * ox + oy * oy);
				nx = ox / l * (px < 0 ? -1 : 1), ny = oy / l * (py < 0 ? -1 : 1);
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
			backdrop(S.wallpaper_blur, sx - 0.5f, sy - 0.5f, &cr, &cg, &cb);
			/* livelier colour, then the tint */
			f32 lum = 0.2126f * cr + 0.7152f * cg + 0.0722f * cb;
			cr = (lum + (cr - lum) * sat) * lift;
			cg = (lum + (cg - lum) * sat) * lift;
			cb = (lum + (cb - lum) * sat) * lift;
			cr = cr * (1 - ta) + tr * ta;
			cg = cg * (1 - ta) + tg * ta;
			cb = cb * (1 - ta) + tb * ta;
			/* light on the rim, brightest facing up, a second thin line of it
			 * just inside, a glow under the top edge and a sheen over the top
			 * half; the lower rim a little darker */
			f32 facing = clamp01(-ny * 0.85f - nx * 0.25f);
			f32 rim = clamp01(1.0f - depth / (1.6f * sc)) * (0.30f + 0.62f * facing);
			f32 e = (depth - 3.2f * sc) / (1.1f * sc);
			f32 inner = expf(-e * e) * 0.10f * (0.3f + facing);
			f32 glow = clamp01(1.0f - depth / (10 * sc)) * 0.10f * facing;
			f32 sheen = clamp01(1.0f - (py + d.h / 2) / (d.h * 0.55f)) * (dark ? 0.05f : 0.07f);
			f32 shade = clamp01(1.0f - depth / (1.6f * sc)) * clamp01(ny) * (dark ? 0.30f : 0.10f);
			f32 add = (rim + inner + glow + sheen) * 255;
			cr = cr * (1 - shade) + add;
			cg = cg * (1 - shade) + add;
			cb = cb * (1 - shade) + add;
			u32 R = (u32)clampf(cr, 0, 255), G = (u32)clampf(cg, 0, 255), B = (u32)clampf(cb, 0, 255);
			u32 src = 0xff000000u | R << 16 | G << 8 | B;
			row[x] = blend_premul(row[x], src, (u32)(cov * alpha * 256));
		}
	}
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
