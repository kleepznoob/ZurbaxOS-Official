/*
 * Vault — the file manager.
 *
 * Browse folders as a grid (with picture thumbnails) or a detailed list,
 * open files in the right app, and copy, move, rename and delete them.
 * Deleted files go to the Trash first and can be restored from there, and
 * archives (zip, 7z, rar, tar and the rest) unpack with a double-click.
 */
#include "zui.h"

typedef struct {
	char *name;
	bool dir;
	i64 size, mtime;
	u32 mode;
	bool selected;
	Canvas *thumb;
	bool thumb_tried;
} Item;

typedef struct {
	char dir[1024];
	char history[32][1024];
	int hpos, hlen;
	Item *items;
	int nitems;
	bool grid;
	bool show_hidden;
	int sort;               /* 0 name, 1 size, 2 modified, 3 type */
	StrBuf search;
	int anchor;             /* for shift-selection */
	int cursor;
	/* clipboard: a paste waits for the files to arrive from the desktop */
	bool paste_pending;
	/* drag and drop: the folder (item, or place in the sidebar) under files being dragged */
	int drop_item;
	char drop_place[1024];
	/* dialogs */
	StrBuf rename_text, newfolder_text, password_text;
	bool password_wrong;    /* the last password given didn't open the archive */
	int rename_index;
	int props_index;
	char error[200];
	f64 error_until;
	u64 dir_mtime;
	f64 last_check;
	bool item_hit;          /* an item took this frame's click */
} Vault;

static Vault vt;

static char *trash_files(void)
{
	return files_trash_dir("files");
}

static char *trash_info(void)
{
	return files_trash_dir("info");
}

static bool in_trash(void)
{
	char *t = trash_files();
	bool r = str_eq(vt.dir, t);
	mem_free(t);
	return r;
}

static void show_error(Ui *ui, const char *msg)
{
	str_copy(vt.error, msg, sizeof vt.error);
	vt.error_until = ui->now + 4;
	ui->wake_at = vt.error_until;
}

/* ---- Listing ---------------------------------------------------------------------- */

static void free_items(void)
{
	for (int i = 0; i < vt.nitems; i++) {
		mem_free(vt.items[i].name);
		canvas_free(vt.items[i].thumb);
	}
	mem_free(vt.items);
	vt.items = NULL;
	vt.nitems = 0;
}

static int cmp_items(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	const Item *x = a, *y = b;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	switch (vt.sort) {
	case 1:
		if (x->size != y->size)
			return x->size > y->size ? -1 : 1;
		break;
	case 2:
		if (x->mtime != y->mtime)
			return x->mtime > y->mtime ? -1 : 1;
		break;
	case 3: {
		int c = str_cmp_nocase(path_ext(x->name), path_ext(y->name));
		if (c)
			return c;
		break;
	}
	}
	return str_cmp_natural(x->name, y->name);
}

static void reload(void)
{
	/* Keep the selection across reloads. */
	StrBuf sel = { 0 };
	for (int i = 0; i < vt.nitems; i++)
		if (vt.items[i].selected)
			sb_fmt(&sel, "/%s/", vt.items[i].name);
	free_items();
	DirList list = { 0 };
	if (dir_list(vt.dir, &list, true)) {
		vt.items = NEW_ARRAY(Item, list.len + 1);
		vec_each(&list, e) {
			if (!vt.show_hidden && e->name[0] == '.')
				continue;
			Item *it = &vt.items[vt.nitems++];
			it->name = str_dup(e->name);
			it->dir = e->type == DT_DIR;
			it->size = e->size;
			it->mtime = e->mtime;
			it->mode = e->mode;
			char key[300];
			fmt(key, sizeof key, "/%s/", e->name);
			it->selected = sel.len && str_find(sb_str(&sel), key);
		}
		dir_list_free(&list);
	}
	sb_free(&sel);
	sort(vt.items, (usize)vt.nitems, sizeof(Item), cmp_items, NULL);
	vt.dir_mtime = (u64)file_mtime(vt.dir);
	vt.cursor = MIN(vt.cursor, vt.nitems - 1);
}

static void go(const char *dir, bool record)
{
	char *norm = path_normalize(dir);
	if (!dir_exists(norm)) {
		mem_free(norm);
		return;
	}
	if (record && !str_eq(norm, vt.dir)) {
		if (vt.hpos < (int)ARRAY_LEN(vt.history) - 1) {
			vt.hpos++;
		} else {
			memmove(vt.history[0], vt.history[1], sizeof vt.history[0] * (ARRAY_LEN(vt.history) - 1));
		}
		str_copy(vt.history[vt.hpos], norm, sizeof vt.history[0]);
		vt.hlen = vt.hpos + 1;
	}
	str_copy(vt.dir, norm, sizeof vt.dir);
	mem_free(norm);
	for (int i = 0; i < vt.nitems; i++)
		vt.items[i].selected = false;
	vt.cursor = -1;
	vt.anchor = -1;
	sb_clear(&vt.search);
	reload();
}

static char *item_path(int i)
{
	return path_join(vt.dir, vt.items[i].name);
}

static int selection_count(void)
{
	int n = 0;
	for (int i = 0; i < vt.nitems; i++)
		n += vt.items[i].selected;
	return n;
}

static void select_only(int i)
{
	for (int k = 0; k < vt.nitems; k++)
		vt.items[k].selected = k == i;
	vt.cursor = vt.anchor = i;
}

/* ---- File operations ---------------------------------------------------------------- */

static void trash_selected(Ui *ui)
{
	char *tf = trash_files(), *ti = trash_info();
	dir_make(tf, 0700);
	dir_make(ti, 0700);
	bool permanent = in_trash();
	int failed = 0;
	for (int i = 0; i < vt.nitems; i++) {
		if (!vt.items[i].selected)
			continue;
		char *src = item_path(i);
		if (permanent) {
			char *info = str_fmt("%s/%s.trashinfo", ti, vt.items[i].name);
			failed += !path_remove_all(src);
			sys_unlink(info);
			mem_free(info);
		} else {
			failed += !files_trash(src);
		}
		mem_free(src);
	}
	mem_free(tf);
	mem_free(ti);
	if (failed)
		show_error(ui, failed == 1 ? "One item couldn't be moved to the Trash." : "Some items couldn't be moved to the Trash.");
	reload();
}

static void restore_selected(Ui *ui)
{
	char *ti = trash_info();
	for (int i = 0; i < vt.nitems; i++) {
		if (!vt.items[i].selected)
			continue;
		char *info = str_fmt("%s/%s.trashinfo", ti, vt.items[i].name);
		char *text = file_read(info, NULL);
		const char *p = text ? str_find(text, "Path=") : NULL;
		if (p) {
			p += 5;
			const char *e = str_chr(p, '\n');
			char *orig = str_ndup(p, e ? (usize)(e - p) : strlen(p));
			char *src = item_path(i);
			char *odir = path_dir(orig);
			dir_make(odir, 0755);
			char *dst = files_unique_path(odir, path_base(orig));
			if (files_move(src, dst))
				sys_unlink(info);
			else
				show_error(ui, "The item couldn't be restored.");
			mem_free(orig);
			mem_free(src);
			mem_free(odir);
			mem_free(dst);
		}
		mem_free(text);
		mem_free(info);
	}
	mem_free(ti);
	reload();
}

