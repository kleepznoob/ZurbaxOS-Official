/* Terminal emulation: the screen grid, scrollback and the escape-code parser. */
#include "term.h"

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET, S_IGNORE_STRING, S_IGNORE_ESC };

#define SCROLLBACK_LINES 5000

static Cell blank(const Term *t)
{
	return (Cell){ ' ', TC_DEFAULT, t->bg, 0 };
}

static Cell *cell(Term *t, int x, int y)
{
#ifdef TERM_CHECKED
	if (x < 0 || x >= t->cols || y < 0 || y >= t->rows)
		die("term: cell %d,%d outside %dx%d", x, y, t->cols, t->rows);
#endif
	return &t->screen[(usize)y * (usize)t->cols + (usize)x];
}

static void clear_cells(Term *t, int x0, int y0, int x1, int y1)
{
	/* Clears [x0, x1) on rows y0..y1 with the current background. */
	Cell b = blank(t);
	for (int y = y0; y <= y1; y++)
		for (int x = (y == y0 ? x0 : 0); x < (y == y1 ? x1 : t->cols); x++)
			*cell(t, x, y) = b;
	t->dirty = true;
}

void term_reset(Term *t)
{
	t->fg = t->bg = TC_DEFAULT;
	t->attr = 0;
	t->cx = t->cy = 0;
	t->pending_wrap = false;
	t->top = 0;
	t->bottom = t->rows - 1;
	t->cursor_visible = true;
	t->autowrap = true;
	t->app_cursor = t->app_keypad = t->bracketed_paste = t->origin = t->insert = false;
	t->mouse_mode = 0;
	t->mouse_sgr = false;
	t->line_drawing = false;
	t->state = S_GROUND;
	clear_cells(t, 0, 0, t->cols, t->rows - 1);
}

void term_init(Term *t, int cols, int rows)
{
	memset(t, 0, sizeof *t);
	t->cols = MAX(cols, 2);
	t->rows = MAX(rows, 2);
	usize n = (usize)t->cols * (usize)t->rows;
	t->main = NEW_ARRAY(Cell, n);
	t->alt = NEW_ARRAY(Cell, n);
	t->wrapped_main = NEW_ARRAY(bool, (usize)t->rows);
	t->wrapped_alt = NEW_ARRAY(bool, (usize)t->rows);
	t->screen = t->main;
	t->wrapped = t->wrapped_main;
	t->sb_cap = SCROLLBACK_LINES;
	t->scrollback = NEW_ARRAY(Line, (usize)t->sb_cap);
	term_reset(t);
	Term *tt = t;
	tt->screen = t->alt;
	clear_cells(t, 0, 0, t->cols, t->rows - 1);
	tt->screen = t->main;
}

void term_free(Term *t)
{
	mem_free(t->main);
	mem_free(t->alt);
	mem_free(t->wrapped_main);
	mem_free(t->wrapped_alt);
	for (int i = 0; i < t->sb_len; i++)
		mem_free(t->scrollback[(t->sb_head + i) % t->sb_cap].cells);
	mem_free(t->scrollback);
	sb_free(&t->osc);
	sb_free(&t->reply);
}

/* ---- Scrollback ------------------------------------------------------------------------ */

static void push_scrollback(Term *t, int y)
{
	if (t->alt_active)
		return;
	Line *l;
	if (t->sb_len < t->sb_cap) {
		l = &t->scrollback[(t->sb_head + t->sb_len) % t->sb_cap];
		t->sb_len++;
	} else {
		l = &t->scrollback[t->sb_head];
		mem_free(l->cells);
		t->sb_head = (t->sb_head + 1) % t->sb_cap;
	}
	/* Drop trailing blanks to save memory. */
	int w = t->cols;
	while (w > 0 && cell(t, w - 1, y)->cp == ' ' && cell(t, w - 1, y)->bg == TC_DEFAULT)
		w--;
	l->cols = w;
	l->cells = w ? NEW_ARRAY(Cell, (usize)w) : NULL;
	if (w)
		memcpy(l->cells, cell(t, 0, y), sizeof(Cell) * (usize)w);
	l->wrapped = t->wrapped[y];
}

