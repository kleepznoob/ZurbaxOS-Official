/*
 * Drag and drop of files between windows, the desktop and the taskbar.
 *
 * An app (or the desktop) starts a drag while the left button is held; from
 * then on the display server carries the files: a picture of them follows
 * the pointer, the window underneath hears that files are over it, and
 * where they're let go decides what happens: a window gets them, the desktop
 * moves or copies them into ~/Desktop, an app's taskbar button opens them in
 * that app. Holding them over a running app's button brings its window up.
 * Let go anywhere else, or pressing Esc, sends the picture back home.
 */
#include "zdisplay.h"

enum { T_NONE, T_WINDOW, T_APP, T_DESKTOP };

static struct {
	bool active;
	bool returning;            /* flying back after a cancelled drag */
	char *paths;
	int count;
	bool from_desktop;
	Window *over;              /* the window the files are over */
	Canvas *img;               /* the picture under the pointer */
	int hot_x, hot_y;          /* the pointer's place in it, physical pixels */
	Rect drawn;                /* where it was last drawn, physical pixels */
	f32 x0, y0;                /* where the drag began */
	f32 rx, ry;                /* where it was let go */
	f64 t0;
	f64 hover_since;           /* over dnd_hover_app since */
	bool hover_raised;
} D;

char dnd_hover_app[48];

bool dnd_active(void)
{
	return D.active;
}

bool dnd_animating(void)
{
	return D.returning;
}

/* ---- The picture ------------------------------------------------------------------ */

static const char *pic_path;
static const Canvas *pic_thumb;
static int pic_count;

static void draw_picture(Ui *ui)
{
	f32 w = ui->width;
	Rect box = rect((w - 76) / 2, 6, 76, 76);
	if (pic_count > 1) {
		/* a stack: sheets behind the first */
		ui_round(ui, rect(box.x + 8, box.y - 2, box.w - 8, box.h - 4), 14, color_hex("#ffffff55"));
		ui_round(ui, rect(box.x + 4, box.y + 1, box.w - 4, box.h - 2), 14, color_hex("#ffffff88"));
	}
	ui_shadow(ui, box, 14, 12, 4, color_hex("#00000066"));
	ui_round(ui, box, 14, color_hex("#ffffffec"));
	Rect in = pad(box, 8);
	if (pic_thumb) {
		f32 s = minf(in.w / (f32)pic_thumb->width, in.h / (f32)pic_thumb->height);
		Rect ir = center_in(in, (f32)pic_thumb->width * s, (f32)pic_thumb->height * s);
		ui_image(ui, ir, pic_thumb, 4, false);
	} else if (dir_exists(pic_path)) {
		ui_app_icon(ui, in, "folder");
	} else {
		ui_icon(ui, center_in(in, 34, 34), ui_file_icon(pic_path, false), color_hex("#5d6573"));
	}
	/* the name, or how many */
	char label[96];
	if (pic_count > 1)
		fmt(label, sizeof label, "%d items", pic_count);
	else
		str_copy(label, path_base(pic_path), sizeof label);
	Font f = ui_font(ui, 12, 600);
	f32 tw = minf(ui_text_width(ui, f, label) + 20, w - 8);
	Rect pill = rect((w - tw) / 2, box.y + box.h + 8, tw, 24);
	ui_round(ui, pill, 12, color_with_alpha(ui->theme.accent, 0xf0));
	ui_push_clip(ui, pad2(pill, 8, 0));
	ui_text_in(ui, f, pill, label, ui->theme.on_accent, ALIGN_CENTER);
	ui_pop_clip(ui);
	if (pic_count > 1) {
		char n[16];
		fmt(n, sizeof n, "%d", pic_count);
		ui_badge(ui, box.x + box.w - 4, box.y + 4, n);
	}
}

static Canvas *make_picture(const char *first, const Canvas *thumb, int count)
{
	f32 w = 150, h = 122;
	Canvas *c = canvas_new((int)ceilf(w * S.scale), (int)ceilf(h * S.scale));
	pic_path = first;
	pic_thumb = thumb;
	pic_count = count;
	Ui *u = desktop_aux_ui(c, w, h);
	ui_frame(u, draw_picture);
	D.hot_x = (int)(w / 2 * S.scale);
	D.hot_y = (int)(46 * S.scale);
	return c;
}

