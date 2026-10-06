/* Toolkit core: ids, state, animation, input, layout, drawing and icons. */
#include "zui.h"
#include "zui/driver.h"

/* ---- Paths ------------------------------------------------------------------------ */

const char *zurbax_root(void)
{
	const char *r = env_get("ZURBAX_ROOT");
	return r && *r ? r : "/system";
}

const char *zurbax_data(void)
{
	const char *r = env_get("ZURBAX_DATA");
	return r && *r ? r : "/data";
}

char *zurbax_path(const char *rel)
{
	return path_join(zurbax_root(), rel);
}

/* ---- Per-widget state ----------------------------------------------------------------- */

typedef struct {
	UiId key;
	void *value;
} StateSlot;

struct UiStateMap {
	StateSlot *slots;
	usize cap, len;
};

static StateSlot *state_find(UiStateMap *m, UiId key)
{
	usize mask = m->cap - 1;
	for (usize i = (usize)(key * 0x9e3779b97f4a7c15ull >> 20) & mask;; i = (i + 1) & mask)
		if (m->slots[i].key == key || m->slots[i].key == 0)
			return &m->slots[i];
}

void *ui_state(Ui *ui, UiId id, usize size)
{
	UiStateMap *m = ui->state;
	if (!id)
		id = 1;
	if ((m->len + 1) * 4 >= m->cap * 3) {
		StateSlot *old = m->slots;
		usize old_cap = m->cap;
		m->cap = m->cap ? m->cap * 2 : 256;
		m->slots = NEW_ARRAY(StateSlot, m->cap);
		for (usize i = 0; i < old_cap; i++)
			if (old[i].key)
				*state_find(m, old[i].key) = old[i];
		mem_free(old);
	}
	StateSlot *s = state_find(m, id);
	if (!s->key) {
		s->key = id;
		s->value = mem_zalloc(MAX(size, 8u));
		m->len++;
	}
	return s->value;
}

/* ---- Ids ---------------------------------------------------------------------------- */

