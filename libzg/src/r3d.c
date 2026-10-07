/*
 * The 3D renderer: matrices, meshes, and a rasterizer for flat-coloured
 * triangles.
 *
 * Drawing a mesh transforms its corners to clip space once, then lights each
 * triangle by its normal, clips against the near plane and projects. What
 * comes out is a list of screen triangles with a colour and a depth
 * gradient. Fog comes from the true distance while filling, worked out
 * every 16 pixels and blended in straight lines between, so it stays right
 * across big ground triangles that reach past the eye. r3_end splits the target into bands of rows
 * that every processor takes in turn; each band paints the sky, then the
 * opaque triangles with a depth test, then the see-through ones from far to
 * near. Depth is 1/w, which interpolates linearly across the screen.
 */
#include "zg.h"

#define TAU 6.28318531f
#define SWAP(a, b)                \
	do {                      \
		__auto_type t_ = (a); \
		(a) = (b);            \
		(b) = t_;             \
	} while (0)

/* ---- Vectors and matrices --------------------------------------------------------------- */

Vec3 v3_norm(Vec3 a)
{
	f32 l = v3_len(a);
	return l > 0 ? v3_mul(a, 1 / l) : a;
}

Mat4 m4_identity(void)
{
	Mat4 r = { 0 };
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
	return r;
}

Mat4 m4_mul(Mat4 a, Mat4 b)
{
	Mat4 r;
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++) {
			f32 s = 0;
			for (int k = 0; k < 4; k++)
				s += a.m[k * 4 + row] * b.m[c * 4 + k];
			r.m[c * 4 + row] = s;
		}
	return r;
}

Mat4 m4_translate(Vec3 t)
{
	Mat4 r = m4_identity();
	r.m[12] = t.x, r.m[13] = t.y, r.m[14] = t.z;
	return r;
}

Mat4 m4_scale(Vec3 s)
{
	Mat4 r = m4_identity();
	r.m[0] = s.x, r.m[5] = s.y, r.m[10] = s.z;
	return r;
}

Mat4 m4_rotate(Vec3 axis, f32 a)
{
	axis = v3_norm(axis);
	f32 c = cosf(a), s = sinf(a), t = 1 - c, x = axis.x, y = axis.y, z = axis.z;
	Mat4 r = m4_identity();
	r.m[0] = t * x * x + c, r.m[1] = t * x * y + s * z, r.m[2] = t * x * z - s * y;
	r.m[4] = t * x * y - s * z, r.m[5] = t * y * y + c, r.m[6] = t * y * z + s * x;
	r.m[8] = t * x * z + s * y, r.m[9] = t * y * z - s * x, r.m[10] = t * z * z + c;
	return r;
}

Mat4 m4_orient(Vec3 right, Vec3 up, Vec3 forward, Vec3 pos)
{
	Mat4 r = m4_identity();
	r.m[0] = right.x, r.m[1] = right.y, r.m[2] = right.z;
	r.m[4] = up.x, r.m[5] = up.y, r.m[6] = up.z;
	r.m[8] = -forward.x, r.m[9] = -forward.y, r.m[10] = -forward.z;
	r.m[12] = pos.x, r.m[13] = pos.y, r.m[14] = pos.z;
	return r;
}

Mat4 m4_look_at(Vec3 eye, Vec3 target, Vec3 up)
{
	Vec3 f = v3_norm(v3_sub(target, eye)), s = v3_norm(v3_cross(f, up)), u = v3_cross(s, f);
	Mat4 r = m4_identity();
	r.m[0] = s.x, r.m[4] = s.y, r.m[8] = s.z;
	r.m[1] = u.x, r.m[5] = u.y, r.m[9] = u.z;
	r.m[2] = -f.x, r.m[6] = -f.y, r.m[10] = -f.z;
	r.m[12] = -v3_dot(s, eye), r.m[13] = -v3_dot(u, eye), r.m[14] = v3_dot(f, eye);
	return r;
}

Mat4 m4_perspective(f32 fovy, f32 aspect, f32 n, f32 f)
{
	f32 t = 1 / tanf(fovy / 2);
	Mat4 r = { 0 };
	r.m[0] = t / aspect;
	r.m[5] = t;
	r.m[10] = (f + n) / (n - f);
	r.m[11] = -1;
	r.m[14] = 2 * f * n / (n - f);
	return r;
}

Vec3 m4_point(Mat4 m, Vec3 p)
{
	return v3(m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12], m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
		  m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]);
}

Vec3 m4_dir(Mat4 m, Vec3 d)
{
	return v3(m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z, m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z, m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z);
}

