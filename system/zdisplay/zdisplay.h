/*
 * zdisplay — the Zurbax display server and desktop shell.
 *
 * One process owns the screen: it drives the display hardware (or an
 * offscreen buffer for testing), reads input devices, composites app
 * windows with shadows and animations, manages windows, and draws the
 * shell itself — top panel, dock, application menu, notifications, lock
 * screen and the boot splash that keeps the firmware logo on screen.
 */
#pragma once

#include "zui.h"
#include "zui/driver.h"

#define MAX_WINDOWS 128
#define MAX_CLIENTS 64
#define PANEL_HEIGHT 30
#define RESIZE_MARGIN 8

/* Dock: icon size, gap between icons, shelf padding and height, icon distance from the bottom edge. */
#define DOCK_ICON 48
#define DOCK_SPACING 6
#define DOCK_PAD 10
#define DOCK_SHELF 38
#define DOCK_BOTTOM 8

/* ---- Output backends ---------------------------------------------------------- */

typedef struct Output Output;
struct Output {
	const char *name;
	int width, height;          /* physical pixels */
	int refresh_mhz;
	f32 dpi;
	void *impl;
	/* Copies the damaged parts of `frame` to the screen and schedules a flip.
	 * Returns false if the previous flip is still pending. */
	bool (*present)(Output *o, const Canvas *frame, const Rect *damage, int ndamage);
	bool (*cursor_set)(Output *o, const Canvas *image, int hot_x, int hot_y); /* false: no hw cursor */
	void (*cursor_move)(Output *o, int x, int y); /* x, y: the hotspot position */
	int cursor_hot_x, cursor_hot_y;
	void (*set_gamma)(Output *o, f32 r, f32 g, f32 b); /* night light */
	int fd;                     /* pollable fd for flip events, or -1 */
	void (*on_fd)(Output *o);
	bool flip_pending;
};

Output *output_drm_open(void);
Output *output_headless_open(int w, int h);

/* ---- Windows -------------------------------------------------------------------- */

typedef struct Client Client;

typedef struct Window {
	u32 id;
	u32 client_win;
	Client *client;
	char title[128];
	char app_id[48];
	u32 flags;
	struct Window *parent;
	Rect rect;                  /* logical, content area */
	Rect saved;                 /* geometry before maximize/tile */
	u32 states;
	u32 min_w, min_h;
	f32 radius;
	/* buffer from the client */
	int memfd;
	u8 *map;
	usize map_size;
	u32 bw, bh, stride, slot_bytes;
	int front;                  /* slot on screen, -1 none */
	int release_after_frame;    /* slot to release once the next frame is out */
	bool want_frame;
	bool mapped;
	u32 cursor;
	/* animation */
	f32 appear;                 /* 0..1 */
	bool closing;
	f32 vanish;                 /* 0..1 while closing */
	Canvas *last_frame;         /* kept to animate a window that is closing */
	Rect anim_from, anim_to;
	f64 anim_t0;
	f32 anim_dur;
	bool animating_rect;
	bool minimized;
	f32 minimize_k;             /* 0 shown .. 1 hidden in the dock */
	u64 focus_serial;
	bool configured_once;
} Window;

struct Client {
	int fd;
	int pid;
	char app_id[48];
	bool hello;
	Window *main_window;
};

/* ---- Shell state --------------------------------------------------------------------- */

typedef struct {
	char id[48];
	char name[64];
	char icon[48];
	char exec[256];
	char category[32];
	char keywords[128];
	char opens[160];            /* file extensions this app opens, comma-separated */
	bool hidden;
} AppInfo;

typedef struct {
	u32 id;
	char app[48];
	char icon[48];
	char title[128];
	char body[512];
	i64 time;
	f64 shown_at;
	f32 slide;                  /* 0..1 entering */
	bool dismissed;
	bool read;
} Notice;

