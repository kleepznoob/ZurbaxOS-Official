/* Glide: fly a paper glider through golden rings over lakes and hills at
 * sunset. The land goes on forever, made up as you fly over it. Each ring
 * adds time and builds a streak; missing one ends the streak, and the
 * flight ends when time runs out or the glider meets the ground.
 *
 * The Arcade's 3D game, drawn with libzg's renderer. The picture is drawn
 * at a lower resolution when the processor can't keep up, so it stays
 * smooth anywhere. */
#include "arcade.h"

#define CELLS 16                  /* cells along a chunk of land, close up */
#define CHUNK 128.0f              /* a chunk's width in world units */
#define VIEW 440.0f               /* how far you can see */
#define NEAR_LOD 250.0f           /* closer chunks have full detail and trees */
#define MAX_CHUNKS 110
#define RINGS 6
#define RAIN 150
#define SPARKS 96
#define RING_R 6.0f

typedef struct {
	i32 cx, cz;
	int lod;                      /* 0 full detail, 1 half */
	Mesh3 *mesh;
	u32 used;                     /* the frame it was last drawn */
} Chunk;

typedef struct {
	Vec3 pos, axis;
} Ring;

typedef struct {
	Vec3 pos, vel;
	f32 life;
	Color color;
} Spark;

enum { TITLE, FLYING, OVER };

static struct {
	R3 *r3;
	Canvas *frame;                /* drawn at reduced resolution, then scaled up */
	Chunk chunks[MAX_CHUNKS];
	u32 frame_no;
	Mesh3 *glider, *ring;
	/* flight */
	int state;
	Vec3 pos, prev;
	f32 yaw, pitch, roll, speed;
	f32 steer, climb;             /* smoothed inputs */
	f32 boost;                    /* 0..1 */
	bool boosting;
	/* chase camera */
	Vec3 cam, cam_vel;
	bool cam_set;
	/* rings: a queue, the next one first */
	Ring rings[RINGS];
	int nrings;
	f32 course;
	Vec3 last_ring;
	/* score */
	int score, streak, best_streak, collected;
	f64 time_left, flown;
	bool new_best, crashed;
	f32 flash, shake;
	/* effects */
	Vec3 rain[RAIN];
	bool rain_set;
	Spark sparks[SPARKS];
	/* resolution */
	f32 res, avg_ms;
	Canvas *preview;
} G;

/* ---- The land ---------------------------------------------------------------------------- */