/* ---- Meshes ------------------------------------------------------------------------------ */

Mesh3 *mesh3_new(void)
{
	return NEW(Mesh3);
}

void mesh3_free(Mesh3 *m)
{
	if (!m)
		return;
	mem_free(m->pos);
	mem_free(m->idx);
	mem_free(m->color);
	mem_free(m->normal);
	mem_free(m);
}

void mesh3_clear(Mesh3 *m)
{
	m->npos = m->ntri = 0;
}

u32 mesh3_vertex(Mesh3 *m, Vec3 p)
{
	if (m->npos == m->cap_pos) {
		m->cap_pos = m->cap_pos ? m->cap_pos * 2 : 64;
		m->pos = mem_realloc(m->pos, m->cap_pos * sizeof *m->pos);
	}
	m->pos[m->npos] = p;
	return m->npos++;
}

void mesh3_tri(Mesh3 *m, u32 a, u32 b, u32 c, Color color)
{
	if (m->ntri == m->cap_tri) {
		m->cap_tri = m->cap_tri ? m->cap_tri * 2 : 64;
		m->idx = mem_realloc(m->idx, m->cap_tri * 3 * sizeof *m->idx);
		m->color = mem_realloc(m->color, m->cap_tri * sizeof *m->color);
		m->normal = mem_realloc(m->normal, m->cap_tri * sizeof *m->normal);
	}
	m->idx[m->ntri * 3] = a, m->idx[m->ntri * 3 + 1] = b, m->idx[m->ntri * 3 + 2] = c;
	m->color[m->ntri++] = color;
}

void mesh3_quad(Mesh3 *m, u32 a, u32 b, u32 c, u32 d, Color color)
{
	mesh3_tri(m, a, b, c, color);
	mesh3_tri(m, a, c, d, color);
}

void mesh3_finish(Mesh3 *m)
{
	for (u32 i = 0; i < m->ntri; i++) {
		Vec3 a = m->pos[m->idx[i * 3]], b = m->pos[m->idx[i * 3 + 1]], c = m->pos[m->idx[i * 3 + 2]];
		m->normal[i] = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
	}
	Vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
	for (u32 i = 0; i < m->npos; i++) {
		Vec3 p = m->pos[i];
		lo = v3(minf(lo.x, p.x), minf(lo.y, p.y), minf(lo.z, p.z));
		hi = v3(maxf(hi.x, p.x), maxf(hi.y, p.y), maxf(hi.z, p.z));
	}
	m->center = m->npos ? v3_mul(v3_add(lo, hi), 0.5f) : v3(0, 0, 0);
	m->radius = 0;
	for (u32 i = 0; i < m->npos; i++)
		m->radius = maxf(m->radius, v3_len(v3_sub(m->pos[i], m->center)));
}

void mesh3_box(Mesh3 *m, Vec3 c, Vec3 size, Color color)
{
	f32 hx = size.x / 2, hy = size.y / 2, hz = size.z / 2;
	u32 v[8];
	for (int i = 0; i < 8; i++)
		v[i] = mesh3_vertex(m, v3(c.x + (i & 1 ? hx : -hx), c.y + (i & 2 ? hy : -hy), c.z + (i & 4 ? hz : -hz)));
	mesh3_quad(m, v[4], v[5], v[7], v[6], color); /* +z */
	mesh3_quad(m, v[1], v[0], v[2], v[3], color); /* -z */
	mesh3_quad(m, v[5], v[1], v[3], v[7], color); /* +x */
	mesh3_quad(m, v[0], v[4], v[6], v[2], color); /* -x */
	mesh3_quad(m, v[6], v[7], v[3], v[2], color); /* +y */
	mesh3_quad(m, v[0], v[1], v[5], v[4], color); /* -y */
}

