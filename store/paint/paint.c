/* Paint: brushes, shapes, a paint bucket and a color picker on one picture.
 * Strokes draw straight into the picture; shapes show as a preview while
 * you drag and land when you let go. Every change can be undone. */
#include "zui.h"

enum { T_BRUSH, T_ERASER, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_PICK, T_COUNT };

static const struct {
	const char *name, *key;
	u32 code;
} TOOLS[T_COUNT] = {
	{ "Brush", "B", KEY_B }, { "Eraser", "E", KEY_E }, { "Line", "L", KEY_L }, { "Rectangle", "R", KEY_R },
	{ "Ellipse", "O", KEY_O }, { "Fill", "F", KEY_F }, { "Pick a color", "I", KEY_I },
};

static const u32 PALETTE[] = {
	0xff1d1d1f, 0xff5c5f66, 0xff9aa0a8, 0xffffffff, 0xffc62828, 0xffef6c00, 0xfff9a825, 0xfffdd835,
	0xff2e7d32, 0xff66bb6a, 0xff00897b, 0xff26c6da, 0xff1565c0, 0xff42a5f5, 0xff5e35b1, 0xffab47bc,
	0xffd81b60, 0xfff48fb1, 0xff6d4c41, 0xffbcaaa4,
};

#define MAX_UNDO 30

static struct {
	Canvas *img;
	Canvas *undo[MAX_UNDO], *redo[MAX_UNDO];
	int nundo, nredo;
	int tool;
	Color color;
	f32 size;
	bool fill_shapes;
	/* a drag in progress */
	bool drawing;
	f32 sx, sy, lx, ly;      /* start and last point, in picture pixels */
	char path[512];
	bool modified;
	f32 zoom;                /* 0 = fit */
	f32 hue, sat, light;     /* the custom color */
} P;

/* ---- Undo ------------------------------------------------------------------------------------ */

static Canvas *copy_of(const Canvas *c)
{
	Canvas *n = canvas_new(c->width, c->height);
	copy_pixels(n, c, 0, 0, 0, 0, c->width, c->height);
	return n;
}

static void push(Canvas **stack, int *n, Canvas *c)
{
	if (*n == MAX_UNDO) {
		canvas_free(stack[0]);
		memmove(stack, stack + 1, sizeof(Canvas *) * (MAX_UNDO - 1));
		(*n)--;
	}
	stack[(*n)++] = c;
}

static void remember(void)
{
	push(P.undo, &P.nundo, copy_of(P.img));
	while (P.nredo)
		canvas_free(P.redo[--P.nredo]);
	P.modified = true;
}

static void undo(void)
{
	if (!P.nundo)
		return;
	push(P.redo, &P.nredo, P.img);
	P.img = P.undo[--P.nundo];
	P.modified = true;
}

static void redo(void)
{
	if (!P.nredo)
		return;
	push(P.undo, &P.nundo, P.img);
	P.img = P.redo[--P.nredo];
	P.modified = true;
}

static void set_picture(Canvas *c, const char *path)
{
	canvas_free(P.img);
	P.img = c;
	while (P.nundo)
		canvas_free(P.undo[--P.nundo]);
	while (P.nredo)
		canvas_free(P.redo[--P.nredo]);
	str_copy(P.path, path ? path : "", sizeof P.path);
	P.modified = false;
	P.zoom = 0;
}

static void new_picture(int w, int h)
{
	Canvas *c = canvas_new(w, h);
	canvas_clear(c, COLOR_WHITE);
	set_picture(c, NULL);
}

/* ---- Drawing into the picture ----------------------------------------------------------------- */

static Color ink(void)
{
	return P.tool == T_ERASER ? COLOR_WHITE : P.color;
}

static void stroke(f32 x0, f32 y0, f32 x1, f32 y1)
{
	Canvas *c = P.img;
	canvas_clip(c, rect(0, 0, (f32)c->width, (f32)c->height));
	draw_line(c, x0, y0, x1, y1, P.size, ink());
	fill_circle(c, x1, y1, P.size / 2, ink());
}

static void ellipse(Canvas *c, Rect r, Color col, bool filled, f32 width)
{
	Path p = { 0 };
	path_ellipse(&p, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, r.h / 2);
	Paint paint = paint_solid(col);
	if (filled) {
		fill_path(c, &p, mat_identity(), &paint, FILL_NONZERO);
	} else {
		StrokeStyle st = { width, CAP_ROUND, JOIN_ROUND, 4 };
		Path out = { 0 };
		path_stroke_outline(&p, mat_identity(), &st, &out);
		fill_path(c, &out, mat_identity(), &paint, FILL_NONZERO);
		path_free(&out);
	}
	path_free(&p);
}

