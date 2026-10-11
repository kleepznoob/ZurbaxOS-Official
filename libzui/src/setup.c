/*
 * The look the setup assistants share (Welcome on the first start, the
 * Installer from the stick): the steps down a sidebar laid over the
 * wallpaper, with a line that fills as they're done, and the page on the
 * right, which slides in from the side it comes from.
 */
#include "zui.h"

static struct {
	Canvas *img;
	char name[64];
	int w, h;
} side;

/* The wallpaper, blurred and darkened, sized to the sidebar. */
static void sidebar_backdrop(Ui *ui, Rect r, const char *wallpaper)
{
	int w = MAX(1, (int)(r.w * ui->scale)), h = MAX(1, (int)(r.h * ui->scale));
	if (!side.img || side.w != w || side.h != h || !str_eq(side.name, wallpaper)) {
		canvas_free(side.img);
		/* rendered small and blurred, then stretched: soft, and quick */
		int sw = MAX(1, w / 5), sh = MAX(1, h / 5);
		Canvas *full = wallpaper_render(wallpaper, sh * 16 / 9 + 1, sh);
		Canvas *small = canvas_new(sw, sh);
		/* the left part of the picture, where its colour is */
		blit_scaled(small, full, rect(0, 0, (f32)MIN(full->width, sw), (f32)sh), rect(0, 0, (f32)sw, (f32)sh), 1);
		canvas_free(full);
		blur_region(small, rect(0, 0, (f32)sw, (f32)sh), 4);
		side.img = canvas_new(w, h);
		blit_scaled(side.img, small, rect(0, 0, (f32)sw, (f32)sh), rect(0, 0, (f32)w, (f32)h), 1);
		canvas_free(small);
		side.w = w, side.h = h;
		str_copy(side.name, wallpaper, sizeof side.name);
	}
	ui_image(ui, r, side.img, 0, true);
	ui_gradient(ui, r, 0, color_hex("#0b0a0d9a"), color_hex("#0b0a0dd8"));
}

f32 setup_ease(f32 t)
{
	return ease_out_cubic(clampf(t, 0, 1));
}

