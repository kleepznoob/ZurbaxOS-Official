/*
 * Installer — puts ZOS on this computer, from the install stick.
 *
 * Four short steps: pick a disk, pick how (erase it, or use its free space
 * next to Windows), confirm, and watch it finish in about a minute. The work
 * is done by zinstall; this app shows its progress.
 *
 * ZURBAX_INSTALL_DEMO=1 in the environment shows made-up disks and a
 * pretend installation, for trying the app outside the live session.
 */
#include "zui.h"

enum { P_DISK, P_HOW, P_CONFIRM, P_INSTALLING, P_DONE, P_FAILED };

#define MIN_FREE (16ull << 30)

typedef struct {
	char name[32];
	char model[64];
	u64 bytes, free_bytes;
	bool removable, usb, boot_medium, esp;
	char contents[64];   /* "Windows", "Empty"... */
	char os[32];         /* the system to keep, for "alongside" */
	bool has_zurbax;
} Disk;

typedef struct {
	int page;
	f64 changed_at;
	int dir;
	Disk disks[16];
	int ndisks;
	int disk;            /* chosen, or -1 */
	bool alongside;
	bool demo;
	/* the running installation */
	int pid, out_fd;
	StrBuf line;
	f32 fraction, shown_fraction;
	char step_text[128];
	char error[256];
	f64 demo_t0;
} Installer;

static Installer in = { .disk = -1, .pid = -1, .out_fd = -1 };

static void go(Ui *ui, int page)
{
	in.dir = page > in.page ? 1 : -1;
	in.page = page;
	in.changed_at = ui->now;
	ui->redraw = true;
}

static void format_size(char *out, usize cap, u64 bytes)
{
	/* Decimal units, like the numbers printed on the box. */
	f64 gb = (f64)bytes / 1e9;
	if (gb >= 1000)
		fmt(out, cap, "%.1f TB", gb / 1000);
	else
		fmt(out, cap, "%.0f GB", gb);
}

static const char *disk_title(const Disk *d)
{
	if (d->model[0] && !str_eq(d->model, "-"))
		return d->model;
	return d->usb || d->removable ? "USB Drive" : "Internal Disk";
}

/* ---- Talking to zinstall ------------------------------------------------------------------ */

static const char *zinstall_path(void)
{
	static char path[512];
	if (file_exists("/system/bin/zinstall"))
		return "/system/bin/zinstall";
	char *p = zurbax_path("bin/zinstall");
	str_copy(path, p, sizeof path);
	mem_free(p);
	return path;
}

static void describe(Disk *d, const char *fs, const char *partname, bool *ntfs, bool *ext4, int *nparts)
{
	(*nparts)++;
	if (str_eq(fs, "ntfs") || str_eq(fs, "bitlocker"))
		*ntfs = true;
	if (str_eq(fs, "ext4"))
		*ext4 = true;
	if (str_eq(partname, "ZURBAX-DATA"))
		d->has_zurbax = true;
}

static void finish_disk(Disk *d, bool ntfs, bool ext4, int nparts)
{
	if (!d->name[0])
		return;
	if (d->has_zurbax)
		str_copy(d->contents, OS_NAME, sizeof d->contents), str_copy(d->os, "the current ZOS files", sizeof d->os);
	else if (ntfs && d->esp)
		str_copy(d->contents, "Windows", sizeof d->contents), str_copy(d->os, "Windows", sizeof d->os);
	else if (ntfs)
		str_copy(d->contents, "Files from Windows", sizeof d->contents), str_copy(d->os, "its files", sizeof d->os);
	else if (ext4)
		str_copy(d->contents, "Linux", sizeof d->contents), str_copy(d->os, "Linux", sizeof d->os);
	else if (nparts == 0)
		str_copy(d->contents, "Empty", sizeof d->contents), str_copy(d->os, "", sizeof d->os);
	else
		fmt(d->contents, sizeof d->contents, "%d partition%s", nparts, nparts == 1 ? "" : "s"), str_copy(d->os, "what's on it", sizeof d->os);
}

