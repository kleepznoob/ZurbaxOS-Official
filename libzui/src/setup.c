/*
 * The look the setup assistants share (Welcome on the first start, the
 * Installer from the stick), the look of zurbax.com: the theme's pixel
 * picture fills the window with stars twinkling over it, the name stands
 * at the top of the steps in big pixels, built block by block, and the
 * page sits on a pane of frosted glass with pixel corners. The steps are
 * keycaps with pixel numbers, joined by a dotted line that fills as they're
 * done; buttons, cards and the tick at the end are made of blocks too.
 * Everything moves in steps, like the rest of the pixel style: the stars at
 * eight frames a second, the blocks in the ordered pattern windows use.
 */
#include "zui.h"

/* the bands of the name, top to bottom, and its shadow, in each theme */
static const struct {
	const char *id;
	const char *band[5], *shade;
} MARKS[] = {
	{ "carbon", { "#ffd98a", "#f7b443", "#e68e0d", "#d9652a", "#b5402f" }, "#2a1608" },
	{ "midnight", { "#dfe5ff", "#b4c3fb", "#7aa2f7", "#6e7fe0", "#8d6fd6" }, "#0b0d18" },
};

/* 11 × 13 letters, the same as the website's */
static const char *const GLYPHS[6][13] = {
	{ ".XXXXXXXXX.", "XXXXXXXXXXX", "XXXXXXXXXXX", "......XXXX.", ".....XXXX..", "....XXXX...", "...XXXX....", "..XXXX.....", ".XXXX......",
	  "XXXX.......", "XXXXXXXXXXX", "XXXXXXXXXXX", ".XXXXXXXXX." },
	{ "XXX.....XXX", "XXX.....XXX", "XXX.....XXX", "XXX.....XXX", "XXX.....XXX", "XXX.....XXX", "XXX.....XXX", "XXX.....XXX", "XXX.....XXX",
	  "XXX.....XXX", "XXXX...XXXX", ".XXXXXXXXX.", "..XXXXXXX.." },
	{ "XXXXXXXXX..", "XXXXXXXXXX.", "XXX....XXXX", "XXX.....XXX", "XXX....XXXX", "XXXXXXXXXX.", "XXXXXXXXX..", "XXX..XXXX..", "XXX...XXXX.",
	  "XXX....XXX.", "XXX....XXXX", "XXX.....XXX", "XXX.....XXX" },
	{ "XXXXXXXXX..", "XXXXXXXXXX.", "XXX....XXXX", "XXX.....XXX", "XXX....XXXX", "XXXXXXXXXX.", "XXXXXXXXXX.", "XXX....XXXX", "XXX.....XXX",
	  "XXX.....XXX", "XXX....XXXX", "XXXXXXXXXX.", "XXXXXXXXX.." },
	{ "...XXXXX...", "..XXXXXXX..", ".XXXX.XXXX.", "XXXX...XXXX", "XXX.....XXX", "XXX.....XXX", "XXXXXXXXXXX", "XXXXXXXXXXX", "XXX.....XXX",
	  "XXX.....XXX", "XXX.....XXX", "XXX.....XXX", "XXX.....XXX" },
	{ "XXX.....XXX", "XXX.....XXX", "XXXX...XXXX", ".XXXX.XXXX.", "..XXXXXXX..", "...XXXXX...", "....XXX....", "...XXXXX...", "..XXXXXXX..",
	  ".XXXX.XXXX.", "XXXX...XXXX", "XXX.....XXX", "XXX.....XXX" },
};
static const int BAND[13] = { 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4 }; /* the band of each row */
enum { GW = 11, GH = 13, GGAP = 2, MARK_COLS = 6 * (GW + GGAP) - GGAP + 1 };

/* a small tick, 7 × 5, for the steps that are done and the chosen card */
static const char *const TICK[5] = { "......X", ".....X.", "X...X..", ".X.X...", "..X...." };

static const HiveTheme *worn; /* the theme the frame wears this frame */

static const HiveTheme *theme_worn(void)
{
	return worn ? worn : hive_theme_find(NULL);
}

static int mark_of(const HiveTheme *h)
{
	for (int i = 0; i < (int)ARRAY_LEN(MARKS); i++)
		if (str_eq(MARKS[i].id, h->id))
			return i;
	return 0;
}

/* One pixel of the style: two logical pixels, on whole device pixels. */
static f32 unit(Ui *ui)
{
	return maxf(1, roundf(2 * ui->scale)) / ui->scale;
}

static f32 snapf(Ui *ui, f32 v)
{
	return roundf(v * ui->scale) / ui->scale;
}

static Rect snap(Ui *ui, Rect r)
{
	f32 x0 = snapf(ui, r.x), y0 = snapf(ui, r.y);
	return rect(x0, y0, snapf(ui, r.x + r.w) - x0, snapf(ui, r.y + r.h) - y0);
}

