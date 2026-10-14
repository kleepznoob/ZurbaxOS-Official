/*
 * Settings — everything about how Zurbax looks and behaves.
 *
 * Preferences live in ~/.config/zurbax/settings.conf. After each change
 * Settings tells the desktop, which applies it immediately: style, accent
 * and wallpaper change on screen as you pick them.
 */
#include "zui.h"

enum {
	PAGE_APPEARANCE, PAGE_DESKTOP, PAGE_NETWORK, PAGE_BLUETOOTH, PAGE_DISPLAY, PAGE_SOUND, PAGE_INPUT,
	PAGE_KEYBOARD, PAGE_USERS, PAGE_DATETIME, PAGE_GAMING, PAGE_SCREENTIME, PAGE_SECURITY, PAGE_ABOUT, PAGE_COUNT
};

static const struct {
	const char *title, *icon, *keywords, *group;
} PAGES[PAGE_COUNT] = {
	{ "Appearance", "palette", "dark light style accent colour color wallpaper background text size motion", "Personal" },
	{ "Desktop", "computer", "taskbar top bar panel start search previews clock seconds battery lock autohide", "Personal" },
	{ "Network", "network-wireless", "wifi wi-fi internet ethernet wireless", "Network" },
	{ "Bluetooth", "bluetooth", "bluetooth headphones devices", "Network" },
	{ "Display", "display", "scale resolution night light brightness hidpi", "Hardware" },
	{ "Sound", "audio-volume-high", "volume speaker audio mute alert", "Hardware" },
	{ "Mouse & Touchpad", "touchpad", "pointer speed natural scrolling tap click trackpad", "Hardware" },
	{ "Keyboard", "keyboard", "layout language shortcuts keys romanian german french spanish", "Hardware" },
	{ "Users", "user", "account name password avatar login", "System" },
	{ "Date & Time", "clock", "time zone 24 hour clock date", "System" },
	{ "Gaming", "gamepad", "game mode performance fps windows exe", "System" },
	{ "Screen Time", "hourglass", "screen time limits usage parental", "System" },
	{ "Privacy & Security", "shield", "privacy security updates lts support protection clipboard lockdown firewall", "System" },
	{ "About", "info", "about version hardware system update computer", "System" },
};

static const char *const LAYOUT_IDS[] = { "us", "gb", "ro", "de", "fr", "es" };
static const char *const LAYOUT_TITLES[] = { "English (US)", "English (UK)", "Romanian", "German", "French", "Spanish" };

typedef struct {
	int page;
	StrBuf search;
	Conf *conf;
	char conf_path[512];
	/* appearance */
	Canvas *thumbs[16];
	Canvas *custom_thumb;
	char custom_path[512];
	/* users */
	StrBuf full_name, pw_old, pw_new, pw_confirm;
	char user_name[64];
	bool has_password;
	u32 pending_seq;
	char user_msg[160];
	bool user_msg_error;
	/* date & time */
	StrBuf tz_search;
	/* about */
	char cpu[96], gpu[96], board[128], kernel[96], drivers[96];
	u64 mem_kb, disk_total, disk_free;
} Settings;

static Settings st;

/* ---- Settings file ------------------------------------------------------------------- */

static const char *get(const char *key, const char *fallback)
{
	return conf_get(st.conf, key, fallback);
}

static bool get_bool(const char *key, bool fallback)
{
	return conf_get_bool(st.conf, key, fallback);
}

static f64 get_float(const char *key, f64 fallback)
{
	return conf_get_float(st.conf, key, fallback);
}

static void commit(Ui *ui)
{
	conf_save(st.conf);
	ui_settings_changed(ui);
}

static void set_str(Ui *ui, const char *key, const char *v)
{
	conf_set(st.conf, key, v);
	commit(ui);
}

static void set_bool(Ui *ui, const char *key, bool v)
{
	conf_set_bool(st.conf, key, v);
	commit(ui);
}

static void set_float(Ui *ui, const char *key, f64 v)
{
	char b[32];
	fmt(b, sizeof b, "%.3f", v);
	conf_set(st.conf, key, b);
	commit(ui);
}

/* ---- Layout helpers ------------------------------------------------------------------- */

static Rect page_body(Rect r)
{
	Rect b = pad4(r, 26, 32, 26, 32);
	f32 w = minf(b.w, 720);
	b.x = r.x + (r.w - w) / 2;
	b.w = w;
	return b;
}

static void page_title(Ui *ui, Rect *b, const char *title, const char *subtitle)
{
	ui_title(ui, cut_top(b, 36), title);
	if (subtitle) {
		ui_label_dim(ui, cut_top(b, 22), subtitle);
	}
	cut_top(b, 16);
}

static void section(Ui *ui, Rect *b, const char *title)
{
	cut_top(b, 6);
	ui_heading(ui, cut_top(b, 28), title);
	cut_top(b, 4);
}

/* A group of rows inside a card. Returns the card's inner area of `rows` rows of height h. */
static Rect card_rows(Ui *ui, Rect *b, int rows, f32 h)
{
	Rect card = cut_top(b, (f32)rows * h + 8);
	ui_card(ui, card);
	cut_top(b, 14);
	return pad2(card, 18, 4);
}

static void row_label(Ui *ui, Rect row, const char *label, const char *sub)
{
	Theme *t = &ui->theme;
	if (sub) {
		ui_text(ui, ui_font(ui, 14, 500), row.x, row.y + row.h / 2 - 3, label, t->text);
		ui_text(ui, ui_font(ui, 12, 400), row.x, row.y + row.h / 2 + 14, sub, t->text_dim);
	} else {
		ui_text(ui, ui_font(ui, 14, 500), row.x, row.y + row.h / 2 + 5, label, t->text);
	}
}

static bool switch_row(Ui *ui, Rect *rows, f32 h, bool last, const char *label, const char *sub, const char *key, bool fallback)
{
	Rect r = cut_top(rows, h);
	if (!last)
		ui_separator(ui, r.x, r.y + r.h, r.x + r.w);
	row_label(ui, r, label, sub);
	bool v = get_bool(key, fallback);
	ui_push_id_str(ui, key);
	bool changed = ui_switch(ui, center_in(cut_right(&r, 52), 46, 26), &v);
	ui_pop_id(ui);
	if (changed)
		set_bool(ui, key, v);
	return changed;
}

/* ---- Pages --------------------------------------------------------------------------- */

static Canvas *thumb_for(int i)
{
	if (!st.thumbs[i])
		st.thumbs[i] = wallpaper_render(WALLPAPER_NAMES[i], 192, 120);
	return st.thumbs[i];
}

static void style_card(Ui *ui, Rect r, bool dark, bool selected)
{
	Theme *t = &ui->theme;
	UiInteract it = ui_interact(ui, ui_id(ui, dark ? "style-dark" : "style-light"), r);
	Rect pic = cut_top(&r, r.h - 28);
	/* A small picture of a window in that style. */
	ui_round(ui, pic, 10, dark ? color_hex("#1e1f23") : color_hex("#e9eaed"));
	Rect w = pad4(pic, 14, 18, 0, 18);
	ui_shadow(ui, w, 6, 8, 3, color_hex("#00000035"));
	ui_round4(ui, w, 6, 6, 0, 0, dark ? color_hex("#2d2e33") : color_hex("#fafafa"));
	Rect hb = cut_top(&w, 18);
	ui_round4(ui, hb, 6, 6, 0, 0, dark ? color_hex("#38393e") : color_hex("#ececee"));
	ui_circle(ui, hb.x + 10, hb.y + 9, 3, dark ? color_hex("#8a8b90") : color_hex("#9a9ba0"));
	ui_round(ui, rect(w.x + 10, w.y + 10, w.w * 0.5f, 6), 3, dark ? color_hex("#5a5b60") : color_hex("#c8c9cd"));
	ui_round(ui, rect(w.x + 10, w.y + 22, w.w * 0.7f, 6), 3, dark ? color_hex("#46474c") : color_hex("#dcdde0"));
	ui_round(ui, rect(w.x + w.w - 50, w.y + w.h - 26, 40, 16), 4, t->accent);
	if (selected)
		ui_border(ui, rect_inset(pic, -3, -3), 12, 2.5f, t->accent);
	else if (it.hovered)
		ui_border(ui, rect_inset(pic, -2, -2), 11, 1.5f, t->border_strong);
	ui_text_in(ui, ui_font(ui, 13, selected ? 700 : 500), r, dark ? "Dark" : "Light", t->text, ALIGN_CENTER);
	if (it.clicked && !selected)
		set_bool(ui, "dark", dark);
}

/* An accent colour from a wallpaper: the average of its colourful pixels,
 * kept bright and saturated enough to read on buttons. */
static void accent_from_wallpaper(Ui *ui)
{
	Canvas *c = wallpaper_render(get("wallpaper", WALLPAPER_DEFAULT), 96, 54);
	if (!c)
		return;
	f64 r = 0, g = 0, bl = 0, wsum = 0;
	for (int y = 0; y < c->height; y++)
		for (int x = 0; x < c->width; x++) {
			u32 p = canvas_row(c, y)[x];
			f32 h, sat, l;
			color_to_hsl(p | 0xff000000u, &h, &sat, &l);
			f64 w = (f64)sat * (1.0 - fabs((f64)l - 0.5) * 1.6);
			if (w <= 0)
				continue;
			r += ((p >> 16) & 0xff) * w, g += ((p >> 8) & 0xff) * w, bl += (p & 0xff) * w, wsum += w;
		}
	canvas_free(c);
	Color avg = wsum > 0 ? RGB((u8)(r / wsum), (u8)(g / wsum), (u8)(bl / wsum)) : THEME_ACCENT_COLORS[0];
	f32 h, sat, l;
	color_to_hsl(avg, &h, &sat, &l);
	Color acc = color_hsl(h, clampf(sat, 0.55f, 0.85f), clampf(l, 0.45f, 0.58f), 255);
	char hex[16];
	fmt(hex, sizeof hex, "#%02x%02x%02x", (acc >> 16) & 0xff, (acc >> 8) & 0xff, acc & 0xff);
	conf_set(st.conf, "accent_follow", "true");
	set_str(ui, "accent", hex);
}

/* A new wallpaper brings its colour along when the accent follows it. */
static void wallpaper_changed(Ui *ui)
{
	if (get_bool("accent_follow", false))
		accent_from_wallpaper(ui);
}

