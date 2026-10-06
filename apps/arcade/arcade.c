/*
 * Arcade — games that come with Zurbax.
 *
 * A wall of game cards with their best scores; each game runs in the same
 * window with a bar to go back, pause or start over. Everything is drawn
 * by the toolkit and every sound is synthesized.
 */
#include "arcade.h"

static const Game *const GAMES[] = { &GAME_BLOCKS, &GAME_SNAKE, &GAME_2048, &GAME_MINES, &GAME_BREAKOUT };

static const Game *playing;
bool game_paused;
static Ui *the_ui;
static Conf *scores;
static char scores_path[512];

/* ---- Shared helpers --------------------------------------------------------------------------- */

int best_score(const char *id)
{
	return scores ? (int)conf_get_int(scores, id, 0) : 0;
}

bool submit_score(const char *id, int score)
{
	if (!scores || score <= best_score(id))
		return false;
	conf_set_int(scores, id, score);
	conf_save(scores);
	return true;
}

bool key_down(u32 code)
{
	return the_ui && ui_key_down(the_ui, code);
}

bool key_hit(Ui *ui, u32 code)
{
	return ui_key_pressed(ui, code, 0);
}

bool game_overlay(Ui *ui, Rect r, const char *title, const char *sub, const char *button)
{
	ui_round(ui, r, 12, color_hex("#0b0d14c0"));
	Rect c = center_in(r, minf(r.w - 40, 360), 170);
	ui_text_in(ui, ui_font(ui, 28, 800), cut_top(&c, 42), title, COLOR_WHITE, ALIGN_CENTER);
	ui_text_wrapped(ui, ui_font(ui, 14, 500), cut_top(&c, 50), sub, color_hex("#ffffffb0"), ALIGN_CENTER);
	cut_top(&c, 12);
	bool go = ui_button_suggested(ui, center_in(cut_top(&c, 40), 180, 40), button);
	return go || key_hit(ui, KEY_ENTER) || key_hit(ui, KEY_SPACE);
}

void stat_box(Ui *ui, Rect r, const char *label, const char *value)
{
	ui_round(ui, r, 10, color_hex("#ffffff0f"));
	ui_text_in(ui, ui_font(ui, 11, 700), rect(r.x + 14, r.y + 8, r.w - 28, 16), label, color_hex("#ffffff80"), ALIGN_START);
	ui_text_in(ui, ui_font(ui, 22, 700), rect(r.x + 14, r.y + 24, r.w - 28, 30), value, COLOR_WHITE, ALIGN_START);
}

void bevel_cell(Ui *ui, Rect r, f32 radius, Color c)
{
	ui_round(ui, r, radius, color_lighten(c, -0.28f));
	Rect in = rect(r.x, r.y, r.w, r.h - maxf(1.5f, r.h * 0.1f));
	ui_gradient(ui, in, radius, color_lighten(c, 0.22f), c);
	ui_round(ui, rect(in.x + in.w * 0.18f, in.y + in.h * 0.12f, in.w * 0.5f, maxf(1.5f, in.h * 0.12f)), in.h * 0.06f, color_hex("#ffffff55"));
}

/* ---- Hub ------------------------------------------------------------------------------------- */

