/* Tests for the 3D renderer: facing, depth, clipping, fog and the sky, plus
 * a sample scene saved for a look (build/tests/r3d.png). */
#include "zg.h"

static int checks, failed;
#define CHECK(c)                                                         \
	do {                                                             \
		checks++;                                                \
		if (!(c)) {                                              \
			failed++;                                        \
			eprint("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		}                                                        \
	} while (0)

static Color at(Canvas *c, int x, int y)
{
	return color_unpremul(canvas_row(c, y)[x]);
}

static bool near_color(Color a, Color b, int tol)
{
	return ABS((int)COLOR_R(a) - (int)COLOR_R(b)) <= tol && ABS((int)COLOR_G(a) - (int)COLOR_G(b)) <= tol &&
	       ABS((int)COLOR_B(a) - (int)COLOR_B(b)) <= tol;
}

static void test_math(void)
{
	Mat4 r = m4_rotate(v3(0, 1, 0), (f32)PI / 2);
	Vec3 p = m4_point(r, v3(1, 0, 0));
	CHECK(fabsf(p.x) < 1e-5f && fabsf(p.z + 1) < 1e-5f); /* +x turns to -z */
	Mat4 t = m4_mul(m4_translate(v3(5, 0, 0)), r);
	p = m4_point(t, v3(1, 0, 0));
	CHECK(fabsf(p.x - 5) < 1e-5f && fabsf(p.z + 1) < 1e-5f);
	Mat4 v = m4_look_at(v3(0, 0, 10), v3(0, 0, 0), v3(0, 1, 0));
	p = m4_point(v, v3(0, 0, 0));
	CHECK(fabsf(p.z + 10) < 1e-5f); /* ten units in front: -z in view space */
	CHECK(fabsf(v3_len(v3_norm(v3(3, 4, 0))) - 1) < 1e-6f);
}

static void test_render(void)
{
	Canvas *c = canvas_new(200, 150);
	R3 *r = r3_new();
	Mesh3 *box = mesh3_new();
	mesh3_box(box, v3(0, 0, 0), v3(2, 2, 2), color_hex("#ff0000"));
	mesh3_finish(box);
	CHECK(box->ntri == 12 && fabsf(box->radius - sqrtf(3)) < 1e-4f);
	Mesh3 *back = mesh3_new();
	mesh3_box(back, v3(0, 0, 0), v3(6, 6, 0.5f), color_hex("#0000ff"));
	mesh3_finish(back);

	/* unlit and fog-free: the box shows its own colour, in front of the wall */
	box->unlit = back->unlit = true;
	canvas_clear(c, COLOR_BLACK);
	r3_begin(r, c);
	r3_camera(r, v3(0, 0, 8), v3(0, 0, 0), v3(0, 1, 0), (f32)PI / 3, 0.1f, 100);
	r3_mesh(r, back, m4_translate(v3(0, 0, -5)));
	r3_mesh(r, box, m4_identity());
	r3_end(r);
	CHECK(near_color(at(c, 100, 75), color_hex("#ff0000"), 1));  /* the box's front face */
	CHECK(near_color(at(c, 100, 50), color_hex("#0000ff"), 1));  /* the wall above it */
	CHECK(near_color(at(c, 2, 2), COLOR_BLACK, 0));               /* untouched outside */
	CHECK(r3_triangles(r) > 0);

	/* drawn in the other order, the depth test still keeps the box in front */
	canvas_clear(c, COLOR_BLACK);
	r3_begin(r, c);
	r3_camera(r, v3(0, 0, 8), v3(0, 0, 0), v3(0, 1, 0), (f32)PI / 3, 0.1f, 100);
	r3_mesh(r, box, m4_identity());
	r3_mesh(r, back, m4_translate(v3(0, 0, -5)));
	r3_end(r);
	CHECK(near_color(at(c, 100, 75), color_hex("#ff0000"), 1));

	/* inside the box every face looks away, so nothing is drawn */
	canvas_clear(c, COLOR_BLACK);
	r3_begin(r, c);
	r3_camera(r, v3(0, 0, 0), v3(0, 0, -1), v3(0, 1, 0), (f32)PI / 3, 0.1f, 100);
	r3_mesh(r, box, m4_identity());
	r3_end(r);
	CHECK(near_color(at(c, 100, 75), COLOR_BLACK, 0));

	/* a floor running under the eye is clipped at the near plane, not lost */
	Mesh3 *floor = mesh3_new();
	u32 a = mesh3_vertex(floor, v3(-50, 0, 50)), b = mesh3_vertex(floor, v3(50, 0, 50));
	u32 cc = mesh3_vertex(floor, v3(50, 0, -50)), d = mesh3_vertex(floor, v3(-50, 0, -50));
	mesh3_quad(floor, a, b, cc, d, color_hex("#00ff00"));
	mesh3_finish(floor);
	floor->unlit = true;
	canvas_clear(c, COLOR_BLACK);
	r3_begin(r, c);
	r3_camera(r, v3(0, 1, 0), v3(0, 0.5f, -3), v3(0, 1, 0), (f32)PI / 3, 0.1f, 200);
	r3_fog(r, color_hex("#ffffff"), 5, 40);
	r3_mesh(r, floor, m4_identity());
	r3_end(r);
	CHECK(near_color(at(c, 100, 148), color_hex("#00ff00"), 2));  /* just below the eye: green */
	Color far = at(c, 100, 58);                                      /* near the horizon: fogged */
	CHECK(COLOR_R(far) > 150 && COLOR_G(far) > 200);

	/* the sky: lighter toward the horizon, and the sun where the light is */
	canvas_clear(c, COLOR_BLACK);
	r3_begin(r, c);
	r3_camera(r, v3(0, 0, 0), v3(0, 0, -1), v3(0, 1, 0), (f32)PI / 3, 0.1f, 100);
	r3_light(r, v3(0, 0.1f, -1), color_hex("#ffcc88"), color_hex("#404050"));
	r3_sky(r, color_hex("#2050c0"), color_hex("#f0b080"), 0.05f, color_hex("#ffcc88"));
	r3_end(r);
	Color top = at(c, 20, 1), horizon = at(c, 20, 74);
	CHECK(COLOR_B(top) > COLOR_R(top) && COLOR_R(horizon) > COLOR_B(horizon));
	Vec2 sp;
	CHECK(r3_project(r, v3(0, 0.1f, -1), &sp, NULL));
	Color sun = at(c, (int)sp.x, (int)sp.y);
	CHECK(COLOR_R(sun) > 240 && COLOR_G(sun) > 220);

	/* lit faces: the one turned to the sun is brighter */
	box->unlit = false;
	canvas_clear(c, COLOR_BLACK);
	r3_begin(r, c);
	r3_camera(r, v3(5, 5, 5), v3(0, 0, 0), v3(0, 1, 0), (f32)PI / 3, 0.1f, 100);
	r3_light(r, v3(0, 1, 0), COLOR_WHITE, color_hex("#303030"));
	r3_fog(r, COLOR_BLACK, 1000, 2000);
	r3_mesh(r, box, m4_identity());
	r3_end(r);
	Vec2 ptop, pside;
	r3_project(r, v3(0, 1, 0), &ptop, NULL);
	r3_project(r, v3(1, 0, 0.3f), &pside, NULL);
	CHECK(COLOR_R(at(c, (int)ptop.x, (int)ptop.y)) > COLOR_R(at(c, (int)pside.x, (int)pside.y)) + 60);

	mesh3_free(floor);
	mesh3_free(back);
	mesh3_free(box);
	r3_free(r);
	canvas_free(c);
}

/* A little low-poly scene for looking at, and a rough speed figure. */
static void sample(void)
{
	Canvas *c = canvas_new(640, 400);
	R3 *r = r3_new();
	Mesh3 *ground = mesh3_new();
	const int N = 40;
	f32 h[41][41];
	for (int z = 0; z <= N; z++)
		for (int x = 0; x <= N; x++) {
			f32 fx = (f32)x - N / 2, fz = (f32)z - N / 2;
			h[z][x] = 3 * sinf(fx * 0.35f) * cosf(fz * 0.3f) + 6 * expf(-((fx - 6) * (fx - 6) + (fz + 10) * (fz + 10)) / 40) - 1;
			mesh3_vertex(ground, v3(fx * 2, h[z][x], fz * 2));
		}
	for (int z = 0; z < N; z++)
		for (int x = 0; x < N; x++) {
			u32 i = (u32)(z * (N + 1) + x);
			f32 avg = (h[z][x] + h[z][x + 1] + h[z + 1][x]) / 3;
			Color col = avg < 0 ? color_hex("#d8c48a") : avg < 3 ? color_hex("#5f9e4a") : color_hex("#8a8f96");
			mesh3_tri(ground, i, i + (u32)N + 1, i + 1, col);
			mesh3_tri(ground, i + 1, i + (u32)N + 1, i + (u32)N + 2, color_lighten(col, -0.04f));
		}
	mesh3_finish(ground);
	Mesh3 *water = mesh3_new();
	u32 a = mesh3_vertex(water, v3(-40, 0, 40)), b = mesh3_vertex(water, v3(40, 0, 40));
	u32 cc = mesh3_vertex(water, v3(40, 0, -40)), d = mesh3_vertex(water, v3(-40, 0, -40));
	mesh3_quad(water, a, b, cc, d, color_with_alpha(color_hex("#3a7bd5"), 200));
	mesh3_finish(water);
	Mesh3 *ring = mesh3_new();
	mesh3_torus(ring, v3(0, 0, 0), 2, 0.35f, 16, 6, color_hex("#ffc83d"));
	mesh3_finish(ring);
	Mesh3 *tree = mesh3_new();
	mesh3_cone(tree, v3(0, 0, 0), 0.25f, 0.25f, 0.8f, 5, color_hex("#7a5230"));
	mesh3_cone(tree, v3(0, 0.6f, 0), 1.2f, 0, 2.6f, 6, color_hex("#2f6b3a"));
	mesh3_finish(tree);

	u64 t0 = time_mono_ns();
	const int frames = 10;
	for (int f = 0; f < frames; f++) {
		r3_begin(r, c);
		r3_camera(r, v3(-6, 9, 34), v3(4, 2, 0), v3(0, 1, 0), 1.0f, 0.2f, 200);
		r3_light(r, v3(-0.5f, 0.35f, -1), color_hex("#ffd9a8"), color_hex("#6070a0"));
		r3_fog(r, color_hex("#f4c49a"), 30, 90);
		r3_sky(r, color_hex("#3d5fa8"), color_hex("#f4c49a"), 0.06f, color_hex("#ffcf8a"));
		r3_mesh(r, ground, m4_identity());
		for (int i = 0; i < 12; i++) {
			int x = (int)((u32)(i * 7919) % 36) + 2, z = (int)((u32)(i * 104729) % 36) + 2;
			if (h[z][x] > 0.5f && h[z][x] < 3)
				r3_mesh(r, tree, m4_translate(v3(((f32)x - N / 2) * 2, h[z][x], ((f32)z - N / 2) * 2)));
		}
		r3_mesh(r, ring, m4_mul(m4_translate(v3(4, 6, 8)), m4_rotate(v3(1, 0, 0), (f32)PI / 2)));
		r3_mesh(r, water, m4_identity());
		r3_end(r);
	}
	f64 ms = (f64)(time_mono_ns() - t0) / 1e6 / frames;
	CHECK(png_save(c, "build/tests/r3d.png"));
	print("r3d: sample scene %u triangles, %d.%d ms a frame at 640x400 on %d processors\n", r3_triangles(r), (int)ms,
	      (int)(ms * 10) % 10, cpu_count());
	mesh3_free(tree);
	mesh3_free(ring);
	mesh3_free(water);
	mesh3_free(ground);
	r3_free(r);
	canvas_free(c);
}

int main(void)
{
	test_math();
	test_render();
	sample();
	print("r3d: %d/%d checks passed\n", checks - failed, checks);
	return failed ? 1 : 0;
}
