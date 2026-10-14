/*
 * Steam in Bridge: the games Steam installed, read from its library folders,
 * on a shelf with their covers, and started through Steam itself. Steam
 * brings its own Proton for Windows games and sets up Easy Anti-Cheat and
 * BattlEye for the ones that use them, so what plays on a Steam Deck plays
 * here.
 */
#include "bridge.h"

typedef struct {
	u32 appid;
	char name[96];
	char dir[600]; /* where it's installed */
	u64 size;
	i64 played;
} Game;

static Game games[256];
static int ngames, gsel = -1;
static f64 scanned_at = -100;

/* covers Steam hasn't got: downloaded one at a time */
static struct {
	int busy; /* 1 downloading, 2 done */
	u32 appid;
} art;
static u32 no_art[256];
static int nno_art;

static char *steam_root(void)
{
	return str_fmt("%s/.local/share/Steam", home_dir());
}

static bool have_steam(void)
{
	return layer_has("/usr/bin/steam") || layer_has("/usr/games/steam");
}

/* Steam, with a steam:// address or none; big: Big Picture */
static void steam_open(const char *url, bool big)
{
	char *args[8];
	int n = 0;
	args[n++] = "--x11";
	args[n++] = "--app";
	args[n++] = "steam";
	args[n++] = "steam";
	if (big)
		args[n++] = "-gamepadui";
	if (url)
		args[n++] = (char *)url;
	args[n] = NULL;
	if (layer_start(args, true) < 0)
		str_copy(bridge_msg, "Steam couldn't start.", sizeof bridge_msg);
}

/* ---- Steam's files ---------------------------------------------------------------------- */

/* The quoted strings on a line of Valve's text format: "key" "value". */
static int vdf_line(const char *line, char out[2][600])
{
	int n = 0;
	for (const char *p = line; *p && n < 2; p++) {
		if (*p != '"')
			continue;
		usize k = 0;
		for (p++; *p && *p != '"'; p++) {
			if (*p == '\\' && p[1])
				p++;
			if (k + 1 < 600)
				out[n][k++] = *p;
		}
		out[n][k] = 0;
		n++;
		if (!*p)
			break;
	}
	return n;
}

/* Calls fn for each "key" "value" at the given depth of a VDF file. */
static void vdf_each(const char *path, int at_depth, void (*fn)(const char *key, const char *value, void *ctx), void *ctx)
{
	char *text = file_read(path, NULL);
	if (!text)
		return;
	int depth = 0;
	for (char *line = text; line && *line;) {
		char *nl = (char *)str_chr(line, '\n');
		if (nl)
			*nl = 0;
		char *t = str_trim(line);
		if (t[0] == '{')
			depth++;
		else if (t[0] == '}')
			depth--;
		else if (depth == at_depth) {
			char kv[2][600];
			if (vdf_line(t, kv) == 2)
				fn(kv[0], kv[1], ctx);
		}
		line = nl ? nl + 1 : NULL;
	}
	mem_free(text);
}

typedef struct {
	Game g;
	int flags;
	char installdir[256];
} Manifest;

static void manifest_field(const char *k, const char *v, void *ctx)
{
	Manifest *m = ctx;
	if (str_eq_nocase(k, "appid"))
		m->g.appid = (u32)str_to_int(v, NULL);
	else if (str_eq_nocase(k, "name"))
		str_copy(m->g.name, v, sizeof m->g.name);
	else if (str_eq_nocase(k, "StateFlags"))
		m->flags = (int)str_to_int(v, NULL);
	else if (str_eq_nocase(k, "installdir"))
		str_copy(m->installdir, v, sizeof m->installdir);
	else if (str_eq_nocase(k, "SizeOnDisk"))
		m->g.size = (u64)str_to_int(v, NULL);
	else if (str_eq_nocase(k, "LastPlayed"))
		m->g.played = str_to_int(v, NULL);
}

