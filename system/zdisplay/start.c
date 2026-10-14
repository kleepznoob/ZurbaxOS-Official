/*
 * Start: a full-screen Start screen in the manner of Windows 8.1.
 *
 * Tiles in named groups scroll sideways over the softened wallpaper: flat
 * colours with white glyphs for Zurbax's apps, the app's own icon for Linux
 * programs, and live tiles for the calendar, the clock and the desktop.
 * Tiles come in four sizes, can be dragged to new places, and right-clicking
 * one brings up the app bar (unpin, pin to the taskbar, resize). The arrow at
 * the bottom left opens All apps, A to Z. Typing anything searches apps,
 * Settings pages, power actions, sums and the web, in a pane on the right.
 *
 * The layout is kept in ~/.config/zurbax/start.conf.
 */
#include "zdisplay.h"

enum { T_SMALL, T_MEDIUM, T_WIDE, T_LARGE };
enum { VIEW_START, VIEW_APPS };

#define HALF 56                 /* a small tile; the grid's unit */
#define GAP 8
#define UNIT (HALF + GAP)
#define BLOCK 4                 /* a group's column of tiles is a wide tile across, in units */
#define LEFT 116                /* the screen's left margin */
#define GROUP_GAP 56

typedef struct {
	char app[96];               /* an app id, or "desktop" */
	int size;
	int group;
	/* placed */
	Rect r;                     /* in content coordinates (before scrolling) */
} Tile;

static Tile tiles[96];
static int ntiles;
static char groups[10][32];
static int ngroups;
static bool loaded;
static char loaded_home[128];

static int view;
static f32 scroll_x, scroll_target, content_w;
static int sel = -1;            /* the tile picked by right-click or the keyboard */
static bool sel_menu;           /* the app bar is up for it */
static u64 bar_frame;           /* when it came up */
static char apps_sel[96];       /* the All apps entry right-clicked */
static StrBuf query;
static bool searching;
static int nav = -1;            /* the keyboard's tile */
static f64 opened_at;
static bool was_open;
/* dragging a tile */
static int drag = -1, press_tile = -1;
static f32 press_x, press_y, drag_dx, drag_dy;
static int drop_before = -1, drop_group = -1;
static bool power_menu, user_menu;
static Rect power_rect, user_rect;
static u64 menu_frame;

/* ---- Looks: a colour and a glyph per Zurbax app -------------------------------------------- */

static const struct {
	const char *id, *glyph, *color;
} LOOKS[] = {
	{ "vault", "metro-folder", "#e3a21a" },     { "drift", "metro-globe", "#2d89ef" },     { "mail", "metro-mail", "#2b5797" },
	{ "music", "metro-music", "#da532c" },      { "photos", "metro-image", "#00aba9" },    { "calendar", "metro-calendar", "#603cba" },
	{ "notes", "metro-note", "#e3a21a" },       { "settings", "metro-settings", "#4d4d4d" }, { "forge", "metro-console", "#1d1d1d" },
	{ "harbor", "metro-bag", "#00a300" },       { "dashboard", "metro-board", "#9f00a7" }, { "weather", "metro-weather", "#2d89ef" },
	{ "clock", "metro-clock", "#1e7145" },      { "calculator", "metro-calculator", "#2b5797" }, { "camera", "metro-camera", "#b91d47" },
	{ "writer", "metro-document", "#2b5797" },  { "arcade", "metro-games", "#00a300" },     { "steam", "metro-controller", "#1e7145" },
	{ "screenshot", "metro-screenshot", "#603cba" }, { "screentime", "metro-hourglass", "#da532c" }, { "pulse", "metro-pulse", "#b91d47" },
	{ "help", "metro-help", "#2d89ef" },        { "welcome", "metro-wave", "#00aba9" },     { "installer", "metro-download", "#00a300" },
	{ "desktop", "metro-desktop", "#2b5797" },
};

/* A Linux program's tile takes the main colour of its icon. */
static struct {
	char id[96];
	Color c;
} icon_colors[64];
static int nicon_colors;

static Color tile_color_of(const char *id, const char *icon)
{
	for (usize i = 0; i < ARRAY_LEN(LOOKS); i++)
		if (str_eq(LOOKS[i].id, id))
			return color_hex(LOOKS[i].color);
	for (int i = 0; i < nicon_colors; i++)
		if (str_eq(icon_colors[i].id, id))
			return icon_colors[i].c;
	Color c = color_hex("#2b5797");
	Canvas *img = NULL;
	if (icon && icon[0] == '/') {
		img = image_load_thumb(icon, 24);
	} else if (icon) {
		char *rel = str_fmt("share/icons/apps/%s.svg", icon);
		char *path = zurbax_path(rel);
		SvgDoc *d = svg_load(path);
		if (d) {
			img = svg_rasterize(d, 24, 24, COLOR_BLACK);
			svg_free(d);
		}
		mem_free(rel);
		mem_free(path);
	}
	if (img) {
		/* the average of its colourful, solid pixels, darkened so white reads on it */
		f64 r = 0, g = 0, b = 0, n = 0;
		for (int y = 0; y < img->height; y++)
			for (int x = 0; x < img->width; x++) {
				Color p = color_unpremul(canvas_row(img, y)[x]);
				int pr = (int)COLOR_R(p), pg = (int)COLOR_G(p), pb = (int)COLOR_B(p);
				int mx = MAX(pr, MAX(pg, pb)), mn = MIN(pr, MIN(pg, pb));
				if (COLOR_A(p) < 200 || mx - mn < 40)
					continue;
				r += pr, g += pg, b += pb, n++;
			}
		if (n > 4)
			c = color_lighten(RGB((u8)(r / n), (u8)(g / n), (u8)(b / n)), -0.25f);
		canvas_free(img);
	}
	if (nicon_colors < (int)ARRAY_LEN(icon_colors)) {
		str_copy(icon_colors[nicon_colors].id, id, sizeof icon_colors[0].id);
		icon_colors[nicon_colors++].c = c;
	}
	return c;
}

static const char *glyph_of(const char *id)
{
	for (usize i = 0; i < ARRAY_LEN(LOOKS); i++)
		if (str_eq(LOOKS[i].id, id))
			return LOOKS[i].glyph;
	return NULL;
}

static const char *name_of(const char *id)
{
	if (str_eq(id, "desktop"))
		return "Desktop";
	const AppInfo *a = shell_app(id);
	return a ? a->name : id;
}

/* ---- The layout, kept in start.conf --------------------------------------------------------- */

static char *conf_path(void)
{
	return str_fmt("%s/.config/zurbax/start.conf", S.set.home);
}

static int add_group(const char *name)
{
	for (int i = 0; i < ngroups; i++)
		if (str_eq(groups[i], name))
			return i;
	if (ngroups >= (int)ARRAY_LEN(groups))
		return ngroups - 1;
	str_copy(groups[ngroups], name, sizeof groups[0]);
	return ngroups++;
}

