/*
 * The desktop: what's in ~/Desktop, as icons on the wallpaper.
 *
 * Icons sit in a grid from the top left, each where it was last put (kept in
 * ~/.config/zurbax/desktop-icons). They're drawn once into a copy of the
 * wallpaper, so the compositor pays nothing for them. Click to select (Ctrl
 * adds, dragging across empty space selects a box of them), double-click to
 * open, drag to move them around, into a folder, onto a window or onto an
 * app on the taskbar. Right-click for the menus: New Folder, New Document,
 * Paste, Cut, Copy, Rename and the rest.
 *
 * The display server runs as root; changes to files happen in zfiles, started
 * as the person, so what they make is theirs.
 */
#include "zdisplay.h"

#define CELL_W 96
#define CELL_H 106
#define ICON 54

typedef struct {
	char name[256];
	bool dir;
	bool selected;
	int col, row;
	i64 size;
	Canvas *thumb;              /* a picture's thumbnail */
	bool thumb_tried;
} Icon;

typedef struct {
	char name[256];
	int col, row;
	f64 set_at;                 /* when it was set for a file on its way; 0 if from the file */
} Spot;

static char dir[256];               /* ~/Desktop */
static char home_seen[128];
static Icon *icons;
static int nicons;
static Spot *spots;                 /* where each name goes */
static int nspots, spots_cap;
static int ino = -1;
static bool rescan_due;

static Canvas *layer;               /* the wallpaper with the icons on it */
static bool layer_dirty = true;
static Ui *aux;                     /* draws the layer and the drag picture */
static Rect last_bounds;            /* what the icons covered when last drawn */

/* pointer */
static int hover = -1;
static bool grabbing, banding, moved;
static int press_icon = -1;
static f32 press_x, press_y, band_x, band_y;
static f64 last_click_t;
static int last_click_icon = -1;
static bool focused;

/* files dragged over the desktop */
static int drop_col = -1, drop_row = -1, drop_folder = -1;

/* menus and renaming, drawn in the shell overlay */
enum { MENU_NONE, MENU_ICON, MENU_DESK };
static int menu;
static f32 menu_x, menu_y;
static u64 menu_opened;
static int renaming = -1;
static StrBuf rename_text;
static u64 rename_frames;
static char rename_soon[256];       /* a just-made item to rename when it shows up */

/* zfiles runs in the background */
static int jobs[32];
static int njobs;

/* ---- Grid ------------------------------------------------------------------------------- */

static Rect grid_area(void)
{
	Rect wa = work_area();
	return rect(wa.x + 10, wa.y + 10, wa.w - 20, wa.h - 16);
}

static int grid_rows(void)
{
	return MAX(1, (int)(grid_area().h / CELL_H));
}

static int grid_cols(void)
{
	return MAX(1, (int)(grid_area().w / CELL_W));
}

static Rect cell_rect(int col, int row)
{
	Rect g = grid_area();
	return rect(g.x + (f32)col * CELL_W, g.y + (f32)row * CELL_H, CELL_W, CELL_H);
}

static void cell_at(f32 x, f32 y, int *col, int *row)
{
	Rect g = grid_area();
	*col = CLAMP((int)floorf((x - g.x) / CELL_W), 0, grid_cols() - 1);
	*row = CLAMP((int)floorf((y - g.y) / CELL_H), 0, grid_rows() - 1);
}

static int icon_in_cell(int col, int row, int skip)
{
	for (int i = 0; i < nicons; i++)
		if (i != skip && icons[i].col == col && icons[i].row == row)
			return i;
	return -1;
}

/* The first free cell from (col, row) on, down each column then across. */
static bool free_cell(int *col, int *row, bool skip_selected)
{
	int rows = grid_rows(), cols = grid_cols();
	for (int k = *col * rows + *row; k < rows * cols * 4; k++) {
		int c = k / rows, r = k % rows;
		bool taken = false;
		for (int i = 0; i < nicons && !taken; i++)
			taken = icons[i].col == c && icons[i].row == r && !(skip_selected && icons[i].selected);
		if (!taken) {
			*col = c, *row = r;
			return true;
		}
	}
	return false;
}

static Rect icon_hit_rect(int i)
{
	return rect_inset(cell_rect(icons[i].col, icons[i].row), 5, 2);
}

static int icon_at(f32 x, f32 y)
{
	for (int i = nicons - 1; i >= 0; i--)
		if (rect_contains(icon_hit_rect(i), x, y))
			return i;
	return -1;
}

/* ---- Where icons go -------------------------------------------------------------------- */

static char *spots_path(void)
{
	return str_fmt("%s/.config/zurbax/desktop-icons", S.set.home);
}

static void spots_load(void)
{
	nspots = 0;
	char *p = spots_path();
	char *text = file_read(p, NULL);
	mem_free(p);
	if (!text)
		return;
	usize n;
	char **lines = str_split(text, '\n', &n);
	for (usize i = 0; i < n; i++) {
		const char *s = lines[i];
		int col = (int)str_to_int(s, &s);
		int row = (int)str_to_int(s, &s);
		if (*s != ' ' || !s[1])
			continue;
		if (nspots == spots_cap) {
			spots_cap = spots_cap ? spots_cap * 2 : 32;
			spots = mem_realloc(spots, sizeof(Spot) * (usize)spots_cap);
		}
		Spot *sp = &spots[nspots++];
		sp->col = col, sp->row = row;
		sp->set_at = 0;
		str_copy(sp->name, s + 1, sizeof sp->name);
	}
	str_split_free(lines, n);
	mem_free(text);
}

