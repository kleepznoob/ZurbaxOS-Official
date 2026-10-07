/*
 * Steam — starts Valve's Steam client, and through it Proton for Windows
 * games, on ZOS.
 *
 * Steam is a standard Linux program: it needs glibc, an X server, Mesa's
 * GPU drivers and more, none of which ZOS uses itself. They come as the
 * Linux layer (layer/build.sh, installed in /data/layers/steam), and
 * layerrun starts Steam inside it with a full-screen Xwayland on zdisplay.
 * This window checks that everything is in place, says what's missing, and
 * starts Steam in Big Picture (made for controllers and the TV) or on the
 * desktop.
 */
#include "zui.h"

#define LAYER "/data/layers/steam"

typedef struct {
	const char *title;
	char detail[200];
	bool ok, needed;
} Check;

static Check checks[7];
static int nchecks;
static bool ready;
static f64 checked_at;
static char message[160];

/* What the layer's Vulkan drivers see, from zos-gpucheck run in the layer
 * (once, in the background): "NAME (DRIVER)", or empty when none. */
static struct {
	bool asked, done;
	int pid, fd;
	StrBuf out;
	char device[160];
} probe = { .pid = -1, .fd = -1 };

static bool in_layer(const char *path)
{
	char *p = str_fmt("%s%s", LAYER, path);
	bool ok = file_exists(p);
	mem_free(p);
	return ok;
}

/* The display's driver: a real GPU driver, or only the firmware's framebuffer. */
static void gpu_driver(char *out, usize cap)
{
	out[0] = 0;
	for (int i = 0; i < 8 && !out[0]; i++) {
		char link[64], target[160] = { 0 };
		fmt(link, sizeof link, "/sys/class/drm/card%d/device/driver", i);
		long n = sys_readlink(link, target, sizeof target - 1);
		if (n > 0 && !str_eq(path_base(target), "simple-framebuffer"))
			str_copy(out, path_base(target), cap);
	}
}

/* "vulkan NAME|DRIVER|TYPE" lines; a discrete GPU first, as games pick it */
static void gpu_parse(const char *text)
{
	usize n = 0;
	char **lines = str_split(text, '\n', &n);
	int best = -1;
	for (usize i = 0; i < n; i++) {
		if (!str_starts(lines[i], "vulkan "))
			continue;
		usize nf = 0;
		char **f = str_split(lines[i] + 7, '|', &nf);
		int score = nf >= 3 ? (str_eq(f[2], "discrete") ? 3 : str_eq(f[2], "integrated") ? 2 : str_eq(f[2], "cpu") ? 0 : 1) : 0;
		if (nf >= 2 && score > best) {
			best = score;
			fmt(probe.device, sizeof probe.device, "%s (%s)", f[0], f[1]);
		}
		str_split_free(f, nf);
	}
	str_split_free(lines, n);
}

static void on_gpu(Loop *l, int fd, u32 events, void *data)
{
	UNUSED(events);
	Ui *ui = data;
	char buf[512];
	long n = sys_read(fd, buf, sizeof buf);
	if (n > 0) {
		sb_add(&probe.out, buf, (usize)n);
		return;
	}
	loop_del_fd(l, fd);
	sys_close(fd);
	probe.fd = -1;
	proc_wait(probe.pid, NULL, false);
	probe.pid = -1;
	gpu_parse(sb_str(&probe.out));
	sb_free(&probe.out);
	probe.done = true;
	checked_at = 0; /* show it now */
	ui_wake(ui);
}

static void gpu_ask(Ui *ui)
{
	probe.asked = true;
	int fds[2];
	if (sys_pipe2(fds, O_CLOEXEC) < 0) {
		probe.done = true;
		return;
	}
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[] = { layerrun, "zos-gpucheck", "--vulkan", NULL };
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = fds[1], .stderr_fd = -1 };
	probe.pid = proc_spawn(layerrun, argv, environ, &o);
	mem_free(layerrun);
	sys_close(fds[1]);
	if (probe.pid < 0) {
		sys_close(fds[0]);
		probe.done = true;
		return;
	}
	probe.fd = fds[0];
	loop_add_fd(ui_loop(ui), probe.fd, EPOLLIN, on_gpu, ui);
}

static Check *add(const char *title, bool ok, bool needed)
{
	Check *c = &checks[nchecks++];
	c->title = title, c->ok = ok, c->needed = needed;
	c->detail[0] = 0;
	return c;
}