static void page_appearance(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	f32 y0 = ui_scroll_begin(ui, r, "page-scroll");
	b.y = y0 + 26;
	b.h = 100000; /* scrolling content: room to lay out everything */
	f32 top = b.y;
	page_title(ui, &b, "Appearance", "Make Zurbax yours: style, colour and wallpaper.");

	/* whole looks in one click: a style, an accent and a wallpaper that go together */
	section(ui, &b, "Themes");
	{
		static const struct {
			const char *name, *about, *wallpaper, *accent;
			bool dark;
		} THEMES[] = {
			{ "Zurbax", "Silk, light, iris", "silk", "iris", false },
			{ "Golden Rain", "Dusk gold, dark", "golden-rain", "gold", true },
			{ "Halloween", "Pumpkin on purple, dark", "halloween", "orange", true },
		};
		Rect row = cut_top(&b, 118);
		f32 cw = minf(200, (row.w - 2 * 14) / 3);
		for (usize i = 0; i < ARRAY_LEN(THEMES); i++) {
			Rect card = rect(row.x + (f32)i * (cw + 14), row.y, cw, row.h);
			ui_push_id(ui, 300 + (i64)i);
			UiInteract it = ui_interact(ui, ui_id(ui, "theme"), card);
			ui_pop_id(ui);
			bool on = str_eq(get("wallpaper", WALLPAPER_DEFAULT), THEMES[i].wallpaper) && get_bool("dark", false) == THEMES[i].dark &&
				  str_eq(get("accent", THEME_ACCENT_DEFAULT), THEMES[i].accent);
			Rect pic = rect(card.x, card.y, card.w, card.h - 34);
			int wi = 0;
			for (int k = 0; k < WALLPAPER_COUNT; k++)
				if (str_eq(WALLPAPER_NAMES[k], THEMES[i].wallpaper))
					wi = k;
			ui_shadow(ui, pic, 8, 8, 3, color_hex("#00000030"));
			ui_image(ui, pic, thumb_for(wi), 8, true);
			/* a little window in the theme's style and accent */
			Rect win = rect(pic.x + 10, pic.y + 12, pic.w * 0.3f, pic.h - 24);
			ui_round(ui, win, 5, THEMES[i].dark ? color_hex("#2a2b30f0") : color_hex("#fafafaf0"));
			ui_round(ui, rect(win.x + 8, win.y + win.h - 16, 34, 9), 4.5f, theme_accent_named(THEMES[i].accent));
			ui_round(ui, rect(win.x + 8, win.y + 9, win.w - 16, 5), 2.5f, THEMES[i].dark ? color_hex("#ffffff30") : color_hex("#00000020"));
			ui_round(ui, rect(win.x + 8, win.y + 18, win.w * 0.5f, 5), 2.5f, THEMES[i].dark ? color_hex("#ffffff20") : color_hex("#00000014"));
			if (on)
				ui_border(ui, rect_inset(pic, -3, -3), 10, 2.5f, t->accent);
			else if (it.hovered)
				ui_border(ui, rect_inset(pic, -2, -2), 9, 1.5f, color_with_alpha(t->text, 0x40));
			ui_text_in(ui, ui_font(ui, 13, 600), rect(card.x, card.y + card.h - 32, card.w, 16), THEMES[i].name, t->text, ALIGN_CENTER);
			ui_text_in(ui, ui_font(ui, 11, 400), rect(card.x, card.y + card.h - 15, card.w, 14), THEMES[i].about, t->text_dim, ALIGN_CENTER);
			if (it.clicked) {
				conf_set(st.conf, "wallpaper", THEMES[i].wallpaper);
				conf_set(st.conf, "accent", THEMES[i].accent);
				conf_set(st.conf, "accent_follow", "false");
				set_bool(ui, "dark", THEMES[i].dark);
			}
		}
	}
	cut_top(&b, 12);

	section(ui, &b, "Style");
	Rect styles = cut_top(&b, 150);
	bool dark = get_bool("dark", false);
	style_card(ui, rect(styles.x, styles.y, 210, styles.h), false, !dark);
	style_card(ui, rect(styles.x + 230, styles.y, 210, styles.h), true, dark);
	cut_top(&b, 12);

	section(ui, &b, "Accent Colour");
	Rect acc = cut_top(&b, 44);
	const char *cur = get("accent", THEME_ACCENT_DEFAULT);
	for (int i = 0; i < THEME_ACCENT_COUNT; i++) {
		Rect sw = rect(acc.x + (f32)i * 52, acc.y, 44, 44);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "accent"), sw);
		ui_pop_id(ui);
		f32 cx = sw.x + 22, cy = sw.y + 22;
		bool sel = str_eq(cur, THEME_ACCENT_NAMES[i]);
		Color c = THEME_ACCENT_COLORS[i];
		ui_circle(ui, cx, cy, it.hovered ? 16 : 15, c);
		if (sel) {
			ui_ring(ui, cx, cy, 20, 2, c);
			ui_icon(ui, rect(cx - 8, cy - 8, 16, 16), "check", COLOR_WHITE);
		}
		if (it.clicked) {
			conf_set(st.conf, "accent_follow", "false");
			set_str(ui, "accent", THEME_ACCENT_NAMES[i]);
		}
		ui_push_id(ui, i);
		if (it.hovered) {
			ui->last_rect = sw;
			ui_tooltip(ui, THEME_ACCENT_NAMES[i]);
		}
		ui_pop_id(ui);
	}
	{
		/* the accent taken from the wallpaper, and kept in step with it */
		Rect sw = rect(acc.x + (f32)THEME_ACCENT_COUNT * 52, acc.y, 44, 44);
		UiInteract it = ui_interact(ui, ui_id(ui, "accent-wall"), sw);
		f32 cx = sw.x + 22, cy = sw.y + 22;
		bool sel = get_bool("accent_follow", false);
		Color c = theme_accent_named(cur);
		ui_circle(ui, cx, cy, it.hovered ? 16 : 15, sel ? c : t->card);
		ui_ring(ui, cx, cy, 15, 2, sel ? c : t->border_strong);
		ui_icon(ui, rect(cx - 8, cy - 8, 16, 16), sel ? "check" : "image", sel ? COLOR_WHITE : t->text_dim);
		if (sel)
			ui_ring(ui, cx, cy, 20, 2, c);
		if (it.clicked)
			accent_from_wallpaper(ui);
		if (it.hovered) {
			ui->last_rect = sw;
			ui_tooltip(ui, "From your wallpaper");
		}
	}
	cut_top(&b, 14);

	section(ui, &b, "Wallpaper");
	const char *wall = get("wallpaper", WALLPAPER_DEFAULT);
	f32 tw = 160, th = 100, gap = 14;
	int cols = MAX(1, (int)((b.w + gap) / (tw + gap)));
	int n = WALLPAPER_COUNT + 1;
	int rows = (n + cols - 1) / cols;
	Rect grid = cut_top(&b, (f32)rows * (th + 30 + gap));
	for (int i = 0; i < n; i++) {
		int cx = i % cols, cy = i / cols;
		Rect cell = rect(grid.x + (f32)cx * (tw + gap), grid.y + (f32)cy * (th + 30 + gap), tw, th);
		ui_push_id(ui, 100 + i);
		UiInteract it = ui_interact(ui, ui_id(ui, "wall"), cell);
		ui_pop_id(ui);
		bool custom = i == WALLPAPER_COUNT;
		bool sel = custom ? wall[0] == '/' : str_eq(wall, WALLPAPER_NAMES[i]);
		ui_shadow(ui, cell, 8, 8, 3, color_hex("#00000030"));
		if (custom) {
			if (st.custom_thumb)
				ui_image(ui, cell, st.custom_thumb, 8, true);
			else {
				ui_round(ui, cell, 8, t->card);
				ui_border(ui, cell, 8, 1, t->border_strong);
				ui_icon(ui, center_in(cell, 28, 28), "image", t->text_dim);
			}
		} else {
			ui_image(ui, cell, thumb_for(i), 8, true);
		}
		if (sel)
			ui_border(ui, rect_inset(cell, -3, -3), 10, 2.5f, t->accent);
		else if (it.hovered)
			ui_border(ui, rect_inset(cell, -2, -2), 9, 1.5f, color_with_alpha(t->text, 0x40));
		ui_text_in(ui, ui_font(ui, 12, sel ? 700 : 500), rect(cell.x, cell.y + th + 4, tw, 22),
			   custom ? (st.custom_thumb ? "Your Picture" : "Choose Picture…") : WALLPAPER_TITLES[i], t->text_dim, ALIGN_CENTER);
		if (it.clicked) {
			if (custom) {
				char *pics = str_fmt("%s/Pictures", home_dir());
				ui_file_dialog_open(ui, "wallpaper", FILE_OPEN, pics, "png,jpg,jpeg,bmp", NULL);
				mem_free(pics);
			} else {
				set_str(ui, "wallpaper", WALLPAPER_NAMES[i]);
				wallpaper_changed(ui);
			}
		}
	}
	cut_top(&b, 10);

	section(ui, &b, "Accessibility");
	Rect rows2 = card_rows(ui, &b, 2, 60);
	Rect tr = cut_top(&rows2, 60);
	ui_separator(ui, tr.x, tr.y + tr.h, tr.x + tr.w);
	row_label(ui, tr, "Text size", "Makes text larger everywhere");
	f32 ts = (f32)get_float("text_scale", 1.0);
	Rect sl = center_in(cut_right(&tr, 240), 230, 24);
	if (ui_slider(ui, sl, &ts, 0.85f, 1.4f))
		set_float(ui, "text_scale", roundf(ts * 20) / 20);
	switch_row(ui, &rows2, 60, true, "Reduce motion", "Fewer and shorter animations", "reduce_motion", false);

	ui_scroll_end(ui, b.y - top + 60);
	char path[1024];
	if (ui_file_dialog(ui, "wallpaper", path, sizeof path)) {
		set_str(ui, "wallpaper", path);
		canvas_free(st.custom_thumb);
		st.custom_thumb = image_load_thumb(path, 320);
		wallpaper_changed(ui);
	}
}

