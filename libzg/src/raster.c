/*
 * The rasterizer.
 *
 * Paths are flattened to line segments, then each segment deposits the exact
 * area it covers into an accumulation buffer, one float per pixel. A running
 * sum along each row turns those deposits into per-pixel coverage, which is
 * then used to blend the paint. This gives precise anti-aliasing with no
 * supersampling.
 */
#include "zg.h"

/* ---- Transforms --------------------------------------------------------------- */

Mat mat_rotate(f32 rad)
{
	f32 s = sinf(rad), c = cosf(rad);
	return (Mat){ c, s, -s, c, 0, 0 };
}

Mat mat_mul(Mat m, Mat n)
{
	return (Mat){
		m.a * n.a + m.c * n.b,
		m.b * n.a + m.d * n.b,
		m.a * n.c + m.c * n.d,
		m.b * n.c + m.d * n.d,
		m.a * n.e + m.c * n.f + m.e,
		m.b * n.e + m.d * n.f + m.f,
	};
}

Mat mat_invert(Mat m)
{
	f32 det = m.a * m.d - m.b * m.c;
	if (fabsf(det) < 1e-12f)
		return mat_identity();
	f32 id = 1 / det;
	return (Mat){
		m.d * id,
		-m.b * id,
		-m.c * id,
		m.a * id,
		(m.c * m.f - m.d * m.e) * id,
		(m.b * m.e - m.a * m.f) * id,
	};
}

f32 mat_scale_factor(Mat m)
{
	return sqrtf(fabsf(m.a * m.d - m.b * m.c));
}

/* ---- Path construction ---------------------------------------------------------- */

static void add_verb(Path *p, PathVerb v)
{
	vec_push(&p->verbs, (u8)v);
}

static void add_pt(Path *p, f32 x, f32 y)
{
	vec_push(&p->pts, (Vec2){ x, y });
}

void path_move(Path *p, f32 x, f32 y)
{
	add_verb(p, PATH_MOVE);
	add_pt(p, x, y);
	p->start = p->cur = (Vec2){ x, y };
}

static void ensure_open(Path *p)
{
	if (!p->verbs.len || p->verbs.data[p->verbs.len - 1] == PATH_CLOSE)
		path_move(p, p->cur.x, p->cur.y);
}

void path_line(Path *p, f32 x, f32 y)
{
	ensure_open(p);
	add_verb(p, PATH_LINE);
	add_pt(p, x, y);
	p->cur = (Vec2){ x, y };
}

void path_quad(Path *p, f32 cx, f32 cy, f32 x, f32 y)
{
	ensure_open(p);
	add_verb(p, PATH_QUAD);
	add_pt(p, cx, cy);
	add_pt(p, x, y);
	p->cur = (Vec2){ x, y };
}

void path_cubic(Path *p, f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y)
{
	ensure_open(p);
	add_verb(p, PATH_CUBIC);
	add_pt(p, c1x, c1y);
	add_pt(p, c2x, c2y);
	add_pt(p, x, y);
	p->cur = (Vec2){ x, y };
}

void path_close(Path *p)
{
	if (p->verbs.len && p->verbs.data[p->verbs.len - 1] != PATH_CLOSE) {
		add_verb(p, PATH_CLOSE);
		p->cur = p->start;
	}
}

void path_arc(Path *p, f32 cx, f32 cy, f32 rx, f32 ry, f32 a0, f32 a1)
{
	/* Split into pieces of at most 90 degrees, each approximated by a cubic. */
	f32 sweep = a1 - a0;
	int n = (int)ceilf(fabsf(sweep) / (PI_F / 2) - 1e-4f);
	if (n < 1)
		n = 1;
	f32 step = sweep / (f32)n;
	f32 k = 4.0f / 3.0f * tanf(step / 4);
	f32 x0 = cx + rx * cosf(a0), y0 = cy + ry * sinf(a0);
	if (!p->verbs.len || p->verbs.data[p->verbs.len - 1] == PATH_CLOSE)
		path_move(p, x0, y0);
	else
		path_line(p, x0, y0);
	f32 a = a0;
	for (int i = 0; i < n; i++) {
		f32 b = a + step;
		f32 ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
		path_cubic(p, cx + rx * (ca - k * sa), cy + ry * (sa + k * ca), cx + rx * (cb + k * sb),
			   cy + ry * (sb - k * cb), cx + rx * cb, cy + ry * sb);
		a = b;
	}
}

