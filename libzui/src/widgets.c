/* Standard widgets. */
#include "zui.h"

#define CTL_RADIUS 6.0f
#define WINDOW_RADIUS 10.0f

static f32 window_radius(Ui *ui)
{
	return (ui->win_states & (STATE_MAXIMIZED | STATE_FULLSCREEN | STATE_TILED_LEFT | STATE_TILED_RIGHT)) ? 0 : WINDOW_RADIUS;
}

/* ---- Buttons ------------------------------------------------------------------------ */

typedef enum { BTN_NORMAL, BTN_SUGGESTED, BTN_DESTRUCTIVE, BTN_FLAT } ButtonKind;

static void button_frame(Ui *ui, Rect r, ButtonKind kind, UiInteract it, f32 hover)
{
	Theme *t = &ui->theme;
	bool down = it.held && it.hovered;
	if (kind == BTN_FLAT) {
		if (down)
			ui_round(ui, r, CTL_RADIUS, t->press);
		else if (hover > 0)
			ui_round(ui, r, CTL_RADIUS, color_scale_alpha(t->hover, hover));
		return;
	}
	Color top, bottom, border;
	if (kind == BTN_NORMAL) {
		top = t->button_top, bottom = t->button_bottom, border = t->button_border;
	} else {
		Color base = kind == BTN_SUGGESTED ? t->accent : t->danger;
		top = color_lighten(base, 0.1f), bottom = base, border = color_lighten(base, -0.25f);
	}
	if (hover > 0 && !down) {
		top = color_mix(top, color_lighten(top, t->dark ? 0.08f : 0.25f), hover);
		bottom = color_mix(bottom, color_lighten(bottom, t->dark ? 0.08f : 0.2f), hover);
	}
	if (down) {
		Color tmp = top;
		top = color_lighten(bottom, -0.06f);
		bottom = color_lighten(tmp, -0.02f);
	}
	if (!down)
		ui_shadow(ui, rect(r.x, r.y + 0.5f, r.w, r.h), CTL_RADIUS, 1.5f, 0.5f, color_hex(t->dark ? "#00000050" : "#0000001a"));
	ui_gradient(ui, r, CTL_RADIUS, top, bottom);
	ui_border(ui, r, CTL_RADIUS, 1, border);
	if (!down) /* top inner highlight */
		ui_line(ui, r.x + CTL_RADIUS, r.y + 1.5f, r.x + r.w - CTL_RADIUS, r.y + 1.5f, 1,
			color_hex(kind == BTN_NORMAL && !t->dark ? "#ffffffcc" : "#ffffff2a"));
}

static bool button_impl(Ui *ui, Rect r, const char *label, const char *icon, ButtonKind kind)
{
	UiId id = ui_id(ui, label && *label ? label : icon);
	UiInteract it = ui_interact(ui, id, r);
	f32 hover = ui_fade(ui, id, it.hovered, 0.12f);
	if (it.hovered)
		ui_set_cursor(ui, CURSOR_POINTER);
	button_frame(ui, r, kind, it, hover);
	Theme *t = &ui->theme;
	Color fg = (kind == BTN_SUGGESTED || kind == BTN_DESTRUCTIVE) ? (kind == BTN_SUGGESTED ? t->on_accent : COLOR_WHITE) : t->text;
	Font f = ui_font(ui, 13, kind == BTN_FLAT ? 500 : 600);
	f32 dy = it.held && it.hovered ? 0.5f : 0;
	if (icon && label && *label) {
		f32 tw = ui_text_width(ui, f, label);
		f32 total = 16 + 7 + tw;
		f32 x = r.x + (r.w - total) / 2;
		ui_icon(ui, rect(x, r.y + (r.h - 16) / 2 + dy, 16, 16), icon, fg);
		ui_text_in(ui, f, rect(x + 23, r.y + dy, tw + 2, r.h), label, fg, ALIGN_START);
	} else if (icon) {
		ui_icon(ui, rect(r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2 + dy, 16, 16), icon, fg);
	} else {
		ui_text_in(ui, f, rect(r.x + 8, r.y + dy, r.w - 16, r.h), label, fg, ALIGN_CENTER);
	}
	return it.clicked;
}

bool ui_button(Ui *ui, Rect r, const char *label) { return button_impl(ui, r, label, NULL, BTN_NORMAL); }
bool ui_button_suggested(Ui *ui, Rect r, const char *label) { return button_impl(ui, r, label, NULL, BTN_SUGGESTED); }
bool ui_button_destructive(Ui *ui, Rect r, const char *label) { return button_impl(ui, r, label, NULL, BTN_DESTRUCTIVE); }
bool ui_button_flat(Ui *ui, Rect r, const char *label) { return button_impl(ui, r, label, NULL, BTN_FLAT); }
bool ui_button_icon_label(Ui *ui, Rect r, const char *icon, const char *label) { return button_impl(ui, r, label, icon, BTN_NORMAL); }