/* The next step of the stars and the other things that move at 8 a second. */
static void wake_for_tick(Ui *ui)
{
	if (ui->theme.reduce_motion)
		return;
	f64 next = (floor(ui->now * 8) + 1) / 8 + 0.001;
	if (!ui->wake_at || next < ui->wake_at)
		ui->wake_at = next;
}

static u32 hash32(u32 x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static f32 rnd(u32 i, u32 salt)
{
	return (f32)(hash32(i * 2654435761u + salt * 0x9e3779b9u) & 0xffffff) / 16777216.0f;
}

/* ---- Blocks ------------------------------------------------------------------------------ */

/* r cut into bands with pixel corners: k steps of p each. */
static int shape_rects(Rect r, f32 p, int k, Rect *out)
{
	int n = 0;
	for (int i = 0; i < k; i++) {
		f32 in = (f32)(k - i) * p;
		out[n++] = rect(r.x + in, r.y + (f32)i * p, r.w - 2 * in, p);
		out[n++] = rect(r.x + in, r.y + r.h - (f32)(i + 1) * p, r.w - 2 * in, p);
	}
	out[n++] = rect(r.x, r.y + (f32)k * p, r.w, r.h - 2 * (f32)k * p);
	return n;
}

void setup_block(Ui *ui, Rect r, int k, Color c)
{
	Rect rs[12];
	int n = shape_rects(snap(ui, r), unit(ui), CLAMP(k, 0, 5), rs);
	for (int i = 0; i < n; i++)
		ui_fill(ui, rs[i], c);
}

/* The edge of a block, a pixel wide. */
static void outline(Ui *ui, Rect r, int k, Color c)
{
	f32 p = unit(ui), in = (f32)k * p;
	r = snap(ui, r);
	ui_fill(ui, rect(r.x + in, r.y, r.w - 2 * in, p), c);
	ui_fill(ui, rect(r.x + in, r.y + r.h - p, r.w - 2 * in, p), c);
	ui_fill(ui, rect(r.x, r.y + in, p, r.h - 2 * in), c);
	ui_fill(ui, rect(r.x + r.w - p, r.y + in, p, r.h - 2 * in), c);
	for (int i = 1; i < k; i++) {
		f32 a = (f32)(k - i) * p, b = (f32)i * p;
		ui_fill(ui, rect(r.x + a, r.y + b, p, p), c);
		ui_fill(ui, rect(r.x + r.w - a - p, r.y + b, p, p), c);
		ui_fill(ui, rect(r.x + a, r.y + r.h - b - p, p, p), c);
		ui_fill(ui, rect(r.x + r.w - a - p, r.y + r.h - b - p, p, p), c);
	}
}

/* Corner brackets around r, like a sight: what's chosen. */
static void brackets(Ui *ui, Rect r, f32 len, Color c)
{
	f32 p = unit(ui);
	r = snap(ui, r);
	f32 x1 = r.x + r.w, y1 = r.y + r.h;
	ui_fill(ui, rect(r.x, r.y, len, p), c), ui_fill(ui, rect(r.x, r.y, p, len), c);
	ui_fill(ui, rect(x1 - len, r.y, len, p), c), ui_fill(ui, rect(x1 - p, r.y, p, len), c);
	ui_fill(ui, rect(r.x, y1 - p, len, p), c), ui_fill(ui, rect(r.x, y1 - len, p, len), c);
	ui_fill(ui, rect(x1 - len, y1 - p, len, p), c), ui_fill(ui, rect(x1 - p, y1 - len, p, len), c);
}

/* A pattern of 'X's drawn in cells of size cell. */
static void pattern(Ui *ui, f32 x, f32 y, const char *const *rows, int n, f32 cell, Color c)
{
	x = snapf(ui, x), y = snapf(ui, y);
	for (int r = 0; r < n; r++)
		for (int k = 0; rows[r][k]; k++)
			if (rows[r][k] == 'X')
				ui_fill(ui, rect(x + (f32)k * cell, y + (f32)r * cell, cell, cell), c);
}

void setup_card(Ui *ui, Rect r, bool selected, f32 hover)
{
	Theme *t = &ui->theme;
	f32 p = unit(ui);
	r = snap(ui, r);
	Color base = t->dark ? COLOR_WHITE : COLOR_BLACK;
	setup_block(ui, r, 2, selected ? color_with_alpha(t->accent, 0x22) : color_with_alpha(base, (u8)(0x0a + hover * 0x0c)));
	outline(ui, r, 2, selected ? t->accent : color_mix(color_with_alpha(base, 0x2c), t->accent, hover * 0.8f));
	/* light along the top, inside the edge */
	ui_fill(ui, rect(r.x + 2 * p, r.y + p, r.w - 4 * p, p), color_with_alpha(COLOR_WHITE, selected ? 0x2c : 0x10));
	if (selected)
		brackets(ui, rect_inset(r, -3 * p, -3 * p), 5 * p, t->accent);
}

void setup_rule(Ui *ui, f32 x0, f32 y, f32 x1)
{
	f32 p = unit(ui);
	Color c = color_with_alpha(ui->theme.text, 0x30);
	y = snapf(ui, y);
	for (f32 x = snapf(ui, x0); x + p <= x1; x += 3 * p)
		ui_fill(ui, rect(x, y, p, p), c);
}

bool setup_button(Ui *ui, Rect r, const char *label, int kind)
{
	Theme *t = &ui->theme;
	UiId id = ui_id(ui, label);
	UiInteract it = ui_interact(ui, id, r);
	f32 hover = ui_fade(ui, id, it.hovered, 0.12f);
	if (it.hovered)
		ui_set_cursor(ui, CURSOR_POINTER);
	f32 p = unit(ui);
	bool down = it.held && it.hovered;
	r = snap(ui, r);
	Color fill, fg = COLOR_WHITE;
	switch (kind) {
	case SETUP_BUTTON_GO:
		fill = t->accent, fg = t->on_accent;
		break;
	case SETUP_BUTTON_DANGER:
		fill = t->danger;
		break;
	case SETUP_BUTTON_FLAT:
		fill = color_with_alpha(t->text, (u8)(hover * 0x18)), fg = color_mix(t->text_dim, t->text, hover);
		break;
	default:
		fill = color_with_alpha(t->text, (u8)(0x16 + hover * 0x10)), fg = t->text;
		break;
	}
	bool solid = kind == SETUP_BUTTON_GO || kind == SETUP_BUTTON_DANGER;
	if (solid) {
		fill = color_mix(fill, COLOR_WHITE, hover * 0.12f);
		/* a hard shadow it sits on, and it goes down onto it when pressed */
		setup_block(ui, rect(r.x, r.y + p, r.w, r.h), 2, color_mix(fill, COLOR_BLACK, 0.45f));
		Rect face = rect(r.x, r.y + (down ? p : 0), r.w, r.h - p);
		setup_block(ui, face, 2, fill);
		ui_fill(ui, rect(face.x + 2 * p, face.y + p, face.w - 4 * p, p), color_with_alpha(COLOR_WHITE, 0x50));
		if (hover > 0 && !t->reduce_motion)
			brackets(ui, rect_inset(r, -2 * p, -2 * p), 3 * p, color_with_alpha(fill, (u8)(hover * 0xff)));
		r = face;
	} else {
		Rect face = rect(r.x, r.y + (down ? p : 0), r.w, r.h);
		setup_block(ui, face, 2, fill);
		if (kind != SETUP_BUTTON_FLAT)
			outline(ui, face, 2, color_with_alpha(t->text, (u8)(0x30 + hover * 0x30)));
		r = face;
	}
	ui_text_in(ui, ui_font(ui, 14, 700), rect(r.x + 8, r.y, r.w - 16, r.h), label, fg, ALIGN_CENTER);
	return it.clicked;
}

void setup_segments(Ui *ui, Rect r, f32 fraction, int n, bool shine)
{
	Theme *t = &ui->theme;
	f32 p = unit(ui), gap = 2 * p;
	f32 w = (r.w - gap * (f32)(n - 1)) / (f32)n;
	int filled = (int)(clampf(fraction, 0, 1) * (f32)n);
	int lit = shine && !t->reduce_motion && filled > 0 ? (int)(ui->now * 12) % (filled + 6) : -1;
	for (int i = 0; i < n; i++) {
		Rect b = snap(ui, rect(r.x + (f32)i * (w + gap), r.y, w, r.h));
		f32 f = clampf(fraction * (f32)n - (f32)i, 0, 1);
		setup_block(ui, b, 1, color_with_alpha(t->text, 0x1a));
		if (f <= 0)
			continue;
		Rect fb = b;
		fb.w = maxf(p, floorf(b.w * f / p) * p);
		Color c = i == lit ? color_mix(t->accent, COLOR_WHITE, 0.5f) : t->accent;
		setup_block(ui, fb, 1, c);
		ui_fill(ui, rect(fb.x + p, fb.y, maxf(0, fb.w - 2 * p), p), color_with_alpha(COLOR_WHITE, 0x48));
	}
	if (lit >= 0)
		wake_for_tick(ui);
}

/* ---- The name in big pixels -------------------------------------------------------------- */

f32 setup_wordmark_width(f32 cell)
{
	return (f32)MARK_COLS * cell;
}

f32 setup_wordmark(Ui *ui, f32 x, f32 y, f32 cell, f64 start)
{
	const HiveTheme *h = theme_worn();
	int m = mark_of(h);
	Color band[5], shade = color_hex(MARKS[m].shade);
	for (int i = 0; i < 5; i++)
		band[i] = color_hex(MARKS[m].band[i]);
	bool still = ui->theme.reduce_motion;
	cell = maxf(1, floorf(cell * ui->scale)) / ui->scale;
	x = snapf(ui, x), y = snapf(ui, y);
	f32 e = (f32)(ui->now - start);
	/* a light that sweeps across now and then, in steps */
	int tick = (int)(ui->now * 8);
	f32 sweep = still || e < 1 ? -1 : (f32)(tick % 48) / 10.0f; /* 0..1 for ten ticks of every 48 */
	bool busy = false;
	for (int pass = 0; pass < 2; pass++) {
		for (int li = 0; li < 6; li++) {
			const char *const *g = GLYPHS[li];
			for (int gy = 0; gy < GH; gy++) {
				for (int gx = 0; gx < GW; gx++) {
					if (g[gy][gx] != 'X')
						continue;
					int cx = li * (GW + GGAP) + gx, cy = gy;
					f32 since = still ? 1 : e - block_turn(cx, cy) * 0.6f;
					if (since < 0) {
						busy = true;
						continue;
					}
					if (pass == 0) {
						ui_fill(ui, rect(x + (f32)(cx + 1) * cell, y + (f32)(cy + 1) * cell, cell, cell), shade);
						continue;
					}
					int b = BAND[gy];
					/* where one band meets the next, the two in a checker */
					if (gy > 0 && BAND[gy - 1] != b && (cx + cy) % 2)
						b = BAND[gy - 1];
					Color c = band[b];
					bool top = gy == 0 || g[gy - 1][gx] != 'X';
					bool low = gx == GW - 1 || g[gy][gx + 1] != 'X' || gy == GH - 1 || g[gy + 1][gx] != 'X';
					if (top)
						c = color_mix(c, COLOR_WHITE, 0.3f);
					else if (low)
						c = color_mix(c, COLOR_BLACK, 0.22f);
					if (since < 0.12f)
						c = color_mix(c, COLOR_WHITE, 1 - since / 0.12f), busy = true;
					if (sweep >= 0) {
						f32 at = ((f32)cx + (f32)(GH - cy) * 0.6f) / (f32)(MARK_COLS + 8);
						if (fabsf(at - sweep) < 0.05f)
							c = color_mix(c, COLOR_WHITE, 0.5f);
					}
					ui_fill(ui, rect(x + (f32)cx * cell, y + (f32)cy * cell, cell, cell), c);
				}
			}
		}
	}
	if (busy)
		ui->redraw = true;
	else
		wake_for_tick(ui);
	return setup_wordmark_width(cell);
}

/* ---- The picture behind ------------------------------------------------------------------ */

static struct {
	Canvas *art;      /* the theme's picture, the size of the window */
	Canvas *frost;    /* the same, blurred and tinted: the glass */
	const char *id;
	int w, h;
	u32 tint;
} back;

static void backdrop_make(Ui *ui, Color tint)
{
	int w = MAX(1, (int)roundf(ui->width * ui->scale)), h = MAX(1, (int)roundf(ui->height * ui->scale));
	const char *id = theme_worn()->id;
	u32 tk = color_premul(tint);
	if (back.art && back.w == w && back.h == h && back.id == id && back.tint == tk)
		return;
	if (!back.art || back.w != w || back.h != h || back.id != id) {
		canvas_free(back.art);
		back.art = bitart_render(id, w, h);
	}
	canvas_free(back.frost);
	/* small, blurred and stretched back: soft, and quick */
	int sw = MAX(1, w / 8), sh = MAX(1, h / 8);
	Canvas *small = canvas_new(sw, sh);
	blit_scaled(small, back.art, rect(0, 0, (f32)w, (f32)h), rect(0, 0, (f32)sw, (f32)sh), 1);
	blur_region(small, rect(0, 0, (f32)sw, (f32)sh), 3);
	fill_rect(small, rect(0, 0, (f32)sw, (f32)sh), tint);
	back.frost = canvas_new(w, h);
	blit_scaled(back.frost, small, rect(0, 0, (f32)sw, (f32)sh), rect(0, 0, (f32)w, (f32)h), 1);
	canvas_free(small);
	back.w = w, back.h = h, back.id = id, back.tint = tk;
}

/* A part of a canvas the size of the window, where it lies. */
static void blit_window_part(Ui *ui, const Canvas *src, Rect r)
{
	Rect d = ui_to_device(ui, r);
	int x0 = MAX(0, (int)roundf(d.x)), y0 = MAX(0, (int)roundf(d.y));
	int x1 = MIN(src->width, (int)roundf(d.x + d.w)), y1 = MIN(src->height, (int)roundf(d.y + d.h));
	if (x1 > x0 && y1 > y0)
		blit_scaled(ui->canvas, src, rect((f32)x0, (f32)y0, (f32)(x1 - x0), (f32)(y1 - y0)),
			    rect((f32)x0, (f32)y0, (f32)(x1 - x0), (f32)(y1 - y0)), 1);
}

/* Stars over the sky of the picture: most of them faint, each flaring now
 * and then, a few in the accent; and a shooting star every few seconds. */
static void stars(Ui *ui, Rect area)
{
	const HiveTheme *h = theme_worn();
	bool still = ui->theme.reduce_motion;
	f32 p = unit(ui);
	int tick = still ? 0 : (int)(ui->now * 8);
	Color dim = color_hex(h->muted), lit = color_hex(h->fg), hot = color_hex(h->accent);
	int n = CLAMP((int)(area.w * area.h / 2600), 24, 160);
	ui_push_clip(ui, area);
	for (int i = 0; i < n; i++) {
		f32 fy = rnd((u32)i, 2);
		f32 x = area.x + floorf(rnd((u32)i, 1) * area.w / p) * p, y = area.y + floorf(fy * fy * area.h * 0.62f / p) * p;
		int period = 14 + (int)(rnd((u32)i, 3) * 40);
		int phase = (tick + (int)(rnd((u32)i, 4) * (f32)period)) % period;
		int level = still ? 1 + i % 2 : phase == 0 ? 3 : phase < 3 ? 2 : 1;
		Color c = level == 3 ? lit : level == 2 ? color_mix(dim, lit, 0.5f) : color_with_alpha(dim, 0xb0);
		if (i % 9 == 0)
			c = level >= 2 ? hot : color_with_alpha(color_mix(dim, hot, 0.4f), 0xc0);
		ui_fill(ui, rect(x, y, p, p), c);
		if (level == 3 && i % 3 == 0) {
			Color arm = color_with_alpha(c, 0x80);
			ui_fill(ui, rect(x - p, y, p, p), arm), ui_fill(ui, rect(x + p, y, p, p), arm);
			ui_fill(ui, rect(x, y - p, p, p), arm), ui_fill(ui, rect(x, y + p, p, p), arm);
		}
	}
	int cycle = tick % 76, which = tick / 76;
	if (!still && cycle < 9) {
		f32 sx = area.x + (0.35f + rnd((u32)which, 7) * 0.6f) * area.w, sy = area.y + (0.03f + rnd((u32)which, 8) * 0.22f) * area.h;
		f32 hx = floorf((sx - (f32)cycle * 7 * p) / p) * p, hy = floorf((sy + (f32)cycle * 3 * p) / p) * p;
		for (int j = 0; j < 7 && j <= cycle * 3; j++)
			ui_fill(ui, rect(hx + (f32)j * 2 * p, hy - (f32)j * p, p * (j ? 2 : 1), p),
				color_with_alpha(j ? hot : lit, (u8)(0xff - j * 0x22)));
	}
	ui_pop_clip(ui);
	wake_for_tick(ui);
}

/* ---- The frame --------------------------------------------------------------------------- */

static struct {
	Rect pane;
	f64 born;
} fr;

f32 setup_ease(f32 t)
{
	return ease_out_cubic(clampf(t, 0, 1));
}

Rect setup_pane(void)
{
	return fr.pane;
}

Rect setup_frame(Ui *ui, const SetupSteps *s)
{
	Theme *t = &ui->theme;
	worn = hive_theme_find(s->theme);
	if (!fr.born)
		fr.born = ui->now;
	bool still = t->reduce_motion;
	f32 p = unit(ui);
	Color tint = color_with_alpha(t->window, t->dark ? 0xc8 : 0xdc);
	backdrop_make(ui, tint);

	/* where the steps go, and the pane for the page */
	f32 sw = snapf(ui, clampf(ui->width * 0.27f, 250, 300));
	Rect pane = snap(ui, rect(sw, 22, ui->width - sw - 22, ui->height - 44));
	const int K = 3; /* the pane's corners, in steps */
	f32 ck = (f32)K * p;

	/* the picture, all but what's under the pane */
	blit_window_part(ui, back.art, rect(0, 0, pane.x, ui->height));
	blit_window_part(ui, back.art, rect(pane.x + pane.w, 0, ui->width - pane.x - pane.w, ui->height));
	blit_window_part(ui, back.art, rect(pane.x, 0, pane.w, pane.y + ck));
	blit_window_part(ui, back.art, rect(pane.x, pane.y + pane.h - ck, pane.w, ui->height - pane.y - pane.h + ck));
	/* darker where the steps are, so they read */
	Rect sb = rect(0, 0, sw, ui->height);
	ui_gradient(ui, sb, 0, color_hex("#00000050"), color_hex("#000000b8"));
	stars(ui, rect(0, 0, ui->width, ui->height));

	/* the pane: a hard shadow, the glass, a pixel edge and a sight's corners */
	setup_block(ui, rect(pane.x + 2 * p, pane.y + 2 * p, pane.w, pane.h), K, color_hex("#00000070"));
	{
		Rect rs[12];
		int n = shape_rects(pane, p, K, rs);
		for (int i = 0; i < n; i++) {
			if (t->glass)
				blit_window_part(ui, back.frost, rs[i]);
			else
				ui_fill(ui, rs[i], color_with_alpha(t->window, 0xff));
		}
	}
	outline(ui, pane, K, color_with_alpha(COLOR_WHITE, 0x24));
	ui_fill(ui, rect(pane.x + ck, pane.y + p, pane.w - 2 * ck, p), color_with_alpha(COLOR_WHITE, 0x12));
	brackets(ui, rect_inset(pane, -3 * p, -3 * p), 8 * p, color_with_alpha(t->accent, 0xc0));
	fr.pane = rect_inset(pane, p, p);

	/* who's asking: the name, built block by block, and what this is */
	f32 hx = 24, hw = sw - 24 - 22;
	f32 cell = maxf(1, floorf(hw * ui->scale / (f32)MARK_COLS)) / ui->scale;
	setup_wordmark(ui, hx, 30, cell, fr.born);
	f32 ty = snapf(ui, 30 + (f32)(GH + 1) * cell + 12);
	ui_pixel_text(ui, hx, ty, s->title, p, t->accent);
	ui_pixel_text(ui, hx + hw - ui_pixel_text_width(OS_VERSION, p), ty, OS_VERSION, p, color_with_alpha(COLOR_WHITE, 0x90));
	Rect side = rect(0, ty + 7 * p + 26, sw, ui->height);

	/* the steps: keycaps joined by a dotted line that fills as they're done */
	f32 done = ui_anim(ui, ui_id(ui, "setup-progress"), (f32)s->current, 7);
	if (fabsf(done - (f32)s->current) > 0.002f)
		ui->redraw = true;
	const f32 row = 50, cap = 28;
	f32 top = side.y;
	f32 hl = ui_anim(ui, ui_id(ui, "setup-highlight"), top + (f32)MIN(s->current, s->count - 1) * row, 14);
	if (s->current < s->count) {
		Rect band = snap(ui, rect(14, hl + 4, sw - 28, row - 8));
		setup_block(ui, band, 2, color_with_alpha(t->accent, 0x26));
		ui_fill(ui, rect(band.x, band.y + 2 * p, p * 2, band.h - 4 * p), t->accent);
		/* a cursor that blinks at the end of the step being done */
		if (still || (int)(ui->now * 4) % 2 == 0)
			ui_pixel_text(ui, band.x + band.w - 16, snapf(ui, band.y + (band.h - 7 * p) / 2), ">", p, t->accent);
	}
	int tick = (int)(ui->now * 4);
	for (int i = 0; i < s->count; i++) {
		f32 y = top + (f32)i * row;
		Rect c = snap(ui, rect(30, y + (row - cap) / 2, cap, cap));
		f32 cx = c.x + c.w / 2;
		/* the line down to the next step, dot by dot */
		if (i < s->count - 1) {
			f32 y0 = c.y + c.h + 3 * p, y1 = c.y + row - 3 * p;
			f32 f = clampf(done - (f32)i, 0, 1);
			for (f32 dy = y0; dy + p <= y1; dy += 2 * p)
				ui_fill(ui, rect(snapf(ui, cx - p / 2), snapf(ui, dy), p, p),
					(dy - y0) / maxf(1, y1 - y0) < f ? t->accent : color_with_alpha(COLOR_WHITE, 0x38));
		}
		bool is_done = i < s->current, now = i == s->current;
		char num[8];
		fmt(num, sizeof num, "%d", i + 1);
		f32 nw = ui_pixel_text_width(num, p);
		f32 nx = snapf(ui, c.x + (c.w - nw) / 2), ny = snapf(ui, c.y + (c.h - 7 * p) / 2);
		if (is_done) {
			setup_block(ui, c, 1, t->accent);
			ui_fill(ui, rect(c.x + p, c.y, c.w - 2 * p, p), color_with_alpha(COLOR_WHITE, 0x50));
			pattern(ui, c.x + (c.w - 7 * p) / 2, c.y + (c.h - 5 * p) / 2, TICK, 5, p, t->on_accent);
		} else if (now) {
			setup_block(ui, c, 1, color_with_alpha(t->accent, 0x40));
			outline(ui, c, 1, t->accent);
			ui_pixel_text(ui, nx, ny, num, p, COLOR_WHITE);
			/* the sight closes in and opens out, in steps */
			f32 out = still ? 2 : (f32)(2 + (tick % 4 == 3 ? 1 : tick % 4 == 1 ? 1 : tick % 4 == 2 ? 2 : 0));
			brackets(ui, rect_inset(c, -out * p, -out * p), 3 * p, t->accent);
		} else {
			outline(ui, c, 1, color_with_alpha(COLOR_WHITE, 0x44));
			ui_pixel_text(ui, nx, ny, num, p, color_with_alpha(COLOR_WHITE, 0x88));
		}
		Rect label = rect(c.x + c.w + 16, y, sw - (c.x + c.w + 16) - 34, row);
		Color lc = now ? COLOR_WHITE : is_done ? color_hex("#ffffffcc") : color_hex("#ffffff80");
		ui_text_in(ui, ui_font(ui, 15, now ? 700 : 500), label, s->steps[i], lc, ALIGN_START);
	}

	/* how far along: a word in pixels, and a bar of blocks, one a step */
	Rect foot = pad4(rect(0, ui->height - 74, sw, 74), 0, 22, 26, 24);
	char where[48];
	if (s->current < s->count)
		fmt(where, sizeof where, "STEP %d/%d", s->current + 1, s->count);
	else
		str_copy(where, "ALL DONE!", sizeof where);
	ui_pixel_text(ui, foot.x, snapf(ui, foot.y), where, p, s->current < s->count ? color_hex("#ffffffa8") : t->accent);
	f32 frac = s->current >= s->count ? 1 : (done + ((still || tick % 2) ? 0.5f : 0.25f)) / (f32)s->count;
	setup_segments(ui, rect(foot.x, foot.y + 7 * p + 12, foot.w, 5 * p), frac, s->count, false);
	if (s->current < s->count)
		wake_for_tick(ui);

	/* the page, sliding in from the side it comes from */
	f32 k = setup_ease((f32)((ui->now - s->changed_at) / 0.42));
	if (k < 1)
		ui->redraw = true;
	f32 dx = (1 - k) * 48 * (f32)(s->dir ? s->dir : 1);
	Rect page = pad4(pane, 22, 34, 22, 34);
	/* over it, a line that says where you are, after blocks in the name's colours */
	Rect kick = cut_top(&page, 30);
	ui_push_clip(ui, fr.pane);
	f32 e = (f32)(ui->now - s->changed_at);
	const HiveTheme *h = theme_worn();
	int m = mark_of(h);
	f32 bx = snapf(ui, kick.x + dx), by = snapf(ui, kick.y + 2 * p), bs = 3 * p;
	for (int i = 0; i < 5; i++) {
		if (!still && e < 0.1f + 0.05f * (f32)i) {
			ui->redraw = true;
			continue;
		}
		ui_fill(ui, rect(bx + (f32)i * (bs + p), by, bs, bs), color_hex(MARKS[m].band[i]));
	}
	char line[64];
	if (s->current < s->count)
		fmt(line, sizeof line, "%02d / %s", s->current + 1, s->steps[s->current]);
	else
		str_copy(line, "FINISHED", sizeof line);
	f32 lx = bx + 5 * (bs + p) + 3 * p;
	ui_pixel_text(ui, lx, snapf(ui, by - 2 * p), line, p, color_with_alpha(t->text, 0x90));
	ui_pop_clip(ui);
	page.x += dx;
	return page;
}

/* The page's title and what it's about; each line arrives a moment after
 * the one before. Returns how far the page's entrance is (0..1). */
f32 setup_title(Ui *ui, Rect *r, f64 changed_at, const char *title, const char *about)
{
	Theme *t = &ui->theme;
	f32 e = (f32)(ui->now - changed_at);
	f32 a = setup_ease(e / 0.45f), b = setup_ease((e - 0.06f) / 0.45f);
	Font tf = ui_font(ui, 32, 800), af = ui_font(ui, 15, 400);
	Rect tr = cut_top(r, 42);
	tr.y += (1 - a) * 14;
	ui_text_in(ui, tf, tr, title, t->text, ALIGN_START);
	cut_top(r, 6);
	f32 h = about ? ui_text_height(ui, af, about, minf(r->w, 620)) : 0;
	Rect ar = cut_top(r, h);
	ar.y += (1 - b) * 18;
	ar.w = minf(ar.w, 620);
	if (about)
		ui_text_wrapped(ui, af, ar, about, t->text_dim, ALIGN_START);
	cut_top(r, 24);
	return setup_ease(e / 0.6f);
}

/* An item's entrance in a list that arrives one after another. */
Rect setup_rise(Ui *ui, f64 changed_at, int index, Rect r)
{
	f32 k = setup_ease((f32)((ui->now - changed_at - 0.08 - 0.06 * index) / 0.5));
	if (k < 1)
		ui->redraw = true;
	r.y += (1 - k) * 22;
	return r;
}

/* A choice made big: an icon on a tinted block, a title, a sentence. */
bool setup_choice(Ui *ui, Rect r, const char *id, const char *icon, const char *title, const char *text, bool selected, bool enabled,
		  Color tint)
{
	Theme *t = &ui->theme;
	UiInteract it = ui_interact(ui, ui_id(ui, id), r);
	f32 hover = ui_fade(ui, ui_id(ui, id), it.hovered && enabled, 0.15f);
	if (it.hovered && enabled)
		ui_set_cursor(ui, CURSOR_POINTER);
	f32 p = unit(ui);
	Rect lift = r;
	lift.y -= roundf(hover * 2) * p;
	if (enabled)
		setup_block(ui, rect(r.x + 2 * p, r.y + 2 * p, r.w, r.h), 2, color_hex("#00000050"));
	setup_card(ui, lift, selected, hover);
	Rect c = pad2(lift, 20, 18);
	Rect ic = snap(ui, rect(c.x, c.y, 48, 48));
	cut_top(&c, 48);
	Color tc = enabled ? tint : t->text_faint;
	setup_block(ui, ic, 2, color_with_alpha(tc, 0x2a));
	outline(ui, ic, 2, color_with_alpha(tc, 0x70));
	ui_icon(ui, rect(ic.x + 12, ic.y + 12, 24, 24), icon, tc);
	if (selected) {
		Rect b = snap(ui, rect(lift.x + lift.w - 20 - 11 * p, lift.y + 18, 11 * p, 9 * p));
		setup_block(ui, b, 1, t->accent);
		pattern(ui, b.x + 2 * p, b.y + 2 * p, TICK, 5, p, t->on_accent);
	}
	cut_top(&c, 14);
	ui_text_in(ui, ui_font(ui, 17, 800), cut_top(&c, 24), title, enabled ? t->text : t->text_faint, ALIGN_START);
	cut_top(&c, 4);
	ui_text_wrapped(ui, ui_font(ui, 13, 400), c, text, enabled ? t->text_dim : t->text_faint, ALIGN_START);
	return it.clicked && enabled;
}

/* The end, in blocks: a badge that builds itself in the ordered pattern,
 * a tick (or a !) that draws itself across it, and sparks flying out. */
static const char *const BIG_TICK[7] = { "........XX", ".......XXX", "XX....XXX.", "XXX..XXX..", ".XXXXXX...", "..XXXX....", "...XX....." };
static const char *const BIG_BANG[9] = { "XX", "XX", "XX", "XX", "XX", "XX", "..", "XX", "XX" };

void setup_tick(Ui *ui, Rect r, f64 since, bool ok)
{
	Theme *t = &ui->theme;
	Color c = ok ? t->success : t->danger;
	bool still = t->reduce_motion;
	f32 e = still ? 9 : (f32)(ui->now - since);
	const int N = 16;
	f32 cell = maxf(1, floorf(minf(r.w, r.h) / (f32)N * ui->scale)) / ui->scale;
	f32 x = snapf(ui, r.x), y = snapf(ui, r.y);
	for (int gy = 0; gy < N; gy++) {
		for (int gx = 0; gx < N; gx++) {
			int dx = MIN(gx, N - 1 - gx), dy = MIN(gy, N - 1 - gy);
			if (dx + dy < 3)
				continue;
			f32 at = e - block_turn(gx, gy) * 0.45f;
			if (at < 0)
				continue;
			Color cc = c;
			bool rim = dx + dy == 3 || dx == 0 || dy == 0;
			if (rim)
				cc = (gx < N / 2 && gy < N / 2) || gy == 0 || gx == 0 ? color_mix(c, COLOR_WHITE, 0.35f) : color_mix(c, COLOR_BLACK, 0.3f);
			if (at < 0.1f)
				cc = color_mix(cc, COLOR_WHITE, 1 - at / 0.1f);
			ui_fill(ui, rect(x + (f32)gx * cell, y + (f32)gy * cell, cell, cell), cc);
		}
	}
	/* the mark, column by column */
	const char *const *g = ok ? BIG_TICK : BIG_BANG;
	int rows = ok ? 7 : 9, cols = ok ? 10 : 2, ox = ok ? 3 : 7, oy = ok ? 4 : 3;
	for (int gy = 0; gy < rows; gy++)
		for (int gx = 0; gx < cols; gx++)
			if (g[gy][gx] == 'X' && e > 0.45f + (f32)gx * 0.03f)
				ui_fill(ui, rect(x + (f32)(ox + gx) * cell, y + (f32)(oy + gy) * cell, cell, cell), COLOR_WHITE);
	/* sparks, in steps */
	int step = (int)((e - 0.5f) * 12);
	if (step >= 0 && step < 8) {
		f32 cx = x + (f32)N * cell / 2, cy = y + (f32)N * cell / 2;
		for (int j = 0; j < 8; j++) {
			f32 a = (f32)j * (PI / 4) + 0.39f;
			f32 d = ((f32)N / 2 + 1 + (f32)step * 1.4f) * cell;
			f32 sx = snapf(ui, cx + cosf(a) * d), sy = snapf(ui, cy + sinf(a) * d);
			Color sc = j % 2 ? t->accent : (j % 4 ? COLOR_WHITE : c);
			ui_fill(ui, rect(sx, sy, cell, cell), color_with_alpha(sc, (u8)(0xff - step * 0x1c)));
		}
	}
	if (e < 1.3f)
		ui->redraw = true;
}