void path_arc_to(Path *p, f32 rx, f32 ry, f32 rot_deg, bool large, bool sweep, f32 x, f32 y)
{
	/* Endpoint to centre parameterisation (SVG implementation notes, F.6.5). */
	f32 x1 = p->cur.x, y1 = p->cur.y;
	if ((x1 == x && y1 == y))
		return;
	rx = fabsf(rx), ry = fabsf(ry);
	if (rx < 1e-6f || ry < 1e-6f) {
		path_line(p, x, y);
		return;
	}
	f32 phi = rot_deg * PI_F / 180, cp = cosf(phi), sp = sinf(phi);
	f32 dx = (x1 - x) / 2, dy = (y1 - y) / 2;
	f32 x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
	f32 lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
	if (lambda > 1) {
		f32 s = sqrtf(lambda);
		rx *= s, ry *= s;
	}
	f32 num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
	f32 den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
	f32 coef = den > 0 ? sqrtf(maxf(0, num / den)) : 0;
	if (large == sweep)
		coef = -coef;
	f32 cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
	f32 cx = cp * cxp - sp * cyp + (x1 + x) / 2;
	f32 cy = sp * cxp + cp * cyp + (y1 + y) / 2;
	f32 ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
	f32 vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
	f32 a0 = atan2f(uy, ux);
	f32 da = atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
	if (!sweep && da > 0)
		da -= TAU_F;
	else if (sweep && da < 0)
		da += TAU_F;
	/* Build the arc in its own frame, then rotate into place. */
	int n = (int)ceilf(fabsf(da) / (PI_F / 2) - 1e-4f);
	if (n < 1)
		n = 1;
	f32 step = da / (f32)n, k = 4.0f / 3.0f * tanf(step / 4), a = a0;
	for (int i = 0; i < n; i++) {
		f32 b = a + step;
		f32 ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
		Vec2 q[3] = {
			{ rx * (ca - k * sa), ry * (sa + k * ca) },
			{ rx * (cb + k * sb), ry * (sb - k * cb) },
			{ rx * cb, ry * sb },
		};
		for (int j = 0; j < 3; j++) {
			f32 qx = q[j].x, qy = q[j].y;
			q[j].x = cp * qx - sp * qy + cx;
			q[j].y = sp * qx + cp * qy + cy;
		}
		if (i == n - 1)
			q[2] = (Vec2){ x, y };
		path_cubic(p, q[0].x, q[0].y, q[1].x, q[1].y, q[2].x, q[2].y);
		a = b;
	}
}

void path_rect(Path *p, Rect r)
{
	path_move(p, r.x, r.y);
	path_line(p, r.x + r.w, r.y);
	path_line(p, r.x + r.w, r.y + r.h);
	path_line(p, r.x, r.y + r.h);
	path_close(p);
}

void path_round_rect4(Path *p, Rect r, f32 tl, f32 tr, f32 br, f32 bl)
{
	f32 m = minf(r.w, r.h) / 2;
	tl = clampf(tl, 0, m), tr = clampf(tr, 0, m), br = clampf(br, 0, m), bl = clampf(bl, 0, m);
	const f32 k = 0.5522847498f; /* cubic approximation of a quarter circle */
	f32 x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
	path_move(p, x0 + tl, y0);
	path_line(p, x1 - tr, y0);
	if (tr > 0)
		path_cubic(p, x1 - tr + tr * k, y0, x1, y0 + tr - tr * k, x1, y0 + tr);
	path_line(p, x1, y1 - br);
	if (br > 0)
		path_cubic(p, x1, y1 - br + br * k, x1 - br + br * k, y1, x1 - br, y1);
	path_line(p, x0 + bl, y1);
	if (bl > 0)
		path_cubic(p, x0 + bl - bl * k, y1, x0, y1 - bl + bl * k, x0, y1 - bl);
	path_line(p, x0, y0 + tl);
	if (tl > 0)
		path_cubic(p, x0, y0 + tl - tl * k, x0 + tl - tl * k, y0, x0 + tl, y0);
	path_close(p);
}

void path_round_rect(Path *p, Rect r, f32 radius)
{
	path_round_rect4(p, r, radius, radius, radius, radius);
}

void path_ellipse(Path *p, f32 cx, f32 cy, f32 rx, f32 ry)
{
	if (p->verbs.len && p->verbs.data[p->verbs.len - 1] != PATH_CLOSE)
		path_close(p);
	path_move(p, cx + rx, cy);
	path_arc(p, cx, cy, rx, ry, 0, TAU_F);
	path_close(p);
}

void path_circle(Path *p, f32 cx, f32 cy, f32 r)
{
	path_ellipse(p, cx, cy, r, r);
}

void path_clear(Path *p)
{
	vec_clear(&p->verbs);
	vec_clear(&p->pts);
	p->start = p->cur = (Vec2){ 0, 0 };
}

void path_free(Path *p)
{
	vec_free(&p->verbs);
	vec_free(&p->pts);
}

Rect path_bounds(const Path *p)
{
	if (!p->pts.len)
		return rect(0, 0, 0, 0);
	f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	vec_each(&p->pts, q)
	{
		x0 = minf(x0, q->x), y0 = minf(y0, q->y);
		x1 = maxf(x1, q->x), y1 = maxf(y1, q->y);
	}
	return rect(x0, y0, x1 - x0, y1 - y0);
}

