/*
 * Input devices: reads Linux evdev nodes directly.
 *
 * Keyboards, mice, touchpads (multi-touch, tap to click, two-finger
 * scrolling with momentum, clickpad right-click areas), touchscreens, game
 * controllers, the power button and the lid switch. New devices are picked
 * up as they appear.
 */
#include "zdisplay.h"

/* ---- Kernel ABI ------------------------------------------------------------------- */

struct input_event {
	i64 sec, usec;
	u16 type, code;
	i32 value;
};

struct input_absinfo {
	i32 value, minimum, maximum, fuzz, flat, resolution;
};

#define EVIOCGNAME(len) IOC(IOC_READ, 'E', 0x06, len)
#define EVIOCGPROP(len) IOC(IOC_READ, 'E', 0x09, len)
#define EVIOCGBIT(ev, len) IOC(IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGABS(abs) IOR('E', 0x40 + (abs), struct input_absinfo)
#define EVIOCGRAB IOW('E', 0x90, int)

enum { EV_SYN = 0, EV_KEY = 1, EV_REL = 2, EV_ABS = 3, EV_MSC = 4, EV_SW = 5, EV_LED = 0x11 };
enum { SYN_REPORT = 0, SYN_DROPPED = 3 };
enum { REL_X = 0, REL_Y = 1, REL_HWHEEL = 6, REL_WHEEL = 8, REL_WHEEL_HI_RES = 11, REL_HWHEEL_HI_RES = 12 };
enum {
	ABS_X = 0, ABS_Y = 1, ABS_Z = 2, ABS_RX = 3, ABS_RY = 4, ABS_RZ = 5, ABS_GAS = 9, ABS_BRAKE = 10,
	ABS_HAT0X = 0x10, ABS_HAT0Y = 0x11, ABS_MT_SLOT = 0x2f, ABS_MT_POSITION_X = 0x35, ABS_MT_POSITION_Y = 0x36,
	ABS_MT_TRACKING_ID = 0x39,
};
/* Game controller buttons (the kernel's gamepad layout, Documentation/input/gamepad.rst) */
enum {
	BTN_JOYSTICK = 0x120, BTN_GAMEPAD = 0x130, BTN_SOUTH = 0x130, BTN_EAST = 0x131, BTN_C = 0x132, BTN_NORTH = 0x133,
	BTN_WEST = 0x134, BTN_Z = 0x135, BTN_TL = 0x136, BTN_TR = 0x137, BTN_TL2 = 0x138, BTN_TR2 = 0x139,
	BTN_SELECT = 0x13a, BTN_START = 0x13b, BTN_MODE = 0x13c, BTN_THUMBL = 0x13d, BTN_THUMBR = 0x13e,
	BTN_DPAD_UP = 0x220, BTN_DPAD_DOWN = 0x221, BTN_DPAD_LEFT = 0x222, BTN_DPAD_RIGHT = 0x223,
};
enum { INPUT_PROP_DIRECT = 1, INPUT_PROP_BUTTONPAD = 2 };
enum { SW_LID = 0 };
enum { LED_NUML = 0, LED_CAPSL = 1 };

/* ---- Devices ------------------------------------------------------------------------ */

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_TOUCHPAD = 4, DEV_TOUCHSCREEN = 8, DEV_SWITCH = 16, DEV_TABLET = 32, DEV_GAMEPAD = 64 };

#define MAX_SLOTS 5

typedef struct {
	int id;                     /* tracking id, -1 when lifted */
	f32 x, y;
	f32 sx, sy;                 /* where the touch started */
	bool fresh_x, fresh_y;      /* new touch: the next position is its start */
} Slot;