static void page_desktop(Ui *ui, Rect r)
{
	Rect b = page_body(r);
	f32 y0 = ui_scroll_begin(ui, r, "page-scroll");
	b.y = y0 + 26;
	b.h = 100000; /* scrolling content: room to lay out everything */
	f32 top = b.y;
	page_title(ui, &b, "Desktop", "The taskbar, the top bar and locking.");
	section(ui, &b, "Taskbar");
	Rect rows = card_rows(ui, &b, 9, 60);
	{
		Rect row = cut_top(&rows, 60);
		ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
		row_label(ui, row, "Size", NULL);
		static const char *const sizes[] = { "Small", "Medium", "Large" };
		int size = (int)CLAMP(conf_get_int(st.conf, "taskbar_size", 1), 0, 2);
		if (ui_segmented(ui, center_in(cut_right(&row, 290), 280, 32), sizes, 3, &size)) {
			conf_set_int(st.conf, "taskbar_size", size);
			commit(ui);
		}
	}
	{
		Rect row = cut_top(&rows, 60);
		ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
		row_label(ui, row, "Icon spacing", "How far apart the app icons sit");
		static const char *const spacings[] = { "Close", "Medium", "Wide" };
		int spacing = (int)CLAMP(conf_get_int(st.conf, "taskbar_spacing", 1), 0, 2);
		if (ui_segmented(ui, center_in(cut_right(&row, 290), 280, 32), spacings, 3, &spacing)) {
			conf_set_int(st.conf, "taskbar_spacing", spacing);
			commit(ui);
		}
	}
	{
		Rect row = cut_top(&rows, 60);
		ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
		row_label(ui, row, "Buttons", "Start and the apps from the left, or in the middle");
		static const char *const aligns[] = { "Left", "Centre" };
		int align = str_eq(get("taskbar_align", "left"), "center");
		if (ui_segmented(ui, center_in(cut_right(&row, 200), 190, 32), aligns, 2, &align))
			set_str(ui, "taskbar_align", align ? "center" : "left");
	}
	switch_row(ui, &rows, 60, false, "App names", "Show each app's name beside its icon", "taskbar_labels", false);
	switch_row(ui, &rows, 60, false, "Search box", "Type here to search, next to Start", "taskbar_search", true);
	switch_row(ui, &rows, 60, false, "Window previews", "Pictures of an app's windows when the pointer rests on it", "taskbar_previews", true);
	switch_row(ui, &rows, 60, false, "Show desktop button", "A thin button at the far right", "taskbar_desktop", true);
	switch_row(ui, &rows, 60, false, "Hide automatically", "Show the taskbar when the pointer reaches the bottom edge", "dock_autohide", false);
	{
		Rect row = cut_top(&rows, 60);
		row_label(ui, row, "Colour", "Dark, or light in the light style");
		static const char *const colors[] = { "Dark", "Match style" };
		int c = str_eq(get("taskbar_color", "dark"), "style");
		if (ui_segmented(ui, center_in(cut_right(&row, 250), 240, 32), colors, 2, &c))
			set_str(ui, "taskbar_color", c ? "style" : "dark");
	}
	section(ui, &b, "Top Bar");
	rows = card_rows(ui, &b, 4, 56);
	switch_row(ui, &rows, 56, false, "Show the top bar", "Off: the clock and status icons move into the taskbar", "topbar", true);
	switch_row(ui, &rows, 56, false, "24-hour clock", NULL, "clock_24h", true);
	switch_row(ui, &rows, 56, false, "Show seconds", NULL, "clock_seconds", false);
	switch_row(ui, &rows, 56, true, "Battery percentage", NULL, "battery_percent", true);
	section(ui, &b, "Privacy");
	rows = card_rows(ui, &b, 1, 60);
	Rect lr = cut_top(&rows, 60);
	row_label(ui, lr, "Lock the screen", "After this long without activity (needs a password)");
	static const char *const opts[] = { "Never", "After 5 minutes", "After 10 minutes", "After 15 minutes", "After 30 minutes" };
	static const int mins[] = { 0, 5, 10, 15, 30 };
	int cur = (int)conf_get_int(st.conf, "lock_after_minutes", 10), sel = 2;
	for (int i = 0; i < 5; i++)
		if (mins[i] == cur)
			sel = i;
	if (ui_dropdown(ui, center_in(cut_right(&lr, 190), 180, 32), "lock-after", opts, 5, &sel)) {
		conf_set_int(st.conf, "lock_after_minutes", mins[sel]);
		commit(ui);
	}
	ui_scroll_end(ui, b.y - top + 40);
}

static void page_display(Ui *ui, Rect r)
{
	Rect b = page_body(r);
	page_title(ui, &b, "Display", "Size of everything on screen, and warmer colours at night.");
	section(ui, &b, "Scale");
	Rect rows = card_rows(ui, &b, 1, 64);
	Rect sr = cut_top(&rows, 64);
	row_label(ui, sr, "Interface size", "Takes effect the next time you log in");
	static const char *const scales[] = { "Automatic", "100%", "125%", "150%", "175%", "200%" };
	static const f32 values[] = { 0, 1, 1.25f, 1.5f, 1.75f, 2 };
	f32 cur = (f32)get_float("display_scale", 0);
	int sel = 0;
	for (int i = 0; i < 6; i++)
		if (fabsf(values[i] - cur) < 0.01f)
			sel = i;
	if (ui_dropdown(ui, center_in(cut_right(&sr, 170), 160, 32), "scale", scales, 6, &sel))
		set_float(ui, "display_scale", values[sel]);
	section(ui, &b, "Night Light");
	rows = card_rows(ui, &b, 2, 60);
	switch_row(ui, &rows, 60, false, "Night Light", "Reduces blue light to help you sleep", "night_light", false);
	Rect nr = cut_top(&rows, 60);
	row_label(ui, nr, "Warmth", NULL);
	f32 k = (f32)get_float("night_light_strength", 0.5);
	if (ui_slider(ui, center_in(cut_right(&nr, 260), 250, 24), &k, 0.1f, 1))
		set_float(ui, "night_light_strength", k);
}

static void page_sound(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	page_title(ui, &b, "Sound", NULL);
	section(ui, &b, "Output");
	Rect rows = card_rows(ui, &b, 2, 60);
	Rect vr = cut_top(&rows, 60);
	ui_separator(ui, vr.x, vr.y + vr.h, vr.x + vr.w);
	row_label(ui, vr, "Volume", NULL);
	f32 v = (f32)conf_get_int(st.conf, "volume", 60) / 100.0f;
	Rect sl = center_in(cut_right(&vr, 300), 290, 24);
	ui_icon(ui, rect(sl.x - 26, sl.y + 3, 18, 18), "audio-volume-low", t->text_dim);
	if (ui_slider(ui, sl, &v, 0, 1)) {
		conf_set_int(st.conf, "volume", (i64)roundf(v * 100));
		commit(ui);
	}
	/* Sound cards the kernel found. */
	Rect dr = cut_top(&rows, 60);
	char *cards = file_read("/proc/asound/cards", NULL);
	char dev[96] = "No sound card found";
	if (cards) {
		const char *p = str_find(cards, " - ");
		if (p) {
			p += 3;
			const char *e = str_chr(p, '\n');
			fmt(dev, sizeof dev, "%.*s", (int)(e ? e - p : (long)strlen(p)), p);
		}
		mem_free(cards);
	}
	row_label(ui, dr, "Output device", dev);
	section(ui, &b, "Alerts");
	rows = card_rows(ui, &b, 1, 60);
	switch_row(ui, &rows, 60, true, "Alert sounds", "Play a sound for notifications", "alert_sounds", true);
}

/* ---- Network ------------------------------------------------------------------------------- */

static struct {
	f64 last_scan;
	WifiNet pick;            /* the network the password dialog is for */
	StrBuf pass;
	bool show_pass;
	WifiNet menu;            /* the network whose menu is open */
	bool ask;                /* open the password prompt on the next frame */
} wf;

static void send_scan(Ui *ui)
{
	if (ui->now - wf.last_scan > 12) {
		wf.last_scan = ui->now;
		wifi_command("scan");
	}
}

static void join(Ui *ui, const WifiNet *n)
{
	if (n->saved || str_eq(n->security, "open")) {
		wifi_connect(n->hex, NULL);
		return;
	}
	wf.pick = *n;
	sb_clear(&wf.pass);
	wf.show_pass = false;
	ui_dialog_open(ui, "wifi-pass");
}

static void password_dialog(Ui *ui)
{
	Theme *t = &ui->theme;
	Rect c;
	if (!ui_dialog_begin(ui, "wifi-pass", 420, 214, &c))
		return;
	char title[160];
	fmt(title, sizeof title, "Join “%s”", wf.pick.ssid);
	ui_text_in(ui, ui_font(ui, 17, 700), rect(c.x, c.y, c.w, 26), title, t->text, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 13, 400), rect(c.x, c.y + 30, c.w, 20), "Type the network's password.", t->text_dim, ALIGN_START);
	Rect er = rect(c.x, c.y + 62, c.w - 44, 36);
	int ev = ui_entry(ui, er, "wifi-pass-entry", &wf.pass, "Password", (wf.show_pass ? 0 : ENTRY_PASSWORD) | ENTRY_AUTOFOCUS);
	if (ui_icon_toggle(ui, rect(er.x + er.w + 8, er.y + 2, 32, 32), wf.show_pass ? "eye-off" : "eye", wf.show_pass,
			   wf.show_pass ? "Hide Password" : "Show Password"))
		wf.show_pass = !wf.show_pass;
	bool ok = wf.pass.len >= 8 && wf.pass.len <= 64;
	if (wf.pass.len && !ok)
		ui_text(ui, ui_font(ui, 12, 500), c.x, c.y + 118, "Wi-Fi passwords are 8 to 63 characters long.", t->text_dim);
	Rect b = rect(c.x, c.y + c.h - 36, c.w, 36);
	bool go = (ev & ENTRY_SUBMIT) && ok;
	if (ok ? ui_button_suggested(ui, cut_right(&b, 100), "Join") : (ui_button(ui, cut_right(&b, 100), "Join"), false))
		go = true;
	cut_right(&b, 10);
	if (ui_button(ui, cut_right(&b, 100), "Cancel")) {
		sb_clear(&wf.pass);
		ui_dialog_close(ui);
	}
	if (go) {
		wifi_connect(wf.pick.hex, sb_str(&wf.pass));
		memset(wf.pass.data, 0, wf.pass.len);
		sb_clear(&wf.pass);
		ui_dialog_close(ui);
	}
	ui_dialog_end(ui);
}

