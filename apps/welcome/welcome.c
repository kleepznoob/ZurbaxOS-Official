/*
 * Welcome — the first-boot assistant (and a short tour later on).
 *
 * Keyboard, Wi-Fi (when there's no cable), time zone, an account and a look,
 * one page each, over the blurred wallpaper. Every step can be skipped: without an account Zurbax
 * simply starts with a user that has no password. Booted from the install
 * USB stick or disc it offers to try Zurbax or install it.
 */
#include "zui.h"

enum { P_WELCOME, P_KEYBOARD, P_NETWORK, P_TIMEZONE, P_ACCOUNT, P_LOOK, P_DONE, P_COUNT };

static const char *const LAYOUT_IDS[] = { "us", "gb", "ro", "de", "fr", "es" };
static const char *const LAYOUT_TITLES[] = { "English (US)", "English (UK)", "Romanian", "German", "French", "Spanish" };

typedef struct {
	bool setup;           /* first boot, full screen */
	bool live;            /* running from the install stick or disc */
	int page;
	f64 changed_at;       /* when the page changed, for the slide */
	int dir;
	f64 now;
	Conf *conf;
	char conf_path[512];
	Canvas *backdrop;
	int backdrop_w, backdrop_h;
	StrBuf test, tz_search, name, pw, pw2;
	bool skip_password;
	u32 pending;
	char error[160];
	bool finishing;
	/* Wi-Fi */
	char pick[65];        /* the network whose password is being typed */
	StrBuf wifi_pass;
	f64 last_scan;
} Welcome;

static Welcome wl;

static void save(Ui *ui)
{
	conf_save(wl.conf);
	ui_settings_changed(ui);
}

/* The Wi-Fi page is for computers with Wi-Fi and no cable. */
static bool wifi_page_wanted(void)
{
	Conf *c = conf_load("/run/zurbax/network.status");
	bool want = conf_get_bool(c, "wifi_available", false) && !str_eq(conf_get(c, "kind", ""), "ethernet");
	conf_free(c);
	return want;
}

static void go(Ui *ui, int page)
{
	wl.dir = page > wl.page ? 1 : -1;
	if (page == P_NETWORK && !wifi_page_wanted())
		page += wl.dir;
	wl.page = CLAMP(page, 0, P_COUNT - 1);
	wl.changed_at = wl.now;
	ui->redraw = true;
	wl.error[0] = 0;
}

/* The wallpaper, blurred and dimmed, behind the assistant. */
static void draw_backdrop(Ui *ui)
{
	int w = (int)(ui->width * ui->scale), h = (int)(ui->height * ui->scale);
	if (!wl.backdrop || wl.backdrop_w != w || wl.backdrop_h != h) {
		canvas_free(wl.backdrop);
		int sw = MAX(1, w / 6), sh = MAX(1, h / 6);
		Canvas *small = wallpaper_render(conf_get(wl.conf, "wallpaper", WALLPAPER_DEFAULT), sw, sh);
		blur_region(small, rect(0, 0, (f32)sw, (f32)sh), 6);
		wl.backdrop = canvas_new(w, h);
		blit_scaled(wl.backdrop, small, rect(0, 0, (f32)sw, (f32)sh), rect(0, 0, (f32)w, (f32)h), 1);
		canvas_free(small);
		wl.backdrop_w = w;
		wl.backdrop_h = h;
	}
	ui_image(ui, rect(0, 0, ui->width, ui->height), wl.backdrop, 0, true);
	ui_fill(ui, rect(0, 0, ui->width, ui->height), color_hex("#00000038"));
}

/* Left side of the card: a large symbol, a title and a sentence. */
static void page_intro(Ui *ui, Rect r, const char *icon, bool app_icon, const char *title, const char *text)
{
	Theme *t = &ui->theme;
	Rect top = cut_top(&r, r.h * 0.5f);
	Rect ic = center_in(rect(top.x, top.y + top.h - 140, top.w, 128), 128, 128);
	if (app_icon) {
		ui_app_icon(ui, ic, icon);
	} else {
		ui_circle(ui, ic.x + 64, ic.y + 64, 60, color_with_alpha(t->accent, 0x22));
		ui_icon(ui, center_in(ic, 60, 60), icon, t->accent);
	}
	cut_top(&r, 6);
	ui_text_in(ui, ui_font(ui, 26, 300), cut_top(&r, 40), title, t->text, ALIGN_CENTER);
	cut_top(&r, 22); /* room for a line under the title (the release, on the first page) */
	ui_text_wrapped(ui, ui_font(ui, 14, 400), pad2(cut_top(&r, 80), 24, 0), text, t->text_dim, ALIGN_CENTER);
}

