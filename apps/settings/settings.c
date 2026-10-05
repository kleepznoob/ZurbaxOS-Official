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
	PAGE_KEYBOARD, PAGE_USERS, PAGE_DATETIME, PAGE_GAMING, PAGE_SCREENTIME, PAGE_ABOUT, PAGE_COUNT
};

static const struct {
	const char *title, *icon, *keywords, *group;
} PAGES[PAGE_COUNT] = {
	{ "Appearance", "palette", "dark light style accent colour color wallpaper background text size motion", "Personal" },
	{ "Desktop", "computer", "dock panel clock seconds battery lock autohide magnify", "Personal" },
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
	Canvas *thumbs[8];
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
	char cpu[96], gpu[96], board[128], kernel[96];
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

static void page_appearance(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	f32 y0 = ui_scroll_begin(ui, r, "page-scroll");
	b.y = y0 + 26;
	b.h = 100000; /* scrolling content: room to lay out everything */
	f32 top = b.y;
	page_title(ui, &b, "Appearance", "Make Zurbax yours: style, colour and wallpaper.");

	section(ui, &b, "Style");
	Rect styles = cut_top(&b, 150);
	bool dark = get_bool("dark", false);
	style_card(ui, rect(styles.x, styles.y, 210, styles.h), false, !dark);
	style_card(ui, rect(styles.x + 230, styles.y, 210, styles.h), true, dark);
	cut_top(&b, 12);

	section(ui, &b, "Accent Colour");
	Rect acc = cut_top(&b, 44);
	const char *cur = get("accent", "blue");
	for (int i = 0; i < THEME_ACCENT_COUNT; i++) {
		Rect sw = rect(acc.x + (f32)i * 52, acc.y, 44, 44);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "accent"), sw);
		ui_pop_id(ui);
		f32 cx = sw.x + 22, cy = sw.y + 22;
		bool sel = str_eq(cur, THEME_ACCENT_NAMES[i]);
		Color c = THEME_ACCENT_COLORS[i];
		ui_shadow(ui, rect(cx - 15, cy - 15, 30, 30), 15, 4, 1, color_hex("#00000030"));
		ui_circle(ui, cx, cy, it.hovered ? 16 : 15, c);
		ui_circle(ui, cx - 4, cy - 5, 6, color_hex("#ffffff30"));
		if (sel) {
			ui_ring(ui, cx, cy, 20, 2.5f, c);
			ui_icon(ui, rect(cx - 8, cy - 8, 16, 16), "check", COLOR_WHITE);
		}
		if (it.clicked)
			set_str(ui, "accent", THEME_ACCENT_NAMES[i]);
		ui_push_id(ui, i);
		if (it.hovered) {
			ui->last_rect = sw;
			ui_tooltip(ui, THEME_ACCENT_NAMES[i]);
		}
		ui_pop_id(ui);
	}
	cut_top(&b, 14);

	section(ui, &b, "Wallpaper");
	const char *wall = get("wallpaper", "lake");
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
	}
}

