/*
 * Welcome — the first-start assistant (and a short tour later on).
 *
 * The steps run down the side over the theme's pixel picture (setup.c): a
 * welcome under the name in big pixels,
 * the keyboard (with a keyboard that lights up as you type), Wi-Fi when
 * there's no cable, the time zone (with a clock that swings to the city's
 * time), an account (with its picture and how strong the password is), a
 * look (Carbon or Midnight, which the assistant itself wears as soon as
 * it's picked, an accent and a wallpaper) and a summary.
 * Every step can be skipped: without an account Zurbax simply starts with
 * a user that has no password. Booted from the install USB stick or disc it
 * offers to try Zurbax or install it.
 */
#include "zui.h"

enum { P_WELCOME, P_KEYBOARD, P_NETWORK, P_TIMEZONE, P_ACCOUNT, P_LOOK, P_DONE, P_COUNT };
static const char *const STEP_NAMES[P_COUNT] = { "Welcome", "Keyboard", "Wi-Fi", "Time Zone", "Account", "Look", "Ready" };

static const char *const LAYOUT_IDS[] = { "us", "gb", "ro", "de", "fr", "es" };
static const char *const LAYOUT_TITLES[] = { "English (US)", "English (UK)", "Romanian", "German", "French", "Spanish" };
/* the letter rows each layout prints on its keys */
static const char *const LAYOUT_ROWS[][3] = {
	{ "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" }, { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" }, { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" },
	{ "QWERTZUIOP", "ASDFGHJKL", "YXCVBNM" }, { "AZERTYUIOP", "QSDFGHJKLM", "WXCVBN" },   { "QWERTYUIOP", "ASDFGHJKL~", "ZXCVBNM" },
};

typedef struct {
	bool setup;           /* first start, full screen */
	bool live;            /* running from the install stick or disc */
	int page;
	f64 changed_at;       /* when the page changed, for its entrance */
	int dir;
	f64 now;
	Conf *conf;
	char conf_path[512];
	StrBuf test, tz_search, name, pw, pw2;
	bool skip_password;
	u32 pending;
	char error[160];
	bool finishing;
	f64 done_at;
	/* the keyboard picture */
	char last_key;
	f64 key_at;
	usize test_len;
	f64 layout_at;
	/* Wi-Fi */
	char pick[65];        /* the network whose password is being typed */
	StrBuf wifi_pass;
	f64 last_scan;
	/* the account picture */
	char initials[8];
	f64 initials_at;
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
	if (wl.page == P_DONE)
		wl.done_at = wl.now;
	ui->redraw = true;
	wl.error[0] = 0;
}

static int layout_index(void)
{
	const char *cur = conf_get(wl.conf, "keyboard_layout", "us");
	for (int i = 0; i < (int)ARRAY_LEN(LAYOUT_IDS); i++)
		if (str_eq(cur, LAYOUT_IDS[i]))
			return i;
	return 0;
}

static void list_choice(Ui *ui, Rect *r, const char *const *titles, const char *const *ids, int n, const char *key, const char *fallback)
{
	const char *cur = conf_get(wl.conf, key, fallback);
	Rect card = cut_top(r, (f32)n * 42 + 8);
	setup_card(ui, card, false, 0);
	Rect rows = pad2(card, 14, 4);
	for (int i = 0; i < n; i++) {
		Rect row = cut_top(&rows, 42);
		if (i < n - 1)
			setup_rule(ui, row.x, row.y + row.h, row.x + row.w);
		ui_push_id(ui, i);
		if (ui_radio(ui, row, titles[i], str_eq(cur, ids[i])) && !str_eq(cur, ids[i])) {
			conf_set(wl.conf, key, ids[i]);
			save(ui);
			wl.layout_at = wl.now;
		}
		ui_pop_id(ui);
	}
}

/* ---- Welcome -------------------------------------------------------------------------------- */

static void feature(Ui *ui, Rect r, const char *icon, const char *title, const char *text)
{
	Theme *t = &ui->theme;
	setup_card(ui, r, false, 0);
	Rect c = pad(r, 18);
	Rect ic = cut_top(&c, 40);
	setup_block(ui, rect(ic.x, ic.y, 40, 40), 2, color_with_alpha(t->accent, 0x26));
	ui_icon(ui, rect(ic.x + 10, ic.y + 10, 20, 20), icon, t->accent);
	cut_top(&c, 14);
	ui_text_in(ui, ui_font(ui, 15, 700), cut_top(&c, 22), title, t->text, ALIGN_START);
	cut_top(&c, 2);
	ui_text_wrapped(ui, ui_font(ui, 13, 400), c, text, t->text_dim, ALIGN_START);
}

