/* User settings, the account database, and polled system status. */
#include "zdisplay.h"

static char *user_conf_path(void)
{
	return str_fmt("%s/.config/zurbax/settings.conf", S.set.home);
}

static void load_account(void)
{
	char *path = str_fmt("%s/system/users.conf", zurbax_data());
	Conf *u = conf_load(path);
	mem_free(path);
	str_copy(S.set.user_name, conf_get(u, "user", "user"), sizeof S.set.user_name);
	str_copy(S.set.full_name, conf_get(u, "full_name", "Zurbax User"), sizeof S.set.full_name);
	S.set.uid = (u32)conf_get_int(u, "uid", 1000);
	S.set.setup_done = conf_get_bool(u, "setup_done", false);
	S.set.gid = (u32)conf_get_int(u, "gid", 1000);
	/* The password hash lives apart, readable by root only. */
	char *spath = str_fmt("%s/system/shadow", zurbax_data());
	Conf *sh = conf_load(spath);
	mem_free(spath);
	const char *pw = conf_get(sh, "password", conf_get(u, "password", ""));
	str_copy(S.set.password_hash, pw, sizeof S.set.password_hash);
	S.set.has_password = pw[0] != 0;
	conf_free(sh);
	const char *home = env_get("ZURBAX_HOME");
	if (home && *home)
		str_copy(S.set.home, home, sizeof S.set.home);
	else
		fmt(S.set.home, sizeof S.set.home, "%s/home/%s", zurbax_data(), S.set.user_name);
	conf_free(u);
}

void settings_load(void)
{
	load_account();
	char *path = user_conf_path();
	Conf *c = conf_load(path);
	mem_free(path);
	Settings *s = &S.set;
	s->dark = conf_get_bool(c, "dark", false);
	str_copy(s->accent_name, conf_get(c, "accent", "blue"), sizeof s->accent_name);
	s->accent = s->accent_name[0] == '#' ? color_hex(s->accent_name) : theme_accent_named(s->accent_name);
	str_copy(s->wallpaper, conf_get(c, "wallpaper", "lake"), sizeof s->wallpaper);
	s->text_scale = (f32)conf_get_float(c, "text_scale", 1.0);
	s->scale_override = (f32)conf_get_float(c, "display_scale", 0);
	s->natural_scroll = conf_get_bool(c, "natural_scroll", true);
	s->tap_to_click = conf_get_bool(c, "tap_to_click", true);
	s->pointer_speed = (f32)conf_get_float(c, "pointer_speed", 0.5);
	str_copy(s->keyboard_layout, conf_get(c, "keyboard_layout", "us"), sizeof s->keyboard_layout);
	s->night_light = conf_get_bool(c, "night_light", false);
	s->night_light_strength = (f32)conf_get_float(c, "night_light_strength", 0.5);
	s->clock_24h = conf_get_bool(c, "clock_24h", true);
	s->clock_seconds = conf_get_bool(c, "clock_seconds", false);
	s->reduce_motion = conf_get_bool(c, "reduce_motion", false);
	s->dock_autohide = conf_get_bool(c, "dock_autohide", false);
	s->dock_magnify = conf_get_bool(c, "dock_magnify", false);
	str_copy(s->dock_apps, conf_get(c, "dock_apps", "vault,drift,mail,music,photos,calendar,notes,forge,harbor,settings"),
		 sizeof s->dock_apps);
	s->do_not_disturb = conf_get_bool(c, "do_not_disturb", false);
	s->lock_after_min = (int)conf_get_int(c, "lock_after_minutes", 10);
	s->show_battery_percent = conf_get_bool(c, "battery_percent", true);
	str_copy(s->timezone, conf_get(c, "timezone", ""), sizeof s->timezone);
	s->game_mode = conf_get_bool(c, "game_mode", true);
	s->show_fps = conf_get_bool(c, "show_fps", false);
	s->mouse_accel = conf_get_bool(c, "mouse_accel", true);
	s->alert_sounds = conf_get_bool(c, "alert_sounds", true);
	S.volume = (int)conf_get_int(c, "volume", 60);
	conf_free(c);
	if (!s->timezone[0]) {
		char *sys = str_fmt("%s/system/system.conf", zurbax_data());
		Conf *sc = conf_load(sys);
		str_copy(s->timezone, conf_get(sc, "timezone", "UTC"), sizeof s->timezone);
		conf_free(sc);
		mem_free(sys);
	}
	fmt(S.screenshot_dir, sizeof S.screenshot_dir, "%s/Pictures/Screenshots", s->home);
}