bool ui_icon_button(Ui *ui, Rect r, const char *icon, const char *tooltip)
{
	UiId id = ui_id(ui, tooltip && *tooltip ? tooltip : icon);
	UiInteract it = ui_interact(ui, id, r);
	f32 hover = ui_fade(ui, id, it.hovered, 0.12f);
	Theme *t = &ui->theme;
	if (it.held && it.hovered)
		ui_round(ui, r, CTL_RADIUS, t->press);
	else if (hover > 0)
		ui_round(ui, r, CTL_RADIUS, color_scale_alpha(t->hover, hover));
	f32 s = minf(18, minf(r.w, r.h) - 8);
	ui_icon(ui, center_in(r, s, s), icon, color_mix(t->text_dim, t->text, hover));
	if (tooltip)
		ui_tooltip(ui, tooltip);
	return it.clicked;
}

bool ui_icon_toggle(Ui *ui, Rect r, const char *icon, bool active, const char *tooltip)
{
	UiId id = ui_id(ui, tooltip && *tooltip ? tooltip : icon);
	UiInteract it = ui_interact(ui, id, r);
	f32 hover = ui_fade(ui, id, it.hovered, 0.12f);
	Theme *t = &ui->theme;
	if (active)
		ui_round(ui, r, CTL_RADIUS, color_with_alpha(t->accent, 0x38));
	else if (hover > 0)
		ui_round(ui, r, CTL_RADIUS, color_scale_alpha(t->hover, hover));
	f32 s = minf(18, minf(r.w, r.h) - 8);
	ui_icon(ui, center_in(r, s, s), icon, active ? t->accent : color_mix(t->text_dim, t->text, hover));
	if (tooltip)
		ui_tooltip(ui, tooltip);
	return it.clicked;
}

/* ---- Toggles ---------------------------------------------------------------------- */

bool ui_switch(Ui *ui, Rect r, bool *on)
{
	UiId id = ui_id_int(ui, (i64)(uintptr_t)on);
	Rect track = center_in(r, 44, 24);
	track.x = r.x + r.w - 44;
	UiInteract it = ui_interact(ui, id, track);
	if (it.clicked)
		*on = !*on;
	if (it.hovered)
		ui_set_cursor(ui, CURSOR_POINTER);
	Theme *t = &ui->theme;
	f32 k = ui_anim(ui, id, *on ? 1.0f : 0.0f, 18);
	Color off = t->dark ? color_hex("#4a4b51") : color_hex("#d3d4d8");
	Color bg = color_mix(off, t->accent, k);
	ui_round(ui, track, 12, bg);
	ui_border(ui, track, 12, 1, color_hex(t->dark ? "#00000060" : "#0000001f"));
	f32 kx = track.x + 2 + k * 20;
	Rect knob = { kx, track.y + 2, 20, 20 };
	ui_shadow(ui, knob, 10, 2, 1, color_hex("#00000045"));
	ui_circle(ui, kx + 10, track.y + 12, 10, COLOR_WHITE);
	ui_ring(ui, kx + 10, track.y + 12, 9.75f, 0.5f, color_hex("#00000022"));
	return it.clicked;
}

bool ui_checkbox(Ui *ui, Rect r, const char *label, bool *checked)
{
	UiId id = ui_id(ui, label);
	UiInteract it = ui_interact(ui, id, r);
	if (it.clicked)
		*checked = !*checked;
	Theme *t = &ui->theme;
	Rect box = { r.x, r.y + (r.h - 18) / 2, 18, 18 };
	f32 k = ui_anim(ui, id, *checked ? 1.0f : 0.0f, 22);
	if (k < 1) {
		ui_round(ui, box, 4, t->input);
		ui_border(ui, box, 4, 1, it.hovered ? t->accent : t->border_strong);
	}
	if (k > 0) {
		ui_round(ui, box, 4, color_scale_alpha(t->accent, k));
		ui_icon(ui, pad(box, 2), "check", color_scale_alpha(t->on_accent, k));
	}
	ui_text_in(ui, ui_font(ui, 13, 400), rect(r.x + 26, r.y, r.w - 26, r.h), label, t->text, ALIGN_START);
	return it.clicked;
}

bool ui_radio(Ui *ui, Rect r, const char *label, bool selected)
{
	UiId id = ui_id(ui, label);
	UiInteract it = ui_interact(ui, id, r);
	Theme *t = &ui->theme;
	f32 cx = r.x + 9, cy = r.y + r.h / 2;
	if (selected) {
		ui_circle(ui, cx, cy, 9, t->accent);
		ui_circle(ui, cx, cy, 3.5f, t->on_accent);
	} else {
		ui_circle(ui, cx, cy, 9, t->input);
		ui_ring(ui, cx, cy, 8.5f, 1, it.hovered ? t->accent : t->border_strong);
	}
	ui_text_in(ui, ui_font(ui, 13, 400), rect(r.x + 26, r.y, r.w - 26, r.h), label, t->text, ALIGN_START);
	return it.clicked;
}