static void spots_save(void)
{
	StrBuf sb = { 0 };
	for (int i = 0; i < nicons; i++)
		sb_fmt(&sb, "%d %d %s\n", icons[i].col, icons[i].row, icons[i].name);
	/* places set for files on their way keep for a while; the rest are of files gone */
	int keep = 0;
	for (int i = 0; i < nspots; i++) {
		bool shown = false;
		for (int k = 0; k < nicons && !shown; k++)
			shown = str_eq(icons[k].name, spots[i].name);
		bool waiting = !shown && spots[i].set_at > 0 && S.now - spots[i].set_at < 60;
		if (waiting)
			sb_fmt(&sb, "%d %d %s\n", spots[i].col, spots[i].row, spots[i].name);
		if (shown || waiting)
			spots[keep++] = spots[i];
	}
	nspots = keep;
	char *p = spots_path();
	char *d = path_dir(p);
	if (!dir_exists(d)) {
		dir_make(d, 0755);
		sys_chown(d, S.set.uid, S.set.gid);
	}
	if (file_write_atomic(p, sb.data ? sb.data : "", sb.len))
		sys_chown(p, S.set.uid, S.set.gid);
	mem_free(d);
	mem_free(p);
	sb_free(&sb);
}

static void spot_set(const char *name, int col, int row)
{
	for (int i = 0; i < nspots; i++)
		if (str_eq(spots[i].name, name)) {
			spots[i].col = col, spots[i].row = row;
			spots[i].set_at = S.now;
			return;
		}
	if (nspots == spots_cap) {
		spots_cap = spots_cap ? spots_cap * 2 : 32;
		spots = mem_realloc(spots, sizeof(Spot) * (usize)spots_cap);
	}
	Spot *sp = &spots[nspots++];
	str_copy(sp->name, name, sizeof sp->name);
	sp->col = col, sp->row = row;
	sp->set_at = S.now;
}

static const Spot *spot_of(const char *name)
{
	for (int i = 0; i < nspots; i++)
		if (str_eq(spots[i].name, name))
			return &spots[i];
	return NULL;
}

/* ---- The folder ------------------------------------------------------------------------- */

static void invalidate(void)
{
	layer_dirty = true;
	request_frame();
}

void desktop_invalidate(void)
{
	invalidate();
	damage_all();
}

static int icon_cmp(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	const Icon *x = a, *y = b;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	return str_cmp_natural(x->name, y->name);
}

static void rescan(void)
{
	rescan_due = false;
	DirList l = { 0 };
	bool ok = dir[0] && dir_list(dir, &l, true);
	Icon *old = icons;
	int nold = nicons;
	icons = NEW_ARRAY(Icon, (ok ? l.len : 0) + 1);
	nicons = 0;
	if (ok) {
		vec_each(&l, e) {
			if (e->name[0] == '.')
				continue;
			Icon *ic = &icons[nicons++];
			memset(ic, 0, sizeof *ic);
			str_copy(ic->name, e->name, sizeof ic->name);
			ic->dir = e->type == DT_DIR;
			ic->size = e->size;
			ic->col = ic->row = -1;
			for (int k = 0; k < nold; k++)
				if (str_eq(old[k].name, ic->name)) {
					ic->selected = old[k].selected;
					ic->thumb = old[k].thumb;
					ic->thumb_tried = old[k].thumb_tried;
					old[k].thumb = NULL;
					/* a picture that changed gets a new thumbnail */
					if (old[k].size != ic->size) {
						canvas_free(ic->thumb);
						ic->thumb = NULL;
						ic->thumb_tried = false;
					}
				}
		}
		dir_list_free(&l);
	}
	for (int k = 0; k < nold; k++)
		canvas_free(old[k].thumb);
	mem_free(old);
	sort(icons, (usize)nicons, sizeof(Icon), icon_cmp, NULL);
	/* known places first, then the first free cells for the rest */
	int rows = grid_rows(), cols = grid_cols();
	for (int i = 0; i < nicons; i++) {
		const Spot *sp = spot_of(icons[i].name);
		if (sp && sp->col >= 0 && sp->row >= 0 && sp->row < rows && sp->col < cols && icon_in_cell(sp->col, sp->row, i) < 0)
			icons[i].col = sp->col, icons[i].row = sp->row;
	}
	bool placed = false;
	for (int i = 0; i < nicons; i++)
		if (icons[i].col < 0) {
			int c = 0, r = 0;
			free_cell(&c, &r, false);
			icons[i].col = c, icons[i].row = r;
			spot_set(icons[i].name, c, r);
			placed = true;
		}
	if (placed)
		spots_save();
	if (hover >= nicons)
		hover = -1;
	if (renaming >= nicons)
		renaming = -1;
	/* a new folder or document: name it right away */
	if (rename_soon[0])
		for (int i = 0; i < nicons; i++)
			if (str_eq(icons[i].name, rename_soon)) {
				for (int k = 0; k < nicons; k++)
					icons[k].selected = k == i;
				renaming = i;
				rename_frames = 0;
				sb_clear(&rename_text);
				sb_adds(&rename_text, icons[i].name);
				rename_soon[0] = 0;
				shell_mark_dirty();
			}
	desktop_invalidate();
}

static void on_inotify(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(loop), UNUSED(events), UNUSED(data);
	u8 buf[4096];
	while (sys_read(fd, buf, sizeof buf) > 0)
		;
	rescan_due = true;
	request_frame();
}