void settings_save(void)
{
	char *path = user_conf_path();
	Conf *c = conf_load(path);
	mem_free(path);
	conf_set_bool(c, "dark", S.set.dark);
	conf_set_bool(c, "night_light", S.set.night_light);
	conf_set_bool(c, "do_not_disturb", S.set.do_not_disturb);
	conf_set(c, "dock_apps", S.set.dock_apps);
	conf_set_int(c, "volume", S.volume);
	conf_save(c);
	conf_free(c);
}

void settings_apply(void)
{
	if (S.set.timezone[0])
		tz_set(S.set.timezone);
	input_set_layout(S.set.keyboard_layout);
	if (S.out && S.out->set_gamma) {
		if (S.set.night_light) {
			f32 k = clampf(S.set.night_light_strength, 0, 1);
			S.out->set_gamma(S.out, 1.0f, 1.0f - 0.18f * k, 1.0f - 0.45f * k);
		} else {
			S.out->set_gamma(S.out, 1, 1, 1);
		}
	}
	S.theme.dark = S.set.dark;
}

/* ---- System status ------------------------------------------------------------------- */

static bool find_device(const char *dir, const char *prefix, char *out, usize cap)
{
	DirList l = { 0 };
	bool found = false;
	if (dir_list(dir, &l, false)) {
		vec_each(&l, e)
		{
			if (!prefix || str_starts(e->name, prefix)) {
				fmt(out, cap, "%s/%s", dir, e->name);
				found = true;
				break;
			}
		}
		dir_list_free(&l);
	}
	return found;
}

void system_status_poll(void)
{
	char dev[256], path[320];
	int old_batt = S.battery, old_bright = S.brightness;
	bool old_charging = S.charging, old_wifi = S.wifi_connected;
	/* Battery */
	S.battery = -1;
	if (find_device("/sys/class/power_supply", "BAT", dev, sizeof dev)) {
		fmt(path, sizeof path, "%s/capacity", dev);
		S.battery = (int)file_read_int(path, -1);
		fmt(path, sizeof path, "%s/status", dev);
		char *st = file_read_line(path);
		S.charging = st && (str_eq(st, "Charging") || str_eq(st, "Full"));
		mem_free(st);
	}
	/* Backlight */
	S.brightness = -1;
	if (find_device("/sys/class/backlight", NULL, dev, sizeof dev)) {
		fmt(path, sizeof path, "%s/brightness", dev);
		i64 b = file_read_int(path, -1);
		fmt(path, sizeof path, "%s/max_brightness", dev);
		i64 m = file_read_int(path, -1);
		if (b >= 0 && m > 0)
			S.brightness = (int)(b * 100 / m);
	}
	/* Network state as reported by the network service. */
	Conf *net = conf_load("/run/zurbax/network.status");
	S.wifi_connected = str_eq(conf_get(net, "state", ""), "connected");
	str_copy(S.wifi_name, conf_get(net, "name", ""), sizeof S.wifi_name);
	S.wifi_strength = (int)conf_get_int(net, "strength", 0);
	conf_free(net);
	if (old_batt != S.battery || old_bright != S.brightness || old_charging != S.charging || old_wifi != S.wifi_connected)
		damage_rect(rect(0, 0, S.sw, PANEL_HEIGHT));
}

void set_volume(int v)
{
	S.volume = CLAMP(v, 0, 100);
	S.muted = false;
	/* The sound service watches this file and applies it to the mixer. */
	char buf[32];
	fmt(buf, sizeof buf, "%d\n", S.volume);
	file_write_str("/run/zurbax/volume", buf);
	shell_osd(S.volume == 0 ? "audio-volume-muted" : S.volume < 34 ? "audio-volume-low" : S.volume < 67 ? "audio-volume-medium" : "audio-volume-high",
		  (f32)S.volume / 100);
}