static void empty_trash(void)
{
	char *tf = trash_files(), *ti = trash_info();
	path_remove_all(tf);
	path_remove_all(ti);
	dir_make(tf, 0700);
	dir_make(ti, 0700);
	mem_free(tf);
	mem_free(ti);
	reload();
}

/* The selection's paths, one per line. */
static char *selected_paths(int *count)
{
	StrBuf sb = { 0 };
	int n = 0;
	for (int i = 0; i < vt.nitems; i++)
		if (vt.items[i].selected) {
			char *p = item_path(i);
			sb_fmt(&sb, "%s\n", p);
			mem_free(p);
			n++;
		}
	if (count)
		*count = n;
	char *r = str_dup(sb_str(&sb));
	sb_free(&sb);
	return r;
}

/* Cut and copy go through the desktop's clipboard, so files can be pasted
 * on the desktop or in another Vault window too. */
static void clip_selected(Ui *ui, bool cut)
{
	char *paths = selected_paths(NULL);
	if (*paths)
		ui_clipboard_set_files(ui, paths, cut);
	mem_free(paths);
}

static void paste(Ui *ui)
{
	vt.paste_pending = true;
	ui_clipboard_files_request(ui);
}

/* Files into a folder: moved within a drive unless Ctrl asks for a copy,
 * copied from another drive. */
static void bring_files(Ui *ui, const char *list, const char *dir, int how)
{
	if (in_trash() && str_eq(dir, vt.dir))
		return;
	char first[1024] = "";
	const char *nl = str_chr(list, '\n');
	fmt(first, sizeof first, "%.*s", (int)(nl ? (usize)(nl - list) : strlen(list)), list);
	bool move = how == 1 || (how == 0 && files_same_drive(first, dir));
	int failed = files_paste(list, dir, move, NULL, 0);
	if (failed)
		show_error(ui, move ? "Some items couldn't be moved here." : "Some items couldn't be copied here.");
	reload();
}

/* The clipboard's files arrived: paste them here. */
static void paste_arrived(Ui *ui)
{
	if (!vt.paste_pending || !ui->clip_files)
		return;
	vt.paste_pending = false;
	const char *nl = str_chr(ui->clip_files, '\n');
	if (!nl || !nl[1])
		return;
	bool cut = str_starts(ui->clip_files, "cut");
	bring_files(ui, nl + 1, vt.dir, cut ? 1 : 2);
	/* cut files moved once; the clipboard doesn't keep pointing at their old place */
	if (cut)
		ui_clipboard_set(ui, "");
}

/* Drags the selection (with item i in it) out to wherever it's let go. */
static void drag_selection(Ui *ui, int i)
{
	if (!vt.items[i].selected)
		select_only(i);
	int n = 0;
	char *paths = selected_paths(&n);
	ui_drag_files(ui, paths, n, n == 1 ? vt.items[i].thumb : NULL);
	mem_free(paths);
}

/* Are files being dragged (or just dropped) at r? */
static bool drop_at(Ui *ui, Rect r)
{
	if (ui->drop_paths) {
		Rect abs = { r.x + ui->ox, r.y + ui->oy, r.w, r.h };
		return rect_contains(abs, ui->mx, ui->my) && rect_contains(ui->clip, ui->mx, ui->my);
	}
	return ui_drop_hover(ui, r);
}

/* Is the drag made of this folder's own selected items? */
static bool dragging_own(Ui *ui, int i)
{
	if (!vt.items[i].selected || !ui->drag_first[0])
		return false;
	char *parent = path_dir(ui->drag_first);
	bool own = str_eq(parent, vt.dir);
	mem_free(parent);
	return own;
}

static bool is_archive(const char *name, bool *disc);
static void unpack_item(Ui *ui, int i);

static void open_item(Ui *ui, int i)
{
	bool disc;
	if (!vt.items[i].dir && is_archive(vt.items[i].name, &disc) && !disc) {
		unpack_item(ui, i);
		return;
	}
	char *p = item_path(i);
	if (vt.items[i].dir)
		go(p, true);
	else
		ui_launch(ui, "", p);
	mem_free(p);
}

/* ---- Thumbnails ------------------------------------------------------------------------ */

static bool is_image(const char *name)
{
	const char *e = path_ext(name);
	return str_eq_nocase(e, "png") || str_eq_nocase(e, "jpg") || str_eq_nocase(e, "jpeg") || str_eq_nocase(e, "bmp");
}

/* Decodes a few thumbnails per frame so the window stays responsive. */
static Canvas *thumb(Ui *ui, int i, f64 *budget_end)
{
	Item *it = &vt.items[i];
	if (it->thumb || it->thumb_tried || it->dir || !is_image(it->name) || it->size > 64ll * 1024 * 1024)
		return it->thumb;
	if (time_mono_ns() / 1e9 > *budget_end) {
		ui->redraw = true;
		return NULL;
	}
	it->thumb_tried = true;
	char *p = item_path(i);
	it->thumb = image_load_thumb(p, 192);
	mem_free(p);
	return it->thumb;
}

/* ---- Drawing --------------------------------------------------------------------------- */

static void format_size(i64 s, char *out, usize cap)
{
	fmt_bytes(out, cap, (u64)s);
}

static const char *kind_of(const Item *it)
{
	if (it->dir)
		return "Folder";
	const char *icon = ui_file_icon(it->name, false);
	if (str_eq(icon, "image"))
		return "Picture";
	if (str_eq(icon, "music"))
		return "Audio";
	if (str_eq(icon, "video"))
		return "Video";
	if (str_eq(icon, "text-file"))
		return "Text";
	if (str_eq(icon, "drive"))
		return "Archive";
	if (str_eq(icon, "computer"))
		return "Windows program";
	return "File";
}

static void handle_click(Ui *ui, int i, UiInteract it)
{
	if (it.clicked || it.right_clicked)
		vt.item_hit = true;
	if (ui_drag_threshold(ui, it)) {
		drag_selection(ui, i);
		return;
	}
	if (it.clicked) {
		if (ui->mods & MOD_CTRL) {
			vt.items[i].selected = !vt.items[i].selected;
			vt.cursor = vt.anchor = i;
		} else if ((ui->mods & MOD_SHIFT) && vt.anchor >= 0) {
			int a = MIN(vt.anchor, i), b = MAX(vt.anchor, i);
			for (int k = 0; k < vt.nitems; k++)
				vt.items[k].selected = k >= a && k <= b;
			vt.cursor = i;
		} else {
			select_only(i);
		}
	}
	if (it.right_clicked) {
		if (!vt.items[i].selected)
			select_only(i);
		ui_popover_toggle(ui, "context");
	}
	if (it.double_clicked)
		open_item(ui, i);
}