static void page_welcome(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	/* the name in big pixels, built block by block, like on zurbax.com */
	f32 cell = clampf(floorf(minf(r.w, 620) / setup_wordmark_width(1)), 3, 8);
	setup_wordmark(ui, r.x, r.y + 2, cell, wl.changed_at);
	cut_top(&r, 14 * cell + 22);
	const char *about = wl.live ? "You're running " OS_NAME " from the USB stick. Nothing on this computer has changed: try it for as long as you like, "
				      "or install it when you're ready."
				    : "Let's get your computer ready. It takes about a minute, and every step can be skipped and changed later in Settings.";
	setup_title(ui, &r, wl.changed_at, wl.live ? "Take it for a spin." : "Hello. Let's make it yours.", about);
	if (wl.live) {
		Rect row = cut_top(&r, 168);
		f32 cw = (row.w - 18) / 2;
		if (setup_choice(ui, setup_rise(ui, wl.changed_at, 0, rect(row.x, row.y, cw, row.h)), "try", "computer", "Try " OS_NAME,
				 "Keep using it from the stick. Your files and apps stay on the stick until you restart.", false, true, t->accent))
			go(ui, P_KEYBOARD);
		if (setup_choice(ui, setup_rise(ui, wl.changed_at, 1, rect(row.x + cw + 18, row.y, cw, row.h)), "install", "download",
				 "Install " OS_NAME, "Put it on this computer, on its own or next to Windows. About five minutes, with Steam and the apps included.",
				 false, true, t->success))
			ui_launch(ui, "installer", NULL);
		cut_top(&r, 22);
	}
	if (r.h >= 172) {
		Rect row = cut_top(&r, 172);
		f32 fw = (row.w - 32) / 3;
		feature(ui, setup_rise(ui, wl.changed_at, 2, rect(row.x, row.y, fw, row.h)), "gamepad", "Built to play",
			"Steam with Proton, Bridge for any Windows game or app, and any controller, rumble included.");
		feature(ui, setup_rise(ui, wl.changed_at, 3, rect(row.x + fw + 16, row.y, fw, row.h)), "bag", "Thousands of apps",
			"Harbor brings all of Flathub. Firefox, LibreOffice and six more are here already.");
		feature(ui, setup_rise(ui, wl.changed_at, 4, rect(row.x + (fw + 16) * 2, row.y, fw, row.h)), "palette", "Yours to shape",
			"Carbon or Midnight, tiles that fall into place and pictures in pixels that move.");
	}
}

/* ---- Keyboard -------------------------------------------------------------------------------- */

/* The layout's letters drawn as keys in blocks, their letters in pixels;
 * the one just typed lights up and goes down. */
static void keyboard_picture(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	int li = layout_index();
	setup_block(ui, r, 3, t->dark ? color_hex("#0e0d10c0") : color_hex("#dcdde1"));
	Rect k = pad(r, 14);
	f32 kw = (k.w - 9 * 6) / 10, kh = minf(40, (k.h - 3 * 6) / 4);
	f32 lit = clampf(1 - (f32)(ui->now - wl.key_at) / 0.7f, 0, 1);
	if (lit > 0)
		ui->redraw = true;
	f32 swap = setup_ease((f32)((ui->now - wl.layout_at) / 0.5));
	for (int row = 0; row < 4; row++) {
		const char *keys = row < 3 ? LAYOUT_ROWS[li][row] : " ";
		int n = (int)strlen(keys);
		f32 rw = row < 3 ? (f32)n * kw + (f32)(n - 1) * 6 : kw * 5 + 24;
		f32 x = k.x + (k.w - rw) / 2, y = k.y + (f32)row * (kh + 6);
		for (int i = 0; i < n; i++) {
			f32 w = row < 3 ? kw : rw;
			/* after a layout change the keys settle back one after another */
			f32 drop = clampf(swap * 1.6f - (f32)i * 0.06f, 0, 1);
			Rect key = rect(x + (f32)i * (kw + 6), y - (1 - drop) * 6, w, kh);
			char c = keys[i] == '~' ? 'N' : keys[i];
			bool hit = lit > 0 && to_upper((u8)wl.last_key) == (u8)c;
			Color face = t->dark ? color_hex("#2a292f") : COLOR_WHITE;
			if (hit)
				face = color_mix(face, t->accent, lit), key.y += 2;
			setup_block(ui, rect(key.x, key.y + 4, key.w, key.h), 1, t->dark ? color_hex("#060507") : color_hex("#b9bac0"));
			setup_block(ui, key, 1, face);
			setup_block(ui, rect(key.x + 2, key.y, key.w - 4, 2), 0, color_with_alpha(COLOR_WHITE, hit ? 0x60 : 0x1c));
			char label[2] = { c, 0 };
			if (row < 3)
				ui_pixel_text(ui, key.x + (key.w - ui_pixel_text_width(label, 2)) / 2, key.y + (key.h - 14) / 2, label, 2,
					      hit ? COLOR_WHITE : t->text);
		}
	}
}

static void page_keyboard(Ui *ui, Rect r)
{
	setup_title(ui, &r, wl.changed_at, "Keyboard",
		    "Choose the layout printed on your keys, then type a few letters to check them on the keyboard below. AltGr types ș ț ă î â "
		    "on every layout.");
	bool wide = r.w > 640;
	Rect list = wide ? cut_left(&r, r.w * 0.42f) : cut_top(&r, 6 * 42 + 8);
	if (wide)
		cut_left(&r, 24);
	else
		cut_top(&r, 16);
	list_choice(ui, &list, LAYOUT_TITLES, LAYOUT_IDS, 6, "keyboard_layout", "us");
	Rect pic = setup_rise(ui, wl.changed_at, 1, cut_top(&r, minf(200, r.h - 50)));
	keyboard_picture(ui, pic);
	cut_top(&r, 12);
	ui_entry(ui, cut_top(&r, 36), "test", &wl.test, "Type here to test your keyboard", ENTRY_AUTOFOCUS);
	if (wl.test.len > wl.test_len) {
		wl.last_key = wl.test.data[wl.test.len - 1];
		/* the Ñ arrives as two bytes */
		if ((u8)wl.last_key == 0x91 || (u8)wl.last_key == 0xb1)
			wl.last_key = 'N';
		wl.key_at = ui->now;
	}
	wl.test_len = wl.test.len;
}

/* ---- Wi-Fi ----------------------------------------------------------------------------------- */