bool ui_slider(Ui *ui, Rect r, f32 *value, f32 min, f32 max)
{
	UiId id = ui_id_int(ui, (i64)(uintptr_t)value);
	UiInteract it = ui_interact(ui, id, r);
	Theme *t = &ui->theme;
	f32 x0 = r.x + 9, x1 = r.x + r.w - 9;
	bool changed = false;
	if (it.held || it.pressed_now) {
		f32 frac = clampf((ui->mx - ui->ox - x0) / (x1 - x0), 0, 1);
		f32 v = min + frac * (max - min);
		changed = v != *value;
		*value = v;
	}
	if (it.hovered && ui->scroll_dy != 0) {
		*value = clampf(*value - ui->scroll_dy / 400 * (max - min), min, max);
		changed = true;
	}
	f32 frac = (max > min) ? clampf((*value - min) / (max - min), 0, 1) : 0;
	f32 cy = r.y + r.h / 2;
	ui_round(ui, rect(x0, cy - 2, x1 - x0, 4), 2, t->dark ? color_hex("#ffffff22") : color_hex("#00000020"));
	ui_round(ui, rect(x0, cy - 2, (x1 - x0) * frac, 4), 2, t->accent);
	f32 kx = x0 + (x1 - x0) * frac;
	ui_shadow(ui, rect(kx - 9, cy - 9, 18, 18), 9, 2.5f, 1, color_hex("#00000040"));
	ui_circle(ui, kx, cy, 9, COLOR_WHITE);
	ui_ring(ui, kx, cy, 8.75f, 0.5f, color_hex("#00000030"));
	if (it.hovered || it.held)
		ui_set_cursor(ui, CURSOR_POINTER);
	return changed;
}

bool ui_segmented(Ui *ui, Rect r, const char *const *items, int n, int *selected)
{
	Theme *t = &ui->theme;
	bool changed = false;
	ui_shadow(ui, r, CTL_RADIUS, 1.5f, 0.5f, color_hex(t->dark ? "#00000050" : "#0000001a"));
	ui_gradient(ui, r, CTL_RADIUS, t->button_top, t->button_bottom);
	f32 w = r.w / (f32)n;
	for (int i = 0; i < n; i++) {
		Rect cell = { r.x + w * (f32)i, r.y, w, r.h };
		ui_push_id(ui, i);
		UiId id = ui_id(ui, items[i]);
		UiInteract it = ui_interact(ui, id, cell);
		ui_pop_id(ui);
		if (it.clicked && *selected != i) {
			*selected = i;
			changed = true;
		}
		if (*selected == i) {
			f32 tl = i == 0 ? CTL_RADIUS : 0, tr = i == n - 1 ? CTL_RADIUS : 0;
			ui_round4(ui, cell, tl, tr, tr, tl, t->dark ? color_hex("#00000040") : color_hex("#0000001a"));
		} else if (it.hovered) {
			f32 tl = i == 0 ? CTL_RADIUS : 0, tr = i == n - 1 ? CTL_RADIUS : 0;
			ui_round4(ui, cell, tl, tr, tr, tl, t->hover);
		}
		if (i > 0)
			ui_vseparator(ui, cell.x, r.y + 1, r.y + r.h - 1);
		ui_text_in(ui, ui_font(ui, 13, *selected == i ? 600 : 500), cell, items[i], t->text, ALIGN_CENTER);
	}
	ui_border(ui, r, CTL_RADIUS, 1, t->button_border);
	return changed;
}

bool ui_dropdown(Ui *ui, Rect r, const char *id, const char *const *items, int n, int *selected)
{
	Theme *t = &ui->theme;
	ui_push_id_str(ui, id);
	UiId bid = ui_id(ui, "button");
	UiInteract it = ui_interact(ui, bid, r);
	f32 hover = ui_fade(ui, bid, it.hovered, 0.12f);
	button_frame(ui, r, BTN_NORMAL, it, hover);
	const char *label = (*selected >= 0 && *selected < n) ? items[*selected] : "";
	ui_text_in(ui, ui_font(ui, 13, 500), rect(r.x + 10, r.y, r.w - 34, r.h), label, t->text, ALIGN_START);
	ui_icon(ui, rect(r.x + r.w - 24, r.y + (r.h - 16) / 2, 16, 16), "pan-down", t->text_dim);
	if (it.clicked)
		ui_popover_toggle(ui, id);
	bool changed = false;
	Rect content;
	f32 h = (f32)n * 32 + 12;
	if (ui_popover_begin(ui, id, r, maxf(r.w, 160), minf(h, 360), &content)) {
		for (int i = 0; i < n; i++) {
			Rect row = { content.x, content.y + (f32)i * 32, content.w, 32 };
			ui_push_id(ui, i);
			if (ui_menu_item(ui, row, i == *selected ? "check" : NULL, items[i], NULL)) {
				changed = *selected != i;
				*selected = i;
			}
			ui_pop_id(ui);
		}
		ui_popover_end(ui);
	}
	ui_pop_id(ui);
	return changed;
}

