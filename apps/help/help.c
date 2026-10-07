/*
 * Help — how ZOS works, in short pages.
 *
 * Topics are written below in a small markup: "# Title|icon" starts a topic,
 * "## " a heading, "- " a bullet, "> " a tip, "K: Super+Space | text" a
 * keyboard shortcut drawn with key caps; other lines are paragraphs (a blank
 * line ends one). Searchable; "help TOPIC" opens a topic by its first word.
 */
#include "zui.h"

static const char GUIDE[] =
	"# Welcome|zurbax-logo\n"
	"ZOS is a desktop system built from scratch, made for games and for making your computer your own.\n"
	"\n"
	"## Getting around\n"
	"- Applications in the top-left corner (or a tap of the Super key) lists every app. Start typing to search.\n"
	"- The dock along the bottom holds your favourite apps and shows which ones are open.\n"
	"- The clock in the middle of the top bar opens your notifications.\n"
	"- The icons on the right open quick settings: network, sound, brightness, night light and Do Not Disturb.\n"
	"- The power button on the far right locks, logs out, restarts or shuts down.\n"
	"\n"
	"> New here? Open Dashboard for your day at a glance, or Arcade for a quick game.\n"
	"\n"
	"# What's new in 2.0|star\n"
	"ZOS 2.0 is called Golden Rain. It's the release for playing.\n"
	"## Games and controllers\n"
	"- Game controllers: Xbox, PlayStation, Switch Pro, Steam and most USB pads work as soon as they're plugged in, up to four at once.\n"
	"- The Guide button opens Applications, and every app can be used with the D-pad, A and B.\n"
	"- Glide, a 3D game in Arcade: fly a paper glider through golden rings over lakes and hills at sunset.\n"
	"- Games can make your controller vibrate, and plugging one in tells you its battery level.\n"
	"- F11 makes any window full screen, with game mode on.\n"
	"- Standard Linux programs: ZOS's display speaks Wayland, so Linux programs and X11 games open as ordinary windows. See Steam and Linux programs.\n"
	"## Everything else\n"
	"- Wi-Fi: join WPA2 and open networks from the top bar, Settings or the first-start assistant.\n"
	"- Mail: your Gmail, Outlook, iCloud or other email account.\n"
	"- The Golden Rain wallpaper, in Settings → Appearance.\n"
	"\n"
	"# The desktop|display\n"
	"## Windows\n"
	"- Drag a window by its header bar. Double-click the header bar to maximize it.\n"
	"- The close button is on the left of the header bar, maximize on the right.\n"
	"- Ctrl+Alt+Left and Ctrl+Alt+Right snap a window to half of the screen.\n"
	"\n"
	"## The dock\n"
	"- Click an app to open it, or to bring it forward if it's already open.\n"
	"- Right-click an app for more: a new window, keep it in the dock or remove it, or quit.\n"
	"- When a window covers the dock, the dock slides away. Move the pointer to the bottom edge to bring it back.\n"
	"\n"
	"## Notifications\n"
	"Apps tell you things with a notification in the corner. Turn on Do Not Disturb in quick settings to keep them quiet; they still collect in the list under the clock.\n"
	"\n"
	"# Keyboard shortcuts|keyboard\n"
	"## Everywhere\n"
	"K: Super | Open or close Applications\n"
	"K: Super+Space | Open or close Applications\n"
	"K: Alt+Tab | Switch between windows (keep Alt held to go on)\n"
	"K: Alt+F4 | Close the window\n"
	"K: Super+D | Show the desktop\n"
	"K: Super+L | Lock the screen\n"
	"K: Print | Save a screenshot to Pictures\n"
	"K: Ctrl+Alt+Up | Maximize the window\n"
	"K: Ctrl+Alt+Down | Restore the window\n"
	"K: Ctrl+Alt+Left | Snap the window to the left half\n"
	"K: Ctrl+Alt+Right | Snap the window to the right half\n"
	"\n"
	"## In apps\n"
	"K: Ctrl+C | Copy\n"
	"K: Ctrl+X | Cut\n"
	"K: Ctrl+V | Paste\n"
	"K: Ctrl+A | Select everything\n"
	"\n"
	"## Music\n"
	"K: Space | Play or pause\n"
	"K: Left | Back ten seconds\n"
	"K: Right | Forward ten seconds\n"
	"\n"
	"# Make it yours|palette\n"
	"Everything about the look is in Settings, under Appearance and Desktop.\n"
	"- Light or dark style, and an accent colour for buttons and highlights.\n"
	"- The wallpaper: built-in pictures, or any photo of yours.\n"
	"- Text size, for easier reading.\n"
	"- Night light warms the screen in the evening to be kinder to your eyes.\n"
	"- The dock can hide itself, and its icons can grow under the pointer.\n"
	"\n"
	"# Games|gamepad\n"
	"Arcade comes with Glide, a 3D flying game, and Blocks, Snake, 2048, Mines and Breakout. Your best scores are kept and show on the Dashboard too.\n"
	"\n"
	"## While you play\n"
	"- Escape (or Start on a controller) pauses a game; press it again to go back to the list of games.\n"
	"- Clicking away from the window pauses the game for you.\n"
	"- F11 makes the game fill the screen.\n"
	"\n"
	"## Controllers\n"
	"Plug in a USB controller and it's ready: Xbox, PlayStation, Switch Pro, Steam and most others.\n"
	"- The Guide (logo) button opens Applications; the D-pad moves and A starts an app.\n"
	"- In any app the D-pad and left stick move, A selects and B goes back.\n"
	"- Settings → Gaming shows each controller live, to test its buttons, sticks and vibration.\n"
	"\n"
	"## Game mode\n"
	"When a game fills the screen, ZOS switches to game mode: the processor runs at full speed and notifications wait quietly in the list instead of popping up.\n"
	"> Settings can show a frames-per-second counter in the corner, to see how smoothly things run.\n"
	"\n"
	"## Steam and Linux programs\n"
	"The Steam app starts Valve's Steam, and through it Proton, which runs Windows games. Steam is a standard Linux program, so it needs the Linux layer: the C library, Xwayland and Mesa's graphics drivers, kept apart from ZOS in /data/layers/steam. The Steam app lists what's in place and what's missing.\n"
	"- Play opens Big Picture, made for controllers and the TV. Desktop Mode opens the regular Steam window.\n"
	"- A game that fills the screen goes straight to the display from its own GPU buffer, with nothing copied in between.\n"
	"- Games can lock the mouse for looking around, and they get the raw mouse movement.\n"
	"> 3D games need your graphics card's driver: AMD (Radeon RX 400 series and newer, Ryzen graphics) and Intel (6th generation and newer, Arc) are built in.\n"
	"\n"
	"# Files|folder\n"
	"Vault shows your files: Documents, Downloads, Music, Pictures and the rest of your home folder.\n"
	"- Double-click a file to open it in its app: pictures in Photos, songs in Music, text in Writer.\n"
	"- Screenshots go to Pictures, in the Screenshots folder.\n"
	"- Put songs in the Music folder and they show up in Music, sorted into albums.\n"
	"\n"
	"# Screen Time|hourglass\n"
	"Screen Time shows how long each app was used today and over the week, and helps keep a balance.\n"
	"- Limits: give an app a daily allowance. Five minutes before it runs out you get a warning, then the app steps aside.\n"
	"- Breaks: a gentle reminder to rest your eyes every so often.\n"
	"- Downtime: quiet hours, for example at night.\n"
	"\n"
	"# Internet|globe\n"
	"With a network cable plugged in, ZOS connects by itself. The network icon in the top bar shows when you're online.\n"
	"- Weather shows the forecast for your city. Search for any place with the button at the top.\n"
	"- Connections are encrypted with TLS 1.3 and checked against the same trusted authorities web browsers use.\n"
	"> Wi-Fi isn't supported yet. Use a network cable, or a USB-to-Ethernet adapter, for now.\n"
	"\n"
	"# Sound|audio-volume-high\n"
	"- The volume keys on your keyboard work everywhere; you'll hear a soft tick at the new level.\n"
	"- Music plays FLAC, Ogg Vorbis and WAV files. MP3 and AAC aren't supported yet.\n"
	"- Turn off alert sounds in Settings, under Sound, if you prefer silence.\n"
	"\n"
	"# Installing ZOS|drive\n"
	"When you start the computer from the ZOS USB stick or DVD, you can try everything first. Nothing on the computer changes until you install.\n"
	"## Two ways to install\n"
	"- Erase the disk: the whole disk becomes ZOS. Everything that was on it is deleted.\n"
	"- Next to Windows: ZOS uses free space on the disk and Windows stays as it is. Make the free space first in Windows, with Disk Management (shrink a partition by at least 16 GB).\n"
	"## Starting ZOS\n"
	"ZOS adds itself to your computer's startup list and starts by default. To pick Windows instead, open the boot menu while the computer starts: F8 or Esc on most ASUS computers, F12 on many others.\n"
	"> Keep the computer plugged in while installing. It takes about a minute.\n"
	"\n"
	"# The command line|terminal\n"
	"Forge is the terminal. It runs Flint, the ZOS shell, with colours, suggestions as you type and Tab completion.\n"
	"- Type help for the list of commands, and sysinfo for a look at your computer.\n"
	"- sudo runs a command as administrator; it asks for your password.\n"
	"- fetch downloads a web address, and play plays a sound file.\n"
	"\n"
	"# Accounts and privacy|lock\n"
	"- Your account is set up the first time ZOS starts. A password is optional: without one, ZOS starts straight to the desktop.\n"
	"- Add or change your password in Settings, under Users. With a password, the screen locks when you step away and at startup.\n"
	"- Super+L locks the screen at any time.\n"
	"\n"
	"# Troubleshooting|help\n"
	"## There's no sound\n"
	"- Check the volume in quick settings and that sound isn't muted.\n"
	"- Some computers start with their outputs quiet: turn the volume up with the keyboard's volume keys.\n"
	"## There's no internet\n"
	"- Check the network cable, and that the router is on. The network icon in the top bar shows the connection.\n"
	"## The screen looks blurry or the wrong size\n"
	"- ZOS uses the resolution your computer's firmware sets. Text size in Settings makes things bigger or smaller.\n"
	"## An app stopped responding\n"
	"- Right-click it in the dock and choose Quit, or open Pulse to see what's using the computer.\n";

