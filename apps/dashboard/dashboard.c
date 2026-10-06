/*
 * Dashboard — your day at a glance.
 *
 * A greeting and the time over a sky that follows the hour, then cards
 * gathered from the other apps: the weather, today's calendar, screen
 * time, the computer's health, recent notes and files, and your best game
 * scores. Each card opens its app.
 */
#include "zui.h"

typedef struct {
	char title[96];
	int sh, sm;
	bool allday;
	Color color;
} DayEvent;

typedef struct {
	char name[64];
	i64 seconds;
} Usage;

typedef struct {
	char name[160], path[512];
	i64 mtime;
} Recent;

static struct {
	/* weather */
	bool have_weather;
	char place[96], condition[64], icon[32];
	f32 temp, hi, lo;
	/* calendar */
	DayEvent events[12];
	int nevents;
	/* screen time */
	Usage usage[32];
	int nusage;
	i64 usage_total;
	/* system */
	f32 cpu, mem_used, mem_total, disk_free, disk_total;
	int battery;
	bool charging;
	i64 uptime;
	u64 cpu_prev_total, cpu_prev_idle;
	/* notes and files */
	Recent notes[3], files[5];
	int nnotes, nfiles;
	/* arcade */
	int best[5];
	f64 next_refresh;
} db;

static const u32 EVENT_COLORS[] = { 0x3689e6, 0x28bca3, 0x68b723, 0xf9c440, 0xf37329, 0xc6262e, 0xa56de2, 0x8a715e };

/* ---- Gathering ---------------------------------------------------------------------------------- */

static const char *weather_icon(int code, bool day)
{
	if (code <= 1)
		return day ? "sun" : "dark-mode";
	if (code == 2)
		return "cloud-sun";
	if (code == 45 || code == 48)
		return "fog";
	if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
		return "rain";
	if ((code >= 71 && code <= 77) || code == 85 || code == 86)
		return "snow";
	if (code >= 95)
		return "storm";
	return "cloud";
}

static const char *weather_text(int code)
{
	if (code <= 1)
		return "Clear";
	if (code == 2)
		return "Partly cloudy";
	if (code == 3)
		return "Overcast";
	if (code == 45 || code == 48)
		return "Fog";
	if (code >= 51 && code <= 57)
		return "Drizzle";
	if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82))
		return "Rain";
	if ((code >= 71 && code <= 77) || code == 85 || code == 86)
		return "Snow";
	if (code >= 95)
		return "Thunderstorm";
	return "Cloudy";
}

static void load_weather(void)
{
	char *conf = str_fmt("%s/.config/zurbax/weather.conf", home_dir());
	char *cache = str_fmt("%s/.cache/zurbax/weather.json", home_dir());
	Conf *c = conf_load(conf);
	str_copy(db.place, conf_get(c, "name", ""), sizeof db.place);
	bool fahrenheit = conf_get_bool(c, "fahrenheit", false);
	conf_free(c);
	usize len;
	char *text = file_read(cache, &len);
	db.have_weather = false;
	Json *j = text ? json_parse(text, len) : NULL;
	if (j && db.place[0]) {
		Json *cur = json_get(j, "current");
		int code = (int)json_num(json_get(cur, "weather_code"), 3);
		bool day = json_num(json_get(cur, "is_day"), 1) != 0;
		f32 k = fahrenheit ? 1.8f : 1, off = fahrenheit ? 32 : 0;
		db.temp = (f32)json_num(json_get(cur, "temperature_2m"), 0) * k + off;
		db.hi = (f32)json_num(json_at(json_path(j, "daily.temperature_2m_max"), 0), 0) * k + off;
		db.lo = (f32)json_num(json_at(json_path(j, "daily.temperature_2m_min"), 0), 0) * k + off;
		str_copy(db.condition, weather_text(code), sizeof db.condition);
		str_copy(db.icon, weather_icon(code, day), sizeof db.icon);
		db.have_weather = true;
	}
	json_free(j);
	mem_free(text);
	mem_free(conf);
	mem_free(cache);
}

