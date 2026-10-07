/*
 * Shell surfaces: everything the desktop draws itself.
 *
 *   panel          the bar across the top: Applications, clock, indicators
 *   dock           pinned and running apps along the bottom
 *   launcher       the Applications menu: search plus a grid of apps
 *   notifications  toasts at the top right, and the notification center
 *   control center quick toggles, sliders and session buttons
 *   switcher       Alt+Tab
 *   lock screen    clock, user and password
 *   osd            volume and brightness feedback
 */
#include "zdisplay.h"


/* ---- Helpers ---------------------------------------------------------------------- */

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

static bool any_maximized(void)
{
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		if (!w->closing && !w->minimized && w->mapped && (w->states & (STATE_MAXIMIZED | STATE_TILED_LEFT | STATE_TILED_RIGHT)))
			return true;
	}
	return false;
}

/* A popover panel hanging from the panel, with an arrow. */
static Rect panel_popover(Ui *ui, f32 anchor_x, f32 w, f32 h, f32 k)
{
	Theme *t = &ui->theme;
	f32 x = clampf(anchor_x - w / 2, 6, S.sw - w - 6);
	f32 y = PANEL_HEIGHT + 8 - (1 - k) * 8;
	Rect r = { x, y, w, h };
	Color bg = t->dark ? color_hex("#2e2f34") : color_hex("#fbfbfc");
	ui_shadow(ui, r, 12, 18, 6, color_scale_alpha(color_hex("#00000055"), k));
	Path p = { 0 };
	f32 ax = clampf(anchor_x, x + 18, x + w - 18);
	path_move(&p, ax - 9, y + 1);
	path_line(&p, ax, y - 8);
	path_line(&p, ax + 9, y + 1);
	path_close(&p);
	ui_round(ui, r, 12, color_scale_alpha(bg, k));
	ui_path_fill(ui, &p, color_scale_alpha(bg, k));
	path_free(&p);
	ui_border(ui, r, 12, 1, color_scale_alpha(t->border_strong, k));
	shell_surface(rect_inset(r, 0, -10));
	return r;
}

static bool click_outside(Ui *ui, Rect r)
{
	return ui->pressed[BUTTON_LEFT] && !rect_contains(r, ui->mx, ui->my);
}

/* ---- Panel ------------------------------------------------------------------------- */

static Rect panel_apps_btn, panel_clock_btn, panel_status_btn;

static bool panel_button(Ui *ui, Rect r, const char *id, bool active)
{
	UiId bid = ui_id(ui, id);
	UiInteract it = ui_interact(ui, bid, r);
	f32 hover = ui_fade(ui, bid, it.hovered || active, 0.12f);
	f32 press = ui_fade(ui, ui_id(ui, "press"), it.held || active, 0.08f);
	if (hover > 0) {
		Rect pill = rect_inset(r, 0, 4);
		ui_round(ui, pill, pill.h / 2, color_scale_alpha(color_hex("#ffffff"), 0.13f * hover + 0.07f * press));
		ui_border(ui, pill, pill.h / 2, 1, color_scale_alpha(color_hex("#ffffff"), 0.10f * hover));
	}
	return it.clicked;
}

/* Frosted glass: the blurred wallpaper behind the bar, a cool tint and a
 * fine highlight along the bottom edge. Turns solid behind maximized windows. */
static void panel_background(Ui *ui, Rect bar, f32 solid)
{
	Rect d = ui_to_device(ui, bar);
	d.w = minf(d.w, (f32)S.wallpaper_blur->width);
	d.h = minf(d.h, (f32)S.wallpaper_blur->height);
	Canvas glass = canvas_view(S.wallpaper_blur, d);
	blit(ui->canvas, &glass, (int)d.x, (int)d.y, 1);
	Paint tint = paint_linear(0, d.y, 0, d.y + d.h, color_hex("#0d122088"), color_hex("#0d122060"));
	fill_rect_paint(ui->canvas, d, &tint);
	if (solid > 0)
		ui_fill(ui, bar, color_scale_alpha(color_hex("#16171b"), solid));
	/* A soft top sheen, then a light hairline and a dark one below it. */
	Paint sheen = paint_linear(0, d.y, 0, d.y + d.h * 0.5f, color_hex("#ffffff12"), color_hex("#ffffff00"));
	fill_rect_paint(ui->canvas, rect(d.x, d.y, d.w, d.h * 0.5f), &sheen);
	ui_fill(ui, rect(bar.x, bar.y + bar.h - 1, bar.w, 1), color_hex("#ffffff1c"));
	ui_fill(ui, rect(bar.x, bar.y + bar.h, bar.w, 1), color_hex("#00000040"));
	Paint drop = paint_linear(0, d.y + d.h + S.scale, 0, d.y + d.h + 6 * S.scale, color_hex("#00000022"), color_hex("#00000000"));
	fill_rect_paint(ui->canvas, rect(d.x, d.y + d.h + S.scale, d.w, 5 * S.scale), &drop);
}

