/*
 * Shell surfaces: everything the desktop draws itself.
 *
 *   top bar        the app menu, the clock and the status icons
 *   taskbar        Start, search, a button per app with window previews, the tray
 *   launcher       every app, full screen, with search that also does sums
 *   notifications  toasts, and the notification center with the calendar
 *   quick settings toggles, sliders and the session buttons
 *   switcher       Alt+Tab
 *   lock screen    clock, user and password
 *   osd            volume and brightness feedback
 *
 * The bars are dark by default, so they read the same over any wallpaper;
 * the taskbar can follow the light style instead.
 */
#include "zdisplay.h"


/* ---- Helpers ---------------------------------------------------------------------- */

static void ago(i64 when, char *out, usize cap);

/* White text with a soft dark shadow, legible over any wallpaper. */
static f32 panel_text(Ui *ui, Font f, f32 x, f32 baseline, const char *s, f32 alpha)
{
	ui_text(ui, f, x + 0.5f, baseline + 1, s, color_scale_alpha(color_hex("#00000080"), alpha));
	return ui_text(ui, f, x, baseline, s, color_scale_alpha(COLOR_WHITE, alpha));
}

static void format_clock(char *buf, usize cap, bool with_date)
{
	Civil c;
	civil_local(time_unix(), &c);
	static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	char t[32];
	int h12 = c.hour % 12 ? c.hour % 12 : 12;
	const char *ampm = c.hour < 12 ? "AM" : "PM";
	if (S.set.clock_24h && S.set.clock_seconds)
		fmt(t, sizeof t, "%02d:%02d:%02d", c.hour, c.min, c.sec);
	else if (S.set.clock_24h)
		fmt(t, sizeof t, "%02d:%02d", c.hour, c.min);
	else if (S.set.clock_seconds)
		fmt(t, sizeof t, "%d:%02d:%02d %s", h12, c.min, c.sec, ampm);
	else
		fmt(t, sizeof t, "%d:%02d %s", h12, c.min, ampm);
	if (with_date)
		fmt(buf, cap, "%s %s %d  %s", days[c.wday], months[c.month - 1], c.day, t);
	else
		str_copy(buf, t, cap);
}

static f32 smooth(f32 k)
{
	return k * k * (3 - 2 * k);
}

/* Dark glass: the blurred wallpaper behind r under a dark tint. */
static void dark_glass(Ui *ui, Rect r, f32 radius, f32 tint)
{
	Rect d = ui_to_device(ui, r);
	if (radius > 0.5f) {
		blit_scaled_round(ui->canvas, S.wallpaper_blur, d, d, radius * S.scale, 1);
		ui_round(ui, r, radius, color_scale_alpha(color_hex("#0b0c10"), tint));
	} else {
		blit_scaled(ui->canvas, S.wallpaper_blur, d, d, 1);
		ui_fill(ui, r, color_scale_alpha(color_hex("#0b0c10"), tint));
	}
}

/* A panel of the shell's own, in the theme's colors. */
static void shell_card(Ui *ui, Rect r, f32 radius, f32 k)
{
	Theme *t = &ui->theme;
	Color bg = t->dark ? color_hex("#232427") : color_hex("#fbfbfa");
	ui_shadow(ui, r, radius, 26, 8, color_scale_alpha(color_hex(t->dark ? "#00000080" : "#00000036"), k));
	ui_round(ui, r, radius, color_scale_alpha(bg, k));
	ui_border(ui, r, radius, 1, color_scale_alpha(color_hex(t->dark ? "#ffffff14" : "#00000012"), k));
}

/* A panel opening from a bar: under the top bar, or above the taskbar
 * while the top bar is off; centred on cx and sliding in as k goes to 1. */
static Rect open_panel(Ui *ui, f32 cx, f32 w, f32 h, f32 k)
{
	f32 x = clampf(roundf(cx - w / 2), 8, S.sw - w - 8);
	f32 top = shell_panel_height(), taskbar = shell_taskbar_height() * (1 - smooth(S.dock_hide_k));
	Rect r;
	if (top > 0) {
		h = minf(h, S.sh - top - taskbar - 16);
		r = rect(x, roundf(top + 6 - (1 - k) * 8), w, h);
	} else {
		f32 bottom = S.sh - taskbar - 8;
		h = minf(h, bottom - 16);
		r = rect(x, roundf(bottom - h + (1 - k) * 8), w, h);
	}
	shell_card(ui, r, 14, k);
	shell_surface(r);
	return r;
}

static bool click_outside(Ui *ui, Rect r)
{
	return ui->pressed[BUTTON_LEFT] && !rect_contains(r, ui->mx, ui->my);
}

static void toggle_launcher(void)
{
	S.launcher_open = !S.launcher_open;
	if (S.launcher_open)
		shell_reload_apps();   /* Harbor may have added apps */
	S.notifications_open = S.session_menu_open = false;
}

static const char *volume_icon(void)
{
	return S.muted || S.volume == 0 ? "audio-volume-muted" : S.volume < 34 ? "audio-volume-low"
	       : S.volume < 67 ? "audio-volume-medium" : "audio-volume-high";
}

/* ---- Bars: shared pieces ------------------------------------------------------------- */

static Taskbar tb;              /* this frame's taskbar layout */
static f32 tb_dy;               /* how far it has slid away */
static Rect app_menu_btn, clock_btn, status_btn, bell_btn, start_btn;
static u64 dock_menu_opened_frame;
static bool app_menu_open;

static struct {
	char id[48];
	f64 started;
} launching;

/* Colors for a bar: dark, or the light style's own when the taskbar follows it. */
typedef struct {
	Color bg, fg, dim, hover, press, active;
	bool dark;
} BarLook;

static BarLook bar_look(Ui *ui, bool follow_style)
{
	Theme *t = &ui->theme;
	BarLook l;
	l.dark = !(follow_style && !t->dark);
	if (l.dark) {
		l.bg = color_hex("#1c1c1f");
		l.fg = COLOR_WHITE;
		l.dim = color_hex("#ffffffa6");
		l.hover = color_hex("#ffffff14");
		l.press = color_hex("#ffffff0c");
		l.active = color_hex("#ffffff1f");
	} else {
		l.bg = color_hex("#f1f1f0");
		l.fg = color_hex("#1d1e22");
		l.dim = color_hex("#00000099");
		l.hover = color_hex("#0000000f");
		l.press = color_hex("#00000008");
		l.active = color_hex("#00000017");
	}
	return l;
}

/* A bar's background: the blurred wallpaper under the bar color. */
static void bar_background(Ui *ui, Rect r, const BarLook *l, f32 alpha)
{
	Rect d = ui_to_device(ui, r);
	blit_scaled(ui->canvas, S.wallpaper_blur, d, d, 1);
	ui_fill(ui, r, color_scale_alpha(l->bg, alpha));
}

/* A button on a bar: a rounded highlight under the pointer, a firmer one while active. */
static UiInteract bar_button(Ui *ui, Rect r, const char *id, bool active, const BarLook *l, f32 inset_y)
{
	UiId bid = ui_id(ui, id);
	UiInteract it = ui_interact(ui, bid, r);
	f32 hover = ui_fade(ui, bid, it.hovered, 0.1f);
	Rect pill = rect_inset(r, 2, inset_y);
	if (active)
		ui_round(ui, pill, 6, l->active);
	else if (it.held && it.hovered)
		ui_round(ui, pill, 6, l->press);
	else if (hover > 0)
		ui_round(ui, pill, 6, color_scale_alpha(l->hover, hover));
	return it;
}

static int app_window_count(const char *id, Window **first)
{
	int n = 0;
	*first = NULL;
	for (int i = S.nwin - 1; i >= 0; i--) {
		Window *w = S.windows[i];
		if (!w->closing && !(w->flags & WIN_POPUP) && str_eq(w->app_id, id)) {
			if (!*first)
				*first = w;
			n++;
		}
	}
	return n;
}

static int unread_for(const char *app)
{
	int n = 0;
	for (int i = 0; i < S.nnotices; i++)
		n += !S.notices[i].read && str_eq(S.notices[i].app, app);
	return n;
}

static void tooltip_at(Ui *ui, f32 cx, f32 y, bool above, const char *text)
{
	Font f = ui_font(ui, 12, 500);
	f32 w = ui_text_width(ui, f, text) + 18;
	Rect tip = rect(roundf(clampf(cx - w / 2, 4, S.sw - w - 4)), roundf(above ? y - 34 : y + 6), w, 26);
	shell_surface(tip);
	ui_shadow(ui, tip, 6, 10, 2, color_hex("#00000040"));
	ui_round(ui, tip, 6, color_hex("#202023f4"));
	ui_text_in(ui, f, tip, text, COLOR_WHITE, ALIGN_CENTER);
}

static void toggle_notifications(void)
{
	S.notifications_open = !S.notifications_open;
	S.launcher_open = S.session_menu_open = app_menu_open = false;
	for (int i = 0; i < S.nnotices; i++)
		S.notices[i].read = true;
}

static void toggle_quick_settings(void)
{
	S.session_menu_open = !S.session_menu_open;
	S.launcher_open = S.notifications_open = app_menu_open = false;
}

static void draw_clock_lines(Ui *ui, Rect r, Color fg, Color dim)
{
	char time[32], date[32];
	Civil c;
	civil_local(time_unix(), &c);
	format_clock(time, sizeof time, false);
	fmt(date, sizeof date, "%d/%02d/%d", c.day, c.month, c.year);
	Font tf = ui_font(ui, 12, 600), df = ui_font(ui, 11, 400);
	f32 cx = r.x + r.w / 2, cy = r.y + r.h / 2;
	ui_text(ui, tf, roundf(cx - ui_text_width(ui, tf, time) / 2), roundf(cy - 2), time, fg);
	ui_text(ui, df, roundf(cx - ui_text_width(ui, df, date) / 2), roundf(cy + 13), date, dim);
}

/* ---- Top bar ------------------------------------------------------------------------- */

/* The app in front, for the app menu. */
static Window *front_window(void)
{
	Window *w = S.focus;
	if (!w || w->minimized || w->closing)
		return NULL;
	while (w->parent)
		w = w->parent;
	return w;
}

static void app_menu_draw(Ui *ui)
{
	Window *w = front_window();
	if (!app_menu_open || !w) {
		app_menu_open = false;
		return;
	}
	Theme *t = &ui->theme;
	bool maxed = w->states & STATE_MAXIMIZED;
	f32 h = 8 + 5 * 34 + 9 + 8;
	Rect r = rect(app_menu_btn.x, PANEL_HEIGHT + 4, 230, h);
	shell_card(ui, r, 10, 1);
	shell_surface(r);
	Rect c = pad(r, 6);
	bool close = false;
	if (ui_menu_item(ui, cut_top(&c, 34), "tab-new", "New Window", NULL)) {
		shell_launch_new(w->app_id);
		close = true;
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "window-minimize", "Minimize", NULL)) {
		window_minimize(w, true);
		close = true;
	}
	if (ui_menu_item(ui, cut_top(&c, 34), maxed ? "window-restore" : "window-maximize", maxed ? "Restore" : "Maximize", NULL)) {
		window_maximize(w, !maxed);
		close = true;
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "view-grid", "Show Desktop", NULL)) {
		wm_show_desktop(true);
		close = true;
	}
	ui_separator(ui, c.x + 4, c.y + 4, c.x + c.w - 4);
	cut_top(&c, 9);
	if (ui_menu_item(ui, cut_top(&c, 34), "window-close", "Quit", "Alt+F4")) {
		window_close_request(w);
		close = true;
	}
	UNUSED(t);
	if (close || ui_key_pressed(ui, KEY_ESC, 0) || (click_outside(ui, r) && !rect_contains(app_menu_btn, ui->mx, ui->my)))
		app_menu_open = false;
}

