/*
 * Photos — your pictures, by month, and a viewer.
 *
 * The library collects pictures from ~/Pictures (screenshots included).
 * The viewer zooms with the scroll wheel or keys, pans by dragging,
 * rotates, shows details, sets a picture as wallpaper and moves pictures
 * to the Trash.
 */
#include "zui.h"

typedef struct {
	char *path;
	i64 mtime, size;
	Canvas *thumb;
	bool thumb_tried;
} Photo;

typedef struct {
	Photo *photos;
	int n, cap;
	/* viewer */
	int current;          /* -1: library */
	Canvas *full;         /* the picture at full size */
	Canvas *fit;          /* scaled to the screen, for drawing when not zoomed in */
	int rotation;         /* quarter turns */
	f32 zoom;             /* 0 = fit */
	f32 pan_x, pan_y;
	bool dragging;
	f32 drag_x, drag_y;
	bool show_info;
	char loaded_path[1024];
	int img_w, img_h;
	char message[128];
	f64 message_until;
	bool from_file;       /* opened with a file: no library behind it */
} Photos;

static Photos ph;

/* ---- Library -------------------------------------------------------------------------- */

static bool is_picture(const char *name)
{
	const char *e = path_ext(name);
	return str_eq_nocase(e, "png") || str_eq_nocase(e, "jpg") || str_eq_nocase(e, "jpeg") || str_eq_nocase(e, "bmp");
}

static void add_photo(const char *path, i64 mtime, i64 size)
{
	if (ph.n == ph.cap) {
		ph.cap = ph.cap ? ph.cap * 2 : 128;
		ph.photos = mem_realloc(ph.photos, sizeof(Photo) * (usize)ph.cap);
	}
	ph.photos[ph.n++] = (Photo){ .path = str_dup(path), .mtime = mtime, .size = size };
}

static void scan(const char *dir, int depth)
{
	DirList list = { 0 };
	if (depth > 4 || !dir_list(dir, &list, true))
		return;
	vec_each(&list, e) {
		if (e->name[0] == '.')
			continue;
		char *p = path_join(dir, e->name);
		if (e->type == DT_DIR)
			scan(p, depth + 1);
		else if (is_picture(e->name))
			add_photo(p, e->mtime, e->size);
		mem_free(p);
	}
	dir_list_free(&list);
}

static int newest_first(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	const Photo *x = a, *y = b;
	return x->mtime > y->mtime ? -1 : x->mtime < y->mtime ? 1 : str_cmp_natural(x->path, y->path);
}

static int by_name(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	return str_cmp_natural(path_base(((const Photo *)a)->path), path_base(((const Photo *)b)->path));
}

static void load_library(void)
{
	char *pics = str_fmt("%s/Pictures", home_dir());
	dir_make(pics, 0755);
	scan(pics, 0);
	mem_free(pics);
	sort(ph.photos, (usize)ph.n, sizeof(Photo), newest_first, NULL);
}

/* Siblings of a file opened from elsewhere, so the arrows browse its folder. */
static void load_folder_of(const char *file)
{
	char *dir = path_dir(file);
	DirList list = { 0 };
	if (dir_list(dir, &list, true)) {
		vec_each(&list, e) {
			if (e->type != DT_DIR && is_picture(e->name)) {
				char *p = path_join(dir, e->name);
				add_photo(p, e->mtime, e->size);
				mem_free(p);
			}
		}
		dir_list_free(&list);
	}
	mem_free(dir);
	sort(ph.photos, (usize)ph.n, sizeof(Photo), by_name, NULL);
	ph.current = 0;
	for (int i = 0; i < ph.n; i++)
		if (str_eq(ph.photos[i].path, file))
			ph.current = i;
}

/* ---- Viewer ------------------------------------------------------------------------------ */

static Canvas *rotate90(const Canvas *src)
{
	Canvas *d = canvas_new(src->height, src->width);
	for (int y = 0; y < src->height; y++) {
		const u32 *row = canvas_row(src, y);
		for (int x = 0; x < src->width; x++)
			canvas_row(d, x)[src->height - 1 - y] = row[x];
	}
	return d;
}

static void unload(void)
{
	canvas_free(ph.full);
	canvas_free(ph.fit);
	ph.full = ph.fit = NULL;
	ph.loaded_path[0] = 0;
}