static u32 hash2(i32 x, i32 z)
{
	u32 h = (u32)x * 374761393u + (u32)z * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static f32 rnd2(i32 x, i32 z)
{
	return (f32)(hash2(x, z) & 0xffffff) / 16777215.0f;
}

static f32 noise(f32 x, f32 z)
{
	f32 fx = floorf(x), fz = floorf(z);
	i32 xi = (i32)fx, zi = (i32)fz;
	f32 u = x - fx, v = z - fz;
	u = u * u * (3 - 2 * u), v = v * v * (3 - 2 * v);
	f32 a = rnd2(xi, zi), b = rnd2(xi + 1, zi), c = rnd2(xi, zi + 1), d = rnd2(xi + 1, zi + 1);
	return lerpf(lerpf(a, b, u), lerpf(c, d, u), v);
}

/* Rolling hills around lakes, with mountain ranges here and there. */
static f32 height_at(f32 x, f32 z)
{
	f32 n = 0, amp = 1, sum = 0, f = 1 / 190.0f;
	for (int o = 0; o < 5; o++) {
		n += noise(x * f + 17.3f * (f32)o, z * f - 31.7f * (f32)o) * amp;
		sum += amp;
		amp *= 0.48f;
		f *= 2.07f;
	}
	n /= sum;
	f32 h = (n - 0.43f) * 110;
	f32 m = noise(x / 700 + 5.1f, z / 700 - 2.3f);
	if (m > 0.58f)
		h += (m - 0.58f) * 210 * n;
	/* beaches: flatter just around the water line */
	if (h > -3 && h < 3)
		h = h * h * h / 9;
	return h;
}

static Color land_color(f32 h, f32 ny, u32 jitter)
{
	const char *c;
	if (h < 0.6f)
		c = "#e2cd98"; /* sand */
	else if (h > 52)
		c = ny < 0.55f ? "#b9bcc4" : "#f1f3f6"; /* snow on the peaks */
	else if (ny < 0.6f)
		c = "#8f877c"; /* rock on steep slopes */
	else if (h < 12)
		c = "#86bb5e";
	else if (h < 30)
		c = "#5f9a4c";
	else
		c = "#7f8a5c"; /* high meadows */
	f32 j = ((f32)(jitter & 255) / 255 - 0.5f) * 0.09f;
	return color_lighten(color_hex(c), j);
}

/* Trees with as few faces as they can get away with. */
static void add_tree(Mesh3 *m, Vec3 p, u32 seed)
{
	f32 s = 0.8f + (f32)(seed & 63) / 100;
	Color trunk = color_hex("#7a5434"), leaf = color_lighten(color_hex(seed & 64 ? "#2f6e3f" : "#3d7d3a"), ((f32)((seed >> 8) & 31) / 31 - 0.5f) * 0.15f);
	/* a three-sided trunk */
	u32 t[6];
	for (int i = 0; i < 3; i++) {
		f32 a = (f32)i * 2.0944f + (f32)(seed & 7);
		t[i] = mesh3_vertex(m, v3(p.x + cosf(a) * 0.4f * s, p.y - 0.5f, p.z - sinf(a) * 0.4f * s));
		t[i + 3] = mesh3_vertex(m, v3(p.x + cosf(a) * 0.3f * s, p.y + 1.8f * s, p.z - sinf(a) * 0.3f * s));
	}
	for (int i = 0; i < 3; i++)
		mesh3_quad(m, t[i], t[(i + 1) % 3], t[(i + 1) % 3 + 3], t[i + 3], trunk);
	if (seed & 128) {
		/* a pine: two five-sided cones */
		for (int k = 0; k < 2; k++) {
			f32 base = p.y + (1.2f + (f32)k * 2.6f) * s, r = (2.4f - (f32)k * 0.8f) * s, h = (4.2f - (f32)k * 1.0f) * s;
			u32 tip = mesh3_vertex(m, v3(p.x, base + h, p.z)), ring[5];
			for (int i = 0; i < 5; i++) {
				f32 a = (f32)i * 1.2566f + (f32)k;
				ring[i] = mesh3_vertex(m, v3(p.x + cosf(a) * r, base, p.z - sinf(a) * r));
			}
			for (int i = 0; i < 5; i++)
				mesh3_tri(m, ring[i], ring[(i + 1) % 5], tip, color_lighten(leaf, (f32)k * 0.06f));
		}
	} else {
		/* a round tree: an eight-sided ball */
		f32 r = 2.5f * s, cy = p.y + 3.6f * s;
		u32 top = mesh3_vertex(m, v3(p.x, cy + r, p.z)), bot = mesh3_vertex(m, v3(p.x, cy - r * 0.8f, p.z)), ring[4];
		for (int i = 0; i < 4; i++) {
			f32 a = (f32)i * 1.5708f + (f32)(seed & 3);
			ring[i] = mesh3_vertex(m, v3(p.x + cosf(a) * r, cy, p.z - sinf(a) * r));
		}
		Color round = color_lighten(leaf, 0.12f);
		for (int i = 0; i < 4; i++) {
			mesh3_tri(m, ring[i], ring[(i + 1) % 4], top, round);
			mesh3_tri(m, ring[(i + 1) % 4], ring[i], bot, round);
		}
	}
}

static void build_chunk(Chunk *c)
{
	if (!c->mesh)
		c->mesh = mesh3_new();
	Mesh3 *m = c->mesh;
	mesh3_clear(m);
	int n = c->lod ? CELLS / 2 : CELLS;
	f32 cell = CHUNK / (f32)n, x0 = (f32)c->cx * CHUNK, z0 = (f32)c->cz * CHUNK;
	static f32 h[CELLS + 1][CELLS + 1];
	f32 lowest = 1e9f;
	for (int j = 0; j <= n; j++)
		for (int i = 0; i <= n; i++) {
			h[j][i] = height_at(x0 + (f32)i * cell, z0 + (f32)j * cell);
			lowest = minf(lowest, h[j][i]);
			mesh3_vertex(m, v3(x0 + (f32)i * cell, h[j][i], z0 + (f32)j * cell));
		}
	u32 row = (u32)n + 1;
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++) {
			u32 a = (u32)j * row + (u32)i, b = a + 1, cc = a + row, d = a + row + 1;
			i32 gx = c->cx * CELLS + i * (CELLS / n), gz = c->cz * CELLS + j * (CELLS / n);
			u32 hs = hash2(gx, gz);
			/* alternate the diagonal for a less regular look */
			u32 t1[3], t2[3];
			if (hs & 1)
				t1[0] = a, t1[1] = cc, t1[2] = b, t2[0] = b, t2[1] = cc, t2[2] = d;
			else
				t1[0] = a, t1[1] = cc, t1[2] = d, t2[0] = a, t2[1] = d, t2[2] = b;
			u32 *tris[2] = { t1, t2 };
			for (int k = 0; k < 2; k++) {
				Vec3 p0 = m->pos[tris[k][0]], p1 = m->pos[tris[k][1]], p2 = m->pos[tris[k][2]];
				Vec3 nrm = v3_norm(v3_cross(v3_sub(p1, p0), v3_sub(p2, p0)));
				f32 avg = (p0.y + p1.y + p2.y) / 3;
				mesh3_tri(m, tris[k][0], tris[k][1], tris[k][2], land_color(avg, nrm.y, hs >> (8 + k * 8)));
			}
		}
	/* skirts hanging from the edges hide cracks where detail levels meet */
	Color skirt = color_hex("#6d6a63");
	for (int i = 0; i < n; i++) {
		u32 e[4][2] = { { (u32)i, (u32)i + 1 },                                     /* north, facing -z */
				{ (u32)n * row + (u32)i + 1, (u32)n * row + (u32)i },          /* south, facing +z */
				{ (u32)(i + 1) * row, (u32)i * row },                          /* west, facing -x */
				{ (u32)i * row + (u32)n, (u32)(i + 1) * row + (u32)n } };      /* east, facing +x */
		for (int k = 0; k < 4; k++) {
			Vec3 p = m->pos[e[k][0]], q = m->pos[e[k][1]];
			u32 lp = mesh3_vertex(m, v3(p.x, p.y - 12, p.z)), lq = mesh3_vertex(m, v3(q.x, q.y - 12, q.z));
			mesh3_quad(m, e[k][0], e[k][1], lq, lp, skirt);
		}
	}
	/* water over the low parts */
	if (lowest < 0) {
		Color water = color_with_alpha(color_hex("#3f8fd0"), 196);
		u32 a = mesh3_vertex(m, v3(x0, 0, z0)), b = mesh3_vertex(m, v3(x0 + CHUNK, 0, z0));
		u32 cc = mesh3_vertex(m, v3(x0 + CHUNK, 0, z0 + CHUNK)), d = mesh3_vertex(m, v3(x0, 0, z0 + CHUNK));
		mesh3_quad(m, a, d, cc, b, water);
	}
	/* trees, close up only */
	if (!c->lod)
		for (int j = 0; j < CELLS; j++)
			for (int i = 0; i < CELLS; i++) {
				i32 gx = c->cx * CELLS + i, gz = c->cz * CELLS + j;
				u32 hs = hash2(gx * 7 + 3, gz * 13 + 5);
				if ((hs & 1023) > 34)
					continue;
				f32 tx = x0 + ((f32)i + 0.2f + (f32)((hs >> 10) & 255) / 420) * cell, tz = z0 + ((f32)j + 0.2f + (f32)((hs >> 18) & 255) / 420) * cell;
				f32 th = height_at(tx, tz);
				f32 slope = fabsf(height_at(tx + 2, tz) - th) + fabsf(height_at(tx, tz + 2) - th);
				if (th > 2 && th < 30 && slope < 2.2f)
					add_tree(m, v3(tx, th, tz), hs >> 4);
			}
	mesh3_finish(m);
}