typedef struct Device {
	int fd;
	char path[32];
	char name[80];
	u32 kind;
	bool clickpad;
	/* relative pointer, accumulated until SYN_REPORT */
	f32 rel_x, rel_y, wheel_y, wheel_x;
	bool hires_wheel;
	/* absolute axes */
	struct input_absinfo ax, ay;
	f32 res_x, res_y;           /* units per millimetre */
	Slot slots[MAX_SLOTS];
	int slot;
	int fingers;                /* from BTN_TOOL_* */
	int prev_fingers;
	f32 last_cx, last_cy;       /* centroid of the touches last frame */
	bool have_last;
	/* tap detection */
	f64 touch_t0;
	int tap_fingers;
	bool tap_moved;
	bool button_down;
	u32 click_button;           /* clickpad: which button the physical click became */
	/* two-finger scrolling */
	bool scrolling;
	f32 scroll_vx, scroll_vy;
	/* touchscreen */
	bool touch_down;
	bool abs_dirty;
	/* game controller */
	int pad;                    /* slot, 0..PAD_MAX-1 */
	bool xbox;                  /* xpad: BTN_X/BTN_Y are the left/top buttons */
	bool joystick;              /* old-style joystick buttons (BTN_TRIGGER...) */
	bool triggers_on_z;         /* ABS_Z/ABS_RZ are triggers, not a second stick */
	struct input_absinfo pad_abs[12];
	u32 pad_buttons, hat_buttons;
	f32 pad_axes[PAD_AXES];
	bool pad_dirty;
} Device;

static Device *pads[PAD_MAX];

static Device *devices[32];
static int ndevices;
static int inotify_fd = -1;
static u32 caps_shown = ~0u;

static bool test_bit(const u8 *bits, u32 n)
{
	return bits[n / 8] & (1u << (n % 8));
}

static void pad_announce(Device *d, bool connected);

static void device_close(Device *d)
{
	if (d->kind & DEV_GAMEPAD) {
		pad_announce(d, false);
		pads[d->pad] = NULL;
	}
	loop_del_fd(S.loop, d->fd);
	sys_close(d->fd);
	for (int i = 0; i < ndevices; i++)
		if (devices[i] == d) {
			devices[i] = devices[--ndevices];
			break;
		}
	mem_free(d);
}

static f32 abs_to_mm(const Device *d, f32 units, bool y)
{
	f32 res = y ? d->res_y : d->res_x;
	return units / res;
}

/* ---- Touchpad ------------------------------------------------------------------------ */

/* Pointer acceleration: slow movements stay precise, fast ones cover distance. */
static f32 accel(f32 speed_mm_per_frame)
{
	return 1.0f + clampf((speed_mm_per_frame - 0.4f) * 0.9f, 0, 2.2f);
}

static void touchpad_frame(Device *d)
{
	int active = 0;
	f32 cx = 0, cy = 0;
	for (int i = 0; i < MAX_SLOTS; i++)
		if (d->slots[i].id >= 0) {
			active++;
			cx += d->slots[i].x;
			cy += d->slots[i].y;
		}
	int fingers = MAX(d->fingers, active);
	if (active > 0) {
		cx /= (f32)active;
		cy /= (f32)active;
	}

	/* Finger count changed: restart relative tracking to avoid jumps. */
	if (fingers != d->prev_fingers) {
		if (fingers > 0 && d->prev_fingers == 0) {
			d->touch_t0 = now_s();
			d->tap_fingers = fingers;
			d->tap_moved = false;
			for (int i = 0; i < MAX_SLOTS; i++)
				d->slots[i].sx = d->slots[i].x, d->slots[i].sy = d->slots[i].y;
		}
		if (fingers > d->tap_fingers && d->prev_fingers > 0)
			d->tap_fingers = fingers;
		if (d->scrolling && fingers != 2) {
			input_scroll(0, 0, SCROLL_TOUCHPAD, SCROLL_PHASE_END);
			d->scrolling = false;
		}
		if (fingers == 0 && d->prev_fingers > 0) {
			/* Tap: short, still, and no physical click. */
			f64 dt = now_s() - d->touch_t0;
			if (S.set.tap_to_click && !d->tap_moved && dt < 0.22 && !d->button_down && d->tap_fingers <= 3) {
				u32 b = d->tap_fingers == 1 ? BUTTON_LEFT : d->tap_fingers == 2 ? BUTTON_RIGHT : BUTTON_MIDDLE;
				input_button(b, true);
				input_button(b, false);
			}
		}
		d->prev_fingers = fingers;
		d->have_last = false;
	}
	if (active == 0)
		return;
	if (!d->have_last) {
		d->last_cx = cx, d->last_cy = cy;
		d->have_last = true;
		return;
	}
	f32 dx_mm = abs_to_mm(d, cx - d->last_cx, false), dy_mm = abs_to_mm(d, cy - d->last_cy, true);
	d->last_cx = cx, d->last_cy = cy;
	for (int i = 0; i < MAX_SLOTS; i++) {
		Slot *s = &d->slots[i];
		if (s->id >= 0 && (fabsf(abs_to_mm(d, s->x - s->sx, false)) > 2.0f ||
				   fabsf(abs_to_mm(d, s->y - s->sy, true)) > 2.0f))
			d->tap_moved = true;
	}
	if (fingers == 1 || (d->button_down && fingers >= 1)) {
		f32 speed = sqrtf(dx_mm * dx_mm + dy_mm * dy_mm);
		f32 k = 3.2f * accel(speed) * (0.5f + S.set.pointer_speed);
		input_pointer_abs(S.px + dx_mm * k, S.py + dy_mm * k);
	} else if (fingers == 2) {
		/* Scroll content with the fingers; zui applies momentum after the
		 * END phase from the recent velocity. */
		f32 k = 6.0f;
		f32 sx = -dx_mm * k, sy = -dy_mm * k;
		if (!d->scrolling) {
			if (fabsf(sx) + fabsf(sy) < 0.5f)
				return;
			d->scrolling = true;
			input_scroll(sx, sy, SCROLL_TOUCHPAD, SCROLL_PHASE_BEGIN);
		} else {
			input_scroll(sx, sy, SCROLL_TOUCHPAD, SCROLL_PHASE_UPDATE);
		}
	}
}