static bool occurs_today(Json *e, const Civil *today)
{
	int y = 0, m = 0, d = 0;
	const char *date = json_str(json_get(e, "date"), "");
	if (strlen(date) >= 10) {
		y = (int)str_to_int(date, NULL);
		m = (int)str_to_int(date + 5, NULL);
		d = (int)str_to_int(date + 8, NULL);
	}
	Civil a = { .year = y, .month = m, .day = d }, b = { .year = today->year, .month = today->month, .day = today->day };
	i64 base = unix_from_civil(&a) / 86400, t = unix_from_civil(&b) / 86400;
	if (t < base)
		return false;
	const char *rep = json_str(json_get(e, "repeat"), "none");
	if (str_eq(rep, "daily"))
		return true;
	if (str_eq(rep, "weekly"))
		return (t - base) % 7 == 0;
	if (str_eq(rep, "monthly"))
		return today->day == d;
	if (str_eq(rep, "yearly"))
		return today->month == m && today->day == d;
	return t == base;
}

static void load_calendar(void)
{
	char *path = str_fmt("%s/.local/share/zurbax/calendar.json", home_dir());
	usize len;
	char *text = file_read(path, &len);
	mem_free(path);
	db.nevents = 0;
	Json *j = text ? json_parse(text, len) : NULL;
	Civil today;
	civil_local(time_unix(), &today);
	for (usize i = 0; j && i < j->count && db.nevents < (int)ARRAY_LEN(db.events); i++) {
		Json *e = json_at(j, i);
		if (!occurs_today(e, &today))
			continue;
		DayEvent *de = &db.events[db.nevents++];
		str_copy(de->title, json_str(json_get(e, "title"), "Event"), sizeof de->title);
		de->allday = json_bool(json_get(e, "allday"), false);
		const char *st = json_str(json_get(e, "start"), "00:00");
		de->sh = (int)str_to_int(st, NULL);
		de->sm = strlen(st) >= 5 ? (int)str_to_int(st + 3, NULL) : 0;
		de->color = 0xff000000u | EVENT_COLORS[(usize)json_num(json_get(e, "color"), 0) % ARRAY_LEN(EVENT_COLORS)];
	}
	/* All-day first, then by time. */
	for (int a = 1; a < db.nevents; a++)
		for (int b = a; b > 0; b--) {
			DayEvent *x = &db.events[b - 1], *y = &db.events[b];
			int kx = x->allday ? -1 : x->sh * 60 + x->sm, ky = y->allday ? -1 : y->sh * 60 + y->sm;
			if (kx <= ky)
				break;
			DayEvent t = *x;
			*x = *y;
			*y = t;
		}
	json_free(j);
	mem_free(text);
}

static void load_screentime(void)
{
	Civil c;
	civil_local(time_unix(), &c);
	char *path = str_fmt("%s/.local/share/zurbax/screentime/%04d-%02d-%02d.conf", home_dir(), c.year, c.month, c.day);
	char *text = file_read(path, NULL);
	mem_free(path);
	db.nusage = 0;
	db.usage_total = 0;
	usize n;
	char **lines = text ? str_split(text, '\n', &n) : NULL;
	for (usize i = 0; lines && i < n && db.nusage < (int)ARRAY_LEN(db.usage); i++) {
		char *eq = (char *)str_chr(lines[i], '=');
		if (!eq)
			continue;
		*eq = 0;
		Usage *u = &db.usage[db.nusage++];
		/* The app's own name from its app.conf, rather than its id. */
		const char *id = str_trim(lines[i]);
		char *rel = str_fmt("apps/%s/app.conf", id);
		char *conf_path = zurbax_path(rel);
		Conf *ac = conf_load(conf_path);
		str_copy(u->name, conf_get(ac, "name", id), sizeof u->name);
		conf_free(ac);
		mem_free(conf_path);
		mem_free(rel);
		u->seconds = str_to_int(str_trim(eq + 1), NULL);
		db.usage_total += u->seconds;
	}
	if (lines)
		str_split_free(lines, n);
	mem_free(text);
	for (int a = 1; a < db.nusage; a++)
		for (int b = a; b > 0 && db.usage[b - 1].seconds < db.usage[b].seconds; b--) {
			Usage t = db.usage[b - 1];
			db.usage[b - 1] = db.usage[b];
			db.usage[b] = t;
		}
}