static void list_choice(Ui *ui, Rect *r, const char *const *titles, const char *const *ids, int n, const char *key, const char *fallback)
{
	const char *cur = conf_get(wl.conf, key, fallback);
	Rect card = cut_top(r, (f32)n * 44 + 8);
	ui_card(ui, card);
	Rect rows = pad2(card, 14, 4);
	for (int i = 0; i < n; i++) {
		Rect row = cut_top(&rows, 44);
		if (i < n - 1)
			ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
		ui_push_id(ui, i);
		if (ui_radio(ui, row, titles[i], str_eq(cur, ids[i]))) {
			conf_set(wl.conf, key, ids[i]);
			save(ui);
		}
		ui_pop_id(ui);
	}
}

static void form_welcome(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect c = center_in(r, r.w, 200);
	if (wl.live) {
		ui_text_in(ui, ui_font(ui, 15, 600), cut_top(&c, 28), "You're trying " OS_NAME " without installing it.", t->text, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 13, 400), cut_top(&c, 24), "Try it as long as you like, or install it on this computer.", t->text_dim, ALIGN_CENTER);
		cut_top(&c, 20);
		Rect b = center_in(cut_top(&c, 44), 380, 44);
		if (ui_button(ui, cut_left(&b, 180), "Try " OS_NAME))
			go(ui, P_KEYBOARD);
		cut_left(&b, 20);
		if (ui_button_suggested(ui, b, "Install " OS_NAME))
			ui_launch(ui, "installer", NULL);
	} else {
		ui_text_in(ui, ui_font(ui, 14, 400), cut_top(&c, 26), "A few choices and you're done. Every step can be skipped.", t->text_dim, ALIGN_CENTER);
		cut_top(&c, 26);
		if (ui_button_suggested(ui, center_in(cut_top(&c, 44), 200, 44), "Get Started"))
			go(ui, P_KEYBOARD);
	}
}

static void form_keyboard(Ui *ui, Rect r)
{
	ui_heading(ui, cut_top(&r, 28), "Keyboard Layout");
	cut_top(&r, 6);
	list_choice(ui, &r, LAYOUT_TITLES, LAYOUT_IDS, 6, "keyboard_layout", "us");
	cut_top(&r, 8);
	ui_entry(ui, cut_top(&r, 34), "test", &wl.test, "Type here to test your keyboard", ENTRY_AUTOFOCUS);
}