static void page_network(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	setup_title(ui, &r, wl.changed_at, "Wi-Fi",
		    "Connect for the weather, mail, the web and new apps from Harbor. You can also do it later, from the top bar.");
	Conf *c = conf_load("/run/zurbax/network.status");
	const char *state = conf_get(c, "wifi_state", "idle");
	bool radio = str_eq(conf_get(c, "wifi", "off"), "on");
	char line[160] = "";
	const char *icon = "network-wireless";
	Color col = t->text_dim;
	if (!radio) {
		str_copy(line, "Wi-Fi is off.", sizeof line);
		icon = "network-wireless-off";
	} else if (str_eq(state, "connected")) {
		fmt(line, sizeof line, "Connected to %s", conf_get(c, "wifi_ssid", ""));
		icon = "check", col = t->success;
	} else if (str_eq(state, "connecting")) {
		fmt(line, sizeof line, "Connecting to %s…", conf_get(c, "wifi_ssid", ""));
	} else if (conf_get(c, "wifi_error", NULL)) {
		fmt(line, sizeof line, "Couldn't connect: %s", conf_get(c, "wifi_error", ""));
		icon = "error", col = t->danger;
	} else {
		str_copy(line, "Choose a network to join.", sizeof line);
	}
	conf_free(c);
	Rect st = cut_top(&r, 50);
	setup_block(ui, st, 2, color_with_alpha(col, 0x1c));
	ui_icon(ui, rect(st.x + 16, st.y + 15, 20, 20), icon, col);
	ui_text_in(ui, ui_font(ui, 14, 600), rect(st.x + 48, st.y, st.w - 60, st.h), line, col, ALIGN_START);
	cut_top(&r, 12);
	if (!radio) {
		if (setup_button(ui, rect(r.x, r.y, 160, 38), "Turn Wi-Fi On", SETUP_BUTTON))
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
	setup_card(ui, list, false, 0);
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
		f32 h = picked ? 98 : 46;
		Rect row = rect(list.x + 4, y, list.w - 8, h);
		y += h;
		ui_push_id_str(ui, w->hex);
		UiInteract in = ui_row(ui, rect(row.x, row.y, row.w, 46), ui_id(ui, "n"), w->connected || picked);
		Color fg = ok ? t->text : t->text_faint;
		ui_icon(ui, rect(row.x + 12, row.y + 13, 20, 20), wifi_signal_icon(w->signal), fg);
		ui_text_in(ui, ui_font(ui, 14, w->connected ? 700 : 500), rect(row.x + 44, row.y, row.w - 90, 46), w->ssid, fg, ALIGN_START);
		if (!str_eq(w->security, "open"))
			ui_icon(ui, rect(row.x + row.w - 30, row.y + 16, 14, 14), "lock", t->text_faint);
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
			Rect pr = rect(row.x + 12, row.y + 52, row.w - 24 - 90, 36);
			int ev = ui_entry(ui, pr, "wifi-pass", &wl.wifi_pass, "Password", ENTRY_PASSWORD | ENTRY_AUTOFOCUS);
			bool good = wl.wifi_pass.len >= 8;
			if ((setup_button(ui, rect(pr.x + pr.w + 8, pr.y, 82, 36), "Join", SETUP_BUTTON_GO) || (ev & ENTRY_SUBMIT)) && good) {
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

/* ---- Time zone ------------------------------------------------------------------------------- */

/* A clock for the chosen city. When the city changes the hands swing round
 * to its time. */
static void city_clock(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	const char *zone = conf_get(wl.conf, "timezone", "UTC");
	i64 now = time_unix();
	f32 off = ui_anim(ui, ui_id(ui, "tz-offset"), (f32)tz_offset_in(zone, now) / 60.0f, 5);
	f64 local = (f64)now + (f64)off * 60 + fmod(ui->now, 1.0) * 0; /* seconds, with the offset as it swings */
	setup_card(ui, r, false, 0);
	Rect c = pad(r, 20);
	f32 d = minf(c.w, c.h - 70);
	f32 cx = c.x + c.w / 2, cy = c.y + d / 2, rad = d / 2;
	ui_circle(ui, cx, cy, rad, t->dark ? color_hex("#1b1a1f") : color_hex("#f6f6f8"));
	ui_ring(ui, cx, cy, rad, 1.5f, t->border_strong);
	for (int i = 0; i < 60; i++) {
		f32 a = (f32)i / 60 * 6.2831853f;
		f32 in = i % 5 ? rad - 6 : rad - 12;
		ui_line(ui, cx + sinf(a) * in, cy - cosf(a) * in, cx + sinf(a) * (rad - 3), cy - cosf(a) * (rad - 3), i % 5 ? 1 : 2.5f,
			i % 5 ? t->text_faint : t->text);
	}
	f64 day = fmod(local, 86400.0);
	if (day < 0)
		day += 86400;
	f32 h = (f32)(day / 43200.0) * 6.2831853f, m = (f32)(fmod(day, 3600.0) / 3600.0) * 6.2831853f;
	f32 s = (f32)fmod(day, 60.0) / 60 * 6.2831853f;
	ui_line(ui, cx, cy, cx + sinf(h) * rad * 0.5f, cy - cosf(h) * rad * 0.5f, 5, t->text);
	ui_line(ui, cx, cy, cx + sinf(m) * rad * 0.75f, cy - cosf(m) * rad * 0.75f, 3.5f, t->text);
	ui_line(ui, cx - sinf(s) * rad * 0.15f, cy + cosf(s) * rad * 0.15f, cx + sinf(s) * rad * 0.85f, cy - cosf(s) * rad * 0.85f, 1.5f,
		t->accent);
	ui_circle(ui, cx, cy, 5, t->accent);
	const ZoneInfo *z = tz_find(zone);
	char city[96], when[64];
	fmt(city, sizeof city, "%s", str_eq(zone, "UTC") || !z ? zone : z->city);
	Civil cv;
	civil_from_unix(now + tz_offset_in(zone, now), &cv);
	i32 o = tz_offset_in(zone, now) / 60;
	fmt(when, sizeof when, "%02d:%02d · UTC%s%d%s", cv.hour, cv.min, o < 0 ? "−" : "+", ABS(o) / 60,
	    ABS(o) % 60 ? (ABS(o) % 60 == 30 ? ":30" : ":45") : "");
	ui_text_in(ui, ui_font(ui, 18, 700), rect(c.x, c.y + d + 14, c.w, 26), city, t->text, ALIGN_CENTER);
	ui_text_in(ui, ui_font(ui, 13, 500), rect(c.x, c.y + d + 40, c.w, 20), when, t->text_dim, ALIGN_CENTER);
	ui->wake_at = ui->now + (fabsf(off * 60 - (f32)tz_offset_in(zone, now)) > 1 ? 0.016 : 0.25);
}

static void page_timezone(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	setup_title(ui, &r, wl.changed_at, "Time Zone", "Pick your city so the clock, the calendar and your reminders are right. Search by city or country.");
	bool wide = r.w > 640;
	if (wide) {
		Rect clock = cut_right(&r, minf(260, r.w * 0.38f));
		city_clock(ui, setup_rise(ui, wl.changed_at, 1, rect(clock.x, clock.y, clock.w, minf(clock.h, clock.w + 70))));
		cut_right(&r, 24);
	}
	ui_entry(ui, cut_top(&r, 36), "tz-search", &wl.tz_search, "Search for your city", ENTRY_SEARCH | ENTRY_AUTOFOCUS);
	cut_top(&r, 10);
	Rect list = r;
	setup_card(ui, list, false, 0);
	usize nz;
	const ZoneInfo *zones = tz_zones(&nz);
	const char *q = sb_str(&wl.tz_search);
	const char *cur = conf_get(wl.conf, "timezone", "UTC");
	f32 y = ui_scroll_begin(ui, pad(list, 4), "tz");
	for (usize i = 0; i < nz; i++) {
		const ZoneInfo *z = &zones[i];
		if (*q && !str_find_nocase(z->city, q) && !str_find_nocase(z->country, q))
			continue;
		Rect row = rect(list.x + 4, y, list.w - 8, 40);
		y += 40;
		ui_push_id(ui, (i64)i);
		UiInteract it = ui_row(ui, row, ui_id(ui, "z"), str_eq(cur, z->zone));
		ui_pop_id(ui);
		char name[128];
		fmt(name, sizeof name, "%s%s%s", z->city, z->country[0] ? ", " : "", z->country);
		ui_text(ui, ui_font(ui, 14, 500), row.x + 12, row.y + 25, name, t->text);
		/* the local time there, to make the choice obvious */
		Civil c;
		civil_from_unix(time_unix() + tz_offset_in(z->zone, time_unix()), &c);
		char tm[16];
		fmt(tm, sizeof tm, "%02d:%02d", c.hour, c.min);
		Font tf = ui_font(ui, 13, 500);
		ui_text(ui, tf, row.x + row.w - 12 - ui_text_width(ui, tf, tm), row.y + 25, tm, t->text_dim);
		if (it.clicked) {
			conf_set(wl.conf, "timezone", z->zone);
			tz_set(z->zone);
			save(ui);
		}
	}
	ui_scroll_end(ui, y - list.y);
}

/* ---- Account --------------------------------------------------------------------------------- */

static int password_score(const char *p)
{
	usize n = strlen(p);
	if (!n)
		return 0;
	bool lower = false, upper = false, digit = false, other = false;
	for (const char *c = p; *c; c++) {
		if (*c >= 'a' && *c <= 'z')
			lower = true;
		else if (*c >= 'A' && *c <= 'Z')
			upper = true;
		else if (is_digit((u8)*c))
			digit = true;
		else
			other = true;
	}
	int kinds = lower + upper + digit + other;
	int s = n >= 6 ? 1 : 0;
	if (n >= 8 && kinds >= 2)
		s = 2;
	if (n >= 10 && kinds >= 3)
		s = 3;
	if (n >= 12 && kinds >= 3)
		s = 4;
	return s;
}

static void initials_of(const char *name, char *out)
{
	int k = 0;
	bool start = true;
	for (const char *c = name; *c && k < 2; c++) {
		if (*c == ' ') {
			start = true;
		} else if (start) {
			out[k++] = (char)to_upper((u8)*c);
			start = false;
		}
	}
	out[k] = 0;
}

static void page_account(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	setup_title(ui, &r, wl.changed_at, "Your Account",
		    "Your name goes on the lock screen and in your apps. A password keeps your files private: Zurbax asks for it when it starts, "
		    "and before big changes like installing apps.");
	bool wide = r.w > 640;
	if (wide) {
		/* the picture people will see on the lock screen */
		Rect card = setup_rise(ui, wl.changed_at, 1, cut_right(&r, minf(240, r.w * 0.36f)));
		card.h = minf(card.h, 250);
		setup_card(ui, card, false, 0);
		char ini[8];
		initials_of(wl.name.len ? sb_str(&wl.name) : "", ini);
		if (!str_eq(ini, wl.initials)) {
			str_copy(wl.initials, ini, sizeof wl.initials);
			wl.initials_at = ui->now;
		}
		f32 pop = 1 + 0.12f * sinf(clampf((f32)(ui->now - wl.initials_at) / 0.35f, 0, 1) * 3.14159f);
		if (ui->now - wl.initials_at < 0.35)
			ui->redraw = true;
		f32 a = 96 * pop;
		ui_avatar(ui, rect(card.x + (card.w - a) / 2, card.y + 30 + (96 - a) / 2, a, a), wl.name.len ? sb_str(&wl.name) : "Zurbax User");
		ui_text_in(ui, ui_font(ui, 17, 700), rect(card.x + 12, card.y + 142, card.w - 24, 26), wl.name.len ? sb_str(&wl.name) : "Your name",
			   wl.name.len ? t->text : t->text_faint, ALIGN_CENTER);
		Rect badge = center_in(rect(card.x, card.y + 176, card.w, 24), 120, 24);
		setup_block(ui, badge, 2, color_with_alpha(t->accent, 0x26));
		ui_text_in(ui, ui_font(ui, 12, 600), badge, "Administrator", t->accent, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 12, 400), rect(card.x + 12, card.y + 210, card.w - 24, 20),
			   wl.skip_password ? "Starts without a password" : "Signs in with a password", t->text_dim, ALIGN_CENTER);
		cut_right(&r, 28);
	}
	ui_text_in(ui, ui_font(ui, 13, 700), cut_top(&r, 22), "Name", t->text, ALIGN_START);
	ui_entry(ui, cut_top(&r, 38), "name", &wl.name, "First and last name", ENTRY_AUTOFOCUS);
	cut_top(&r, 20);
	ui_text_in(ui, ui_font(ui, 13, 700), cut_top(&r, 22), "Password", t->text, ALIGN_START);
	if (!wl.skip_password) {
		ui_entry(ui, cut_top(&r, 38), "pw", &wl.pw, "Password", ENTRY_PASSWORD);
		cut_top(&r, 10);
		/* how strong it is, in four bars that fill as it gets better */
		int score = password_score(sb_str(&wl.pw));
		static const char *const words[] = { "", "Weak", "Fair", "Good", "Strong" };
		Color cols[] = { t->danger, t->danger, t->warning, color_hex("#7ac74f"), t->success };
		Rect meter = cut_top(&r, 18);
		Rect bars = cut_left(&meter, meter.w - 70);
		f32 bw = (bars.w - 18) / 4;
		f32 fill = ui_anim(ui, ui_id(ui, "pw-meter"), (f32)score, 10);
		if (fabsf(fill - (f32)score) > 0.01f)
			ui->redraw = true;
		for (int i = 0; i < 4; i++) {
			Rect b = rect(bars.x + (f32)i * (bw + 6), bars.y + 5, bw, 8);
			setup_block(ui, b, 1, color_with_alpha(t->text, 0x1a));
			f32 f = clampf(fill - (f32)i, 0, 1);
			if (f > 0)
				setup_block(ui, rect(b.x, b.y, b.w * f, b.h), 1, cols[score]);
		}
		ui_text_in(ui, ui_font(ui, 12, 600), meter, words[score], cols[score], ALIGN_END);
		cut_top(&r, 10);
		ui_entry(ui, cut_top(&r, 38), "pw2", &wl.pw2, "Type it again", ENTRY_PASSWORD);
		Rect note = cut_top(&r, 26);
		if (wl.pw2.len) {
			bool same = str_eq(sb_str(&wl.pw), sb_str(&wl.pw2));
			ui_icon(ui, rect(note.x, note.y + 7, 14, 14), same ? "check" : "error", same ? t->success : t->danger);
			ui_text_in(ui, ui_font(ui, 12, 500), rect(note.x + 20, note.y, note.w - 20, note.h), same ? "They match" : "They don't match yet",
				   same ? t->success : t->danger, ALIGN_START);
		}
		cut_top(&r, 6);
	}
	ui_checkbox(ui, cut_top(&r, 28), "Use this computer without a password", &wl.skip_password);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), cut_top(&r, 34), "You can add a password, or change it, any time in Settings, under Users.", t->text_faint,
			ALIGN_START);
	if (wl.error[0])
		ui_text(ui, ui_font(ui, 13, 500), r.x, r.y + 18, wl.error, t->danger);
}