static void load_system(void)
{
	char *stat = file_read("/proc/stat", NULL);
	if (stat && str_starts(stat, "cpu ")) {
		u64 v[10] = { 0 }, total = 0;
		const char *p = stat + 4;
		for (int i = 0; i < 10; i++) {
			v[i] = (u64)str_to_int(p, &p);
			total += v[i];
		}
		u64 idle = v[3] + v[4];
		if (db.cpu_prev_total && total > db.cpu_prev_total)
			db.cpu = 1 - (f32)(idle - db.cpu_prev_idle) / (f32)(total - db.cpu_prev_total);
		db.cpu_prev_total = total, db.cpu_prev_idle = idle;
	}
	mem_free(stat);
	char *mi = file_read("/proc/meminfo", NULL);
	i64 total = 0, avail = 0;
	for (const char *p = mi; p && *p;) {
		if (str_starts(p, "MemTotal:"))
			total = str_to_int(p + 9, NULL);
		else if (str_starts(p, "MemAvailable:"))
			avail = str_to_int(p + 13, NULL);
		p = str_chr(p, '\n');
		if (p)
			p++;
	}
	mem_free(mi);
	db.mem_total = (f32)total / 1048576.0f;
	db.mem_used = (f32)(total - avail) / 1048576.0f;
	struct statfs sf;
	if (sys_statfs(home_dir(), &sf) == 0) {
		db.disk_total = (f32)((f64)sf.f_blocks * (f64)sf.f_bsize / 1e9);
		db.disk_free = (f32)((f64)sf.f_bavail * (f64)sf.f_bsize / 1e9);
	}
	db.battery = -1;
	DirList ps = { 0 };
	dir_list("/sys/class/power_supply", &ps, false);
	for (usize i = 0; i < ps.len; i++) {
		char *t = str_fmt("/sys/class/power_supply/%s/type", ps.data[i].name);
		char *type = file_read_line(t);
		mem_free(t);
		if (type && str_eq(type, "Battery")) {
			char *cap = str_fmt("/sys/class/power_supply/%s/capacity", ps.data[i].name);
			char *st = str_fmt("/sys/class/power_supply/%s/status", ps.data[i].name);
			db.battery = (int)file_read_int(cap, -1);
			char *status = file_read_line(st);
			db.charging = status && (str_eq(status, "Charging") || str_eq(status, "Full"));
			mem_free(status);
			mem_free(cap);
			mem_free(st);
		}
		mem_free(type);
	}
	dir_list_free(&ps);
	struct sysinfo si;
	if (sys_sysinfo(&si) == 0)
		db.uptime = si.uptime;
}

static void add_recent(Recent *list, int *n, int max, const char *name, const char *path, i64 mtime)
{
	int at = *n;
	while (at > 0 && list[at - 1].mtime < mtime)
		at--;
	if (at >= max)
		return;
	int last = MIN(*n, max - 1);
	for (int i = last; i > at; i--)
		list[i] = list[i - 1];
	str_copy(list[at].name, name, sizeof list[at].name);
	str_copy(list[at].path, path, sizeof list[at].path);
	list[at].mtime = mtime;
	if (*n < max)
		(*n)++;
}

static void load_notes_and_files(void)
{
	db.nnotes = db.nfiles = 0;
	char *dir = str_fmt("%s/.local/share/zurbax/notes", home_dir());
	DirList l = { 0 };
	if (dir_list(dir, &l, true))
		for (usize i = 0; i < l.len; i++) {
			if (S_ISDIR(l.data[i].mode))
				continue;
			char *p = path_join(dir, l.data[i].name);
			char *first = file_read_line(p);
			add_recent(db.notes, &db.nnotes, 3, first && first[0] ? first : "Untitled note", p, l.data[i].mtime);
			mem_free(first);
			mem_free(p);
		}
	dir_list_free(&l);
	mem_free(dir);
	static const char *const folders[] = { "Documents", "Pictures", "Downloads", "Music", "Desktop" };
	for (usize f = 0; f < ARRAY_LEN(folders); f++) {
		char *d = path_join(home_dir(), folders[f]);
		DirList fl = { 0 };
		if (dir_list(d, &fl, true))
			for (usize i = 0; i < fl.len; i++) {
				if (S_ISDIR(fl.data[i].mode) || fl.data[i].name[0] == '.')
					continue;
				char *p = path_join(d, fl.data[i].name);
				add_recent(db.files, &db.nfiles, 5, fl.data[i].name, p, fl.data[i].mtime);
				mem_free(p);
			}
		dir_list_free(&fl);
		mem_free(d);
	}
}

