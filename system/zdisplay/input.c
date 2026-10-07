/* Input routing and keyboard mapping. Hardware event reading lives in input_evdev.c. */
#include "zdisplay.h"

/* ---- Keyboard layouts ------------------------------------------------------------------
 * The US layout is the base; other layouts list only the keys that differ.
 * Each entry: key code, unshifted text, shifted text, AltGr text (optional). */

typedef struct {
	u16 code;
	const char *lower, *upper, *altgr;
} KeyDef;

typedef struct {
	const char *name;      /* id stored in settings */
	const char *title;     /* shown in Settings */
	const KeyDef *keys;
} Layout;

static const KeyDef US[] = {
	{ KEY_Q, "q", "Q" }, { KEY_W, "w", "W" }, { KEY_E, "e", "E" }, { KEY_R, "r", "R" }, { KEY_T, "t", "T" },
	{ KEY_Y, "y", "Y" }, { KEY_U, "u", "U" }, { KEY_I, "i", "I" }, { KEY_O, "o", "O" }, { KEY_P, "p", "P" },
	{ KEY_A, "a", "A" }, { KEY_S, "s", "S" }, { KEY_D, "d", "D" }, { KEY_F, "f", "F" }, { KEY_G, "g", "G" },
	{ KEY_H, "h", "H" }, { KEY_J, "j", "J" }, { KEY_K, "k", "K" }, { KEY_L, "l", "L" },
	{ KEY_Z, "z", "Z" }, { KEY_X, "x", "X" }, { KEY_C, "c", "C" }, { KEY_V, "v", "V" }, { KEY_B, "b", "B" },
	{ KEY_N, "n", "N" }, { KEY_M, "m", "M" },
	{ KEY_1, "1", "!" }, { KEY_2, "2", "@" }, { KEY_3, "3", "#" }, { KEY_4, "4", "$" }, { KEY_5, "5", "%" },
	{ KEY_6, "6", "^" }, { KEY_7, "7", "&" }, { KEY_8, "8", "*" }, { KEY_9, "9", "(" }, { KEY_0, "0", ")" },
	{ KEY_MINUS, "-", "_" }, { KEY_EQUAL, "=", "+" }, { KEY_LEFTBRACE, "[", "{" }, { KEY_RIGHTBRACE, "]", "}" },
	{ KEY_BACKSLASH, "\\", "|" }, { KEY_SEMICOLON, ";", ":" }, { KEY_APOSTROPHE, "'", "\"" }, { KEY_GRAVE, "`", "~" },
	{ KEY_COMMA, ",", "<" }, { KEY_DOT, ".", ">" }, { KEY_SLASH, "/", "?" }, { KEY_SPACE, " ", " " },
	{ KEY_102ND, "\\", "|" },
	/* AltGr extras available on every layout. */
	{ KEY_E, NULL, NULL, "€" }, { KEY_A, NULL, NULL, "â" }, { KEY_I, NULL, NULL, "î" }, { KEY_S, NULL, NULL, "ș" },
	{ KEY_T, NULL, NULL, "ț" }, { KEY_Q, NULL, NULL, "ă" }, { KEY_C, NULL, NULL, "©" },
	{ 0 },
};

static const KeyDef GB[] = {
	{ KEY_2, "2", "\"" }, { KEY_3, "3", "£" }, { KEY_APOSTROPHE, "'", "@" }, { KEY_BACKSLASH, "#", "~" },
	{ KEY_GRAVE, "`", "¬" }, { KEY_102ND, "\\", "|" }, { KEY_4, "4", "$", "€" },
	{ 0 },
};

static const KeyDef RO[] = {
	{ KEY_LEFTBRACE, "ă", "Ă", "[" }, { KEY_RIGHTBRACE, "î", "Î", "]" }, { KEY_SEMICOLON, "ș", "Ș", ";" },
	{ KEY_APOSTROPHE, "ț", "Ț", "'" }, { KEY_BACKSLASH, "â", "Â", "\\" }, { KEY_GRAVE, "„", "”", "`" },
	{ KEY_COMMA, ",", ";", "<" }, { KEY_DOT, ".", ":", ">" }, { KEY_102ND, "\\", "|" },
	{ KEY_9, "9", "(", "{" }, { KEY_0, "0", ")", "}" },
	{ 0 },
};

