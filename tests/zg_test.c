/* Tests for libzg; also writes build/tests/zg_sample.png for visual review. */
#include "zg.h"

static int failures, checks;

#define CHECK(cond)                                                          \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			failures++;                                          \
			eprint("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                            \
	} while (0)

static void test_deflate(void)
{
	StrBuf sb = { 0 };
	for (int i = 0; i < 2000; i++)
		sb_fmt(&sb, "line %d of a fairly repetitive test text %d\n", i, i % 17);
	usize zlen, back_len;
	u8 *z = zlib_deflate((u8 *)sb.data, sb.len, &zlen);
	CHECK(zlen < sb.len / 4);
	u8 *back = zlib_inflate(z, zlen, &back_len, true);
	CHECK(back && back_len == sb.len && memcmp(back, sb.data, sb.len) == 0);
	mem_free(z);
	mem_free(back);
	sb_free(&sb);
	CHECK(crc32_update(0, "123456789", 9) == 0xcbf43926u);
	CHECK(adler32_update(1, "Wikipedia", 9) == 0x11e60398u);
}

static void test_png_roundtrip(void)
{
	Canvas *c = canvas_new(37, 23);
	for (int y = 0; y < 23; y++)
		for (int x = 0; x < 37; x++)
			canvas_row(c, y)[x] = color_premul(RGBA(x * 7, y * 11, (x * y) & 255, 255 - x * 3));
	usize len;
	u8 *png = png_encode(c, &len);
	Canvas *d = png_decode(png, len);
	CHECK(d && d->width == 37 && d->height == 23);
	bool same = d != NULL;
	for (int y = 0; d && y < 23; y++)
		for (int x = 0; x < 37; x++) {
			Color a = color_unpremul(canvas_row(c, y)[x]), b = color_unpremul(canvas_row(d, y)[x]);
			for (int sh = 0; sh < 32; sh += 8)
				if (ABS((int)((a >> sh) & 255) - (int)((b >> sh) & 255)) > 2)
					same = false;
		}
	CHECK(same);
	mem_free(png);
	canvas_free(c);
	canvas_free(d);
}

/* GIFs decode to the same pixels ImageMagick gets. */
static void test_gif(void)
{
	static const char *const names[] = { "grad", "plasma-interlaced" };
	for (usize k = 0; k < ARRAY_LEN(names); k++) {
		char gif[128], png[128];
		fmt(gif, sizeof gif, "tests/data/zg/%s.gif", names[k]);
		fmt(png, sizeof png, "tests/data/zg/%s.png", names[k]);
		Canvas *a = image_load(gif), *b = image_load(png);
		CHECK(a && b);
		if (!a || !b)
			continue;
		CHECK(a->width == b->width && a->height == b->height);
		bool same = a->width == b->width && a->height == b->height;
		for (int y = 0; same && y < a->height; y++)
			for (int x = 0; x < a->width; x++)
				if (canvas_row(a, y)[x] != canvas_row(b, y)[x]) {
					same = false;
					break;
				}
		CHECK(same);
		canvas_free(a);
		canvas_free(b);
	}
}

static void test_raster(void)
{
	/* A half-covered pixel should get ~50% coverage. */
	Canvas *c = canvas_new(10, 10);
	fill_rect(c, rect(2.5f, 0, 5, 10), COLOR_WHITE);
	u32 left = canvas_row(c, 5)[2] >> 24, mid = canvas_row(c, 5)[4] >> 24, right = canvas_row(c, 5)[7] >> 24;
	CHECK(left >= 120 && left <= 136);
	CHECK(mid == 255);
	CHECK(right >= 120 && right <= 136);
	CHECK(canvas_row(c, 5)[8] == 0);
	/* Area of a circle should match pi r^2 closely. */
	canvas_clear(c, COLOR_CLEAR);
	Canvas *big = canvas_new(200, 200);
	fill_circle(big, 100, 100, 80, COLOR_WHITE);
	f64 sum = 0;
	for (int y = 0; y < 200; y++)
		for (int x = 0; x < 200; x++)
			sum += (canvas_row(big, y)[x] >> 24) / 255.0;
	if (fabs(sum - PI * 6400) >= 6400 * PI * 0.002)
		eprint("  circle area %f, want %f\n", sum, PI * 6400);
	CHECK(fabs(sum - PI * 6400) < 6400 * PI * 0.002);
	canvas_free(big);
	canvas_free(c);
}

static void test_text_layout(void)
{
	Font f = font(14, 400);
	CHECK(text_width_z(f, "Hello") > 30 && text_width_z(f, "Hello") < 50);
	CHECK(text_width_z(f, "0123") == text_width_z(f, "8888")); /* tabular figures */
	TextLine lines[8];
	const char *s = "The quick brown fox jumps over the lazy dog";
	int n = text_wrap(f, s, strlen(s), 120, lines, 8);
	CHECK(n >= 2 && n <= 4);
	for (int i = 0; i < n && i < 8; i++)
		CHECK(lines[i].width <= 120.5f);
	CHECK(text_hit(f, "Hello", 5, 0) == 0);
	CHECK(text_hit(f, "Hello", 5, 1000) == 5);
	CHECK(text_fit(f, "Hello world", 11, text_width_z(f, "Hello")) == 5);
}

/* ---- Visual sample ---------------------------------------------------------------- */

static void sample_sheet(void)
{
	int W = 1200, H = 900;
	Canvas *c = canvas_new(W, H);
	Paint bg = paint_linear(0, 0, (f32)W, (f32)H, color_hex("#1f3b57"), color_hex("#3f6f7a"));
	paint_add_stop(&bg, 0.55f, color_hex("#2c5568"));
	fill_rect_paint(c, rect(0, 0, (f32)W, (f32)H), &bg);
	Paint glow = paint_radial(900, 160, 520, color_hex("#f6c38f80"), color_hex("#f6c38f00"));
	fill_rect_paint(c, rect(0, 0, (f32)W, (f32)H), &glow);

	/* A window-like card with a soft shadow. */
	Rect card = rect(60, 60, 700, 780);
	draw_shadow(c, card, 14, 28, 0, 14, color_hex("#00000070"));
	fill_round_rect(c, card, 14, color_hex("#fafafa"));
	fill_round_rect(c, rect(card.x, card.y, card.w, 52), 14, color_hex("#f0f0f0"));
	fill_rect(c, rect(card.x, card.y + 38, card.w, 14), color_hex("#f0f0f0"));
	fill_rect(c, rect(card.x, card.y + 52, card.w, 1), color_hex("#dddddd"));
	fill_circle(c, card.x + 24, card.y + 26, 7, color_hex("#e0574f"));
	draw_text_in(c, font(14, 600), rect(card.x, card.y, card.w, 52), "Zurbax Sans", color_hex("#333333"), ALIGN_CENTER);

	f32 x = card.x + 32, y = card.y + 110;
	draw_text_z(c, font(44, 300), x, y, "Zurbax Sans Light", color_hex("#222222"));
	y += 58;
	draw_text_z(c, font(32, 700), x, y, "Bold Headline 2026", color_hex("#222222"));
	y += 48;
	draw_text_z(c, font(22, 500), x, y, "Medium: The quick brown fox", color_hex("#333333"));
	y += 40;
	const int sizes[] = { 18, 16, 15, 14, 13, 12, 11 };
	for (usize i = 0; i < ARRAY_LEN(sizes); i++) {
		char label[96];
		fmt(label, sizeof label, "%dpx  The quick brown fox jumps over the lazy dog.", sizes[i]);
		draw_text_z(c, font((f32)sizes[i], 400), x, y, label, color_hex("#333333"));
		y += (f32)sizes[i] * 1.6f;
	}
	y += 10;
	draw_text_z(c, font(15, 400), x, y, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", color_hex("#333333"));
	y += 26;
	draw_text_z(c, font(15, 400), x, y, "abcdefghijklmnopqrstuvwxyz 0123456789", color_hex("#333333"));
	y += 26;
	draw_text_z(c, font(15, 400), x, y, "!? @#$%&*()[]{} +-=/\\<> \"quotes\" \xe2\x80\x9c" "curly\xe2\x80\x9d \xe2\x80\xa6", color_hex("#333333"));
	y += 26;
	draw_text_z(c, font(15, 400), x, y, "\xc3\xa9\xc3\xa8\xc3\xaa \xc3\xa0\xc3\xa2 \xc3\xb1 \xc3\xbc\xc3\xb6\xc3\xa4 \xc3\xa7 \xc3\x89\xc3\x80 \xc3\x96 \xc8\x99\xc8\x9b\xc4\x83 21\xc2\xb0" "C \xe2\x82\xac" "5", color_hex("#333333"));
	y += 34;
	draw_text_z(c, font_mono(14, 400), x, y, "mono: ls -la ~/Documents | grep zurbax", color_hex("#2a6f4f"));
	y += 30;
	Rect para = rect(x, y, 300, 120);
	draw_text_wrapped(c, font(13, 400), para,
			  "Wrapped text flows onto new lines at word boundaries, so paragraphs in apps read naturally "
			  "without any extra work from the developer.",
			  color_hex("#555555"), ALIGN_START, 1.1f);

	/* Controls */
	f32 cx = 820, cy = 100;
	Rect btn = rect(cx, cy, 300, 44);
	draw_shadow(c, btn, 10, 6, 0, 2, color_hex("#00000040"));
	Paint bp = paint_linear(0, btn.y, 0, btn.y + btn.h, color_hex("#4c9be8"), color_hex("#3584e4"));
	fill_round_rect_paint(c, btn, 10, &bp);
	draw_text_in(c, font(15, 600), btn, "Install Zurbax", COLOR_WHITE, ALIGN_CENTER);
	cy += 70;
	fill_round_rect(c, rect(cx, cy, 300, 44), 10, color_hex("#ffffff30"));
	stroke_round_rect(c, rect(cx, cy, 300, 44), 10, 1, color_hex("#ffffff60"));
	draw_text_in(c, font(15, 500), rect(cx, cy, 300, 44), "Secondary", COLOR_WHITE, ALIGN_CENTER);
	cy += 74;
	/* Switch */
	fill_round_rect(c, rect(cx, cy, 52, 30), 15, color_hex("#3584e4"));
	draw_shadow(c, rect(cx + 24, cy + 2, 26, 26), 13, 3, 0, 1, color_hex("#00000050"));
	fill_circle(c, cx + 37, cy + 15, 13, COLOR_WHITE);
	fill_round_rect(c, rect(cx + 70, cy, 52, 30), 15, color_hex("#ffffff40"));
	fill_circle(c, cx + 85, cy + 15, 13, COLOR_WHITE);
	/* Slider */
	cy += 60;
	fill_round_rect(c, rect(cx, cy, 300, 6), 3, color_hex("#ffffff40"));
	fill_round_rect(c, rect(cx, cy, 190, 6), 3, color_hex("#ffffff"));
	draw_shadow(c, rect(cx + 179, cy - 8, 22, 22), 11, 3, 0, 1, color_hex("#00000060"));
	fill_circle(c, cx + 190, cy + 3, 11, COLOR_WHITE);
	/* Shapes */
	cy += 50;
	Path p = { 0 };
	path_move(&p, cx + 20, cy + 120);
	path_cubic(&p, cx + 80, cy - 40, cx + 220, cy + 220, cx + 280, cy + 20);
	Paint stroke = paint_linear(cx, 0, cx + 300, 0, color_hex("#f9d976"), color_hex("#f39f86"));
	StrokeStyle st = { 6, CAP_ROUND, JOIN_ROUND, 4 };
	stroke_path(c, &p, mat_identity(), &stroke, &st);
	path_free(&p);
	cy += 160;
	for (int i = 0; i < 6; i++) {
		Color col = color_hsl((f32)i * 55 + 10, 0.65f, 0.6f, 255);
		fill_round_rect(c, rect(cx + (f32)i * 50, cy, 40, 40), 10, col);
	}
	cy += 70;
	/* Frosted glass panel over the background */
	Rect glass = rect(cx, cy, 300, 120);
	blur_region(c, glass, 24);
	fill_round_rect(c, glass, 16, color_hex("#ffffff28"));
	stroke_round_rect(c, glass, 16, 1, color_hex("#ffffff50"));
	draw_text_in(c, font(36, 300), rect(glass.x, glass.y + 8, glass.w, 60), "12:45", COLOR_WHITE, ALIGN_CENTER);
	draw_text_in(c, font(13, 500), rect(glass.x, glass.y + 66, glass.w, 30), "Monday, October 5", color_hex("#ffffffcc"), ALIGN_CENTER);

	png_save(c, "build/tests/zg_sample.png");
	/* 3x smooth (bilinear) zoom of the small text for close inspection. */
	Rect z = rect(85, 375, 375, 195);
	Canvas *zoom = canvas_new((int)z.w * 3, (int)z.h * 3);
	blit_scaled(zoom, c, z, rect(0, 0, z.w * 3, z.h * 3), 1);
	png_save(zoom, "build/tests/zg_zoom.png");
	canvas_free(zoom);
	canvas_free(c);
}

int main(void)
{
	test_deflate();
	test_png_roundtrip();
	test_gif();
	test_raster();
	test_text_layout();
	u64 t0 = time_mono_ns();
	sample_sheet();
	u64 t1 = time_mono_ns();
	print("libzg: %d/%d checks passed (sample sheet rendered in %llu ms)\n", checks - failures, checks,
	      (unsigned long long)((t1 - t0) / 1000000));
	return failures ? 1 : 0;
}
