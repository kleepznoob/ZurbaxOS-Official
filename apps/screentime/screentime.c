/*
 * Screen Time — how you spend time on this computer.
 *
 * The desktop counts the time each app is in front while you're using the
 * computer. This app shows today and the past week, and sets daily limits
 * per app, a downtime in the evening and break reminders. Everything stays
 * on this computer.
 */
#include "zui.h"

typedef struct {
	char app[48];
	i64 secs;
} Usage;

typedef struct {
	int page;
	Usage today[64];
	int ntoday;
	i64 today_total;
	i64 week[7];          /* oldest first, the last is today */
	char week_label[7][8];
	Usage week_apps[64];
	int nweek_apps;
	Conf *prefs;
	char prefs_path[512];
	/* limit editor */
	char limit_app[48];
	int limit_h, limit_m;
	int down_h1, down_m1, down_h2, down_m2;
	f64 loaded_at;
} ScreenTime;

static ScreenTime stt;

static void day_string(i64 t, char *out, usize cap)
{
	Civil c;
	civil_local(t, &c);
	fmt(out, cap, "%04d-%02d-%02d", c.year, c.month, c.day);
}

static int cmp_usage(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	i64 x = ((const Usage *)a)->secs, y = ((const Usage *)b)->secs;
	return x > y ? -1 : x < y ? 1 : 0;
}

static void add_usage(Usage *list, int *n, int cap, const char *app, i64 secs)
{
	for (int i = 0; i < *n; i++)
		if (str_eq(list[i].app, app)) {
			list[i].secs += secs;
			return;
		}
	if (*n < cap) {
		str_copy(list[*n].app, app, sizeof list[0].app);
		list[*n].secs = secs;
		(*n)++;
	}
}

/* Reads a day's file: "app = seconds" lines. */
static i64 read_day(const char *date, Usage *list, int *n, int cap)
{
	char *path = str_fmt("%s/.local/share/zurbax/screentime/%s.conf", home_dir(), date);
	char *text = file_read(path, NULL);
	mem_free(path);
	if (!text)
		return 0;
	i64 total = 0;
	usize nl;
	char **lines = str_split(text, '\n', &nl);
	for (usize i = 0; i < nl; i++) {
		char *eq = (char *)str_chr(lines[i], '=');
		if (!eq)
			continue;
		*eq = 0;
		char *app = str_trim(lines[i]);
		i64 s = str_to_int(eq + 1, NULL);
		if (!*app || s <= 0)
			continue;
		total += s;
		if (list)
			add_usage(list, n, cap, app, s);
	}
	str_split_free(lines, nl);
	mem_free(text);
	return total;
}

