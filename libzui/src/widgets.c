/* Standard widgets. */
#include "zui.h"

#define CTL_RADIUS 8.0f
#define WINDOW_RADIUS 12.0f

static f32 window_radius(Ui *ui)
{
	if (ui->win_states & STATE_TILED) /* Hive's windows stay rounded, zoomed too */
		return WINDOW_RADIUS;
	return (ui->win_states & (STATE_MAXIMIZED | STATE_FULLSCREEN | STATE_TILED_LEFT | STATE_TILED_RIGHT)) ? 0 : WINDOW_RADIUS;
}

/* ---- Buttons ------------------------------------------------------------------------ */

typedef enum { BTN_NORMAL, BTN_SUGGESTED, BTN_DESTRUCTIVE, BTN_FLAT } ButtonKind;

/* The liquid finish: a sheen over the top of a control and light along its
 * rim, as on a drop of glass. */
static void liquid(Ui *ui, Rect r, f32 radius, f32 strength)
{
	ui_gradient(ui, rect(r.x, r.y, r.w, r.h * 0.6f), radius, color_scale_alpha(COLOR_WHITE, strength), COLOR_CLEAR);
	ui_border(ui, r, radius, 1, color_scale_alpha(COLOR_WHITE, strength * 1.3f));
}

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
	/* A capsule of colour with the liquid finish, a shade deeper under the
	 * pointer and deeper still, with the sheen gone, while pressed. */
	Color base = kind == BTN_NORMAL ? t->button_top : kind == BTN_SUGGESTED ? t->accent : t->danger;
	Color hot = kind == BTN_NORMAL ? color_lighten(base, t->dark ? 0.07f : -0.045f) : color_lighten(base, t->dark ? 0.08f : 0.06f);
	Color fill = color_mix(base, hot, hover);
	if (down)
		fill = kind == BTN_NORMAL ? color_lighten(base, t->dark ? 0.12f : -0.09f) : color_lighten(base, -0.1f);
	f32 radius = minf(r.h / 2, 16);
	ui_round(ui, r, radius, fill);
	if (!down)
		liquid(ui, r, radius, kind == BTN_NORMAL ? (t->dark ? 0.07f : 0.35f) : 0.16f);
}

static bool button_impl(Ui *ui, Rect r, const char *label, const char *icon, ButtonKind kind)
{
	UiId id = ui_id(ui, label && *label ? label : icon ? icon : "__button");
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
	Color off = t->dark ? color_hex("#47484d") : color_hex("#d6d6d3");
	Color bg = color_mix(off, t->accent, k);
	ui_round(ui, track, 12, bg);
	liquid(ui, track, 12, 0.10f);
	/* the knob is a drop that stretches while it's held */
	f32 stretch = ui_anim(ui, id + 1, it.held ? 1.0f : 0.0f, 20) * 6;
	f32 kw = 20 + stretch, kx = track.x + 2 + k * (40 - kw);
	Rect knob = { kx, track.y + 2, kw, 20 };
	ui_shadow(ui, knob, 10, 3, 1, color_hex("#00000040"));
	ui_round(ui, knob, 10, COLOR_WHITE);
	ui_gradient(ui, rect(knob.x, knob.y + 10, knob.w, 10), 10, COLOR_CLEAR, color_hex("#00000012"));
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
	ui_round(ui, r, CTL_RADIUS, t->button_top);
	f32 w = r.w / (f32)n;
	/* the selected item sits on a raised thumb that glides between items */
	f32 at = ui_anim(ui, ui_id(ui, items[0]) ^ (UiId)n * 0x9e37, (f32)*selected, 20);
	Rect thumb = rect(r.x + w * at + 2, r.y + 2, w - 4, r.h - 4);
	ui_shadow(ui, thumb, CTL_RADIUS - 2, 3, 1, color_hex(t->dark ? "#00000060" : "#00000022"));
	ui_round(ui, thumb, CTL_RADIUS - 2, t->dark ? color_hex("#4a4b50") : COLOR_WHITE);
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
		if (*selected != i && it.hovered)
			ui_round(ui, pad(cell, 2), CTL_RADIUS - 2, t->hover);
		ui_text_in(ui, ui_font(ui, 13, *selected == i ? 600 : 500), cell, items[i], *selected == i ? t->text : t->text_dim, ALIGN_CENTER);
	}
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
		ui_round(ui, rect(track.x, track.y, maxf(w, h), h), h / 2, t->accent);
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
	ui_shadow(ui, r, 14, 8, 2, color_hex(t->dark ? "#00000050" : "#00000012"));
	ui_round(ui, r, 14, t->card);
	ui_border(ui, r, 14, 1, t->border);
	/* light along the top edge */
	ui_gradient(ui, rect(r.x, r.y, r.w, minf(r.h, 28)), 14, color_hex(t->dark ? "#ffffff0a" : "#ffffff60"), COLOR_CLEAR);
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
		ui_round(ui, r, CTL_RADIUS, t->dark ? color_hex("#ffffff14") : color_hex("#0000000f"));
		ic = t->accent;
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
		if (!ui->wake_at || s->last_activity + 0.85 < ui->wake_at)
			ui->wake_at = s->last_activity + 0.85;
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
		ui_border(ui, r, rad, 1, color_hex(ui->theme.dark ? "#ffffff1a" : "#00000030"));
}

