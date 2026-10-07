/* padsim: a virtual game controller for testing controller support in a
 * virtual machine, through the kernel's uinput like a real USB pad.
 *
 * It looks like an Xbox-style controller (sticks, triggers, a hat D-pad) and
 * plays a script from the kernel command line:
 *
 *     zurbax.pad=12:guide,13.5:down,14:a+,14.2:a-,15:lx=0.8,16:lx=0
 *
 * Each step is SECONDS:WHAT, counted from when padsim starts. WHAT is a
 * button (a b x y lb rb back start guide ls rs, or up down left right for the
 * D-pad) pressed and let go, BUTTON+ to hold it, BUTTON- to let go, or
 * AXIS=VALUE for a stick (lx ly rx ry, -1..1) or trigger (lt rt, 0..1).
 * Progress goes to the kernel log. Test builds start it from /system/test;
 * it is never part of a release. */
#include "zr.h"

#define UI_SET_EVBIT 0x40045564
#define UI_SET_KEYBIT 0x40045565
#define UI_SET_ABSBIT 0x40045567
#define UI_DEV_SETUP 0x405c5503
#define UI_ABS_SETUP 0x401c5504
#define UI_DEV_CREATE 0x5501
#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3

struct input_event {
	i64 sec, usec;
	u16 type, code;
	i32 value;
};

struct uinput_setup {
	u16 bustype, vendor, product, version;
	char name[80];
	u32 ff_effects_max;
};

struct uinput_abs_setup {
	u16 code, pad;
	i32 value, minimum, maximum, fuzz, flat, resolution;
};

static const struct { const char *name; u16 code; } BUTTONS[] = {
	{ "a", 0x130 }, { "b", 0x131 }, { "y", 0x133 }, { "x", 0x134 }, { "lb", 0x136 }, { "rb", 0x137 },
	{ "back", 0x13a }, { "start", 0x13b }, { "guide", 0x13c }, { "ls", 0x13d }, { "rs", 0x13e },
};

/* code, minimum, maximum */
static const struct { const char *name; u16 code; i32 min, max; } AXES[] = {
	{ "lx", 0x00, -32768, 32767 }, { "ly", 0x01, -32768, 32767 }, { "rx", 0x03, -32768, 32767 }, { "ry", 0x04, -32768, 32767 },
	{ "lt", 0x02, 0, 1023 }, { "rt", 0x05, 0, 1023 }, { "hatx", 0x10, -1, 1 }, { "haty", 0x11, -1, 1 },
};

static int fd = -1;

static void say(const char *f, ...) PRINTF_LIKE(1, 2);
static void say(const char *f, ...)
{
	char line[300];
	va_list ap;
	va_start(ap, f);
	int n = fmt(line, sizeof line, "<6>padsim: ");
	n += fmt_v(line + n, sizeof line - (usize)n, f, ap);
	va_end(ap);
	int k = (int)sys_open("/dev/kmsg", O_WRONLY | O_CLOEXEC, 0);
	if (k >= 0) {
		sys_write(k, line, (usize)n);
		sys_close(k);
	}
}

static void emit(u16 type, u16 code, i32 value)
{
	struct input_event e = { .type = type, .code = code, .value = value };
	sys_write(fd, &e, sizeof e);
}

static void sync_events(void)
{
	emit(EV_SYN, 0, 0);
}

static bool create(void)
{
	fd = (int)sys_open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC, 0);
	if (fd < 0)
		return false;
	sys_ioctl(fd, UI_SET_EVBIT, (void *)(long)EV_KEY);
	sys_ioctl(fd, UI_SET_EVBIT, (void *)(long)EV_ABS);
	for (usize i = 0; i < ARRAY_LEN(BUTTONS); i++)
		sys_ioctl(fd, UI_SET_KEYBIT, (void *)(long)BUTTONS[i].code);
	for (usize i = 0; i < ARRAY_LEN(AXES); i++) {
		sys_ioctl(fd, UI_SET_ABSBIT, (void *)(long)AXES[i].code);
		struct uinput_abs_setup a = { .code = AXES[i].code, .minimum = AXES[i].min, .maximum = AXES[i].max };
		if (AXES[i].max > 1000)
			a.fuzz = 16, a.flat = AXES[i].min < 0 ? 128 : 0;
		sys_ioctl(fd, UI_ABS_SETUP, &a);
	}
	struct uinput_setup s = { .bustype = 0x03, .vendor = 0x045e, .product = 0x028e, .version = 1 };
	str_copy(s.name, "Zurbax Test Controller", sizeof s.name);
	sys_ioctl(fd, UI_DEV_SETUP, &s);
	return sys_ioctl(fd, UI_DEV_CREATE, NULL) == 0;
}