void panel_draw(Ui *ui)
{
	Rect bar = rect(0, 0, S.sw, PANEL_HEIGHT);
	shell_surface(rect(0, 0, S.sw, PANEL_HEIGHT + 8));
	f32 solid = ui_fade(ui, ui_id(ui, "panel-solid"), any_maximized(), 0.25f);
	panel_background(ui, bar, solid);
	Font bold = ui_font(ui, 13, 700), medium = ui_font(ui, 13, 500);
	f32 base = roundf((PANEL_HEIGHT + font_cap_height(bold)) / 2);

	/* Applications: the Zurbax mark and the label. */
	const char *apps = "Applications";
	f32 aw = 8 + 16 + 7 + ui_text_width(ui, bold, apps) + 12;
	panel_apps_btn = rect(4, 0, aw, PANEL_HEIGHT);
	if (panel_button(ui, panel_apps_btn, "panel-apps", S.launcher_open)) {
		S.launcher_open = !S.launcher_open;
		if (S.launcher_open)
			shell_reload_apps();   /* Harbor may have added apps */
		S.notifications_open = S.session_menu_open = false;
	}
	f32 iy = (PANEL_HEIGHT - 16) / 2;
	ui_icon(ui, rect(12.5f, iy + 1, 16, 16), "zurbax-logo", color_hex("#00000055"));
	ui_icon(ui, rect(12, iy, 16, 16), "zurbax-logo", COLOR_WHITE);
	panel_text(ui, bold, 12 + 16 + 7, base, apps, 1);

	/* Date in medium weight, the time in bold beside it. */
	char date[48], time[32];
	{
		Civil c;
		civil_local(time_unix(), &c);
		static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
		static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
		fmt(date, sizeof date, "%s %d %s", days[c.wday], c.day, months[c.month - 1]);
		format_clock(time, sizeof time, false);
	}
	f32 dw = ui_text_width(ui, medium, date), tw = ui_text_width(ui, bold, time), gap = 9;
	f32 cw = dw + gap + tw, cx = roundf((S.sw - cw) / 2);
	panel_clock_btn = rect(cx - 14, 0, cw + 28, PANEL_HEIGHT);
	if (panel_button(ui, panel_clock_btn, "panel-clock", S.notifications_open)) {
		S.notifications_open = !S.notifications_open;
		S.launcher_open = S.session_menu_open = false;
		for (int i = 0; i < S.nnotices; i++)
			S.notices[i].read = true;
	}
	panel_text(ui, medium, cx, base, date, 0.82f);
	ui_circle(ui, cx + dw + gap / 2, base - font_cap_height(bold) / 2, 1.3f, color_scale_alpha(COLOR_WHITE, 0.55f));
	panel_text(ui, bold, cx + dw + gap, base, time, 1);
	int unread = 0;
	for (int i = 0; i < S.nnotices; i++)
		unread += !S.notices[i].read;
	if (unread)
		ui_circle(ui, cx + cw + 6, base - font_cap_height(bold) - 1, 3, ui->theme.accent);

	/* Status: network, sound and battery grouped in one pill, power apart. */
	f32 icon = 16;
	f32 x = S.sw - 6;
	Color white = COLOR_WHITE, shadow = color_hex("#00000055");
#define STATUS_ICON(name) do { x -= icon; \
		ui_icon(ui, rect(x + 0.5f, iy + 1, icon, icon), name, shadow); \
		ui_icon(ui, rect(x, iy, icon, icon), name, white); } while (0)
	Rect power = rect(x - 30, 0, 30, PANEL_HEIGHT);
	bool power_click = panel_button(ui, power, "panel-power", false);
	x -= 7;
	STATUS_ICON("system-shutdown");
	x -= 7 + 6;
	f32 right = x;
	x -= 10;
	if (S.battery >= 0) {
		if (S.set.show_battery_percent) {
			char pct[8];
			fmt(pct, sizeof pct, "%d%%", S.battery);
			Font small = ui_font(ui, 12, 600);
			x -= ui_text_width(ui, small, pct);
			panel_text(ui, small, x, base, pct, 1);
			x -= 4;
		}
		STATUS_ICON(S.charging ? "battery-charging" : S.battery > 80 ? "battery-full" : S.battery > 45 ? "battery-good" :
							   S.battery > 15 ? "battery-low" : "battery-caution");
		x -= 12;
	}
	STATUS_ICON(S.muted || S.volume == 0 ? "audio-volume-muted" : S.volume < 34 ? "audio-volume-low" :
						   S.volume < 67 ? "audio-volume-medium" : "audio-volume-high");
	x -= 12;
	STATUS_ICON(S.wifi_connected && S.net_wired ? "network-wired"
		    : S.wifi_connected ? (S.wifi_strength > 66 ? "network-wireless" : S.wifi_strength > 33 ? "network-wireless-good" : "network-wireless-weak")
				       : "network-wireless-none");
	if (S.set.do_not_disturb) {
		x -= 12;
		STATUS_ICON("notifications-disabled");
	}
	char pad_names[PAD_MAX][64];
	if (input_gamepads(pad_names, PAD_MAX) > 0) {
		x -= 12;
		STATUS_ICON("gamepad");
	}
	x -= 10;