/* Steam's own parts, which it lists among the games */
static bool is_tool(const char *name)
{
	return str_starts(name, "Proton") || str_starts(name, "Steam Linux Runtime") || str_starts(name, "Steamworks Common");
}

static int by_played(const void *x, const void *y, void *ctx)
{
	const Game *a = x, *b = y;
	UNUSED(ctx);
	if (a->played != b->played)
		return a->played > b->played ? -1 : 1;
	return str_cmp_nocase(a->name, b->name);
}

typedef struct {
	char paths[16][600];
	int n;
} Libraries;

static void library_path(const char *k, const char *v, void *ctx)
{
	Libraries *l = ctx;
	if (!str_eq_nocase(k, "path") || l->n >= 16)
		return;
	for (int i = 0; i < l->n; i++)
		if (str_eq(l->paths[i], v))
			return;
	str_copy(l->paths[l->n++], v, sizeof l->paths[0]);
}

static void scan(void)
{
	u32 kept = gsel >= 0 && gsel < ngames ? games[gsel].appid : 0;
	ngames = 0;
	char *root = steam_root();
	Libraries libs = { 0 };
	library_path("path", root, &libs);
	char *folders = str_fmt("%s/steamapps/libraryfolders.vdf", root);
	vdf_each(folders, 2, library_path, &libs);
	mem_free(folders);
	for (int i = 0; i < libs.n; i++) {
		char *dir = str_fmt("%s/steamapps", libs.paths[i]);
		DirList l = { 0 };
		if (dir_list(dir, &l, false)) {
			vec_each(&l, e)
			{
				if (!str_starts(e->name, "appmanifest_") || !str_ends(e->name, ".acf") || ngames >= (int)ARRAY_LEN(games))
					continue;
				char *path = str_fmt("%s/%s", dir, e->name);
				Manifest m = { 0 };
				vdf_each(path, 1, manifest_field, &m);
				mem_free(path);
				/* installed (flag 4), and a game */
				if (!m.g.appid || !(m.flags & 4) || !m.g.name[0] || is_tool(m.g.name))
					continue;
				bool dup = false;
				for (int k = 0; k < ngames && !dup; k++)
					dup = games[k].appid == m.g.appid;
				if (dup)
					continue;
				fmt(m.g.dir, sizeof m.g.dir, "%s/common/%s", dir, m.installdir);
				games[ngames++] = m.g;
			}
			dir_list_free(&l);
		}
		mem_free(dir);
	}
	mem_free(root);
	sort(games, (usize)ngames, sizeof games[0], by_played, NULL);
	gsel = -1;
	for (int i = 0; i < ngames; i++)
		if (games[i].appid == kept)
			gsel = i;
}

/* ---- Covers ------------------------------------------------------------------------------ */

static void get_art(void *arg)
{
	UNUSED(arg);
	char key[32], src[24];
	fmt(key, sizeof key, "steam-%u", art.appid);
	char *path = cover_path(key, "");
	if (steam_art(art.appid, path, src, sizeof src) && str_starts(src, "wide:")) {
		char *wide = cover_path(key, "-wide");
		sys_rename(path, wide);
		mem_free(wide);
	}
	mem_free(path);
	atomic_store(&art.busy, 2);
}

/* Steam's own copy of the cover first, then one downloaded before. */
static Cover *game_cover(u32 appid)
{
	char key[32];
	fmt(key, sizeof key, "steam-%u", appid);
	Cover *c = cover_slot(key);
	if (!c->loaded) {
		c->loaded = true;
		char *root = steam_root();
		char *tries[] = {
			str_fmt("%s/appcache/librarycache/%u/library_600x900.jpg", root, appid),
			str_fmt("%s/appcache/librarycache/%u_library_600x900.jpg", root, appid),
			cover_path(key, ""),
		};
		for (usize i = 0; i < ARRAY_LEN(tries); i++) {
			if (!c->art && file_exists(tries[i]))
				c->art = image_load_thumb(tries[i], 900);
			mem_free(tries[i]);
		}
		if (!c->art) {
			char *w = cover_path(key, "-wide");
			c->art = file_exists(w) ? image_load_thumb(w, 900) : NULL;
			c->wide = c->art != NULL;
			mem_free(w);
		}
		mem_free(root);
	}
	return c;
}