static const KeyDef DE[] = {
	{ KEY_Y, "z", "Z" }, { KEY_Z, "y", "Y" },
	{ KEY_2, "2", "\"", "²" }, { KEY_3, "3", "§", "³" }, { KEY_6, "6", "&" }, { KEY_7, "7", "/", "{" },
	{ KEY_8, "8", "(", "[" }, { KEY_9, "9", ")", "]" }, { KEY_0, "0", "=", "}" },
	{ KEY_MINUS, "ß", "?", "\\" }, { KEY_EQUAL, "´", "`" }, { KEY_LEFTBRACE, "ü", "Ü" },
	{ KEY_RIGHTBRACE, "+", "*", "~" }, { KEY_SEMICOLON, "ö", "Ö" }, { KEY_APOSTROPHE, "ä", "Ä" },
	{ KEY_GRAVE, "^", "°" }, { KEY_BACKSLASH, "#", "'" }, { KEY_COMMA, ",", ";" }, { KEY_DOT, ".", ":" },
	{ KEY_SLASH, "-", "_" }, { KEY_102ND, "<", ">", "|" }, { KEY_Q, "q", "Q", "@" }, { KEY_M, "m", "M", "µ" },
	{ 0 },
};

static const KeyDef FR[] = {
	{ KEY_Q, "a", "A" }, { KEY_A, "q", "Q" }, { KEY_W, "z", "Z" }, { KEY_Z, "w", "W" },
	{ KEY_SEMICOLON, "m", "M" }, { KEY_M, ",", "?" }, { KEY_COMMA, ";", "." }, { KEY_DOT, ":", "/" },
	{ KEY_SLASH, "!", "§" }, { KEY_APOSTROPHE, "ù", "%" }, { KEY_BACKSLASH, "*", "µ" },
	{ KEY_LEFTBRACE, "^", "¨" }, { KEY_RIGHTBRACE, "$", "£", "¤" }, { KEY_GRAVE, "²", "²" },
	{ KEY_1, "&", "1" }, { KEY_2, "é", "2", "~" }, { KEY_3, "\"", "3", "#" }, { KEY_4, "'", "4", "{" },
	{ KEY_5, "(", "5", "[" }, { KEY_6, "-", "6", "|" }, { KEY_7, "è", "7", "`" }, { KEY_8, "_", "8", "\\" },
	{ KEY_9, "ç", "9", "^" }, { KEY_0, "à", "0", "@" }, { KEY_MINUS, ")", "°", "]" }, { KEY_EQUAL, "=", "+", "}" },
	{ KEY_102ND, "<", ">" },
	{ 0 },
};

static const KeyDef ES[] = {
	{ KEY_2, "2", "\"", "@" }, { KEY_3, "3", "·", "#" }, { KEY_6, "6", "&" }, { KEY_7, "7", "/" },
	{ KEY_8, "8", "(" }, { KEY_9, "9", ")" }, { KEY_0, "0", "=" }, { KEY_MINUS, "'", "?" }, { KEY_EQUAL, "¡", "¿" },
	{ KEY_LEFTBRACE, "`", "^", "[" }, { KEY_RIGHTBRACE, "+", "*", "]" }, { KEY_SEMICOLON, "ñ", "Ñ" },
	{ KEY_APOSTROPHE, "´", "¨", "{" }, { KEY_BACKSLASH, "ç", "Ç", "}" }, { KEY_GRAVE, "º", "ª", "\\" },
	{ KEY_COMMA, ",", ";" }, { KEY_DOT, ".", ":" }, { KEY_SLASH, "-", "_" }, { KEY_102ND, "<", ">" },
	{ KEY_1, "1", "!", "|" },
	{ 0 },
};

static const Layout LAYOUTS[] = {
	{ "us", "English (US)", NULL }, { "gb", "English (UK)", GB }, { "ro", "Romanian", RO },
	{ "de", "German", DE },         { "fr", "French", FR },       { "es", "Spanish", ES },
};

static const Layout *active_layout = &LAYOUTS[0];

/* Looks a key up in the active layout, then in the US base. */
static const KeyDef *find_key(u32 code, bool want_altgr)
{
	for (int pass = 0; pass < 2; pass++) {
		const KeyDef *k = pass == 0 ? active_layout->keys : US;
		if (!k)
			continue;
		for (; k->code; k++) {
			if (k->code != code)
				continue;
			if (want_altgr ? k->altgr != NULL : k->lower != NULL)
				return k;
		}
	}
	return NULL;
}

void input_set_layout(const char *name)
{
	const Layout *old = active_layout;
	active_layout = &LAYOUTS[0];
	for (usize i = 0; i < ARRAY_LEN(LAYOUTS); i++)
		if (str_eq(LAYOUTS[i].name, name))
			active_layout = &LAYOUTS[i];
	if (old && old != active_layout)
		wl_keymap_changed(); /* Wayland programs get the new layout */
}

/* What a key types in the active layout, for describing the layout to
 * Wayland programs: unshifted, shifted and AltGr text (NULL for none). */
bool input_key_texts(u32 code, const char **lower, const char **upper, const char **altgr)
{
	const KeyDef *k = find_key(code, false), *g = find_key(code, true);
	*lower = k ? k->lower : NULL;
	*upper = k ? k->upper : NULL;
	*altgr = g ? g->altgr : NULL;
	return k || g;
}