bool ui_spin(Ui *ui, Rect r, const char *id, int *value, int min, int max)
{
	Theme *t = &ui->theme;
	ui_push_id_str(ui, id);
	bool changed = false;
	ui_round(ui, r, CTL_RADIUS, t->input);
	ui_border(ui, r, CTL_RADIUS, 1, t->border_strong);
	Rect body = r;
	Rect plus = cut_right(&body, 32), minus = cut_right(&body, 32);
	char buf[32];
	fmt(buf, sizeof buf, "%d", *value);
	ui_text_in(ui, ui_font(ui, 13, 500), pad2(body, 10, 0), buf, t->text, ALIGN_START);
	ui_vseparator(ui, minus.x, r.y + 1, r.y + r.h - 1);
	ui_vseparator(ui, plus.x, r.y + 1, r.y + r.h - 1);
	if (ui_icon_button(ui, minus, "remove", NULL) && *value > min) {
		(*value)--;
		changed = true;
	}
	if (ui_icon_button(ui, plus, "add", NULL) && *value < max) {
		(*value)++;
		changed = true;
	}
	ui_pop_id(ui);
	return changed;
}

/* ---- Indicators ------------------------------------------------------------------------ */

void ui_progress(Ui *ui, Rect r, f32 fraction)
{
	Theme *t = &ui->theme;
	f32 h = minf(r.h, 6);
	Rect track = { r.x, r.y + (r.h - h) / 2, r.w, h };
	ui_round(ui, track, h / 2, t->dark ? color_hex("#ffffff1c") : color_hex("#0000001a"));
	f32 w = track.w * clampf(fraction, 0, 1);
	if (w > 0.5f)
		ui_gradient(ui, rect(track.x, track.y, maxf(w, h), h), h / 2, color_lighten(t->accent, 0.15f), t->accent);
}

void ui_spinner(Ui *ui, Rect r)
{
	f32 s = minf(r.w, r.h);
	f32 cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = s / 2 - 2;
	f32 a = (f32)fmod(ui->now * 5.0, (double)TAU_F);
	ui_ring(ui, cx, cy, rad, 2.5f, ui->theme.border);
	ui_arc(ui, cx, cy, rad, a, a + 1.9f, 2.5f, ui->theme.accent);
	ui->redraw = true;
}

void ui_separator(Ui *ui, f32 x0, f32 y, f32 x1)
{
	ui_fill(ui, rect(x0, y, x1 - x0, 1.0f / ui->scale), ui->theme.border);
}

void ui_vseparator(Ui *ui, f32 x, f32 y0, f32 y1)
{
	ui_fill(ui, rect(x, y0, 1.0f / ui->scale, y1 - y0), ui->theme.border);
}

void ui_card(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_shadow(ui, r, 8, 4, 1, color_hex(t->dark ? "#00000055" : "#00000014"));
	ui_round(ui, r, 8, t->card);
	ui_border(ui, r, 8, 1, t->border);
}

void ui_badge(Ui *ui, f32 cx, f32 cy, const char *text)
{
	Font f = ui_font(ui, 11, 700);
	f32 w = maxf(18, ui_text_width(ui, f, text) + 10);
	Rect r = { cx - w / 2, cy - 9, w, 18 };
	ui_round(ui, r, 9, ui->theme.danger);
	ui_text_in(ui, f, r, text, COLOR_WHITE, ALIGN_CENTER);
}

void ui_avatar(Ui *ui, Rect r, const char *name)
{
	f32 s = minf(r.w, r.h);
	f32 cx = r.x + r.w / 2, cy = r.y + r.h / 2;
	u64 h = hash_bytes(name, strlen(name));
	Color base = color_hsl((f32)(h % 360), 0.55f, 0.55f, 255);
	Rect circle = { cx - s / 2, cy - s / 2, s, s };
	ui_gradient(ui, circle, s / 2, color_lighten(base, 0.18f), color_lighten(base, -0.12f));
	char initials[8] = { 0 };
	int n = 0;
	bool start = true;
	for (const char *p = name; *p && n < 2; p++) {
		if (*p == ' ') {
			start = true;
			continue;
		}
		if (start && is_alpha(*p))
			initials[n++] = (char)to_upper(*p);
		start = false;
	}
	if (!n)
		initials[0] = '?';
	ui_text_in(ui, ui_font(ui, s * 0.38f / ui->theme.text_scale, 600), circle, initials, COLOR_WHITE, ALIGN_CENTER);
}