void topbar_draw(Ui *ui)
{
	f32 h = shell_panel_height();
	if (h <= 0) {
		app_menu_open = false;
		return;
	}
	bool fullscreen = S.focus && (S.focus->states & STATE_FULLSCREEN) && !S.focus->minimized && !S.focus->closing;
	if (fullscreen)
		return;
	BarLook l = bar_look(ui, false);
	Rect bar = rect(0, 0, S.sw, h);
	shell_surface(bar);
	bar_background(ui, bar, &l, 0.88f);
	Font bold = ui_font(ui, 13, 600), medium = ui_font(ui, 13, 500);
	f32 base = roundf((h + font_cap_height(bold)) / 2);

	/* the app in front, with its menu */
	Window *w = front_window();
	const AppInfo *a = w ? shell_app(w->app_id) : NULL;
	const char *name = a ? a->name : w && w->title[0] ? w->title : "Desktop";
	f32 nw = ui_text_width(ui, bold, name);
	app_menu_btn = rect(6, 0, 12 + (a ? 22 : 0) + nw + 22, h);
	if (bar_button(ui, app_menu_btn, "tb-app", app_menu_open, &l, 3).clicked && w) {
		app_menu_open = !app_menu_open;
		S.notifications_open = S.session_menu_open = S.launcher_open = false;
	}
	f32 x = app_menu_btn.x + 10;
	if (a) {
		ui_app_icon(ui, rect(x, roundf((h - 18) / 2), 18, 18), a->icon);
		x += 22;
	}
	ui_text(ui, bold, x, base, name, l.fg);
	if (w)
		ui_icon(ui, rect(x + nw + 4, roundf((h - 12) / 2), 12, 12), "pan-down", l.dim);

	/* the date and time */
	char date[48], time[32];
	{
		Civil c;
		civil_local(time_unix(), &c);
		static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
		static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
		fmt(date, sizeof date, "%s %d %s", days[c.wday], c.day, months[c.month - 1]);
		format_clock(time, sizeof time, false);
	}
	f32 dw = ui_text_width(ui, medium, date), tw = ui_text_width(ui, bold, time);
	f32 cw = dw + 8 + tw, cx = roundf((S.sw - cw) / 2);
	clock_btn = rect(cx - 12, 0, cw + 24, h);
	if (bar_button(ui, clock_btn, "tb-clock", S.notifications_open, &l, 3).clicked)
		toggle_notifications();
	ui_text(ui, medium, cx, base, date, l.dim);
	ui_text(ui, bold, cx + dw + 8, base, time, l.fg);
	int unread = 0;
	for (int i = 0; i < S.nnotices; i++)
		unread += !S.notices[i].read;
	if (unread)
		ui_circle(ui, cx + cw + 6, h / 2, 3, color_lighten(ui->theme.accent, 0.2f));

	/* status */
	const char *icons[8];
	int bat, ni = shell_status_icons(icons, &bat);
	char pct[8] = "";
	if (bat >= 0 && S.set.show_battery_percent)
		fmt(pct, sizeof pct, "%d%%", S.battery);
	Font small = ui_font(ui, 12, 600);
	f32 pw = pct[0] ? ui_text_width(ui, small, pct) + 4 : 0;
	f32 sw = 20 + (f32)ni * 16 + (f32)(ni - 1) * 10 + pw;
	status_btn = rect(S.sw - 6 - sw, 0, sw, h);
	if (bar_button(ui, status_btn, "tb-status", S.session_menu_open, &l, 3).clicked)
		toggle_quick_settings();
	x = status_btn.x + 10;
	for (int i = 0; i < ni; i++) {
		ui_icon(ui, rect(x, roundf((h - 16) / 2), 16, 16), icons[i], l.fg);
		x += 16;
		if (i == bat && pct[0]) {
			ui_text(ui, small, x + 4, base, pct, l.fg);
			x += pw;
		}
		x += 10;
	}
	app_menu_draw(ui);
}

/* ---- Taskbar ------------------------------------------------------------------------- */

/* Window previews over a task button: a picture of each of the app's windows. */
static char preview_app[48];
static f64 preview_since;
static Rect preview_rect;

static void previews_draw(Ui *ui, Rect slot, const char *id, const BarLook *l)
{
	Window *list[6];
	int n = 0;
	for (int i = S.nwin - 1; i >= 0 && n < 6; i--) {
		Window *w = S.windows[i];
		if (!w->closing && !(w->flags & WIN_POPUP) && str_eq(w->app_id, id))
			list[n++] = w;
	}
	if (!n)
		return;
	f32 tw = 208, th = 128, gap = 8;
	f32 w = (f32)n * tw + (f32)(n - 1) * gap + 16, h = th + 16 + 30;
	Rect r = rect(roundf(clampf(slot.x + slot.w / 2 - w / 2, 6, S.sw - w - 6)), roundf(slot.y - h - 8), w, h);
	preview_rect = r;
	shell_surface(r);
	ui_shadow(ui, r, 10, 18, 5, color_hex("#00000060"));
	ui_round(ui, r, 10, color_scale_alpha(l->bg, 0.97f));
	ui_border(ui, r, 10, 1, l->dark ? color_hex("#ffffff14") : color_hex("#00000014"));
	for (int i = 0; i < n; i++) {
		Window *win = list[i];
		Rect cell = rect(r.x + 8 + (f32)i * (tw + gap), r.y + 8, tw, th + 30);
		ui_push_id(ui, (i64)win->id);
		UiInteract it = bar_button(ui, cell, "pv", win == S.focus, l, 0);
		/* the title, and a close button */
		Rect head = rect(cell.x + 8, cell.y, cell.w - 16, 28);
		Rect close = rect(cell.x + cell.w - 26, cell.y + 5, 20, 20);
		ui_text_in(ui, ui_font(ui, 12, 500), rect(head.x, head.y, head.w - 22, head.h), win->title[0] ? win->title : id, l->fg, ALIGN_START);
		UiInteract ci = ui_interact(ui, ui_id(ui, "pv-close"), close);
		if (ci.hovered)
			ui_round(ui, close, 4, color_hex("#e0443e"));
		ui_icon(ui, pad(close, 4), "window-close", ci.hovered ? COLOR_WHITE : l->dim);
		/* the picture, fitted */
		Rect area = rect(cell.x + 6, cell.y + 28, cell.w - 12, th - 4);
		Rect v = win->rect;
		f32 k = minf(area.w / maxf(v.w, 1), area.h / maxf(v.h, 1));
		Rect pic = rect(roundf(area.x + (area.w - v.w * k) / 2), roundf(area.y + (area.h - v.h * k) / 2), roundf(v.w * k), roundf(v.h * k));
		ui_fill(ui, pic, color_hex("#00000030"));
		compose_window_thumb(ui->canvas, win, ui_to_device(ui, pic));
		if (ci.clicked) {
			window_close_request(win);
		} else if (it.clicked) {
			window_minimize(win, false);
			window_raise(win);
			window_focus(win);
			preview_app[0] = 0;
		}
		ui_pop_id(ui);
	}
	/* pictures change while it's open */
	if (!ui->wake_at || ui->wake_at > S.now + 0.25)
		ui->wake_at = S.now + 0.25;
}

static void dock_menu_draw(Ui *ui, Rect anchor)
{
	Theme *t = &ui->theme;
	const AppInfo *a = shell_app(S.dock_menu);
	Window *first;
	int count = app_window_count(S.dock_menu, &first);
	bool pinned = false;
	{
		usize n;
		char **parts = str_split(S.set.dock_apps, ',', &n);
		for (usize i = 0; i < n; i++)
			pinned |= str_eq(str_trim(parts[i]), S.dock_menu);
		str_split_free(parts, n);
	}
	int items = 2 + (count > 0);
	f32 w = 240, h = 12 + 34 * (f32)items + 30;
	Rect r = rect(clampf(roundf(anchor.x + anchor.w / 2 - w / 2), 6, S.sw - w - 6), roundf(anchor.y - h - 8), w, h);
	shell_card(ui, r, 10, 1);
	shell_surface(r);
	Rect c = pad(r, 6);
	ui_text(ui, ui_font(ui, 12, 700), c.x + 10, c.y + 17, a ? a->name : S.dock_menu, t->text_dim);
	cut_top(&c, 26);
	bool close = false;
	if (ui_menu_item(ui, cut_top(&c, 34), "tab-new", count ? "New Window" : "Open", NULL)) {
		shell_launch_new(S.dock_menu);
		close = true;
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "pin", pinned ? "Unpin from Taskbar" : "Pin to Taskbar", NULL)) {
		/* Rebuild the pinned list with or without this app. */
		StrBuf sb = { 0 };
		usize n;
		char **parts = str_split(S.set.dock_apps, ',', &n);
		for (usize i = 0; i < n; i++) {
			char *id = str_trim(parts[i]);
			if (!*id || str_eq(id, S.dock_menu))
				continue;
			if (sb.len)
				sb_addc(&sb, ',');
			sb_adds(&sb, id);
		}
		str_split_free(parts, n);
		if (!pinned) {
			if (sb.len)
				sb_addc(&sb, ',');
			sb_adds(&sb, S.dock_menu);
		}
		str_copy(S.set.dock_apps, sb_str(&sb), sizeof S.set.dock_apps);
		sb_free(&sb);
		settings_save();
		close = true;
	}
	if (count && ui_menu_item(ui, cut_top(&c, 34), "window-close", count > 1 ? "Close All Windows" : "Close Window", NULL)) {
		for (int i = S.nwin - 1; i >= 0; i--)
			if (str_eq(S.windows[i]->app_id, S.dock_menu) && !S.windows[i]->closing)
				window_close_request(S.windows[i]);
		close = true;
	}
	/* The click that opened the menu mustn't close it again. */
	bool fresh = ui->frame_no == dock_menu_opened_frame;
	if (close || ui_key_pressed(ui, KEY_ESC, 0) ||
	    (!fresh && (ui->pressed[BUTTON_LEFT] || ui->pressed[BUTTON_RIGHT]) && !rect_contains(rect_inset(r, -2, -2), ui->mx, ui->my)))
		S.dock_menu[0] = 0;
}

static void open_launcher(void)
{
	if (!S.launcher_open)
		toggle_launcher();
	app_menu_open = false;
}