void mesh3_cone(Mesh3 *m, Vec3 base, f32 radius, f32 top_radius, f32 height, int sides, Color color)
{
	u32 first = m->npos;
	for (int i = 0; i < sides; i++) {
		f32 a = (f32)i * TAU / (f32)sides;
		mesh3_vertex(m, v3(base.x + cosf(a) * radius, base.y, base.z - sinf(a) * radius));
	}
	u32 bottom = mesh3_vertex(m, base);
	if (top_radius <= 0) {
		u32 tip = mesh3_vertex(m, v3(base.x, base.y + height, base.z));
		for (int i = 0; i < sides; i++) {
			u32 a = first + (u32)i, b = first + (u32)((i + 1) % sides);
			mesh3_tri(m, a, b, tip, color);
			mesh3_tri(m, b, a, bottom, color);
		}
		return;
	}
	u32 top_first = m->npos;
	for (int i = 0; i < sides; i++) {
		f32 a = (f32)i * TAU / (f32)sides;
		mesh3_vertex(m, v3(base.x + cosf(a) * top_radius, base.y + height, base.z - sinf(a) * top_radius));
	}
	u32 top = mesh3_vertex(m, v3(base.x, base.y + height, base.z));
	for (int i = 0; i < sides; i++) {
		u32 a = first + (u32)i, b = first + (u32)((i + 1) % sides);
		u32 ta = top_first + (u32)i, tb = top_first + (u32)((i + 1) % sides);
		mesh3_quad(m, a, b, tb, ta, color);
		mesh3_tri(m, b, a, bottom, color);
		mesh3_tri(m, ta, tb, top, color);
	}
}

void mesh3_torus(Mesh3 *m, Vec3 c, f32 R, f32 r, int segments, int sides, Color color)
{
	u32 first = m->npos;
	for (int i = 0; i < segments; i++) {
		f32 a = (f32)i * TAU / (f32)segments;
		Vec3 dir = v3(cosf(a), 0, -sinf(a));
		for (int j = 0; j < sides; j++) {
			f32 b = (f32)j * TAU / (f32)sides;
			Vec3 p = v3_add(c, v3_add(v3_mul(dir, R + cosf(b) * r), v3(0, sinf(b) * r, 0)));
			mesh3_vertex(m, p);
		}
	}
	for (int i = 0; i < segments; i++)
		for (int j = 0; j < sides; j++) {
			u32 a = first + (u32)(i * sides + j), b = first + (u32)(((i + 1) % segments) * sides + j);
			u32 cc = first + (u32)(((i + 1) % segments) * sides + (j + 1) % sides), d = first + (u32)(i * sides + (j + 1) % sides);
			mesh3_quad(m, a, b, cc, d, color);
		}
}

void mesh3_sphere(Mesh3 *m, Vec3 c, f32 radius, int rings, int sides, Color color)
{
	u32 top = mesh3_vertex(m, v3(c.x, c.y + radius, c.z));
	u32 first = m->npos;
	for (int i = 1; i < rings; i++) {
		f32 phi = (f32)i * (TAU / 2) / (f32)rings, y = cosf(phi), rr = sinf(phi);
		for (int j = 0; j < sides; j++) {
			f32 a = (f32)j * TAU / (f32)sides;
			mesh3_vertex(m, v3(c.x + cosf(a) * rr * radius, c.y + y * radius, c.z - sinf(a) * rr * radius));
		}
	}
	u32 bottom = mesh3_vertex(m, v3(c.x, c.y - radius, c.z));
	for (int j = 0; j < sides; j++) {
		u32 a = first + (u32)j, b = first + (u32)((j + 1) % sides);
		mesh3_tri(m, top, a, b, color);
		u32 la = first + (u32)((rings - 2) * sides + j), lb = first + (u32)((rings - 2) * sides + (j + 1) % sides);
		mesh3_tri(m, bottom, lb, la, color);
	}
	for (int i = 0; i < rings - 2; i++)
		for (int j = 0; j < sides; j++) {
			u32 a = first + (u32)(i * sides + j), b = first + (u32)(i * sides + (j + 1) % sides);
			u32 cc = first + (u32)((i + 1) * sides + (j + 1) % sides), d = first + (u32)((i + 1) * sides + j);
			mesh3_quad(m, a, d, cc, b, color);
		}
}

/* ---- Renderer ---------------------------------------------------------------------------- */

#define BAND 16
#define MAX_WORKERS 7

typedef struct {
	f32 x, y, z, w;   /* clip space */
} ClipVert;

typedef ClipVert Corner;

/* A triangle ready to fill: corners sorted top to bottom, how depth
 * changes across the screen, and its lit colour. */
typedef struct {
	f32 x[3], y[3];
	f32 s01, s02, s12;          /* x change per row along each edge */
	f32 z0, dzdx, dzdy;         /* 1/w at corner 0, and its gradient */
	f32 rgb[3];                 /* lit colour, 0..255 */
	f32 depth;                  /* for ordering see-through triangles */
	i32 ytop, ybot;             /* rows [ytop, ybot) */
	u32 alpha;
} STri;

typedef struct {
	STri *data;
	u32 len, cap;
} TriList;