static Chunk *chunk_at(i32 cx, i32 cz, int lod)
{
	Chunk *free_slot = NULL, *oldest = NULL;
	for (int i = 0; i < MAX_CHUNKS; i++) {
		Chunk *c = &G.chunks[i];
		if (c->mesh && c->cx == cx && c->cz == cz && c->lod == lod)
			return c;
		if (!c->mesh && !free_slot)
			free_slot = c;
		if (c->mesh && (!oldest || c->used < oldest->used))
			oldest = c;
	}
	Chunk *c = free_slot ? free_slot : oldest;
	c->cx = cx, c->cz = cz, c->lod = lod;
	build_chunk(c);
	return c;
}

/* ---- Models ------------------------------------------------------------------------------ */

static void make_models(void)
{
	if (G.glider)
		return;
	/* a paper glider: two wings angled up from a spine, gold along their
	 * trailing edges, and a golden keel */
	Mesh3 *g = mesh3_new();
	u32 nose = mesh3_vertex(g, v3(0, 0.05f, -3.0f)), tail = mesh3_vertex(g, v3(0, 0.1f, 1.6f));
	u32 lw = mesh3_vertex(g, v3(-3.1f, 0.75f, 1.55f)), rw = mesh3_vertex(g, v3(3.1f, 0.75f, 1.55f));
	u32 lm = mesh3_vertex(g, v3(-2.4f, 0.58f, 0.75f)), rm = mesh3_vertex(g, v3(2.4f, 0.58f, 0.75f));
	u32 lt = mesh3_vertex(g, v3(-0.8f, 0.2f, 1.6f)), rt = mesh3_vertex(g, v3(0.8f, 0.2f, 1.6f));
	u32 keel = mesh3_vertex(g, v3(0, -0.7f, 1.2f));
	Color paper = color_hex("#fffdf6"), paper2 = color_hex("#f3efe4"), gold = color_hex("#ffc23a");
	mesh3_tri(g, nose, lm, tail, paper);
	mesh3_tri(g, nose, tail, rm, paper2);
	mesh3_tri(g, lm, lw, lt, gold);
	mesh3_tri(g, lm, lt, tail, paper);
	mesh3_tri(g, rm, rt, rw, gold);
	mesh3_tri(g, rm, tail, rt, paper2);
	mesh3_tri(g, nose, keel, tail, gold);
	g->two_sided = true;
	mesh3_finish(g);
	G.glider = g;
	Mesh3 *r = mesh3_new();
	mesh3_torus(r, v3(0, 0, 0), RING_R, 0.75f, 18, 5, color_hex("#ffc83d"));
	r->unlit = true;
	mesh3_finish(r);
	G.ring = r;
}

/* ---- Rings ------------------------------------------------------------------------------- */

static void add_ring(void)
{
	G.course += (rand_f32() - 0.5f) * 0.8f;
	Vec3 dir = v3(-sinf(G.course), 0, -cosf(G.course));
	Vec3 p = v3_add(G.last_ring, v3_mul(dir, 70 + rand_f32() * 25));
	f32 ground = maxf(height_at(p.x, p.z), 0);
	/* clear of the hills on the way there, too */
	for (int k = 1; k <= 4; k++) {
		Vec3 q = v3_lerp(G.last_ring, p, (f32)k / 5);
		ground = maxf(ground, maxf(height_at(q.x, q.z), 0) - 4);
	}
	p.y = clampf(G.last_ring.y + (rand_f32() - 0.5f) * 18, ground + 10, ground + 34);
	Ring *r = &G.rings[G.nrings++];
	r->pos = p;
	r->axis = v3_norm(v3_sub(p, G.last_ring));
	G.last_ring = p;
}

