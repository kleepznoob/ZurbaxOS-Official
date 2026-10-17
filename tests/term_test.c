/* Forge's terminal emulator: fed Flatpak's progress output, random escape
 * codes and resizes, with every cell access checked. */
#define TERM_CHECKED 1
#include "../apps/forge/term.c"

static u64 seed = 0x9e3779b97f4a7c15ull;
static u32 rnd(void)
{
	seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
	return (u32)seed;
}

static void feeds(Term *t, const char *s)
{
	term_feed(t, (const u8 *)s, strlen(s));
}

/* What `flatpak install` prints on a terminal: a table it redraws in place
 * (row addressing from a cursor-position query), progress with block
 * characters, colours, and the cursor hidden meanwhile. */
static void flatpak_like(Term *t, int steps)
{
	feeds(t, "\x1b[?25l\x1b[1mLooking for matches\xe2\x80\xa6\x1b[22m\r\n");
	feeds(t, "\x1b[6n");
	for (int s = 0; s < steps; s++) {
		char line[512];
		int row = 3 + (int)(rnd() % 60);
		fmt(line, sizeof line, "\x1b[%d;1H\x1b[0J", row);
		feeds(t, line);
		for (int r = 0; r < 9; r++) {
			fmt(line, sizeof line, "%2d. \x1b[2m[%s]\x1b[22m org.freedesktop.Platform.GL.default  25.08  flathub  ", r + 1,
			    r < s % 9 ? "\xe2\x9c\x93" : r == s % 9 ? "\xe2\x86\x93" : " ");
			feeds(t, line);
			if (r == s % 9) {
				feeds(t, "\x1b[32m");
				for (int k = 0; k < 20; k++)
					feeds(t, k < (s * 3) % 20 ? "\xe2\x96\x88" : "\xe2\x96\x8f");
				feeds(t, "\x1b[0m 986.0\xc2\xa0kB/s");
			}
			feeds(t, "\x1b[K\r\n");
		}
		fmt(line, sizeof line, "Installing %d/8\xe2\x80\xa6 \x1b[1m%3d%%\x1b[22m\r", s % 8 + 1, s % 101);
		feeds(t, line);
		feeds(t, "\x1b]9;4;1;42\x1b\\");
		if (s % 37 == 0)
			term_resize(t, 20 + (int)(rnd() % 200), 5 + (int)(rnd() % 70));
	}
	feeds(t, "\x1b[?25h\r\n");
}

static void random_codes(Term *t, int n)
{
	static const char *const pieces[] = {
		"\x1b[", "\x1b]", "\x1b(", "\x1b)0", "\x1bM", "\x1b" "D", "\x1b" "E", "\x1b" "7", "\x1b" "8", "\x1b" "c", "\x1bP", "\x1b\\",
		"\r", "\n", "\t", "\b", "\a", ";", ":", "?", ">", "1", "9", "25", "99999", "1049", "47", "6", "r", "H", "J", "K", "L", "M",
		"P", "@", "X", "S", "T", "A", "B", "C", "D", "E", "F", "G", "d", "f", "h", "l", "m", "n", "s", "u", "c", "q", "t", "x",
		"\xe2\x96\x88", "\xf0\x9f\x98\x80", "\xc2", "\xff", "abc", " ", "\x1b[38;2;1;2;3m", "\x1b[48;5;200m",
	};
	for (int i = 0; i < n; i++) {
		feeds(t, pieces[rnd() % ARRAY_LEN(pieces)]);
		if (rnd() % 4000 == 0)
			term_resize(t, 2 + (int)(rnd() % 300), 2 + (int)(rnd() % 100));
	}
}

int main(void)
{
	Term t = { 0 };
	term_init(&t, 120, 40);
	flatpak_like(&t, 5000);
	print("term: flatpak progress ok\n");
	/* small screens, where a table is taller than the window */
	term_resize(&t, 30, 6);
	flatpak_like(&t, 2000);
	print("term: flatpak on a small screen ok\n");
	for (int round = 0; round < 40; round++) {
		random_codes(&t, 200000);
		sb_clear(&t.reply);
	}
	print("term: random codes ok\n");
	term_free(&t);
	print("term: all ok\n");
	return 0;
}
