/*
 * libzui — the Zurbax application toolkit.
 *
 * Immediate-mode: an app describes its whole interface in one `frame`
 * function that runs whenever something changes. Widgets are plain function
 * calls that draw themselves and report interaction:
 *
 *     if (ui_button(ui, r, "Save")) save_document();
 *
 * Layout uses rectangle cutting: start from the window rectangle and slice
 * off pieces (cut_top, cut_left, ...) for each part of the interface.
 * All coordinates are logical pixels; the toolkit handles HiDPI scaling.
 */
#pragma once

#include "zg.h"
#include "zui/proto.h"
#include "zui/keys.h"
#include "zui/audio.h"

/* ---- Theme -------------------------------------------------------------------- */

typedef struct {
	bool dark;
	bool reduce_motion;
	f32 text_scale;
	Color accent, accent_hover, accent_pressed, on_accent;
	Color window;       /* window background */
	Color view;         /* content areas: lists, documents */
	Color sidebar;
	Color header_top, header_bottom;
	Color card;
	Color border;       /* hairline separators */
	Color border_strong;
	Color text, text_dim, text_faint;
	Color hover, press; /* translucent overlays */
	Color selection;    /* selected rows */
	Color input;        /* text field background */
	Color button_top, button_bottom, button_border;
	Color danger, success, warning;
	Color shadow;
} Theme;

void theme_build(Theme *t, const ThemeInfo *info);
#define THEME_ACCENT_DEFAULT "iris"
Color theme_accent_named(const char *name); /* "iris", "blue", "teal"... */
extern const char *const THEME_ACCENT_NAMES[];
extern const u32 THEME_ACCENT_COLORS[];
extern const int THEME_ACCENT_COUNT;

/* ---- Context -------------------------------------------------------------------- */

typedef u64 UiId;

typedef struct {
	u32 code;
	u32 mods;
	char text[8];
	bool pressed;   /* includes repeats */
	bool repeat;
} UiKey;

typedef struct {
	bool hovered;
	bool held;           /* button is down and was pressed on this widget */
	bool clicked;        /* released over the widget */
	bool right_clicked;
	bool double_clicked;
	bool pressed_now;    /* went down this frame */
} UiInteract;

typedef struct UiStateMap UiStateMap;

typedef struct Ui {
	/* target */
	Canvas *canvas;
	f32 scale;
	f32 width, height;      /* logical window size */
	Theme theme;
	u32 win_states;         /* STATE_* from the server */

	/* time */
	f64 now;                /* seconds, monotonic */
	f32 dt;                 /* seconds since the last frame */
	u64 frame_no;

	/* input for this frame */
	f32 mx, my;
	bool pointer_inside;
	bool down[6], pressed[6], released[6];
	int click_count;
	f32 scroll_dx, scroll_dy;
	u32 scroll_phase, scroll_source;
	UiKey keys[256];
	int nkeys;
	u32 mods;
	bool held[256];         /* keys down right now, by code (games) */
	bool window_focused;
	char *drop_paths;       /* newline-separated files dropped this frame, or NULL */
	/* game controllers */
	struct {
		bool connected;
		char name[64];
		u32 buttons;        /* bit per PAD_* button, held now */
		u32 hit;            /* went down since the last frame */
		f32 axes[PAD_AXES];
		int battery;        /* percent, -1 if unknown */
		bool rumble;        /* can vibrate */
	} pads[PAD_MAX];
	u64 pad_raw_frame;      /* the last frame the app said it reads controllers itself */
	u32 pad_keys;           /* keys made from controller input that are held (translation) */
	f64 pad_repeat_at;

	/* widget bookkeeping */
	UiId hot, active, focus;
	UiId last_id;
	Rect last_rect;         /* absolute rect of the last interactive widget */
	bool focus_claimed;     /* a widget took keyboard focus this frame */
	UiId scroll_stack[8];
	Rect scroll_view[8];
	int scroll_depth;
	UiId id_stack[16];
	int id_depth;
	UiStateMap *state;
	f32 ox, oy;             /* translation of the current scope (scrolling) */
	Rect clip;              /* current clip, absolute logical coordinates */
	struct {
		f32 ox, oy;
		Rect clip;
		Canvas *canvas;
	} stack[32];
	int depth;

	/* overlays: popovers and dialogs draw last, above everything */
	Canvas *overlay;
	bool in_overlay;
	UiId open_popover, open_dialog;
	Rect overlay_rect, overlay_rect_prev;
	bool overlay_drawn;

	/* tooltip */
	UiId tooltip_id;
	f64 tooltip_since;
	char tooltip_text[160];
	Rect tooltip_anchor;

	/* requests to the driver */
	bool redraw;            /* draw another frame soon (animations) */
	f64 wake_at;            /* draw again at this time (caret blink, timers); 0 = none */
	u32 cursor;
	u32 action;             /* ACTION_* to send to the server */
	bool begin_move;
	u32 begin_resize;
	char *clipboard_out;    /* text to put on the clipboard */
	bool want_paste;
	char *paste_in;         /* clipboard text that arrived for the focused field */
	bool quit;

	/* a screenshot that arrived (ui_screenshot_request); the app takes ownership */
	Canvas *screenshot;
	bool screenshot_ready;
	i32 screenshot_status;

	/* replies to requests (ui_account_update) */
	u32 reply_seq;
	i32 reply_status;
	char reply_message[128];

	/* the app */
	void *user;
	void *driver;
	const char *app_id;
	char title[128];
} Ui;