static bool visible(int i)
{
	const char *q = sb_str(&vt.search);
	return !*q || str_find_nocase(vt.items[i].name, q);
}

static f32 draw_grid(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	f32 cw = 112, ch = 116;
	int cols = MAX(1, (int)((r.w - 16) / cw));
	f32 extra = (r.w - 16 - (f32)cols * cw) / (f32)cols;
	cw += extra;
	f32 y0 = ui_scroll_begin(ui, r, "grid");
	f64 budget = time_mono_ns() / 1e9 + 0.012;
	int n = 0;
	for (int i = 0; i < vt.nitems; i++) {
		if (!visible(i))
			continue;
		Item *it = &vt.items[i];
		Rect cell = rect(r.x + 8 + (f32)(n % cols) * cw, y0 + 8 + (f32)(n / cols) * ch, cw, ch);
		n++;
		ui_push_id(ui, i);
		UiInteract in = ui_interact(ui, ui_id(ui, "item"), rect_inset(cell, 4, 2));
		ui_pop_id(ui);
		bool target = it->dir && !dragging_own(ui, i) && drop_at(ui, rect_inset(cell, 4, 2));
		if (target) {
			vt.drop_item = i;
			ui_drop_highlight(ui, rect_inset(cell, 4, 2), 8);
		} else if (it->selected)
			ui_round(ui, rect_inset(cell, 4, 2), 8, color_with_alpha(t->accent, 0x30));
		else if (in.hovered)
			ui_round(ui, rect_inset(cell, 4, 2), 8, t->hover);
		Rect icon = rect(cell.x + (cell.w - 64) / 2, cell.y + 8, 64, 64);
		Canvas *th = thumb(ui, i, &budget);
		if (th) {
			f32 s = minf(64 / (f32)th->width, 64 / (f32)th->height);
			Rect ir = center_in(icon, (f32)th->width * s, (f32)th->height * s);
			ui_shadow(ui, ir, 3, 5, 2, color_hex("#00000040"));
			ui_image(ui, ir, th, 3, false);
			ui_border(ui, ir, 3, 1, color_hex("#ffffff50"));
		} else if (it->dir) {
			ui_app_icon(ui, icon, "folder");
		} else {
			/* A sheet of paper with the type's symbol. */
			Rect paper = center_in(icon, 48, 60);
			ui_shadow(ui, paper, 3, 4, 2, color_hex("#00000030"));
			ui_gradient(ui, paper, 3, color_hex("#ffffff"), color_hex("#eef0f3"));
			ui_border(ui, paper, 3, 1, color_hex("#00000022"));
			ui_icon(ui, center_in(paper, 24, 24), ui_file_icon(it->name, false), color_hex("#5d6573"));
			const char *ext = path_ext(it->name);
			if (*ext && strlen(ext) <= 4) {
				char up[8];
				str_copy(up, ext, sizeof up);
				for (char *c = up; *c; c++)
					*c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
				ui_text_in(ui, ui_font(ui, 8, 700), rect(paper.x, paper.y + paper.h - 16, paper.w, 12), up, color_hex("#8a919d"),
					   ALIGN_CENTER);
			}
		}
		Rect label = rect(cell.x + 6, cell.y + 76, cell.w - 12, 36);
		ui_push_clip(ui, label);
		f32 tw = ui_text_width(ui, ui_font(ui, 12, 500), it->name);
		if (tw <= label.w)
			ui_text_in(ui, ui_font(ui, 12, 500), rect(label.x, label.y, label.w, 18), it->name, t->text, ALIGN_CENTER);
		else
			ui_text_wrapped(ui, ui_font(ui, 12, 500), label, it->name, t->text, ALIGN_CENTER);
		ui_pop_clip(ui);
		handle_click(ui, i, in);
	}
	f32 h = (f32)((n + cols - 1) / cols) * ch + 16;
	ui_scroll_end(ui, h);
	return h;
}

static void draw_list(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect head = cut_top(&r, 28);
	ui_fill(ui, head, t->header_bottom);
	ui_separator(ui, head.x, head.y + head.h, head.x + head.w);
	static const char *const cols[] = { "Name", "Size", "Type", "Modified" };
	f32 cx[5] = { head.x + 44, head.x + head.w - 380, head.x + head.w - 280, head.x + head.w - 160, head.x + head.w };
	static const int sort_of[] = { 0, 1, 3, 2 };
	for (int i = 0; i < 4; i++) {
		Rect c = rect(cx[i] - 8, head.y, cx[i + 1] - cx[i], head.h);
		ui_push_id(ui, i);
		UiInteract in = ui_interact(ui, ui_id(ui, "col"), c);
		ui_pop_id(ui);
		if (in.hovered)
			ui_fill(ui, c, t->hover);
		ui_text(ui, ui_font(ui, 12, vt.sort == sort_of[i] ? 700 : 600), cx[i], head.y + 19, cols[i],
			vt.sort == sort_of[i] ? t->text : t->text_dim);
		if (in.clicked) {
			vt.sort = sort_of[i];
			reload();
		}
	}
	f32 y = ui_scroll_begin(ui, r, "list");
	for (int i = 0; i < vt.nitems; i++) {
		if (!visible(i))
			continue;
		Item *it = &vt.items[i];
		Rect row = rect(r.x, y, r.w, 34);
		y += 34;
		ui_push_id(ui, i);
		UiInteract in = ui_row(ui, row, ui_id(ui, "row"), it->selected);
		ui_pop_id(ui);
		if (it->dir && !dragging_own(ui, i) && drop_at(ui, row)) {
			vt.drop_item = i;
			ui_drop_highlight(ui, rect_inset(row, 2, 1), 6);
		}
		ui_icon(ui, rect(row.x + 16, row.y + 8, 18, 18), ui_file_icon(it->name, it->dir), it->dir ? t->accent : t->text_dim);
		ui_push_clip(ui, rect(cx[0], row.y, cx[1] - cx[0] - 12, row.h));
		ui_text(ui, ui_font(ui, 13, 400), cx[0], row.y + 22, it->name, t->text);
		ui_pop_clip(ui);
		char size[32] = "", when[32];
		if (it->dir) {
			str_copy(size, "—", sizeof size);
		} else {
			format_size(it->size, size, sizeof size);
		}
		Civil c;
		civil_local(it->mtime, &c);
		fmt(when, sizeof when, "%d %s %d, %02d:%02d", c.day, MONTH_NAMES[c.month - 1], c.year, c.hour, c.min);
		Font f = ui_font(ui, 12, 400);
		ui_text(ui, f, cx[1], row.y + 22, size, t->text_dim);
		ui_text(ui, f, cx[2], row.y + 22, kind_of(it), t->text_dim);
		ui_push_clip(ui, rect(cx[3], row.y, cx[4] - cx[3], row.h));
		ui_text(ui, f, cx[3], row.y + 22, when, t->text_dim);
		ui_pop_clip(ui);
		handle_click(ui, i, in);
	}
	ui_scroll_end(ui, y - r.y);
}

