/*
 * Headless output: the screen is just memory. Used for development and
 * automated tests, driven through a small text control socket:
 *
 *   screenshot PATH        save the current screen as PNG
 *   move X Y               move the pointer (logical pixels)
 *   click X Y [BUTTON]     press and release a button there
 *   down / up [BUTTON]     press or release a button
 *   scroll DY              scroll by DY logical pixels
 *   key CODE [MODS]        press and release a key (evdev code)
 *   type TEXT              type text
 *   launch APP [ARG]       start an app
 *   frame                  render now
 */
#include "zdisplay.h"

typedef struct {
	Canvas *screen;
} Headless;

static bool hl_present(Output *o, const Canvas *frame, const Rect *damage, int n)
{
	Headless *h = o->impl;
	for (int i = 0; i < n; i++)
		copy_pixels(h->screen, frame, (int)damage[i].x, (int)damage[i].y, (int)damage[i].x, (int)damage[i].y,
			    (int)damage[i].w, (int)damage[i].h);
	return true;
}

Output *output_headless_open(int w, int h)
{
	Output *o = NEW(Output);
	Headless *hl = NEW(Headless);
	hl->screen = canvas_new(w, h);
	o->name = "headless";
	o->width = w;
	o->height = h;
	o->refresh_mhz = 60000;
	o->dpi = 96;
	o->impl = hl;
	o->present = hl_present;
	o->fd = -1;
	return o;
}

/* ---- Control socket ----------------------------------------------------------------- */

static void reply(int fd, const char *s)
{
	sys_sendto(fd, s, strlen(s), MSG_NOSIGNAL, NULL, 0);
}

