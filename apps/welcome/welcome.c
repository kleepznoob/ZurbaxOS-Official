/*
 * Welcome — the first-start assistant (and a short tour later on).
 *
 * The steps run down a sidebar over the wallpaper (setup.c): a welcome,
 * the keyboard (with a keyboard that lights up as you type), Wi-Fi when
 * there's no cable, the time zone (with a clock that swings to the city's
 * time), an account (with its picture and how strong the password is), a
 * look (with a little desktop that changes as you choose) and a summary.
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
	ui_card(ui, card);
	Rect rows = pad2(card, 14, 4);
	for (int i = 0; i < n; i++) {
		Rect row = cut_top(&rows, 42);
		if (i < n - 1)
			ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
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
	ui_round(ui, r, 14, t->card);
	ui_border(ui, r, 14, 1, t->border);
	Rect c = pad(r, 18);
	Rect ic = cut_top(&c, 40);
	ui_round(ui, rect(ic.x, ic.y, 40, 40), 11, color_with_alpha(t->accent, 0x22));
	ui_icon(ui, rect(ic.x + 10, ic.y + 10, 20, 20), icon, t->accent);
	cut_top(&c, 14);
	ui_text_in(ui, ui_font(ui, 15, 700), cut_top(&c, 22), title, t->text, ALIGN_START);
	cut_top(&c, 2);
	ui_text_wrapped(ui, ui_font(ui, 13, 400), c, text, t->text_dim, ALIGN_START);
}

static void page_welcome(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	/* the logo floats above the greeting */
	f32 bob = t->reduce_motion ? 0 : sinf((f32)ui->now * 1.6f) * 4;
	f32 in = setup_ease((f32)((ui->now - wl.changed_at) / 0.6));
	f32 size = 84 * (0.85f + 0.15f * in);
	ui_app_icon(ui, rect(r.x, r.y + 4 + bob, size, size), "zurbax");
	cut_top(&r, 108);
	const char *about = wl.live ? "You're running " OS_NAME " from the USB stick. Nothing on this computer has changed: try it for as long as you like, "
				      "or install it when you're ready."
				    : "Let's get your computer ready. It takes about a minute, and every step can be skipped and changed later in Settings.";
	setup_title(ui, &r, wl.changed_at, "Welcome to " OS_NAME, about);
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
			"Steam with Proton, Heroic and ProtonPlus, and any controller, rumble included.");
		feature(ui, setup_rise(ui, wl.changed_at, 3, rect(row.x + fw + 16, row.y, fw, row.h)), "bag", "Thousands of apps",
			"Harbor brings all of Flathub. Firefox, LibreOffice and six more are here already.");
		feature(ui, setup_rise(ui, wl.changed_at, 4, rect(row.x + (fw + 16) * 2, row.y, fw, row.h)), "palette", "Yours to shape",
			"Light or dark, any accent colour and ten wallpapers the system paints itself.");
	}
}

/* ---- Keyboard -------------------------------------------------------------------------------- */

/* The layout's letters drawn as keys; the one just typed lights up. */
static void keyboard_picture(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	int li = layout_index();
	ui_round(ui, r, 14, t->dark ? color_hex("#17161a") : color_hex("#dcdde1"));
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
				face = color_mix(face, t->accent, lit);
			ui_round(ui, rect(key.x, key.y + 2, key.w, key.h), 7, t->dark ? color_hex("#0c0b0e") : color_hex("#b9bac0"));
			ui_round(ui, key, 7, face);
			char label[4] = { c, 0 };
			if (keys[i] == '~')
				str_copy(label, "\xc3\x91", sizeof label);
			if (row < 3)
				ui_text_in(ui, ui_font(ui, 13, 600), key, label, hit ? COLOR_WHITE : t->text, ALIGN_CENTER);
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
	ui_round(ui, st, 12, color_with_alpha(col, 0x18));
	ui_icon(ui, rect(st.x + 16, st.y + 15, 20, 20), icon, col);
	ui_text_in(ui, ui_font(ui, 14, 600), rect(st.x + 48, st.y, st.w - 60, st.h), line, col, ALIGN_START);
	cut_top(&r, 12);
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
	ui_round(ui, r, 16, t->card);
	ui_border(ui, r, 16, 1, t->border);
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
		ui_round(ui, card, 16, t->card);
		ui_border(ui, card, 16, 1, t->border);
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
		ui_round(ui, badge, 12, color_with_alpha(t->accent, 0x22));
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
			Rect b = rect(bars.x + (f32)i * (bw + 6), bars.y + 6, bw, 6);
			ui_round(ui, b, 3, t->border);
			f32 f = clampf(fill - (f32)i, 0, 1);
			if (f > 0)
				ui_round(ui, rect(b.x, b.y, b.w * f, b.h), 3, cols[score]);
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
	Canvas *img[16];
	int w, h;
} thumbs;