static void art_tick(Ui *ui)
{
	int st = atomic_add(&art.busy, 0);
	if (st == 2) {
		char key[32];
		fmt(key, sizeof key, "steam-%u", art.appid);
		forget_cover(key);
		if (!game_cover(art.appid)->art && nno_art < (int)ARRAY_LEN(no_art))
			no_art[nno_art++] = art.appid;
		atomic_store(&art.busy, 0);
		st = 0;
	}
	if (st == 0) {
		for (int i = 0; i < ngames; i++) {
			if (game_cover(games[i].appid)->art)
				continue;
			bool tried = false;
			for (int k = 0; k < nno_art && !tried; k++)
				tried = no_art[k] == games[i].appid;
			if (tried)
				continue;
			art.appid = games[i].appid;
			atomic_store(&art.busy, 1);
			if (thread_spawn(get_art, NULL) < 0)
				atomic_store(&art.busy, 0);
			break;
		}
	}
	if (atomic_add(&art.busy, 0))
		wake_within(ui, 0.3);
}

static void draw_game_cover(Ui *ui, Rect r, const Game *g, f32 radius)
{
	Cover *c = game_cover(g->appid);
	draw_cover_img(ui, r, c->art, c->wide, NULL, g->name, 0, radius);
}

/* ---- The shelf ---------------------------------------------------------------------------- */

void steam_tools(Ui *ui, Rect *tools)
{
	if (have_steam() && ui_button(ui, center_in(cut_right(tools, 138), 130, 32), "Open Steam"))
		steam_open(NULL, false);
}

static void details(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Game *g = &games[gsel];
	ui_card(ui, r);
	Rect c = pad(r, 18);
	Rect top = cut_top(&c, 108);
	draw_game_cover(ui, rect(top.x, top.y, 72, 108), g, 8);
	ui_text_wrapped(ui, ui_font(ui, 16, 700), rect(top.x + 88, top.y + 4, top.w - 88, 44), g->name, t->text, ALIGN_START);
	char size[64] = "";
	if (g->size)
		fmt(size, sizeof size, "%.1f GB", (f64)g->size / 1e9);
	ui_text(ui, ui_font(ui, 12, 400), top.x + 88, top.y + 62, size, t->text_dim);
	ui_text(ui, ui_font(ui, 12, 400), top.x + 88, top.y + 80, "From Steam", t->text_faint);
	cut_top(&c, 14);
	Rect btns = cut_top(&c, 36);
	char url[64];
	if (ui_button_suggested(ui, cut_left(&btns, 120), "Play")) {
		fmt(url, sizeof url, "steam://rungameid/%u", g->appid);
		steam_open(url, false);
		fmt(bridge_msg, sizeof bridge_msg, "Starting %s through Steam…", g->name);
	}
	cut_left(&btns, 8);
	if (ui_button(ui, cut_left(&btns, 110), "Store Page")) {
		fmt(url, sizeof url, "steam://store/%u", g->appid);
		steam_open(url, false);
	}
	cut_left(&btns, 8);
	if (ui_button(ui, cut_left(&btns, 80), "Files"))
		ui_launch(ui, "vault", g->dir);
	cut_top(&c, 18);
	ui_heading(ui, cut_top(&c, 26), "Anti-cheat");
	ui_text_wrapped(ui, ui_font(ui, 12, 400), cut_top(&c, 90),
			"Steam sets up Easy Anti-Cheat and BattlEye by itself for the games that use them. They let players in "
			"when the game's maker allows Linux, as on the Steam Deck; a few makers don't (Bridge's Cloud shelf has those).",
			t->text_dim, ALIGN_START);
	Rect bottom = cut_bottom(&c, 34);
	if (ui_button_flat(ui, cut_right(&bottom, 110), "Uninstall…")) {
		fmt(url, sizeof url, "steam://uninstall/%u", g->appid);
		steam_open(url, false);
	}
}