static void run_command(int fd, char *line)
{
	char *cmd = str_trim(line);
	char *arg = (char *)str_chr(cmd, ' ');
	if (arg)
		*arg++ = 0;
	else
		arg = "";
	const char *p = arg;
	if (str_eq(cmd, "screenshot")) {
		compose_frame();
		Headless *h = S.out->impl;
		/* Include a software cursor in screenshots. */
		Canvas *shot = canvas_new(h->screen->width, h->screen->height);
		copy_pixels(shot, h->screen, 0, 0, 0, 0, shot->width, shot->height);
		bool ok = png_save(shot, str_trim(arg));
		canvas_free(shot);
		reply(fd, ok ? "ok\n" : "error\n");
		return;
	}
	if (str_eq(cmd, "move") || str_eq(cmd, "click")) {
		f32 x = (f32)str_to_float(p, &p), y = (f32)str_to_float(p, &p);
		u32 button = (u32)str_to_int(p, NULL);
		if (!button)
			button = BUTTON_LEFT;
		input_pointer_abs(x, y);
		if (str_eq(cmd, "click")) {
			input_button(button, true);
			input_button(button, false);
		}
	} else if (str_eq(cmd, "rel")) {
		/* mouse motion as a real mouse sends it, for pointer locks */
		f32 dx = (f32)str_to_float(p, &p), dy = (f32)str_to_float(p, &p);
		input_pointer_rel(dx, dy);
	} else if (str_eq(cmd, "down") || str_eq(cmd, "up")) {
		u32 button = (u32)str_to_int(p, NULL);
		input_button(button ? button : BUTTON_LEFT, str_eq(cmd, "down"));
	} else if (str_eq(cmd, "pad")) {
		/* a virtual controller: "pad BUTTON 1|0" or "pad axis AXIS VALUE" (PAD_* numbers) */
		if (str_starts(p, "axis")) {
			p += 4;
			int axis = (int)str_to_int(p, &p);
			input_fake_pad(-1, false, axis, (f32)str_to_float(p, NULL));
		} else {
			int b = (int)str_to_int(p, &p);
			input_fake_pad(b, str_to_int(p, NULL) != 0, -1, 0);
		}
	} else if (str_eq(cmd, "scroll")) {
		input_scroll(0, (f32)str_to_float(p, NULL), SCROLL_WHEEL, SCROLL_PHASE_NONE);
	} else if (str_eq(cmd, "key")) {
		u32 code = (u32)str_to_int(p, &p);
		u32 mods = (u32)str_to_int(p, NULL);
		u32 mod_keys[] = { KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTMETA };
		for (int i = 0; i < 4; i++)
			if (mods & (1u << i))
				input_key(mod_keys[i], true, false);
		input_key(code, true, false);
		input_key(code, false, false);
		for (int i = 3; i >= 0; i--)
			if (mods & (1u << i))
				input_key(mod_keys[i], false, false);
	} else if (str_eq(cmd, "type")) {
		/* Deliver text straight to the focused window or shell; a Linux
		 * program's window gets the keys of a US keyboard instead */
		if (S.focus && S.focus->wl) {
			static const char lower[] = "1234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./";
			static const char upper[] = "!@#$%^&*()_+\0\0QWERTYUIOP{}\0\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";
			for (const char *t = arg; *t; t++) {
				u32 code = *t == ' ' ? KEY_SPACE : 0;
				bool shift = false;
				for (u32 i = 0; !code && i < sizeof lower - 1; i++) {
					if (lower[i] && lower[i] == *t)
						code = KEY_1 + i;
					else if (upper[i] && upper[i] == *t)
						code = KEY_1 + i, shift = true;
				}
				if (!code)
					continue;
				if (shift)
					input_key(KEY_LEFTSHIFT, true, false);
				input_key(code, true, false);
				input_key(code, false, false);
				if (shift)
					input_key(KEY_LEFTSHIFT, false, false);
			}
			reply(fd, "ok\n");
			return;
		}
		for (const char *t = arg; *t;) {
			u32 cp;
			char ch[8] = { 0 };
			usize n = utf8_decode(t, strlen(t), &cp);
			memcpy(ch, t, n);
			t += n;
			if (!shell_key(0, KEY_PRESSED, 0, ch))
				server_key_event(S.focus, 0, KEY_PRESSED, ch);
		}
	} else if (str_eq(cmd, "notify")) {
		/* "notify APP TITLE|BODY", as an app would */
		char *sp = (char *)str_chr(arg, ' ');
		if (sp)
			*sp++ = 0;
		char *bar = sp ? (char *)str_chr(sp, '|') : NULL;
		if (bar)
			*bar++ = 0;
		shell_notify(arg, arg, sp ? sp : "", bar ? bar : "");
	} else if (str_eq(cmd, "launch")) {
		char *sp = (char *)str_chr(arg, ' ');
		if (sp)
			*sp++ = 0;
		shell_launch(arg, sp ? sp : "");
	} else if (str_eq(cmd, "frame")) {
		damage_all();
		compose_frame();
	} else {
		reply(fd, "unknown command\n");
		return;
	}
	reply(fd, "ok\n");
}

static void on_control(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(loop), UNUSED(events), UNUSED(data);
	char buf[2048];
	struct sockaddr_un from;
	u32 flen = sizeof from;
	long n = sys_recvfrom(fd, buf, sizeof buf - 1, 0, &from, &flen);
	if (n <= 0)
		return;
	buf[n] = 0;
	/* Datagram control: reply to the sender's bound address if it has one. */
	int out = (int)sys_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (flen > sizeof(u16))
		sys_connect(out, &from, flen);
	char *save = buf;
	for (char *line = save; line && *line;) {
		char *nl = (char *)str_chr(line, '\n');
		if (nl)
			*nl = 0;
		if (*line)
			run_command(flen > sizeof(u16) ? out : -1, line);
		line = nl ? nl + 1 : NULL;
	}
	sys_close(out);
	request_frame();
}

void headless_control_init(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	str_copy(a.sun_path, path, sizeof a.sun_path);
	sys_unlink(path);
	int fd = (int)sys_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0 || sys_bind(fd, &a, sizeof a) < 0)
		die("cannot bind control socket %s", path);
	loop_add_fd(S.loop, fd, EPOLLIN, on_control, NULL);
}
