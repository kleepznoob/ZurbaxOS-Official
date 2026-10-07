/* 2048: slide the tiles, equal numbers merge. Tiles glide to their places
 * and merged tiles pop. Arrow keys, WASD or a swipe with the mouse. */
#include "arcade.h"

#define N 4

typedef struct {
	int value;
	f32 fx, fy;              /* where it's drawn from (animating to its cell) */
	f32 pop;                 /* merge or appear pulse */
} Tile;

static struct {
	Tile t[N][N];
	int score;
	bool over, won, keep_going, new_best;
	f32 anim;                /* 1 -> 0 while tiles slide */
	f32 drag_x, drag_y;
	bool dragging;
} g;

static void add_random(void)
{
	int free[N * N], n = 0;
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++)
			if (!g.t[y][x].value)
				free[n++] = y * N + x;
	if (!n)
		return;
	int k = free[rand_u32() % (u32)n];
	Tile *t = &g.t[k / N][k % N];
	t->value = rand_u32() % 10 == 0 ? 4 : 2;
	t->fx = (f32)(k % N), t->fy = (f32)(k / N);
	t->pop = -1; /* appears from nothing */
}

static void start(void)
{
	memset(&g, 0, sizeof g);
	add_random();
	add_random();
}

static bool can_move(void)
{
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			int v = g.t[y][x].value;
			if (!v || (x + 1 < N && g.t[y][x + 1].value == v) || (y + 1 < N && g.t[y + 1][x].value == v))
				return true;
		}
	return false;
}

/* Slides toward (dx, dy); returns whether anything moved. */
static bool slide(int dx, int dy)
{
	bool moved = false, merged[N][N] = { { false } };
	Tile old[N][N];
	memcpy(old, g.t, sizeof old);
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++)
			g.t[y][x].fx = (f32)x, g.t[y][x].fy = (f32)y, g.t[y][x].pop = 0;
	int order[N] = { 0, 1, 2, 3 };
	if (dx > 0 || dy > 0)
		for (int i = 0; i < N; i++)
			order[i] = N - 1 - i;
	for (int a = 0; a < N; a++)
		for (int b2 = 0; b2 < N; b2++) {
			int x = dx ? order[b2] : a, y = dy ? order[b2] : a;
			if (!g.t[y][x].value)
				continue;
			int cx = x, cy = y;
			while (true) {
				int nx = cx + dx, ny = cy + dy;
				if (nx < 0 || ny < 0 || nx >= N || ny >= N)
					break;
				if (!g.t[ny][nx].value) {
					g.t[ny][nx] = g.t[cy][cx];
					g.t[cy][cx].value = 0;
					cx = nx, cy = ny;
					moved = true;
				} else if (g.t[ny][nx].value == g.t[cy][cx].value && !merged[ny][nx]) {
					int v = g.t[ny][nx].value * 2;
					f32 fx = g.t[cy][cx].fx, fy = g.t[cy][cx].fy;
					g.t[ny][nx].value = v;
					g.t[ny][nx].fx = fx, g.t[ny][nx].fy = fy;
					g.t[ny][nx].pop = 1;
					g.t[cy][cx].value = 0;
					merged[ny][nx] = true;
					g.score += v;
					if (v == 2048 && !g.won)
						g.won = true;
					moved = true;
					break;
				} else {
					break;
				}
			}
		}
	if (!moved)
		memcpy(g.t, old, sizeof old);
	return moved;
}

static void move(int dx, int dy)
{
	if (g.over || (g.won && !g.keep_going) || game_paused)
		return;
	if (slide(dx, dy)) {
		g.anim = 1;
		bool any_merge = false;
		for (int y = 0; y < N; y++)
			for (int x = 0; x < N; x++)
				any_merge |= g.t[y][x].pop > 0;
		sound_play(any_merge ? "pop" : "click");
		add_random();
		if (g.won && !g.keep_going)
			sound_play("win");
		if (!can_move()) {
			g.over = true;
			g.new_best = submit_score("2048", g.score);
			sound_play("lose");
		}
	}
}

static Color tile_color(int v)
{
	static const char *const c[] = { "#eee4da", "#ede0c8", "#f2b179", "#f59563", "#f67c5f", "#f65e3b",
					 "#edcf72", "#edcc61", "#edc850", "#edc53f", "#edc22e", "#3c3a32" };
	int i = 0;
	while ((2 << i) < v && i < 11)
		i++;
	return color_hex(c[i]);
}

static void draw_tile(Ui *ui, Rect r, int v, f32 scale)
{
	Rect t = center_in(r, r.w * scale, r.h * scale);
	ui_shadow(ui, t, 8, 8, 3, color_hex("#00000030"));
	bevel_cell(ui, t, 8 * scale, tile_color(v));
	char s[16];
	fmt(s, sizeof s, "%d", v);
	f32 size = (v < 100 ? 40 : v < 1000 ? 34 : v < 10000 ? 28 : 22) * (t.w / 100);
	ui_text_in(ui, ui_font(ui, size, 800), rect(t.x, t.y - t.h * 0.03f, t.w, t.h), s, v <= 4 ? color_hex("#776e65") : COLOR_WHITE, ALIGN_CENTER);
}