static bool pinned(const char *id)
{
	for (int i = 0; i < ntiles; i++)
		if (str_eq(tiles[i].app, id))
			return true;
	return false;
}

static void add_tile(int group, const char *id, int size)
{
	if (ntiles >= (int)ARRAY_LEN(tiles) || pinned(id) || (!str_eq(id, "desktop") && !shell_app(id)))
		return;
	Tile *t = &tiles[ntiles++];
	memset(t, 0, sizeof *t);
	str_copy(t->app, id, sizeof t->app);
	t->size = size;
	t->group = group;
}

static void save_layout(void)
{
	StrBuf sb = { 0 };
	sb_adds(&sb, "# The Start screen: groups and their tiles (small, medium, wide or large)\n");
	for (int g = 0; g < ngroups; g++) {
		sb_fmt(&sb, "group %s\n", groups[g]);
		for (int i = 0; i < ntiles; i++)
			if (tiles[i].group == g)
				sb_fmt(&sb, "tile %s %s\n", tiles[i].app,
				       tiles[i].size == T_SMALL ? "small" : tiles[i].size == T_WIDE ? "wide" : tiles[i].size == T_LARGE ? "large" : "medium");
	}
	char *p = conf_path();
	char *d = path_dir(p);
	if (!dir_exists(d)) {
		dir_make(d, 0755);
		sys_chown(d, S.set.uid, S.set.gid);
	}
	if (file_write_atomic(p, sb.data, sb.len))
		sys_chown(p, S.set.uid, S.set.gid);
	mem_free(d);
	mem_free(p);
	sb_free(&sb);
}

static void default_layout(void)
{
	int g = add_group("Everyday");
	add_tile(g, "desktop", T_WIDE);
	add_tile(g, "drift", T_MEDIUM);
	add_tile(g, "mail", T_MEDIUM);
	add_tile(g, "calendar", T_MEDIUM);
	add_tile(g, "weather", T_MEDIUM);
	add_tile(g, "photos", T_WIDE);
	add_tile(g, "vault", T_MEDIUM);
	add_tile(g, "music", T_MEDIUM);
	g = add_group("Play");
	add_tile(g, "steam", T_WIDE);
	add_tile(g, "arcade", T_MEDIUM);
	add_tile(g, "camera", T_MEDIUM);
	g = add_group("Work");
	add_tile(g, "writer", T_MEDIUM);
	add_tile(g, "notes", T_MEDIUM);
	add_tile(g, "dashboard", T_WIDE);
	add_tile(g, "calculator", T_SMALL);
	add_tile(g, "clock", T_SMALL);
	add_tile(g, "forge", T_SMALL);
	add_tile(g, "screenshot", T_SMALL);
	g = add_group("System");
	add_tile(g, "settings", T_MEDIUM);
	add_tile(g, "harbor", T_MEDIUM);
	add_tile(g, "installer", T_WIDE);
	add_tile(g, "help", T_SMALL);
	add_tile(g, "screentime", T_SMALL);
	add_tile(g, "pulse", T_SMALL);
	/* the Linux programs that came installed */
	int apps = -1;
	for (int i = 0; i < S.napps; i++)
		if (S.apps[i].is_linux && ntiles < 40) {
			if (apps < 0)
				apps = add_group("Apps");
			add_tile(apps, S.apps[i].id, T_MEDIUM);
		}
}

static void load_layout(void)
{
	loaded = true;
	str_copy(loaded_home, S.set.home, sizeof loaded_home);
	ntiles = ngroups = 0;
	char *p = conf_path();
	char *text = file_read(p, NULL);
	mem_free(p);
	if (!text) {
		default_layout();
		return;
	}
	usize n;
	char **lines = str_split(text, '\n', &n);
	int g = -1;
	for (usize i = 0; i < n; i++) {
		char *l = str_trim(lines[i]);
		if (str_starts(l, "group ")) {
			g = add_group(str_trim(l + 6));
		} else if (str_starts(l, "tile ") && g >= 0) {
			char *id = str_trim(l + 5), *sp = (char *)str_chr(id, ' ');
			int size = T_MEDIUM;
			if (sp) {
				*sp = 0;
				const char *sz = str_trim(sp + 1);
				size = str_eq(sz, "small") ? T_SMALL : str_eq(sz, "wide") ? T_WIDE : str_eq(sz, "large") ? T_LARGE : T_MEDIUM;
			}
			add_tile(g, id, size);
		}
	}
	str_split_free(lines, n);
	mem_free(text);
	if (!ntiles)
		default_layout();
}

/* ---- Placing the tiles ---------------------------------------------------------------------- */

static void tile_units(int size, int *w, int *h)
{
	*w = size == T_SMALL ? 1 : size == T_MEDIUM ? 2 : 4;
	*h = size == T_SMALL ? 1 : size == T_LARGE ? 4 : 2;
}

static f32 tiles_top(void)
{
	return 150;
}

/* Panes and menus over the tiles: the tiles under them don't take the pointer. */
static void above_begin(Ui *ui, Rect r)
{
	ui->overlay_rect = rect_union(ui->overlay_rect, r);
	ui->in_overlay = true;
}

static void above_end(Ui *ui)
{
	ui->in_overlay = false;
}

/* The screen's bottom: the taskbar stays over Start, to close it again. */
static f32 bottom(void)
{
	return S.sh - shell_taskbar_reserved();
}

/* Lays every group out in columns a wide tile across, filling down then over. */
static void place(int rows)
{
	f32 x = LEFT;
	for (int g = 0; g < ngroups; g++) {
		static u8 used[64][32];  /* [column][row], in units */
		memset(used, 0, sizeof used);
		int maxcol = 0;
		bool any = false;
		for (int i = 0; i < ntiles; i++) {
			Tile *t = &tiles[i];
			if (t->group != g)
				continue;
			any = true;
			int w, h;
			tile_units(t->size, &w, &h);
			bool done = false;
			for (int block = 0; block < 16 && !done; block++)
				for (int y = 0; y + h <= rows && !done; y++)
					for (int cx = 0; cx + w <= BLOCK && !done; cx++) {
						int c0 = block * BLOCK + cx;
						bool free_ = true;
						for (int a = 0; a < w && free_; a++)
							for (int b = 0; b < h && free_; b++)
								free_ = !used[c0 + a][y + b];
						if (!free_)
							continue;
						for (int a = 0; a < w; a++)
							for (int b = 0; b < h; b++)
								used[c0 + a][y + b] = 1;
						t->r = rect(x + (f32)c0 * UNIT, tiles_top() + (f32)y * UNIT, (f32)(w * UNIT - GAP), (f32)(h * UNIT - GAP));
						maxcol = MAX(maxcol, c0 + w);
						done = true;
					}
		}
		if (!any)
			continue;
		x += (f32)(((maxcol + BLOCK - 1) / BLOCK) * BLOCK) * UNIT - GAP + GROUP_GAP;
	}
	content_w = x - GROUP_GAP + LEFT;
}