static UiId mix(UiId seed, const void *data, usize n)
{
	const u8 *p = data;
	UiId h = seed ^ 0xcbf29ce484222325ull;
	for (usize i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h ? h : 1;
}

static UiId scope(Ui *ui)
{
	return ui->id_depth ? ui->id_stack[ui->id_depth - 1] : 0x5a55ull;
}

UiId ui_id(Ui *ui, const char *label)
{
	return mix(scope(ui), label, strlen(label));
}

UiId ui_id_int(Ui *ui, i64 n)
{
	return mix(scope(ui), &n, sizeof n);
}

void ui_push_id(Ui *ui, i64 n)
{
	UiId id = ui_id_int(ui, n);
	if (ui->id_depth < (int)ARRAY_LEN(ui->id_stack))
		ui->id_stack[ui->id_depth++] = id;
}

void ui_push_id_str(Ui *ui, const char *s)
{
	UiId id = ui_id(ui, s);
	if (ui->id_depth < (int)ARRAY_LEN(ui->id_stack))
		ui->id_stack[ui->id_depth++] = id;
}

void ui_pop_id(Ui *ui)
{
	if (ui->id_depth > 0)
		ui->id_depth--;
}

/* ---- Animation ------------------------------------------------------------------------ */

typedef struct {
	f32 value;
	bool init;
} AnimState;

f32 ui_anim(Ui *ui, UiId id, f32 target, f32 speed)
{
	AnimState *a = ui_state(ui, mix(id, "anim", 4), sizeof *a);
	if (!a->init || ui->theme.reduce_motion) {
		a->value = target;
		a->init = true;
		return target;
	}
	f32 k = 1 - expf(-speed * clampf(ui->dt, 0, 0.1f));
	a->value += (target - a->value) * k;
	if (fabsf(target - a->value) < 0.002f)
		a->value = target;
	else
		ui->redraw = true;
	return a->value;
}

f32 ui_fade(Ui *ui, UiId id, bool on, f32 seconds)
{
	AnimState *a = ui_state(ui, mix(id, "fade", 4), sizeof *a);
	f32 target = on ? 1.0f : 0.0f;
	if (!a->init || ui->theme.reduce_motion || seconds <= 0) {
		a->value = target;
		a->init = true;
		return target;
	}
	f32 step = clampf(ui->dt, 0, 0.1f) / seconds;
	if (a->value < target)
		a->value = minf(target, a->value + step);
	else if (a->value > target)
		a->value = maxf(target, a->value - step);
	if (a->value != target)
		ui->redraw = true;
	return a->value;
}

/* ---- Input ------------------------------------------------------------------------------ */

bool ui_mouse_in(Ui *ui, Rect r)
{
	if (!ui->pointer_inside)
		return false;
	f32 x = ui->mx, y = ui->my;
	Rect abs = { r.x + ui->ox, r.y + ui->oy, r.w, r.h };
	if (!rect_contains(abs, x, y) || !rect_contains(ui->clip, x, y))
		return false;
	/* Widgets beneath an open popover or dialog do not see the pointer. */
	if (!ui->in_overlay && !rect_empty(ui->overlay_rect_prev) && rect_contains(ui->overlay_rect_prev, x, y))
		return false;
	return true;
}

UiInteract ui_interact(Ui *ui, UiId id, Rect r)
{
	UiInteract it = { 0 };
	ui->last_id = id;
	ui->last_rect = (Rect){ r.x + ui->ox, r.y + ui->oy, r.w, r.h };
	bool inside = ui_mouse_in(ui, r);
	if (inside) {
		ui->hot = id;
		it.hovered = true;
	}
	if (inside && ui->pressed[BUTTON_LEFT]) {
		ui->active = id;
		it.pressed_now = true;
	}
	if (ui->active == id) {
		it.held = ui->down[BUTTON_LEFT];
		if (ui->released[BUTTON_LEFT]) {
			if (inside) {
				it.clicked = true;
				it.double_clicked = ui->click_count >= 2;
			}
			ui->active = 0;
			it.held = false;
		}
	}
	if (inside && ui->released[BUTTON_RIGHT])
		it.right_clicked = true;
	/* A click usually changes state that was already drawn this frame. */
	if (it.clicked || it.right_clicked || it.pressed_now)
		ui->redraw = true;
	return it;
}

bool ui_key_pressed(Ui *ui, u32 code, u32 mods)
{
	for (int i = 0; i < ui->nkeys; i++) {
		UiKey *k = &ui->keys[i];
		if (k->pressed && k->code == code && (k->mods & ~(u32)MOD_CAPS) == mods) {
			k->code = 0; /* consumed */
			k->text[0] = 0;
			return true;
		}
	}
	return false;
}

void ui_set_cursor(Ui *ui, u32 shape)
{
	ui->cursor = shape;
}

void ui_request_frame(Ui *ui)
{
	ui->redraw = true;
}

/* ---- Layout --------------------------------------------------------------------------- */

Rect cut_top(Rect *r, f32 h)
{
	h = minf(h, r->h);
	Rect out = { r->x, r->y, r->w, h };
	r->y += h;
	r->h -= h;
	return out;
}

Rect cut_bottom(Rect *r, f32 h)
{
	h = minf(h, r->h);
	r->h -= h;
	return (Rect){ r->x, r->y + r->h, r->w, h };
}

Rect cut_left(Rect *r, f32 w)
{
	w = minf(w, r->w);
	Rect out = { r->x, r->y, w, r->h };
	r->x += w;
	r->w -= w;
	return out;
}

Rect cut_right(Rect *r, f32 w)
{
	w = minf(w, r->w);
	r->w -= w;
	return (Rect){ r->x + r->w, r->y, w, r->h };
}

Rect pad(Rect r, f32 p)
{
	return rect_inset(r, p, p);
}

Rect pad2(Rect r, f32 x, f32 y)
{
	return rect_inset(r, x, y);
}

Rect pad4(Rect r, f32 top, f32 right, f32 bottom, f32 left)
{
	return (Rect){ r.x + left, r.y + top, maxf(0, r.w - left - right), maxf(0, r.h - top - bottom) };
}

Rect center_in(Rect o, f32 w, f32 h)
{
	return (Rect){ o.x + (o.w - w) / 2, o.y + (o.h - h) / 2, w, h };
}

Rect grid_cell(Rect a, int cols, int rows, int index, f32 gap)
{
	f32 cw = (a.w - gap * (f32)(cols - 1)) / (f32)cols;
	f32 ch = (a.h - gap * (f32)(rows - 1)) / (f32)rows;
	int cx = index % cols, cy = index / cols;
	return (Rect){ a.x + (f32)cx * (cw + gap), a.y + (f32)cy * (ch + gap), cw, ch };
}

/* ---- Drawing ----------------------------------------------------------------------------- */

static Rect dev(Ui *ui, Rect r)
{
	f32 s = ui->scale;
	return (Rect){ (r.x + ui->ox) * s, (r.y + ui->oy) * s, r.w * s, r.h * s };
}

Rect ui_to_device(Ui *ui, Rect r)
{
	return dev(ui, r);
}

static void apply_clip(Ui *ui)
{
	f32 s = ui->scale;
	Rect c = ui->clip;
	canvas_clip(ui->canvas, (Rect){ floorf(c.x * s), floorf(c.y * s), ceilf(c.w * s), ceilf(c.h * s) });
}

void ui_push_clip(Ui *ui, Rect r)
{
	if (ui->depth < (int)ARRAY_LEN(ui->stack)) {
		ui->stack[ui->depth].ox = ui->ox;
		ui->stack[ui->depth].oy = ui->oy;
		ui->stack[ui->depth].clip = ui->clip;
		ui->stack[ui->depth].canvas = ui->canvas;
		ui->depth++;
	}
	Rect abs = { r.x + ui->ox, r.y + ui->oy, r.w, r.h };
	ui->clip = rect_intersect(ui->clip, abs);
	apply_clip(ui);
}

void ui_pop_clip(Ui *ui)
{
	if (ui->depth > 0) {
		ui->depth--;
		ui->ox = ui->stack[ui->depth].ox;
		ui->oy = ui->stack[ui->depth].oy;
		ui->clip = ui->stack[ui->depth].clip;
		ui->canvas = ui->stack[ui->depth].canvas;
	}
	apply_clip(ui);
}

Font ui_font(Ui *ui, f32 size, int weight)
{
	return font(size * ui->theme.text_scale, weight);
}

Font ui_font_mono(Ui *ui, f32 size, int weight)
{
	return font_mono(size * ui->theme.text_scale, weight);
}

static Font devfont(Ui *ui, Font f)
{
	f.size *= ui->scale;
	return f;
}

void ui_fill(Ui *ui, Rect r, Color c)
{
	fill_rect(ui->canvas, dev(ui, r), c);
}

void ui_round(Ui *ui, Rect r, f32 radius, Color c)
{
	fill_round_rect(ui->canvas, dev(ui, r), radius * ui->scale, c);
}

void ui_round4(Ui *ui, Rect r, f32 tl, f32 tr, f32 br, f32 bl, Color c)
{
	Path p = { 0 };
	f32 s = ui->scale;
	path_round_rect4(&p, dev(ui, r), tl * s, tr * s, br * s, bl * s);
	Paint paint = paint_solid(c);
	fill_path(ui->canvas, &p, mat_identity(), &paint, FILL_NONZERO);
	path_free(&p);
}

void ui_gradient(Ui *ui, Rect r, f32 radius, Color top, Color bottom)
{
	Rect d = dev(ui, r);
	Paint p = paint_linear(0, d.y, 0, d.y + d.h, top, bottom);
	fill_round_rect_paint(ui->canvas, d, radius * ui->scale, &p);
}

void ui_border(Ui *ui, Rect r, f32 radius, f32 width, Color c)
{
	stroke_round_rect(ui->canvas, dev(ui, r), radius * ui->scale, width * ui->scale, c);
}

void ui_circle(Ui *ui, f32 cx, f32 cy, f32 r, Color c)
{
	f32 s = ui->scale;
	fill_circle(ui->canvas, (cx + ui->ox) * s, (cy + ui->oy) * s, r * s, c);
}

void ui_ring(Ui *ui, f32 cx, f32 cy, f32 r, f32 width, Color c)
{
	f32 s = ui->scale;
	stroke_circle(ui->canvas, (cx + ui->ox) * s, (cy + ui->oy) * s, r * s, width * s, c);
}

void ui_arc(Ui *ui, f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 width, Color c)
{
	f32 s = ui->scale;
	Path p = { 0 };
	path_arc(&p, (cx + ui->ox) * s, (cy + ui->oy) * s, r * s, r * s, a0, a1);
	Paint paint = paint_solid(c);
	StrokeStyle st = { width * s, CAP_ROUND, JOIN_ROUND, 4 };
	stroke_path(ui->canvas, &p, mat_identity(), &paint, &st);
	path_free(&p);
}

void ui_line(Ui *ui, f32 x0, f32 y0, f32 x1, f32 y1, f32 width, Color c)
{
	f32 s = ui->scale;
	draw_line(ui->canvas, (x0 + ui->ox) * s, (y0 + ui->oy) * s, (x1 + ui->ox) * s, (y1 + ui->oy) * s, width * s, c);
}

void ui_shadow(Ui *ui, Rect r, f32 radius, f32 blur, f32 dy, Color c)
{
	f32 s = ui->scale;
	draw_shadow(ui->canvas, dev(ui, r), radius * s, blur * s, 0, dy * s, c);
}

void ui_blur(Ui *ui, Rect r, f32 radius)
{
	blur_region(ui->canvas, rect_intersect(dev(ui, r), ui->canvas->clip), radius * ui->scale);
}

void ui_path_fill(Ui *ui, const Path *p, Color c)
{
	Mat m = { ui->scale, 0, 0, ui->scale, ui->ox * ui->scale, ui->oy * ui->scale };
	Paint paint = paint_solid(c);
	fill_path(ui->canvas, p, m, &paint, FILL_NONZERO);
}

void ui_path_stroke(Ui *ui, const Path *p, f32 width, Color c)
{
	Mat m = { ui->scale, 0, 0, ui->scale, ui->ox * ui->scale, ui->oy * ui->scale };
	Paint paint = paint_solid(c);
	StrokeStyle st = { width, CAP_ROUND, JOIN_ROUND, 4 };
	stroke_path(ui->canvas, p, m, &paint, &st);
}

f32 ui_text_n(Ui *ui, Font f, f32 x, f32 baseline, const char *s, usize n, Color c)
{
	f32 sc = ui->scale;
	f32 end = draw_text(ui->canvas, devfont(ui, f), (x + ui->ox) * sc, (baseline + ui->oy) * sc, s, n, c);
	return end / sc - ui->ox;
}

f32 ui_text(Ui *ui, Font f, f32 x, f32 baseline, const char *s, Color c)
{
	return ui_text_n(ui, f, x, baseline, s, strlen(s), c);
}

void ui_text_in(Ui *ui, Font f, Rect r, const char *s, Color c, Align align)
{
	draw_text_in(ui->canvas, devfont(ui, f), dev(ui, r), s, c, align);
}

f32 ui_text_wrapped(Ui *ui, Font f, Rect r, const char *s, Color c, Align align)
{
	return draw_text_wrapped(ui->canvas, devfont(ui, f), dev(ui, r), s, c, align, 1.0f) / ui->scale;
}

f32 ui_text_width(Ui *ui, Font f, const char *s)
{
	return text_width(devfont(ui, f), s, strlen(s)) / ui->scale;
}

f32 ui_text_width_n(Ui *ui, Font f, const char *s, usize n)
{
	return text_width(devfont(ui, f), s, n) / ui->scale;
}

f32 ui_text_height(Ui *ui, Font f, const char *s, f32 width)
{
	return text_wrap_height(devfont(ui, f), s, width * ui->scale, 1.0f) / ui->scale;
}

f32 ui_line_height(Ui *ui, Font f)
{
	UNUSED(ui);
	return font_line_height(f);
}

/* ---- Icons --------------------------------------------------------------------------- */

typedef struct {
	char *key;
	Canvas *img;
} IconEntry;

static Map icon_cache;

static SvgDoc *load_icon_doc(const char *name, bool app)
{
	const char *dirs_sym[] = { "share/icons/symbolic", "share/icons/apps" };
	const char *dirs_app[] = { "share/icons/apps", "share/icons/symbolic" };
	const char **dirs = app ? dirs_app : dirs_sym;
	for (int i = 0; i < 2; i++) {
		char *rel = str_fmt("%s/%s.svg", dirs[i], name);
		char *path = zurbax_path(rel);
		SvgDoc *d = svg_load(path);
		mem_free(rel);
		mem_free(path);
		if (d)
			return d;
	}
	if (app) {
		/* Apps installed from Harbor keep their icon next to the program. */
		char *path = str_fmt("%s/apps/%s/icon.svg", zurbax_data(), name);
		SvgDoc *d = svg_load(path);
		mem_free(path);
		if (d)
			return d;
	}
	return NULL;
}

static const Canvas *icon_image(const char *name, int size, bool app)
{
	char key[160];
	fmt(key, sizeof key, "%c:%s@%d", app ? 'a' : 's', name, size);
	Canvas *img = map_get(&icon_cache, key);
	if (img)
		return img;
	SvgDoc *doc = load_icon_doc(name, app);
	if (!doc && !app)
		doc = load_icon_doc("help", false);
	if (!doc)
		return NULL;
	/* Symbolic icons render in white and are tinted when drawn. */
	img = svg_rasterize(doc, size, size, COLOR_WHITE);
	svg_free(doc);
	map_set(&icon_cache, key, img);
	return img;
}

static void blit_tinted(Canvas *dst, const Canvas *src, int dx, int dy, Color tint)
{
	u32 pc = color_premul(tint);
	Rect clip = rect_intersect(dst->clip, rect(0, 0, (f32)dst->width, (f32)dst->height));
	int x0 = MAX(dx, (int)clip.x), y0 = MAX(dy, (int)clip.y);
	int x1 = MIN(dx + src->width, (int)(clip.x + clip.w)), y1 = MIN(dy + src->height, (int)(clip.y + clip.h));
	for (int y = y0; y < y1; y++) {
		u32 *d = canvas_row(dst, y);
		const u32 *s = canvas_row(src, y - dy) - dx;
		for (int x = x0; x < x1; x++) {
			u32 a = s[x] >> 24;
			if (a)
				d[x] = blend_premul(d[x], pc, a + (a >> 7));
		}
	}
}

void ui_icon(Ui *ui, Rect r, const char *name, Color c)
{
	Rect d = dev(ui, r);
	int size = (int)roundf(minf(d.w, d.h));
	if (size <= 0)
		return;
	const Canvas *img = icon_image(name, size, false);
	if (!img)
		return;
	int x = (int)roundf(d.x + (d.w - (f32)size) / 2), y = (int)roundf(d.y + (d.h - (f32)size) / 2);
	blit_tinted(ui->canvas, img, x, y, c);
}

void ui_app_icon(Ui *ui, Rect r, const char *app_id)
{
	Rect d = dev(ui, r);
	int size = (int)roundf(minf(d.w, d.h));
	if (size <= 0)
		return;
	const Canvas *img = icon_image(app_id, size, true);
	if (!img)
		img = icon_image("zurbax", size, true);
	if (!img)
		return;
	blit(ui->canvas, img, (int)roundf(d.x + (d.w - (f32)size) / 2), (int)roundf(d.y + (d.h - (f32)size) / 2), 1);
}

void ui_image(Ui *ui, Rect r, const Canvas *img, f32 radius, bool cover)
{
	if (!img)
		return;
	Rect d = dev(ui, r);
	f32 iw = (f32)img->width, ih = (f32)img->height;
	Rect src = rect(0, 0, iw, ih), dst = d;
	if (cover) {
		f32 s = maxf(d.w / iw, d.h / ih);
		f32 sw = d.w / s, sh = d.h / s;
		src = rect((iw - sw) / 2, (ih - sh) / 2, sw, sh);
	} else {
		f32 s = minf(d.w / iw, d.h / ih);
		dst = center_in(d, iw * s, ih * s);
	}
	blit_scaled_round(ui->canvas, img, src, dst, radius * ui->scale, 1);
}

/* ---- Context lifecycle ----------------------------------------------------------------- */

Ui *ui_new(void)
{
	Ui *ui = NEW(Ui);
	ui->state = NEW(UiStateMap);
	ui->scale = 1;
	ui->window_focused = true;
	ThemeInfo info = { 0 };
	theme_build(&ui->theme, &info);
	return ui;
}

void ui_free(Ui *ui)
{
	if (!ui)
		return;
	for (usize i = 0; i < ui->state->cap; i++)
		if (ui->state->slots[i].key)
			mem_free(ui->state->slots[i].value);
	mem_free(ui->state->slots);
	mem_free(ui->state);
	canvas_free(ui->overlay);
	mem_free(ui->clipboard_out);
	mem_free(ui->paste_in);
	mem_free(ui->drop_paths);
	mem_free(ui);
}

void ui_attach(Ui *ui, Canvas *canvas, f32 w, f32 h, f32 scale)
{
	ui->canvas = canvas;
	ui->width = w;
	ui->height = h;
	if (scale != ui->scale)
		font_cache_clear();
	ui->scale = scale;
	if (ui->overlay && (ui->overlay->width != canvas->width || ui->overlay->height != canvas->height)) {
		canvas_free(ui->overlay);
		ui->overlay = NULL;
	}
	ui->redraw = true;
}

void ui_set_theme(Ui *ui, const ThemeInfo *info)
{
	theme_build(&ui->theme, info);
	ui->redraw = true;
}

/* ---- Input feed ---------------------------------------------------------------------- */

static f64 last_click_time;
static f32 last_click_x, last_click_y;

void ui_input_pointer(Ui *ui, u32 kind, f32 x, f32 y, u32 button, u32 mods)
{
	ui->mods = mods;
	switch (kind) {
	case POINTER_ENTER:
	case POINTER_MOTION:
		ui->pointer_inside = true;
		ui->mx = x, ui->my = y;
		break;
	case POINTER_LEAVE:
		ui->pointer_inside = false;
		break;
	case POINTER_DOWN:
		ui->mx = x, ui->my = y;
		ui->pointer_inside = true;
		if (button < ARRAY_LEN(ui->down)) {
			ui->down[button] = true;
			ui->pressed[button] = true;
		}
		if (button == BUTTON_LEFT) {
			f64 t = (f64)time_mono_ns() / 1e9;
			bool near = fabsf(x - last_click_x) < 5 && fabsf(y - last_click_y) < 5;
			ui->click_count = (t - last_click_time < 0.4 && near) ? ui->click_count + 1 : 1;
			last_click_time = t;
			last_click_x = x, last_click_y = y;
		}
		break;
	case POINTER_UP:
		ui->mx = x, ui->my = y;
		if (button < ARRAY_LEN(ui->down)) {
			ui->down[button] = false;
			ui->released[button] = true;
		}
		break;
	}
	ui->redraw = true;
}

void ui_input_scroll(Ui *ui, f32 dx, f32 dy, u32 source, u32 phase)
{
	ui->scroll_dx += dx;
	ui->scroll_dy += dy;
	ui->scroll_source = source;
	if (phase)
		ui->scroll_phase = phase;
	ui->redraw = true;
}

bool ui_key_down(Ui *ui, u32 code)
{
	return code < ARRAY_LEN(ui->held) && ui->held[code];
}

void ui_input_key(Ui *ui, u32 code, u32 state, u32 mods, const char *text)
{
	ui->mods = mods;
	if (code < ARRAY_LEN(ui->held))
		ui->held[code] = state != KEY_RELEASED;
	if (state == KEY_RELEASED) {
		ui->redraw = true;
		return;
	}
	if (ui->nkeys < (int)ARRAY_LEN(ui->keys)) {
		UiKey *k = &ui->keys[ui->nkeys++];
		k->code = code;
		k->mods = mods;
		k->pressed = true;
		k->repeat = state == KEY_REPEATED;
		str_copy(k->text, text ? text : "", sizeof k->text);
	}
	ui->redraw = true;
}

void ui_input_focus(Ui *ui, bool focused)
{
	ui->window_focused = focused;
	if (!focused) {
		for (usize i = 0; i < ARRAY_LEN(ui->down); i++)
			ui->down[i] = false;
		memset(ui->held, 0, sizeof ui->held);
	}
	ui->redraw = true;
}

void ui_input_paste(Ui *ui, const char *text)
{
	mem_free(ui->paste_in);
	ui->paste_in = str_dup(text);
	ui->redraw = true;
}

void ui_input_drop(Ui *ui, const char *paths)
{
	mem_free(ui->drop_paths);
	ui->drop_paths = str_dup(paths);
	ui->redraw = true;
}

bool ui_needs_frame(const Ui *ui)
{
	return ui->redraw;
}

/* ---- Frame ------------------------------------------------------------------------------ */

static void draw_tooltip(Ui *ui)
{
	if (!ui->tooltip_text[0] || ui->hot != ui->tooltip_id || ui->down[BUTTON_LEFT])
		return;
	f64 shown = ui->now - ui->tooltip_since;
	if (shown < 0.6) {
		ui->redraw = true;
		return;
	}
	Font f = ui_font(ui, 12, 500);
	f32 w = ui_text_width(ui, f, ui->tooltip_text) + 16, h = 24;
	Rect a = ui->tooltip_anchor;
	f32 x = clampf(a.x + a.w / 2 - w / 2, 4, ui->width - w - 4);
	f32 y = a.y + a.h + 6;
	if (y + h > ui->height - 4)
		y = a.y - h - 6;
	Rect r = { x, y, w, h };
	f32 alpha = clampf((f32)(shown - 0.6) * 6, 0, 1);
	if (alpha < 1)
		ui->redraw = true;
	ui_shadow(ui, r, 6, 6, 2, color_scale_alpha(color_hex("#00000050"), alpha));
	ui_round(ui, r, 6, color_scale_alpha(color_hex("#2b2d33f2"), alpha));
	ui_text_in(ui, f, r, ui->tooltip_text, color_scale_alpha(COLOR_WHITE, alpha), ALIGN_CENTER);
}

bool ui_frame(Ui *ui, void (*draw)(Ui *ui))
{
	f64 now = (f64)time_mono_ns() / 1e9;
	ui->dt = ui->now > 0 ? (f32)(now - ui->now) : 1.0f / 60;
	ui->now = now;
	ui->frame_no++;
	ui->redraw = false;
	ui->wake_at = 0;
	ui->focus_claimed = false;
	ui->scroll_depth = 0;
	ui->cursor = CURSOR_DEFAULT;
	ui->hot = 0;
	ui->ox = ui->oy = 0;
	ui->depth = 0;
	ui->id_depth = 0;
	ui->clip = rect(0, 0, ui->width, ui->height);
	ui->overlay_rect_prev = ui->overlay_rect;
	ui->overlay_rect = rect(0, 0, 0, 0);
	ui->overlay_drawn = false;
	ui->in_overlay = false;
	UiId prev_tooltip = ui->tooltip_id;
	ui->tooltip_text[0] = 0;
	apply_clip(ui);

	draw(ui);

	/* Clicking anywhere that does not take focus drops keyboard focus. */
	if (ui->pressed[BUTTON_LEFT] && !ui->focus_claimed)
		ui->focus = 0;
	if (ui->released[BUTTON_LEFT])
		ui->active = 0;

	ui->depth = 0;
	ui->ox = ui->oy = 0;
	ui->clip = rect(0, 0, ui->width, ui->height);
	apply_clip(ui);
	if (ui->overlay_drawn && ui->overlay)
		blit(ui->canvas, ui->overlay, 0, 0, 1);
	if (ui->tooltip_id != prev_tooltip)
		ui->tooltip_since = ui->now;
	draw_tooltip(ui);

	/* Per-frame input is consumed. */
	memset(ui->pressed, 0, sizeof ui->pressed);
	memset(ui->released, 0, sizeof ui->released);
	ui->scroll_dx = ui->scroll_dy = 0;
	ui->scroll_phase = 0;
	ui->nkeys = 0;
	mem_free(ui->drop_paths);
	ui->drop_paths = NULL;
	mem_free(ui->paste_in);
	ui->paste_in = NULL;
	return ui->redraw;
}