static void load_arcade(void)
{
	char *path = str_fmt("%s/.local/share/zurbax/arcade.conf", home_dir());
	Conf *c = conf_load(path);
	static const char *const ids[] = { "blocks", "snake", "2048", "mines", "breakout" };
	for (int i = 0; i < 5; i++)
		db.best[i] = (int)conf_get_int(c, ids[i], 0);
	conf_free(c);
	mem_free(path);
}

static void refresh(Ui *ui)
{
	load_weather();
	load_calendar();
	load_screentime();
	load_system();
	load_notes_and_files();
	load_arcade();
	db.next_refresh = ui->now + 5;
}

/* ---- Drawing -------------------------------------------------------------------------------------- */

static void fmt_duration(char *out, usize cap, i64 s)
{
	if (s >= 3600)
		fmt(out, cap, "%lldh %lldm", (long long)(s / 3600), (long long)(s / 60 % 60));
	else
		fmt(out, cap, "%lldm", (long long)(s / 60));
}

/* A card; returns true when it's clicked (to open its app). */
static bool card(Ui *ui, Rect r, const char *id, const char *icon, const char *title, Rect *content)
{
	Theme *t = &ui->theme;
	UiInteract it = ui_interact(ui, ui_id(ui, id), r);
	f32 h = ui_fade(ui, ui_id(ui, id), it.hovered, 0.15f);
	ui_shadow(ui, r, 14, 18 + h * 8, 4 + h * 2, color_hex(t->dark ? "#00000060" : "#00000022"));
	ui_round(ui, r, 14, t->card);
	ui_border(ui, r, 14, 1, t->border);
	Rect head = rect(r.x + 18, r.y + 14, r.w - 36, 22);
	ui_icon(ui, rect(head.x, head.y + 2, 16, 16), icon, t->accent);
	ui_text_in(ui, ui_font(ui, 12, 700), rect(head.x + 24, head.y, head.w - 24, 20), title, t->text_dim, ALIGN_START);
	*content = pad4(r, 44, 18, 16, 18);
	return it.clicked;
}

static void weather_card(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect c;
	if (card(ui, r, "weather", "cloud-sun", db.have_weather ? db.place : "WEATHER", &c))
		ui_launch(ui, "weather", NULL);
	if (!db.have_weather) {
		ui_text_wrapped(ui, ui_font(ui, 14, 400), c, "Open Weather once to see the forecast here.", t->text_dim, ALIGN_START);
		return;
	}
	ui_icon(ui, rect(c.x + c.w - 64, c.y + 4, 56, 56), db.icon, t->accent);
	char temp[16];
	fmt(temp, sizeof temp, "%.0f°", (double)db.temp);
	ui_text_in(ui, ui_font(ui, 48, 300), rect(c.x, c.y, c.w - 70, 58), temp, t->text, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 15, 600), rect(c.x, c.y + 62, c.w, 22), db.condition, t->text, ALIGN_START);
	char hl[48];
	fmt(hl, sizeof hl, "High %.0f°  ·  Low %.0f°", (double)db.hi, (double)db.lo);
	ui_text_in(ui, ui_font(ui, 13, 400), rect(c.x, c.y + 86, c.w, 20), hl, t->text_dim, ALIGN_START);
}

