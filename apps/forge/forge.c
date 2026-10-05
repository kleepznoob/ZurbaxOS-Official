/*
 * Forge — the terminal.
 *
 * Each tab runs the Flint shell on its own pseudo-terminal. Forge draws the
 * terminal grid with true colour, draws box and block characters itself so
 * lines join without gaps, keeps 5000 lines of history, and supports
 * selection, copy and paste, zoom, and mouse reporting for full-screen
 * programs.
 */
#include "zui.h"
#include "term.h"

typedef struct {
	Term term;
	int master;
	int pid;
	bool exited;
	int exit_status;
	int scroll;             /* lines of history shown above the screen */
	/* selection, in absolute line coordinates (0 = oldest history line) */
	bool selecting, has_sel;
	int sel_ax, sel_ay, sel_bx, sel_by;
} Session;

typedef struct {
	Session *tabs[16];
	int ntabs, current;
	f32 font_size;
	f32 cell_w, cell_h;
	Ui *ui;
	char start_dir[1024];
} Forge;

static Forge fg;

/* ---- Colours ----------------------------------------------------------------------------- */

static const u32 PALETTE16[16] = {
	0xff2b2e36, 0xffe5534b, 0xff57ab5a, 0xffd4a72c, 0xff539bf5, 0xffb083f0, 0xff39c5cf, 0xffc5ced8,
	0xff6e7681, 0xfff47067, 0xff6bc46d, 0xffeac55f, 0xff6cb6ff, 0xffdcbdfb, 0xff56d4dd, 0xfff0f3f6,
};
#define TERM_BG 0xff1b1d23u
#define TERM_FG 0xffd8dee6u

static Color color_of(u32 c, bool fg, u16 attr)
{
	if (c == TC_DEFAULT)
		return fg ? TERM_FG : TERM_BG;
	if ((c >> 24) == 0x01)
		return 0xff000000u | (c & 0xffffff);
	u32 i = c & 0xff;
	if (fg && (attr & A_BOLD) && i < 8)
		i += 8; /* bold text uses the bright colours */
	if (i < 16)
		return PALETTE16[i];
	if (i < 232) {
		i -= 16;
		static const u8 lv[] = { 0, 95, 135, 175, 215, 255 };
		return RGB(lv[i / 36], lv[i / 6 % 6], lv[i % 6]);
	}
	u8 g = (u8)(8 + (i - 232) * 10);
	return RGB(g, g, g);
}

/* ---- Sessions ------------------------------------------------------------------------------ */

static void on_pty(Loop *loop, int fd, u32 events, void *data);

static void set_window_size(Session *s)
{
	struct winsize ws = { (u16)s->term.rows, (u16)s->term.cols, (u16)(s->term.cols * fg.cell_w), (u16)(s->term.rows * fg.cell_h) };
	sys_ioctl(s->master, TIOCSWINSZ, &ws);
}

static Session *session_new(int cols, int rows, const char *cwd)
{
	int m = (int)sys_open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK, 0);
	if (m < 0)
		return NULL;
	int unlock = 0;
	u32 n = 0;
	sys_ioctl(m, TIOCSPTLCK, &unlock);
	if (sys_ioctl(m, TIOCGPTN, &n) < 0) {
		sys_close(m);
		return NULL;
	}
	char path[32];
	fmt(path, sizeof path, "/dev/pts/%u", n);
	int sl = (int)sys_open(path, O_RDWR | O_NOCTTY | O_CLOEXEC, 0);
	if (sl < 0) {
		sys_close(m);
		return NULL;
	}
	Session *s = NEW(Session);
	term_init(&s->term, cols, rows);
	s->master = m;
	set_window_size(s);
	/* The shell gets a terminal-friendly environment. */
	usize nenv = 0;
	while (environ[nenv])
		nenv++;
	char **envp = NEW_ARRAY(char *, nenv + 4);
	int ne = 0;
	envp[ne++] = "TERM=xterm-256color";
	envp[ne++] = "COLORTERM=truecolor";
	envp[ne++] = "TERM_PROGRAM=Forge";
	for (char **e = environ; *e; e++)
		if (!str_starts(*e, "TERM=") && !str_starts(*e, "COLORTERM=") && !str_starts(*e, "TERM_PROGRAM="))
			envp[ne++] = *e;
	envp[ne] = NULL;
	const char *shell = file_exists("/system/bin/flint") ? "/system/bin/flint" : NULL;
	char shell_path[512];
	if (!shell) {
		/* Development: the shell next to this app in the build tree. */
		char *root = zurbax_path("bin/flint");
		str_copy(shell_path, root, sizeof shell_path);
		mem_free(root);
		shell = shell_path;
	}
	char *argv[] = { (char *)shell, NULL };
	SpawnOpts o = { .stdin_fd = sl, .stdout_fd = sl, .stderr_fd = sl, .new_session = true, .controlling_tty = true,
			.cwd = cwd && dir_exists(cwd) ? cwd : home_dir() };
	s->pid = proc_spawn(shell, argv, envp, &o);
	mem_free(envp);
	sys_close(sl);
	if (s->pid < 0) {
		term_free(&s->term);
		sys_close(m);
		mem_free(s);
		return NULL;
	}
	loop_add_fd(ui_loop(fg.ui), m, EPOLLIN, on_pty, s);
	return s;
}