static void ensure_loaded(void)
{
	if (ph.current < 0 || ph.current >= ph.n)
		return;
	Photo *p = &ph.photos[ph.current];
	if (str_eq(ph.loaded_path, p->path))
		return;
	unload();
	str_copy(ph.loaded_path, p->path, sizeof ph.loaded_path);
	ph.full = image_load(p->path);
	if (!ph.full)
		return;
	ph.img_w = ph.full->width;
	ph.img_h = ph.full->height;
	for (int i = 0; i < ph.rotation; i++) {
		Canvas *r = rotate90(ph.full);
		canvas_free(ph.full);
		ph.full = r;
	}
	/* A copy at most about screen-sized keeps drawing fast when fitted. */
	f32 s = minf(1, minf(2560.0f / (f32)ph.full->width, 1600.0f / (f32)ph.full->height));
	if (s < 1)
		ph.fit = canvas_resize(ph.full, MAX(1, (int)((f32)ph.full->width * s)), MAX(1, (int)((f32)ph.full->height * s)));
}

static void open_photo(int i)
{
	ph.current = i;
	ph.zoom = 0;
	ph.pan_x = ph.pan_y = 0;
	ph.rotation = 0;
	ensure_loaded();
}

static void flash(Ui *ui, const char *msg)
{
	str_copy(ph.message, msg, sizeof ph.message);
	ph.message_until = ui->now + 2.5;
	ui->wake_at = ph.message_until;
}

static void set_wallpaper(Ui *ui)
{
	char *path = str_fmt("%s/.config/zurbax/settings.conf", home_dir());
	Conf *c = conf_load(path);
	conf_set(c, "wallpaper", ph.photos[ph.current].path);
	conf_save(c);
	conf_free(c);
	mem_free(path);
	ui_settings_changed(ui);
	flash(ui, "Set as wallpaper");
}

static void delete_current(Ui *ui)
{
	Photo *p = &ph.photos[ph.current];
	char *tf = str_fmt("%s/.local/share/Trash/files", home_dir());
	char *ti = str_fmt("%s/.local/share/Trash/info", home_dir());
	dir_make(tf, 0700);
	dir_make(ti, 0700);
	char *dst = path_join(tf, path_base(p->path));
	char *info = str_fmt("%s/%s.trashinfo", ti, path_base(p->path));
	char *body = str_fmt("[Trash Info]\nPath=%s\n", p->path);
	file_write(info, body, strlen(body));
	bool ok = sys_rename(p->path, dst) == 0;
	if (!ok)
		sys_unlink(info);
	mem_free(body);
	mem_free(info);
	mem_free(dst);
	mem_free(tf);
	mem_free(ti);
	if (!ok) {
		flash(ui, "The picture couldn't be moved to the Trash");
		return;
	}
	mem_free(p->path);
	canvas_free(p->thumb);
	memmove(p, p + 1, sizeof(Photo) * (usize)(ph.n - ph.current - 1));
	ph.n--;
	unload();
	if (ph.n == 0)
		ph.current = -1;
	else
		open_photo(MIN(ph.current, ph.n - 1));
	flash(ui, "Moved to the Trash");
}

static f32 fit_scale(Rect view)
{
	if (!ph.full)
		return 1;
	return minf(1, minf(view.w / (f32)ph.full->width, view.h / (f32)ph.full->height));
}

