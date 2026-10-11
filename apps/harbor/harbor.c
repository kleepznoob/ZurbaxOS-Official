/* Harbor: the app store.
 *
 * Apps come from sources: the one built into Zurbax (always there, works
 * offline) and any web sources listed in sources.conf, whose app lists must
 * be signed by a trusted publisher. Installing hands the package to
 * harborctl, which checks it again before putting it in /data/apps.
 *
 * Linux apps come from Flathub, installed with Flatpak into the Linux layer
 * (flathub.c). */
#include "zui.h"
#include "zn.h"
#include "flathub.h"
#include "../../system/harborctl/harbor_repo.h"

typedef struct {
	char name[64];
	char base[512];          /* folder or URL, ending in '/' */
	bool system;             /* the source built into /system */
	bool web;
	HarborIndex index;
	char index_path[512];    /* where its index is on disk (for harborctl) */
	char error[160];
} Source;

enum { OP_NONE, OP_INSTALL, OP_REMOVE };

typedef struct {
	HarborApp *app;
	Source *src;
	SvgDoc *icon;
	Canvas *shot;
	Color tint;              /* from the icon, for banners */
	char installed[24];      /* version, or "" */
	int op;                  /* running operation */
	char error[160];
} Entry;

static struct {
	Source sources[8];
	int nsources;
	Entry entries[256];
	int nentries;
	int loading;             /* atomic */
	int busy;                /* atomic: an install or removal is running */
	Entry *job;
	int job_op;
	char job_result[200];
	int job_done;            /* atomic */
	/* view */
	int tab;                 /* 0 home, 1 Flathub, 2 installed */
	bool update_all;         /* keep updating until none are left */
	Entry *detail;
	FlatApp *flat_detail;
	char category[32];
	StrBuf search;
	Ui *ui;
	Map icon_cache;          /* "id@size" -> Canvas* */
	char cache_dir[512];
} H;

/* ---- Paths ------------------------------------------------------------------------------------- */

static char *apps_dir(void)
{
	return path_join(zurbax_data(), "apps");
}

static void read_installed(Entry *e)
{
	e->installed[0] = 0;
	char *dir = apps_dir();
	char *marker = str_fmt("%s/%s/.harbor", dir, e->app->id);
	Conf *c = conf_load(marker);
	const char *v = conf_get(c, "version", NULL);
	if (v && file_exists(marker))
		str_copy(e->installed, v, sizeof e->installed);
	conf_free(c);
	mem_free(marker);
	mem_free(dir);
}

static bool has_update(const Entry *e)
{
	return e->installed[0] && harbor_version_cmp(e->app->version, e->installed) > 0;
}

/* ---- Loading sources --------------------------------------------------------------------------- */

static u8 *source_file(Source *s, const char *name, usize *len)
{
	char *path = str_fmt("%s%s", s->base, name);
	u8 *data = NULL;
	if (!s->web) {
		data = (u8 *)file_read(path, len);
	} else {
		HttpResponse *r = http_get(path);
		if (r->status == 200 && r->body) {
			data = r->body;
			*len = r->body_len;
			r->body = NULL;
		}
		http_response_free(r);
	}
	mem_free(path);
	return data;
}

static void add_source(const char *name, const char *base, bool system)
{
	if (H.nsources >= (int)ARRAY_LEN(H.sources))
		return;
	Source *s = &H.sources[H.nsources++];
	memset(s, 0, sizeof *s);
	str_copy(s->name, name, sizeof s->name);
	str_copy(s->base, base, sizeof s->base - 1);
	usize n = strlen(s->base);
	if (n && s->base[n - 1] != '/') {
		s->base[n] = '/';
		s->base[n + 1] = 0;
	}
	s->system = system;
	s->web = str_starts(base, "http://") || str_starts(base, "https://");
}

static void load_source(Source *s, int idx)
{
	usize len;
	char *text = (char *)source_file(s, "index", &len);
	if (!text) {
		str_copy(s->error, s->web ? "Couldn't reach this source." : "This source is missing.", sizeof s->error);
		return;
	}
	if (s->system) {
		fmt(s->index_path, sizeof s->index_path, "%sindex", s->base);
	} else {
		/* Web sources must be signed; harborctl checks again from the copy we keep. */
		usize slen;
		u8 *sig = source_file(s, "index.sig", &slen);
		char *keys = zurbax_path("share/harbor/keys");
		bool ok = sig && harbor_verify(text, len, sig, slen, keys);
		mem_free(keys);
		if (!ok) {
			str_copy(s->error, "This source isn't signed by a trusted publisher.", sizeof s->error);
			mem_free(sig);
			mem_free(text);
			return;
		}
		char *dir = str_fmt("%s/%d", H.cache_dir, idx);
		dir_make(dir, 0755);
		fmt(s->index_path, sizeof s->index_path, "%s/index", dir);
		file_write(s->index_path, text, len);
		char *sp = str_fmt("%s.sig", s->index_path);
		file_write(sp, sig, slen);
		mem_free(sp);
		mem_free(sig);
		mem_free(dir);
	}
	harbor_parse(text, &s->index);
	if (!s->index.name[0])
		str_copy(s->index.name, s->name, sizeof s->index.name);
	mem_free(text);
}

static Color icon_tint(SvgDoc *d)
{
	Canvas *c = svg_rasterize(d, 24, 24, COLOR_WHITE);
	u64 r = 0, g = 0, b = 0, n = 0;
	for (int y = 0; y < c->height; y++)
		for (int x = 0; x < c->width; x++) {
			u32 p = canvas_row(c, y)[x];
			u32 a = p >> 24;
			if (a < 200)
				continue;
			r += (p >> 16) & 0xff, g += (p >> 8) & 0xff, b += p & 0xff, n++;
		}
	canvas_free(c);
	if (!n)
		return 0xff3b82c4;
	Color avg = RGB((u8)(r / n), (u8)(g / n), (u8)(b / n));
	f32 h, s, l;
	color_to_hsl(avg, &h, &s, &l);
	return color_hsl(h, clampf(s, 0.35f, 0.75f), clampf(l, 0.32f, 0.48f), 255);
}