static void drop_first_ring(void)
{
	memmove(&G.rings[0], &G.rings[1], (usize)(G.nrings - 1) * sizeof G.rings[0]);
	G.nrings--;
	add_ring();
}

static void burst(Vec3 at, Color color, int n, f32 speed)
{
	for (int i = 0; i < SPARKS && n > 0; i++) {
		Spark *s = &G.sparks[i];
		if (s->life > 0)
			continue;
		Vec3 d = v3_norm(v3(rand_f32() - 0.5f, rand_f32() - 0.5f, rand_f32() - 0.5f));
		s->pos = at;
		s->vel = v3_mul(d, speed * (0.4f + rand_f32()));
		s->life = 0.5f + rand_f32() * 0.5f;
		s->color = color;
		n--;
	}
}

/* ---- Flight ------------------------------------------------------------------------------ */

static Vec3 forward_of(f32 yaw, f32 pitch)
{
	return v3(-sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch));
}

static void start(void)
{
	R3 *r3 = G.r3;
	Canvas *frame = G.frame, *preview = G.preview;
	Chunk chunks[MAX_CHUNKS];
	memcpy(chunks, G.chunks, sizeof chunks);
	Mesh3 *glider = G.glider, *ring = G.ring;
	f32 res = G.res;
	memset(&G, 0, sizeof G);
	G.r3 = r3, G.frame = frame, G.preview = preview, G.glider = glider, G.ring = ring;
	memcpy(G.chunks, chunks, sizeof chunks);
	G.res = res > 0 ? res : 1;
	G.state = TITLE;
	G.pos = v3(0, maxf(height_at(0, 0), 0) + 26, 0);
	G.prev = G.pos;
	G.speed = 34;
	G.boost = 1;
	G.time_left = 45;
	G.last_ring = G.pos;
	for (int i = 0; i < RINGS; i++)
		add_ring();
}

static void crash(void)
{
	G.state = OVER;
	G.crashed = true;
	G.shake = 1;
	burst(G.pos, color_hex("#fff4d6"), 50, 18);
	sound_play("hit");
	sound_play("lose");
	G.new_best = submit_score("glide", G.score);
}

/* One step of flight from steering (-1 left .. 1 right), climb (-1 dive ..
 * 1 climb) and boost. The demo on the title screen flies the same way but
 * can't crash or run out of time, and makes no sound. */
static void step(f32 dt, f32 sx, f32 sy, bool want_boost, bool demo)
{
	f32 k = 1 - expf(-dt * 7);
	G.steer += (sx - G.steer) * k;
	G.climb += (sy - G.climb) * k;
	G.boosting = want_boost && G.boost > 0.02f;
	if (G.boosting)
		G.boost = maxf(0, G.boost - dt * 0.28f);
	else
		G.boost = minf(1, G.boost + dt * 0.035f);

	/* bank to turn (right wing down turns right); pitch follows the stick;
	 * diving gains speed, climbing costs it */
	f32 ek = 1 - expf(-dt * 3.5f);
	G.roll += (-G.steer * 0.85f - G.roll) * ek;
	G.pitch += (G.climb * 0.5f - G.pitch) * ek;
	if (G.pos.y > 150)
		G.pitch = minf(G.pitch, -0.1f); /* a gentle ceiling */
	G.yaw += sinf(G.roll) * 1.25f * dt;
	f32 base = G.boosting ? 64 : 36;
	G.speed += (base - G.speed) * (1 - expf(-dt * 1.2f)) - sinf(G.pitch) * 14 * dt;
	G.speed = clampf(G.speed, 22, 80);
	G.prev = G.pos;
	G.pos = v3_add(G.pos, v3_mul(forward_of(G.yaw, G.pitch), G.speed * dt));
	G.flown += G.speed * dt;

	/* the next ring: through it, or past it */
	if (G.nrings) {
		Ring *r = &G.rings[0];
		f32 before = v3_dot(v3_sub(G.prev, r->pos), r->axis), after = v3_dot(v3_sub(G.pos, r->pos), r->axis);
		if (before < 0 && after >= 0) {
			f32 t = before / (before - after);
			Vec3 at = v3_lerp(G.prev, G.pos, t);
			if (v3_len(v3_sub(at, r->pos)) < RING_R - 0.2f) {
				G.streak++;
				G.best_streak = MAX(G.best_streak, G.streak);
				G.collected++;
				burst(r->pos, color_hex("#ffd45a"), 40, 14);
				if (!demo) {
					G.score += 100 * MIN(G.streak, 5);
					G.time_left += maxf(2.5f, 4.5f - (f32)G.collected * 0.05f);
					G.boost = minf(1, G.boost + 0.3f);
					G.flash = 1;
					sound_play("coin");
				}
			} else {
				G.streak = 0;
				if (!demo)
					sound_play("pop");
			}
			drop_first_ring();
		} else if (v3_len(v3_sub(G.pos, r->pos)) > 260) {
			G.streak = 0; /* wandered off: lead on from here */
			G.last_ring = v3_add(G.pos, v3_mul(forward_of(G.yaw, 0), 10));
			G.course = G.yaw;
			G.nrings = 0;
			for (int i = 0; i < RINGS; i++)
				add_ring();
		}
	}

	f32 ground = maxf(height_at(G.pos.x, G.pos.z), 0);
	if (demo) {
		G.pos.y = maxf(G.pos.y, ground + 2);
		return;
	}
	G.time_left -= dt;
	if (G.time_left <= 0) {
		G.time_left = 0;
		G.state = OVER;
		G.new_best = submit_score("glide", G.score);
		sound_play("lose");
		return;
	}
	if (G.pos.y < ground + 0.8f)
		crash();
}

