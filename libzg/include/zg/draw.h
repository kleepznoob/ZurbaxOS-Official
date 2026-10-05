/* Convenience drawing: shapes, shadows, blur and image blits. */
#pragma once

#include "path.h"

void fill_rect(Canvas *c, Rect r, Color color);
void fill_rect_paint(Canvas *c, Rect r, const Paint *paint);
void fill_round_rect(Canvas *c, Rect r, f32 radius, Color color);
void fill_round_rect_paint(Canvas *c, Rect r, f32 radius, const Paint *paint);
void stroke_round_rect(Canvas *c, Rect r, f32 radius, f32 width, Color color);
void fill_circle(Canvas *c, f32 cx, f32 cy, f32 r, Color color);
void stroke_circle(Canvas *c, f32 cx, f32 cy, f32 r, f32 width, Color color);
void draw_line(Canvas *c, f32 x0, f32 y0, f32 x1, f32 y1, f32 width, Color color);

/* Soft drop shadow for a rounded rectangle, blurred by `blur` pixels. */
void draw_shadow(Canvas *c, Rect r, f32 radius, f32 blur, f32 dx, f32 dy, Color color);

/* In-place blur of a canvas region (approximately Gaussian, three box passes). */
void blur_region(Canvas *c, Rect r, f32 radius);

/* Copies `src` onto `c` with alpha blending. opacity in [0,1]. */
void blit(Canvas *c, const Canvas *src, int dx, int dy, f32 opacity);
/* Scales `src` into `dst` rect with bilinear filtering. */
void blit_scaled(Canvas *c, const Canvas *src, Rect src_rect, Rect dst, f32 opacity);
/* Like blit_scaled but clips the result to a rounded rectangle. */
void blit_scaled_round(Canvas *c, const Canvas *src, Rect src_rect, Rect dst, f32 radius, f32 opacity);
/* Copies pixels without blending (fast path for opaque content). */
void copy_pixels(Canvas *c, const Canvas *src, int sx, int sy, int dx, int dy, int w, int h);
/* High-quality downscale (box filter) producing a new canvas. */
Canvas *canvas_resize(const Canvas *src, int w, int h);