/* ---- Drawing a tile ------------------------------------------------------------------------- */

static void tile_face(Ui *ui, const Tile *t, Rect r, f32 alpha)
{
	const AppInfo *a = shell_app(t->app);
	Color bg = tile_color_of(t->app, a ? a->icon : NULL);
	Color white = color_scale_alpha(COLOR_WHITE, alpha);
	if (str_eq(t->app, "desktop")) {
		/* the desktop itself, in miniature */
		Rect d = ui_to_device(ui, r);
		blit_scaled(ui->canvas, S.wallpaper, rect(0, 0, (f32)S.wallpaper->width, (f32)S.wallpaper->height), d, alpha);
		ui_fill(ui, rect(r.x, r.y + r.h - 30, r.w, 30), color_scale_alpha(color_hex("#00000060"), alpha));
		ui_text(ui, ui_font(ui, 13, 400), r.x + 10, r.y + r.h - 10, "Desktop", white);
		return;
	}
	ui_fill(ui, r, color_scale_alpha(bg, alpha));
	Civil c;
	civil_local(time_unix(), &c);
	bool small = t->size == T_SMALL;
	/* live tiles: today, and the time */
	if (str_eq(t->app, "calendar") && !small) {
		char day[8];
		fmt(day, sizeof day, "%d", c.day);
		Font big = ui_font(ui, t->size == T_LARGE ? 96 : 52, 300);
		ui_text_in(ui, big, rect(r.x, r.y + r.h * 0.12f, r.w, r.h * 0.6f), day, white, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 15, 400), rect(r.x, r.y + r.h * 0.66f, r.w, 20), WEEKDAY_NAMES[c.wday], white, ALIGN_CENTER);
		return;
	}
	if (str_eq(t->app, "clock") && !small) {
		char tm[16];
		if (S.set.clock_24h)
			fmt(tm, sizeof tm, "%d:%02d", c.hour, c.min);
		else
			fmt(tm, sizeof tm, "%d:%02d", c.hour % 12 ? c.hour % 12 : 12, c.min);
		ui_text_in(ui, ui_font(ui, t->size == T_LARGE ? 64 : 34, 300), rect(r.x, r.y + r.h * 0.18f, r.w, r.h * 0.5f), tm, white, ALIGN_CENTER);
		ui_text(ui, ui_font(ui, 13, 400), r.x + 10, r.y + r.h - 10, "Clock", white);
		return;
	}
	const char *glyph = glyph_of(t->app);
	f32 gs = small ? 30 : t->size == T_LARGE ? 96 : 48;
	Rect gr = rect(r.x + (r.w - gs) / 2, r.y + (r.h - gs) / 2 - (small ? 0 : 8), gs, gs);
	if (glyph)
		ui_icon(ui, gr, glyph, white);
	else
		ui_app_icon(ui, gr, a ? a->icon : t->app);
	if (!small) {
		ui_push_clip(ui, r);
		ui_text(ui, ui_font(ui, 13, 400), r.x + 10, r.y + r.h - 10, name_of(t->app), white);
		ui_pop_clip(ui);
	}
}

/* ---- Launching ---------------------------------------------------------------------------------- */

static Conf *usage;

static Conf *usage_conf(void)
{
	if (!usage) {
		char *path = str_fmt("%s/.config/zurbax/launcher.conf", S.set.home);
		usage = conf_load(path);
		mem_free(path);
	}
	return usage;
}

static void close_start(void)
{
	S.launcher_open = false;
	sel_menu = false;
	sel = -1;
	power_menu = user_menu = false;
}

static void open_app(const char *id)
{
	if (str_eq(id, "desktop")) {
		close_start();
		return;
	}
	Conf *c = usage_conf();
	char key[64];
	fmt(key, sizeof key, "use.%s", id);
	conf_set_int(c, key, conf_get_int(c, key, 0) + 1);
	conf_save(c);
	shell_launch(id, NULL);
	close_start();
}

/* ---- Search: apps, Settings pages, power, sums, the web --------------------------------------- */

static const char *calc_p;
static bool calc_ok;
static f64 calc_expr(void);

static void calc_space(void)
{
	while (*calc_p == ' ')
		calc_p++;
}

static f64 calc_atom(void)
{
	calc_space();
	if (*calc_p == '(') {
		calc_p++;
		f64 v = calc_expr();
		calc_space();
		if (*calc_p == ')')
			calc_p++;
		else
			calc_ok = false;
		return v;
	}
	if (*calc_p == '-') {
		calc_p++;
		return -calc_atom();
	}
	if (*calc_p == '+') {
		calc_p++;
		return calc_atom();
	}
	if (!is_digit(*calc_p) && *calc_p != '.') {
		calc_ok = false;
		return 0;
	}
	const char *end;
	f64 v = str_to_float(calc_p, &end);
	if (end == calc_p)
		calc_ok = false;
	calc_p = end;
	return v;
}

static f64 calc_power(void)
{
	f64 v = calc_atom();
	calc_space();
	if (*calc_p == '^') {
		calc_p++;
		v = pow(v, calc_power());
	}
	return v;
}

static f64 calc_term(void)
{
	f64 v = calc_power();
	for (;;) {
		calc_space();
		char op = *calc_p;
		if (op != '*' && op != '/' && op != '%' && op != 'x')
			return v;
		calc_p++;
		f64 r = calc_power();
		if (op == '*' || op == 'x')
			v *= r;
		else if (op == '/')
			v = r != 0 ? v / r : (calc_ok = false, 0);
		else
			v = r != 0 ? fmod(v, r) : (calc_ok = false, 0);
	}
}

static f64 calc_expr(void)
{
	f64 v = calc_term();
	for (;;) {
		calc_space();
		if (*calc_p == '+')
			calc_p++, v += calc_term();
		else if (*calc_p == '-')
			calc_p++, v -= calc_term();
		else
			return v;
	}
}

static bool calculate(const char *q, char *out, usize cap)
{
	bool has_op = false, has_digit = false;
	for (const char *s = q; *s; s++) {
		if (is_digit(*s))
			has_digit = true;
		else if (str_chr("+-*/%^x()", *s))
			has_op = true;
		else if (*s != ' ' && *s != '.')
			return false;
	}
	if (!has_digit || !has_op)
		return false;
	calc_p = q;
	calc_ok = true;
	f64 v = calc_expr();
	calc_space();
	if (!calc_ok || *calc_p || v != v)
		return false;
	if (fabs(v - round(v)) < 1e-9 && fabs(v) < 1e15)
		fmt(out, cap, "%lld", (long long)round(v));
	else
		fmt(out, cap, "%.6g", v);
	return true;
}

