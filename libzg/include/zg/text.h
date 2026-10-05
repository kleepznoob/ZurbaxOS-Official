/*
 * Text rendering with Zurbax Sans, the system typeface.
 *
 * Zurbax Sans is an original geometric monoline design defined as pen
 * strokes rather than outlines, so every weight from Light to Heavy comes
 * from the same drawings. A monospaced variant serves the terminal.
 */
#pragma once

#include "canvas.h"

typedef struct {
	f32 size;     /* pixel size of the em */
	int weight;   /* 300 light, 400 regular, 500 medium, 600 semibold, 700 bold */
	bool mono;
} Font;

static inline Font font(f32 size, int weight) { return (Font){ size, weight, false }; }
static inline Font font_mono(f32 size, int weight) { return (Font){ size, weight, true }; }

f32 font_ascent(Font f);       /* above the baseline, positive */
f32 font_descent(Font f);      /* below the baseline, positive */
f32 font_line_height(Font f);
f32 font_cap_height(Font f);
f32 font_x_height(Font f);

/* Measures n bytes of UTF-8 text. */
f32 text_width(Font f, const char *s, usize n);
static inline f32 text_width_z(Font f, const char *s) { return text_width(f, s, __builtin_strlen(s)); }
/* Pen advance for one code point (including kerning with `prev`, 0 if none). */
f32 glyph_advance(Font f, u32 cp, u32 prev);

/* Draws text with its baseline at y. Returns the x after the last glyph. */
f32 draw_text(Canvas *c, Font f, f32 x, f32 y, const char *s, usize n, Color color);
static inline f32 draw_text_z(Canvas *c, Font f, f32 x, f32 y, const char *s, Color color)
{
	return draw_text(c, f, x, y, s, __builtin_strlen(s), color);
}

typedef enum { ALIGN_START, ALIGN_CENTER, ALIGN_END } Align;

/* Draws a single line inside `r`, vertically centered, truncated with an
 * ellipsis if it does not fit. */
void draw_text_in(Canvas *c, Font f, Rect r, const char *s, Color color, Align align);

/* Byte offset of the caret position nearest to pixel x (relative to text start). */
usize text_hit(Font f, const char *s, usize n, f32 x);
/* Number of bytes that fit within max_width. */
usize text_fit(Font f, const char *s, usize n, f32 max_width);

typedef struct {
	usize start;  /* byte offset */
	usize len;
	f32 width;
} TextLine;

/* Word-wraps text into lines no wider than max_width. Explicit newlines break.
 * Returns the number of lines (writes at most max_lines). */
int text_wrap(Font f, const char *s, usize n, f32 max_width, TextLine *lines, int max_lines);
/* Height needed to draw wrapped text. */
f32 text_wrap_height(Font f, const char *s, f32 max_width, f32 line_spacing);
/* Draws wrapped text; returns the height used. */
f32 draw_text_wrapped(Canvas *c, Font f, Rect r, const char *s, Color color, Align align, f32 line_spacing);

/* Drops cached glyph images (e.g. after a theme or scale change). */
void font_cache_clear(void);