static void form_network(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Conf *c = conf_load("/run/zurbax/network.status");
	const char *state = conf_get(c, "wifi_state", "idle");
	bool radio = str_eq(conf_get(c, "wifi", "off"), "on");
	char line[160] = "";
	const char *icon = NULL;
	Color col = t->text_dim;
	if (!radio) {
		str_copy(line, "Wi-Fi is off.", sizeof line);
	} else if (str_eq(state, "connected")) {
		fmt(line, sizeof line, "Connected to %s", conf_get(c, "wifi_ssid", ""));
		icon = "check", col = t->success;
	} else if (str_eq(state, "connecting")) {
		fmt(line, sizeof line, "Connecting to %s…", conf_get(c, "wifi_ssid", ""));
	} else if (conf_get(c, "wifi_error", NULL)) {
		fmt(line, sizeof line, "Couldn't connect: %s", conf_get(c, "wifi_error", ""));
		icon = "error", col = t->danger;
	}
	conf_free(c);
	ui_heading(ui, cut_top(&r, 28), "Choose a Network");
	Rect st = cut_top(&r, 26);
	if (icon)
		ui_icon(ui, rect(st.x, st.y + 5, 16, 16), icon, col);
	ui_text_in(ui, ui_font(ui, 13, 500), rect(st.x + (icon ? 24 : 0), st.y, st.w, st.h), line, col, ALIGN_START);
	cut_top(&r, 6);
	if (!radio) {
		if (ui_button(ui, rect(r.x, r.y, 160, 36), "Turn Wi-Fi On"))
			wifi_command("radio on");
		return;
	}
	if (wl.now - wl.last_scan > 10) {
		wl.last_scan = wl.now;
		wifi_command("scan");
	}
	WifiNet nets[24];
	int n = wifi_networks(nets, 24);
	Rect list = r;
	ui_card(ui, list);
	if (!n) {
		ui_spinner(ui, center_in(list, 24, 24));
		ui->wake_at = ui->now + 0.5;
		return;
	}
	f32 y = ui_scroll_begin(ui, pad(list, 4), "nets");
	for (int i = 0; i < n; i++) {
		WifiNet *w = &nets[i];
		bool ok = wifi_supported(w);
		bool picked = str_eq(wl.pick, w->hex) && !w->connected;
		f32 h = picked ? 96 : 44;
		Rect row = rect(list.x + 4, y, list.w - 8, h);
		y += h;
		ui_push_id_str(ui, w->hex);
		UiInteract in = ui_row(ui, rect(row.x, row.y, row.w, 44), ui_id(ui, "n"), w->connected || picked);
		Color fg = ok ? t->text : t->text_faint;
		ui_icon(ui, rect(row.x + 12, row.y + 12, 20, 20), wifi_signal_icon(w->signal), fg);
		ui_text_in(ui, ui_font(ui, 13, w->connected ? 700 : 500), rect(row.x + 44, row.y, row.w - 90, 44), w->ssid, fg, ALIGN_START);
		if (!str_eq(w->security, "open"))
			ui_icon(ui, rect(row.x + row.w - 30, row.y + 15, 14, 14), "lock", t->text_faint);
		if (in.clicked && ok && !w->connected) {
			if (w->saved || str_eq(w->security, "open")) {
				wifi_connect(w->hex, NULL);
				wl.pick[0] = 0;
			} else {
				str_copy(wl.pick, w->hex, sizeof wl.pick);
				sb_clear(&wl.wifi_pass);
			}
		}
		if (picked) {
			Rect pr = rect(row.x + 12, row.y + 50, row.w - 24 - 90, 36);
			int ev = ui_entry(ui, pr, "wifi-pass", &wl.wifi_pass, "Password", ENTRY_PASSWORD | ENTRY_AUTOFOCUS);
			bool good = wl.wifi_pass.len >= 8;
			if ((ui_button_suggested(ui, rect(pr.x + pr.w + 8, pr.y, 82, 36), "Join") || (ev & ENTRY_SUBMIT)) && good) {
				wifi_connect(w->hex, sb_str(&wl.wifi_pass));
				memset(wl.wifi_pass.data, 0, wl.wifi_pass.len);
				sb_clear(&wl.wifi_pass);
				wl.pick[0] = 0;
			}
		}
		ui_pop_id(ui);
	}
	ui_scroll_end(ui, y - list.y);
	ui->wake_at = ui->now + 1;   /* the service's state changes on its own */
}

static void form_timezone(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_heading(ui, cut_top(&r, 28), "Where Are You?");
	cut_top(&r, 6);
	ui_entry(ui, cut_top(&r, 34), "tz-search", &wl.tz_search, "Search for your city", ENTRY_SEARCH | ENTRY_AUTOFOCUS);
	cut_top(&r, 8);
	Rect list = r;
	ui_card(ui, list);
	usize nz;
	const ZoneInfo *zones = tz_zones(&nz);
	const char *q = sb_str(&wl.tz_search);
	const char *cur = conf_get(wl.conf, "timezone", "UTC");
	f32 y = ui_scroll_begin(ui, pad(list, 4), "tz");
	for (usize i = 0; i < nz; i++) {
		const ZoneInfo *z = &zones[i];
		if (*q && !str_find_nocase(z->city, q) && !str_find_nocase(z->country, q))
			continue;
		Rect row = rect(list.x + 4, y, list.w - 8, 38);
		y += 38;
		ui_push_id(ui, (i64)i);
		UiInteract it = ui_row(ui, row, ui_id(ui, "z"), str_eq(cur, z->zone));
		ui_pop_id(ui);
		char name[128];
		fmt(name, sizeof name, "%s%s%s", z->city, z->country[0] ? ", " : "", z->country);
		ui_text(ui, ui_font(ui, 13, 500), row.x + 12, row.y + 24, name, t->text);
		/* The local time there, to make the choice obvious. */
		Civil c;
		civil_from_unix(time_unix() + tz_offset_in(z->zone, time_unix()), &c);
		char tm[16];
		fmt(tm, sizeof tm, "%02d:%02d", c.hour, c.min);
		Font tf = ui_font(ui, 12, 500);
		ui_text(ui, tf, row.x + row.w - 12 - ui_text_width(ui, tf, tm), row.y + 24, tm, t->text_dim);
		if (it.clicked) {
			conf_set(wl.conf, "timezone", z->zone);
			tz_set(z->zone);
			save(ui);
		}
	}
	ui_scroll_end(ui, y - list.y);
}

