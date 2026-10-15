/*
 * File chooser: a dialog inside the app's window for opening a file,
 * saving one, or picking a folder. Places on the left, a path bar on top,
 * and the folder's contents in a list. Double-click opens a folder or
 * picks a file; typing a name and pressing Enter saves.
 */
#include "zui.h"

typedef struct {
	bool open;
	u32 mode;
	char dir[512];
	char filters[128];     /* "png,jpg,jpeg" or "" for all */
	DirList list;
	bool loaded;
	int selected;
	StrBuf name;
	bool show_hidden;
	char result[1024];
	bool done;
	char error[128];
} FileDialog;

static FileDialog *dialog_state(Ui *ui, const char *id)
{
	ui_push_id_str(ui, id);
	FileDialog *fd = ui_state(ui, ui_id(ui, "file-dialog-state"), sizeof(FileDialog));
	ui_pop_id(ui);
	return fd;
}

const char *ui_file_icon(const char *name, bool is_dir)
{
	if (is_dir)
		return "folder";
	const char *ext = path_ext(name);
	static const struct {
		const char *exts, *icon;
	} map[] = {
		{ "png jpg jpeg gif bmp webp svg", "image" },
		{ "mp3 flac wav ogg opus m4a aac", "music" },
		{ "mp4 mkv webm avi mov", "video" },
		{ "txt md log conf ini csv json xml html css c h py sh js rs go", "text-file" },
		{ "zip tar gz xz zst 7z", "drive" },
		{ "exe msi", "computer" },
	};
	if (*ext) {
		char low[16];
		str_copy(low, ext, sizeof low);
		str_lower(low);
		for (usize i = 0; i < ARRAY_LEN(map); i++) {
			const char *p = str_find(map[i].exts, low);
			usize n = strlen(low);
			while (p) {
				bool start = p == map[i].exts || p[-1] == ' ';
				bool end = p[n] == 0 || p[n] == ' ';
				if (start && end)
					return map[i].icon;
				p = str_find(p + 1, low);
			}
		}
	}
	return "file";
}

static bool matches_filter(const FileDialog *fd, const char *name)
{
	if (!fd->filters[0])
		return true;
	const char *ext = path_ext(name);
	if (!*ext)
		return false;
	usize n;
	char **parts = str_split(fd->filters, ',', &n);
	bool ok = false;
	for (usize i = 0; i < n && !ok; i++)
		ok = str_eq_nocase(str_trim(parts[i]), ext);
	str_split_free(parts, n);
	return ok;
}

static void load_dir(FileDialog *fd)
{
	dir_list_free(&fd->list);
	fd->list = (DirList){ 0 };
	fd->selected = -1;
	fd->error[0] = 0;
	if (!dir_list(fd->dir, &fd->list, true))
		str_copy(fd->error, "This folder can't be opened.", sizeof fd->error);
	fd->loaded = true;
}

static void change_dir(FileDialog *fd, const char *dir)
{
	char *norm = path_normalize(dir);
	str_copy(fd->dir, norm, sizeof fd->dir);
	mem_free(norm);
	load_dir(fd);
}

void ui_file_dialog_open(Ui *ui, const char *id, u32 mode, const char *start_dir, const char *filters, const char *suggested_name)
{
	FileDialog *fd = dialog_state(ui, id);
	fd->open = true;
	fd->mode = mode;
	fd->done = false;
	str_copy(fd->filters, filters ? filters : "", sizeof fd->filters);
	const char *start = start_dir && *start_dir ? start_dir : home_dir();
	if (!dir_exists(start))
		start = home_dir();
	change_dir(fd, start);
	sb_clear(&fd->name);
	if (suggested_name)
		sb_adds(&fd->name, suggested_name);
	ui_push_id_str(ui, id);
	ui_dialog_open(ui, "file-dialog");
	if (mode == FILE_SAVE)
		ui_focus(ui, "fd-name");
	ui_pop_id(ui);
}

bool ui_file_dialog_is_open(Ui *ui, const char *id)
{
	FileDialog *fd = dialog_state(ui, id);
	return fd->open || fd->done;
}