static void run_checks(Ui *ui)
{
	nchecks = 0;
	bool layer = in_layer("/lib64/ld-linux-x86-64.so.2");
	Check *c = add("Linux layer", layer, true);
	str_copy(c->detail,
		 layer ? "Ready in " LAYER
		       : "Missing: it comes on ZOS's install media when built with it (layer/build.sh)",
		 sizeof c->detail);
	bool steam = layer && (in_layer("/usr/bin/steam") || in_layer("/usr/games/steam"));
	c = add("Steam client", steam, true);
	str_copy(c->detail, steam ? "Ready; it updates itself when it starts" : "Comes with the Linux layer", sizeof c->detail);

	char driver[48];
	gpu_driver(driver, sizeof driver);
	bool gpu = driver[0] && !str_eq(driver, "simpledrm");
	c = add("Graphics driver", gpu, false);
	if (gpu)
		fmt(c->detail, sizeof c->detail, "%s", str_eq(driver, "amdgpu") ? "AMD (amdgpu)" : str_eq(driver, "i915") || str_eq(driver, "xe") ? "Intel" : driver);
	else
		str_copy(c->detail, "Only the firmware's framebuffer: games would have no 3D acceleration", sizeof c->detail);

	bool vulkan = layer && in_layer("/usr/share/vulkan/icd.d");
	c = add("Vulkan", vulkan, true);
	str_copy(c->detail, vulkan ? "Mesa's drivers, for Proton's DirectX translation" : "Mesa's Vulkan drivers come with the Linux layer",
		 sizeof c->detail);
	if (vulkan && in_layer("/usr/bin/zos-gpucheck")) {
		if (!probe.asked)
			gpu_ask(ui);
		/* games run without one too, very slowly or not at all */
		c = add("Game GPU", probe.device[0] != 0, false);
		str_copy(c->detail, !probe.done ? "Looking..." : probe.device[0] ? probe.device : "No Vulkan GPU found: most Windows games need one",
			 sizeof c->detail);
	}

	char *ns = file_read_line("/proc/sys/user/max_user_namespaces");
	bool userns = ns && str_to_int(ns, NULL) > 0;
	mem_free(ns);
	c = add("Containers", userns, true);
	str_copy(c->detail, userns ? "Steam runs games in its own runtime" : "The kernel lacks user namespaces", sizeof c->detail);

	bool ntsync = file_exists("/dev/ntsync");
	c = add("Windows games", ntsync, false);
	str_copy(c->detail, ntsync ? "Proton can use the kernel's fast synchronization (ntsync)" : "Proton works, a little slower without ntsync",
		 sizeof c->detail);

	ready = true;
	for (int i = 0; i < nchecks; i++)
		ready &= checks[i].ok || !checks[i].needed;
	checked_at = ui->now;
}

/* Starts Steam through layerrun and closes this window. */
static void play(Ui *ui, bool big_picture)
{
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[] = { layerrun, "--x11", "--app", "steam", "steam", big_picture ? "-gamepadui" : NULL, NULL };
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .new_session = true };
	int pid = proc_spawn(layerrun, argv, environ, &o);
	mem_free(layerrun);
	if (pid < 0) {
		str_copy(message, "Steam couldn't start.", sizeof message);
		return;
	}
	ui->quit = true;
}

static void check_row(Ui *ui, Rect r, const Check *c)
{
	Theme *t = &ui->theme;
	Rect icon = rect(r.x, r.y + (r.h - 22) / 2, 22, 22);
	Color col = c->ok ? t->success : c->needed ? t->danger : t->warning;
	ui_icon(ui, icon, c->ok ? "check" : c->needed ? "error" : "warning", col);
	Rect text = pad4(r, 6, 0, 6, 36);
	ui_text_in(ui, ui_font(ui, 14, 600), cut_top(&text, 20), c->title, t->text, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 12, 400), text, c->detail, t->text_dim, ALIGN_START);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	/* look again now and then: the layer may have just been copied in */
	if (!nchecks || ui->now - checked_at > 3)
		run_checks(ui);
	ui->wake_at = checked_at + 3;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	ui_headerbar(ui, cut_top(&win, 46), "Steam", HEADER_NO_MAXIMIZE);
	Rect b = pad2(win, 28, 20);
	Rect hero = cut_top(&b, 76);
	ui_app_icon(ui, rect(hero.x, hero.y + 6, 64, 64), "steam");
	Rect words = pad4(hero, 10, 0, 0, 82);
	ui_text_in(ui, ui_font(ui, 22, 700), cut_top(&words, 30), "Your Steam library on ZOS", t->text, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 13, 400), cut_top(&words, 20),
		   ready ? "Everything is in place. Windows games run through Proton." : "A few things are needed first.", t->text_dim, ALIGN_START);
	cut_top(&b, 12);
	Rect card = cut_top(&b, (f32)nchecks * 52 + 12);
	ui_card(ui, card);
	Rect rows = pad2(card, 16, 6);
	for (int i = 0; i < nchecks; i++) {
		if (i)
			ui_separator(ui, rows.x + 36, rows.y, rows.x + rows.w);
		check_row(ui, cut_top(&rows, 52), &checks[i]);
	}
	Rect bottom = cut_bottom(&b, 36);
	if (ready) {
		if (ui_button_suggested(ui, cut_right(&bottom, 150), "Play") || ui_key_pressed(ui, KEY_ENTER, 0))
			play(ui, true);
		cut_right(&bottom, 10);
		if (ui_button(ui, cut_right(&bottom, 150), "Desktop Mode"))
			play(ui, false);
		ui_text_in(ui, ui_font(ui, 12, 400), bottom, message[0] ? message : "Play opens Big Picture, made for controllers.",
			   t->text_faint, ALIGN_START);
	} else {
		ui_text_wrapped(ui, ui_font(ui, 12, 400), bottom,
				"The Linux layer holds the standard Linux pieces Steam needs (the C library, Xwayland, Mesa). "
				"ZOS doesn't use them itself, so they're kept apart.",
				t->text_faint, ALIGN_START);
	}
}

int main(void)
{
	UiAppDesc d = { .id = "steam", .title = "Steam", .width = 600, .height = 620, .min_width = 520, .min_height = 580, .frame = frame };
	return ui_run(&d);
}
