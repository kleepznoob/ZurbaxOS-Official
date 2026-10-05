/* Vector paths, transforms, paints, filling and stroking. */
#pragma once

#include "canvas.h"

/* ---- Affine transform: x' = a*x + c*y + e, y' = b*x + d*y + f ------------- */

typedef struct {
	f32 a, b, c, d, e, f;
} Mat;

static inline Mat mat_identity(void) { return (Mat){ 1, 0, 0, 1, 0, 0 }; }
static inline Mat mat_translate(f32 x, f32 y) { return (Mat){ 1, 0, 0, 1, x, y }; }
static inline Mat mat_scale(f32 sx, f32 sy) { return (Mat){ sx, 0, 0, sy, 0, 0 }; }
Mat mat_rotate(f32 radians);
Mat mat_mul(Mat m, Mat n); /* apply n first, then m */
Mat mat_invert(Mat m);
static inline Vec2 mat_apply(Mat m, Vec2 p) { return (Vec2){ m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f }; }
f32 mat_scale_factor(Mat m); /* average scale, for stroke widths and flattening */

/* ---- Paths ---------------------------------------------------------------- */

typedef enum { PATH_MOVE, PATH_LINE, PATH_QUAD, PATH_CUBIC, PATH_CLOSE } PathVerb;

typedef struct {
	VEC(u8) verbs;
	VEC(Vec2) pts;
	Vec2 start, cur;
} Path;

void path_move(Path *p, f32 x, f32 y);
void path_line(Path *p, f32 x, f32 y);
void path_quad(Path *p, f32 cx, f32 cy, f32 x, f32 y);
void path_cubic(Path *p, f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y);
/* Elliptical arc around (cx,cy) from angle a0 to a1 (radians, y-down so positive
 * angles turn clockwise on screen). Connects from the current point if any. */
void path_arc(Path *p, f32 cx, f32 cy, f32 rx, f32 ry, f32 a0, f32 a1);
/* SVG-style arc to (x,y). */
void path_arc_to(Path *p, f32 rx, f32 ry, f32 rotation_deg, bool large, bool sweep, f32 x, f32 y);
void path_close(Path *p);
void path_rect(Path *p, Rect r);
void path_round_rect(Path *p, Rect r, f32 radius);
void path_round_rect4(Path *p, Rect r, f32 tl, f32 tr, f32 br, f32 bl);
void path_ellipse(Path *p, f32 cx, f32 cy, f32 rx, f32 ry);
void path_circle(Path *p, f32 cx, f32 cy, f32 r);
void path_clear(Path *p);
void path_free(Path *p);
Rect path_bounds(const Path *p);

/* ---- Paints -------------------------------------------------------------------- */

typedef enum { PAINT_SOLID, PAINT_LINEAR, PAINT_RADIAL, PAINT_IMAGE } PaintKind;

typedef struct {
	f32 pos;   /* 0..1 */
	Color color;
} GradientStop;

#define MAX_STOPS 8

typedef struct {
	PaintKind kind;
	Color color;              /* PAINT_SOLID */
	Vec2 p0, p1;              /* linear: from p0 to p1; radial: centre p0 */
	f32 radius;               /* radial */
	GradientStop stops[MAX_STOPS];
	int nstops;
	Mat radial_mat;           /* radial: optional unit-circle -> canvas map (ellipses) */
	bool has_radial_mat;
	const Canvas *image;      /* PAINT_IMAGE */
	Mat image_mat;            /* image space -> canvas space */
	f32 opacity;              /* multiplies everything; 0 means 1 */
} Paint;

Paint paint_solid(Color c);
Paint paint_linear(f32 x0, f32 y0, f32 x1, f32 y1, Color c0, Color c1);
Paint paint_radial(f32 cx, f32 cy, f32 r, Color inner, Color outer);
void paint_add_stop(Paint *p, f32 pos, Color c);
Paint paint_image(const Canvas *img, Mat image_to_canvas);

/* ---- Filling and stroking --------------------------------------------------------- */

typedef enum { FILL_NONZERO, FILL_EVENODD } FillRule;
typedef enum { CAP_BUTT, CAP_ROUND, CAP_SQUARE } LineCap;
typedef enum { JOIN_MITER, JOIN_ROUND, JOIN_BEVEL } LineJoin;

typedef struct {
	f32 width;
	LineCap cap;
	LineJoin join;
	f32 miter_limit;
} StrokeStyle;

void fill_path(Canvas *c, const Path *p, Mat m, const Paint *paint, FillRule rule);
void stroke_path(Canvas *c, const Path *p, Mat m, const Paint *paint, const StrokeStyle *style);
/* Splits a path into dashes: pattern alternates on/off lengths (user units). */
void path_dash(const Path *in, const f32 *pattern, int n, f32 offset, Path *out);
/* Converts a stroke into a fillable outline (nonzero). */
void path_stroke_outline(const Path *in, Mat m, const StrokeStyle *style, Path *out);

/* ---- Coverage masks (used by text and shadows) ------------------------------------- */

typedef struct {
	u8 *data;
	int x, y;       /* position in canvas pixels */
	int w, h;
} Mask;

/* Rasterises a path into an 8-bit coverage mask (already transformed by m). */
bool mask_from_path(Mask *out, const Path *p, Mat m, FillRule rule, Rect clip);
void mask_free(Mask *m);
void fill_mask(Canvas *c, const Mask *mask, int dx, int dy, const Paint *paint);
void fill_mask_color(Canvas *c, const Mask *mask, int dx, int dy, Color color);