static void load(Ui *ui)
{
	stt.loaded_at = ui->now;
	i64 now = time_unix();
	char date[16];
	day_string(now, date, sizeof date);
	stt.ntoday = 0;
	stt.today_total = read_day(date, stt.today, &stt.ntoday, 64);
	sort(stt.today, (usize)stt.ntoday, sizeof(Usage), cmp_usage, NULL);
	stt.nweek_apps = 0;
	for (int i = 0; i < 7; i++) {
		i64 t = now - (i64)(6 - i) * 86400;
		day_string(t, date, sizeof date);
		stt.week[i] = read_day(date, stt.week_apps, &stt.nweek_apps, 64);
		Civil c;
		civil_local(t, &c);
		static const char *const d[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
		str_copy(stt.week_label[i], i == 6 ? "Today" : d[c.wday], sizeof stt.week_label[0]);
	}
	sort(stt.week_apps, (usize)stt.nweek_apps, sizeof(Usage), cmp_usage, NULL);
}

static void format_time(i64 s, char *out, usize cap)
{
	if (s < 60)
		fmt(out, cap, s ? "%lld s" : "0 min", (long long)s);
	else if (s < 3600)
		fmt(out, cap, "%lld min", (long long)(s / 60));
	else
		fmt(out, cap, "%lld h %lld min", (long long)(s / 3600), (long long)(s % 3600 / 60));
}

static const char *app_name(const char *id, char *buf, usize cap)
{
	/* Read the app's name from its app.conf. */
	char *path = str_fmt("%s/apps/%s/app.conf", zurbax_root(), id);
	Conf *c = conf_load(path);
	mem_free(path);
	str_copy(buf, conf_get(c, "name", id), cap);
	conf_free(c);
	if (buf[0] >= 'a' && buf[0] <= 'z')
		buf[0] = (char)(buf[0] - 32);
	return buf;
}

static void save_prefs(void)
{
	char *dir = path_dir(stt.prefs_path);
	dir_make(dir, 0755);
	mem_free(dir);
	conf_save(stt.prefs);
}

static Color app_color(int i)
{
	static const u32 c[] = { 0xff3689e6, 0xff28bca3, 0xfff37329, 0xffa56de2, 0xff68b723, 0xffde3e80, 0xfff9c440, 0xff8a715e };
	return c[(usize)i % ARRAY_LEN(c)];
}

static void usage_list(Ui *ui, Rect *b, Usage *list, int n, i64 total, bool limits)
{
	Theme *t = &ui->theme;
	if (n == 0) {
		ui_placeholder(ui, cut_top(b, 200), "hourglass", "Nothing Yet", "Time spent in apps will show up here");
		return;
	}
	i64 maxs = list[0].secs;
	for (int i = 0; i < n; i++) {
		Rect row = cut_top(b, 58);
		ui_push_id(ui, i);
		char name[64], tm[32];
		app_name(list[i].app, name, sizeof name);
		format_time(list[i].secs, tm, sizeof tm);
		ui_app_icon(ui, rect(row.x, row.y + 9, 40, 40), list[i].app);
		Rect info = pad4(row, 8, 0, 8, 54);
		ui_text(ui, ui_font(ui, 14, 600), info.x, info.y + 16, name, t->text);
		Font tf = ui_font(ui, 13, 500);
		f32 right = info.x + info.w;
		if (limits) {
			char key[80];
			fmt(key, sizeof key, "limit.%s", list[i].app);
			i64 lim = conf_get_int(stt.prefs, key, 0);
			char lab[48];
			if (lim)
				fmt(lab, sizeof lab, "Limit %lld h %02lld", (long long)(lim / 60), (long long)(lim % 60));
			else
				str_copy(lab, "Set limit", sizeof lab);
			Rect lb = rect(right - 116, row.y + 13, 116, 30);
			if (ui_button_flat(ui, lb, lab)) {
				str_copy(stt.limit_app, list[i].app, sizeof stt.limit_app);
				stt.limit_h = (int)(lim ? lim / 60 : 1);
				stt.limit_m = (int)(lim % 60);
				ui_dialog_open(ui, "limit");
			}
			right -= 126;
		}
		ui_text(ui, tf, right - ui_text_width(ui, tf, tm), info.y + 16, tm, t->text_dim);
		Rect bar = rect(info.x, info.y + 28, right - info.x, 8);
		ui_round(ui, bar, 4, t->dark ? color_hex("#ffffff12") : color_hex("#0000000c"));
		ui_round(ui, rect(bar.x, bar.y, maxf(8, bar.w * (f32)list[i].secs / (f32)maxs), bar.h), 4, app_color(i));
		ui_pop_id(ui);
	}
	UNUSED(total);
}

static void page_today(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = pad4(r, 24, 32, 24, 32);
	f32 w = minf(b.w, 700);
	b.x = r.x + (r.w - w) / 2;
	b.w = w;
	f32 y0 = ui_scroll_begin(ui, r, "today");
	b.y = y0 + 24;
	b.h = 100000;
	f32 top = b.y;
	char tot[32];
	format_time(stt.today_total, tot, sizeof tot);
	ui_label_dim(ui, cut_top(&b, 20), "Screen time today");
	ui_text(ui, ui_font(ui, 40, 300), b.x, b.y + 40, tot, t->text);
	cut_top(&b, 54);
	i64 avg = 0;
	int days = 0;
	for (int i = 0; i < 6; i++)
		if (stt.week[i]) {
			avg += stt.week[i];
			days++;
		}
	if (days) {
		avg /= days;
		char a[32], line[96];
		format_time(avg, a, sizeof a);
		i64 diff = stt.today_total - avg;
		fmt(line, sizeof line, "Your daily average is %s · today is %s", a, diff > 0 ? "above it" : "below it");
		ui_label_dim(ui, cut_top(&b, 20), line);
	}
	cut_top(&b, 14);
	/* One bar split by app. */
	Rect sbar = cut_top(&b, 14);
	ui_round(ui, sbar, 7, t->dark ? color_hex("#ffffff12") : color_hex("#0000000c"));
	f32 x = sbar.x;
	for (int i = 0; i < stt.ntoday && stt.today_total; i++) {
		f32 ww = sbar.w * (f32)stt.today[i].secs / (f32)stt.today_total;
		ui_round(ui, rect(x, sbar.y, maxf(ww - 2, 2), sbar.h), 7, app_color(i));
		x += ww;
	}
	cut_top(&b, 22);
	ui_heading(ui, cut_top(&b, 28), "Apps");
	cut_top(&b, 4);
	usage_list(ui, &b, stt.today, stt.ntoday, stt.today_total, true);
	ui_scroll_end(ui, b.y - top + 40);
}

static void page_week(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = pad4(r, 24, 32, 24, 32);
	f32 w = minf(b.w, 700);
	b.x = r.x + (r.w - w) / 2;
	b.w = w;
	f32 y0 = ui_scroll_begin(ui, r, "week");
	b.y = y0 + 24;
	b.h = 100000;
	f32 top = b.y;
	i64 total = 0, maxd = 1;
	for (int i = 0; i < 7; i++) {
		total += stt.week[i];
		maxd = MAX(maxd, stt.week[i]);
	}
	char avg[32];
	format_time(total / 7, avg, sizeof avg);
	ui_label_dim(ui, cut_top(&b, 20), "Daily average this week");
	ui_text(ui, ui_font(ui, 40, 300), b.x, b.y + 40, avg, t->text);
	cut_top(&b, 64);
	/* Bar chart. */
	Rect chart = cut_top(&b, 200);
	ui_card(ui, chart);
	Rect in = pad4(chart, 20, 20, 30, 20);
	f32 bw = in.w / 7;
	for (int i = 0; i < 7; i++) {
		f32 h = (in.h - 10) * (f32)stt.week[i] / (f32)maxd;
		Rect bar = rect(in.x + bw * (f32)i + bw * 0.25f, in.y + in.h - h, bw * 0.5f, maxf(h, 3));
		ui_round(ui, bar, 5, i == 6 ? t->accent : color_with_alpha(t->accent, 0x70));
		ui_text_in(ui, ui_font(ui, 12, i == 6 ? 700 : 500), rect(in.x + bw * (f32)i, in.y + in.h + 6, bw, 18), stt.week_label[i],
			   i == 6 ? t->text : t->text_dim, ALIGN_CENTER);
		if (stt.week[i] && ui_mouse_in(ui, bar)) {
			char tm[32];
			format_time(stt.week[i], tm, sizeof tm);
			ui->last_rect = bar;
			ui_push_id(ui, i);
			ui_tooltip(ui, tm);
			ui_pop_id(ui);
		}
	}
	cut_top(&b, 20);
	ui_heading(ui, cut_top(&b, 28), "Most Used This Week");
	cut_top(&b, 4);
	usage_list(ui, &b, stt.week_apps, MIN(stt.nweek_apps, 10), total, false);
	ui_scroll_end(ui, b.y - top + 40);
}

static void parse_hm(const char *s, int *h, int *m)
{
	*h = (int)str_to_int(s, &s);
	*m = *s == ':' ? (int)str_to_int(s + 1, NULL) : 0;
}

static void page_settings(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = pad4(r, 24, 32, 24, 32);
	f32 w = minf(b.w, 700);
	b.x = r.x + (r.w - w) / 2;
	b.w = w;
	ui_title(ui, cut_top(&b, 36), "Healthy Habits");
	ui_label_dim(ui, cut_top(&b, 22), "Limits and reminders apply on this computer. Nothing leaves it.");
	cut_top(&b, 16);
	Rect card = cut_top(&b, 64 * 3 + 8);
	ui_card(ui, card);
	Rect rows = pad2(card, 18, 4);
	struct {
		const char *label, *sub, *key;
		bool def;
	} sw[] = {
		{ "Count screen time", "See which apps you use and for how long", "enabled", true },
		{ "Break reminders", "A reminder to rest your eyes after an hour of use", "breaks", false },
		{ "Downtime", "A nudge to wind down in the evening", "downtime_on", false },
	};
	for (int i = 0; i < 3; i++) {
		Rect row = cut_top(&rows, 64);
		if (i < 2)
			ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
		ui_text(ui, ui_font(ui, 14, 500), row.x, row.y + 28, sw[i].label, t->text);
		ui_text(ui, ui_font(ui, 12, 400), row.x, row.y + 46, sw[i].sub, t->text_dim);
		bool v = conf_get_bool(stt.prefs, sw[i].key, sw[i].def);
		ui_push_id(ui, i);
		if (ui_switch(ui, center_in(cut_right(&row, 52), 46, 26), &v)) {
			conf_set_bool(stt.prefs, sw[i].key, v);
			if (i == 2) {
				char d[32];
				fmt(d, sizeof d, "%02d:%02d-%02d:%02d", stt.down_h1, stt.down_m1, stt.down_h2, stt.down_m2);
				conf_set(stt.prefs, "downtime", v ? d : "");
			}
			save_prefs();
		}
		ui_pop_id(ui);
	}
	if (conf_get_bool(stt.prefs, "downtime_on", false)) {
		cut_top(&b, 14);
		ui_heading(ui, cut_top(&b, 28), "Downtime Hours");
		Rect row = cut_top(&b, 38);
		bool ch = false;
		ui_label_dim(ui, cut_left(&row, 50), "From");
		ch |= ui_spin(ui, cut_left(&row, 100), "dh1", &stt.down_h1, 0, 23);
		ch |= ui_spin(ui, cut_left(&row, 100), "dm1", &stt.down_m1, 0, 59);
		cut_left(&row, 16);
		ui_label_dim(ui, cut_left(&row, 30), "to");
		ch |= ui_spin(ui, cut_left(&row, 100), "dh2", &stt.down_h2, 0, 23);
		ch |= ui_spin(ui, cut_left(&row, 100), "dm2", &stt.down_m2, 0, 59);
		if (ch) {
			char d[32];
			fmt(d, sizeof d, "%02d:%02d-%02d:%02d", stt.down_h1, stt.down_m1, stt.down_h2, stt.down_m2);
			conf_set(stt.prefs, "downtime", d);
			save_prefs();
		}
	}
}

static void limit_dialog(Ui *ui)
{
	Rect c;
	if (!ui_dialog_begin(ui, "limit", 400, 230, &c))
		return;
	char name[64], title[120];
	app_name(stt.limit_app, name, sizeof name);
	fmt(title, sizeof title, "Daily Limit for %s", name);
	ui_heading(ui, cut_top(&c, 28), title);
	ui_label_dim(ui, cut_top(&c, 22), "You'll get a reminder 5 minutes before.");
	cut_top(&c, 14);
	Rect row = cut_top(&c, 38);
	ui_spin(ui, cut_left(&row, 110), "lh", &stt.limit_h, 0, 23);
	ui_label_dim(ui, cut_left(&row, 50), " hours");
	ui_spin(ui, cut_left(&row, 110), "lm", &stt.limit_m, 0, 59);
	ui_label_dim(ui, cut_left(&row, 60), " minutes");
	Rect bt = cut_bottom(&c, 34);
	char key[80];
	fmt(key, sizeof key, "limit.%s", stt.limit_app);
	if (ui_button_suggested(ui, cut_right(&bt, 90), "Save")) {
		int mins = stt.limit_h * 60 + stt.limit_m;
		if (mins > 0)
			conf_set_int(stt.prefs, key, mins);
		else
			conf_unset(stt.prefs, key);
		save_prefs();
		ui_dialog_close(ui);
	}
	cut_right(&bt, 8);
	if (ui_button(ui, cut_right(&bt, 90), "Cancel"))
		ui_dialog_close(ui);
	if (conf_get_int(stt.prefs, key, 0) && ui_button_destructive(ui, cut_left(&bt, 130), "Remove Limit")) {
		conf_unset(stt.prefs, key);
		save_prefs();
		ui_dialog_close(ui);
	}
	ui_dialog_end(ui);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	/* The desktop updates the numbers as you go; refresh now and then. */
	if (ui->now - stt.loaded_at > 30)
		load(ui);
	ui->wake_at = stt.loaded_at + 30.5;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	Rect tools = ui_headerbar(ui, header, "Screen Time", HEADER_NO_TITLE);
	static const char *const pages[] = { "Today", "This Week", "Habits" };
	ui_segmented(ui, center_in(tools, 320, 30), pages, 3, &stt.page);
	ui_fill(ui, win, t->window);
	switch (stt.page) {
	case 0: page_today(ui, win); break;
	case 1: page_week(ui, win); break;
	case 2: page_settings(ui, win); break;
	}
	limit_dialog(ui);
}

static void init(Ui *ui)
{
	fmt(stt.prefs_path, sizeof stt.prefs_path, "%s/.config/zurbax/screentime.conf", home_dir());
	stt.prefs = conf_load(stt.prefs_path);
	stt.down_h1 = 22, stt.down_h2 = 7;
	const char *d = conf_get(stt.prefs, "downtime", "");
	if (*d) {
		parse_hm(d, &stt.down_h1, &stt.down_m1);
		const char *dash = str_chr(d, '-');
		if (dash)
			parse_hm(dash + 1, &stt.down_h2, &stt.down_m2);
	}
	load(ui);
}

int main(void)
{
	UiAppDesc d = { .id = "screentime", .title = "Screen Time", .width = 820, .height = 640, .min_width = 600, .min_height = 460,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
