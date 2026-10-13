/*
 * zdisplay — the Zurbax display server and desktop shell.
 *
 * One process owns the screen: it drives the display hardware (or an
 * offscreen buffer for testing), reads input devices, composites app
 * windows with shadows and animations, manages windows, and draws the
 * shell itself — the top bar, the taskbar, the launcher, notifications,
 * lock screen and the boot splash that keeps the firmware logo on screen.
 */
#pragma once

#include "zui.h"
#include "zui/driver.h"

#define MAX_WINDOWS 128
#define MAX_CLIENTS 64
#define PANEL_HEIGHT 30         /* the top bar */
#define RESIZE_MARGIN 8
#define CURSOR_CUSTOM 12            /* after the protocol's shapes: S.cursor_custom */


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
	/* Programs' GPU buffers (linux-dmabuf). device: the GPU's device number,
	 * 0 when there is none to share buffers with. A buffer imported as a
	 * framebuffer can go straight to the screen with flip_to. */
	u64 device;
	u32 (*fb_import)(Output *o, int dmabuf, u32 width, u32 height, u32 offset, u32 stride, u64 modifier);
	void (*fb_release)(Output *o, u32 fb);
	bool (*flip_to)(Output *o, u32 fb);
	/* Hands the screen to another compositor (a GNOME session) and takes it
	 * back: the right to set modes goes, then returns with a full redraw. */
	void (*pause)(Output *o, bool paused);
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
	char app_id[96];
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
	void *wl;                   /* a Wayland program's surface (wayland.c), or NULL */
	Rect shown;                 /* logical rect it was last drawn at */
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
	char id[96];
	char name[64];
	char icon[256];             /* an icon name, or a picture's path */
	char exec[256];             /* the program; for Linux apps, the menu entry's command */
	char category[32];
	char keywords[192];
	char opens[160];            /* file extensions this app opens, comma-separated */
	bool hidden;
	bool is_linux;              /* a Linux app (Flatpak), started through layerrun */
} AppInfo;