static void watch(void)
{
	if (ino >= 0) {
		loop_del_fd(S.loop, ino);
		sys_close(ino);
		ino = -1;
	}
	ino = (int)sys_inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino < 0)
		return;
	if (sys_inotify_add_watch(ino, dir, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB) < 0) {
		sys_close(ino);
		ino = -1;
		return;
	}
	loop_add_fd(S.loop, ino, EPOLLIN, on_inotify, NULL);
}

void desktop_init(void)
{
	str_copy(home_seen, S.set.home, sizeof home_seen);
	if (!S.set.home[0])
		return;
	fmt(dir, sizeof dir, "%s/Desktop", S.set.home);
	if (!dir_exists(dir) && dir_exists(S.set.home)) {
		dir_make(dir, 0755);
		sys_chown(dir, S.set.uid, S.set.gid);
	}
	spots_load();
	watch();
	rescan();
}

const char *desktop_dir(void)
{
	return dir;
}

static char *icon_path(int i)
{
	return path_join(dir, icons[i].name);
}

static int selected_count(void)
{
	int n = 0;
	for (int i = 0; i < nicons; i++)
		n += icons[i].selected;
	return n;
}

static char *selected_paths(int *count)
{
	StrBuf sb = { 0 };
	int n = 0;
	for (int i = 0; i < nicons; i++)
		if (icons[i].selected) {
			sb_fmt(&sb, "%s/%s\n", dir, icons[i].name);
			n++;
		}
	if (count)
		*count = n;
	char *r = str_dup(sb_str(&sb));
	sb_free(&sb);
	return r;
}

static void select_only(int i)
{
	for (int k = 0; k < nicons; k++)
		icons[k].selected = k == i;
}

/* ---- zfiles ----------------------------------------------------------------------------------- */

static void run_zfiles(char **args, int n)
{
	char *bin = zurbax_path("bin/zfiles");
	char **argv = NEW_ARRAY(char *, (usize)n + 2);
	argv[0] = "zfiles";
	for (int i = 0; i < n; i++)
		argv[i + 1] = args[i];
	argv[n + 1] = NULL;
	int pid = shell_user_spawn(bin, argv);
	if (pid > 0 && njobs < (int)ARRAY_LEN(jobs))
		jobs[njobs++] = pid;
	else if (pid < 0)
		shell_notify("system", "error", "Files couldn't be changed", "The file helper didn't start.");
	mem_free(argv);
	mem_free(bin);
}

bool desktop_reaped(int pid, int status)
{
	for (int i = 0; i < njobs; i++)
		if (jobs[i] == pid) {
			jobs[i] = jobs[--njobs];
			int code = (status >> 8) & 0xff;
			if ((status & 0x7f) == 0 && code > 0)
				shell_notify("system", "warning", code == 1 ? "An item couldn't be changed" : "Some items couldn't be changed",
					     "It may be in use, or you may not have permission.");
			return true;
		}
	return false;
}

/* Moves or copies files into `into`, starting from cell (col, row) when that's the desktop. */
static void bring(const char *list, const char *into, bool copy_wanted, int col, int row)
{
	bool to_desktop = str_eq(into, dir);
	char **args = NULL;
	int nargs = 0, cap = 0;
	char *keep[512];
	int nkeep = 0;
	bool moved_any = false, copy = copy_wanted;
	usize n;
	char **lines = str_split(list, '\n', &n);
	/* across drives it's a copy, unless asked otherwise later */
	for (usize i = 0; i < n && !copy; i++) {
		const char *src = str_trim(lines[i]);
		if (*src == '/' && !files_same_drive(src, into))
			copy = true;
	}
	for (usize i = 0; i < n && nkeep < (int)ARRAY_LEN(keep) - 2; i++) {
		const char *src = str_trim(lines[i]);
		if (*src != '/')
			continue;
		char *parent = path_dir(src);
		bool here = str_eq(parent, into);
		mem_free(parent);
		if (here && !copy) {
			/* already here: it just moves to where it was put */
			if (to_desktop && col >= 0) {
				int c = col, r = row;
				if (free_cell(&c, &r, false))
					spot_set(path_base(src), c, r), col = c, row = r + 1;
				moved_any = true;
			}
			continue;
		}
		if (files_inside(into, src))
			continue; /* a folder into itself */
		char *dst = files_unique_path(into, path_base(src));
		if (to_desktop && col >= 0) {
			int c = col, r = row;
			if (free_cell(&c, &r, false)) {
				spot_set(path_base(dst), c, r);
				col = c, row = r + 1;
				if (row >= grid_rows())
					col++, row = 0;
			}
		}
		if (nargs + 2 > cap) {
			cap = cap ? cap * 2 : 16;
			args = mem_realloc(args, sizeof(char *) * (usize)cap);
		}
		keep[nkeep++] = str_dup(src);
		keep[nkeep++] = dst;
		args[nargs++] = keep[nkeep - 2];
		args[nargs++] = keep[nkeep - 1];
	}
	str_split_free(lines, n);
	if (nargs) {
		char **all = NEW_ARRAY(char *, (usize)nargs + 1);
		all[0] = copy ? "copy" : "move";
		memcpy(all + 1, args, sizeof(char *) * (usize)nargs);
		run_zfiles(all, nargs + 1);
		mem_free(all);
	}
	if (nargs || moved_any) {
		spots_save();
		rescan_due = true;
	}
	for (int i = 0; i < nkeep; i++)
		mem_free(keep[i]);
	mem_free(args);
}

/* ---- Actions --------------------------------------------------------------------------- */