static void session_close(Session *s)
{
	if (!s->exited && s->pid > 0)
		sys_kill(-s->pid, SIGHUP);
	loop_del_fd(ui_loop(fg.ui), s->master);
	sys_close(s->master);
	if (s->pid > 0) {
		int st;
		proc_wait(s->pid, &st, true);
	}
	term_free(&s->term);
	mem_free(s);
}

static void write_pty(Session *s, const void *data, usize n)
{
	if (s->exited)
		return;
	const u8 *p = data;
	while (n) {
		long w = sys_write(s->master, p, n);
		if (w == -EINTR)
			continue;
		if (w == -EAGAIN) {
			struct pollfd pf = { s->master, POLLOUT, 0 };
			sys_poll(&pf, 1, 100);
			continue;
		}
		if (w <= 0)
			return;
		p += w;
		n -= (usize)w;
	}
}

static void on_pty(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(loop), UNUSED(fd);
	Session *s = data;
	u8 buf[65536];
	/* Read what's there, but leave room to draw between large bursts. */
	for (int i = 0; i < 8; i++) {
		long n = sys_read(s->master, buf, sizeof buf);
		if (n > 0) {
			term_feed(&s->term, buf, (usize)n);
			if (s->term.reply.len) {
				write_pty(s, s->term.reply.data, s->term.reply.len);
				sb_clear(&s->term.reply);
			}
			s->scroll = 0;
			continue;
		}
		if (n == -EAGAIN || n == -EINTR)
			break;
		/* EIO: the shell and everything it started have closed the terminal. */
		s->exited = true;
		int st = 0;
		if (s->pid > 0 && proc_wait(s->pid, &st, false) > 0)
			s->exit_status = (st >> 8) & 0xff;
		loop_del_fd(ui_loop(fg.ui), s->master);
		break;
	}
	UNUSED(events);
	ui_wake(fg.ui);
}

static Session *cur(void)
{
	return fg.ntabs ? fg.tabs[fg.current] : NULL;
}

/* ---- Drawing --------------------------------------------------------------------------------- */

/* Rounds a rectangle to whole device pixels so neighbouring cells meet without seams. */
static Rect snap(Ui *ui, Rect r)
{
	f32 k = ui->scale;
	f32 x0 = roundf(r.x * k) / k, y0 = roundf(r.y * k) / k;
	f32 x1 = roundf((r.x + r.w) * k) / k, y1 = roundf((r.y + r.h) * k) / k;
	return rect(x0, y0, x1 - x0, y1 - y0);
}

/* Box drawing: which arms a character has (left, right, up, down) and if they're heavy or double. */
static bool box_arms(u32 cp, int *l, int *r, int *u, int *d)
{
	*l = *r = *u = *d = 0;
	switch (cp) {
	case 0x2500: case 0x2501: *l = *r = cp == 0x2501 ? 2 : 1; return true;
	case 0x2502: case 0x2503: *u = *d = cp == 0x2503 ? 2 : 1; return true;
	case 0x250c: *r = *d = 1; return true;
	case 0x2510: *l = *d = 1; return true;
	case 0x2514: *r = *u = 1; return true;
	case 0x2518: *l = *u = 1; return true;
	case 0x251c: *u = *d = *r = 1; return true;
	case 0x2524: *u = *d = *l = 1; return true;
	case 0x252c: *l = *r = *d = 1; return true;
	case 0x2534: *l = *r = *u = 1; return true;
	case 0x253c: *l = *r = *u = *d = 1; return true;
	case 0x256d: *r = *d = 1; return true; /* rounded corners drawn square-ish */
	case 0x256e: *l = *d = 1; return true;
	case 0x256f: *l = *u = 1; return true;
	case 0x2570: *r = *u = 1; return true;
	case 0x2550: *l = *r = 3; return true;
	case 0x2551: *u = *d = 3; return true;
	case 0x2554: *r = *d = 3; return true;
	case 0x2557: *l = *d = 3; return true;
	case 0x255a: *r = *u = 3; return true;
	case 0x255d: *l = *u = 3; return true;
	case 0x2560: *u = *d = *r = 3; return true;
	case 0x2563: *u = *d = *l = 3; return true;
	case 0x2566: *l = *r = *d = 3; return true;
	case 0x2569: *l = *r = *u = 3; return true;
	case 0x256c: *l = *r = *u = *d = 3; return true;
	}
	return false;
}