static void parse_disks(const char *text)
{
	in.ndisks = 0;
	Disk *d = NULL;
	bool ntfs = false, ext4 = false;
	int nparts = 0;
	usize n;
	char **lines = str_split(text, '\n', &n);
	for (usize i = 0; i < n; i++) {
		usize nf;
		char **f = str_split(lines[i], ' ', &nf);
		if (nf >= 9 && str_eq(f[0], "disk") && in.ndisks < (int)ARRAY_LEN(in.disks)) {
			if (d)
				finish_disk(d, ntfs, ext4, nparts);
			d = &in.disks[in.ndisks++];
			memset(d, 0, sizeof *d);
			ntfs = ext4 = false;
			nparts = 0;
			str_copy(d->name, f[1], sizeof d->name);
			d->bytes = (u64)str_to_int(f[2], NULL);
			d->removable = str_eq(f[3], "1");
			d->usb = str_eq(f[4], "1");
			d->boot_medium = str_eq(f[5], "1");
			d->free_bytes = (u64)str_to_int(f[6], NULL);
			d->esp = str_eq(f[7], "1");
			/* The model is the rest of the line. */
			const char *m = lines[i];
			for (int k = 0; k < 8 && m; k++) {
				m = str_chr(m, ' ');
				if (m)
					m++;
			}
			str_copy(d->model, m ? m : "-", sizeof d->model);
		} else if (nf >= 5 && str_eq(f[0], "part") && d) {
			describe(d, f[3], f[4], &ntfs, &ext4, &nparts);
		}
		str_split_free(f, nf);
	}
	if (d)
		finish_disk(d, ntfs, ext4, nparts);
	str_split_free(lines, n);
}

static const char DEMO_DISKS[] = "disk nvme0n1 512110190592 0 0 0 0 1 Samsung SSD 980 PRO 512GB\n"
				 "part nvme0n1p1 104857600 fat EFI system partition\n"
				 "part nvme0n1p2 16777216 - Microsoft reserved partition\n"
				 "part nvme0n1p3 511000000000 ntfs Basic data partition\n"
				 "disk sda 2000398934016 0 0 0 1200000000000 1 ST2000DM008-2FR102\n"
				 "part sda1 800000000000 ntfs Basic data partition\n"
				 "disk sdb 31914983424 1 1 1 0 0 SanDisk Ultra\n"
				 "part sdb1 67108864 fat ZURBAX-INSTALL\n";

static void load_disks(void)
{
	if (in.demo) {
		parse_disks(DEMO_DISKS);
		return;
	}
	char *argv[] = { (char *)zinstall_path(), "disks", NULL };
	int status = 0;
	char *out = proc_run_capture(argv, &status);
	if (out && str_starts(out, "error ")) {
		str_copy(in.error, out + 6, sizeof in.error);
		char *nl = (char *)str_chr(in.error, '\n');
		if (nl)
			*nl = 0;
	}
	parse_disks(out ? out : "");
	mem_free(out);
}

static void handle_line(Ui *ui, const char *l)
{
	if (str_starts(l, "step ")) {
		const char *rest;
		in.fraction = (f32)str_to_float(l + 5, &rest);
		str_copy(in.step_text, rest && *rest ? rest + 1 : "", sizeof in.step_text);
	} else if (str_eq(l, "done")) {
		in.fraction = 1;
		go(ui, P_DONE);
	} else if (str_starts(l, "error ")) {
		str_copy(in.error, l + 6, sizeof in.error);
		go(ui, P_FAILED);
	}
	ui->redraw = true;
}

static void on_output(Loop *loop, int fd, u32 events, void *data)
{
	Ui *ui = data;
	char buf[1024];
	long n = sys_read(fd, buf, sizeof buf);
	if (n > 0) {
		for (long i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				handle_line(ui, sb_str(&in.line));
				sb_clear(&in.line);
			} else {
				sb_addc(&in.line, buf[i]);
			}
		}
		ui_wake(ui);
		return;
	}
	/* zinstall has exited. */
	loop_del_fd(loop, fd);
	sys_close(fd);
	in.out_fd = -1;
	int status;
	proc_wait(in.pid, &status, false);
	in.pid = -1;
	if (in.page == P_INSTALLING) {
		str_copy(in.error, "The installation stopped unexpectedly.", sizeof in.error);
		go(ui, P_FAILED);
	}
	ui_wake(ui);
}