struct R3 {
	Canvas *target;
	f32 *depth;
	usize depth_cap;
	int w, h;
	Mat4 view, proj, vp;
	Vec3 eye, fwd, right, up;
	f32 tan_y, aspect, znear, zfar;
	f32 planes[6][4];
	Vec3 sun;
	f32 sun_rgb[3], amb_rgb[3];
	f32 fog_rgb[3], fog_start, fog_end, fog_scale;
	/* the sky */
	bool sky;
	u32 sky_lut[256];
	f32 glow_rgb[3], sun_cos, sun_edge;
	u32 sun_px;
	TriList solid, glass;
	ClipVert *tmp;
	u32 tmp_cap;
	u32 drawn;
	/* workers */
	int nworkers, gen, next_band, done, alive, quit;
};

R3 *r3_new(void)
{
	R3 *r = NEW(R3);
	r->sun = v3_norm(v3(0.3f, 0.8f, 0.4f));
	r->sun_rgb[0] = r->sun_rgb[1] = r->sun_rgb[2] = 0.8f;
	r->amb_rgb[0] = r->amb_rgb[1] = r->amb_rgb[2] = 0.35f;
	r->fog_start = 1e8f, r->fog_end = 2e8f, r->fog_scale = 1e-8f;
	return r;
}

void r3_free(R3 *r)
{
	if (!r)
		return;
	if (r->nworkers) {
		atomic_store(&r->quit, 1);
		atomic_add(&r->gen, 1);
		futex_wake(&r->gen, 1 << 30);
		int a;
		while ((a = atomic_load(&r->alive)) > 0)
			futex_wait(&r->alive, a, 100);
	}
	mem_free(r->depth);
	mem_free(r->solid.data);
	mem_free(r->glass.data);
	mem_free(r->tmp);
	mem_free(r);
}

static void rgb_of(Color c, f32 *out)
{
	out[0] = (f32)COLOR_R(c) / 255, out[1] = (f32)COLOR_G(c) / 255, out[2] = (f32)COLOR_B(c) / 255;
}

void r3_begin(R3 *r, Canvas *target)
{
	r->target = target;
	r->w = target->width, r->h = target->height;
	usize n = (usize)r->w * (usize)r->h;
	if (n > r->depth_cap) {
		mem_free(r->depth);
		r->depth = mem_alloc(n * sizeof *r->depth);
		r->depth_cap = n;
	}
	r->solid.len = r->glass.len = 0;
	r->sky = false;
}

void r3_camera(R3 *r, Vec3 eye, Vec3 target, Vec3 up, f32 fovy, f32 znear, f32 zfar)
{
	r->eye = eye;
	r->znear = znear, r->zfar = zfar;
	r->aspect = (f32)r->w / (f32)MAX(1, r->h);
	r->tan_y = tanf(fovy / 2);
	r->view = m4_look_at(eye, target, up);
	r->proj = m4_perspective(fovy, r->aspect, znear, zfar);
	r->vp = m4_mul(r->proj, r->view);
	r->fwd = v3_norm(v3_sub(target, eye));
	r->right = v3_norm(v3_cross(r->fwd, up));
	r->up = v3_cross(r->right, r->fwd);
	/* frustum planes from the combined matrix (rows combined), normalized */
	const f32 *m = r->vp.m;
	for (int i = 0; i < 6; i++) {
		int row = i / 2;
		f32 sgn = i % 2 ? -1 : 1;
		f32 *p = r->planes[i];
		for (int k = 0; k < 4; k++)
			p[k] = m[k * 4 + 3] + sgn * m[k * 4 + row];
		f32 l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		for (int k = 0; k < 4; k++)
			p[k] /= l;
	}
}

void r3_light(R3 *r, Vec3 toward_sun, Color sun, Color ambient)
{
	r->sun = v3_norm(toward_sun);
	rgb_of(sun, r->sun_rgb);
	rgb_of(ambient, r->amb_rgb);
}

void r3_fog(R3 *r, Color fog, f32 start, f32 end)
{
	rgb_of(fog, r->fog_rgb);
	r->fog_start = start;
	r->fog_end = maxf(end, start + 0.01f);
	r->fog_scale = 1 / (r->fog_end - r->fog_start);
}

void r3_sky(R3 *r, Color zenith, Color horizon, f32 sun_size, Color sun)
{
	r->sky = true;
	/* elevation 0 (horizon) .. 1 (straight up), eased so the horizon band is wide */
	for (int i = 0; i < 256; i++) {
		f32 t = (f32)i / 255;
		r->sky_lut[i] = color_premul(color_mix(horizon, zenith, sqrtf(t)));
	}
	rgb_of(sun, r->glow_rgb);
	r->sun_cos = sun_size > 0 ? cosf(sun_size) : 2;
	r->sun_edge = sun_size > 0 ? cosf(sun_size * 0.8f) : 2;
	r->sun_px = color_premul(color_mix(sun, COLOR_WHITE, 0.55f));
}