static void viewer(Ui *ui, Rect r)
{
	ensure_loaded();
	ui_fill(ui, r, color_hex("#141414"));
	Rect view = pad(r, 20);
	if (!ph.full) {
		ui_placeholder(ui, r, "image", "Can't Show This Picture", "The file may be damaged or in an unsupported format");
		return;
	}
	f32 fs = fit_scale(view);
	f32 scale = ph.zoom > 0 ? ph.zoom : fs;
	/* Zoom around the pointer with the wheel. */
	if (ui_mouse_in(ui, r) && ui->scroll_dy != 0 && ui->scroll_source == SCROLL_WHEEL) {
		f32 old = scale;
		scale = clampf(scale * (ui->scroll_dy < 0 ? 1.15f : 1 / 1.15f), fs * 0.5f, 8);
		f32 cx = view.x + view.w / 2 + ph.pan_x, cy = view.y + view.h / 2 + ph.pan_y;
		ph.pan_x += (ui->mx - cx) * (1 - scale / old);
		ph.pan_y += (ui->my - cy) * (1 - scale / old);
		ph.zoom = scale;
		ui->scroll_dy = 0;
	}
	UiInteract it = ui_interact(ui, ui_id(ui, "canvas"), r);
	if (it.pressed_now) {
		ph.dragging = true;
		ph.drag_x = ui->mx;
		ph.drag_y = ui->my;
	}
	if (ph.dragging) {
		if (ui->down[BUTTON_LEFT]) {
			ph.pan_x += ui->mx - ph.drag_x;
			ph.pan_y += ui->my - ph.drag_y;
			ph.drag_x = ui->mx;
			ph.drag_y = ui->my;
			ui_set_cursor(ui, CURSOR_GRAB);
		} else {
			ph.dragging = false;
		}
	}
	if (it.double_clicked) {
		ph.zoom = ph.zoom > 0 ? 0 : 1;
		ph.pan_x = ph.pan_y = 0;
		scale = ph.zoom > 0 ? 1 : fs;
	}
	f32 w = (f32)ph.full->width * scale, h = (f32)ph.full->height * scale;
	/* Keep the picture from being dragged out of sight. */
	f32 mx = maxf(0, (w - view.w) / 2 + 40), my = maxf(0, (h - view.h) / 2 + 40);
	ph.pan_x = clampf(ph.pan_x, -mx, mx);
	ph.pan_y = clampf(ph.pan_y, -my, my);
	if (w <= view.w)
		ph.pan_x = 0;
	if (h <= view.h)
		ph.pan_y = 0;
	Rect dst = rect(view.x + (view.w - w) / 2 + ph.pan_x, view.y + (view.h - h) / 2 + ph.pan_y, w, h);
	Rect d = ui_to_device(ui, dst);
	Rect clip = ui_to_device(ui, r);
	Rect vis = rect_intersect(d, clip);
	if (!rect_empty(vis)) {
		const Canvas *src = ph.full;
		f32 k = 1;
		if (ph.fit && scale * ui->scale <= (f32)ph.fit->width / (f32)ph.full->width * 1.01f) {
			src = ph.fit;
			k = (f32)ph.fit->width / (f32)ph.full->width;
		}
		/* Only the visible part of the source, so zoomed views stay fast. */
		f32 sx = (vis.x - d.x) / d.w * (f32)ph.full->width * k, sy = (vis.y - d.y) / d.h * (f32)ph.full->height * k;
		f32 sw = vis.w / d.w * (f32)ph.full->width * k, sh = vis.h / d.h * (f32)ph.full->height * k;
		ui_shadow(ui, dst, 2, 20, 6, color_hex("#00000080"));
		blit_scaled(ui->canvas, src, rect(sx, sy, sw, sh), vis, 1);
	}
	/* Zoom level badge. */
	char z[16];
	fmt(z, sizeof z, "%d%%", (int)roundf(scale * 100));
	Font zf = ui_font(ui, 12, 600);
	f32 zw = ui_text_width(ui, zf, z) + 20;
	Rect zr = rect(r.x + r.w - zw - 16, r.y + r.h - 40, zw, 26);
	ui_round(ui, zr, 13, color_hex("#000000a0"));
	ui_text_in(ui, zf, zr, z, COLOR_WHITE, ALIGN_CENTER);
	/* Side arrows. */
	if (ph.n > 1) {
		for (int side = 0; side < 2; side++) {
			Rect a = rect(side ? r.x + r.w - 64 : r.x + 16, r.y + r.h / 2 - 24, 48, 48);
			ui_push_id(ui, side);
			UiInteract ai = ui_interact(ui, ui_id(ui, "arrow"), a);
			ui_pop_id(ui);
			f32 k = ui_fade(ui, ui_id(ui, side ? "ra" : "la"), ai.hovered || ui_mouse_in(ui, rect(side ? r.x + r.w - 140 : r.x, r.y, 140, r.h)), 0.15f);
			if (k > 0) {
				ui_circle(ui, a.x + 24, a.y + 24, 24, color_scale_alpha(color_hex("#000000a0"), k));
				ui_icon(ui, center_in(a, 22, 22), side ? "go-forward" : "go-back", color_scale_alpha(COLOR_WHITE, k));
			}
			if (ai.clicked)
				open_photo((ph.current + (side ? 1 : ph.n - 1)) % ph.n);
		}
	}
	if (ui->now < ph.message_until) {
		Font mf = ui_font(ui, 13, 600);
		f32 mw = ui_text_width(ui, mf, ph.message) + 32;
		Rect m = rect(r.x + (r.w - mw) / 2, r.y + 20, mw, 34);
		ui_round(ui, m, 17, color_hex("#000000c0"));
		ui_text_in(ui, mf, m, ph.message, COLOR_WHITE, ALIGN_CENTER);
	}
}