void taskbar_draw(Ui *ui)
{
	Theme *t = &ui->theme;
	shell_taskbar(&tb);
	tb_dy = roundf(smooth(S.dock_hide_k) * (tb.bar.h + 2));
	if (S.dock_hide_k >= 0.999f && !S.dock_menu[0]) {
		preview_app[0] = 0;
		return;
	}
	BarLook l = bar_look(ui, S.set.taskbar_light);
	Rect bar = tb.bar;
	bar.y += tb_dy;
	shell_surface(bar);
	bar_background(ui, bar, &l, l.dark ? 0.86f : 0.9f);
	ui_fill(ui, rect(0, bar.y, S.sw, 1), l.dark ? color_hex("#ffffff12") : color_hex("#00000014"));
	const char *tip = NULL;
	f32 tip_x = 0;

	/* Start */
	start_btn = tb.start;
	start_btn.y += tb_dy;
	{
		UiInteract it = bar_button(ui, start_btn, "tk-start", S.launcher_open, &l, 4);
		/* a grid of nine dots: every app; they take the accent while the launcher is open */
		f32 g = roundf(tb.icon * 0.7f), dot = roundf(g / 4.2f), gap = (g - 3 * dot) / 2;
		Rect grid = center_in(start_btn, g, g);
		Color dc = S.launcher_open ? color_lighten(t->accent, l.dark ? 0.2f : 0) : l.fg;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				ui_round(ui, rect(roundf(grid.x + (f32)c * (dot + gap)), roundf(grid.y + (f32)r * (dot + gap)), dot, dot), dot * 0.3f, dc);
		if (it.hovered)
			tip = "Start", tip_x = start_btn.x + start_btn.w / 2;
		if (it.clicked)
			toggle_launcher(), app_menu_open = false;
	}

	/* search */
	if (tb.search.w > 0) {
		Rect s = tb.search;
		s.y += tb_dy;
		UiId sid = ui_id(ui, "tk-search");
		UiInteract it = ui_interact(ui, sid, s);
		f32 hover = ui_fade(ui, sid, it.hovered, 0.1f);
		Color fill = l.dark ? color_hex("#ffffff12") : color_hex("#ffffffd0");
		ui_round(ui, s, s.h / 2, color_mix(fill, l.dark ? color_hex("#ffffff1c") : COLOR_WHITE, hover));
		if (!l.dark)
			ui_border(ui, s, s.h / 2, 1, color_hex("#00000014"));
		ui_icon(ui, rect(s.x + 12, s.y + (s.h - 16) / 2, 16, 16), "search", l.dim);
		ui_text_in(ui, ui_font(ui, 13, 400), rect(s.x + 36, s.y, s.w - 44, s.h), "Type here to search", l.dim, ALIGN_START);
		if (it.hovered)
			ui_set_cursor(ui, CURSOR_TEXT);
		if (it.clicked)
			open_launcher();
	}

	/* task buttons */
	char ids[32][48];
	int n = shell_dock_items(ids, 32);
	Rect menu_anchor = { 0 }, hovered_slot = { 0 };
	const char *hovered_id = NULL;
	for (int i = 0; i < n && i < tb.n; i++) {
		Rect slot = rect(tb.apps_x + (f32)i * tb.slot_w, bar.y, tb.slot_w, bar.h);
		ui_push_id_str(ui, ids[i]);
		Window *first = NULL;
		int count = app_window_count(ids[i], &first);
		bool focused = first && S.focus && str_eq(S.focus->app_id, ids[i]) && !S.focus->minimized;
		UiInteract it = bar_button(ui, slot, "task", focused, &l, 4);
		const AppInfo *app = shell_app(ids[i]);
		f32 size = tb.icon;
		if (it.held && it.hovered)
			size -= 2;
		/* starting: the icon pulses until its window appears */
		if (str_eq(launching.id, ids[i])) {
			f64 lt = S.now - launching.started;
			Window *fw;
			if (lt > 6.0 || app_window_count(ids[i], &fw) > 0) {
				launching.id[0] = 0;
			} else {
				size *= 0.86f + 0.14f * (0.5f + 0.5f * cosf((f32)lt * 7));
				ui->redraw = true;
			}
		}
		f32 ix = tb.labels ? slot.x + 12 : slot.x + (slot.w - size) / 2;
		Rect icon = rect(roundf(ix + (tb.icon - size) / 2 * (tb.labels ? 1 : 0)), roundf(slot.y + (slot.h - size) / 2 - 1), size, size);
		ui_app_icon(ui, icon, app ? app->icon : ids[i]);
		if (tb.labels) {
			const char *label = count == 1 && first->title[0] ? first->title : app ? app->name : ids[i];
			ui_text_in(ui, ui_font(ui, 12, focused ? 600 : 400), rect(slot.x + 18 + tb.icon, slot.y, slot.w - 26 - tb.icon, slot.h), label,
				   l.fg, ALIGN_START);
		}
		/* open: a short line at the bottom, longer and in the accent for the app in front */
		if (count > 0) {
			f32 lw = focused ? (tb.labels ? slot.w - 24 : 16) : 6;
			Color lc = focused ? color_lighten(t->accent, l.dark ? 0.15f : 0) : l.dim;
			ui_round(ui, rect(roundf(slot.x + slot.w / 2 - lw / 2), slot.y + slot.h - 5, lw, 3), 1.5f, lc);
			if (count > 1 && !focused)
				ui_round(ui, rect(roundf(slot.x + slot.w / 2 + 5), slot.y + slot.h - 5, 3, 3), 1.5f, lc);
		}
		/* unread notifications from the app */
		int unread = unread_for(ids[i]);
		if (unread) {
			char num[8];
			fmt(num, sizeof num, "%d", MIN(unread, 99));
			ui_badge(ui, icon.x + icon.w - 1, icon.y + 3, num);
		}
		if (it.hovered && !S.dock_menu[0]) {
			hovered_slot = slot;
			hovered_id = ids[i];
			if (!count)
				tip = app ? app->name : ids[i], tip_x = slot.x + slot.w / 2;
		}
		if (str_eq(S.dock_menu, ids[i]))
			menu_anchor = slot;
		if (it.clicked && !S.dock_menu[0]) {
			preview_app[0] = 0;
			if (count == 0) {
				str_copy(launching.id, ids[i], sizeof launching.id);
				launching.started = S.now;
				shell_launch(ids[i], NULL);
			} else if (first == S.focus && !first->minimized) {
				window_minimize(first, true);
			} else {
				window_minimize(first, false);
				window_raise(first);
				window_focus(first);
			}
			S.launcher_open = false;
		}
		/* middle click: a new window */
		if (it.hovered && ui->pressed[BUTTON_MIDDLE])
			shell_launch_new(ids[i]);
		if (it.right_clicked && !S.dock_menu[0]) {
			str_copy(S.dock_menu, ids[i], sizeof S.dock_menu);
			menu_anchor = slot;
			dock_menu_opened_frame = ui->frame_no;
			preview_app[0] = 0;
		}
		ui_pop_id(ui);
	}

	/* the tray: notifications, then status and clock while the top bar is off, then show desktop */
	bell_btn = tb.bell;
	bell_btn.y += tb_dy;
	{
		int unread = 0;
		for (int i = 0; i < S.nnotices; i++)
			unread += !S.notices[i].read;
		UiInteract it = bar_button(ui, bell_btn, "tk-bell", S.notifications_open && !shell_panel_height(), &l, 4);
		ui_icon(ui, center_in(bell_btn, 18, 18), S.set.do_not_disturb ? "notifications-disabled" : "bell", l.fg);
		if (unread) {
			char num[8];
			fmt(num, sizeof num, "%d", MIN(unread, 99));
			ui_badge(ui, bell_btn.x + bell_btn.w / 2 + 9, bell_btn.y + bell_btn.h / 2 - 8, num);
		}
		if (it.hovered)
			tip = "Notifications", tip_x = bell_btn.x + bell_btn.w / 2;
		if (it.clicked)
			toggle_notifications();
	}
	if (tb.status.w > 0) {
		Rect s = tb.status;
		s.y += tb_dy;
		if (bar_button(ui, s, "tk-status", S.session_menu_open, &l, 4).clicked)
			toggle_quick_settings();
		const char *icons[8];
		int bat, ni = shell_status_icons(icons, &bat);
		f32 x = s.x + 8;
		for (int i = 0; i < ni; i++) {
			ui_icon(ui, rect(x + 5, roundf(s.y + (s.h - 16) / 2), 16, 16), icons[i], l.fg);
			x += 26;
			if (i == bat && S.set.show_battery_percent) {
				char pct[8];
				fmt(pct, sizeof pct, "%d%%", S.battery);
				ui_text(ui, ui_font(ui, 12, 500), x, roundf(s.y + s.h / 2 + 4), pct, l.fg);
				x += 34;
			}
		}
		status_btn = s;
	}
	if (tb.clock.w > 0) {
		Rect c = tb.clock;
		c.y += tb_dy;
		if (bar_button(ui, c, "tk-clock", S.notifications_open, &l, 4).clicked)
			toggle_notifications();
		draw_clock_lines(ui, c, l.fg, l.dim);
		clock_btn = c;
	}
	if (tb.desktop.w > 0) {
		Rect d = tb.desktop;
		d.y += tb_dy;
		UiId did = ui_id(ui, "tk-desktop");
		UiInteract it = ui_interact(ui, did, d);
		ui_fill(ui, rect(d.x, d.y + 10, 1, d.h - 20), l.dark ? color_hex("#ffffff24") : color_hex("#00000024"));
		if (it.hovered)
			ui_fill(ui, rect(d.x + 1, d.y, d.w - 1, d.h), l.hover);
		if (it.hovered)
			tip = "Show desktop", tip_x = S.sw - 60;
		if (it.clicked)
			wm_show_desktop(!S.show_desktop);
	}

	/* window previews after a moment over a button with windows */
	if (S.set.taskbar_previews && !S.dock_menu[0] && !S.launcher_open) {
		bool over_preview = preview_app[0] && rect_contains(rect_inset(preview_rect, -2, -12), ui->mx, ui->my);
		Window *fw;
		if (hovered_id && app_window_count(hovered_id, &fw) > 0) {
			if (!str_eq(preview_app, hovered_id)) {
				str_copy(preview_app, hovered_id, sizeof preview_app);
				preview_since = S.now;
			}
		} else if (!over_preview) {
			preview_app[0] = 0;
		}
		static Rect preview_slot;
		if (hovered_id)
			preview_slot = hovered_slot;
		if (preview_app[0]) {
			if (S.now - preview_since >= 0.4)
				previews_draw(ui, preview_slot, preview_app, &l);
			else if (!ui->wake_at || ui->wake_at > preview_since + 0.42)
				ui->wake_at = preview_since + 0.42;
		}
	} else {
		preview_app[0] = 0;
	}
	if (S.dock_menu[0]) {
		if (menu_anchor.w == 0)
			S.dock_menu[0] = 0;
		else
			dock_menu_draw(ui, menu_anchor);
	}
	if (tip && !preview_app[0] && !S.dock_menu[0])
		tooltip_at(ui, tip_x, bar.y, true, tip);
}

/* ---- Launcher ---------------------------------------------------------------------------
 * Every app, full screen between the bars over the softened wallpaper. With
 * nothing typed: the apps you open most, the categories, and all apps A to
 * Z. Typing searches apps, Settings pages and actions, works out sums, and
 * offers to search the web or Harbor. */

static StrBuf launcher_query;
static int launcher_first_match = -1;
static char launcher_cat[32];       /* "" for all */

/* How often each app is opened from the launcher and the taskbar. */
static Conf *usage;

static Conf *usage_conf(void)
{
	if (!usage) {
		char *path = str_fmt("%s/.config/zurbax/launcher.conf", S.set.home);
		usage = conf_load(path);
		mem_free(path);
	}
	return usage;
}

static void usage_bump(const char *id)
{
	Conf *c = usage_conf();
	char key[64];
	fmt(key, sizeof key, "use.%s", id);
	conf_set_int(c, key, conf_get_int(c, key, 0) + 1);
	conf_save(c);
}

static void launch_app(const char *id)
{
	usage_bump(id);
	shell_launch(id, NULL);
	S.launcher_open = false;
}

static bool app_matches(const AppInfo *a, const char *q)
{
	if (launcher_cat[0] && !*q && !str_eq(a->category, launcher_cat))
		return false;
	if (!*q)
		return true;
	return str_find_nocase(a->name, q) || str_find_nocase(a->keywords, q) || str_find_nocase(a->category, q) ||
	       str_find_nocase(a->id, q);
}

/* The highlighted app, moved with arrow keys or a controller's D-pad. */
static int launcher_sel = -1, launcher_sel_app = -1, launcher_shown, launcher_cols = 7;
static bool launcher_follow;

/* ---- the calculator: + - * / % ^, brackets and decimals ---- */

static const char *calc_p;
static bool calc_ok;

static f64 calc_expr(void);

static void calc_space(void)
{
	while (*calc_p == ' ')
		calc_p++;
}

static f64 calc_atom(void)
{
	calc_space();
	if (*calc_p == '(') {
		calc_p++;
		f64 v = calc_expr();
		calc_space();
		if (*calc_p == ')')
			calc_p++;
		else
			calc_ok = false;
		return v;
	}
	if (*calc_p == '-') {
		calc_p++;
		return -calc_atom();
	}
	if (*calc_p == '+') {
		calc_p++;
		return calc_atom();
	}
	if (!is_digit(*calc_p) && *calc_p != '.') {
		calc_ok = false;
		return 0;
	}
	const char *end;
	f64 v = str_to_float(calc_p, &end);
	if (end == calc_p)
		calc_ok = false;
	calc_p = end;
	return v;
}

static f64 calc_power(void)
{
	f64 v = calc_atom();
	calc_space();
	if (*calc_p == '^') {
		calc_p++;
		v = pow(v, calc_power());
	}
	return v;
}