/* A key whose shifted text differs only in case is a letter: Caps Lock applies. */
static bool is_letter_key(const KeyDef *k)
{
	if (!k->lower || !k->upper || str_eq(k->lower, k->upper))
		return false;
	u8 c = (u8)k->lower[0];
	if (c < 0x80)
		return is_alpha(c);
	return (u8)k->upper[0] >= 0xC3; /* accented and other non-ASCII letters */
}

const char *keymap_text(u32 code, u32 mods)
{
	if ((mods & MOD_ALT) && (mods & MOD_CTRL)) {
		const KeyDef *k = find_key(code, true);
		if (k)
			return k->altgr;
	}
	if (mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))
		return "";
	switch (code) {
	case KEY_ENTER:
	case KEY_KPENTER: return "\n";
	case KEY_TAB: return "\t";
	}
	const KeyDef *k = find_key(code, false);
	if (!k)
		return "";
	bool caps = mods & MOD_CAPS, shift = mods & MOD_SHIFT;
	bool upper = is_letter_key(k) ? (shift ^ caps) : shift;
	const char *s = upper ? k->upper : k->lower;
	return s ? s : "";
}

/* ---- Pointer -------------------------------------------------------------------------- */

static void mark_active(void)
{
	S.last_input = now_s();
	if (S.osd_until && S.now > S.osd_until)
		S.osd_k = 0;
}

void input_activity(void)
{
	mark_active();
}

void input_pointer_abs(f32 x, f32 y)
{
	mark_active();
	f32 nx = clampf(x, 0, S.sw - 1), ny = clampf(y, 0, S.sh - 1);
	wl_confine(&nx, &ny);
	if (nx == S.px && ny == S.py)
		return;
	S.px = nx;
	S.py = ny;
	cursor_update();
	if (shell_pointer(POINTER_MOTION, 0))
		return;
	wm_pointer_motion();
}

void input_pointer_rel(f32 dx, f32 dy)
{
	f32 speed = 0.4f + S.set.pointer_speed * 1.6f;
	wl_relative_motion(dx * speed, dy * speed, dx, dy);
	/* a game holding the pointer gets the motion alone; the cursor stays put */
	if (wl_pointer_locked()) {
		mark_active();
		return;
	}
	input_pointer_abs(S.px + dx * speed, S.py + dy * speed);
}

void input_button(u32 button, bool pressed)
{
	mark_active();
	if (pressed)
		S.buttons |= 1u << button;
	else
		S.buttons &= ~(1u << button);
	if (shell_pointer(pressed ? POINTER_DOWN : POINTER_UP, button))
		return;
	wm_pointer_button(button, pressed);
}

void input_scroll(f32 dx, f32 dy, u32 source, u32 phase)
{
	mark_active();
	if (!S.set.natural_scroll) {
		dx = -dx;
		dy = -dy;
	}
	if (shell_scroll(dx, dy, source, phase))
		return;
	u32 edges;
	Window *w = window_at(S.px, S.py, &edges);
	if (w)
		server_scroll_event(w, dx, dy, source, phase);
}

/* ---- Keyboard ------------------------------------------------------------------------- */

static void update_mods(u32 code, bool pressed)
{
	u32 bit = 0;
	switch (code) {
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT: bit = MOD_SHIFT; break;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL: bit = MOD_CTRL; break;
	case KEY_LEFTALT: bit = MOD_ALT; break;
	case KEY_RIGHTALT: bit = MOD_ALT | MOD_CTRL; break; /* AltGr */
	case KEY_LEFTMETA:
	case KEY_RIGHTMETA: bit = MOD_SUPER; break;
	case KEY_CAPSLOCK:
		if (pressed)
			S.mods ^= MOD_CAPS;
		return;
	}
	if (bit) {
		if (pressed)
			S.mods |= bit;
		else
			S.mods &= ~bit;
	}
}

void input_key(u32 code, bool pressed, bool repeat)
{
	mark_active();
	update_mods(code, pressed);
	const char *text = pressed ? keymap_text(code, S.mods) : "";
	u32 state = !pressed ? KEY_RELEASED : repeat ? KEY_REPEATED : KEY_PRESSED;
	if (shell_key(code, state, S.mods, text))
		return;
	/* Media and brightness keys are handled by the shell even without focus. */
	if (pressed && !repeat) {
		switch (code) {
		case KEY_VOLUMEUP: set_volume(S.volume + 5); return;
		case KEY_VOLUMEDOWN: set_volume(S.volume - 5); return;
		case KEY_MUTE: S.muted = !S.muted; set_volume(S.muted ? 0 : S.volume); return;
		case KEY_BRIGHTNESSUP: set_brightness(S.brightness + 8); return;
		case KEY_BRIGHTNESSDOWN: set_brightness(S.brightness - 8); return;
		}
	}
	server_key_event(S.focus, code, state, text);
}