void ui_label(Ui *ui, Rect r, const char *text)
{
	ui_text_in(ui, ui_font(ui, 13, 400), r, text, ui->theme.text, ALIGN_START);
}

void ui_label_dim(Ui *ui, Rect r, const char *text)
{
	ui_text_in(ui, ui_font(ui, 12, 400), r, text, ui->theme.text_dim, ALIGN_START);
}

void ui_heading(Ui *ui, Rect r, const char *text)
{
	ui_text_in(ui, ui_font(ui, 15, 700), r, text, ui->theme.text, ALIGN_START);
}

void ui_title(Ui *ui, Rect r, const char *text)
{
	ui_text_in(ui, ui_font(ui, 22, 600), r, text, ui->theme.text, ALIGN_START);
}

void ui_placeholder(Ui *ui, Rect r, const char *icon, const char *title, const char *subtitle)
{
	Theme *t = &ui->theme;
	f32 h = 64 + 16 + 26 + (subtitle ? 40 : 0);
	f32 y = r.y + (r.h - h) / 2;
	ui_icon(ui, rect(r.x + (r.w - 64) / 2, y, 64, 64), icon, t->text_faint);
	y += 80;
	ui_text_in(ui, ui_font(ui, 18, 700), rect(r.x, y, r.w, 26), title, t->text_dim, ALIGN_CENTER);
	if (subtitle)
		ui_text_wrapped(ui, ui_font(ui, 13, 400), rect(r.x + 40, y + 30, r.w - 80, 60), subtitle, t->text_faint, ALIGN_CENTER);
}

void ui_tooltip(Ui *ui, const char *text)
{
	if (ui->hot != ui->last_id || !ui->last_id)
		return;
	ui->tooltip_id = ui->last_id;
	str_copy(ui->tooltip_text, text, sizeof ui->tooltip_text);
	ui->tooltip_anchor = ui->last_rect;
}

/* ---- Rows and sidebars ------------------------------------------------------------------ */

UiInteract ui_row(Ui *ui, Rect r, UiId id, bool selected)
{
	UiInteract it = ui_interact(ui, id, r);
	Theme *t = &ui->theme;
	f32 hover = ui_fade(ui, id, it.hovered, 0.1f);
	if (selected)
		ui_round(ui, r, CTL_RADIUS, t->selection);
	else if (hover > 0)
		ui_round(ui, r, CTL_RADIUS, color_scale_alpha(t->hover, hover));
	return it;
}

bool ui_sidebar_item(Ui *ui, Rect r, const char *icon, const char *label, bool selected)
{
	UiId id = ui_id(ui, label);
	UiInteract it = ui_interact(ui, id, r);
	Theme *t = &ui->theme;
	f32 hover = ui_fade(ui, id, it.hovered, 0.1f);
	Color fg = t->text, ic = t->text_dim;
	if (selected) {
		ui_round(ui, r, CTL_RADIUS, t->accent);
		fg = ic = t->on_accent;
	} else if (hover > 0) {
		ui_round(ui, r, CTL_RADIUS, color_scale_alpha(t->hover, hover));
	}
	f32 x = r.x + 10;
	if (icon) {
		ui_icon(ui, rect(x, r.y + (r.h - 18) / 2, 18, 18), icon, ic);
		x += 28;
	}
	ui_text_in(ui, ui_font(ui, 13, selected ? 600 : 500), rect(x, r.y, r.x + r.w - x - 6, r.h), label, fg, ALIGN_START);
	return it.clicked;
}

void ui_sidebar_heading(Ui *ui, Rect r, const char *label)
{
	ui_text_in(ui, ui_font(ui, 11, 700), pad4(r, 0, 0, 0, 10), label, ui->theme.text_faint, ALIGN_START);
}

/* ---- Scrolling ----------------------------------------------------------------------------- */

typedef struct {
	f32 offset;
	f32 velocity;
	f32 content;
	f32 view;
	f64 last_activity;
	bool dragging;
	f32 drag_start_y, drag_start_offset;
	bool kinetic;
} ScrollState;

