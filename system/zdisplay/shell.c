/*
 * The desktop shell: top panel, dock, launcher, notifications, control
 * center, window switcher, lock screen and OSDs.
 *
 * The shell draws with libzui onto its own transparent overlay canvas, which
 * the compositor blends over the wallpaper and windows each frame. Shell
 * surfaces (panel, dock, popovers) sit above app windows; the lock screen
 * covers everything.
 */
#include "zdisplay.h"

#define DOCK_ICON 44
#define DOCK_GAP 10
#define DOCK_PAD 10

static Ui *ui;              /* the shell's own toolkit context */
static Canvas *overlay;     /* logical-size, redrawn each shell frame */

/* which shell surface currently holds the pointer / keyboard */
static bool pointer_on_shell;

/* ---- App database ------------------------------------------------------------------- */

static int app_cmp(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	return str_cmp_nocase(((const AppInfo *)a)->name, ((const AppInfo *)b)->name);
}

void shell_reload_apps(void)
{
	S.napps = 0;
	const char *dirs[2];
	char *sys = zurbax_path("apps");
	char *usr = str_fmt("%s/apps", zurbax_data());
	dirs[0] = sys;
	dirs[1] = usr;
	for (int d = 0; d < 2; d++) {
		DirList l = { 0 };
		if (!dir_list(dirs[d], &l, false))
			continue;
		vec_each(&l, e)
		{
			if (S.napps >= (int)ARRAY_LEN(S.apps))
				break;
			char *meta = str_fmt("%s/%s/app.conf", dirs[d], e->name);
			if (!file_exists(meta)) {
				mem_free(meta);
				continue;
			}
			Conf *c = conf_load(meta);
			mem_free(meta);
			AppInfo *a = &S.apps[S.napps];
			memset(a, 0, sizeof *a);
			str_copy(a->id, e->name, sizeof a->id);
			str_copy(a->name, conf_get(c, "name", e->name), sizeof a->name);
			str_copy(a->icon, conf_get(c, "icon", e->name), sizeof a->icon);
			str_copy(a->category, conf_get(c, "category", "Other"), sizeof a->category);
			str_copy(a->keywords, conf_get(c, "keywords", ""), sizeof a->keywords);
			str_copy(a->opens, conf_get(c, "opens", ""), sizeof a->opens);
			fmt(a->exec, sizeof a->exec, "%s/%s/%s", dirs[d], e->name, conf_get(c, "exec", e->name));
			a->hidden = conf_get_bool(c, "hidden", false);
			conf_free(c);
			if (!a->hidden)
				S.napps++;
		}
		dir_list_free(&l);
	}
	mem_free(sys);
	mem_free(usr);
	sort(S.apps, (usize)S.napps, sizeof(AppInfo), app_cmp, NULL);
}

const AppInfo *shell_app(const char *id)
{
	for (int i = 0; i < S.napps; i++)
		if (str_eq(S.apps[i].id, id))
			return &S.apps[i];
	return NULL;
}

/* Which app opens a file: by its extension, from the apps' "opens" lists. */
static const AppInfo *app_for_file(const char *path)
{
	if (str_starts(path, "http://") || str_starts(path, "https://"))
		return shell_app("drift");
	if (str_starts(path, "mailto:"))
		return shell_app("mail");
	if (dir_exists(path))
		return shell_app("vault");
	const char *ext = path_ext(path);
	if (!*ext)
		return shell_app("writer");
	for (int i = 0; i < S.napps; i++) {
		usize n;
		char **parts = str_split(S.apps[i].opens, ',', &n);
		bool hit = false;
		for (usize k = 0; k < n && !hit; k++)
			hit = str_eq_nocase(str_trim(parts[k]), ext);
		str_split_free(parts, n);
		if (hit)
			return &S.apps[i];
	}
	return NULL;
}