static Rect drag_rect(f32 x0, f32 y0, f32 x1, f32 y1)
{
	return rect(minf(x0, x1), minf(y0, y1), fabsf(x1 - x0), fabsf(y1 - y0));
}

static void shape_into(Canvas *c, f32 x0, f32 y0, f32 x1, f32 y1, f32 scale, f32 ox, f32 oy)
{
	f32 w = P.size * scale;
	Rect r = drag_rect(x0 * scale + ox, y0 * scale + oy, x1 * scale + ox, y1 * scale + oy);
	switch (P.tool) {
	case T_LINE:
		draw_line(c, x0 * scale + ox, y0 * scale + oy, x1 * scale + ox, y1 * scale + oy, w, P.color);
		fill_circle(c, x0 * scale + ox, y0 * scale + oy, w / 2, P.color);
		fill_circle(c, x1 * scale + ox, y1 * scale + oy, w / 2, P.color);
		break;
	case T_RECT:
		if (P.fill_shapes)
			fill_rect(c, r, P.color);
		else
			stroke_round_rect(c, r, 0, w, P.color);
		break;
	case T_ELLIPSE:
		if (r.w > 0.5f && r.h > 0.5f)
			ellipse(c, r, P.color, P.fill_shapes, w);
		break;
	}
}

/* Paint bucket: fills the area of similar color around (x, y). */
static void flood(int x, int y)
{
	Canvas *c = P.img;
	if (x < 0 || y < 0 || x >= c->width || y >= c->height)
		return;
	u32 target = canvas_row(c, y)[x], with = color_premul(P.color);
	if (target == with)
		return;
	#define NEAR(p) (ABS((int)((p) >> 24) - (int)(target >> 24)) + ABS((int)(((p) >> 16) & 255) - (int)((target >> 16) & 255)) + \
			 ABS((int)(((p) >> 8) & 255) - (int)((target >> 8) & 255)) + ABS((int)((p) & 255) - (int)(target & 255)) < 48)
	VEC(u32) stack = { 0 };
	u8 *seen = mem_zalloc((usize)c->width * (usize)c->height);
	vec_push(&stack, (u32)(y * c->width + x));
	while (stack.len) {
		u32 i = vec_pop(&stack);
		int px = (int)(i % (u32)c->width), py = (int)(i / (u32)c->width);
		/* run left and right along the row */
		int l = px, r = px;
		u32 *row = canvas_row(c, py);
		while (l > 0 && !seen[py * c->width + l - 1] && NEAR(row[l - 1]))
			l--;
		while (r < c->width - 1 && !seen[py * c->width + r + 1] && NEAR(row[r + 1]))
			r++;
		for (int k = l; k <= r; k++) {
			if (seen[py * c->width + k] || !NEAR(row[k]))
				continue;
			seen[py * c->width + k] = 1;
			row[k] = with;
			for (int dy = -1; dy <= 1; dy += 2) {
				int ny = py + dy;
				if (ny < 0 || ny >= c->height)
					continue;
				u32 q = canvas_row(c, ny)[k];
				if (!seen[ny * c->width + k] && NEAR(q))
					vec_push(&stack, (u32)(ny * c->width + k));
			}
		}
	}
	#undef NEAR
	mem_free(seen);
	vec_free(&stack);
}

/* ---- Files ----------------------------------------------------------------------------------- */

static void save_to(Ui *ui, const char *path)
{
	char p[512];
	if (str_ends(path, ".png") || str_ends(path, ".PNG"))
		str_copy(p, path, sizeof p);
	else
		fmt(p, sizeof p, "%s.png", path);
	if (png_save(P.img, p)) {
		str_copy(P.path, p, sizeof P.path);
		P.modified = false;
	} else {
		ui_notify(ui, "Couldn't save the picture", p, "paint");
	}
}

/* ---- Window ------------------------------------------------------------------------------------ */