/* ---- Look ------------------------------------------------------------------------------------ */

static struct {
	Canvas *img[24];
	int w, h;
} thumbs;

/* The wallpapers to choose from: the themes' pictures, then the painted ones. */
static char bit_names[8][32];

static const char *wallpaper_name(int i)
{
	if (i < HIVE_THEME_COUNT) {
		fmt(bit_names[i], sizeof bit_names[i], "bit-%s", HIVE_THEMES[i].id);
		return bit_names[i];
	}
	return WALLPAPER_NAMES[i - HIVE_THEME_COUNT];
}

static const char *wallpaper_title(int i)
{
	return i < HIVE_THEME_COUNT ? HIVE_THEMES[i].name : WALLPAPER_TITLES[i - HIVE_THEME_COUNT];
}

static const Canvas *wallpaper_thumb(Ui *ui, int i, f32 w, f32 h)
{
	int pw = (int)(w * ui->scale), ph = (int)(h * ui->scale);
	if (thumbs.w != pw || thumbs.h != ph) {
		for (int k = 0; k < 24; k++)
			canvas_free(thumbs.img[k]), thumbs.img[k] = NULL;
		thumbs.w = pw, thumbs.h = ph;
	}
	if (!thumbs.img[i])
		thumbs.img[i] = wallpaper_render(wallpaper_name(i), pw, ph);
	return thumbs.img[i];
}