static void open_selected(void)
{
	int opened = 0;
	for (int i = 0; i < nicons && opened < 12; i++)
		if (icons[i].selected) {
			char *p = icon_path(i);
			shell_launch("", p);
			mem_free(p);
			opened++;
		}
}

static void trash_selected(void)
{
	char **args = NEW_ARRAY(char *, (usize)nicons + 2);
	int n = 0;
	args[n++] = "trash";
	for (int i = 0; i < nicons; i++)
		if (icons[i].selected)
			args[n++] = icon_path(i);
	if (n > 1)
		run_zfiles(args, n);
	for (int i = 1; i < n; i++)
		mem_free(args[i]);
	mem_free(args);
}

static void clip_selected(bool cut)
{
	char *paths = selected_paths(NULL);
	if (*paths) {
		mem_free(S.clipboard);
		S.clipboard = str_fmt("%s\n%s", cut ? "cut" : "copy", paths);
		S.clipboard_len = strlen(S.clipboard);
		str_copy(S.clipboard_mime, CLIPBOARD_FILES, sizeof S.clipboard_mime);
		clip_history_add(S.clipboard_mime, S.clipboard, S.clipboard_len);
		wl_clipboard_changed();
	}
	mem_free(paths);
}

static bool clipboard_has_files(void)
{
	return S.clipboard && str_eq(S.clipboard_mime, CLIPBOARD_FILES) && str_chr(S.clipboard, '\n');
}

static void paste(int col, int row)
{
	if (!clipboard_has_files())
		return;
	bool cut = str_starts(S.clipboard, "cut");
	bring(str_chr(S.clipboard, '\n') + 1, dir, !cut, col, row);
	if (cut) {
		/* moved once; the clipboard doesn't keep pointing at the old places */
		mem_free(S.clipboard);
		S.clipboard = NULL;
		S.clipboard_len = 0;
		S.clipboard_mime[0] = 0;
	}
}

static void make_new(bool folder, int col, int row)
{
	char *p = files_unique_path(dir, folder ? "New Folder" : "New Document.txt");
	if (col < 0 || !free_cell(&col, &row, false))
		col = row = 0, free_cell(&col, &row, false);
	spot_set(path_base(p), col, row);
	spots_save();
	str_copy(rename_soon, path_base(p), sizeof rename_soon);
	char *args[] = { folder ? "mkdir" : "touch", p };
	run_zfiles(args, 2);
	mem_free(p);
}

static void start_rename(int i)
{
	if (i < 0 || i >= nicons)
		return;
	renaming = i;
	rename_frames = 0;
	sb_clear(&rename_text);
	sb_adds(&rename_text, icons[i].name);
	shell_mark_dirty();
}

static void finish_rename(bool keep)
{
	if (renaming < 0)
		return;
	int i = renaming;
	renaming = -1;
	shell_mark_dirty();
	const char *name = str_trim(rename_text.data ? rename_text.data : "");
	if (!keep || i >= nicons || !*name || str_chr(name, '/') || str_eq(name, icons[i].name) || str_eq(name, ".") ||
	    str_eq(name, ".."))
		return;
	char *from = icon_path(i), *to = path_join(dir, name);
	spot_set(name, icons[i].col, icons[i].row);
	spots_save();
	char *args[] = { "rename", from, to };
	run_zfiles(args, 3);
	mem_free(from);
	mem_free(to);
}

static void arrange_by_name(void)
{
	sort(icons, (usize)nicons, sizeof(Icon), icon_cmp, NULL);
	int rows = grid_rows();
	for (int i = 0; i < nicons; i++) {
		icons[i].col = i / rows, icons[i].row = i % rows;
		spot_set(icons[i].name, icons[i].col, icons[i].row);
	}
	spots_save();
	desktop_invalidate();
}

/* The selection moves by whole cells, as one piece; icons in the way step aside. */
static void move_selection(int dcol, int drow)
{
	if (!dcol && !drow)
		return;
	int rows = grid_rows(), cols = grid_cols();
	for (int i = 0; i < nicons; i++)
		if (icons[i].selected) {
			icons[i].col = CLAMP(icons[i].col + dcol, 0, cols - 1);
			icons[i].row = CLAMP(icons[i].row + drow, 0, rows - 1);
		}
	/* collisions: the unselected icon (or a later selected one) finds a free cell */
	for (int i = 0; i < nicons; i++) {
		int other = icon_in_cell(icons[i].col, icons[i].row, i);
		if (other < 0 || other < i)
			continue;
		int v = icons[i].selected && !icons[other].selected ? other : i;
		int c = 0, r = 0;
		icons[v].col = icons[v].row = -100;
		free_cell(&c, &r, false);
		icons[v].col = c, icons[v].row = r;
	}
	for (int i = 0; i < nicons; i++)
		spot_set(icons[i].name, icons[i].col, icons[i].row);
	spots_save();
	desktop_invalidate();
}

/* ---- Drawing ---------------------------------------------------------------------------- */

Ui *desktop_aux_ui(Canvas *target, f32 w, f32 h)
{
	if (!aux)
		aux = ui_new();
	ui_attach(aux, target, w, h, S.scale);
	ui_set_theme(aux, &S.theme);
	return aux;
}

static bool is_picture(const char *name)
{
	const char *e = path_ext(name);
	return str_eq_nocase(e, "png") || str_eq_nocase(e, "jpg") || str_eq_nocase(e, "jpeg") || str_eq_nocase(e, "bmp") ||
	       str_eq_nocase(e, "gif");
}