bool r3_visible(R3 *r, Vec3 c, f32 radius)
{
	for (int i = 0; i < 6; i++) {
		const f32 *p = r->planes[i];
		if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -radius)
			return false;
	}
	return true;
}

bool r3_project(R3 *r, Vec3 p, Vec2 *out, f32 *scale)
{
	const f32 *m = r->vp.m;
	f32 x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
	f32 y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
	f32 w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
	if (w < r->znear)
		return false;
	out->x = (x / w * 0.5f + 0.5f) * (f32)r->w;
	out->y = (0.5f - y / w * 0.5f) * (f32)r->h;
	if (scale)
		*scale = (f32)r->h * 0.5f / (r->tan_y * w);
	return true;
}

static STri *push_tri(TriList *l)
{
	if (l->len == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 1024;
		l->data = mem_realloc(l->data, l->cap * sizeof *l->data);
	}
	return &l->data[l->len++];
}

/* Projects a clipped triangle and works out its depth gradient. */
static void setup(R3 *r, const Corner *a, const Corner *b, const Corner *c, const f32 *rgb, u32 alpha, bool cull)
{
	f32 sx[3], sy[3], sz[3];
	const Corner *v[3] = { a, b, c };
	for (int i = 0; i < 3; i++) {
		f32 iw = 1 / v[i]->w;
		sx[i] = (v[i]->x * iw * 0.5f + 0.5f) * (f32)r->w;
		sy[i] = (0.5f - v[i]->y * iw * 0.5f) * (f32)r->h;
		sz[i] = iw;
	}
	f32 area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
	if (cull && area >= 0)
		return;
	if (fabsf(area) < 1e-4f)
		return;
	/* sort the corners top to bottom */
	int o[3] = { 0, 1, 2 };
	if (sy[o[1]] < sy[o[0]])
		SWAP(o[0], o[1]);
	if (sy[o[2]] < sy[o[1]])
		SWAP(o[1], o[2]);
	if (sy[o[1]] < sy[o[0]])
		SWAP(o[0], o[1]);
	i32 ytop = (i32)ceilf(sy[o[0]] - 0.5f), ybot = (i32)ceilf(sy[o[2]] - 0.5f);
	ytop = MAX(ytop, 0), ybot = MIN(ybot, r->h);
	if (ytop >= ybot)
		return;
	f32 xmin = minf(sx[0], minf(sx[1], sx[2])), xmax = maxf(sx[0], maxf(sx[1], sx[2]));
	if (xmax < 0 || xmin > (f32)r->w)
		return;
	STri *t = push_tri(alpha < 255 ? &r->glass : &r->solid);
	for (int i = 0; i < 3; i++)
		t->x[i] = sx[o[i]], t->y[i] = sy[o[i]];
	f32 dy01 = t->y[1] - t->y[0], dy02 = t->y[2] - t->y[0], dy12 = t->y[2] - t->y[1];
	t->s01 = dy01 > 0 ? (t->x[1] - t->x[0]) / dy01 : 0;
	t->s02 = dy02 > 0 ? (t->x[2] - t->x[0]) / dy02 : 0;
	t->s12 = dy12 > 0 ? (t->x[2] - t->x[1]) / dy12 : 0;
	/* the depth plane through the three corners */
	f32 x10 = sx[1] - sx[0], x20 = sx[2] - sx[0], y10 = sy[1] - sy[0], y20 = sy[2] - sy[0];
	f32 inv = 1 / area;
	t->dzdx = ((sz[1] - sz[0]) * y20 - (sz[2] - sz[0]) * y10) * inv;
	t->dzdy = ((sz[2] - sz[0]) * x10 - (sz[1] - sz[0]) * x20) * inv;
	t->z0 = sz[0] + t->dzdx * (t->x[0] - sx[0]) + t->dzdy * (t->y[0] - sy[0]);
	for (int i = 0; i < 3; i++)
		t->rgb[i] = minf(1, rgb[i]) * 255;
	t->depth = a->w + b->w + c->w;
	t->ytop = ytop, t->ybot = ybot;
	t->alpha = alpha;
}

static Corner lerp_corner(const Corner *a, const Corner *b, f32 t)
{
	Corner c;
	c.x = lerpf(a->x, b->x, t), c.y = lerpf(a->y, b->y, t), c.z = lerpf(a->z, b->z, t), c.w = lerpf(a->w, b->w, t);
	return c;
}