static int wallpaper_index(void)
{
	const char *cur = look_wallpaper(wl.conf);
	for (int i = 0; i < HIVE_THEME_COUNT + WALLPAPER_COUNT; i++)
		if (str_eq(cur, wallpaper_name(i)))
			return i;
	return 0;
}

/* A theme in a card: its picture with Hive's glass bar over it, its name in
 * pixels and its colours in a row of blocks. Choosing it sets the accent and
 * the wallpaper to go with it, and the assistant wears it straight away. */
static struct {
	Canvas *img[4];
	int w, h;
} art;

static void theme_card(Ui *ui, Rect r, int i, bool selected)
{
	Theme *t = &ui->theme;
	const HiveTheme *h = &HIVE_THEMES[i];
	UiInteract it = ui_interact(ui, ui_id(ui, h->id), r);
	f32 hover = ui_fade(ui, ui_id(ui, h->id), it.hovered, 0.15f);
	if (it.hovered)
		ui_set_cursor(ui, CURSOR_POINTER);
	setup_block(ui, rect(r.x + 4, r.y + 4, r.w, r.h), 2, color_hex("#00000050"));
	setup_card(ui, r, selected, hover);
	Rect pic = pad4(r, 10, 10, 0, 10);
	pic.h = r.h - 10 - 46;
	int pw = (int)(pic.w * ui->scale), ph = (int)(pic.h * ui->scale);
	if (art.w != pw || art.h != ph) {
		for (int k = 0; k < 4; k++)
			canvas_free(art.img[k]), art.img[k] = NULL;
		art.w = pw, art.h = ph;
	}
	if (!art.img[i])
		art.img[i] = bitart_render(h->id, pw, ph);
	ui_image(ui, pic, art.img[i], 0, true);
	/* Hive's bar: three islands of glass */
	Color glass = color_hex("#ffffff2a");
	setup_block(ui, rect(pic.x + 8, pic.y + 8, 52, 12), 1, glass);
	setup_block(ui, rect(pic.x + pic.w / 2 - 22, pic.y + 8, 44, 12), 1, glass);
	setup_block(ui, rect(pic.x + pic.w - 44, pic.y + 8, 36, 12), 1, glass);
	for (int k = 0; k < 4; k++)
		ui_fill(ui, rect(pic.x + 14 + (f32)k * 10, pic.y + 12, 4, 4), k ? color_hex(h->muted) : color_hex(h->accent));
	/* two tiles side by side */
	Rect tl = rect(pic.x + 8, pic.y + 28, (pic.w - 24) * 0.55f, pic.h - 38);
	Rect tr = rect(tl.x + tl.w + 8, tl.y, pic.w - 24 - tl.w, tl.h);
	setup_block(ui, tl, 1, color_with_alpha(color_hex(h->bg), 0xd8));
	setup_block(ui, tr, 1, color_with_alpha(color_hex(h->bg), 0xd8));
	setup_block(ui, rect(tl.x - 2, tl.y - 2, tl.w + 4, 2), 0, color_hex(h->accent));
	for (int k = 0; k < 3; k++) {
		ui_fill(ui, rect(tl.x + 8, tl.y + 10 + (f32)k * 10, (tl.w - 16) * (k == 1 ? 0.6f : 0.85f), 4), color_with_alpha(color_hex(h->fg), 0x70));
		ui_fill(ui, rect(tr.x + 8, tr.y + 10 + (f32)k * 10, (tr.w - 16) * (k == 2 ? 0.5f : 0.8f), 4), color_with_alpha(color_hex(h->fg), 0x50));
	}
	/* the name, and the colours */
	Rect foot = rect(r.x + 14, r.y + r.h - 40, r.w - 28, 30);
	ui_pixel_text(ui, foot.x, foot.y + 8, h->name, 3, selected ? t->text : color_mix(t->text_dim, t->text, hover));
	const char *cols[] = { h->bg, h->fg, h->accent, h->accent2, h->muted };
	for (int k = 0; k < 5; k++) {
		Rect sw = rect(foot.x + foot.w - (f32)(5 - k) * 18, foot.y + 7, 14, 14);
		setup_block(ui, sw, 1, color_hex(cols[k]));
	}
	if (it.clicked && !selected) {
		char wall[64];
		fmt(wall, sizeof wall, "bit-%s", h->id);
		conf_set(wl.conf, "hive_theme", h->id);
		conf_set(wl.conf, "accent", h->accent);
		conf_set(wl.conf, "accent_follow", "false");
		conf_set(wl.conf, "wallpaper", wall);
		conf_set_bool(wl.conf, "dark", true);
		save(ui);
	}
}

