/* Blocks: falling pieces, full rows clear. Hold, ghost piece, three next
 * pieces, levels that speed up every ten lines. */
#include "arcade.h"

#define W 10
#define H 20

static const char *const COLORS[] = { "#3fd0e0", "#f2c43d", "#a35ce8", "#4ccf68", "#e8505b", "#4a7cf0", "#f08c3a" };
/* I O T S Z J L, as 4x4 bit masks for each of the four rotations. */
static const u16 SHAPES[7][4] = {
	{ 0x0F00, 0x2222, 0x00F0, 0x4444 }, { 0x6600, 0x6600, 0x6600, 0x6600 }, { 0x4E00, 0x4640, 0x0E40, 0x4C40 },
	{ 0x6C00, 0x4620, 0x06C0, 0x8C40 }, { 0xC600, 0x2640, 0x0C60, 0x4C80 }, { 0x8E00, 0x6440, 0x0E20, 0x44C0 },
	{ 0x2E00, 0x4460, 0x0E80, 0xC440 },
};

static struct {
	u8 board[H][W];          /* 0 empty, else piece + 1 */
	int piece, rot, px, py;
	int hold;                /* -1 none */
	bool hold_used;
	int bag[7], bag_left;
	int next[3];
	int score, lines, level;
	f64 fall_timer, lock_timer;
	bool over, new_best;
	u8 clearing[H];
	f64 clear_t;
} b;

static bool cell(int piece, int rot, int x, int y)
{
	return (SHAPES[piece][rot] >> (15 - (y * 4 + x))) & 1;
}

static bool fits(int piece, int rot, int px, int py)
{
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++) {
			if (!cell(piece, rot, x, y))
				continue;
			int bx = px + x, by = py + y;
			if (bx < 0 || bx >= W || by >= H || (by >= 0 && b.board[by][bx]))
				return false;
		}
	return true;
}

static int from_bag(void)
{
	if (b.bag_left == 0) {
		for (int i = 0; i < 7; i++)
			b.bag[i] = i;
		for (int i = 6; i > 0; i--) {
			int j = (int)(rand_u32() % (u32)(i + 1)), t = b.bag[i];
			b.bag[i] = b.bag[j], b.bag[j] = t;
		}
		b.bag_left = 7;
	}
	return b.bag[--b.bag_left];
}

static void spawn(int piece)
{
	b.piece = piece;
	b.rot = 0;
	b.px = 3;
	b.py = piece == 0 ? -1 : 0;
	b.lock_timer = 0;
	if (!fits(b.piece, b.rot, b.px, b.py)) {
		b.over = true;
		b.new_best = submit_score("blocks", b.score);
		sound_play("lose");
	}
}

static void next_piece(void)
{
	spawn(b.next[0]);
	b.next[0] = b.next[1];
	b.next[1] = b.next[2];
	b.next[2] = from_bag();
	b.hold_used = false;
}

static void start(void)
{
	memset(&b, 0, sizeof b);
	b.hold = -1;
	b.level = 1;
	for (int i = 0; i < 3; i++)
		b.next[i] = from_bag();
	next_piece();
}

static void lock_piece(void)
{
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			if (cell(b.piece, b.rot, x, y) && b.py + y >= 0)
				b.board[b.py + y][b.px + x] = (u8)(b.piece + 1);
	int full = 0;
	for (int y = 0; y < H; y++) {
		bool f = true;
		for (int x = 0; x < W; x++)
			f &= b.board[y][x] != 0;
		b.clearing[y] = f;
		full += f;
	}
	if (full) {
		static const int points[] = { 0, 100, 300, 500, 800 };
		b.score += points[full] * b.level;
		b.lines += full;
		b.level = 1 + b.lines / 10;
		b.clear_t = 0.22;
		sound_play(full == 4 ? "win" : "coin");
	} else {
		sound_play("pop");
		next_piece();
	}
}

static void remove_full(void)
{
	int to = H - 1;
	for (int y = H - 1; y >= 0; y--) {
		if (b.clearing[y])
			continue;
		if (to != y)
			memcpy(b.board[to], b.board[y], W);
		to--;
	}
	while (to >= 0)
		memset(b.board[to--], 0, W);
	memset(b.clearing, 0, sizeof b.clearing);
	next_piece();
}

