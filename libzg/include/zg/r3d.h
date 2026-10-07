/*
 * libzg 3D — a small software renderer for games and previews.
 *
 * Scenes are made of flat-coloured triangles (the low-poly look): each one
 * is lit by a sun and an ambient light, fades into distance fog, and is
 * depth-tested against the others. Drawing calls only collect triangles;
 * r3_end rasterizes them on every processor at once, in horizontal bands.
 *
 * Coordinates are right-handed with +y up. Triangles face the viewer when
 * their corners run counter-clockwise, and the far side isn't drawn.
 */
#pragma once

#include "canvas.h"

/* ---- Vectors and matrices --------------------------------------------------------------- */

typedef struct {
	f32 x, y, z;
} Vec3;

/* Column-major 4x4: m[col * 4 + row]. */
typedef struct {
	f32 m[16];
} Mat4;

static inline Vec3 v3(f32 x, f32 y, f32 z) { return (Vec3){ x, y, z }; }
static inline Vec3 v3_add(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 v3_sub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 v3_mul(Vec3 a, f32 s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline f32 v3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 v3_cross(Vec3 a, Vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline f32 v3_len(Vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline Vec3 v3_lerp(Vec3 a, Vec3 b, f32 t) { return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t); }
Vec3 v3_norm(Vec3 a);                              /* unit length (zero stays zero) */

Mat4 m4_identity(void);
Mat4 m4_mul(Mat4 a, Mat4 b);                       /* a·b: applies b first, then a */
Mat4 m4_translate(Vec3 t);
Mat4 m4_scale(Vec3 s);
Mat4 m4_rotate(Vec3 axis, f32 radians);
/* Places an object: its +x, +y and -z (forward) along the given axes, at pos. */
Mat4 m4_orient(Vec3 right, Vec3 up, Vec3 forward, Vec3 pos);
Mat4 m4_look_at(Vec3 eye, Vec3 target, Vec3 up);
Mat4 m4_perspective(f32 fovy_radians, f32 aspect, f32 znear, f32 zfar);
Vec3 m4_point(Mat4 m, Vec3 p);                     /* transforms a position */
Vec3 m4_dir(Mat4 m, Vec3 d);                       /* transforms a direction (no translation) */

/* ---- Meshes ------------------------------------------------------------------------------ */

typedef struct {
	Vec3 *pos;
	u32 npos, cap_pos;
	u32 *idx;         /* three per triangle */
	Color *color;     /* one per triangle; alpha below 255 is see-through */
	Vec3 *normal;     /* one per triangle, from mesh3_finish */
	u32 ntri, cap_tri;
	bool unlit;       /* glows in its own colour (no sun or shade) */
	bool two_sided;   /* both faces are drawn (thin things: wings, leaves) */
	Vec3 center;      /* bounding sphere, from mesh3_finish */
	f32 radius;
} Mesh3;

Mesh3 *mesh3_new(void);
void mesh3_free(Mesh3 *m);
void mesh3_clear(Mesh3 *m);
u32 mesh3_vertex(Mesh3 *m, Vec3 p);
void mesh3_tri(Mesh3 *m, u32 a, u32 b, u32 c, Color color);
void mesh3_quad(Mesh3 *m, u32 a, u32 b, u32 c, u32 d, Color color);  /* a b c d counter-clockwise */
void mesh3_finish(Mesh3 *m);                       /* normals and bounds, after the last change */

/* Shapes, added to a mesh. Sides are the number of facets around. */
void mesh3_box(Mesh3 *m, Vec3 center, Vec3 size, Color color);
/* A cone (top_radius 0) or cylinder standing on base, height along +y. */
void mesh3_cone(Mesh3 *m, Vec3 base, f32 radius, f32 top_radius, f32 height, int sides, Color color);
/* A ring lying in the xz-plane (axis +y): ring radius R, tube radius r. */
void mesh3_torus(Mesh3 *m, Vec3 center, f32 R, f32 r, int segments, int sides, Color color);
void mesh3_sphere(Mesh3 *m, Vec3 center, f32 radius, int rings, int sides, Color color);

/* ---- Renderer ---------------------------------------------------------------------------- */

typedef struct R3 R3;

R3 *r3_new(void);
void r3_free(R3 *r);
/* Starts a frame drawn into target (all of it). */
void r3_begin(R3 *r, Canvas *target);
void r3_camera(R3 *r, Vec3 eye, Vec3 target, Vec3 up, f32 fovy_radians, f32 znear, f32 zfar);
void r3_light(R3 *r, Vec3 toward_sun, Color sun, Color ambient);
/* Things fade into the fog colour between start and end (distance from the eye). */
void r3_fog(R3 *r, Color fog, f32 start, f32 end);
/* Fills the background: zenith above, horizon at eye level, and a sun disc
 * with a glow in the direction of the light (sun_size 0 for none). */
void r3_sky(R3 *r, Color zenith, Color horizon, f32 sun_size, Color sun);
/* Whether a sphere is (partly) inside the view, for skipping whole objects. */
bool r3_visible(R3 *r, Vec3 center, f32 radius);
void r3_mesh(R3 *r, const Mesh3 *m, Mat4 model);
/* Where a point lands on the target; false if it's behind the eye. *scale
 * is pixels per world unit at that distance. */
bool r3_project(R3 *r, Vec3 p, Vec2 *out, f32 *scale);
/* Rasterizes everything collected since r3_begin. */
void r3_end(R3 *r);
u32 r3_triangles(R3 *r);                           /* drawn in the last frame */
/* How far away the scene is at a pixel of the last frame (huge for sky). */
f32 r3_distance_at(R3 *r, int x, int y);
