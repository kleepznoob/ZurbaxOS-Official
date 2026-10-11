/*
 * Installer — puts Zurbax on this computer, from the install stick or disc.
 *
 * The steps run down a sidebar like Welcome's (setup.c): pick a disk (each
 * drawn as a bar of what's on it), pick how (erase it, or use its free
 * space next to Windows, with the disk shown before and after), check the
 * summary, and watch a slideshow of what's coming while it installs. The
 * work is done by zinstall; this app shows its progress.
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
	struct { u64 bytes; char fs[12]; } parts[8];
	int nparts;
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
	f64 started_at, result_at;
	/* the slideshow */
	Canvas *slides[8];
	int nslides;
	bool slides_loaded;
} Installer;

static Installer in = { .disk = -1, .pid = -1, .out_fd = -1 };

static void go(Ui *ui, int page)
{
	in.dir = page > in.page ? 1 : -1;
	in.page = page;
	in.changed_at = ui->now;
	if (page == P_DONE || page == P_FAILED)
		in.result_at = ui->now;
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
		str_copy(d->contents, OS_NAME, sizeof d->contents), str_copy(d->os, "the current Zurbax files", sizeof d->os);
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
			if (d->nparts < (int)ARRAY_LEN(d->parts)) {
				d->parts[d->nparts].bytes = (u64)str_to_int(f[2], NULL);
				str_copy(d->parts[d->nparts].fs, f[3], sizeof d->parts[0].fs);
				d->nparts++;
			}
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
	in.started_at = 0;   /* set on the first frame of the page */
	go(ui, P_INSTALLING);
	if (in.demo) {
		in.demo_t0 = 0;
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
	static const char *const steps[] = { "Reading the install media", "Preparing the disk", "Creating partitions", "Copying Zurbax",
					     "Preparing storage for your files", "Setting up startup" };
	if (in.demo_t0 <= 0)
		in.demo_t0 = ui->now;
	f32 t = (f32)(ui->now - in.demo_t0) / 30.0f;
	in.fraction = minf(t, 1);
	str_copy(in.step_text, steps[MIN((int)(t * 6), 5)], sizeof in.step_text);
	if (t >= 1)
		go(ui, P_DONE);
	ui->redraw = true;
}

/* ---- Pages ------------------------------------------------------------------------------------ */

enum { S_DISK, S_HOW, S_SUMMARY, S_INSTALL, S_FINISH };
static const char *const STEPS[] = { "Disk", "Install type", "Summary", "Installing", "Finish" };

static int step_of(int page)
{
	switch (page) {
	case P_DISK:
		return S_DISK;
	case P_HOW:
		return S_HOW;
	case P_CONFIRM:
		return S_SUMMARY;
	case P_INSTALLING:
		return S_INSTALL;
	default:
		return S_FINISH;
	}
}

static Color fs_color(Ui *ui, const char *fs)
{
	if (str_eq(fs, "ntfs") || str_eq(fs, "bitlocker"))
		return color_hex("#3d7bd9");
	if (str_eq(fs, "ext4"))
		return color_hex("#e0812f");
	if (str_eq(fs, "fat"))
		return color_hex("#8a8f99");
	return ui->theme.dark ? color_hex("#5a5960") : color_hex("#b8b9bf");
}

/* What's on a disk, as a bar: each partition its width, the free space
 * last. With grow < 1 the Zurbax part (when there is one) is still filling. */
static void disk_bar(Ui *ui, Rect r, const Disk *d, int zurbax, f32 grow)
{
	Theme *t = &ui->theme;
	ui_round(ui, r, 6, t->dark ? color_hex("#2a292f") : color_hex("#e4e5e9"));
	if (!d->bytes)
		return;
	ui_push_clip(ui, r);
	f32 x = r.x;
	if (zurbax == 2) {
		/* the whole disk, erased */
		ui_round(ui, rect(r.x, r.y, r.w * grow, r.h), 6, t->accent);
	} else {
		for (int i = 0; i < d->nparts; i++) {
			f32 w = maxf(3, r.w * (f32)((f64)d->parts[i].bytes / (f64)d->bytes));
			ui_fill(ui, rect(x, r.y, w - 1.5f, r.h), fs_color(ui, d->parts[i].fs));
			x += w;
		}
		if (zurbax == 1) {
			/* Zurbax in the free space */
			f32 w = r.w * (f32)((f64)d->free_bytes / (f64)d->bytes);
			ui_fill(ui, rect(r.x + r.w - w, r.y, w * grow, r.h), t->accent);
		}
	}
	ui_pop_clip(ui);
}

static void legend(Ui *ui, f32 x, f32 y, Color c, const char *label)
{
	ui_round(ui, rect(x, y + 4, 10, 10), 3, c);
	ui_text(ui, ui_font(ui, 12, 500), x + 16, y + 13, label, ui->theme.text_dim);
}

static void page_disk(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	setup_title(ui, &r, in.changed_at, "Where should Zurbax go?",
		    "Choose the disk to install Zurbax on. Each bar shows what's on a disk now; the next step lets you keep it, if there's room.");
	if (in.ndisks == 0) {
		ui_placeholder(ui, r, "drive", "No disks found", in.error[0] ? in.error : "Connect a disk and look again.");
		return;
	}
	f32 y = ui_scroll_begin(ui, r, "disks");
	for (int i = 0; i < in.ndisks; i++) {
		Disk *d = &in.disks[i];
		char size[32], sub[160];
		format_size(size, sizeof size, d->bytes);
		if (d->boot_medium)
			str_copy(sub, "Zurbax is running from this stick", sizeof sub);
		else
			fmt(sub, sizeof sub, "%s%s", d->contents, d->usb ? " · USB" : "");
		ui_push_id(ui, i);
		Rect row = setup_rise(ui, in.changed_at, i, rect(r.x, y, r.w, 96));
		bool ok = !d->boot_medium, sel = in.disk == i;
		UiInteract it = ui_interact(ui, ui_id(ui, "disk"), row);
		f32 hover = ui_fade(ui, ui_id(ui, "disk"), it.hovered && ok, 0.15f);
		ui_round(ui, row, 14, sel ? color_mix(t->card, t->accent, t->dark ? 0.16f : 0.07f) : color_mix(t->card, t->hover, hover));
		ui_border(ui, row, 14, sel ? 2 : 1, sel ? t->accent : t->border);
		Rect c = pad2(row, 18, 14);
		ui_icon(ui, rect(c.x, c.y + 4, 32, 32), d->usb || d->removable ? "usb" : "drive", ok ? t->text_dim : t->text_faint);
		Rect tx = rect(c.x + 48, c.y, c.w - 48, 40);
		Font sf = ui_font(ui, 15, 700);
		ui_text_in(ui, sf, rect(tx.x + tx.w - 90, tx.y, 90, 22), size, ok ? t->text : t->text_faint, ALIGN_END);
		ui_text_in(ui, ui_font(ui, 15, 700), rect(tx.x, tx.y, tx.w - 100, 22), disk_title(d), ok ? t->text : t->text_faint, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 13, 400), rect(tx.x, tx.y + 22, tx.w - 100, 18), sub, ok ? t->text_dim : t->text_faint, ALIGN_START);
		disk_bar(ui, rect(c.x + 48, c.y + 50, c.w - 48, 12), d, 0, 1);
		if (it.clicked && ok)
			in.disk = i;
		ui_pop_id(ui);
		y += 108;
	}
	f32 lx = r.x;
	legend(ui, lx, y, color_hex("#3d7bd9"), "Windows");
	legend(ui, lx + 100, y, color_hex("#e0812f"), "Linux");
	legend(ui, lx + 180, y, color_hex("#8a8f99"), "Startup");
	legend(ui, lx + 268, y, t->dark ? color_hex("#2a292f") : color_hex("#e4e5e9"), "Free");
	ui_scroll_end(ui, (f32)in.ndisks * 108 + 24);
}