static void path_bar(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_round(ui, r, 6, t->input);
	ui_border(ui, r, 6, 1, t->border_strong);
	const char *home = home_dir();
	usize hl = strlen(home);
	bool in_home = str_starts(vt.dir, home) && (vt.dir[hl] == '/' || vt.dir[hl] == 0);
	char crumbs[16][128], paths[16][1024];
	int n = 0;
	str_copy(crumbs[n], in_home ? "Home" : "Computer", 128);
	str_copy(paths[n++], in_home ? home : "/", 1024);
	char acc[1024];
	str_copy(acc, in_home ? home : "", sizeof acc);
	for (const char *p = vt.dir + (in_home ? hl : 0); *p && n < 16;) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *e = str_chr(p, '/');
		usize len = e ? (usize)(e - p) : strlen(p);
		usize al = strlen(acc);
		fmt(acc + al, sizeof acc - al, "/%.*s", (int)len, p);
		fmt(crumbs[n], 128, "%.*s", (int)len, p);
		str_copy(paths[n++], acc, 1024);
		p += len;
	}
	if (in_trash()) {
		n = 1;
		str_copy(crumbs[0], "Trash", 128);
	}
	f32 x = r.x + 6;
	Font f = ui_font(ui, 13, 500);
	int first = 0;
	f32 total = 0;
	for (int i = n - 1; i >= 0; i--) {
		total += ui_text_width(ui, f, crumbs[i]) + 34;
		if (total > r.w - 20) {
			first = i + 1;
			break;
		}
	}
	for (int i = first; i < n; i++) {
		f32 w = ui_text_width(ui, f, crumbs[i]) + 16;
		Rect b = rect(x, r.y + 3, w, r.h - 6);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "crumb"), b);
		ui_pop_id(ui);
		if (it.hovered)
			ui_round(ui, b, 4, t->hover);
		if (i == 0)
			ui_icon(ui, rect(b.x + 4, b.y + (b.h - 14) / 2, 14, 14), in_trash() ? "trash" : in_home ? "go-home" : "drive", t->text_dim);
		ui_text_in(ui, ui_font(ui, 13, i == n - 1 ? 600 : 500), i == 0 ? rect(b.x + 14, b.y, b.w, b.h) : b, crumbs[i],
			   i == n - 1 ? t->text : t->text_dim, ALIGN_CENTER);
		if (i == 0)
			w += 14;
		if (it.clicked && i != n - 1)
			go(paths[i], true);
		x += w;
		if (i < n - 1) {
			ui_icon(ui, rect(x, r.y + r.h / 2 - 6, 12, 12), "pan-end", t->text_faint);
			x += 14;
		}
	}
}

static void sidebar(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_sidebar_background(ui, r);
	ui_vseparator(ui, r.x + r.w - 1, r.y, r.y + r.h);
	Rect s = pad(r, 8);
	ui_sidebar_heading(ui, cut_top(&s, 26), "Places");
	static const struct {
		const char *label, *icon, *rel;
	} places[] = {
		{ "Home", "go-home", "" },          { "Desktop", "computer", "Desktop" }, { "Documents", "file", "Documents" },
		{ "Downloads", "download", "Downloads" }, { "Music", "music", "Music" },        { "Pictures", "image", "Pictures" },
		{ "Videos", "video", "Videos" },
	};
	for (usize i = 0; i < ARRAY_LEN(places); i++) {
		char p[1024];
		if (places[i].rel[0])
			fmt(p, sizeof p, "%s/%s", home_dir(), places[i].rel);
		else
			str_copy(p, home_dir(), sizeof p);
		ui_push_id(ui, (i64)i);
		Rect pr = cut_top(&s, 32);
		if (drop_at(ui, pr) && !str_eq(vt.dir, p)) {
			str_copy(vt.drop_place, p, sizeof vt.drop_place);
			ui_drop_highlight(ui, pr, 6);
		}
		if (ui_sidebar_item(ui, pr, places[i].icon, places[i].label, str_eq(vt.dir, p)))
			go(p, true);
		ui_pop_id(ui);
	}
	char *tf = trash_files();
	DirList tl = { 0 };
	bool full = dir_list(tf, &tl, false) && tl.len > 0;
	dir_list_free(&tl);
	ui_push_id(ui, 50);
	Rect trash_r = cut_top(&s, 32);
	if (drop_at(ui, trash_r) && !in_trash()) {
		str_copy(vt.drop_place, tf, sizeof vt.drop_place);
		ui_drop_highlight(ui, trash_r, 6);
	}
	if (ui_sidebar_item(ui, trash_r, "trash", full ? "Trash (full)" : "Trash", in_trash())) {
		dir_make(tf, 0700);
		go(tf, true);
	}
	ui_pop_id(ui);
	mem_free(tf);
	cut_top(&s, 8);
	ui_sidebar_heading(ui, cut_top(&s, 26), "Devices");
	ui_push_id(ui, 60);
	if (ui_sidebar_item(ui, cut_top(&s, 32), "drive", "This Computer", str_eq(vt.dir, "/")))
		go("/", true);
	ui_pop_id(ui);
	/* Removable drives the system mounted under /media. */
	DirList media = { 0 };
	if (dir_list("/media", &media, false)) {
		int k = 0;
		vec_each(&media, e) {
			char p[512];
			fmt(p, sizeof p, "/media/%s", e->name);
			ui_push_id(ui, 70 + k++);
			if (ui_sidebar_item(ui, cut_top(&s, 32), "usb", e->name, str_eq(vt.dir, p)))
				go(p, true);
			ui_pop_id(ui);
		}
		dir_list_free(&media);
	}
	/* Free space on the data drive. */
	struct statfs sf;
	if (sys_statfs(home_dir(), &sf) == 0 && sf.f_blocks) {
		Rect bar = cut_bottom(&s, 44);
		f32 used = 1 - (f32)sf.f_bavail / (f32)sf.f_blocks;
		char fr[32], txt[64];
		fmt_bytes(fr, sizeof fr, sf.f_bavail * (u64)sf.f_bsize);
		fmt(txt, sizeof txt, "%s free", fr);
		ui_text(ui, ui_font(ui, 11, 500), bar.x + 4, bar.y + 14, txt, t->text_dim);
		ui_progress(ui, rect(bar.x + 4, bar.y + 22, bar.w - 8, 6), used);
	}
}

/* ---- Unpacking ------------------------------------------------------------------------ */

/* Archives open by unpacking, next to themselves, through the Linux layer's
 * libarchive and Zurbax's helper for it (layer/helpers/unarchive.c). Disc
 * images only through Extract: opening one means something else. */
