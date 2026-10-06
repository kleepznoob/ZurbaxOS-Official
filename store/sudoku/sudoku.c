/* Sudoku: fresh puzzles with exactly one solution.
 *
 * A puzzle starts as a random full grid; clues are then taken away one at a
 * time, in random order, as long as the solver still finds a single answer. */
#include "zui.h"

enum { EASY, MEDIUM, HARD };

static struct {
	u8 solution[81];
	u8 given[81];            /* 1..9 for clues, 0 for blanks */
	u8 value[81];            /* what you entered */
	u16 notes[81];           /* bit n = pencil mark n */
	int selected;
	bool notes_mode;
	int level;
	int hints, mistakes;
	f64 elapsed;             /* seconds played */
	f64 last_tick;
	bool won;
	char path[512];
	Ui *ui;
} G;

/* ---- Solving and making ------------------------------------------------------------------- */

static bool allowed(const u8 *g, int cell, int v)
{
	int r = cell / 9, c = cell % 9;
	for (int i = 0; i < 9; i++)
		if (g[r * 9 + i] == v || g[i * 9 + c] == v)
			return false;
	int br = r / 3 * 3, bc = c / 3 * 3;
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 3; k++)
			if (g[(br + i) * 9 + bc + k] == v)
				return false;
	return true;
}

/* Counts solutions, stopping at `limit`. Picks the blank with fewest options. */
static int solve(u8 *g, int limit)
{
	int best = -1, best_n = 10;
	for (int i = 0; i < 81; i++) {
		if (g[i])
			continue;
		int n = 0;
		for (int v = 1; v <= 9; v++)
			n += allowed(g, i, v);
		if (n < best_n) {
			best_n = n;
			best = i;
			if (n <= 1)
				break;
		}
	}
	if (best < 0)
		return 1;
	int count = 0;
	for (int v = 1; v <= 9 && count < limit; v++) {
		if (!allowed(g, best, v))
			continue;
		g[best] = (u8)v;
		count += solve(g, limit - count);
		g[best] = 0;
	}
	return count;
}

