/* Mines: clear the field without stepping on a mine. The first click is
 * always safe; right-click plants a flag; clicking a number whose flags
 * are all placed opens its neighbours. Three sizes. */
#include "arcade.h"

#define MW 30
#define MH 16

static const struct { const char *name; int w, h, mines; } LEVELS[] = { { "Easy", 9, 9, 10 }, { "Medium", 16, 16, 40 }, { "Hard", 30, 16, 99 } };

static struct {
	int level;
	int w, h, mines;
	bool mine[MH][MW], open[MH][MW], flag[MH][MW];
	u8 count[MH][MW];
	bool placed, over, won, new_best;
	int opened, flags;
	f64 time;
	int boom_x, boom_y;
} m;

static void start(void)
{
	int level = m.level;
	memset(&m, 0, sizeof m);
	m.level = level;
	m.w = LEVELS[level].w, m.h = LEVELS[level].h, m.mines = LEVELS[level].mines;
	m.boom_x = m.boom_y = -1;
}

static void place(int sx, int sy)
{
	int n = 0;
	while (n < m.mines) {
		int x = (int)(rand_u32() % (u32)m.w), y = (int)(rand_u32() % (u32)m.h);
		/* Keep the first click and its neighbours clear. */
		if (m.mine[y][x] || (ABS(x - sx) <= 1 && ABS(y - sy) <= 1))
			continue;
		m.mine[y][x] = true;
		n++;
	}
	for (int y = 0; y < m.h; y++)
		for (int x = 0; x < m.w; x++) {
			int c = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int nx = x + dx, ny = y + dy;
					if ((dx || dy) && nx >= 0 && ny >= 0 && nx < m.w && ny < m.h && m.mine[ny][nx])
						c++;
				}
			m.count[y][x] = (u8)c;
		}
	m.placed = true;
}

static void reveal(int x, int y)
{
	/* Flood fill without recursion. */
	static int stack[MW * MH * 2];
	int sp = 0;
	stack[sp++] = x, stack[sp++] = y;
	while (sp) {
		int cy = stack[--sp], cx = stack[--sp];
		if (cx < 0 || cy < 0 || cx >= m.w || cy >= m.h || m.open[cy][cx] || m.flag[cy][cx])
			continue;
		m.open[cy][cx] = true;
		m.opened++;
		if (m.mine[cy][cx]) {
			m.over = true;
			m.boom_x = cx, m.boom_y = cy;
			sound_play("hit");
			sound_play("lose");
			return;
		}
		if (m.count[cy][cx] == 0)
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					if (dx || dy) {
						stack[sp++] = cx + dx;
						stack[sp++] = cy + dy;
					}
	}
}

static void check_win(void)
{
	if (!m.over && m.opened == m.w * m.h - m.mines) {
		m.won = m.over = true;
		/* Faster is better: the score is what's left of an hour, by level. */
		int score = MAX(1, (3600 - (int)m.time)) * (m.level + 1);
		m.new_best = submit_score("mines", score);
		sound_play("win");
	}
}

static void paint_mine(Ui *ui, f32 cx, f32 cy, f32 r)
{
	for (int k = 0; k < 4; k++) {
		f32 a = (f32)k * PI / 4;
		ui_line(ui, cx - cosf(a) * r * 1.35f, cy - sinf(a) * r * 1.35f, cx + cosf(a) * r * 1.35f, cy + sinf(a) * r * 1.35f, maxf(1.2f, r * 0.22f),
			color_hex("#1c1c22"));
	}
	ui_circle(ui, cx, cy, r, color_hex("#24242c"));
	ui_circle(ui, cx - r * 0.35f, cy - r * 0.35f, r * 0.28f, color_hex("#ffffffa0"));
}

static void paint_flag(Ui *ui, f32 cx, f32 cy, f32 s)
{
	ui_line(ui, cx - s * 0.12f, cy - s * 0.32f, cx - s * 0.12f, cy + s * 0.3f, maxf(1.2f, s * 0.07f), color_hex("#2b2b33"));
	Path p = { 0 };
	path_move(&p, cx - s * 0.1f, cy - s * 0.34f);
	path_line(&p, cx + s * 0.3f, cy - s * 0.18f);
	path_line(&p, cx - s * 0.1f, cy - s * 0.02f);
	path_close(&p);
	ui_path_fill(ui, &p, color_hex("#e8414e"));
	path_free(&p);
	ui_round(ui, rect(cx - s * 0.3f, cy + s * 0.26f, s * 0.38f, s * 0.08f), s * 0.03f, color_hex("#2b2b33"));
}