static bool is_archive(const char *name, bool *disc)
{
	static const char *const exts[] = { "zip", "7z", "rar", "tar", "tgz", "txz", "tzst", "gz", "xz", "zst", "lzma",
					    "lz4", "cab", "cpio", "xar", "lha", "lzh", "iso" };
	const char *e = path_ext(name);
	*disc = str_eq_nocase(e, "iso");
	for (usize i = 0; i < ARRAY_LEN(exts); i++)
		if (str_eq_nocase(e, exts[i]))
			return true;
	return false;
}

static struct {
	char archive[1024], dir[1024], name[256], password[256];
	bool with_password, stopping;
	int pid;
	i64 done, total;        /* bytes of the archive read, from the helper */
	char result[400];       /* "done PATH", or what went wrong */
	int code;               /* the helper's exit status, -1 if it didn't finish */
	int busy, finished;     /* atomics */
	Ui *ui;
} up;

static void unpack_line(const char *line)
{
	if (str_starts(line, "progress ")) {
		const char *rest;
		up.done = str_to_int(line + 9, &rest);
		up.total = str_to_int(rest, NULL);
	} else if (*line) {
		str_copy(up.result, line, sizeof up.result);
	}
}

static void unpack_thread(void *arg)
{
	UNUSED(arg);
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[8];
	int n = 0;
	argv[n++] = layerrun;
	argv[n++] = "--isolated";
	argv[n++] = "/.zos/system/libexec/unarchive";
	if (up.with_password)
		argv[n++] = "-p";
	argv[n++] = up.archive;
	argv[n++] = up.dir;
	argv[n] = NULL;
	int out[2] = { -1, -1 }, in[2] = { -1, -1 }, status = -1;
	up.result[0] = 0;
	if (sys_pipe2(out, O_CLOEXEC) == 0 && (!up.with_password || sys_pipe2(in, O_CLOEXEC) == 0)) {
		/* a session of its own, so Stop reaches the helper inside the layer too */
		SpawnOpts o = { .stdin_fd = in[0], .stdout_fd = out[1], .stderr_fd = out[1], .new_session = true };
		int pid = proc_spawn(layerrun, argv, NULL, &o);
		up.pid = pid;
		sys_close(out[1]);
		if (in[0] >= 0) {
			sys_close(in[0]);
			write_all(in[1], up.password, strlen(up.password));
			write_all(in[1], "\n", 1);
			sys_close(in[1]);
		}
		memset(up.password, 0, sizeof up.password);
		char buf[1024], line[512];
		usize len = 0;
		for (;;) {
			long got = sys_read(out[0], buf, sizeof buf);
			if (got == -EINTR)
				continue;
			if (got <= 0)
				break;
			for (long i = 0; i < got; i++) {
				if (buf[i] == '\n') {
					line[len] = 0;
					unpack_line(line);
					len = 0;
				} else if (len + 1 < sizeof line) {
					line[len++] = buf[i];
				}
			}
			ui_wake(up.ui);
		}
		line[len] = 0;
		unpack_line(line);
		sys_close(out[0]);
		if (pid > 0)
			proc_wait(pid, &status, false);
	}
	mem_free(layerrun);
	up.code = (status & 0x7f) == 0 ? (status >> 8) & 0xff : -1;
	atomic_store(&up.finished, 1);
	ui_wake(up.ui);
}

static void start_unpack(Ui *ui, const char *archive, const char *password)
{
	if (atomic_load(&up.busy)) {
		show_error(ui, "Another archive is being unpacked; wait for it to finish.");
		return;
	}
	char *lib = str_fmt("%s/layers/steam/usr/lib/libarchive.so.13", zurbax_data());
	bool layer = file_exists(lib);
	mem_free(lib);
	if (!layer) {
		show_error(ui, "Unpacking needs the Linux layer, which comes with Zurbax once it's installed.");
		return;
	}
	if (archive != up.archive)
		str_copy(up.archive, archive, sizeof up.archive);
	char *dir = path_dir(up.archive);
	str_copy(up.dir, dir, sizeof up.dir);
	mem_free(dir);
	str_copy(up.name, path_base(up.archive), sizeof up.name);
	up.with_password = password != NULL;
	str_copy(up.password, password ? password : "", sizeof up.password);
	struct stat st;
	up.total = sys_stat(up.archive, &st) == 0 ? (i64)st.st_size : 0;
	up.done = 0;
	up.pid = 0;
	up.stopping = false;
	up.ui = ui;
	atomic_store(&up.busy, 1);
	if (thread_spawn(unpack_thread, NULL) < 0) {
		atomic_store(&up.busy, 0);
		show_error(ui, "The archive couldn't be unpacked.");
	}
}

static void unpack_item(Ui *ui, int i)
{
	char *p = item_path(i);
	start_unpack(ui, p, NULL);
	mem_free(p);
}

/* What came of an unpacking: the new folder or file chosen, a password
 * asked for, or what went wrong. */
static void unpack_poll(Ui *ui)
{
	if (!atomic_load(&up.finished))
		return;
	atomic_store(&up.finished, 0);
	atomic_store(&up.busy, 0);
	reload();
	if (up.code == 0 && str_starts(up.result, "done ")) {
		const char *made = up.result + 5;
		char *parent = path_dir(made);
		if (str_eq(parent, vt.dir))
			for (int i = 0; i < vt.nitems; i++)
				vt.items[i].selected = str_eq(vt.items[i].name, path_base(made));
		mem_free(parent);
	} else if (up.code == 3) {
		vt.password_wrong = up.with_password;
		sb_clear(&vt.password_text);
		ui_dialog_open(ui, "unpack-password");
	} else if (!up.stopping) {
		show_error(ui, up.result[0] && up.code > 0 ? up.result : "The archive couldn't be unpacked.");
	}
}