/* Does one step of the script. */
static void step(const char *what)
{
	const char *eq = str_chr(what, '=');
	if (eq) {
		for (usize i = 0; i < ARRAY_LEN(AXES); i++)
			if (strlen(AXES[i].name) == (usize)(eq - what) && memcmp(AXES[i].name, what, (usize)(eq - what)) == 0) {
				f64 v = clampf((f32)str_to_float(eq + 1, NULL), AXES[i].min < 0 ? -1 : 0, 1);
				i32 raw = AXES[i].min < 0 ? (i32)(v * 32767) : (i32)(v * AXES[i].max);
				emit(EV_ABS, AXES[i].code, raw);
				sync_events();
				return;
			}
		say("unknown axis in %s", what);
		return;
	}
	char name[16];
	str_copy(name, what, sizeof name);
	usize n = strlen(name);
	int mode = 0; /* 0 tap, 1 hold, -1 let go */
	if (n && (name[n - 1] == '+' || name[n - 1] == '-')) {
		mode = name[n - 1] == '+' ? 1 : -1;
		name[n - 1] = 0;
	}
	/* the D-pad is a hat, like on Xbox controllers */
	static const char *const DIRS[] = { "left", "right", "up", "down" };
	for (int i = 0; i < 4; i++)
		if (str_eq(name, DIRS[i])) {
			u16 code = i < 2 ? 0x10 : 0x11;
			i32 v = i % 2 ? 1 : -1;
			if (mode >= 0) {
				emit(EV_ABS, code, v);
				sync_events();
			}
			if (mode == 0)
				sleep_ms(90);
			if (mode <= 0) {
				emit(EV_ABS, code, 0);
				sync_events();
			}
			return;
		}
	for (usize i = 0; i < ARRAY_LEN(BUTTONS); i++)
		if (str_eq(name, BUTTONS[i].name)) {
			if (mode >= 0) {
				emit(EV_KEY, BUTTONS[i].code, 1);
				sync_events();
			}
			if (mode == 0)
				sleep_ms(90);
			if (mode <= 0) {
				emit(EV_KEY, BUTTONS[i].code, 0);
				sync_events();
			}
			return;
		}
	say("unknown button %s", what);
}

int main(void)
{
	char *cmdline = file_read("/proc/cmdline", NULL);
	const char *script = cmdline ? str_find(cmdline, "zurbax.pad=") : NULL;
	if (!create()) {
		say("cannot create the controller (is uinput in the kernel?)");
		return 1;
	}
	say("controller ready");
	if (!script)
		for (;;)
			sleep_ms(60000);
	script += strlen("zurbax.pad=");
	usize len = 0;
	while (script[len] && script[len] != ' ' && script[len] != '\n')
		len++;
	char *s = str_ndup(script, len);
	usize count;
	char **steps = str_split(s, ',', &count);
	u64 t0 = time_mono_ms();
	for (usize i = 0; i < count; i++) {
		const char *colon = str_chr(steps[i], ':');
		if (!colon)
			continue;
		u64 at = t0 + (u64)(str_to_float(steps[i], NULL) * 1000);
		u64 now = time_mono_ms();
		if (at > now)
			sleep_ms((u32)(at - now));
		say("%s", colon + 1);
		step(colon + 1);
	}
	str_split_free(steps, count);
	for (;;)
		sleep_ms(60000);
}