f32 ui_scroll_begin(Ui *ui, Rect view, const char *id)
{
	UiId sid = ui_id(ui, id);
	ScrollState *s = ui_state(ui, sid, sizeof *s);
	s->view = view.h;
	f32 max_off = maxf(0, s->content - view.h);
	if (ui_mouse_in(ui, view) && (ui->scroll_dy != 0 || ui->scroll_phase)) {
		s->offset += ui->scroll_dy;
		s->last_activity = ui->now;
		if (ui->scroll_source == SCROLL_TOUCHPAD) {
			if (ui->scroll_phase == SCROLL_PHASE_END) {
				s->kinetic = true;
			} else {
				f32 dt = maxf(ui->dt, 1.0f / 240);
				s->velocity = s->velocity * 0.3f + (ui->scroll_dy / dt) * 0.7f;
				s->kinetic = false;
			}
		} else {
			s->kinetic = false;
			s->velocity = 0;
		}
		ui->scroll_dy = 0; /* consumed by the innermost scroller */
	}
	if (s->kinetic) {
		s->offset += s->velocity * ui->dt;
		s->velocity *= expf(-4.0f * ui->dt);
		if (fabsf(s->velocity) < 8)
			s->kinetic = false, s->velocity = 0;
		ui->redraw = true;
		s->last_activity = ui->now;
	}
	s->offset = clampf(s->offset, 0, max_off);
	if (ui->scroll_depth < (int)ARRAY_LEN(ui->scroll_stack)) {
		ui->scroll_stack[ui->scroll_depth] = sid;
		ui->scroll_view[ui->scroll_depth] = (Rect){ view.x, view.y, view.w, view.h };
		ui->scroll_depth++;
	}
	ui_push_clip(ui, view);
	ui->oy -= roundf(s->offset * ui->scale) / ui->scale;
	return view.y;
}

void ui_scroll_end(Ui *ui, f32 content_height)
{
	if (ui->scroll_depth == 0)
		return;
	ui->scroll_depth--;
	UiId sid = ui->scroll_stack[ui->scroll_depth];
	Rect view = ui->scroll_view[ui->scroll_depth];
	ScrollState *s = ui_state(ui, sid, sizeof *s);
	ui_pop_clip(ui);
	if (s->content != content_height) {
		s->content = content_height;
		s->offset = clampf(s->offset, 0, maxf(0, content_height - view.h));
	}
	if (content_height <= view.h + 0.5f)
		return;
	/* Overlay scrollbar that shows while scrolling or when the pointer is near. */
	Rect zone = { view.x + view.w - 14, view.y, 14, view.h };
	bool near = ui_mouse_in(ui, zone) || s->dragging;
	f32 vis = ui_fade(ui, sid ^ 0x51, near || ui->now - s->last_activity < 0.8, 0.2f);
	if (ui->now - s->last_activity < 0.8)
		ui->wake_at = ui->wake_at ? minf((f32)ui->wake_at, (f32)(s->last_activity + 0.85)) : s->last_activity + 0.85;
	f32 frac = view.h / content_height;
	f32 bar_h = maxf(32, view.h * frac);
	f32 max_off = content_height - view.h;
	f32 bar_y = view.y + (view.h - bar_h) * (s->offset / max_off);
	f32 bw = near ? 8 : 5;
	Rect bar = { view.x + view.w - bw - 3, bar_y + 2, bw, bar_h - 4 };
	UiInteract it = ui_interact(ui, sid ^ 0x77, zone);
	if (it.pressed_now) {
		s->dragging = true;
		s->drag_start_y = ui->my;
		s->drag_start_offset = s->offset;
		if (ui->my < bar_y || ui->my > bar_y + bar_h) /* click in the track jumps */
			s->drag_start_offset = s->offset = clampf((ui->my - view.y - bar_h / 2) / (view.h - bar_h) * max_off, 0, max_off);
	}
	if (s->dragging) {
		if (ui->down[BUTTON_LEFT]) {
			s->offset = clampf(s->drag_start_offset + (ui->my - s->drag_start_y) / (view.h - bar_h) * max_off, 0, max_off);
			s->last_activity = ui->now;
		} else {
			s->dragging = false;
		}
	}
	if (vis > 0)
		ui_round(ui, bar, bw / 2, color_scale_alpha(ui->theme.dark ? color_hex("#ffffff70") : color_hex("#00000060"), vis));
}

void ui_scroll_to(Ui *ui, const char *id, f32 offset)
{
	ScrollState *s = ui_state(ui, ui_id(ui, id), sizeof(ScrollState));
	s->offset = maxf(0, offset);
	s->kinetic = false;
}

/* ---- Window chrome ----------------------------------------------------------------------------- */

void ui_window_background(Ui *ui, Color c)
{
	f32 rad = window_radius(ui);
	Rect r = rect(0, 0, ui->width, ui->height);
	ui_round(ui, r, rad, c);
	if (rad > 0)
		ui_border(ui, r, rad, 1, color_hex(ui->theme.dark ? "#000000a0" : "#00000048"));
}

static bool window_button(Ui *ui, Rect r, const char *icon, const char *id)
{
	UiId bid = ui_id(ui, id);
	UiInteract it = ui_interact(ui, bid, r);
	f32 hover = ui_fade(ui, bid, it.hovered, 0.12f);
	Theme *t = &ui->theme;
	if (it.held && it.hovered)
		ui_circle(ui, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, t->press);
	else if (hover > 0)
		ui_circle(ui, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, color_scale_alpha(t->hover, hover * 1.4f));
	ui_icon(ui, center_in(r, 16, 16), icon, color_mix(t->text_dim, t->text, 0.5f + hover * 0.5f));
	return it.clicked;
}