typedef enum { B_HEADING, B_PARA, B_BULLET, B_TIP, B_KEY } BlockKind;

typedef struct {
	BlockKind kind;
	char *text;
	char *keys;              /* for B_KEY */
} Block;

typedef struct {
	char title[64], icon[48];
	Block *blocks;
	int nblocks;
} Topic;

static Topic topics[32];
static int ntopics, current;
static StrBuf search;

static void parse_guide(void)
{
	usize n;
	char **lines = str_split(GUIDE, '\n', &n);
	Topic *t = NULL;
	StrBuf para = { 0 };
	for (usize i = 0; i <= n; i++) {
		const char *l = i < n ? lines[i] : "";
		bool plain = l[0] && !str_starts(l, "#") && !str_starts(l, "- ") && !str_starts(l, "> ") && !str_starts(l, "K: ");
		if (!plain && para.len && t) {
			t->blocks = mem_realloc(t->blocks, sizeof(Block) * (usize)(t->nblocks + 1));
			t->blocks[t->nblocks++] = (Block){ B_PARA, sb_take(&para), NULL };
		}
		if (plain) {
			if (para.len)
				sb_addc(&para, ' ');
			sb_adds(&para, l);
			continue;
		}
		if (str_starts(l, "# ") && ntopics < (int)ARRAY_LEN(topics)) {
			t = &topics[ntopics++];
			const char *bar = str_chr(l, '|');
			str_copy(t->title, l + 2, MIN(sizeof t->title, (usize)(bar ? bar - l - 2 : (long)strlen(l + 2)) + 1));
			str_copy(t->icon, bar ? bar + 1 : "help", sizeof t->icon);
			continue;
		}
		if (!t || !l[0])
			continue;
		Block b = { 0 };
		if (str_starts(l, "## "))
			b.kind = B_HEADING, b.text = str_dup(l + 3);
		else if (str_starts(l, "- "))
			b.kind = B_BULLET, b.text = str_dup(l + 2);
		else if (str_starts(l, "> "))
			b.kind = B_TIP, b.text = str_dup(l + 2);
		else if (str_starts(l, "K: ")) {
			const char *bar = str_chr(l, '|');
			b.kind = B_KEY;
			b.keys = str_ndup(l + 3, bar ? (usize)(bar - l - 3) : strlen(l + 3));
			str_copy(b.keys, str_trim(b.keys), strlen(b.keys) + 1);
			b.text = str_dup(bar ? str_trim((char *)bar + 1) : "");
		}
		t->blocks = mem_realloc(t->blocks, sizeof(Block) * (usize)(t->nblocks + 1));
		t->blocks[t->nblocks++] = b;
	}
	sb_free(&para);
	str_split_free(lines, n);
}