static void page_look(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	setup_title(ui, &r, wl.changed_at, "Make It Yours",
		    "Pick a theme: it colours the whole system, this assistant included. Then an accent and a wallpaper, if you like. All of it "
		    "changes later in Settings.");
	const HiveTheme *cur = hive_theme_find(conf_get(wl.conf, "hive_theme", NULL));
	Rect row = setup_rise(ui, wl.changed_at, 1, cut_top(&r, minf(210, r.h - 150)));
	f32 cw = (row.w - 24) / 2;
	for (int i = 0; i < HIVE_THEME_COUNT && i < 2; i++)
		theme_card(ui, rect(row.x + (f32)i * (cw + 24), row.y, cw, row.h), i, cur == &HIVE_THEMES[i]);
	cut_top(&r, 22);
	/* the accent: blocks in a row */
	Rect ar = setup_rise(ui, wl.changed_at, 2, cut_top(&r, 22));
	ui_pixel_text(ui, ar.x, ar.y + 4, "ACCENT", 2, t->text_dim);
	Rect sw_row = rect(ar.x + 80, ar.y - 4, ar.w - 80, 30);
	const char *acc = look_accent(wl.conf);
	f32 step = minf(38, sw_row.w / (f32)(THEME_ACCENT_COUNT + 1));
	for (int i = 0; i <= THEME_ACCENT_COUNT; i++) {
		/* the theme's own accent first, then the rest */
		const char *name = i ? THEME_ACCENT_NAMES[i - 1] : cur->accent;
		Color c = i ? THEME_ACCENT_COLORS[i - 1] : color_hex(cur->accent);
		Rect sw = rect(sw_row.x + (f32)i * step, sw_row.y, step - 8, 26);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "accent"), sw);
		ui_pop_id(ui);
		bool on = str_eq(acc, name);
		if (it.hovered)
			sw = rect(sw.x, sw.y - 2, sw.w, sw.h);
		setup_block(ui, rect(sw.x + 2, sw.y + 2, sw.w, sw.h), 1, color_hex("#00000060"));
		setup_block(ui, sw, 1, c);
		ui_fill(ui, rect(sw.x + 2, sw.y, sw.w - 4, 2), color_with_alpha(COLOR_WHITE, 0x50));
		if (on)
			setup_block(ui, rect(sw.x + sw.w / 2 - 3, sw.y + sw.h / 2 - 3, 6, 6), 0, COLOR_WHITE);
		if (it.clicked && !on) {
			conf_set(wl.conf, "accent", name);
			conf_set(wl.conf, "accent_follow", "false");
			save(ui);
		}
	}
	cut_top(&r, 20);
	/* the wallpaper: the themes' pictures first, then the painted ones */
	Rect wr = setup_rise(ui, wl.changed_at, 3, cut_top(&r, 22));
	ui_pixel_text(ui, wr.x, wr.y + 4, "WALLPAPER", 2, t->text_dim);
	cut_top(&r, 6);
	r = setup_rise(ui, wl.changed_at, 4, r);
	int total = HIVE_THEME_COUNT + WALLPAPER_COUNT;
	int per = r.w > 700 ? 8 : 6;
	f32 gw = (r.w - (f32)(per - 1) * 10) / (f32)per, gh = gw * 9 / 16;
	int sel = wallpaper_index();
	for (int i = 0; i < total; i++) {
		Rect cell = rect(r.x + (f32)(i % per) * (gw + 10), r.y + (f32)(i / per) * (gh + 10), gw, gh);
		if (cell.y + cell.h > r.y + r.h)
			break;
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "wp"), cell);
		ui_pop_id(ui);
		ui_image(ui, cell, wallpaper_thumb(ui, i, gw, gh), 0, true);
		if (i == sel)
			setup_card(ui, rect_inset(cell, -2, -2), true, 0);
		else if (it.hovered)
			setup_card(ui, rect_inset(cell, -2, -2), false, 1);
		if (it.hovered)
			ui_tooltip(ui, wallpaper_title(i));
		if (it.clicked && i != sel) {
			conf_set(wl.conf, "wallpaper", wallpaper_name(i));
			save(ui);
		}
	}
}