static void fly(Ui *ui, f32 dt)
{
	/* the left stick or D-pad, arrows or WASD; boost on A, a trigger, Space or Shift */
	f32 sx = ui_pad_axis(ui, PAD_LX), sy = -ui_pad_axis(ui, PAD_LY);
	if (pad_held(ui, PAD_LEFT) || key_down(KEY_LEFT) || key_down(KEY_A))
		sx = -1;
	if (pad_held(ui, PAD_RIGHT) || key_down(KEY_RIGHT) || key_down(KEY_D))
		sx = 1;
	if (pad_held(ui, PAD_UP) || key_down(KEY_UP) || key_down(KEY_W))
		sy = 1;
	if (pad_held(ui, PAD_DOWN) || key_down(KEY_DOWN) || key_down(KEY_S))
		sy = -1;
	bool boost = ui_pad_down(ui, PAD_A) || ui_pad_axis(ui, PAD_RTRIGGER) > 0.3f || key_down(KEY_SPACE) || key_down(KEY_LEFTSHIFT);
	step(dt, sx, sy, boost, false);
}

/* On the title screen the glider flies itself through the rings. */
static void attract(f32 dt)
{
	f32 sx = 0, sy = 0;
	if (G.nrings) {
		Vec3 to = v3_sub(G.rings[0].pos, G.pos);
		f32 err = atan2f(-to.x, -to.z) - G.yaw;
		while (err > (f32)PI)
			err -= 2 * (f32)PI;
		while (err < -(f32)PI)
			err += 2 * (f32)PI;
		sx = clampf(-err * 2.5f, -1, 1);
		sy = clampf(to.y * 0.09f, -1, 1);
	}
	step(dt, sx, sy, false, true);
}

/* ---- Drawing ----------------------------------------------------------------------------- */

static void blend_px(Canvas *c, int x, int y, Color col, f32 a)
{
	if (x < 0 || y < 0 || x >= c->width || y >= c->height || a <= 0)
		return;
	u32 *p = &canvas_row(c, y)[x];
	u32 ia = (u32)(clampf(a, 0, 1) * 255), na = 255 - ia;
	u32 r = (COLOR_R(col) * ia + ((*p >> 16) & 255) * na) / 255, g = (COLOR_G(col) * ia + ((*p >> 8) & 255) * na) / 255,
	    b = (COLOR_B(col) * ia + (*p & 255) * na) / 255;
	*p = 0xff000000u | r << 16 | g << 8 | b;
}

/* A short streak between two world points, hidden behind the scene. */
static void streak(Canvas *c, Vec3 a, Vec3 b, Color col, f32 alpha)
{
	Vec2 pa, pb;
	if (!r3_project(G.r3, a, &pa, NULL) || !r3_project(G.r3, b, &pb, NULL))
		return;
	f32 dist = v3_len(v3_sub(a, G.cam));
	int steps = (int)maxf(fabsf(pb.x - pa.x), fabsf(pb.y - pa.y)) + 1;
	steps = MIN(steps, 64);
	for (int i = 0; i <= steps; i++) {
		f32 t = (f32)i / (f32)steps;
		int x = (int)lerpf(pa.x, pb.x, t), y = (int)lerpf(pa.y, pb.y, t);
		if (r3_distance_at(G.r3, x, y) < dist)
			continue;
		blend_px(c, x, y, col, alpha * (0.4f + 0.6f * t));
	}
}

static void draw_effects(Canvas *c, f32 dt)
{
	/* golden rain falling through the evening light around the camera */
	if (!G.rain_set) {
		for (int i = 0; i < RAIN; i++)
			G.rain[i] = v3_add(G.cam, v3((rand_f32() - 0.5f) * 120, (rand_f32() - 0.5f) * 70, (rand_f32() - 0.5f) * 120));
		G.rain_set = true;
	}
	Vec3 fall = v3(1.5f, -16, 0.5f);
	for (int i = 0; i < RAIN; i++) {
		Vec3 *p = &G.rain[i];
		*p = v3_add(*p, v3_mul(fall, dt));
		Vec3 d = v3_sub(*p, G.cam);
		/* keep them in a box around the eye */
		if (d.x > 60) p->x -= 120;
		if (d.x < -60) p->x += 120;
		if (d.z > 60) p->z -= 120;
		if (d.z < -60) p->z += 120;
		if (d.y < -35) p->y += 70;
		if (d.y > 35) p->y -= 70;
		f32 dist = v3_len(d);
		if (dist < 3 || dist > 60)
			continue;
		f32 a = minf(1, (1 - dist / 60) * 1.3f);
		streak(c, *p, v3_add(*p, v3_mul(fall, 0.09f)), color_hex("#ffe3a0"), a);
	}
	for (int i = 0; i < SPARKS; i++) {
		Spark *s = &G.sparks[i];
		if (s->life <= 0)
			continue;
		s->life -= dt;
		s->vel = v3_add(v3_mul(s->vel, 1 - dt * 1.5f), v3(0, -9 * dt, 0));
		Vec3 next = v3_add(s->pos, v3_mul(s->vel, dt));
		streak(c, s->pos, v3_add(next, v3_mul(s->vel, 0.03f)), s->color, minf(1, s->life * 2));
		s->pos = next;
	}
}