static f64 calc_term(void)
{
	f64 v = calc_power();
	for (;;) {
		calc_space();
		char op = *calc_p;
		if (op != '*' && op != '/' && op != '%' && op != 'x')
			return v;
		calc_p++;
		f64 r = calc_power();
		if (op == '*' || op == 'x')
			v *= r;
		else if (op == '/')
			v = r != 0 ? v / r : (calc_ok = false, 0);
		else
			v = r != 0 ? fmod(v, r) : (calc_ok = false, 0);
	}
}

static f64 calc_expr(void)
{
	f64 v = calc_term();
	for (;;) {
		calc_space();
		if (*calc_p == '+')
			calc_p++, v += calc_term();
		else if (*calc_p == '-')
			calc_p++, v -= calc_term();
		else
			return v;
	}
}

/* A sum typed in the search field, worked out; false if it isn't one. */
static bool calculate(const char *q, char *out, usize cap)
{
	bool has_op = false, has_digit = false;
	for (const char *s = q; *s; s++) {
		if (is_digit(*s))
			has_digit = true;
		else if (str_chr("+-*/%^x()", *s))
			has_op = true;
		else if (*s != ' ' && *s != '.')
			return false;
	}
	if (!has_digit || !has_op)
		return false;
	calc_p = q;
	calc_ok = true;
	f64 v = calc_expr();
	calc_space();
	if (!calc_ok || *calc_p || v != v)
		return false;
	if (fabs(v - round(v)) < 1e-9 && fabs(v) < 1e15)
		fmt(out, cap, "%lld", (long long)round(v));
	else
		fmt(out, cap, "%.6g", v);
	return true;
}

/* ---- Settings pages and actions the search knows ---- */

static const struct {
	const char *title, *icon, *keywords;
} SETTINGS_PAGES[] = {
	{ "Appearance", "palette", "dark light style accent colour color wallpaper background theme" },
	{ "Desktop", "computer", "taskbar top bar panel clock seconds battery lock autohide" },
	{ "Network", "network-wireless", "wifi wi-fi internet ethernet wireless" },
	{ "Bluetooth", "bluetooth", "bluetooth headphones devices" },
	{ "Display", "display", "scale resolution night light brightness monitor" },
	{ "Sound", "audio-volume-high", "volume speaker audio mute microphone" },
	{ "Mouse & Touchpad", "touchpad", "pointer speed natural scrolling tap click trackpad" },
	{ "Keyboard", "keyboard", "layout language shortcuts keys" },
	{ "Users", "user", "account name password avatar login" },
	{ "Date & Time", "clock", "time zone 24 hour clock date" },
	{ "Gaming", "gamepad", "game mode controller fps" },
	{ "Screen Time", "hourglass", "screen time limits usage parental" },
	{ "About", "info", "about version hardware system update" },
};

static const struct {
	const char *title, *icon, *keywords;
	u32 action;
} ACTIONS[] = {
	{ "Lock", "lock", "lock screen away", SESSION_LOCK },
	{ "Log Out", "system-log-out", "log out sign out logout", SESSION_LOGOUT },
	{ "Restart", "system-restart", "restart reboot", SESSION_RESTART },
	{ "Shut Down", "system-shutdown", "shut down shutdown power off turn off", SESSION_SHUTDOWN },
	{ "Sleep", "system-sleep", "sleep suspend", SESSION_SLEEP },
};

static bool words_match(const char *title, const char *keywords, const char *q)
{
	return str_find_nocase(title, q) || str_find_nocase(keywords, q);
}

void launcher_nav(int dx, int dy)
{
	if (launcher_sel < 0)
		launcher_sel = 0;
	else
		launcher_sel = CLAMP(launcher_sel + dx + dy * launcher_cols, 0, MAX(0, launcher_shown - 1));
	launcher_follow = true;
}

void launcher_activate(void)
{
	int app = launcher_sel >= 0 ? launcher_sel_app : launcher_first_match;
	const char *q = sb_str(&launcher_query);
	char result[64];
	if ((app < 0 || app >= S.napps) && *q && calculate(q, result, sizeof result)) {
		mem_free(S.clipboard);
		S.clipboard = str_dup(result);
		S.clipboard_len = strlen(result);
		str_copy(S.clipboard_mime, "text/plain", sizeof S.clipboard_mime);
		shell_notify("zurbax", "calculator", "Copied", result);
		S.launcher_open = false;
		return;
	}
	if (app < 0 || app >= S.napps)
		return;
	launch_app(S.apps[app].id);
}

bool launcher_key(Ui *ui, u32 code, u32 mods, const char *text)
{
	UNUSED(ui), UNUSED(mods), UNUSED(text);
	if (code == KEY_ESC) {
		if (launcher_query.len)
			sb_clear(&launcher_query), launcher_sel = -1;
		else
			S.launcher_open = false;
		return true;
	}
	if (code == KEY_ENTER || code == KEY_KPENTER) {
		launcher_activate();
		return true;
	}
	if (code == KEY_DOWN || code == KEY_UP) {
		launcher_nav(0, code == KEY_DOWN ? 1 : -1);
		return true;
	}
	/* left and right move through the apps unless there's text to edit */
	if ((code == KEY_LEFT || code == KEY_RIGHT) && (launcher_sel >= 0 || !launcher_query.len)) {
		launcher_nav(code == KEY_RIGHT ? 1 : -1, 0);
		return true;
	}
	return false;
}

/* A rounded chip over the backdrop; returns true when clicked. */
static bool chip(Ui *ui, Rect r, const char *label, bool on, f32 k)
{
	UiId id = ui_id(ui, label);
	UiInteract it = ui_interact(ui, id, r);
	f32 hover = ui_fade(ui, id, it.hovered, 0.12f);
	Color bg = on ? color_hex("#ffffffe8") : color_scale_alpha(COLOR_WHITE, 0.1f + 0.08f * hover);
	ui_round(ui, r, r.h / 2, color_scale_alpha(bg, k));
	ui_text_in(ui, ui_font(ui, 13, on ? 600 : 500), r, label, color_scale_alpha(on ? color_hex("#17181c") : color_hex("#ffffffe6"), k), ALIGN_CENTER);
	return it.clicked;
}

/* One app in the grid: the icon over the name. */
static bool app_cell(Ui *ui, Rect cell, const AppInfo *a, bool sel, f32 k)
{
	ui_push_id_str(ui, a->id);
	UiId id = ui_id(ui, "app");
	UiInteract it = ui_interact(ui, id, cell);
	f32 hover = ui_fade(ui, id, it.hovered, 0.12f);
	f32 bga = maxf(0.1f * hover, sel ? 0.16f : 0) + (it.held && it.hovered ? 0.05f : 0);
	if (bga > 0)
		ui_round(ui, cell, 16, color_scale_alpha(COLOR_WHITE, bga * k));
	f32 is = 60 * (it.held && it.hovered ? 0.95f : 1);
	ui_app_icon(ui, rect(roundf(cell.x + (cell.w - is) / 2), roundf(cell.y + 12 + (60 - is) / 2), is, is), a->icon);
	ui_text_in(ui, ui_font(ui, 13, 500), rect(cell.x + 6, cell.y + 80, cell.w - 12, 22), a->name, color_scale_alpha(COLOR_WHITE, k), ALIGN_CENTER);
	ui_pop_id(ui);
	return it.clicked;
}

/* A result row: an icon, a title, and what it does. */
static bool result_row(Ui *ui, Rect r, const char *icon, const char *title, const char *sub, f32 k)
{
	UiId id = ui_id(ui, title);
	UiInteract it = ui_interact(ui, id, r);
	f32 hover = ui_fade(ui, id, it.hovered, 0.12f);
	if (hover > 0)
		ui_round(ui, r, 10, color_scale_alpha(COLOR_WHITE, 0.1f * hover * k));
	ui_icon(ui, rect(r.x + 14, r.y + (r.h - 20) / 2, 20, 20), icon, color_scale_alpha(COLOR_WHITE, k));
	ui_text_in(ui, ui_font(ui, 14, 600), rect(r.x + 48, r.y, r.w * 0.55f, r.h), title, color_scale_alpha(COLOR_WHITE, k), ALIGN_START);
	if (sub)
		ui_text_in(ui, ui_font(ui, 12, 400), rect(r.x + r.w * 0.55f, r.y, r.w * 0.45f - 14, r.h), sub, color_scale_alpha(color_hex("#ffffffa0"), k),
			   ALIGN_END);
	return it.clicked;
}

static void url_encode(const char *in, char *out, usize cap)
{
	usize n = 0;
	for (const u8 *s = (const u8 *)in; *s && n + 4 < cap; s++) {
		if (is_alnum((char)*s) || str_chr("-_.~", (char)*s))
			out[n++] = (char)*s;
		else if (*s == ' ')
			out[n++] = '+';
		else
			n += (usize)fmt(out + n, cap - n, "%%%02X", *s);
	}
	out[n] = 0;
}