void shell_launch(const char *app_id, const char *arg)
{
	if ((!app_id || !*app_id) && arg && *arg) {
		const AppInfo *h = app_for_file(arg);
		if (!h) {
			const char *ext = path_ext(arg);
			if (str_eq_nocase(ext, "exe") || str_eq_nocase(ext, "msi"))
				shell_notify("system", "computer", "Windows programs can't run yet", path_base(arg));
			else
				shell_notify("system", "file", "No app can open this file", path_base(arg));
			return;
		}
		app_id = h->id;
	}
	const AppInfo *a = shell_app(app_id);
	char path[280];
	if (a) {
		str_copy(path, a->exec, sizeof path);
	} else {
		char *p = zurbax_path("apps");
		fmt(path, sizeof path, "%s/%s/%s", p, app_id, app_id);
		mem_free(p);
		if (!file_exists(path))
			return;
	}
	/* Already running and no argument? Just focus it. */
	if (!arg || !*arg) {
		for (int i = S.nwin - 1; i >= 0; i--) {
			Window *w = S.windows[i];
			if (str_eq(w->app_id, app_id) && !w->closing && !(w->flags & WIN_POPUP)) {
				window_minimize(w, false);
				window_raise(w);
				window_focus(w);
				return;
			}
		}
	}
	char *argv[4];
	int n = 0;
	argv[n++] = path;
	if (arg && *arg)
		argv[n++] = (char *)arg;
	argv[n] = NULL;
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .new_session = true };
	if (S.set.uid && sys_getuid() == 0) {
		o.drop_privileges = true;
		o.uid = S.set.uid;
		o.gid = S.set.gid;
	}
	/* Apps get the user's home, name, time zone and the display socket. */
	usize nenv = 0;
	while (environ[nenv])
		nenv++;
	char **envp = NEW_ARRAY(char *, nenv + 8);
	int ne = 0;
	char home[160], user[96], lang[64], tzv[80];
	fmt(home, sizeof home, "HOME=%s", S.set.home);
	fmt(user, sizeof user, "USER=%s", S.set.user_name);
	fmt(lang, sizeof lang, "LANG=%s", "en_US.UTF-8");
	fmt(tzv, sizeof tzv, "TZ=%s", S.set.timezone);
	envp[ne++] = home;
	envp[ne++] = user;
	envp[ne++] = lang;
	envp[ne++] = tzv;
	for (char **e = environ; *e; e++)
		if (!str_starts(*e, "HOME=") && !str_starts(*e, "USER=") && !str_starts(*e, "LANG=") && !str_starts(*e, "TZ="))
			envp[ne++] = *e;
	envp[ne] = NULL;
	if (!dir_exists(S.set.home)) {
		char *parent = path_dir(S.set.home);
		dir_make(parent, 0755);
		mem_free(parent);
		dir_make(S.set.home, 0700);
		if (o.drop_privileges)
			sys_chown(S.set.home, o.uid, o.gid);
	}
	int pid = proc_spawn(path, argv, envp, &o);
	mem_free(envp);
	if (pid < 0)
		shell_notify("system", "error", "Could not open", app_id);
}

/* ---- Notifications ------------------------------------------------------------------- */

void shell_notify(const char *app, const char *icon, const char *title, const char *body)
{
	if (S.set.do_not_disturb && !str_eq(app, "system"))
		return;
	if (S.nnotices >= (int)ARRAY_LEN(S.notices)) {
		memmove(&S.notices[0], &S.notices[1], sizeof(Notice) * (usize)(S.nnotices - 1));
		S.nnotices--;
	}
	Notice *n = &S.notices[S.nnotices++];
	memset(n, 0, sizeof *n);
	n->id = ++S.next_notice_id;
	str_copy(n->app, app, sizeof n->app);
	str_copy(n->icon, icon, sizeof n->icon);
	str_copy(n->title, title, sizeof n->title);
	str_copy(n->body, body, sizeof n->body);
	n->time = time_unix();
	n->shown_at = now_s();
	/* While a game runs full screen, keep it in the notification center only. */
	if (S.gaming)
		n->dismissed = true;
	shell_mark_dirty();
}

void shell_osd(const char *icon, f32 value)
{
	str_copy(S.osd_icon, icon, sizeof S.osd_icon);
	S.osd_value = value;
	S.osd_until = now_s() + 1.5;
	shell_mark_dirty();
}

/* ---- Geometry ----------------------------------------------------------------------- */

f32 shell_panel_height(void)
{
	return PANEL_HEIGHT;
}

static int dock_apps(char ids[][48], int max)
{
	int n = 0;
	usize count;
	char **parts = str_split(S.set.dock_apps, ',', &count);
	for (usize i = 0; i < count && n < max; i++) {
		char *id = str_trim(parts[i]);
		if (*id)
			str_copy(ids[n++], id, 48);
	}
	str_split_free(parts, count);
	/* Append running apps that aren't pinned. */
	for (int i = 0; i < S.nwin && n < max; i++) {
		Window *w = S.windows[i];
		if (w->closing || (w->flags & WIN_POPUP))
			continue;
		bool have = false;
		for (int k = 0; k < n; k++)
			if (str_eq(ids[k], w->app_id))
				have = true;
		if (!have)
			str_copy(ids[n++], w->app_id, 48);
	}
	return n;
}