Rect setup_frame(Ui *ui, const SetupSteps *s)
{
	Theme *t = &ui->theme;
	Rect win = rect(0, 0, ui->width, ui->height);
	ui_fill(ui, win, t->window);
	f32 sw = clampf(ui->width * 0.27f, 240, 300);
	Rect sb = cut_left(&win, sw);
	sidebar_backdrop(ui, sb, s->wallpaper ? s->wallpaper : WALLPAPER_DEFAULT);

	/* who's asking */
	Rect head = pad4(cut_top(&sb, 116), 34, 24, 0, 26);
	ui_app_icon(ui, rect(head.x, head.y, 44, 44), "zurbax");
	ui_text_in(ui, ui_font(ui, 17, 700), rect(head.x + 56, head.y + 2, head.w - 56, 22), OS_NAME " " OS_VERSION, COLOR_WHITE, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 13, 500), rect(head.x + 56, head.y + 24, head.w - 56, 18), s->title, color_hex("#ffffffa0"), ALIGN_START);

	/* the steps */
	f32 done = ui_anim(ui, ui_id(ui, "setup-progress"), (f32)s->current, 7);
	if (fabsf(done - (f32)s->current) > 0.002f)
		ui->redraw = true;
	const f32 row = 54, cx_off = 40, rad = 15;
	f32 top = sb.y + 10;
	f32 hl = ui_anim(ui, ui_id(ui, "setup-highlight"), top + (f32)MIN(s->current, s->count - 1) * row, 14);
	ui_round(ui, rect(sb.x + 14, hl + 4, sb.w - 28, row - 8), 12, color_hex("#ffffff14"));
	for (int i = 0; i < s->count; i++) {
		f32 y = top + (f32)i * row, cx = sb.x + cx_off, cy = y + row / 2;
		/* the line down to the next step, filled as far as the steps are done */
		if (i < s->count - 1) {
			f32 y0 = cy + rad + 4, y1 = cy + row - rad - 4;
			ui_line(ui, cx, y0, cx, y1, 2, color_hex("#ffffff30"));
			f32 f = clampf(done - (f32)i, 0, 1);
			if (f > 0)
				ui_line(ui, cx, y0, cx, y0 + (y1 - y0) * f, 2, t->accent);
		}
		f32 state = clampf(done - (f32)i + 1, 0, 2); /* <1 to come, 1 now, 2 done */
		bool is_done = i < s->current, now = i == s->current;
		char num[8];
		fmt(num, sizeof num, "%d", i + 1);
		if (is_done) {
			f32 k = clampf(state - 1, 0, 1);
			ui_circle(ui, cx, cy, rad * (0.6f + 0.4f * k), t->accent);
			ui_icon(ui, rect(cx - 8, cy - 8, 16, 16), "check", COLOR_WHITE);
		} else if (now) {
			/* the current step breathes */
			f32 pulse = 0.5f + 0.5f * sinf((f32)ui->now * 3.2f);
			if (!t->reduce_motion)
				ui_ring(ui, cx, cy, rad + 3 + pulse * 3, 2, color_with_alpha(t->accent, (u8)(0x70 * (1 - pulse))));
			ui_circle(ui, cx, cy, rad, color_with_alpha(t->accent, 0x40));
			ui_ring(ui, cx, cy, rad, 2, t->accent);
			ui_text_in(ui, ui_font(ui, 13, 700), rect(cx - rad, cy - rad, rad * 2, rad * 2), num, COLOR_WHITE, ALIGN_CENTER);
		} else {
			ui_ring(ui, cx, cy, rad, 1.5f, color_hex("#ffffff40"));
			ui_text_in(ui, ui_font(ui, 13, 600), rect(cx - rad, cy - rad, rad * 2, rad * 2), num, color_hex("#ffffff80"), ALIGN_CENTER);
		}
		Rect label = rect(cx + rad + 16, y, sb.x + sb.w - (cx + rad + 16) - 16, row);
		Color lc = now ? COLOR_WHITE : is_done ? color_hex("#ffffffcc") : color_hex("#ffffff80");
		ui_text_in(ui, ui_font(ui, 15, now ? 700 : 500), label, s->steps[i], lc, ALIGN_START);
	}
	if (s->current < s->count && !t->reduce_motion)
		ui->wake_at = ui->now + 0.05;

	/* how far along */
	Rect foot = pad4(cut_bottom(&sb, 70), 0, 26, 28, 26);
	char where[48];
	if (s->current < s->count)
		fmt(where, sizeof where, "Step %d of %d", s->current + 1, s->count);
	else
		str_copy(where, "All done", sizeof where);
	ui_text_in(ui, ui_font(ui, 12, 600), cut_top(&foot, 18), where, color_hex("#ffffffa0"), ALIGN_START);
	cut_top(&foot, 8);
	Rect bar = cut_top(&foot, 4);
	ui_round(ui, bar, 2, color_hex("#ffffff26"));
	ui_round(ui, rect(bar.x, bar.y, bar.w * clampf(done / (f32)MAX(1, s->count - 1), 0, 1), bar.h), 2, t->accent);

	/* the page, sliding in from the side it comes from */
	f32 k = setup_ease((f32)((ui->now - s->changed_at) / 0.42));
	if (k < 1)
		ui->redraw = true;
	Rect page = pad4(win, 44, 52, 28, 52);
	page.x += (1 - k) * 48 * (f32)(s->dir ? s->dir : 1);
	return page;
}

/* The page's title and what it's about; each line arrives a moment after
 * the one before. Returns how far the page's entrance is (0..1). */