static void start_install(Ui *ui)
{
	in.fraction = in.shown_fraction = 0;
	str_copy(in.step_text, "Starting", sizeof in.step_text);
	in.error[0] = 0;
	go(ui, P_INSTALLING);
	if (in.demo) {
		in.demo_t0 = ui->now;
		return;
	}
	int fds[2];
	if (sys_pipe2(fds, O_CLOEXEC) < 0) {
		str_copy(in.error, "The installer couldn't start.", sizeof in.error);
		go(ui, P_FAILED);
		return;
	}
	char *argv[] = { (char *)zinstall_path(), "install", in.disks[in.disk].name, in.alongside ? "alongside" : "erase", NULL };
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = fds[1], .stderr_fd = fds[1] };
	in.pid = proc_spawn(argv[0], argv, NULL, &o);
	sys_close(fds[1]);
	if (in.pid < 0) {
		sys_close(fds[0]);
		str_copy(in.error, "The installer couldn't start.", sizeof in.error);
		go(ui, P_FAILED);
		return;
	}
	in.out_fd = fds[0];
	loop_add_fd(ui_loop(ui), in.out_fd, EPOLLIN, on_output, ui);
}

static void demo_tick(Ui *ui)
{
	static const char *const steps[] = { "Reading the install stick", "Preparing the disk", "Creating partitions", "Copying ZOS",
					     "Preparing storage for your files", "Setting up startup" };
	f32 t = (f32)(ui->now - in.demo_t0) / 6.0f;
	in.fraction = minf(t, 1);
	str_copy(in.step_text, steps[MIN((int)(t * 6), 5)], sizeof in.step_text);
	if (t >= 1)
		go(ui, P_DONE);
	ui->redraw = true;
}

/* ---- Pages ------------------------------------------------------------------------------------ */

static void page_header(Ui *ui, Rect *r, const char *title, const char *subtitle)
{
	Theme *t = &ui->theme;
	ui_text_in(ui, ui_font(ui, 22, 700), cut_top(r, 34), title, t->text, ALIGN_START);
	cut_top(r, 4);
	f32 h = ui_text_height(ui, ui_font(ui, 14, 400), subtitle, r->w);
	ui_text_wrapped(ui, ui_font(ui, 14, 400), cut_top(r, h), subtitle, t->text_dim, ALIGN_START);
	cut_top(r, 18);
}

static bool choice_card(Ui *ui, Rect r, const char *id, const char *icon, const char *title, const char *subtitle, const char *side,
			bool selected, bool enabled, Color icon_color)
{
	Theme *t = &ui->theme;
	UiInteract it = ui_interact(ui, ui_id(ui, id), r);
	f32 hover = ui_fade(ui, ui_id(ui, id), it.hovered && enabled, 0.12f);
	Color bg = selected ? color_mix(t->card, t->accent, 0.10f) : color_mix(t->card, t->hover, hover);
	ui_round(ui, r, 10, bg);
	ui_border(ui, r, 10, selected ? 2 : 1, selected ? t->accent : t->border);
	Rect c = pad2(r, 16, 12);
	Rect ir = cut_left(&c, 40);
	ui_icon(ui, center_in(ir, 32, 32), icon, enabled ? icon_color : t->text_faint);
	cut_left(&c, 14);
	if (side) {
		Font sf = ui_font(ui, 14, 600);
		Rect sr = cut_right(&c, ui_text_width(ui, sf, side) + 4);
		ui_text_in(ui, sf, sr, side, enabled ? t->text : t->text_faint, ALIGN_END);
		cut_right(&c, 12);
	}
	Font tf = ui_font(ui, 15, 600), df = ui_font(ui, 13, 400);
	f32 dh = ui_text_height(ui, df, subtitle, c.w);
	f32 total = 22 + 2 + dh;
	Rect tr = cut_top(&c, (c.h - total) / 2);
	UNUSED(tr);
	ui_text_in(ui, tf, cut_top(&c, 22), title, enabled ? t->text : t->text_faint, ALIGN_START);
	cut_top(&c, 2);
	ui_text_wrapped(ui, df, cut_top(&c, dh), subtitle, enabled ? t->text_dim : t->text_faint, ALIGN_START);
	return it.clicked && enabled;
}