const Cell *term_row(const Term *t, int y, int scroll, int *cols)
{
	int idx = y - scroll; /* rows above 0 come from the scrollback */
	if (idx >= 0) {
		*cols = t->cols;
		return &t->screen[(usize)idx * (usize)t->cols];
	}
	int sb = t->sb_len + idx;
	if (sb < 0) {
		*cols = 0;
		return NULL;
	}
	const Line *l = &t->scrollback[(t->sb_head + sb) % t->sb_cap];
	*cols = l->cols;
	return l->cells;
}

/* ---- Scrolling --------------------------------------------------------------------------- */

static void scroll_up(Term *t, int n)
{
	n = MIN(n, t->bottom - t->top + 1);
	for (int k = 0; k < n; k++) {
		if (t->top == 0)
			push_scrollback(t, 0);
		if (t->bottom > t->top) {
			memmove(cell(t, 0, t->top), cell(t, 0, t->top + 1), sizeof(Cell) * (usize)t->cols * (usize)(t->bottom - t->top));
			memmove(&t->wrapped[t->top], &t->wrapped[t->top + 1], sizeof(bool) * (usize)(t->bottom - t->top));
		}
		t->wrapped[t->bottom] = false;
		Cell b = blank(t);
		for (int x = 0; x < t->cols; x++)
			*cell(t, x, t->bottom) = b;
	}
	t->dirty = true;
}

static void scroll_down(Term *t, int n)
{
	n = MIN(n, t->bottom - t->top + 1);
	for (int k = 0; k < n; k++) {
		if (t->bottom > t->top) {
			memmove(cell(t, 0, t->top + 1), cell(t, 0, t->top), sizeof(Cell) * (usize)t->cols * (usize)(t->bottom - t->top));
			memmove(&t->wrapped[t->top + 1], &t->wrapped[t->top], sizeof(bool) * (usize)(t->bottom - t->top));
		}
		t->wrapped[t->top] = false;
		Cell b = blank(t);
		for (int x = 0; x < t->cols; x++)
			*cell(t, x, t->top) = b;
	}
	t->dirty = true;
}

static void line_feed(Term *t)
{
	if (t->cy == t->bottom)
		scroll_up(t, 1);
	else if (t->cy < t->rows - 1)
		t->cy++;
}

/* ---- Printing -------------------------------------------------------------------------------- */

/* DEC special graphics: the line-drawing characters used by some programs. */
static u32 dec_graphics(u32 c)
{
	static const u16 map[] = {
		0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
		0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
	};
	if (c >= 0x60 && c <= 0x7e)
		return map[c - 0x60];
	return c;
}

static void put_char(Term *t, u32 c)
{
	if (t->line_drawing)
		c = dec_graphics(c);
	if (t->pending_wrap) {
		if (t->autowrap) {
			t->wrapped[t->cy] = true;
			t->cx = 0;
			line_feed(t);
		}
		t->pending_wrap = false;
	}
	if (t->insert && t->cx < t->cols - 1)
		memmove(cell(t, t->cx + 1, t->cy), cell(t, t->cx, t->cy), sizeof(Cell) * (usize)(t->cols - t->cx - 1));
	*cell(t, t->cx, t->cy) = (Cell){ c, t->fg, t->bg, t->attr };
	if (t->cx == t->cols - 1)
		t->pending_wrap = true;
	else
		t->cx++;
	t->dirty = true;
}

/* ---- Escape sequences --------------------------------------------------------------------- */

static int param(const Term *t, int i, int fallback)
{
	return i < t->nparams && t->params[i] > 0 ? t->params[i] : fallback;
}

static void move_to(Term *t, int x, int y)
{
	int top = t->origin ? t->top : 0, bottom = t->origin ? t->bottom : t->rows - 1;
	t->cx = CLAMP(x, 0, t->cols - 1);
	t->cy = CLAMP(y + (t->origin ? top : 0), top, bottom);
	t->pending_wrap = false;
}

