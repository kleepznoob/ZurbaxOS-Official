/*
 * Games in the cloud: they run on a gaming PC far away, and stream here,
 * picture and sound one way, keyboard, mouse and controller the other.
 *
 * It's how games whose anti-cheat turns Linux away on purpose play on
 * Zurbax: Fortnite (Epic keeps Easy Anti-Cheat's Linux support switched off
 * for it), Valorant, Call of Duty. Nothing of theirs runs here, so there's
 * nothing to turn away; the anti-cheat runs with the game, on Windows, in
 * the data centre. Fortnite is free on Xbox Cloud Gaming with a Microsoft
 * account.
 *
 * The services stream to browsers built on Chromium, so Bridge uses one
 * from Flathub: Microsoft Edge, which Xbox Cloud Gaming streams to at its
 * best quality (Chrome, Chromium or Brave if one is already there),
 * installed the first time, and opened as an app window on the service.
 */
#include "bridge.h"

static const char *const BROWSERS[] = { "com.microsoft.Edge", "com.google.Chrome", "org.chromium.Chromium", "com.brave.Browser" };
#define GET_BROWSER "com.microsoft.Edge"

typedef struct {
	const char *name, *what, *about, *url, *alt, *alt_url, *tone;
} Service;

static const Service SERVICES[] = {
	{ "Fortnite", "Free on Xbox Cloud Gaming",
	  "Fortnite plays in the cloud: free with a Microsoft account, no Game Pass needed. A controller or keyboard and "
	  "mouse work as on a PC. It's also on GeForce NOW, with an NVIDIA account.",
	  "https://www.xbox.com/play/games/fortnite/BT5P2X999VH2", "GeForce NOW", "https://play.geforcenow.com/", "#4b2bb8" },
	{ "Xbox Cloud Gaming", "Hundreds of games with Game Pass",
	  "Call of Duty, Forza, Halo and hundreds more, streamed from Xbox servers. Most need Game Pass Ultimate; "
	  "Fortnite and a few others are free.",
	  "https://www.xbox.com/play", NULL, NULL, "#0e7a0d" },
	{ "GeForce NOW", "Your own games on NVIDIA's PCs",
	  "Plays the games you own on Steam, Epic, Ubisoft and more on NVIDIA's gaming PCs, anti-cheat and all. Free with "
	  "a wait and time limit, or paid for priority and higher quality.",
	  "https://play.geforcenow.com/", NULL, NULL, "#4f7a00" },
};

static int csel = -1;
static f64 looked_at = -100;
static char browser[64]; /* the Flatpak id of the browser to use, "" if none */

/* installing the browser, on a thread of its own */
static struct {
	Mutex lock;
	int busy; /* 1 installing, 2 done */
	f32 progress;
	bool ok;
} inst;

static void find_browser(void)
{
	browser[0] = 0;
	for (usize i = 0; i < ARRAY_LEN(BROWSERS) && !browser[0]; i++) {
		char *user = str_fmt("%s/.local/share/flatpak/app/%s/current", home_dir(), BROWSERS[i]);
		char *sys = str_fmt("%s/layers/steam/var/lib/flatpak/app/%s/current", zurbax_data(), BROWSERS[i]);
		if (dir_exists(user) || dir_exists(sys))
			str_copy(browser, BROWSERS[i], sizeof browser);
		mem_free(user);
		mem_free(sys);
	}
}