static void load_thread(void *arg)
{
	UNUSED(arg);
	for (int i = 0; i < H.nsources; i++)
		load_source(&H.sources[i], i);
	int n = 0;
	for (int i = 0; i < H.nsources && n < (int)ARRAY_LEN(H.entries); i++) {
		Source *s = &H.sources[i];
		for (int k = 0; k < s->index.napps && n < (int)ARRAY_LEN(H.entries); k++) {
			HarborApp *a = &s->index.apps[k];
			/* the same app from two sources: keep the newer */
			Entry *dup = NULL;
			for (int j = 0; j < n; j++)
				if (str_eq(H.entries[j].app->id, a->id))
					dup = &H.entries[j];
			if (dup && harbor_version_cmp(a->version, dup->app->version) <= 0)
				continue;
			Entry *e = dup ? dup : &H.entries[n++];
			memset(e, 0, sizeof *e);
			e->app = a;
			e->src = s;
			usize len;
			if (a->icon[0]) {
				char *svg = (char *)source_file(s, a->icon, &len);
				if (svg) {
					e->icon = svg_parse(svg, len);
					if (e->icon)
						e->tint = icon_tint(e->icon);
				}
				mem_free(svg);
			}
			if (a->screenshot[0]) {
				u8 *png = source_file(s, a->screenshot, &len);
				if (png)
					e->shot = image_decode(png, len);
				mem_free(png);
			}
			read_installed(e);
		}
	}
	H.nentries = n;
	atomic_store(&H.loading, 0);
	ui_wake(H.ui);
}

static void start_loading(void)
{
	if (atomic_load(&H.loading))
		return;
	atomic_store(&H.loading, 1);
	H.nsources = 0;
	H.nentries = 0;
	H.detail = NULL;
	char *sys = zurbax_path("share/harbor-repo");
	add_source(OS_NAME, sys, true);
	mem_free(sys);
	/* web sources: one per line in the system list and the user's own */
	char *lists[2] = { zurbax_path("share/harbor/sources.conf"), path_join(home_dir(), ".config/zurbax/harbor-sources.conf") };
	for (int l = 0; l < 2; l++) {
		char *text = file_read(lists[l], NULL);
		for (char *line = text; line && *line;) {
			char *nl = (char *)str_chr(line, '\n');
			if (nl)
				*nl = 0;
			char *t = str_trim(line);
			if (*t && *t != '#' && (str_starts(t, "https://") || str_starts(t, "http://")))
				add_source(t, t, false);
			line = nl ? nl + 1 : NULL;
		}
		mem_free(text);
		mem_free(lists[l]);
	}
	thread_spawn(load_thread, NULL);
}

/* ---- Installing --------------------------------------------------------------------------------- */

static void job_thread(void *arg)
{
	UNUSED(arg);
	Entry *e = H.job;
	char result[200] = "";
	char *ctl = zurbax_path("bin/harborctl");
	char *out = NULL;
	int status = -1;
	if (H.job_op == OP_REMOVE) {
		char *argv[] = { ctl, "remove", e->app->id, NULL };
		out = proc_run_capture(argv, &status);
	} else if (e->src->system) {
		char *argv[] = { ctl, "install", "system", "-", e->app->id, NULL };
		out = proc_run_capture(argv, &status);
	} else {
		usize len;
		u8 *pkg = source_file(e->src, e->app->file, &len);
		if (!pkg) {
			str_copy(result, "error The download failed.", sizeof result);
		} else {
			char *path = str_fmt("%s/%s", H.cache_dir, e->app->file);
			file_write(path, pkg, len);
			mem_free(pkg);
			char *argv[] = { ctl, "install", e->src->index_path, path, e->app->id, NULL };
			out = proc_run_capture(argv, &status);
			sys_unlink(path);
			mem_free(path);
		}
	}
	if (out) {
		char *t = str_trim(out);
		char *nl = (char *)str_chr(t, '\n');
		if (nl)
			*nl = 0;
		str_copy(result, *t ? t : "error harborctl didn't answer", sizeof result);
	} else if (!result[0]) {
		str_copy(result, "error harborctl couldn't be started", sizeof result);
	}
	mem_free(out);
	mem_free(ctl);
	str_copy(H.job_result, result, sizeof H.job_result);
	atomic_store(&H.job_done, 1);
	ui_wake(H.ui);
}

static void start_job(Entry *e, int op)
{
	if (atomic_load(&H.busy))
		return;
	atomic_store(&H.busy, 1);
	H.job = e;
	H.job_op = op;
	e->op = op;
	e->error[0] = 0;
	thread_spawn(job_thread, NULL);
}

static void finish_job(Ui *ui)
{
	if (!atomic_load(&H.job_done))
		return;
	atomic_store(&H.job_done, 0);
	Entry *e = H.job;
	int op = H.job_op;
	e->op = OP_NONE;
	atomic_store(&H.busy, 0);
	if (str_starts(H.job_result, "done")) {
		read_installed(e);
		if (op == OP_INSTALL) {
			char body[160];
			fmt(body, sizeof body, "%s is in your Applications.", e->app->name);
			ui_notify(ui, "App installed", body, e->app->id);
			sound_play("complete");
		}
	} else {
		const char *why = str_starts(H.job_result, "error ") ? H.job_result + 6 : H.job_result;
		fmt(e->error, sizeof e->error, "%c%s.", to_upper(why[0]), why[0] ? why + 1 : "Something went wrong");
		sound_play("error");
	}
}

/* ---- Drawing helpers ----------------------------------------------------------------------------- */

static void draw_icon(Ui *ui, Rect r, Entry *e)
{
	if (!e->icon) {
		ui_app_icon(ui, r, "harbor");
		return;
	}
	Rect d = ui_to_device(ui, r);
	int size = (int)roundf(minf(d.w, d.h));
	char key[96];
	fmt(key, sizeof key, "%s@%d", e->app->id, size);
	Canvas *c = map_get(&H.icon_cache, key);
	if (!c) {
		c = svg_rasterize(e->icon, size, size, COLOR_WHITE);
		map_set(&H.icon_cache, key, c);
	}
	blit(ui->canvas, c, (int)roundf(d.x + (d.w - (f32)size) / 2), (int)roundf(d.y + (d.h - (f32)size) / 2), 1);
}