static Rect picture_rect(f32 x, f32 y)
{
	if (!D.img)
		return rect(0, 0, 0, 0);
	return rect(roundf(x * S.scale) - (f32)D.hot_x, roundf(y * S.scale) - (f32)D.hot_y, (f32)D.img->width, (f32)D.img->height);
}

void dnd_draw(Canvas *frame)
{
	if ((!D.active && !D.returning) || !D.img)
		return;
	f32 x = S.px, y = S.py, alpha = 0.94f;
	if (D.returning) {
		f32 t = clampf((f32)((S.now - D.t0) / 0.28), 0, 1);
		f32 k = ease_in_out_cubic(t);
		x = lerpf(D.rx, D.x0, k);
		y = lerpf(D.ry, D.y0, k);
		alpha *= 1 - t * t;
	}
	Rect r = picture_rect(x, y);
	blit(frame, D.img, (int)r.x, (int)r.y, alpha);
}

/* Repaints where the picture was and where it is now. */
static void picture_moved(void)
{
	damage_physical(D.drawn);
	f32 x = S.px, y = S.py;
	if (D.returning) {
		f32 k = ease_in_out_cubic(clampf((f32)((S.now - D.t0) / 0.28), 0, 1));
		x = lerpf(D.rx, D.x0, k);
		y = lerpf(D.ry, D.y0, k);
	}
	D.drawn = picture_rect(x, y);
	damage_physical(D.drawn);
	request_frame();
}

/* ---- Where the files are ------------------------------------------------------------- */

static int target(Window **win, char *app, usize cap)
{
	*win = NULL;
	app[0] = 0;
	if (S.locked || S.launcher_open)
		return T_NONE;
	if (S.dock_hide_k < 0.5f && S.py >= S.sh - shell_taskbar_height()) {
		char ids[32][48];
		int n = shell_dock_items(ids, 32);
		for (int i = 0; i < n; i++)
			if (rect_contains(shell_dock_slot(n, i), S.px, S.py)) {
				str_copy(app, ids[i], cap);
				return T_APP;
			}
		return T_NONE;
	}
	if (S.py < shell_panel_height())
		return T_NONE;
	u32 edges;
	Window *w = window_at(S.px, S.py, &edges);
	if (w) {
		*win = w;
		return T_WINDOW;
	}
	return T_DESKTOP;
}

static void tell_window(Window *w, u32 kind)
{
	if (w && w->wl) {
		wl_drag_motion(w, D.paths, kind);
		return;
	}
	if (!w || !w->client)
		return;
	MsgDragOver m = { .kind = kind, .count = (u32)D.count, .mods = S.mods, .x = S.px - w->rect.x, .y = S.py - w->rect.y };
	const char *nl = str_chr(D.paths, '\n');
	fmt(m.first, sizeof m.first, "%.*s", (int)(nl ? (usize)(nl - D.paths) : strlen(D.paths)), D.paths);
	server_send(w->client, &m, MSG_DRAG_OVER, sizeof m, w->client_win, NULL, 0);
}

static void set_hover_app(const char *app)
{
	if (str_eq(app, dnd_hover_app))
		return;
	str_copy(dnd_hover_app, app, sizeof dnd_hover_app);
	D.hover_since = S.now;
	D.hover_raised = false;
	shell_mark_dirty();
}

void dnd_motion(void)
{
	if (!D.active)
		return;
	Window *w;
	char app[48];
	int t = target(&w, app, sizeof app);
	if (w != D.over) {
		tell_window(D.over, DRAG_LEAVE);
		D.over = w;
		tell_window(w, DRAG_ENTER);
	} else if (w) {
		tell_window(w, DRAG_MOTION);
	}
	set_hover_app(t == T_APP ? app : "");
	if (t == T_DESKTOP)
		desktop_drag_over(S.px, S.py, D.paths, D.from_desktop);
	else
		desktop_drag_over(-1, -1, NULL, D.from_desktop);
	S.cursor = CURSOR_DEFAULT;
	cursor_update();
	picture_moved();
}

/* Files held over a running app's taskbar button bring its window up. */
void dnd_tick(void)
{
	if (D.returning) {
		picture_moved();
		if (S.now - D.t0 >= 0.3) {
			D.returning = false;
			canvas_free(D.img);
			D.img = NULL;
		}
	}
	if (!D.active || !dnd_hover_app[0] || D.hover_raised || S.now - D.hover_since < 0.7)
		return;
	D.hover_raised = true;
	for (int i = S.nwin - 1; i >= 0; i--) {
		Window *w = S.windows[i];
		if (str_eq(w->app_id, dnd_hover_app) && !w->closing && !(w->flags & WIN_POPUP)) {
			window_minimize(w, false);
			window_raise(w);
			break;
		}
	}
}