/* The window controls: small round buttons on a faint disc; close turns
 * red under the pointer. */
static bool window_button(Ui *ui, Rect r, const char *icon, const char *id, bool danger)
{
	UiId bid = ui_id(ui, id);
	UiInteract it = ui_interact(ui, bid, r);
	f32 hover = ui_fade(ui, bid, it.hovered, 0.12f);
	Theme *t = &ui->theme;
	f32 cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = r.w / 2;
	Color disc = t->dark ? color_hex("#ffffff14") : color_hex("#0000000e");
	Color hot = danger ? t->danger : (t->dark ? color_hex("#ffffff26") : color_hex("#0000001c"));
	Color c = color_mix(disc, hot, hover);
	if (it.held && it.hovered)
		c = danger ? color_lighten(t->danger, -0.1f) : (t->dark ? color_hex("#ffffff33") : color_hex("#00000028"));
	if (!ui->window_focused && hover == 0)
		c = color_scale_alpha(c, 0.6f);
	ui_circle(ui, cx, cy, rad, c);
	Color fg = danger ? color_mix(t->text_dim, COLOR_WHITE, hover) : color_mix(t->text_dim, t->text, hover);
	ui_icon(ui, center_in(r, 14, 14), icon, fg);
	return it.clicked;
}

Rect ui_headerbar(Ui *ui, Rect r, const char *title, u32 flags)
{
	Theme *t = &ui->theme;
	f32 rad = window_radius(ui);
	if (!(flags & HEADER_TRANSPARENT)) {
		ui_round4(ui, r, rad, rad, 0, 0, (flags & HEADER_FLAT) ? t->window : t->header_top);
		if (!(flags & HEADER_FLAT))
			ui_fill(ui, rect(r.x, r.y + r.h - 1, r.w, 1), t->border);
	}
	/* Dragging the empty bar moves the window; double-click maximizes. */
	UiId drag_id = ui_id(ui, "__headerbar");
	UiInteract it = ui_interact(ui, drag_id, r);
	ui->header_id = drag_id;
	ui->header_move = it.pressed_now && ui->click_count < 2;
	ui->header_max = it.pressed_now && ui->click_count >= 2 && !(flags & HEADER_NO_MAXIMIZE);
	ui->header_menu = it.right_clicked;

	/* minimize, maximize and close, at the right */
	f32 size = 24, gap = 10, x = r.x + r.w - 12 - size, y = r.y + roundf((r.h - size) / 2);
	if (window_button(ui, rect(x, y, size, size), "window-close", "__close", true))
		ui->action = ACTION_CLOSE;
	if (!(flags & HEADER_NO_MAXIMIZE)) {
		x -= size + gap;
		bool maxed = ui->win_states & STATE_MAXIMIZED;
		if (window_button(ui, rect(x, y, size, size), maxed ? "window-restore" : "window-maximize", "__max", false))
			ui->action = ACTION_TOGGLE_MAXIMIZE;
	}
	if (!(flags & HEADER_NO_MINIMIZE)) {
		x -= size + gap;
		if (window_button(ui, rect(x, y, size, size), "window-minimize", "__min", false))
			ui->action = ACTION_MINIMIZE;
	}
	Rect inner = { r.x + 8, r.y, x - 10 - (r.x + 8), r.h };
	if (title && !(flags & HEADER_NO_TITLE)) {
		/* centred on the window, but never under the controls */
		f32 side = r.x + r.w - x + 10;
		ui_text_in(ui, ui_font(ui, 13, 600), rect(r.x + side, r.y, r.w - 2 * side, r.h), title,
			   ui->window_focused ? t->text : t->text_dim, ALIGN_CENTER);
	}
	return inner;
}