static void info_panel(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->sidebar);
	ui_vseparator(ui, r.x, r.y, r.y + r.h);
	Photo *p = &ph.photos[ph.current];
	Rect in = pad(r, 18);
	ui_heading(ui, cut_top(&in, 28), "Details");
	cut_top(&in, 8);
	char buf[128];
	struct {
		const char *label;
		char value[256];
	} rows[5];
	str_copy(rows[0].value, path_base(p->path), sizeof rows[0].value);
	rows[0].label = "Name";
	rows[1].label = "Size";
	fmt(rows[1].value, sizeof rows[1].value, "%d × %d pixels", ph.img_w, ph.img_h);
	rows[2].label = "File size";
	fmt_bytes(rows[2].value, sizeof rows[2].value, (u64)p->size);
	Civil c;
	civil_local(p->mtime, &c);
	rows[3].label = "Date";
	fmt(rows[3].value, sizeof rows[3].value, "%d %s %d, %02d:%02d", c.day, MONTH_NAMES[c.month - 1], c.year, c.hour, c.min);
	rows[4].label = "Folder";
	char *dir = path_dir(p->path);
	const char *home = home_dir();
	if (str_starts(dir, home))
		fmt(rows[4].value, sizeof rows[4].value, "~%s", dir + strlen(home));
	else
		str_copy(rows[4].value, dir, sizeof rows[4].value);
	mem_free(dir);
	for (int i = 0; i < 5; i++) {
		ui_text(ui, ui_font(ui, 12, 500), in.x, in.y + 14, rows[i].label, t->text_dim);
		f32 h = ui_text_wrapped(ui, ui_font(ui, 13, 500), rect(in.x, in.y + 20, in.w, 60), rows[i].value, t->text, ALIGN_START);
		cut_top(&in, 30 + maxf(h, 16));
	}
	UNUSED(buf);
}