static void frame(Ui *ui, Rect r)
{
	if (key_hit(ui, KEY_LEFT) || key_hit(ui, KEY_A) || pad_hit(ui, PAD_LEFT))
		move(-1, 0);
	if (key_hit(ui, KEY_RIGHT) || key_hit(ui, KEY_D) || pad_hit(ui, PAD_RIGHT))
		move(1, 0);
	if (key_hit(ui, KEY_UP) || key_hit(ui, KEY_W) || pad_hit(ui, PAD_UP))
		move(0, -1);
	if (key_hit(ui, KEY_DOWN) || key_hit(ui, KEY_S) || pad_hit(ui, PAD_DOWN))
		move(0, 1);
	g.anim = maxf(0, g.anim - ui->dt * 9);
	f32 size = minf(r.w - 40, r.h - 90);
	size = minf(size, 520);
	Rect board = rect(r.x + (r.w - size) / 2, r.y + 80, size, size);
	/* Scores above. */
	char v[32];
	fmt(v, sizeof v, "%d", g.score);
	stat_box(ui, rect(board.x + board.w - 250, r.y + 4, 120, 62), "SCORE", v);
	fmt(v, sizeof v, "%d", MAX(best_score("2048"), g.score));
	stat_box(ui, rect(board.x + board.w - 120, r.y + 4, 120, 62), "BEST", v);
	ui_text_in(ui, ui_font(ui, 44, 900), rect(board.x, r.y, 200, 70), "2048", color_hex("#edc22e"), ALIGN_START);
	/* Swipes with the mouse. */
	UiInteract it = ui_interact(ui, ui_id(ui, "board"), board);
	if (it.pressed_now) {
		g.dragging = true;
		g.drag_x = ui->mx, g.drag_y = ui->my;
	}
	if (g.dragging && !ui->down[BUTTON_LEFT]) {
		g.dragging = false;
		f32 ddx = ui->mx - g.drag_x, ddy = ui->my - g.drag_y;
		if (fabsf(ddx) > 30 || fabsf(ddy) > 30) {
			if (fabsf(ddx) > fabsf(ddy))
				move(ddx > 0 ? 1 : -1, 0);
			else
				move(0, ddy > 0 ? 1 : -1);
		}
	}
	ui_round(ui, board, 14, color_hex("#bbada0"));
	f32 gap = size * 0.03f, cell = (size - gap * (N + 1)) / N;
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++)
			ui_round(ui, rect(board.x + gap + (f32)x * (cell + gap), board.y + gap + (f32)y * (cell + gap), cell, cell), 8, color_hex("#cdc1b4"));
	f32 k = g.anim * g.anim;
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			Tile *t = &g.t[y][x];
			if (!t->value)
				continue;
			f32 dx = (f32)x + (t->fx - (f32)x) * k, dy = (f32)y + (t->fy - (f32)y) * k;
			Rect cr = rect(board.x + gap + dx * (cell + gap), board.y + gap + dy * (cell + gap), cell, cell);
			f32 scale = 1;
			if (t->pop > 0 && g.anim < 0.6f)
				scale = 1 + 0.12f * sinf(PI * (0.6f - g.anim) / 0.6f);
			else if (t->pop < 0)
				scale = 1 - g.anim * 0.9f;
			draw_tile(ui, cr, t->value, scale);
		}
	if (g.won && !g.keep_going) {
		if (game_overlay(ui, board, "2048!", "You made the tile. Keep going for more?", "Keep Going")) {
			g.keep_going = true;
			submit_score("2048", g.score);
		}
	} else if (g.over) {
		char sub[96];
		fmt(sub, sizeof sub, g.new_best ? "A new best: %d!" : "No more moves. %d points.", g.score);
		if (game_overlay(ui, board, "Game Over", sub, "Try Again"))
			start();
	} else if (game_paused) {
		if (game_overlay(ui, board, "Paused", "The tiles will wait.", "Resume"))
			game_paused = false;
	}
}

static void preview(Ui *ui, Rect r)
{
	f32 s = minf(r.h - 10, 150);
	Rect b = rect(r.x + (r.w - s) / 2, r.y + (r.h - s) / 2, s, s);
	ui_round(ui, b, 10, color_hex("#bbada0"));
	static const int vals[16] = { 2, 0, 4, 8, 0, 16, 0, 32, 64, 0, 128, 0, 0, 512, 0, 2048 };
	f32 gap = s * 0.04f, c = (s - gap * 5) / 4;
	for (int i = 0; i < 16; i++) {
		Rect cr = rect(b.x + gap + (f32)(i % 4) * (c + gap), b.y + gap + (f32)(i / 4) * (c + gap), c, c);
		if (vals[i])
			draw_tile(ui, cr, vals[i], 1);
		else
			ui_round(ui, cr, 5, color_hex("#cdc1b4"));
	}
}

const Game GAME_2048 = { "2048", "2048", "Slide and merge your way to 2048.", "Arrow keys, WASD or swipe with the mouse", "D-pad or left stick to slide", start, frame, preview,
			 0xffc98a3a, 0xff7a4a14 };