static void draw_file_icon(Ui *ui, Rect r, const Icon *ic)
{
	if (ic->thumb) {
		f32 s = minf(r.w / (f32)ic->thumb->width, r.h / (f32)ic->thumb->height);
		Rect ir = center_in(r, (f32)ic->thumb->width * s, (f32)ic->thumb->height * s);
		ui_shadow(ui, ir, 3, 6, 2, color_hex("#00000060"));
		ui_image(ui, ir, ic->thumb, 3, false);
		ui_border(ui, ir, 3, 1.5f, color_hex("#ffffffd0"));
		return;
	}
	if (ic->dir) {
		ui_app_icon(ui, r, "folder");
		return;
	}
	Rect paper = center_in(r, 42, 52);
	ui_shadow(ui, paper, 3, 6, 2, color_hex("#00000050"));
	ui_gradient(ui, paper, 3, color_hex("#ffffff"), color_hex("#eef0f3"));
	ui_border(ui, paper, 3, 1, color_hex("#00000022"));
	ui_icon(ui, center_in(rect(paper.x, paper.y, paper.w, paper.h - 8), 22, 22), ui_file_icon(ic->name, false), color_hex("#5d6573"));
	const char *ext = path_ext(ic->name);
	if (*ext && strlen(ext) <= 4) {
		char up[8];
		str_copy(up, ext, sizeof up);
		for (char *c = up; *c; c++)
			*c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
		ui_text_in(ui, ui_font(ui, 8, 700), rect(paper.x, paper.y + paper.h - 14, paper.w, 10), up, color_hex("#8a919d"), ALIGN_CENTER);
	}
}

/* The longest start of s[0..n) that fits in w, with "…" when it's cut. */
static void fit_line(Ui *ui, Font f, const char *s, usize n, f32 w, bool more, char *out, usize cap)
{
	while (n > 0 && s[n - 1] == ' ')
		n--;
	if (!more && ui_text_width_n(ui, f, s, n) <= w) {
		fmt(out, cap, "%.*s", (int)n, s);
		return;
	}
	f32 dots = ui_text_width(ui, f, "…");
	while (n > 0 && ui_text_width_n(ui, f, s, n) + dots > w)
		n = utf8_prev(s, n);
	while (n > 0 && s[n - 1] == ' ')
		n--;
	fmt(out, cap, "%.*s…", (int)n, s);
}

/* A name under its icon in up to `max` lines, white on a soft shadow so it
 * reads on any wallpaper; selected, on the accent. */
static void draw_label(Ui *ui, f32 cx, f32 y, f32 w, const char *name, bool selected)
{
	Font f = ui_font(ui, 12, 500);
	Font df = f;
	df.size *= ui->scale;
	int max = selected ? 3 : 2;
	TextLine tl[8];
	usize len = strlen(name);
	int n = MIN(text_wrap(df, name, len, w * ui->scale, tl, 8), 8);
	char lines[3][256];
	int nl = 0;
	for (int i = 0; i < n && nl < max; i++) {
		usize start = tl[i].start, end = i + 1 < n ? tl[i + 1].start : len;
		bool last = nl == max - 1;
		/* the last line takes the rest of the name, cut to fit */
		if (last)
			end = len;
		fit_line(ui, f, name + start, end - start, w, false, lines[nl], sizeof lines[0]);
		nl++;
	}
	f32 lh = 16, top = y;
	if (selected) {
		f32 bw = 0;
		for (int i = 0; i < nl; i++)
			bw = maxf(bw, ui_text_width(ui, f, lines[i]));
		ui_round(ui, rect(cx - bw / 2 - 5, top - 1, bw + 10, lh * (f32)nl + 3), 5, color_with_alpha(ui->theme.accent, 0xe6));
	}
	for (int i = 0; i < nl; i++) {
		f32 tw = ui_text_width(ui, f, lines[i]);
		f32 x = cx - tw / 2, base = top + lh * (f32)i + 12;
		if (!selected)
			ui_text(ui, f, x + 1, base + 1, lines[i], color_hex("#000000b0"));
		ui_text(ui, f, x, base, lines[i], selected ? ui->theme.on_accent : COLOR_WHITE);
	}
}

static Rect band_rect(void)
{
	return rect(minf(press_x, band_x), minf(press_y, band_y), fabsf(band_x - press_x), fabsf(band_y - press_y));
}

static void draw_icons(Ui *ui)
{
	Theme *t = &ui->theme;
	f64 budget = (f64)time_mono_ns() / 1e9 + 0.015;
	for (int i = 0; i < nicons; i++) {
		Icon *ic = &icons[i];
		Rect cell = cell_rect(ic->col, ic->row);
		Rect hit = rect_inset(cell, 5, 2);
		if (i == drop_folder) {
			ui_round(ui, hit, 10, color_with_alpha(t->accent, 0x50));
			ui_border(ui, hit, 10, 2, t->accent);
		} else if (ic->selected) {
			ui_round(ui, hit, 10, color_with_alpha(t->accent, 0x45));
			ui_border(ui, hit, 10, 1, color_with_alpha(t->accent, 0xb0));
		} else if (i == hover) {
			ui_round(ui, hit, 10, color_hex("#ffffff26"));
		}
		/* pictures get a thumbnail, a few per drawing so nothing stalls */
		if (!ic->thumb && !ic->thumb_tried && !ic->dir && is_picture(ic->name) && ic->size < (48ll << 20)) {
			if ((f64)time_mono_ns() / 1e9 < budget) {
				ic->thumb_tried = true;
				char *p = icon_path(i);
				ic->thumb = image_load_thumb(p, (int)(ICON * S.scale * 2));
				mem_free(p);
			} else {
				layer_dirty = true;
			}
		}
		draw_file_icon(ui, rect(cell.x + (cell.w - ICON) / 2, cell.y + 6, ICON, ICON), ic);
		if (i != renaming)
			draw_label(ui, cell.x + cell.w / 2, cell.y + 8 + ICON + 4, cell.w - 10, ic->name, ic->selected);
	}
	/* where dragged files would land */
	if (drop_col >= 0 && drop_folder < 0) {
		Rect c = rect_inset(cell_rect(drop_col, drop_row), 5, 2);
		ui_round(ui, c, 10, color_hex("#ffffff1c"));
		ui_border(ui, c, 10, 1.5f, color_hex("#ffffffa0"));
	}
	if (banding && moved) {
		Rect b = band_rect();
		ui_fill(ui, b, color_with_alpha(t->accent, 0x38));
		ui_border(ui, b, 0, 1, color_with_alpha(t->accent, 0xd0));
	}
}