static bool choose(FileDialog *fd, const char *path)
{
	str_copy(fd->result, path, sizeof fd->result);
	fd->done = true;
	fd->open = false;
	return true;
}

/* Breadcrumb buttons for each folder in the path, from the home folder when inside it. */
static void path_bar(Ui *ui, FileDialog *fd, Rect r)
{
	Theme *t = &ui->theme;
	const char *home = home_dir();
	usize hl = strlen(home);
	bool in_home = str_starts(fd->dir, home) && (fd->dir[hl] == '/' || fd->dir[hl] == 0);
	char parts[16][128];
	char paths[16][512];
	int n = 0;
	if (in_home) {
		str_copy(parts[n], "Home", sizeof parts[0]);
		str_copy(paths[n++], home, sizeof paths[0]);
	} else {
		str_copy(parts[n], "Computer", sizeof parts[0]);
		str_copy(paths[n++], "/", sizeof paths[0]);
	}
	const char *p = fd->dir + (in_home ? hl : 0);
	char acc[512];
	str_copy(acc, in_home ? home : "", sizeof acc);
	while (*p && n < 16) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *e = str_chr(p, '/');
		usize len = e ? (usize)(e - p) : strlen(p);
		usize al = strlen(acc);
		fmt(acc + al, sizeof acc - al, "/%.*s", (int)len, p);
		fmt(parts[n], sizeof parts[0], "%.*s", (int)len, p);
		str_copy(paths[n++], acc, sizeof paths[0]);
		p += len;
	}
	/* Show the last few crumbs that fit. */
	Font f = ui_font(ui, 13, 500);
	int first = 0;
	f32 total = 0;
	for (int i = n - 1; i >= 0; i--) {
		total += ui_text_width(ui, f, parts[i]) + 30;
		if (total > r.w) {
			first = i + 1;
			break;
		}
	}
	f32 x = r.x;
	for (int i = first; i < n; i++) {
		f32 w = ui_text_width(ui, f, parts[i]) + 20;
		Rect b = rect(x, r.y, w, r.h);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "crumb"), b);
		ui_pop_id(ui);
		if (it.hovered || i == n - 1)
			ui_round(ui, b, 6, i == n - 1 ? t->hover : t->press);
		ui_text_in(ui, ui_font(ui, 13, i == n - 1 ? 600 : 500), b, parts[i], i == n - 1 ? t->text : t->text_dim, ALIGN_CENTER);
		if (it.clicked && i != n - 1) {
			change_dir(fd, paths[i]);
			return;
		}
		x += w;
		if (i < n - 1) {
			ui_icon(ui, rect(x, r.y + r.h / 2 - 6, 12, 12), "pan-end", t->text_faint);
			x += 14;
		}
	}
}

static void format_size(i64 size, char *out, usize cap)
{
	if (size < 1024)
		fmt(out, cap, "%lld bytes", (long long)size);
	else if (size < 1024 * 1024)
		fmt(out, cap, "%.1f kB", (double)size / 1024);
	else if (size < 1024ll * 1024 * 1024)
		fmt(out, cap, "%.1f MB", (double)size / (1024 * 1024));
	else
		fmt(out, cap, "%.2f GB", (double)size / (1024.0 * 1024 * 1024));
}