void launcher_draw(Ui *ui)
{
	S.launcher_k = ui_anim(ui, ui_id(ui, "launcher-k"), S.launcher_open ? 1.0f : 0.0f, 18);
	f32 k = S.launcher_k;
	if (k <= 0.01f) {
		sb_clear(&launcher_query);
		launcher_sel = -1;
		launcher_cat[0] = 0;
		return;
	}
	f32 top = shell_panel_height(), bottom = S.sh - shell_taskbar_height();
	Rect area = rect(0, top, S.sw, bottom - top), d = ui_to_device(ui, area);
	shell_surface(area);
	blit_scaled(ui->canvas, S.wallpaper_blur, d, d, k);
	ui_fill(ui, area, color_scale_alpha(color_hex("#08080b"), 0.45f * k));
	f32 lift = roundf((1 - smooth(k)) * 24);
	f32 cx = roundf(S.sw / 2);
	bool hot = false;

	/* search */
	Rect search = rect(cx - 260, top + 36 + lift, 520, 44);
	ui_round(ui, search, 22, color_scale_alpha(color_hex("#ffffff1f"), k));
	ui_border(ui, search, 22, 1, color_scale_alpha(color_hex("#ffffff2a"), k));
	Theme saved = ui->theme;
	ui->theme.text = color_scale_alpha(COLOR_WHITE, k);
	ui->theme.text_dim = ui->theme.text_faint = color_scale_alpha(color_hex("#ffffffa0"), k);
	int ev = ui_entry(ui, pad4(search, 0, 8, 0, 8), "launcher-search", &launcher_query, "Search apps, settings, sums and the web",
			  ENTRY_SEARCH | ENTRY_AUTOFOCUS | ENTRY_FLAT);
	ui->theme = saved;
	if (S.launcher_open && ui->focus != ui_id(ui, "launcher-search"))
		ui_focus(ui, "launcher-search");
	if (ev & ENTRY_CHANGED)
		launcher_sel = -1;
	hot |= ui_mouse_in(ui, search);
	const char *q = sb_str(&launcher_query);
	f32 cell_w = 132, cell_h = 118;
	launcher_cols = CLAMP((int)((S.sw - 120) / cell_w), 3, 8);
	f32 gw = (f32)launcher_cols * cell_w, gx = roundf(cx - gw / 2);
	f32 y = search.y + search.h + 26;
	Font hf = ui_font(ui, 13, 600);
	Color head = color_scale_alpha(color_hex("#ffffffb8"), k);
	launcher_first_match = -1;

	if (*q) {
		/* apps that match, then everything else that does */
		int shown = 0, max_apps = launcher_cols * 2;
		for (int i = 0; i < S.napps && shown < max_apps; i++) {
			AppInfo *a = &S.apps[i];
			if (!app_matches(a, q))
				continue;
			if (launcher_first_match < 0)
				launcher_first_match = i;
			if (shown == 0) {
				ui_text(ui, hf, gx + 6, y + 13, "Apps", head);
				y += 24;
			}
			Rect cell = rect(gx + (f32)(shown % launcher_cols) * cell_w + 4, y + (f32)(shown / launcher_cols) * cell_h, cell_w - 8, cell_h - 8);
			bool sel = launcher_sel >= 0 ? shown == launcher_sel : shown == 0;
			if (sel && launcher_sel >= 0)
				launcher_sel_app = i;
			hot |= ui_mouse_in(ui, cell);
			if (app_cell(ui, cell, a, sel, k))
				launch_app(a->id);
			shown++;
		}
		launcher_shown = shown;
		if (launcher_sel >= shown)
			launcher_sel = shown - 1;
		if (shown)
			y += ceilf((f32)shown / (f32)launcher_cols) * cell_h + 10;
		f32 rw = minf(gw, 720), rx = roundf(cx - rw / 2);
		bool any = false;
#define SECTION(name) do { if (!any) { ui_text(ui, hf, rx + 6, y + 13, name, head); y += 24; any = true; } } while (0)
		char result[64];
		if (calculate(q, result, sizeof result)) {
			SECTION("Results");
			char sub[96];
			fmt(sub, sizeof sub, "%s  ·  Enter copies it", q);
			char title[80];
			fmt(title, sizeof title, "= %s", result);
			Rect r = rect(rx, y, rw, 48);
			hot |= ui_mouse_in(ui, r);
			if (result_row(ui, r, "calculator", title, sub, k)) {
				launcher_sel = -1;
				launcher_first_match = -1;
				launcher_activate();
			}
			y += 52;
		}
		for (usize i = 0; i < ARRAY_LEN(SETTINGS_PAGES) && y < bottom - 60; i++) {
			if (!words_match(SETTINGS_PAGES[i].title, SETTINGS_PAGES[i].keywords, q))
				continue;
			SECTION("Results");
			char title[64];
			fmt(title, sizeof title, "Settings › %s", SETTINGS_PAGES[i].title);
			Rect r = rect(rx, y, rw, 48);
			hot |= ui_mouse_in(ui, r);
			if (result_row(ui, r, SETTINGS_PAGES[i].icon, title, "Open in Settings", k)) {
				shell_launch("settings", SETTINGS_PAGES[i].title);
				S.launcher_open = false;
			}
			y += 52;
		}
		for (usize i = 0; i < ARRAY_LEN(ACTIONS) && y < bottom - 60; i++) {
			if (!words_match(ACTIONS[i].title, ACTIONS[i].keywords, q))
				continue;
			SECTION("Results");
			Rect r = rect(rx, y, rw, 48);
			hot |= ui_mouse_in(ui, r);
			if (result_row(ui, r, ACTIONS[i].icon, ACTIONS[i].title, "Power", k)) {
				S.launcher_open = false;
				shell_session_action(ACTIONS[i].action);
			}
			y += 52;
		}
		if (y < bottom - 110) {
			SECTION("Results");
			char enc[256], url[320], title[160];
			url_encode(q, enc, sizeof enc);
			fmt(title, sizeof title, "Search the web for “%s”", q);
			Rect r = rect(rx, y, rw, 48);
			hot |= ui_mouse_in(ui, r);
			if (result_row(ui, r, "globe", title, "Drift", k)) {
				fmt(url, sizeof url, "https://duckduckgo.com/?q=%s", enc);
				shell_launch("drift", url);
				S.launcher_open = false;
			}
			y += 52;
			fmt(title, sizeof title, "Find “%s” in Harbor", q);
			r = rect(rx, y, rw, 48);
			hot |= ui_mouse_in(ui, r);
			if (result_row(ui, r, "harbor", title, "Thousands of apps from Flathub", k)) {
				char arg[300];
				fmt(arg, sizeof arg, "search:%s", q);
				shell_launch("harbor", arg);
				S.launcher_open = false;
			}
		}
#undef SECTION
	} else {
		/* the apps you open most */
		Conf *c = usage_conf();
		int top_ids[16], ntop = 0;
		for (int i = 0; i < S.napps; i++) {
			char key[64];
			fmt(key, sizeof key, "use.%s", S.apps[i].id);
			i64 n = conf_get_int(c, key, 0);
			if (n <= 0)
				continue;
			int at = ntop < launcher_cols ? ntop++ : launcher_cols;
			if (at == launcher_cols) {
				/* replace the least used if this one beats it */
				int low = 0;
				for (int j = 1; j < ntop; j++) {
					char kj[64], kl[64];
					fmt(kj, sizeof kj, "use.%s", S.apps[top_ids[j]].id);
					fmt(kl, sizeof kl, "use.%s", S.apps[top_ids[low]].id);
					if (conf_get_int(c, kj, 0) < conf_get_int(c, kl, 0))
						low = j;
				}
				char kl[64];
				fmt(kl, sizeof kl, "use.%s", S.apps[top_ids[low]].id);
				if (n > conf_get_int(c, kl, 0))
					top_ids[low] = i;
				continue;
			}
			top_ids[at] = i;
		}
		/* most used first */
		for (int a = 0; a < ntop; a++)
			for (int b = a + 1; b < ntop; b++) {
				char ka[64], kb[64];
				fmt(ka, sizeof ka, "use.%s", S.apps[top_ids[a]].id);
				fmt(kb, sizeof kb, "use.%s", S.apps[top_ids[b]].id);
				if (conf_get_int(c, kb, 0) > conf_get_int(c, ka, 0)) {
					int t = top_ids[a];
					top_ids[a] = top_ids[b];
					top_ids[b] = t;
				}
			}
		if (ntop >= 2 && !launcher_cat[0]) {
			ui_text(ui, hf, gx + 6, y + 13, "Frequent", head);
			y += 24;
			for (int i = 0; i < ntop; i++) {
				Rect cell = rect(gx + (f32)i * cell_w + 4, y, cell_w - 8, cell_h - 8);
				hot |= ui_mouse_in(ui, cell);
				ui_push_id(ui, 9000 + i);
				if (app_cell(ui, cell, &S.apps[top_ids[i]], false, k))
					launch_app(S.apps[top_ids[i]].id);
				ui_pop_id(ui);
			}
			y += cell_h + 8;
		}
		/* categories that have apps, as chips */
		static const char *const ORDER[] = { "Internet", "Office", "Media", "Graphics", "Games", "Utilities", "System", "Other" };
		const char *cats[ARRAY_LEN(ORDER) + 1];
		int nc = 0;
		cats[nc++] = "All Apps";
		for (usize ci = 0; ci < ARRAY_LEN(ORDER); ci++)
			for (int i = 0; i < S.napps; i++)
				if (str_eq(S.apps[i].category, ORDER[ci])) {
					cats[nc++] = ORDER[ci];
					break;
				}
		Font cf = ui_font(ui, 13, 600);
		f32 x = gx + 4;
		for (int ci = 0; ci < nc; ci++) {
			f32 w = ui_text_width(ui, cf, cats[ci]) + 28;
			bool on = ci == 0 ? !launcher_cat[0] : str_eq(launcher_cat, cats[ci]);
			Rect r = rect(x, y, w, 32);
			hot |= ui_mouse_in(ui, r);
			if (chip(ui, r, cats[ci], on, k)) {
				str_copy(launcher_cat, ci == 0 ? "" : cats[ci], sizeof launcher_cat);
				launcher_sel = -1;
				ui_scroll_to(ui, "launcher-grid", 0);
			}
			x += w + 8;
		}
		y += 32 + 18;
		/* every app, A to Z */
		Rect body = rect(gx, y, gw, bottom - y - 8);
		static f32 grid_offset;
		if (launcher_follow && launcher_sel >= 0) {
			f32 row = (f32)(launcher_sel / launcher_cols) * cell_h;
			if (row < grid_offset)
				ui_scroll_to(ui, "launcher-grid", row);
			else if (row + cell_h > grid_offset + body.h)
				ui_scroll_to(ui, "launcher-grid", row + cell_h - body.h);
		}
		launcher_follow = false;
		f32 oy0 = ui->oy;
		f32 y0 = ui_scroll_begin(ui, body, "launcher-grid");
		grid_offset = oy0 - ui->oy;
		int shown = 0;
		for (int i = 0; i < S.napps; i++) {
			AppInfo *a = &S.apps[i];
			if (!app_matches(a, ""))
				continue;
			if (launcher_first_match < 0)
				launcher_first_match = i;
			Rect cell = rect(body.x + (f32)(shown % launcher_cols) * cell_w + 4, y0 + (f32)(shown / launcher_cols) * cell_h, cell_w - 8, cell_h - 8);
			bool sel = launcher_sel >= 0 && shown == launcher_sel;
			if (sel)
				launcher_sel_app = i;
			hot |= ui_mouse_in(ui, cell);
			if (app_cell(ui, cell, a, sel, k))
				launch_app(a->id);
			shown++;
		}
		launcher_shown = shown;
		if (launcher_sel >= shown)
			launcher_sel = shown - 1;
		ui_scroll_end(ui, ceilf((f32)shown / (f32)launcher_cols) * cell_h);
	}
	/* a click on the backdrop closes it */
	if (S.launcher_open && ui->pressed[BUTTON_LEFT] && !hot && ui->my > top && ui->my < bottom)
		S.launcher_open = false;
}

/* ---- Notifications ----------------------------------------------------------------- */

static void notice_card(Ui *ui, Rect r, Notice *n, f32 alpha, bool hovered, bool card)
{
	Theme *t = &ui->theme;
	if (card)
		shell_card(ui, r, 16, alpha);
	else if (hovered)
		ui_round(ui, r, 14, color_scale_alpha(t->hover, alpha));
	Rect c = pad(r, 14);
	Rect icon = cut_left(&c, 36);
	const AppInfo *a = shell_app(n->app);
	if (a)
		ui_app_icon(ui, rect(icon.x, icon.y + 2, 36, 36), a->icon);
	else
		ui_icon(ui, rect(icon.x + 6, icon.y + 8, 24, 24), n->icon[0] ? n->icon : "bell", color_scale_alpha(t->text_dim, alpha));
	cut_left(&c, 12);
	char when[16];
	ago(n->time, when, sizeof when);
	Font sf = ui_font(ui, 11, 500);
	f32 ww = ui_text_width(ui, sf, when);
	ui_text_in(ui, sf, rect(c.x + c.w - ww - (hovered ? 22 : 0), c.y, ww, 18), when, color_scale_alpha(t->text_faint, alpha), ALIGN_END);
	ui_text_in(ui, ui_font(ui, 13, 600), rect(c.x, c.y, c.w - ww - 30, 18), n->title, color_scale_alpha(t->text, alpha), ALIGN_START);
	if (n->body[0])
		ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(c.x, c.y + 21, c.w - 4, 34), n->body, color_scale_alpha(t->text_dim, alpha), ALIGN_START);
}

static const char *const WD[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };

/* How many week rows this month takes. */
static int month_rows(void)
{
	Civil c, fc;
	civil_local(time_unix(), &c);
	Civil first = c;
	first.day = 1;
	civil_from_unix(unix_from_civil(&first), &fc);
	return ((fc.wday + 6) % 7 + days_in_month(c.year, c.month) + 6) / 7;
}

#define MONTH_H(rows) (18 + 30 + 14 + 28 + (f32)(rows) * 32)