static u32 parse_ext_color(const Term *t, int *i)
{
	/* 38;5;N or 38;2;R;G;B */
	if (*i + 1 < t->nparams && t->params[*i + 1] == 5 && *i + 2 < t->nparams) {
		u32 c = TC_INDEX(t->params[*i + 2] & 255);
		*i += 2;
		return c;
	}
	if (*i + 1 < t->nparams && t->params[*i + 1] == 2 && *i + 4 < t->nparams) {
		u32 c = TC_RGB(t->params[*i + 2] & 255, t->params[*i + 3] & 255, t->params[*i + 4] & 255);
		*i += 4;
		return c;
	}
	return TC_DEFAULT;
}

static void sgr(Term *t)
{
	if (t->nparams == 0) {
		t->fg = t->bg = TC_DEFAULT;
		t->attr = 0;
		return;
	}
	for (int i = 0; i < t->nparams; i++) {
		int p = t->params[i];
		if (p < 0)
			p = 0;
		switch (p) {
		case 0: t->fg = t->bg = TC_DEFAULT, t->attr = 0; break;
		case 1: t->attr |= A_BOLD; break;
		case 2: t->attr |= A_DIM; break;
		case 3: t->attr |= A_ITALIC; break;
		case 4: t->attr |= A_UNDERLINE; break;
		case 7: t->attr |= A_REVERSE; break;
		case 8: t->attr |= A_HIDDEN; break;
		case 9: t->attr |= A_STRIKE; break;
		case 21:
		case 22: t->attr &= ~(u16)(A_BOLD | A_DIM); break;
		case 23: t->attr &= ~(u16)A_ITALIC; break;
		case 24: t->attr &= ~(u16)A_UNDERLINE; break;
		case 27: t->attr &= ~(u16)A_REVERSE; break;
		case 28: t->attr &= ~(u16)A_HIDDEN; break;
		case 29: t->attr &= ~(u16)A_STRIKE; break;
		case 38: t->fg = parse_ext_color(t, &i); break;
		case 39: t->fg = TC_DEFAULT; break;
		case 48: t->bg = parse_ext_color(t, &i); break;
		case 49: t->bg = TC_DEFAULT; break;
		default:
			if (p >= 30 && p <= 37)
				t->fg = TC_INDEX(p - 30);
			else if (p >= 40 && p <= 47)
				t->bg = TC_INDEX(p - 40);
			else if (p >= 90 && p <= 97)
				t->fg = TC_INDEX(p - 90 + 8);
			else if (p >= 100 && p <= 107)
				t->bg = TC_INDEX(p - 100 + 8);
		}
	}
}

static void switch_screen(Term *t, bool alt)
{
	if (alt == t->alt_active)
		return;
	t->alt_active = alt;
	t->screen = alt ? t->alt : t->main;
	t->wrapped = alt ? t->wrapped_alt : t->wrapped_main;
	if (alt) {
		u32 bg = t->bg;
		t->bg = TC_DEFAULT;
		clear_cells(t, 0, 0, t->cols, t->rows - 1);
		t->bg = bg;
	}
	t->dirty = true;
}

static void save_cursor(Term *t)
{
	t->saved.cx = t->cx, t->saved.cy = t->cy;
	t->saved.fg = t->fg, t->saved.bg = t->bg, t->saved.attr = t->attr;
	t->saved.line_drawing = t->line_drawing;
}

static void restore_cursor(Term *t)
{
	t->cx = MIN(t->saved.cx, t->cols - 1), t->cy = MIN(t->saved.cy, t->rows - 1);
	t->fg = t->saved.fg, t->bg = t->saved.bg, t->attr = t->saved.attr;
	t->line_drawing = t->saved.line_drawing;
	t->pending_wrap = false;
}

static void set_mode(Term *t, bool on)
{
	for (int i = 0; i < MAX(t->nparams, 1); i++) {
		int p = t->params[i];
		if (t->private_mark == '?') {
			switch (p) {
			case 1: t->app_cursor = on; break;
			case 6: t->origin = on, move_to(t, 0, 0); break;
			case 7: t->autowrap = on; break;
			case 25: t->cursor_visible = on; break;
			case 47:
			case 1047: switch_screen(t, on); break;
			case 1049:
				if (on) {
					save_cursor(t);
					switch_screen(t, true);
				} else {
					switch_screen(t, false);
					restore_cursor(t);
				}
				break;
			case 1048: on ? save_cursor(t) : restore_cursor(t); break;
			case 1000:
			case 1002:
			case 1003: t->mouse_mode = on ? p : 0; break;
			case 1006: t->mouse_sgr = on; break;
			case 2004: t->bracketed_paste = on; break;
			}
		} else if (p == 4) {
			t->insert = on;
		}
	}
}

