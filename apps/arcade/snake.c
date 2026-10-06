/* Snake: eat, grow, don't bite the walls or yourself. Moves smoothly between
 * cells and speeds up as it grows. */
#include "arcade.h"

#define GW 24
#define GH 17
#define MAXLEN (GW * GH)

static struct {
	int x[MAXLEN], y[MAXLEN];    /* head first */
	int len;
	int dx, dy, ndx, ndy;        /* direction, and the one asked for */
	int qdx, qdy;                /* a second turn queued within one step */
	int fx, fy;                  /* food */
	f64 step_t, interval;
	int score;
	bool over, new_best, started;
	f32 eat_pulse;
} s;

static void place_food(void)
{
	for (int tries = 0; tries < 1000; tries++) {
		int x = (int)(rand_u32() % GW), y = (int)(rand_u32() % GH);
		bool free = true;
		for (int i = 0; i < s.len && free; i++)
			free = s.x[i] != x || s.y[i] != y;
		if (free) {
			s.fx = x, s.fy = y;
			return;
		}
	}
}

static void start(void)
{
	memset(&s, 0, sizeof s);
	s.len = 4;
	for (int i = 0; i < s.len; i++)
		s.x[i] = 8 - i, s.y[i] = GH / 2;
	s.dx = s.ndx = 1;
	s.interval = 0.13;
	place_food();
}

static void turn(int dx, int dy)
{
	s.started = true;
	/* The last direction asked for this step decides what a second turn may be. */
	int cdx = s.ndx, cdy = s.ndy;
	if (cdx != s.dx || cdy != s.dy) {
		if (dx != -cdx || dy != -cdy)
			s.qdx = dx, s.qdy = dy;
		return;
	}
	if (dx == -s.dx && dy == -s.dy)
		return;
	s.ndx = dx, s.ndy = dy;
}

static void step(void)
{
	s.dx = s.ndx, s.dy = s.ndy;
	if (s.qdx || s.qdy) {
		if (s.qdx != -s.dx || s.qdy != -s.dy)
			s.ndx = s.qdx, s.ndy = s.qdy;
		s.qdx = s.qdy = 0;
	}
	int nx = s.x[0] + s.dx, ny = s.y[0] + s.dy;
	bool eat = nx == s.fx && ny == s.fy;
	bool hit = nx < 0 || ny < 0 || nx >= GW || ny >= GH;
	for (int i = 0; i < s.len - (eat ? 0 : 1) && !hit; i++)
		hit = s.x[i] == nx && s.y[i] == ny;
	if (hit) {
		s.over = true;
		s.new_best = submit_score("snake", s.score);
		sound_play("lose");
		return;
	}
	if (eat && s.len < MAXLEN)
		s.len++;
	for (int i = s.len - 1; i > 0; i--)
		s.x[i] = s.x[i - 1], s.y[i] = s.y[i - 1];
	s.x[0] = nx, s.y[0] = ny;
	if (eat) {
		s.score += 10;
		s.interval = maxf(0.06f, (f32)s.interval * 0.975f);
		s.eat_pulse = 1;
		sound_play("coin");
		place_food();
	}
}

static void paint_apple(Ui *ui, f32 cx, f32 cy, f32 r)
{
	ui_circle(ui, cx, cy + r * 0.1f, r * 1.25f, color_hex("#ff4d4d22"));
	ui_gradient(ui, rect(cx - r, cy - r * 0.9f, 2 * r, 2 * r), r, color_hex("#ff6b5e"), color_hex("#c81e2c"));
	ui_circle(ui, cx - r * 0.35f, cy - r * 0.3f, r * 0.25f, color_hex("#ffffff60"));
	ui_line(ui, cx, cy - r * 0.8f, cx + r * 0.15f, cy - r * 1.25f, maxf(1.5f, r * 0.15f), color_hex("#6b4226"));
	ui_round(ui, rect(cx + r * 0.1f, cy - r * 1.35f, r * 0.6f, r * 0.32f), r * 0.16f, color_hex("#5ccf5a"));
}