static void touchpad_button(Device *d, bool pressed)
{
	if (pressed) {
		u32 b = BUTTON_LEFT;
		if (d->clickpad) {
			/* Two fingers anywhere, or one in the bottom-right corner: right click. */
			int active = 0;
			Slot *one = NULL;
			for (int i = 0; i < MAX_SLOTS; i++)
				if (d->slots[i].id >= 0) {
					active++;
					one = &d->slots[i];
				}
			int fingers = MAX(active, d->fingers);
			if (fingers == 2)
				b = BUTTON_RIGHT;
			else if (fingers >= 3)
				b = BUTTON_MIDDLE;
			else if (one) {
				f32 fx = (one->x - d->ax.minimum) / (f32)MAX(1, d->ax.maximum - d->ax.minimum);
				f32 fy = (one->y - d->ay.minimum) / (f32)MAX(1, d->ay.maximum - d->ay.minimum);
				if (fy > 0.82f && fx > 0.55f)
					b = BUTTON_RIGHT;
			}
		}
		d->click_button = b;
		d->button_down = true;
		input_button(b, true);
	} else if (d->button_down) {
		d->button_down = false;
		input_button(d->click_button, false);
	}
}

/* ---- Event handling ------------------------------------------------------------------ */

static void update_leds(void)
{
	u32 caps = S.mods & MOD_CAPS;
	if (caps == caps_shown)
		return;
	caps_shown = caps;
	for (int i = 0; i < ndevices; i++) {
		if (!(devices[i]->kind & DEV_KEYBOARD))
			continue;
		struct input_event ev[2] = { { .type = EV_LED, .code = LED_CAPSL, .value = caps ? 1 : 0 },
					     { .type = EV_SYN, .code = SYN_REPORT } };
		sys_write(devices[i]->fd, ev, sizeof ev);
	}
}

static u32 mouse_button(u32 code)
{
	switch (code) {
	case BTN_LEFT_CODE: return BUTTON_LEFT;
	case BTN_RIGHT_CODE: return BUTTON_RIGHT;
	case BTN_MIDDLE_CODE: return BUTTON_MIDDLE;
	case BTN_SIDE_CODE: return BUTTON_BACK;
	case BTN_EXTRA_CODE: return BUTTON_FORWARD;
	default: return 0;
	}
}

/* ---- Game controllers ------------------------------------------------------------------ */

static void pad_message(const Device *d, bool connected, MsgGamepad *m)
{
	memset(m, 0, sizeof *m);
	m->pad = (u32)d->pad;
	m->connected = connected;
	if (connected) {
		m->buttons = d->pad_buttons | d->hat_buttons;
		memcpy(m->axes, d->pad_axes, sizeof m->axes);
	}
	str_copy(m->name, d->name, sizeof m->name);
}

static void pad_announce(Device *d, bool connected)
{
	MsgGamepad m;
	pad_message(d, connected, &m);
	if (S.focus)
		server_gamepad_event(S.focus, &m);
}