static const struct {
	const char *title, *icon, *keywords;
} SETTINGS_PAGES[] = {
	{ "Appearance", "palette", "dark light style accent colour color wallpaper background theme halloween" },
	{ "Desktop", "computer", "taskbar top bar panel clock seconds battery lock autohide start" },
	{ "Network", "network-wireless", "wifi wi-fi internet ethernet wireless" },
	{ "Bluetooth", "bluetooth", "bluetooth headphones devices" },
	{ "Display", "display", "scale resolution night light brightness monitor" },
	{ "Sound", "audio-volume-high", "volume speaker audio mute microphone" },
	{ "Mouse & Touchpad", "touchpad", "pointer speed natural scrolling tap click trackpad" },
	{ "Keyboard", "keyboard", "layout language shortcuts keys" },
	{ "Users", "user", "account name password avatar login" },
	{ "Date & Time", "clock", "time zone 24 hour clock date" },
	{ "Gaming", "gamepad", "game mode controller fps" },
	{ "Screen Time", "hourglass", "screen time limits usage parental" },
	{ "Privacy & Security", "shield", "privacy security updates firewall lock camera microphone" },
	{ "About", "info", "about version hardware system update" },
};

static const struct {
	const char *title, *icon, *keywords;
	u32 action;
} ACTIONS[] = {
	{ "Lock", "lock", "lock screen away", SESSION_LOCK },
	{ "Sign Out", "system-log-out", "log out sign out logout", SESSION_LOGOUT },
	{ "Restart", "system-restart", "restart reboot", SESSION_RESTART },
	{ "Shut Down", "system-shutdown", "shut down shutdown power off turn off", SESSION_SHUTDOWN },
	{ "Sleep", "system-sleep", "sleep suspend", SESSION_SLEEP },
};

static bool app_matches(const AppInfo *a, const char *q)
{
	return str_find_nocase(a->name, q) || str_find_nocase(a->keywords, q) || str_find_nocase(a->category, q) ||
	       str_find_nocase(a->id, q);
}

static void url_encode(const char *in, char *out, usize cap)
{
	usize n = 0;
	for (const u8 *s = (const u8 *)in; *s && n + 4 < cap; s++) {
		if (is_alnum((char)*s) || str_chr("-_.~", (char)*s))
			out[n++] = (char)*s;
		else if (*s == ' ')
			out[n++] = '+';
		else
			n += (usize)fmt(out + n, cap - n, "%%%02X", *s);
	}
	out[n] = 0;
}

/* One result: what it is, and what it does. */
static bool result(Ui *ui, Rect r, const char *glyph, const char *app_icon, Color tile, const char *title, const char *sub, f32 k)
{
	UiId id = ui_id(ui, title);
	UiInteract it = ui_interact(ui, id, r);
	if (it.hovered)
		ui_fill(ui, r, color_scale_alpha(color_hex("#ffffff1c"), k));
	Rect sq = rect(r.x + 8, r.y + (r.h - 36) / 2, 36, 36);
	if (app_icon && !glyph) {
		ui_fill(ui, sq, color_scale_alpha(tile, k));
		ui_app_icon(ui, pad(sq, 4), app_icon);
	} else {
		ui_fill(ui, sq, color_scale_alpha(tile, k));
		ui_icon(ui, pad(sq, 8), glyph, color_scale_alpha(COLOR_WHITE, k));
	}
	ui_text(ui, ui_font(ui, 15, 400), r.x + 56, r.y + (sub ? 22 : 30), title, color_scale_alpha(COLOR_WHITE, k));
	if (sub)
		ui_text(ui, ui_font(ui, 12, 400), r.x + 56, r.y + 40, sub, color_scale_alpha(color_hex("#ffffffa0"), k));
	return it.clicked;
}

/* The search pane, sliding in from the right. */
static void search_pane(Ui *ui, f32 k)
{
	f32 w = 400;
	f32 slide = (1 - ease_out_cubic(clampf((f32)(S.now - opened_at) * 5, 0, 1))) * 60;
	Rect pane = rect(S.sw - w + slide, 0, w, bottom());
	ui_fill(ui, pane, color_scale_alpha(color_hex("#1b1b1bf4"), k));
	shell_surface(pane);
	Rect c = pad(pane, 28);
	ui_text(ui, ui_font(ui, 30, 300), c.x, c.y + 32, "Search", color_scale_alpha(COLOR_WHITE, k));
	cut_top(&c, 52);
	Rect box = cut_top(&c, 38);
	ui_fill(ui, box, color_scale_alpha(COLOR_WHITE, k));
	Theme saved = ui->theme;
	ui->theme.text = color_hex("#111111");
	ui->theme.text_dim = ui->theme.text_faint = color_hex("#777777");
	ui->theme.input = COLOR_WHITE;
	int ev = ui_entry(ui, pad4(box, 0, 4, 0, 4), "start-search", &query, "Everywhere", ENTRY_FLAT | ENTRY_AUTOFOCUS);
	ui->theme = saved;
	if (ui->focus != ui_id(ui, "start-search"))
		ui_focus(ui, "start-search");
	ui_icon(ui, rect(box.x + box.w - 30, box.y + 9, 20, 20), "metro-search", color_hex("#444444"));
	cut_top(&c, 18);
	const char *q = sb_str(&query);
	bool first = true, go = (ev & ENTRY_SUBMIT) != 0;
	f32 rh = 56;
	if (*q) {
		char res[64];
		if (calculate(q, res, sizeof res) && c.h > rh) {
			char title[80];
			fmt(title, sizeof title, "= %s", res);
			if (result(ui, cut_top(&c, rh), "metro-calculator", NULL, color_hex("#2b5797"), title, "Copy the answer", k) || (go && first)) {
				mem_free(S.clipboard);
				S.clipboard = str_dup(res);
				S.clipboard_len = strlen(res);
				str_copy(S.clipboard_mime, "text/plain", sizeof S.clipboard_mime);
				clip_history_add("text/plain", res, strlen(res));
				wl_clipboard_changed();
				shell_notify("zurbax", "calculator", "Copied", res);
				close_start();
				return;
			}
			first = false;
		}
		for (int i = 0; i < S.napps && c.h > rh; i++) {
			AppInfo *a = &S.apps[i];
			if (!app_matches(a, q))
				continue;
			const char *glyph = glyph_of(a->id);
			if (result(ui, cut_top(&c, rh), glyph, a->icon, tile_color_of(a->id, a->icon), a->name, a->is_linux ? "App" : "Zurbax app", k) ||
			    (go && first)) {
				open_app(a->id);
				return;
			}
			first = false;
		}
		for (usize i = 0; i < ARRAY_LEN(SETTINGS_PAGES) && c.h > rh; i++) {
			if (!str_find_nocase(SETTINGS_PAGES[i].title, q) && !str_find_nocase(SETTINGS_PAGES[i].keywords, q))
				continue;
			if (result(ui, cut_top(&c, rh), "metro-settings", NULL, color_hex("#4d4d4d"), SETTINGS_PAGES[i].title, "Settings", k) || (go && first)) {
				shell_launch("settings", SETTINGS_PAGES[i].title);
				close_start();
				return;
			}
			first = false;
		}
		for (usize i = 0; i < ARRAY_LEN(ACTIONS) && c.h > rh; i++) {
			if (!str_find_nocase(ACTIONS[i].title, q) && !str_find_nocase(ACTIONS[i].keywords, q))
				continue;
			if (result(ui, cut_top(&c, rh), "metro-power", NULL, color_hex("#b91d47"), ACTIONS[i].title, "Power", k) || (go && first)) {
				close_start();
				shell_session_action(ACTIONS[i].action);
				return;
			}
			first = false;
		}
		if (c.h > rh * 2) {
			char title[160], enc[256], url[320];
			fmt(title, sizeof title, "“%s” on the web", q);
			if (result(ui, cut_top(&c, rh), "metro-globe", NULL, color_hex("#2d89ef"), title, "Search with Drift", k) || (go && first)) {
				url_encode(q, enc, sizeof enc);
				fmt(url, sizeof url, "https://duckduckgo.com/?q=%s", enc);
				shell_launch("drift", url);
				close_start();
				return;
			}
			fmt(title, sizeof title, "“%s” in Harbor", q);
			if (result(ui, cut_top(&c, rh), "metro-bag", NULL, color_hex("#00a300"), title, "Thousands of apps", k)) {
				char arg[300];
				fmt(arg, sizeof arg, "search:%s", q);
				shell_launch("harbor", arg);
				close_start();
				return;
			}
		}
	} else {
		ui_text(ui, ui_font(ui, 14, 400), c.x, c.y + 16, "Apps, settings, files, sums and the web.", color_scale_alpha(color_hex("#ffffffa0"), k));
	}
	if (ev & ENTRY_ESCAPE) {
		searching = false;
		sb_clear(&query);
	}
}