/* ---- Flattening -------------------------------------------------------------------
 * Produces polylines in device space. Each polyline is a run of points with a
 * flag saying whether it is closed. */

typedef struct {
	VEC(Vec2) pts;
	VEC(u32) starts;  /* index of first point of each polyline */
	VEC(u8) closed;
} Poly;

static void poly_begin(Poly *pl, Vec2 p)
{
	vec_push(&pl->starts, (u32)pl->pts.len);
	vec_push(&pl->closed, 0);
	vec_push(&pl->pts, p);
}

static void poly_free(Poly *pl)
{
	vec_free(&pl->pts);
	vec_free(&pl->starts);
	vec_free(&pl->closed);
}

static void flatten_quad(Poly *pl, Vec2 p0, Vec2 p1, Vec2 p2, f32 tol)
{
	f32 dx = p0.x - 2 * p1.x + p2.x, dy = p0.y - 2 * p1.y + p2.y;
	f32 dd = sqrtf(dx * dx + dy * dy);
	int n = (int)ceilf(sqrtf(dd / (8 * tol)));
	n = CLAMP(n, 1, 100);
	for (int i = 1; i <= n; i++) {
		f32 t = (f32)i / (f32)n, u = 1 - t;
		vec_push(&pl->pts, ((Vec2){ u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x,
					    u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y }));
	}
}

static void flatten_cubic(Poly *pl, Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, f32 tol)
{
	f32 ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
	f32 bx = p1.x - 2 * p2.x + p3.x, by = p1.y - 2 * p2.y + p3.y;
	f32 dd = sqrtf(maxf(ax * ax + ay * ay, bx * bx + by * by));
	int n = (int)ceilf(sqrtf(dd * 6 / (8 * tol)));
	n = CLAMP(n, 1, 200);
	for (int i = 1; i <= n; i++) {
		f32 t = (f32)i / (f32)n, u = 1 - t;
		f32 a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
		vec_push(&pl->pts, ((Vec2){ a * p0.x + b * p1.x + c * p2.x + d * p3.x,
					    a * p0.y + b * p1.y + c * p2.y + d * p3.y }));
	}
}

static void flatten(const Path *p, Mat m, Poly *pl, f32 tol)
{
	usize pi = 0;
	Vec2 cur = { 0, 0 };
	bool open = false;
	for (usize i = 0; i < p->verbs.len; i++) {
		switch (p->verbs.data[i]) {
		case PATH_MOVE:
			cur = mat_apply(m, p->pts.data[pi++]);
			poly_begin(pl, cur);
			open = true;
			break;
		case PATH_LINE:
			cur = mat_apply(m, p->pts.data[pi++]);
			vec_push(&pl->pts, cur);
			break;
		case PATH_QUAD: {
			Vec2 c = mat_apply(m, p->pts.data[pi]), e = mat_apply(m, p->pts.data[pi + 1]);
			pi += 2;
			flatten_quad(pl, cur, c, e, tol);
			cur = e;
			break;
		}
		case PATH_CUBIC: {
			Vec2 c1 = mat_apply(m, p->pts.data[pi]), c2 = mat_apply(m, p->pts.data[pi + 1]);
			Vec2 e = mat_apply(m, p->pts.data[pi + 2]);
			pi += 3;
			flatten_cubic(pl, cur, c1, c2, e, tol);
			cur = e;
			break;
		}
		case PATH_CLOSE:
			if (open && pl->closed.len) {
				pl->closed.data[pl->closed.len - 1] = 1;
				cur = pl->pts.data[pl->starts.data[pl->starts.len - 1]];
			}
			open = false;
			break;
		}
	}
}

/* ---- Accumulation ------------------------------------------------------------------- */

typedef struct {
	f32 *acc;
	int w, h;       /* pixels covered */
	int stride;     /* w + 2 */
	f32 ox, oy;     /* device position of acc[0] */
} Acc;