/* Tool pictures, drawn here so each says what it does. */
static void tool_icon(Ui *ui, Rect r, int tool, Color c)
{
	f32 x = r.x, y = r.y, w = r.w;
	switch (tool) {
	case T_BRUSH:
		ui_line(ui, x + w * 0.78f, y + w * 0.18f, x + w * 0.42f, y + w * 0.56f, 2.2f, c);
		ui_circle(ui, x + w * 0.32f, y + w * 0.68f, w * 0.13f, c);
		ui_line(ui, x + w * 0.22f, y + w * 0.82f, x + w * 0.14f, y + w * 0.9f, 2, c);
		break;
	case T_ERASER: {
		Path p = { 0 };
		path_move(&p, x + w * 0.2f, y + w * 0.62f);
		path_line(&p, x + w * 0.55f, y + w * 0.22f);
		path_line(&p, x + w * 0.85f, y + w * 0.48f);
		path_line(&p, x + w * 0.5f, y + w * 0.88f);
		path_close(&p);
		ui_path_stroke(ui, &p, 1.8f, c);
		path_free(&p);
		ui_line(ui, x + w * 0.36f, y + w * 0.44f, x + w * 0.66f, y + w * 0.7f, 1.8f, c);
		ui_line(ui, x + w * 0.5f, y + w * 0.88f, x + w * 0.9f, y + w * 0.88f, 1.5f, c);
		break;
	}
	case T_LINE:
		ui_line(ui, x + w * 0.2f, y + w * 0.8f, x + w * 0.8f, y + w * 0.2f, 2, c);
		ui_circle(ui, x + w * 0.2f, y + w * 0.8f, 2.2f, c);
		ui_circle(ui, x + w * 0.8f, y + w * 0.2f, 2.2f, c);
		break;
	case T_RECT:
		ui_border(ui, rect(x + w * 0.16f, y + w * 0.26f, w * 0.68f, w * 0.48f), 1.5f, 2, c);
		break;
	case T_ELLIPSE: {
		Path p = { 0 };
		path_ellipse(&p, x + w * 0.5f, y + w * 0.5f, w * 0.36f, w * 0.26f);
		ui_path_stroke(ui, &p, 2, c);
		path_free(&p);
		break;
	}
	case T_FILL: {
		Path p = { 0 };
		path_move(&p, x + w * 0.18f, y + w * 0.42f);
		path_line(&p, x + w * 0.46f, y + w * 0.16f);
		path_line(&p, x + w * 0.74f, y + w * 0.44f);
		path_line(&p, x + w * 0.46f, y + w * 0.72f);
		path_close(&p);
		ui_path_stroke(ui, &p, 1.8f, c);
		path_free(&p);
		ui_circle(ui, x + w * 0.8f, y + w * 0.72f, w * 0.09f, c);
		break;
	}
	case T_PICK:
		ui_line(ui, x + w * 0.2f, y + w * 0.8f, x + w * 0.62f, y + w * 0.38f, 2, c);
		ui_circle(ui, x + w * 0.7f, y + w * 0.3f, w * 0.14f, c);
		ui_circle(ui, x + w * 0.18f, y + w * 0.82f, 1.6f, c);
		break;
	}
}

static void tool_bar(Ui *ui, Rect bar)
{
	Theme *t = &ui->theme;
	ui_fill(ui, bar, t->sidebar);
	ui_fill(ui, rect(bar.x + bar.w - 1, bar.y, 1, bar.h), t->border);
	Rect c = pad(bar, 10);
	for (int i = 0; i < T_COUNT; i++) {
		char tip[48];
		fmt(tip, sizeof tip, "%s (%s)", TOOLS[i].name, TOOLS[i].key);
		Rect b = cut_top(&c, 40);
		ui_push_id(ui, i);
		UiId id = ui_id(ui, "tool");
		UiInteract in = ui_interact(ui, id, b);
		ui_pop_id(ui);
		bool on = P.tool == i;
		if (on)
			ui_round(ui, b, 8, color_with_alpha(t->accent, 0x30));
		else if (in.hovered)
			ui_round(ui, b, 8, t->hover);
		tool_icon(ui, center_in(b, 22, 22), i, on ? t->accent : t->text);
		if (in.hovered)
			ui_tooltip(ui, tip);
		if (in.clicked)
			P.tool = i;
		cut_top(&c, 4);
	}
}