static void csi_dispatch(Term *t, char f)
{
	int n = param(t, 0, 1);
	switch (f) {
	case 'A': move_to(t, t->cx, t->cy - n - (t->origin ? t->top : 0)); break;
	case 'B': move_to(t, t->cx, t->cy + n - (t->origin ? t->top : 0)); break;
	case 'C': move_to(t, t->cx + n, t->cy - (t->origin ? t->top : 0)); break;
	case 'D': move_to(t, t->cx - n, t->cy - (t->origin ? t->top : 0)); break;
	case 'E': move_to(t, 0, t->cy + n - (t->origin ? t->top : 0)); break;
	case 'F': move_to(t, 0, t->cy - n - (t->origin ? t->top : 0)); break;
	case 'G':
	case '`': move_to(t, n - 1, t->cy - (t->origin ? t->top : 0)); break;
	case 'd': move_to(t, t->cx, n - 1); break;
	case 'H':
	case 'f': move_to(t, param(t, 1, 1) - 1, param(t, 0, 1) - 1); break;
	case 'J': {
		int m = t->nparams ? t->params[0] : 0;
		if (m == 0)
			clear_cells(t, t->cx, t->cy, t->cols, t->rows - 1);
		else if (m == 1)
			clear_cells(t, 0, 0, t->cx + 1, t->cy);
		else if (m == 2 || m == 3) {
			clear_cells(t, 0, 0, t->cols, t->rows - 1);
			if (m == 3 && !t->alt_active) {
				for (int i = 0; i < t->sb_len; i++)
					mem_free(t->scrollback[(t->sb_head + i) % t->sb_cap].cells);
				t->sb_len = t->sb_head = 0;
			}
		}
		break;
	}
	case 'K': {
		int m = t->nparams ? t->params[0] : 0;
		if (m == 0)
			clear_cells(t, t->cx, t->cy, t->cols, t->cy);
		else if (m == 1)
			clear_cells(t, 0, t->cy, t->cx + 1, t->cy);
		else
			clear_cells(t, 0, t->cy, t->cols, t->cy);
		break;
	}
	case 'L':
		if (t->cy >= t->top && t->cy <= t->bottom) {
			int top = t->top;
			t->top = t->cy;
			scroll_down(t, n);
			t->top = top;
		}
		break;
	case 'M':
		if (t->cy >= t->top && t->cy <= t->bottom) {
			int top = t->top;
			t->top = t->cy;
			/* Delete lines: shift up within the region, without keeping history. */
			for (int k = 0; k < MIN(n, t->bottom - t->top + 1); k++) {
				if (t->bottom > t->top)
					memmove(cell(t, 0, t->top), cell(t, 0, t->top + 1), sizeof(Cell) * (usize)t->cols * (usize)(t->bottom - t->top));
				Cell b = blank(t);
				for (int x = 0; x < t->cols; x++)
					*cell(t, x, t->bottom) = b;
			}
			t->top = top;
			t->dirty = true;
		}
		break;
	case 'P': {
		n = MIN(n, t->cols - t->cx);
		if (t->cols - t->cx - n > 0)
			memmove(cell(t, t->cx, t->cy), cell(t, t->cx + n, t->cy), sizeof(Cell) * (usize)(t->cols - t->cx - n));
		clear_cells(t, t->cols - n, t->cy, t->cols, t->cy);
		break;
	}
	case '@': {
		n = MIN(n, t->cols - t->cx);
		if (t->cols - t->cx - n > 0)
			memmove(cell(t, t->cx + n, t->cy), cell(t, t->cx, t->cy), sizeof(Cell) * (usize)(t->cols - t->cx - n));
		clear_cells(t, t->cx, t->cy, t->cx + n, t->cy);
		break;
	}
	case 'X': clear_cells(t, t->cx, t->cy, MIN(t->cols, t->cx + n), t->cy); break;
	case 'S': scroll_up(t, n); break;
	case 'T': scroll_down(t, n); break;
	case 'r': {
		int top = param(t, 0, 1) - 1, bottom = param(t, 1, t->rows) - 1;
		if (top < bottom && bottom < t->rows) {
			t->top = top;
			t->bottom = bottom;
			move_to(t, 0, 0);
		}
		break;
	}
	case 'm': sgr(t); break;
	case 'h': set_mode(t, true); break;
	case 'l': set_mode(t, false); break;
	case 's': save_cursor(t); break;
	case 'u': restore_cursor(t); break;
	case 'n':
		if (t->nparams && t->params[0] == 6)
			sb_fmt(&t->reply, "\x1b[%d;%dR", t->cy + 1, t->cx + 1);
		else if (t->nparams && t->params[0] == 5)
			sb_adds(&t->reply, "\x1b[0n");
		break;
	case 'c':
		if (t->private_mark == '>')
			sb_adds(&t->reply, "\x1b[>1;10;0c");
		else
			sb_adds(&t->reply, "\x1b[?62;22c");
		break;
	case 't': break; /* window manipulation: ignored */
	case 'q': break; /* cursor style */
	}
}