static const char *action_label(const Entry *e)
{
	if (e->op == OP_INSTALL)
		return e->installed[0] ? "Updating…" : "Installing…";
	if (e->op == OP_REMOVE)
		return "Removing…";
	if (has_update(e))
		return "Update";
	return e->installed[0] ? "Open" : "Install";
}

/* The main button for an app: Install, Update or Open. */
static void action_button(Ui *ui, Rect r, Entry *e)
{
	const char *label = action_label(e);
	if (e->op) {
		ui_round(ui, r, 6, ui->theme.hover);
		ui_border(ui, r, 6, 1, ui->theme.border);
		ui_spinner(ui, rect(r.x + 10, r.y + (r.h - 16) / 2, 16, 16));
		ui_text_in(ui, ui_font(ui, 13, 600), rect(r.x + 30, r.y, r.w - 36, r.h), label, ui->theme.text_dim, ALIGN_CENTER);
		return;
	}
	bool open = e->installed[0] && !has_update(e);
	bool clicked = open ? ui_button(ui, r, label) : ui_button_suggested(ui, r, label);
	if (!clicked)
		return;
	if (open)
		ui_launch(ui, e->app->id, NULL);
	else
		start_job(e, OP_INSTALL);
}

static bool matches(const Entry *e)
{
	if (H.category[0] && !str_eq(e->app->category, H.category))
		return false;
	const char *q = sb_str(&H.search);
	if (!*q)
		return true;
	return str_find_nocase(e->app->name, q) || str_find_nocase(e->app->summary, q) || str_find_nocase(e->app->category, q);
}

/* ---- Pages ------------------------------------------------------------------------------------------- */

static void app_card(Ui *ui, Rect r, Entry *e, int idx)
{
	Theme *t = &ui->theme;
	ui_push_id(ui, idx);
	UiId id = ui_id(ui, "card");
	UiInteract in = ui_interact(ui, id, r);
	f32 hv = ui_fade(ui, id, in.hovered, 0.15f);
	ui_shadow(ui, r, 12, 10 + 6 * hv, 2 + 2 * hv, color_with_alpha(t->shadow, (u8)(0x30 + 0x20 * hv)));
	ui_round(ui, r, 12, t->card);
	Rect c = pad(r, 16);
	Rect icon = cut_left(&c, 64);
	draw_icon(ui, rect(icon.x, icon.y, 64, 64), e);
	cut_left(&c, 14);
	ui_text_in(ui, ui_font(ui, 15, 700), rect(c.x, c.y, c.w, 22), e->app->name, t->text, ALIGN_START);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(c.x, c.y + 24, c.w, 34), e->app->summary, t->text_dim, ALIGN_START);
	Rect btn = rect(r.x + r.w - 16 - 104, r.y + r.h - 16 - 32, 104, 32);
	action_button(ui, btn, e);
	ui_text_in(ui, ui_font(ui, 11, 600), rect(c.x, btn.y, c.w - 110, 32), e->app->category, t->text_faint, ALIGN_START);
	if (in.clicked && !rect_contains(btn, ui->mx, ui->my))
		H.detail = e;
	ui_pop_id(ui);
}

static void banner(Ui *ui, Rect r, Entry *e)
{
	Color tint = e->tint ? e->tint : ui->theme.accent;
	ui_shadow(ui, r, 16, 20, 6, 0x40000000);
	ui_gradient(ui, r, 16, color_lighten(tint, 0.12f), color_lighten(tint, -0.18f));
	/* soft rings for depth */
	ui_push_clip(ui, r);
	ui_circle(ui, r.x + r.w * 0.82f, r.y + r.h * 0.2f, r.h * 0.9f, 0x14ffffff);
	ui_circle(ui, r.x + r.w * 0.95f, r.y + r.h * 1.1f, r.h * 0.7f, 0x10ffffff);
	if (e->shot && r.w > 640) {
		f32 sw = r.h * 1.15f, sh = sw * (f32)e->shot->height / (f32)e->shot->width;
		Rect sr = rect(r.x + r.w - sw - 30, r.y + 30, sw, sh);
		ui_shadow(ui, sr, 10, 18, 6, 0x50000000);
		ui_image(ui, sr, e->shot, 10, true);
	}
	ui_pop_clip(ui);
	Rect c = pad(r, 32);
	draw_icon(ui, rect(c.x, c.y + (c.h - 112) / 2, 112, 112), e);
	f32 x = c.x + 136;
	ui_text(ui, ui_font(ui, 12, 700), x, c.y + c.h / 2 - 46, "FEATURED", 0xccffffff);
	ui_text(ui, ui_font(ui, 30, 700), x, c.y + c.h / 2 - 8, e->app->name, COLOR_WHITE);
	f32 tw = minf(420, r.w - 136 - 64 - (e->shot && r.w > 640 ? r.h * 1.15f + 30 : 0));
	ui_text_wrapped(ui, ui_font(ui, 14, 500), rect(x, c.y + c.h / 2 + 4, tw, 40), e->app->summary, 0xeeffffff, ALIGN_START);
	Rect btn = rect(x, c.y + c.h / 2 + 50, 120, 34);
	action_button(ui, btn, e);
	UiInteract in = ui_interact(ui, ui_id(ui, "banner"), r);
	if (in.clicked && !rect_contains(btn, ui->mx, ui->my))
		H.detail = e;
}