static void context_menu(Ui *ui)
{
	Rect c;
	int n = selection_count();
	bool trash = in_trash();
	int first = -1;
	for (int i = 0; i < vt.nitems && first < 0; i++)
		if (vt.items[i].selected)
			first = i;
	bool disc, archive = n == 1 && !trash && !vt.items[first].dir && is_archive(vt.items[first].name, &disc);
	/* install images: onto a USB stick, with flash in Forge (it asks for the password and the stick) */
	bool image = n == 1 && !trash && !vt.items[first].dir &&
		     (str_eq_nocase(path_ext(vt.items[first].name), "iso") || str_eq_nocase(path_ext(vt.items[first].name), "img"));
	f32 h = n == 0 ? 120 : trash ? 120 : 260 + (archive ? 34 : 0) + (image ? 34 : 0);
	if (!ui_popover_begin(ui, "context", rect(ui->mx, ui->my, 1, 1), 230, h, &c))
		return;
	if (n == 0) {
		if (ui_menu_item(ui, cut_top(&c, 34), "folder", "New Folder", "Shift+Ctrl+N")) {
			ui_popover_close(ui);
			sb_clear(&vt.newfolder_text);
			sb_adds(&vt.newfolder_text, "New Folder");
			ui_dialog_open(ui, "newfolder");
		}
		if (ui_menu_item(ui, cut_top(&c, 34), "edit-paste", "Paste", "Ctrl+V")) {
			ui_popover_close(ui);
			paste(ui);
		}
		if (ui_menu_item(ui, cut_top(&c, 34), "eye", vt.show_hidden ? "Hide Hidden Files" : "Show Hidden Files", "Ctrl+H")) {
			vt.show_hidden = !vt.show_hidden;
			reload();
			ui_popover_close(ui);
		}
	} else if (trash) {
		if (ui_menu_item(ui, cut_top(&c, 34), "edit-undo", "Restore", NULL)) {
			ui_popover_close(ui);
			restore_selected(ui);
		}
		if (ui_menu_item(ui, cut_top(&c, 34), "trash", "Delete Forever", "Delete")) {
			ui_popover_close(ui);
			trash_selected(ui);
		}
	} else {
		if (ui_menu_item(ui, cut_top(&c, 34), "document-open", "Open", "Enter")) {
			ui_popover_close(ui);
			if (first >= 0)
				open_item(ui, first);
		}
		if (archive && ui_menu_item(ui, cut_top(&c, 34), "mail-archive", "Extract", NULL)) {
			ui_popover_close(ui);
			unpack_item(ui, first);
		}
		if (image && ui_menu_item(ui, cut_top(&c, 34), "usb", "Make Bootable USB…", NULL)) {
			ui_popover_close(ui);
			char *p = item_path(first);
			if (str_chr(p, '"')) {
				show_error(ui, "Rename the file without quotation marks first.");
			} else {
				char *cmd = str_fmt("run:sudo flash \"%s\"", p);
				ui_launch(ui, "forge", cmd);
				mem_free(cmd);
			}
			mem_free(p);
		}
		if (ui_menu_item(ui, cut_top(&c, 34), "edit-cut", "Cut", "Ctrl+X")) {
			clip_selected(ui, true);
			ui_popover_close(ui);
		}
		if (ui_menu_item(ui, cut_top(&c, 34), "edit-copy", "Copy", "Ctrl+C")) {
			clip_selected(ui, false);
			ui_popover_close(ui);
		}
		if (n == 1 && ui_menu_item(ui, cut_top(&c, 34), "edit", "Rename…", "F2")) {
			ui_popover_close(ui);
			vt.rename_index = first;
			sb_clear(&vt.rename_text);
			sb_adds(&vt.rename_text, vt.items[first].name);
			ui_dialog_open(ui, "rename");
		}
		if (ui_menu_item(ui, cut_top(&c, 34), "trash", "Move to Trash", "Delete")) {
			ui_popover_close(ui);
			trash_selected(ui);
		}
		if (n == 1 && ui_menu_item(ui, cut_top(&c, 34), "info", "Properties", "Alt+Enter")) {
			ui_popover_close(ui);
			vt.props_index = first;
			ui_dialog_open(ui, "props");
		}
	}
	ui_popover_end(ui);
}

static void dialogs(Ui *ui)
{
	Theme *t = &ui->theme;
	Rect c;
	if (ui_dialog_begin(ui, "rename", 420, 170, &c)) {
		ui_heading(ui, cut_top(&c, 28), "Rename");
		cut_top(&c, 8);
		int ev = ui_entry(ui, cut_top(&c, 34), "rename-entry", &vt.rename_text, "Name", ENTRY_AUTOFOCUS);
		Rect b = cut_bottom(&c, 34);
		bool go_ = ui_button_suggested(ui, cut_right(&b, 100), "Rename") || (ev & ENTRY_SUBMIT);
		cut_right(&b, 8);
		if (ui_button(ui, cut_right(&b, 90), "Cancel"))
			ui_dialog_close(ui);
		if (go_ && vt.rename_text.len && vt.rename_index >= 0 && vt.rename_index < vt.nitems) {
			if (str_chr(sb_str(&vt.rename_text), '/')) {
				show_error(ui, "Names can't contain “/”.");
			} else {
				char *from = item_path(vt.rename_index);
				char *to = path_join(vt.dir, sb_str(&vt.rename_text));
				struct stat st;
				if (!str_eq(from, to) && sys_lstat(to, &st) == 0)
					show_error(ui, "Something with that name already exists here.");
				else if (sys_rename(from, to) < 0)
					show_error(ui, "The item couldn't be renamed.");
				mem_free(from);
				mem_free(to);
				reload();
			}
			ui_dialog_close(ui);
		}
		ui_dialog_end(ui);
	}
	if (ui_dialog_begin(ui, "newfolder", 420, 170, &c)) {
		ui_heading(ui, cut_top(&c, 28), "New Folder");
		cut_top(&c, 8);
		int ev = ui_entry(ui, cut_top(&c, 34), "newfolder-entry", &vt.newfolder_text, "Folder name", ENTRY_AUTOFOCUS);
		Rect b = cut_bottom(&c, 34);
		bool go_ = ui_button_suggested(ui, cut_right(&b, 100), "Create") || (ev & ENTRY_SUBMIT);
		cut_right(&b, 8);
		if (ui_button(ui, cut_right(&b, 90), "Cancel"))
			ui_dialog_close(ui);
		if (go_ && vt.newfolder_text.len) {
			char *p = files_unique_path(vt.dir, sb_str(&vt.newfolder_text));
			if (sys_mkdir(p, 0755) < 0)
				show_error(ui, "The folder couldn't be created here.");
			mem_free(p);
			reload();
			ui_dialog_close(ui);
		}
		ui_dialog_end(ui);
	}
	if (ui_dialog_begin(ui, "unpack-password", 420, 200, &c)) {
		ui_heading(ui, cut_top(&c, 28), vt.password_wrong ? "That password didn't work" : "This archive has a password");
		ui_push_clip(ui, cut_top(&c, 22));
		ui_text(ui, ui_font(ui, 13, 400), c.x, c.y - 6, up.name, t->text_dim);
		ui_pop_clip(ui);
		cut_top(&c, 6);
		int ev = ui_entry(ui, cut_top(&c, 34), "password-entry", &vt.password_text, "Password", ENTRY_PASSWORD | ENTRY_AUTOFOCUS);
		Rect b = cut_bottom(&c, 34);
		bool go_ = ui_button_suggested(ui, cut_right(&b, 100), "Unpack") || (ev & ENTRY_SUBMIT);
		cut_right(&b, 8);
		if (ui_button(ui, cut_right(&b, 90), "Cancel")) {
			sb_clear(&vt.password_text);
			ui_dialog_close(ui);
		}
		if (go_ && vt.password_text.len) {
			ui_dialog_close(ui);
			start_unpack(ui, up.archive, sb_str(&vt.password_text));
			sb_clear(&vt.password_text);
		}
		ui_dialog_end(ui);
	}
	if (ui_dialog_begin(ui, "props", 440, 330, &c)) {
		if (vt.props_index < 0 || vt.props_index >= vt.nitems) {
			ui_dialog_close(ui);
			ui_dialog_end(ui);
			return;
		}
		Item *it = &vt.items[vt.props_index];
		Rect top = cut_top(&c, 70);
		ui_icon(ui, rect(top.x, top.y + 8, 48, 48), ui_file_icon(it->name, it->dir), it->dir ? t->accent : t->text_dim);
		ui_push_clip(ui, top);
		ui_text(ui, ui_font(ui, 16, 600), top.x + 64, top.y + 30, it->name, t->text);
		ui_text(ui, ui_font(ui, 12, 400), top.x + 64, top.y + 50, kind_of(it), t->text_dim);
		ui_pop_clip(ui);
		char size[64], when[64], perm[16], where[1024];
		if (it->dir) {
			/* Count what's inside. */
			char *p = item_path(vt.props_index);
			DirList l = { 0 };
			int n = dir_list(p, &l, false) ? (int)l.len : 0;
			dir_list_free(&l);
			mem_free(p);
			fmt(size, sizeof size, n == 1 ? "1 item" : "%d items", n);
		} else {
			char b[32];
			format_size(it->size, b, sizeof b);
			fmt(size, sizeof size, "%s (%lld bytes)", b, (long long)it->size);
		}
		Civil cv;
		civil_local(it->mtime, &cv);
		fmt(when, sizeof when, "%d %s %d at %02d:%02d", cv.day, MONTH_NAMES[cv.month - 1], cv.year, cv.hour, cv.min);
		static const char rwx[] = "rwxrwxrwx";
		for (int i = 0; i < 9; i++)
			perm[i] = (it->mode & (0400u >> i)) ? rwx[i] : '-';
		perm[9] = 0;
		str_copy(where, vt.dir, sizeof where);
		const char *labels[] = { "Size", "Modified", "Location", "Permissions" };
		const char *values[] = { size, when, where, perm };
		for (int i = 0; i < 4; i++) {
			Rect row = cut_top(&c, 36);
			ui_text(ui, ui_font(ui, 13, 500), row.x, row.y + 22, labels[i], t->text_dim);
			ui_push_clip(ui, row);
			ui_text(ui, ui_font(ui, 13, 400), row.x + 110, row.y + 22, values[i], t->text);
			ui_pop_clip(ui);
		}
		Rect b = cut_bottom(&c, 34);
		if (ui_button(ui, cut_right(&b, 90), "Close"))
			ui_dialog_close(ui);
		ui_dialog_end(ui);
	}
}

