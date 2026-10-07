/*
 * The Zurbax display protocol.
 *
 * Apps talk to the display server (zdisplay) over a SOCK_SEQPACKET unix
 * socket, one message per packet. Window pixels travel through shared
 * memory: each window owns a memfd holding two pixel buffers ("slots");
 * the app draws into one slot while the server shows the other.
 *
 * Coordinates in input events and window sizes are logical pixels; buffers
 * are in device pixels (logical * scale).
 */
#pragma once

#include "zr.h"

#define ZUI_SOCKET_PATH "/run/zurbax/display.sock"
#define ZUI_PROTOCOL_VERSION 1
#define ZUI_MAX_MSG 4096

enum {
	/* app -> server */
	MSG_HELLO = 1,
	MSG_WIN_CREATE,
	MSG_WIN_DESTROY,
	MSG_WIN_BUFFER,     /* + memfd */
	MSG_WIN_COMMIT,
	MSG_WIN_TITLE,
	MSG_WIN_ACTION,     /* maximize, minimize, close, fullscreen... */
	MSG_WIN_MOVE,       /* begin interactive move (title bar drag) */
	MSG_WIN_RESIZE,     /* begin interactive resize from an edge */
	MSG_WIN_SIZE_HINTS,
	MSG_CURSOR,
	MSG_NOTIFY,
	MSG_CLIPBOARD_SET,  /* + memfd holding the data */
	MSG_CLIPBOARD_GET,
	MSG_LAUNCH,
	MSG_SETTINGS_CHANGED,
	MSG_SCREENSHOT,     /* + memfd to receive pixels */
	MSG_SESSION,        /* lock, logout, restart, shut down, sleep */
	MSG_WIN_SET_SIZE,   /* the app asks for a new size (e.g. switching modes) */
	MSG_ACCOUNT,        /* change the user's name or password (Settings) */

	/* server -> app */
	MSG_WELCOME = 64,
	MSG_CONFIGURE,
	MSG_RELEASE,        /* a buffer slot may be drawn into again */
	MSG_FRAME,          /* the last commit reached the screen */
	MSG_POINTER,
	MSG_SCROLL,
	MSG_KEY,
	MSG_FOCUS,
	MSG_CLOSE_REQUEST,
	MSG_THEME,
	MSG_CLIPBOARD_DATA, /* + memfd */
	MSG_POPUP_DONE,
	MSG_DROP_FILES,
	MSG_REPLY,          /* result of a request such as MSG_ACCOUNT */
	MSG_SCREENSHOT_DATA,/* + memfd with the pixels */
	MSG_GAMEPAD,        /* a game controller's state changed */
};

typedef struct {
	u16 type;
	u16 size;   /* total message size in bytes, header included */
	u32 win;    /* window id, 0 if not window-specific */
} MsgHeader;

/* ---- app -> server ------------------------------------------------------- */

typedef struct {
	MsgHeader h;
	u32 version;
	u32 pid;
	char app_id[48];       /* e.g. "settings" — matches /system/apps/<id> */
} MsgHello;

enum {
	WIN_RESIZABLE = 1 << 0,
	WIN_POPUP = 1 << 1,       /* menu/tooltip: positioned on parent, closes on outside click */
	WIN_DIALOG = 1 << 2,      /* centred over parent, keeps it inactive */
	WIN_NO_SHADOW = 1 << 3,
	WIN_KEEP_ABOVE = 1 << 4,
	WIN_SPLASH = 1 << 5,      /* borderless, centred (installer, setup) */
	WIN_FULLSCREEN_START = 1 << 6,
};

typedef struct {
	MsgHeader h;
	u32 flags;
	u32 parent;            /* parent window for popups/dialogs */
	i32 x, y;              /* popup position relative to parent (logical) */
	u32 width, height;     /* requested logical size */
	u32 min_width, min_height;
	f32 corner_radius;     /* so the server can shape the shadow */
	char title[128];
} MsgWinCreate;

typedef struct {
	MsgHeader h;
	u32 width, height;     /* device pixels per slot */
	u32 stride;            /* pixels per row */
	u32 slot_bytes;        /* offset of slot 1 */
} MsgWinBuffer;