static void page_how(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Disk *d = &in.disks[in.disk];
	char about[256];
	fmt(about, sizeof about, "On %s. Zurbax needs at least 16 GB; the bars show the disk now, and after.", disk_title(d));
	setup_title(ui, &r, in.changed_at, "How should Zurbax use this disk?", about);
	bool empty = str_eq(d->contents, "Empty");
	bool can = (d->free_bytes >= MIN_FREE || d->has_zurbax) && d->esp && !empty;
	Rect row = cut_top(&r, 176);
	f32 cw = empty ? row.w : (row.w - 18) / 2;
	char sub[256];
	fmt(sub, sizeof sub, empty ? "The whole disk becomes Zurbax's." : "Everything on the disk is deleted, including %s.", d->os);
	if (setup_choice(ui, setup_rise(ui, in.changed_at, 0, rect(row.x, row.y, cw, row.h)), "erase", "drive",
			 empty ? "Use the whole disk" : "Erase disk and install", sub, !in.alongside, true, empty ? t->accent : t->danger))
		in.alongside = false;
	if (!empty) {
		char size[32], label[128];
		format_size(size, sizeof size, d->free_bytes);
		if (d->has_zurbax) {
			str_copy(label, "Reinstall, keep the rest", sizeof label);
			str_copy(sub, "Zurbax's storage is replaced; other systems on the disk stay as they are.", sizeof sub);
		} else {
			fmt(label, sizeof label, "Install alongside %s", d->os);
			if (can)
				fmt(sub, sizeof sub, "Uses the %s of free space. %s and its files stay as they are.", size, d->os);
			else if (!d->esp)
				str_copy(sub, "Not possible on this disk: it has no EFI system partition to start from.", sizeof sub);
			else
				str_copy(sub, "Needs 16 GB of free space first. In Windows, open Disk Management and shrink a partition.", sizeof sub);
		}
		if (setup_choice(ui, setup_rise(ui, in.changed_at, 1, rect(row.x + cw + 18, row.y, cw, row.h)), "alongside", "computer", label, sub,
				 in.alongside, can, t->success))
			in.alongside = true;
	}
	cut_top(&r, 26);
	/* before and after */
	Rect ba = setup_rise(ui, in.changed_at, 2, cut_top(&r, 120));
	ui_round(ui, ba, 14, t->card);
	ui_border(ui, ba, 14, 1, t->border);
	Rect c = pad2(ba, 20, 18);
	f32 grow = setup_ease((f32)((ui->now - in.changed_at - 0.3) / 0.9));
	static f64 last_choice_at;
	static bool last_alongside;
	if (last_alongside != in.alongside)
		last_alongside = in.alongside, last_choice_at = ui->now;
	f32 grow2 = minf(grow, setup_ease((f32)((ui->now - last_choice_at) / 0.7)));
	if (grow2 < 1)
		ui->redraw = true;
	ui_text_in(ui, ui_font(ui, 12, 700), rect(c.x, c.y, 60, 16), "NOW", t->text_dim, ALIGN_START);
	disk_bar(ui, rect(c.x + 64, c.y + 2, c.w - 64, 14), d, 0, 1);
	ui_text_in(ui, ui_font(ui, 12, 700), rect(c.x, c.y + 34, 60, 16), "AFTER", t->text_dim, ALIGN_START);
	disk_bar(ui, rect(c.x + 64, c.y + 36, c.w - 64, 14), d, in.alongside ? 1 : 2, grow2);
	legend(ui, c.x + 64, c.y + 62, t->accent, "Zurbax");
	if (in.alongside)
		ui_text_in(ui, ui_font(ui, 12, 400), rect(c.x + 150, c.y + 62, c.w - 150, 18),
			   "To start the other system, open the boot menu while the computer starts.", t->text_dim, ALIGN_START);
}