typedef struct {
	/* settings */
	bool dark;
	Color accent;
	char accent_name[16];
	char wallpaper[128];
	f32 text_scale;
	f32 scale_override;
	bool natural_scroll;
	bool tap_to_click;
	f32 pointer_speed;
	char keyboard_layout[16];
	bool night_light;
	f32 night_light_strength;
	bool clock_24h;
	bool clock_seconds;
	bool reduce_motion;
	bool dock_autohide;
	bool dock_magnify;
	char dock_apps[512];        /* comma-separated app ids */
	bool do_not_disturb;
	int lock_after_min;
	bool show_battery_percent;
	char timezone[64];
	char user_name[64];
	char full_name[96];
	u32 uid, gid;
	char home[128];
	bool has_password;
	char password_hash[160];
	bool game_mode;
	bool show_fps;
	bool mouse_accel;
	bool alert_sounds;
	bool setup_done;            /* the first-boot assistant has finished */
} Settings;

/* ---- Global server state --------------------------------------------------------------- */

typedef struct {
	Output *out;
	Loop *loop;
	int listen_fd;
	Settings set;
	ThemeInfo theme;

	f32 scale;
	f32 sw, sh;                 /* logical screen size */
	Canvas *frame;              /* composited screen, physical pixels */
	Canvas *wallpaper;          /* scaled to the screen */
	Canvas *wallpaper_blur;     /* for the lock screen */

	Window *windows[MAX_WINDOWS]; /* bottom to top */
	int nwin;
	Window *focus;
	u32 next_window_id;
	u64 focus_serial;
	Client *clients[MAX_CLIENTS];
	int nclients;

	/* damage, physical pixels */
	Rect damage[16];
	int ndamage;
	bool full_damage;

	/* pointer */
	f32 px, py;                 /* logical */
	u32 buttons;
	u32 mods;
	Window *pointer_window;
	Window *grab_window;        /* window receiving pointer while a button is held */
	enum { GRAB_NONE, GRAB_MOVE, GRAB_RESIZE } grab;
	u32 grab_edges;
	f32 grab_x, grab_y;
	Rect grab_rect;
	int snap_zone;              /* 0 none, 1 maximize, 2 left, 3 right */
	Rect snap_preview;
	f32 snap_k;
	u32 cursor;
	bool hw_cursor;
	Canvas *cursor_img[16];

	/* shell */
	AppInfo apps[96];
	int napps;
	Notice notices[64];
	int nnotices;
	u32 next_notice_id;
	bool launcher_open;
	f32 launcher_k;
	bool notifications_open;
	bool session_menu_open;
	bool switcher_open;
	int switcher_index;
	bool locked;
	f32 lock_k;                 /* 1 locked, animating to 0 on unlock */
	bool splash;
	f32 splash_k;
	f64 splash_until;
	bool splash_wait_window;   /* an app is starting; keep the splash until it maps */
	f64 now;
	f64 last_input;
	bool show_desktop;
	char clipboard_mime[48];
	char *clipboard;
	usize clipboard_len;
	f32 osd_k;
	f64 osd_until;
	char osd_icon[48];
	f32 osd_value;
	int volume;                 /* 0..100 */
	bool muted;
	int brightness;             /* 0..100, -1 unavailable */
	int battery;                /* 0..100, -1 none */
	bool charging;
	bool wifi_connected;
	char wifi_name[64];
	int wifi_strength;
	bool shutting_down;
	u32 shutdown_action;
	f32 shutdown_k;
	bool headless;
	char screenshot_dir[160];
	u32 frames_composed;         /* for the frame rate counter */
	f32 fps;
	bool gaming;                 /* game mode active: a full-screen window has focus */
	/* screenshots */
	bool capturing;              /* composing a frame for a screenshot: no cursor */
	bool selecting_area;         /* the user is dragging out an area to capture */
	bool sel_dragging;
	f32 sel_x0, sel_y0, sel_x1, sel_y1;
	Client *shot_client;
	u32 shot_win;
	/* dock intellihide */
	bool dock_hide;              /* target: the dock slides away */
	bool dock_revealed;          /* the pointer touched the bottom edge */
	f32 dock_hide_k;             /* 0 shown .. 1 hidden */
	char dock_menu[48];          /* app whose dock menu is open */
	bool setup_mode;             /* first boot: only the setup assistant, no panel or dock */
} Server;

extern Server S;