/* The month, with today marked. */
static void month_view(Ui *ui, Rect body)
{
	Theme *t = &ui->theme;
	Civil c;
	civil_local(time_unix(), &c);
	ui_text_in(ui, ui_font(ui, 13, 500), cut_top(&body, 18), WEEKDAY_NAMES[c.wday], t->accent, ALIGN_START);
	char day[48];
	fmt(day, sizeof day, "%s %d, %d", MONTH_NAMES[c.month - 1], c.day, c.year);
	ui_text_in(ui, ui_font(ui, 20, 700), cut_top(&body, 30), day, t->text, ALIGN_START);
	cut_top(&body, 14);
	Rect grid = cut_top(&body, 28 + (f32)month_rows() * 32);
	f32 cw = grid.w / 7;
	for (int i = 0; i < 7; i++)
		ui_text_in(ui, ui_font(ui, 11, 600), rect(grid.x + (f32)i * cw, grid.y, cw, 22), WD[i], t->text_faint, ALIGN_CENTER);
	Civil first = c;
	first.day = 1;
	Civil fc;
	civil_from_unix(unix_from_civil(&first), &fc);
	int offset = (fc.wday + 6) % 7, dim = days_in_month(c.year, c.month);
	for (int d = 1; d <= dim; d++) {
		int idx = offset + d - 1;
		Rect cell = rect(grid.x + (f32)(idx % 7) * cw, grid.y + 28 + (f32)(idx / 7) * 32, cw, 30);
		char num[4];
		fmt(num, sizeof num, "%d", d);
		bool weekend = idx % 7 >= 5;
		if (d == c.day) {
			ui_circle(ui, cell.x + cw / 2, cell.y + 15, 14, t->accent);
			ui_text_in(ui, ui_font(ui, 13, 700), cell, num, t->on_accent, ALIGN_CENTER);
		} else {
			ui_text_in(ui, ui_font(ui, 13, 400), cell, num, weekend ? t->text_dim : t->text, ALIGN_CENTER);
		}
	}
}

void notifications_draw(Ui *ui)
{
	Theme *t = &ui->theme;
	/* Toasts: under the top bar in the middle, or above the taskbar at the
	 * right while the top bar is off (they wait while a panel is open). */
	if (!S.notifications_open) {
		if (S.session_menu_open || S.launcher_open)
			return;
		bool top = shell_panel_height() > 0;
		f32 y = top ? shell_panel_height() + 10 : S.sh - shell_taskbar_height() * (1 - smooth(S.dock_hide_k)) - 10;
		for (int i = S.nnotices - 1; i >= 0 && (top ? y < S.sh / 2 : y > S.sh / 2); i--) {
			Notice *n = &S.notices[i];
			if (n->dismissed)
				continue;
			ui_push_id(ui, (i64)n->id);
			n->slide = ui_anim(ui, ui_id(ui, "slide"), 1, 14);
			f32 k = smooth(n->slide);
			Rect r = top ? rect(roundf(S.sw / 2 - 200), roundf(y - (1 - k) * 18), 400, 76)
				     : rect(roundf(S.sw - 412 + (1 - k) * 60), roundf(y - 76), 400, 76);
			shell_surface(r);
			UiInteract it = ui_interact(ui, ui_id(ui, "toast"), r);
			notice_card(ui, r, n, k, it.hovered, true);
			if (it.hovered)
				ui_icon(ui, rect(r.x + r.w - 28, r.y + 14, 14, 14), "close", t->text_faint);
			if (it.clicked) {
				n->dismissed = true;
				n->read = true;
				if (shell_app(n->app))
					shell_launch(n->app, NULL);
			}
			ui_pop_id(ui);
			y += top ? 84 : -84;
		}
		return;
	}
	/* The notification center, by the clock: the month, then the list. */
	f32 k = ui_fade(ui, ui_id(ui, "nc"), true, 0.12f);
	f32 w = 380, cal = MONTH_H(month_rows());
	f32 h = 20 + cal + 22 + 38 + (S.nnotices ? 78 * (f32)S.nnotices : 150) + 20;
	Rect r = open_panel(ui, shell_panel_height() > 0 ? clock_btn.x + clock_btn.w / 2 : S.sw - w / 2 - 8, w, h, k);
	if (click_outside(ui, r) && !rect_contains(clock_btn, ui->mx, ui->my) && !rect_contains(bell_btn, ui->mx, ui->my)) {
		S.notifications_open = false;
		return;
	}
	Rect body = pad(r, 20);
	month_view(ui, cut_top(&body, cal));
	cut_top(&body, 8);
	ui_separator(ui, body.x, body.y, body.x + body.w);
	cut_top(&body, 14);
	Rect head = cut_top(&body, 30);
	ui_text_in(ui, ui_font(ui, 15, 700), head, "Notifications", t->text, ALIGN_START);
	if (S.nnotices && ui_button_flat(ui, cut_right(&head, 70), "Clear"))
		S.nnotices = 0;
	cut_top(&body, 8);
	if (S.nnotices == 0) {
		ui_placeholder(ui, body, "bell", "All caught up", S.set.do_not_disturb ? "Do Not Disturb is on." : NULL);
		return;
	}
	f32 y0 = ui_scroll_begin(ui, body, "nc-list");
	f32 y = y0;
	for (int i = S.nnotices - 1; i >= 0; i--) {
		Notice *n = &S.notices[i];
		Rect card = rect(body.x, y, body.w, 74);
		ui_push_id(ui, (i64)n->id);
		UiInteract it = ui_interact(ui, ui_id(ui, "row"), card);
		notice_card(ui, card, n, k, it.hovered, false);
		if (it.hovered && ui_icon_button(ui, rect(card.x + card.w - 32, card.y + 8, 24, 24), "close", NULL)) {
			memmove(&S.notices[i], &S.notices[i + 1], sizeof(Notice) * (usize)(S.nnotices - i - 1));
			S.nnotices--;
		} else if (it.clicked && shell_app(n->app)) {
			shell_launch(n->app, NULL);
			S.notifications_open = false;
		}
		ui_pop_id(ui);
		y += 78;
	}
	ui_scroll_end(ui, y - y0);
}

/* ---- Quick settings ------------------------------------------------------------------ */

static bool quick_toggle(Ui *ui, Rect r, const char *icon, const char *label, const char *sub, bool on)
{
	Theme *t = &ui->theme;
	UiId id = ui_id(ui, label);
	UiInteract it = ui_interact(ui, id, r);
	f32 k = ui_anim(ui, id, on ? 1.0f : 0.0f, 18);
	f32 hover = ui_fade(ui, id ^ 0x77, it.hovered, 0.12f);
	Color off = t->dark ? color_hex("#ffffff12") : color_hex("#0000000b");
	ui_round(ui, r, r.h / 2, color_mix(off, t->accent, k));
	if (hover > 0)
		ui_round(ui, r, r.h / 2, color_scale_alpha(t->hover, hover));
	Color fg = color_mix(t->text, t->on_accent, k);
	ui_icon(ui, rect(r.x + 16, r.y + (r.h - 18) / 2, 18, 18), icon, fg);
	ui_text_in(ui, ui_font(ui, 13, 600), rect(r.x + 44, r.y + (sub ? 8 : 0), r.w - 52, sub ? 18 : r.h), label, fg, ALIGN_START);
	if (sub)
		ui_text_in(ui, ui_font(ui, 11, 400), rect(r.x + 44, r.y + 26, r.w - 52, 16), sub, color_scale_alpha(fg, 0.72f), ALIGN_START);
	return it.clicked;
}

/* A thick slider with its icon inside the track. */
static bool big_slider(Ui *ui, Rect r, const char *id, const char *icon, f32 *value, f32 min)
{
	Theme *t = &ui->theme;
	UiId sid = ui_id(ui, id);
	UiInteract it = ui_interact(ui, sid, r);
	bool changed = false;
	if (it.held || it.pressed_now) {
		f32 v = clampf((ui->mx - r.x) / r.w, min, 1);
		changed = v != *value;
		*value = v;
	}
	if (it.hovered && ui->scroll_dy != 0) {
		*value = clampf(*value - ui->scroll_dy / 400, min, 1);
		changed = true;
	}
	if (it.hovered || it.held)
		ui_set_cursor(ui, CURSOR_POINTER);
	ui_round(ui, r, r.h / 2, t->dark ? color_hex("#ffffff12") : color_hex("#0000000b"));
	f32 w = maxf(r.h, r.w * clampf(*value, 0, 1));
	ui_round(ui, rect(r.x, r.y, w, r.h), r.h / 2, t->accent);
	ui_icon(ui, rect(r.x + 12, r.y + (r.h - 16) / 2, 16, 16), icon, t->on_accent);
	return changed;
}

/* The Wi-Fi networks, inside the control center. */
static bool cc_wifi;

static void cc_wifi_view(Ui *ui, Rect body)
{
	Theme *t = &ui->theme;
	Rect head = cut_top(&body, 36);
	if (ui_icon_button(ui, cut_left(&head, 32), "go-back", "Back"))
		cc_wifi = false;
	cut_left(&head, 6);
	ui_text_in(ui, ui_font(ui, 15, 700), head, "Wi-Fi", t->text, ALIGN_START);
	bool on = S.wifi_radio;
	if (ui_switch(ui, center_in(cut_right(&head, 52), 46, 26), &on))
		wifi_command(on ? "radio on" : "radio off");
	cut_top(&body, 8);
	Rect more = cut_bottom(&body, 34);
	if (ui_button_flat(ui, more, "Wi-Fi Settings…")) {
		shell_launch("settings", "wifi");
		S.session_menu_open = false;
		cc_wifi = false;
	}
	ui_separator(ui, body.x, body.y + body.h - 4, body.x + body.w);
	if (!S.wifi_radio) {
		ui_placeholder(ui, body, "network-wireless-off", "Wi-Fi Is Off", NULL);
		return;
	}
	WifiNet nets[12];
	int n = wifi_networks(nets, 12);
	if (!n) {
		ui_spinner(ui, center_in(body, 24, 24));
		return;
	}
	f32 y = body.y;
	for (int i = 0; i < n && y + 44 <= body.y + body.h - 6; i++) {
		WifiNet *w = &nets[i];
		Rect row = rect(body.x - 6, y, body.w + 12, 44);
		ui_push_id_str(ui, w->hex);
		bool ok = wifi_supported(w);
		UiInteract in = ui_row(ui, row, ui_id(ui, "net"), w->connected);
		ui_pop_id(ui);
		Color fg = ok ? t->text : t->text_faint;
		ui_icon(ui, rect(row.x + 10, row.y + 12, 20, 20), wifi_signal_icon(w->signal), w->connected ? t->accent : fg);
		ui_text_in(ui, ui_font(ui, 13, w->connected ? 700 : 500), rect(row.x + 40, row.y + 4, row.w - 80, 20), w->ssid, fg, ALIGN_START);
		const char *sub = w->connected ? (str_eq(S.wifi_state, "connected") ? "Connected" : "Connecting…") : w->saved ? "Saved" : NULL;
		if (sub)
			ui_text_in(ui, ui_font(ui, 11, 400), rect(row.x + 40, row.y + 23, row.w - 80, 16), sub, t->text_dim, ALIGN_START);
		if (!str_eq(w->security, "open"))
			ui_icon(ui, rect(row.x + row.w - 28, row.y + 14, 14, 14), "lock", t->text_faint);
		if (in.clicked && ok && !w->connected) {
			if (w->saved || str_eq(w->security, "open")) {
				wifi_connect(w->hex, NULL);
			} else {
				/* passwords are typed in Settings, which opens asking for this one */
				char arg[80];
				fmt(arg, sizeof arg, "wifi:%s", w->hex);
				shell_launch("settings", arg);
				S.session_menu_open = false;
				cc_wifi = false;
			}
		}
		y += 46;
	}
}

/* Sleep, restart, shut down and log out, inside the quick settings. */
static bool cc_power;

static void cc_power_view(Ui *ui, Rect body)
{
	Theme *t = &ui->theme;
	Rect head = cut_top(&body, 36);
	if (ui_icon_button(ui, cut_left(&head, 32), "go-back", "Back"))
		cc_power = false;
	cut_left(&head, 6);
	ui_text_in(ui, ui_font(ui, 15, 700), head, "Power", t->text, ALIGN_START);
	cut_top(&body, 8);
	static const struct { const char *icon, *label; u32 action; } items[] = {
		{ "system-sleep", "Sleep", SESSION_SLEEP },
		{ "system-restart", "Restart", SESSION_RESTART },
		{ "system-shutdown", "Shut Down", SESSION_SHUTDOWN },
		{ "lock", "Lock", SESSION_LOCK },
		{ "system-log-out", "Log Out", SESSION_LOGOUT },
	};
	for (usize i = 0; i < ARRAY_LEN(items); i++) {
		if (i == 3) {
			ui_separator(ui, body.x + 6, body.y + 5, body.x + body.w - 6);
			cut_top(&body, 10);
		}
		if (ui_menu_item(ui, cut_top(&body, 40), items[i].icon, items[i].label, NULL)) {
			S.session_menu_open = false;
			cc_power = false;
			shell_session_action(items[i].action);
		}
	}
}