void steam_tab(Ui *ui, Rect b)
{
	Theme *t = &ui->theme;
	if (!have_steam()) {
		ui_placeholder(ui, b, "steam", "Steam comes with the Linux layer", "Install Zurbax to get Steam, with Proton for Windows games.");
		return;
	}
	if (ui->now - scanned_at > 5) {
		scanned_at = ui->now;
		scan();
	}
	wake_within(ui, 5);
	art_tick(ui);
	if (!ngames) {
		ui_placeholder(ui, b, "steam", "No Steam games yet", "Sign in to Steam and install some games; they appear here, ready to play.");
		if (ui_button_suggested(ui, rect(b.x + b.w / 2 - 70, b.y + b.h / 2 + 70, 140, 34), "Open Steam"))
			steam_open(NULL, false);
		return;
	}
	if (gsel >= 0 && gsel < ngames) {
		Rect side = cut_right(&b, 340);
		cut_right(&b, 18);
		details(ui, side);
	}
	Rect bar = cut_top(&b, 34);
	cut_top(&b, 10);
	char count[64];
	fmt(count, sizeof count, ngames == 1 ? "1 game from Steam" : "%d games from Steam", ngames);
	ui_text(ui, ui_font(ui, 13, 600), bar.x, bar.y + 22, count, t->text_dim);
	if (ui_button_flat(ui, cut_right(&bar, 110), "Big Picture"))
		steam_open(NULL, true);
	if (ui_button_flat(ui, cut_right(&bar, 80), "Store"))
		steam_open("steam://store", false);
	f32 tw = 150, ch = 225, th = ch + 48, gap = 22;
	int per = MAX(1, (int)((b.w + gap) / (tw + gap)));
	f32 y = ui_scroll_begin(ui, b, "steam-lib");
	for (int i = 0; i < ngames; i++) {
		Rect cell = rect(b.x + (f32)(i % per) * (tw + gap), y + 8 + (f32)(i / per) * (th + gap), tw, th);
		ui_push_id(ui, (int)games[i].appid);
		UiInteract it = ui_interact(ui, ui_id(ui, "tile"), cell);
		f32 lift = ui_anim(ui, ui_id(ui, "lift"), it.hovered, 14) * 5;
		Rect a = rect(cell.x, cell.y - lift, tw, ch);
		ui_shadow(ui, a, 12, 14 + lift * 3, 4 + lift, color_hex("#00000058"));
		draw_game_cover(ui, a, &games[i], 12);
		if (gsel == i)
			ui_border(ui, rect_inset(a, -4, -4), 15, 2.5f, t->accent);
		ui_text_in(ui, ui_font(ui, 13, 600), rect(cell.x, cell.y + ch + 8, tw, 20), games[i].name, t->text, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 11, 400), rect(cell.x, cell.y + ch + 27, tw, 16), "Steam", t->text_faint, ALIGN_CENTER);
		if (it.clicked)
			gsel = i;
		if (it.double_clicked) {
			char url[64];
			fmt(url, sizeof url, "steam://rungameid/%u", games[i].appid);
			steam_open(url, false);
			fmt(bridge_msg, sizeof bridge_msg, "Starting %s through Steam…", games[i].name);
		}
		ui_pop_id(ui);
	}
	ui_scroll_end(ui, 8 + (f32)((ngames + per - 1) / per) * (th + gap));
	if (bridge_msg[0])
		ui_text(ui, ui_font(ui, 12, 400), b.x, b.y + b.h - 4, bridge_msg, t->text_dim);
}