void input_gamepads_focus(Window *w, bool focused)
{
	/* the newly focused window learns what's plugged in; the old one lets go of everything */
	for (int i = 0; i < PAD_MAX; i++) {
		if (!pads[i])
			continue;
		MsgGamepad m;
		pad_message(pads[i], true, &m);
		if (!focused) {
			m.buttons = 0;
			memset(m.axes, 0, sizeof m.axes);
		}
		server_gamepad_event(w, &m);
	}
}

int input_gamepads(char names[][64], int max)
{
	int n = 0;
	for (int i = 0; i < PAD_MAX && n < max; i++)
		if (pads[i])
			str_copy(names[n++], pads[i]->name, 64);
	return n;
}

static int pad_button(const Device *d, u32 code)
{
	if (d->joystick && code >= BTN_JOYSTICK && code < BTN_JOYSTICK + 12) {
		static const int order[12] = { PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LB, PAD_RB, PAD_LT, PAD_RT, PAD_BACK, PAD_START, PAD_LS, PAD_RS };
		return order[code - BTN_JOYSTICK];
	}
	switch (code) {
	case BTN_SOUTH: return PAD_A;
	case BTN_EAST: return PAD_B;
	case BTN_NORTH: return d->xbox ? PAD_X : PAD_Y;
	case BTN_WEST: return d->xbox ? PAD_Y : PAD_X;
	case BTN_TL: return PAD_LB;
	case BTN_TR: return PAD_RB;
	case BTN_TL2: return PAD_LT;
	case BTN_TR2: return PAD_RT;
	case BTN_SELECT: return PAD_BACK;
	case BTN_START: return PAD_START;
	case BTN_MODE: return PAD_GUIDE;
	case BTN_THUMBL: return PAD_LS;
	case BTN_THUMBR: return PAD_RS;
	case BTN_DPAD_UP: return PAD_UP;
	case BTN_DPAD_DOWN: return PAD_DOWN;
	case BTN_DPAD_LEFT: return PAD_LEFT;
	case BTN_DPAD_RIGHT: return PAD_RIGHT;
	default: return -1;
	}
}

/* The shell uses controllers too: Guide opens Applications, and while it's
 * open the D-pad moves through the apps, A starts one and B closes it. */
static bool pad_shell(Device *d, int b, bool down)
{
	if (!down || S.locked)
		return false;
	if (b == PAD_GUIDE && !S.setup_mode) {
		S.launcher_open = !S.launcher_open;
		if (S.launcher_open)
			shell_reload_apps();
		shell_mark_dirty();
		return true;
	}
	if (!S.launcher_open)
		return false;
	switch (b) {
	case PAD_UP: launcher_nav(0, -1); break;
	case PAD_DOWN: launcher_nav(0, 1); break;
	case PAD_LEFT: launcher_nav(-1, 0); break;
	case PAD_RIGHT: launcher_nav(1, 0); break;
	case PAD_A: launcher_activate(); break;
	case PAD_B: S.launcher_open = false; break;
	default: return false;
	}
	shell_mark_dirty();
	return true;
}

static void pad_key(Device *d, u32 code, i32 value)
{
	int b = pad_button(d, code);
	if (b < 0 || value == 2)
		return;
	u32 bit = 1u << b;
	bool down = value != 0;
	if (pad_shell(d, b, down))
		return;
	d->pad_buttons = down ? (d->pad_buttons | bit) : (d->pad_buttons & ~bit);
	d->pad_dirty = true;
}

static f32 pad_norm(const struct input_absinfo *a, i32 v, bool centered)
{
	f32 range = (f32)MAX(1, a->maximum - a->minimum);
	f32 x = ((f32)v - (f32)a->minimum) / range;   /* 0..1 */
	if (!centered)
		return clampf(x, 0, 1);
	x = x * 2 - 1;
	/* the hardware's own dead zone around the centre */
	f32 flat = (f32)a->flat * 2 / range;
	if (fabsf(x) < flat)
		return 0;
	return clampf(x, -1, 1);
}