/* Everything the icons (and the band) cover: what to repaint when they change. */
static Rect icons_bounds(void)
{
	Rect b = rect(0, 0, 0, 0);
	bool any = false;
	for (int i = 0; i < nicons; i++) {
		Rect c = cell_rect(icons[i].col, icons[i].row);
		b = any ? rect_union(b, c) : c;
		any = true;
	}
	if (drop_col >= 0) {
		Rect c = cell_rect(drop_col, drop_row);
		b = any ? rect_union(b, c) : c;
		any = true;
	}
	if (banding) {
		Rect c = rect_inset(band_rect(), -4, -4);
		b = any ? rect_union(b, c) : c;
	}
	return b;
}

const Canvas *desktop_background(void)
{
	if (!nicons || S.setup_mode || !S.wallpaper)
		return S.wallpaper;
	if (!layer || layer->width != S.frame->width || layer->height != S.frame->height) {
		canvas_free(layer);
		layer = canvas_new(S.frame->width, S.frame->height);
		layer_dirty = true;
		damage_all();
	}
	if (layer_dirty) {
		layer_dirty = false;
		copy_pixels(layer, S.wallpaper, 0, 0, 0, 0, layer->width, layer->height);
		Ui *u = desktop_aux_ui(layer, S.sw, S.sh);
		ui_frame(u, draw_icons);
		if (layer_dirty)
			request_frame(); /* thumbnails still to make */
	}
	return layer;
}

/* The icons changed: draw them again and repaint what they cover, then and now. */
static void changed(void)
{
	Rect b = icons_bounds();
	damage_rect(last_bounds);
	damage_rect(b);
	last_bounds = b;
	invalidate();
}

/* ---- Pointer and keys --------------------------------------------------------------------- */

bool desktop_grabbing(void)
{
	return grabbing;
}

bool desktop_focused(void)
{
	return focused && !S.focus;
}

void desktop_unfocus(void)
{
	focused = false;
}

bool desktop_pointer(u32 kind, u32 button)
{
	if (S.setup_mode || S.locked || !dir[0])
		return false;
	f32 x = S.px, y = S.py;
	if (kind == POINTER_LEAVE) {
		if (hover >= 0) {
			hover = -1;
			changed();
		}
		return false;
	}
	if (kind == POINTER_MOTION) {
		if (grabbing) {
			f32 dx = x - press_x, dy = y - press_y;
			if (dx * dx + dy * dy > 36)
				moved = true;
			if (press_icon >= 0 && moved && (S.buttons & (1u << BUTTON_LEFT))) {
				/* the selection goes for a ride */
				int n = 0;
				char *paths = selected_paths(&n);
				const Canvas *th = icons[press_icon].thumb;
				Canvas *copy = n == 1 && th ? canvas_resize(th, th->width, th->height) : NULL;
				grabbing = false;
				dnd_start(NULL, paths, n, copy);
				mem_free(paths);
				return true;
			}
			if (banding) {
				band_x = x, band_y = y;
				Rect b = band_rect();
				for (int i = 0; i < nicons; i++) {
					bool in = !rect_empty(rect_intersect(b, icon_hit_rect(i)));
					if (!(S.mods & MOD_CTRL))
						icons[i].selected = in;
					else if (in)
						icons[i].selected = true;
				}
				changed();
			}
			return true;
		}
		int h = icon_at(x, y);
		if (h != hover) {
			hover = h;
			changed();
		}
		return false;
	}
	if (kind == POINTER_DOWN) {
		if (renaming >= 0)
			finish_rename(true);
		menu = MENU_NONE;
		focused = true;
		window_focus(NULL);
		int i = icon_at(x, y);
		if (button == BUTTON_LEFT) {
			grabbing = true;
			moved = false;
			press_x = band_x = x, press_y = band_y = y;
			press_icon = i;
			banding = i < 0;
			if (i >= 0) {
				bool twice = i == last_click_icon && S.now - last_click_t < 0.4;
				last_click_t = S.now;
				last_click_icon = i;
				if (S.mods & MOD_CTRL)
					icons[i].selected = !icons[i].selected;
				else if (!icons[i].selected)
					select_only(i);
				if (twice) {
					open_selected();
					grabbing = false;
					last_click_icon = -1;
				}
			} else if (!(S.mods & MOD_CTRL)) {
				select_only(-1);
			}
		} else if (button == BUTTON_RIGHT) {
			if (i >= 0) {
				if (!icons[i].selected)
					select_only(i);
				menu = MENU_ICON;
			} else {
				select_only(-1);
				menu = MENU_DESK;
			}
			menu_x = x, menu_y = y;
			menu_opened = 0;
		}
		changed();
		shell_mark_dirty();
		return true;
	}
	if (kind == POINTER_UP) {
		if (button == BUTTON_LEFT && grabbing) {
			/* a plain click on one of several selected icons picks just it */
			if (press_icon >= 0 && !moved && !(S.mods & MOD_CTRL))
				select_only(press_icon);
			grabbing = false;
			banding = false;
			press_icon = -1;
			changed();
		}
		return true;
	}
	return false;
}