static void today_card(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect c;
	if (card(ui, r, "today", "calendar", "TODAY", &c))
		ui_launch(ui, "calendar", NULL);
	if (!db.nevents) {
		ui_text_in(ui, ui_font(ui, 16, 600), cut_top(&c, 26), "Nothing planned", t->text, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 13, 400), cut_top(&c, 20), "Your day is free.", t->text_dim, ALIGN_START);
		return;
	}
	for (int i = 0; i < db.nevents && c.h >= 30; i++) {
		DayEvent *e = &db.events[i];
		Rect row = cut_top(&c, 34);
		ui_round(ui, rect(row.x, row.y + 6, 4, 22), 2, e->color);
		char when[16];
		if (e->allday)
			str_copy(when, "All day", sizeof when);
		else
			fmt(when, sizeof when, "%02d:%02d", e->sh, e->sm);
		ui_text_in(ui, ui_font(ui, 13, 600), rect(row.x + 14, row.y, 64, 34), when, t->text_dim, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 14, 500), rect(row.x + 80, row.y, row.w - 80, 34), e->title, t->text, ALIGN_START);
	}
}

static void screentime_card(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect c;
	if (card(ui, r, "screentime", "hourglass", "SCREEN TIME TODAY", &c))
		ui_launch(ui, "screentime", NULL);
	char total[32];
	fmt_duration(total, sizeof total, db.usage_total);
	ui_text_in(ui, ui_font(ui, 30, 700), cut_top(&c, 40), db.usage_total ? total : "0m", t->text, ALIGN_START);
	cut_top(&c, 4);
	static const char *const bar_colors[] = { "#3689e6", "#28bca3", "#f9c440" };
	for (int i = 0; i < MIN(db.nusage, 3); i++) {
		Rect row = cut_top(&c, 30);
		char d[32];
		fmt_duration(d, sizeof d, db.usage[i].seconds);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(row.x, row.y, row.w * 0.4f, 18), db.usage[i].name, t->text, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 12, 500), rect(row.x, row.y, row.w, 18), d, t->text_dim, ALIGN_END);
		Rect bar = rect(row.x, row.y + 20, row.w, 5);
		ui_round(ui, bar, 2.5f, t->hover);
		f32 k = db.usage_total ? (f32)db.usage[i].seconds / (f32)db.usage[0].seconds : 0;
		ui_round(ui, rect(bar.x, bar.y, maxf(5, bar.w * k), bar.h), 2.5f, color_hex(bar_colors[i]));
	}
}

static void meter(Ui *ui, Rect r, const char *label, f32 k, const char *value, Color c)
{
	Theme *t = &ui->theme;
	f32 cx = r.x + r.w / 2, cy = r.y + 38, rad = 30;
	ui_ring(ui, cx, cy, rad, 7, t->hover);
	if (k > 0.005f)
		ui_arc(ui, cx, cy, rad, -PI / 2, -PI / 2 + 2 * PI * clampf(k, 0, 1), 7, c);
	ui_text_in(ui, ui_font(ui, 13, 700), rect(r.x, cy - 10, r.w, 20), value, t->text, ALIGN_CENTER);
	ui_text_in(ui, ui_font(ui, 12, 500), rect(r.x, cy + rad + 10, r.w, 18), label, t->text_dim, ALIGN_CENTER);
}

static void system_card(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect c;
	if (card(ui, r, "system", "activity", "THIS COMPUTER", &c))
		ui_launch(ui, "pulse", NULL);
	int n = db.battery >= 0 ? 4 : 3;
	f32 w = c.w / (f32)n;
	char v[32];
	fmt(v, sizeof v, "%.0f%%", (double)(db.cpu * 100));
	meter(ui, rect(c.x, c.y, w, c.h), "Processor", db.cpu, v, color_hex("#3689e6"));
	fmt(v, sizeof v, "%.1f GB", (double)db.mem_used);
	meter(ui, rect(c.x + w, c.y, w, c.h), "Memory", db.mem_total > 0 ? db.mem_used / db.mem_total : 0, v, color_hex("#a56de2"));
	fmt(v, sizeof v, "%.0f GB", (double)db.disk_free);
	meter(ui, rect(c.x + 2 * w, c.y, w, c.h), "Free space", db.disk_total > 0 ? 1 - db.disk_free / db.disk_total : 0, v, color_hex("#28bca3"));
	if (db.battery >= 0) {
		fmt(v, sizeof v, "%d%%", db.battery);
		meter(ui, rect(c.x + 3 * w, c.y, w, c.h), db.charging ? "Charging" : "Battery", (f32)db.battery / 100, v,
		      color_hex(db.battery < 20 && !db.charging ? "#c6262e" : "#68b723"));
	}
	char up[64];
	i64 h = db.uptime / 3600, m = db.uptime / 60 % 60;
	fmt(up, sizeof up, h ? "On for %lldh %lldm" : "On for %lldm", h ? (long long)h : (long long)m, (long long)m);
	ui_text_in(ui, ui_font(ui, 12, 500), rect(r.x + 18, r.y + 14, r.w - 36, 20), up, t->text_faint, ALIGN_END);
}