static void draw_arm(Ui *ui, f32 x0, f32 y0, f32 x1, f32 y1, int kind, Color c, bool vertical)
{
	f32 w = kind == 2 ? 2.2f : 1.1f;
	if (kind == 3) {
		f32 o = 1.6f;
		if (vertical) {
			ui_fill(ui, rect(x0 - o - 0.5f, y0, 1, y1 - y0), c);
			ui_fill(ui, rect(x0 + o - 0.5f, y0, 1, y1 - y0), c);
		} else {
			ui_fill(ui, rect(x0, y0 - o - 0.5f, x1 - x0, 1), c);
			ui_fill(ui, rect(x0, y0 + o - 0.5f, x1 - x0, 1), c);
		}
		return;
	}
	if (vertical)
		ui_fill(ui, rect(x0 - w / 2, y0, w, y1 - y0), c);
	else
		ui_fill(ui, rect(x0, y0 - w / 2, x1 - x0, w), c);
}

static bool draw_special(Ui *ui, u32 cp, Rect r, Color c)
{
	r = snap(ui, r);
	int l, rr, u, d;
	f32 cx = floorf(r.x + r.w / 2) + 0.5f, cy = floorf(r.y + r.h / 2) + 0.5f;
	if (box_arms(cp, &l, &rr, &u, &d)) {
		f32 ext = 0.6f;
		if (l)
			draw_arm(ui, r.x, cy, cx + ext, cy, l, c, false);
		if (rr)
			draw_arm(ui, cx - ext, cy, r.x + r.w, cy, rr, c, false);
		if (u)
			draw_arm(ui, cx, r.y, cx, cy + ext, u, c, true);
		if (d)
			draw_arm(ui, cx, cy - ext, cx, r.y + r.h, d, c, true);
		return true;
	}
	switch (cp) {
	case 0x2588: ui_fill(ui, r, c); return true;                                        /* █ */
	case 0x2580: ui_fill(ui, rect(r.x, r.y, r.w, r.h / 2), c); return true;              /* ▀ */
	case 0x2584: ui_fill(ui, rect(r.x, r.y + r.h / 2, r.w, r.h / 2), c); return true;    /* ▄ */
	case 0x258c: ui_fill(ui, rect(r.x, r.y, r.w / 2, r.h), c); return true;              /* ▌ */
	case 0x2590: ui_fill(ui, rect(r.x + r.w / 2, r.y, r.w / 2, r.h), c); return true;    /* ▐ */
	case 0x2591: ui_fill(ui, r, color_scale_alpha(c, 0.25f)); return true;               /* ░ */
	case 0x2592: ui_fill(ui, r, color_scale_alpha(c, 0.5f)); return true;                /* ▒ */
	case 0x2593: ui_fill(ui, r, color_scale_alpha(c, 0.75f)); return true;               /* ▓ */
	case 0x2581: case 0x2582: case 0x2583: case 0x2585: case 0x2586: case 0x2587: {    /* ▁..▇ */
		f32 k = (f32)(cp - 0x2580) / 8;
		ui_fill(ui, rect(r.x, r.y + r.h * (1 - k), r.w, r.h * k), c);
		return true;
	}
	}
	return false;
}

static bool in_selection(const Session *s, int x, int ay)
{
	if (!s->has_sel)
		return false;
	int ax0 = s->sel_ax, ay0 = s->sel_ay, ax1 = s->sel_bx, ay1 = s->sel_by;
	if (ay0 > ay1 || (ay0 == ay1 && ax0 > ax1)) {
		int tx = ax0, ty = ay0;
		ax0 = ax1, ay0 = ay1, ax1 = tx, ay1 = ty;
	}
	if (ay < ay0 || ay > ay1)
		return false;
	if (ay == ay0 && x < ax0)
		return false;
	if (ay == ay1 && x >= ax1)
		return false;
	return true;
}