static void page_disk(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	page_header(ui, &r, "Where should ZOS go?", "Choose the disk to install ZOS on. The next step lets you keep what's already there, if there's room.");
	Rect bottom = cut_bottom(&r, 36);
	cut_bottom(&r, 16);
	if (in.ndisks == 0) {
		ui_placeholder(ui, r, "drive", "No disks found", in.error[0] ? in.error : "Connect a disk and look again.");
	} else {
		f32 y = ui_scroll_begin(ui, r, "disks");
		for (int i = 0; i < in.ndisks; i++) {
			Disk *d = &in.disks[i];
			char size[32], sub[160];
			format_size(size, sizeof size, d->bytes);
			if (d->boot_medium)
				str_copy(sub, "ZOS is running from this stick", sizeof sub);
			else
				fmt(sub, sizeof sub, "%s%s", d->contents, d->usb ? " · USB" : "");
			ui_push_id(ui, i);
			Rect row = rect(r.x, y, r.w, 74);
			if (choice_card(ui, row, "disk", d->usb || d->removable ? "usb" : "drive", disk_title(d), sub, size, in.disk == i, !d->boot_medium,
					t->text_dim))
				in.disk = i;
			ui_pop_id(ui);
			y += 84;
		}
		ui_scroll_end(ui, (f32)in.ndisks * 84);
	}
	if (ui_button(ui, cut_left(&bottom, 120), "Look Again")) {
		load_disks();
		if (in.disk >= in.ndisks)
			in.disk = -1;
	}
	bool can = in.disk >= 0;
	if (ui_button_suggested(ui, cut_right(&bottom, 120), "Next") && can) {
		Disk *d = &in.disks[in.disk];
		in.alongside = (d->free_bytes >= MIN_FREE || d->has_zurbax) && d->esp;
		go(ui, P_HOW);
	}
}

static void page_how(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Disk *d = &in.disks[in.disk];
	char title[128];
	fmt(title, sizeof title, "How should ZOS use %s?", disk_title(d));
	page_header(ui, &r, title, "ZOS needs at least 16 GB.");
	Rect bottom = cut_bottom(&r, 36);
	cut_bottom(&r, 16);

	bool empty = str_eq(d->contents, "Empty");
	char sub[256];
	fmt(sub, sizeof sub, empty ? "The whole disk is used for ZOS." : "Everything on the disk is deleted, including %s.", d->os);
	if (choice_card(ui, cut_top(&r, 82), "erase", "drive", empty ? "Use the whole disk" : "Erase disk and install ZOS", sub, NULL,
			!in.alongside, true, empty ? t->text_dim : t->danger))
		in.alongside = false;
	cut_top(&r, 10);

	if (!empty) {
		bool can = (d->free_bytes >= MIN_FREE || d->has_zurbax) && d->esp;
		char size[32], label[128];
		format_size(size, sizeof size, d->free_bytes);
		if (d->has_zurbax) {
			str_copy(label, "Reinstall ZOS, keep the rest", sizeof label);
			str_copy(sub, "ZOS's storage is replaced; other systems on the disk stay as they are.", sizeof sub);
		} else {
			fmt(label, sizeof label, "Install alongside %s", d->os);
			if (can)
				fmt(sub, sizeof sub, "Uses %s of free space. %s and its files stay as they are.", size, d->os);
			else if (!d->esp)
				str_copy(sub, "Not possible on this disk: it has no EFI system partition to start from.", sizeof sub);
			else
				fmt(sub, sizeof sub, "Needs 16 GB of free space first. In Windows, open Disk Management and shrink a partition to make room.");
		}
		if (choice_card(ui, cut_top(&r, 82), "alongside", "computer", label, sub, NULL, in.alongside, can, t->accent))
			in.alongside = true;
		if (in.alongside) {
			cut_top(&r, 16);
			ui_text_wrapped(ui, ui_font(ui, 13, 400), cut_top(&r, 40),
					"ZOS starts by default. To start the other system, open your computer's boot menu while it starts (F8 or Esc on most ASUS computers).",
					t->text_dim, ALIGN_START);
		}
	}
	if (ui_button(ui, cut_left(&bottom, 100), "Back"))
		go(ui, P_DISK);
	if (ui_button_suggested(ui, cut_right(&bottom, 120), "Next"))
		go(ui, P_CONFIRM);
}