/* ---- Start and finish ------------------------------------------------------------------ */

void dnd_start(Window *source, const char *paths, int count, Canvas *thumb)
{
	if (D.active || !(S.buttons & (1u << BUTTON_LEFT)) || !paths || !*paths) {
		canvas_free(thumb);
		return;
	}
	if (D.returning) {
		D.returning = false;
		damage_physical(D.drawn);
	}
	mem_free(D.paths);
	canvas_free(D.img);
	D.paths = str_dup(paths);
	D.count = MAX(count, 1);
	D.from_desktop = source == NULL;
	D.active = true;
	D.over = NULL;
	D.x0 = S.px, D.y0 = S.py;
	char first[1024];
	const char *nl = str_chr(paths, '\n');
	fmt(first, sizeof first, "%.*s", (int)(nl ? (usize)(nl - paths) : strlen(paths)), paths);
	D.img = make_picture(first, thumb, D.count);
	canvas_free(thumb);
	D.drawn = rect(0, 0, 0, 0);
	/* the window it came from lets go of the pointer */
	if (source) {
		if (S.pointer_window)
			server_pointer_event(S.pointer_window, POINTER_LEAVE, 0);
		S.pointer_window = NULL;
	}
	S.grab_window = NULL;
	S.grab = GRAB_NONE;
	dnd_motion();
}

static void send_drop(Window *w)
{
	usize n = strlen(D.paths);
	int fd = (int)sys_memfd_create("drop", MFD_CLOEXEC);
	if (fd < 0)
		return;
	write_all(fd, D.paths, n);
	MsgDropFiles m = { .count = (u32)D.count, .size = (u32)n, .mods = S.mods, .x = S.px - w->rect.x, .y = S.py - w->rect.y };
	server_send(w->client, &m, MSG_DROP_FILES, sizeof m, w->client_win, &fd, 1);
	sys_close(fd);
}

static void finish(bool cancel)
{
	if (!D.active)
		return;
	Window *w = NULL;
	char app[48];
	int t = cancel ? T_NONE : target(&w, app, sizeof app);
	bool done = false;
	if (t == T_WINDOW && w->wl && wl_drag_drop(w)) {
		/* a Linux program takes them through its own drag and drop */
		window_raise(w);
		window_focus(w);
		done = true;
	} else if (t == T_WINDOW && w->client && !w->wl) {
		send_drop(w);
		if (w->minimized)
			window_minimize(w, false);
		window_raise(w);
		window_focus(w);
		done = true;
	} else if (t == T_APP) {
		/* opens in that app: the first file (apps take one at a time) */
		char first[1024];
		const char *nl = str_chr(D.paths, '\n');
		fmt(first, sizeof first, "%.*s", (int)(nl ? (usize)(nl - D.paths) : strlen(D.paths)), D.paths);
		shell_launch(app, first);
		done = true;
	} else if (t == T_DESKTOP) {
		desktop_drop(D.paths, S.px, S.py, S.mods, D.from_desktop);
		done = true;
	}
	if (D.over && !(done && t == T_WINDOW && w == D.over))
		tell_window(D.over, DRAG_LEAVE);
	D.over = NULL;
	desktop_drag_over(-1, -1, NULL, D.from_desktop);
	set_hover_app("");
	D.active = false;
	damage_physical(D.drawn);
	if (done) {
		canvas_free(D.img);
		D.img = NULL;
	} else {
		D.returning = true;
		D.rx = S.px, D.ry = S.py;
		D.t0 = S.now;
	}
	mem_free(D.paths);
	D.paths = NULL;
	request_frame();
	wm_pointer_motion();
}

void dnd_button(u32 button, bool pressed)
{
	if (pressed) {
		if (button == BUTTON_RIGHT)
			finish(true);
		return;
	}
	if (button == BUTTON_LEFT)
		finish(false);
}

bool dnd_key(u32 code, u32 state)
{
	if (!D.active)
		return false;
	if (code == KEY_ESC && state == KEY_PRESSED)
		finish(true);
	/* Ctrl (copy rather than move) shows in the windows' highlights */
	else if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL)
		dnd_motion();
	return true;
}

void dnd_window_gone(Window *w)
{
	if (D.over == w)
		D.over = NULL;
}