static void draw_terminal(Ui *ui, Session *s, Rect area)
{
	Term *t = &s->term;
	ui_fill(ui, area, TERM_BG);
	Font f = ui_font_mono(ui, fg.font_size, 400), fb = ui_font_mono(ui, fg.font_size, 700);
	f32 cw = fg.cell_w, ch = fg.cell_h;
	f32 x0 = area.x + 10, y0 = area.y + 6;
	f32 base = roundf((ch + font_cap_height(f)) / 2) - 1;
	bool focused = ui->window_focused;
	for (int y = 0; y < t->rows; y++) {
		int cols;
		const Cell *row = term_row(t, y, s->scroll, &cols);
		f32 ry = y0 + (f32)y * ch;
		int abs_y = t->sb_len - s->scroll + y;
		/* Backgrounds first, merged into runs. */
		for (int x = 0; x < t->cols;) {
			Cell c = x < cols && row ? row[x] : (Cell){ ' ', TC_DEFAULT, TC_DEFAULT, 0 };
			u32 bgc = (c.attr & A_REVERSE) ? (c.fg == TC_DEFAULT ? 0xfe000000u : c.fg) : c.bg;
			bool sel = in_selection(s, x, abs_y);
			int x2 = x + 1;
			while (x2 < t->cols) {
				Cell d = x2 < cols && row ? row[x2] : (Cell){ ' ', TC_DEFAULT, TC_DEFAULT, 0 };
				u32 bg2 = (d.attr & A_REVERSE) ? (d.fg == TC_DEFAULT ? 0xfe000000u : d.fg) : d.bg;
				if (bg2 != bgc || in_selection(s, x2, abs_y) != sel)
					break;
				x2++;
			}
			Rect br = snap(ui, rect(x0 + (f32)x * cw, ry, (f32)(x2 - x) * cw, ch));
			if (sel)
				ui_fill(ui, br, color_with_alpha(ui->theme.accent, 0x70));
			else if (bgc == 0xfe000000u)
				ui_fill(ui, br, TERM_FG);
			else if (bgc != TC_DEFAULT)
				ui_fill(ui, br, color_of(bgc, false, 0));
			x = x2;
		}
		if (!row)
			continue;
		/* Then text, in runs of the same style. */
		for (int x = 0; x < cols;) {
			Cell c = row[x];
			if (c.cp == ' ' || (c.attr & A_HIDDEN)) {
				x++;
				continue;
			}
			Color col = (c.attr & A_REVERSE) ? color_of(c.bg, false, 0) : color_of(c.fg, true, c.attr);
			if (c.attr & A_DIM)
				col = color_scale_alpha(col, 0.6f);
			Rect cr = rect(x0 + (f32)x * cw, ry, cw, ch);
			if (c.cp >= 0x2500 && c.cp <= 0x259f && draw_special(ui, c.cp, cr, col)) {
				x++;
				continue;
			}
			char run[1024];
			usize n = 0;
			int x2 = x;
			while (x2 < cols && n < sizeof run - 4) {
				Cell d = row[x2];
				if (d.fg != c.fg || d.bg != c.bg || d.attr != c.attr || (d.cp >= 0x2500 && d.cp <= 0x259f))
					break;
				n += utf8_encode(d.cp ? d.cp : ' ', run + n);
				x2++;
			}
			run[n] = 0;
			ui_text(ui, (c.attr & A_BOLD) ? fb : f, x0 + (f32)x * cw, ry + base, run, col);
			if (c.attr & A_UNDERLINE)
				ui_fill(ui, rect(x0 + (f32)x * cw, ry + base + 2, (f32)(x2 - x) * cw, 1), col);
			if (c.attr & A_STRIKE)
				ui_fill(ui, rect(x0 + (f32)x * cw, ry + ch / 2, (f32)(x2 - x) * cw, 1), col);
			x = x2;
		}
	}
	/* Cursor: a solid block when focused, an outline when not. */
	if (t->cursor_visible && s->scroll == 0 && !s->exited) {
		Rect cr = snap(ui, rect(x0 + (f32)t->cx * cw, y0 + (f32)t->cy * ch, cw, ch));
		Color cc = ui->theme.accent;
		if (focused) {
			ui_fill(ui, cr, cc);
			Cell c = t->screen[(usize)t->cy * (usize)t->cols + (usize)t->cx];
			if (c.cp != ' ') {
				char b[8];
				b[utf8_encode(c.cp, b)] = 0;
				ui_text(ui, (c.attr & A_BOLD) ? fb : f, cr.x, cr.y + base, b, TERM_BG);
			}
		} else {
			ui_border(ui, cr, 0, 1, cc);
		}
	}
	if (s->exited) {
		char msg[96];
		fmt(msg, sizeof msg, "The shell has ended (status %d). Press Enter to start a new one.", s->exit_status);
		Font mf = ui_font(ui, 13, 500);
		f32 w = ui_text_width(ui, mf, msg) + 32;
		Rect m = rect(area.x + (area.w - w) / 2, area.y + area.h - 56, w, 36);
		ui_round(ui, m, 18, color_hex("#000000c0"));
		ui_text_in(ui, mf, m, msg, COLOR_WHITE, ALIGN_CENTER);
	}
	/* A thin scrollbar while looking at history. */
	if (s->scroll > 0 && t->sb_len > 0) {
		f32 total = (f32)(t->sb_len + t->rows);
		f32 h = maxf(30, area.h * (f32)t->rows / total);
		f32 y = area.y + (area.h - h) * (1 - (f32)s->scroll / (f32)t->sb_len);
		ui_round(ui, rect(area.x + area.w - 7, y, 4, h), 2, color_hex("#ffffff50"));
	}
}

/* ---- Input ----------------------------------------------------------------------------------- */