static void options_bar(Ui *ui, Rect bar)
{
	Theme *t = &ui->theme;
	ui_fill(ui, rect(bar.x, bar.y + bar.h - 1, bar.w, 1), t->border);
	Rect c = pad2(bar, 14, 9);
	/* current color, then the palette */
	Rect cur = cut_left(&c, 34);
	ui_round(ui, cur, 8, P.color);
	ui_border(ui, cur, 8, 1, t->border_strong);
	UiInteract ci = ui_interact(ui, ui_id(ui, "custom"), cur);
	if (ci.clicked)
		ui_popover_toggle(ui, "colors");
	ui_tooltip(ui, "Mix a color");
	cut_left(&c, 12);
	f32 sw = 22;
	for (usize i = 0; i < ARRAY_LEN(PALETTE); i++) {
		Rect s = cut_left(&c, sw);
		Rect sq = center_in(s, 20, 20);
		ui_push_id(ui, (i64)i);
		UiInteract in = ui_interact(ui, ui_id(ui, "sw"), sq);
		ui_pop_id(ui);
		ui_round(ui, sq, 5, PALETTE[i]);
		ui_border(ui, sq, 5, 1, PALETTE[i] == P.color ? t->accent : t->border);
		if (in.clicked)
			P.color = PALETTE[i];
		cut_left(&c, 3);
	}
	cut_left(&c, 16);
	ui_text_in(ui, ui_font(ui, 12, 600), cut_left(&c, 30), "Size", t->text_dim, ALIGN_START);
	ui_slider(ui, cut_left(&c, 140), &P.size, 1, 80);
	char sz[16];
	fmt(sz, sizeof sz, "%d", (int)P.size);
	ui_text_in(ui, ui_font(ui, 12, 600), cut_left(&c, 34), sz, t->text, ALIGN_CENTER);
	if (P.tool == T_RECT || P.tool == T_ELLIPSE) {
		cut_left(&c, 8);
		ui_checkbox(ui, cut_left(&c, 90), "Filled", &P.fill_shapes);
	}
	Rect pc;
	if (ui_popover_begin(ui, "colors", cur, 240, 150, &pc)) {
		cut_top(&pc, 4);
		ui_text(ui, ui_font(ui, 11, 700), pc.x, pc.y + 10, "HUE", t->text_dim);
		cut_top(&pc, 16);
		bool ch = ui_slider(ui, cut_top(&pc, 24), &P.hue, 0, 360);
		ui_text(ui, ui_font(ui, 11, 700), pc.x, pc.y + 14, "SATURATION", t->text_dim);
		cut_top(&pc, 20);
		ch |= ui_slider(ui, cut_top(&pc, 24), &P.sat, 0, 1);
		ui_text(ui, ui_font(ui, 11, 700), pc.x, pc.y + 14, "LIGHTNESS", t->text_dim);
		cut_top(&pc, 20);
		ch |= ui_slider(ui, cut_top(&pc, 24), &P.light, 0, 1);
		if (ch)
			P.color = color_hsl(P.hue, P.sat, P.light, 255);
		ui_popover_end(ui);
	}
}