static void network_menu(Ui *ui, Rect anchor)
{
	Rect c;
	WifiNet *n = &wf.menu;
	int items = (n->connected ? 1 : 0) + (n->saved ? 1 : 0) + (!n->connected ? 1 : 0);
	if (!ui_popover_begin(ui, "wifi-menu", anchor, 200, 8 + 36 * (f32)items, &c))
		return;
	f32 y = c.y + 4;
	if (n->connected) {
		if (ui_menu_item(ui, rect(c.x + 4, y, c.w - 8, 34), "window-close", "Disconnect", NULL)) {
			wifi_command("disconnect");
			ui_popover_close(ui);
		}
		y += 36;
	} else {
		if (ui_menu_item(ui, rect(c.x + 4, y, c.w - 8, 34), "network-wireless", "Connect", NULL)) {
			ui_popover_close(ui);
			join(ui, n);
		}
		y += 36;
	}
	if (n->saved && ui_menu_item(ui, rect(c.x + 4, y, c.w - 8, 34), "trash", "Forget", NULL)) {
		char cmd[96];
		fmt(cmd, sizeof cmd, "forget %s", n->hex);
		wifi_command(cmd);
		ui_popover_close(ui);
	}
	ui_popover_end(ui);
}

static void page_network(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	f32 y0 = ui_scroll_begin(ui, r, "page-scroll");
	b.y = y0 + 26;
	b.h = 100000;
	f32 top = b.y;
	page_title(ui, &b, "Network", NULL);
	/* the network service publishes its state here */
	char *status = file_read("/run/zurbax/network.status", NULL);
	Conf *c = NULL;
	if (status) {
		c = conf_load("/run/zurbax/network.status");
		mem_free(status);
	}
	section(ui, &b, "Wi-Fi");
	bool adapter = c && conf_get_bool(c, "wifi_available", false);
	bool radio = c && str_eq(conf_get(c, "wifi", "off"), "on");
	const char *state = c ? conf_get(c, "wifi_state", "idle") : "idle";
	const char *ssid = c ? conf_get(c, "wifi_ssid", "") : "";
	Rect rows = card_rows(ui, &b, 1, 64);
	Rect wr = cut_top(&rows, 64);
	char sub[160];
	if (!c)
		str_copy(sub, "The network service isn't running", sizeof sub);
	else if (!adapter)
		str_copy(sub, "No Wi-Fi adapter found", sizeof sub);
	else if (!radio)
		str_copy(sub, "Off", sizeof sub);
	else if (str_eq(state, "connected"))
		fmt(sub, sizeof sub, "Connected to %s", ssid);
	else if (str_eq(state, "connecting"))
		fmt(sub, sizeof sub, "Connecting to %s…", ssid);
	else
		str_copy(sub, "Not connected", sizeof sub);
	ui_icon(ui, rect(wr.x, wr.y + 22, 20, 20), radio && adapter ? "network-wireless" : "network-wireless-off", t->text_dim);
	row_label(ui, pad4(wr, 0, 0, 0, 32), "Wi-Fi", sub);
	if (adapter) {
		bool on = radio;
		if (ui_switch(ui, center_in(cut_right(&wr, 52), 46, 26), &on))
			wifi_command(on ? "radio on" : "radio off");
	}
	if (adapter && radio) {
		send_scan(ui);
		const char *err = conf_get(c, "wifi_error", "");
		if (err[0] && !str_eq(state, "connected")) {
			Rect er = cut_top(&b, 44);
			ui_round(ui, er, 10, color_with_alpha(t->danger, 0x22));
			ui_icon(ui, rect(er.x + 14, er.y + 14, 16, 16), "error", t->danger);
			char msg[220];
			fmt(msg, sizeof msg, "Couldn't connect: %s", err);
			ui_text_in(ui, ui_font(ui, 13, 500), rect(er.x + 40, er.y, er.w - 50, er.h), msg, t->text, ALIGN_START);
			cut_top(&b, 14);
		}
		WifiNet nets[40];
		int n = wifi_networks(nets, 40);
		Rect head = cut_top(&b, 28);
		ui_heading(ui, head, "Networks");
		if (conf_get_bool(c, "wifi_scanning", false) || !n)
			ui_spinner(ui, rect(head.x + head.w - 20, head.y + 5, 18, 18));
		cut_top(&b, 4);
		if (!n) {
			Rect card = cut_top(&b, 64);
			ui_card(ui, card);
			ui_text_in(ui, ui_font(ui, 13, 500), card, "Looking for networks…", t->text_dim, ALIGN_CENTER);
			cut_top(&b, 14);
		} else {
			Rect list = card_rows(ui, &b, n, 56);
			for (int i = 0; i < n; i++) {
				WifiNet *w = &nets[i];
				Rect row = cut_top(&list, 56);
				if (i)
					ui_separator(ui, row.x, row.y, row.x + row.w);
				ui_push_id_str(ui, w->hex);
				bool ok = wifi_supported(w);
				UiInteract in = ui_row(ui, pad2(row, -10, 3), ui_id(ui, "net"), false);
				Color fg = ok ? t->text : t->text_faint;
				ui_icon(ui, rect(row.x, row.y + 18, 20, 20), wifi_signal_icon(w->signal), ok ? t->text_dim : t->text_faint);
				const char *what = w->connected ? (str_eq(state, "connected") ? "Connected" : "Connecting…")
						   : str_eq(w->security, "wpa3") ? "WPA3 only (not supported yet)"
						   : str_eq(w->security, "enterprise") ? "Enterprise (not supported yet)"
						   : str_eq(w->security, "wep") || str_eq(w->security, "wpa") ? "Outdated security (not supported)"
						   : w->saved ? "Saved" : str_eq(w->security, "open") ? "Open network" : "Secured";
				ui_text_in(ui, ui_font(ui, 14, w->connected ? 700 : 500), rect(row.x + 34, row.y + 8, row.w - 120, 22), w->ssid, fg, ALIGN_START);
				ui_text_in(ui, ui_font(ui, 12, 400), rect(row.x + 34, row.y + 28, row.w - 120, 18), what,
					   w->connected ? t->accent : t->text_dim, ALIGN_START);
				Rect right = cut_right(&row, 40);
				if (!str_eq(w->security, "open"))
					ui_icon(ui, rect(right.x - 22, row.y + 20, 16, 16), "lock", t->text_faint);
				if ((w->saved || w->connected) && ok) {
					Rect mb = center_in(right, 32, 32);
					if (ui_icon_button(ui, mb, "more-vertical", "Options")) {
						wf.menu = *w;
						ui_popover_toggle(ui, "wifi-menu");
					}
					if (str_eq(wf.menu.hex, w->hex))
						network_menu(ui, mb);
				}
				if (in.clicked && ok && !w->connected)
					join(ui, w);
				ui_pop_id(ui);
			}
		}
	}
	section(ui, &b, "Wired");
	rows = card_rows(ui, &b, 1, 64);
	Rect er = cut_top(&rows, 64);
	const char *eth = c ? conf_get(c, "ethernet", "") : "";
	ui_icon(ui, rect(er.x, er.y + 22, 20, 20), "network-wired", t->text_dim);
	row_label(ui, pad4(er, 0, 0, 0, 32), "Ethernet", eth[0] ? eth : "Cable unplugged");
	if (c) {
		const char *ip = conf_get(c, "address", "");
		if (ip[0]) {
			section(ui, &b, "Details");
			rows = card_rows(ui, &b, 2, 50);
			Rect a = cut_top(&rows, 50);
			ui_separator(ui, a.x, a.y + a.h, a.x + a.w);
			row_label(ui, a, "IP address", NULL);
			ui_text(ui, ui_font(ui, 13, 400), a.x + a.w - ui_text_width(ui, ui_font(ui, 13, 400), ip), a.y + 30, ip, t->text_dim);
			Rect d = cut_top(&rows, 50);
			const char *dns = conf_get(c, "dns", "");
			row_label(ui, d, "DNS", NULL);
			ui_text(ui, ui_font(ui, 13, 400), d.x + d.w - ui_text_width(ui, ui_font(ui, 13, 400), dns), d.y + 30, dns, t->text_dim);
		}
		conf_free(c);
	}
	ui_scroll_end(ui, b.y - top + 40);
	if (wf.ask) {
		wf.ask = false;
		ui_dialog_open(ui, "wifi-pass");
	}
	password_dialog(ui);
	/* the service's state changes on its own: look again shortly */
	ui->wake_at = ui->wake_at ? minf((f32)ui->wake_at, (f32)(ui->now + 1)) : ui->now + 1;
}

static void page_bluetooth(Ui *ui, Rect r)
{
	Rect b = page_body(r);
	page_title(ui, &b, "Bluetooth", NULL);
	bool adapter = dir_exists("/sys/class/bluetooth/hci0");
	ui_placeholder(ui, cut_top(&b, 260), "bluetooth", adapter ? "Bluetooth Is Coming Soon" : "No Bluetooth Adapter",
		       adapter ? "Your adapter was found. Pairing devices arrives in an update." :
				 "This computer doesn't seem to have Bluetooth.");
}

static void page_input(Ui *ui, Rect r)
{
	Rect b = page_body(r);
	page_title(ui, &b, "Mouse & Touchpad", NULL);
	section(ui, &b, "Pointer");
	Rect rows = card_rows(ui, &b, 1, 60);
	Rect sr = cut_top(&rows, 60);
	row_label(ui, sr, "Pointer speed", NULL);
	f32 sp = (f32)get_float("pointer_speed", 0.5);
	if (ui_slider(ui, center_in(cut_right(&sr, 260), 250, 24), &sp, 0, 1))
		set_float(ui, "pointer_speed", sp);
	section(ui, &b, "Touchpad");
	rows = card_rows(ui, &b, 2, 60);
	switch_row(ui, &rows, 60, false, "Tap to click", "Tap with one finger to click, two to right-click", "tap_to_click", true);
	switch_row(ui, &rows, 60, true, "Natural scrolling", "Content moves with your fingers", "natural_scroll", true);
}