static char code_letter(u32 code)
{
	static const struct {
		u32 code;
		char c;
	} map[] = {
		{ KEY_A, 'a' }, { KEY_B, 'b' }, { KEY_C, 'c' }, { KEY_D, 'd' }, { KEY_E, 'e' }, { KEY_F, 'f' }, { KEY_G, 'g' },
		{ KEY_H, 'h' }, { KEY_I, 'i' }, { KEY_J, 'j' }, { KEY_K, 'k' }, { KEY_L, 'l' }, { KEY_M, 'm' }, { KEY_N, 'n' },
		{ KEY_O, 'o' }, { KEY_P, 'p' }, { KEY_Q, 'q' }, { KEY_R, 'r' }, { KEY_S, 's' }, { KEY_T, 't' }, { KEY_U, 'u' },
		{ KEY_V, 'v' }, { KEY_W, 'w' }, { KEY_X, 'x' }, { KEY_Y, 'y' }, { KEY_Z, 'z' }, { KEY_LEFTBRACE, '[' },
		{ KEY_BACKSLASH, '\\' }, { KEY_RIGHTBRACE, ']' }, { KEY_SPACE, ' ' }, { KEY_1, '1' }, { KEY_2, '2' }, { KEY_3, '3' },
		{ KEY_4, '4' }, { KEY_5, '5' }, { KEY_6, '6' }, { KEY_7, '7' }, { KEY_8, '8' }, { KEY_9, '9' }, { KEY_0, '0' },
		{ KEY_DOT, '.' }, { KEY_COMMA, ',' }, { KEY_SLASH, '/' }, { KEY_MINUS, '-' },
	};
	for (usize i = 0; i < ARRAY_LEN(map); i++)
		if (map[i].code == code)
			return map[i].c;
	return 0;
}

static char *selection_text(Session *s)
{
	if (!s->has_sel)
		return NULL;
	Term *t = &s->term;
	int ax0 = s->sel_ax, ay0 = s->sel_ay, ax1 = s->sel_bx, ay1 = s->sel_by;
	if (ay0 > ay1 || (ay0 == ay1 && ax0 > ax1)) {
		int tx = ax0, ty = ay0;
		ax0 = ax1, ay0 = ay1, ax1 = tx, ay1 = ty;
	}
	StrBuf sb = { 0 };
	for (int ay = ay0; ay <= ay1; ay++) {
		int cols;
		const Cell *row = term_row(t, ay - t->sb_len, 0, &cols);
		int from = ay == ay0 ? ax0 : 0, to = ay == ay1 ? ax1 : cols;
		to = MIN(to, cols);
		/* Trailing spaces aren't part of the text. */
		int end = to;
		while (end > from && row && row[end - 1].cp == ' ')
			end--;
		for (int x = from; x < end && row; x++)
			sb_addcp(&sb, row[x].cp);
		if (ay < ay1)
			sb_addc(&sb, '\n');
	}
	return sb_take(&sb);
}

static void paste_text(Session *s, const char *text)
{
	if (s->term.bracketed_paste)
		write_pty(s, "\x1b[200~", 6);
	/* Newlines become carriage returns, as typed. */
	for (const char *p = text; *p; p++) {
		char c = *p == '\n' ? '\r' : *p;
		write_pty(s, &c, 1);
	}
	if (s->term.bracketed_paste)
		write_pty(s, "\x1b[201~", 6);
}

static void open_tab(void);
static void close_tab(int i);

static void send_key(Session *s, const UiKey *k)
{
	bool ctrl = k->mods & MOD_CTRL, alt = (k->mods & MOD_ALT) && !(k->mods & MOD_CTRL), shift = k->mods & MOD_SHIFT;
	const char *seq = NULL;
	char buf[16];
	bool app = s->term.app_cursor;
	int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
	switch (k->code) {
	case KEY_ENTER:
	case KEY_KPENTER: seq = "\r"; break;
	case KEY_BACKSPACE: seq = ctrl ? "\x08" : "\x7f"; break;
	case KEY_TAB: seq = shift ? "\x1b[Z" : "\t"; break;
	case KEY_ESC: seq = "\x1b"; break;
	case KEY_UP:
	case KEY_DOWN:
	case KEY_RIGHT:
	case KEY_LEFT: {
		char d = k->code == KEY_UP ? 'A' : k->code == KEY_DOWN ? 'B' : k->code == KEY_RIGHT ? 'C' : 'D';
		if (mod > 1)
			fmt(buf, sizeof buf, "\x1b[1;%d%c", mod, d);
		else
			fmt(buf, sizeof buf, app ? "\x1bO%c" : "\x1b[%c", d);
		seq = buf;
		break;
	}
	case KEY_HOME: seq = app ? "\x1bOH" : "\x1b[H"; break;
	case KEY_END: seq = app ? "\x1bOF" : "\x1b[F"; break;
	case KEY_PAGEUP: seq = "\x1b[5~"; break;
	case KEY_PAGEDOWN: seq = "\x1b[6~"; break;
	case KEY_INSERT: seq = "\x1b[2~"; break;
	case KEY_DELETE: seq = "\x1b[3~"; break;
	case KEY_F1: seq = "\x1bOP"; break;
	case KEY_F2: seq = "\x1bOQ"; break;
	case KEY_F3: seq = "\x1bOR"; break;
	case KEY_F4: seq = "\x1bOS"; break;
	case KEY_F5: seq = "\x1b[15~"; break;
	case KEY_F6: seq = "\x1b[17~"; break;
	case KEY_F7: seq = "\x1b[18~"; break;
	case KEY_F8: seq = "\x1b[19~"; break;
	case KEY_F9: seq = "\x1b[20~"; break;
	case KEY_F10: seq = "\x1b[21~"; break;
	case KEY_F11: seq = "\x1b[23~"; break;
	case KEY_F12: seq = "\x1b[24~"; break;
	}
	if (seq) {
		if (alt && seq[0] != '\x1b')
			write_pty(s, "\x1b", 1);
		write_pty(s, seq, strlen(seq));
		return;
	}
	if (ctrl) {
		char l = code_letter(k->code);
		if (l >= 'a' && l <= 'z')
			buf[0] = (char)(l - 'a' + 1);
		else if (l == '[' || l == '3')
			buf[0] = 0x1b;
		else if (l == '\\' || l == '4')
			buf[0] = 0x1c;
		else if (l == ']' || l == '5')
			buf[0] = 0x1d;
		else if (l == ' ' || l == '2')
			buf[0] = 0;
		else if (l == '/' || l == '7')
			buf[0] = 0x1f;
		else
			return;
		write_pty(s, buf, 1);
		return;
	}
	if (alt) {
		char l = code_letter(k->code);
		if (l) {
			buf[0] = 0x1b;
			buf[1] = shift && l >= 'a' && l <= 'z' ? (char)(l - 32) : l;
			write_pty(s, buf, 2);
		}
		return;
	}
	if (k->text[0])
		write_pty(s, k->text, strlen(k->text));
}