/* Pushes a scope for widget ids (use inside loops: ui_push_id(ui, i)). */
UiId ui_id(Ui *ui, const char *label);
UiId ui_id_int(Ui *ui, i64 n);
void ui_push_id(Ui *ui, i64 n);
void ui_push_id_str(Ui *ui, const char *s);
void ui_pop_id(Ui *ui);

/* Generic per-widget state storage, zeroed on first use. */
void *ui_state(Ui *ui, UiId id, usize size);

/* Smoothly animates a value toward target; returns the current value. */
f32 ui_anim(Ui *ui, UiId id, f32 target, f32 speed);
/* 0..1 fade that follows `on` (hover highlights, reveals). */
f32 ui_fade(Ui *ui, UiId id, bool on, f32 seconds);

/* Hit testing that respects clipping and open overlays. */
bool ui_mouse_in(Ui *ui, Rect r);
UiInteract ui_interact(Ui *ui, UiId id, Rect r);
bool ui_key_pressed(Ui *ui, u32 code, u32 mods); /* consumes the key */
bool ui_key_down(Ui *ui, u32 code);              /* held down right now */

/* Game controllers. Apps that don't call ui_pad_raw get keys from them:
 * the D-pad and left stick are the arrow keys, A is Enter, B is Escape and
 * X is Space, so menus and keyboard games work with a controller as is. */
int ui_pad_count(Ui *ui);
bool ui_pad_down(Ui *ui, int button);             /* held on any controller */
bool ui_pad_pressed(Ui *ui, int button);          /* went down since the last frame; consumes */
bool ui_pad_held(Ui *ui, int pad, int button);    /* one controller (local multiplayer) */
f32 ui_pad_axis(Ui *ui, int axis);                /* -1..1 (triggers 0..1), with a dead zone */
f32 ui_pad_axis_of(Ui *ui, int pad, int axis);
void ui_pad_raw(Ui *ui);                          /* call each frame: this app reads controllers itself */
/* Shakes a controller (pad -1: all of them) for ms milliseconds; strong is
 * the heavy motor, weak the light buzz, each 0..1. Only while focused. */
void ui_pad_rumble(Ui *ui, int pad, f32 strong, f32 weak, u32 ms);
void ui_set_cursor(Ui *ui, u32 shape);
void ui_request_frame(Ui *ui);

/* ---- Layout: rectangle cutting --------------------------------------------------- */

Rect cut_top(Rect *r, f32 h);
Rect cut_bottom(Rect *r, f32 h);
Rect cut_left(Rect *r, f32 w);
Rect cut_right(Rect *r, f32 w);
Rect pad(Rect r, f32 p);
Rect pad2(Rect r, f32 x, f32 y);
Rect pad4(Rect r, f32 top, f32 right, f32 bottom, f32 left);
Rect center_in(Rect outer, f32 w, f32 h);
Rect grid_cell(Rect area, int cols, int rows, int index, f32 gap);

/* ---- Drawing (logical coordinates) ------------------------------------------------- */