static void page_keyboard(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	page_title(ui, &b, "Keyboard", NULL);
	section(ui, &b, "Layout");
	const char *cur = get("keyboard_layout", "us");
	Rect rows = card_rows(ui, &b, 6, 46);
	for (int i = 0; i < 6; i++) {
		Rect row = cut_top(&rows, 46);
		if (i < 5)
			ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
		if (ui_radio(ui, row, LAYOUT_TITLES[i], str_eq(cur, LAYOUT_IDS[i])))
			set_str(ui, "keyboard_layout", LAYOUT_IDS[i]);
	}
	section(ui, &b, "Shortcuts");
	static const char *const keys[][2] = {
		{ "Super + Space", "Open Applications" }, { "Alt + Tab", "Switch windows" }, { "Super + D", "Show the desktop" },
		{ "Super + L", "Lock the screen" }, { "Print Screen", "Take a screenshot" }, { "Ctrl + Alt + ← / →", "Tile a window left or right" },
		{ "Ctrl + Alt + ↑", "Maximize a window" }, { "AltGr + S / T / A / I / Q", "ș ț â î ă on any layout" },
	};
	rows = card_rows(ui, &b, 8, 42);
	for (int i = 0; i < 8; i++) {
		Rect row = cut_top(&rows, 42);
		if (i < 7)
			ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
		ui_text(ui, ui_font(ui, 13, 400), row.x, row.y + 26, keys[i][1], t->text);
		Font kf = ui_font(ui, 12, 600);
		f32 kw = ui_text_width(ui, kf, keys[i][0]) + 16;
		Rect kr = rect(row.x + row.w - kw, row.y + 9, kw, 24);
		ui_round(ui, kr, 5, t->dark ? color_hex("#ffffff14") : color_hex("#0000000c"));
		ui_border(ui, kr, 5, 1, t->border);
		ui_text_in(ui, kf, kr, keys[i][0], t->text_dim, ALIGN_CENTER);
	}
}

static void page_users(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	page_title(ui, &b, "Users", NULL);
	/* Results of a request to the desktop. */
	if (st.pending_seq && ui->reply_seq == st.pending_seq) {
		st.pending_seq = 0;
		str_copy(st.user_msg, ui->reply_status == 0 ? "Your changes were saved." : ui->reply_message, sizeof st.user_msg);
		st.user_msg_error = ui->reply_status != 0;
		if (ui->reply_status == 0) {
			sb_clear(&st.pw_old);
			sb_clear(&st.pw_new);
			sb_clear(&st.pw_confirm);
			char *path = str_fmt("%s/system/users.conf", zurbax_data());
			Conf *u = conf_load(path);
			st.has_password = conf_get_bool(u, "has_password", false);
			conf_free(u);
			mem_free(path);
		}
	}
	Rect top = cut_top(&b, 96);
	ui_avatar(ui, rect(top.x, top.y + 8, 80, 80), sb_str(&st.full_name));
	ui_text(ui, ui_font(ui, 22, 400), top.x + 100, top.y + 44, sb_str(&st.full_name), t->text);
	char sub[96];
	fmt(sub, sizeof sub, "%s · Administrator", st.user_name);
	ui_text(ui, ui_font(ui, 13, 400), top.x + 100, top.y + 66, sub, t->text_dim);
	cut_top(&b, 10);
	section(ui, &b, "Name");
	Rect nr = cut_top(&b, 34);
	ui_entry(ui, cut_left(&nr, 320), "full-name", &st.full_name, "Your name", 0);
	cut_left(&nr, 10);
	if (ui_button(ui, cut_left(&nr, 90), "Save"))
		st.pending_seq = ui_account_update(ui, ACCOUNT_SET_NAME, sb_str(&st.full_name), NULL, NULL);
	cut_top(&b, 18);
	section(ui, &b, st.has_password ? "Change Password" : "Add a Password");
	ui_label_dim(ui, cut_top(&b, 22), st.has_password ? "You'll need your current password." :
							    "Without a password, anyone at this computer can use your account.");
	cut_top(&b, 6);
	if (st.has_password) {
		ui_entry(ui, cut_left(&(Rect){ b.x, b.y, 320, 34 }, 320), "pw-old", &st.pw_old, "Current password", ENTRY_PASSWORD);
		cut_top(&b, 42);
	}
	Rect p1 = cut_top(&b, 34);
	ui_entry(ui, cut_left(&p1, 320), "pw-new", &st.pw_new, "New password", ENTRY_PASSWORD);
	cut_top(&b, 8);
	Rect p2 = cut_top(&b, 34);
	ui_entry(ui, cut_left(&p2, 320), "pw-confirm", &st.pw_confirm, "Confirm new password", ENTRY_PASSWORD);
	cut_top(&b, 12);
	Rect br = cut_top(&b, 34);
	bool match = str_eq(sb_str(&st.pw_new), sb_str(&st.pw_confirm));
	if (st.pw_confirm.len && !match)
		ui_text(ui, ui_font(ui, 12, 500), br.x + 340, br.y + 22, "The passwords don't match", t->danger);
	if (ui_button_suggested(ui, cut_left(&br, 160), st.has_password ? "Change Password" : "Set Password") && st.pw_new.len && match)
		st.pending_seq = ui_account_update(ui, ACCOUNT_SET_PASSWORD, NULL, sb_str(&st.pw_old), sb_str(&st.pw_new));
	if (st.has_password) {
		cut_left(&br, 10);
		if (ui_button_flat(ui, cut_left(&br, 150), "Remove Password"))
			st.pending_seq = ui_account_update(ui, ACCOUNT_REMOVE_PASSWORD, NULL, sb_str(&st.pw_old), NULL);
	}
	if (st.user_msg[0]) {
		cut_top(&b, 10);
		ui_text(ui, ui_font(ui, 13, 500), b.x, b.y + 16, st.user_msg, st.user_msg_error ? t->danger : t->success);
	}
}

static void page_datetime(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	page_title(ui, &b, "Date & Time", NULL);
	Civil c;
	civil_local(time_unix(), &c);
	char now[64];
	fmt(now, sizeof now, "%02d:%02d · %d %s %d", c.hour, c.min, c.day, MONTH_NAMES[c.month - 1], c.year);
	ui_text(ui, ui_font(ui, 28, 300), b.x, b.y + 30, now, t->text);
	cut_top(&b, 50);
	ui->wake_at = ui->now + 1;
	section(ui, &b, "Time Zone");
	const char *cur = get("timezone", tz_name());
	const ZoneInfo *cz = tz_find(cur);
	char curl[128];
	fmt(curl, sizeof curl, "Current: %s%s%s", cz ? cz->city : cur, cz && cz->country[0] ? ", " : "", cz ? cz->country : "");
	ui_label_dim(ui, cut_top(&b, 22), curl);
	cut_top(&b, 6);
	ui_entry(ui, cut_top(&b, 32), "tz-search", &st.tz_search, "Search for a city", ENTRY_SEARCH);
	cut_top(&b, 8);
	Rect list = cut_top(&b, minf(260, b.h - 20));
	ui_card(ui, list);
	usize nz;
	const ZoneInfo *zones = tz_zones(&nz);
	const char *q = sb_str(&st.tz_search);
	f32 y = ui_scroll_begin(ui, pad(list, 4), "tz-list");
	for (usize i = 0; i < nz; i++) {
		const ZoneInfo *z = &zones[i];
		if (*q && !str_find_nocase(z->city, q) && !str_find_nocase(z->country, q))
			continue;
		Rect row = rect(list.x + 4, y, list.w - 8, 36);
		y += 36;
		ui_push_id(ui, (i64)i);
		UiInteract it = ui_row(ui, row, ui_id(ui, "tz"), str_eq(cur, z->zone));
		ui_pop_id(ui);
		char name[128];
		fmt(name, sizeof name, "%s%s%s", z->city, z->country[0] ? ", " : "", z->country);
		ui_text(ui, ui_font(ui, 13, 500), row.x + 10, row.y + 23, name, t->text);
		if (it.clicked) {
			set_str(ui, "timezone", z->zone);
			tz_set(z->zone);
		}
	}
	ui_scroll_end(ui, y - list.y);
	cut_top(&b, 14);
	Rect rows = card_rows(ui, &b, 1, 56);
	switch_row(ui, &rows, 56, true, "24-hour clock", NULL, "clock_24h", true);
}