/* ---- Ready ----------------------------------------------------------------------------------- */

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

static void summary_row(Ui *ui, Rect *r, const char *icon, const char *what, const char *value)
{
	Theme *t = &ui->theme;
	Rect row = cut_top(r, 40);
	ui_icon(ui, rect(row.x, row.y + 11, 18, 18), icon, t->accent);
	ui_text_in(ui, ui_font(ui, 14, 500), rect(row.x + 30, row.y, 130, row.h), what, t->text_dim, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 14, 700), rect(row.x + 160, row.y, row.w - 160, row.h), value, t->text, ALIGN_START);
	setup_rule(ui, row.x, row.y + row.h - 1, row.x + row.w);
}

static void page_done(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	setup_tick(ui, rect(r.x, r.y, 76, 76), wl.done_at, true);
	cut_top(&r, 96);
	setup_title(ui, &r, wl.changed_at, "You're All Set", OS_NAME " is ready. Here's what you chose, and where to find things.");
	bool wide = r.w > 560;
	Rect sum = wide ? cut_left(&r, r.w * 0.54f) : cut_top(&r, 170);
	if (wide)
		cut_left(&r, 28);
	sum = setup_rise(ui, wl.changed_at, 1, sum);
	char look[96];
	const HiveTheme *ht = hive_theme_find(conf_get(wl.conf, "hive_theme", NULL));
	int wi = wallpaper_index();
	if (wi < HIVE_THEME_COUNT && &HIVE_THEMES[wi] == ht)
		fmt(look, sizeof look, "%s, with its picture", ht->name);
	else
		fmt(look, sizeof look, "%s · %s", ht->name, wallpaper_title(wi));
	char who[160];
	fmt(who, sizeof who, "%s%s", wl.name.len ? sb_str(&wl.name) : "Zurbax User",
	    (!wl.skip_password && wl.pw.len) ? ", with a password" : ", no password");
	const ZoneInfo *z = tz_find(conf_get(wl.conf, "timezone", "UTC"));
	summary_row(ui, &sum, "keyboard", "Keyboard", LAYOUT_TITLES[layout_index()]);
	summary_row(ui, &sum, "location", "Time zone", z ? z->city : conf_get(wl.conf, "timezone", "UTC"));
	summary_row(ui, &sum, "user", "Account", who);
	summary_row(ui, &sum, "palette", "Look", look);
	if (wide) {
		/* Hive's keys to start with, as keycaps */
		static const char *const keys[] = { "SUPER+SPACE", "SUPER+ENTER", "SUPER+1-9" };
		static const char *const texts[] = { "Every app, by name", "A terminal, tiled in", "Your nine workspaces" };
		for (int i = 0; i < 3; i++) {
			Rect tip = setup_rise(ui, wl.changed_at, 2 + i, cut_top(&r, 58));
			cut_top(&r, 10);
			setup_card(ui, tip, false, 0);
			f32 kw = ui_pixel_text_width(keys[i], 2) + 20;
			Rect cap = rect(tip.x + 14, tip.y + 14, kw, 30);
			setup_block(ui, rect(cap.x, cap.y + 4, cap.w, cap.h), 1, color_hex("#00000080"));
			setup_block(ui, cap, 1, color_mix(t->window, COLOR_WHITE, 0.08f));
			ui_fill(ui, rect(cap.x + 2, cap.y, cap.w - 4, 2), color_with_alpha(COLOR_WHITE, 0x30));
			ui_pixel_text(ui, cap.x + 10, cap.y + 8, keys[i], 2, t->accent);
			ui_text_in(ui, ui_font(ui, 13, 600), rect(cap.x + cap.w + 14, tip.y, tip.w - cap.w - 40, tip.h), texts[i], t->text, ALIGN_START);
		}
	}
}