static void summary_row(Ui *ui, Rect *r, const char *icon, const char *what, const char *value)
{
	Theme *t = &ui->theme;
	Rect row = cut_top(r, 42);
	ui_icon(ui, rect(row.x, row.y + 12, 18, 18), icon, t->accent);
	ui_text_in(ui, ui_font(ui, 14, 500), rect(row.x + 30, row.y, 150, row.h), what, t->text_dim, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 14, 600), rect(row.x + 180, row.y, row.w - 180, row.h), value, t->text, ALIGN_START);
	ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
}

static void page_confirm(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Disk *d = &in.disks[in.disk];
	char size[32], disk[128], how[160];
	format_size(size, sizeof size, d->bytes);
	fmt(disk, sizeof disk, "%s (%s)", disk_title(d), size);
	bool erase = !in.alongside;
	if (erase)
		str_copy(how, "Erase the disk and install", sizeof how);
	else
		fmt(how, sizeof how, "Next to %s, in the free space", d->os);
	setup_title(ui, &r, in.changed_at, "Ready to install", "Check that this is right. Nothing has changed yet.");
	Rect list = setup_rise(ui, in.changed_at, 0, cut_top(&r, 4 * 42));
	summary_row(ui, &list, "drive", "Disk", disk);
	summary_row(ui, &list, "computer", "Installation", how);
	summary_row(ui, &list, "apps", "Included", "Steam, Proton, GNOME, 8 apps and every driver");
	summary_row(ui, &list, "clock", "Time", "About five minutes");
	cut_top(&r, 22);
	Rect box = setup_rise(ui, in.changed_at, 1, cut_top(&r, 86));
	Color tint = erase ? t->danger : t->success;
	ui_round(ui, box, 12, color_with_alpha(tint, 0x1c));
	ui_border(ui, box, 12, 1, color_with_alpha(tint, 0x60));
	Rect c = pad2(box, 18, 16);
	ui_icon(ui, center_in(cut_left(&c, 36), 28, 28), erase ? "warning" : "shield-check", tint);
	cut_left(&c, 14);
	const char *head = erase ? "Everything on this disk will be erased" : "Your other system stays";
	char body[256];
	if (erase)
		fmt(body, sizeof body, "%s on %s will be deleted. This can't be undone, so copy anything you need first.",
		    str_eq(d->contents, "Empty") ? "Anything" : d->os, disk_title(d));
	else
		fmt(body, sizeof body, "%s keeps its files. Zurbax goes into the free space and adds itself to the startup menu.", d->os);
	ui_text_in(ui, ui_font(ui, 15, 700), cut_top(&c, 24), head, t->text, ALIGN_START);
	ui_text_wrapped(ui, ui_font(ui, 13, 400), c, body, t->text_dim, ALIGN_START);
}