static const Canvas *wallpaper_thumb(Ui *ui, int i, f32 w, f32 h)
{
	int pw = (int)(w * ui->scale), ph = (int)(h * ui->scale);
	if (thumbs.w != pw || thumbs.h != ph) {
		for (int k = 0; k < 16; k++)
			canvas_free(thumbs.img[k]), thumbs.img[k] = NULL;
		thumbs.w = pw, thumbs.h = ph;
	}
	if (!thumbs.img[i])
		thumbs.img[i] = wallpaper_render(WALLPAPER_NAMES[i], pw, ph);
	return thumbs.img[i];
}

static int wallpaper_index(void)
{
	const char *cur = conf_get(wl.conf, "wallpaper", WALLPAPER_DEFAULT);
	for (int i = 0; i < WALLPAPER_COUNT; i++)
		if (str_eq(cur, WALLPAPER_NAMES[i]))
			return i;
	return 0;
}

/* A small desktop that changes as the choices do: the wallpaper, a window
 * in the light or dark style, and a button in the accent colour. */
static void look_preview(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	f32 dk = ui_anim(ui, ui_id(ui, "look-dark"), t->dark ? 1 : 0, 8);
	if (dk > 0.002f && dk < 0.998f)
		ui->redraw = true;
	ui_shadow(ui, r, 12, 30, 10, color_with_alpha(t->shadow, 0x50));
	Rect scr = r;
	f32 tw = 16 * 10, th = 9 * 10;
	const Canvas *wp = wallpaper_thumb(ui, wallpaper_index(), tw * 2, th * 2);
	ui_image(ui, scr, wp, 12, true);
	/* top bar and taskbar */
	ui_round4(ui, rect(scr.x, scr.y, scr.w, 16), 12, 12, 0, 0, color_hex("#000000a0"));
	ui_round(ui, rect(scr.x + 8, scr.y + 5, 40, 6), 3, color_hex("#ffffffb0"));
	Rect bar = rect(scr.x, scr.y + scr.h - 26, scr.w, 26);
	ui_round4(ui, bar, 0, 0, 12, 12, color_hex("#141218e8"));
	for (int i = 0; i < 6; i++) {
		static const char *const apps[] = { "drift", "vault", "mail", "music", "harbor", "steam" };
		ui_app_icon(ui, rect(bar.x + 14 + (f32)i * 24, bar.y + 4, 18, 18), apps[i]);
	}
	/* a window */
	Rect w = rect(scr.x + scr.w * 0.16f, scr.y + scr.h * 0.18f, scr.w * 0.62f, scr.h * 0.58f);
	Color bg = color_mix(color_hex("#fafafa"), color_hex("#26252b"), dk), head = color_mix(color_hex("#ececef"), color_hex("#323137"), dk);
	Color line = color_mix(color_hex("#d4d4da"), color_hex("#4a4950"), dk);
	ui_shadow(ui, w, 8, 16, 6, color_hex("#00000060"));
	ui_round(ui, w, 8, bg);
	ui_round4(ui, rect(w.x, w.y, w.w, 20), 8, 8, 0, 0, head);
	for (int i = 0; i < 3; i++)
		ui_circle(ui, w.x + w.w - 14 - (f32)i * 12, w.y + 10, 3.5f, line);
	Rect body = pad4(w, 30, 14, 12, 14);
	ui_round(ui, rect(body.x, body.y, body.w * 0.6f, 8), 4, line);
	ui_round(ui, rect(body.x, body.y + 16, body.w * 0.85f, 6), 3, line);
	ui_round(ui, rect(body.x, body.y + 28, body.w * 0.7f, 6), 3, line);
	ui_round(ui, rect(body.x, body.y + 40, body.w * 0.78f, 6), 3, line);
	ui_round(ui, rect(body.x + body.w - 64, body.y + body.h - 22, 64, 22), 6, t->accent);
	ui_round(ui, rect(body.x + body.w - 50, body.y + body.h - 14, 36, 6), 3, color_with_alpha(COLOR_WHITE, 0xd0));
}