static bool account_ready(void)
{
	if (wl.skip_password || (!wl.pw.len && !wl.pw2.len))
		return true;
	return str_eq(sb_str(&wl.pw), sb_str(&wl.pw2));
}

static void frame(Ui *ui)
{
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
	/* the steps shown: Wi-Fi only when it's wanted */
	bool wifi = wifi_page_wanted();
	const char *steps[P_COUNT];
	int n = 0, cur = 0;
	for (int p = 0; p < P_COUNT; p++) {
		if (p == P_NETWORK && !wifi)
			continue;
		if (p == wl.page)
			cur = n;
		steps[n++] = STEP_NAMES[p];
	}
	SetupSteps s = { .title = wl.live ? "Try or install" : "Setup", .steps = steps, .count = n, .current = cur,
			 .changed_at = wl.changed_at, .dir = wl.dir, .theme = conf_get(wl.conf, "hive_theme", NULL) };
	Rect page = setup_frame(ui, &s);
	Rect bottom = cut_bottom(&page, 40);
	cut_bottom(&page, 18);
	ui_push_clip(ui, setup_pane());
	switch (wl.page) {
	case P_WELCOME:
		page_welcome(ui, page);
		break;
	case P_KEYBOARD:
		page_keyboard(ui, page);
		break;
	case P_NETWORK:
		page_network(ui, page);
		break;
	case P_TIMEZONE:
		page_timezone(ui, page);
		break;
	case P_ACCOUNT:
		page_account(ui, page);
		break;
	case P_LOOK:
		page_look(ui, page);
		break;
	case P_DONE:
		page_done(ui, page);
		break;
	}
	ui_pop_clip(ui);
	/* the buttons along the bottom */
	setup_rule(ui, bottom.x, bottom.y - 12, bottom.x + bottom.w);
	if (wl.page == P_WELCOME) {
		if (!wl.live) {
			if (setup_button(ui, cut_right(&bottom, 170), "Let's Go!", SETUP_BUTTON_GO) || ui_key_pressed(ui, KEY_ENTER, 0))
				go(ui, P_KEYBOARD);
		}
		if (wl.setup && setup_button(ui, cut_left(&bottom, 130), "Skip Setup", SETUP_BUTTON_FLAT)) {
			wl.skip_password = true;
			finish(ui);
		}
	} else if (wl.page < P_DONE) {
		if (setup_button(ui, cut_left(&bottom, 100), "Back", SETUP_BUTTON))
			go(ui, wl.page - 1);
		bool can = wl.page != P_ACCOUNT || account_ready();
		if (setup_button(ui, cut_right(&bottom, 130), "Next", SETUP_BUTTON_GO) && can)
			go(ui, wl.page + 1);
		cut_right(&bottom, 10);
		if (setup_button(ui, cut_right(&bottom, 90), "Skip", SETUP_BUTTON_FLAT)) {
			if (wl.page == P_ACCOUNT) {
				sb_clear(&wl.pw);
				sb_clear(&wl.pw2);
				wl.skip_password = true;
			}
			go(ui, wl.page + 1);
		}
		if (ui_key_pressed(ui, KEY_ENTER, 0) && can && !ui_is_focused(ui, "test") && !ui_is_focused(ui, "wifi-pass"))
			go(ui, wl.page + 1);
	} else {
		if (setup_button(ui, cut_left(&bottom, 100), "Back", SETUP_BUTTON))
			go(ui, wl.page - 1);
		if (setup_button(ui, cut_right(&bottom, 220), wl.finishing ? "Starting…" : "Start Using " OS_NAME, SETUP_BUTTON_GO) ||
		    ui_key_pressed(ui, KEY_ENTER, 0))
			finish(ui);
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
	fmt(wl.conf_path, sizeof wl.conf_path, "%s/.config/zurbax/settings.conf", home_dir());
	char *dir = path_dir(wl.conf_path);
	dir_make(dir, 0755);
	mem_free(dir);
	wl.conf = conf_load(wl.conf_path);
	wl.live = file_exists("/run/zurbax-live") || env_get("ZURBAX_WELCOME_LIVE") != NULL;
	if (!conf_get(wl.conf, "timezone", NULL))
		conf_set(wl.conf, "timezone", "UTC");
	wl.changed_at = ui->now;
	/* a page to start on, for screenshots of the assistant */
	const char *p = env_get("ZURBAX_WELCOME_PAGE");
	if (p)
		wl.page = CLAMP((int)str_to_int(p, NULL), 0, P_COUNT - 1), wl.done_at = ui->now;
}

int main(int argc, char **argv)
{
	wl.setup = argc > 1 && str_eq(argv[1], "--setup");
	UiAppDesc d = { .id = "welcome", .title = "Welcome", .width = 1040, .height = 680, .min_width = 820, .min_height = 600,
			.flags = WIN_RESIZABLE | (wl.setup ? WIN_FULLSCREEN_START : 0), .init = init, .frame = frame, .close = on_close };
	return ui_run(&d);
}