static void page_desktop(Ui *ui, Rect r)
{
	Rect b = page_body(r);
	page_title(ui, &b, "Desktop", "The dock, the top bar and locking.");
	section(ui, &b, "Dock");
	Rect rows = card_rows(ui, &b, 2, 60);
	switch_row(ui, &rows, 60, false, "Hide automatically", "Show the dock when the pointer reaches the bottom", "dock_autohide", false);
	switch_row(ui, &rows, 60, true, "Magnify icons", "Icons grow as the pointer passes over them", "dock_magnify", false);
	section(ui, &b, "Top Bar");
	rows = card_rows(ui, &b, 3, 56);
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

static void page_network(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	page_title(ui, &b, "Network", NULL);
	/* The network service publishes its state here. */
	char *status = file_read("/run/zurbax/network.status", NULL);
	Conf *c = NULL;
	if (status) {
		c = conf_load("/run/zurbax/network.status");
		mem_free(status);
	}
	section(ui, &b, "Wi-Fi");
	Rect rows = card_rows(ui, &b, 1, 64);
	Rect wr = cut_top(&rows, 64);
	bool wifi_on = get_bool("wifi", true);
	const char *ssid = c ? conf_get(c, "wifi_ssid", "") : "";
	ui_icon(ui, rect(wr.x, wr.y + 22, 20, 20), ssid[0] ? "network-wireless" : "network-wireless-none", t->text_dim);
	Rect lab = pad4(wr, 0, 0, 0, 32);
	row_label(ui, lab, "Wi-Fi", !c ? "The network service isn't running" : ssid[0] ? ssid : wifi_on ? "Not connected" : "Off");
	if (ui_switch(ui, center_in(cut_right(&wr, 52), 46, 26), &wifi_on))
		set_bool(ui, "wifi", wifi_on);
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

static void page_gaming(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	page_title(ui, &b, "Gaming", "Get the most out of this computer when you play.");
	Rect rows = card_rows(ui, &b, 3, 64);
	switch_row(ui, &rows, 64, false, "Game Mode", "Full speed for the processor and no notifications while a game is full screen",
		   "game_mode", true);
	switch_row(ui, &rows, 64, false, "Frame rate counter", "Shows frames per second in the corner of the screen", "show_fps", false);
	switch_row(ui, &rows, 64, true, "Mouse acceleration", "Turn off for precise aiming", "mouse_accel", true);
	section(ui, &b, "Windows Programs");
	Rect card = cut_top(&b, 92);
	ui_card(ui, card);
	Rect in = pad(card, 18);
	ui_icon(ui, rect(in.x, in.y + 4, 32, 32), "computer", t->text_dim);
	ui_text(ui, ui_font(ui, 14, 600), in.x + 48, in.y + 16, "Running .exe programs", t->text);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(in.x + 48, in.y + 24, in.w - 48, 40),
			"Zurbax can't run Windows programs yet. Games made for Zurbax are in Harbor and Arcade.", t->text_dim, ALIGN_START);
	cut_top(&b, 14);
	if (ui_button(ui, cut_left(&(Rect){ b.x, b.y, 200, 34 }, 200), "Browse Games in Harbor"))
		ui_launch(ui, "harbor", "games");
}

static void page_screentime(Ui *ui, Rect r)
{
	Rect b = page_body(r);
	page_title(ui, &b, "Screen Time", "See how long you use the computer and set healthy limits.");
	Rect rows = card_rows(ui, &b, 2, 64);
	switch_row(ui, &rows, 64, false, "Track screen time", "Counts time per app on this computer only", "screen_time", true);
	switch_row(ui, &rows, 64, true, "Break reminders", "A gentle reminder to rest your eyes every hour", "break_reminders", false);
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

static void page_about(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = page_body(r);
	Rect top = cut_top(&b, 200);
	ui_app_icon(ui, center_in(cut_top(&top, 110), 100, 100), "zurbax");
	ui_text_in(ui, ui_font(ui, 28, 300), cut_top(&top, 40), "Zurbax", t->text, ALIGN_CENTER);
	ui_text_in(ui, ui_font(ui, 13, 400), cut_top(&top, 20), "Version 1.0 · Built from scratch on the Linux kernel", t->text_dim, ALIGN_CENTER);
	Rect rows = card_rows(ui, &b, 6, 44);
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
	about_row(ui, &rows, "Kernel", st.kernel, true);
	Rect btns = cut_top(&b, 34);
	if (ui_button(ui, cut_left(&btns, 170), "Check for Updates"))
		ui_launch(ui, "harbor", "updates");
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
	if (sys_uname(&u) == 0)
		fmt(st.kernel, sizeof st.kernel, "Linux %s", u.release);
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
	const char *w = get("wallpaper", "lake");
	if (w[0] == '/')
		st.custom_thumb = image_load_thumb(w, 320);
	load_about();
	if (start_page)
		for (int i = 0; i < PAGE_COUNT; i++)
			if (str_eq_nocase(PAGES[i].title, start_page) || (str_eq(start_page, "users") && i == PAGE_USERS) ||
			    (str_eq(start_page, "network") && i == PAGE_NETWORK) || (str_eq(start_page, "about") && i == PAGE_ABOUT))
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