f32 setup_title(Ui *ui, Rect *r, f64 changed_at, const char *title, const char *about)
{
	Theme *t = &ui->theme;
	f32 e = (f32)(ui->now - changed_at);
	f32 a = setup_ease(e / 0.45f), b = setup_ease((e - 0.06f) / 0.45f);
	Font tf = ui_font(ui, 30, 800), af = ui_font(ui, 15, 400);
	Rect tr = cut_top(r, 42);
	tr.y += (1 - a) * 14;
	ui_text_in(ui, tf, tr, title, t->text, ALIGN_START);
	cut_top(r, 6);
	f32 h = about ? ui_text_height(ui, af, about, minf(r->w, 620)) : 0;
	Rect ar = cut_top(r, h);
	ar.y += (1 - b) * 18;
	ar.w = minf(ar.w, 620);
	if (about)
		ui_text_wrapped(ui, af, ar, about, t->text_dim, ALIGN_START);
	cut_top(r, 26);
	return setup_ease(e / 0.6f);
}

/* An item's entrance in a list that arrives one after another. */
Rect setup_rise(Ui *ui, f64 changed_at, int index, Rect r)
{
	f32 k = setup_ease((f32)((ui->now - changed_at - 0.08 - 0.06 * index) / 0.5));
	if (k < 1)
		ui->redraw = true;
	r.y += (1 - k) * 22;
	return r;
}

/* A choice made big: an icon in a tinted circle, a title, a sentence. */
bool setup_choice(Ui *ui, Rect r, const char *id, const char *icon, const char *title, const char *text, bool selected, bool enabled,
		  Color tint)
{
	Theme *t = &ui->theme;
	UiInteract it = ui_interact(ui, ui_id(ui, id), r);
	f32 hover = ui_fade(ui, ui_id(ui, id), it.hovered && enabled, 0.15f);
	Rect lift = r;
	lift.y -= hover * 2;
	if (enabled)
		ui_shadow(ui, lift, 14, 18 + hover * 10, 6 + hover * 4, color_with_alpha(t->shadow, (u8)(0x30 + hover * 0x30)));
	ui_round(ui, lift, 14, selected ? color_mix(t->card, t->accent, t->dark ? 0.18f : 0.08f) : t->card);
	ui_border(ui, lift, 14, selected ? 2 : 1, selected ? t->accent : color_mix(t->border, t->accent, hover * 0.6f));
	Rect c = pad2(lift, 20, 18);
	Rect ic = cut_top(&c, 48);
	ui_circle(ui, ic.x + 24, ic.y + 24, 24, color_with_alpha(enabled ? tint : t->text_faint, 0x26));
	ui_icon(ui, rect(ic.x + 12, ic.y + 12, 24, 24), icon, enabled ? tint : t->text_faint);
	if (selected)
		ui_icon(ui, rect(lift.x + lift.w - 34, lift.y + 16, 18, 18), "check", t->accent);
	cut_top(&c, 14);
	ui_text_in(ui, ui_font(ui, 17, 700), cut_top(&c, 24), title, enabled ? t->text : t->text_faint, ALIGN_START);
	cut_top(&c, 4);
	ui_text_wrapped(ui, ui_font(ui, 13, 400), c, text, enabled ? t->text_dim : t->text_faint, ALIGN_START);
	return it.clicked && enabled;
}

/* A large tick that draws itself: a disc that grows, and a ring that
 * spreads and fades. */
void setup_tick(Ui *ui, Rect r, f64 since, bool ok)
{
	Theme *t = &ui->theme;
	Color c = ok ? t->success : t->danger;
	f32 e = (f32)(ui->now - since);
	f32 cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = minf(r.w, r.h) / 2;
	f32 grow = setup_ease(e / 0.5f);
	f32 burst = clampf((e - 0.2f) / 0.8f, 0, 1);
	if (burst > 0 && burst < 1)
		ui_ring(ui, cx, cy, rad * (1 + burst * 0.6f), 3 * (1 - burst), color_with_alpha(c, (u8)(0xa0 * (1 - burst))));
	ui_circle(ui, cx, cy, rad * grow, color_with_alpha(c, 0x2a));
	ui_circle(ui, cx, cy, rad * 0.72f * grow, c);
	f32 tick = setup_ease((e - 0.25f) / 0.35f);
	if (tick > 0) {
		f32 s = rad * 0.7f * tick;
		ui_icon(ui, rect(cx - s / 2, cy - s / 2, s, s), ok ? "check" : "warning", COLOR_WHITE);
	}
	if (e < 1.2f)
		ui->redraw = true;
}
