/*
 * The desktop shell: the top bar, the taskbar, the app launcher,
 * notifications, quick settings, window switcher, lock screen and OSDs.
 *
 * The shell draws with libzui onto its own transparent overlay canvas, which
 * the compositor blends over the wallpaper and windows each frame. Shell
 * surfaces (the bars and the panels that open from them) sit above app
 * windows; the lock screen covers everything.
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

/* ---- Linux apps: the menu entries Flatpak exports ------------------------------------------ */

/* A menu entry's freedesktop.org categories as one of Zurbax's. */
static const char *category_of(const char *cats)
{
	static const struct {
		const char *fd, *zos;
	} map[] = {
		{ "Game", "Games" }, { "WebBrowser", "Internet" }, { "Email", "Internet" }, { "Chat", "Internet" },
		{ "Network", "Internet" }, { "Graphics", "Graphics" }, { "Photography", "Graphics" }, { "AudioVideo", "Media" },
		{ "Audio", "Media" }, { "Video", "Media" }, { "Office", "Office" }, { "Education", "Office" },
		{ "Science", "Office" }, { "Development", "Utilities" }, { "Settings", "System" }, { "System", "System" },
		{ "Utility", "Utilities" },
	};
	for (usize i = 0; i < ARRAY_LEN(map); i++) {
		usize n = 0;
		char **parts = str_split(cats, ';', &n);
		bool hit = false;
		for (usize k = 0; k < n && !hit; k++)
			hit = str_eq(parts[k], map[i].fd);
		str_split_free(parts, n);
		if (hit)
			return map[i].zos;
	}
	return "Other";
}