Font ui_font(Ui *ui, f32 size, int weight);
Font ui_font_mono(Ui *ui, f32 size, int weight);
void ui_fill(Ui *ui, Rect r, Color c);
void ui_round(Ui *ui, Rect r, f32 radius, Color c);
void ui_round4(Ui *ui, Rect r, f32 tl, f32 tr, f32 br, f32 bl, Color c);
void ui_gradient(Ui *ui, Rect r, f32 radius, Color top, Color bottom);
void ui_border(Ui *ui, Rect r, f32 radius, f32 width, Color c);
void ui_circle(Ui *ui, f32 cx, f32 cy, f32 r, Color c);
void ui_ring(Ui *ui, f32 cx, f32 cy, f32 r, f32 width, Color c);
void ui_arc(Ui *ui, f32 cx, f32 cy, f32 r, f32 a0, f32 a1, f32 width, Color c);
void ui_line(Ui *ui, f32 x0, f32 y0, f32 x1, f32 y1, f32 width, Color c);
void ui_shadow(Ui *ui, Rect r, f32 radius, f32 blur, f32 dy, Color c);
void ui_blur(Ui *ui, Rect r, f32 radius);
void ui_path_fill(Ui *ui, const Path *p, Color c);
void ui_path_stroke(Ui *ui, const Path *p, f32 width, Color c);
f32 ui_text(Ui *ui, Font f, f32 x, f32 baseline, const char *s, Color c);
f32 ui_text_n(Ui *ui, Font f, f32 x, f32 baseline, const char *s, usize n, Color c);
void ui_text_in(Ui *ui, Font f, Rect r, const char *s, Color c, Align align);
f32 ui_text_wrapped(Ui *ui, Font f, Rect r, const char *s, Color c, Align align);
f32 ui_text_width(Ui *ui, Font f, const char *s);
f32 ui_text_width_n(Ui *ui, Font f, const char *s, usize n);
f32 ui_text_height(Ui *ui, Font f, const char *s, f32 width); /* wrapped */
f32 ui_line_height(Ui *ui, Font f);
void ui_icon(Ui *ui, Rect r, const char *name, Color c);       /* symbolic icon */
void ui_app_icon(Ui *ui, Rect r, const char *app_id);          /* full-color app icon */
void ui_image(Ui *ui, Rect r, const Canvas *img, f32 radius, bool cover);
void ui_push_clip(Ui *ui, Rect r);
void ui_pop_clip(Ui *ui);
Rect ui_to_device(Ui *ui, Rect r);

/* ---- Widgets ----------------------------------------------------------------------- */

bool ui_button(Ui *ui, Rect r, const char *label);
bool ui_button_suggested(Ui *ui, Rect r, const char *label);
bool ui_button_destructive(Ui *ui, Rect r, const char *label);
bool ui_button_flat(Ui *ui, Rect r, const char *label);
bool ui_button_icon_label(Ui *ui, Rect r, const char *icon, const char *label);
bool ui_icon_button(Ui *ui, Rect r, const char *icon, const char *tooltip);
bool ui_icon_toggle(Ui *ui, Rect r, const char *icon, bool active, const char *tooltip);
bool ui_switch(Ui *ui, Rect r, bool *on);
bool ui_checkbox(Ui *ui, Rect r, const char *label, bool *checked);
bool ui_radio(Ui *ui, Rect r, const char *label, bool selected);
bool ui_slider(Ui *ui, Rect r, f32 *value, f32 min, f32 max);
bool ui_segmented(Ui *ui, Rect r, const char *const *items, int n, int *selected);
bool ui_dropdown(Ui *ui, Rect r, const char *id, const char *const *items, int n, int *selected);
bool ui_spin(Ui *ui, Rect r, const char *id, int *value, int min, int max);
void ui_progress(Ui *ui, Rect r, f32 fraction);
void ui_spinner(Ui *ui, Rect r);
void ui_separator(Ui *ui, f32 x0, f32 y, f32 x1);
void ui_vseparator(Ui *ui, f32 x, f32 y0, f32 y1);
void ui_card(Ui *ui, Rect r);
void ui_badge(Ui *ui, f32 cx, f32 cy, const char *text);
void ui_avatar(Ui *ui, Rect r, const char *name);
void ui_label(Ui *ui, Rect r, const char *text);         /* body text */
void ui_label_dim(Ui *ui, Rect r, const char *text);     /* secondary text */
void ui_heading(Ui *ui, Rect r, const char *text);       /* section title */
void ui_title(Ui *ui, Rect r, const char *text);         /* large page title */
void ui_placeholder(Ui *ui, Rect r, const char *icon, const char *title, const char *subtitle);
void ui_tooltip(Ui *ui, const char *text);               /* for the widget just drawn */

