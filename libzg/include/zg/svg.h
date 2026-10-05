/*
 * A compact SVG renderer for icons and illustrations.
 *
 * Supports: svg/g/path/rect/circle/ellipse/line/polyline/polygon/use,
 * linear and radial gradients, fill/stroke with opacity, transforms,
 * viewBox, inline style attributes and `currentColor` (so symbolic icons can
 * be tinted to match the theme).
 */
#pragma once

#include "path.h"

typedef struct SvgDoc SvgDoc;

SvgDoc *svg_parse(const char *text, usize len);
SvgDoc *svg_load(const char *path);
void svg_free(SvgDoc *doc);
void svg_size(const SvgDoc *doc, f32 *w, f32 *h);

/* Renders the document scaled to fit `dst`. `current_color` replaces currentColor. */
void svg_render(const SvgDoc *doc, Canvas *c, Rect dst, Color current_color);
/* Convenience: renders into a new canvas of the given size. */
Canvas *svg_rasterize(const SvgDoc *doc, int w, int h, Color current_color);