/* ---- Installing: a slideshow of what's coming, and the progress ---------------------------- */

static const char *const SLIDE_FILES[] = { "harbor", "settings", "dashboard", "launcher", "music", "lock" };
static const char *const SLIDE_TITLES[] = { "Thousands of apps", "Made to your taste", "Your day at a glance", "Every app on one screen",
					    "Your music, beautifully", "Two desktops, one machine" };
static const char *const SLIDE_TEXTS[] = {
	"Harbor brings the whole of Flathub. Firefox, LibreOffice, GIMP and VLC are installed already.",
	"Light or dark, any accent, ten wallpapers. Settings has everything in one place.",
	"The dashboard shows the weather, your next events, your mail and your music.",
	"Press the grid at the left of the taskbar, or just start typing.",
	"Music plays your library with album art, and keeps going while you work.",
	"Sign in to Zurbax's own desktop or to GNOME. Your files are in both.",
};
#define NSLIDES ((int)ARRAY_LEN(SLIDE_FILES))
#define SLIDE_SECONDS 7.0

static void load_slides(void)
{
	if (in.slides_loaded)
		return;
	in.slides_loaded = true;
	for (int i = 0; i < NSLIDES; i++) {
		char rel[64];
		fmt(rel, sizeof rel, "share/setup/%s.jpg", SLIDE_FILES[i]);
		char *p = zurbax_path(rel);
		in.slides[i] = image_load(p);
		mem_free(p);
	}
	in.nslides = NSLIDES;
}