/* File types Linux apps declare (MIME), as the extensions Zurbax opens files by. */
static void mime_to_exts(const char *mimes, char *out, usize cap)
{
	static const struct {
		const char *mime, *exts;
	} map[] = {
		{ "text/html", "html,htm" }, { "text/plain", "txt" }, { "text/markdown", "md" }, { "text/csv", "csv" },
		{ "application/pdf", "pdf" }, { "application/epub+zip", "epub" }, { "application/zip", "zip" },
		{ "image/png", "png" }, { "image/jpeg", "jpg,jpeg" }, { "image/gif", "gif" }, { "image/webp", "webp" },
		{ "image/svg+xml", "svg" }, { "image/bmp", "bmp" }, { "image/tiff", "tif,tiff" }, { "audio/mpeg", "mp3" },
		{ "audio/flac", "flac" }, { "audio/ogg", "ogg,oga" }, { "audio/x-wav", "wav" }, { "audio/mp4", "m4a" },
		{ "video/mp4", "mp4" }, { "video/x-matroska", "mkv" }, { "video/webm", "webm" }, { "video/quicktime", "mov" },
		{ "video/x-msvideo", "avi" }, { "application/msword", "doc" }, { "application/vnd.ms-excel", "xls" },
		{ "application/vnd.oasis.opendocument.text", "odt" }, { "application/vnd.oasis.opendocument.spreadsheet", "ods" },
		{ "application/vnd.oasis.opendocument.presentation", "odp" },
		{ "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx" },
		{ "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "xlsx" },
		{ "application/vnd.openxmlformats-officedocument.presentationml.presentation", "pptx" },
	};
	out[0] = 0;
	usize n = 0;
	char **parts = str_split(mimes, ';', &n);
	for (usize k = 0; k < n; k++)
		for (usize i = 0; i < ARRAY_LEN(map); i++)
			if (str_eq(parts[k], map[i].mime) && strlen(out) + strlen(map[i].exts) + 2 < cap) {
				if (out[0])
					str_copy(out + strlen(out), ",", cap - strlen(out));
				str_copy(out + strlen(out), map[i].exts, cap - strlen(out));
			}
	str_split_free(parts, n);
}

/* The picture for a menu entry's Icon in a share folder's icon theme:
 * the larger pictures first (Zurbax draws them down to size). */
static bool find_icon(const char *share, const char *name, char *out, usize cap)
{
	if (name[0] == '/') {
		str_copy(out, name, cap);
		return file_exists(name);
	}
	static const char *const sizes[] = { "512x512", "256x256", "192x192", "128x128", "scalable", "96x96", "64x64", "48x48" };
	for (usize i = 0; i < ARRAY_LEN(sizes); i++) {
		fmt(out, cap, "%s/icons/hicolor/%s/apps/%s.%s", share, sizes[i], name, str_eq(sizes[i], "scalable") ? "svg" : "png");
		if (file_exists(out))
			return true;
	}
	fmt(out, cap, "%s/pixmaps/%s.png", share, name);
	return file_exists(out);
}

/* The menu entries in share/applications: what they're called, their icon,
 * their command and the files they open. */
static void load_desktop_entries(const char *share)
{
	char *dir = str_fmt("%s/applications", share);
	DirList l = { 0 };
	if (!dir_list(dir, &l, false)) {
		mem_free(dir);
		return;
	}
	vec_each(&l, e)
	{
		if (S.napps >= (int)ARRAY_LEN(S.apps))
			break;
		if (!str_ends(e->name, ".desktop"))
			continue;
		char *path = str_fmt("%s/%s", dir, e->name);
		char *text = file_read(path, NULL);
		mem_free(path);
		if (!text)
			continue;
		const char *keys[] = { "Type", "Name", "GenericName", "Comment", "Exec", "Icon", "NoDisplay", "Hidden", "Categories",
				       "Keywords", "MimeType" };
		const char *vals[ARRAY_LEN(keys)] = { 0 };
		bool entry = false;
		usize n = 0;
		char **lines = str_split(text, '\n', &n);
		for (usize i = 0; i < n; i++) {
			char *line = str_trim(lines[i]);
			if (line[0] == '[') {
				entry = str_eq(line, "[Desktop Entry]");
				continue;
			}
			char *eq = (char *)str_chr(line, '=');
			if (!entry || !eq)
				continue;
			*eq = 0;
			char *key = str_trim(line), *val = str_trim(eq + 1);
			for (usize k = 0; k < ARRAY_LEN(keys); k++)
				if (str_eq(key, keys[k]) && !vals[k])
					vals[k] = val;
		}
		bool show = vals[0] && str_eq(vals[0], "Application") && vals[1] && vals[4] &&
			    !(vals[6] && str_eq(vals[6], "true")) && !(vals[7] && str_eq(vals[7], "true"));
		if (show) {
			AppInfo *a = &S.apps[S.napps];
			memset(a, 0, sizeof *a);
			str_copy(a->id, e->name, sizeof a->id);
			a->id[strlen(a->id) - 8] = 0; /* without ".desktop" */
			str_copy(a->name, vals[1], sizeof a->name);
			if (!vals[5] || !find_icon(share, vals[5], a->icon, sizeof a->icon))
				str_copy(a->icon, "zurbax", sizeof a->icon);
			str_copy(a->exec, vals[4], sizeof a->exec);
			str_copy(a->category, category_of(vals[8] ? vals[8] : ""), sizeof a->category);
			fmt(a->keywords, sizeof a->keywords, "%s;%s;%s", vals[2] ? vals[2] : "", vals[9] ? vals[9] : "",
			    vals[3] ? vals[3] : "");
			mime_to_exts(vals[10] ? vals[10] : "", a->opens, sizeof a->opens);
			a->is_linux = true;
			/* a Zurbax app of the same id stays */
			if (!shell_app(a->id))
				S.napps++;
		}
		str_split_free(lines, n);
		mem_free(text);
	}
	dir_list_free(&l);
	mem_free(dir);
}

/* A Linux app's command line: its menu entry's command (the desktop entry
 * specification's quoting, with the file or address in place of its field
 * codes), started in the Linux session by layerrun. */
static int linux_argv(const AppInfo *a, const char *arg, char *buf, usize cap, char **argv, int max)
{
	int n = 0;
	char *layerrun = zurbax_path("bin/layerrun");
	usize used = 0;
#define PUT(s)                                                              \
	do {                                                                    \
		usize l_ = strlen(s);                                               \
		if (n < max - 1 && used + l_ + 1 <= cap) {                          \
			memcpy(buf + used, s, l_ + 1);                              \
			argv[n++] = buf + used;                                     \
			used += l_ + 1;                                             \
		}                                                                   \
	} while (0)
	PUT(layerrun);
	PUT("--app");
	PUT(a->id);
	mem_free(layerrun);
	bool placed = false;
	for (const char *p = a->exec; *p;) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		char tok[512];
		usize t = 0;
		bool quoted = false;
		while (*p && (quoted || *p != ' ') && t + 1 < sizeof tok) {
			if (*p == '"') {
				quoted = !quoted;
				p++;
			} else if (*p == '\\' && p[1]) {
				tok[t++] = p[1];
				p += 2;
			} else {
				tok[t++] = *p++;
			}
		}
		tok[t] = 0;
		/* Flatpak's file forwarding needs the document portal; the file goes as it is */
		if (str_eq(tok, "--file-forwarding") || str_eq(tok, "@@") || str_eq(tok, "@@u") || str_eq(tok, "@@f"))
			continue;
		if (str_eq(tok, "%u") || str_eq(tok, "%U") || str_eq(tok, "%f") || str_eq(tok, "%F")) {
			if (arg && *arg && !placed)
				PUT(arg);
			placed = true;
			continue;
		}
		if (tok[0] == '%' && tok[1] && !tok[2])
			continue; /* %i %c %k and the like */
		PUT(tok);
	}
#undef PUT
	argv[n] = NULL;
	return n;
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
			/* The installer only makes sense when running from the install stick. */
			if (conf_get_bool(c, "live_only", false) && !file_exists("/run/zurbax-live"))
				a->hidden = true;
			conf_free(c);
			if (!a->hidden)
				S.napps++;
		}
		dir_list_free(&l);
	}
	mem_free(sys);
	mem_free(usr);
	/* Flatpak apps the user installed */
	char *share = str_fmt("%s/.local/share/flatpak/exports/share", S.set.home);
	load_desktop_entries(share);
	mem_free(share);
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
	/* Zurbax's own apps first, then Linux apps */
	for (int pass = 0; pass < 2; pass++)
	for (int i = 0; i < S.napps; i++) {
		if (S.apps[i].is_linux != (pass == 1))
			continue;
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

/* /run/user/UID, the runtime folder of a standard Linux session, made for
 * the user (private to them) with the Wayland socket under its usual name.
 * NULL when it can't be made (zdisplay not running as root, in development). */
char *user_runtime_dir(u32 uid, u32 gid)
{
	char *dir = str_fmt("/run/user/%u", uid);
	struct stat st;
	if (sys_lstat(dir, &st) < 0) {
		sys_mkdir("/run/user", 0755);
		if (sys_mkdir(dir, 0700) == 0)
			sys_chown(dir, uid, gid);
	}
	if (sys_lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != uid) {
		mem_free(dir);
		return NULL;
	}
	char *link = str_fmt("%s/wayland-0", dir);
	char cur[200] = { 0 };
	if (sys_readlink(link, cur, sizeof cur - 1) <= 0 || !str_eq(cur, wl_socket_path())) {
		sys_unlink(link);
		if (sys_symlink(wl_socket_path(), link) == 0)
			sys_lchown(link, uid, gid);
	}
	mem_free(link);
	return dir;
}

/* Starts a program as the user, with the user's home, name, time zone,
 * runtime folder and the display's sockets. Returns its pid or -errno. */
static int user_spawn(const char *path, char **argv)
{
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .new_session = true };
	static const u32 groups[] = { GID_AUDIO, GID_VIDEO, GID_INPUT };
	if (S.set.uid && sys_getuid() == 0) {
		o.drop_privileges = true;
		o.uid = S.set.uid;
		o.gid = S.set.gid;
		o.groups = groups;
		o.ngroups = (int)ARRAY_LEN(groups);
	}
	/* Apps get the user's home, name, time zone and the display socket. */
	usize nenv = 0;
	while (environ[nenv])
		nenv++;
	char **envp = NEW_ARRAY(char *, nenv + 9);
	int ne = 0;
	char home[160], user[96], lang[64], tzv[80], wayland[200], screen[48], runtime[64];
	fmt(home, sizeof home, "HOME=%s", S.set.home);
	fmt(user, sizeof user, "USER=%s", S.set.user_name);
	fmt(lang, sizeof lang, "LANG=%s", "en_US.UTF-8");
	fmt(tzv, sizeof tzv, "TZ=%s", S.set.timezone);
	/* Standard Linux programs (through layerrun) find the Wayland socket, and
	 * learn the screen's real size for a full-screen X server. */
	fmt(wayland, sizeof wayland, "WAYLAND_DISPLAY=%s", wl_socket_path());
	fmt(screen, sizeof screen, "ZOS_SCREEN=%dx%d", S.out->width, S.out->height);
	envp[ne++] = home;
	envp[ne++] = user;
	envp[ne++] = lang;
	envp[ne++] = tzv;
	envp[ne++] = wayland;
	envp[ne++] = screen;
	/* the runtime folder standard Linux programs keep their sockets in */
	char *rt = user_runtime_dir(o.drop_privileges ? o.uid : (u32)sys_getuid(), o.drop_privileges ? o.gid : (u32)sys_getgid());
	if (rt) {
		fmt(runtime, sizeof runtime, "XDG_RUNTIME_DIR=%s", rt);
		envp[ne++] = runtime;
		mem_free(rt);
	}
	for (char **e = environ; *e; e++)
		if (!str_starts(*e, "HOME=") && !str_starts(*e, "USER=") && !str_starts(*e, "LANG=") && !str_starts(*e, "TZ=") &&
		    !str_starts(*e, "WAYLAND_DISPLAY=") && !str_starts(*e, "ZOS_SCREEN=") &&
		    !(rt && str_starts(*e, "XDG_RUNTIME_DIR=")))
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
	return pid;
}