static void list_card(Ui *ui, Rect r, const char *id, const char *icon, const char *title, Recent *items, int n, const char *empty,
		      const char *app)
{
	Theme *t = &ui->theme;
	Rect c;
	bool open_app = card(ui, r, id, icon, title, &c);
	if (!n) {
		ui_text_wrapped(ui, ui_font(ui, 14, 400), c, empty, t->text_dim, ALIGN_START);
		if (open_app)
			ui_launch(ui, app, NULL);
		return;
	}
	bool row_clicked = false;
	for (int i = 0; i < n && c.h >= 30; i++) {
		Rect row = cut_top(&c, 34);
		ui_push_id_str(ui, id);
		UiInteract it = ui_row(ui, row, ui_id_int(ui, i), false);
		ui_pop_id(ui);
		ui_icon(ui, rect(row.x + 6, row.y + 9, 16, 16), str_eq(id, "notes") ? "notes" : ui_file_icon(items[i].name, false), t->text_dim);
		ui_text_in(ui, ui_font(ui, 14, 500), rect(row.x + 30, row.y, row.w - 36, 34), items[i].name, t->text, ALIGN_START);
		if (it.clicked) {
			ui_launch(ui, app, str_eq(id, "notes") ? NULL : items[i].path);
			row_clicked = true;
		}
	}
	if (open_app && !row_clicked)
		ui_launch(ui, app, NULL);
}

static void arcade_card(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect c;
	if (card(ui, r, "arcade", "gamepad", "BEST SCORES", &c))
		ui_launch(ui, "arcade", NULL);
	static const char *const names[] = { "Blocks", "Snake", "2048", "Mines", "Breakout" };
	bool any = false;
	for (int i = 0; i < 5; i++)
		any |= db.best[i] > 0;
	if (!any) {
		ui_text_wrapped(ui, ui_font(ui, 14, 400), c, "No games played yet. Your best scores in Arcade show up here.", t->text_dim, ALIGN_START);
		return;
	}
	for (int i = 0; i < 5 && c.h >= 24; i++) {
		if (!db.best[i])
			continue;
		Rect row = cut_top(&c, 26);
		ui_text_in(ui, ui_font(ui, 14, 500), row, names[i], t->text, ALIGN_START);
		char s[24];
		fmt(s, sizeof s, "%d", db.best[i]);
		ui_text_in(ui, ui_font(ui, 14, 700), row, s, t->accent, ALIGN_END);
	}
}