static void form_account(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_heading(ui, cut_top(&r, 28), "Your Name");
	cut_top(&r, 6);
	ui_entry(ui, cut_top(&r, 36), "name", &wl.name, "First and last name", ENTRY_AUTOFOCUS);
	cut_top(&r, 18);
	ui_heading(ui, cut_top(&r, 28), "Password");
	cut_top(&r, 6);
	if (!wl.skip_password) {
		ui_entry(ui, cut_top(&r, 36), "pw", &wl.pw, "Password", ENTRY_PASSWORD);
		cut_top(&r, 8);
		int ev = ui_entry(ui, cut_top(&r, 36), "pw2", &wl.pw2, "Confirm password", ENTRY_PASSWORD);
		UNUSED(ev);
		cut_top(&r, 8);
		if (wl.pw2.len && !str_eq(sb_str(&wl.pw), sb_str(&wl.pw2)))
			ui_text(ui, ui_font(ui, 12, 500), r.x, r.y + 14, "The passwords don't match", t->danger);
		cut_top(&r, 20);
	}
	ui_checkbox(ui, cut_top(&r, 28), "Use this computer without a password", &wl.skip_password);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), cut_top(&r, 40), "You can add a password, or change it, any time in Settings.", t->text_faint,
			ALIGN_START);
	if (wl.error[0])
		ui_text(ui, ui_font(ui, 13, 500), r.x, r.y + 18, wl.error, t->danger);
}

static void style_choice(Ui *ui, Rect r, bool dark, bool selected)
{
	Theme *t = &ui->theme;
	UiInteract it = ui_interact(ui, ui_id(ui, dark ? "dark" : "light"), r);
	Rect pic = cut_top(&r, r.h - 26);
	ui_round(ui, pic, 10, dark ? color_hex("#1e1f23") : color_hex("#e9eaed"));
	Rect w = pad4(pic, 14, 16, 0, 16);
	ui_round4(ui, w, 6, 6, 0, 0, dark ? color_hex("#2d2e33") : color_hex("#fafafa"));
	ui_round4(ui, cut_top(&w, 16), 6, 6, 0, 0, dark ? color_hex("#38393e") : color_hex("#ececee"));
	ui_round(ui, rect(w.x + 10, w.y + 10, w.w * 0.5f, 6), 3, dark ? color_hex("#5a5b60") : color_hex("#c8c9cd"));
	ui_round(ui, rect(w.x + w.w - 46, w.y + w.h - 24, 36, 14), 4, t->accent);
	if (selected)
		ui_border(ui, rect_inset(pic, -3, -3), 12, 2.5f, t->accent);
	else if (it.hovered)
		ui_border(ui, rect_inset(pic, -2, -2), 11, 1.5f, t->border_strong);
	ui_text_in(ui, ui_font(ui, 13, selected ? 700 : 500), r, dark ? "Dark" : "Light", t->text, ALIGN_CENTER);
	if (it.clicked && !selected) {
		conf_set_bool(wl.conf, "dark", dark);
		save(ui);
	}
}