static void style_choice(Ui *ui, Rect r, bool dark, bool selected)
{
	Theme *t = &ui->theme;
	UiInteract it = ui_interact(ui, ui_id(ui, dark ? "dark" : "light"), r);
	ui_round(ui, r, 12, dark ? color_hex("#1e1d22") : color_hex("#eeeef1"));
	Rect w = pad4(r, 12, 14, 0, 14);
	w.h -= 28;
	ui_round4(ui, w, 6, 6, 0, 0, dark ? color_hex("#2d2c32") : color_hex("#ffffff"));
	ui_round(ui, rect(w.x + 8, w.y + 8, w.w * 0.5f, 5), 3, dark ? color_hex("#5a5960") : color_hex("#cfd0d4"));
	ui_round(ui, rect(w.x + w.w - 30, w.y + w.h - 16, 24, 10), 4, t->accent);
	ui_text_in(ui, ui_font(ui, 13, selected ? 700 : 500), rect(r.x, r.y + r.h - 26, r.w, 22), dark ? "Dark" : "Light",
		   dark ? COLOR_WHITE : color_hex("#1b1a1f"), ALIGN_CENTER);
	if (selected)
		ui_border(ui, rect_inset(r, -3, -3), 14, 2.5f, t->accent);
	else if (it.hovered)
		ui_border(ui, rect_inset(r, -2, -2), 13, 1.5f, t->border_strong);
	if (it.clicked && !selected) {
		conf_set_bool(wl.conf, "dark", dark);
		save(ui);
	}
}