static void esc_dispatch(Term *t, char c)
{
	switch (c) {
	case '7': save_cursor(t); break;
	case '8': restore_cursor(t); break;
	case 'D': line_feed(t); break;
	case 'E': t->cx = 0, line_feed(t); break;
	case 'M':
		if (t->cy == t->top)
			scroll_down(t, 1);
		else if (t->cy > 0)
			t->cy--;
		break;
	case 'c': term_reset(t); break;
	case '=': t->app_keypad = true; break;
	case '>': t->app_keypad = false; break;
	}
}

static void osc_dispatch(Term *t)
{
	const char *s = sb_str(&t->osc);
	int code = (int)str_to_int(s, &s);
	if (*s == ';')
		s++;
	if (code == 0 || code == 2)
		str_copy(t->title, s, sizeof t->title);
	sb_clear(&t->osc);
}

static void control(Term *t, u8 c)
{
	switch (c) {
	case '\a': t->bell = true; break;
	case '\b':
		if (t->cx > 0)
			t->cx--;
		t->pending_wrap = false;
		break;
	case '\t': {
		int next = (t->cx / 8 + 1) * 8;
		t->cx = MIN(next, t->cols - 1);
		break;
	}
	case '\n':
	case '\v':
	case '\f':
		line_feed(t);
		t->pending_wrap = false;
		break;
	case '\r':
		t->cx = 0;
		t->pending_wrap = false;
		break;
	case 0x0e:
	case 0x0f: break;
	}
}