/* Clips against the near plane (w >= znear), then sets up what's left. */
static void emit(R3 *r, const Corner *v, const f32 *rgb, u32 alpha, bool cull)
{
	f32 n = r->znear;
	int inside = (v[0].w >= n) + (v[1].w >= n) + (v[2].w >= n);
	if (inside == 3) {
		setup(r, &v[0], &v[1], &v[2], rgb, alpha, cull);
		return;
	}
	if (inside == 0)
		return;
	Corner out[4];
	int k = 0;
	for (int i = 0; i < 3; i++) {
		const Corner *a = &v[i], *b = &v[(i + 1) % 3];
		bool ain = a->w >= n, bin = b->w >= n;
		if (ain)
			out[k++] = *a;
		if (ain != bin)
			out[k++] = lerp_corner(a, b, (n - a->w) / (b->w - a->w));
	}
	setup(r, &out[0], &out[1], &out[2], rgb, alpha, cull);
	if (k == 4)
		setup(r, &out[0], &out[2], &out[3], rgb, alpha, cull);
}

void r3_mesh(R3 *r, const Mesh3 *m, Mat4 model)
{
	if (!m->ntri || !r->target)
		return;
	if (m->radius > 0) {
		Vec3 c = m4_point(model, m->center);
		f32 s = maxf(v3_len(m4_dir(model, v3(1, 0, 0))), maxf(v3_len(m4_dir(model, v3(0, 1, 0))), v3_len(m4_dir(model, v3(0, 0, 1)))));
		if (!r3_visible(r, c, m->radius * s))
			return;
	}
	if (m->npos > r->tmp_cap) {
		mem_free(r->tmp);
		r->tmp_cap = m->npos + 256;
		r->tmp = mem_alloc(r->tmp_cap * sizeof *r->tmp);
	}
	Mat4 mvp = m4_mul(r->vp, model);
	const f32 *k = mvp.m;
	for (u32 i = 0; i < m->npos; i++) {
		Vec3 p = m->pos[i];
		ClipVert *o = &r->tmp[i];
		o->x = k[0] * p.x + k[4] * p.y + k[8] * p.z + k[12];
		o->y = k[1] * p.x + k[5] * p.y + k[9] * p.z + k[13];
		o->z = k[2] * p.x + k[6] * p.y + k[10] * p.z + k[14];
		o->w = k[3] * p.x + k[7] * p.y + k[11] * p.z + k[15];
	}
	Vec3 eye_local = v3_sub(r->eye, v3(model.m[12], model.m[13], model.m[14]));
	for (u32 t = 0; t < m->ntri; t++) {
		const ClipVert *a = &r->tmp[m->idx[t * 3]], *b = &r->tmp[m->idx[t * 3 + 1]], *c = &r->tmp[m->idx[t * 3 + 2]];
		/* wholly outside one side of the view, or past the far plane */
		if ((a->x > a->w && b->x > b->w && c->x > c->w) || (a->x < -a->w && b->x < -b->w && c->x < -c->w) ||
		    (a->y > a->w && b->y > b->w && c->y > c->w) || (a->y < -a->w && b->y < -b->w && c->y < -c->w) ||
		    (a->w > r->zfar && b->w > r->zfar && c->w > r->zfar))
			continue;
		Color col = m->color[t];
		f32 base[3];
		rgb_of(col, base);
		f32 lit[3];
		if (m->unlit) {
			lit[0] = base[0], lit[1] = base[1], lit[2] = base[2];
		} else {
			Vec3 nrm = v3_norm(m4_dir(model, m->normal[t]));
			if (m->two_sided) {
				/* light the side that faces the eye */
				Vec3 p0 = m4_dir(model, m->pos[m->idx[t * 3]]);
				if (v3_dot(nrm, v3_sub(eye_local, p0)) < 0)
					nrm = v3_mul(nrm, -1);
			}
			f32 d = maxf(0, v3_dot(nrm, r->sun));
			f32 sky = 0.8f + 0.2f * nrm.y; /* faces turned up catch more of the sky */
			for (int i = 0; i < 3; i++)
				lit[i] = base[i] * (r->amb_rgb[i] * sky + r->sun_rgb[i] * d);
		}
		Corner v[3] = { *a, *b, *c };
		emit(r, v, lit, COLOR_A(col), !m->two_sided);
	}
}

/* ---- Filling bands ----------------------------------------------------------------------- */

