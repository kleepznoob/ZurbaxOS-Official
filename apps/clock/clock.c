/*
 * Clock — world clocks, alarms, stopwatch and timer.
 *
 * Alarms are stored in ~/.config/zurbax/alarms.conf, which the desktop
 * checks every minute, so they ring even when Clock is closed.
 */
#include "zui.h"

enum { PAGE_WORLD, PAGE_ALARMS, PAGE_STOPWATCH, PAGE_TIMER };

typedef struct {
	int hour, min;
	u8 days;           /* bit 0 = Sunday ... bit 6 = Saturday; 0 = once */
	bool enabled;
	char label[64];
} Alarm;

typedef struct {
	int page;
	char cities[16][48];
	int ncities;
	Alarm alarms[32];
	int nalarms;
	/* stopwatch */
	bool sw_running;
	u64 sw_start_ns, sw_accum_ns;
	u64 laps[100];
	int nlaps;
	/* timer */
	bool tm_running, tm_paused, tm_done;
	u64 tm_end_ns, tm_total_ns, tm_left_ns;
	int tm_h, tm_m, tm_s;
	/* add-city dialog */
	StrBuf city_query;
	/* alarm editor */
	int edit_index;    /* -1 new */
	Alarm edit;
	StrBuf edit_label;
} Clock;

static Clock ck;