static void frame(Ui *ui, Rect r)
{
	if (key_hit(ui, KEY_UP) || key_hit(ui, KEY_W))
		turn(0, -1);
	if (key_hit(ui, KEY_DOWN) || key_hit(ui, KEY_S))
		turn(0, 1);
	if (key_hit(ui, KEY_LEFT) || key_hit(ui, KEY_A))
		turn(-1, 0);
	if (key_hit(ui, KEY_RIGHT) || key_hit(ui, KEY_D))
		turn(1, 0);
	if (!s.over && !game_paused && s.started) {
		s.step_t += ui->dt;
		while (s.step_t >= s.interval && !s.over) {
			s.step_t -= s.interval;
			step();
		}
	}
	s.eat_pulse = maxf(0, s.eat_pulse - ui->dt * 3);
	f32 cs = floorf(minf(r.w / GW, (r.h - 60) / GH));
	Rect field = rect(r.x + (r.w - cs * GW) / 2, r.y + 50, cs * GW, cs * GH);
	/* Score line above the field. */
	char line[96];
	fmt(line, sizeof line, "Score %d", s.score);
	ui_text_in(ui, ui_font(ui, 20, 800), rect(field.x, r.y + 8, 200, 30), line, COLOR_WHITE, ALIGN_START);
	fmt(line, sizeof line, "Best %d", MAX(best_score("snake"), s.score));
	ui_text_in(ui, ui_font(ui, 15, 600), rect(field.x + field.w - 200, r.y + 12, 200, 26), line, color_hex("#ffffff90"), ALIGN_END);
	/* A checkered lawn. */
	ui_round(ui, pad(field, -6), 14, color_hex("#173d24"));
	for (int y = 0; y < GH; y++)
		for (int x = 0; x < GW; x++)
			ui_fill(ui, rect(field.x + (f32)x * cs, field.y + (f32)y * cs, cs, cs), color_hex((x + y) % 2 ? "#2a6a3c" : "#2f7543"));
	paint_apple(ui, field.x + ((f32)s.fx + 0.5f) * cs, field.y + ((f32)s.fy + 0.5f) * cs, cs * 0.36f * (1 + 0.08f * sinf((f32)ui->now * 5)));
	/* The body, sliding between cells: the head moves ahead by the step's
	 * progress and the tail catches up, with straight runs in between. */
	f32 t = s.started && !s.over && !game_paused ? clampf((f32)(s.step_t / s.interval), 0, 1) : 0;
	f32 ptx[MAXLEN + 1], pty[MAXLEN + 1];
	int np = 0;
	ptx[np] = (f32)s.x[0] + (f32)s.ndx * t, pty[np++] = (f32)s.y[0] + (f32)s.ndy * t;
	for (int i = 0; i < s.len - 1; i++)
		ptx[np] = (f32)s.x[i], pty[np++] = (f32)s.y[i];
	if (s.len > 1) {
		int l = s.len - 1;
		ptx[np] = (f32)s.x[l] + ((f32)s.x[l - 1] - (f32)s.x[l]) * t;
		pty[np++] = (f32)s.y[l] + ((f32)s.y[l - 1] - (f32)s.y[l]) * t;
	}
	for (int i = np - 1; i > 0; i--) {
		f32 k = (f32)i / (f32)MAX(1, np - 1);
		Color c = color_mix(color_hex("#7af07a"), color_hex("#2fb85a"), k);
		f32 inset = cs * (0.1f + 0.08f * k);
		f32 x0 = minf(ptx[i], ptx[i - 1]), x1 = maxf(ptx[i], ptx[i - 1]), y0 = minf(pty[i], pty[i - 1]), y1 = maxf(pty[i], pty[i - 1]);
		Rect seg = rect(field.x + x0 * cs + inset, field.y + y0 * cs + inset, (x1 - x0 + 1) * cs - 2 * inset, (y1 - y0 + 1) * cs - 2 * inset);
		ui_round(ui, seg, cs * 0.32f, c);
	}
	/* Head with eyes looking where it goes. */
	f32 hx = field.x + (ptx[0] + 0.5f) * cs, hy = field.y + (pty[0] + 0.5f) * cs;
	ui_circle(ui, hx, hy, cs * (0.46f + 0.08f * s.eat_pulse), color_hex("#8cf58c"));
	for (int e = -1; e <= 1; e += 2) {
		f32 ex = hx + (f32)s.ndx * cs * 0.15f + (f32)(s.ndy ? e : 0) * cs * 0.18f;
		f32 ey = hy + (f32)s.ndy * cs * 0.15f + (f32)(s.ndx ? e : 0) * cs * 0.18f;
		ui_circle(ui, ex, ey, cs * 0.12f, COLOR_WHITE);
		ui_circle(ui, ex + (f32)s.ndx * cs * 0.04f, ey + (f32)s.ndy * cs * 0.04f, cs * 0.06f, color_hex("#10301a"));
	}
	if (s.over) {
		char sub[96];
		fmt(sub, sizeof sub, s.new_best ? "A new best: %d points!" : "You scored %d.", s.score);
		if (game_overlay(ui, field, "Game Over", sub, "Play Again"))
			start();
	} else if (game_paused) {
		if (game_overlay(ui, field, "Paused", "The snake waits for you.", "Resume"))
			game_paused = false;
	} else if (!s.started) {
		Rect m = center_in(field, 320, 44);
		ui_round(ui, m, 22, color_hex("#00000060"));
		ui_text_in(ui, ui_font(ui, 15, 700), m, "Press an arrow key to start", COLOR_WHITE, ALIGN_CENTER);
	}
}

static void preview(Ui *ui, Rect r)
{
	f32 cs = minf(r.h / 5, 26);
	f32 ox = r.x + (r.w - cs * 9) / 2, oy = r.y + (r.h - cs * 4) / 2;
	static const int px[] = { 6, 5, 4, 3, 3, 3, 2, 1 }, py[] = { 1, 1, 1, 1, 2, 3, 3, 3 };
	for (int i = 7; i >= 0; i--) {
		Color c = color_mix(color_hex("#7af07a"), color_hex("#2fb85a"), (f32)i / 7);
		ui_round(ui, rect(ox + (f32)px[i] * cs + 2, oy + (f32)py[i] * cs + 2, cs - 4, cs - 4), cs * 0.3f, c);
		if (i)
			ui_round(ui, rect(ox + (f32)MIN(px[i], px[i - 1]) * cs + 2, oy + (f32)MIN(py[i], py[i - 1]) * cs + 2,
					  (f32)(ABS(px[i] - px[i - 1]) + 1) * cs - 4, (f32)(ABS(py[i] - py[i - 1]) + 1) * cs - 4),
				 cs * 0.3f, c);
	}
	paint_apple(ui, ox + 8.5f * cs, oy + 1.5f * cs, cs * 0.38f);
}

const Game GAME_SNAKE = { "snake", "Snake", "Eat the apples. Don't bite yourself.", "Arrow keys or WASD to steer", start, frame, preview, 0xff1f7a45,
			  0xff0d3a20 };
