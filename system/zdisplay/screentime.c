/*
 * Screen time: counts how long each app is in front while you use the
 * computer (not while it's idle or locked), enforces daily limits gently,
 * and reminds you to take breaks.
 *
 * Usage per day:  ~/.local/share/zurbax/screentime/YYYY-MM-DD.conf   (app = seconds)
 * Preferences:    ~/.config/zurbax/screentime.conf
 *                 limit.<app> = minutes, downtime = 22:00-07:00, breaks = true
 */
#include "zdisplay.h"

static Conf *today;
static char today_date[16];
static f64 last_tick;
static f64 last_save;
static f64 last_break_reminder;
static f64 active_since;          /* continuous use, for break reminders */
static char warned[32][48];       /* apps already warned today */
static int nwarned;
static bool downtime_notified;

static char *usage_path(const char *date)
{
	return str_fmt("%s/.local/share/zurbax/screentime/%s.conf", S.set.home, date);
}

static void current_date(char *out, usize cap)
{
	Civil c;
	civil_local(time_unix(), &c);
	fmt(out, cap, "%04d-%02d-%02d", c.year, c.month, c.day);
}

static void save(void)
{
	if (!today)
		return;
	char *path = usage_path(today_date);
	char *dir = path_dir(path);
	if (!dir_exists(dir)) {
		dir_make(dir, 0755);
		/* Belongs to the user, who reads it from the Screen Time app. */
		sys_chown(dir, S.set.uid, S.set.gid);
	}
	mem_free(dir);
	conf_save(today);
	sys_chown(path, S.set.uid, S.set.gid);
	mem_free(path);
}

static void open_day(void)
{
	char d[16];
	current_date(d, sizeof d);
	if (today && str_eq(d, today_date))
		return;
	save();
	conf_free(today);
	str_copy(today_date, d, sizeof today_date);
	char *path = usage_path(d);
	today = conf_load(path);
	mem_free(path);
	nwarned = 0;
	downtime_notified = false;
}

static bool was_warned(const char *app)
{
	for (int i = 0; i < nwarned; i++)
		if (str_eq(warned[i], app))
			return true;
	return false;
}

static void mark_warned(const char *app)
{
	if (nwarned < (int)ARRAY_LEN(warned))
		str_copy(warned[nwarned++], app, sizeof warned[0]);
}

static bool in_downtime(const char *spec)
{
	/* "22:00-07:00" */
	if (!spec || !*spec)
		return false;
	const char *p = spec;
	int h1 = (int)str_to_int(p, &p);
	int m1 = *p == ':' ? (int)str_to_int(p + 1, &p) : 0;
	if (*p != '-')
		return false;
	int h2 = (int)str_to_int(p + 1, &p);
	int m2 = *p == ':' ? (int)str_to_int(p + 1, &p) : 0;
	Civil c;
	civil_local(time_unix(), &c);
	int now = c.hour * 60 + c.min, a = h1 * 60 + m1, b = h2 * 60 + m2;
	return a <= b ? (now >= a && now < b) : (now >= a || now < b);
}

void screentime_tick(void)
{
	f64 now = S.now;
	if (last_tick == 0) {
		last_tick = now;
		active_since = now;
		return;
	}
	f64 dt = now - last_tick;
	if (dt < 5.0)
		return;
	last_tick = now;
	char *prefs_path = str_fmt("%s/.config/zurbax/screentime.conf", S.set.home);
	Conf *prefs = conf_load(prefs_path);
	mem_free(prefs_path);
	bool tracking = conf_get_bool(prefs, "enabled", true);
	bool idle = now - S.last_input > 120 || S.locked;
	if (idle)
		active_since = now;
	if (tracking && !idle && S.focus && !S.focus->closing && !S.focus->minimized && dt < 60) {
		open_day();
		const char *app = S.focus->app_id[0] ? S.focus->app_id : "other";
		i64 secs = conf_get_int(today, app, 0) + (i64)roundf((f32)dt);
		conf_set_int(today, app, secs);
		/* Daily limits: a warning five minutes before, then a gentle stop. */
		char key[80];
		fmt(key, sizeof key, "limit.%s", app);
		i64 limit = conf_get_int(prefs, key, 0) * 60;
		const AppInfo *ai = shell_app(app);
		const char *name = ai ? ai->name : app;
		if (limit > 0) {
			if (secs >= limit - 300 && secs < limit && !was_warned(app)) {
				char body[160];
				fmt(body, sizeof body, "5 minutes left with %s today.", name);
				shell_notify("screentime", "hourglass", "Screen Time", body);
				mark_warned(app);
			} else if (secs >= limit && S.focus) {
				char body[160];
				fmt(body, sizeof body, "You've reached today's limit for %s.", name);
				shell_notify("screentime", "hourglass", "Time's up", body);
				window_minimize(S.focus, true);
			}
		}
		if (now - last_save > 60) {
			save();
			last_save = now;
		}
	}
	/* Break reminder after an hour of continuous use. */
	if (conf_get_bool(prefs, "breaks", false) && now - active_since > 3600 && now - last_break_reminder > 3600) {
		shell_notify("screentime", "eye", "Time for a break", "Look at something far away for 20 seconds.");
		last_break_reminder = now;
		active_since = now;
	}
	const char *downtime = conf_get(prefs, "downtime", "");
	if (in_downtime(downtime)) {
		if (!downtime_notified) {
			shell_notify("screentime", "dark-mode", "Downtime", "It's getting late. Consider wrapping up for today.");
			downtime_notified = true;
		}
	} else {
		downtime_notified = false;
	}
	conf_free(prefs);
}

void screentime_flush(void)
{
	save();
}