/* A round icon button in the quick settings' header. */
static bool cc_round_button(Ui *ui, Rect r, const char *icon, const char *tip)
{
	Theme *t = &ui->theme;
	UiId id = ui_id(ui, tip);
	UiInteract it = ui_interact(ui, id, r);
	f32 hover = ui_fade(ui, id, it.hovered, 0.12f);
	Color base = t->dark ? color_hex("#ffffff12") : color_hex("#0000000b");
	ui_circle(ui, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, color_mix(base, t->dark ? color_hex("#ffffff24") : color_hex("#00000018"), hover));
	ui_icon(ui, center_in(r, 16, 16), icon, t->text);
	ui_tooltip(ui, tip);
	return it.clicked;
}

void controlcenter_draw(Ui *ui)
{
	if (!S.session_menu_open) {
		cc_wifi = cc_power = false;
		return;
	}
	Theme *t = &ui->theme;
	f32 k = ui_fade(ui, ui_id(ui, "cc"), true, 0.12f);
	f32 w = 360, h = 18 + 40 + 18 + 52 + 10 + 52 + 20 + 36 + (S.brightness >= 0 ? 48 : 0) + 20;
	if (cc_wifi)
		h = maxf(h, 400);
	else if (cc_power)
		h = maxf(h, 18 + 36 + 8 + 5 * 40 + 10 + 18);
	Rect r = open_panel(ui, status_btn.x + status_btn.w / 2, w, h, k);
	if (click_outside(ui, r) && !rect_contains(status_btn, ui->mx, ui->my)) {
		S.session_menu_open = false;
		return;
	}
	Rect body = pad(r, 18);
	if (cc_wifi) {
		cc_wifi_view(ui, body);
		return;
	}
	if (cc_power) {
		cc_power_view(ui, body);
		return;
	}
	/* who, and the session buttons */
	Rect user = cut_top(&body, 40);
	ui_avatar(ui, cut_left(&user, 40), S.set.full_name[0] ? S.set.full_name : S.set.user_name);
	cut_left(&user, 12);
	Rect power = cut_right(&user, 36);
	cut_right(&user, 8);
	Rect lock = cut_right(&user, 36);
	cut_right(&user, 8);
	Rect settings = cut_right(&user, 36);
	cut_right(&user, 8);
	ui_text_in(ui, ui_font(ui, 14, 700), rect(user.x, user.y + 2, user.w, 20), S.set.full_name[0] ? S.set.full_name : S.set.user_name, t->text, ALIGN_START);
	char sub[48];
	if (S.battery >= 0)
		fmt(sub, sizeof sub, "%d%%%s", S.battery, S.charging ? " · charging" : " battery");
	else
		str_copy(sub, OS_PRETTY_NAME, sizeof sub);
	ui_text_in(ui, ui_font(ui, 12, 400), rect(user.x, user.y + 22, user.w, 16), sub, t->text_dim, ALIGN_START);
	if (cc_round_button(ui, center_in(settings, 36, 36), "settings", "Settings")) {
		shell_launch("settings", NULL);
		S.session_menu_open = false;
	}
	if (cc_round_button(ui, center_in(lock, 36, 36), "lock", "Lock")) {
		S.session_menu_open = false;
		shell_session_action(SESSION_LOCK);
	}
	if (cc_round_button(ui, center_in(power, 36, 36), "system-shutdown", "Power"))
		cc_power = true;
	cut_top(&body, 18);

	/* toggles */
	Rect row = cut_top(&body, 52);
	Rect a = cut_left(&row, (row.w - 10) / 2);
	cut_left(&row, 10);
	/* with a Wi-Fi adapter the tile is about Wi-Fi (a cable shows in the Horizon anyway) */
	bool wifi_tile = S.wifi_available;
	bool wifi_on = str_eq(S.wifi_state, "connected");
	char ssid[64] = "";
	if (wifi_tile && wifi_on) {
		Conf *net = conf_load("/run/zurbax/network.status");
		str_copy(ssid, conf_get(net, "wifi_ssid", "Connected"), sizeof ssid);
		conf_free(net);
	}
	const char *net_sub = wifi_tile ? (!S.wifi_radio ? "Off" : wifi_on ? ssid : str_eq(S.wifi_state, "connecting") ? "Connecting…" : "Not connected")
					: S.wifi_connected ? (S.net_wired ? "Wired" : S.wifi_name) : "Not connected";
	if (quick_toggle(ui, a, wifi_tile ? "network-wireless" : S.net_wired ? "network-wired" : "network-wireless", wifi_tile ? "Wi-Fi" : "Network",
			 net_sub, wifi_tile ? wifi_on : S.wifi_connected)) {
		if (wifi_tile) {
			cc_wifi = true;
			wifi_command("scan");
		} else {
			shell_launch("settings", "network");
			S.session_menu_open = false;
		}
	}
	if (quick_toggle(ui, row, "dark-mode", "Dark Style", S.set.dark ? "On" : "Off", S.set.dark)) {
		S.set.dark = !S.set.dark;
		settings_save();
		server_broadcast_theme();
		damage_all();
	}
	cut_top(&body, 10);
	row = cut_top(&body, 52);
	a = cut_left(&row, (row.w - 10) / 2);
	cut_left(&row, 10);
	if (quick_toggle(ui, a, "night-light", "Night Light", S.set.night_light ? "On" : "Off", S.set.night_light)) {
		S.set.night_light = !S.set.night_light;
		settings_save();
		settings_apply();
	}
	if (quick_toggle(ui, row, "notifications-disabled", "Do Not Disturb", S.set.do_not_disturb ? "On" : "Off", S.set.do_not_disturb)) {
		S.set.do_not_disturb = !S.set.do_not_disturb;
		settings_save();
	}
	cut_top(&body, 20);

	/* sound and brightness */
	f32 v = S.muted ? 0 : (f32)S.volume / 100;
	if (big_slider(ui, cut_top(&body, 36), "cc-volume", volume_icon(), &v, 0))
		set_volume((int)roundf(v * 100));
	if (S.brightness >= 0) {
		cut_top(&body, 12);
		f32 b = (f32)S.brightness / 100;
		if (big_slider(ui, cut_top(&body, 36), "cc-brightness", "display-brightness", &b, 0.05f))
			set_brightness((int)roundf(b * 100));
	}
}

/* ---- Window switcher ------------------------------------------------------------------ */

void switcher_draw(Ui *ui)
{
	Window *list[32];
	int n = 0;
	for (int i = S.nwin - 1; i >= 0 && n < 32; i--)
		if (!S.windows[i]->closing && !(S.windows[i]->flags & WIN_POPUP))
			list[n++] = S.windows[i];
	if (n == 0)
		return;
	int sel = ((S.switcher_index % n) + n) % n;
	f32 cell = 104, w = minf((f32)n * cell + 32, S.sw - 32), h = 168;
	Rect r = center_in(rect(0, 0, S.sw, S.sh), w, h);
	shell_surface(r);
	ui_shadow(ui, r, 24, 30, 8, color_hex("#00000070"));
	dark_glass(ui, r, 24, 0.62f);
	ui_border(ui, r, 24, 1, color_hex("#ffffff1c"));
	f32 x0 = r.x + 16 - maxf(0, (f32)(sel + 1) * cell - (w - 32));   /* keep the selection in view */
	ui_push_clip(ui, pad(r, 8));
	for (int i = 0; i < n; i++) {
		Rect c = rect(x0 + (f32)i * cell, r.y + 16, cell, 104);
		if (i == sel)
			ui_round(ui, pad(c, 4), 18, color_hex("#ffffff24"));
		const AppInfo *a = shell_app(list[i]->app_id);
		ui_app_icon(ui, rect(c.x + (c.w - 64) / 2, c.y + 20, 64, 64), a ? a->icon : list[i]->app_id);
	}
	ui_pop_clip(ui);
	const AppInfo *a = shell_app(list[sel]->app_id);
	const char *title = list[sel]->title[0] ? list[sel]->title : (a ? a->name : list[sel]->app_id);
	ui_text_in(ui, ui_font(ui, 13, 600), rect(r.x + 16, r.y + h - 44, w - 32, 24), title, COLOR_WHITE, ALIGN_CENTER);
}

/* ---- Lock screen ----------------------------------------------------------------------
 * The wallpaper as it is, the date over a large clock, waiting notifications
 * on frosted glass, and at the bottom the user with a password field. Moving
 * the mouse or typing softly blurs the picture behind, to bring the password
 * forward; left alone, it clears again. Unlocking slides everything up. */

static StrBuf lock_password;
static f64 lock_error_at;

bool lockscreen_key(Ui *ui, u32 code, u32 mods, const char *text)
{
	UNUSED(ui), UNUSED(mods), UNUSED(text);
	/* Without a password, any key unlocks. */
	if (!S.set.has_password && code != KEY_LEFTSHIFT && code != KEY_LEFTMETA) {
		S.locked = false;
		return true;
	}
	return false;
}

/* Frosted glass: the blurred wallpaper behind r, lightened. */
static void glass(Ui *ui, Rect r, f32 radius, f32 alpha, Color tint)
{
	Rect d = ui_to_device(ui, r);
	blit_scaled_round(ui->canvas, S.wallpaper_blur, d, d, radius * S.scale, alpha);
	ui_round(ui, r, radius, color_scale_alpha(tint, alpha));
	ui_border(ui, r, radius, 1, color_scale_alpha(color_hex("#ffffff2e"), alpha));
}

static void lock_text_center(Ui *ui, Font f, f32 cx, f32 baseline, const char *s, f32 alpha)
{
	panel_text(ui, f, cx - ui_text_width(ui, f, s) / 2, baseline, s, alpha);
}

static void ago(i64 when, char *out, usize cap)
{
	i64 d = time_unix() - when;
	if (d < 60)
		str_copy(out, "now", cap);
	else if (d < 3600)
		fmt(out, cap, "%dm ago", (int)(d / 60));
	else if (d < 86400)
		fmt(out, cap, "%dh ago", (int)(d / 3600));
	else
		fmt(out, cap, "%dd ago", (int)(d / 86400));
}

/* Waiting notifications under the clock, newest first; returns the height used. */
static f32 lock_notices(Ui *ui, f32 cx, f32 y, f32 k)
{
	const Notice *list[3];
	int n = 0, more = 0;
	for (int i = S.nnotices - 1; i >= 0; i--) {
		if (S.notices[i].dismissed)
			continue;
		if (n < 3)
			list[n++] = &S.notices[i];
		else
			more++;
	}
	f32 w = minf(400, S.sw - 48), h = 64, x = cx - w / 2;
	for (int i = 0; i < n; i++) {
		Rect card = rect(x, y + (f32)i * (h + 8), w, h);
		glass(ui, card, 18, k, color_hex("#ffffff24"));
		const AppInfo *a = shell_app(list[i]->app);
		ui_app_icon(ui, rect(card.x + 14, card.y + 14, 36, 36), a ? a->icon : list[i]->icon);
		char when[16];
		ago(list[i]->time, when, sizeof when);
		Font tf = ui_font(ui, 13, 700), bf = ui_font(ui, 13, 400), sf = ui_font(ui, 11, 500);
		f32 ww = ui_text_width(ui, sf, when);
		ui_text(ui, sf, card.x + card.w - 14 - ww, card.y + 24, when, color_scale_alpha(color_hex("#ffffffb0"), k));
		ui_push_clip(ui, rect(card.x + 62, card.y, card.w - 62 - 22 - ww, card.h));
		ui_text(ui, tf, card.x + 62, card.y + 26, list[i]->title, color_scale_alpha(COLOR_WHITE, k));
		ui_pop_clip(ui);
		ui_push_clip(ui, rect(card.x + 62, card.y, card.w - 76, card.h));
		ui_text(ui, bf, card.x + 62, card.y + 46, list[i]->body[0] ? list[i]->body : (a ? a->name : ""),
			color_scale_alpha(color_hex("#ffffffd8"), k));
		ui_pop_clip(ui);
	}
	f32 used = (f32)n * (h + 8);
	if (more) {
		char m[48];
		fmt(m, sizeof m, "%d more", more);
		lock_text_center(ui, ui_font(ui, 12, 600), cx, y + used + 14, m, k * 0.8f);
		used += 22;
	}
	return used;
}