/* Lights, sky and land for a camera, plus the rings and the glider. */
static void draw_world(Canvas *c, Vec3 eye, Vec3 look, Vec3 up, f32 fov, bool with_glider)
{
	R3 *r = G.r3;
	G.frame_no++;
	r3_begin(r, c);
	r3_camera(r, eye, look, up, fov, 0.3f, VIEW);
	Color horizon = color_hex("#f4c6a0");
	r3_light(r, v3(0.38f, 0.16f, -1), color_hex("#ffdcb0"), color_hex("#8790c0"));
	r3_fog(r, horizon, 140, VIEW);
	r3_sky(r, color_hex("#5d7fc4"), horizon, 0.05f, color_hex("#ffd59a"));
	i32 ccx = (i32)floorf(eye.x / CHUNK), ccz = (i32)floorf(eye.z / CHUNK);
	int reach = (int)ceilf(VIEW / CHUNK);
	for (i32 dz = -reach; dz <= reach; dz++)
		for (i32 dx = -reach; dx <= reach; dx++) {
			i32 cx = ccx + dx, cz = ccz + dz;
			f32 mx = ((f32)cx + 0.5f) * CHUNK - eye.x, mz = ((f32)cz + 0.5f) * CHUNK - eye.z;
			f32 d = sqrtf(mx * mx + mz * mz);
			if (d > VIEW + CHUNK * 0.72f)
				continue;
			/* skip what's behind before building anything */
			if (!r3_visible(r, v3(mx + eye.x, 20, mz + eye.z), CHUNK * 0.75f + 40))
				continue;
			Chunk *ch = chunk_at(cx, cz, d < NEAR_LOD ? 0 : 1);
			ch->used = G.frame_no;
			r3_mesh(r, ch->mesh, m4_identity());
		}
	for (int i = 0; i < G.nrings; i++) {
		Ring *ring = &G.rings[i];
		Vec3 ax = ring->axis, right = v3_norm(v3_cross(ax, v3(0, 1, 0)));
		Vec3 fwd = v3_mul(v3_cross(right, ax), -1);
		f32 pulse = i == 0 ? 1 + 0.06f * sinf((f32)G.frame_no * 0.15f) : 1;
		Mat4 m = m4_mul(m4_orient(right, ax, fwd, ring->pos), m4_scale(v3(pulse, pulse, pulse)));
		r3_mesh(r, G.ring, m);
	}
	if (with_glider && !G.crashed) {
		Vec3 f = forward_of(G.yaw, G.pitch);
		Vec3 flat_right = v3_norm(v3_cross(f, v3(0, 1, 0)));
		Vec3 up0 = v3_cross(flat_right, f);
		Vec3 right = v3_add(v3_mul(flat_right, cosf(G.roll)), v3_mul(up0, sinf(G.roll)));
		Vec3 upv = v3_cross(right, f);
		r3_mesh(r, G.glider, m4_orient(right, upv, f, G.pos));
	}
	r3_end(r);
}

static void chase_camera(f32 dt, Vec3 *eye, Vec3 *look, Vec3 *up)
{
	Vec3 f = forward_of(G.yaw, G.pitch * 0.6f);
	Vec3 want = v3_add(v3_sub(G.pos, v3_mul(f, 11)), v3(0, 4.2f, 0));
	if (!G.cam_set || dt <= 0) {
		G.cam = want, G.cam_set = true;
	} else {
		f32 k = 1 - expf(-dt * 6);
		G.cam = v3_lerp(G.cam, want, k);
	}
	/* never inside a hill */
	G.cam.y = maxf(G.cam.y, maxf(height_at(G.cam.x, G.cam.z), 0) + 1.5f);
	Vec3 eye_pos = G.cam;
	if (G.shake > 0)
		eye_pos = v3_add(eye_pos, v3(sinf((f32)G.frame_no * 1.7f), cosf((f32)G.frame_no * 2.3f), 0));
	*eye = eye_pos;
	*look = v3_add(G.pos, v3_mul(forward_of(G.yaw, G.pitch), 7));
	Vec3 right = v3_norm(v3_cross(v3_sub(*look, *eye), v3(0, 1, 0)));
	*up = v3_norm(v3_add(v3(0, 1, 0), v3_mul(right, -sinf(G.roll) * 0.35f)));
}