static Rect dock_rect(int *out_n)
{
	char ids[32][48];
	int n = dock_apps(ids, 32);
	if (out_n)
		*out_n = n;
	f32 w = (f32)n * DOCK_ICON + (f32)(n - 1) * DOCK_GAP + 2 * DOCK_PAD;
	f32 h = DOCK_ICON + 2 * DOCK_PAD;
	return rect((S.sw - w) / 2, S.sh - h - 8, w, h);
}

f32 shell_dock_height(void)
{
	return DOCK_ICON + 2 * DOCK_PAD + 16;
}

Rect shell_dock_icon_rect(const char *app_id)
{
	char ids[32][48];
	int n = dock_apps(ids, 32);
	Rect d = dock_rect(NULL);
	for (int i = 0; i < n; i++)
		if (str_eq(ids[i], app_id))
			return rect(d.x + DOCK_PAD + (f32)i * (DOCK_ICON + DOCK_GAP), d.y + DOCK_PAD, DOCK_ICON, DOCK_ICON);
	/* Not in the dock: shrink toward the launcher button at the left. */
	return rect(d.x, d.y + d.h, DOCK_ICON, DOCK_ICON);
}

/* ---- Drawing dispatch (in zui frame) ------------------------------------------------- */

void panel_draw(Ui *ui);
void dock_draw(Ui *ui);
void launcher_draw(Ui *ui);
void notifications_draw(Ui *ui);
void controlcenter_draw(Ui *ui);
void switcher_draw(Ui *ui);
void lockscreen_draw(Ui *ui);
void osd_draw(Ui *ui);
bool launcher_key(Ui *ui, u32 code, u32 mods, const char *text);
bool lockscreen_key(Ui *ui, u32 code, u32 mods, const char *text);

static void shell_frame(Ui *ui)
{
	if (S.locked || S.lock_k > 0)
		lockscreen_draw(ui);
	if (!S.locked) {
		panel_draw(ui);
		dock_draw(ui);
		if (S.launcher_open || S.launcher_k > 0)
			launcher_draw(ui);
		notifications_draw(ui);
		controlcenter_draw(ui);
		if (S.switcher_open)
			switcher_draw(ui);
	}
	osd_draw(ui);
	if (S.set.show_fps) {
		char f[16];
		fmt(f, sizeof f, "%d FPS", (int)roundf(S.fps));
		Font ff = ui_font(ui, 12, 700);
		f32 w = ui_text_width(ui, ff, f) + 18;
		Rect r = rect(S.sw - w - 10, PANEL_HEIGHT + 10, w, 24);
		ui_round(ui, r, 12, color_hex("#000000a0"));
		ui_text_in(ui, ff, r, f, S.fps >= 55 ? color_hex("#7be07b") : S.fps >= 30 ? color_hex("#f9c440") : color_hex("#ff6b6b"),
			   ALIGN_CENTER);
		shell_surface(r);
	}
	S.shutdown_k = ui_anim(ui, ui_id(ui, "shutdown-k"), S.shutting_down ? 1.0f : 0.0f, 5);
	if (S.shutdown_k > 0) {
		f32 k = S.shutdown_k;
		ui_fill(ui, rect(0, 0, S.sw, S.sh), color_scale_alpha(COLOR_BLACK, k));
	}
}

/* ---- Shell lifecycle ----------------------------------------------------------------- */

void shell_init(void)
{
	ui = ui_new();
	overlay = canvas_new(S.frame->width, S.frame->height);
	ui_attach(ui, overlay, S.sw, S.sh, S.scale);
	ui_set_theme(ui, &S.theme);
	ui->window_focused = true;
	shell_reload_apps();
	S.brightness = -1;
	S.battery = -1;
	S.volume = S.volume ? S.volume : 60;
}

/* ---- Rendering --------------------------------------------------------------------
 * The overlay is re-rendered at most once per frame, only when something in
 * the shell changed. Each surface reports its rectangle so that exactly the
 * areas that changed (this frame's and last frame's surfaces) get repainted. */

