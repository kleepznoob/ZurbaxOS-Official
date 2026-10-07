/* Breakout: bounce the ball off the paddle to clear the bricks. The ball
 * leaves the paddle at an angle set by where it hits; three lives; each
 * level has its own wall and a slightly faster ball. Mouse or arrow keys. */
#include "arcade.h"

#define COLS 12
#define ROWS 8
#define FW 720.0f   /* field size in game units; drawn scaled to fit */
#define FH 560.0f

static const char *const ROW_COLORS[ROWS] = { "#ff5f6d", "#ff8c42", "#ffc93c", "#9be15d", "#3fd0b6", "#4aa8ff", "#8f7bff", "#e070ff" };

static struct {
	u8 brick[ROWS][COLS];   /* hits left, 0 = gone */
	int left;
	f32 px;                  /* paddle centre */
	f32 bx, by, vx, vy;      /* ball */
	bool stuck;              /* waiting on the paddle */
	int lives, score, level;
	bool over, won_level, new_best;
	f32 shake, flash;
	f32 last_mx;
} k;

static const f32 PADDLE_W = 110, PADDLE_H = 14, PADDLE_Y = FH - 40, BALL_R = 8;

static void build_level(void)
{
	memset(k.brick, 0, sizeof k.brick);
	k.left = 0;
	for (int y = 0; y < ROWS; y++)
		for (int x = 0; x < COLS; x++) {
			bool on;
			switch (k.level % 4) {
			case 1: on = true; break;                                            /* a full wall */
			case 2: on = (x + y) % 2 == 0 || y < 2; break;                        /* checkers */
			case 3: on = ABS(x * 2 + 1 - COLS) <= (ROWS - y) * 2; break;          /* a pyramid */
			default: on = !(x == 0 || x == COLS - 1) && y != 3 && y != 4; break; /* two bands */
			}
			if (on) {
				k.brick[y][x] = (u8)(y < 2 && k.level > 1 ? 2 : 1);
				k.left++;
			}
		}
}

static void serve(void)
{
	k.stuck = true;
	k.bx = k.px;
	k.by = PADDLE_Y - BALL_R - 1;
	k.vx = k.vy = 0;
}

static void start(void)
{
	memset(&k, 0, sizeof k);
	k.lives = 3;
	k.level = 1;
	k.px = FW / 2;
	k.last_mx = -1;
	build_level();
	serve();
}

static f32 ball_speed(void)
{
	return 360 + 40 * (f32)(k.level - 1);
}

static void launch(void)
{
	k.stuck = false;
	f32 a = -PI / 2 + ((f32)(rand_u32() % 100) / 100.0f - 0.5f) * 0.6f;
	k.vx = cosf(a) * ball_speed();
	k.vy = sinf(a) * ball_speed();
}

static void physics(f32 dt)
{
	if (k.stuck) {
		k.bx = k.px;
		return;
	}
	/* Small steps so a fast ball can't pass through a brick. */
	int steps = (int)ceilf(dt * ball_speed() / 4);
	f32 h = dt / (f32)MAX(steps, 1);
	for (int s = 0; s < steps; s++) {
		k.bx += k.vx * h;
		k.by += k.vy * h;
		if (k.bx < BALL_R)
			k.bx = BALL_R, k.vx = fabsf(k.vx), sound_play("click");
		if (k.bx > FW - BALL_R)
			k.bx = FW - BALL_R, k.vx = -fabsf(k.vx), sound_play("click");
		if (k.by < BALL_R)
			k.by = BALL_R, k.vy = fabsf(k.vy), sound_play("click");
		/* The paddle: the angle depends on where it's hit. */
		if (k.vy > 0 && k.by + BALL_R >= PADDLE_Y && k.by + BALL_R <= PADDLE_Y + PADDLE_H + 6 && fabsf(k.bx - k.px) <= PADDLE_W / 2 + BALL_R) {
			f32 off = clampf((k.bx - k.px) / (PADDLE_W / 2), -1, 1);
			f32 a = -PI / 2 + off * 1.05f;
			k.vx = cosf(a) * ball_speed();
			k.vy = sinf(a) * ball_speed();
			k.by = PADDLE_Y - BALL_R;
			sound_play("pop");
		}
		if (k.by > FH + 30) {
			k.lives--;
			k.shake = 1;
			sound_play("lose");
			rumble(0.9f, 0.4f, 350);
			if (k.lives <= 0) {
				k.over = true;
				k.new_best = submit_score("breakout", k.score);
			} else {
				serve();
			}
			return;
		}
		/* Bricks. */
		f32 bw = FW / COLS, bh = 24, top = 60;
		int cx = (int)(k.bx / bw), cy = (int)((k.by - top) / bh);
		for (int y = cy - 1; y <= cy + 1; y++)
			for (int x = cx - 1; x <= cx + 1; x++) {
				if (x < 0 || y < 0 || x >= COLS || y >= ROWS || !k.brick[y][x])
					continue;
				f32 l = (f32)x * bw, t = top + (f32)y * bh;
				f32 nx = clampf(k.bx, l, l + bw), ny = clampf(k.by, t, t + bh);
				f32 dx = k.bx - nx, dy = k.by - ny;
				if (dx * dx + dy * dy > BALL_R * BALL_R)
					continue;
				/* Bounce off the side it came through. */
				if (fabsf(dx) > fabsf(dy))
					k.vx = dx > 0 ? fabsf(k.vx) : -fabsf(k.vx);
				else
					k.vy = dy > 0 ? fabsf(k.vy) : -fabsf(k.vy);
				if (--k.brick[y][x] == 0) {
					k.left--;
					k.score += 10 * (ROWS - y);
					sound_play("hit");
				} else {
					sound_play("click");
				}
				k.flash = 1;
				if (k.left == 0) {
					k.won_level = true;
					sound_play("win");
				}
				return;
			}
	}
}