static void acc_line(Acc *a, f32 x0, f32 y0, f32 x1, f32 y1)
{
	x0 -= a->ox, x1 -= a->ox, y0 -= a->oy, y1 -= a->oy;
	if (y0 == y1)
		return;
	f32 dir = 1;
	if (y0 > y1) {
		dir = -1;
		f32 t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
	}
	if (y1 <= 0 || y0 >= (f32)a->h)
		return;
	f32 dxdy = (x1 - x0) / (y1 - y0);
	f32 x = x0;
	if (y0 < 0) {
		x -= y0 * dxdy;
		y0 = 0;
	}
	if (y1 > (f32)a->h)
		y1 = (f32)a->h;
	f32 wmax = (f32)a->w;
	int yend = (int)ceilf(y1);
	for (int y = (int)y0; y < yend; y++) {
		f32 *row = a->acc + (usize)y * a->stride;
		f32 dy = minf((f32)(y + 1), y1) - maxf((f32)y, y0);
		f32 xnext = x + dxdy * dy;
		f32 d = dy * dir;
		/* Clamp horizontally: everything left of the buffer folds into column 0. */
		f32 xa = clampf(x, 0, wmax), xb = clampf(xnext, 0, wmax);
		f32 xl = minf(xa, xb), xr = maxf(xa, xb);
		f32 xlf = floorf(xl);
		int xli = (int)xlf;
		f32 xrc = ceilf(xr);
		int xri = (int)xrc;
		if (xri <= xli + 1) {
			f32 xmf = 0.5f * (xa + xb) - xlf;
			row[xli] += d - d * xmf;
			row[xli + 1] += d * xmf;
		} else {
			f32 s = 1 / (xr - xl);
			f32 x0f = xl - xlf;
			f32 a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
			f32 x1f = xr - xrc + 1;
			f32 am = 0.5f * s * x1f * x1f;
			row[xli] += d * a0;
			if (xri == xli + 2) {
				row[xli + 1] += d * (1 - a0 - am);
			} else {
				f32 a1 = s * (1.5f - x0f);
				row[xli + 1] += d * (a1 - a0);
				for (int xi = xli + 2; xi < xri - 1; xi++)
					row[xi] += d * s;
				f32 a2 = a1 + (f32)(xri - xli - 3) * s;
				row[xri - 1] += d * (1 - a2 - am);
			}
			row[xri] += d * am;
		}
		x = xnext;
	}
}

static bool acc_from_poly(Acc *a, const Poly *pl, Rect clip)
{
	if (!pl->pts.len)
		return false;
	f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	vec_each(&pl->pts, q)
	{
		x0 = minf(x0, q->x), y0 = minf(y0, q->y);
		x1 = maxf(x1, q->x), y1 = maxf(y1, q->y);
	}
	Rect b = rect_intersect(rect(floorf(x0), floorf(y0), ceilf(x1) - floorf(x0) + 1, ceilf(y1) - floorf(y0) + 1), clip);
	int bx0 = (int)floorf(b.x), by0 = (int)floorf(b.y);
	int bx1 = (int)ceilf(b.x + b.w), by1 = (int)ceilf(b.y + b.h);
	if (bx1 <= bx0 || by1 <= by0)
		return false;
	a->w = bx1 - bx0;
	a->h = by1 - by0;
	a->stride = a->w + 2;
	a->ox = (f32)bx0;
	a->oy = (f32)by0;
	a->acc = mem_zalloc(sizeof(f32) * (usize)a->stride * (usize)a->h);
	for (usize i = 0; i < pl->starts.len; i++) {
		usize s = pl->starts.data[i];
		usize e = i + 1 < pl->starts.len ? pl->starts.data[i + 1] : pl->pts.len;
		if (e - s < 2)
			continue;
		for (usize k = s; k + 1 < e; k++)
			acc_line(a, pl->pts.data[k].x, pl->pts.data[k].y, pl->pts.data[k + 1].x, pl->pts.data[k + 1].y);
		/* Fills are always implicitly closed. */
		acc_line(a, pl->pts.data[e - 1].x, pl->pts.data[e - 1].y, pl->pts.data[s].x, pl->pts.data[s].y);
	}
	return true;
}

static inline u32 coverage(f32 sum, FillRule rule)
{
	f32 v = fabsf(sum);
	if (rule == FILL_EVENODD) {
		v = fmodf(v, 2);
		if (v > 1)
			v = 2 - v;
	} else if (v > 1) {
		v = 1;
	}
	return (u32)(v * 256 + 0.5f);
}

/* ---- Paint evaluation ------------------------------------------------------------------ */

typedef struct {
	const Paint *p;
	u32 solid;          /* premultiplied */
	u32 lut[256];       /* gradient colors, premultiplied */
	f32 gx, gy, gk;     /* linear: t = (x*gx + y*gy) - gk */
	Mat inv;            /* image: canvas -> image */
	u32 opacity;        /* 0..256 */
} Shader;

static void build_lut(Shader *s, const Paint *p)
{
	int n = p->nstops;
	for (int i = 0; i < 256; i++) {
		f32 t = (f32)i / 255.0f;
		Color c;
		if (n == 0) {
			c = COLOR_BLACK;
		} else if (t <= p->stops[0].pos) {
			c = p->stops[0].color;
		} else if (t >= p->stops[n - 1].pos) {
			c = p->stops[n - 1].color;
		} else {
			int k = 0;
			while (k + 1 < n && p->stops[k + 1].pos < t)
				k++;
			f32 span = p->stops[k + 1].pos - p->stops[k].pos;
			f32 u = span > 0 ? (t - p->stops[k].pos) / span : 0;
			/* Interpolate premultiplied so transparent stops don't darken. */
			u32 a = color_premul(p->stops[k].color), b = color_premul(p->stops[k + 1].color);
			u32 ui = (u32)(u * 256);
			u32 out = 0;
			for (int sh = 0; sh < 32; sh += 8) {
				u32 ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
				out |= ((ca * (256 - ui) + cb * ui) >> 8) << sh;
			}
			s->lut[i] = out;
			continue;
		}
		s->lut[i] = color_premul(c);
	}
}