static bool fill(u8 *g, int cell)
{
	if (cell == 81)
		return true;
	if (g[cell])
		return fill(g, cell + 1);
	u8 order[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	for (int i = 8; i > 0; i--) {
		int j = (int)rand_range(0, (u32)i);
		u8 t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
	for (int i = 0; i < 9; i++) {
		if (!allowed(g, cell, order[i]))
			continue;
		g[cell] = order[i];
		if (fill(g, cell + 1))
			return true;
		g[cell] = 0;
	}
	return false;
}

static void make_puzzle(int level)
{
	static const int target[3] = { 40, 32, 25 };
	memset(G.solution, 0, sizeof G.solution);
	fill(G.solution, 0);
	memcpy(G.given, G.solution, 81);
	int order[81];
	for (int i = 0; i < 81; i++)
		order[i] = i;
	for (int i = 80; i > 0; i--) {
		int j = (int)rand_range(0, (u32)i);
		int t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
	int clues = 81;
	for (int k = 0; k < 81 && clues > target[level]; k++) {
		int cell = order[k];
		u8 keep = G.given[cell];
		G.given[cell] = 0;
		u8 tmp[81];
		memcpy(tmp, G.given, 81);
		if (solve(tmp, 2) != 1)
			G.given[cell] = keep;
		else
			clues--;
	}
}

/* ---- Saving -------------------------------------------------------------------------------- */

static void save(void)
{
	StrBuf sb = { 0 };
	sb_fmt(&sb, "level = %d\nhints = %d\nmistakes = %d\nelapsed = %d\nwon = %d\nselected = %d\n", G.level, G.hints, G.mistakes,
	       (int)G.elapsed, G.won, G.selected);
	sb_adds(&sb, "solution = ");
	for (int i = 0; i < 81; i++)
		sb_addc(&sb, (char)('0' + G.solution[i]));
	sb_adds(&sb, "\ngiven = ");
	for (int i = 0; i < 81; i++)
		sb_addc(&sb, (char)('0' + G.given[i]));
	sb_adds(&sb, "\nvalue = ");
	for (int i = 0; i < 81; i++)
		sb_addc(&sb, (char)('0' + G.value[i]));
	sb_adds(&sb, "\nnotes = ");
	for (int i = 0; i < 81; i++)
		sb_fmt(&sb, "%03x", G.notes[i]);
	sb_addc(&sb, '\n');
	file_write_atomic(G.path, sb.data, sb.len);
	sb_free(&sb);
}

static bool load(void)
{
	Conf *c = conf_load(G.path);
	const char *sol = conf_get(c, "solution", ""), *giv = conf_get(c, "given", ""), *val = conf_get(c, "value", "");
	const char *notes = conf_get(c, "notes", "");
	bool ok = strlen(sol) == 81 && strlen(giv) == 81 && strlen(val) == 81 && strlen(notes) == 243;
	if (ok) {
		for (int i = 0; i < 81; i++) {
			G.solution[i] = (u8)(sol[i] - '0');
			G.given[i] = (u8)(giv[i] - '0');
			G.value[i] = (u8)(val[i] - '0');
			char hx[4] = { notes[i * 3], notes[i * 3 + 1], notes[i * 3 + 2], 0 };
			u8 b[2];
			char pad[5] = { '0', hx[0], hx[1], hx[2], 0 };
			hex_decode(pad, b, 2);
			G.notes[i] = (u16)(b[0] << 8 | b[1]);
		}
		G.level = (int)CLAMP(conf_get_int(c, "level", MEDIUM), EASY, HARD);
		G.hints = (int)conf_get_int(c, "hints", 0);
		G.mistakes = (int)conf_get_int(c, "mistakes", 0);
		G.elapsed = (f64)conf_get_int(c, "elapsed", 0);
		G.won = conf_get_int(c, "won", 0) != 0;
		G.selected = (int)CLAMP(conf_get_int(c, "selected", 40), 0, 80);
	}
	conf_free(c);
	return ok;
}

static void new_game(int level)
{
	G.level = level;
	make_puzzle(level);
	memset(G.value, 0, sizeof G.value);
	memset(G.notes, 0, sizeof G.notes);
	G.hints = G.mistakes = 0;
	G.elapsed = 0;
	G.won = false;
	G.selected = 40;
	save();
}

/* ---- Playing ------------------------------------------------------------------------------------ */

static int shown(int i)
{
	return G.given[i] ? G.given[i] : G.value[i];
}

static bool conflicts(int cell)
{
	int v = shown(cell);
	if (!v)
		return false;
	int r = cell / 9, c = cell % 9, br = r / 3 * 3, bc = c / 3 * 3;
	for (int i = 0; i < 81; i++) {
		if (i == cell || shown(i) != v)
			continue;
		int r2 = i / 9, c2 = i % 9;
		if (r2 == r || c2 == c || (r2 / 3 * 3 == br && c2 / 3 * 3 == bc))
			return true;
	}
	return false;
}

static void check_won(void)
{
	for (int i = 0; i < 81; i++)
		if (shown(i) != G.solution[i])
			return;
	G.won = true;
	sound_play("win");
}

static void clear_notes_for(int cell, int v)
{
	int r = cell / 9, c = cell % 9, br = r / 3 * 3, bc = c / 3 * 3;
	for (int i = 0; i < 81; i++) {
		int r2 = i / 9, c2 = i % 9;
		if (r2 == r || c2 == c || (r2 / 3 * 3 == br && c2 / 3 * 3 == bc))
			G.notes[i] &= (u16) ~(1 << v);
	}
}

static void enter(int v)
{
	int i = G.selected;
	if (G.won || G.given[i])
		return;
	if (v == 0) {
		G.value[i] = 0;
		G.notes[i] = 0;
	} else if (G.notes_mode) {
		if (!G.value[i])
			G.notes[i] ^= (u16)(1 << v);
	} else {
		G.value[i] = G.value[i] == v ? 0 : (u8)v;
		if (G.value[i]) {
			G.notes[i] = 0;
			clear_notes_for(i, v);
			if (G.value[i] != G.solution[i]) {
				G.mistakes++;
				sound_play("error");
			} else {
				sound_play("click");
			}
		}
		check_won();
	}
	save();
}

static void hint(void)
{
	if (G.won)
		return;
	/* the selected blank, or the first wrong or empty square */
	int cell = -1;
	if (!G.given[G.selected] && G.value[G.selected] != G.solution[G.selected])
		cell = G.selected;
	for (int i = 0; i < 81 && cell < 0; i++)
		if (!G.given[i] && G.value[i] != G.solution[i])
			cell = i;
	if (cell < 0)
		return;
	G.value[cell] = G.solution[cell];
	G.notes[cell] = 0;
	clear_notes_for(cell, G.solution[cell]);
	G.selected = cell;
	G.hints++;
	check_won();
	save();
}

/* ---- Drawing ------------------------------------------------------------------------------------ */

static void draw_grid(Ui *ui, Rect g)
{
	Theme *t = &ui->theme;
	f32 cell = g.w / 9;
	ui_shadow(ui, g, 10, 18, 4, 0x30000000);
	ui_round(ui, g, 10, t->dark ? 0xff23262b : COLOR_WHITE);
	int sel = G.selected, sr = sel / 9, sc = sel % 9;
	int sv = shown(sel);
	for (int i = 0; i < 81; i++) {
		int r = i / 9, c = i % 9;
		Rect cr = rect(g.x + cell * (f32)c, g.y + cell * (f32)r, cell, cell);
		bool related = r == sr || c == sc || (r / 3 == sr / 3 && c / 3 == sc / 3);
		if (i == sel)
			ui_fill(ui, cr, color_with_alpha(t->accent, 0x55));
		else if (sv && shown(i) == sv)
			ui_fill(ui, cr, color_with_alpha(t->accent, 0x30));
		else if (related)
			ui_fill(ui, cr, color_with_alpha(t->accent, 0x12));
		int v = shown(i);
		if (v) {
			char s[2] = { (char)('0' + v), 0 };
			Color col = G.given[i] ? t->text : (v != G.solution[i] || conflicts(i)) ? t->danger : t->accent;
			ui_text_in(ui, ui_font(ui, cell * 0.52f, G.given[i] ? 600 : 500), cr, s, col, ALIGN_CENTER);
		} else if (G.notes[i]) {
			Font nf = ui_font(ui, cell * 0.22f, 500);
			for (int n = 1; n <= 9; n++) {
				if (!(G.notes[i] & (1 << n)))
					continue;
				char s[2] = { (char)('0' + n), 0 };
				Rect nr = rect(cr.x + cell / 3 * (f32)((n - 1) % 3), cr.y + cell / 3 * (f32)((n - 1) / 3), cell / 3, cell / 3);
				ui_text_in(ui, nf, nr, s, n == sv ? t->accent : t->text_dim, ALIGN_CENTER);
			}
		}
	}
	/* lines: thin between cells, thick between boxes */
	for (int k = 1; k < 9; k++) {
		bool thick = k % 3 == 0;
		f32 w = thick ? 2 : 1;
		Color col = thick ? (t->dark ? 0xff9aa3b0 : 0xff3b4656) : t->border;
		ui_fill(ui, rect(g.x + cell * (f32)k - w / 2, g.y, w, g.h), col);
		ui_fill(ui, rect(g.x, g.y + cell * (f32)k - w / 2, g.w, w), col);
	}
	ui_border(ui, g, 10, 2, t->dark ? 0xff9aa3b0 : 0xff3b4656);
	UiInteract in = ui_interact(ui, ui_id(ui, "grid"), g);
	if (in.pressed_now) {
		int c = (int)((ui->mx - g.x) / cell), r = (int)((ui->my - g.y) / cell);
		if (r >= 0 && r < 9 && c >= 0 && c < 9)
			G.selected = r * 9 + c;
	}
}

static void pad_panel(Ui *ui, Rect p)
{
	Theme *t = &ui->theme;
	/* time and stats */
	int secs = (int)G.elapsed;
	char clock[32];
	fmt(clock, sizeof clock, "%d:%02d", secs / 60, secs % 60);
	ui_text_in(ui, ui_font(ui, 34, 300), cut_top(&p, 48), clock, t->text, ALIGN_CENTER);
	char stats[64];
	fmt(stats, sizeof stats, "%d mistake%s · %d hint%s", G.mistakes, G.mistakes == 1 ? "" : "s", G.hints, G.hints == 1 ? "" : "s");
	ui_text_in(ui, ui_font(ui, 12, 500), cut_top(&p, 20), stats, t->text_dim, ALIGN_CENTER);
	cut_top(&p, 20);
	/* number pad */
	f32 gap = 8, bw = (p.w - gap * 2) / 3;
	for (int n = 1; n <= 9; n++) {
		Rect b = rect(p.x + (bw + gap) * (f32)((n - 1) % 3), p.y + (bw * 0.8f + gap) * (f32)((n - 1) / 3), bw, bw * 0.8f);
		ui_push_id(ui, n);
		UiId id = ui_id(ui, "num");
		UiInteract in = ui_interact(ui, id, b);
		f32 hv = ui_fade(ui, id, in.hovered, 0.12f);
		/* dim numbers that are all placed */
		int used = 0;
		for (int i = 0; i < 81; i++)
			used += shown(i) == n && shown(i) == G.solution[i];
		ui_round(ui, b, 10, color_mix(t->card, t->accent, 0.08f + 0.1f * hv));
		ui_border(ui, b, 10, 1, t->border);
		char s[2] = { (char)('0' + n), 0 };
		ui_text_in(ui, ui_font(ui, bw * 0.36f, 500), b, s, used >= 9 ? t->text_faint : t->text, ALIGN_CENTER);
		if (in.clicked)
			enter(n);
		ui_pop_id(ui);
	}
	cut_top(&p, (bw * 0.8f + gap) * 3 + 8);
	Rect row = cut_top(&p, 40);
	f32 w3 = (row.w - 16) / 3;
	if (ui_button_icon_label(ui, cut_left(&row, w3), "edit-clear", "Erase"))
		enter(0);
	cut_left(&row, 8);
	Rect nb = cut_left(&row, w3);
	if (G.notes_mode ? ui_button_suggested(ui, nb, "Notes") : ui_button_icon_label(ui, nb, "edit", "Notes"))
		G.notes_mode = !G.notes_mode;
	cut_left(&row, 8);
	if (ui_button_icon_label(ui, row, "help", "Hint"))
		hint();
	cut_top(&p, 18);
	static const char *const LEVELS[] = { "Easy", "Medium", "Hard" };
	ui_text(ui, ui_font(ui, 11, 700), p.x, p.y + 10, "NEW PUZZLE", t->text_dim);
	cut_top(&p, 18);
	int lv = G.level;
	if (ui_segmented(ui, cut_top(&p, 34), LEVELS, 3, &lv))
		new_game(lv);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	G.ui = ui;
	/* the clock runs while the window is focused and the puzzle unsolved */
	if (!G.won && ui->window_focused) {
		if (G.last_tick > 0)
			G.elapsed += minf(1, (f32)(ui->now - G.last_tick));
		ui->wake_at = ui->now + 1.0 - fmod(G.elapsed, 1.0) + 0.01;
	}
	G.last_tick = ui->now;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	ui_headerbar(ui, header, "Sudoku", HEADER_FLAT);
	win = pad(win, 24);
	Rect panel = cut_right(&win, minf(260, win.w * 0.35f));
	cut_right(&win, 24);
	f32 size = floorf(minf(win.w, win.h) / 9) * 9;
	Rect g = rect(win.x + (win.w - size) / 2, win.y + (win.h - size) / 2, size, size);
	draw_grid(ui, g);
	pad_panel(ui, rect(panel.x, g.y, panel.w, size));
	if (G.won) {
		Rect card = center_in(g, minf(320, size - 40), 160);
		ui_shadow(ui, card, 14, 24, 8, 0x60000000);
		ui_round(ui, card, 14, t->card);
		int secs = (int)G.elapsed;
		char sub[96];
		fmt(sub, sizeof sub, "Solved in %d:%02d with %d hint%s", secs / 60, secs % 60, G.hints, G.hints == 1 ? "" : "s");
		ui_text_in(ui, ui_font(ui, 22, 700), rect(card.x, card.y + 22, card.w, 30), "Well done!", t->text, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(card.x, card.y + 56, card.w, 22), sub, t->text_dim, ALIGN_CENTER);
		if (ui_button_suggested(ui, rect(card.x + (card.w - 150) / 2, card.y + 100, 150, 36), "Next Puzzle"))
			new_game(G.level);
	}
	/* keyboard */
	for (int n = 1; n <= 9; n++)
		if (ui_key_pressed(ui, (u32)(KEY_1 + n - 1), 0) || ui_key_pressed(ui, (u32)(KEY_1 + n - 1), MOD_SHIFT))
			enter(n);
	if (ui_key_pressed(ui, KEY_BACKSPACE, 0) || ui_key_pressed(ui, KEY_DELETE, 0) || ui_key_pressed(ui, KEY_0, 0))
		enter(0);
	if (ui_key_pressed(ui, KEY_N, 0))
		G.notes_mode = !G.notes_mode;
	if (ui_key_pressed(ui, KEY_H, 0))
		hint();
	int r = G.selected / 9, c = G.selected % 9;
	if (ui_key_pressed(ui, KEY_LEFT, 0))
		c = (c + 8) % 9;
	if (ui_key_pressed(ui, KEY_RIGHT, 0))
		c = (c + 1) % 9;
	if (ui_key_pressed(ui, KEY_UP, 0))
		r = (r + 8) % 9;
	if (ui_key_pressed(ui, KEY_DOWN, 0))
		r = (r + 1) % 9;
	G.selected = r * 9 + c;
}

static bool on_close(Ui *ui)
{
	UNUSED(ui);
	save();
	return true;
}

static void init(Ui *ui)
{
	G.ui = ui;
	char *dir = path_join(home_dir(), ".local/share/zurbax");
	dir_make(dir, 0755);
	fmt(G.path, sizeof G.path, "%s/sudoku.conf", dir);
	mem_free(dir);
	if (!load() || G.won)
		new_game(load() ? G.level : MEDIUM);
}

int main(void)
{
	UiAppDesc d = { .id = "sudoku", .title = "Sudoku", .width = 900, .height = 660, .min_width = 640, .min_height = 480,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame, .close = on_close };
	return ui_run(&d);
}