typedef struct {
	MsgHeader h;
	u32 slot;
	i32 dx, dy, dw, dh;    /* damaged area in device pixels (dw==0: all) */
} MsgWinCommit;

typedef struct {
	MsgHeader h;
	char title[128];
} MsgWinTitle;

enum { ACTION_CLOSE = 1, ACTION_MINIMIZE, ACTION_MAXIMIZE, ACTION_UNMAXIMIZE, ACTION_TOGGLE_MAXIMIZE,
       ACTION_FULLSCREEN, ACTION_UNFULLSCREEN, ACTION_RAISE, ACTION_SHOW_MENU };

typedef struct {
	MsgHeader h;
	u32 action;
	i32 x, y;              /* for ACTION_SHOW_MENU: where (logical, window-relative) */
} MsgWinAction;

enum { EDGE_TOP = 1, EDGE_BOTTOM = 2, EDGE_LEFT = 4, EDGE_RIGHT = 8 };

typedef struct {
	MsgHeader h;
	u32 edges;             /* MSG_WIN_RESIZE only */
} MsgWinMove;

typedef struct {
	MsgHeader h;
	u32 min_width, min_height, max_width, max_height;
} MsgWinSizeHints;

typedef struct {
	MsgHeader h;
	u32 width, height;     /* logical */
} MsgWinSetSize;

enum {
	CURSOR_DEFAULT = 0, CURSOR_POINTER, CURSOR_TEXT, CURSOR_MOVE, CURSOR_RESIZE_NS, CURSOR_RESIZE_EW,
	CURSOR_RESIZE_NWSE, CURSOR_RESIZE_NESW, CURSOR_BUSY, CURSOR_CROSSHAIR, CURSOR_GRAB, CURSOR_HIDDEN,
};

typedef struct {
	MsgHeader h;
	u32 shape;
} MsgCursor;

enum { NOTIFY_NORMAL = 0, NOTIFY_URGENT = 1 };

typedef struct {
	MsgHeader h;
	u32 urgency;
	u32 replaces;          /* id of a previous notification to replace, or 0 */
	char icon[48];         /* app icon name or symbolic icon */
	char title[128];
	char body[512];
} MsgNotify;

typedef struct {
	MsgHeader h;
	u32 size;
	char mime[48];         /* "text/plain", "image/png", "text/uri-list" */
} MsgClipboard;

typedef struct {
	MsgHeader h;
	char app_id[48];       /* app to start, or "" to open `arg` with its default app */
	char arg[1024];        /* file path or URL */
} MsgLaunch;

enum { SESSION_LOCK = 1, SESSION_LOGOUT, SESSION_RESTART, SESSION_SHUTDOWN, SESSION_SLEEP };

typedef struct {
	MsgHeader h;
	u32 action;
} MsgSession;

enum { SCREENSHOT_SCREEN = 0, SCREENSHOT_WINDOW = 1, SCREENSHOT_AREA = 2 };

typedef struct {
	MsgHeader h;
	u32 mode;              /* SCREENSHOT_* */
} MsgScreenshot;

enum { ACCOUNT_SET_NAME = 1, ACCOUNT_SET_PASSWORD = 2, ACCOUNT_REMOVE_PASSWORD = 4, ACCOUNT_FINISH_SETUP = 8 };

typedef struct {
	MsgHeader h;
	u32 seq;               /* echoed in the MSG_REPLY */
	u32 flags;             /* ACCOUNT_* */
	char full_name[96];
	char old_password[128];
	char new_password[128];
} MsgAccount;

/* ---- server -> app ---------------------------------------------------------- */

typedef struct {
	u8 dark;
	u8 reduce_motion;
	u8 high_contrast;
	u8 _pad;
	u32 accent;            /* 0xAARRGGBB */
	f32 text_scale;        /* user font size multiplier */
	u32 _reserved[4];
} ThemeInfo;

typedef struct {
	MsgHeader h;
	u32 version;
	u32 screen_width, screen_height; /* logical */
	f32 scale;
	ThemeInfo theme;
	u32 uid;
	char user_name[64];
	char home[128];
} MsgWelcome;

