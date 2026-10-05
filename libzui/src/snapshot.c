/* Offscreen rendering of an app's frame, for tests and previews. */
#include "zui.h"
#include "zui/driver.h"

bool ui_snapshot(const UiAppDesc *desc, int w, int h, f32 scale, bool dark, const char *png_path)
{
	Ui *ui = ui_new();
	ThemeInfo info = { .dark = dark, .text_scale = 1 };
	const char *accent = env_get("ZUI_ACCENT");
	if (accent)
		info.accent = theme_accent_named(accent);
	ui_set_theme(ui, &info);
	ui->user = desc->user;
	ui->app_id = desc->id;
	ui->win_states = STATE_ACTIVE;
	ui->window_focused = true;
	str_copy(ui->title, desc->title ? desc->title : desc->id, sizeof ui->title);
	Canvas *c = canvas_new((int)roundf((f32)w * scale), (int)roundf((f32)h * scale));
	ui_attach(ui, c, (f32)w, (f32)h, scale);
	if (desc->init)
		desc->init(ui);
	/* A few frames let layout that depends on the previous frame settle. */
	for (int i = 0; i < 3; i++) {
		memset(c->pixels, 0, (usize)c->stride * (usize)c->height * 4);
		ui_frame(ui, desc->frame);
	}
	bool ok = png_save(c, png_path);
	canvas_free(c);
	ui_free(ui);
	return ok;
}