static void try_rotate(int dir)
{
	int r = (b.rot + dir + 4) % 4;
	static const int kicks[] = { 0, -1, 1, -2, 2 };
	for (int i = 0; i < 5; i++)
		for (int up = 0; up <= 1; up++)
			if (fits(b.piece, r, b.px + kicks[i], b.py - up)) {
				b.rot = r;
				b.px += kicks[i];
				b.py -= up;
				b.lock_timer = 0;
				return;
			}
}

static int ghost_y(void)
{
	int y = b.py;
	while (fits(b.piece, b.rot, b.px, y + 1))
		y++;
	return y;
}

static void draw_piece_in(Ui *ui, Rect r, int piece)
{
	if (piece < 0)
		return;
	f32 s = minf(r.w, r.h) / 4.4f;
	int minx = 4, maxx = 0, miny = 4, maxy = 0;
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			if (cell(piece, 0, x, y))
				minx = MIN(minx, x), maxx = MAX(maxx, x), miny = MIN(miny, y), maxy = MAX(maxy, y);
	f32 ox = r.x + (r.w - (f32)(maxx - minx + 1) * s) / 2, oy = r.y + (r.h - (f32)(maxy - miny + 1) * s) / 2;
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			if (cell(piece, 0, x, y))
				bevel_cell(ui, rect(ox + (f32)(x - minx) * s + 1, oy + (f32)(y - miny) * s + 1, s - 2, s - 2), s * 0.18f, color_hex(COLORS[piece]));
}

static void frame(Ui *ui, Rect r)
{
	f32 dt = ui->dt;
	if (!b.over && !game_paused) {
		if (b.clear_t > 0) {
			b.clear_t -= dt;
			if (b.clear_t <= 0)
				remove_full();
		} else {
			if (key_hit(ui, KEY_LEFT) && fits(b.piece, b.rot, b.px - 1, b.py))
				b.px--, b.lock_timer = 0;
			if (key_hit(ui, KEY_RIGHT) && fits(b.piece, b.rot, b.px + 1, b.py))
				b.px++, b.lock_timer = 0;
			if (key_hit(ui, KEY_UP) || key_hit(ui, KEY_X))
				try_rotate(1);
			if (key_hit(ui, KEY_Z))
				try_rotate(-1);
			if (key_hit(ui, KEY_C) && !b.hold_used) {
				int h = b.hold;
				b.hold = b.piece;
				b.hold_used = true;
				if (h < 0)
					next_piece(), b.hold_used = true;
				else
					spawn(h);
			}
			if (key_hit(ui, KEY_SPACE)) {
				int g = ghost_y();
				b.score += 2 * (g - b.py);
				b.py = g;
				lock_piece();
			} else {
				f64 interval = maxf(0.05f, 0.8f * powf(0.84f, (f32)(b.level - 1)));
				if (key_down(KEY_DOWN))
					interval = minf((f32)interval, 0.04f);
				b.fall_timer += dt;
				while (b.fall_timer >= interval && !b.over && b.clear_t <= 0) {
					b.fall_timer -= interval;
					if (fits(b.piece, b.rot, b.px, b.py + 1)) {
						b.py++;
						if (key_down(KEY_DOWN))
							b.score++;
					} else {
						b.fall_timer = 0;
						break;
					}
				}
				/* A short grace period on the ground before it locks. */
				if (!fits(b.piece, b.rot, b.px, b.py + 1)) {
					b.lock_timer += dt;
					if (b.lock_timer > 0.45)
						lock_piece();
				}
			}
		}
	}
	/* Layout: the board in the middle, panels either side. */
	f32 cs = floorf(minf((r.h - 10) / H, (r.w - 400) / W));
	cs = maxf(cs, 14);
	f32 bw = cs * W, bh = cs * H;
	Rect board = rect(r.x + (r.w - bw) / 2, r.y + (r.h - bh) / 2, bw, bh);
	ui_round(ui, pad(board, -8), 14, color_hex("#0a0c12"));
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			ui_round(ui, rect(board.x + (f32)x * cs + 1, board.y + (f32)y * cs + 1, cs - 2, cs - 2), cs * 0.12f, color_hex("#ffffff07"));
	for (int y = 0; y < H; y++) {
		bool flash = b.clearing[y] && b.clear_t > 0;
		for (int x = 0; x < W; x++)
			if (b.board[y][x]) {
				Rect c = rect(board.x + (f32)x * cs + 1, board.y + (f32)y * cs + 1, cs - 2, cs - 2);
				bevel_cell(ui, c, cs * 0.18f, flash ? COLOR_WHITE : color_hex(COLORS[b.board[y][x] - 1]));
			}
	}
	if (!b.over && b.clear_t <= 0) {
		int gy = ghost_y();
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++) {
				if (!cell(b.piece, b.rot, x, y))
					continue;
				Rect g = rect(board.x + (f32)(b.px + x) * cs + 2, board.y + (f32)(gy + y) * cs + 2, cs - 4, cs - 4);
				if (gy + y >= 0)
					ui_border(ui, g, cs * 0.16f, 1.5f, color_with_alpha(color_hex(COLORS[b.piece]), 0x90));
				if (b.py + y >= 0)
					bevel_cell(ui, rect(board.x + (f32)(b.px + x) * cs + 1, board.y + (f32)(b.py + y) * cs + 1, cs - 2, cs - 2), cs * 0.18f,
						   color_hex(COLORS[b.piece]));
			}
	}
	/* Left: hold and stats. Right: next pieces. */
	f32 pw = 150;
	Rect left = rect(board.x - 24 - pw, board.y, pw, bh), right = rect(board.x + bw + 24, board.y, pw, bh);
	Rect hold = cut_top(&left, 130);
	ui_round(ui, hold, 12, color_hex("#ffffff0d"));
	ui_text_in(ui, ui_font(ui, 11, 700), rect(hold.x + 14, hold.y + 8, 100, 16), "HOLD (C)", color_hex("#ffffff80"), ALIGN_START);
	draw_piece_in(ui, pad4(hold, 28, 22, 12, 22), b.hold);
	cut_top(&left, 14);
	char v[32];
	fmt(v, sizeof v, "%d", b.score);
	stat_box(ui, cut_top(&left, 62), "SCORE", v);
	cut_top(&left, 10);
	fmt(v, sizeof v, "%d", b.level);
	stat_box(ui, cut_top(&left, 62), "LEVEL", v);
	cut_top(&left, 10);
	fmt(v, sizeof v, "%d", b.lines);
	stat_box(ui, cut_top(&left, 62), "LINES", v);
	cut_top(&left, 10);
	fmt(v, sizeof v, "%d", MAX(best_score("blocks"), b.score));
	stat_box(ui, cut_top(&left, 62), "BEST", v);
	Rect next = cut_top(&right, 330);
	ui_round(ui, next, 12, color_hex("#ffffff0d"));
	ui_text_in(ui, ui_font(ui, 11, 700), rect(next.x + 14, next.y + 8, 100, 16), "NEXT", color_hex("#ffffff80"), ALIGN_START);
	for (int i = 0; i < 3; i++)
		draw_piece_in(ui, rect(next.x + 22, next.y + 34 + (f32)i * 98, next.w - 44, 80), b.next[i]);
	if (b.over) {
		char sub[96];
		fmt(sub, sizeof sub, b.new_best ? "A new best: %d points!" : "%d points.", b.score);
		if (game_overlay(ui, board, "Game Over", sub, "Play Again"))
			start();
	} else if (game_paused) {
		if (game_overlay(ui, board, "Paused", "Take your time.", "Resume"))
			game_paused = false;
	}
}