static void home_page(Ui *ui, Rect area)
{
	Theme *t = &ui->theme;
	f32 y = ui_scroll_begin(ui, area, "home");
	Rect col = pad2(rect(area.x, y, area.w, 100000), maxf(24, (area.w - 1100) / 2), 24);
	f32 cy = col.y;
	/* feature the first app you don't have yet */
	Entry *featured = H.nentries ? &H.entries[0] : NULL;
	for (int i = 0; i < H.nentries; i++)
		if (!H.entries[i].installed[0]) {
			featured = &H.entries[i];
			break;
		}
	if (featured && !H.search.len && !H.category[0]) {
		banner(ui, rect(col.x, cy, col.w, 260), featured);
		cy += 260 + 28;
	}
	/* categories */
	const char *cats[16];
	int ncat = 0;
	for (int i = 0; i < H.nentries; i++) {
		bool seen = false;
		for (int k = 0; k < ncat; k++)
			seen |= str_eq(cats[k], H.entries[i].app->category);
		if (!seen && ncat < 16 && H.entries[i].app->category[0])
			cats[ncat++] = H.entries[i].app->category;
	}
	if (ncat > 1) {
		f32 x = col.x;
		for (int k = -1; k < ncat; k++) {
			const char *label = k < 0 ? "All" : cats[k];
			bool on = k < 0 ? !H.category[0] : str_eq(H.category, cats[k]);
			f32 w = ui_text_width(ui, ui_font(ui, 13, 600), label) + 28;
			Rect chip = rect(x, cy, w, 32);
			ui_push_id(ui, 500 + k);
			UiInteract in = ui_interact(ui, ui_id(ui, "chip"), chip);
			ui_pop_id(ui);
			ui_round(ui, chip, 16, on ? t->accent : in.hovered ? t->hover : t->card);
			if (!on)
				ui_border(ui, chip, 16, 1, t->border);
			ui_text_in(ui, ui_font(ui, 13, 600), chip, label, on ? t->on_accent : t->text, ALIGN_CENTER);
			if (in.clicked)
				str_copy(H.category, k < 0 ? "" : cats[k], sizeof H.category);
			x += w + 8;
		}
		cy += 32 + 24;
	}
	ui_text(ui, ui_font(ui, 18, 700), col.x, cy + 18, H.search.len ? "Results" : H.category[0] ? H.category : "Apps", t->text);
	cy += 36;
	int cols = MAX(1, MIN(3, (int)(col.w / 330)));
	f32 gap = 16, cw = (col.w - gap * (f32)(cols - 1)) / (f32)cols;
	int shown = 0;
	for (int i = 0; i < H.nentries; i++) {
		if (!matches(&H.entries[i]))
			continue;
		Rect r = rect(col.x + (cw + gap) * (f32)(shown % cols), cy + (136 + gap) * (f32)(shown / cols), cw, 136);
		app_card(ui, r, &H.entries[i], i);
		shown++;
	}
	if (!shown)
		ui_placeholder(ui, rect(col.x, cy, col.w, 240), "search", "No apps found", "Try another word or category.");
	cy += (136 + gap) * (f32)((shown + cols - 1) / cols) + 24;
	/* sources that didn't load */
	for (int i = 0; i < H.nsources; i++)
		if (H.sources[i].error[0]) {
			char line[300];
			fmt(line, sizeof line, "%s: %s", H.sources[i].name, H.sources[i].error);
			ui_icon(ui, rect(col.x, cy + 2, 16, 16), "warning", t->warning);
			ui_text(ui, ui_font(ui, 12, 500), col.x + 24, cy + 14, line, t->text_dim);
			cy += 24;
		}
	ui_scroll_end(ui, cy - y + 24);
}


/* ---- Flathub --------------------------------------------------------------------------------------- */

static void flat_action_button(Ui *ui, Rect r, FlatApp *a)
{
	if (a->op) {
		ui_round(ui, r, 6, ui->theme.hover);
		ui_border(ui, r, 6, 1, ui->theme.border);
		if (a->fraction >= 0) {
			ui_progress(ui, rect(r.x + 10, r.y + r.h - 9, r.w - 20, 4), a->fraction);
			char pct[16];
			fmt(pct, sizeof pct, "%d%%", (int)(a->fraction * 100 + 0.5f));
			ui_text_in(ui, ui_font(ui, 12, 600), rect(r.x, r.y - 3, r.w, r.h), pct, ui->theme.text_dim, ALIGN_CENTER);
		} else {
			const char *label = a->op == FLAT_REMOVE ? "Removing…" : a->op == FLAT_UPDATE ? "Updating…" : "Installing…";
			ui_spinner(ui, rect(r.x + 10, r.y + (r.h - 16) / 2, 16, 16));
			ui_text_in(ui, ui_font(ui, 12, 600), rect(r.x + 30, r.y, r.w - 36, r.h), label, ui->theme.text_dim, ALIGN_CENTER);
		}
		return;
	}
	if (a->installed[0] && !a->update) {
		if (ui_button(ui, r, "Open"))
			ui_launch(ui, a->id, NULL);
	} else if (ui_button_suggested(ui, r, a->update ? "Update" : "Install") && !flathub_busy()) {
		flathub_job(a, a->update ? FLAT_UPDATE : FLAT_INSTALL);
	}
}

/* Flathub checked who publishes it */
static void verified_badge(Ui *ui, f32 cx, f32 cy)
{
	ui_circle(ui, cx, cy, 7.5f, ui->theme.accent);
	ui_icon(ui, rect(cx - 5.5f, cy - 5.5f, 11, 11), "check", ui->theme.on_accent);
}