/* main.c */
void damage_all(void);
void damage_rect(Rect logical);
void damage_physical(Rect r);
void request_frame(void);
f64 now_s(void);
Rect work_area(void);

/* compose.c */
void compose_frame(void);
void compose_init(void);
void wallpaper_reload(void);
void cursor_update(void);

/* wm.c */
Window *window_create(Client *c, const MsgWinCreate *m);
void window_destroy(Window *w, bool animate);
void window_focus(Window *w);
void window_raise(Window *w);
void window_set_rect(Window *w, Rect r, bool animate);
void window_maximize(Window *w, bool on);
void window_tile(Window *w, int side); /* -1 left, 1 right */
void window_minimize(Window *w, bool on);
void window_configure(Window *w);
void window_close_request(Window *w);
Window *window_at(f32 x, f32 y, u32 *edges);
Rect window_visual_rect(const Window *w);
void wm_pointer_motion(void);
void wm_pointer_button(u32 button, bool pressed);
void wm_begin_move(Window *w);
void wm_begin_resize(Window *w, u32 edges);
void wm_animate(f64 dt);
bool wm_any_animation(void);
Window *wm_top_window(void);
void wm_show_desktop(bool on);

/* server.c */
void server_init(void);
void server_broadcast_theme(void);
void server_send(Client *c, void *msg, u16 type, usize size, u32 win, const int *fds, int nfds);
void server_frame_done(void);
void server_pointer_event(Window *w, u32 kind, u32 button);
void server_scroll_event(Window *w, f32 dx, f32 dy, u32 source, u32 phase);
void server_key_event(Window *w, u32 code, u32 state, const char *text);
void server_focus_event(Window *w, bool focused);
void client_disconnect(Client *c);

/* input.c */
void input_init(void);
void input_key(u32 code, bool pressed, bool repeat);
void input_set_layout(const char *name);
const char *keymap_text(u32 code, u32 mods);
void input_pointer_abs(f32 x, f32 y);
void input_pointer_rel(f32 dx, f32 dy);
void input_button(u32 button, bool pressed);
void input_scroll(f32 dx, f32 dy, u32 source, u32 phase);

/* shell.c */
void shell_init(void);
void shell_reload_apps(void);
void shell_prepare(void);                 /* re-renders the shell overlay if needed */
void shell_draw(Canvas *frame);           /* blends the overlay onto the frame */
void shell_mark_dirty(void);
void shell_surface(Rect r);               /* a shell surface occupies r this frame */
bool shell_pointer(u32 kind, u32 button); /* true: consumed by the shell */
bool shell_scroll(f32 dx, f32 dy, u32 source, u32 phase);
bool shell_key(u32 code, u32 state, u32 mods, const char *text);
void shell_tick(void);
bool shell_animating(void);
f64 shell_next_wake(void);
void shell_notify(const char *app, const char *icon, const char *title, const char *body);
void shell_osd(const char *icon, f32 value);
void shell_lock(void);
void shell_launch(const char *app_id, const char *arg);
void shell_launch_new(const char *app_id);
const AppInfo *shell_app(const char *id);
Rect shell_dock_icon_rect(const char *app_id);
int shell_dock_items(char ids[][48], int max);
Rect shell_dock_bar(int n);
Rect shell_dock_slot(int n, int i);
f32 shell_panel_height(void);
f32 shell_dock_height(void);
void shell_session_action(u32 action);
void shell_screenshot(void);
void screenshot_request(Client *c, u32 win, u32 mode);
void screenshot_client_gone(Client *c);
bool shell_check_password(const char *pw);

/* settings.c */
void settings_load(void);
void settings_save(void);
void settings_apply(void);
void system_status_poll(void);
void game_mode_update(void);
void set_volume(int v);
void set_brightness(int v);
int account_update(const MsgAccount *m, char *msg, usize cap);

/* screentime.c */
void screentime_tick(void);
void screentime_flush(void);

/* splash.c */
void splash_start(void);
void splash_draw(Canvas *frame);
void splash_tick(void);
void splash_finish(void);

/* headless.c: scripted control for automated tests */
void headless_control_init(const char *socket_path);