typedef struct {
	u32 id;
	char app[96];
	char icon[256];
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
	bool dock_autohide;         /* the taskbar hides until the pointer reaches the bottom edge */
	bool topbar;                /* the bar across the top: app menu, clock, status */
	int taskbar_size;           /* 0 small, 1 medium, 2 large */
	bool taskbar_center;        /* the buttons centred, or from the left */
	bool taskbar_labels;        /* app names beside the icons */
	bool taskbar_search;        /* the search box */
	bool taskbar_desktop;       /* the show-desktop button at the far right */
	bool taskbar_previews;      /* window previews when hovering a button */
	bool taskbar_light;         /* in the light style the taskbar is light too (otherwise always dark) */
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
	bool pad_rumble;            /* controllers may vibrate */
	bool alert_sounds;
	bool setup_done;            /* the first-boot assistant has finished */
	char desktop[16];           /* what unlocking opens: "zurbax", or "gnome" from the Linux layer */
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
	AppInfo apps[256];
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
	bool wifi_connected;        /* any network, wired or not */
	bool net_wired;
	char wifi_name[64];
	int wifi_strength;
	bool wifi_radio, wifi_available;   /* from the network service */
	char wifi_state[16];               /* off, idle, connecting, connected */
	bool shutting_down;
	u32 shutdown_action;
	f32 shutdown_k;
	bool headless;
	char screenshot_dir[160];
	u32 frames_composed;         /* for the frame rate counter */
	f32 fps;
	bool gaming;                 /* game mode active: a full-screen window has focus */
	u64 started_ms;              /* when the display server started (monotonic) */
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
	/* a cursor picture from a Wayland program (CURSOR_CUSTOM) */
	const Canvas *cursor_custom;
	int cursor_custom_hx, cursor_custom_hy;
	u32 cursor_custom_serial;
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
void compose_splash_frame(Rect area);
void wallpaper_reload(void);
void cursor_update(void);
void compose_window_thumb(Canvas *dst, Window *w, Rect physical);

/* wm.c */
Window *window_create(Client *c, const MsgWinCreate *m);
void window_destroy(Window *w, bool animate);
void window_focus(Window *w);
void window_raise(Window *w);
void window_set_rect(Window *w, Rect r, bool animate);
void window_maximize(Window *w, bool on);
void window_fullscreen(Window *w, bool on);
void window_tile(Window *w, int side); /* -1 left, 1 right */
void window_minimize(Window *w, bool on);
void window_configure(Window *w);
void window_close_request(Window *w);
void window_damage(Window *w);
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
void wm_refit(void);

/* server.c */
void server_init(void);
void server_broadcast_theme(void);
void server_send(Client *c, void *msg, u16 type, usize size, u32 win, const int *fds, int nfds);
void server_frame_done(void);
void server_pointer_event(Window *w, u32 kind, u32 button);
void server_scroll_event(Window *w, f32 dx, f32 dy, u32 source, u32 phase);
void server_key_event(Window *w, u32 code, u32 state, const char *text);
void server_gamepad_event(Window *w, const MsgGamepad *m);
/* Game controllers (input_evdev.c) */
void input_gamepads_focus(Window *w, bool focused);
int input_gamepads(char names[][64], int max);
void input_activity(void);
void input_fake_pad(int button, bool down, int axis, f32 value);   /* headless tests */
void input_pad_rumble(int pad, f32 strong, f32 weak, u32 ms);       /* pad -1: all */
int input_pad_battery(int pad);                                     /* percent, or -1 if unknown */
/* The Applications launcher with arrow keys or a controller (shell_ui.c) */
void launcher_nav(int dx, int dy);
void launcher_activate(void);
void server_focus_event(Window *w, bool focused);
void client_disconnect(Client *c);

/* input.c */
void input_init(void);
/* Stops reading every input device (another compositor has them), or starts again. */
void input_pause(bool paused);
void input_key(u32 code, bool pressed, bool repeat);
void input_set_layout(const char *name);
const char *keymap_text(u32 code, u32 mods);
bool input_key_texts(u32 code, const char **lower, const char **upper, const char **altgr);
void input_pointer_abs(f32 x, f32 y);
void input_pointer_rel(f32 dx, f32 dy);
void input_button(u32 button, bool pressed);
void input_scroll(f32 dx, f32 dy, u32 source, u32 phase);

/* shell.c */
void shell_init(void);
void shell_reload_apps(void);
char *user_runtime_dir(u32 uid, u32 gid);
void shell_services_tick(void);
/* Starts a program as the person (home, runtime folder, groups); pid or -errno. */
int shell_user_spawn(const char *path, char **argv);
/* Stops zportal (GNOME brings its own portals) until shell_services_tick starts it again. */
void shell_stop_portal(void);

/* ---- GNOME, the second desktop (gnome.c) ---- */
bool gnome_available(void);          /* the layer has GNOME, and Zurbax has zsys */
bool gnome_running(void);            /* the screen is GNOME's now */
bool gnome_start(void);              /* hands the screen over and starts the session */
void gnome_reaped(int pid, int status);
void gnome_ended(int pid, int status);
void shell_prepare(void);                 /* re-renders the shell overlay if needed */
void shell_draw(Canvas *frame);           /* blends the overlay onto the frame */
void shell_mark_dirty(void);
void shell_surface(Rect r);               /* a shell surface occupies r this frame */
bool shell_pointer(u32 kind, u32 button); /* true: consumed by the shell */
bool shell_scroll(f32 dx, f32 dy, u32 source, u32 phase);
bool shell_idle(void);                    /* nothing of the shell on screen this frame */
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
/* The taskbar along the bottom, laid out for the current settings. */
typedef struct {
	Rect bar;
	Rect start;                           /* the Start button */
	Rect search;                          /* the search box (empty when it's turned off) */
	Rect tray;                            /* the right end */
	Rect clock;                           /* the clock in the tray (empty while the top bar shows it) */
	Rect bell;                            /* notifications */
	Rect status;                          /* status icons in the tray (empty while the top bar shows them) */
	Rect desktop;                         /* show the desktop, at the far right (empty when turned off) */
	f32 apps_x, slot_w, icon;             /* the first task button's left edge, a task button's width, icon size */
	int n;                                /* task buttons */
	bool labels;                          /* names beside the icons */
} Taskbar;
void shell_taskbar(Taskbar *t);
int shell_status_icons(const char *icons[8], int *battery_at);
Rect shell_dock_slot(int n, int i);
f32 shell_panel_height(void);             /* the top bar, or 0 when it's turned off */
f32 shell_taskbar_height(void);
f32 shell_taskbar_reserved(void);         /* the screen kept for the taskbar (0 while it auto-hides) */
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
void splash_thread_start(void);
void splash_thread_stop(void);
void splash_draw(Canvas *frame);
void splash_tick(void);
void splash_finish(void);

/* wayland.c: the Wayland protocol, for standard Linux programs (Xwayland, games) */
void wl_server_init(void);
const char *wl_socket_path(void);         /* for programs' WAYLAND_DISPLAY */
void wl_flush(void);                      /* before waiting: sends what's queued */
void wl_frame_done(void);                 /* after each composed frame */
void wl_window_draw(Canvas *frame, Window *w, Rect physical, f32 opacity);
void wl_window_attach(Window *w);
Canvas *wl_window_gone(Window *w, bool keep_picture);
void wl_window_configure(Window *w);
void wl_window_close(Window *w);
void wl_pointer_press(Window *w);         /* any click, w under it or NULL: closes menus */
void wl_pointer_event(Window *w, u32 kind, u32 button);
void wl_scroll_event(Window *w, f32 dx, f32 dy, u32 source, u32 phase);
void wl_key_event(Window *w, u32 code, u32 state);
void wl_focus_event(Window *w, bool focused);
void wl_keymap_changed(void);
void wl_clipboard_changed(void);          /* Zurbax's clipboard has something new */
void wl_relative_motion(f32 dx, f32 dy, f32 raw_dx, f32 raw_dy);
bool wl_pointer_locked(void);
void wl_confine(f32 *x, f32 *y);
bool wl_window_scanout(Window *w);        /* its GPU buffer straight to the screen */
void wl_flip_complete(void);              /* the output finished a flip */
/* wm.c: a window for a Wayland surface */
Window *window_create_wl(void *surface, const char *title, const char *app_id, f32 w, f32 h, u32 flags, Window *parent, f32 x, f32 y);

/* desktop.c: the files in ~/Desktop as icons on the wallpaper */
void desktop_init(void);
const Canvas *desktop_background(void);     /* the wallpaper with the icons on it, for composing */
void desktop_invalidate(void);              /* the wallpaper, theme or size changed */
bool desktop_pointer(u32 kind, u32 button); /* the pointer on the desktop itself; true: taken */
bool desktop_grabbing(void);                /* a press on the desktop holds the pointer */
bool desktop_key(u32 code, u32 state, u32 mods, const char *text);
bool desktop_focused(void);
void desktop_unfocus(void);
void desktop_tick(void);
bool desktop_reaped(int pid, int status);   /* a file helper finished */
void desktop_menu_draw(Ui *ui);             /* its menus and renaming, in the shell overlay */
bool desktop_wants_pointer(void);
bool desktop_wants_keys(void);
void desktop_drag_over(f32 x, f32 y, const char *paths, bool from_desktop); /* x < 0: not over it */
void desktop_drop(const char *paths, f32 x, f32 y, u32 mods, bool from_desktop);
const char *desktop_dir(void);
Ui *desktop_aux_ui(Canvas *target, f32 w, f32 h); /* a drawing context for offscreen pictures */

/* cliphistory.c: what was copied lately, on Super+V */
void clip_history_add(const char *mime, const char *data, usize len);
void clip_history_toggle(void);
bool clip_history_open(void);
void clip_history_draw(Ui *ui);

/* dnd.c: files dragged between windows, the desktop and the taskbar */
void dnd_start(Window *source, const char *paths, int count, Canvas *thumb); /* source NULL: the desktop */
bool dnd_active(void);
void dnd_motion(void);
void dnd_button(u32 button, bool pressed);
bool dnd_key(u32 code, u32 state);
void dnd_draw(Canvas *frame);
void dnd_tick(void);
bool dnd_animating(void);
void dnd_window_gone(Window *w);
extern char dnd_hover_app[48];              /* the taskbar button files are held over */

/* headless.c: scripted control for automated tests */
void headless_control_init(const char *socket_path);