static void canvas_area(Ui *ui, Rect area)
{
	Theme *t = &ui->theme;
	ui_fill(ui, area, t->dark ? 0xff1c1d20 : 0xffd6d9de);
	f32 fit = minf((area.w - 40) / (f32)P.img->width, (area.h - 40) / (f32)P.img->height);
	f32 z = P.zoom > 0 ? P.zoom : minf(fit, 1);
	f32 w = (f32)P.img->width * z, h = (f32)P.img->height * z;
	Rect view = rect(area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h);
	ui_push_clip(ui, area);
	ui_shadow(ui, view, 0, 16, 4, 0x40000000);
	Rect dev = ui_to_device(ui, view);
	blit_scaled(ui->canvas, P.img, rect(0, 0, (f32)P.img->width, (f32)P.img->height), dev, 1);
	UiInteract in = ui_interact(ui, ui_id(ui, "canvas"), area);
	f32 px = (ui->mx - view.x) / z, py = (ui->my - view.y) / z;
	if (in.hovered)
		ui_set_cursor(ui, P.tool == T_PICK || P.tool == T_FILL ? CURSOR_POINTER : CURSOR_CROSSHAIR);
	if (in.pressed_now) {
		if (P.tool == T_PICK) {
			int x = (int)px, y = (int)py;
			if (x >= 0 && y >= 0 && x < P.img->width && y < P.img->height)
				P.color = color_with_alpha(color_unpremul(canvas_row(P.img, y)[x]), 255);
			P.tool = T_BRUSH;
		} else if (P.tool == T_FILL) {
			remember();
			flood((int)px, (int)py);
		} else {
			remember();
			P.drawing = true;
			P.sx = P.lx = px, P.sy = P.ly = py;
			if (P.tool == T_BRUSH || P.tool == T_ERASER)
				stroke(px, py, px, py);
		}
	}
	if (P.drawing && in.held) {
		if (P.tool == T_BRUSH || P.tool == T_ERASER) {
			if (px != P.lx || py != P.ly)
				stroke(P.lx, P.ly, px, py);
		}
		P.lx = px, P.ly = py;
	}
	if (P.drawing && !in.held) {
		if (P.tool != T_BRUSH && P.tool != T_ERASER)
			shape_into(P.img, P.sx, P.sy, P.lx, P.ly, 1, 0, 0);
		P.drawing = false;
	}
	/* shape preview, drawn on the screen only */
	if (P.drawing && P.tool != T_BRUSH && P.tool != T_ERASER)
		shape_into(ui->canvas, P.sx, P.sy, P.lx, P.ly, z * ui->scale, dev.x, dev.y);
	/* the brush outline under the pointer */
	if (in.hovered && (P.tool == T_BRUSH || P.tool == T_ERASER))
		ui_ring(ui, ui->mx, ui->my, maxf(2, P.size * z / 2), 1, 0x99000000);
	ui_pop_clip(ui);
	/* zoom with Ctrl + wheel */
	if (in.hovered && ui->scroll_dy != 0 && (ui->mods & MOD_CTRL)) {
		P.zoom = clampf(z * (ui->scroll_dy < 0 ? 1.1f : 1 / 1.1f), 0.1f, 8);
		ui->scroll_dy = 0;
	}
	char zl[16];
	fmt(zl, sizeof zl, "%d%%", (int)roundf(z * 100));
	Rect badge = rect(area.x + area.w - 70, area.y + area.h - 34, 60, 24);
	ui_round(ui, badge, 12, 0x99000000);
	ui_text_in(ui, ui_font(ui, 12, 600), badge, zl, COLOR_WHITE, ALIGN_CENTER);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 48);
	char title[160];
	const char *name = P.path[0] ? path_base(P.path) : "Untitled";
	fmt(title, sizeof title, "%s%s", P.modified ? "• " : "", name);
	Rect hb = ui_headerbar(ui, header, title, 0);
	hb = pad2(hb, 4, 9);
	if (ui_icon_button(ui, cut_left(&hb, 32), "document-open", "Open (Ctrl+O)") || ui_key_pressed(ui, KEY_O, MOD_CTRL))
		ui_file_dialog_open(ui, "open", FILE_OPEN, NULL, "png,jpg,jpeg,bmp,gif", NULL);
	if (ui_icon_button(ui, cut_left(&hb, 32), "document-save", "Save (Ctrl+S)") || ui_key_pressed(ui, KEY_S, MOD_CTRL)) {
		if (P.path[0] && str_ends(P.path, ".png"))
			save_to(ui, P.path);
		else
			ui_file_dialog_open(ui, "save", FILE_SAVE, NULL, "png", P.path[0] ? path_base(P.path) : "Drawing.png");
	}
	if (ui_icon_button(ui, cut_left(&hb, 32), "add", "New Picture (Ctrl+N)") || ui_key_pressed(ui, KEY_N, MOD_CTRL))
		new_picture(1280, 800);
	if (ui_icon_button(ui, cut_right(&hb, 32), "edit-redo", "Redo (Ctrl+Shift+Z)") || ui_key_pressed(ui, KEY_Z, MOD_CTRL | MOD_SHIFT) ||
	    ui_key_pressed(ui, KEY_Y, MOD_CTRL))
		redo();
	if (ui_icon_button(ui, cut_right(&hb, 32), "edit-undo", "Undo (Ctrl+Z)") || ui_key_pressed(ui, KEY_Z, MOD_CTRL))
		undo();
	char path[512];
	if (ui_file_dialog(ui, "open", path, sizeof path)) {
		Canvas *c = image_load(path);
		if (c)
			set_picture(c, path);
		else
			ui_notify(ui, "Couldn't open the picture", path_base(path), "paint");
	}
	if (ui_file_dialog(ui, "save", path, sizeof path))
		save_to(ui, path);
	Rect tools = cut_left(&win, 60);
	tool_bar(ui, tools);
	options_bar(ui, cut_top(&win, 52));
	canvas_area(ui, win);
	if (!ui->focus) {
		for (int i = 0; i < T_COUNT; i++)
			if (ui_key_pressed(ui, TOOLS[i].code, 0))
				P.tool = i;
		if (ui_key_pressed(ui, KEY_LEFTBRACE, 0))
			P.size = maxf(1, P.size - 2);
		if (ui_key_pressed(ui, KEY_RIGHTBRACE, 0))
			P.size = minf(80, P.size + 2);
		if (ui_key_pressed(ui, KEY_0, MOD_CTRL))
			P.zoom = 0;
	}
}

static char *open_arg;

static void init(Ui *ui)
{
	UNUSED(ui);
	P.color = PALETTE[12];
	P.size = 8;
	P.hue = 210, P.sat = 0.7f, P.light = 0.45f;
	Canvas *c = open_arg ? image_load(open_arg) : NULL;
	if (c)
		set_picture(c, open_arg);
	else
		new_picture(1280, 800);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		open_arg = argv[1];
	UiAppDesc d = { .id = "paint", .title = "Paint", .width = 1100, .height = 760, .min_width = 700, .min_height = 480,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