#undef STATUS_ICON
	panel_status_btn = rect(x, 0, right - x, PANEL_HEIGHT);
	/* The group sits in a faint pill even when idle, so it reads as one control. */
	Rect pill = rect_inset(panel_status_btn, 0, 4);
	ui_round(ui, pill, pill.h / 2, color_hex("#ffffff0d"));
	if (panel_button(ui, panel_status_btn, "panel-status", S.session_menu_open) || power_click) {
		S.session_menu_open = !S.session_menu_open;
		S.launcher_open = S.notifications_open = false;
	}
}

/* ---- Dock -------------------------------------------------------------------------- */

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

static struct {
	char id[48];
	f64 started;
} bouncing;

#define GOLD color_hex("#f5b83a")

/* A glass bubble: a dome of light over a dark (or pale) disc, with a rim. */
static void dock_bubble(Ui *ui, f32 cx, f32 cy, f32 r, f32 hover)
{
	Theme *t = &ui->theme;
	ui_shadow(ui, rect(cx - r, cy - r + 2, 2 * r, 2 * r), r, 12, 4, color_hex("#00000048"));
	Rect d = ui_to_device(ui, rect(cx - r, cy - r, 2 * r, 2 * r));
	Path p = { 0 };
	path_circle(&p, d.x + d.w / 2, d.y + d.h / 2, d.w / 2);
	Paint fill = t->dark ? paint_radial(d.x + d.w / 2, d.y + d.h * 0.3f, d.w * 0.8f, color_mix(color_hex("#4a4c55ec"), color_hex("#5c5f6af0"), hover),
					    color_hex("#16171ce8"))
			     : paint_radial(d.x + d.w / 2, d.y + d.h * 0.3f, d.w * 0.8f, color_hex("#fffffff2"),
					    color_mix(color_hex("#d9dce4e8"), color_hex("#e8eaf0f0"), hover));
	fill_path(ui->canvas, &p, mat_identity(), &fill, FILL_NONZERO);
	path_free(&p);
	/* the rim, and light catching the top of the glass */
	Path rim = { 0 };
	path_circle(&rim, cx, cy, r - 0.5f);
	ui_path_stroke(ui, &rim, 1, t->dark ? color_hex("#000000a0") : color_hex("#0000002c"));
	path_free(&rim);
	Path shine = { 0 };
	path_arc(&shine, cx, cy, r - 2, r - 2, PI_F * 1.15f, PI_F * 1.85f);
	ui_path_stroke(ui, &shine, 1.5f, t->dark ? color_hex("#ffffff30") : color_hex("#ffffffe0"));
	path_free(&shine);
}

/* A raindrop, the mark of the app in front. */
static void dock_drop(Ui *ui, f32 cx, f32 tip, f32 h, Color c)
{
	Path p = { 0 };
	f32 w = h * 0.62f;
	path_move(&p, cx, tip);
	path_cubic(&p, cx + w * 0.15f, tip + h * 0.3f, cx + w / 2, tip + h * 0.48f, cx + w / 2, tip + h * 0.7f);
	path_cubic(&p, cx + w / 2, tip + h * 0.88f, cx + w * 0.26f, tip + h, cx, tip + h);
	path_cubic(&p, cx - w * 0.26f, tip + h, cx - w / 2, tip + h * 0.88f, cx - w / 2, tip + h * 0.7f);
	path_cubic(&p, cx - w / 2, tip + h * 0.48f, cx - w * 0.15f, tip + h * 0.3f, cx, tip);
	path_close(&p);
	ui_path_fill(ui, &p, c);
	path_free(&p);
}

/* The golden thread the bubbles hang on: the arc itself. */
static void dock_thread(Ui *ui, int n, f32 drop, f32 alpha)
{
	if (n < 2)
		return;
	Path p = { 0 };
	for (int k = 0; k <= 4 * (n - 1); k++) {
		f32 x, y;
		shell_dock_point(n, (f32)k / 4, &x, &y);
		if (k == 0)
			path_move(&p, x, y + drop);
		else
			path_line(&p, x, y + drop);
	}
	ui_path_stroke(ui, &p, 6, color_scale_alpha(color_hex("#f5b83a22"), alpha));
	ui_path_stroke(ui, &p, 1.6f, color_scale_alpha(color_hex("#ffd884a0"), alpha));
	path_free(&p);
}