Rect ui_headerbar(Ui *ui, Rect r, const char *title, u32 flags)
{
	Theme *t = &ui->theme;
	f32 rad = window_radius(ui);
	if (!(flags & HEADER_TRANSPARENT)) {
		if (flags & HEADER_FLAT)
			ui_round4(ui, r, rad, rad, 0, 0, t->window);
		else {
			Path p = { 0 };
			Rect d = ui_to_device(ui, r);
			path_round_rect4(&p, d, rad * ui->scale, rad * ui->scale, 0, 0);
			Paint g = paint_linear(0, d.y, 0, d.y + d.h, t->header_top, t->header_bottom);
			fill_path(ui->canvas, &p, mat_identity(), &g, FILL_NONZERO);
			path_free(&p);
			ui_fill(ui, rect(r.x, r.y + r.h - 1, r.w, 1), t->border_strong);
		}
		if (!t->dark && rad > 0)
			ui_line(ui, r.x + rad, r.y + 1.5f, r.x + r.w - rad, r.y + 1.5f, 1, color_hex("#ffffffb0"));
	}
	/* Dragging the empty bar moves the window; double-click maximizes. */
	UiId drag_id = ui_id(ui, "__headerbar");
	UiInteract it = ui_interact(ui, drag_id, r);
	if (it.pressed_now && ui->click_count < 2)
		ui->begin_move = true;
	if (it.pressed_now && ui->click_count >= 2 && !(flags & HEADER_NO_MAXIMIZE))
		ui->action = ACTION_TOGGLE_MAXIMIZE;
	if (it.right_clicked)
		ui->action = ACTION_SHOW_MENU;

	Rect close_r = { r.x + 9, r.y + (r.h - 30) / 2, 30, 30 };
	if (window_button(ui, close_r, "window-close", "__close"))
		ui->action = ACTION_CLOSE;
	Rect inner = { close_r.x + close_r.w + 6, r.y, r.w - 2 * (close_r.w + 15), r.h };
	if (!(flags & HEADER_NO_MAXIMIZE)) {
		Rect max_r = { r.x + r.w - 39, close_r.y, 30, 30 };
		bool maxed = ui->win_states & STATE_MAXIMIZED;
		if (window_button(ui, max_r, maxed ? "window-restore" : "window-maximize", "__max"))
			ui->action = ACTION_TOGGLE_MAXIMIZE;
	}
	if (title && !(flags & HEADER_NO_TITLE))
		ui_text_in(ui, ui_font(ui, 13, 700), rect(r.x + 60, r.y, r.w - 120, r.h), title,
			   ui->window_focused ? t->text : t->text_dim, ALIGN_CENTER);
	return inner;
}

/* ---- Popovers ---------------------------------------------------------------------------------- */

void ui_popover_toggle(Ui *ui, const char *id)
{
	UiId pid = ui_id(ui, id);
	ui->open_popover = ui->open_popover == pid ? 0 : pid;
	ui->redraw = true;
}

void ui_popover_close(Ui *ui)
{
	ui->open_popover = 0;
	ui->redraw = true;
}

bool ui_popover_is_open(Ui *ui, const char *id)
{
	return ui->open_popover == ui_id(ui, id);
}

static void enter_overlay(Ui *ui)
{
	if (!ui->overlay)
		ui->overlay = canvas_new(ui->canvas->width, ui->canvas->height);
	if (!ui->overlay_drawn) {
		memset(ui->overlay->pixels, 0, (usize)ui->overlay->stride * (usize)ui->overlay->height * 4);
		ui->overlay_drawn = true;
	}
	if (ui->depth < (int)ARRAY_LEN(ui->stack)) {
		ui->stack[ui->depth].ox = ui->ox;
		ui->stack[ui->depth].oy = ui->oy;
		ui->stack[ui->depth].clip = ui->clip;
		ui->stack[ui->depth].canvas = ui->canvas;
		ui->depth++;
	}
	ui->canvas = ui->overlay;
	ui->ox = ui->oy = 0;
	ui->clip = rect(0, 0, ui->width, ui->height);
	ui->in_overlay = true;
}

static void leave_overlay(Ui *ui)
{
	ui_pop_clip(ui); /* restores canvas, translation and clip */
	ui->in_overlay = false;
}