/* A controller drawn from shapes, lit up as it's used: the test in Gaming. */
static void controller_test(Ui *ui, Rect r, int pad)
{
	Theme *t = &ui->theme;
	f32 cx = r.x + r.w / 2, cy = r.y + r.h / 2 + 6;
	Color body = t->dark ? color_hex("#3a3b41") : color_hex("#e4e5ea");
	Color part = t->dark ? color_hex("#26272c") : color_hex("#c9cbd2");
	Color lit = t->accent;
	bool dark = t->dark;
	/* triggers and bumpers */
	for (int side = -1; side <= 1; side += 2) {
		f32 tx = cx + (f32)side * 112 - 34;
		f32 v = ui_pad_axis_of(ui, pad, side < 0 ? PAD_LTRIGGER : PAD_RTRIGGER);
		bool td = ui_pad_held(ui, pad, side < 0 ? PAD_LT : PAD_RT);
		Rect tr = rect(tx, cy - 112, 68, 14);
		ui_round(ui, tr, 7, part);
		if (v > 0 || td)
			ui_round(ui, rect(tr.x, tr.y, tr.w * (td && v == 0 ? 1 : v), tr.h), 7, lit);
		bool bd = ui_pad_held(ui, pad, side < 0 ? PAD_LB : PAD_RB);
		ui_round(ui, rect(tx - 6, cy - 90, 80, 16), 8, bd ? lit : part);
	}
	/* the body and grips */
	ui_shadow(ui, rect(cx - 170, cy - 72, 340, 130), 60, 18, 6, color_hex("#00000030"));
	ui_circle(ui, cx - 118, cy + 44, 52, body);
	ui_circle(ui, cx + 118, cy + 44, 52, body);
	ui_round(ui, rect(cx - 170, cy - 72, 340, 124), 60, body);
	/* sticks */
	for (int side = -1; side <= 1; side += 2) {
		f32 sx = side < 0 ? cx - 108 : cx + 52, sy = side < 0 ? cy - 18 : cy + 30;
		f32 ax = ui_pad_axis_of(ui, pad, side < 0 ? PAD_LX : PAD_RX), ay = ui_pad_axis_of(ui, pad, side < 0 ? PAD_LY : PAD_RY);
		bool pressed = ui_pad_held(ui, pad, side < 0 ? PAD_LS : PAD_RS);
		ui_circle(ui, sx, sy, 30, part);
		ui_circle(ui, sx + ax * 12, sy + ay * 12, 21, pressed ? lit : (dark ? color_hex("#4c4d54") : COLOR_WHITE));
		ui_ring(ui, sx + ax * 12, sy + ay * 12, 21, 1.5f, color_hex("#00000022"));
	}
	/* D-pad */
	f32 dx = cx - 52, dy = cy + 30;
	static const int dirs[4] = { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT };
	static const f32 off[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
	ui_round(ui, rect(dx - 9, dy - 9, 18, 18), 3, part);
	for (int i = 0; i < 4; i++)
		ui_round(ui, rect(dx + off[i][0] * 18 - 9, dy + off[i][1] * 18 - 9, 18, 18), 4,
			 ui_pad_held(ui, pad, dirs[i]) ? lit : part);
	/* face buttons, in their usual colours */
	static const struct { int b; const char *label; u32 color; f32 x, y; } face[4] = {
		{ PAD_A, "A", 0xff3ccf5a, 0, 1 }, { PAD_B, "B", 0xffe5484d, 1, 0 }, { PAD_X, "X", 0xff3b82f6, -1, 0 }, { PAD_Y, "Y", 0xfff5b400, 0, -1 },
	};
	f32 fx = cx + 108, fy = cy - 18;
	for (int i = 0; i < 4; i++) {
		f32 bx = fx + face[i].x * 26, by = fy + face[i].y * 26;
		bool on = ui_pad_held(ui, pad, face[i].b);
		ui_circle(ui, bx, by, 14, on ? face[i].color : part);
		ui_text_in(ui, ui_font(ui, 12, 800), rect(bx - 14, by - 14, 28, 28), face[i].label, on ? COLOR_WHITE : face[i].color, ALIGN_CENTER);
	}
	/* back, guide, start */
	ui_round(ui, rect(cx - 44, cy - 34, 22, 12), 6, ui_pad_held(ui, pad, PAD_BACK) ? lit : part);
	ui_round(ui, rect(cx + 22, cy - 34, 22, 12), 6, ui_pad_held(ui, pad, PAD_START) ? lit : part);
	ui_circle(ui, cx, cy - 46, 13, ui_pad_held(ui, pad, PAD_GUIDE) ? lit : part);
	ui_icon(ui, rect(cx - 7, cy - 53, 14, 14), "gamepad", dark ? color_hex("#9a9ba3") : color_hex("#6b6d76"));
}

static void page_gaming(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	/* the controller test reads buttons itself: B isn't Escape here */
	ui_pad_raw(ui);
	Rect b = page_body(r);
	f32 y0 = ui_scroll_begin(ui, r, "page-scroll");
	b.y = y0 + 26;
	b.h = 100000;
	f32 top = b.y;
	page_title(ui, &b, "Gaming", "Get the most out of this computer when you play.");
	Rect rows = card_rows(ui, &b, 4, 64);
	switch_row(ui, &rows, 64, false, "Game Mode", "Full speed for the processor and no notifications while a game is full screen",
		   "game_mode", true);
	switch_row(ui, &rows, 64, false, "Frame rate counter", "Shows frames per second in the corner of the screen", "show_fps", false);
	switch_row(ui, &rows, 64, false, "Controller vibration", "Lets games shake your controller when something happens", "pad_rumble", true);
	switch_row(ui, &rows, 64, true, "Mouse acceleration", "Turn off for precise aiming", "mouse_accel", true);
	section(ui, &b, "Windows Programs");
	Rect card = cut_top(&b, 92);
	ui_card(ui, card);
	Rect in = pad(card, 18);
	ui_icon(ui, rect(in.x, in.y + 4, 32, 32), "bridge", t->text_dim);
	ui_text(ui, ui_font(ui, 14, 600), in.x + 48, in.y + 16, "Running .exe programs", t->text);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(in.x + 48, in.y + 24, in.w - 48, 40),
			"Bridge runs Windows programs and games with Proton, keeps your Steam games on a shelf, and plays Fortnite and other games in the cloud.",
			t->text_dim, ALIGN_START);
	cut_top(&b, 14);
	if (ui_button(ui, cut_left(&(Rect){ b.x, b.y, 200, 34 }, 200), "Open Bridge"))
		ui_launch(ui, "bridge", NULL);
	cut_top(&b, 34 + 14);
	/* controllers, with a live test */
	section(ui, &b, "Controllers");
	static int shown_pad;
	int n = ui_pad_count(ui);
	if (!n) {
		Rect card = cut_top(&b, 92);
		ui_card(ui, card);
		Rect in = pad(card, 18);
		ui_icon(ui, rect(in.x, in.y + 4, 32, 32), "gamepad", t->text_dim);
		ui_text(ui, ui_font(ui, 14, 600), in.x + 48, in.y + 16, "No controller connected", t->text);
		ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(in.x + 48, in.y + 24, in.w - 48, 40),
				"Plug in a USB controller: Xbox, PlayStation, Switch Pro and most others work, in games and to move around the desktop.",
				t->text_dim, ALIGN_START);
	} else {
		if (!ui->pads[shown_pad].connected)
			for (int i = 0; i < PAD_MAX; i++)
				if (ui->pads[i].connected) {
					shown_pad = i;
					break;
				}
		Rect card = cut_top(&b, 56 + 270);
		ui_card(ui, card);
		Rect in = pad(card, 18);
		Rect head = cut_top(&in, 30);
		ui_icon(ui, rect(head.x, head.y + 5, 20, 20), "gamepad", t->accent);
		ui_text_in(ui, ui_font(ui, 14, 600), rect(head.x + 30, head.y, head.w - 200, 30), ui->pads[shown_pad].name, t->text, ALIGN_START);
		if (n > 1) {
			char label[32];
			fmt(label, sizeof label, "Player %d of %d", shown_pad + 1, n);
			if (ui_button(ui, rect(head.x + head.w - 140, head.y, 140, 30), label)) {
				for (int k = 1; k <= PAD_MAX; k++)
					if (ui->pads[(shown_pad + k) % PAD_MAX].connected) {
						shown_pad = (shown_pad + k) % PAD_MAX;
						break;
					}
			}
		} else {
			ui_text_in(ui, ui_font(ui, 12, 400), rect(head.x, head.y, head.w, 30), "Press buttons to test them", t->text_dim, ALIGN_END);
		}
		/* battery and a vibration test along the bottom */
		Rect foot = rect(in.x, in.y + in.h - 32, in.w, 32);
		int level = ui->pads[shown_pad].battery;
		if (level >= 0) {
			char bat[32];
			fmt(bat, sizeof bat, "Battery %d%%", level);
			ui_icon(ui, rect(foot.x, foot.y + 7, 18, 18), level > 80 ? "battery-full" : level > 45 ? "battery-good" : level > 15 ? "battery-low" : "battery-caution",
				level < 20 ? color_hex("#e5484d") : t->text_dim);
			ui_text_in(ui, ui_font(ui, 12, 600), rect(foot.x + 26, foot.y, 160, 32), bat, level < 20 ? color_hex("#e5484d") : t->text_dim, ALIGN_START);
		}
		if (ui->pads[shown_pad].rumble && ui_button(ui, rect(foot.x + foot.w - 140, foot.y, 140, 32), "Test Vibration"))
			ui_pad_rumble(ui, shown_pad, 0.8f, 0.6f, 400);
		in.h -= 36;
		controller_test(ui, in, shown_pad);
	}
	cut_top(&b, 14);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), cut_top(&b, 40),
			"Press the Guide (logo) button for Applications, then use the D-pad and A to start an app. "
			"In apps, the D-pad moves, A selects and B goes back.", t->text_faint, ALIGN_START);
	ui_scroll_end(ui, b.y - top + 40);
}

static void page_screentime(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	page_title(ui, &b, "Screen Time", "See how long you use the computer and set healthy limits.");
	/* Today's total, from the desktop's count. */
	Civil c;
	civil_local(time_unix(), &c);
	char *path = str_fmt("%s/.local/share/zurbax/screentime/%04d-%02d-%02d.conf", home_dir(), c.year, c.month, c.day);
	Conf *day = conf_load(path);
	mem_free(path);
	i64 total = 0;
	char *text = NULL;
	UNUSED(text);
	path = str_fmt("%s/.local/share/zurbax/screentime/%04d-%02d-%02d.conf", home_dir(), c.year, c.month, c.day);
	char *raw = file_read(path, NULL);
	mem_free(path);
	if (raw) {
		for (const char *p = raw; p && *p;) {
			const char *eq = str_chr(p, '=');
			if (!eq)
				break;
			total += str_to_int(eq + 1, NULL);
			p = str_chr(eq, '\n');
			if (p)
				p++;
		}
		mem_free(raw);
	}
	conf_free(day);
	char tot[48];
	if (total >= 3600)
		fmt(tot, sizeof tot, "%lld h %lld min today", (long long)(total / 3600), (long long)(total % 3600 / 60));
	else
		fmt(tot, sizeof tot, "%lld min today", (long long)(total / 60));
	Rect card = cut_top(&b, 96);
	ui_card(ui, card);
	Rect in = pad(card, 20);
	ui_app_icon(ui, rect(in.x, in.y, 56, 56), "screentime");
	ui_text(ui, ui_font(ui, 24, 300), in.x + 72, in.y + 30, tot, t->text);
	ui_text(ui, ui_font(ui, 12, 400), in.x + 72, in.y + 50, "Limits per app, downtime and break reminders are in the Screen Time app.", t->text_dim);
	cut_top(&b, 16);
	if (ui_button_suggested(ui, cut_left(&(Rect){ b.x, b.y, 200, 34 }, 200), "Open Screen Time"))
		ui_launch(ui, "screentime", NULL);
}

static void about_row(Ui *ui, Rect *rows, const char *label, const char *value, bool last)
{
	Theme *t = &ui->theme;
	Rect r = cut_top(rows, 44);
	if (!last)
		ui_separator(ui, r.x, r.y + r.h, r.x + r.w);
	ui_text(ui, ui_font(ui, 13, 500), r.x, r.y + 27, label, t->text_dim);
	Font f = ui_font(ui, 13, 500);
	ui_text(ui, f, r.x + r.w - ui_text_width(ui, f, value), r.y + 27, value, t->text);
}

/* ---- Privacy & Security --------------------------------------------------------------- */

/* zupdate, the system's updater, reads requests from a socket and writes how
 * it's going to a status file (see system/zupdate). */
static void update_command(const char *cmd)
{
	int fd = (int)sys_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	str_copy(a.sun_path, "/run/zurbax/update.sock", sizeof a.sun_path);
	sys_sendto(fd, cmd, strlen(cmd), 0, &a, sizeof a);
	sys_close(fd);
}

static void month_year(const char *date, char *out, usize cap)
{
	static const char *const MONTHS[] = { "January", "February", "March", "April", "May", "June", "July",
					      "August", "September", "October", "November", "December" };
	int y = (int)str_to_int(date, &date), m = *date == '-' ? (int)str_to_int(date + 1, NULL) : 0;
	if (m >= 1 && m <= 12)
		fmt(out, cap, "%s %d", MONTHS[m - 1], y);
	else
		str_copy(out, date, cap);
}