void term_feed(Term *t, const u8 *data, usize len)
{
	for (usize i = 0; i < len; i++) {
		u8 c = data[i];
		switch (t->state) {
		case S_GROUND:
			if (t->utf8_left) {
				if ((c & 0xC0) == 0x80) {
					t->utf8_cp = (t->utf8_cp << 6) | (c & 0x3f);
					if (--t->utf8_left == 0)
						put_char(t, t->utf8_cp);
					continue;
				}
				t->utf8_left = 0;
				put_char(t, 0xfffd);
			}
			if (c == 0x1b)
				t->state = S_ESC;
			else if (c < 0x20 || c == 0x7f)
				control(t, c);
			else if (c < 0x80)
				put_char(t, c);
			else if ((c & 0xE0) == 0xC0)
				t->utf8_cp = c & 0x1f, t->utf8_left = 1;
			else if ((c & 0xF0) == 0xE0)
				t->utf8_cp = c & 0x0f, t->utf8_left = 2;
			else if ((c & 0xF8) == 0xF0)
				t->utf8_cp = c & 0x07, t->utf8_left = 3;
			break;
		case S_ESC:
			if (c == '[') {
				t->state = S_CSI;
				t->nparams = 0;
				t->private_mark = 0;
				t->intermediate = 0;
				memset(t->params, 0, sizeof t->params);
			} else if (c == ']') {
				t->state = S_OSC;
				sb_clear(&t->osc);
			} else if (c == '(' || c == ')') {
				t->intermediate = (char)c;
				t->state = S_CHARSET;
			} else if (c == 'P' || c == '_' || c == '^' || c == 'X') {
				t->state = S_IGNORE_STRING;
			} else {
				esc_dispatch(t, (char)c);
				t->state = S_GROUND;
			}
			break;
		case S_CHARSET:
			if (t->intermediate == '(')
				t->line_drawing = c == '0';
			t->state = S_GROUND;
			break;
		case S_CSI:
			if (c >= '0' && c <= '9') {
				if (t->nparams == 0)
					t->nparams = 1;
				int *p = &t->params[t->nparams - 1];
				*p = MIN(*p * 10 + (c - '0'), 99999);
			} else if (c == ';' || c == ':') {
				if (t->nparams == 0)
					t->nparams = 1;
				if (t->nparams < (int)ARRAY_LEN(t->params))
					t->params[t->nparams++] = 0;
			} else if (c == '?' || c == '>' || c == '=' || c == '<') {
				t->private_mark = (char)c;
			} else if (c >= 0x20 && c <= 0x2f) {
				t->intermediate = (char)c;
			} else if (c >= 0x40 && c <= 0x7e) {
				if (!t->intermediate)
					csi_dispatch(t, (char)c);
				t->state = S_GROUND;
			} else if (c == 0x1b) {
				t->state = S_ESC;
			} else if (c < 0x20) {
				control(t, c);
			}
			break;
		case S_OSC:
			if (c == '\a')
				osc_dispatch(t), t->state = S_GROUND;
			else if (c == 0x1b)
				t->state = S_OSC_ESC;
			else if (t->osc.len < 4096)
				sb_addc(&t->osc, (char)c);
			break;
		case S_OSC_ESC:
			osc_dispatch(t);
			t->state = c == '\\' ? S_GROUND : S_ESC;
			if (t->state == S_ESC)
				i--;
			break;
		case S_IGNORE_STRING:
			if (c == 0x1b)
				t->state = S_IGNORE_ESC;
			else if (c == '\a')
				t->state = S_GROUND;
			break;
		case S_IGNORE_ESC:
			t->state = c == '\\' ? S_GROUND : S_IGNORE_STRING;
			break;
		}
	}
	t->generation++;
}

/* Resizing keeps the text at the top-left and the cursor on its line. */
void term_resize(Term *t, int cols, int rows)
{
	cols = MAX(cols, 2);
	rows = MAX(rows, 2);
	if (cols == t->cols && rows == t->rows)
		return;
	/* When the screen gets shorter, lines above the cursor move to the scrollback. */
	int drop = 0;
	if (!t->alt_active && t->cy >= rows)
		drop = t->cy - rows + 1;
	for (int y = 0; y < drop; y++)
		push_scrollback(t, y);
	Cell *screens[2] = { t->main, t->alt };
	bool *wraps[2] = { t->wrapped_main, t->wrapped_alt };
	for (int s = 0; s < 2; s++) {
		Cell *n = NEW_ARRAY(Cell, (usize)cols * (usize)rows);
		bool *w = NEW_ARRAY(bool, (usize)rows);
		int skip = s == 0 ? drop : 0;
		for (int y = 0; y < rows; y++) {
			for (int x = 0; x < cols; x++) {
				int sy = y + skip;
				n[(usize)y * (usize)cols + (usize)x] = (sy < t->rows && x < t->cols) ? screens[s][(usize)sy * (usize)t->cols + (usize)x] :
													 (Cell){ ' ', TC_DEFAULT, TC_DEFAULT, 0 };
			}
			w[y] = y + skip < t->rows ? wraps[s][y + skip] : false;
		}
		mem_free(screens[s]);
		mem_free(wraps[s]);
		screens[s] = n;
		wraps[s] = w;
	}
	t->main = screens[0];
	t->alt = screens[1];
	t->wrapped_main = wraps[0];
	t->wrapped_alt = wraps[1];
	t->screen = t->alt_active ? t->alt : t->main;
	t->wrapped = t->alt_active ? t->wrapped_alt : t->wrapped_main;
	t->cy = CLAMP(t->cy - drop, 0, rows - 1);
	t->cx = CLAMP(t->cx, 0, cols - 1);
	t->cols = cols;
	t->rows = rows;
	t->top = 0;
	t->bottom = rows - 1;
	t->pending_wrap = false;
	t->dirty = true;
	t->generation++;
}