static void pad_abs(Device *d, u32 code, i32 value)
{
	if (code == ABS_HAT0X || code == ABS_HAT0Y) {
		u32 neg = 1u << (code == ABS_HAT0X ? PAD_LEFT : PAD_UP), pos = 1u << (code == ABS_HAT0X ? PAD_RIGHT : PAD_DOWN);
		d->hat_buttons &= ~(neg | pos);
		if (value < 0)
			d->hat_buttons |= neg;
		else if (value > 0)
			d->hat_buttons |= pos;
		/* the shell's turn first */
		if (value != 0 && pad_shell(d, value < 0 ? (code == ABS_HAT0X ? PAD_LEFT : PAD_UP) : (code == ABS_HAT0X ? PAD_RIGHT : PAD_DOWN), true))
			d->hat_buttons &= ~(neg | pos);
		d->pad_dirty = true;
		return;
	}
	if (code > ABS_BRAKE)
		return;
	const struct input_absinfo *a = &d->pad_abs[code];
	switch (code) {
	case ABS_X: d->pad_axes[PAD_LX] = pad_norm(a, value, true); break;
	case ABS_Y: d->pad_axes[PAD_LY] = pad_norm(a, value, true); break;
	case ABS_RX: d->pad_axes[PAD_RX] = pad_norm(a, value, true); break;
	case ABS_RY: d->pad_axes[PAD_RY] = pad_norm(a, value, true); break;
	case ABS_Z:
		if (d->triggers_on_z)
			d->pad_axes[PAD_LTRIGGER] = pad_norm(a, value, false);
		else
			d->pad_axes[PAD_RX] = pad_norm(a, value, true);
		break;
	case ABS_RZ:
		if (d->triggers_on_z)
			d->pad_axes[PAD_RTRIGGER] = pad_norm(a, value, false);
		else
			d->pad_axes[PAD_RY] = pad_norm(a, value, true);
		break;
	case ABS_BRAKE: d->pad_axes[PAD_LTRIGGER] = pad_norm(a, value, false); break;
	case ABS_GAS: d->pad_axes[PAD_RTRIGGER] = pad_norm(a, value, false); break;
	default: return;
	}
	d->pad_dirty = true;
}

static void pad_syn(Device *d)
{
	if (!d->pad_dirty)
		return;
	d->pad_dirty = false;
	/* analog triggers count as pressed past half way */
	d->pad_buttons = (d->pad_axes[PAD_LTRIGGER] > 0.5f) ? (d->pad_buttons | 1u << PAD_LT) : d->pad_buttons;
	d->pad_buttons = (d->pad_axes[PAD_RTRIGGER] > 0.5f) ? (d->pad_buttons | 1u << PAD_RT) : d->pad_buttons;
	input_activity();
	pad_announce(d, true);
}

static void pad_setup(Device *d, const u8 *absbits, const u8 *keybits)
{
	int slot = -1;
	for (int i = 0; i < PAD_MAX && slot < 0; i++)
		if (!pads[i])
			slot = i;
	if (slot < 0) {
		d->kind &= ~(u32)DEV_GAMEPAD;
		return;
	}
	d->pad = slot;
	pads[slot] = d;
	d->joystick = !test_bit(keybits, BTN_SOUTH) && test_bit(keybits, BTN_JOYSTICK);
	for (u32 a = 0; a <= ABS_BRAKE; a++)
		if (test_bit(absbits, a))
			sys_ioctl(d->fd, EVIOCGABS(a), &d->pad_abs[a]);
	/* with a right stick on RX/RY, Z and RZ are the triggers */
	d->triggers_on_z = test_bit(absbits, ABS_RX) && test_bit(absbits, ABS_RY);
	/* xpad (Xbox controllers) reports the left and top buttons as BTN_X and BTN_Y */
	char link[160], target[160] = "";
	fmt(link, sizeof link, "/sys/class/input/%s/device/device/driver", path_base(d->path));
	long n = sys_readlink(link, target, sizeof target - 1);
	if (n > 0) {
		target[n] = 0;
		d->xbox = str_eq(path_base(target), "xpad");
	}
	pad_announce(d, true);
}

/* A controller with no hardware behind it, for the headless test desktop. */
void input_fake_pad(int button, bool down, int axis, f32 value)
{
	static Device fake = { .fd = -1, .kind = DEV_GAMEPAD, .pad = -1, .name = "Virtual Controller" };
	if (fake.pad < 0) {
		for (int i = 0; i < PAD_MAX && fake.pad < 0; i++)
			if (!pads[i])
				fake.pad = i, pads[i] = &fake;
		if (fake.pad < 0)
			return;
	}
	if (button >= 0 && button < PAD_BUTTONS && !pad_shell(&fake, button, down)) {
		u32 bit = 1u << button;
		fake.pad_buttons = down ? (fake.pad_buttons | bit) : (fake.pad_buttons & ~bit);
	}
	if (axis >= 0 && axis < PAD_AXES)
		fake.pad_axes[axis] = clampf(value, -1, 1);
	pad_announce(&fake, true);
}