bool ui_file_dialog(Ui *ui, const char *id, char *out_path, usize cap)
{
	FileDialog *fd = dialog_state(ui, id);
	if (fd->done) {
		fd->done = false;
		str_copy(out_path, fd->result, cap);
		return true;
	}
	if (!fd->open)
		return false;
	ui_push_id_str(ui, id);
	Rect c;
	f32 w = minf(760, ui->width - 40), h = minf(520, ui->height - 40);
	if (!ui_dialog_begin(ui, "file-dialog", w, h, &c)) {
		/* Closed with Escape. */
		fd->open = false;
		ui_pop_id(ui);
		return false;
	}
	Theme *t = &ui->theme;
	c = pad(c, -20); /* use the whole dialog */
	const char *title = fd->mode == FILE_SAVE ? "Save File" : fd->mode == FILE_FOLDER ? "Choose a Folder" : "Open File";
	Rect head = cut_top(&c, 50);
	ui_fill(ui, head, t->header_bottom);
	ui_separator(ui, head.x, head.y + head.h, head.x + head.w);
	ui_text(ui, ui_font(ui, 14, 700), head.x + 18, head.y + 30, title, t->text);
	Rect bar = pad4(head, 10, 12, 10, 150);
	if (ui_icon_toggle(ui, cut_right(&bar, 30), "eye", fd->show_hidden, "Show Hidden Files"))
		fd->show_hidden = !fd->show_hidden;
	path_bar(ui, fd, bar);

	Rect foot = cut_bottom(&c, 58);
	ui_separator(ui, foot.x, foot.y, foot.x + foot.w);
	/* Places. */
	Rect side = cut_left(&c, 170);
	ui_sidebar_background(ui, side);
	ui_vseparator(ui, side.x + side.w, side.y, side.y + side.h);
	Rect sp = pad(side, 8);
	static const struct {
		const char *label, *icon, *rel;
	} places[] = {
		{ "Home", "go-home", "" },        { "Desktop", "computer", "Desktop" }, { "Documents", "file", "Documents" },
		{ "Downloads", "download", "Downloads" }, { "Music", "music", "Music" },       { "Pictures", "image", "Pictures" },
		{ "Videos", "video", "Videos" },
	};
	for (usize i = 0; i < ARRAY_LEN(places); i++) {
		char path[512];
		if (places[i].rel[0])
			fmt(path, sizeof path, "%s/%s", home_dir(), places[i].rel);
		else
			str_copy(path, home_dir(), sizeof path);
		if (places[i].rel[0] && !dir_exists(path))
			continue;
		ui_push_id(ui, (i64)i);
		if (ui_sidebar_item(ui, cut_top(&sp, 32), places[i].icon, places[i].label, str_eq(fd->dir, path)))
			change_dir(fd, path);
		ui_pop_id(ui);
	}
	cut_top(&sp, 8);
	ui_push_id(ui, 99);
	if (ui_sidebar_item(ui, cut_top(&sp, 32), "drive", "Computer", str_eq(fd->dir, "/")))
		change_dir(fd, "/");
	ui_pop_id(ui);

	/* Contents. */
	ui_fill(ui, c, t->view);
	Rect cols = cut_top(&c, 28);
	ui_separator(ui, cols.x, cols.y + cols.h, cols.x + cols.w);
	Font hf = ui_font(ui, 11, 700);
	ui_text(ui, hf, cols.x + 44, cols.y + 18, "NAME", t->text_faint);
	ui_text(ui, hf, cols.x + cols.w - 230, cols.y + 18, "SIZE", t->text_faint);
	ui_text(ui, hf, cols.x + cols.w - 130, cols.y + 18, "MODIFIED", t->text_faint);
	if (!fd->loaded)
		load_dir(fd);
	bool picked = false;
	if (fd->error[0]) {
		ui_placeholder(ui, c, "warning", "Can't Open Folder", fd->error);
	} else {
		f32 y = ui_scroll_begin(ui, c, "fd-list");
		int shown = 0;
		for (usize i = 0; i < fd->list.len; i++) {
			DirEntry *e = &fd->list.data[i];
			bool is_dir = e->type == DT_DIR;
			if (!fd->show_hidden && e->name[0] == '.')
				continue;
			if (!is_dir && (fd->mode == FILE_FOLDER || !matches_filter(fd, e->name)))
				continue;
			Rect row = rect(c.x, y, c.w, 32);
			y += 32;
			shown++;
			ui_push_id(ui, (i64)i);
			UiInteract it = ui_row(ui, row, ui_id(ui, "f"), fd->selected == (int)i);
			ui_pop_id(ui);
			ui_icon(ui, rect(row.x + 16, row.y + 7, 18, 18), ui_file_icon(e->name, is_dir), is_dir ? t->accent : t->text_dim);
			ui_push_clip(ui, rect(row.x, row.y, row.w - 240, row.h));
			ui_text(ui, ui_font(ui, 13, 400), row.x + 44, row.y + 21, e->name, t->text);
			ui_pop_clip(ui);
			char size[32] = "—", when[32];
			if (!is_dir)
				format_size(e->size, size, sizeof size);
			Civil cv;
			civil_local(e->mtime, &cv);
			fmt(when, sizeof when, "%02d.%02d.%04d", cv.day, cv.month, cv.year);
			ui_text(ui, ui_font(ui, 12, 400), row.x + row.w - 230, row.y + 21, size, t->text_dim);
			ui_text(ui, ui_font(ui, 12, 400), row.x + row.w - 130, row.y + 21, when, t->text_dim);
			if (it.clicked) {
				fd->selected = (int)i;
				if (!is_dir && fd->mode == FILE_SAVE) {
					sb_clear(&fd->name);
					sb_adds(&fd->name, e->name);
				}
			}
			if (it.double_clicked) {
				char *path = path_join(fd->dir, e->name);
				if (is_dir) {
					change_dir(fd, path);
				} else if (fd->mode == FILE_OPEN) {
					picked = choose(fd, path);
				} else if (fd->mode == FILE_SAVE) {
					sb_clear(&fd->name);
					sb_adds(&fd->name, e->name);
				}
				mem_free(path);
				if (is_dir || picked)
					break;
			}
		}
		ui_scroll_end(ui, y - c.y);
		if (shown == 0 && !picked)
			ui_placeholder(ui, c, "folder-open", "Folder Is Empty", fd->filters[0] ? "No matching files here" : "");
	}

	/* Footer: name field when saving, and the buttons. */
	Rect fb = pad2(foot, 16, 12);
	Rect ok = cut_right(&fb, 100);
	cut_right(&fb, 8);
	Rect cancel = cut_right(&fb, 90);
	cut_right(&fb, 16);
	bool submit = false;
	if (fd->mode == FILE_SAVE) {
		ui_label(ui, cut_left(&fb, 50), "Name");
		int ev = ui_entry(ui, fb, "fd-name", &fd->name, "File name", 0);
		submit = (ev & ENTRY_SUBMIT) != 0;
	} else if (fd->selected >= 0 && fd->selected < (int)fd->list.len) {
		ui_label_dim(ui, fb, fd->list.data[fd->selected].name);
	}
	const char *ok_label = fd->mode == FILE_SAVE ? "Save" : fd->mode == FILE_FOLDER ? "Choose" : "Open";
	if (!picked && (ui_button_suggested(ui, ok, ok_label) || submit || (fd->mode == FILE_OPEN && ui_key_pressed(ui, KEY_ENTER, 0)))) {
		if (fd->mode == FILE_SAVE && fd->name.len) {
			char *path = path_join(fd->dir, sb_str(&fd->name));
			picked = choose(fd, path);
			mem_free(path);
		} else if (fd->mode == FILE_FOLDER) {
			if (fd->selected >= 0 && fd->list.data[fd->selected].type == DT_DIR) {
				char *path = path_join(fd->dir, fd->list.data[fd->selected].name);
				picked = choose(fd, path);
				mem_free(path);
			} else {
				picked = choose(fd, fd->dir);
			}
		} else if (fd->selected >= 0) {
			DirEntry *e = &fd->list.data[fd->selected];
			char *path = path_join(fd->dir, e->name);
			if (e->type == DT_DIR)
				change_dir(fd, path);
			else
				picked = choose(fd, path);
			mem_free(path);
		}
	}
	if (ui_button(ui, cancel, "Cancel")) {
		fd->open = false;
		ui_dialog_close(ui);
	}
	ui_dialog_end(ui);
	if (picked) {
		ui_dialog_close(ui);
		fd->done = false;
		str_copy(out_path, fd->result, cap);
		ui_pop_id(ui);
		return true;
	}
	ui_pop_id(ui);
	return false;
}