static void hub(Ui *ui, Rect r)
{
	Rect area = pad4(r, 26, 32, 26, 32);
	ui_text_in(ui, ui_font(ui, 28, 800), cut_top(&area, 40), "Arcade", COLOR_WHITE, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 14, 500), cut_top(&area, 22), "Pick a game. Your best scores are kept.", color_hex("#ffffff90"), ALIGN_START);
	cut_top(&area, 22);
	int n = (int)ARRAY_LEN(GAMES);
	int cols = area.w > 900 ? 3 : 2;
	f32 gap = 20, cw = (area.w - gap * (f32)(cols - 1)) / (f32)cols, ch = minf(250, cw * 0.72f);
	f32 y = ui_scroll_begin(ui, area, "hub");
	for (int i = 0; i < n; i++) {
		const Game *g = GAMES[i];
		Rect c = rect(area.x + (f32)(i % cols) * (cw + gap), y + (f32)(i / cols) * (ch + gap), cw, ch);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "card"), c);
		f32 h = ui_fade(ui, ui_id(ui, "card"), it.hovered, 0.15f);
		Rect card = rect(c.x, c.y - h * 4, c.w, c.h);
		ui_shadow(ui, card, 16, 22 + h * 10, 8, color_hex("#00000070"));
		ui_gradient(ui, card, 16, g->top, g->bottom);
		Rect art = pad4(card, 14, 14, 76, 14);
		ui_push_clip(ui, art);
		g->preview(ui, art);
		ui_pop_clip(ui);
		Rect info = rect(card.x + 18, card.y + card.h - 66, card.w - 36, 56);
		ui_text_in(ui, ui_font(ui, 20, 800), rect(info.x, info.y, info.w, 26), g->name, COLOR_WHITE, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(info.x, info.y + 28, info.w, 18), g->tagline, color_hex("#ffffffb8"), ALIGN_START);
		int best = best_score(g->id);
		if (best) {
			char b[32];
			fmt(b, sizeof b, "Best %d", best);
			Font f = ui_font(ui, 12, 700);
			f32 w = ui_text_width(ui, f, b) + 20;
			Rect pill = rect(info.x + info.w - w, info.y + 4, w, 22);
			ui_round(ui, pill, 11, color_hex("#00000040"));
			ui_text_in(ui, f, pill, b, COLOR_WHITE, ALIGN_CENTER);
		}
		if (it.clicked) {
			playing = g;
			game_paused = false;
			g->start();
			sound_play("click");
		}
		ui_pop_id(ui);
	}
	ui_scroll_end(ui, (f32)((n + cols - 1) / cols) * (ch + gap));
}

static void game_bar(Ui *ui, Rect r)
{
	Rect b = pad2(r, 16, 8);
	if (ui_button_icon_label(ui, cut_left(&b, 110), "go-back", "Games")) {
		playing = NULL;
		return;
	}
	/* Escape pauses; a second Escape goes back to the games. */
	if (key_hit(ui, KEY_ESC)) {
		if (game_paused) {
			playing = NULL;
			return;
		}
		game_paused = true;
	}
	ui_text_in(ui, ui_font(ui, 16, 800), center_in(b, 300, b.h), playing->name, COLOR_WHITE, ALIGN_CENTER);
	if (ui_button(ui, cut_right(&b, 110), "New Game")) {
		playing->start();
		game_paused = false;
	}
	cut_right(&b, 8);
	if (ui_button(ui, cut_right(&b, 90), game_paused ? "Resume" : "Pause") || key_hit(ui, KEY_P))
		game_paused = !game_paused;
	ui_text_in(ui, ui_font(ui, 12, 500), rect(b.x + 8, b.y, b.w - 16, b.h), playing->controls, color_hex("#ffffff70"), ALIGN_START);
}

static void frame(Ui *ui)
{
	the_ui = ui;
	ui_window_background(ui, color_hex("#12141c"));
	Rect win = rect(0, 0, ui->width, ui->height);
	ui_gradient(ui, win, 0, color_hex("#1b1e2b"), color_hex("#0e1016"));
	Theme saved = ui->theme;
	ui->theme.text = ui->theme.text_dim = COLOR_WHITE;
	Rect header = cut_top(&win, 46);
	ui_headerbar(ui, header, "", HEADER_TRANSPARENT | HEADER_FLAT | HEADER_NO_TITLE);
	ui->theme = saved;
	if (!playing) {
		hub(ui, win);
		return;
	}
	game_bar(ui, cut_top(&win, 50));
	if (!playing)
		return;
	if (!ui->window_focused && !game_paused)
		game_paused = true; /* stepping away pauses */
	playing->frame(ui, pad4(win, 6, 20, 20, 20));
	ui->redraw = true;
}

static void init(Ui *ui)
{
	UNUSED(ui);
	fmt(scores_path, sizeof scores_path, "%s/.local/share/zurbax/arcade.conf", home_dir());
	char *d = path_dir(scores_path);
	dir_make(d, 0755);
	mem_free(d);
	scores = conf_load(scores_path);
}

int main(void)
{
	UiAppDesc d = { .id = "arcade", .title = "Arcade", .width = 1000, .height = 760, .min_width = 760, .min_height = 640, .flags = WIN_RESIZABLE,
			.init = init, .frame = frame };
	return ui_run(&d);
}