static bool topic_matches(const Topic *t, const char *q)
{
	if (!q[0] || str_find_nocase(t->title, q))
		return true;
	for (int i = 0; i < t->nblocks; i++)
		if (str_find_nocase(t->blocks[i].text, q) || (t->blocks[i].keys && str_find_nocase(t->blocks[i].keys, q)))
			return true;
	return false;
}

/* Key caps for "Ctrl+Alt+Left"; returns the width used. */
static f32 draw_keys(Ui *ui, f32 x, f32 y, const char *keys, bool draw)
{
	Theme *t = &ui->theme;
	Font f = ui_font(ui, 12, 700);
	usize n;
	char **parts = str_split(keys, '+', &n);
	f32 start = x;
	for (usize i = 0; i < n; i++) {
		const char *k = parts[i];
		if (str_eq(k, "Left"))
			k = "←";
		else if (str_eq(k, "Right"))
			k = "→";
		else if (str_eq(k, "Up"))
			k = "↑";
		else if (str_eq(k, "Down"))
			k = "↓";
		f32 w = maxf(28, ui_text_width(ui, f, k) + 18);
		if (draw) {
			Rect cap = rect(x, y, w, 26);
			ui_round(ui, rect(cap.x, cap.y + 2, cap.w, cap.h), 6, t->border_strong);
			ui_round(ui, cap, 6, t->card);
			ui_border(ui, cap, 6, 1, t->border);
			ui_text_in(ui, f, cap, k, t->text, ALIGN_CENTER);
		}
		x += w;
		if (i + 1 < n) {
			if (draw)
				ui_text_in(ui, ui_font(ui, 12, 600), rect(x, y, 16, 26), "+", t->text_faint, ALIGN_CENTER);
			x += 16;
		}
	}
	str_split_free(parts, n);
	return x - start;
}

