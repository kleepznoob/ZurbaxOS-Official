/* Terminal emulation: a grid of character cells driven by VT/xterm escape codes. */
#pragma once

#include "zr.h"

/* Colours: the top byte says what the rest means. */
#define TC_DEFAULT 0xff000000u           /* the theme's foreground or background */
#define TC_INDEX(i) ((u32)(i))            /* palette entry 0..255 */
#define TC_RGB(r, g, b) (0x01000000u | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

enum { A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDERLINE = 8, A_REVERSE = 16, A_STRIKE = 32, A_HIDDEN = 64 };

typedef struct {
	u32 cp;
	u32 fg, bg;
	u16 attr;
} Cell;

typedef struct {
	Cell *cells;
	int cols;
	bool wrapped;          /* continues on the next line */
} Line;

typedef struct {
	int cols, rows;
	Cell *main, *alt, *screen;
	bool *wrapped;         /* per row of the active screen */
	bool *wrapped_main, *wrapped_alt;
	Line *scrollback;      /* ring buffer */
	int sb_cap, sb_len, sb_head;
	int cx, cy;
	bool pending_wrap;
	u32 fg, bg;
	u16 attr;
	struct {
		int cx, cy;
		u32 fg, bg;
		u16 attr;
		bool line_drawing;
	} saved;
	int top, bottom;       /* scroll region, inclusive */
	bool cursor_visible, app_cursor, app_keypad, autowrap, bracketed_paste, alt_active, origin, insert;
	int mouse_mode;        /* 0 off, 1000 clicks, 1002 drags, 1003 motion */
	bool mouse_sgr;
	bool line_drawing;     /* DEC special graphics in G0 */
	/* parser */
	int state;
	int params[16];
	int nparams;
	char private_mark;
	char intermediate;
	StrBuf osc;
	u32 utf8_cp;
	int utf8_left;
	char title[256];
	StrBuf reply;          /* bytes to send back to the program (status reports) */
	bool dirty;
	bool bell;
	u64 generation;        /* bumps on every change, for redraw decisions */
} Term;

void term_init(Term *t, int cols, int rows);
void term_free(Term *t);
void term_resize(Term *t, int cols, int rows);
void term_feed(Term *t, const u8 *data, usize len);
void term_reset(Term *t);
/* A row as seen with `scroll` lines of history above the screen (0 = live). */
const Cell *term_row(const Term *t, int y, int scroll, int *cols);