/* ---- The app bar: what to do with the tile (or app) right-clicked ------------------------------ */

static bool bar_button(Ui *ui, f32 x, f32 y, const char *glyph, const char *label, f32 k)
{
	Rect r = rect(x, y, 96, 76);
	UiId id = ui_id(ui, label);
	UiInteract it = ui_interact(ui, id, r);
	Color c = color_scale_alpha(COLOR_WHITE, k * (it.hovered ? 1 : 0.85f));
	ui_ring(ui, x + 48, y + 22, 18, 1.8f, c);
	if (it.held)
		ui_circle(ui, x + 48, y + 22, 18, color_scale_alpha(COLOR_WHITE, 0.3f * k));
	ui_icon(ui, rect(x + 38, y + 12, 20, 20), glyph, c);
	/* labels take two lines when they need them, as on Windows 8.1 */
	ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(x + 4, y + 44, 88, 32), label, c, ALIGN_CENTER);
	return it.clicked;
}

static bool on_taskbar(const char *id)
{
	usize n;
	char **parts = str_split(S.set.dock_apps, ',', &n);
	bool yes = false;
	for (usize i = 0; i < n; i++)
		yes |= str_eq(str_trim(parts[i]), id);
	str_split_free(parts, n);
	return yes;
}

static void toggle_taskbar_pin(const char *id)
{
	bool was = on_taskbar(id);
	StrBuf sb = { 0 };
	usize n;
	char **parts = str_split(S.set.dock_apps, ',', &n);
	for (usize i = 0; i < n; i++) {
		char *p = str_trim(parts[i]);
		if (!*p || str_eq(p, id))
			continue;
		if (sb.len)
			sb_addc(&sb, ',');
		sb_adds(&sb, p);
	}
	str_split_free(parts, n);
	if (!was) {
		if (sb.len)
			sb_addc(&sb, ',');
		sb_adds(&sb, id);
	}
	str_copy(S.set.dock_apps, sb_str(&sb), sizeof S.set.dock_apps);
	sb_free(&sb);
	settings_save();
}

/* Returns true if the bar took the click. */
static bool app_bar(Ui *ui, f32 k)
{
	const char *id = view == VIEW_START ? (sel >= 0 && sel < ntiles ? tiles[sel].app : NULL) : (apps_sel[0] ? apps_sel : NULL);
	if (!id || !sel_menu)
		return false;
	Rect bar = rect(0, bottom() - 92, S.sw, 92);
	ui_fill(ui, bar, color_scale_alpha(color_hex("#1d1d1df2"), k));
	shell_surface(bar);
	bool app = !str_eq(id, "desktop");
	f32 x = 24, y = bar.y + 8;
	bool took = ui_mouse_in(ui, bar);
	if (view == VIEW_START) {
		if (bar_button(ui, x, y, "pin", "Unpin from Start", k)) {
			memmove(&tiles[sel], &tiles[sel + 1], sizeof(Tile) * (usize)(ntiles - sel - 1));
			ntiles--;
			sel = -1;
			sel_menu = false;
			save_layout();
			return true;
		}
		x += 100;
	} else if (app) {
		bool p = pinned(id);
		if (bar_button(ui, x, y, "pin", p ? "Unpin from Start" : "Pin to Start", k)) {
			if (p) {
				for (int i = 0; i < ntiles; i++)
					if (str_eq(tiles[i].app, id)) {
						memmove(&tiles[i], &tiles[i + 1], sizeof(Tile) * (usize)(ntiles - i - 1));
						ntiles--;
						break;
					}
			} else {
				add_tile(add_group("Apps"), id, T_MEDIUM);
			}
			save_layout();
			sel_menu = false;
			return true;
		}
		x += 100;
	}
	if (app) {
		if (bar_button(ui, x, y, "metro-apps", on_taskbar(id) ? "Unpin from taskbar" : "Pin to taskbar", k)) {
			toggle_taskbar_pin(id);
			sel_menu = false;
			return true;
		}
		x += 100;
	}
	if (view == VIEW_START) {
		static const char *const NAMES[] = { "Small", "Medium", "Wide", "Large" };
		char label[32];
		int next = (tiles[sel].size + 1) % 4;
		fmt(label, sizeof label, "Resize: %s", NAMES[next]);
		if (bar_button(ui, x, y, "fullscreen", label, k)) {
			tiles[sel].size = next;
			save_layout();
			return true;
		}
		x += 100;
	}
	if (app && bar_button(ui, x, y, "tab-new", "Open new window", k)) {
		shell_launch_new(id);
		close_start();
		return true;
	}
	/* a click elsewhere puts the bar away */
	if (!took && ui->frame_no != bar_frame && (ui->pressed[BUTTON_LEFT] || ui->pressed[BUTTON_RIGHT]))
		sel_menu = false;
	return took;
}

/* ---- The header: Start, the person, power and search -------------------------------------------- */