static void frame(Ui *ui, Rect r)
{
	if (m.placed && !m.over && !game_paused)
		m.time += ui->dt;
	/* Level choice and status above the field. */
	Rect top = cut_top(&r, 46);
	const char *names[] = { LEVELS[0].name, LEVELS[1].name, LEVELS[2].name };
	int lvl = m.level;
	if (ui_segmented(ui, rect(top.x, top.y + 4, 270, 34), names, 3, &lvl) && lvl != m.level) {
		m.level = lvl;
		start();
	}
	char st[64];
	fmt(st, sizeof st, "%d left    %d:%02d", m.mines - m.flags, (int)m.time / 60, (int)m.time % 60);
	ui_text_in(ui, ui_font(ui, 18, 700), rect(top.x + top.w - 300, top.y, 300, 42), st, COLOR_WHITE, ALIGN_END);
	cut_top(&r, 10);
	f32 cs = floorf(minf(r.w / (f32)m.w, r.h / (f32)m.h));
	cs = minf(cs, 40);
	Rect field = rect(r.x + (r.w - cs * (f32)m.w) / 2, r.y, cs * (f32)m.w, cs * (f32)m.h);
	static const char *const NUM_COLORS[] = { "", "#3b82f6", "#22a35a", "#e5484d", "#7c3aed", "#b45309", "#0e9aa7", "#333333", "#888888" };
	ui_round(ui, pad(field, -6), 10, color_hex("#2a2f3d"));
	for (int y = 0; y < m.h; y++)
		for (int x = 0; x < m.w; x++) {
			Rect c = rect(field.x + (f32)x * cs + 1, field.y + (f32)y * cs + 1, cs - 2, cs - 2);
			ui_push_id(ui, y * MW + x);
			UiInteract it = ui_interact(ui, ui_id(ui, "c"), c);
			bool open = m.open[y][x];
			if (!open) {
				Color base = color_hex(it.hovered && !m.over ? "#8aa4d6" : "#6f87b8");
				bevel_cell(ui, c, cs * 0.14f, base);
				if (m.flag[y][x])
					paint_flag(ui, c.x + c.w / 2, c.y + c.h / 2, cs);
				else if (m.over && m.mine[y][x])
					paint_mine(ui, c.x + c.w / 2, c.y + c.h / 2, cs * 0.22f);
			} else {
				bool boom = x == m.boom_x && y == m.boom_y;
				ui_round(ui, c, cs * 0.12f, color_hex(boom ? "#ff5a5f" : "#dfe5ef"));
				if (m.mine[y][x])
					paint_mine(ui, c.x + c.w / 2, c.y + c.h / 2, cs * 0.22f);
				else if (m.count[y][x]) {
					char n[2] = { (char)('0' + m.count[y][x]), 0 };
					ui_text_in(ui, ui_font(ui, cs * 0.55f, 800), c, n, color_hex(NUM_COLORS[m.count[y][x]]), ALIGN_CENTER);
				}
			}
			if (!m.over && !game_paused) {
				if (it.clicked && !m.flag[y][x]) {
					if (!m.placed)
						place(x, y);
					if (!open) {
						reveal(x, y);
						if (!m.over)
							sound_play("click");
					} else if (m.count[y][x]) {
						/* Chord: open around a satisfied number. */
						int f = 0;
						for (int dy = -1; dy <= 1; dy++)
							for (int dx = -1; dx <= 1; dx++) {
								int nx = x + dx, ny = y + dy;
								if (nx >= 0 && ny >= 0 && nx < m.w && ny < m.h && m.flag[ny][nx])
									f++;
							}
						if (f == m.count[y][x])
							for (int dy = -1; dy <= 1 && !m.over; dy++)
								for (int dx = -1; dx <= 1 && !m.over; dx++)
									reveal(x + dx, y + dy);
					}
					check_win();
				}
				if (it.right_clicked && !open) {
					m.flag[y][x] = !m.flag[y][x];
					m.flags += m.flag[y][x] ? 1 : -1;
					sound_play("pop");
				}
			}
			ui_pop_id(ui);
		}
	if (m.over) {
		char sub[96];
		if (m.won)
			fmt(sub, sizeof sub, m.new_best ? "Cleared in %d:%02d — a new best!" : "Cleared in %d:%02d.", (int)m.time / 60, (int)m.time % 60);
		else
			str_copy(sub, "Boom. That one was a mine.", sizeof sub);
		if (game_overlay(ui, field, m.won ? "Cleared!" : "Game Over", sub, "Play Again"))
			start();
	} else if (game_paused) {
		if (game_overlay(ui, field, "Paused", "The clock has stopped.", "Resume"))
			game_paused = false;
	}
}

static void preview(Ui *ui, Rect r)
{
	f32 cs = minf(r.h / 5, 28);
	f32 ox = r.x + (r.w - cs * 7) / 2, oy = r.y + (r.h - cs * 5) / 2;
	static const char *const cells = "..1F1..1121111.......12F1..1-1-1..";
	for (int i = 0; i < 35; i++) {
		Rect c = rect(ox + (f32)(i % 7) * cs + 1, oy + (f32)(i / 7) * cs + 1, cs - 2, cs - 2);
		char ch = cells[i];
		if (ch == 'F' || ch == '-') {
			bevel_cell(ui, c, cs * 0.14f, color_hex("#6f87b8"));
			if (ch == 'F')
				paint_flag(ui, c.x + c.w / 2, c.y + c.h / 2, cs);
		} else {
			ui_round(ui, c, cs * 0.12f, color_hex("#dfe5ef"));
			if (ch >= '1' && ch <= '3') {
				char n[2] = { ch, 0 };
				ui_text_in(ui, ui_font(ui, cs * 0.55f, 800), c, n, color_hex(ch == '1' ? "#3b82f6" : ch == '2' ? "#22a35a" : "#e5484d"), ALIGN_CENTER);
			}
		}
	}
}

const Game GAME_MINES = { "mines", "Mines", "Find the mines without stepping on one.", "Click to open, right-click to flag, click a number to open around it",
			  start, frame, preview, 0xff3a5a9a, 0xff1a2a50 };