static void keys(Ui *ui)
{
	if (ui->open_dialog)
		return;
	if (ui_key_pressed(ui, KEY_A, MOD_CTRL))
		for (int i = 0; i < vt.nitems; i++)
			vt.items[i].selected = visible(i);
	if (ui_key_pressed(ui, KEY_C, MOD_CTRL) && selection_count())
		clip_selected(ui, false);
	if (ui_key_pressed(ui, KEY_X, MOD_CTRL) && selection_count())
		clip_selected(ui, true);
	if (ui_key_pressed(ui, KEY_V, MOD_CTRL))
		paste(ui);
	if (ui_key_pressed(ui, KEY_H, MOD_CTRL)) {
		vt.show_hidden = !vt.show_hidden;
		reload();
	}
	if (ui_key_pressed(ui, KEY_N, MOD_CTRL | MOD_SHIFT)) {
		sb_clear(&vt.newfolder_text);
		sb_adds(&vt.newfolder_text, "New Folder");
		ui_dialog_open(ui, "newfolder");
	}
	if (ui_key_pressed(ui, KEY_DELETE, 0) && selection_count())
		trash_selected(ui);
	if (ui_key_pressed(ui, KEY_F2, 0) && selection_count() == 1)
		for (int i = 0; i < vt.nitems; i++)
			if (vt.items[i].selected) {
				vt.rename_index = i;
				sb_clear(&vt.rename_text);
				sb_adds(&vt.rename_text, vt.items[i].name);
				ui_dialog_open(ui, "rename");
			}
	if (ui_key_pressed(ui, KEY_BACKSPACE, 0) && !ui_is_focused(ui, "search")) {
		char *up = path_dir(vt.dir);
		go(up, true);
		mem_free(up);
	}
	if (ui_key_pressed(ui, KEY_LEFT, MOD_ALT) && vt.hpos > 0) {
		vt.hpos--;
		go(vt.history[vt.hpos], false);
	}
	if (ui_key_pressed(ui, KEY_RIGHT, MOD_ALT) && vt.hpos + 1 < vt.hlen) {
		vt.hpos++;
		go(vt.history[vt.hpos], false);
	}
	if (ui_key_pressed(ui, KEY_ENTER, 0) && vt.cursor >= 0 && vt.cursor < vt.nitems)
		open_item(ui, vt.cursor);
	int step = vt.grid ? MAX(1, (int)((ui->width - 220 - 16) / 112)) : 1;
	int dir = ui_key_pressed(ui, KEY_DOWN, 0) ? step : ui_key_pressed(ui, KEY_UP, 0) ? -step :
		  ui_key_pressed(ui, KEY_RIGHT, 0) ? 1 : ui_key_pressed(ui, KEY_LEFT, 0) ? -1 : 0;
	if (dir && vt.nitems)
		select_only(CLAMP(vt.cursor < 0 ? 0 : vt.cursor + dir, 0, vt.nitems - 1));
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	/* Pick up changes made elsewhere. */
	if (ui->now - vt.last_check > 1.0) {
		vt.last_check = ui->now;
		if ((u64)file_mtime(vt.dir) != vt.dir_mtime)
			reload();
	}
	ui->wake_at = ui->now + (atomic_load(&up.busy) ? 0.25 : 1.05);
	paste_arrived(ui);
	unpack_poll(ui);
	vt.drop_item = -1;
	vt.drop_place[0] = 0;
	keys(ui);
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	Rect tools = ui_headerbar(ui, header, "", HEADER_NO_TITLE);
	if (ui_icon_button(ui, cut_left(&tools, 34), "go-back", "Back (Alt+Left)") && vt.hpos > 0) {
		vt.hpos--;
		go(vt.history[vt.hpos], false);
	}
	if (ui_icon_button(ui, cut_left(&tools, 34), "go-forward", "Forward (Alt+Right)") && vt.hpos + 1 < vt.hlen) {
		vt.hpos++;
		go(vt.history[vt.hpos], false);
	}
	cut_left(&tools, 8);
	Rect search = cut_right(&tools, 200);
	ui_entry(ui, center_in(search, 196, 30), "search", &vt.search, "Search this folder", ENTRY_SEARCH);
	cut_right(&tools, 6);
	static const char *const views[] = { "Grid", "List" };
	int view = vt.grid ? 0 : 1;
	if (ui_segmented(ui, center_in(cut_right(&tools, 110), 106, 30), views, 2, &view))
		vt.grid = view == 0;
	cut_right(&tools, 10);
	path_bar(ui, center_in(tools, tools.w, 32));

	Rect side = cut_left(&win, 210);
	sidebar(ui, side);
	Rect status = cut_bottom(&win, 28);
	ui_fill(ui, win, t->view);
	/* Trash banner. */
	if (in_trash() && vt.nitems) {
		Rect ban = cut_top(&win, 44);
		ui_fill(ui, ban, t->header_bottom);
		ui_separator(ui, ban.x, ban.y + ban.h, ban.x + ban.w);
		ui_text(ui, ui_font(ui, 13, 500), ban.x + 16, ban.y + 27, "Items in the Trash can be restored or deleted forever.", t->text_dim);
		if (ui_button_destructive(ui, center_in(cut_right(&ban, 140), 128, 30), "Empty Trash"))
			empty_trash();
	}
	if (vt.nitems == 0) {
		ui_placeholder(ui, win, in_trash() ? "trash" : "folder-open", in_trash() ? "Trash Is Empty" : "Folder Is Empty",
			       in_trash() ? "" : "Drag files here or paste with Ctrl+V");
		UiInteract bg = ui_interact(ui, ui_id(ui, "bg"), win);
		if (bg.right_clicked)
			ui_popover_toggle(ui, "context");
	} else {
		/* Clicks on empty space clear the selection. */
		UiInteract bg = ui_interact(ui, ui_id(ui, "bg"), win);
		vt.item_hit = false;
		if (vt.grid)
			draw_grid(ui, win);
		else
			draw_list(ui, win);
		if (bg.clicked && !vt.item_hit && !(ui->mods & (MOD_CTRL | MOD_SHIFT)))
			for (int i = 0; i < vt.nitems; i++)
				vt.items[i].selected = false;
		if (bg.right_clicked && !vt.item_hit) {
			for (int i = 0; i < vt.nitems; i++)
				vt.items[i].selected = false;
			ui_popover_toggle(ui, "context");
		}
	}
	/* Status bar. */
	ui_fill(ui, status, t->window);
	ui_separator(ui, status.x, status.y, status.x + status.w);
	char txt[160];
	int n = selection_count();
	if (ui->now < vt.error_until) {
		ui_text(ui, ui_font(ui, 12, 600), status.x + 14, status.y + 18, vt.error, t->danger);
	} else if (atomic_load(&up.busy)) {
		i64 done = up.done, total = up.total;
		int pct = total > 0 ? (int)(MIN(done, total) * 100 / total) : 0;
		fmt(txt, sizeof txt, up.stopping ? "Stopping…" : "Unpacking %s… %d%%", up.name, pct);
		Rect stop = cut_right(&status, 70);
		Rect bar = cut_right(&status, 170);
		ui_push_clip(ui, status);
		ui_text(ui, ui_font(ui, 12, 500), status.x + 14, status.y + 18, txt, t->text_dim);
		ui_pop_clip(ui);
		Rect track = rect(bar.x + 6, bar.y + 11, bar.w - 12, 6);
		ui_round(ui, track, 3, t->hover);
		ui_round(ui, rect(track.x, track.y, track.w * (f32)pct / 100.0f, track.h), 3, t->accent);
		if (!up.stopping && up.pid > 0 && ui_button_flat(ui, center_in(stop, 60, 22), "Stop")) {
			up.stopping = true;
			sys_kill(-up.pid, SIGTERM);
		}
	} else if (n) {
		i64 total = 0;
		for (int i = 0; i < vt.nitems; i++)
			if (vt.items[i].selected && !vt.items[i].dir)
				total += vt.items[i].size;
		char b[32];
		format_size(total, b, sizeof b);
		if (n == 1)
			fmt(txt, sizeof txt, "1 item selected (%s)", b);
		else
			fmt(txt, sizeof txt, "%d items selected (%s)", n, b);
		ui_text(ui, ui_font(ui, 12, 500), status.x + 14, status.y + 18, txt, t->text_dim);
	} else {
		fmt(txt, sizeof txt, vt.nitems == 1 ? "1 item" : "%d items", vt.nitems);
		ui_text(ui, ui_font(ui, 12, 500), status.x + 14, status.y + 18, txt, t->text_dim);
	}
	/* files from outside dragged over the folder: it lights up */
	if (ui->drag_over && vt.drop_item < 0 && !vt.drop_place[0] && ui_drop_hover(ui, win)) {
		char *parent = path_dir(ui->drag_first);
		if (!str_eq(parent, vt.dir) && !in_trash())
			ui_drop_highlight(ui, rect_inset(win, 3, 3), 8);
		mem_free(parent);
	}
	/* files let go here: into the folder under them, or this one */
	if (ui->drop_paths) {
		char *trash = trash_files();
		if (vt.drop_place[0] && str_eq(vt.drop_place, trash)) {
			usize n;
			char **lines = str_split(ui->drop_paths, '\n', &n);
			int failed = 0;
			for (usize i = 0; i < n; i++)
				if (*str_trim(lines[i]) == '/')
					failed += !files_trash(str_trim(lines[i]));
			str_split_free(lines, n);
			if (failed)
				show_error(ui, "Some items couldn't be moved to the Trash.");
			reload();
		} else {
			char target[1024];
			if (vt.drop_place[0])
				str_copy(target, vt.drop_place, sizeof target);
			else if (vt.drop_item >= 0 && vt.drop_item < vt.nitems) {
				char *p = item_path(vt.drop_item);
				str_copy(target, p, sizeof target);
				mem_free(p);
			} else
				str_copy(target, vt.dir, sizeof target);
			bring_files(ui, ui->drop_paths, target, (ui->drop_mods & MOD_CTRL) ? 2 : 0);
		}
		mem_free(trash);
	}
	context_menu(ui);
	dialogs(ui);
	char title[300];
	fmt(title, sizeof title, "%s — Vault", in_trash() ? "Trash" : str_eq(vt.dir, home_dir()) ? "Home" : path_base(vt.dir));
	ui_set_title(ui, title);
}

static const char *start_dir;

static void init(Ui *ui)
{
	vt.grid = true;
	vt.rename_index = vt.props_index = -1;
	vt.hpos = -1;
	/* The standard folders, created on first use. */
	static const char *const std[] = { "Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos" };
	for (usize i = 0; i < ARRAY_LEN(std); i++) {
		char *p = str_fmt("%s/%s", home_dir(), std[i]);
		dir_make(p, 0755);
		mem_free(p);
	}
	/* started on an archive (opened from the desktop or a download): its folder, unpacking it */
	bool disc;
	if (start_dir && !dir_exists(start_dir) && file_exists(start_dir) && is_archive(start_dir, &disc)) {
		char *dir = path_dir(start_dir);
		go(dir, true);
		mem_free(dir);
		start_unpack(ui, start_dir, NULL);
		return;
	}
	go(start_dir && dir_exists(start_dir) ? start_dir : home_dir(), true);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		start_dir = argv[1];
	UiAppDesc d = { .id = "vault", .title = "Vault", .width = 960, .height = 620, .min_width = 640, .min_height = 400,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