static void page_look(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	setup_title(ui, &r, wl.changed_at, "Make It Yours",
		    "Light or dark, a colour for buttons and highlights, and a wallpaper. Everything here changes later in Settings, under Appearance.");
	/* the little desktop on the left, the style and colour beside it */
	Rect top = cut_top(&r, 196);
	Rect pv = cut_left(&top, minf(top.w * 0.46f, 340));
	look_preview(ui, setup_rise(ui, wl.changed_at, 1, rect(pv.x, pv.y + 22, pv.w, minf(pv.w * 10 / 16, top.h - 22))));
	cut_left(&top, 26);
	ui_text_in(ui, ui_font(ui, 13, 700), cut_top(&top, 22), "Style", t->text, ALIGN_START);
	Rect s = cut_top(&top, 92);
	f32 cw = (s.w - 14) / 2;
	bool dark = conf_get_bool(wl.conf, "dark", false);
	style_choice(ui, rect(s.x, s.y, cw, s.h), false, !dark);
	style_choice(ui, rect(s.x + cw + 14, s.y, cw, s.h), true, dark);
	cut_top(&top, 14);
	ui_text_in(ui, ui_font(ui, 13, 700), cut_top(&top, 22), "Accent colour", t->text, ALIGN_START);
	Rect row = cut_top(&top, 36);
	const char *cur = conf_get(wl.conf, "accent", THEME_ACCENT_DEFAULT);
	f32 step = minf(38, row.w / (f32)THEME_ACCENT_COUNT);
	for (int i = 0; i < THEME_ACCENT_COUNT; i++) {
		Rect sw = rect(row.x + (f32)i * step, row.y, step, 34);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "accent"), sw);
		f32 grow = ui_fade(ui, ui_id(ui, "accent-hover"), it.hovered, 0.12f);
		ui_pop_id(ui);
		Color c = THEME_ACCENT_COLORS[i];
		f32 cx = sw.x + step / 2, cy = sw.y + 17, rr = minf(12, step / 2 - 4);
		ui_circle(ui, cx, cy, rr + grow * 2, c);
		if (str_eq(cur, THEME_ACCENT_NAMES[i])) {
			ui_ring(ui, cx, cy, rr + 4, 2, c);
			ui_icon(ui, rect(cx - 7, cy - 7, 14, 14), "check", COLOR_WHITE);
		}
		if (it.clicked) {
			conf_set(wl.conf, "accent", THEME_ACCENT_NAMES[i]);
			save(ui);
		}
	}
	cut_top(&r, 16);
	ui_text_in(ui, ui_font(ui, 13, 700), cut_top(&r, 22), "Wallpaper", t->text, ALIGN_START);
	cut_top(&r, 4);
	int per = 5;
	f32 gw = (r.w - (f32)(per - 1) * 10) / (f32)per, gh = gw * 9 / 16;
	int sel = wallpaper_index();
	for (int i = 0; i < WALLPAPER_COUNT; i++) {
		Rect cell = rect(r.x + (f32)(i % per) * (gw + 10), r.y + (f32)(i / per) * (gh + 10), gw, gh);
		if (cell.y + cell.h > r.y + r.h)
			break;
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "wp"), cell);
		ui_pop_id(ui);
		ui_image(ui, cell, wallpaper_thumb(ui, i, gw, gh), 8, true);
		if (i == sel)
			ui_border(ui, rect_inset(cell, -3, -3), 10, 2.5f, t->accent);
		else if (it.hovered)
			ui_border(ui, rect_inset(cell, -2, -2), 9, 1.5f, t->border_strong);
		if (it.hovered)
			ui_tooltip(ui, WALLPAPER_TITLES[i]);
		if (it.clicked && i != sel) {
			conf_set(wl.conf, "wallpaper", WALLPAPER_NAMES[i]);
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
	ui_text_in(ui, ui_font(ui, 14, 600), rect(row.x + 160, row.y, row.w - 160, row.h), value, t->text, ALIGN_START);
	ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
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
	fmt(look, sizeof look, "%s · %s", conf_get_bool(wl.conf, "dark", false) ? "Dark" : "Light", WALLPAPER_TITLES[wallpaper_index()]);
	char who[160];
	fmt(who, sizeof who, "%s%s", wl.name.len ? sb_str(&wl.name) : "Zurbax User",
	    (!wl.skip_password && wl.pw.len) ? ", with a password" : ", no password");
	const ZoneInfo *z = tz_find(conf_get(wl.conf, "timezone", "UTC"));
	summary_row(ui, &sum, "keyboard", "Keyboard", LAYOUT_TITLES[layout_index()]);
	summary_row(ui, &sum, "location", "Time zone", z ? z->city : conf_get(wl.conf, "timezone", "UTC"));
	summary_row(ui, &sum, "user", "Account", who);
	summary_row(ui, &sum, "palette", "Look", look);
	if (wide) {
		static const char *const icons[] = { "view-grid", "bag", "gamepad" };
		static const char *const heads[] = { "Start", "Harbor", "Steam" };
		static const char *const texts[] = { "Every app, at the left of the taskbar.", "Thousands more apps, one click each.",
						     "Your games, with Proton for Windows ones." };
		for (int i = 0; i < 3; i++) {
			Rect tip = setup_rise(ui, wl.changed_at, 2 + i, cut_top(&r, 62));
			cut_top(&r, 10);
			ui_round(ui, tip, 12, t->card);
			ui_border(ui, tip, 12, 1, t->border);
			ui_icon(ui, rect(tip.x + 16, tip.y + 21, 20, 20), icons[i], t->accent);
			ui_text_in(ui, ui_font(ui, 14, 700), rect(tip.x + 50, tip.y + 10, tip.w - 60, 22), heads[i], t->text, ALIGN_START);
			ui_text_in(ui, ui_font(ui, 12, 400), rect(tip.x + 50, tip.y + 32, tip.w - 60, 18), texts[i], t->text_dim, ALIGN_START);
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
			 .changed_at = wl.changed_at, .dir = wl.dir, .wallpaper = conf_get(wl.conf, "wallpaper", WALLPAPER_DEFAULT) };
	Rect page = setup_frame(ui, &s);
	Rect bottom = cut_bottom(&page, 40);
	cut_bottom(&page, 18);
	ui_push_clip(ui, rect(ui->width - page.w - 120, 0, page.w + 120, ui->height));
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
	ui_separator(ui, bottom.x, bottom.y - 10, bottom.x + bottom.w);
	if (wl.page == P_WELCOME) {
		if (!wl.live) {
			if (ui_button_suggested(ui, cut_right(&bottom, 150), "Get Started") || ui_key_pressed(ui, KEY_ENTER, 0))
				go(ui, P_KEYBOARD);
		}
		if (wl.setup && ui_button_flat(ui, cut_left(&bottom, 130), "Skip Setup")) {
			wl.skip_password = true;
			finish(ui);
		}
	} else if (wl.page < P_DONE) {
		if (ui_button(ui, cut_left(&bottom, 100), "Back"))
			go(ui, wl.page - 1);
		bool can = wl.page != P_ACCOUNT || account_ready();
		if (ui_button_suggested(ui, cut_right(&bottom, 120), "Next") && can)
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
	} else {
		if (ui_button(ui, cut_left(&bottom, 100), "Back"))
			go(ui, wl.page - 1);
		if (ui_button_suggested(ui, cut_right(&bottom, 210), wl.finishing ? "Starting…" : "Start Using " OS_NAME) ||
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
