/* Shared by the Arcade's games. */
#pragma once

#include "zui.h"

typedef struct {
	const char *id, *name, *tagline, *controls;
	void (*start)(void);
	void (*frame)(Ui *ui, Rect r);   /* plays and draws the game in r */
	void (*preview)(Ui *ui, Rect r); /* art for its card in the hub */
	Color top, bottom;               /* card colours */
} Game;

extern const Game GAME_BLOCKS, GAME_SNAKE, GAME_2048, GAME_MINES, GAME_BREAKOUT;

/* Scores are kept per game in ~/.local/share/zurbax/arcade.conf. */
int best_score(const char *id);
bool submit_score(const char *id, int score); /* true if it's a new best */

/* Keys held down right now (for continuous movement). */
bool key_down(u32 code);
/* A key press this frame (consumed). */
bool key_hit(Ui *ui, u32 code);

/* A dimmed panel over the game with a title, a line and a button; returns
 * true when the button (or Enter / Space) is pressed. */
bool game_overlay(Ui *ui, Rect r, const char *title, const char *sub, const char *button);

/* A score box for the side panels. */
void stat_box(Ui *ui, Rect r, const char *label, const char *value);

/* Rounded cell with a soft bevel, the look of every game piece. */
void bevel_cell(Ui *ui, Rect r, f32 radius, Color c);

extern bool game_paused;