static void lock_status(Ui *ui, f32 k)
{
	f32 x = S.sw - 20, base = 26;
	Font small = ui_font(ui, 13, 600);
	Color white = color_scale_alpha(COLOR_WHITE, k);
	if (S.battery >= 0) {
		char pct[8];
		fmt(pct, sizeof pct, "%d%%", S.battery);
		x -= ui_text_width(ui, small, pct);
		panel_text(ui, small, x, base, pct, k);
		x -= 26;
		ui_icon(ui, rect(x, base - 14, 20, 20), S.charging ? "battery-charging" : S.battery > 80 ? "battery-full" : S.battery > 45 ? "battery-good"
						      : S.battery > 15 ? "battery-low" : "battery-caution", white);
		x -= 14;
	}
	x -= 20;
	ui_icon(ui, rect(x, base - 14, 20, 20),
		S.wifi_connected && S.net_wired ? "network-wired"
		: S.wifi_connected ? (S.wifi_strength > 66 ? "network-wireless" : S.wifi_strength > 33 ? "network-wireless-good" : "network-wireless-weak")
				   : "network-wireless-none",
		white);
	x -= 12;
	char layout[8] = "US";
	for (int i = 0; i < 2 && S.set.keyboard_layout[i]; i++)
		layout[i] = (char)to_upper(S.set.keyboard_layout[i]);
	x -= ui_text_width(ui, small, layout);
	panel_text(ui, small, x, base, layout, k * 0.9f);
}

/* Sleep, Restart and Shut Down, bottom right. */
static void lock_power(Ui *ui, f32 k)
{
	static const struct { const char *icon, *label; u32 action; } items[] = {
		{ "system-sleep", "Sleep", SESSION_SLEEP },
		{ "system-restart", "Restart", SESSION_RESTART },
		{ "system-shutdown", "Shut Down", SESSION_SHUTDOWN },
	};
	f32 size = 40, gap = 30, y = S.sh - 86;
	f32 x = S.sw - 28 - (3 * size + 2 * gap) - 10;
	for (int i = 0; i < 3; i++) {
		Rect b = rect(x + (f32)i * (size + gap), y, size, size);
		UiInteract it = ui_interact(ui, ui_id(ui, items[i].label), rect_inset(b, -14, -6));
		f32 hover = ui_fade(ui, ui_id(ui, items[i].label), it.hovered, 0.12f);
		glass(ui, b, size / 2, k, color_mix(color_hex("#ffffff1c"), color_hex("#ffffff48"), hover));
		ui_icon(ui, rect(b.x + 10, b.y + 10, 20, 20), items[i].icon, color_scale_alpha(COLOR_WHITE, k));
		lock_text_center(ui, ui_font(ui, 11, 600), b.x + size / 2, b.y + size + 17, items[i].label, k * (0.7f + 0.3f * hover));
		if (it.clicked)
			shell_session_action(items[i].action);
	}
}

void lockscreen_draw(Ui *ui)
{
	S.lock_k = ui_anim(ui, ui_id(ui, "lock-k"), S.locked ? 1.0f : 0.0f, 9);
	f32 k = S.lock_k;
	if (k <= 0.001f)
		return;
	shell_surface(rect(0, 0, S.sw, S.sh));
	/* the picture: clear at rest, blurred and dimmed while someone is at the keyboard */
	bool attention = S.now - S.last_input < 8 || lock_password.len > 0 || S.now - lock_error_at < 3;
	f32 focus = ui_anim(ui, ui_id(ui, "lock-focus"), attention ? 1.0f : 0.0f, 6);
	if (attention)
		ui->wake_at = S.last_input + 8.05;
	Rect full = rect(0, 0, S.sw, S.sh), d = ui_to_device(ui, full);
	blit_scaled(ui->canvas, S.wallpaper, rect(0, 0, (f32)S.wallpaper->width, (f32)S.wallpaper->height), d, k);
	if (focus > 0.001f) {
		blit_scaled(ui->canvas, S.wallpaper_blur, rect(0, 0, (f32)S.wallpaper_blur->width, (f32)S.wallpaper_blur->height), d, k * focus);
		ui_fill(ui, full, color_scale_alpha(color_hex("#00000038"), k * focus));
	}
	/* a soft shade behind the clock and the user keeps white text readable on any picture */
	Paint top = paint_linear(0, 0, 0, S.sh * 0.42f * S.scale, color_scale_alpha(color_hex("#0000004a"), k), color_hex("#00000000"));
	fill_rect_paint(ui->canvas, rect(d.x, d.y, d.w, d.h * 0.42f), &top);
	Paint bottom = paint_linear(0, S.sh * 0.6f * S.scale, 0, S.sh * S.scale, color_hex("#00000000"), color_scale_alpha(color_hex("#00000058"), k));
	fill_rect_paint(ui->canvas, rect(d.x, d.y + d.h * 0.6f, d.w, d.h * 0.4f), &bottom);
	/* unlocking lifts it all away */
	f32 lift = (1 - k) * (1 - k) * S.sh * 0.18f;
	f32 cx = S.sw / 2;

	Civil c;
	civil_local(time_unix(), &c);
	char clock[16], date[64];
	if (S.set.clock_24h)
		fmt(clock, sizeof clock, "%d:%02d", c.hour, c.min);
	else
		fmt(clock, sizeof clock, "%d:%02d", c.hour % 12 ? c.hour % 12 : 12, c.min);
	fmt(date, sizeof date, "%s %d %s", WEEKDAY_NAMES[c.wday], c.day, MONTH_NAMES[c.month - 1]);
	f32 date_base = maxf(64, S.sh * 0.11f) - lift;
	lock_text_center(ui, ui_font(ui, 22, 600), cx, date_base, date, k * 0.92f);
	Font big = ui_font(ui, S.sh < 700 ? 104 : 136, 650);
	f32 clock_base = date_base + 26 + font_cap_height(big);
	f32 cw = ui_text_width(ui, big, clock);
	ui_text(ui, big, cx - cw / 2, clock_base + 3, clock, color_scale_alpha(color_hex("#00000038"), k));
	ui_text(ui, big, cx - cw / 2, clock_base, clock, color_scale_alpha(color_hex("#fffffff2"), k));
	/* wake for the next minute */
	f64 next_minute = S.now + (f64)(60 - c.sec) + 0.05;
	if (ui->wake_at <= 0 || ui->wake_at > next_minute)
		ui->wake_at = next_minute;
	lock_notices(ui, cx, clock_base + 40, k);

	/* the user, and the password */
	f32 field_y = S.sh - 112 + lift * 0.5f;
	f32 av = 64;
	ui_avatar(ui, rect(cx - av / 2, field_y - av - 40, av, av), S.set.full_name[0] ? S.set.full_name : S.set.user_name);
	lock_text_center(ui, ui_font(ui, 15, 700), cx, field_y - 14, S.set.full_name[0] ? S.set.full_name : S.set.user_name, k);
	if (S.set.has_password) {
		f32 shake = 0;
		if (S.now - lock_error_at < 0.45) {
			f32 t = (f32)(S.now - lock_error_at);
			shake = sinf(t * 60) * 10 * (1 - t / 0.45f);
			ui->redraw = true;
		}
		Rect pill = rect(cx - 120 + shake, field_y, 240, 34);
		glass(ui, pill, 17, k, color_hex("#ffffff30"));
		/* white text on the glass, whatever the theme */
		Theme saved = ui->theme;
		ui->theme.text = COLOR_WHITE;
		ui->theme.text_dim = ui->theme.text_faint = color_hex("#ffffffa8");
		ui->theme.accent = COLOR_WHITE;
		Rect entry = rect(pill.x + 14, pill.y + 1, pill.w - 14 - 36, pill.h - 2);
		int ev = ui_entry(ui, entry, "lock-password", &lock_password, "Enter Password", ENTRY_PASSWORD | ENTRY_AUTOFOCUS | ENTRY_FLAT);
		ui->theme = saved;
		ui->focus = ui_id(ui, "lock-password");
		/* the arrow: unlock with the mouse */
		Rect go = rect(pill.x + pill.w - 31, pill.y + 4, 26, 26);
		bool have = lock_password.len > 0;
		UiInteract it = ui_interact(ui, ui_id(ui, "lock-go"), go);
		if (have) {
			ui_round(ui, go, 13, color_scale_alpha(color_hex(it.hovered ? "#ffffff60" : "#ffffff40"), k));
			ui_icon(ui, rect(go.x + 5, go.y + 5, 16, 16), "go-forward", color_scale_alpha(COLOR_WHITE, k));
		}
		if ((ev & ENTRY_SUBMIT) || (have && it.clicked)) {
			if (shell_check_password(sb_str(&lock_password))) {
				S.locked = false;
			} else {
				lock_error_at = S.now;
			}
			sb_clear(&lock_password);
		}
		const char *hint = S.now - lock_error_at < 2.5 ? "Incorrect password. Try again."
				   : (S.mods & MOD_CAPS) ? "Caps Lock is on"
				   : "Type your password, then press Enter";
		lock_text_center(ui, ui_font(ui, 12, 600), cx, field_y + 58, hint, k * (S.now - lock_error_at < 2.5 ? 1 : 0.78f));
	} else {
		lock_text_center(ui, ui_font(ui, 13, 600), cx, field_y + 22, "Click or press any key to unlock", k * 0.85f);
		if (ui->pressed[BUTTON_LEFT] && ui->my < S.sh - 110)
			S.locked = false;
	}
	lock_status(ui, k);
	lock_power(ui, k);
}

/* ---- OSD -------------------------------------------------------------------------------- */

void osd_draw(Ui *ui)
{
	bool show = S.osd_until > 0 && S.now < S.osd_until;
	f32 k = ui_fade(ui, ui_id(ui, "osd"), show, 0.15f);
	if (k <= 0.001f)
		return;
	Rect r = rect(roundf(S.sw / 2 - 140), roundf(S.sh - shell_taskbar_reserved() - 90 + (1 - k) * 12), 280, 52);
	shell_surface(r);
	ui_shadow(ui, r, 26, 18, 5, color_scale_alpha(color_hex("#00000055"), k));
	ui_round(ui, r, 26, color_scale_alpha(color_hex("#16171bf2"), k));
	ui_border(ui, r, 26, 1, color_scale_alpha(color_hex("#ffffff1a"), k));
	ui_icon(ui, rect(r.x + 18, r.y + 16, 20, 20), S.osd_icon, color_scale_alpha(COLOR_WHITE, k));
	char pct[8];
	fmt(pct, sizeof pct, "%d", (int)roundf(clampf(S.osd_value, 0, 1) * 100));
	Font f = ui_font(ui, 13, 600);
	ui_text_in(ui, f, rect(r.x + r.w - 52, r.y, 34, r.h), pct, color_scale_alpha(color_hex("#ffffffc0"), k), ALIGN_END);
	Rect bar = rect(r.x + 52, r.y + 23, r.w - 52 - 62, 6);
	ui_round(ui, bar, 3, color_scale_alpha(color_hex("#ffffff2a"), k));
	ui_round(ui, rect(bar.x, bar.y, maxf(6, bar.w * clampf(S.osd_value, 0, 1)), bar.h), 3, color_scale_alpha(COLOR_WHITE, k));
}