static void shader_init(Shader *s, const Paint *p)
{
	s->p = p;
	s->opacity = p->opacity > 0 ? (u32)(clampf(p->opacity, 0, 1) * 256 + 0.5f) : 256;
	switch (p->kind) {
	case PAINT_SOLID:
		s->solid = color_premul(p->color);
		break;
	case PAINT_LINEAR: {
		f32 dx = p->p1.x - p->p0.x, dy = p->p1.y - p->p0.y;
		f32 len2 = dx * dx + dy * dy;
		if (len2 < 1e-6f)
			len2 = 1e-6f;
		s->gx = dx / len2;
		s->gy = dy / len2;
		s->gk = p->p0.x * s->gx + p->p0.y * s->gy;
		build_lut(s, p);
		break;
	}
	case PAINT_RADIAL:
		build_lut(s, p);
		if (p->has_radial_mat)
			s->inv = mat_invert(p->radial_mat);
		break;
	case PAINT_IMAGE:
		s->inv = mat_invert(p->image_mat);
		break;
	}
}

static u32 sample_bilinear(const Canvas *img, f32 x, f32 y)
{
	x -= 0.5f, y -= 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	f32 fx = x - (f32)x0, fy = y - (f32)y0;
	int x1 = x0 + 1, y1 = y0 + 1;
	x0 = CLAMP(x0, 0, img->width - 1), x1 = CLAMP(x1, 0, img->width - 1);
	y0 = CLAMP(y0, 0, img->height - 1), y1 = CLAMP(y1, 0, img->height - 1);
	u32 p00 = canvas_row(img, y0)[x0], p10 = canvas_row(img, y0)[x1];
	u32 p01 = canvas_row(img, y1)[x0], p11 = canvas_row(img, y1)[x1];
	u32 wx = (u32)(fx * 256), wy = (u32)(fy * 256);
	u32 out = 0;
	for (int sh = 0; sh < 32; sh += 8) {
		u32 a = (p00 >> sh) & 0xff, b = (p10 >> sh) & 0xff, c = (p01 >> sh) & 0xff, d = (p11 >> sh) & 0xff;
		u32 top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + d * wx;
		out |= ((top * (256 - wy) + bot * wy) >> 16) << sh;
	}
	return out;
}

static inline u32 shade(const Shader *s, int x, int y)
{
	f32 px = (f32)x + 0.5f, py = (f32)y + 0.5f;
	switch (s->p->kind) {
	case PAINT_SOLID:
		return s->solid;
	case PAINT_LINEAR: {
		f32 t = px * s->gx + py * s->gy - s->gk;
		int i = (int)(clampf(t, 0, 1) * 255 + 0.5f);
		return s->lut[i];
	}
	case PAINT_RADIAL: {
		f32 t;
		if (s->p->has_radial_mat) {
			Vec2 q = mat_apply(s->inv, (Vec2){ px, py });
			t = sqrtf(q.x * q.x + q.y * q.y);
		} else {
			f32 dx = px - s->p->p0.x, dy = py - s->p->p0.y;
			t = sqrtf(dx * dx + dy * dy) / maxf(s->p->radius, 1e-3f);
		}
		int i = (int)(clampf(t, 0, 1) * 255 + 0.5f);
		return s->lut[i];
	}
	case PAINT_IMAGE: {
		Vec2 q = mat_apply(s->inv, (Vec2){ px, py });
		const Canvas *img = s->p->image;
		if (q.x < -0.5f || q.y < -0.5f || q.x > (f32)img->width + 0.5f || q.y > (f32)img->height + 0.5f)
			return 0;
		return sample_bilinear(img, q.x, q.y);
	}
	}
	return 0;
}

static void blend_span(Canvas *c, int x0, int x1, int y, const Shader *s, const u32 *cov)
{
	u32 *row = canvas_row(c, y);
	for (int x = x0; x < x1; x++) {
		u32 k = cov[x - x0];
		if (!k)
			continue;
		k = k * s->opacity >> 8;
		u32 src = shade(s, x, y);
		if (k >= 256 && (src >> 24) == 255)
			row[x] = src;
		else
			row[x] = blend_premul(row[x], src, k);
	}
}