static const u8 BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static inline u32 add_glow(u32 px, f32 g, const f32 *rgb, int dither)
{
	if (g <= 0.002f)
		return px;
	int add[3];
	for (int i = 0; i < 3; i++)
		add[i] = (int)(g * rgb[i] * 255 + (f32)dither / 16);
	int rr = MIN(255, (int)((px >> 16) & 255) + add[0]);
	int gg = MIN(255, (int)((px >> 8) & 255) + add[1]);
	int bb = MIN(255, (int)(px & 255) + add[2]);
	return 0xff000000u | (u32)rr << 16 | (u32)gg << 8 | (u32)bb;
}

static void sky_rows(R3 *r, int y0, int y1)
{
	/* the view ray through pixel (x, y): fwd + right * u + up * v */
	f32 ux = 2 * r->tan_y * r->aspect / (f32)r->w, vy = 2 * r->tan_y / (f32)r->h;
	Vec3 dx = v3_mul(r->right, ux);
	for (int y = y0; y < y1; y++) {
		u32 *row = canvas_row(r->target, y);
		f32 v = r->tan_y - ((f32)y + 0.5f) * vy;
		Vec3 d = v3_add(r->fwd, v3_add(v3_mul(r->right, -r->tan_y * r->aspect + 0.5f * ux), v3_mul(r->up, v)));
		for (int x = 0; x < r->w; x++, d = v3_add(d, dx)) {
			f32 il = 1 / sqrtf(v3_dot(d, d));
			f32 e = d.y * il;
			int dither = BAYER[y & 3][x & 3];
			int i = (int)(clampf(e, 0, 1) * 255 + (f32)dither / 16);
			u32 px = r->sky_lut[MIN(i, 255)];
			if (e < 0) /* below the horizon (seen past the ground's edge): a touch darker */
				px = (((px >> 1) & 0x7f7f7f) + ((px >> 2) & 0x3f3f3f) + ((px >> 3) & 0x1f1f1f)) | 0xff000000u;
			if (r->sun_cos <= 1) {
				f32 c = v3_dot(d, r->sun) * il;
				if (c > r->sun_cos) {
					f32 t = clampf((c - r->sun_cos) / (r->sun_edge - r->sun_cos), 0, 1);
					px = t >= 1 ? r->sun_px : color_premul(color_mix(color_unpremul(px), color_unpremul(r->sun_px), t));
				} else if (c > 0) {
					f32 c2 = c * c, c4 = c2 * c2, c8 = c4 * c4, c16 = c8 * c8, c64 = c16 * c16 * c16 * c16;
					px = add_glow(px, 0.55f * c64 + 0.22f * c8 * c2, r->glow_rgb, dither);
				}
			}
			row[x] = px;
		}
	}
}

#define SPAN 16

static inline u32 shade(i32 r, i32 g, i32 b)
{
	return 0xff000000u | (u32)CLAMP(r >> 16, 0, 255) << 16 | (u32)CLAMP(g >> 16, 0, 255) << 8 | (u32)CLAMP(b >> 16, 0, 255);
}

/* The fog amount for a distance, eased at both ends. */
static inline f32 fog_at(const R3 *r, f32 w)
{
	f32 f = clampf((w - r->fog_start) * r->fog_scale, 0, 1);
	return f * f * (3 - 2 * f);
}