/* Live checks: what the running kernel actually has on. */
static bool proc_value_at_least(const char *path, int want)
{
	char *v = file_read_line(path);
	bool ok = v && str_to_int(v, NULL) >= want;
	mem_free(v);
	return ok;
}

static int network_listeners(void)
{
	/* sockets in LISTEN (0A) on anything but the loopback address */
	static const char *const FILES[] = { "/proc/net/tcp", "/proc/net/tcp6" };
	int n = 0;
	for (usize f = 0; f < ARRAY_LEN(FILES); f++) {
		char *t = file_read(FILES[f], NULL);
		for (char *line = t ? (char *)str_chr(t, '\n') : NULL; line && line[1];) {
			line++;
			char *nl = (char *)str_chr(line, '\n');
			if (nl)
				*nl = 0;
			char local[64] = "", st_[8] = "";
			const char *q = str_chr(line, ':');
			if (q) {
				q++;
				while (*q == ' ')
					q++;
				usize i = 0;
				while (*q && *q != ' ' && i + 1 < sizeof local)
					local[i++] = *q++;
				local[i] = 0;
				while (*q == ' ')
					q++;
				while (*q && *q != ' ')
					q++; /* remote */
				while (*q == ' ')
					q++;
				str_copy(st_, q, 3);
			}
			bool loopback = str_starts(local, "0100007F") || str_starts(local, "00000000000000000000000001000000");
			if (str_eq(st_, "0A") && !loopback)
				n++;
			line = nl;
		}
		mem_free(t);
	}
	return n;
}

static void check_row(Ui *ui, Rect *rows, bool ok, const char *label, const char *sub, bool last)
{
	Rect r = cut_top(rows, 58);
	if (!last)
		ui_separator(ui, r.x, r.y + r.h, r.x + r.w);
	ui_icon(ui, rect(r.x, r.y + 18, 22, 22), ok ? "shield-check" : "shield", ok ? color_hex("#3fb950") : color_hex("#e3a21a"));
	row_label(ui, rect(r.x + 36, r.y, r.w - 36, r.h), label, sub);
}

static void page_security(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	f32 y0 = ui_scroll_begin(ui, r, "page-scroll");
	b.y = y0 + 26;
	b.h = 100000;
	f32 top = b.y;
	Conf *u = conf_load("/run/zurbax/update.status");
	const char *state = conf_get(u, "state", "");
	const char *until = conf_get(u, "until", OS_SUPPORT_END);
	char until_text[40], sub[160];
	month_year(until, until_text, sizeof until_text);
	fmt(sub, sizeof sub, OS_NAME " " OS_VERSION " LTS gets security updates every two weeks until %s.", until_text);
	page_title(ui, &b, "Privacy & Security", sub);

	/* updates */
	Rect card = cut_top(&b, 128);
	ui_card(ui, card);
	Rect in = pad(card, 18);
	const char *available = conf_get(u, "available", ""), *message = conf_get(u, "message", "");
	f32 progress = (f32)conf_get_float(u, "progress", 0);
	char title[120], line[240];
	const char *icon = "shield-check";
	Color ic = color_hex("#3fb950");
	line[0] = 0;
	i64 checked = conf_get_int(u, "checked", 0);
	char ago[48] = "not yet";
	if (checked) {
		i64 d = time_unix() - checked;
		if (d < 120)
			str_copy(ago, "just now", sizeof ago);
		else if (d < 7200)
			fmt(ago, sizeof ago, "%d minutes ago", (int)(d / 60));
		else if (d < 2 * 86400)
			fmt(ago, sizeof ago, "%d hours ago", (int)(d / 3600));
		else
			fmt(ago, sizeof ago, "%d days ago", (int)(d / 86400));
	}
	const char *action = NULL, *cmd = NULL;
	if (str_eq(state, "checking")) {
		str_copy(title, "Checking for updates…", sizeof title);
		fmt(line, sizeof line, "You have " OS_NAME " %s.", OS_FULL_VERSION);
	} else if (str_eq(state, "available")) {
		fmt(title, sizeof title, OS_NAME " %s is available", available);
		str_copy(line, conf_get(u, "notes", "Security fixes."), sizeof line);
		action = "Download and Install", cmd = "install";
		icon = "shield", ic = t->accent;
	} else if (str_eq(state, "downloading")) {
		fmt(title, sizeof title, "Downloading " OS_NAME " %s… %d%%", available, (int)(progress * 100));
		str_copy(line, conf_get(u, "notes", ""), sizeof line);
		icon = "download", ic = t->accent;
	} else if (str_eq(state, "ready")) {
		fmt(title, sizeof title, OS_NAME " %s is ready", available);
		str_copy(line, "Restart to finish. Your apps, files and settings stay as they are.", sizeof line);
		action = "Restart Now", cmd = "restart";
	} else if (str_eq(state, "error")) {
		str_copy(title, "The last check didn't finish", sizeof title);
		str_copy(line, message, sizeof line);
		action = "Try Again", cmd = "check";
		icon = "shield", ic = color_hex("#e3a21a");
	} else if (str_eq(state, "live")) {
		str_copy(title, "Updates start once Zurbax is installed", sizeof title);
		str_copy(line, "This is the live session from the install stick.", sizeof line);
		icon = "shield", ic = t->text_dim;
	} else if (!state[0]) {
		str_copy(title, "The updater isn't running", sizeof title);
		str_copy(line, "It starts with Zurbax; restarting the computer brings it back.", sizeof line);
		icon = "shield", ic = color_hex("#e3a21a");
	} else {
		str_copy(title, "You're up to date", sizeof title);
		if (message[0])
			str_copy(line, message, sizeof line);
		else
			fmt(line, sizeof line, OS_NAME " %s. Last checked %s.", OS_FULL_VERSION, ago);
	}
	ui_icon(ui, rect(in.x, in.y + 4, 36, 36), icon, ic);
	ui_text(ui, ui_font(ui, 16, 600), in.x + 52, in.y + 18, title, t->text);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(in.x + 52, in.y + 26, in.w - 52, 34), line, t->text_dim, ALIGN_START);
	if (str_eq(state, "downloading"))
		ui_progress(ui, rect(in.x + 52, in.y + 66, in.w - 52, 6), progress);
	Rect btns = rect(in.x + 52, in.y + in.h - 34, in.w - 52, 34);
	if (action && ui_button_suggested(ui, cut_left(&btns, 200), action)) {
		if (str_eq(cmd, "restart"))
			ui_session(ui, SESSION_RESTART);
		else
			update_command(cmd);
	}
	if (action)
		cut_left(&btns, 10);
	bool working = str_eq(state, "checking") || str_eq(state, "downloading");
	if (!working && !str_eq(state, "live") && state[0] && ui_button(ui, cut_left(&btns, 160), "Check Now"))
		update_command("check");
	if (working)
		ui->redraw = true;
	cut_top(&b, 14);
	Rect rows = card_rows(ui, &b, 1, 64);
	{
		Rect row = cut_top(&rows, 64);
		row_label(ui, row, "Install security updates automatically", "They're downloaded and checked on their own; a restart finishes them");
		bool on = conf_get_int(u, "auto", 1) != 0;
		if (ui_switch(ui, center_in(cut_right(&row, 52), 46, 26), &on))
			update_command(on ? "auto on" : "auto off");
	}

	/* long-term support */
	section(ui, &b, "Long-Term Support");
	rows = card_rows(ui, &b, 4, 44);
	about_row(ui, &rows, "Release", OS_NAME " " OS_VERSION " LTS", false);
	about_row(ui, &rows, "Installed version", OS_FULL_VERSION, false);
	about_row(ui, &rows, "Security updates", "Every two weeks", false);
	about_row(ui, &rows, "Supported until", until_text, true);

	/* what's protecting this computer, checked live */
	section(ui, &b, "Protections");
	char *lockdown = file_read_line("/sys/kernel/security/lockdown");
	bool locked = lockdown && (str_find(lockdown, "[integrity]") || str_find(lockdown, "[confidentiality]"));
	mem_free(lockdown);
	int listeners = network_listeners();
	char lsub[120];
	if (listeners)
		fmt(lsub, sizeof lsub, "%d program%s accept%s connections from other computers", listeners, listeners == 1 ? "" : "s",
		    listeners == 1 ? "s" : "");
	else
		str_copy(lsub, "Nothing on this computer accepts connections from the network", sizeof lsub);
	rows = card_rows(ui, &b, 7, 58);
	check_row(ui, &rows, true, "Verified updates", "Every update is signed by Zurbax and checked before it's installed", false);
	check_row(ui, &rows, locked, "Kernel lockdown", locked ? "Nothing can change the running kernel, not even the administrator" : "Off on this computer", false);
	check_row(ui, &rows, proc_value_at_least("/proc/sys/kernel/randomize_va_space", 2), "Address randomization",
		  "Programs and their memory load at a different random place each time", false);
	check_row(ui, &rows, proc_value_at_least("/proc/sys/kernel/yama/ptrace_scope", 1), "Apps kept apart",
		  "An app can't read another app's memory or watch what it does", false);
	check_row(ui, &rows, true, "Memory protection", "Stack canaries, zeroed memory and hardened allocators catch attacks early", false);
	check_row(ui, &rows, true, "Sandboxed Linux apps", "Flatpak apps run in a sandbox with only what they're given", false);
	check_row(ui, &rows, listeners == 0, "Closed to the network", lsub, true);

	/* privacy */
	section(ui, &b, "Privacy");
	rows = card_rows(ui, &b, 3, 64);
	check_row(ui, &rows, true, "Nothing is collected", "No telemetry, no ads and no account needed. What you do stays on this computer", false);
	{
		Rect row = cut_top(&rows, 64);
		ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
		row_label(ui, row, "Clipboard history", "Super+V shows what you copied; it's kept in memory only, never saved");
		bool on = get_bool("clipboard_history", true);
		ui_push_id_str(ui, "clipboard_history");
		if (ui_switch(ui, center_in(cut_right(&row, 52), 46, 26), &on))
			set_bool(ui, "clipboard_history", on);
		ui_pop_id(ui);
	}
	switch_row(ui, &rows, 64, true, "Lock when the computer sleeps", "With a password set, it's asked for when you come back",
		   "lock_on_sleep", true);
	conf_free(u);
	ui_scroll_end(ui, b.y - top + 40);
}