static void composite_acc(Canvas *c, const Acc *a, const Paint *paint, FillRule rule)
{
	Shader s;
	shader_init(&s, paint);
	u32 *cov = mem_alloc(sizeof(u32) * (usize)(a->w + 1));
	int ox = (int)a->ox, oy = (int)a->oy;
	for (int y = 0; y < a->h; y++) {
		const f32 *row = a->acc + (usize)y * a->stride;
		f32 sum = 0;
		int first = -1, last = -1;
		for (int x = 0; x < a->w; x++) {
			sum += row[x];
			u32 k = coverage(sum, rule);
			cov[x] = k;
			if (k) {
				if (first < 0)
					first = x;
				last = x;
			}
		}
		if (first >= 0)
			blend_span(c, ox + first, ox + last + 1, oy + y, &s, cov + first);
	}
	mem_free(cov);
}

static Rect canvas_clip_rect(const Canvas *c)
{
	return rect_intersect(c->clip, rect(0, 0, (f32)c->width, (f32)c->height));
}

void fill_path(Canvas *c, const Path *p, Mat m, const Paint *paint, FillRule rule)
{
	Poly pl = { 0 };
	flatten(p, m, &pl, 0.1f);
	Acc a;
	if (acc_from_poly(&a, &pl, canvas_clip_rect(c))) {
		composite_acc(c, &a, paint, rule);
		mem_free(a.acc);
	}
	poly_free(&pl);
}

/* ---- Masks --------------------------------------------------------------------------- */

bool mask_from_path(Mask *out, const Path *p, Mat m, FillRule rule, Rect clip)
{
	Poly pl = { 0 };
	flatten(p, m, &pl, 0.15f);
	Acc a;
	bool ok = acc_from_poly(&a, &pl, clip);
	poly_free(&pl);
	if (!ok) {
		memset(out, 0, sizeof *out);
		return false;
	}
	out->x = (int)a.ox;
	out->y = (int)a.oy;
	out->w = a.w;
	out->h = a.h;
	out->data = mem_alloc((usize)a.w * (usize)a.h);
	for (int y = 0; y < a.h; y++) {
		const f32 *row = a.acc + (usize)y * a.stride;
		u8 *dst = out->data + (usize)y * a.w;
		f32 sum = 0;
		for (int x = 0; x < a.w; x++) {
			sum += row[x];
			u32 k = coverage(sum, rule);
			dst[x] = (u8)(k > 255 ? 255 : k);
		}
	}
	mem_free(a.acc);
	return true;
}

void mask_free(Mask *m)
{
	mem_free(m->data);
	m->data = NULL;
}

void fill_mask(Canvas *c, const Mask *mask, int dx, int dy, const Paint *paint)
{
	Shader s;
	shader_init(&s, paint);
	Rect clip = canvas_clip_rect(c);
	int cx0 = (int)clip.x, cy0 = (int)clip.y, cx1 = (int)(clip.x + clip.w), cy1 = (int)(clip.y + clip.h);
	for (int y = 0; y < mask->h; y++) {
		int py = mask->y + dy + y;
		if (py < cy0 || py >= cy1)
			continue;
		const u8 *src = mask->data + (usize)y * mask->w;
		u32 *row = canvas_row(c, py);
		for (int x = 0; x < mask->w; x++) {
			int px = mask->x + dx + x;
			if (px < cx0 || px >= cx1 || !src[x])
				continue;
			u32 k = ((u32)src[x] + (src[x] >> 7)) * s.opacity >> 8; /* 255 -> 256 */
			row[px] = blend_premul(row[px], shade(&s, px, py), k);
		}
	}
}

void fill_mask_color(Canvas *c, const Mask *mask, int dx, int dy, Color color)
{
	Paint p = paint_solid(color);
	fill_mask(c, mask, dx, dy, &p);
}

/* ---- Paint constructors ---------------------------------------------------------------- */

Paint paint_solid(Color c)
{
	return (Paint){ .kind = PAINT_SOLID, .color = c };
}

Paint paint_linear(f32 x0, f32 y0, f32 x1, f32 y1, Color c0, Color c1)
{
	Paint p = { .kind = PAINT_LINEAR, .p0 = { x0, y0 }, .p1 = { x1, y1 } };
	paint_add_stop(&p, 0, c0);
	paint_add_stop(&p, 1, c1);
	return p;
}

Paint paint_radial(f32 cx, f32 cy, f32 r, Color inner, Color outer)
{
	Paint p = { .kind = PAINT_RADIAL, .p0 = { cx, cy }, .radius = r };
	paint_add_stop(&p, 0, inner);
	paint_add_stop(&p, 1, outer);
	return p;
}

void paint_add_stop(Paint *p, f32 pos, Color c)
{
	if (p->nstops >= MAX_STOPS)
		return;
	int i = p->nstops++;
	while (i > 0 && p->stops[i - 1].pos > pos) {
		p->stops[i] = p->stops[i - 1];
		i--;
	}
	p->stops[i] = (GradientStop){ clampf(pos, 0, 1), c };
}