static void flat_card(Ui *ui, Rect r, FlatApp *a, int idx)
{
	Theme *t = &ui->theme;
	ui_push_id(ui, 2000 + idx);
	UiId id = ui_id(ui, "fcard");
	UiInteract in = ui_interact(ui, id, r);
	f32 hv = ui_fade(ui, id, in.hovered, 0.15f);
	ui_shadow(ui, r, 12, 10 + 6 * hv, 2 + 2 * hv, color_with_alpha(t->shadow, (u8)(0x30 + 0x20 * hv)));
	ui_round(ui, r, 12, t->card);
	Rect c = pad(r, 16);
	Rect icon = cut_left(&c, 64);
	flathub_icon(ui, rect(icon.x, icon.y, 64, 64), a);
	cut_left(&c, 14);
	Font nf = ui_font(ui, 15, 700);
	f32 nw = minf(ui_text_width(ui, nf, a->name), c.w - (a->verified ? 22 : 0));
	ui_text_in(ui, nf, rect(c.x, c.y, nw, 22), a->name, t->text, ALIGN_START);
	if (a->verified)
		verified_badge(ui, c.x + nw + 14, c.y + 11);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(c.x, c.y + 24, c.w, 34), a->summary, t->text_dim, ALIGN_START);
	Rect btn = rect(r.x + r.w - 16 - 104, r.y + r.h - 16 - 32, 104, 32);
	flat_action_button(ui, btn, a);
	ui_text_in(ui, ui_font(ui, 11, 600), rect(c.x, btn.y, c.w - 110, 32), a->developer[0] ? a->developer : a->category, t->text_faint,
		   ALIGN_START);
	if (in.clicked && !rect_contains(btn, ui->mx, ui->my))
		H.flat_detail = a;
	ui_pop_id(ui);
}

static void flathub_page(Ui *ui, Rect area)
{
	Theme *t = &ui->theme;
	if (!flathub_available()) {
		ui_placeholder(ui, area, "computer", "Linux apps aren't set up", "Flathub's apps need the Linux layer, which isn't installed.");
		return;
	}
	flathub_lock();
	int n;
	bool search = H.search.len > 0;
	FlatApp **list = search ? flathub_results(&n) : flathub_popular(&n);
	if (!n) {
		const char *err = flathub_error();
		if ((search ? flathub_searching() : flathub_loading()) || (!search && !err[0])) {
			ui_spinner(ui, center_in(area, 32, 32));
			ui_text_in(ui, ui_font(ui, 13, 500), rect(area.x, area.y + area.h / 2 + 24, area.w, 20),
				   search ? "Searching Flathub…" : "Getting Flathub's apps…", t->text_dim, ALIGN_CENTER);
		} else if (search) {
			ui_placeholder(ui, area, "search", "No apps found", "Try another word.");
		} else {
			ui_placeholder(ui, rect(area.x, area.y, area.w, area.h - 60), "network-offline", "Couldn't reach Flathub", err);
			if (ui_button_suggested(ui, rect(area.x + (area.w - 120) / 2, area.y + area.h / 2 + 70, 120, 34), "Try Again"))
				flathub_refresh();
		}
		flathub_unlock();
		return;
	}
	f32 y = ui_scroll_begin(ui, area, "flathub");
	Rect col = pad2(rect(area.x, y, area.w, 100000), maxf(24, (area.w - 1100) / 2), 24);
	f32 cy = col.y;
	if (!search) {
		/* a header for the section, in Flathub's spirit */
		Rect hr = rect(col.x, cy, col.w, 120);
		ui_shadow(ui, hr, 16, 18, 6, 0x30000000);
		ui_gradient(ui, hr, 16, 0xff4a86cf, 0xff2e5d9f);
		ui_push_clip(ui, hr);
		ui_circle(ui, hr.x + hr.w * 0.86f, hr.y + hr.h * 0.1f, hr.h * 1.1f, 0x16ffffff);
		ui_circle(ui, hr.x + hr.w * 0.98f, hr.y + hr.h * 1.2f, hr.h * 0.8f, 0x10ffffff);
		ui_pop_clip(ui);
		ui_text(ui, ui_font(ui, 26, 700), hr.x + 28, hr.y + 52, "Flathub", COLOR_WHITE);
		ui_text(ui, ui_font(ui, 14, 500), hr.x + 28, hr.y + 80, "Thousands of Linux apps, each in its own sandbox.", 0xe6ffffff);
		cy += 120 + 28;
	}
	int ng;
	FlatApp **games = search ? NULL : flathub_gaming(&ng);
	int cols = MAX(1, MIN(3, (int)(col.w / 330)));
	f32 gap = 16, cw = (col.w - gap * (f32)(cols - 1)) / (f32)cols;
	if (games && ng) {
		ui_text(ui, ui_font(ui, 18, 700), col.x, cy + 18, "Gaming", t->text);
		ui_text(ui, ui_font(ui, 12, 500), col.x + 90, cy + 18, "Every store, Windows games, emulators, streaming", t->text_dim);
		cy += 36;
		for (int i = 0; i < ng; i++)
			flat_card(ui, rect(col.x + (cw + gap) * (f32)(i % cols), cy + (136 + gap) * (f32)(i / cols), cw, 136), games[i], 100 + i);
		cy += (136 + gap) * (f32)((ng + cols - 1) / cols) + 12;
	}
	ui_text(ui, ui_font(ui, 18, 700), col.x, cy + 18, search ? "Results" : "Popular", t->text);
	if (search && flathub_searching())
		ui_spinner(ui, rect(col.x + 90, cy + 4, 18, 18));
	cy += 36;
	for (int i = 0; i < n; i++) {
		Rect r = rect(col.x + (cw + gap) * (f32)(i % cols), cy + (136 + gap) * (f32)(i / cols), cw, 136);
		flat_card(ui, r, list[i], i);
	}
	cy += (136 + gap) * (f32)((n + cols - 1) / cols) + 8;
	ui_text(ui, ui_font(ui, 11, 500), col.x, cy + 12, "Apps from Flathub are made by their developers and run in a sandbox.", t->text_faint);
	cy += 28;
	flathub_unlock();
	ui_scroll_end(ui, cy - y + 24);
}