static void page_confirm(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Disk *d = &in.disks[in.disk];
	char size[32], sub[256];
	format_size(size, sizeof size, d->bytes);
	fmt(sub, sizeof sub, "ZOS will be installed on %s (%s). It takes about a minute.", disk_title(d), size);
	page_header(ui, &r, "Ready to install", sub);
	Rect bottom = cut_bottom(&r, 36);
	cut_bottom(&r, 16);
	bool erase = !in.alongside;
	Rect box = cut_top(&r, 92);
	Color tint = erase ? t->danger : t->success;
	ui_round(ui, box, 10, color_with_alpha(tint, 0x1e));
	ui_border(ui, box, 10, 1, color_with_alpha(tint, 0x60));
	Rect c = pad2(box, 18, 16);
	ui_icon(ui, center_in(cut_left(&c, 36), 30, 30), erase ? "warning" : "shield-check", tint);
	cut_left(&c, 14);
	const char *head = erase ? "Everything on this disk will be erased" : "Your other system stays";
	char body[256];
	if (erase)
		fmt(body, sizeof body, "%s on %s will be deleted. This can't be undone, so copy anything you need first.",
		    str_eq(d->contents, "Empty") ? "Anything" : d->os, disk_title(d));
	else
		fmt(body, sizeof body, "%s keeps its files. ZOS goes into the free space and adds itself to the startup menu.", d->os);
	ui_text_in(ui, ui_font(ui, 15, 700), cut_top(&c, 24), head, t->text, ALIGN_START);
	ui_text_wrapped(ui, ui_font(ui, 13, 400), c, body, t->text_dim, ALIGN_START);
	cut_top(&r, 22);
	static const char *const facts[] = { "Your account and settings are set up when ZOS first starts.",
					     "Keep the computer plugged in until it's done." };
	for (int i = 0; i < 2; i++) {
		Rect row = cut_top(&r, 28);
		ui_icon(ui, center_in(cut_left(&row, 22), 16, 16), "check", t->accent);
		cut_left(&row, 8);
		ui_text_in(ui, ui_font(ui, 14, 400), row, facts[i], t->text, ALIGN_START);
	}
	if (ui_button(ui, cut_left(&bottom, 100), "Back"))
		go(ui, P_HOW);
	Rect go_r = cut_right(&bottom, 170);
	if (erase ? ui_button_destructive(ui, go_r, "Erase and Install") : ui_button_suggested(ui, go_r, "Install"))
		start_install(ui);
}

static void page_installing(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	if (in.demo)
		demo_tick(ui);
	in.shown_fraction += (in.fraction - in.shown_fraction) * minf(1, ui->dt * 6);
	if (fabsf(in.fraction - in.shown_fraction) > 0.002f)
		ui->redraw = true;
	Rect c = center_in(r, minf(440, r.w), 230);
	ui_app_icon(ui, center_in(cut_top(&c, 96), 96, 96), "installer");
	cut_top(&c, 18);
	ui_text_in(ui, ui_font(ui, 20, 700), cut_top(&c, 30), "Installing ZOS", t->text, ALIGN_CENTER);
	cut_top(&c, 18);
	ui_progress(ui, cut_top(&c, 8), in.shown_fraction);
	cut_top(&c, 12);
	ui_text_in(ui, ui_font(ui, 13, 400), cut_top(&c, 20), in.step_text, t->text_dim, ALIGN_CENTER);
}