static void handle_key(Device *d, u32 code, i32 value)
{
	if (d->kind & DEV_GAMEPAD) {
		pad_key(d, code, value);
		return;
	}
	if (code < 0x100) {
		if (code == KEY_POWER && value == 1) {
			/* The power button opens the session menu; a long press is left to the firmware. */
			S.session_menu_open = true;
			shell_mark_dirty();
			return;
		}
		input_key(code, value != 0, value == 2);
		update_leds();
		return;
	}
	if (d->kind & DEV_TOUCHPAD) {
		switch (code) {
		case BTN_TOOL_FINGER_CODE: d->fingers = value ? 1 : (d->fingers == 1 ? 0 : d->fingers); return;
		case BTN_TOOL_DOUBLETAP_CODE: d->fingers = value ? 2 : (d->fingers == 2 ? 0 : d->fingers); return;
		case BTN_TOOL_TRIPLETAP_CODE: d->fingers = value ? 3 : (d->fingers == 3 ? 0 : d->fingers); return;
		case BTN_TOOL_QUADTAP_CODE: d->fingers = value ? 4 : (d->fingers == 4 ? 0 : d->fingers); return;
		case BTN_LEFT_CODE: touchpad_button(d, value != 0); return;
		case BTN_RIGHT_CODE:
		case BTN_MIDDLE_CODE: input_button(mouse_button(code), value != 0); return;
		default: return;
		}
	}
	if (d->kind & DEV_TOUCHSCREEN) {
		if (code == BTN_TOUCH_CODE) {
			d->touch_down = value != 0;
			d->abs_dirty = true;
		}
		return;
	}
	u32 b = mouse_button(code);
	if (b && value != 2)
		input_button(b, value != 0);
}

static void handle_abs(Device *d, u32 code, i32 value)
{
	if (d->kind & DEV_GAMEPAD) {
		pad_abs(d, code, value);
		return;
	}
	if (d->kind & DEV_TOUCHPAD) {
		switch (code) {
		case ABS_MT_SLOT: d->slot = CLAMP(value, 0, MAX_SLOTS - 1); break;
		case ABS_MT_TRACKING_ID:
			d->slots[d->slot].id = value;
			if (value >= 0)
				d->slots[d->slot].fresh_x = d->slots[d->slot].fresh_y = true;
			break;
		case ABS_MT_POSITION_X: {
			Slot *sl = &d->slots[d->slot];
			sl->x = (f32)value;
			if (sl->fresh_x)
				sl->sx = sl->x, sl->fresh_x = false;
			break;
		}
		case ABS_MT_POSITION_Y: {
			Slot *sl = &d->slots[d->slot];
			sl->y = (f32)value;
			if (sl->fresh_y)
				sl->sy = sl->y, sl->fresh_y = false;
			break;
		}
		}
		return;
	}
	if (d->kind & (DEV_TOUCHSCREEN | DEV_TABLET)) {
		if (code == ABS_X || code == ABS_MT_POSITION_X)
			d->slots[0].x = (f32)value, d->abs_dirty = true;
		else if (code == ABS_Y || code == ABS_MT_POSITION_Y)
			d->slots[0].y = (f32)value, d->abs_dirty = true;
	}
}