Paint paint_image(const Canvas *img, Mat m)
{
	return (Paint){ .kind = PAINT_IMAGE, .image = img, .image_mat = m };
}

/* ---- Stroking --------------------------------------------------------------------------
 * Each segment becomes a quad, each join and round cap a disc or wedge, all wound
 * the same way so the nonzero rule merges overlaps into one clean shape. */

static void add_quad(Path *o, Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
	/* Normalise orientation so every piece has positive signed area. */
	f32 area = (a.x * b.y - b.x * a.y) + (b.x * c.y - c.x * b.y) + (c.x * d.y - d.x * c.y) + (d.x * a.y - a.x * d.y);
	if (area < 0) {
		Vec2 t = b;
		b = d;
		d = t;
	}
	path_move(o, a.x, a.y);
	path_line(o, b.x, b.y);
	path_line(o, c.x, c.y);
	path_line(o, d.x, d.y);
	path_close(o);
}

static void add_tri(Path *o, Vec2 a, Vec2 b, Vec2 c)
{
	f32 area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
	if (area < 0) {
		Vec2 t = b;
		b = c;
		c = t;
	}
	path_move(o, a.x, a.y);
	path_line(o, b.x, b.y);
	path_line(o, c.x, c.y);
	path_close(o);
}

static void add_disc(Path *o, Vec2 c, f32 r)
{
	/* Polygonal disc, positive orientation, fine enough for any size. */
	int n = CLAMP((int)(r * 2.2f) + 8, 8, 96);
	path_move(o, c.x + r, c.y);
	for (int i = 1; i < n; i++) {
		f32 a = TAU_F * (f32)i / (f32)n;
		path_line(o, c.x + r * cosf(a), c.y + r * sinf(a));
	}
	path_close(o);
}

void path_stroke_outline(const Path *in, Mat m, const StrokeStyle *st, Path *out)
{
	Poly pl = { 0 };
	flatten(in, m, &pl, 0.15f);
	f32 hw = st->width * mat_scale_factor(m) / 2;
	if (hw <= 0)
		hw = 0.5f;
	f32 miter = st->miter_limit > 0 ? st->miter_limit : 4;
	for (usize i = 0; i < pl.starts.len; i++) {
		usize s = pl.starts.data[i];
		usize e = i + 1 < pl.starts.len ? pl.starts.data[i + 1] : pl.pts.len;
		bool closed = pl.closed.data[i];
		/* Drop duplicate points. */
		VEC(Vec2) pts = { 0 };
		for (usize k = s; k < e; k++) {
			Vec2 q = pl.pts.data[k];
			if (pts.len) {
				Vec2 l = vec_last(&pts);
				if (fabsf(l.x - q.x) < 1e-4f && fabsf(l.y - q.y) < 1e-4f)
					continue;
			}
			vec_push(&pts, q);
		}
		if (closed && pts.len > 2) {
			Vec2 f = pts.data[0], l = vec_last(&pts);
			if (fabsf(l.x - f.x) < 1e-4f && fabsf(l.y - f.y) < 1e-4f)
				pts.len--;
		}
		if (pts.len == 1) {
			if (st->cap == CAP_ROUND)
				add_disc(out, pts.data[0], hw);
			else if (st->cap == CAP_SQUARE)
				path_rect(out, rect(pts.data[0].x - hw, pts.data[0].y - hw, 2 * hw, 2 * hw));
			vec_free(&pts);
			continue;
		}
		usize nseg = closed ? pts.len : pts.len - 1;
		for (usize k = 0; k < nseg; k++) {
			Vec2 a = pts.data[k], b = pts.data[(k + 1) % pts.len];
			f32 dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
			if (len < 1e-6f)
				continue;
			f32 nx = -dy / len * hw, ny = dx / len * hw;
			f32 ex = 0, ey = 0;
			bool first = !closed && k == 0, last = !closed && k == nseg - 1;
			Vec2 a2 = a, b2 = b;
			if (st->cap == CAP_SQUARE) {
				ex = dx / len * hw, ey = dy / len * hw;
				if (first)
					a2 = (Vec2){ a.x - ex, a.y - ey };
				if (last)
					b2 = (Vec2){ b.x + ex, b.y + ey };
			}
			add_quad(out, (Vec2){ a2.x + nx, a2.y + ny }, (Vec2){ b2.x + nx, b2.y + ny },
				 (Vec2){ b2.x - nx, b2.y - ny }, (Vec2){ a2.x - nx, a2.y - ny });
		}
		/* Joins */
		usize njoin = closed ? pts.len : (pts.len >= 2 ? pts.len - 2 : 0);
		for (usize k = 0; k < njoin; k++) {
			usize ib = closed ? k : k + 1;
			Vec2 a = pts.data[(ib + pts.len - 1) % pts.len], b = pts.data[ib], c2 = pts.data[(ib + 1) % pts.len];
			if (st->join == JOIN_ROUND) {
				add_disc(out, b, hw);
				continue;
			}
			f32 d0x = b.x - a.x, d0y = b.y - a.y, l0 = sqrtf(d0x * d0x + d0y * d0y);
			f32 d1x = c2.x - b.x, d1y = c2.y - b.y, l1 = sqrtf(d1x * d1x + d1y * d1y);
			if (l0 < 1e-6f || l1 < 1e-6f)
				continue;
			d0x /= l0, d0y /= l0, d1x /= l1, d1y /= l1;
			f32 cross = d0x * d1y - d0y * d1x;
			if (fabsf(cross) < 1e-4f)
				continue;
			/* Outer side of the turn */
			f32 side = cross > 0 ? -1.0f : 1.0f;
			Vec2 p0 = { b.x - d0y * hw * side, b.y + d0x * hw * side };
			Vec2 p1 = { b.x - d1y * hw * side, b.y + d1x * hw * side };
			add_tri(out, b, p0, p1);
			if (st->join == JOIN_MITER) {
				f32 cosang = d0x * d1x + d0y * d1y;
				f32 ml = 1 / sqrtf(maxf((1 + cosang) / 2, 1e-6f));
				if (ml <= miter) {
					f32 mx = -(d0y + d1y), my = d0x + d1x;
					f32 mlen = sqrtf(mx * mx + my * my);
					if (mlen > 1e-6f) {
						Vec2 tip = { b.x + mx / mlen * hw * ml * side, b.y + my / mlen * hw * ml * side };
						add_tri(out, p0, tip, p1);
					}
				}
			}
		}
		if (!closed && st->cap == CAP_ROUND) {
			add_disc(out, pts.data[0], hw);
			add_disc(out, vec_last(&pts), hw);
		}
		vec_free(&pts);
	}
	poly_free(&pl);
}

