/*
 * Boot splash.
 *
 * The firmware draws its logo (the ASUS logo, on an ASUS machine) before
 * the kernel starts. Nothing in Zurbax clears the screen while booting, and
 * the firmware also hands the very same image to the OS through the ACPI
 * BGRT table. zdisplay draws that image at exactly the position the firmware
 * used and adds a spinner underneath, so from power-on to the desktop the
 * logo never moves or blinks. When the desktop is ready, the splash fades out.
 */
#include "zdisplay.h"

#define BGRT_DIR "/sys/firmware/acpi/bgrt"

static Canvas *logo;
static int logo_x, logo_y;
static f32 spin_cx, spin_cy, dot_r, ring_r;
static f64 started, fade_t0, wait_from;
static bool fading;

static bool read_int_file(const char *path, i64 *out)
{
	i64 v = file_read_int(path, -1);
	if (v < 0)
		return false;
	*out = v;
	return true;
}

static void load_bgrt(int sw, int sh)
{
	i64 xoff = 0, yoff = 0, status = 0;
	read_int_file(BGRT_DIR "/status", &status);
	if (!read_int_file(BGRT_DIR "/xoffset", &xoff) || !read_int_file(BGRT_DIR "/yoffset", &yoff))
		return;
	logo = image_load(BGRT_DIR "/image");
	if (!logo)
		return;
	/* The offsets are valid only in the mode the firmware used. If our mode is
	 * the same (the usual case: the panel's native mode), the image is centred
	 * horizontally by those offsets. Otherwise place it like the firmware
	 * would: centred, with its middle at 38.2% of the screen height. */
	bool same_mode = ABS(2 * xoff + logo->width - sw) <= 2 && yoff + logo->height <= sh;
	if (same_mode) {
		logo_x = (int)xoff;
		logo_y = (int)yoff;
	} else {
		if (logo->width > sw * 3 / 4) {
			Canvas *s = canvas_resize(logo, sw * 3 / 4, logo->height * (sw * 3 / 4) / logo->width);
			canvas_free(logo);
			logo = s;
		}
		logo_x = (sw - logo->width) / 2;
		logo_y = (int)(sh * 0.382f) - logo->height / 2;
	}
}

static void load_fallback_logo(int sw, int sh)
{
	char *path = zurbax_path("share/icons/apps/zurbax.svg");
	SvgDoc *doc = svg_load(path);
	mem_free(path);
	if (!doc)
		return;
	int size = (int)(150 * S.scale);
	logo = svg_rasterize(doc, size, size, COLOR_WHITE);
	svg_free(doc);
	logo_x = (sw - size) / 2;
	logo_y = (int)(sh * 0.382f) - size / 2;
}

void splash_start(void)
{
	int sw = S.frame->width, sh = S.frame->height;
	load_bgrt(sw, sh);
	if (!logo)
		load_fallback_logo(sw, sh);
	f32 s = S.scale;
	dot_r = 2.6f * s;
	ring_r = 15 * s;
	spin_cx = sw / 2.0f;
	f32 below = logo ? (f32)(logo_y + logo->height) : sh * 0.5f;
	/* Halfway between the logo and the bottom edge, a little high. */
	spin_cy = below + (sh - below) * 0.42f;
	spin_cy = clampf(spin_cy, below + ring_r + 40 * s, sh - ring_r - 40 * s);
	S.splash = true;
	S.splash_k = 1;
	started = wait_from = now_s();
	/* The splash stays up at least this long so the spinner is seen spinning
	 * rather than flashing; the desktop is usually ready well before. */
	S.splash_until = started + 1.1;
	damage_all();
}

void splash_finish(void)
{
	if (!S.splash || fading)
		return;
	fading = true;
	fade_t0 = now_s();
}

static Rect spinner_rect(void)
{
	f32 m = ring_r + dot_r + 2;
	return rect(spin_cx - m, spin_cy - m, 2 * m, 2 * m);
}

