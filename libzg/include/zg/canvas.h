/*
 * libzg — the Zurbax graphics engine.
 *
 * A canvas is a rectangle of premultiplied ARGB pixels. Colors in the public
 * API use straight (non-premultiplied) alpha and are converted on the way in.
 * All drawing is anti-aliased through an analytic coverage rasterizer, so
 * curves, text and thin strokes stay crisp at any size.
 */
#pragma once

#include "zr.h"

/* ---- Color ---------------------------------------------------------------- */

/* 0xAARRGGBB, straight alpha. */
typedef u32 Color;

#define RGBA(r, g, b, a) ((Color)(((u32)(a) << 24) | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b)))
#define RGB(r, g, b) RGBA(r, g, b, 255)
#define COLOR_A(c) (((c) >> 24) & 0xff)
#define COLOR_R(c) (((c) >> 16) & 0xff)
#define COLOR_G(c) (((c) >> 8) & 0xff)
#define COLOR_B(c) ((c) & 0xff)
#define COLOR_CLEAR 0x00000000u
#define COLOR_BLACK 0xff000000u
#define COLOR_WHITE 0xffffffffu

Color color_hex(const char *s);                 /* "#rrggbb", "#rgb", "#rrggbbaa" */
Color color_mix(Color a, Color b, f32 t);       /* linear in sRGB */
Color color_with_alpha(Color c, u8 a);
Color color_lighten(Color c, f32 amount);       /* amount in [-1, 1]; negative darkens */
Color color_scale_alpha(Color c, f32 s);
/* HSL in [0,360], [0,1], [0,1]. */
Color color_hsl(f32 h, f32 s, f32 l, u8 a);
void color_to_hsl(Color c, f32 *h, f32 *s, f32 *l);

/* ---- Geometry -------------------------------------------------------------- */

typedef struct {
	f32 x, y;
} Vec2;

typedef struct {
	f32 x, y, w, h;
} Rect;

static inline Rect rect(f32 x, f32 y, f32 w, f32 h) { return (Rect){ x, y, w, h }; }
static inline f32 rect_right(Rect r) { return r.x + r.w; }
static inline f32 rect_bottom(Rect r) { return r.y + r.h; }
static inline bool rect_contains(Rect r, f32 x, f32 y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
Rect rect_intersect(Rect a, Rect b);
Rect rect_union(Rect a, Rect b);
Rect rect_inset(Rect r, f32 dx, f32 dy);
bool rect_empty(Rect r);

/* ---- Canvas ---------------------------------------------------------------- */

typedef struct {
	u32 *pixels;    /* premultiplied ARGB, row-major */
	int width;
	int height;
	int stride;     /* pixels per row (>= width) */
	bool owned;     /* whether `pixels` was allocated by the canvas */
	Rect clip;      /* drawing is confined to this rectangle */
} Canvas;

Canvas *canvas_new(int w, int h);
/* Wraps existing memory (e.g. a framebuffer); does not take ownership. */
Canvas canvas_wrap(void *pixels, int w, int h, int stride);
/* A sub-canvas sharing the parent's pixels, with its own clip. */
Canvas canvas_view(const Canvas *parent, Rect area);
void canvas_free(Canvas *c);
void canvas_clip(Canvas *c, Rect r);
void canvas_clear(Canvas *c, Color color);
static inline u32 *canvas_row(const Canvas *c, int y) { return c->pixels + (usize)y * c->stride; }

/* Premultiplied-pixel helpers (internal format). */
u32 color_premul(Color c);
Color color_unpremul(u32 p);
/* Composite a premultiplied source over a premultiplied destination. */
static inline u32 blend_premul(u32 dst, u32 src, u32 cov /* 0..256 */)
{
	u32 sa = (src >> 24) * cov >> 8;
	if (sa == 0)
		return dst;
	u32 ia = 255 - sa;
	u32 sr = ((src >> 16) & 0xff) * cov >> 8;
	u32 sg = ((src >> 8) & 0xff) * cov >> 8;
	u32 sb = (src & 0xff) * cov >> 8;
	u32 da = dst >> 24, dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
	u32 oa = sa + (da * ia + 127) / 255;
	u32 orr = sr + (dr * ia + 127) / 255;
	u32 og = sg + (dg * ia + 127) / 255;
	u32 ob = sb + (db * ia + 127) / 255;
	return oa << 24 | orr << 16 | og << 8 | ob;
}