static const char *const DAY_SHORT[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* ---- Storage ------------------------------------------------------------------------- */

static char *config_path(const char *name)
{
	return str_fmt("%s/.config/zurbax/%s", home_dir(), name);
}

static void save_cities(void)
{
	char *path = config_path("clock.conf");
	char *dir = path_dir(path);
	dir_make(dir, 0755);
	mem_free(dir);
	Conf *c = conf_load(path);
	StrBuf sb = { 0 };
	for (int i = 0; i < ck.ncities; i++) {
		if (i)
			sb_addc(&sb, ',');
		sb_adds(&sb, ck.cities[i]);
	}
	conf_set(c, "world", sb_str(&sb));
	conf_save(c);
	conf_free(c);
	sb_free(&sb);
	mem_free(path);
}

static void load_cities(void)
{
	char *path = config_path("clock.conf");
	Conf *c = conf_load(path);
	mem_free(path);
	const char *list = conf_get(c, "world", "Europe/London,America/New_York,Asia/Tokyo");
	usize n;
	char **parts = str_split(list, ',', &n);
	ck.ncities = 0;
	for (usize i = 0; i < n && ck.ncities < (int)ARRAY_LEN(ck.cities); i++) {
		char *z = str_trim(parts[i]);
		if (tz_find(z))
			str_copy(ck.cities[ck.ncities++], z, sizeof ck.cities[0]);
	}
	str_split_free(parts, n);
	conf_free(c);
}

/* Shared format with the desktop: "HH:MM DAYS ENABLED label". */
static void save_alarms(void)
{
	char *path = config_path("alarms.conf");
	char *dir = path_dir(path);
	dir_make(dir, 0755);
	mem_free(dir);
	StrBuf sb = { 0 };
	sb_adds(&sb, "# Zurbax alarms: time, days (bit 0 = Sunday, 0 = once), enabled, label\n");
	for (int i = 0; i < ck.nalarms; i++) {
		Alarm *a = &ck.alarms[i];
		sb_fmt(&sb, "%02d:%02d %d %d %s\n", a->hour, a->min, a->days, a->enabled ? 1 : 0, a->label);
	}
	file_write_atomic(path, sb.data, sb.len);
	sb_free(&sb);
	mem_free(path);
}

static void load_alarms(void)
{
	char *path = config_path("alarms.conf");
	char *text = file_read(path, NULL);
	mem_free(path);
	ck.nalarms = 0;
	if (!text)
		return;
	usize n;
	char **lines = str_split(text, '\n', &n);
	for (usize i = 0; i < n && ck.nalarms < (int)ARRAY_LEN(ck.alarms); i++) {
		const char *p = str_trim(lines[i]);
		if (!*p || *p == '#')
			continue;
		Alarm a = { 0 };
		a.hour = (int)str_to_int(p, &p);
		if (*p == ':')
			p++;
		a.min = (int)str_to_int(p, &p);
		a.days = (u8)str_to_int(p, &p);
		a.enabled = str_to_int(p, &p) != 0;
		while (*p == ' ')
			p++;
		str_copy(a.label, p, sizeof a.label);
		ck.alarms[ck.nalarms++] = a;
	}
	str_split_free(lines, n);
	mem_free(text);
}

/* ---- Helpers ------------------------------------------------------------------------- */

static bool use_24h(void)
{
	char *path = config_path("settings.conf");
	Conf *c = conf_load(path);
	mem_free(path);
	bool v = conf_get_bool(c, "clock_24h", true);
	conf_free(c);
	return v;
}

static void format_hm(char *out, usize cap, int h, int m, bool h24)
{
	if (h24)
		fmt(out, cap, "%02d:%02d", h, m);
	else
		fmt(out, cap, "%d:%02d %s", h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
}

static void format_duration(char *out, usize cap, u64 ns, bool centis)
{
	u64 cs = ns / 10000000ull;
	u64 s = cs / 100, m = s / 60, h = m / 60;
	if (centis) {
		if (h)
			fmt(out, cap, "%d:%02d:%02d.%02d", (int)h, (int)(m % 60), (int)(s % 60), (int)(cs % 100));
		else
			fmt(out, cap, "%02d:%02d.%02d", (int)(m % 60), (int)(s % 60), (int)(cs % 100));
	} else {
		if (h)
			fmt(out, cap, "%d:%02d:%02d", (int)h, (int)(m % 60), (int)(s % 60));
		else
			fmt(out, cap, "%02d:%02d", (int)(m % 60), (int)(s % 60));
	}
}

static void wake_next_second(Ui *ui)
{
	i64 ms = time_unix_ms();
	f64 next = ui->now + (f64)(1000 - ms % 1000) / 1000.0 + 0.002;
	if (ui->wake_at == 0 || next < ui->wake_at)
		ui->wake_at = next;
}

/* ---- Analog clock ---------------------------------------------------------------------- */

static void analog_clock(Ui *ui, f32 cx, f32 cy, f32 r, const Civil *c, f64 frac_sec)
{
	Theme *t = &ui->theme;
	bool dark = t->dark;
	/* Bezel and face. */
	ui_shadow(ui, rect(cx - r, cy - r, 2 * r, 2 * r), r, 14, 5, color_hex("#00000040"));
	ui_circle(ui, cx, cy, r, dark ? color_hex("#3a3b40") : color_hex("#d9dbe0"));
	ui_circle(ui, cx, cy, r - 3, dark ? color_hex("#202125") : color_hex("#ffffff"));
	Path face = { 0 };
	path_circle(&face, cx, cy, r - 3);
	Rect fr = ui_to_device(ui, rect(cx - r, cy - r, 2 * r, 2 * r));
	Paint shade = paint_radial(fr.x + fr.w * 0.5f, fr.y + fr.h * 0.35f, fr.w * 0.6f,
				   dark ? color_hex("#2c2d32") : color_hex("#ffffff"), dark ? color_hex("#18191c") : color_hex("#eceef2"));
	fill_path(ui->canvas, &face, mat_scale(ui->scale, ui->scale), &shade, FILL_NONZERO);
	path_free(&face);
	ui_ring(ui, cx, cy, r - 3, 1, color_hex(dark ? "#00000080" : "#00000018"));
	/* Ticks: thick at the hours, fine at the minutes. */
	Color tick = dark ? color_hex("#c9cbd1") : color_hex("#3b3d43");
	for (int i = 0; i < 60; i++) {
		f32 a = (f32)i / 60 * TAU_F;
		bool hour = i % 5 == 0;
		f32 r0 = r - (hour ? 15 : 9), r1 = r - 7;
		ui_line(ui, cx + sinf(a) * r0, cy - cosf(a) * r0, cx + sinf(a) * r1, cy - cosf(a) * r1, hour ? 2.6f : 1,
			hour ? tick : color_scale_alpha(tick, 0.5f));
	}
	Font nf = ui_font(ui, r * 0.15f, 600);
	static const char *const nums[] = { "12", "3", "6", "9" };
	for (int i = 0; i < 4; i++) {
		f32 a = (f32)i / 4 * TAU_F, rr = r - 30;
		Rect nr = center_in(rect(cx + sinf(a) * rr - 20, cy - cosf(a) * rr - 20, 40, 40), 40, 40);
		ui_text_in(ui, nf, nr, nums[i], tick, ALIGN_CENTER);
	}
	f32 sec = (f32)c->sec + (f32)frac_sec;
	f32 min = (f32)c->min + sec / 60;
	f32 hr = (f32)(c->hour % 12) + min / 60;
	f32 ah = hr / 12 * TAU_F, am = min / 60 * TAU_F, as = sec / 60 * TAU_F;
	Color hand = dark ? color_hex("#eceef2") : color_hex("#26282d");
	Color hs = color_hex("#00000033");
	/* Hands cast a slight shadow down and to the right. */
	ui_line(ui, cx + 1.5f, cy + 2.5f, cx + 1.5f + sinf(ah) * r * 0.5f, cy + 2.5f - cosf(ah) * r * 0.5f, 6, hs);
	ui_line(ui, cx + 1.5f, cy + 2.5f, cx + 1.5f + sinf(am) * r * 0.74f, cy + 2.5f - cosf(am) * r * 0.74f, 4, hs);
	ui_line(ui, cx - sinf(ah) * 8, cy + cosf(ah) * 8, cx + sinf(ah) * r * 0.5f, cy - cosf(ah) * r * 0.5f, 6, hand);
	ui_line(ui, cx - sinf(am) * 10, cy + cosf(am) * 10, cx + sinf(am) * r * 0.74f, cy - cosf(am) * r * 0.74f, 4, hand);
	Color red = color_hex("#e0533d");
	ui_line(ui, cx - sinf(as) * 16, cy + cosf(as) * 16, cx + sinf(as) * r * 0.8f, cy - cosf(as) * r * 0.8f, 1.6f, red);
	ui_circle(ui, cx, cy, 5, red);
	ui_circle(ui, cx, cy, 2, dark ? color_hex("#202125") : COLOR_WHITE);
}

/* ---- World ---------------------------------------------------------------------------- */

static void add_city_dialog(Ui *ui)
{
	Rect c;
	if (!ui_dialog_begin(ui, "add-city", 380, 440, &c))
		return;
	Theme *t = &ui->theme;
	ui_heading(ui, cut_top(&c, 28), "Add a City");
	cut_top(&c, 6);
	ui_entry(ui, cut_top(&c, 32), "city-search", &ck.city_query, "Search for a city or country", ENTRY_SEARCH | ENTRY_AUTOFOCUS);
	cut_top(&c, 10);
	Rect buttons = cut_bottom(&c, 34);
	cut_bottom(&c, 10);
	usize nz;
	const ZoneInfo *zones = tz_zones(&nz);
	const char *q = sb_str(&ck.city_query);
	f32 y = ui_scroll_begin(ui, c, "city-list");
	i64 now = time_unix();
	for (usize i = 1; i < nz; i++) {
		const ZoneInfo *z = &zones[i];
		if (*q && !str_find_nocase(z->city, q) && !str_find_nocase(z->country, q) && !str_find_nocase(z->zone, q))
			continue;
		Rect row = rect(c.x, y, c.w, 44);
		y += 44;
		ui_push_id(ui, (i64)i);
		UiInteract it = ui_row(ui, row, ui_id(ui, "z"), false);
		ui_pop_id(ui);
		Rect in = pad2(row, 10, 4);
		ui_text(ui, ui_font(ui, 14, 500), in.x, in.y + 17, z->city, t->text);
		ui_text(ui, ui_font(ui, 12, 400), in.x, in.y + 33, z->country, t->text_dim);
		i32 off = tz_offset_in(z->zone, now);
		char o[24];
		int oh = off / 3600, om = ABS(off % 3600) / 60;
		if (om)
			fmt(o, sizeof o, "UTC%+d:%02d", oh, om);
		else
			fmt(o, sizeof o, "UTC%+d", oh);
		f32 ow = ui_text_width(ui, ui_font(ui, 12, 500), o);
		ui_text(ui, ui_font(ui, 12, 500), in.x + in.w - ow, in.y + 25, o, t->text_dim);
		if (it.clicked && ck.ncities < (int)ARRAY_LEN(ck.cities)) {
			bool have = false;
			for (int k = 0; k < ck.ncities; k++)
				have |= str_eq(ck.cities[k], z->zone);
			if (!have) {
				str_copy(ck.cities[ck.ncities++], z->zone, sizeof ck.cities[0]);
				save_cities();
			}
			ui_dialog_close(ui);
		}
	}
	ui_scroll_end(ui, y - c.y);
	if (ui_button(ui, cut_right(&buttons, 100), "Cancel"))
		ui_dialog_close(ui);
	ui_dialog_end(ui);
}

static void page_world(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	i64 now = time_unix();
	Civil local;
	civil_local(now, &local);
	bool h24 = use_24h();
	i32 local_off = tz_offset(now);

	/* Local time: an analog clock beside the digital time and date. */
	Rect top = cut_top(&r, 230);
	f32 rad = 92;
	analog_clock(ui, top.x + 40 + rad, top.y + top.h / 2, rad, &local, 0);
	Rect info = pad4(top, 0, 24, 0, 40 + rad * 2 + 36);
	char hm[32];
	format_hm(hm, sizeof hm, local.hour, local.min, h24);
	Font big = ui_font(ui, 54, 300);
	ui_text(ui, big, info.x, info.y + info.h / 2 + 8, hm, t->text);
	static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
					      "August", "September", "October", "November", "December" };
	static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
	char date[64];
	fmt(date, sizeof date, "%s, %d %s %d", days[local.wday], local.day, months[local.month - 1], local.year);
	ui_text(ui, ui_font(ui, 15, 400), info.x, info.y + info.h / 2 + 36, date, t->text_dim);
	const ZoneInfo *lz = tz_find(tz_name());
	char where[96];
	fmt(where, sizeof where, "%s", lz ? lz->city : tz_name());
	ui_text(ui, ui_font(ui, 13, 600), info.x, info.y + info.h / 2 - 50, where, t->accent);

	ui_separator(ui, r.x + 24, r.y, r.x + r.w - 24);
	cut_top(&r, 12);
	if (ck.ncities == 0) {
		ui_placeholder(ui, r, "globe", "No World Clocks", "Add cities to see their time at a glance");
		return;
	}
	f32 y = ui_scroll_begin(ui, r, "world-list");
	int remove = -1;
	for (int i = 0; i < ck.ncities; i++) {
		const ZoneInfo *z = tz_find(ck.cities[i]);
		if (!z)
			continue;
		Rect row = rect(r.x + 24, y, r.w - 48, 64);
		y += 72;
		ui_push_id(ui, i);
		ui_card(ui, row);
		Rect in = pad2(row, 16, 10);
		i32 off = tz_offset_in(z->zone, now);
		Civil c;
		civil_from_unix(now + off, &c);
		bool night = c.hour < 6 || c.hour >= 20;
		ui_icon(ui, rect(in.x, in.y + 12, 22, 22), night ? "dark-mode" : "sun", night ? t->text_dim : color_hex("#f0a030"));
		f32 tx = in.x + 36;
		ui_text(ui, ui_font(ui, 15, 600), tx, in.y + 18, z->city, t->text);
		/* "Today, 2 h ahead" style relation to local time. */
		i64 dl = (i64)(c.year * 400 + c.month * 32 + c.day) - (i64)(local.year * 400 + local.month * 32 + local.day);
		int diff = (off - local_off) / 60;
		char rel[64], d[24];
		if (diff == 0)
			str_copy(d, "same time", sizeof d);
		else if (diff % 60)
			fmt(d, sizeof d, "%d h %d min %s", ABS(diff) / 60, ABS(diff) % 60, diff > 0 ? "ahead" : "behind");
		else
			fmt(d, sizeof d, "%d h %s", ABS(diff) / 60, diff > 0 ? "ahead" : "behind");
		fmt(rel, sizeof rel, "%s · %s", dl > 0 ? "Tomorrow" : dl < 0 ? "Yesterday" : "Today", d);
		ui_text(ui, ui_font(ui, 12, 400), tx, in.y + 37, rel, t->text_dim);
		char tm[32];
		format_hm(tm, sizeof tm, c.hour, c.min, h24);
		Font tf = ui_font(ui, 28, 300);
		f32 tw = ui_text_width(ui, tf, tm);
		Rect del = cut_right(&in, 30);
		ui_text(ui, tf, in.x + in.w - tw - 10, in.y + 32, tm, t->text);
		if (ui_icon_button(ui, center_in(del, 28, 28), "remove", "Remove"))
			remove = i;
		ui_pop_id(ui);
	}
	ui_scroll_end(ui, y - r.y);
	if (remove >= 0) {
		memmove(ck.cities[remove], ck.cities[remove + 1], sizeof ck.cities[0] * (usize)(ck.ncities - remove - 1));
		ck.ncities--;
		save_cities();
	}
	wake_next_second(ui);
}

/* ---- Alarms ----------------------------------------------------------------------------- */

static void open_alarm_editor(Ui *ui, int index)
{
	ck.edit_index = index;
	if (index >= 0) {
		ck.edit = ck.alarms[index];
	} else {
		ck.edit = (Alarm){ .hour = 7, .min = 0, .days = 0x3e, .enabled = true };
	}
	sb_clear(&ck.edit_label);
	sb_adds(&ck.edit_label, ck.edit.label);
	ui_dialog_open(ui, "alarm-edit");
}

static void alarm_dialog(Ui *ui)
{
	Rect c;
	if (!ui_dialog_begin(ui, "alarm-edit", 400, 300, &c))
		return;
	Theme *t = &ui->theme;
	ui_heading(ui, cut_top(&c, 28), ck.edit_index >= 0 ? "Edit Alarm" : "New Alarm");
	cut_top(&c, 12);
	Rect tr = cut_top(&c, 44);
	Rect spins = center_in(tr, 250, 40);
	ui_spin(ui, cut_left(&spins, 115), "alarm-h", &ck.edit.hour, 0, 23);
	ui_text_in(ui, ui_font(ui, 24, 300), cut_left(&spins, 20), ":", t->text, ALIGN_CENTER);
	ui_spin(ui, cut_left(&spins, 115), "alarm-m", &ck.edit.min, 0, 59);
	cut_top(&c, 16);
	ui_label_dim(ui, cut_top(&c, 20), "Repeat");
	cut_top(&c, 4);
	Rect dr = cut_top(&c, 34);
	f32 dw = (dr.w - 6 * 6) / 7;
	for (int i = 0; i < 7; i++) {
		int d = (i + 1) % 7; /* Monday first */
		Rect b = rect(dr.x + (f32)i * (dw + 6), dr.y, dw, dr.h);
		ui_push_id(ui, i);
		bool on = ck.edit.days & (1u << d);
		UiInteract it = ui_interact(ui, ui_id(ui, "day"), b);
		ui_round(ui, b, b.h / 2, on ? color_with_alpha(t->accent, 0x30) : it.hovered ? t->hover : t->card);
		ui_border(ui, b, b.h / 2, 1, on ? color_with_alpha(t->accent, 0x90) : t->border);
		if (it.clicked)
			ck.edit.days ^= (u8)(1u << d);
		ui_text_in(ui, ui_font(ui, 12, on ? 700 : 500), b, DAY_SHORT[d], on ? t->accent : t->text_dim, ALIGN_CENTER);
		ui_pop_id(ui);
	}
	cut_top(&c, 14);
	ui_entry(ui, cut_top(&c, 32), "alarm-label", &ck.edit_label, "Label (optional)", 0);
	Rect buttons = cut_bottom(&c, 34);
	if (ck.edit_index >= 0 && ui_button_destructive(ui, cut_left(&buttons, 90), "Delete")) {
		memmove(&ck.alarms[ck.edit_index], &ck.alarms[ck.edit_index + 1], sizeof(Alarm) * (usize)(ck.nalarms - ck.edit_index - 1));
		ck.nalarms--;
		save_alarms();
		ui_dialog_close(ui);
	}
	if (ui_button_suggested(ui, cut_right(&buttons, 90), "Save")) {
		str_copy(ck.edit.label, sb_str(&ck.edit_label), sizeof ck.edit.label);
		ck.edit.enabled = true;
		if (ck.edit_index >= 0)
			ck.alarms[ck.edit_index] = ck.edit;
		else if (ck.nalarms < (int)ARRAY_LEN(ck.alarms))
			ck.alarms[ck.nalarms++] = ck.edit;
		save_alarms();
		ui_dialog_close(ui);
	}
	cut_right(&buttons, 8);
	if (ui_button(ui, cut_right(&buttons, 90), "Cancel"))
		ui_dialog_close(ui);
	ui_dialog_end(ui);
}

static void describe_days(u8 days, char *out, usize cap)
{
	if (days == 0)
		str_copy(out, "Once", cap);
	else if (days == 0x7f)
		str_copy(out, "Every day", cap);
	else if (days == 0x3e)
		str_copy(out, "Weekdays", cap);
	else if (days == 0x41)
		str_copy(out, "Weekends", cap);
	else {
		StrBuf sb = { 0 };
		for (int i = 1; i <= 7; i++) {
			int d = i % 7;
			if (days & (1u << d)) {
				if (sb.len)
					sb_adds(&sb, ", ");
				sb_adds(&sb, DAY_SHORT[d]);
			}
		}
		str_copy(out, sb_str(&sb), cap);
		sb_free(&sb);
	}
}

static void page_alarms(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	if (ck.nalarms == 0) {
		Rect p = r;
		ui_placeholder(ui, cut_top(&p, p.h - 80), "alarm", "No Alarms", "Alarms ring even when Clock is closed");
		if (ui_button_suggested(ui, center_in(cut_top(&p, 40), 140, 34), "Add Alarm"))
			open_alarm_editor(ui, -1);
		return;
	}
	bool h24 = use_24h();
	cut_top(&r, 12);
	f32 y = ui_scroll_begin(ui, r, "alarm-list");
	for (int i = 0; i < ck.nalarms; i++) {
		Alarm *a = &ck.alarms[i];
		Rect row = rect(r.x + 24, y, r.w - 48, 76);
		y += 84;
		ui_push_id(ui, i);
		ui_card(ui, row);
		UiInteract it = ui_interact(ui, ui_id(ui, "row"), row);
		Rect in = pad2(row, 18, 10);
		char tm[32];
		format_hm(tm, sizeof tm, a->hour, a->min, h24);
		Color tc = a->enabled ? t->text : t->text_faint;
		ui_text(ui, ui_font(ui, 34, 300), in.x, in.y + 38, tm, tc);
		char days[64], sub[140];
		describe_days(a->days, days, sizeof days);
		if (a->label[0])
			fmt(sub, sizeof sub, "%s · %s", a->label, days);
		else
			str_copy(sub, days, sizeof sub);
		ui_text(ui, ui_font(ui, 12, 400), in.x, in.y + 56, sub, t->text_dim);
		Rect sw = cut_right(&in, 50);
		bool en = a->enabled;
		if (ui_switch(ui, center_in(sw, 46, 26), &en)) {
			a->enabled = en;
			save_alarms();
		} else if (it.clicked) {
			open_alarm_editor(ui, i);
		}
		ui_pop_id(ui);
	}
	ui_scroll_end(ui, y - r.y);
}

/* ---- Stopwatch ---------------------------------------------------------------------------- */

static u64 sw_elapsed(void)
{
	return ck.sw_accum_ns + (ck.sw_running ? time_mono_ns() - ck.sw_start_ns : 0);
}

static void page_stopwatch(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	char buf[32];
	u64 el = sw_elapsed();
	format_duration(buf, sizeof buf, el, true);
	Rect top = cut_top(&r, 170);
	Font f = ui_font(ui, 64, 200);
	ui_text_in(ui, f, cut_top(&top, 110), buf, t->text, ALIGN_CENTER);
	Rect btns = center_in(cut_top(&top, 44), 300, 40);
	Rect b1 = cut_left(&btns, 145);
	cut_left(&btns, 10);
	if (!ck.sw_running) {
		if (ui_button_suggested(ui, b1, el ? "Resume" : "Start")) {
			ck.sw_running = true;
			ck.sw_start_ns = time_mono_ns();
		}
		if (ui_button(ui, btns, "Reset") && el) {
			ck.sw_accum_ns = 0;
			ck.nlaps = 0;
		}
	} else {
		if (ui_button_destructive(ui, b1, "Stop")) {
			ck.sw_accum_ns = sw_elapsed();
			ck.sw_running = false;
		}
		if (ui_button(ui, btns, "Lap") && ck.nlaps < (int)ARRAY_LEN(ck.laps))
			ck.laps[ck.nlaps++] = el;
	}
	if (ck.sw_running)
		ui->redraw = true;
	if (ck.nlaps == 0)
		return;
	/* Laps: newest first, with the fastest and slowest marked. */
	cut_top(&r, 10);
	Rect list = pad2(r, 60, 0);
	u64 best = ~0ull, worst = 0;
	for (int i = 0; i < ck.nlaps; i++) {
		u64 d = ck.laps[i] - (i ? ck.laps[i - 1] : 0);
		best = MIN(best, d);
		worst = MAX(worst, d);
	}
	f32 y = ui_scroll_begin(ui, list, "laps");
	for (int i = ck.nlaps - 1; i >= 0; i--) {
		Rect row = rect(list.x, y, list.w, 40);
		y += 40;
		u64 d = ck.laps[i] - (i ? ck.laps[i - 1] : 0);
		char n[16], dd[32], tot[32];
		fmt(n, sizeof n, "Lap %d", i + 1);
		format_duration(dd, sizeof dd, d, true);
		format_duration(tot, sizeof tot, ck.laps[i], true);
		Color c = ck.nlaps > 2 && d == best ? t->success : ck.nlaps > 2 && d == worst ? t->danger : t->text;
		ui_text(ui, ui_font(ui, 14, 500), row.x + 8, row.y + 25, n, t->text_dim);
		ui_text_in(ui, ui_font(ui, 15, 500), row, dd, c, ALIGN_CENTER);
		Font tf = ui_font(ui, 14, 400);
		ui_text(ui, tf, row.x + row.w - 8 - ui_text_width(ui, tf, tot), row.y + 25, tot, t->text_dim);
		ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
	}
	ui_scroll_end(ui, y - list.y);
}

/* ---- Timer -------------------------------------------------------------------------------- */

static void start_timer(u64 total)
{
	if (total == 0)
		return;
	ck.tm_total_ns = total;
	ck.tm_end_ns = time_mono_ns() + total;
	ck.tm_running = true;
	ck.tm_paused = false;
	ck.tm_done = false;
}

static void page_timer(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	if (ck.tm_running) {
		u64 now = time_mono_ns();
		u64 left = ck.tm_paused ? ck.tm_left_ns : (ck.tm_end_ns > now ? ck.tm_end_ns - now : 0);
		f32 frac = ck.tm_total_ns ? (f32)((f64)left / (f64)ck.tm_total_ns) : 0;
		Rect top = cut_top(&r, r.h - 70);
		f32 rad = minf(top.w, top.h) / 2 - 20;
		f32 cx = top.x + top.w / 2, cy = top.y + top.h / 2;
		ui_ring(ui, cx, cy, rad, 10, t->border);
		ui_arc(ui, cx, cy, rad, -PI_F / 2, -PI_F / 2 + TAU_F * frac, 10, ck.tm_paused ? t->text_faint : t->accent);
		char buf[32];
		format_duration(buf, sizeof buf, left + 999999999ull, false);
		ui_text_in(ui, ui_font(ui, rad * 0.42f, 200), rect(cx - rad, cy - 40, rad * 2, 60), buf, t->text, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(cx - rad, cy + 24, rad * 2, 20), ck.tm_paused ? "Paused" : "Remaining",
			   t->text_dim, ALIGN_CENTER);
		Rect btns = center_in(r, 300, 38);
		Rect b1 = cut_left(&btns, 145);
		cut_left(&btns, 10);
		if (ck.tm_paused) {
			if (ui_button_suggested(ui, b1, "Resume")) {
				ck.tm_end_ns = time_mono_ns() + ck.tm_left_ns;
				ck.tm_paused = false;
			}
		} else if (ui_button(ui, b1, "Pause")) {
			ck.tm_left_ns = left;
			ck.tm_paused = true;
		}
		if (ui_button_destructive(ui, btns, "Cancel"))
			ck.tm_running = false;
		if (!ck.tm_paused)
			ui->redraw = true;
		return;
	}
	if (ck.tm_done) {
		Rect p = cut_top(&r, r.h - 80);
		ui_placeholder(ui, p, "timer", "Time's Up", "Your timer has finished");
		if (ui_button_suggested(ui, center_in(r, 140, 34), "Done"))
			ck.tm_done = false;
		return;
	}
	/* Presets, then a custom duration. */
	Rect body = pad2(r, 40, 20);
	ui_heading(ui, cut_top(&body, 26), "Quick Timers");
	cut_top(&body, 8);
	static const int presets[] = { 1, 3, 5, 10, 15, 30, 45, 60 };
	Rect grid = cut_top(&body, 120);
	for (int i = 0; i < 8; i++) {
		Rect cell = grid_cell(grid, 4, 2, i, 10);
		char label[16];
		if (presets[i] < 60)
			fmt(label, sizeof label, "%d min", presets[i]);
		else
			str_copy(label, "1 hour", sizeof label);
		ui_push_id(ui, i);
		if (ui_button(ui, cell, label))
			start_timer((u64)presets[i] * 60 * 1000000000ull);
		ui_pop_id(ui);
	}
	cut_top(&body, 24);
	ui_heading(ui, cut_top(&body, 26), "Custom");
	cut_top(&body, 8);
	Rect row = cut_top(&body, 40);
	ui_spin(ui, cut_left(&row, 110), "tm-h", &ck.tm_h, 0, 23);
	ui_label_dim(ui, cut_left(&row, 40), " h");
	ui_spin(ui, cut_left(&row, 110), "tm-m", &ck.tm_m, 0, 59);
	ui_label_dim(ui, cut_left(&row, 50), " min");
	ui_spin(ui, cut_left(&row, 110), "tm-s", &ck.tm_s, 0, 59);
	ui_label_dim(ui, cut_left(&row, 40), " s");
	cut_top(&body, 16);
	u64 total = ((u64)ck.tm_h * 3600 + (u64)ck.tm_m * 60 + (u64)ck.tm_s) * 1000000000ull;
	Rect start = cut_top(&body, 36);
	if (ui_button_suggested(ui, cut_left(&start, 140), "Start") && total)
		start_timer(total);
}

/* ---- Frame -------------------------------------------------------------------------------- */

static void check_timer(Ui *ui)
{
	if (!ck.tm_running || ck.tm_paused)
		return;
	u64 now = time_mono_ns();
	if (now >= ck.tm_end_ns) {
		ck.tm_running = false;
		ck.tm_done = true;
		ck.page = PAGE_TIMER;
		ui_notify(ui, "Time's up", "Your timer has finished.", "timer");
		ui_window_action(ui, ACTION_RAISE);
	} else {
		f64 at = ui->now + (f64)(ck.tm_end_ns - now) / 1e9;
		if (ui->wake_at == 0 || at < ui->wake_at)
			ui->wake_at = at;
	}
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	check_timer(ui);
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	Rect tools = ui_headerbar(ui, header, "Clock", HEADER_NO_TITLE);
	static const char *const pages[] = { "World", "Alarms", "Stopwatch", "Timer" };
	ui_segmented(ui, center_in(tools, 360, 30), pages, 4, &ck.page);
	if (ck.page == PAGE_WORLD || ck.page == PAGE_ALARMS) {
		Rect add = cut_right(&tools, 34);
		if (ui_icon_button(ui, add, "add", ck.page == PAGE_WORLD ? "Add City" : "Add Alarm")) {
			if (ck.page == PAGE_WORLD) {
				sb_clear(&ck.city_query);
				ui_dialog_open(ui, "add-city");
			} else {
				open_alarm_editor(ui, -1);
			}
		}
	}
	ui_fill(ui, win, t->view);
	switch (ck.page) {
	case PAGE_WORLD: page_world(ui, win); break;
	case PAGE_ALARMS: page_alarms(ui, win); break;
	case PAGE_STOPWATCH: page_stopwatch(ui, win); break;
	case PAGE_TIMER: page_timer(ui, win); break;
	}
	add_city_dialog(ui);
	alarm_dialog(ui);
}

static void init(Ui *ui)
{
	UNUSED(ui);
	load_cities();
	load_alarms();
	ck.tm_m = 5;
}

int main(int argc, char **argv)
{
	if (argc > 1 && str_eq(argv[1], "--timer"))
		ck.page = PAGE_TIMER;
	else if (argc > 1 && str_eq(argv[1], "--alarms"))
		ck.page = PAGE_ALARMS;
	UiAppDesc d = { .id = "clock", .title = "Clock", .width = 640, .height = 600, .min_width = 520, .min_height = 480,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