static u64 dock_menu_opened_frame;

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
	f32 w = 236, h = 16 + 34 * (f32)items + 30;
	Rect r = rect(clampf(anchor.x + anchor.w / 2 - w / 2, 6, S.sw - w - 6), anchor.y - h - 14, w, h);
	Color bg = t->dark ? color_hex("#2e2f34") : color_hex("#fbfbfc");
	ui_shadow(ui, r, 10, 16, 5, color_hex("#00000050"));
	ui_round(ui, r, 10, bg);
	Path arrow = { 0 };
	f32 ax = anchor.x + anchor.w / 2;
	path_move(&arrow, ax - 9, r.y + r.h - 1);
	path_line(&arrow, ax, r.y + r.h + 8);
	path_line(&arrow, ax + 9, r.y + r.h - 1);
	path_close(&arrow);
	ui_path_fill(ui, &arrow, bg);
	path_free(&arrow);
	ui_border(ui, r, 10, 1, t->border_strong);
	shell_surface(rect_inset(r, 0, -12));
	Rect c = pad(r, 8);
	ui_text(ui, ui_font(ui, 12, 700), c.x + 8, c.y + 16, a ? a->name : S.dock_menu, t->text_dim);
	cut_top(&c, 26);
	bool close = false;
	if (ui_menu_item(ui, cut_top(&c, 34), "tab-new", count ? "New Window" : "Open", NULL)) {
		shell_launch_new(S.dock_menu);
		close = true;
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "pin", pinned ? "Remove from Dock" : "Keep in Dock", NULL)) {
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
	if (count && ui_menu_item(ui, cut_top(&c, 34), "window-close", count > 1 ? "Quit All Windows" : "Quit", NULL)) {
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

void dock_draw(Ui *ui)
{
	char ids[32][48];
	int n = shell_dock_items(ids, 32);
	if (n == 0)
		return;
	f32 k = S.dock_hide_k * S.dock_hide_k * (3 - 2 * S.dock_hide_k);
	f32 drop = roundf(k * (DOCK_BUBBLE + DOCK_BOTTOM + DOCK_LIFT + 18));
	Rect bar = shell_dock_bar(n);
	Rect area = rect(bar.x - 120, bar.y - 70, bar.w + 240, S.sh - bar.y + 70);
	if (S.dock_hide_k >= 0.999f && !S.dock_menu[0])
		return; /* gone: nothing drawn (the spot it left is repainted as a previous surface) */
	shell_surface(area);
	dock_thread(ui, n, drop, 1 - k);

	const char *hover_name = NULL;
	Rect hover_rect = { 0 }, menu_anchor = { 0 };
	bool near = ui_mouse_in(ui, rect(bar.x - 40, bar.y - 40, bar.w + 80, S.sh - bar.y + 40));
	for (int i = 0; i < n; i++) {
		f32 cx, cy;
		shell_dock_center(n, i, &cx, &cy);
		cy += drop;
		Rect slot = rect(cx - DOCK_BUBBLE / 2.0f, cy - DOCK_BUBBLE / 2.0f, DOCK_BUBBLE, DOCK_BUBBLE);
		ui_push_id_str(ui, ids[i]);
		UiId id = ui_id(ui, "dock");
		f32 half = minf(DOCK_STEP, (S.sw - 80) / (f32)n) / 2;
		UiInteract it = ui_interact(ui, id, rect(cx - half, slot.y - 6, 2 * half, S.sh - slot.y + 6));
		f32 hover = ui_fade(ui, id, it.hovered && !S.dock_menu[0], 0.14f);
		/* the bubble lifts and grows under the pointer; with "grow" on, its neighbours swell too */
		f32 grow = 0.10f * hover;
		if (S.set.dock_magnify && near && !S.dock_menu[0]) {
			f32 dx = fabsf(ui->mx - cx) / (DOCK_STEP * 2.4f);
			grow = maxf(grow, maxf(0, 1 - dx * dx) * 0.28f);
			ui->redraw = true;
		}
		if (it.held)
			grow -= 0.05f;
		f32 r = DOCK_BUBBLE / 2.0f * (1 + grow), by = cy - 7 * hover;
		dock_bubble(ui, cx, by, r, hover);
		const AppInfo *a = shell_app(ids[i]);
		f32 is = DOCK_ICON * (1 + grow);
		ui_app_icon(ui, rect(cx - is / 2, by - is / 2, is, is), a ? a->icon : ids[i]);
		/* running: a golden ring, a pip for each window (up to three); the
		 * app in front glows and wears a raindrop */
		Window *first;
		int count = app_window_count(ids[i], &first);
		bool focused = first && S.focus && str_eq(S.focus->app_id, ids[i]);
		if (count > 0) {
			Path ring = { 0 };
			path_circle(&ring, cx, by, r + 3.5f);
			if (focused)
				ui_path_stroke(ui, &ring, 7, color_hex("#f5b83a40"));
			ui_path_stroke(ui, &ring, focused ? 2.6f : 2, color_scale_alpha(GOLD, focused ? 1 : 0.75f));
			path_free(&ring);
			for (int w = 0; w < MIN(count, 3); w++) {
				f32 ang = PI_F / 2 + ((f32)w - (f32)(MIN(count, 3) - 1) / 2) * 0.32f;
				ui_circle(ui, cx + cosf(ang) * (r + 3.5f), by + sinf(ang) * (r + 3.5f), 2.6f, color_hex("#fff4d6"));
			}
			if (focused)
				dock_drop(ui, cx, by - r - 17, 10, GOLD);
		}
		/* starting: a golden arc runs round the bubble until the window appears */
		if (str_eq(bouncing.id, ids[i])) {
			f64 bt = S.now - bouncing.started;
			Window *fw;
			if (bt > 4.0 || app_window_count(ids[i], &fw) > 0) {
				bouncing.id[0] = 0;
			} else {
				f32 a0 = (f32)bt * 7;
				Path arc = { 0 };
				path_arc(&arc, cx, by, r + 3.5f, r + 3.5f, a0, a0 + PI_F * 0.7f);
				ui_path_stroke(ui, &arc, 3, GOLD);
				path_free(&arc);
				ui->redraw = true;
			}
		}
		if (it.hovered && !S.dock_menu[0]) {
			hover_name = a ? a->name : ids[i];
			hover_rect = rect(cx - r, by - r, 2 * r, 2 * r);
		}
		if (str_eq(S.dock_menu, ids[i]))
			menu_anchor = slot;
		if (it.clicked && !S.dock_menu[0]) {
			if (count == 0) {
				str_copy(bouncing.id, ids[i], sizeof bouncing.id);
				bouncing.started = S.now;
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
		if (it.right_clicked && !S.dock_menu[0]) {
			str_copy(S.dock_menu, ids[i], sizeof S.dock_menu);
			menu_anchor = slot;
			dock_menu_opened_frame = ui->frame_no;
		}
		ui_pop_id(ui);
	}
	if (S.dock_menu[0]) {
		if (menu_anchor.w == 0)
			S.dock_menu[0] = 0;
		else
			dock_menu_draw(ui, menu_anchor);
	}
	if (hover_name) {
		/* the name in a capsule above the bubble */
		Font f = ui_font(ui, 13, 600);
		f32 tw = ui_text_width(ui, f, hover_name) + 26;
		Rect tip = rect(roundf(hover_rect.x + hover_rect.w / 2 - tw / 2), hover_rect.y - 54, tw, 28);
		ui_shadow(ui, tip, 14, 10, 3, color_hex("#00000045"));
		ui_round(ui, tip, 14, color_hex("#1b1c21ee"));
		ui_border(ui, tip, 14, 1, color_hex("#f5b83a55"));
		ui_text_in(ui, f, tip, hover_name, COLOR_WHITE, ALIGN_CENTER);
	}
}

/* ---- Launcher ---------------------------------------------------------------------- */

static StrBuf launcher_query;
static int launcher_first_match = -1;

static bool app_matches(const AppInfo *a, const char *q)
{
	if (!*q)
		return true;
	return str_find_nocase(a->name, q) || str_find_nocase(a->keywords, q) || str_find_nocase(a->category, q) ||
	       str_find_nocase(a->id, q);
}

/* The highlighted app, moved with arrow keys or a controller's D-pad. */
static int launcher_sel = -1, launcher_sel_app = -1, launcher_shown;
static bool launcher_follow;
#define LAUNCHER_COLS 5

void launcher_nav(int dx, int dy)
{
	if (launcher_sel < 0)
		launcher_sel = 0;
	else
		launcher_sel = CLAMP(launcher_sel + dx + dy * LAUNCHER_COLS, 0, MAX(0, launcher_shown - 1));
	launcher_follow = true;
}

void launcher_activate(void)
{
	int app = launcher_sel >= 0 ? launcher_sel_app : launcher_first_match;
	if (app < 0 || app >= S.napps)
		return;
	shell_launch(S.apps[app].id, NULL);
	S.launcher_open = false;
}

bool launcher_key(Ui *ui, u32 code, u32 mods, const char *text)
{
	UNUSED(ui), UNUSED(mods), UNUSED(text);
	if (code == KEY_ESC) {
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

void launcher_draw(Ui *ui)
{
	S.launcher_k = ui_anim(ui, ui_id(ui, "launcher-k"), S.launcher_open ? 1.0f : 0.0f, 22);
	f32 k = S.launcher_k;
	if (k <= 0.01f) {
		sb_clear(&launcher_query);
		launcher_sel = -1;
		return;
	}
	Theme *t = &ui->theme;
	f32 cols = LAUNCHER_COLS, cell_w = 116, cell_h = 104;
	f32 w = cols * cell_w + 32, h = minf(560, S.sh - PANEL_HEIGHT - 120);
	Rect r = panel_popover(ui, panel_apps_btn.x + panel_apps_btn.w / 2 + w / 2 - 40, w, h, k);
	if (S.launcher_open && click_outside(ui, r) && !rect_contains(panel_apps_btn, ui->mx, ui->my)) {
		S.launcher_open = false;
		return;
	}
	ui_push_clip(ui, r);
	Rect body = pad(r, 16);
	Rect search = cut_top(&body, 36);
	int ev = ui_entry(ui, search, "launcher-search", &launcher_query, "Search Apps", ENTRY_SEARCH | ENTRY_AUTOFOCUS);
	if (S.launcher_open && ui->focus != ui_id(ui, "launcher-search"))
		ui_focus(ui, "launcher-search");
	if (ev & ENTRY_CHANGED)
		launcher_sel = -1;
	cut_top(&body, 12);
	/* keep the highlighted app in view */
	static f32 grid_offset;
	if (launcher_follow && launcher_sel >= 0) {
		f32 top = (f32)(launcher_sel / LAUNCHER_COLS) * cell_h;
		if (top < grid_offset)
			ui_scroll_to(ui, "launcher-grid", top);
		else if (top + cell_h > grid_offset + body.h)
			ui_scroll_to(ui, "launcher-grid", top + cell_h - body.h);
	}
	launcher_follow = false;
	f32 oy0 = ui->oy;
	f32 y0 = ui_scroll_begin(ui, body, "launcher-grid");
	grid_offset = oy0 - ui->oy;
	int shown = 0;
	launcher_first_match = -1;
	const char *q = sb_str(&launcher_query);
	for (int i = 0; i < S.napps; i++) {
		AppInfo *a = &S.apps[i];
		if (!app_matches(a, q))
			continue;
		if (launcher_first_match < 0)
			launcher_first_match = i;
		f32 cx = body.x + (f32)(shown % (int)cols) * cell_w, cy = y0 + (f32)(shown / (int)cols) * cell_h;
		Rect cell = rect(cx + 2, cy, cell_w - 4, cell_h - 4);
		ui_push_id_str(ui, a->id);
		bool sel = launcher_sel >= 0 ? shown == launcher_sel : shown == 0 && *q;
		if (sel && launcher_sel >= 0)
			launcher_sel_app = i;
		UiInteract it = ui_row(ui, cell, ui_id(ui, "app"), sel);
		ui_app_icon(ui, rect(cell.x + (cell.w - 56) / 2, cell.y + 8, 56, 56), a->icon);
		ui_text_in(ui, ui_font(ui, 12, 500), rect(cell.x + 4, cell.y + 68, cell.w - 8, 22), a->name, t->text, ALIGN_CENTER);
		if (it.clicked) {
			shell_launch(a->id, NULL);
			S.launcher_open = false;
		}
		ui_pop_id(ui);
		shown++;
	}
	launcher_shown = shown;
	if (launcher_sel >= shown)
		launcher_sel = shown - 1;
	if (shown == 0)
		ui_placeholder(ui, body, "search", "No Results", "Try a different search.");
	f32 rows = ceilf((f32)shown / cols);
	ui_scroll_end(ui, rows * cell_h);
	ui_pop_clip(ui);
}

/* ---- Notifications ----------------------------------------------------------------- */

static void notice_card(Ui *ui, Rect r, Notice *n, f32 alpha, bool with_close)
{
	Theme *t = &ui->theme;
	Color bg = t->dark ? color_hex("#2e2f34") : color_hex("#fbfbfc");
	ui_shadow(ui, r, 12, 14, 5, color_scale_alpha(color_hex("#00000045"), alpha));
	ui_round(ui, r, 12, color_scale_alpha(bg, alpha));
	ui_border(ui, r, 12, 1, color_scale_alpha(t->border_strong, alpha));
	Rect c = pad(r, 14);
	Rect icon = cut_left(&c, 40);
	const AppInfo *a = shell_app(n->app);
	if (a)
		ui_app_icon(ui, rect(icon.x, icon.y, 40, 40), a->icon);
	else
		ui_icon(ui, rect(icon.x + 8, icon.y + 8, 24, 24), n->icon[0] ? n->icon : "bell", color_scale_alpha(t->text_dim, alpha));
	cut_left(&c, 12);
	ui_text_in(ui, ui_font(ui, 13, 700), rect(c.x, c.y, c.w - 20, 18), n->title, color_scale_alpha(t->text, alpha), ALIGN_START);
	if (n->body[0])
		ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(c.x, c.y + 22, c.w - 4, 34), n->body, color_scale_alpha(t->text_dim, alpha), ALIGN_START);
	if (with_close)
		ui_icon(ui, rect(r.x + r.w - 26, r.y + 10, 14, 14), "close", color_scale_alpha(t->text_faint, alpha));
}

void notifications_draw(Ui *ui)
{
	Theme *t = &ui->theme;
	/* Toasts */
	if (!S.notifications_open) {
		f32 y = PANEL_HEIGHT + 12;
		for (int i = S.nnotices - 1; i >= 0 && y < S.sh / 2; i--) {
			Notice *n = &S.notices[i];
			if (n->dismissed)
				continue;
			ui_push_id(ui, (i64)n->id);
			n->slide = ui_anim(ui, ui_id(ui, "slide"), 1, 14);
			Rect r = rect(S.sw - 360 + (1 - n->slide) * 380, y, 348, 78);
			shell_surface(r);
			UiInteract it = ui_interact(ui, ui_id(ui, "toast"), r);
			notice_card(ui, r, n, 1, it.hovered);
			if (it.clicked) {
				n->dismissed = true;
				n->read = true;
				if (shell_app(n->app))
					shell_launch(n->app, NULL);
			}
			ui_pop_id(ui);
			y += 88;
		}
		return;
	}
	/* Notification center with a calendar. */
	f32 k = ui_fade(ui, ui_id(ui, "nc"), true, 0.12f);
	f32 w = 380, h = minf(S.sh - PANEL_HEIGHT - 80, 600);
	Rect r = panel_popover(ui, S.sw / 2, w, h, k);
	if (click_outside(ui, r) && !rect_contains(panel_clock_btn, ui->mx, ui->my)) {
		S.notifications_open = false;
		return;
	}
	Rect body = pad(r, 18);
	/* Month calendar */
	Civil c;
	civil_local(time_unix(), &c);
	char title[48];
	fmt(title, sizeof title, "%s %d", MONTH_NAMES[c.month - 1], c.year);
	ui_text_in(ui, ui_font(ui, 15, 700), cut_top(&body, 26), title, t->text, ALIGN_START);
	cut_top(&body, 6);
	Rect grid = cut_top(&body, 7 * 30);
	static const char *wd[] = { "M", "T", "W", "T", "F", "S", "S" };
	f32 cw = grid.w / 7;
	for (int i = 0; i < 7; i++)
		ui_text_in(ui, ui_font(ui, 11, 700), rect(grid.x + (f32)i * cw, grid.y, cw, 24), wd[i], t->text_faint, ALIGN_CENTER);
	Civil first = c;
	first.day = 1;
	Civil fc;
	civil_from_unix(unix_from_civil(&first), &fc);
	int offset = (fc.wday + 6) % 7, dim = days_in_month(c.year, c.month);
	for (int d = 1; d <= dim; d++) {
		int idx = offset + d - 1;
		Rect cell = rect(grid.x + (f32)(idx % 7) * cw, grid.y + 28 + (f32)(idx / 7) * 30, cw, 28);
		char num[4];
		fmt(num, sizeof num, "%d", d);
		if (d == c.day) {
			ui_circle(ui, cell.x + cw / 2, cell.y + 14, 13, t->accent);
			ui_text_in(ui, ui_font(ui, 13, 700), cell, num, t->on_accent, ALIGN_CENTER);
		} else {
			ui_text_in(ui, ui_font(ui, 13, 400), cell, num, t->text, ALIGN_CENTER);
		}
	}
	cut_top(&body, 8);
	ui_separator(ui, body.x, body.y, body.x + body.w);
	cut_top(&body, 12);
	Rect head = cut_top(&body, 28);
	ui_text_in(ui, ui_font(ui, 15, 700), head, "Notifications", t->text, ALIGN_START);
	int visible = 0;
	for (int i = 0; i < S.nnotices; i++)
		visible += 1;
	if (visible && ui_button_flat(ui, cut_right(&head, 90), "Clear All"))
		S.nnotices = 0;
	cut_top(&body, 8);
	if (S.nnotices == 0) {
		ui_placeholder(ui, body, "bell", "No Notifications", NULL);
		return;
	}
	f32 y0 = ui_scroll_begin(ui, body, "nc-list");
	f32 y = y0;
	for (int i = S.nnotices - 1; i >= 0; i--) {
		Notice *n = &S.notices[i];
		Rect card = rect(body.x, y, body.w, 78);
		ui_push_id(ui, (i64)n->id);
		notice_card(ui, card, n, k, false);
		if (ui_icon_button(ui, rect(card.x + card.w - 34, card.y + 6, 28, 28), "close", NULL)) {
			memmove(&S.notices[i], &S.notices[i + 1], sizeof(Notice) * (usize)(S.nnotices - i - 1));
			S.nnotices--;
		}
		ui_pop_id(ui);
		y += 88;
	}
	ui_scroll_end(ui, y - y0);
}

/* ---- Control center ------------------------------------------------------------------ */

static bool quick_toggle(Ui *ui, Rect r, const char *icon, const char *label, const char *sub, bool on)
{
	Theme *t = &ui->theme;
	UiId id = ui_id(ui, label);
	UiInteract it = ui_interact(ui, id, r);
	f32 k = ui_anim(ui, id, on ? 1.0f : 0.0f, 18);
	Color off = t->dark ? color_hex("#3c3d43") : color_hex("#ececef");
	ui_round(ui, r, 12, color_mix(off, t->accent, k));
	if (it.hovered)
		ui_round(ui, r, 12, t->hover);
	Color fg = color_mix(t->text, t->on_accent, k);
	ui_icon(ui, rect(r.x + 14, r.y + (r.h - 20) / 2, 20, 20), icon, fg);
	ui_text_in(ui, ui_font(ui, 13, 600), rect(r.x + 44, r.y + 9, r.w - 50, 18), label, fg, ALIGN_START);
	if (sub)
		ui_text_in(ui, ui_font(ui, 11, 400), rect(r.x + 44, r.y + 27, r.w - 50, 16), sub, color_scale_alpha(fg, 0.75f), ALIGN_START);
	return it.clicked;
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

void controlcenter_draw(Ui *ui)
{
	if (!S.session_menu_open) {
		cc_wifi = false;
		return;
	}
	Theme *t = &ui->theme;
	f32 k = ui_fade(ui, ui_id(ui, "cc"), true, 0.12f);
	f32 w = 340, h = 400;
	Rect r = panel_popover(ui, panel_status_btn.x + panel_status_btn.w / 2, w, h, k);
	if (click_outside(ui, r) && !rect_contains(panel_status_btn, ui->mx, ui->my)) {
		S.session_menu_open = false;
		return;
	}
	Rect body = pad(r, 16);
	/* User */
	Rect user = cut_top(&body, 44);
	ui_avatar(ui, cut_left(&user, 44), S.set.full_name);
	cut_left(&user, 12);
	ui_text_in(ui, ui_font(ui, 14, 700), rect(user.x, user.y + 4, user.w - 40, 20), S.set.full_name, t->text, ALIGN_START);
	char batt[48];
	if (S.battery >= 0)
		fmt(batt, sizeof batt, "Battery %d%%%s", S.battery, S.charging ? ", charging" : "");
	else
		str_copy(batt, OS_PRETTY_NAME, sizeof batt);
	ui_text_in(ui, ui_font(ui, 12, 400), rect(user.x, user.y + 24, user.w - 40, 16), batt, t->text_dim, ALIGN_START);
	if (ui_icon_button(ui, rect(r.x + r.w - 52, user.y + 6, 32, 32), "settings", "Settings")) {
		shell_launch("settings", NULL);
		S.session_menu_open = false;
	}
	cut_top(&body, 14);
	if (cc_wifi) {
		cc_wifi_view(ui, body);
		return;
	}
	/* Toggles */
	Rect row = cut_top(&body, 52);
	Rect a = cut_left(&row, (row.w - 10) / 2);
	cut_left(&row, 10);
	/* with a Wi-Fi adapter the tile is about Wi-Fi (a cable shows in the top bar anyway) */
	bool wifi_tile = S.wifi_available;
	bool wifi_on = str_eq(S.wifi_state, "connected");
	char ssid[64] = "";
	if (wifi_tile && wifi_on) {
		Conf *net = conf_load("/run/zurbax/network.status");
		str_copy(ssid, conf_get(net, "wifi_ssid", "Connected"), sizeof ssid);
		conf_free(net);
	}
	const char *net_sub = wifi_tile ? (!S.wifi_radio ? "Off" : wifi_on ? ssid : str_eq(S.wifi_state, "connecting") ? "Connecting…" : "Not connected")
					: S.wifi_connected ? S.wifi_name : "Not connected";
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
	cut_top(&body, 18);
	/* Sliders */
	Rect vol = cut_top(&body, 30);
	ui_icon(ui, cut_left(&vol, 20), "audio-volume-high", t->text_dim);
	cut_left(&vol, 10);
	f32 v = (f32)S.volume / 100;
	if (ui_slider(ui, vol, &v, 0, 1))
		set_volume((int)roundf(v * 100));
	cut_top(&body, 8);
	if (S.brightness >= 0) {
		Rect br = cut_top(&body, 30);
		ui_icon(ui, cut_left(&br, 20), "display-brightness", t->text_dim);
		cut_left(&br, 10);
		f32 b = (f32)S.brightness / 100;
		if (ui_slider(ui, br, &b, 0.05f, 1))
			set_brightness((int)roundf(b * 100));
	}
	/* Session */
	Rect bottom = cut_bottom(&body, 40);
	ui_separator(ui, r.x + 16, bottom.y - 12, r.x + r.w - 16);
	static const char *icons[] = { "lock", "system-log-out", "system-restart", "system-shutdown" };
	static const char *tips[] = { "Lock", "Log Out", "Restart", "Shut Down" };
	static const u32 actions[] = { SESSION_LOCK, SESSION_LOGOUT, SESSION_RESTART, SESSION_SHUTDOWN };
	f32 bw = 52, gap = (bottom.w - 4 * bw) / 3;
	for (int i = 0; i < 4; i++) {
		Rect b = rect(bottom.x + (f32)i * (bw + gap), bottom.y, bw, 40);
		if (ui_icon_button(ui, b, icons[i], tips[i])) {
			S.session_menu_open = false;
			shell_session_action(actions[i]);
		}
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
	f32 cell = 112, w = (f32)n * cell + 24, h = 150;
	Rect r = center_in(rect(0, 0, S.sw, S.sh), w, h);
	shell_surface(r);
	ui_shadow(ui, r, 18, 24, 8, color_hex("#00000070"));
	ui_round(ui, r, 18, color_hex("#24252be8"));
	for (int i = 0; i < n; i++) {
		Rect c = rect(r.x + 12 + (f32)i * cell, r.y + 12, cell, h - 24);
		if (i == sel)
			ui_round(ui, pad(c, 2), 12, color_hex("#ffffff24"));
		const AppInfo *a = shell_app(list[i]->app_id);
		ui_app_icon(ui, rect(c.x + (c.w - 64) / 2, c.y + 10, 64, 64), a ? a->icon : list[i]->app_id);
		ui_text_in(ui, ui_font(ui, 12, 600), rect(c.x + 6, c.y + 84, c.w - 12, 20), list[i]->title[0] ? list[i]->title : (a ? a->name : list[i]->app_id),
			   COLOR_WHITE, ALIGN_CENTER);
	}
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
	Rect r = rect((S.sw - 260) / 2, S.sh - 140, 260, 48);
	shell_surface(r);
	ui_shadow(ui, r, 24, 16, 5, color_scale_alpha(color_hex("#00000050"), k));
	ui_round(ui, r, 24, color_scale_alpha(color_hex("#25262bf0"), k));
	ui_icon(ui, rect(r.x + 16, r.y + 14, 20, 20), S.osd_icon, color_scale_alpha(COLOR_WHITE, k));
	Rect bar = rect(r.x + 50, r.y + 21, r.w - 70, 6);
	ui_round(ui, bar, 3, color_scale_alpha(color_hex("#ffffff30"), k));
	ui_round(ui, rect(bar.x, bar.y, bar.w * clampf(S.osd_value, 0, 1), bar.h), 3, color_scale_alpha(COLOR_WHITE, k));
}
