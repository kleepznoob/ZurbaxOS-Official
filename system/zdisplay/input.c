/* Input routing and keyboard mapping. Hardware event reading lives in input_evdev.c. */
#include "zdisplay.h"

/* ---- Keyboard layout ------------------------------------------------------------------ */

typedef struct {
	const char *name;
	const char *lower[64];   /* indexed by key code 0..63 for the main block */
	const char *upper[64];
} Layout;

/* Base (US) rows; other layouts override a handful of keys. */
#define KROW_NUM_L { [KEY_1]="1",[KEY_2]="2",[KEY_3]="3",[KEY_4]="4",[KEY_5]="5",[KEY_6]="6",[KEY_7]="7",[KEY_8]="8",[KEY_9]="9",[KEY_0]="0",[KEY_MINUS]="-",[KEY_EQUAL]="=" }
#define KROW_NUM_U { [KEY_1]="!",[KEY_2]="@",[KEY_3]="#",[KEY_4]="$",[KEY_5]="%",[KEY_6]="^",[KEY_7]="&",[KEY_8]="*",[KEY_9]="(",[KEY_0]=")",[KEY_MINUS]="_",[KEY_EQUAL]="+" }

static const Layout layouts[] = {
	{ "us",
	  { [KEY_Q]="q",[KEY_W]="w",[KEY_E]="e",[KEY_R]="r",[KEY_T]="t",[KEY_Y]="y",[KEY_U]="u",[KEY_I]="i",[KEY_O]="o",[KEY_P]="p",
	    [KEY_A]="a",[KEY_S]="s",[KEY_D]="d",[KEY_F]="f",[KEY_G]="g",[KEY_H]="h",[KEY_J]="j",[KEY_K]="k",[KEY_L]="l",
	    [KEY_Z]="z",[KEY_X]="x",[KEY_C]="c",[KEY_V]="v",[KEY_B]="b",[KEY_N]="n",[KEY_M]="m",
	    [KEY_1]="1",[KEY_2]="2",[KEY_3]="3",[KEY_4]="4",[KEY_5]="5",[KEY_6]="6",[KEY_7]="7",[KEY_8]="8",[KEY_9]="9",[KEY_0]="0",
	    [KEY_MINUS]="-",[KEY_EQUAL]="=",[KEY_LEFTBRACE]="[",[KEY_RIGHTBRACE]="]",[KEY_BACKSLASH]="\\",[KEY_SEMICOLON]=";",
	    [KEY_APOSTROPHE]="'",[KEY_GRAVE]="`",[KEY_COMMA]=",",[KEY_DOT]=".",[KEY_SLASH]="/",[KEY_SPACE]=" " },
	  { [KEY_Q]="Q",[KEY_W]="W",[KEY_E]="E",[KEY_R]="R",[KEY_T]="T",[KEY_Y]="Y",[KEY_U]="U",[KEY_I]="I",[KEY_O]="O",[KEY_P]="P",
	    [KEY_A]="A",[KEY_S]="S",[KEY_D]="D",[KEY_F]="F",[KEY_G]="G",[KEY_H]="H",[KEY_J]="J",[KEY_K]="K",[KEY_L]="L",
	    [KEY_Z]="Z",[KEY_X]="X",[KEY_C]="C",[KEY_V]="V",[KEY_B]="B",[KEY_N]="N",[KEY_M]="M",
	    [KEY_1]="!",[KEY_2]="@",[KEY_3]="#",[KEY_4]="$",[KEY_5]="%",[KEY_6]="^",[KEY_7]="&",[KEY_8]="*",[KEY_9]="(",[KEY_0]=")",
	    [KEY_MINUS]="_",[KEY_EQUAL]="+",[KEY_LEFTBRACE]="{",[KEY_RIGHTBRACE]="}",[KEY_BACKSLASH]="|",[KEY_SEMICOLON]=":",
	    [KEY_APOSTROPHE]="\"",[KEY_GRAVE]="~",[KEY_COMMA]="<",[KEY_DOT]=">",[KEY_SLASH]="?",[KEY_SPACE]=" " } },
};

static const Layout *active_layout = &layouts[0];

/* A few common AltGr compositions, shared across layouts. */
static const char *altgr_text(u32 code)
{
	switch (code) {
	case KEY_E: return "\xe2\x82\xac"; /* € */
	case KEY_A: return "\xc3\xa2";     /* â */
	case KEY_I: return "\xc3\xae";     /* î */
	case KEY_S: return "\xc8\x99";     /* ș */
	case KEY_T: return "\xc8\x9b";     /* ț */
	case KEY_Q: return "\xc4\x83";     /* ă */
	case KEY_C: return "\xc2\xa9";     /* © */
	default: return NULL;
	}
}

void input_set_layout(const char *name)
{
	for (usize i = 0; i < ARRAY_LEN(layouts); i++)
		if (str_eq(layouts[i].name, name)) {
			active_layout = &layouts[i];
			return;
		}
	active_layout = &layouts[0];
}

const char *keymap_text(u32 code, u32 mods)
{
	if ((mods & MOD_ALT) && (mods & MOD_CTRL)) {
		const char *t = altgr_text(code);
		if (t)
			return t;
	}
	if (mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))
		return "";
	switch (code) {
	case KEY_ENTER:
	case KEY_KPENTER: return "\n";
	case KEY_TAB: return "\t";
	}
	if (code >= ARRAY_LEN(active_layout->lower))
		return "";
	bool caps = mods & MOD_CAPS, shift = mods & MOD_SHIFT;
	const char *base = active_layout->lower[code];
	/* Caps Lock affects only letters. */
	bool letter = base && is_alpha((u8)base[0]) && !base[1];
	bool upper = letter ? (shift ^ caps) : shift;
	const char *s = upper ? active_layout->upper[code] : active_layout->lower[code];
	return s ? s : "";
}

/* ---- Pointer -------------------------------------------------------------------------- */

static void mark_active(void)
{
	S.last_input = now_s();
	if (S.osd_until && S.now > S.osd_until)
		S.osd_k = 0;
}

void input_pointer_abs(f32 x, f32 y)
{
	mark_active();
	f32 nx = clampf(x, 0, S.sw - 1), ny = clampf(y, 0, S.sh - 1);
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
