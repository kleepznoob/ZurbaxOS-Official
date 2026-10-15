/*
 * Pixfont: a 5×7 font of square pixels, for the pixel style's clock,
 * numbers and names. Capitals, figures and a little punctuation; small
 * letters are drawn as capitals and anything else as a space. Each pixel
 * is a square of cell logical pixels, laid on whole device pixels.
 */
#include "zui.h"

static const char *const DIGITS[10][7] = {
	{ " XXX ", "X   X", "X  XX", "X X X", "XX  X", "X   X", " XXX " }, { "  X  ", " XX  ", "  X  ", "  X  ", "  X  ", "  X  ", " XXX " },
	{ " XXX ", "X   X", "    X", "   X ", "  X  ", " X   ", "XXXXX" }, { "XXXXX", "   X ", "  X  ", "   X ", "    X", "X   X", " XXX " },
	{ "   X ", "  XX ", " X X ", "X  X ", "XXXXX", "   X ", "   X " }, { "XXXXX", "X    ", "XXXX ", "    X", "    X", "X   X", " XXX " },
	{ "  XX ", " X   ", "X    ", "XXXX ", "X   X", "X   X", " XXX " }, { "XXXXX", "    X", "   X ", "  X  ", " X   ", " X   ", " X   " },
	{ " XXX ", "X   X", "X   X", " XXX ", "X   X", "X   X", " XXX " }, { " XXX ", "X   X", "X   X", " XXXX", "    X", "   X ", " XX  " },
};

static const char *const LETTERS[26][7] = {
	{ " XXX ", "X   X", "X   X", "XXXXX", "X   X", "X   X", "X   X" }, { "XXXX ", "X   X", "X   X", "XXXX ", "X   X", "X   X", "XXXX " },
	{ " XXX ", "X   X", "X    ", "X    ", "X    ", "X   X", " XXX " }, { "XXXX ", "X   X", "X   X", "X   X", "X   X", "X   X", "XXXX " },
	{ "XXXXX", "X    ", "X    ", "XXXX ", "X    ", "X    ", "XXXXX" }, { "XXXXX", "X    ", "X    ", "XXXX ", "X    ", "X    ", "X    " },
	{ " XXX ", "X   X", "X    ", "X XXX", "X   X", "X   X", " XXXX" }, { "X   X", "X   X", "X   X", "XXXXX", "X   X", "X   X", "X   X" },
	{ " XXX ", "  X  ", "  X  ", "  X  ", "  X  ", "  X  ", " XXX " }, { "  XXX", "   X ", "   X ", "   X ", "   X ", "X  X ", " XX  " },
	{ "X   X", "X  X ", "X X  ", "XX   ", "X X  ", "X  X ", "X   X" }, { "X    ", "X    ", "X    ", "X    ", "X    ", "X    ", "XXXXX" },
	{ "X   X", "XX XX", "X X X", "X X X", "X   X", "X   X", "X   X" }, { "X   X", "X   X", "XX  X", "X X X", "X  XX", "X   X", "X   X" },
	{ " XXX ", "X   X", "X   X", "X   X", "X   X", "X   X", " XXX " }, { "XXXX ", "X   X", "X   X", "XXXX ", "X    ", "X    ", "X    " },
	{ " XXX ", "X   X", "X   X", "X   X", "X X X", "X  X ", " XX X" }, { "XXXX ", "X   X", "X   X", "XXXX ", "X X  ", "X  X ", "X   X" },
	{ " XXXX", "X    ", "X    ", " XXX ", "    X", "    X", "XXXX " }, { "XXXXX", "  X  ", "  X  ", "  X  ", "  X  ", "  X  ", "  X  " },
	{ "X   X", "X   X", "X   X", "X   X", "X   X", "X   X", " XXX " }, { "X   X", "X   X", "X   X", "X   X", "X   X", " X X ", "  X  " },
	{ "X   X", "X   X", "X   X", "X X X", "X X X", "X X X", " X X " }, { "X   X", "X   X", " X X ", "  X  ", " X X ", "X   X", "X   X" },
	{ "X   X", "X   X", " X X ", "  X  ", "  X  ", "  X  ", "  X  " }, { "XXXXX", "    X", "   X ", "  X  ", " X   ", "X    ", "XXXXX" },
};

static const char *const COLON[7] = { "     ", "  X  ", "  X  ", "     ", "  X  ", "  X  ", "     " };
static const char *const PERCENT[7] = { "XX   ", "XX  X", "   X ", "  X  ", " X   ", "X  XX", "   XX" };
static const char *const DOT[7] = { "     ", "     ", "     ", "     ", "     ", " XX  ", " XX  " };
static const char *const DASH[7] = { "     ", "     ", "     ", "XXXXX", "     ", "     ", "     " };

static const char *const *glyph(u32 cp)
{
	if (cp >= '0' && cp <= '9')
		return DIGITS[cp - '0'];
	if (cp >= 'a' && cp <= 'z')
		cp -= 'a' - 'A';
	if (cp >= 0xe0 && cp <= 0xff) /* accented small letters, near enough */
		cp = cp == 0xe7 ? 'C' : cp == 0xf1 ? 'N' : cp >= 0xf2 && cp <= 0xf6 ? 'O' : cp >= 0xe8 && cp <= 0xeb ? 'E' : cp >= 0xec && cp <= 0xef ? 'I' : cp >= 0xf9 && cp <= 0xfc ? 'U' : 'A';
	if (cp >= 'A' && cp <= 'Z')
		return LETTERS[cp - 'A'];
	switch (cp) {
	case ':': return COLON;
	case '%': return PERCENT;
	case '.': return DOT;
	case '-': return DASH;
	default: return NULL;
	}
}

/* a colon sits in three columns, the rest in five, each followed by one blank */
static int advance(u32 cp)
{
	return cp == ':' || cp == '.' ? 4 : 6;
}

f32 ui_pixel_text_width(const char *s, f32 cell)
{
	int cols = 0;
	for (usize i = 0, n = strlen(s); i < n;) {
		u32 cp = 0;
		usize k = utf8_decode(s + i, n - i, &cp);
		i += k ? k : 1;
		cols += advance(cp);
	}
	return cols ? (f32)(cols - 1) * cell : 0;
}

f32 ui_pixel_text(Ui *ui, f32 x, f32 y, const char *s, f32 cell, Color c)
{
	f32 x0 = x;
	for (usize i = 0, n = strlen(s); i < n;) {
		u32 cp = 0;
		usize k = utf8_decode(s + i, n - i, &cp);
		i += k ? k : 1;
		const char *const *g = glyph(cp);
		int shift = cp == ':' || cp == '.' ? 1 : 0; /* narrow ones: their middle columns */
		if (g)
			for (int r = 0; r < 7; r++)
				for (int k = 0; k < 5; k++)
					if (g[r][k] == 'X')
						ui_fill(ui, rect(x + (f32)(k - shift) * cell, y + (f32)r * cell, cell, cell), c);
		x += (f32)advance(cp) * cell;
	}
	return x - x0;
}