static void page_installing(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	if (in.demo)
		demo_tick(ui);
	in.shown_fraction += (in.fraction - in.shown_fraction) * minf(1, ui->dt * 4);
	if (fabsf(in.fraction - in.shown_fraction) > 0.002f)
		ui->redraw = true;
	load_slides();
	if (in.started_at <= 0)
		in.started_at = ui->now;
	setup_title(ui, &r, in.changed_at, "Installing Zurbax", "Sit back: this takes a few minutes. Here's some of what's waiting.");
	/* the slideshow: each picture slides in from the right */
	Rect prog = cut_bottom(&r, 74);
	cut_bottom(&r, 18);
	Rect show = r;
	f32 sh = minf(show.h, show.w * 9 / 16 + 64);
	show.h = sh;
	f64 e = ui->now - in.started_at;
	int cur = (int)(e / SLIDE_SECONDS) % NSLIDES, prev = (cur + NSLIDES - 1) % NSLIDES;
	f32 k = setup_ease((f32)((fmod(e, SLIDE_SECONDS)) / 0.8));
	if (e < SLIDE_SECONDS)
		k = 1;
	ui_shadow(ui, show, 16, 30, 10, color_with_alpha(t->shadow, 0x40));
	ui_round(ui, show, 16, color_hex("#141218"));
	Rect pic = rect(show.x, show.y, show.w, show.h - 64);
	ui_push_clip(ui, show);
	for (int pass = 0; pass < 2; pass++) {
		int i = pass ? cur : prev;
		if (!pass && k >= 1)
			continue;
		f32 dx = pass ? (1 - k) * show.w : -k * show.w * 0.3f;
		Rect pr = rect(pic.x + dx, pic.y, pic.w, pic.h);
		if (in.slides[i])
			ui_image(ui, pr, in.slides[i], 0, true);
		else
			ui_gradient(ui, pr, 0, t->accent, color_hex("#141218"));
		Rect cap = rect(show.x + 20 + dx, show.y + show.h - 64, show.w - 40, 64);
		ui_text_in(ui, ui_font(ui, 15, 700), rect(cap.x, cap.y + 10, cap.w, 22), SLIDE_TITLES[i], COLOR_WHITE, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 12, 400), rect(cap.x, cap.y + 32, cap.w - 90, 18), SLIDE_TEXTS[i], color_hex("#ffffffb0"), ALIGN_START);
	}
	ui_pop_clip(ui);
	for (int i = 0; i < NSLIDES; i++) {
		f32 x = show.x + show.w - 20 - (f32)(NSLIDES - i) * 14, y = show.y + show.h - 32;
		ui_circle(ui, x, y, i == cur ? 4 : 3, i == cur ? COLOR_WHITE : color_hex("#ffffff50"));
	}
	ui->wake_at = ui->now + (k < 1 ? 0.016 : 0.25);
	/* how far along, in words and numbers */
	char pct[16], left[64] = "";
	fmt(pct, sizeof pct, "%d%%", (int)(in.shown_fraction * 100 + 0.5f));
	f32 f = in.fraction;
	if (f > 0.05f && f < 1) {
		f64 rest = e / f * (1 - f);
		if (rest < 60)
			str_copy(left, "Less than a minute left", sizeof left);
		else
			fmt(left, sizeof left, "About %d minute%s left", (int)(rest / 60 + 0.5), (int)(rest / 60 + 0.5) == 1 ? "" : "s");
	}
	ui_text_in(ui, ui_font(ui, 14, 600), rect(prog.x, prog.y, prog.w - 80, 22), in.step_text, t->text, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 22, 800), rect(prog.x + prog.w - 80, prog.y - 4, 80, 28), pct, t->text, ALIGN_END);
	Rect bar = rect(prog.x, prog.y + 32, prog.w, 8);
	ui_round(ui, bar, 4, t->border);
	f32 w = bar.w * in.shown_fraction;
	ui_round(ui, rect(bar.x, bar.y, w, bar.h), 4, t->accent);
	/* a light that runs along the filled part */
	if (w > 40 && !t->reduce_motion) {
		f32 sx = bar.x + fmodf((f32)ui->now * 160, w + 60) - 30;
		ui_push_clip(ui, rect(bar.x, bar.y, w, bar.h));
		ui_round(ui, rect(sx, bar.y, 30, bar.h), 4, color_with_alpha(COLOR_WHITE, 0x50));
		ui_pop_clip(ui);
		ui->wake_at = ui->now + 0.016;
	}
	ui_text_in(ui, ui_font(ui, 12, 400), rect(prog.x, prog.y + 48, prog.w, 18), left, t->text_dim, ALIGN_START);
}

static void page_result(Ui *ui, Rect r, bool ok)
{
	Theme *t = &ui->theme;
	setup_tick(ui, rect(r.x, r.y, 80, 80), in.result_at, ok);
	cut_top(&r, 104);
	setup_title(ui, &r, in.changed_at, ok ? "Zurbax is installed" : "Zurbax couldn't be installed",
		    ok ? "Remove the USB stick or disc, then restart. Zurbax starts straight away and asks a few questions to set up your account."
		       : in.error);
	if (ok) {
		static const char *const icons[] = { "usb", "system-restart", "user" };
		static const char *const heads[] = { "Remove the stick", "Restart", "Say hello" };
		static const char *const texts[] = { "Or the disc. Zurbax is on the disk now.", "Zurbax starts on its own, no menu to pick from.",
						     "Welcome sets up your name, Wi-Fi and look." };
		Rect row = cut_top(&r, 120);
		f32 cw = (row.w - 32) / 3;
		for (int i = 0; i < 3; i++) {
			Rect c = setup_rise(ui, in.changed_at, i, rect(row.x + (f32)i * (cw + 16), row.y, cw, row.h));
			ui_round(ui, c, 14, t->card);
			ui_border(ui, c, 14, 1, t->border);
			ui_icon(ui, rect(c.x + 18, c.y + 18, 22, 22), icons[i], t->accent);
			ui_text_in(ui, ui_font(ui, 15, 700), rect(c.x + 18, c.y + 52, c.w - 36, 22), heads[i], t->text, ALIGN_START);
			ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(c.x + 18, c.y + 76, c.w - 36, 40), texts[i], t->text_dim, ALIGN_START);
		}
	}
}