bool desktop_key(u32 code, u32 state, u32 mods, const char *text)
{
	UNUSED(text);
	if (!desktop_focused() || S.setup_mode || S.locked || state == KEY_RELEASED || !dir[0])
		return false;
	bool any = selected_count() > 0;
	mods &= ~(u32)MOD_CAPS;
	if (code == KEY_ESC) {
		menu = MENU_NONE;
		select_only(-1);
	} else if (code == KEY_DELETE && any) {
		trash_selected();
	} else if (code == KEY_F2 && selected_count() == 1) {
		for (int i = 0; i < nicons; i++)
			if (icons[i].selected)
				start_rename(i);
	} else if ((code == KEY_ENTER || code == KEY_KPENTER) && any) {
		open_selected();
	} else if (code == KEY_A && mods == MOD_CTRL) {
		for (int i = 0; i < nicons; i++)
			icons[i].selected = true;
	} else if (code == KEY_C && mods == MOD_CTRL && any) {
		clip_selected(false);
	} else if (code == KEY_X && mods == MOD_CTRL && any) {
		clip_selected(true);
	} else if (code == KEY_V && mods == MOD_CTRL) {
		paste(-1, 0);
	} else if (code == KEY_N && mods == (MOD_CTRL | MOD_SHIFT)) {
		make_new(true, -1, 0);
	} else if ((code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT) && mods == 0 && nicons) {
		/* arrows walk the selection through the grid */
		int cur = -1;
		for (int i = 0; i < nicons && cur < 0; i++)
			if (icons[i].selected)
				cur = i;
		if (cur < 0) {
			select_only(0);
		} else {
			int dc = code == KEY_LEFT ? -1 : code == KEY_RIGHT ? 1 : 0, dr = code == KEY_UP ? -1 : code == KEY_DOWN ? 1 : 0;
			int best = -1;
			f32 best_d = 1e9f;
			for (int i = 0; i < nicons; i++) {
				int ddc = icons[i].col - icons[cur].col, ddr = icons[i].row - icons[cur].row;
				if (i == cur || (dc && ddc * dc <= 0) || (dr && ddr * dr <= 0))
					continue;
				f32 d = (f32)(ddc * ddc + ddr * ddr) + (dc ? (f32)(ddr * ddr) * 4 : (f32)(ddc * ddc) * 4);
				if (d < best_d)
					best_d = d, best = i;
			}
			if (best >= 0)
				select_only(best);
		}
	} else {
		return false;
	}
	changed();
	shell_mark_dirty();
	return true;
}

void desktop_tick(void)
{
	if (!str_eq(home_seen, S.set.home))
		desktop_init();
	if (rescan_due)
		rescan();
	/* the work area changed (taskbar size, top bar): icons off the grid come back */
	static int rows_seen, cols_seen;
	if (rows_seen != grid_rows() || cols_seen != grid_cols()) {
		bool first = !rows_seen;
		rows_seen = grid_rows(), cols_seen = grid_cols();
		if (!first)
			rescan();
	}
}

/* ---- Dragging onto the desktop ---------------------------------------------------------- */

void desktop_drag_over(f32 x, f32 y, const char *paths, bool from_desktop)
{
	int col = -1, row = -1, folder = -1;
	if (x >= 0 && dir[0]) {
		int i = icon_at(x, y);
		if (i >= 0 && icons[i].dir && !(from_desktop && icons[i].selected))
			folder = i;
		else
			cell_at(x, y, &col, &row);
		UNUSED(paths);
	}
	if (col != drop_col || row != drop_row || folder != drop_folder) {
		drop_col = col, drop_row = row, drop_folder = folder;
		changed();
	}
}

void desktop_drop(const char *paths, f32 x, f32 y, u32 mods, bool from_desktop)
{
	if (!dir[0])
		return;
	int col, row;
	cell_at(x, y, &col, &row);
	int i = icon_at(x, y);
	bool copy = (mods & MOD_CTRL) != 0;
	if (i >= 0 && icons[i].dir && !(from_desktop && icons[i].selected)) {
		/* into the folder */
		char *into = icon_path(i);
		bring(paths, into, copy, -1, 0);
		mem_free(into);
		return;
	}
	if (from_desktop && !copy) {
		/* the icons move as a group, the one held landing where it was let go */
		int anchor = press_icon >= 0 ? press_icon : -1;
		for (int k = 0; k < nicons && anchor < 0; k++)
			if (icons[k].selected)
				anchor = k;
		if (anchor >= 0)
			move_selection(col - icons[anchor].col, row - icons[anchor].row);
		press_icon = -1;
		return;
	}
	bring(paths, dir, copy, col, row);
}

/* ---- Menus and renaming (in the shell overlay) -------------------------------------------- */

bool desktop_wants_pointer(void)
{
	return menu != MENU_NONE || renaming >= 0;
}

bool desktop_wants_keys(void)
{
	return renaming >= 0 || menu != MENU_NONE;
}