static bool dirty = true;
static Rect surfaces[24], prev_surfaces[24];
static int nsurfaces, nprev;

void shell_mark_dirty(void)
{
	dirty = true;
	request_frame();
}

void shell_surface(Rect r)
{
	if (nsurfaces < (int)ARRAY_LEN(surfaces))
		surfaces[nsurfaces++] = rect_inset(r, -24, -24); /* room for shadows */
}

void shell_prepare(void)
{
	if (!dirty && !ui->redraw && !(ui->wake_at > 0 && now_s() >= ui->wake_at))
		return;
	dirty = false;
	if (overlay->width != S.frame->width || overlay->height != S.frame->height) {
		canvas_free(overlay);
		overlay = canvas_new(S.frame->width, S.frame->height);
	}
	ui_attach(ui, overlay, S.sw, S.sh, S.scale);
	memcpy(prev_surfaces, surfaces, sizeof surfaces);
	nprev = nsurfaces;
	nsurfaces = 0;
	memset(overlay->pixels, 0, (usize)overlay->stride * (usize)overlay->height * 4);
	ui_set_theme(ui, &S.theme);
	ui->win_states = STATE_ACTIVE;
	ui->window_focused = true;
	ui_frame(ui, shell_frame);
	for (int i = 0; i < nprev; i++)
		damage_rect(prev_surfaces[i]);
	for (int i = 0; i < nsurfaces; i++)
		damage_rect(surfaces[i]);
	if (S.locked || S.lock_k > 0 || S.shutdown_k > 0)
		damage_all();
}

/* Blends the shell overlay onto the composited frame (within its clip). */
void shell_draw(Canvas *frame)
{
	blit(frame, overlay, 0, 0, 1);
}

/* ---- Event entry points -------------------------------------------------------------- */

static bool shell_wants_pointer(void)
{
	/* The shell consumes pointer input when it owns a full-screen or docked
	 * surface the pointer is over, or an overlay is open. */
	if (S.locked || S.launcher_open || S.notifications_open || S.session_menu_open || S.switcher_open)
		return true;
	if (S.py < shell_panel_height())
		return true;
	Rect d = dock_rect(NULL);
	if (rect_contains(rect_inset(d, -4, -4), S.px, S.py))
		return true;
	return false;
}

bool shell_pointer(u32 kind, u32 button)
{
	bool want = shell_wants_pointer() || pointer_on_shell;
	ui_input_pointer(ui, kind == POINTER_DOWN ? POINTER_DOWN : kind == POINTER_UP ? POINTER_UP : POINTER_MOTION,
			 S.px, S.py, button, S.mods);
	if (kind == POINTER_DOWN)
		pointer_on_shell = want;
	if (kind == POINTER_UP)
		pointer_on_shell = false;
	if (want)
		shell_mark_dirty();
	return want;
}

bool shell_scroll(f32 dx, f32 dy, u32 source, u32 phase)
{
	if (!shell_wants_pointer())
		return false;
	ui->mx = S.px, ui->my = S.py;
	ui_input_scroll(ui, dx, dy, source, phase);
	shell_mark_dirty();
	return true;
}