static void frame(Ui *ui)
{
	SetupSteps s = { .title = "Installer", .steps = STEPS, .count = (int)ARRAY_LEN(STEPS),
			 .current = in.page == P_DONE ? (int)ARRAY_LEN(STEPS) : step_of(in.page), .changed_at = in.changed_at, .dir = in.dir };
	Rect page = setup_frame(ui, &s);
	Rect bottom = cut_bottom(&page, 40);
	cut_bottom(&page, 18);
	ui_push_clip(ui, rect(ui->width - page.w - 120, 0, page.w + 120, ui->height));
	switch (in.page) {
	case P_DISK:
		page_disk(ui, page);
		break;
	case P_HOW:
		page_how(ui, page);
		break;
	case P_CONFIRM:
		page_confirm(ui, page);
		break;
	case P_INSTALLING:
		page_installing(ui, page);
		break;
	case P_DONE:
		page_result(ui, page, true);
		break;
	case P_FAILED:
		page_result(ui, page, false);
		break;
	}
	ui_pop_clip(ui);
	if (in.page == P_INSTALLING)
		return;
	ui_separator(ui, bottom.x, bottom.y - 10, bottom.x + bottom.w);
	switch (in.page) {
	case P_DISK:
		if (ui_button(ui, cut_left(&bottom, 130), "Look Again")) {
			load_disks();
			if (in.disk >= in.ndisks)
				in.disk = -1;
		}
		if (ui_button_suggested(ui, cut_right(&bottom, 120), "Next") && in.disk >= 0) {
			Disk *d = &in.disks[in.disk];
			in.alongside = (d->free_bytes >= MIN_FREE || d->has_zurbax) && d->esp && !str_eq(d->contents, "Empty");
			go(ui, P_HOW);
		}
		break;
	case P_HOW:
		if (ui_button(ui, cut_left(&bottom, 100), "Back"))
			go(ui, P_DISK);
		if (ui_button_suggested(ui, cut_right(&bottom, 120), "Next"))
			go(ui, P_CONFIRM);
		break;
	case P_CONFIRM: {
		if (ui_button(ui, cut_left(&bottom, 100), "Back"))
			go(ui, P_HOW);
		Rect go_r = cut_right(&bottom, 180);
		if (!in.alongside ? ui_button_destructive(ui, go_r, "Erase and Install") : ui_button_suggested(ui, go_r, "Install"))
			start_install(ui);
		break;
	}
	case P_DONE:
		if (ui_button(ui, cut_left(&bottom, 140), "Keep Trying"))
			ui->quit = true;
		if (ui_button_suggested(ui, cut_right(&bottom, 150), "Restart Now"))
			ui_session(ui, SESSION_RESTART);
		break;
	case P_FAILED:
		if (ui_button(ui, cut_left(&bottom, 100), "Close"))
			ui->quit = true;
		if (ui_button_suggested(ui, cut_right(&bottom, 140), "Try Again")) {
			load_disks();
			in.disk = -1;
			go(ui, P_DISK);
		}
		break;
	}
}

static bool on_close(Ui *ui)
{
	UNUSED(ui);
	/* Stopping halfway would leave the disk unusable. */
	return in.page != P_INSTALLING;
}

static void init(Ui *ui)
{
	in.demo = env_get("ZURBAX_INSTALL_DEMO") != NULL;
	in.changed_at = ui->now;
	load_disks();
	/* One disk to choose from (besides the stick): choose it already. */
	int candidates = 0, last = -1;
	for (int i = 0; i < in.ndisks; i++)
		if (!in.disks[i].boot_medium)
			candidates++, last = i;
	if (candidates == 1)
		in.disk = last;
	/* a page to start on, for screenshots */
	const char *p = env_get("ZURBAX_INSTALL_PAGE");
	if (p && in.disk < 0)
		in.disk = 0;
	if (p) {
		int page = CLAMP((int)str_to_int(p, NULL), 0, P_FAILED);
		if (page == P_INSTALLING)
			start_install(ui);
		else
			go(ui, page);
	}
}

int main(void)
{
	UiAppDesc d = { .id = "installer", .title = "Install Zurbax", .width = 1040, .height = 680, .min_width = 860, .min_height = 600,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame, .close = on_close };
	return ui_run(&d);
}