static void mouse_report(Session *s, int button, int x, int y, bool press)
{
	char buf[32];
	if (s->term.mouse_sgr)
		fmt(buf, sizeof buf, "\x1b[<%d;%d;%d%c", button, x + 1, y + 1, press ? 'M' : 'm');
	else
		fmt(buf, sizeof buf, "\x1b[M%c%c%c", 32 + (press ? button : 3), 33 + MIN(x, 222), 33 + MIN(y, 222));
	write_pty(s, buf, strlen(buf));
}

static void handle_input(Ui *ui, Session *s, Rect area)
{
	Term *t = &s->term;
	for (int i = 0; i < ui->nkeys; i++) {
		UiKey *k = &ui->keys[i];
		if (!k->pressed)
			continue;
		bool cs = (k->mods & MOD_CTRL) && (k->mods & MOD_SHIFT);
		if (cs && k->code == KEY_C) {
			char *txt = selection_text(s);
			if (txt) {
				ui_clipboard_set(ui, txt);
				mem_free(txt);
			}
			continue;
		}
		if (cs && k->code == KEY_V) {
			ui->want_paste = true;
			continue;
		}
		if (cs && k->code == KEY_T) {
			open_tab();
			return;
		}
		if (cs && k->code == KEY_W) {
			close_tab(fg.current);
			return;
		}
		if ((k->mods & MOD_CTRL) && (k->code == KEY_EQUAL || k->code == KEY_MINUS || k->code == KEY_0) && !(k->mods & MOD_SHIFT)) {
			fg.font_size = k->code == KEY_0 ? 14 : clampf(fg.font_size + (k->code == KEY_EQUAL ? 1 : -1), 8, 32);
			continue;
		}
		if ((k->mods & MOD_SHIFT) && (k->code == KEY_PAGEUP || k->code == KEY_PAGEDOWN) && !t->alt_active) {
			s->scroll = CLAMP(s->scroll + (k->code == KEY_PAGEUP ? t->rows - 1 : -(t->rows - 1)), 0, t->sb_len);
			continue;
		}
		if (s->exited) {
			if (k->code == KEY_ENTER) {
				/* Replace the finished shell with a fresh one in the same tab. */
				int cols = t->cols, rows = t->rows;
				Session *n = session_new(cols, rows, NULL);
				if (n) {
					session_close(s);
					fg.tabs[fg.current] = n;
				}
				return;
			}
			continue;
		}
		s->scroll = 0;
		s->has_sel = false;
		send_key(s, k);
	}
	if (ui->paste_in && !s->exited) {
		paste_text(s, ui->paste_in);
		s->scroll = 0;
	}
	/* Mouse. */
	f32 x0 = area.x + 10, y0 = area.y + 6;
	int mx = (int)((ui->mx - x0) / fg.cell_w), my = (int)((ui->my - y0) / fg.cell_h);
	mx = CLAMP(mx, 0, t->cols - 1);
	my = CLAMP(my, 0, t->rows - 1);
	UiInteract it = ui_interact(ui, ui_id(ui, "term"), area);
	if (ui_mouse_in(ui, area))
		ui_set_cursor(ui, CURSOR_TEXT);
	bool report = t->mouse_mode && !(ui->mods & MOD_SHIFT) && !s->exited;
	if (ui_mouse_in(ui, area) && ui->scroll_dy != 0) {
		int lines = (int)roundf(-ui->scroll_dy / (ui->scroll_source == SCROLL_TOUCHPAD ? fg.cell_h : 16));
		if (lines == 0)
			lines = ui->scroll_dy < 0 ? 1 : -1;
		if (report) {
			for (int k = 0; k < ABS(lines); k++)
				mouse_report(s, lines > 0 ? 64 : 65, mx, my, true);
		} else if (t->alt_active) {
			/* Full-screen programs without mouse support get arrow keys. */
			for (int k = 0; k < MIN(ABS(lines), 10); k++)
				write_pty(s, lines > 0 ? (t->app_cursor ? "\x1bOA" : "\x1b[A") : (t->app_cursor ? "\x1bOB" : "\x1b[B"), 3);
		} else {
			s->scroll = CLAMP(s->scroll + lines, 0, t->sb_len);
		}
	}
	if (report) {
		if (it.pressed_now)
			mouse_report(s, ui->pressed[BUTTON_RIGHT] ? 2 : ui->pressed[BUTTON_MIDDLE] ? 1 : 0, mx, my, true);
		if (ui->released[BUTTON_LEFT] && ui_mouse_in(ui, area))
			mouse_report(s, 0, mx, my, false);
		return;
	}
	int ay = t->sb_len - s->scroll + my;
	if (it.pressed_now) {
		s->selecting = true;
		s->has_sel = false;
		s->sel_ax = s->sel_bx = mx;
		s->sel_ay = s->sel_by = ay;
	}
	if (s->selecting) {
		if (ui->down[BUTTON_LEFT]) {
			/* Past the right half of a cell includes it. */
			int ex = (int)((ui->mx - x0) / fg.cell_w + 0.5f);
			s->sel_bx = CLAMP(ex, 0, t->cols);
			s->sel_by = ay;
			s->has_sel = s->sel_bx != s->sel_ax || s->sel_by != s->sel_ay;
			/* Drag beyond the edge scrolls through history. */
			if (ui->my < area.y && !t->alt_active)
				s->scroll = MIN(s->scroll + 1, t->sb_len), ui->redraw = true;
			else if (ui->my > area.y + area.h)
				s->scroll = MAX(s->scroll - 1, 0), ui->redraw = true;
		} else {
			s->selecting = false;
		}
	}
	if (it.double_clicked) {
		/* Select a word. */
		int cols;
		const Cell *row = term_row(t, my, s->scroll, &cols);
		if (row && mx < cols) {
			int a = mx, b = mx;
			while (a > 0 && row[a - 1].cp != ' ')
				a--;
			while (b < cols && row[b].cp != ' ')
				b++;
			s->sel_ax = a, s->sel_bx = b, s->sel_ay = s->sel_by = ay;
			s->has_sel = b > a;
			s->selecting = false;
		}
	}
	if (ui->pressed[BUTTON_MIDDLE] && ui_mouse_in(ui, area))
		ui->want_paste = true;
	if (it.right_clicked)
		ui_popover_toggle(ui, "term-menu");
}