static void flat_detail_page(Ui *ui, Rect area, FlatApp *a)
{
	Theme *t = &ui->theme;
	if (atomic_load(&a->details) == 0)
		flathub_details(a);
	flathub_lock();
	f32 y = ui_scroll_begin(ui, area, "fdetail");
	Rect col = pad2(rect(area.x, y, area.w, 100000), maxf(24, (area.w - 900) / 2), 28);
	f32 cy = col.y;
	flathub_icon(ui, rect(col.x, cy, 112, 112), a);
	f32 x = col.x + 136;
	ui_text(ui, ui_font(ui, 28, 700), x, cy + 36, a->name, t->text);
	const char *who = a->developer[0] ? a->developer : "Flathub";
	ui_text(ui, ui_font(ui, 14, 500), x, cy + 60, who, t->text_dim);
	if (a->verified) {
		f32 wx = x + ui_text_width(ui, ui_font(ui, 14, 500), who) + 16;
		verified_badge(ui, wx, cy + 55);
		ui_text(ui, ui_font(ui, 13, 600), wx + 12, cy + 60, "Verified", t->accent);
	}
	ui_text_in(ui, ui_font(ui, 14, 400), rect(x, cy + 66, col.w - 136, 22), a->summary, t->text, ALIGN_START);
	Rect btn = rect(x, cy + 96, 124, 34);
	flat_action_button(ui, btn, a);
	if (a->installed[0] && !a->op && ui_button_destructive(ui, rect(btn.x + 134, btn.y, 104, 34), "Remove") && !flathub_busy())
		flathub_job(a, FLAT_REMOVE);
	cy += 130 + 16;
	if (a->error[0]) {
		Rect er = rect(col.x, cy, col.w, 40);
		ui_round(ui, er, 8, color_with_alpha(t->danger, 0x22));
		ui_icon(ui, rect(er.x + 12, er.y + 12, 16, 16), "error", t->danger);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(er.x + 38, er.y, er.w - 50, er.h), a->error, t->text, ALIGN_START);
		cy += 40 + 12;
	}
	cy += 12;
	int d = atomic_load(&a->details);
	if (d == 1) {
		ui_spinner(ui, rect(col.x + (col.w - 28) / 2, cy + 40, 28, 28));
		cy += 120;
	} else if (d == 3) {
		ui_placeholder(ui, rect(col.x, cy, col.w, 160), "network-offline", "Couldn't get the details", "Flathub didn't answer.");
		cy += 170;
	}
	if (a->shot) {
		f32 sw = minf(col.w, (f32)a->shot->width), sh = sw * (f32)a->shot->height / (f32)a->shot->width;
		Rect sr = rect(col.x + (col.w - sw) / 2, cy, sw, sh);
		ui_shadow(ui, sr, 12, 24, 8, 0x40000000);
		ui_image(ui, sr, a->shot, 12, true);
		cy += sh + 32;
	}
	if (d == 2 && a->description && a->description[0])
		cy += ui_text_wrapped(ui, ui_font(ui, 14, 400), rect(col.x, cy, col.w, 4000), a->description, t->text, ALIGN_START) + 24;
	if (d == 2) {
		ui_separator(ui, col.x, cy, col.x + col.w);
		cy += 20;
		char dl[32] = "—", inst[32] = "—";
		if (a->download_size)
			fmt_bytes(dl, sizeof dl, a->download_size);
		if (a->installed_size)
			fmt_bytes(inst, sizeof inst, a->installed_size);
		const char *labels[4] = { "Version", "Download", "Installed Size", "License" };
		const char *values[4] = { a->version[0] ? a->version : "—", dl, inst, a->license[0] ? a->license : "—" };
		f32 w = col.w / 4;
		for (int i = 0; i < 4; i++) {
			ui_text(ui, ui_font(ui, 11, 700), col.x + w * (f32)i, cy + 12, labels[i], t->text_faint);
			ui_text_in(ui, ui_font(ui, 14, 600), rect(col.x + w * (f32)i, cy + 18, w - 12, 24), values[i], t->text, ALIGN_START);
		}
		cy += 60;
		if (a->runtime[0]) {
			char rt[160];
			fmt(rt, sizeof rt, "Runs on %s, which Flatpak installs once for every app that needs it.", a->runtime);
			ui_text(ui, ui_font(ui, 12, 400), col.x, cy + 10, rt, t->text_dim);
			cy += 28;
		}
	}
	flathub_unlock();
	ui_scroll_end(ui, cy - y + 24);
}

/* The installed page's Linux apps (under flathub_lock). */
static f32 flat_installed(Ui *ui, Rect col, f32 cy, FlatApp **list, int n)
{
	Theme *t = &ui->theme;
	ui_text(ui, ui_font(ui, 13, 700), col.x + 4, cy + 14, "Linux apps from Flathub", t->text_dim);
	int updates = flathub_updates();
	FlatApp *all = flathub_all();
	if (all->op) {
		ui_spinner(ui, rect(col.x + col.w - 20, cy, 18, 18));
		if (all->progress[0])
			ui_text_in(ui, ui_font(ui, 12, 500), rect(col.x, cy, col.w - 28, 20), all->progress, t->text_dim, ALIGN_END);
	} else if (updates && !flathub_busy()) {
		char label[40];
		fmt(label, sizeof label, updates == 1 ? "Update" : "Update All (%d)", updates);
		if (ui_button_suggested(ui, rect(col.x + col.w - 150, cy - 6, 150, 30), label))
			flathub_job(NULL, FLAT_UPDATE);
	}
	if (all->error[0])
		ui_text_in(ui, ui_font(ui, 12, 500), rect(col.x + 200, cy, col.w - 360, 20), all->error, t->danger, ALIGN_END);
	cy += 32;
	ui_card(ui, rect(col.x, cy, col.w, 72 * (f32)n));
	for (int i = 0; i < n; i++) {
		FlatApp *a = list[i];
		Rect r = rect(col.x, cy + 72 * (f32)i, col.w, 72);
		if (i)
			ui_separator(ui, r.x + 16, r.y, r.x + r.w - 16);
		Rect c = pad2(r, 16, 12);
		flathub_icon(ui, rect(c.x, c.y, 48, 48), a);
		ui_text_in(ui, ui_font(ui, 14, 700), rect(c.x + 62, c.y + 6, c.w - 300, 20), a->name[0] ? a->name : a->id, t->text, ALIGN_START);
		char ver[80];
		fmt(ver, sizeof ver, a->update ? "Version %s · update ready" : "Version %s", a->installed);
		ui_text(ui, ui_font(ui, 12, 400), c.x + 62, c.y + 40, ver, a->update ? t->accent : t->text_dim);
		ui_push_id(ui, 3000 + i);
		Rect btns = cut_right(&c, 220);
		if (ui_interact(ui, ui_id(ui, "frow"), rect(r.x, r.y, r.w - 240, r.h)).clicked)
			H.flat_detail = a;
		if (a->op) {
			ui_spinner(ui, rect(btns.x + btns.w - 24, btns.y + 14, 20, 20));
		} else {
			if (ui_button_destructive(ui, rect(btns.x, btns.y + 6, 100, 34), "Remove") && !flathub_busy())
				flathub_job(a, FLAT_REMOVE);
			flat_action_button(ui, rect(btns.x + 112, btns.y + 6, 104, 34), a);
		}
		ui_pop_id(ui);
	}
	return cy + 72 * (f32)n;
}