static void page_about(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	Rect top = cut_top(&b, 180);
	ui_app_icon(ui, center_in(cut_top(&top, 110), 100, 100), "zurbax");
	ui_text_in(ui, ui_font(ui, 28, 300), cut_top(&top, 40), OS_NAME " " OS_VERSION " LTS", t->text, ALIGN_CENTER);
	ui_text_in(ui, ui_font(ui, 12, 400), cut_top(&top, 18), "Built from scratch on the Linux kernel", t->text_dim, ALIGN_CENTER);
	Rect rows = card_rows(ui, &b, 7, 44);
	char mem[32], disk[64], tot[32], fr[32];
	fmt_bytes(mem, sizeof mem, st.mem_kb * 1024);
	fmt_bytes(tot, sizeof tot, st.disk_total);
	fmt_bytes(fr, sizeof fr, st.disk_free);
	fmt(disk, sizeof disk, "%s (%s free)", tot, fr);
	about_row(ui, &rows, "Computer", st.board[0] ? st.board : "Unknown", false);
	about_row(ui, &rows, "Processor", st.cpu[0] ? st.cpu : "Unknown", false);
	about_row(ui, &rows, "Memory", mem, false);
	about_row(ui, &rows, "Graphics", st.gpu[0] ? st.gpu : "Firmware framebuffer", false);
	about_row(ui, &rows, "Storage", disk, false);
	about_row(ui, &rows, "Kernel", st.kernel, false);
	about_row(ui, &rows, "Drivers", st.drivers, true);
	Rect btns = cut_top(&b, 34);
	if (ui_button(ui, cut_left(&btns, 170), "Check for Updates"))
		st.page = PAGE_SECURITY;
	cut_left(&btns, 10);
	if (ui_button(ui, cut_left(&btns, 150), "System Monitor"))
		ui_launch(ui, "pulse", NULL);
}

static void load_about(void)
{
	char *ci = file_read("/proc/cpuinfo", NULL);
	if (ci) {
		const char *m = str_find(ci, "model name");
		if (m && (m = str_chr(m, ':'))) {
			m += 2;
			const char *e = str_chr(m, '\n');
			fmt(st.cpu, sizeof st.cpu, "%.*s", (int)(e ? e - m : (long)strlen(m)), m);
		}
		mem_free(ci);
	}
	char *mi = file_read("/proc/meminfo", NULL);
	if (mi) {
		const char *p = str_find(mi, "MemTotal:");
		if (p)
			st.mem_kb = (u64)str_to_int(p + 9, NULL);
		mem_free(mi);
	}
	char *v = file_read_line("/sys/class/dmi/id/sys_vendor"), *pn = file_read_line("/sys/class/dmi/id/product_name");
	if (pn)
		fmt(st.board, sizeof st.board, "%s %s", v ? v : "", pn);
	mem_free(v);
	mem_free(pn);
	struct utsname u;
	if (sys_uname(&u) == 0) {
		/* drivers: the modules in drivers.sfs (one line each in depmod's
		 * modules.dep) and how many are loaded (lines of /proc/modules) */
		char *dep = str_fmt("/data/layers/drivers/lib/modules/%s/modules.dep", u.release);
		char *all = file_read(dep, NULL), *loaded = file_read("/proc/modules", NULL);
		int n_all = 0, n_loaded = 0;
		for (const char *p = all; p && *p; p++)
			n_all += *p == '\n';
		for (const char *p = loaded; p && *p; p++)
			n_loaded += *p == '\n';
		char count[16];
		if (n_all >= 1000)
			fmt(count, sizeof count, "%d,%03d", n_all / 1000, n_all % 1000);
		else
			fmt(count, sizeof count, "%d", n_all);
		if (n_all)
			fmt(st.drivers, sizeof st.drivers, "%d in use, %s ready for any device", n_loaded, count);
		else
			str_copy(st.drivers, "Built into the kernel", sizeof st.drivers);
		mem_free(all);
		mem_free(loaded);
		mem_free(dep);
		/* Ember, Zurbax's kernel, says so in /proc/version; its release is Linux's */
		char *pv = file_read_line("/proc/version");
		char base[64];
		str_copy(base, u.release, sizeof base);
		char *dash = (char *)str_chr(base, '-');
		if (dash)
			*dash = 0;
		if (pv && str_starts(pv, "Ember version ")) {
			const char *ver = pv + 14, *end = str_chr(ver, ' ');
			fmt(st.kernel, sizeof st.kernel, "Ember %.*s (Linux %s)", (int)(end ? end - ver : (long)strlen(ver)), ver, base);
		} else {
			fmt(st.kernel, sizeof st.kernel, "Linux %s", u.release);
		}
		mem_free(pv);
	}
	char link[256];
	long n = sys_readlink("/sys/class/drm/card0/device/driver", link, sizeof link - 1);
	if (n > 0) {
		link[n] = 0;
		const char *d = path_base(link);
		str_copy(st.gpu, str_eq(d, "i915") || str_eq(d, "xe") ? "Intel Graphics" : str_eq(d, "amdgpu") ? "AMD Radeon Graphics" :
				  str_eq(d, "nouveau") ? "NVIDIA Graphics" : d, sizeof st.gpu);
	}
	struct statfs sf;
	if (sys_statfs(zurbax_data(), &sf) == 0 || sys_statfs("/", &sf) == 0) {
		st.disk_total = sf.f_blocks * (u64)sf.f_bsize;
		st.disk_free = sf.f_bavail * (u64)sf.f_bsize;
	}
}

/* ---- Frame ------------------------------------------------------------------------------ */

static bool page_matches(int i, const char *q)
{
	return !*q || str_find_nocase(PAGES[i].title, q) || str_find_nocase(PAGES[i].keywords, q);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	Rect side = cut_left(&win, 250);
	Rect tools = ui_headerbar(ui, header, PAGES[st.page].title, 0);
	UNUSED(tools);
	ui_fill(ui, side, t->sidebar);
	ui_vseparator(ui, side.x + side.w - 1, side.y, side.y + side.h);
	Rect s = pad4(side, 10, 10, 0, 10);
	int ev = ui_entry(ui, cut_top(&s, 32), "search", &st.search, "Search settings", ENTRY_SEARCH);
	cut_top(&s, 6);
	const char *q = sb_str(&st.search);
	if (ev & ENTRY_SUBMIT)
		for (int i = 0; i < PAGE_COUNT; i++)
			if (page_matches(i, q)) {
				st.page = i;
				break;
			}
	f32 y = ui_scroll_begin(ui, s, "nav");
	const char *group = NULL;
	for (int i = 0; i < PAGE_COUNT; i++) {
		if (!page_matches(i, q))
			continue;
		if (!*q && (!group || !str_eq(group, PAGES[i].group))) {
			group = PAGES[i].group;
			ui_sidebar_heading(ui, rect(s.x, y, s.w, 28), group);
			y += 28;
		}
		ui_push_id(ui, i);
		if (ui_sidebar_item(ui, rect(s.x, y, s.w, 34), PAGES[i].icon, PAGES[i].title, st.page == i))
			st.page = i;
		ui_pop_id(ui);
		y += 36;
	}
	ui_scroll_end(ui, y - s.y);
	ui_fill(ui, win, t->window);
	ui_push_id(ui, 1000 + st.page);
	switch (st.page) {
	case PAGE_APPEARANCE: page_appearance(ui, win); break;
	case PAGE_DESKTOP: page_desktop(ui, win); break;
	case PAGE_DISPLAY: page_display(ui, win); break;
	case PAGE_SOUND: page_sound(ui, win); break;
	case PAGE_NETWORK: page_network(ui, win); break;
	case PAGE_BLUETOOTH: page_bluetooth(ui, win); break;
	case PAGE_INPUT: page_input(ui, win); break;
	case PAGE_KEYBOARD: page_keyboard(ui, win); break;
	case PAGE_USERS: page_users(ui, win); break;
	case PAGE_DATETIME: page_datetime(ui, win); break;
	case PAGE_GAMING: page_gaming(ui, win); break;
	case PAGE_SCREENTIME: page_screentime(ui, win); break;
	case PAGE_SECURITY: page_security(ui, win); break;
	case PAGE_ABOUT: page_about(ui, win); break;
	}
	ui_pop_id(ui);
}

static const char *start_page;

static void init(Ui *ui)
{
	UNUSED(ui);
	fmt(st.conf_path, sizeof st.conf_path, "%s/.config/zurbax/settings.conf", home_dir());
	char *dir = path_dir(st.conf_path);
	dir_make(dir, 0755);
	mem_free(dir);
	st.conf = conf_load(st.conf_path);
	char *path = str_fmt("%s/system/users.conf", zurbax_data());
	Conf *u = conf_load(path);
	sb_adds(&st.full_name, conf_get(u, "full_name", "Zurbax User"));
	str_copy(st.user_name, conf_get(u, "user", "user"), sizeof st.user_name);
	st.has_password = conf_get_bool(u, "has_password", false);
	conf_free(u);
	mem_free(path);
	const char *w = get("wallpaper", WALLPAPER_DEFAULT);
	if (w[0] == '/')
		st.custom_thumb = image_load_thumb(w, 320);
	load_about();
	if (start_page && str_starts(start_page, "wifi:")) {
		/* from quick settings: a secured network to join */
		str_copy(wf.pick.hex, start_page + 5, sizeof wf.pick.hex);
		u8 raw[32];
		usize rl = strlen(wf.pick.hex) / 2;
		if (rl <= 32 && hex_decode(wf.pick.hex, raw, rl)) {
			usize w = 0;
			for (usize i = 0; i < rl && w + 1 < sizeof wf.pick.ssid; i++)
				wf.pick.ssid[w++] = raw[i] >= 0x20 && raw[i] != 0x7f ? (char)raw[i] : '?';
			wf.pick.ssid[w] = 0;
			wf.ask = true;   /* ids belong to the page, so the page opens it */
		}
		start_page = "wifi";
	}
	if (start_page)
		for (int i = 0; i < PAGE_COUNT; i++)
			if (str_eq_nocase(PAGES[i].title, start_page) || (str_eq(start_page, "users") && i == PAGE_USERS) ||
			    ((str_eq(start_page, "network") || str_eq(start_page, "wifi")) && i == PAGE_NETWORK) || (str_eq(start_page, "about") && i == PAGE_ABOUT) ||
			    ((str_eq(start_page, "security") || str_eq(start_page, "updates")) && i == PAGE_SECURITY))
				st.page = i;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		start_page = argv[1];
	UiAppDesc d = { .id = "settings", .title = "Settings", .width = 980, .height = 680, .min_width = 760, .min_height = 500,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