/* Lays out (and if draw, draws) a topic's blocks; returns the height. */
static f32 layout_topic(Ui *ui, const Topic *tp, Rect r, bool draw)
{
	Theme *t = &ui->theme;
	f32 y = r.y;
	Font title = ui_font(ui, 28, 700), body = ui_font(ui, 15, 400), head = ui_font(ui, 17, 700);
	if (draw) {
		ui_icon(ui, rect(r.x, y + 4, 30, 30), tp->icon, t->accent);
		ui_text_in(ui, title, rect(r.x + 42, y, r.w - 42, 40), tp->title, t->text, ALIGN_START);
	}
	y += 58;
	for (int i = 0; i < tp->nblocks; i++) {
		const Block *b = &tp->blocks[i];
		switch (b->kind) {
		case B_HEADING:
			y += 10;
			if (draw)
				ui_text_in(ui, head, rect(r.x, y, r.w, 26), b->text, t->text, ALIGN_START);
			y += 34;
			break;
		case B_PARA: {
			f32 h = ui_text_height(ui, body, b->text, r.w);
			if (draw)
				ui_text_wrapped(ui, body, rect(r.x, y, r.w, h), b->text, t->text, ALIGN_START);
			y += h + 14;
			break;
		}
		case B_BULLET: {
			f32 h = ui_text_height(ui, body, b->text, r.w - 22);
			if (draw) {
				ui_circle(ui, r.x + 6, y + 11, 3, t->accent);
				ui_text_wrapped(ui, body, rect(r.x + 22, y, r.w - 22, h), b->text, t->text, ALIGN_START);
			}
			y += h + 8;
			break;
		}
		case B_TIP: {
			f32 h = ui_text_height(ui, body, b->text, r.w - 64);
			Rect box = rect(r.x, y + 4, r.w, h + 28);
			if (draw) {
				ui_round(ui, box, 10, color_with_alpha(t->accent, 0x1c));
				ui_round(ui, rect(box.x, box.y, 4, box.h), 2, t->accent);
				ui_icon(ui, rect(box.x + 18, box.y + 14, 18, 18), "info", t->accent);
				ui_text_wrapped(ui, body, rect(box.x + 46, box.y + 14, r.w - 64, h), b->text, t->text, ALIGN_START);
			}
			y += box.h + 18;
			break;
		}
		case B_KEY: {
			Rect row = rect(r.x, y, r.w, 38);
			if (draw) {
				ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
				draw_keys(ui, row.x, row.y + 6, b->keys, true);
				ui_text_in(ui, body, rect(row.x + 210, row.y, row.w - 210, row.h), b->text, t->text, ALIGN_START);
			}
			y += 38;
			break;
		}
		}
	}
	return y - r.y + 24;
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	ui_headerbar(ui, cut_top(&win, 46), "Help", 0);
	Rect side = cut_left(&win, 250);
	ui_fill(ui, side, t->sidebar);
	ui_vseparator(ui, side.x + side.w, side.y, side.y + side.h);
	Rect s = pad4(side, 12, 12, 8, 12);
	ui_entry(ui, cut_top(&s, 34), "search", &search, "Search help", ENTRY_SEARCH);
	cut_top(&s, 8);
	const char *q = sb_str(&search);
	int first = -1;
	for (int i = 0; i < ntopics; i++) {
		if (!topic_matches(&topics[i], q))
			continue;
		if (first < 0)
			first = i;
		if (ui_sidebar_item(ui, cut_top(&s, 36), topics[i].icon, topics[i].title, i == current))
			current = i;
	}
	if (first >= 0 && !topic_matches(&topics[current], q))
		current = first;
	if (first < 0) {
		ui_placeholder(ui, win, "search", "Nothing found", "Try other words.");
		return;
	}
	Rect body = pad4(win, 24, 48, 24, 48);
	f32 maxw = minf(body.w, 720);
	Rect col = rect(body.x, body.y, maxw, body.h);
	f32 h = layout_topic(ui, &topics[current], rect(col.x, 0, col.w, 0), false);
	ui_push_id(ui, current);
	f32 y = ui_scroll_begin(ui, win, "page");
	layout_topic(ui, &topics[current], rect(col.x, y + 24, col.w, h), true);
	ui_scroll_end(ui, h + 48);
	ui_pop_id(ui);
}

int main(int argc, char **argv)
{
	parse_guide();
	if (argc > 1)
		for (int i = 0; i < ntopics; i++)
			if (str_find_nocase(topics[i].title, argv[1]))
				current = i;
	UiAppDesc d = { .id = "help", .title = "Help", .width = 1000, .height = 700, .min_width = 700, .min_height = 480, .flags = WIN_RESIZABLE,
			.frame = frame };
	return ui_run(&d);
}