static bool item(Ui *ui, Rect *c, const char *icon, const char *label, const char *keys)
{
	return ui_menu_item(ui, cut_top(c, 34), icon, label, keys);
}

static void separator(Ui *ui, Rect *c)
{
	Rect s = cut_top(c, 9);
	ui_separator(ui, s.x + 8, s.y + 4, s.x + s.w - 8);
}

void desktop_menu_draw(Ui *ui)
{
	if (renaming >= 0 && renaming < nicons) {
		Icon *ic = &icons[renaming];
		Rect cell = cell_rect(ic->col, ic->row);
		Rect r = rect(cell.x - 14, cell.y + 8 + ICON + 2, cell.w + 28, 28);
		ui_shadow(ui, r, 6, 10, 3, color_hex("#00000050"));
		if (rename_frames == 0) {
			/* the name is selected, without its extension, ready to be typed over */
			const char *ext = path_ext(ic->name);
			usize stem = strlen(ic->name) - (*ext && !ic->dir && ext - 1 != ic->name ? strlen(ext) + 1 : 0);
			ui_focus(ui, "desk-rename");
			ui_textarea_select(ui, "desk-rename", 0, stem);
		}
		int ev = ui_entry(ui, r, "desk-rename", &rename_text, "Name", ENTRY_AUTOFOCUS);
		shell_surface(r);
		rename_frames++;
		if (ev & ENTRY_SUBMIT)
			finish_rename(true);
		else if ((ev & ENTRY_ESCAPE) || ui_key_pressed(ui, KEY_ESC, 0))
			finish_rename(false);
		else if (rename_frames > 2 && !ui_is_focused(ui, "desk-rename"))
			finish_rename(true); /* clicked elsewhere */
		else if (rename_frames > 1 && ui->pressed[BUTTON_LEFT] && !rect_contains(r, ui->mx, ui->my))
			finish_rename(true);
	}
	if (menu == MENU_NONE)
		return;
	Theme *t = &ui->theme;
	int nsel = selected_count();
	bool paste_ok = clipboard_has_files();
	f32 w = 250, h;
	if (menu == MENU_ICON)
		h = 12 + 34 * (nsel == 1 ? 6 : 5) + 9 * 2;
	else
		h = 12 + 34 * 8 + 9 * 2;
	Rect r = rect(clampf(menu_x, 6, S.sw - w - 6), clampf(menu_y, shell_panel_height() + 6, S.sh - h - 6), w, h);
	ui_shadow(ui, r, 12, 22, 8, color_hex("#00000055"));
	ui_round(ui, r, 12, t->card);
	ui_border(ui, r, 12, 1, t->border);
	shell_surface(r);
	Rect c = pad(r, 6);
	int col, row;
	cell_at(menu_x, menu_y, &col, &row);
	bool close = false;
	if (menu == MENU_ICON) {
		if (item(ui, &c, "document-open", nsel > 1 ? "Open All" : "Open", "Enter"))
			open_selected(), close = true;
		separator(ui, &c);
		if (item(ui, &c, "edit-cut", "Cut", "Ctrl+X"))
			clip_selected(true), close = true;
		if (item(ui, &c, "edit-copy", "Copy", "Ctrl+C"))
			clip_selected(false), close = true;
		if (nsel == 1 && item(ui, &c, "edit", "Rename…", "F2")) {
			for (int i = 0; i < nicons; i++)
				if (icons[i].selected)
					start_rename(i);
			close = true;
		}
		separator(ui, &c);
		if (item(ui, &c, "folder-open", "Show in Files", NULL))
			shell_launch("vault", dir), close = true;
		if (item(ui, &c, "trash", "Move to Trash", "Delete"))
			trash_selected(), close = true;
	} else {
		if (item(ui, &c, "folder", "New Folder", "Shift+Ctrl+N"))
			make_new(true, col, row), close = true;
		if (item(ui, &c, "text-file", "New Document", NULL))
			make_new(false, col, row), close = true;
		if (paste_ok) {
			if (item(ui, &c, "edit-paste", "Paste", "Ctrl+V"))
				paste(col, row), close = true;
		} else {
			Rect p = cut_top(&c, 34);
			ui_icon(ui, rect(p.x + 10, p.y + 9, 16, 16), "edit-paste", t->text_faint);
			ui_text(ui, ui_font(ui, 13, 400), p.x + 36, p.y + 22, "Paste", t->text_faint);
		}
		separator(ui, &c);
		if (item(ui, &c, "sort", "Arrange by Name", NULL))
			arrange_by_name(), close = true;
		if (item(ui, &c, "folder-open", "Open Desktop in Files", NULL))
			shell_launch("vault", dir), close = true;
		if (item(ui, &c, "terminal", "Open in Terminal", NULL))
			shell_launch("forge", dir), close = true;
		separator(ui, &c);
		if (item(ui, &c, "wallpaper", "Change Background…", NULL))
			shell_launch("settings", "appearance"), close = true;
		if (item(ui, &c, "display", "Display Settings", NULL))
			shell_launch("settings", "display"), close = true;
	}
	/* the click that opened it mustn't close it */
	bool fresh = menu_opened == 0;
	if (fresh)
		menu_opened = ui->frame_no;
	if (close || ui_key_pressed(ui, KEY_ESC, 0) ||
	    (ui->frame_no != menu_opened && (ui->pressed[BUTTON_LEFT] || ui->pressed[BUTTON_RIGHT]) &&
	     !rect_contains(rect_inset(r, -2, -2), ui->mx, ui->my))) {
		menu = MENU_NONE;
		changed();
	}
}
