/*
 * Screenshot — capture the screen, a window or an area.
 *
 * The window steps out of the way while the picture is taken, then comes
 * back with a preview to save, copy or open. Print Screen does a quick
 * full-screen capture without this app.
 */
#include "zui.h"

typedef struct {
	int mode;            /* SCREENSHOT_* */
	int delay;
	bool waiting;        /* hidden, counting down */
	f64 shoot_at;
	bool requested;
	Canvas *shot;
	char saved_path[1024];
	char message[160];
	f64 message_until;
} Shot;

static Shot sh;

static void flash(Ui *ui, const char *m)
{
	str_copy(sh.message, m, sizeof sh.message);
	sh.message_until = ui->now + 3;
	ui->wake_at = sh.message_until;
}

static bool save_png(Ui *ui, const char *path)
{
	if (!sh.shot)
		return false;
	char *dir = path_dir(path);
	dir_make(dir, 0755);
	mem_free(dir);
	if (!png_save(sh.shot, path)) {
		flash(ui, "The picture couldn't be saved there.");
		return false;
	}
	str_copy(sh.saved_path, path, sizeof sh.saved_path);
	char m[300];
	fmt(m, sizeof m, "Saved to %s", path_base(path));
	flash(ui, m);
	return true;
}

static void default_path(char *out, usize cap)
{
	Civil c;
	civil_local(time_unix(), &c);
	fmt(out, cap, "%s/Pictures/Screenshots/Screenshot %04d-%02d-%02d %02d.%02d.%02d.png", home_dir(), c.year, c.month, c.day, c.hour,
	    c.min, c.sec);
}

static void take(Ui *ui)
{
	sh.waiting = true;
	sh.requested = false;
	/* Leave time for the window to get out of the way. */
	sh.shoot_at = ui->now + 0.35 + sh.delay;
	ui_window_action(ui, ACTION_MINIMIZE);
	ui->wake_at = sh.shoot_at;
}

static bool mode_card(Ui *ui, Rect r, const char *icon, const char *label, bool selected)
{
	Theme *t = &ui->theme;
	UiInteract it = ui_interact(ui, ui_id(ui, label), r);
	f32 hover = ui_fade(ui, ui_id(ui, label), it.hovered, 0.12f);
	ui_round(ui, r, 10, selected ? color_with_alpha(t->accent, 0x26) : color_mix(t->card, t->hover, hover));
	ui_border(ui, r, 10, selected ? 2 : 1, selected ? t->accent : t->border);
	ui_icon(ui, rect(r.x + (r.w - 36) / 2, r.y + 18, 36, 36), icon, selected ? t->accent : t->text_dim);
	ui_text_in(ui, ui_font(ui, 13, selected ? 700 : 500), rect(r.x, r.y + r.h - 34, r.w, 22), label, t->text, ALIGN_CENTER);
	return it.clicked;
}

static void chooser(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect b = pad(r, 20);
	Rect cards = cut_top(&b, 110);
	static const char *const icons[] = { "display", "window-restore", "selection-mode" };
	static const char *const labels[] = { "Entire Screen", "Window", "Area" };
	f32 cw = (cards.w - 24) / 3;
	for (int i = 0; i < 3; i++) {
		ui_push_id(ui, i);
		if (mode_card(ui, rect(cards.x + (f32)i * (cw + 12), cards.y, cw, cards.h), icons[i], labels[i], sh.mode == i))
			sh.mode = i;
		ui_pop_id(ui);
	}
	cut_top(&b, 18);
	Rect row = cut_top(&b, 34);
	ui_label(ui, cut_left(&row, 140), "Delay in seconds");
	ui_spin(ui, cut_left(&row, 110), "delay", &sh.delay, 0, 15);
	Rect bottom = cut_bottom(&b, 36);
	if (ui_button_suggested(ui, cut_right(&bottom, 170), "Take Screenshot") || ui_key_pressed(ui, KEY_ENTER, 0))
		take(ui);
	ui_text(ui, ui_font(ui, 12, 400), bottom.x, bottom.y + 23, "Tip: Print Screen saves the whole screen at once.", t->text_faint);
}