bool shell_key(u32 code, u32 state, u32 mods, const char *text)
{
	if (S.locked) {
		if (state != KEY_RELEASED)
			lockscreen_key(ui, code, mods, text);
		ui_input_key(ui, code, state, mods, text);
		shell_mark_dirty();
		return true;
	}
	bool pressed = state != KEY_RELEASED;
	/* Global shortcuts. */
	if (pressed) {
		if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) {
			/* Tap Super to toggle the launcher (handled on release elsewhere). */
		}
		if ((mods & MOD_SUPER) && code == KEY_SPACE) {
			S.launcher_open = !S.launcher_open;
			shell_mark_dirty();
			return true;
		}
		if ((mods & MOD_ALT) && code == KEY_TAB) {
			S.switcher_open = true;
			S.switcher_index += (mods & MOD_SHIFT) ? -1 : 1;
			shell_mark_dirty();
			return true;
		}
		if ((mods & MOD_SUPER) && code == KEY_D) {
			wm_show_desktop(!S.show_desktop);
			return true;
		}
		if ((mods & MOD_SUPER) && code == KEY_L) {
			shell_lock();
			return true;
		}
		if (code == KEY_PRINT) {
			shell_screenshot();
			return true;
		}
		if ((mods & MOD_CTRL) && (mods & MOD_ALT) && code >= KEY_LEFT && code <= KEY_DOWN) {
			Window *w = S.focus;
			if (w) {
				if (code == KEY_UP) window_maximize(w, true);
				else if (code == KEY_DOWN) window_maximize(w, false);
				else window_tile(w, code == KEY_LEFT ? -1 : 1);
			}
			return true;
		}
		if (S.launcher_open) {
			launcher_key(ui, code, mods, text);
			ui_input_key(ui, code, state, mods, text);
			shell_mark_dirty();
			return true;
		}
	}
	/* Alt release ends the window switcher. */
	if (state == KEY_RELEASED && (code == KEY_LEFTALT || code == KEY_RIGHTALT) && S.switcher_open) {
		S.switcher_open = false;
		/* Pick the n-th most recently stacked window. */
		Window *list[64];
		int ln = 0;
		for (int i = S.nwin - 1; i >= 0 && ln < 64; i--)
			if (!S.windows[i]->closing && !(S.windows[i]->flags & WIN_POPUP))
				list[ln++] = S.windows[i];
		if (ln > 0) {
			Window *target = list[((S.switcher_index % ln) + ln) % ln];
			window_minimize(target, false);
			window_raise(target);
			window_focus(target);
		}
		S.switcher_index = 0;
		shell_mark_dirty();
		return true;
	}
	if (S.launcher_open || S.notifications_open || S.session_menu_open) {
		ui_input_key(ui, code, state, mods, text);
		shell_mark_dirty();
		return true;
	}
	return false;
}

static void finish_session_action(void);

/* Alarms set in Clock live in ~/.config/zurbax/alarms.conf:
 * "HH:MM DAYS ENABLED label" with DAYS bit 0 = Sunday, 0 = ring once. */
static void check_alarms(void)
{
	char *path = str_fmt("%s/.config/zurbax/alarms.conf", S.set.home);
	char *text = file_read(path, NULL);
	if (!text) {
		mem_free(path);
		return;
	}
	Civil now;
	civil_local(time_unix(), &now);
	StrBuf out = { 0 };
	bool changed = false;
	usize n;
	char **lines = str_split(text, '\n', &n);
	for (usize i = 0; i < n; i++) {
		const char *line = lines[i];
		const char *p = line;
		while (*p == ' ')
			p++;
		if (!*p || *p == '#') {
			if (*line)
				sb_fmt(&out, "%s\n", line);
			continue;
		}
		int h = (int)str_to_int(p, &p);
		if (*p == ':')
			p++;
		int m = (int)str_to_int(p, &p);
		int days = (int)str_to_int(p, &p);
		int enabled = (int)str_to_int(p, &p);
		while (*p == ' ')
			p++;
		const char *label = p;
		bool due = enabled && h == now.hour && m == now.min && (days == 0 || (days & (1 << now.wday)));
		if (due) {
			char t[16];
			fmt(t, sizeof t, "%02d:%02d", h, m);
			shell_notify("clock", "alarm", *label ? label : "Alarm", t);
			if (days == 0) {
				enabled = 0; /* one-time alarms switch themselves off */
				changed = true;
			}
		}
		sb_fmt(&out, "%02d:%02d %d %d %s\n", h, m, days, enabled, label);
	}
	if (changed)
		file_write_atomic(path, out.data, out.len);
	str_split_free(lines, n);
	sb_free(&out);
	mem_free(text);
	mem_free(path);
}