/* The header sky: dawn, day, dusk and night colours. */
static void sky(int hour, Color *a, Color *b)
{
	if (hour >= 5 && hour < 9)
		*a = color_hex("#f6a58a"), *b = color_hex("#f8d5a3");
	else if (hour >= 9 && hour < 17)
		*a = color_hex("#4a8fe7"), *b = color_hex("#8fc3f5");
	else if (hour >= 17 && hour < 21)
		*a = color_hex("#7b5ea7"), *b = color_hex("#f08a6c");
	else
		*a = color_hex("#141c3a"), *b = color_hex("#33416e");
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	if (ui->now >= db.next_refresh)
		refresh(ui);
	ui->wake_at = db.next_refresh;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Civil now;
	civil_local(time_unix(), &now);
	Color a, b;
	sky(now.hour, &a, &b);
	Rect hero = cut_top(&win, 190);
	ui_gradient(ui, hero, 0, a, b);
	Theme saved = ui->theme;
	ui->theme.text = ui->theme.text_dim = COLOR_WHITE;
	ui->theme.hover = color_hex("#ffffff30");
	ui_headerbar(ui, rect(hero.x, hero.y, hero.w, 46), "", HEADER_TRANSPARENT | HEADER_FLAT | HEADER_NO_TITLE);
	ui->theme = saved;
	const char *greet = now.hour < 5 ? "Good night" : now.hour < 12 ? "Good morning" : now.hour < 18 ? "Good afternoon" : "Good evening";
	const char *who = ui_user_name(ui);
	char line[160];
	if (who && who[0]) {
		char first[64];
		str_copy(first, who, sizeof first);
		char *sp = (char *)str_chr(first, ' ');
		if (sp)
			*sp = 0;
		fmt(line, sizeof line, "%s, %s", greet, first);
	} else {
		str_copy(line, greet, sizeof line);
	}
	Rect hc = pad4(hero, 54, 36, 20, 36);
	ui_text_in(ui, ui_font(ui, 30, 700), rect(hc.x, hc.y, hc.w, 40), line, COLOR_WHITE, ALIGN_START);
	static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
	Civil c0 = { .year = now.year, .month = now.month, .day = now.day };
	int wd = (int)((unix_from_civil(&c0) / 86400 % 7 + 11) % 7);
	fmt(line, sizeof line, "%s, %d %s", days[wd], now.day, months[CLAMP(now.month, 1, 12) - 1]);
	ui_text_in(ui, ui_font(ui, 16, 500), rect(hc.x, hc.y + 44, hc.w, 24), line, color_hex("#ffffffd8"), ALIGN_START);
	char clock[16];
	fmt(clock, sizeof clock, "%02d:%02d", now.hour, now.min);
	ui_text_in(ui, ui_font(ui, 64, 300), rect(hc.x, hc.y - 6, hc.w, 80), clock, COLOR_WHITE, ALIGN_END);
	ui->wake_at = minf((f32)ui->wake_at, (f32)(ui->now + 60 - now.sec));

	/* Cards, three columns when there's room. */
	Rect area = pad4(win, 22, 28, 24, 28);
	f32 y = ui_scroll_begin(ui, area, "cards");
	int cols = area.w > 980 ? 3 : 2;
	f32 gap = 18, cw = (area.w - gap * (f32)(cols - 1)) / (f32)cols;
	f32 col_y[3] = { y, y, y };
	struct { int kind; f32 h; } order[] = { { 0, 164 }, { 1, 230 }, { 2, 190 }, { 3, 170 }, { 4, 170 }, { 5, 220 }, { 6, 170 } };
	for (usize i = 0; i < ARRAY_LEN(order); i++) {
		int col = 0;
		for (int k = 1; k < cols; k++)
			if (col_y[k] < col_y[col])
				col = k;
		Rect r = rect(area.x + (f32)col * (cw + gap), col_y[col], cw, order[i].h);
		col_y[col] += order[i].h + gap;
		switch (order[i].kind) {
		case 0: weather_card(ui, r); break;
		case 1: today_card(ui, r); break;
		case 2: screentime_card(ui, r); break;
		case 3: system_card(ui, r); break;
		case 4: list_card(ui, r, "notes", "notes", "RECENT NOTES", db.notes, db.nnotes, "No notes yet.", "notes"); break;
		case 5: list_card(ui, r, "files", "folder", "RECENT FILES", db.files, db.nfiles, "Files you save show up here.", "vault"); break;
		case 6: arcade_card(ui, r); break;
		}
	}
	f32 bottom = MAX(col_y[0], MAX(col_y[1], col_y[2]));
	ui_scroll_end(ui, bottom - y);
}

static void tick(Ui *ui)
{
	ui->redraw = true;
}

int main(void)
{
	UiAppDesc d = { .id = "dashboard", .title = "Dashboard", .width = 1080, .height = 760, .min_width = 680, .min_height = 520, .flags = WIN_RESIZABLE,
			.frame = frame, .tick = tick, .tick_ms = 5000 };
	return ui_run(&d);
}