enum {
	STATE_ACTIVE = 1 << 0,
	STATE_MAXIMIZED = 1 << 1,
	STATE_FULLSCREEN = 1 << 2,
	STATE_TILED_LEFT = 1 << 3,
	STATE_TILED_RIGHT = 1 << 4,
	STATE_MINIMIZED = 1 << 5,
};

typedef struct {
	MsgHeader h;
	u32 width, height;     /* logical size the window should draw at */
	f32 scale;
	u32 states;
} MsgConfigure;

typedef struct {
	MsgHeader h;
	u32 slot;
} MsgRelease;

typedef struct {
	MsgHeader h;
	u64 time_ns;           /* when the frame was shown */
} MsgFrame;

enum { POINTER_MOTION = 1, POINTER_DOWN, POINTER_UP, POINTER_ENTER, POINTER_LEAVE };
enum { BUTTON_LEFT = 1, BUTTON_RIGHT = 2, BUTTON_MIDDLE = 3, BUTTON_BACK = 4, BUTTON_FORWARD = 5 };

enum {
	MOD_SHIFT = 1 << 0,
	MOD_CTRL = 1 << 1,
	MOD_ALT = 1 << 2,
	MOD_SUPER = 1 << 3,
	MOD_CAPS = 1 << 4,
};

typedef struct {
	MsgHeader h;
	u32 kind;
	u32 button;
	u32 mods;
	f32 x, y;              /* logical, window-relative */
	u64 time_ms;
} MsgPointer;

enum { SCROLL_WHEEL = 1, SCROLL_TOUCHPAD = 2 };
enum { SCROLL_PHASE_NONE = 0, SCROLL_PHASE_BEGIN, SCROLL_PHASE_UPDATE, SCROLL_PHASE_END };

typedef struct {
	MsgHeader h;
	f32 dx, dy;            /* logical pixels; positive dy scrolls content up (moves view down) */
	u32 source;
	u32 phase;             /* touchpad gestures: END lets apps start kinetic scrolling */
	u32 mods;
	f32 x, y;
} MsgScroll;

enum { KEY_RELEASED = 0, KEY_PRESSED = 1, KEY_REPEATED = 2 };

typedef struct {
	MsgHeader h;
	u32 code;              /* evdev key code (see keys.h) */
	u32 state;
	u32 mods;
	char text[8];          /* UTF-8 text the key produced, "" for none */
	u64 time_ms;
} MsgKey;

typedef struct {
	MsgHeader h;
	u32 focused;
} MsgFocus;

typedef struct {
	MsgHeader h;
	ThemeInfo theme;
} MsgTheme;

typedef struct {
	MsgHeader h;
	u32 count;
	f32 x, y;
	char paths[3000];      /* newline-separated */
} MsgDropFiles;

typedef struct {
	MsgHeader h;
	u32 seq;
	i32 status;            /* 0 = done, negative = failed */
	char message[128];
} MsgReply;

typedef struct {
	MsgHeader h;
	i32 status;            /* 0 = pixels attached, negative = cancelled or failed */
	u32 width, height;     /* device pixels, premultiplied ARGB, rows packed */
} MsgScreenshotData;

/* Game controllers. Buttons are named by position, Xbox style: A is the
 * bottom face button, B the right, X the left and Y the top, whatever the
 * labels on the controller say. */
enum {
	PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LB, PAD_RB, PAD_LT, PAD_RT, PAD_BACK, PAD_START, PAD_GUIDE,
	PAD_LS, PAD_RS, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_BUTTONS
};
/* Sticks go from -1 to 1 (up and left negative); triggers from 0 to 1. */
enum { PAD_LX, PAD_LY, PAD_RX, PAD_RY, PAD_LTRIGGER, PAD_RTRIGGER, PAD_AXES };
#define PAD_MAX 4

typedef struct {
	MsgHeader h;
	u32 pad;               /* 0..PAD_MAX-1 */
	u32 connected;         /* 0 when it was unplugged */
	u32 buttons;           /* bit per PAD_* button */
	f32 axes[PAD_AXES];
	char name[64];
} MsgGamepad;