static void handle_syn(Device *d)
{
	if (d->kind & DEV_GAMEPAD) {
		pad_syn(d);
		return;
	}
	if (d->rel_x != 0 || d->rel_y != 0) {
		f32 speed = sqrtf(d->rel_x * d->rel_x + d->rel_y * d->rel_y);
		f32 k = S.set.mouse_accel ? 1.0f + clampf((speed - 3) * 0.06f, 0, 1.4f) : 1.0f;
		input_pointer_rel(d->rel_x * k, d->rel_y * k);
		d->rel_x = d->rel_y = 0;
	}
	if (d->wheel_y != 0 || d->wheel_x != 0) {
		/* One wheel notch scrolls about three lines. */
		input_scroll(-d->wheel_x * 48, -d->wheel_y * 48, SCROLL_WHEEL, SCROLL_PHASE_NONE);
		d->wheel_x = d->wheel_y = 0;
	}
	if (d->kind & DEV_TOUCHPAD)
		touchpad_frame(d);
	if ((d->kind & DEV_TOUCHSCREEN) && d->abs_dirty) {
		d->abs_dirty = false;
		f32 fx = (d->slots[0].x - d->ax.minimum) / (f32)MAX(1, d->ax.maximum - d->ax.minimum);
		f32 fy = (d->slots[0].y - d->ay.minimum) / (f32)MAX(1, d->ay.maximum - d->ay.minimum);
		input_pointer_abs(fx * S.sw, fy * S.sh);
		bool down = (S.buttons >> BUTTON_LEFT) & 1;
		if (down != d->touch_down)
			input_button(BUTTON_LEFT, d->touch_down);
	}
	/* Tablets and virtual machines' pointers: absolute position, mouse buttons. */
	if ((d->kind & DEV_TABLET) && d->abs_dirty) {
		d->abs_dirty = false;
		f32 fx = (d->slots[0].x - d->ax.minimum) / (f32)MAX(1, d->ax.maximum - d->ax.minimum);
		f32 fy = (d->slots[0].y - d->ay.minimum) / (f32)MAX(1, d->ay.maximum - d->ay.minimum);
		input_pointer_abs(fx * S.sw, fy * S.sh);
	}
}

static void on_device(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(loop), UNUSED(fd);
	Device *d = data;
	struct input_event ev[64];
	for (;;) {
		long n = sys_read(d->fd, ev, sizeof ev);
		if (n == -EAGAIN)
			break;
		if (n <= 0 || (events & (EPOLLHUP | EPOLLERR))) {
			if (n == -EINTR)
				continue;
			device_close(d);
			return;
		}
		for (long i = 0; i < n / (long)sizeof ev[0]; i++) {
			struct input_event *e = &ev[i];
			switch (e->type) {
			case EV_KEY: handle_key(d, e->code, e->value); break;
			case EV_REL:
				switch (e->code) {
				case REL_X: d->rel_x += (f32)e->value; break;
				case REL_Y: d->rel_y += (f32)e->value; break;
				case REL_WHEEL: if (!d->hires_wheel) d->wheel_y += (f32)e->value; break;
				case REL_HWHEEL: if (!d->hires_wheel) d->wheel_x += (f32)e->value; break;
				case REL_WHEEL_HI_RES: d->wheel_y += (f32)e->value / 120.0f; break;
				case REL_HWHEEL_HI_RES: d->wheel_x += (f32)e->value / 120.0f; break;
				}
				break;
			case EV_ABS: handle_abs(d, e->code, e->value); break;
			case EV_SW:
				if (e->code == SW_LID && e->value == 1) {
					if (S.set.has_password)
						shell_lock();
					shell_session_action(SESSION_SLEEP);
				}
				break;
			case EV_SYN:
				if (e->code == SYN_REPORT)
					handle_syn(d);
				break;
			}
		}
	}
}

