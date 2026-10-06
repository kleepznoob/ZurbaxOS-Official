/* zdisplay entry point: sets up the output, input and shell, then runs the frame loop. */
#include "zdisplay.h"

Server S;

f64 now_s(void)
{
	return (f64)time_mono_ns() / 1e9;
}

void request_frame(void)
{
	loop_wake(S.loop);
}

void damage_physical(Rect r)
{
	r = rect_intersect(r, rect(0, 0, (f32)S.frame->width, (f32)S.frame->height));
	if (rect_empty(r))
		return;
	r = (Rect){ floorf(r.x), floorf(r.y), ceilf(r.x + r.w) - floorf(r.x), ceilf(r.y + r.h) - floorf(r.y) };
	/* Merge with an overlapping rectangle when that does not waste much area. */
	for (int i = 0; i < S.ndamage; i++) {
		Rect u = rect_union(S.damage[i], r);
		if (u.w * u.h <= (S.damage[i].w * S.damage[i].h + r.w * r.h) * 1.3f) {
			S.damage[i] = u;
			return;
		}
	}
	if (S.ndamage < (int)ARRAY_LEN(S.damage)) {
		S.damage[S.ndamage++] = r;
		return;
	}
	S.full_damage = true;
}

void damage_rect(Rect r)
{
	damage_physical((Rect){ r.x * S.scale - 2, r.y * S.scale - 2, r.w * S.scale + 4, r.h * S.scale + 4 });
}

void damage_all(void)
{
	S.full_damage = true;
}

Rect work_area(void)
{
	f32 top = shell_panel_height();
	return rect(0, top, S.sw, S.sh - top);
}

static f32 pick_scale(void)
{
	if (S.set.scale_override >= 1)
		return S.set.scale_override;
	/* Aim for about 96 logical dots per inch, in quarter steps. */
	f32 dpi = S.out->dpi > 0 ? S.out->dpi : 96;
	f32 s = roundf(dpi / 96 * 4) / 4;
	if (S.out->width <= 1920)
		s = minf(s, 1.25f);
	return clampf(s, 1, 2.5f);
}

static void on_output_fd(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(loop), UNUSED(fd), UNUSED(events), UNUSED(data);
	if (S.out->on_fd)
		S.out->on_fd(S.out);
}

static void on_status_timer(Loop *loop, void *data)
{
	UNUSED(loop), UNUSED(data);
	system_status_poll();
}

static bool needs_frame(void)
{
	return S.full_damage || S.ndamage > 0 || wm_any_animation() || shell_animating() || S.splash;
}

static void usage(void)
{
	eprint("usage: zdisplay [--headless WIDTHxHEIGHT] [--scale N] [--control SOCKET] [--no-splash]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int hw = 0, hh = 0;
	f32 scale_arg = 0;
	const char *control = NULL;
	bool no_splash = false;
	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "--headless") && i + 1 < argc) {
			const char *p = argv[++i];
			hw = (int)str_to_int(p, &p);
			if (*p == 'x')
				hh = (int)str_to_int(p + 1, NULL);
		} else if (str_eq(argv[i], "--scale") && i + 1 < argc) {
			scale_arg = (f32)str_to_float(argv[++i], NULL);
		} else if (str_eq(argv[i], "--control") && i + 1 < argc) {
			control = argv[++i];
		} else if (str_eq(argv[i], "--no-splash")) {
			no_splash = true;
		} else {
			usage();
		}
	}
	sig_ignore(SIGPIPE);
	S.loop = loop_new();
	settings_load();
	if (scale_arg > 0)
		S.set.scale_override = scale_arg;

	S.headless = hw > 0;
	S.out = S.headless ? output_headless_open(hw, hh > 0 ? hh : hw * 9 / 16) : output_drm_open();
	if (!S.out)
		die("no usable display output");
	S.scale = pick_scale();
	S.sw = (f32)S.out->width / S.scale;
	S.sh = (f32)S.out->height / S.scale;
	S.frame = canvas_new(S.out->width, S.out->height);
	S.now = now_s();
	S.last_input = S.now;
	eprint("zdisplay: %s %dx%d @ %d Hz, scale %.2f\n", S.out->name, S.out->width, S.out->height,
	       S.out->refresh_mhz / 1000, (double)S.scale);

	settings_apply();
	compose_init();
	/* The splash goes up first: the firmware logo stays put and the spinner
	 * appears while the wallpaper and the shell are still loading. */
	if (!S.headless && !no_splash) {
		splash_start();
		compose_splash_frame(rect(0, 0, 0, 0));
		splash_thread_start();
	}
	wallpaper_reload();
	server_init();
	if (!S.headless)
		input_init();
	shell_init();
	if (control)
		headless_control_init(control);
	if (S.out->fd >= 0)
		loop_add_fd(S.loop, S.out->fd, EPOLLIN, on_output_fd, NULL);
	system_status_poll();
	loop_timer(S.loop, 2000, 2000, on_status_timer, NULL);
	splash_thread_stop();
	damage_all();

	f64 last = now_s();
	for (;;) {
		f64 t = now_s();
		f64 dt = t - last;
		last = t;
		S.now = t;
		wm_animate(dt);
		shell_tick();
		splash_tick();
		if (needs_frame() && !S.out->flip_pending)
			compose_frame();
		int timeout = -1;
		if (needs_frame())
			timeout = S.out->flip_pending ? 50 : (S.headless ? 16 : 0);
		/* While idle, wake for the clock in the panel once a second. */
		if (timeout < 0) {
			f64 frac = t - floor(t);
			timeout = (int)((1.0 - frac) * 1000) + 5;
			f64 wake = shell_next_wake();
			if (wake > 0) {
				int ms = wake <= t ? 0 : (int)((wake - t) * 1000) + 1;
				timeout = MIN(timeout, ms);
			}
		}
		loop_once(S.loop, timeout);
	}
}