static void fill(R3 *r, const STri *t, int by0, int by1)
{
	int ys = MAX(t->ytop, by0), ye = MIN(t->ybot, by1);
	int W = r->w;
	/* lit colour and the fog colour as 16.16, and their difference */
	i32 lit[3], dfog[3];
	for (int i = 0; i < 3; i++) {
		lit[i] = (i32)(t->rgb[i] * 65536);
		dfog[i] = (i32)(r->fog_rgb[i] * 255 * 65536) - lit[i];
	}
	for (int y = ys; y < ye; y++) {
		f32 py = (f32)y + 0.5f;
		f32 xa = t->x[0] + (py - t->y[0]) * t->s02;
		f32 xb = py < t->y[1] ? t->x[0] + (py - t->y[0]) * t->s01 : t->x[1] + (py - t->y[1]) * t->s12;
		f32 xl = minf(xa, xb), xr = maxf(xa, xb);
		int xs = (int)ceilf(xl - 0.5f), xe = (int)ceilf(xr - 0.5f);
		xs = MAX(xs, 0), xe = MIN(xe, W);
		if (xs >= xe)
			continue;
		f32 z = t->z0 + t->dzdx * ((f32)xs + 0.5f - t->x[0]) + t->dzdy * (py - t->y[0]);
		u32 *px = canvas_row(r->target, y);
		f32 *depth = r->depth + (usize)y * (usize)W;
		u32 a = t->alpha, ia = 255 - a;
		/* the fog at the start of each stretch of SPAN pixels, from the true distance */
		f32 f0 = fog_at(r, 1 / maxf(z, 1e-9f));
		for (int x = xs; x < xe;) {
			int n = MIN(SPAN, xe - x);
			f32 z1 = z + t->dzdx * (f32)n;
			f32 f1 = fog_at(r, 1 / maxf(z1, 1e-9f));
			i32 fq = (i32)(f0 * 65536), dfq = (i32)((f1 - f0) * 65536 / (f32)n);
			int end = x + n;
			for (; x < end; x++, z += t->dzdx, fq += dfq) {
				if (z <= depth[x])
					continue;
				u32 c = shade(lit[0] + (i32)(((i64)dfog[0] * fq) >> 16), lit[1] + (i32)(((i64)dfog[1] * fq) >> 16),
					      lit[2] + (i32)(((i64)dfog[2] * fq) >> 16));
				if (a >= 255) {
					depth[x] = z;
					px[x] = c;
				} else {
					u32 d = px[x];
					u32 R = (((c >> 16) & 255) * a + ((d >> 16) & 255) * ia) / 255;
					u32 G = (((c >> 8) & 255) * a + ((d >> 8) & 255) * ia) / 255;
					u32 B = ((c & 255) * a + (d & 255) * ia) / 255;
					px[x] = 0xff000000u | R << 16 | G << 8 | B;
				}
			}
			z = z1, f0 = f1;
		}
	}
}

static void band(R3 *r, int b)
{
	int y0 = b * BAND, y1 = MIN(r->h, y0 + BAND);
	memset(r->depth + (usize)y0 * (usize)r->w, 0, (usize)(y1 - y0) * (usize)r->w * sizeof(f32));
	if (r->sky)
		sky_rows(r, y0, y1);
	for (u32 i = 0; i < r->solid.len; i++) {
		const STri *t = &r->solid.data[i];
		if (t->ytop < y1 && t->ybot > y0)
			fill(r, t, y0, y1);
	}
	for (u32 i = 0; i < r->glass.len; i++) {
		const STri *t = &r->glass.data[i];
		if (t->ytop < y1 && t->ybot > y0)
			fill(r, t, y0, y1);
	}
}

static void run_bands(R3 *r)
{
	int nbands = (r->h + BAND - 1) / BAND;
	for (;;) {
		int b = atomic_add(&r->next_band, 1) - 1;
		if (b >= nbands)
			break;
		band(r, b);
	}
}

static void worker(void *arg)
{
	R3 *r = arg;
	int seen = 0;
	for (;;) {
		int g;
		while ((g = atomic_load(&r->gen)) == seen)
			futex_wait(&r->gen, seen, -1);
		seen = g;
		if (atomic_load(&r->quit))
			break;
		run_bands(r);
		if (atomic_add(&r->done, 1) == r->nworkers)
			futex_wake(&r->done, 1);
	}
	atomic_add(&r->alive, -1);
	futex_wake(&r->alive, 1);
}

static int far_first(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	f32 da = ((const STri *)a)->depth, db = ((const STri *)b)->depth;
	return da > db ? -1 : da < db ? 1 : 0;
}

void r3_end(R3 *r)
{
	if (!r->target)
		return;
	if (r->glass.len > 1)
		sort(r->glass.data, r->glass.len, sizeof *r->glass.data, far_first, NULL);
	r->drawn = r->solid.len + r->glass.len;
	if (!r->nworkers && !r->quit) {
		int want = MIN(MAX_WORKERS, cpu_count() - 1);
		for (int i = 0; i < want; i++) {
			atomic_add(&r->alive, 1);
			if (thread_spawn(worker, r) < 0) {
				atomic_add(&r->alive, -1);
				break;
			}
			r->nworkers++;
		}
		if (!r->nworkers)
			r->quit = 1; /* don't try again */
	}
	atomic_store(&r->next_band, 0);
	atomic_store(&r->done, 0);
	if (r->nworkers) {
		atomic_add(&r->gen, 1);
		futex_wake(&r->gen, r->nworkers);
	}
	run_bands(r);
	int d;
	while (r->nworkers && (d = atomic_load(&r->done)) < r->nworkers)
		futex_wait(&r->done, d, -1);
	r->target = NULL;
}

u32 r3_triangles(R3 *r)
{
	return r->drawn;
}