/* ---- Glass --------------------------------------------------------------------------------------- */

void ui_glass(Ui *ui, Rect r, Color tint)
{
	if (!ui->theme.glass || ui->in_overlay || !ui->canvas) {
		ui_fill(ui, r, tint | 0xff000000u);
		return;
	}
	/* written, not blended: what was drawn there gives way to the tint, the
	 * window's own shape (its rounded corners) kept */
	f32 s = ui->scale;
	Rect d = rect_intersect(rect(roundf((r.x + ui->ox) * s), roundf((r.y + ui->oy) * s), roundf(r.w * s), roundf(r.h * s)), ui->canvas->clip);
	u32 p = color_premul(tint);
	for (int y = (int)d.y; y < (int)(d.y + d.h); y++) {
		u32 *row = canvas_row(ui->canvas, y);
		for (int x = (int)d.x; x < (int)(d.x + d.w); x++) {
			u32 a = row[x] >> 24;
			row[x] = a >= 255 ? p : blend_premul(0, p, a + (a >> 7));
		}
	}
	Rect g = rect_intersect(rect(r.x + ui->ox, r.y + ui->oy, r.w, r.h), rect(0, 0, ui->width, ui->height));
	if (ui->nglass < WIN_GLASS_MAX && g.w > 0 && g.h > 0)
		ui->glass[ui->nglass++] = g;
}

void ui_sidebar_background(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_glass(ui, r, color_with_alpha(t->sidebar, t->dark ? 0x88 : 0xd0));
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
	f32 y = a.y + a.h + 6;
	bool above = y + h > ui->height - 6 && a.y - h - 6 > 6;
	if (above)
		y = a.y - h - 6;
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
	Color bg = t->dark ? color_hex("#2c2d31") : COLOR_WHITE;
	ui_shadow(ui, r, 12, 18, 6, color_scale_alpha(color_hex(t->dark ? "#00000070" : "#00000030"), k));
	ui_round(ui, r, 12, color_scale_alpha(bg, k));
	ui_border(ui, r, 12, 1, color_scale_alpha(t->border_strong, k));
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
	if (it.hovered)
		ui_round(ui, r, 8, t->dark ? color_hex("#ffffff14") : color_hex("#0000000f"));
	f32 x = r.x + 10;
	if (icon)
		ui_icon(ui, rect(x, r.y + (r.h - 16) / 2, 16, 16), icon, dim);
	x += 26;
	ui_text_in(ui, ui_font(ui, 13, 500), rect(x, r.y, r.w - (x - r.x) - (shortcut ? 84 : 8), r.h), label, fg, ALIGN_START);
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
	ui_shadow(ui, r, 16, 26, 8, color_scale_alpha(color_hex("#00000060"), k));
	ui_round(ui, r, 16, color_scale_alpha(t->window, k));
	ui_border(ui, r, 16, 1, color_scale_alpha(t->border, k));
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