static void install_thread(void *arg)
{
	UNUSED(arg);
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[] = { layerrun, "flatpak", "install", "--user", "--noninteractive", "-y", "flathub", GET_BROWSER, NULL };
	int fds[2], status = -1;
	if (sys_pipe2(fds, O_CLOEXEC) == 0) {
		SpawnOpts o = { .stdin_fd = -1, .stdout_fd = fds[1], .stderr_fd = fds[1] };
		int pid = proc_spawn(layerrun, argv, environ, &o);
		sys_close(fds[1]);
		/* flatpak's progress: lines like "Installing 2/3… ████  45%  3.1 MB/s" */
		char buf[1024], line[256];
		usize len = 0;
		for (;;) {
			long got = sys_read(fds[0], buf, sizeof buf);
			if (got == -EINTR)
				continue;
			if (got <= 0)
				break;
			for (long i = 0; i < got; i++) {
				if (buf[i] != '\n' && buf[i] != '\r') {
					if (len + 1 < sizeof line)
						line[len++] = buf[i];
					continue;
				}
				line[len] = 0;
				len = 0;
				const char *pct = str_chr(line, '%');
				if (!pct)
					continue;
				const char *d = pct;
				while (d > line && d[-1] >= '0' && d[-1] <= '9')
					d--;
				mutex_lock(&inst.lock);
				if (d < pct)
					inst.progress = (f32)str_to_int(d, NULL) / 100.0f;
				mutex_unlock(&inst.lock);
			}
		}
		sys_close(fds[0]);
		if (pid > 0)
			proc_wait(pid, &status, false);
	}
	mem_free(layerrun);
	mutex_lock(&inst.lock);
	inst.ok = status == 0;
	mutex_unlock(&inst.lock);
	atomic_store(&inst.busy, 2);
}

/* The service in the browser, as an app window of its own. */
static void open_service(const char *url)
{
	if (!browser[0])
		return;
	char app[700];
	fmt(app, sizeof app, "--app=%s", url);
	char *args[] = { "--app", browser, "flatpak", "run", browser, app, "--start-maximized", "--ozone-platform-hint=auto", NULL };
	if (layer_start(args, true) < 0)
		str_copy(bridge_msg, "The browser couldn't start.", sizeof bridge_msg);
	else
		str_copy(bridge_msg, "Opening… sign in once, and the game streams here.", sizeof bridge_msg);
}

static void browser_card(Ui *ui, Rect *b)
{
	Theme *t = &ui->theme;
	int busy = atomic_add(&inst.busy, 0);
	if (busy == 2) {
		mutex_lock(&inst.lock);
		bool ok = inst.ok;
		mutex_unlock(&inst.lock);
		atomic_store(&inst.busy, 0);
		looked_at = -100;
		find_browser();
		if (!ok || !browser[0])
			str_copy(bridge_msg, "Microsoft Edge didn't install; check the connection and try again.", sizeof bridge_msg);
		busy = 0;
	}
	if (browser[0] && !busy)
		return;
	Rect card = cut_top(b, 108);
	cut_top(b, 16);
	ui_card(ui, card);
	Rect c = pad(card, 18);
	ui_text(ui, ui_font(ui, 14, 700), c.x, c.y + 16, "Cloud games stream to a browser made for them", t->text);
	if (busy) {
		mutex_lock(&inst.lock);
		f32 p = inst.progress;
		mutex_unlock(&inst.lock);
		char status[96];
		fmt(status, sizeof status, "Getting Microsoft Edge from Flathub… %d%%", (int)(p * 100));
		ui_text(ui, ui_font(ui, 12, 400), c.x, c.y + 38, status, t->text_dim);
		ui_progress(ui, rect(c.x, c.y + 50, c.w, 6), p);
		wake_within(ui, 0.25);
		return;
	}
	ui_text(ui, ui_font(ui, 12, 400), c.x, c.y + 38, "Microsoft Edge, from Flathub, about 400 MB. Once, then it updates with your other apps.",
		t->text_dim);
	if (!layer_has("/usr/bin/flatpak")) {
		ui_text(ui, ui_font(ui, 12, 400), c.x, c.y + 56, "It needs the Linux layer, which comes with Zurbax when it's installed.", t->danger);
		return;
	}
	if (ui_button_suggested(ui, rect(c.x, c.y + 48, 170, 32), "Get Microsoft Edge")) {
		inst.progress = 0;
		atomic_store(&inst.busy, 1);
		if (thread_spawn(install_thread, NULL) < 0)
			atomic_store(&inst.busy, 0);
	}
}