static void installed_page(Ui *ui, Rect area)
{
	Theme *t = &ui->theme;
	f32 y = ui_scroll_begin(ui, area, "installed");
	Rect col = pad2(rect(area.x, y, area.w, 100000), maxf(24, (area.w - 900) / 2), 24);
	f32 cy = col.y;
	int updates = 0, count = 0;
	for (int i = 0; i < H.nentries; i++) {
		updates += has_update(&H.entries[i]);
		count += H.entries[i].installed[0] != 0;
	}
	if (updates) {
		Rect bar = rect(col.x, cy, col.w, 56);
		ui_round(ui, bar, 12, color_with_alpha(t->accent, 0x22));
		char text[80];
		fmt(text, sizeof text, updates == 1 ? "1 update is ready" : "%d updates are ready", updates);
		ui_icon(ui, rect(bar.x + 18, bar.y + 20, 16, 16), "download", t->accent);
		ui_text(ui, ui_font(ui, 14, 600), bar.x + 44, bar.y + 34, text, t->text);
		if (!atomic_load(&H.busy) && ui_button_suggested(ui, rect(bar.x + bar.w - 140, bar.y + 11, 124, 34), "Update All"))
			H.update_all = true;   /* frame() runs them one at a time */
		cy += 56 + 20;
	}
	flathub_lock();
	int nflat;
	FlatApp **flat = flathub_installed(&nflat);
	if (!count && !nflat) {
		flathub_unlock();
		ui_placeholder(ui, rect(col.x, cy, col.w, 300), "bag", "Nothing installed from Harbor yet", "Apps you install show up here.");
		ui_scroll_end(ui, 340);
		return;
	}
	if (count) {
		ui_text(ui, ui_font(ui, 13, 700), col.x + 4, cy + 14, OS_NAME " apps", t->text_dim);
		cy += 28;
	}
	ui_card(ui, rect(col.x, cy, col.w, 72 * (f32)count));
	int row = 0;
	for (int i = 0; i < H.nentries; i++) {
		Entry *e = &H.entries[i];
		if (!e->installed[0])
			continue;
		Rect r = rect(col.x, cy + 72 * (f32)row, col.w, 72);
		if (row)
			ui_separator(ui, r.x + 16, r.y, r.x + r.w - 16);
		Rect c = pad2(r, 16, 12);
		draw_icon(ui, rect(c.x, c.y, 48, 48), e);
		ui_text(ui, ui_font(ui, 14, 700), c.x + 62, c.y + 20, e->app->name, t->text);
		char ver[64];
		fmt(ver, sizeof ver, has_update(e) ? "%s → %s" : "Version %s", e->installed, e->app->version);
		ui_text(ui, ui_font(ui, 12, 400), c.x + 62, c.y + 40, ver, t->text_dim);
		ui_push_id(ui, i);
		Rect btns = cut_right(&c, 220);
		if (e->op) {
			ui_spinner(ui, rect(btns.x + btns.w - 24, btns.y + 14, 20, 20));
		} else {
			if (ui_button_destructive(ui, rect(btns.x, btns.y + 6, 100, 34), "Remove"))
				start_job(e, OP_REMOVE);
			action_button(ui, rect(btns.x + 112, btns.y + 6, 104, 34), e);
		}
		ui_pop_id(ui);
		row++;
	}
	cy += 72 * (f32)count;
	if (nflat)
		cy = flat_installed(ui, col, cy + (count ? 24 : 0), flat, nflat);
	flathub_unlock();
	ui_scroll_end(ui, cy - y + 24);
}

static void detail_page(Ui *ui, Rect area, Entry *e)
{
	Theme *t = &ui->theme;
	f32 y = ui_scroll_begin(ui, area, "detail");
	Rect col = pad2(rect(area.x, y, area.w, 100000), maxf(24, (area.w - 900) / 2), 28);
	f32 cy = col.y;
	draw_icon(ui, rect(col.x, cy, 112, 112), e);
	f32 x = col.x + 136;
	ui_text(ui, ui_font(ui, 28, 700), x, cy + 36, e->app->name, t->text);
	ui_text(ui, ui_font(ui, 14, 500), x, cy + 60, e->app->developer[0] ? e->app->developer : e->src->name, t->text_dim);
	ui_text_in(ui, ui_font(ui, 14, 400), rect(x, cy + 66, col.w - 136, 22), e->app->summary, t->text, ALIGN_START);
	Rect btn = rect(x, cy + 96, 124, 34);
	action_button(ui, btn, e);
	if (e->installed[0] && !e->op && ui_button_destructive(ui, rect(btn.x + 134, btn.y, 104, 34), "Remove"))
		start_job(e, OP_REMOVE);
	cy += 130 + 16;
	if (e->error[0]) {
		Rect er = rect(col.x, cy, col.w, 40);
		ui_round(ui, er, 8, color_with_alpha(t->danger, 0x22));
		ui_icon(ui, rect(er.x + 12, er.y + 12, 16, 16), "error", t->danger);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(er.x + 38, er.y, er.w - 50, er.h), e->error, t->text, ALIGN_START);
		cy += 40 + 12;
	}
	cy += 12;
	if (e->shot) {
		f32 sw = minf(col.w, (f32)e->shot->width), sh = sw * (f32)e->shot->height / (f32)e->shot->width;
		Rect sr = rect(col.x + (col.w - sw) / 2, cy, sw, sh);
		ui_shadow(ui, sr, 12, 24, 8, 0x40000000);
		ui_image(ui, sr, e->shot, 12, true);
		cy += sh + 32;
	}
	if (e->app->description) {
		cy += ui_text_wrapped(ui, ui_font(ui, 14, 400), rect(col.x, cy, col.w, 2000), e->app->description, t->text, ALIGN_START) + 24;
	}
	/* details */
	ui_separator(ui, col.x, cy, col.x + col.w);
	cy += 20;
	char size[32];
	fmt_bytes(size, sizeof size, e->app->size);
	const char *labels[4] = { "Version", "Size", "Category", "Source" };
	const char *values[4] = { e->app->version, size, e->app->category, e->src->index.name[0] ? e->src->index.name : e->src->name };
	f32 w = col.w / 4;
	for (int i = 0; i < 4; i++) {
		ui_text(ui, ui_font(ui, 11, 700), col.x + w * (f32)i, cy + 12, labels[i], t->text_faint);
		ui_text(ui, ui_font(ui, 14, 600), col.x + w * (f32)i, cy + 34, values[i], t->text);
	}
	cy += 60;
	ui_scroll_end(ui, cy - y + 24);
}