static void library(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->view);
	if (ph.n == 0) {
		ui_placeholder(ui, r, "image", "No Pictures Yet", "Pictures you save in the Pictures folder appear here");
		return;
	}
	f32 cell = 150, gap = 4;
	Rect inner = pad2(r, 24, 0);
	int cols = MAX(2, (int)((inner.w + gap) / (cell + gap)));
	cell = (inner.w - gap * (f32)(cols - 1)) / (f32)cols;
	f32 y = ui_scroll_begin(ui, r, "library") + 10;
	f64 budget = (f64)time_mono_ns() / 1e9 + 0.015;
	int last_month = -1, col = 0;
	Rect view = r;
	for (int i = 0; i < ph.n; i++) {
		Photo *p = &ph.photos[i];
		Civil c;
		civil_local(p->mtime, &c);
		int month = c.year * 12 + c.month;
		if (month != last_month) {
			if (col)
				y += cell + gap;
			col = 0;
			y += last_month < 0 ? 4 : 18;
			char title[48];
			fmt(title, sizeof title, "%s %d", MONTH_NAMES[c.month - 1], c.year);
			ui_text(ui, ui_font(ui, 20, 600), inner.x, y + 22, title, t->text);
			y += 38;
			last_month = month;
		}
		Rect cr = rect(inner.x + (f32)col * (cell + gap), y, cell, cell);
		/* Decode thumbnails only for cells on screen, a few per frame. */
		bool on_screen = cr.y + cr.h + ui->oy >= view.y - cell && cr.y + ui->oy <= view.y + view.h + cell;
		if (on_screen && !p->thumb && !p->thumb_tried) {
			if ((f64)time_mono_ns() / 1e9 < budget) {
				p->thumb_tried = true;
				p->thumb = image_load_thumb(p->path, 300);
			} else {
				ui->redraw = true;
			}
		}
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "p"), cr);
		ui_pop_id(ui);
		if (p->thumb)
			ui_image(ui, cr, p->thumb, 4, true);
		else
			ui_round(ui, cr, 4, t->dark ? color_hex("#ffffff10") : color_hex("#0000000c"));
		if (it.hovered)
			ui_round(ui, cr, 4, color_hex("#ffffff28"));
		if (it.clicked)
			open_photo(i);
		if (++col == cols) {
			col = 0;
			y += cell + gap;
		}
	}
	if (col)
		y += cell + gap;
	ui_scroll_end(ui, y - r.y + 24);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	bool viewing = ph.current >= 0 && ph.current < ph.n;
	char title[256];
	if (viewing)
		str_copy(title, path_base(ph.photos[ph.current].path), sizeof title);
	else
		fmt(title, sizeof title, "Photos");
	Rect tools = ui_headerbar(ui, header, title, 0);
	if (viewing) {
		if (!ph.from_file && (ui_button_icon_label(ui, center_in(cut_left(&tools, 100), 96, 30), "go-back", "Library") ||
				      ui_key_pressed(ui, KEY_ESC, 0))) {
			ph.current = -1;
			unload();
		}
		if (ph.current >= 0) {
			if (ui_icon_button(ui, cut_right(&tools, 34), "trash", "Move to Trash (Delete)") || ui_key_pressed(ui, KEY_DELETE, 0))
				delete_current(ui);
		}
		if (ph.current >= 0) {
			if (ui_icon_toggle(ui, cut_right(&tools, 34), "info", ph.show_info, "Details (I)") || ui_key_pressed(ui, KEY_I, 0))
				ph.show_info = !ph.show_info;
			if (ui_icon_button(ui, cut_right(&tools, 34), "wallpaper", "Set as Wallpaper"))
				set_wallpaper(ui);
			if (ui_icon_button(ui, cut_right(&tools, 34), "refresh", "Rotate (R)") || ui_key_pressed(ui, KEY_R, 0)) {
				ph.rotation = (ph.rotation + 1) % 4;
				Canvas *rf = rotate90(ph.full);
				canvas_free(ph.full);
				ph.full = rf;
				if (ph.fit) {
					Canvas *rt = rotate90(ph.fit);
					canvas_free(ph.fit);
					ph.fit = rt;
				}
			}
			cut_right(&tools, 6);
			if (ui_icon_button(ui, cut_right(&tools, 34), "zoom-in", "Zoom In (+)") || ui_key_pressed(ui, KEY_EQUAL, 0))
				ph.zoom = clampf((ph.zoom > 0 ? ph.zoom : fit_scale(pad(win, 20))) * 1.25f, 0.05f, 8);
			if (ui_icon_button(ui, cut_right(&tools, 34), "zoom-out", "Zoom Out (-)") || ui_key_pressed(ui, KEY_MINUS, 0))
				ph.zoom = clampf((ph.zoom > 0 ? ph.zoom : fit_scale(pad(win, 20))) / 1.25f, 0.05f, 8);
			if (ui_key_pressed(ui, KEY_0, 0)) {
				ph.zoom = 0;
				ph.pan_x = ph.pan_y = 0;
			}
			if (ph.n > 1 && (ui_key_pressed(ui, KEY_RIGHT, 0) || ui_key_pressed(ui, KEY_SPACE, 0)))
				open_photo((ph.current + 1) % ph.n);
			if (ph.n > 1 && ui_key_pressed(ui, KEY_LEFT, 0))
				open_photo((ph.current + ph.n - 1) % ph.n);
			if (ph.show_info)
				info_panel(ui, cut_right(&win, 260));
			viewer(ui, win);
		}
	} else {
		char count[48];
		fmt(count, sizeof count, ph.n == 1 ? "1 picture" : "%d pictures", ph.n);
		ui_text_in(ui, ui_font(ui, 12, 500), cut_right(&tools, 120), count, t->text_dim, ALIGN_CENTER);
		library(ui, win);
	}
}

static const char *start_file;

static void init(Ui *ui)
{
	UNUSED(ui);
	ph.current = -1;
	if (start_file && file_exists(start_file)) {
		ph.from_file = true;
		load_folder_of(start_file);
		open_photo(ph.current);
	} else {
		load_library();
	}
}

int main(int argc, char **argv)
{
	if (argc > 1)
		start_file = argv[1];
	UiAppDesc d = { .id = "photos", .title = "Photos", .width = 1000, .height = 680, .min_width = 560, .min_height = 400,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