/* Nearest-pixel scaling, for showing the reduced-resolution picture. */
static void scale_up(Canvas *dst, const Canvas *src)
{
	static i32 xs[8192];
	int w = MIN(dst->width, 8192);
	for (int x = 0; x < w; x++)
		xs[x] = (i32)((i64)x * src->width / dst->width);
	for (int y = 0; y < dst->height; y++) {
		const u32 *s = canvas_row(src, (int)((i64)y * src->height / dst->height));
		u32 *d = canvas_row(dst, y);
		for (int x = 0; x < w; x++)
			d[x] = s[xs[x]];
	}
}

static void hud(Ui *ui, Rect r)
{
	char t[64];
	Font big = ui_font(ui, 30, 800), mid = ui_font(ui, 15, 700), small = ui_font(ui, 12, 700);
	Color shadow = color_hex("#00000050");
	/* time, big in the middle */
	int secs = (int)ceilf((f32)G.time_left);
	fmt(t, sizeof t, "%d", secs);
	Color tc = G.time_left < 8 && G.state == FLYING && ((int)(G.time_left * 4) & 1) ? color_hex("#ff8a7a") : COLOR_WHITE;
	ui_text_in(ui, big, rect(r.x, r.y + 13, r.w, 40), t, shadow, ALIGN_CENTER);
	ui_text_in(ui, big, rect(r.x, r.y + 12, r.w, 40), t, tc, ALIGN_CENTER);
	ui_text_in(ui, small, rect(r.x, r.y + 50, r.w, 16), "SECONDS", color_hex("#ffffffb0"), ALIGN_CENTER);
	/* score and streak */
	fmt(t, sizeof t, "%d", G.score);
	ui_text_in(ui, small, rect(r.x + 18, r.y + 14, 200, 16), "SCORE", color_hex("#ffffffb0"), ALIGN_START);
	ui_text_in(ui, ui_font(ui, 24, 800), rect(r.x + 18, r.y + 30, 200, 30), t, COLOR_WHITE, ALIGN_START);
	if (G.streak > 1) {
		fmt(t, sizeof t, "x%d streak", MIN(G.streak, 5));
		ui_text_in(ui, mid, rect(r.x + 18, r.y + 60, 200, 20), t, color_hex("#ffd45a"), ALIGN_START);
	}
	fmt(t, sizeof t, "%d", MAX(best_score("glide"), G.score));
	ui_text_in(ui, small, rect(r.x + r.w - 218, r.y + 14, 200, 16), "BEST", color_hex("#ffffffb0"), ALIGN_END);
	ui_text_in(ui, ui_font(ui, 24, 800), rect(r.x + r.w - 218, r.y + 30, 200, 30), t, COLOR_WHITE, ALIGN_END);
	/* boost */
	Rect bar = rect(r.x + 18, r.y + r.h - 30, 160, 10);
	ui_text_in(ui, small, rect(bar.x, bar.y - 20, 160, 16), "BOOST", color_hex("#ffffffb0"), ALIGN_START);
	ui_round(ui, bar, 5, color_hex("#00000055"));
	ui_gradient(ui, rect(bar.x, bar.y, maxf(bar.h, bar.w * G.boost), bar.h), 5, color_hex("#ffe08a"), color_hex("#ffb330"));
	/* where the next ring is, when it's off the screen */
	if (G.nrings && G.state == FLYING) {
		Vec2 p;
		bool on = r3_project(G.r3, G.rings[0].pos, &p, NULL);
		f32 sx = (f32)G.frame->width, sy = (f32)G.frame->height;
		if (!on || p.x < 0 || p.y < 0 || p.x > sx || p.y > sy) {
			Vec3 d = v3_sub(G.rings[0].pos, G.cam);
			Vec3 f = v3_norm(v3_sub(v3_add(G.pos, v3_mul(forward_of(G.yaw, G.pitch), 7)), G.cam));
			Vec3 right = v3_norm(v3_cross(f, v3(0, 1, 0))), up = v3_cross(right, f);
			f32 a = atan2f(v3_dot(d, up), v3_dot(d, right));
			f32 cx = r.x + r.w / 2 + cosf(a) * (r.w / 2 - 40), cy = r.y + r.h / 2 - sinf(a) * (r.h / 2 - 40);
			ui_circle(ui, cx, cy, 16, color_hex("#00000050"));
			Path pa = { 0 };
			f32 ca = cosf(a), sa = -sinf(a);
			path_move(&pa, cx + ca * 11, cy + sa * 11);
			path_line(&pa, cx - ca * 6 - sa * 8, cy - sa * 6 + ca * 8);
			path_line(&pa, cx - ca * 6 + sa * 8, cy - sa * 6 - ca * 8);
			path_close(&pa);
			ui_path_fill(ui, &pa, color_hex("#ffd45a"));
			path_free(&pa);
		}
	}
	if (G.flash > 0)
		ui_fill(ui, r, color_with_alpha(color_hex("#ffe7a0"), (u8)(G.flash * 50)));
}

