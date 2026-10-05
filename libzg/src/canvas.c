/* Canvases, colors and rectangles. */
#include "zg.h"

/* ---- Color ------------------------------------------------------------------- */

static int hexval(char c)
{
	if (is_digit(c))
		return c - '0';
	c = (char)(c | 32);
	return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
}

Color color_hex(const char *s)
{
	if (*s == '#')
		s++;
	usize n = strlen(s);
	u32 r, g, b, a = 255;
	if (n == 3 || n == 4) {
		r = (u32)hexval(s[0]) * 17;
		g = (u32)hexval(s[1]) * 17;
		b = (u32)hexval(s[2]) * 17;
		if (n == 4)
			a = (u32)hexval(s[3]) * 17;
	} else if (n >= 6) {
		r = (u32)(hexval(s[0]) * 16 + hexval(s[1]));
		g = (u32)(hexval(s[2]) * 16 + hexval(s[3]));
		b = (u32)(hexval(s[4]) * 16 + hexval(s[5]));
		if (n >= 8)
			a = (u32)(hexval(s[6]) * 16 + hexval(s[7]));
	} else {
		return COLOR_BLACK;
	}
	return RGBA(r, g, b, a);
}

Color color_mix(Color a, Color b, f32 t)
{
	t = clampf(t, 0, 1);
	u32 ti = (u32)(t * 256 + 0.5f), si = 256 - ti;
	u32 ca = (COLOR_A(a) * si + COLOR_A(b) * ti) >> 8;
	u32 cr = (COLOR_R(a) * si + COLOR_R(b) * ti) >> 8;
	u32 cg = (COLOR_G(a) * si + COLOR_G(b) * ti) >> 8;
	u32 cb = (COLOR_B(a) * si + COLOR_B(b) * ti) >> 8;
	return RGBA(cr, cg, cb, ca);
}

Color color_with_alpha(Color c, u8 a)
{
	return (c & 0x00ffffffu) | ((u32)a << 24);
}

Color color_scale_alpha(Color c, f32 s)
{
	u32 a = (u32)clampf(COLOR_A(c) * s + 0.5f, 0, 255);
	return (c & 0x00ffffffu) | (a << 24);
}

Color color_lighten(Color c, f32 amount)
{
	Color target = amount >= 0 ? (COLOR_WHITE & 0x00ffffffu) | (c & 0xff000000u) : (c & 0xff000000u);
	return color_mix(c, target, fabsf(amount));
}

static f32 hue_channel(f32 p, f32 q, f32 t)
{
	if (t < 0)
		t += 1;
	if (t > 1)
		t -= 1;
	if (t < 1.0f / 6)
		return p + (q - p) * 6 * t;
	if (t < 0.5f)
		return q;
	if (t < 2.0f / 3)
		return p + (q - p) * (2.0f / 3 - t) * 6;
	return p;
}

Color color_hsl(f32 h, f32 s, f32 l, u8 a)
{
	h = fmodf(h, 360) / 360;
	if (h < 0)
		h += 1;
	f32 r, g, b;
	if (s <= 0) {
		r = g = b = l;
	} else {
		f32 q = l < 0.5f ? l * (1 + s) : l + s - l * s;
		f32 p = 2 * l - q;
		r = hue_channel(p, q, h + 1.0f / 3);
		g = hue_channel(p, q, h);
		b = hue_channel(p, q, h - 1.0f / 3);
	}
	return RGBA((u32)(r * 255 + 0.5f), (u32)(g * 255 + 0.5f), (u32)(b * 255 + 0.5f), a);
}

void color_to_hsl(Color c, f32 *h, f32 *s, f32 *l)
{
	f32 r = COLOR_R(c) / 255.0f, g = COLOR_G(c) / 255.0f, b = COLOR_B(c) / 255.0f;
	f32 mx = maxf(r, maxf(g, b)), mn = minf(r, minf(g, b));
	*l = (mx + mn) / 2;
	if (mx == mn) {
		*h = *s = 0;
		return;
	}
	f32 d = mx - mn;
	*s = *l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
	if (mx == r)
		*h = (g - b) / d + (g < b ? 6 : 0);
	else if (mx == g)
		*h = (b - r) / d + 2;
	else
		*h = (r - g) / d + 4;
	*h *= 60;
}