static void page_result(Ui *ui, Rect r, bool ok)
{
	Theme *t = &ui->theme;
	Rect c = center_in(r, minf(460, r.w), 260);
	Rect ir = center_in(cut_top(&c, 80), 80, 80);
	ui_circle(ui, ir.x + 40, ir.y + 40, 40, color_with_alpha(ok ? t->success : t->danger, 0x26));
	ui_icon(ui, center_in(ir, 40, 40), ok ? "check" : "warning", ok ? t->success : t->danger);
	cut_top(&c, 18);
	ui_text_in(ui, ui_font(ui, 20, 700), cut_top(&c, 30), ok ? "ZOS is installed" : "ZOS couldn't be installed", t->text, ALIGN_CENTER);
	cut_top(&c, 6);
	const char *msg = ok ? "Remove the USB stick, then restart to start using ZOS." : in.error;
	f32 h = ui_text_height(ui, ui_font(ui, 14, 400), msg, c.w);
	ui_text_wrapped(ui, ui_font(ui, 14, 400), cut_top(&c, h), msg, t->text_dim, ALIGN_CENTER);
	cut_top(&c, 22);
	Rect b = center_in(cut_top(&c, 38), 340, 38);
	if (ok) {
		if (ui_button(ui, cut_left(&b, 160), "Keep Trying"))
			ui->quit = true;
		if (ui_button_suggested(ui, cut_right(&b, 160), "Restart Now"))
			ui_session(ui, SESSION_RESTART);
	} else {
		if (ui_button(ui, cut_left(&b, 160), "Close"))
			ui->quit = true;
		if (ui_button_suggested(ui, cut_right(&b, 160), "Try Again")) {
			load_disks();
			in.disk = -1;
			go(ui, P_DISK);
		}
	}
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	ui_headerbar(ui, cut_top(&win, 46), "Install ZOS", HEADER_NO_MAXIMIZE | HEADER_FLAT);
	f32 slide = clampf((f32)(ui->now - in.changed_at) / 0.22f, 0, 1);
	slide = 1 - (1 - slide) * (1 - slide);
	if (slide < 1)
		ui->redraw = true;
	Rect body = pad4(win, 18, 40, 28, 40);
	body.x += (1 - slide) * 30 * (f32)in.dir;
	ui_push_clip(ui, win);
	switch (in.page) {
	case P_DISK:
		page_disk(ui, body);
		break;
	case P_HOW:
		page_how(ui, body);
		break;
	case P_CONFIRM:
		page_confirm(ui, body);
		break;
	case P_INSTALLING:
		page_installing(ui, body);
		break;
	case P_DONE:
		page_result(ui, body, true);
		break;
	case P_FAILED:
		page_result(ui, body, false);
		break;
	}
	ui_pop_clip(ui);
}

static bool on_close(Ui *ui)
{
	UNUSED(ui);
	/* Stopping halfway would leave the disk unusable. */
	return in.page != P_INSTALLING;
}

static void init(Ui *ui)
{
	UNUSED(ui);
	in.demo = env_get("ZURBAX_INSTALL_DEMO") != NULL;
	load_disks();
	/* One disk to choose from (besides the stick): choose it already. */
	int candidates = 0, last = -1;
	for (int i = 0; i < in.ndisks; i++)
		if (!in.disks[i].boot_medium)
			candidates++, last = i;
	if (candidates == 1)
		in.disk = last;
}

int main(void)
{
	UiAppDesc d = { .id = "installer", .title = "Install ZOS", .width = 720, .height = 560, .min_width = 620, .min_height = 520,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame, .close = on_close };
	return ui_run(&d);
}