static bool header(Ui *ui, const char *title, f32 k)
{
	Color w = color_scale_alpha(COLOR_WHITE, k);
	ui_text(ui, ui_font(ui, 42, 300), LEFT, 82, title, w);
	bool took = false;
	f32 x = S.sw - 48;
	/* search */
	Rect sr = rect(x - 8, 40, 40, 40);
	UiInteract si = ui_interact(ui, ui_id(ui, "start-searchbtn"), sr);
	if (si.hovered)
		ui_fill(ui, sr, color_scale_alpha(color_hex("#ffffff20"), k));
	ui_icon(ui, pad(sr, 10), "metro-search", w);
	if (si.clicked) {
		searching = true;
		opened_at = S.now;
		took = true;
	}
	x -= 48;
	Rect pr = rect(x - 8, 40, 40, 40);
	UiInteract pi = ui_interact(ui, ui_id(ui, "start-power"), pr);
	if (pi.hovered)
		ui_fill(ui, pr, color_scale_alpha(color_hex("#ffffff20"), k));
	ui_icon(ui, pad(pr, 10), "metro-power", w);
	if (pi.clicked) {
		power_menu = !power_menu;
		user_menu = sel_menu = false;
		menu_frame = ui->frame_no;
		took = true;
	}
	x -= 56;
	/* the person */
	const char *name = S.set.full_name[0] ? S.set.full_name : S.set.user_name[0] ? S.set.user_name : "You";
	Font nf = ui_font(ui, 20, 300);
	f32 nw = ui_text_width(ui, nf, name);
	Rect ur = rect(x - nw - 18, 36, nw + 18 + 48, 48);
	UiInteract ui_ = ui_interact(ui, ui_id(ui, "start-user"), ur);
	if (ui_.hovered)
		ui_fill(ui, ur, color_scale_alpha(color_hex("#ffffff20"), k));
	ui_text(ui, nf, ur.x + 8, ur.y + 31, name, w);
	Rect av = rect(ur.x + ur.w - 44, ur.y + 4, 40, 40);
	ui_fill(ui, av, color_scale_alpha(color_hex("#ffffff30"), k));
	ui_icon(ui, pad(av, 8), "metro-person", w);
	if (ui_.clicked) {
		user_menu = !user_menu;
		power_menu = sel_menu = false;
		menu_frame = ui->frame_no;
		took = true;
	}
	power_rect = pr, user_rect = ur;
	return took || ui_mouse_in(ui, ur) || ui_mouse_in(ui, pr) || ui_mouse_in(ui, sr);
}

/* The person's and the power menus, over everything else. */
static void header_menus(Ui *ui, f32 k)
{
	Color w = color_scale_alpha(COLOR_WHITE, k);
	if (!power_menu && !user_menu)
		return;
	static const char *const PW[] = { "Sleep", "Shut down", "Restart" };
	static const u32 PA[] = { SESSION_SLEEP, SESSION_SHUTDOWN, SESSION_RESTART };
	static const char *const US[] = { "Lock", "Sign out", "Change account settings" };
	const char *const *items = power_menu ? PW : US;
	Rect anchor = power_menu ? power_rect : user_rect;
	Rect m = rect(minf(anchor.x, S.sw - 236), anchor.y + anchor.h + 4, 220, 3 * 40 + 12);
	ui_fill(ui, m, color_scale_alpha(color_hex("#2b2b2b"), k));
	ui_border(ui, m, 0, 1, color_scale_alpha(color_hex("#ffffff30"), k));
	shell_surface(m);
	above_begin(ui, m);
	for (int i = 0; i < 3; i++) {
		Rect r = rect(m.x + 6, m.y + 6 + (f32)i * 40, m.w - 12, 40);
		ui_push_id(ui, 700 + i + (power_menu ? 10 : 0));
		UiInteract it = ui_interact(ui, ui_id(ui, "menuitem"), r);
		ui_pop_id(ui);
		if (it.hovered)
			ui_fill(ui, r, color_scale_alpha(color_hex("#ffffff20"), k));
		ui_text(ui, ui_font(ui, 14, 400), r.x + 12, r.y + 26, items[i], w);
		if (it.clicked) {
			if (power_menu) {
				close_start();
				shell_session_action(PA[i]);
			} else if (i == 0) {
				close_start();
				shell_lock();
			} else if (i == 1) {
				close_start();
				shell_session_action(SESSION_LOGOUT);
			} else {
				shell_launch("settings", "Users");
				close_start();
			}
			above_end(ui);
			return;
		}
	}
	bool inside = ui_mouse_in(ui, m);
	above_end(ui);
	if (!inside && !ui_mouse_in(ui, power_rect) && !ui_mouse_in(ui, user_rect) && ui->frame_no != menu_frame && (ui->pressed[BUTTON_LEFT] || ui->pressed[BUTTON_RIGHT]))
		power_menu = user_menu = false;
}

/* A round arrow at the bottom left: down to All apps, up back to Start. */
static bool arrow_button(Ui *ui, bool down, f32 k)
{
	Rect r = rect(LEFT, bottom() - 64, 36, 36);
	UiInteract it = ui_interact(ui, ui_id(ui, down ? "start-down" : "start-up"), r);
	Color c = color_scale_alpha(COLOR_WHITE, k * (it.hovered ? 1 : 0.75f));
	ui_ring(ui, r.x + 18, r.y + 18, 16, 1.6f, c);
	ui_icon(ui, rect(r.x + 9, r.y + 9, 18, 18), down ? "go-down" : "go-up", c);
	return it.clicked;
}

/* ---- All apps ------------------------------------------------------------------------------------ */