void stroke_path(Canvas *c, const Path *p, Mat m, const Paint *paint, const StrokeStyle *style)
{
	Path outline = { 0 };
	path_stroke_outline(p, m, style, &outline);
	fill_path(c, &outline, mat_identity(), paint, FILL_NONZERO);
	path_free(&outline);
}

/* ---- Dashing ------------------------------------------------------------------- */

void path_dash(const Path *in, const f32 *pattern, int n, f32 offset, Path *out)
{
	f32 total = 0;
	for (int i = 0; i < n; i++)
		total += maxf(pattern[i], 0);
	if (n <= 0 || total <= 0) {
		/* No usable pattern: copy the path through unchanged. */
		Poly pl = { 0 };
		flatten(in, mat_identity(), &pl, 0.05f);
		for (usize i = 0; i < pl.starts.len; i++) {
			usize s = pl.starts.data[i];
			usize e = i + 1 < pl.starts.len ? pl.starts.data[i + 1] : pl.pts.len;
			for (usize k = s; k < e; k++)
				(k == s ? path_move : path_line)(out, pl.pts.data[k].x, pl.pts.data[k].y);
			if (pl.closed.data[i])
				path_close(out);
		}
		poly_free(&pl);
		return;
	}
	Poly pl = { 0 };
	flatten(in, mat_identity(), &pl, 0.05f);
	for (usize i = 0; i < pl.starts.len; i++) {
		usize s = pl.starts.data[i];
		usize e = i + 1 < pl.starts.len ? pl.starts.data[i + 1] : pl.pts.len;
		/* Walk the polyline (closing it if needed), emitting the "on" spans. */
		int idx = 0;
		f32 remain = 0, phase = fmodf(offset, total);
		if (phase < 0)
			phase += total;
		while (phase >= pattern[idx] && pattern[idx] >= 0) {
			phase -= pattern[idx];
			idx = (idx + 1) % n;
		}
		remain = pattern[idx] - phase;
		bool on = (idx % 2) == 0, drawing = false;
		usize count = e - s + (pl.closed.data[i] ? 1 : 0);
		for (usize k = 0; k + 1 < count; k++) {
			Vec2 a = pl.pts.data[s + k], b = pl.pts.data[s + (k + 1) % (e - s)];
			f32 dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy), t = 0;
			while (len - t > 1e-6f) {
				f32 step = minf(remain, len - t);
				f32 x0 = a.x + dx * t / len, y0 = a.y + dy * t / len;
				f32 x1 = a.x + dx * (t + step) / len, y1 = a.y + dy * (t + step) / len;
				if (on) {
					if (!drawing)
						path_move(out, x0, y0);
					path_line(out, x1, y1);
					drawing = true;
				}
				t += step;
				remain -= step;
				if (remain <= 1e-6f) {
					idx = (idx + 1) % n;
					remain = pattern[idx];
					on = !on;
					drawing = false;
				}
			}
		}
	}
	poly_free(&pl);
}