static void device_open(const char *path)
{
	for (int i = 0; i < ndevices; i++)
		if (str_eq(devices[i]->path, path))
			return;
	if (ndevices >= (int)ARRAY_LEN(devices))
		return;
	int fd = (int)sys_open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
	if (fd < 0)
		fd = (int)sys_open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
	if (fd < 0)
		return;
	u8 evbits[4] = { 0 }, keybits[96] = { 0 }, relbits[4] = { 0 }, absbits[8] = { 0 }, props[4] = { 0 };
	u8 swbits[4] = { 0 };
	sys_ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits);
	sys_ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
	sys_ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits);
	sys_ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
	sys_ioctl(fd, EVIOCGBIT(EV_SW, sizeof swbits), swbits);
	sys_ioctl(fd, EVIOCGPROP(sizeof props), props);

	Device *d = NEW(Device);
	d->fd = fd;
	str_copy(d->path, path, sizeof d->path);
	sys_ioctl(fd, EVIOCGNAME(sizeof d->name - 1), d->name);
	for (int i = 0; i < MAX_SLOTS; i++)
		d->slots[i].id = -1;

	if (test_bit(evbits, EV_KEY) && (test_bit(keybits, KEY_A) || test_bit(keybits, KEY_POWER) ||
					 test_bit(keybits, KEY_VOLUMEUP) || test_bit(keybits, KEY_BRIGHTNESSUP)))
		d->kind |= DEV_KEYBOARD;
	if (test_bit(evbits, EV_REL) && test_bit(relbits, REL_X) && test_bit(relbits, REL_Y))
		d->kind |= DEV_MOUSE;
	if (test_bit(evbits, EV_REL) && test_bit(relbits, REL_WHEEL))
		d->kind |= DEV_MOUSE;
	d->hires_wheel = test_bit(relbits, REL_WHEEL_HI_RES);
	bool mt = test_bit(evbits, EV_ABS) && test_bit(absbits, ABS_MT_POSITION_X);
	bool abs_xy = test_bit(evbits, EV_ABS) && test_bit(absbits, ABS_X);
	if (test_bit(props, INPUT_PROP_DIRECT) && (mt || abs_xy))
		d->kind |= DEV_TOUCHSCREEN;
	else if (mt && test_bit(keybits, BTN_TOOL_FINGER_CODE))
		d->kind |= DEV_TOUCHPAD;
	else if (abs_xy && test_bit(absbits, ABS_Y) && test_bit(keybits, BTN_LEFT_CODE))
		d->kind |= DEV_TABLET;
	if (test_bit(evbits, EV_SW) && test_bit(swbits, SW_LID))
		d->kind |= DEV_SWITCH;
	/* game controllers: the gamepad buttons, or an old-style joystick with axes */
	if (test_bit(evbits, EV_KEY) && (test_bit(keybits, BTN_GAMEPAD) || (test_bit(keybits, BTN_JOYSTICK) && abs_xy)) &&
	    !(d->kind & (DEV_TOUCHPAD | DEV_TOUCHSCREEN)))
		d->kind = DEV_GAMEPAD;
	d->clickpad = test_bit(props, INPUT_PROP_BUTTONPAD);

	if (d->kind & (DEV_TOUCHPAD | DEV_TOUCHSCREEN | DEV_TABLET)) {
		u32 cx = mt ? ABS_MT_POSITION_X : ABS_X, cy = mt ? ABS_MT_POSITION_Y : ABS_Y;
		sys_ioctl(fd, EVIOCGABS(cx), &d->ax);
		sys_ioctl(fd, EVIOCGABS(cy), &d->ay);
		/* Touchpads report their resolution; assume a 100 mm wide pad if not. */
		d->res_x = d->ax.resolution > 0 ? (f32)d->ax.resolution : (f32)MAX(1, d->ax.maximum - d->ax.minimum) / 100.0f;
		d->res_y = d->ay.resolution > 0 ? (f32)d->ay.resolution : d->res_x;
	}
	if (!d->kind) {
		sys_close(fd);
		mem_free(d);
		return;
	}
	devices[ndevices++] = d;
	loop_add_fd(S.loop, fd, EPOLLIN, on_device, d);
	if (d->kind & DEV_GAMEPAD)
		pad_setup(d, absbits, keybits);
	caps_shown = ~0u;
	update_leds();
}

static void scan(void)
{
	DirList list = { 0 };
	if (!dir_list("/dev/input", &list, false))
		return;
	vec_each(&list, e) {
		if (str_starts(e->name, "event")) {
			char path[32];
			fmt(path, sizeof path, "/dev/input/%s", e->name);
			device_open(path);
		}
	}
	dir_list_free(&list);
}

static void on_inotify(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(loop), UNUSED(events), UNUSED(data);
	u8 buf[2048] __attribute__((aligned(8)));
	bool rescan = false;
	for (;;) {
		long n = sys_read(fd, buf, sizeof buf);
		if (n <= 0)
			break;
		for (long off = 0; off < n;) {
			struct inotify_event *e = (struct inotify_event *)(buf + off);
			if ((e->mask & (IN_CREATE | IN_ATTRIB)) && e->len && str_starts(e->name, "event"))
				rescan = true;
			off += (long)sizeof *e + e->len;
		}
	}
	/* The node may exist before its permissions are final; opening it on the
	 * attribute change catches that case. */
	if (rescan)
		scan();
}

void input_init(void)
{
	inotify_fd = (int)sys_inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0) {
		sys_inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB);
		loop_add_fd(S.loop, inotify_fd, EPOLLIN, on_inotify, NULL);
	}
	scan();
	S.px = S.sw / 2;
	S.py = S.sh / 2;
}