static void all_apps(Ui *ui, f32 k, f32 ox)
{
	Color w = color_scale_alpha(COLOR_WHITE, k);
	ui_text(ui, ui_font(ui, 42, 300), LEFT + ox, 82, "Apps", w);
	ui_text(ui, ui_font(ui, 18, 300), LEFT + ox + ui_text_width(ui, ui_font(ui, 42, 300), "Apps") + 18, 80, "by name", color_scale_alpha(color_hex("#ffffffb0"), k));
	f32 top = 140, bot = bottom() - 90, eh = 46, cw = 260;
	int rows = MAX(1, (int)((bot - top) / eh));
	int slot = 0;
	char letter = 0;
	f32 maxx = 0;
	for (int i = 0; i < S.napps; i++) {
		const AppInfo *a = &S.apps[i];
		char l = a->name[0];
		if (l >= 'a' && l <= 'z')
			l = (char)(l - 32);
		if (!(l >= 'A' && l <= 'Z'))
			l = '#';
		if (l != letter) {
			letter = l;
			/* a letter heading doesn't sit at the bottom of a column */
			if (slot % rows == rows - 1)
				slot++;
			f32 x = LEFT + ox - scroll_x + (f32)(slot / rows) * cw, y = top + (f32)(slot % rows) * eh;
			char lt[2] = { l, 0 };
			ui_text(ui, ui_font(ui, 22, 300), x + 4, y + 32, lt, w);
			slot++;
		}
		f32 x = LEFT + ox - scroll_x + (f32)(slot / rows) * cw, y = top + (f32)(slot % rows) * eh;
		slot++;
		maxx = maxf(maxx, x + scroll_x - ox + cw);
		Rect r = rect(x, y, cw - 16, eh - 4);
		ui_push_id_str(ui, a->id);
		UiInteract it = ui_interact(ui, ui_id(ui, "app"), r);
		ui_pop_id(ui);
		bool picked = sel_menu && str_eq(apps_sel, a->id);
		if (it.hovered || picked)
			ui_fill(ui, r, color_scale_alpha(color_hex(picked ? "#ffffff38" : "#ffffff1c"), k));
		Rect sq = rect(r.x + 2, r.y + 2, 38, 38);
		ui_fill(ui, sq, color_scale_alpha(tile_color_of(a->id, a->icon), k));
		const char *glyph = glyph_of(a->id);
		if (glyph)
			ui_icon(ui, pad(sq, 8), glyph, w);
		else
			ui_app_icon(ui, pad(sq, 4), a->icon);
		ui_push_clip(ui, r);
		ui_text(ui, ui_font(ui, 14, 400), r.x + 50, r.y + 27, a->name, w);
		ui_pop_clip(ui);
		if (it.clicked && !sel_menu)
			open_app(a->id);
		if (it.right_clicked) {
			str_copy(apps_sel, a->id, sizeof apps_sel);
			sel_menu = true;
			bar_frame = ui->frame_no;
		}
	}
	content_w = maxx + LEFT;
}

/* ---- Keyboard and controller --------------------------------------------------------------------- */

void launcher_nav(int dx, int dy)
{
	if (view != VIEW_START || !ntiles)
		return;
	if (nav < 0 || nav >= ntiles) {
		nav = 0;
		return;
	}
	Rect cur = tiles[nav].r;
	f32 cx = cur.x + cur.w / 2, cy = cur.y + cur.h / 2;
	int best = -1;
	f32 bd = 1e9f;
	for (int i = 0; i < ntiles; i++) {
		if (i == nav)
			continue;
		Rect r = tiles[i].r;
		f32 x = r.x + r.w / 2, y = r.y + r.h / 2, ddx = x - cx, ddy = y - cy;
		if ((dx > 0 && ddx <= 4) || (dx < 0 && ddx >= -4) || (dy > 0 && ddy <= 4) || (dy < 0 && ddy >= -4))
			continue;
		f32 d = dx ? fabsf(ddx) + fabsf(ddy) * 2 : fabsf(ddy) + fabsf(ddx) * 2;
		if (d < bd)
			bd = d, best = i;
	}
	if (best >= 0)
		nav = best;
	/* keep it in view */
	Rect r = tiles[nav].r;
	if (r.x - scroll_target < LEFT)
		scroll_target = maxf(0, r.x - LEFT);
	else if (r.x + r.w - scroll_target > S.sw - LEFT)
		scroll_target = r.x + r.w - (S.sw - LEFT);
	shell_mark_dirty();
}

void launcher_activate(void)
{
	if (searching)
		return; /* the search pane's entry takes Enter itself */
	if (view == VIEW_START && nav >= 0 && nav < ntiles)
		open_app(tiles[nav].app);
}