static void form_look(Ui *ui, Rect r)
{
	ui_heading(ui, cut_top(&r, 28), "Style");
	cut_top(&r, 6);
	Rect s = cut_top(&r, 130);
	bool dark = conf_get_bool(wl.conf, "dark", false);
	f32 cw = (s.w - 16) / 2;
	style_choice(ui, rect(s.x, s.y, cw, s.h), false, !dark);
	style_choice(ui, rect(s.x + cw + 16, s.y, cw, s.h), true, dark);
	cut_top(&r, 16);
	ui_heading(ui, cut_top(&r, 28), "Accent Colour");
	cut_top(&r, 6);
	Rect row = cut_top(&r, 40);
	const char *cur = conf_get(wl.conf, "accent", THEME_ACCENT_DEFAULT);
	for (int i = 0; i < THEME_ACCENT_COUNT; i++) {
		Rect sw = rect(row.x + (f32)i * 40, row.y, 36, 36);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "accent"), sw);
		ui_pop_id(ui);
		f32 cx = sw.x + 18, cy = sw.y + 18;
		ui_circle(ui, cx, cy, it.hovered ? 14 : 13, THEME_ACCENT_COLORS[i]);
		if (str_eq(cur, THEME_ACCENT_NAMES[i])) {
			ui_ring(ui, cx, cy, 17, 2, THEME_ACCENT_COLORS[i]);
			ui_icon(ui, rect(cx - 7, cy - 7, 14, 14), "check", COLOR_WHITE);
		}
		if (it.clicked) {
			conf_set(wl.conf, "accent", THEME_ACCENT_NAMES[i]);
			save(ui);
		}
	}
}

static void finish(Ui *ui)
{
	if (wl.finishing)
		return;
	wl.finishing = true;
	/* The account: a name (or a friendly default) and maybe a password. */
	u32 flags = ACCOUNT_SET_NAME | ACCOUNT_FINISH_SETUP;
	bool pw = !wl.skip_password && wl.pw.len && str_eq(sb_str(&wl.pw), sb_str(&wl.pw2));
	if (pw)
		flags |= ACCOUNT_SET_PASSWORD;
	const char *name = wl.name.len ? sb_str(&wl.name) : "Zurbax User";
	wl.pending = ui_account_update(ui, flags, name, NULL, pw ? sb_str(&wl.pw) : NULL);
	if (!wl.pending)
		ui->quit = true; /* not connected (preview): nothing to wait for */
}