static void frame(Ui *ui, Rect r)
{
	f32 dt = minf(ui->dt, 0.05f);
	f32 scale = minf(r.w / FW, (r.h - 40) / FH);
	Rect field = rect(r.x + (r.w - FW * scale) / 2, r.y + 40, FW * scale, FH * scale);
	if (k.shake > 0)
		field.x += sinf((f32)ui->now * 60) * 6 * k.shake;
	k.shake = maxf(0, k.shake - dt * 3);
	k.flash = maxf(0, k.flash - dt * 6);
	bool playing = !k.over && !k.won_level && !game_paused;
	if (playing) {
		/* Mouse moves the paddle when it moves; otherwise the arrow keys do. */
		if (ui->pointer_inside && ui->mx != k.last_mx && ui_mouse_in(ui, field)) {
			k.px = (ui->mx - field.x) / scale;
			k.last_mx = ui->mx;
		}
		f32 speed = 620 * dt;
		if (key_down(KEY_LEFT) || key_down(KEY_A))
			k.px -= speed;
		if (key_down(KEY_RIGHT) || key_down(KEY_D))
			k.px += speed;
		/* the stick steers with how far it's pushed; the D-pad at full speed */
		f32 stick = ui_pad_axis(ui, PAD_LX);
		if (ui_pad_down(ui, PAD_LEFT))
			stick = -1;
		if (ui_pad_down(ui, PAD_RIGHT))
			stick = 1;
		k.px += stick * speed * 1.15f;
		k.px = clampf(k.px, PADDLE_W / 2, FW - PADDLE_W / 2);
		if (k.stuck && (key_hit(ui, KEY_SPACE) || key_hit(ui, KEY_UP) || pad_hit(ui, PAD_A) || (ui->pressed[BUTTON_LEFT] && ui_mouse_in(ui, field))))
			launch();
		physics(dt);
	}
	/* Status line. */
	char line[64];
	fmt(line, sizeof line, "Score %d", k.score);
	ui_text_in(ui, ui_font(ui, 20, 800), rect(field.x, r.y + 4, 200, 30), line, COLOR_WHITE, ALIGN_START);
	fmt(line, sizeof line, "Level %d", k.level);
	ui_text_in(ui, ui_font(ui, 16, 700), rect(field.x + field.w / 2 - 100, r.y + 8, 200, 26), line, color_hex("#ffffffa0"), ALIGN_CENTER);
	for (int i = 0; i < 3; i++)
		ui_circle(ui, field.x + field.w - 12 - (f32)i * 22, r.y + 20, 7, i < k.lives ? color_hex("#ff6b81") : color_hex("#ffffff20"));
	/* The field. */
	ui_round(ui, pad(field, -6), 14, color_hex("#0b0d16"));
	ui_gradient(ui, field, 10, color_hex("#141a2e"), color_hex("#0d1020"));
	ui_push_clip(ui, field);
	f32 bw = FW / COLS, bh = 24, top = 60;
	for (int y = 0; y < ROWS; y++)
		for (int x = 0; x < COLS; x++) {
			if (!k.brick[y][x])
				continue;
			Rect b = rect(field.x + ((f32)x * bw + 2) * scale, field.y + (top + (f32)y * bh + 2) * scale, (bw - 4) * scale, (bh - 4) * scale);
			Color c = color_hex(ROW_COLORS[y]);
			if (k.brick[y][x] > 1)
				c = color_lighten(c, -0.25f);
			bevel_cell(ui, b, 5 * scale, c);
		}
	/* Paddle with a soft glow, then the ball and its trail. */
	Rect p = rect(field.x + (k.px - PADDLE_W / 2) * scale, field.y + PADDLE_Y * scale, PADDLE_W * scale, PADDLE_H * scale);
	ui_round(ui, pad(p, -6 * scale), 12 * scale, color_hex("#4aa8ff22"));
	ui_gradient(ui, p, PADDLE_H * scale / 2, color_hex("#e6f0ff"), color_hex("#8fb4ff"));
	f32 bx = field.x + k.bx * scale, by = field.y + k.by * scale;
	if (!k.stuck)
		for (int i = 1; i <= 4; i++)
			ui_circle(ui, bx - k.vx * 0.012f * (f32)i * scale, by - k.vy * 0.012f * (f32)i * scale, BALL_R * scale * (1 - 0.18f * (f32)i),
				  color_with_alpha(color_hex("#ffd166"), (u8)(80 - i * 18)));
	ui_circle(ui, bx, by, BALL_R * scale * 1.8f, color_hex("#ffd16630"));
	ui_circle(ui, bx, by, BALL_R * scale, color_mix(color_hex("#ffd166"), COLOR_WHITE, k.flash * 0.6f));
	ui_pop_clip(ui);
	if (k.stuck && playing) {
		Rect m = center_in(rect(field.x, field.y + field.h * 0.55f, field.w, 40), 300, 36);
		ui_round(ui, m, 18, color_hex("#00000070"));
		ui_text_in(ui, ui_font(ui, 14, 700), m, pad_connected(ui) ? "Press A to launch" : "Click or press Space to launch", COLOR_WHITE,
			   ALIGN_CENTER);
	}
	if (k.won_level) {
		char sub[64];
		fmt(sub, sizeof sub, "Level %d cleared. %d points so far.", k.level, k.score);
		if (game_overlay(ui, field, "Cleared!", sub, "Next Level")) {
			k.won_level = false;
			k.level++;
			build_level();
			serve();
		}
	} else if (k.over) {
		char sub[96];
		fmt(sub, sizeof sub, k.new_best ? "A new best: %d points!" : "You reached level %d with %d points.", k.new_best ? k.score : k.level, k.score);
		if (game_overlay(ui, field, "Game Over", sub, "Play Again"))
			start();
	} else if (game_paused) {
		if (game_overlay(ui, field, "Paused", "The ball is hanging in the air.", "Resume"))
			game_paused = false;
	}
}

static void preview(Ui *ui, Rect r)
{
	f32 bw = minf(r.w / 8, 34), bh = bw * 0.42f;
	f32 ox = r.x + (r.w - bw * 7) / 2, oy = r.y + 6;
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 7; x++)
			if ((x + y) % 3 != 1 || y == 0)
				bevel_cell(ui, rect(ox + (f32)x * bw + 2, oy + (f32)y * bh + 2, bw - 4, bh - 4), 3, color_hex(ROW_COLORS[y * 2]));
	f32 py = r.y + r.h - 14;
	ui_gradient(ui, rect(r.x + r.w / 2 - 40, py, 80, 9), 4.5f, color_hex("#e6f0ff"), color_hex("#8fb4ff"));
	ui_circle(ui, r.x + r.w / 2 + 30, py - 34, 13, color_hex("#ffd16630"));
	ui_circle(ui, r.x + r.w / 2 + 30, py - 34, 7, color_hex("#ffd166"));
}

const Game GAME_BREAKOUT = { "breakout", "Breakout", "Bounce the ball, break every brick.", "Mouse or ← → to move, Space to launch", "Stick or D-pad to move, A to launch", start, frame, preview,
			     0xff2a3f8a, 0xff121a40 };