int shell_user_spawn(const char *path, char **argv)
{
	return user_spawn(path, argv);
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
	if (!a) {
		/* maybe just installed from Harbor */
		shell_reload_apps();
		a = shell_app(app_id);
	}
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
	char *argv[64];
	char argbuf[2048];
	int n = 0;
	if (a && a->is_linux) {
		n = linux_argv(a, arg, argbuf, sizeof argbuf, argv, (int)ARRAY_LEN(argv));
		str_copy(path, argv[0], sizeof path);
	} else {
		argv[n++] = path;
		if (arg && *arg)
			argv[n++] = (char *)arg;
		argv[n] = NULL;
	}
	if (user_spawn(path, argv) < 0)
		shell_notify("system", "error", "Could not open", app_id);
}

/* ---- User services ------------------------------------------------------------------- */

/* zportal (the desktop services Linux programs ask for) runs for the whole
 * session, started again if it stops. Apps that exited are reaped here too. */
static int zportal_pid;
static f64 zportal_started = -100;

void shell_stop_portal(void)
{
	if (zportal_pid > 0) {
		sys_kill(zportal_pid, SIGTERM);
		sys_wait4(zportal_pid, NULL, 0, NULL);
		zportal_pid = 0;
	}
}

void shell_services_tick(void)
{
	int st;
	long pid;
	while ((pid = sys_wait4(-1, &st, WNOHANG, NULL)) > 0) {
		if (pid == zportal_pid)
			zportal_pid = 0;
		else if (!desktop_reaped((int)pid, st))
			gnome_reaped((int)pid, st);
	}
	/* in a GNOME session GNOME's own portals answer */
	if (zportal_pid || S.now - zportal_started < 10 || gnome_running())
		return;
	char *path = zurbax_path("bin/zportal");
	if (file_exists(path)) {
		char *argv[] = { "zportal", NULL };
		zportal_started = S.now;
		int p = user_spawn(path, argv);
		zportal_pid = p > 0 ? p : 0;
	}
	mem_free(path);
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

/* ---- Top bar and taskbar geometry ----------------------------------------------------
 * The top bar holds the app menu, the clock and the status icons. The
 * taskbar along the bottom holds Start, the search box, a button for each
 * pinned or running app, and the tray: notifications, and the clock and
 * status icons too while the top bar is turned off. */

f32 shell_panel_height(void)
{
	return S.set.topbar && !S.setup_mode ? PANEL_HEIGHT : 0;
}

static const f32 TASKBAR_H[] = { 40, 48, 56 }, TASKBAR_ICON[] = { 24, 28, 32 };

f32 shell_taskbar_height(void)
{
	return TASKBAR_H[CLAMP(S.set.taskbar_size, 0, 2)];
}

f32 shell_taskbar_reserved(void)
{
	return S.set.dock_autohide || S.setup_mode ? 0 : shell_taskbar_height();
}

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

/* The status icons: controller, quiet mode, network, sound, battery, power. */
int shell_status_icons(const char *icons[8], int *battery_at)
{
	int n = 0;
	char pads[PAD_MAX][64];
	*battery_at = -1;
	if (input_gamepads(pads, PAD_MAX) > 0)
		icons[n++] = "gamepad";
	if (S.set.do_not_disturb)
		icons[n++] = "notifications-disabled";
	icons[n++] = S.wifi_connected && S.net_wired ? "network-wired"
		     : S.wifi_connected ? (S.wifi_strength > 66 ? "network-wireless" : S.wifi_strength > 33 ? "network-wireless-good" : "network-wireless-weak")
					: "network-wireless-none";
	icons[n++] = S.muted || S.volume == 0 ? "audio-volume-muted" : S.volume < 34 ? "audio-volume-low"
		     : S.volume < 67 ? "audio-volume-medium" : "audio-volume-high";
	if (S.battery >= 0) {
		*battery_at = n;
		icons[n++] = S.charging ? "battery-charging" : S.battery > 80 ? "battery-full" : S.battery > 45 ? "battery-good"
			     : S.battery > 15 ? "battery-low" : "battery-caution";
	}
	icons[n++] = "system-shutdown";
	return n;
}

void shell_taskbar(Taskbar *t)
{
	memset(t, 0, sizeof *t);
	char ids[32][48];
	t->n = shell_dock_items(ids, 32);
	f32 h = shell_taskbar_height();
	t->icon = TASKBAR_ICON[CLAMP(S.set.taskbar_size, 0, 2)];
	t->bar = rect(0, S.sh - h, S.sw, h);
	/* the tray, from the right edge */
	f32 x = S.sw;
	if (S.set.taskbar_desktop) {
		t->desktop = rect(x - 10, t->bar.y, 10, h);
		x -= 10;
	}
	if (!shell_panel_height()) {
		t->clock = rect(x - 88, t->bar.y, 88, h);
		x -= 88;
		const char *icons[8];
		int bat, ni = shell_status_icons(icons, &bat);
		f32 sw = (f32)ni * 26 + 16 + (bat >= 0 && S.set.show_battery_percent ? 34 : 0);
		t->status = rect(x - sw, t->bar.y, sw, h);
		x -= sw;
	}
	t->bell = rect(x - 40, t->bar.y, 40, h);
	x -= 40;
	t->tray = rect(x - 4, t->bar.y, S.sw - x + 4, h);
	/* Start, search and the task buttons */
	f32 room = t->tray.x - 12;
	f32 start_w = h + 4;
	f32 search_w = S.set.taskbar_search ? (S.set.taskbar_center ? 200 : 240) : 0;
	t->labels = S.set.taskbar_labels;
	t->slot_w = t->labels ? 172 : h + 4;
	f32 group = start_w + (search_w ? search_w + 8 : 0) + 6 + (f32)t->n * t->slot_w;
	if (group > room && t->labels) {
		t->slot_w = maxf(h + 4, floorf((room - start_w - search_w - 14) / (f32)MAX(t->n, 1)));
		t->labels = t->slot_w >= 110;
		if (!t->labels)
			t->slot_w = h + 4;
		group = start_w + (search_w ? search_w + 8 : 0) + 6 + (f32)t->n * t->slot_w;
	}
	if (group > room && search_w) {
		search_w = 0;
		group = start_w + 6 + (f32)t->n * t->slot_w;
	}
	if (group > room)
		t->slot_w = floorf(maxf(28, (room - start_w - 6) / (f32)MAX(t->n, 1)));
	group = start_w + (search_w ? search_w + 8 : 0) + 6 + (f32)t->n * t->slot_w;
	f32 x0 = S.set.taskbar_center ? roundf(minf((S.sw - group) / 2, room - group)) : 4;
	t->start = rect(maxf(4, x0), t->bar.y, start_w, h);
	f32 ax = t->start.x + start_w + 4;
	if (search_w) {
		f32 sh = roundf(h * 0.66f);
		t->search = rect(ax, roundf(t->bar.y + (h - sh) / 2), search_w, sh);
		ax += search_w + 8;
	}
	t->apps_x = ax + 2;
}

Rect shell_dock_slot(int n, int i)
{
	Taskbar t;
	shell_taskbar(&t);
	UNUSED(n);
	return rect(t.apps_x + (f32)i * t.slot_w, t.bar.y, t.slot_w, t.bar.h);
}

/* Is (x, y) on the taskbar? */
static bool dock_hit(f32 x, f32 y)
{
	UNUSED(x);
	return y >= S.sh - shell_taskbar_height();
}

Rect shell_dock_icon_rect(const char *app_id)
{
	Taskbar t;
	shell_taskbar(&t);
	char ids[32][48];
	int n = shell_dock_items(ids, 32);
	for (int i = 0; i < n; i++)
		if (str_eq(ids[i], app_id)) {
			Rect s = shell_dock_slot(n, i);
			return rect(t.labels ? s.x + 10 : s.x + (s.w - t.icon) / 2, s.y + (s.h - t.icon) / 2, t.icon, t.icon);
		}
	return center_in(t.start, t.icon, t.icon);
}

/* ---- Drawing dispatch (in zui frame) ------------------------------------------------- */

void topbar_draw(Ui *ui);
void taskbar_draw(Ui *ui);
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
	if (!S.locked && !S.setup_mode) {
		/* Start covers the whole screen and the top bar; the taskbar stays */
		/* under a settled Start screen the top bar mustn't catch clicks */
		if (!S.launcher_open || S.launcher_k < 0.98f)
			topbar_draw(ui);
		if (S.launcher_open || S.launcher_k > 0)
			launcher_draw(ui);
		taskbar_draw(ui);
		desktop_menu_draw(ui);
		clip_history_draw(ui);
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
		Rect r = rect(S.sw - w - 10, shell_panel_height() + 10, w, 24);
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
	desktop_init();
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

/* Nothing of the shell is on screen this frame (after shell_prepare). */
bool shell_idle(void)
{
	return nsurfaces == 0;
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
	if (desktop_wants_pointer() || clip_history_open())
		return true;
	if (S.py < shell_panel_height())
		return true;
	if (S.dock_hide_k < 0.5f && dock_hit(S.px, S.py))
		return true;
	return false;
}

/* The taskbar stays put, keeping its strip of the screen. With auto-hide on
 * it slides away and comes back when the pointer reaches the bottom edge;
 * it is also out of the way of full-screen windows. */
static bool dock_should_hide(void)
{
	if (S.launcher_open || S.switcher_open || S.locked || S.dock_menu[0] || S.notifications_open || S.session_menu_open)
		return false;
	f32 h = shell_taskbar_height();
	if (S.py >= S.sh - 3)
		S.dock_revealed = true;
	else if (S.dock_revealed && S.py < S.sh - h - 40)
		S.dock_revealed = false;
	if (S.focus && (S.focus->states & STATE_FULLSCREEN) && !S.focus->minimized)
		return !S.dock_revealed;
	return S.set.dock_autohide && !S.dock_revealed;
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
	/* Global shortcuts. Tapping Super alone opens Applications, like elementary. */
	static bool super_alone;
	if (state == KEY_RELEASED && (code == KEY_LEFTMETA || code == KEY_RIGHTMETA)) {
		bool tap = super_alone;
		super_alone = false;
		if (tap) {
			S.launcher_open = !S.launcher_open;
			if (S.launcher_open)
				shell_reload_apps();
			shell_mark_dirty();
			return true;
		}
	}
	if (pressed) {
		super_alone = (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) && state == KEY_PRESSED;
		if ((mods & MOD_ALT) && code == KEY_F4 && S.focus) {
			window_close_request(S.focus);
			return true;
		}
		if ((mods & MOD_SUPER) && code == KEY_SPACE) {
			S.launcher_open = !S.launcher_open;
			if (S.launcher_open)
				shell_reload_apps();
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
		/* Super+V: what was copied lately */
		if ((mods & MOD_SUPER) && code == KEY_V && !S.locked) {
			clip_history_toggle();
			return true;
		}
		/* Shift+Print or Super+Shift+S: a screenshot of an area, dragged out */
		if ((code == KEY_PRINT && (mods & MOD_SHIFT)) || ((mods & MOD_SUPER) && (mods & MOD_SHIFT) && code == KEY_S)) {
			screenshot_request(NULL, 0, SCREENSHOT_AREA);
			return true;
		}
		if (code == KEY_PRINT) {
			shell_screenshot();
			return true;
		}
		/* Ctrl+Alt+T opens a terminal, Super+E the files, as people are used to */
		if ((mods & MOD_CTRL) && (mods & MOD_ALT) && code == KEY_T) {
			shell_launch_new("forge");
			return true;
		}
		if ((mods & MOD_SUPER) && code == KEY_E) {
			shell_launch_new("vault");
			return true;
		}
		/* Super+arrows: halves of the screen, maximize, and back (or away) */
		if ((mods & MOD_SUPER) && !(mods & (MOD_CTRL | MOD_ALT)) && (code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT) && S.focus &&
		    !(S.focus->flags & WIN_POPUP)) {
			Window *w = S.focus;
			if (code == KEY_UP)
				window_maximize(w, true);
			else if (code == KEY_DOWN) {
				if (w->states & (STATE_MAXIMIZED | STATE_TILED_LEFT | STATE_TILED_RIGHT))
					window_maximize(w, false);
				else
					window_minimize(w, true);
			} else
				window_tile(w, code == KEY_LEFT ? -1 : 1);
			return true;
		}
		/* F11: full screen for the window in front (and game mode with it) */
		if (code == KEY_F11 && !mods && S.focus && !(S.focus->flags & WIN_POPUP) && (S.focus->flags & WIN_RESIZABLE)) {
			window_fullscreen(S.focus, !(S.focus->states & STATE_FULLSCREEN));
			return true;
		}
		if ((mods & MOD_CTRL) && (mods & MOD_ALT) && (code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT)) {
			Window *w = S.focus;
			if (w) {
				if (code == KEY_UP) window_maximize(w, true);
				else if (code == KEY_DOWN) window_maximize(w, false);
				else window_tile(w, code == KEY_LEFT ? -1 : 1);
			}
			return true;
		}
		if ((desktop_wants_keys() || clip_history_open()) && !S.launcher_open) {
			ui_input_key(ui, code, state, mods, text);
			shell_mark_dirty();
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
	desktop_tick();
	dnd_tick();
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
	if (dnd_animating())
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