static bool account_ready(void)
{
	if (wl.skip_password || (!wl.pw.len && !wl.pw2.len))
		return true;
	return str_eq(sb_str(&wl.pw), sb_str(&wl.pw2));
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	if (wl.pending && ui->reply_seq == wl.pending) {
		wl.pending = 0;
		if (ui->reply_status == 0)
			ui->quit = true;
		else {
			wl.finishing = false;
			str_copy(wl.error, ui->reply_message, sizeof wl.error);
		}
	}
	wl.now = ui->now;
	draw_backdrop(ui);
	f32 slide = clampf((f32)((ui->now - wl.changed_at) / 0.28), 0, 1);
	slide = ease_out_cubic(slide);
	if (slide < 1)
		ui->redraw = true;
	/* The card. */
	f32 cw = minf(900, ui->width - 60), ch = minf(560, ui->height - 60);
	Rect card = center_in(rect(0, 0, ui->width, ui->height), cw, ch);
	ui_shadow(ui, card, 14, 40, 14, color_hex("#00000070"));
	ui_round(ui, card, 14, t->window);
	ui_border(ui, card, 14, 1, color_hex("#00000030"));
	Rect body = pad(card, 30);
	Rect bottom = cut_bottom(&body, 40);
	cut_bottom(&body, 16);
	/* Pages slide in from the side they come from. */
	f32 off = (1 - slide) * 40 * (f32)wl.dir;
	ui_push_clip(ui, card);
	Rect left = cut_left(&body, body.w * 0.46f);
	left.x += off;
	cut_left(&body, 30);
	Rect right = body;
	right.x += off;
	switch (wl.page) {
	case P_WELCOME:
		page_intro(ui, left, "zurbax", true, "Welcome to " OS_NAME, "Let's get your computer ready. It takes about a minute.");
		/* the release, under the greeting */
		ui_text_in(ui, ui_font(ui, 13, 700), rect(left.x, left.y + left.h * 0.5f + 46, left.w, 18), "Version " OS_VERSION,
			   t->accent, ALIGN_CENTER);
		form_welcome(ui, right);
		break;
	case P_KEYBOARD:
		page_intro(ui, left, "keyboard", false, "Keyboard", "Choose the layout printed on your keys. AltGr types ș ț ă î â on any of them.");
		form_keyboard(ui, right);
		break;
	case P_NETWORK:
		page_intro(ui, left, "network-wireless", false, "Wi-Fi", "Connect to the internet for the weather, mail, the web and new apps. You can do this later too.");
		form_network(ui, right);
		break;
	case P_TIMEZONE:
		page_intro(ui, left, "location", false, "Time Zone", "Pick your city so the clock and calendar are right.");
		form_timezone(ui, right);
		break;
	case P_ACCOUNT:
		page_intro(ui, left, "user", false, "Your Account", "Add your name and, if you like, a password to keep your files private.");
		form_account(ui, right);
		break;
	case P_LOOK:
		page_intro(ui, left, "palette", false, "Make It Yours", "Light or dark, and a colour for buttons and highlights. Change them later in Settings.");
		form_look(ui, right);
		break;
	case P_DONE:
		page_intro(ui, left, "check", false, "You're All Set", OS_NAME " is ready. Have fun!");
		{
			Rect c = center_in(right, right.w, 120);
			ui_text_wrapped(ui, ui_font(ui, 14, 400), cut_top(&c, 50),
					"Start, at the left of the taskbar, has every app. The top bar has the clock and quick settings.", t->text_dim,
					ALIGN_CENTER);
			cut_top(&c, 16);
			if (ui_button_suggested(ui, center_in(cut_top(&c, 44), 220, 44), wl.finishing ? "Starting…" : "Start Using " OS_NAME) ||
			    ui_key_pressed(ui, KEY_ENTER, 0))
				finish(ui);
		}
		break;
	}
	ui_pop_clip(ui);
	/* Progress dots and the buttons. */
	f32 dots_w = (f32)P_COUNT * 16;
	for (int i = 0; i < P_COUNT; i++) {
		f32 x = bottom.x + (bottom.w - dots_w) / 2 + (f32)i * 16 + 8, y = bottom.y + bottom.h / 2;
		ui_circle(ui, x, y, i == wl.page ? 4 : 3, i == wl.page ? t->accent : t->border_strong);
	}
	if (wl.page > P_WELCOME && wl.page < P_DONE) {
		if (ui_button(ui, cut_left(&bottom, 100), "Back"))
			go(ui, wl.page - 1);
		bool can = wl.page != P_ACCOUNT || account_ready();
		if (ui_button_suggested(ui, cut_right(&bottom, 110), "Next") && can)
			go(ui, wl.page + 1);
		cut_right(&bottom, 8);
		if (ui_button_flat(ui, cut_right(&bottom, 90), "Skip")) {
			if (wl.page == P_ACCOUNT) {
				sb_clear(&wl.pw);
				sb_clear(&wl.pw2);
				wl.skip_password = true;
			}
			go(ui, wl.page + 1);
		}
		if (ui_key_pressed(ui, KEY_ENTER, 0) && can && !ui_is_focused(ui, "test") && !ui_is_focused(ui, "wifi-pass"))
			go(ui, wl.page + 1);
	}
	if (wl.setup && wl.page == P_WELCOME) {
		/* Skip everything: straight to the desktop with defaults. */
		if (ui_button_flat(ui, cut_right(&bottom, 160), "Skip Setup")) {
			wl.skip_password = true;
			finish(ui);
		}
	}
}

static bool on_close(Ui *ui)
{
	/* Closing the assistant during setup still finishes it with what's chosen. */
	if (wl.setup && !wl.finishing) {
		finish(ui);
		return false;
	}
	return true;
}

static void init(Ui *ui)
{
	UNUSED(ui);
	fmt(wl.conf_path, sizeof wl.conf_path, "%s/.config/zurbax/settings.conf", home_dir());
	char *dir = path_dir(wl.conf_path);
	dir_make(dir, 0755);
	mem_free(dir);
	wl.conf = conf_load(wl.conf_path);
	wl.live = file_exists("/run/zurbax-live");
	if (!conf_get(wl.conf, "timezone", NULL))
		conf_set(wl.conf, "timezone", "UTC");
}

int main(int argc, char **argv)
{
	wl.setup = argc > 1 && str_eq(argv[1], "--setup");
	UiAppDesc d = { .id = "welcome", .title = "Welcome", .width = 960, .height = 640, .min_width = 760, .min_height = 560,
			.flags = WIN_RESIZABLE | (wl.setup ? WIN_FULLSCREEN_START : 0), .init = init, .frame = frame, .close = on_close };
	return ui_run(&d);
}