/* Selectable rows (lists, sidebars). Returns the interaction; draw content on top. */
UiInteract ui_row(Ui *ui, Rect r, UiId id, bool selected);
bool ui_sidebar_item(Ui *ui, Rect r, const char *icon, const char *label, bool selected);
void ui_sidebar_heading(Ui *ui, Rect r, const char *label);

/* Text entry. Returns a mask of ENTRY_* events. */
enum { ENTRY_CHANGED = 1, ENTRY_SUBMIT = 2, ENTRY_ESCAPE = 4, ENTRY_FOCUSED = 8, ENTRY_UP = 16, ENTRY_DOWN = 32 };
enum { ENTRY_PASSWORD = 1, ENTRY_SEARCH = 2, ENTRY_AUTOFOCUS = 4, ENTRY_FLAT = 8, ENTRY_URL = 16 };
int ui_entry(Ui *ui, Rect r, const char *id, StrBuf *text, const char *placeholder, u32 flags);
/* Multi-line editor with wrapping, selection and scrolling. */
enum { TEXT_READONLY = 1, TEXT_MONO = 2, TEXT_NO_WRAP = 4, TEXT_NO_FRAME = 8 };
int ui_textarea(Ui *ui, Rect r, const char *id, StrBuf *text, f32 font_size, u32 flags);
void ui_focus(Ui *ui, const char *id);
usize ui_textarea_cursor(Ui *ui, const char *id);                      /* byte offset of the caret */
void ui_textarea_select(Ui *ui, const char *id, usize start, usize end); /* select and scroll to it */
bool ui_is_focused(Ui *ui, const char *id);

/* Scrolling: content drawn between begin and end moves with the scroll offset.
 * begin returns the y where content starts; pass the content's total height to end. */
f32 ui_scroll_begin(Ui *ui, Rect view, const char *id);
void ui_scroll_end(Ui *ui, f32 content_height);
void ui_scroll_to(Ui *ui, const char *id, f32 offset);

/* Window chrome: an elementary-style header bar with close (left) and
 * maximize (right) controls. Returns the space between the controls. */
enum { HEADER_FLAT = 1, HEADER_NO_TITLE = 2, HEADER_TRANSPARENT = 4, HEADER_NO_MAXIMIZE = 8, HEADER_NO_MINIMIZE = 16 };
Rect ui_headerbar(Ui *ui, Rect r, const char *title, u32 flags);
/* The window background with rounded corners; call first in a frame. */
void ui_window_background(Ui *ui, Color c);

/* Popovers and menus. Toggle with ui_popover_toggle; draw contents between
 * begin/end only when begin returns true. */
void ui_popover_toggle(Ui *ui, const char *id);
void ui_popover_close(Ui *ui);
bool ui_popover_is_open(Ui *ui, const char *id);
bool ui_popover_begin(Ui *ui, const char *id, Rect anchor, f32 w, f32 h, Rect *content);
void ui_popover_end(Ui *ui);
bool ui_menu_item(Ui *ui, Rect r, const char *icon, const char *label, const char *shortcut);

/* Modal dialogs inside the window. */
void ui_dialog_open(Ui *ui, const char *id);
void ui_dialog_close(Ui *ui);
bool ui_dialog_begin(Ui *ui, const char *id, f32 w, f32 h, Rect *content);
void ui_dialog_end(Ui *ui);

/* File chooser. Open it once, then call ui_file_dialog every frame; it returns
 * true (with the path) in the frame the user picks something. `filters` is a
 * comma-separated list of extensions ("png,jpg") or NULL for all files. */
enum { FILE_OPEN = 1, FILE_SAVE, FILE_FOLDER };
void ui_file_dialog_open(Ui *ui, const char *id, u32 mode, const char *start_dir, const char *filters, const char *suggested_name);
bool ui_file_dialog(Ui *ui, const char *id, char *out_path, usize cap);
bool ui_file_dialog_is_open(Ui *ui, const char *id); /* false once picked from or cancelled */
const char *ui_file_icon(const char *name, bool is_dir); /* symbolic icon for a file type */

/* Wallpapers: built-in names, or an absolute path to a picture. */
#define WALLPAPER_DEFAULT "silk"
extern const char *const WALLPAPER_NAMES[];
extern const char *const WALLPAPER_TITLES[];
extern const int WALLPAPER_COUNT;
Canvas *wallpaper_render(const char *name, int w, int h);