static void preview(Ui *ui, Rect r)
{
	f32 cs = minf(r.h / 7, 22);
	static const u8 art[7][10] = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 3, 0, 0, 0, 0, 0, 0 }, { 0, 0, 3, 3, 3, 0, 0, 0, 0, 0 },
				       { 0, 0, 0, 0, 0, 0, 0, 0, 2, 2 }, { 6, 0, 0, 0, 0, 0, 1, 0, 2, 2 }, { 6, 6, 6, 4, 4, 0, 1, 5, 5, 7 },
				       { 2, 2, 4, 4, 1, 1, 1, 1, 5, 5 } };
	f32 ox = r.x + (r.w - cs * 10) / 2, oy = r.y + r.h - cs * 7;
	for (int y = 0; y < 7; y++)
		for (int x = 0; x < 10; x++)
			if (art[y][x])
				bevel_cell(ui, rect(ox + (f32)x * cs + 1, oy + (f32)y * cs + 1, cs - 2, cs - 2), cs * 0.2f, color_hex(COLORS[art[y][x] - 1]));
}

const Game GAME_BLOCKS = { "blocks", "Blocks", "Fit the falling pieces, clear the rows.", "← → move   ↑ rotate   ↓ drop   Space slam   C hold",
			   start, frame, preview, 0xff3a2a7a, 0xff1b1440 };