static void details(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	const Service *s = &SERVICES[csel];
	ui_card(ui, r);
	Rect c = pad(r, 18);
	Rect top = cut_top(&c, 108);
	draw_cover_img(ui, rect(top.x, top.y, 72, 108), NULL, false, NULL, s->name, color_hex(s->tone), 8);
	ui_text_wrapped(ui, ui_font(ui, 16, 700), rect(top.x + 88, top.y + 4, top.w - 88, 44), s->name, t->text, ALIGN_START);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(top.x + 88, top.y + 50, top.w - 88, 40), s->what, t->text_dim, ALIGN_START);
	cut_top(&c, 14);
	Rect btns = cut_top(&c, 36);
	bool ready = browser[0] != 0;
	if (ui_button_suggested(ui, cut_left(&btns, 120), "Play") && ready)
		open_service(s->url);
	if (s->alt) {
		cut_left(&btns, 8);
		if (ui_button(ui, cut_left(&btns, 130), s->alt) && ready)
			open_service(s->alt_url);
	}
	if (!ready)
		ui_text(ui, ui_font(ui, 11, 400), c.x, c.y + 14, "Get the browser above first.", t->text_faint);
	cut_top(&c, 24);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), cut_top(&c, 110), s->about, t->text_dim, ALIGN_START);
	cut_top(&c, 8);
	ui_text_wrapped(ui, ui_font(ui, 11, 400), cut_top(&c, 60),
			"Streaming wants a steady connection: 15 Mbit/s or more, and a cable or 5 GHz Wi-Fi for the smoothest play.",
			t->text_faint, ALIGN_START);
}

void cloud_tab(Ui *ui, Rect b)
{
	Theme *t = &ui->theme;
	if (ui->now - looked_at > 10) {
		looked_at = ui->now;
		find_browser();
	}
	if (csel < 0)
		csel = 0;
	Rect side = cut_right(&b, 340);
	cut_right(&b, 18);
	details(ui, side);
	/* why the cloud */
	Rect why = cut_top(&b, 104);
	cut_top(&b, 14);
	ui_card(ui, why);
	Rect w = pad(why, 18);
	ui_text(ui, ui_font(ui, 14, 700), w.x, w.y + 16, "For the games that won't run on Linux", t->text);
	ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(w.x, w.y + 26, w.w, 50),
			"Some games' makers set their anti-cheat to turn away every Linux system, Steam Deck too: Fortnite, Valorant, "
			"Call of Duty. In the cloud they run on a Windows PC far away and stream here, anti-cheat and all.",
			t->text_dim, ALIGN_START);
	browser_card(ui, &b);
	f32 tw = 150, ch = 225, th = ch + 48, gap = 22;
	int per = MAX(1, (int)((b.w + gap) / (tw + gap)));
	f32 y = ui_scroll_begin(ui, b, "cloud-lib");
	int n = (int)ARRAY_LEN(SERVICES);
	for (int i = 0; i < n; i++) {
		Rect cell = rect(b.x + (f32)(i % per) * (tw + gap), y + 8 + (f32)(i / per) * (th + gap), tw, th);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "tile"), cell);
		f32 lift = ui_anim(ui, ui_id(ui, "lift"), it.hovered, 14) * 5;
		Rect a = rect(cell.x, cell.y - lift, tw, ch);
		ui_shadow(ui, a, 12, 14 + lift * 3, 4 + lift, color_hex("#00000058"));
		draw_cover_img(ui, a, NULL, false, NULL, SERVICES[i].name, color_hex(SERVICES[i].tone), 12);
		if (csel == i)
			ui_border(ui, rect_inset(a, -4, -4), 15, 2.5f, t->accent);
		ui_text_in(ui, ui_font(ui, 13, 600), rect(cell.x, cell.y + ch + 8, tw, 20), SERVICES[i].name, t->text, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 11, 400), rect(cell.x, cell.y + ch + 27, tw, 16), "Cloud", t->text_faint, ALIGN_CENTER);
		if (it.clicked)
			csel = i;
		if (it.double_clicked && browser[0])
			open_service(SERVICES[i].url);
		ui_pop_id(ui);
	}
	ui_scroll_end(ui, 8 + (f32)((n + per - 1) / per) * (th + gap));
	if (bridge_msg[0])
		ui_text(ui, ui_font(ui, 12, 400), b.x, b.y + b.h - 4, bridge_msg, t->text_dim);
}