bool ui_popover_begin(Ui *ui, const char *id, Rect anchor, f32 w, f32 h, Rect *content)
{
	UiId pid = ui_id(ui, id);
	if (ui->open_popover != pid)
		return false;
	Rect a = { anchor.x + ui->ox, anchor.y + ui->oy, anchor.w, anchor.h };
	f32 x = clampf(a.x + a.w / 2 - w / 2, 6, ui->width - w - 6);
	f32 y = a.y + a.h + 10;
	bool above = y + h > ui->height - 6 && a.y - h - 10 > 6;
	if (above)
		y = a.y - h - 10;
	Rect r = { x, y, w, h };
	/* Clicking outside (but not on the anchor, which toggles) or Escape closes it. */
	bool outside_click = ui->pressed[BUTTON_LEFT] && !rect_contains(r, ui->mx, ui->my) && !rect_contains(a, ui->mx, ui->my);
	if (outside_click || ui_key_pressed(ui, KEY_ESC, 0)) {
		ui->open_popover = 0;
		ui->redraw = true;
		return false;
	}
	enter_overlay(ui);
	ui->overlay_rect = rect_union(ui->overlay_rect, rect_inset(r, -2, -12));
	Theme *t = &ui->theme;
	f32 k = ui_fade(ui, pid, true, 0.12f);
	Color bg = t->dark ? color_hex("#36373c") : COLOR_WHITE;
	ui_shadow(ui, r, 10, 14, 4, color_scale_alpha(color_hex("#00000045"), k));
	/* Arrow pointing at the anchor. */
	f32 ax = clampf(a.x + a.w / 2, x + 16, x + w - 16);
	Path p = { 0 };
	if (above) {
		path_move(&p, ax - 9, y + h - 1);
		path_line(&p, ax, y + h + 8);
		path_line(&p, ax + 9, y + h - 1);
	} else {
		path_move(&p, ax - 9, y + 1);
		path_line(&p, ax, y - 8);
		path_line(&p, ax + 9, y + 1);
	}
	path_close(&p);
	ui_round(ui, r, 10, color_scale_alpha(bg, k));
	ui_path_fill(ui, &p, color_scale_alpha(bg, k));
	path_free(&p);
	ui_border(ui, r, 10, 1, color_scale_alpha(t->border_strong, k));
	ui_push_clip(ui, r);
	*content = pad(r, 6);
	return true;
}

void ui_popover_end(Ui *ui)
{
	ui_pop_clip(ui);
	leave_overlay(ui);
}

bool ui_menu_item(Ui *ui, Rect r, const char *icon, const char *label, const char *shortcut)
{
	UiId id = ui_id(ui, label);
	UiInteract it = ui_interact(ui, id, r);
	Theme *t = &ui->theme;
	Color fg = t->text, dim = t->text_dim;
	if (it.hovered) {
		ui_round(ui, r, 6, t->accent);
		fg = dim = t->on_accent;
	}
	f32 x = r.x + 10;
	if (icon)
		ui_icon(ui, rect(x, r.y + (r.h - 16) / 2, 16, 16), icon, dim);
	x += 26;
	ui_text_in(ui, ui_font(ui, 13, 500), rect(x, r.y, r.w - (x - r.x) - 70, r.h), label, fg, ALIGN_START);
	if (shortcut)
		ui_text_in(ui, ui_font(ui, 12, 400), rect(r.x + r.w - 80, r.y, 70, r.h), shortcut, dim, ALIGN_END);
	if (it.clicked) {
		ui->open_popover = 0;
		ui->redraw = true;
	}
	return it.clicked;
}

/* ---- Dialogs ----------------------------------------------------------------------------------- */

void ui_dialog_open(Ui *ui, const char *id)
{
	ui->open_dialog = ui_id(ui, id);
	ui->redraw = true;
}

void ui_dialog_close(Ui *ui)
{
	ui->open_dialog = 0;
	ui->redraw = true;
}

bool ui_dialog_begin(Ui *ui, const char *id, f32 w, f32 h, Rect *content)
{
	UiId did = ui_id(ui, id);
	if (ui->open_dialog != did)
		return false;
	enter_overlay(ui);
	ui->overlay_rect = rect(0, 0, ui->width, ui->height);
	f32 k = ui_fade(ui, did, true, 0.15f);
	f32 rad = window_radius(ui);
	ui_round(ui, rect(0, 0, ui->width, ui->height), rad, color_scale_alpha(color_hex("#00000059"), k));
	Rect r = center_in(rect(0, 0, ui->width, ui->height), w, h);
	r.y += (1 - k) * 12;
	Theme *t = &ui->theme;
	ui_shadow(ui, r, 12, 22, 8, color_scale_alpha(color_hex("#00000060"), k));
	ui_round(ui, r, 12, color_scale_alpha(t->window, k));
	ui_border(ui, r, 12, 1, color_scale_alpha(t->border_strong, k));
	if (ui_key_pressed(ui, KEY_ESC, 0)) {
		ui->open_dialog = 0;
		ui->redraw = true;
	}
	ui_push_clip(ui, r);
	*content = pad(r, 20);
	return true;
}

void ui_dialog_end(Ui *ui)
{
	ui_pop_clip(ui);
	leave_overlay(ui);
}