/* ---- Tabs ------------------------------------------------------------------------------------ */

static void grid_size(Rect area, int *cols, int *rows)
{
	*cols = MAX(10, (int)((area.w - 20) / fg.cell_w));
	*rows = MAX(4, (int)((area.h - 12) / fg.cell_h));
}

static Rect content_area(Ui *ui)
{
	Rect win = rect(0, 0, ui->width, ui->height);
	cut_top(&win, 46);
	if (fg.ntabs > 1)
		cut_top(&win, 36);
	return win;
}

static void open_tab(void)
{
	if (fg.ntabs >= (int)ARRAY_LEN(fg.tabs))
		return;
	int cols, rows;
	grid_size(content_area(fg.ui), &cols, &rows);
	/* New tabs start in the current tab's folder when the shell reported it. */
	Session *s = session_new(cols, rows, fg.ntabs ? NULL : fg.start_dir);
	if (!s)
		return;
	fg.tabs[fg.ntabs] = s;
	fg.current = fg.ntabs++;
}

static void close_tab(int i)
{
	session_close(fg.tabs[i]);
	memmove(&fg.tabs[i], &fg.tabs[i + 1], sizeof(Session *) * (usize)(fg.ntabs - i - 1));
	fg.ntabs--;
	if (fg.ntabs == 0) {
		fg.ui->quit = true;
		return;
	}
	fg.current = MIN(fg.current, fg.ntabs - 1);
}

static const char *tab_title(Session *s)
{
	return s->term.title[0] ? s->term.title : "Flint";
}