void set_brightness(int v)
{
	char dev[256], path[320];
	if (!find_device("/sys/class/backlight", NULL, dev, sizeof dev))
		return;
	fmt(path, sizeof path, "%s/max_brightness", dev);
	i64 m = file_read_int(path, 0);
	if (m <= 0)
		return;
	v = CLAMP(v, 1, 100);
	char buf[32];
	fmt(buf, sizeof buf, "%lld", (long long)(m * v / 100));
	fmt(path, sizeof path, "%s/brightness", dev);
	file_write_str(path, buf);
	S.brightness = v;
	shell_osd("display-brightness", (f32)v / 100);
}

/* Changes the account on behalf of Settings, which runs without the right to
 * write the account file. Password changes need the current password. */
int account_update(const MsgAccount *m, char *msg, usize cap)
{
	static f64 last_failure;
	bool pw_change = m->flags & (ACCOUNT_SET_PASSWORD | ACCOUNT_REMOVE_PASSWORD);
	/* During first-boot setup there is no password yet to confirm. */
	if (pw_change && S.set.has_password && S.set.setup_done) {
		if (now_s() - last_failure < 2.0) {
			str_copy(msg, "Please wait a moment before trying again.", cap);
			return -1;
		}
		if (!password_check(m->old_password, S.set.password_hash)) {
			last_failure = now_s();
			str_copy(msg, "The current password is incorrect.", cap);
			return -1;
		}
	}
	if ((m->flags & ACCOUNT_SET_PASSWORD) && !m->new_password[0]) {
		str_copy(msg, "The new password is empty.", cap);
		return -1;
	}
	char *path = str_fmt("%s/system/users.conf", zurbax_data());
	char *spath = str_fmt("%s/system/shadow", zurbax_data());
	char *dir = path_dir(path);
	dir_make(dir, 0755);
	mem_free(dir);
	Conf *c = conf_load(path);   /* public profile: name, uid, and whether a password is set */
	Conf *sh = conf_load(spath); /* the password hash, root only */
	if (m->flags & ACCOUNT_SET_NAME) {
		char name[96];
		str_copy(name, m->full_name, sizeof name);
		char *t = str_trim(name);
		if (*t) {
			conf_set(c, "full_name", t);
			str_copy(S.set.full_name, t, sizeof S.set.full_name);
		}
	}
	if (m->flags & ACCOUNT_SET_PASSWORD) {
		char *rec = password_hash(m->new_password);
		conf_set(sh, "password", rec);
		str_copy(S.set.password_hash, rec, sizeof S.set.password_hash);
		S.set.has_password = true;
		mem_free(rec);
	} else if (m->flags & ACCOUNT_REMOVE_PASSWORD) {
		conf_set(sh, "password", "");
		S.set.password_hash[0] = 0;
		S.set.has_password = false;
	}
	if (m->flags & ACCOUNT_FINISH_SETUP) {
		S.set.setup_done = true;
		S.setup_mode = false;
		damage_all();
	}
	conf_set_bool(c, "setup_done", S.set.setup_done);
	conf_unset(c, "password");
	conf_set_bool(c, "has_password", S.set.has_password);
	conf_set(c, "user", S.set.user_name);
	conf_set_int(c, "uid", S.set.uid);
	conf_set_int(c, "gid", S.set.gid);
	if (!conf_get(c, "full_name", NULL))
		conf_set(c, "full_name", S.set.full_name);
	bool ok = conf_save(c) && conf_save(sh);
	sys_chmod(path, 0644);
	sys_chmod(spath, 0600);
	conf_free(c);
	conf_free(sh);
	mem_free(path);
	mem_free(spath);
	shell_mark_dirty();
	str_copy(msg, ok ? "Saved." : "The account file could not be written.", cap);
	return ok ? 0 : -1;
}

/* Game mode: while a full-screen window has focus, run the processor at full
 * speed and hold back notifications. The previous governor is restored after. */
void game_mode_update(void)
{
	static char saved[32];
	bool want = S.set.game_mode && S.focus && (S.focus->states & STATE_FULLSCREEN) && !S.focus->closing;
	if (want == S.gaming)
		return;
	S.gaming = want;
	if (want && !saved[0]) {
		char *g = file_read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
		str_copy(saved, g ? g : "", sizeof saved);
		mem_free(g);
	}
	const char *gov = want ? "performance" : saved;
	if (!gov[0])
		return;
	for (int i = 0; i < 256; i++) {
		char path[96];
		fmt(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
		if (!file_exists(path))
			break;
		file_write_str(path, gov);
	}
}