static void preview(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect bottom = cut_bottom(&r, 64);
	ui_fill(ui, r, t->dark ? color_hex("#1a1b1e") : color_hex("#dfe1e5"));
	Rect pic = pad(r, 18);
	f32 s = minf(pic.w / (f32)sh.shot->width, pic.h / (f32)sh.shot->height);
	s = minf(s, 1);
	Rect ir = center_in(pic, (f32)sh.shot->width * s, (f32)sh.shot->height * s);
	ui_shadow(ui, ir, 4, 18, 5, color_hex("#00000060"));
	ui_image(ui, ir, sh.shot, 4, false);
	ui_separator(ui, bottom.x, bottom.y, bottom.x + bottom.w);
	Rect bb = pad2(bottom, 16, 14);
	if (ui_button_suggested(ui, cut_right(&bb, 100), "Save")) {
		char path[1024];
		default_path(path, sizeof path);
		save_png(ui, path);
	}
	cut_right(&bb, 8);
	if (ui_button(ui, cut_right(&bb, 100), "Save As…")) {
		char *dir = str_fmt("%s/Pictures/Screenshots", home_dir());
		dir_make(dir, 0755);
		char name[96];
		Civil c;
		civil_local(time_unix(), &c);
		fmt(name, sizeof name, "Screenshot %04d-%02d-%02d %02d.%02d.%02d.png", c.year, c.month, c.day, c.hour, c.min, c.sec);
		ui_file_dialog_open(ui, "save", FILE_SAVE, dir, NULL, name);
		mem_free(dir);
	}
	cut_right(&bb, 8);
	if (ui_button_icon_label(ui, cut_right(&bb, 100), "edit-copy", "Copy")) {
		usize len;
		u8 *png = png_encode(sh.shot, &len);
		if (png) {
			ui_clipboard_set_data(ui, "image/png", png, len);
			mem_free(png);
			flash(ui, "Copied to the clipboard");
		}
	}
	cut_right(&bb, 8);
	if (ui_button(ui, cut_right(&bb, 90), "Open")) {
		if (!sh.saved_path[0]) {
			char path[1024];
			default_path(path, sizeof path);
			save_png(ui, path);
		}
		if (sh.saved_path[0])
			ui_launch(ui, "photos", sh.saved_path);
	}
	if (ui_button_flat(ui, cut_left(&bb, 150), "New Screenshot")) {
		canvas_free(sh.shot);
		sh.shot = NULL;
		sh.saved_path[0] = 0;
	}
	if (ui->now < sh.message_until) {
		Font f = ui_font(ui, 13, 600);
		f32 w = ui_text_width(ui, f, sh.message) + 30;
		Rect m = rect(r.x + (r.w - w) / 2, r.y + 14, w, 32);
		ui_round(ui, m, 16, color_hex("#000000c0"));
		ui_text_in(ui, f, m, sh.message, COLOR_WHITE, ALIGN_CENTER);
	}
	char path[1024];
	if (ui_file_dialog(ui, "save", path, sizeof path))
		save_png(ui, path);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	/* The countdown runs while the window is hidden. */
	if (sh.waiting && !sh.requested && ui->now >= sh.shoot_at) {
		sh.requested = true;
		ui_screenshot_request(ui, (u32)sh.mode);
	} else if (sh.waiting && !sh.requested) {
		ui->wake_at = sh.shoot_at;
	}
	if (ui->screenshot_ready) {
		ui->screenshot_ready = false;
		sh.waiting = false;
		if (ui->screenshot) {
			canvas_free(sh.shot);
			sh.shot = ui->screenshot;
			ui->screenshot = NULL;
			sh.saved_path[0] = 0;
			/* Big captures get a bigger window to preview them in. */
			if (ui->width < 760)
				ui_window_resize(ui, 820, 600);
		}
		ui_window_action(ui, ACTION_RAISE);
	}
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	ui_headerbar(ui, header, sh.shot ? "Screenshot" : "Take a Screenshot", sh.shot ? 0 : HEADER_NO_MAXIMIZE);
	if (sh.shot)
		preview(ui, win);
	else
		chooser(ui, win);
}

int main(int argc, char **argv)
{
	if (argc > 1 && str_eq(argv[1], "--area"))
		sh.mode = SCREENSHOT_AREA;
	UiAppDesc d = { .id = "screenshot", .title = "Screenshot", .width = 520, .height = 330, .min_width = 460, .min_height = 300,
			.flags = WIN_RESIZABLE, .frame = frame };
	return ui_run(&d);
}