void shell_tick(void)
{
	static i64 last_minute = -1;
	i64 minute = time_unix() / (S.set.clock_seconds ? 1 : 60);
	if (minute != last_minute) {
		bool new_minute = last_minute >= 0 && (S.set.clock_seconds ? time_unix() % 60 == 0 : true);
		last_minute = minute;
		shell_mark_dirty();
		if (new_minute)
			check_alarms();
	}
	if (S.launcher_k != (S.launcher_open ? 1.0f : 0.0f) || S.lock_k != (S.locked ? 1.0f : 0.0f) ||
	    S.shutdown_k != (S.shutting_down ? 1.0f : 0.0f))
		shell_mark_dirty();
	/* Auto-dismiss notifications after a while. */
	for (int i = S.nnotices - 1; i >= 0; i--) {
		Notice *n = &S.notices[i];
		if (!S.notifications_open && !n->dismissed && now_s() - n->shown_at > 6.0 && n->slide >= 1) {
			n->dismissed = true;
			shell_mark_dirty();
		}
	}
	/* OSD timeout. */
	if (S.osd_until && now_s() > S.osd_until + 0.3) {
		S.osd_until = 0;
		shell_mark_dirty();
	}
	/* Frame rate counter, refreshed twice a second. */
	static f64 fps_t0;
	static u32 fps_frames;
	if (S.now - fps_t0 >= 0.5) {
		S.fps = (f32)(S.frames_composed - fps_frames) / (f32)(S.now - fps_t0);
		fps_frames = S.frames_composed;
		fps_t0 = S.now;
		if (S.set.show_fps)
			shell_mark_dirty();
	}
	game_mode_update();
	screentime_tick();
	/* The fade to black finished: carry out the action. */
	static bool done;
	if (S.shutting_down && S.shutdown_k >= 0.999f && !done) {
		done = true;
		finish_session_action();
	}
	if (!S.shutting_down)
		done = false;
	/* Idle lock. */
	if (S.set.lock_after_min > 0 && !S.locked && S.set.has_password &&
	    now_s() - S.last_input > S.set.lock_after_min * 60.0)
		shell_lock();
}

bool shell_animating(void)
{
	if (S.launcher_k != (S.launcher_open ? 1.0f : 0.0f))
		return true;
	if (S.lock_k != (S.locked ? 1.0f : 0.0f))
		return true;
	if (S.osd_until && now_s() < S.osd_until + 0.3)
		return true;
	if (S.shutdown_k != (S.shutting_down ? 1.0f : 0.0f))
		return true;
	for (int i = 0; i < S.nnotices; i++)
		if (S.notices[i].slide < 1 && !S.notices[i].dismissed)
			return true;
	if (dirty || (ui && ui->redraw))
		return true;
	return ui && ui->wake_at > 0 && now_s() >= ui->wake_at;
}

f64 shell_next_wake(void)
{
	return ui ? ui->wake_at : 0;
}

void shell_lock(void)
{
	S.locked = true;
	S.lock_k = 0;
	shell_mark_dirty();
}

/* Asks zinit to power off or restart once the screen has faded to black. */
static void finish_session_action(void)
{
	u32 action = S.shutdown_action;
	if (action == SESSION_LOGOUT) {
		/* Close every app, then return to the lock screen. */
		for (int i = S.nclients - 1; i >= 0; i--) {
			if (S.clients[i]->pid > 0)
				sys_kill(S.clients[i]->pid, SIGTERM);
		}
		S.shutting_down = false;
		S.locked = true;
		S.lock_k = 1;
		shell_mark_dirty();
		return;
	}
	settings_save();
	screentime_flush();
	int fd = unix_connect("/run/zinit.sock", SOCK_SEQPACKET);
	const char *cmd = action == SESSION_RESTART ? "reboot" : "poweroff";
	if (fd >= 0) {
		sys_sendto(fd, cmd, strlen(cmd), MSG_NOSIGNAL, NULL, 0);
		sys_close(fd);
	} else if (!S.headless) {
		/* No init to ask (development): just exit. */
		exit(0);
	}
}

void shell_session_action(u32 action)
{
	switch (action) {
	case SESSION_LOCK:
		shell_lock();
		break;
	case SESSION_LOGOUT:
	case SESSION_RESTART:
	case SESSION_SHUTDOWN:
		S.shutting_down = true;
		S.shutdown_action = action;
		shell_mark_dirty();
		break;
	case SESSION_SLEEP:
		file_write_str("/sys/power/state", "mem");
		break;
	}
	request_frame();
}

bool shell_check_password(const char *pw)
{
	if (!S.set.has_password)
		return true;
	return password_check(pw, S.set.password_hash);
}

void shell_screenshot(void)
{
	dir_make(S.screenshot_dir, 0755);
	Civil c;
	civil_local(time_unix(), &c);
	char path[256];
	fmt(path, sizeof path, "%s/Screenshot %04d-%02d-%02d %02d.%02d.%02d.png", S.screenshot_dir,
	    c.year, c.month, c.day, c.hour, c.min, c.sec);
	/* Capture the composited screen without the cursor. */
	compose_frame();
	if (png_save(S.frame, path))
		shell_notify("system", "screenshot", "Screenshot saved", path_base(path));
}