bool launcher_key(Ui *ui, u32 code, u32 mods, const char *text)
{
	UNUSED(ui);
	if (code == KEY_ESC) {
		if (sel_menu || power_menu || user_menu)
			sel_menu = power_menu = user_menu = false;
		else if (searching) {
			searching = false;
			sb_clear(&query);
		} else if (view == VIEW_APPS)
			view = VIEW_START;
		else
			close_start();
		return true;
	}
	if (searching)
		return false; /* the entry has the keys */
	if (code == KEY_ENTER || code == KEY_KPENTER) {
		launcher_activate();
		return true;
	}
	if (code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP || code == KEY_DOWN) {
		launcher_nav(code == KEY_RIGHT ? 1 : code == KEY_LEFT ? -1 : 0, code == KEY_DOWN ? 1 : code == KEY_UP ? -1 : 0);
		return true;
	}
	/* typing anything searches, as on Windows 8.1 */
	if (text && (u8)text[0] >= 0x20 && !(mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
		if (!searching) {
			searching = true;
			opened_at = S.now;
			sb_clear(&query);
		}
		return false; /* the entry gets this key too, and types it */
	}
	return false;
}

/* ---- The screen ---------------------------------------------------------------------------------------- */

void launcher_draw(Ui *ui)
{
	S.launcher_k = ui_anim(ui, ui_id(ui, "launcher-k"), S.launcher_open ? 1.0f : 0.0f, S.set.reduce_motion ? 30 : 9);
	f32 k = S.launcher_k;
	if (S.launcher_open && !was_open) {
		opened_at = S.now;
		view = VIEW_START;
		searching = false;
		sb_clear(&query);
		scroll_x = scroll_target = 0;
		sel = nav = -1;
		sel_menu = false;
		if (!loaded || !str_eq(loaded_home, S.set.home))
			load_layout();
	}
	was_open = S.launcher_open;
	if (k <= 0.01f)
		return;
	if (!loaded)
		load_layout();
	Rect full = rect(0, 0, S.sw, S.sh), d = ui_to_device(ui, full);
	shell_surface(full);
	/* the desktop's picture behind it, softened, as Windows 8.1 can show */
	blit_scaled(ui->canvas, S.wallpaper_blur, rect(0, 0, (f32)S.wallpaper_blur->width, (f32)S.wallpaper_blur->height), d, k);
	ui_fill(ui, full, color_scale_alpha(color_mix(color_hex("#0b0b12"), ui->theme.accent, 0.18f), 0.55f * k));
	/* tiles fly in from the right */
	f32 intro = S.set.reduce_motion ? 0 : (1 - ease_out_cubic(clampf((f32)(S.now - opened_at) * 2.6f, 0, 1))) * 220;
	if (intro > 0.5f)
		ui->redraw = true;
	/* horizontal scrolling: the wheel goes sideways */
	if (!searching && ui->scroll_dy != 0) {
		scroll_target += ui->scroll_dy * 1.6f + ui->scroll_dx;
		ui->scroll_dy = 0;
	}
	scroll_target = clampf(scroll_target, 0, maxf(0, content_w - S.sw));
	scroll_x += (scroll_target - scroll_x) * minf(1, ui->dt * 14);
	if (fabsf(scroll_target - scroll_x) > 0.5f)
		ui->redraw = true;
	else
		scroll_x = scroll_target;
	bool took = false;

	if (view == VIEW_APPS) {
		took |= header(ui, "", k);
		all_apps(ui, k, intro * 0.3f);
		if (!sel_menu && arrow_button(ui, false, k)) {
			view = VIEW_START;
			scroll_x = scroll_target = 0;
			took = true;
		}
	} else {
		took |= header(ui, "Start", k);
		int rows = MAX(2, (int)((bottom() - tiles_top() - 90 + GAP) / UNIT));
		rows = MIN(rows, 12);
		place(rows);
		/* group names */
		for (int g = 0; g < ngroups; g++) {
			f32 gx = 1e9f;
			for (int i = 0; i < ntiles; i++)
				if (tiles[i].group == g)
					gx = minf(gx, tiles[i].r.x);
			if (gx < 1e8f)
				ui_text(ui, ui_font(ui, 17, 300), gx - scroll_x + intro, tiles_top() - 14, groups[g], color_scale_alpha(COLOR_WHITE, k));
		}
		/* the tiles */
		drop_before = -1;
		for (int i = 0; i < ntiles; i++) {
			Tile *t = &tiles[i];
			f32 stagger = intro * (1 + (f32)t->group * 0.35f);
			Rect r = rect(t->r.x - scroll_x + stagger, t->r.y, t->r.w, t->r.h);
			ui_push_id_str(ui, t->app);
			UiInteract it = ui_interact(ui, ui_id(ui, "tile"), r);
			ui_pop_id(ui);
			if (i == drag) {
				/* where it was: an empty space */
				ui_border(ui, r, 0, 1, color_scale_alpha(color_hex("#ffffff50"), k));
				continue;
			}
			/* pressed tiles tip in a little */
			Rect fr = r;
			if (it.held && drag < 0)
				fr = rect_inset(r, 3, 3);
			tile_face(ui, t, fr, k);
			if (it.hovered || i == nav)
				ui_border(ui, rect_inset(fr, -1, -1), 0, 2, color_scale_alpha(color_hex("#ffffffc0"), k));
			if (sel_menu && i == sel) {
				/* selected: a check in the corner */
				ui_border(ui, rect_inset(fr, -2, -2), 0, 3, color_scale_alpha(ui->theme.accent, k));
				Path p = { 0 };
				path_move(&p, fr.x + fr.w - 26, fr.y - 2);
				path_line(&p, fr.x + fr.w + 2, fr.y - 2);
				path_line(&p, fr.x + fr.w + 2, fr.y + 26);
				path_close(&p);
				ui_path_fill(ui, &p, color_scale_alpha(ui->theme.accent, k));
				path_free(&p);
				ui_icon(ui, rect(fr.x + fr.w - 13, fr.y + 1, 11, 11), "check", COLOR_WHITE);
			}
			/* dropping another tile here goes before this one */
			if (drag >= 0 && rect_contains(r, ui->mx, ui->my))
				drop_before = i, drop_group = t->group;
			if (it.pressed_now) {
				press_tile = i;
				press_x = ui->mx, press_y = ui->my;
				drag_dx = r.x - ui->mx, drag_dy = r.y - ui->my;
			}
			if (it.clicked && drag < 0 && !sel_menu)
				open_app(t->app);
			else if (it.clicked && sel_menu)
				sel = i;
			if (it.right_clicked) {
				sel = i;
				sel_menu = true;
				bar_frame = ui->frame_no;
			}
		}
		/* dragging a tile somewhere else */
		if (press_tile >= 0 && ui->down[BUTTON_LEFT] && drag < 0) {
			f32 dx = ui->mx - press_x, dy = ui->my - press_y;
			if (dx * dx + dy * dy > 100)
				drag = press_tile;
		}
		if (drag >= 0 && drag < ntiles) {
			Tile *t = &tiles[drag];
			/* over empty space in a group's column: the end of that group */
			if (drop_before < 0)
				for (int g = 0; g < ngroups && drop_before < 0; g++) {
					f32 gx0 = 1e9f, gx1 = -1e9f;
					for (int i = 0; i < ntiles; i++)
						if (tiles[i].group == g)
							gx0 = minf(gx0, tiles[i].r.x - scroll_x), gx1 = maxf(gx1, tiles[i].r.x + tiles[i].r.w - scroll_x);
					if (ui->mx >= gx0 - 20 && ui->mx <= gx1 + 40)
						drop_group = g, drop_before = ntiles;
				}
			Rect r = rect(ui->mx + drag_dx, ui->my + drag_dy, t->r.w, t->r.h);
			ui_shadow(ui, r, 0, 16, 6, color_scale_alpha(color_hex("#00000080"), k));
			tile_face(ui, t, r, k * 0.9f);
			ui->redraw = true;
			/* near the edges it scrolls */
			if (ui->mx > S.sw - 60)
				scroll_target += 18;
			else if (ui->mx < 40)
				scroll_target -= 18;
			if (!ui->down[BUTTON_LEFT]) {
				if (drop_before >= 0 && drop_before != drag) {
					Tile moved = *t;
					moved.group = drop_group;
					memmove(&tiles[drag], &tiles[drag + 1], sizeof(Tile) * (usize)(ntiles - drag - 1));
					ntiles--;
					int at = drop_before > drag ? drop_before - 1 : drop_before;
					at = CLAMP(at, 0, ntiles);
					memmove(&tiles[at + 1], &tiles[at], sizeof(Tile) * (usize)(ntiles - at));
					tiles[at] = moved;
					ntiles++;
					save_layout();
				} else if (drop_before < 0 && ui->mx > 0) {
					/* past every group: a new one */
					t->group = add_group("New group");
					save_layout();
				}
				drag = -1;
			}
		}
		if (!ui->down[BUTTON_LEFT])
			press_tile = -1;
		if (!sel_menu && arrow_button(ui, true, k)) {
			view = VIEW_APPS;
			scroll_x = scroll_target = 0;
			sel_menu = false;
			took = true;
		}
	}
	/* a scrollbar along the bottom when there's more to the side */
	if (content_w > S.sw + 1 && !sel_menu) {
		f32 track = S.sw - 2 * LEFT, thumb = maxf(60, track * S.sw / content_w);
		f32 tx = LEFT + (track - thumb) * (scroll_x / (content_w - S.sw));
		ui_fill(ui, rect(LEFT, bottom() - 14, track, 4), color_scale_alpha(color_hex("#ffffff20"), k));
		ui_fill(ui, rect(tx, bottom() - 14, thumb, 4), color_scale_alpha(color_hex("#ffffff90"), k));
	}
	if (sel_menu) {
		above_begin(ui, rect(0, bottom() - 92, S.sw, 92));
		took |= app_bar(ui, k);
		above_end(ui);
	}
	if (searching) {
		above_begin(ui, rect(S.sw - 400, 0, 400, bottom()));
		search_pane(ui, k);
		above_end(ui);
	}
	header_menus(ui, k);
	UNUSED(took);
}
