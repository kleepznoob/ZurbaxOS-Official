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

static bool force_new_instance;

/* Starts another copy of an app even if one is already open. */
void shell_launch_new(const char *app_id)
{
	force_new_instance = true;
	shell_launch(app_id, NULL);
	force_new_instance = false;
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
	if ((!arg || !*arg) && !force_new_instance) {
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
	/* A chime, unless sounds are off or a game is running full screen. */
	if (S.set.alert_sounds && !S.gaming && !str_eq(app, "system"))
		sound_play("notify");
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

/* ---- Dock geometry -------------------------------------------------------------------
 * elementary style: a short shelf along the bottom edge with rounded top corners;
 * the icons stand on it and rise above it. */

int shell_dock_items(char ids[][48], int max)
{
	int n = 0;
	usize count;
	char **parts = str_split(S.set.dock_apps, ',', &count);
	for (usize i = 0; i < count && n < max; i++) {
		char *id = str_trim(parts[i]);
		if (*id && shell_app(id))
			str_copy(ids[n++], id, 48);
	}
	str_split_free(parts, count);
	/* Running apps that aren't pinned follow the pinned ones. */
	for (int i = 0; i < S.nwin && n < max; i++) {
		Window *w = S.windows[i];
		if (w->closing || (w->flags & WIN_POPUP) || !w->app_id[0])
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

Rect shell_dock_bar(int n)
{
	f32 w = (f32)n * DOCK_ICON + (f32)MAX(n - 1, 0) * DOCK_SPACING + 2 * DOCK_PAD;
	return rect(roundf((S.sw - w) / 2), S.sh - DOCK_SHELF, w, DOCK_SHELF);
}

Rect shell_dock_slot(int n, int i)
{
	Rect bar = shell_dock_bar(n);
	return rect(bar.x + DOCK_PAD + (f32)i * (DOCK_ICON + DOCK_SPACING), S.sh - DOCK_BOTTOM - DOCK_ICON, DOCK_ICON, DOCK_ICON);
}

/* Everything the dock covers: the shelf and the icons rising above it. */
static Rect dock_rect(int *out_n)
{
	char ids[32][48];
	int n = shell_dock_items(ids, 32);
	if (out_n)
		*out_n = n;
	if (n == 0)
		return rect(S.sw / 2, S.sh, 0, 0);
	Rect bar = shell_dock_bar(n);
	f32 top = S.sh - DOCK_BOTTOM - DOCK_ICON - 4;
	return rect(bar.x, top, bar.w, S.sh - top);
}

f32 shell_dock_height(void)
{
	return DOCK_BOTTOM + DOCK_ICON + 10;
}

Rect shell_dock_icon_rect(const char *app_id)
{
	char ids[32][48];
	int n = shell_dock_items(ids, 32);
	for (int i = 0; i < n; i++)
		if (str_eq(ids[i], app_id))
			return shell_dock_slot(n, i);
	Rect bar = shell_dock_bar(MAX(n, 1));
	return rect(bar.x, S.sh, DOCK_ICON, DOCK_ICON);
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

static void area_select_draw(Ui *ui);

static void shell_frame(Ui *ui)
{
	if (S.locked || S.lock_k > 0)
		lockscreen_draw(ui);
	bool fullscreen = S.focus && (S.focus->states & STATE_FULLSCREEN) && !S.focus->minimized && !S.focus->closing;
	if (!S.locked && !S.setup_mode) {
		if (!fullscreen)
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
	if (S.selecting_area)
		area_select_draw(ui);
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
	if (!S.set.setup_done) {
		/* First boot: the setup assistant, full screen, before anything else. */
		S.setup_mode = true;
		S.splash_wait_window = true;
		shell_launch("welcome", "--setup");
	} else if (S.set.has_password) {
		/* The lock screen doubles as the login screen. */
		S.locked = true;
		S.lock_k = 1;
	}
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
	if (S.selecting_area)
		return true;
	if (S.setup_mode)
		return false;
	if (S.locked || S.launcher_open || S.notifications_open || S.session_menu_open || S.switcher_open || S.dock_menu[0])
		return true;
	if (S.py < shell_panel_height())
		return true;
	Rect d = dock_rect(NULL);
	if (S.dock_hide_k < 0.5f && rect_contains(rect_inset(d, -4, -4), S.px, S.py))
		return true;
	return false;
}

/* Intellihide: the dock steps aside when a window needs the space (or always,
 * with auto-hide on) and comes back when the pointer touches the bottom edge. */
static bool dock_should_hide(void)
{
	if (S.launcher_open || S.switcher_open || S.locked || S.dock_menu[0])
		return false;
	Rect d = dock_rect(NULL);
	Rect zone = rect_inset(d, -16, -20);
	if (S.py >= S.sh - 3)
		S.dock_revealed = true;
	else if (S.dock_revealed && !rect_contains(rect(zone.x, zone.y, zone.w, S.sh - zone.y), S.px, S.py))
		S.dock_revealed = false;
	if (S.focus && (S.focus->states & STATE_FULLSCREEN) && !S.focus->minimized)
		return !S.dock_revealed;
	if (S.dock_revealed)
		return false;
	if (S.set.dock_autohide)
		return true;
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		if (!w->mapped || w->closing || w->minimized || (w->flags & WIN_POPUP))
			continue;
		if (!rect_empty(rect_intersect(window_visual_rect(w), rect_inset(d, -6, -6))))
			return true;
	}
	return false;
}

static void finish_area(bool cancel);
static Rect selection_rect(void);

bool shell_pointer(u32 kind, u32 button)
{
	if (S.selecting_area) {
		if (kind == POINTER_DOWN && button == BUTTON_LEFT) {
			S.sel_dragging = true;
			S.sel_x0 = S.sel_x1 = S.px;
			S.sel_y0 = S.sel_y1 = S.py;
		} else if (kind == POINTER_MOTION && S.sel_dragging) {
			S.sel_x1 = S.px;
			S.sel_y1 = S.py;
		} else if (kind == POINTER_UP && S.sel_dragging) {
			S.sel_x1 = S.px;
			S.sel_y1 = S.py;
			finish_area(false);
		} else if (kind == POINTER_DOWN && button == BUTTON_RIGHT) {
			finish_area(true);
		}
		shell_mark_dirty();
		damage_all();
		return true;
	}
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
	if (S.selecting_area) {
		if (code == KEY_ESC && state == KEY_PRESSED)
			finish_area(true);
		return true;
	}
	if (S.locked) {
		if (state != KEY_RELEASED)
			lockscreen_key(ui, code, mods, text);
		ui_input_key(ui, code, state, mods, text);
		shell_mark_dirty();
		return true;
	}
	bool pressed = state != KEY_RELEASED;
	if (S.setup_mode)
		return false; /* everything goes to the setup assistant */
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
	/* Slide the dock in or out. */
	static f64 last_dock_tick;
	f32 dock_dt = minf((f32)(S.now - last_dock_tick), 0.05f);
	last_dock_tick = S.now;
	bool hide = dock_should_hide();
	f32 target = hide ? 1.0f : 0.0f;
	if (S.dock_hide_k != target) {
		f32 step = dock_dt * (S.set.reduce_motion ? 20 : 5);
		S.dock_hide_k = target > S.dock_hide_k ? minf(target, S.dock_hide_k + step) : maxf(target, S.dock_hide_k - step);
		shell_mark_dirty();
	}
	S.dock_hide = hide;
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
	if (S.dock_hide_k != (S.dock_hide ? 1.0f : 0.0f))
		return true;
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

/* Composes the screen without the pointer and copies a logical area of it. */
static Canvas *capture(Rect area)
{
	S.capturing = true;
	damage_all();
	compose_frame();
	S.capturing = false;
	damage_all();
	request_frame();
	Rect p = rect_intersect(rect(area.x * S.scale, area.y * S.scale, area.w * S.scale, area.h * S.scale),
				rect(0, 0, (f32)S.frame->width, (f32)S.frame->height));
	int w = (int)p.w, h = (int)p.h;
	if (w <= 0 || h <= 0)
		return NULL;
	Canvas *c = canvas_new(w, h);
	copy_pixels(c, S.frame, (int)p.x, (int)p.y, 0, 0, w, h);
	return c;
}

static void save_to_pictures(const Canvas *img)
{
	if (!dir_exists(S.screenshot_dir)) {
		dir_make(S.screenshot_dir, 0755);
		sys_chown(S.screenshot_dir, S.set.uid, S.set.gid);
	}
	Civil c;
	civil_local(time_unix(), &c);
	char path[256];
	fmt(path, sizeof path, "%s/Screenshot %04d-%02d-%02d %02d.%02d.%02d.png", S.screenshot_dir, c.year, c.month, c.day, c.hour, c.min, c.sec);
	if (S.set.alert_sounds)
		sound_play("camera");
	if (png_save(img, path)) {
		sys_chown(path, S.set.uid, S.set.gid);
		shell_notify("system", "screenshot", "Screenshot saved", path_base(path));
	}
}

void shell_screenshot(void)
{
	Canvas *img = capture(rect(0, 0, S.sw, S.sh));
	if (img) {
		save_to_pictures(img);
		canvas_free(img);
	}
}

static void send_shot(Client *c, u32 win, Canvas *img)
{
	MsgScreenshotData m = { .status = img ? 0 : -1 };
	if (!img || !c || c->fd < 0) {
		if (c && c->fd >= 0)
			server_send(c, &m, MSG_SCREENSHOT_DATA, sizeof m, win, NULL, 0);
		return;
	}
	m.width = (u32)img->width;
	m.height = (u32)img->height;
	int fd = (int)sys_memfd_create("screenshot", MFD_CLOEXEC);
	if (fd < 0) {
		m.status = -1;
		server_send(c, &m, MSG_SCREENSHOT_DATA, sizeof m, win, NULL, 0);
		return;
	}
	for (int y = 0; y < img->height; y++)
		write_all(fd, canvas_row(img, y), (usize)img->width * 4);
	server_send(c, &m, MSG_SCREENSHOT_DATA, sizeof m, win, &fd, 1);
	sys_close(fd);
}

void screenshot_request(Client *c, u32 win, u32 mode)
{
	if (mode == SCREENSHOT_AREA) {
		/* Let the user drag out the area; finished in shell_pointer. */
		S.selecting_area = true;
		S.sel_dragging = false;
		S.shot_client = c;
		S.shot_win = win;
		S.cursor = CURSOR_CROSSHAIR;
		cursor_update();
		shell_mark_dirty();
		damage_all();
		return;
	}
	Rect area = rect(0, 0, S.sw, S.sh);
	if (mode == SCREENSHOT_WINDOW) {
		/* The frontmost window that isn't the one asking. */
		for (int i = S.nwin - 1; i >= 0; i--) {
			Window *w = S.windows[i];
			if (w->client != c && w->mapped && !w->closing && !w->minimized && !(w->flags & WIN_POPUP)) {
				area = w->rect;
				break;
			}
		}
	}
	Canvas *img = capture(area);
	send_shot(c, win, img);
	canvas_free(img);
}

void screenshot_client_gone(Client *c)
{
	if (S.shot_client == c) {
		S.shot_client = NULL;
		S.selecting_area = false;
		shell_mark_dirty();
	}
}

static Rect selection_rect(void)
{
	f32 x0 = minf(S.sel_x0, S.sel_x1), y0 = minf(S.sel_y0, S.sel_y1);
	return rect(x0, y0, fabsf(S.sel_x1 - S.sel_x0), fabsf(S.sel_y1 - S.sel_y0));
}

static void finish_area(bool cancel)
{
	S.selecting_area = false;
	S.sel_dragging = false;
	S.cursor = CURSOR_DEFAULT;
	cursor_update();
	shell_mark_dirty();
	Rect r = selection_rect();
	Canvas *img = NULL;
	if (!cancel && r.w >= 2 && r.h >= 2) {
		/* Draw once more without the selection overlay before capturing. */
		shell_prepare();
		img = capture(r);
	}
	if (S.shot_client)
		send_shot(S.shot_client, S.shot_win, img);
	else if (img)
		save_to_pictures(img);
	canvas_free(img);
	S.shot_client = NULL;
	damage_all();
}

/* The dimmed screen with a clear window where the area is. */
static void area_select_draw(Ui *ui)
{
	Color dim = color_hex("#00000070");
	if (!S.sel_dragging) {
		ui_fill(ui, rect(0, 0, S.sw, S.sh), dim);
		const char *hint = "Drag to choose an area · Esc to cancel";
		Font f = ui_font(ui, 14, 600);
		f32 w = ui_text_width(ui, f, hint) + 36;
		Rect b = rect((S.sw - w) / 2, S.sh / 2 - 22, w, 44);
		ui_round(ui, b, 22, color_hex("#000000b0"));
		ui_text_in(ui, f, b, hint, COLOR_WHITE, ALIGN_CENTER);
	} else {
		Rect r = selection_rect();
		ui_fill(ui, rect(0, 0, S.sw, r.y), dim);
		ui_fill(ui, rect(0, r.y + r.h, S.sw, S.sh - r.y - r.h), dim);
		ui_fill(ui, rect(0, r.y, r.x, r.h), dim);
		ui_fill(ui, rect(r.x + r.w, r.y, S.sw - r.x - r.w, r.h), dim);
		ui_border(ui, rect_inset(r, -1, -1), 0, 1.5f, COLOR_WHITE);
		char sz[32];
		fmt(sz, sizeof sz, "%d × %d", (int)(r.w * S.scale), (int)(r.h * S.scale));
		Font f = ui_font(ui, 12, 700);
		f32 w = ui_text_width(ui, f, sz) + 16;
		Rect lb = rect(r.x, r.y + r.h + 6 + 22 > S.sh ? r.y - 28 : r.y + r.h + 6, w, 22);
		ui_round(ui, lb, 6, color_hex("#000000c0"));
		ui_text_in(ui, f, lb, sz, COLOR_WHITE, ALIGN_CENTER);
	}
	shell_surface(rect(0, 0, S.sw, S.sh));
}