static void frame(Ui *ui, Rect r)
{
	ui_pad_raw(ui);
	make_models();
	if (!G.r3) {
		G.r3 = r3_new();
		start();
	}
	f32 dt = minf(ui->dt, 0.05f);
	bool running = !game_paused;
	if (running) {
		if (G.state == TITLE)
			attract(dt);
		else if (G.state == FLYING)
			fly(ui, dt);
		G.flash = maxf(0, G.flash - dt * 3);
		G.shake = maxf(0, G.shake - dt * 2);
	}
	/* the picture: the window's own pixels, or a smaller one scaled up */
	Canvas view = canvas_view(ui->canvas, ui_to_device(ui, r));
	if (view.width < 8 || view.height < 8)
		return;
	int rw = MAX(8, (int)((f32)view.width * G.res)), rh = MAX(8, (int)((f32)view.height * G.res));
	Canvas *target = &view;
	if (G.res < 0.99f) {
		if (!G.frame || G.frame->width != rw || G.frame->height != rh) {
			canvas_free(G.frame);
			G.frame = canvas_new(rw, rh);
		}
		target = G.frame;
	} else if (!G.frame || G.frame->width != view.width || G.frame->height != view.height) {
		canvas_free(G.frame);
		G.frame = canvas_new(view.width, view.height); /* kept for its size, used by the hud */
	}
	u64 t0 = time_mono_ns();
	Vec3 eye, look, up;
	chase_camera(running ? dt : 0, &eye, &look, &up);
	f32 fov = 1.05f + (G.speed - 36) * 0.004f;
	draw_world(target, eye, look, up, fov, true);
	draw_effects(target, running ? dt : 0);
	if (target != &view)
		scale_up(&view, target);
	/* adjust the resolution to keep up: aim for well under a frame */
	f32 ms = (f32)(time_mono_ns() - t0) / 1e6f;
	G.avg_ms = G.avg_ms ? G.avg_ms * 0.9f + ms * 0.1f : ms;
	if (G.avg_ms > 15 && G.res > 0.4f)
		G.res = maxf(0.4f, G.res * 0.97f);
	else if (G.avg_ms < 8 && G.res < 1)
		G.res = minf(1, G.res * 1.02f);

	if (G.state != TITLE)
		hud(ui, r);
	if (G.state == TITLE) {
		Rect card = center_in(r, minf(r.w - 40, 420), 190);
		ui_round(ui, card, 18, color_hex("#10142080"));
		Rect c = pad(card, 22);
		ui_text_in(ui, ui_font(ui, 40, 800), cut_top(&c, 50), "Glide", COLOR_WHITE, ALIGN_CENTER);
		ui_text_wrapped(ui, ui_font(ui, 14, 500), cut_top(&c, 44), "Fly through the golden rings. Each one adds time; miss one and your streak ends.",
				color_hex("#ffffffc0"), ALIGN_CENTER);
		cut_top(&c, 10);
		bool go = ui_button_suggested(ui, center_in(cut_top(&c, 40), 160, 40), "Take Off");
		if (go || key_hit(ui, KEY_ENTER) || key_hit(ui, KEY_SPACE) || pad_hit(ui, PAD_A) || pad_hit(ui, PAD_START)) {
			Vec3 keep_cam = G.cam;
			start();
			G.cam = keep_cam, G.cam_set = true;
			G.state = FLYING;
			G.yaw = 0;
			sound_play("click");
		}
	} else if (G.state == OVER) {
		char sub[128];
		if (G.new_best)
			fmt(sub, sizeof sub, "A new best: %d points, %d rings.", G.score, G.collected);
		else
			fmt(sub, sizeof sub, "%d points, %d rings, best streak %d.", G.score, G.collected, G.best_streak);
		if (game_overlay(ui, r, G.crashed ? "Down You Go" : "Time's Up", sub, "Fly Again")) {
			start();
			G.state = FLYING;
		}
	} else if (game_paused) {
		if (game_overlay(ui, r, "Paused", "The glider hangs in the sunset.", "Resume"))
			game_paused = false;
	}
}

/* The hub card: a still of the land, made once. */
static void preview(Ui *ui, Rect r)
{
	make_models();
	if (!G.r3) {
		G.r3 = r3_new();
		start();
	}
	Rect d = ui_to_device(ui, r);
	int w = (int)d.w, h = (int)d.h;
	if (w < 8 || h < 8)
		return;
	if (!G.preview || G.preview->width != w || G.preview->height != h) {
		canvas_free(G.preview);
		G.preview = canvas_new(w, h);
		Vec3 eye = v3(0, maxf(height_at(0, 30), 0) + 30, 30);
		f32 yaw = G.yaw, pitch = G.pitch, roll = G.roll;
		Vec3 pos = G.pos;
		G.pos = v3(2, eye.y - 2.5f, 19), G.yaw = 0.12f, G.pitch = 0.05f, G.roll = -0.3f;
		draw_world(G.preview, eye, v3(10, eye.y - 14, -120), v3(0, 1, 0), 0.9f, true);
		G.pos = pos, G.yaw = yaw, G.pitch = pitch, G.roll = roll;
	}
	ui_image(ui, r, G.preview, 10, true);
}

const Game GAME_GLIDE = { "glide", "Glide", "Fly through golden rings at sunset.", "← → turn   ↑ ↓ climb and dive   Space boost",
			  "Left stick to fly   A or RT boost", start, frame, preview, 0xffd9873a, 0xff5a3270 };