u32 color_premul(Color c)
{
	u32 a = COLOR_A(c);
	if (a == 255)
		return c;
	if (a == 0)
		return 0;
	u32 r = (COLOR_R(c) * a + 127) / 255;
	u32 g = (COLOR_G(c) * a + 127) / 255;
	u32 b = (COLOR_B(c) * a + 127) / 255;
	return a << 24 | r << 16 | g << 8 | b;
}

Color color_unpremul(u32 p)
{
	u32 a = p >> 24;
	if (a == 255 || a == 0)
		return a ? p : 0;
	u32 r = MIN(255u, (((p >> 16) & 0xff) * 255 + a / 2) / a);
	u32 g = MIN(255u, (((p >> 8) & 0xff) * 255 + a / 2) / a);
	u32 b = MIN(255u, ((p & 0xff) * 255 + a / 2) / a);
	return a << 24 | r << 16 | g << 8 | b;
}

/* ---- Rectangles ------------------------------------------------------------------ */

Rect rect_intersect(Rect a, Rect b)
{
	f32 x0 = maxf(a.x, b.x), y0 = maxf(a.y, b.y);
	f32 x1 = minf(a.x + a.w, b.x + b.w), y1 = minf(a.y + a.h, b.y + b.h);
	if (x1 <= x0 || y1 <= y0)
		return (Rect){ x0, y0, 0, 0 };
	return (Rect){ x0, y0, x1 - x0, y1 - y0 };
}

Rect rect_union(Rect a, Rect b)
{
	if (rect_empty(a))
		return b;
	if (rect_empty(b))
		return a;
	f32 x0 = minf(a.x, b.x), y0 = minf(a.y, b.y);
	f32 x1 = maxf(a.x + a.w, b.x + b.w), y1 = maxf(a.y + a.h, b.y + b.h);
	return (Rect){ x0, y0, x1 - x0, y1 - y0 };
}

Rect rect_inset(Rect r, f32 dx, f32 dy)
{
	r.x += dx;
	r.y += dy;
	r.w = maxf(0, r.w - 2 * dx);
	r.h = maxf(0, r.h - 2 * dy);
	return r;
}

bool rect_empty(Rect r)
{
	return r.w <= 0 || r.h <= 0;
}

/* ---- Canvas ---------------------------------------------------------------------- */

Canvas *canvas_new(int w, int h)
{
	Canvas *c = NEW(Canvas);
	c->width = MAX(w, 1);
	c->height = MAX(h, 1);
	c->stride = c->width;
	c->pixels = mem_zalloc((usize)c->stride * (usize)c->height * 4);
	c->owned = true;
	c->clip = rect(0, 0, (f32)c->width, (f32)c->height);
	return c;
}

Canvas canvas_wrap(void *pixels, int w, int h, int stride)
{
	return (Canvas){ pixels, w, h, stride, false, rect(0, 0, (f32)w, (f32)h) };
}

Canvas canvas_view(const Canvas *parent, Rect area)
{
	Rect a = rect_intersect(area, rect(0, 0, (f32)parent->width, (f32)parent->height));
	int x = (int)a.x, y = (int)a.y;
	int w = (int)(a.x + a.w) - x, h = (int)(a.y + a.h) - y;
	Canvas v = { parent->pixels + (usize)y * parent->stride + x, MAX(w, 0), MAX(h, 0), parent->stride, false,
		     rect(0, 0, (f32)MAX(w, 0), (f32)MAX(h, 0)) };
	return v;
}

void canvas_free(Canvas *c)
{
	if (!c)
		return;
	if (c->owned)
		mem_free(c->pixels);
	mem_free(c);
}

void canvas_clip(Canvas *c, Rect r)
{
	c->clip = rect_intersect(r, rect(0, 0, (f32)c->width, (f32)c->height));
}

void canvas_clear(Canvas *c, Color color)
{
	u32 p = color_premul(color);
	for (int y = 0; y < c->height; y++) {
		u32 *row = canvas_row(c, y);
		for (int x = 0; x < c->width; x++)
			row[x] = p;
	}
}