/* ---- Setup assistants (setup.c): Welcome and the Installer share this look ---- */

typedef struct {
	const char *title;              /* under the name: "Setup", "Installer" */
	const char *const *steps;
	int count;
	int current;                    /* count when every step is done */
	f64 changed_at;                 /* when the page changed, for its entrance */
	int dir;                        /* 1 forward, -1 back */
	const char *wallpaper;          /* behind the steps; NULL for the default */
} SetupSteps;

/* Draws the window: the steps on the left, and returns the page's area. */
Rect setup_frame(Ui *ui, const SetupSteps *s);
f32 setup_title(Ui *ui, Rect *r, f64 changed_at, const char *title, const char *about);
Rect setup_rise(Ui *ui, f64 changed_at, int index, Rect r);
bool setup_choice(Ui *ui, Rect r, const char *id, const char *icon, const char *title, const char *text, bool selected, bool enabled,
		  Color tint);
void setup_tick(Ui *ui, Rect r, f64 since, bool ok);
f32 setup_ease(f32 t);

/* ---- App runtime -------------------------------------------------------------------- */

typedef struct {
	const char *id;
	const char *title;
	int width, height;
	int min_width, min_height;
	u32 flags;                         /* WIN_* */
	void (*init)(Ui *ui);
	void (*frame)(Ui *ui);
	bool (*close)(Ui *ui);             /* return false to keep the window open */
	void (*tick)(Ui *ui);              /* every tick_ms, if set */
	u32 tick_ms;
	void *user;
} UiAppDesc;

/* Connects to the display server and runs until the window closes.
 * With ZUI_SNAPSHOT=path in the environment, renders one frame to a PNG instead. */
int ui_run(const UiAppDesc *desc);

/* Event-loop integration for apps doing I/O. */
Loop *ui_loop(Ui *ui);
void ui_wake(Ui *ui);                 /* thread-safe: schedule a redraw */
void ui_set_title(Ui *ui, const char *title);
void ui_window_action(Ui *ui, u32 action);
void ui_window_resize(Ui *ui, int width, int height); /* ask for a new logical size */
void ui_notify(Ui *ui, const char *title, const char *body, const char *icon);
void ui_launch(Ui *ui, const char *app_id, const char *arg);
void ui_settings_changed(Ui *ui);
void ui_session(Ui *ui, u32 action);
/* Asks the desktop to change the account; the answer arrives in ui->reply_* with the returned seq. */
u32 ui_account_update(Ui *ui, u32 flags, const char *full_name, const char *old_password, const char *new_password);
void ui_clipboard_set(Ui *ui, const char *text);
void ui_clipboard_set_data(Ui *ui, const char *mime, const void *data, usize len);
/* Asks the desktop for a screenshot (SCREENSHOT_SCREEN, _WINDOW or _AREA). When it
 * arrives, ui->screenshot_ready is set and ui->screenshot holds the picture (or NULL). */
void ui_screenshot_request(Ui *ui, u32 mode);
const char *ui_user_name(Ui *ui);

/* Wi-Fi through the network service (Settings, the top bar, Welcome). */
typedef struct {
	char ssid[100];          /* for showing */
	char hex[65];            /* the name as the service knows it */
	int signal;              /* 0..100 */
	char security[12];       /* open, wpa2, wpa3, wpa, wep, enterprise */
	bool saved, connected;
} WifiNet;
int wifi_networks(WifiNet *out, int max);       /* strongest first */
bool wifi_supported(const WifiNet *w);          /* open and WPA2 networks can be joined */
bool wifi_command(const char *cmd);             /* "scan", "disconnect", "forget HEX", "radio on|off" */
void wifi_connect(const char *ssid_hex, const char *password);   /* password may be NULL */
const char *wifi_signal_icon(int signal);

/* Renders a frame offscreen (for tests, previews and screenshots). */
bool ui_snapshot(const UiAppDesc *desc, int w, int h, f32 scale, bool dark, const char *png_path);

/* Shared helpers used by the shell and apps. */
const char *zurbax_root(void);        /* "/system", or $ZURBAX_ROOT for development */
char *zurbax_path(const char *rel);   /* zurbax_root() + "/" + rel, allocated */
const char *zurbax_data(void);        /* persistent data, "/data" or $ZURBAX_DATA */