static void tab_bar(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->header_bottom);
	ui_separator(ui, r.x, r.y + r.h - 1, r.x + r.w);
	f32 tw = minf(240, (r.w - 50) / (f32)fg.ntabs);
	int close = -1;
	for (int i = 0; i < fg.ntabs; i++) {
		Rect tab = rect(r.x + 6 + (f32)i * tw, r.y + 4, tw - 4, r.h - 4);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "tab"), tab);
		bool active = i == fg.current;
		if (active)
			ui_round4(ui, tab, 6, 6, 0, 0, TERM_BG);
		else if (it.hovered)
			ui_round4(ui, tab, 6, 6, 0, 0, t->hover);
		ui_push_clip(ui, rect(tab.x + 8, tab.y, tab.w - 36, tab.h));
		ui_icon(ui, rect(tab.x + 10, tab.y + (tab.h - 14) / 2, 14, 14), "terminal", active ? color_hex("#9aa4b2") : t->text_dim);
		ui_text(ui, ui_font(ui, 12, active ? 600 : 500), tab.x + 30, tab.y + tab.h / 2 + 4, tab_title(fg.tabs[i]),
			active ? COLOR_WHITE : t->text_dim);
		ui_pop_clip(ui);
		Rect cb = rect(tab.x + tab.w - 24, tab.y + (tab.h - 18) / 2, 18, 18);
		UiInteract ci = ui_interact(ui, ui_id(ui, "x"), cb);
		if (ci.hovered || it.hovered || active)
			ui_icon(ui, pad(cb, 3), "close", active ? color_hex("#9aa4b2") : t->text_dim);
		if (ci.clicked)
			close = i;
		else if (it.clicked)
			fg.current = i;
		ui_pop_id(ui);
	}
	if (ui_icon_button(ui, rect(r.x + 8 + (f32)fg.ntabs * tw, r.y + 5, 28, r.h - 10), "add", "New Tab (Shift+Ctrl+T)"))
		open_tab();
	if (close >= 0)
		close_tab(close);
}

static void context_menu(Ui *ui, Session *s)
{
	Rect c;
	if (!ui_popover_begin(ui, "term-menu", rect(ui->mx, ui->my, 1, 1), 240, 160, &c))
		return;
	if (ui_menu_item(ui, cut_top(&c, 34), "edit-copy", "Copy", "Shift+Ctrl+C")) {
		char *txt = selection_text(s);
		if (txt) {
			ui_clipboard_set(ui, txt);
			mem_free(txt);
		}
		ui_popover_close(ui);
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "edit-paste", "Paste", "Shift+Ctrl+V")) {
		ui->want_paste = true;
		ui_popover_close(ui);
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "tab-new", "New Tab", "Shift+Ctrl+T")) {
		ui_popover_close(ui);
		open_tab();
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "edit", "Clear History", NULL)) {
		Term *t = &s->term;
		for (int i = 0; i < t->sb_len; i++)
			mem_free(t->scrollback[(t->sb_head + i) % t->sb_cap].cells);
		t->sb_len = t->sb_head = 0;
		s->scroll = 0;
		ui_popover_close(ui);
	}
	ui_popover_end(ui);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	/* Cell size from the monospaced font. */
	Font f = ui_font_mono(ui, fg.font_size, 400);
	fg.cell_w = ui_text_width(ui, f, "M");
	fg.cell_h = roundf(fg.font_size * ui->theme.text_scale * 1.36f);
	if (fg.ntabs == 0)
		open_tab();
	Session *s = cur();
	ui_window_background(ui, TERM_BG);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	/* A dark header bar to match the terminal. */
	Theme saved = *t;
	t->header_top = color_hex("#2a2d35");
	t->header_bottom = color_hex("#23262d");
	t->text = color_hex("#e6e9ee");
	t->text_dim = color_hex("#9aa4b2");
	t->hover = color_hex("#ffffff14");
	t->border = color_hex("#00000060");
	Rect tools = ui_headerbar(ui, header, s ? tab_title(s) : "Forge", 0);
	if (ui_icon_button(ui, cut_right(&tools, 34), "tab-new", "New Tab (Shift+Ctrl+T)"))
		open_tab();
	if (fg.ntabs > 1)
		tab_bar(ui, cut_top(&win, 36));
	*t = saved;
	if (!s)
		return;
	int cols, rows;
	grid_size(win, &cols, &rows);
	for (int i = 0; i < fg.ntabs; i++)
		if (fg.tabs[i]->term.cols != cols || fg.tabs[i]->term.rows != rows) {
			term_resize(&fg.tabs[i]->term, cols, rows);
			set_window_size(fg.tabs[i]);
		}
	handle_input(ui, s, win);
	if (fg.ntabs == 0)
		return;
	s = cur();
	draw_terminal(ui, s, win);
	context_menu(ui, s);
	if (s->term.bell) {
		s->term.bell = false;
	}
	char title[300];
	fmt(title, sizeof title, "%s — Forge", tab_title(s));
	ui_set_title(ui, title);
}

static bool on_close(Ui *ui)
{
	UNUSED(ui);
	while (fg.ntabs)
		close_tab(fg.ntabs - 1);
	return true;
}

static void init(Ui *ui)
{
	fg.ui = ui;
	fg.font_size = 14;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		str_copy(fg.start_dir, argv[1], sizeof fg.start_dir);
	sig_ignore(SIGPIPE);
	UiAppDesc d = { .id = "forge", .title = "Forge", .width = 860, .height = 540, .min_width = 420, .min_height = 260,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame, .close = on_close };
	return ui_run(&d);
}