/* ---- Startup thread ---------------------------------------------------------------------
 * Loading the wallpaper and the shell takes a moment, longer on slow disks
 * or the first boot. A thread keeps the spinner turning meanwhile, so the
 * boot never stutters; it owns the screen until the main loop takes over. */

enum { THREAD_NONE, THREAD_RUNNING, THREAD_STOPPING, THREAD_DONE };
static int thread_state;

static void splash_thread(void *arg)
{
	UNUSED(arg);
	while (atomic_load(&thread_state) == THREAD_RUNNING) {
		if (S.out->flip_pending) {
			struct pollfd p = { S.out->fd, POLLIN, 0 };
			if (S.out->fd >= 0 && sys_poll(&p, 1, 20) > 0 && S.out->on_fd)
				S.out->on_fd(S.out);
			continue;
		}
		compose_splash_frame(spinner_rect());
		sleep_ms(14);
	}
	atomic_store(&thread_state, THREAD_DONE);
	futex_wake(&thread_state, 1);
}

void splash_thread_start(void)
{
	if (!S.splash)
		return;
	thread_state = THREAD_RUNNING;
	if (thread_spawn(splash_thread, NULL) < 0)
		thread_state = THREAD_NONE;
}

void splash_thread_stop(void)
{
	if (thread_state == THREAD_NONE)
		return;
	atomic_store(&thread_state, THREAD_STOPPING);
	int st;
	while ((st = atomic_load(&thread_state)) != THREAD_DONE)
		futex_wait(&thread_state, st, 100);
	thread_state = THREAD_NONE;
	/* Give the first window its usual time to appear from here. */
	wait_from = now_s();
	damage_all();
}

/* Called every frame while the splash is up. */
void splash_tick(void)
{
	if (!S.splash)
		return;
	f64 t = now_s();
	if (!fading && t >= S.splash_until) {
		/* Ready once the desktop has something to show: a window if one is
		 * being started (the setup assistant on first boot), else at once. */
		bool shown = false;
		for (int i = 0; i < S.nwin; i++)
			shown |= S.windows[i]->mapped;
		bool waiting = S.splash_wait_window && !shown && t - wait_from < 10.0;
		if (!waiting)
			splash_finish();
	}
	if (fading) {
		f32 k = 1 - (f32)((t - fade_t0) / 0.55);
		S.splash_k = clampf(k, 0, 1);
		damage_all();
		if (S.splash_k <= 0) {
			S.splash = false;
			if (logo)
				canvas_free(logo);
			logo = NULL;
			cursor_update();
		}
		return;
	}
	damage_physical(spinner_rect());
}

static f32 ease_angle(f64 t)
{
	/* Each dot speeds through the bottom of the circle and slows at the top,
	 * which makes the dots bunch up and spread apart as they travel. */
	const f64 period = 1.9;
	f64 p = fmod(t, period) / period;
	f64 a = p - sin(2 * PI * p) * 0.13;
	return (f32)(-PI / 2 + 2 * PI * a);
}

void splash_draw(Canvas *frame)
{
	f32 k = fading ? S.splash_k * S.splash_k * (3 - 2 * S.splash_k) : 1;
	Rect full = rect(0, 0, (f32)frame->width, (f32)frame->height);
	fill_rect(frame, full, color_scale_alpha(COLOR_BLACK, k));
	if (logo)
		blit(frame, logo, logo_x, logo_y, k);
	f64 t = now_s() - started;
	/* The spinner fades in, so a fast boot shows the logo almost alone. */
	f32 appear = clampf((f32)((t - 0.25) / 0.4), 0, 1);
	f32 alpha = appear * k;
	if (alpha <= 0)
		return;
	for (int i = 0; i < 6; i++) {
		f32 a = ease_angle(t - i * 0.075);
		f32 x = spin_cx + cosf(a) * ring_r, y = spin_cy + sinf(a) * ring_r;
		f32 da = alpha * (1 - i * 0.07f);
		fill_circle(frame, x, y, dot_r, color_scale_alpha(COLOR_WHITE, da));
	}
}