/* ---- Window ------------------------------------------------------------------------------------------- */

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	H.ui = ui;
	finish_job(ui);
	flathub_poll(ui);
	/* "Update All": the next one, until none are left */
	if (H.update_all && !atomic_load(&H.busy)) {
		Entry *next = NULL;
		for (int i = 0; i < H.nentries && !next; i++)
			if (has_update(&H.entries[i]) && !H.entries[i].error[0])
				next = &H.entries[i];
		if (next)
			start_job(next, OP_INSTALL);
		else
			H.update_all = false;
	}
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 48);
	Rect hb = ui_headerbar(ui, header, NULL, HEADER_NO_TITLE);
	hb = pad2(hb, 4, 8);
	if (H.detail) {
		if (ui_icon_button(ui, cut_left(&hb, 32), "go-back", "Back") || ui_key_pressed(ui, KEY_ESC, 0) || ui_key_pressed(ui, KEY_LEFT, MOD_ALT))
			H.detail = NULL;
	} else if (H.flat_detail) {
		if (ui_icon_button(ui, cut_left(&hb, 32), "go-back", "Back") || ui_key_pressed(ui, KEY_ESC, 0) || ui_key_pressed(ui, KEY_LEFT, MOD_ALT))
			H.flat_detail = NULL;
	}
	Rect search = cut_right(&hb, minf(240, hb.w * 0.3f));
	int ev = ui_entry(ui, search, "search", &H.search, H.tab == 1 ? "Search Flathub" : "Search apps", ENTRY_SEARCH);
	if (ev & ENTRY_CHANGED) {
		H.detail = NULL;
		H.flat_detail = NULL;
		if (H.tab == 1)
			flathub_search(sb_str(&H.search));
		else
			H.tab = 0;
	}
	int updates = 0;
	for (int i = 0; i < H.nentries; i++)
		updates += has_update(&H.entries[i]);
	updates += flathub_updates();
	char installed_label[32];
	fmt(installed_label, sizeof installed_label, updates ? "Installed (%d)" : "Installed", updates);
	const char *tabs[] = { OS_NAME, "Flathub", installed_label };
	Rect seg = center_in(hb, 360, 32);
	int tab = H.tab;
	if (ui_segmented(ui, seg, tabs, 3, &tab)) {
		/* the search box searches the section it's in */
		if ((tab == 1) != (H.tab == 1) && H.search.len) {
			sb_clear(&H.search);
			flathub_search("");
		}
		H.tab = tab;
		H.detail = NULL;
		H.flat_detail = NULL;
	}
	ui_fill(ui, rect(win.x, win.y, win.w, 1), t->border);
	ui_fill(ui, win, t->view);
	if (flathub_busy() || flathub_loading() || flathub_searching())
		ui->wake_at = ui->now + 0.1;
	if (H.flat_detail) {
		flat_detail_page(ui, win, H.flat_detail);
		return;
	}
	if (H.tab == 1) {
		flathub_page(ui, win);
		return;
	}
	if (atomic_load(&H.loading)) {
		ui_spinner(ui, center_in(win, 32, 32));
		ui_text_in(ui, ui_font(ui, 13, 500), rect(win.x, win.y + win.h / 2 + 24, win.w, 20), "Getting the app list…", t->text_dim, ALIGN_CENTER);
		ui->wake_at = ui->now + 0.1;
		return;
	}
	if (!H.nentries) {
		ui_placeholder(ui, win, "bag", "No apps available", "Harbor couldn't find any app sources.");
		return;
	}
	if (H.detail)
		detail_page(ui, win, H.detail);
	else if (H.tab == 2)
		installed_page(ui, win);
	else
		home_page(ui, win);
	if (atomic_load(&H.busy))
		ui->wake_at = ui->now + 0.1;
	if (ui_key_pressed(ui, KEY_F, MOD_CTRL))
		ui_focus(ui, "search");
	if (ui_key_pressed(ui, KEY_R, MOD_CTRL) || ui_key_pressed(ui, KEY_F5, 0)) {
		start_loading();
		flathub_refresh();
	}
}

static void init(Ui *ui)
{
	H.ui = ui;
	char *cache = path_join(home_dir(), ".cache/zurbax/harbor");
	dir_make(cache, 0700);
	str_copy(H.cache_dir, cache, sizeof H.cache_dir);
	mem_free(cache);
	start_loading();
	flathub_init(ui, H.cache_dir);
}

int main(void)
{
	UiAppDesc d = { .id = "harbor", .title = "Harbor", .width = 1100, .height = 760, .min_width = 640, .min_height = 480,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
