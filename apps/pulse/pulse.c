/*
 * Pulse — system monitor.
 *
 * Live graphs of processor, memory, network and disk activity, a process
 * list that can end programs, and a summary of the hardware. Everything
 * comes straight from /proc and /sys.
 */
#include "zui.h"

#define HISTORY 90
#define MAX_CPUS 64
#define MAX_PROCS 1024

typedef struct {
	int pid;
	char name[64];
	char user[24];
	u32 uid;
	char state;
	u64 ticks, prev_ticks;
	u64 rss_kb;
	f32 cpu;
	int threads;
	bool seen;
	bool kernel;         /* a kernel thread: no program file */
} Proc;

typedef struct {
	int page;
	/* processor */
	int ncpu;
	u64 prev_total[MAX_CPUS + 1], prev_idle[MAX_CPUS + 1];
	f32 cpu_now[MAX_CPUS + 1];      /* index 0 = all */
	f32 cpu_hist[HISTORY];
	f32 core_hist[MAX_CPUS][HISTORY];
	char cpu_model[96];
	f32 cpu_mhz;
	f32 temp_c;
	/* memory */
	u64 mem_total_kb, mem_avail_kb, swap_total_kb, swap_free_kb, cached_kb;
	f32 mem_hist[HISTORY];
	/* network */
	u64 net_rx, net_tx;
	f32 rx_rate, tx_rate;           /* bytes per second */
	f32 rx_hist[HISTORY], tx_hist[HISTORY];
	/* disk */
	u64 disk_read, disk_write;
	f32 read_rate, write_rate;
	f32 read_hist[HISTORY], write_hist[HISTORY];
	u64 data_total, data_free;
	/* processes */
	Proc procs[MAX_PROCS];
	int nprocs;
	u64 total_ticks_delta;
	int sort_col;
	bool sort_desc;
	StrBuf filter;
	int selected_pid;
	bool show_kernel;
	/* hardware */
	char kernel[96];
	char gpu[128];
	char board[128];
	i64 uptime;
	int battery;
	char battery_state[24];
	f64 last_sample;
	bool first;
} Pulse;

static Pulse pl;

/* ---- Sampling ---------------------------------------------------------------------- */

static void push(f32 *hist, f32 v)
{
	memmove(hist, hist + 1, sizeof(f32) * (HISTORY - 1));
	hist[HISTORY - 1] = v;
}

static void sample_cpu(void)
{
	char *stat = file_read("/proc/stat", NULL);
	if (!stat)
		return;
	usize n;
	char **lines = str_split(stat, '\n', &n);
	int ncpu = 0;
	for (usize i = 0; i < n; i++) {
		const char *l = lines[i];
		if (!str_starts(l, "cpu"))
			break;
		int idx = 0;
		const char *p = l + 3;
		if (*p != ' ') {
			idx = (int)str_to_int(p, &p) + 1;
			ncpu = MAX(ncpu, idx);
		}
		if (idx > MAX_CPUS)
			continue;
		u64 v[10] = { 0 };
		for (int k = 0; k < 10 && *p; k++)
			v[k] = (u64)str_to_int(p, &p);
		u64 idle = v[3] + v[4], total = 0;
		for (int k = 0; k < 8; k++)
			total += v[k];
		u64 dt = total - pl.prev_total[idx], di = idle - pl.prev_idle[idx];
		pl.cpu_now[idx] = dt ? clampf(1 - (f32)di / (f32)dt, 0, 1) : 0;
		if (idx == 0)
			pl.total_ticks_delta = dt;
		pl.prev_total[idx] = total;
		pl.prev_idle[idx] = idle;
	}
	str_split_free(lines, n);
	mem_free(stat);
	pl.ncpu = MIN(ncpu, MAX_CPUS);
	push(pl.cpu_hist, pl.cpu_now[0]);
	for (int c = 0; c < pl.ncpu; c++)
		push(pl.core_hist[c], pl.cpu_now[c + 1]);
	pl.cpu_mhz = (f32)file_read_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 0) / 1000.0f;
	/* The hottest thermal zone reported as a CPU sensor, or any zone. */
	f32 best = 0;
	for (int z = 0; z < 16; z++) {
		char path[96];
		fmt(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
		i64 t = file_read_int(path, -1);
		if (t < 0)
			continue;
		best = maxf(best, (f32)t / 1000.0f);
	}
	pl.temp_c = best;
}

static u64 meminfo_value(const char *text, const char *key)
{
	const char *p = str_find(text, key);
	if (!p)
		return 0;
	p += strlen(key);
	while (*p == ' ' || *p == ':')
		p++;
	return (u64)str_to_int(p, NULL);
}

static void sample_memory(void)
{
	char *m = file_read("/proc/meminfo", NULL);
	if (!m)
		return;
	pl.mem_total_kb = meminfo_value(m, "MemTotal");
	pl.mem_avail_kb = meminfo_value(m, "MemAvailable");
	pl.swap_total_kb = meminfo_value(m, "SwapTotal");
	pl.swap_free_kb = meminfo_value(m, "SwapFree");
	pl.cached_kb = meminfo_value(m, "\nCached");
	mem_free(m);
	push(pl.mem_hist, pl.mem_total_kb ? 1 - (f32)pl.mem_avail_kb / (f32)pl.mem_total_kb : 0);
}

static void sample_network(f32 dt)
{
	char *d = file_read("/proc/net/dev", NULL);
	if (!d)
		return;
	u64 rx = 0, tx = 0;
	usize n;
	char **lines = str_split(d, '\n', &n);
	for (usize i = 2; i < n; i++) {
		char *colon = (char *)str_chr(lines[i], ':');
		if (!colon)
			continue;
		*colon = 0;
		if (str_eq(str_trim(lines[i]), "lo"))
			continue;
		const char *p = colon + 1;
		u64 v[16] = { 0 };
		for (int k = 0; k < 16 && *p; k++)
			v[k] = (u64)str_to_int(p, &p);
		rx += v[0];
		tx += v[8];
	}
	str_split_free(lines, n);
	mem_free(d);
	if (!pl.first && dt > 0) {
		pl.rx_rate = (f32)(rx - pl.net_rx) / dt;
		pl.tx_rate = (f32)(tx - pl.net_tx) / dt;
	}
	pl.net_rx = rx;
	pl.net_tx = tx;
	push(pl.rx_hist, pl.rx_rate);
	push(pl.tx_hist, pl.tx_rate);
}

static void sample_disk(f32 dt)
{
	char *d = file_read("/proc/diskstats", NULL);
	if (!d)
		return;
	u64 rd = 0, wr = 0;
	usize n;
	char **lines = str_split(d, '\n', &n);
	for (usize i = 0; i < n; i++) {
		const char *p = lines[i];
		str_to_int(p, &p);
		str_to_int(p, &p);
		while (*p == ' ')
			p++;
		char name[32];
		usize k = 0;
		while (*p && *p != ' ' && k < sizeof name - 1)
			name[k++] = *p++;
		name[k] = 0;
		/* Whole disks only: nvme0n1, sda, mmcblk0 — not partitions or loops. */
		bool whole = (str_starts(name, "nvme") && !str_chr(name + 5, 'p')) || (str_starts(name, "sd") && strlen(name) == 3) ||
			     (str_starts(name, "mmcblk") && !str_chr(name + 6, 'p')) || (str_starts(name, "vd") && strlen(name) == 3);
		if (!whole)
			continue;
		u64 v[11] = { 0 };
		for (int j = 0; j < 11 && *p; j++)
			v[j] = (u64)str_to_int(p, &p);
		rd += v[2] * 512;
		wr += v[6] * 512;
	}
	str_split_free(lines, n);
	mem_free(d);
	if (!pl.first && dt > 0) {
		pl.read_rate = (f32)(rd - pl.disk_read) / dt;
		pl.write_rate = (f32)(wr - pl.disk_write) / dt;
	}
	pl.disk_read = rd;
	pl.disk_write = wr;
	push(pl.read_hist, pl.read_rate);
	push(pl.write_hist, pl.write_rate);
	struct statfs st;
	const char *data = zurbax_data();
	if (sys_statfs(data, &st) == 0 || sys_statfs("/", &st) == 0) {
		pl.data_total = st.f_blocks * (u64)st.f_bsize;
		pl.data_free = st.f_bavail * (u64)st.f_bsize;
	}
}

static Proc *find_proc(int pid)
{
	for (int i = 0; i < pl.nprocs; i++)
		if (pl.procs[i].pid == pid)
			return &pl.procs[i];
	return NULL;
}

static void user_name(u32 uid, char *out, usize cap)
{
	if (uid == 0)
		str_copy(out, "system", cap);
	else if (uid == 1000)
		str_copy(out, env_get("USER") ? env_get("USER") : "user", cap);
	else
		fmt(out, cap, "%u", uid);
}

static void sample_procs(void)
{
	for (int i = 0; i < pl.nprocs; i++)
		pl.procs[i].seen = false;
	DirList list = { 0 };
	if (!dir_list("/proc", &list, false))
		return;
	vec_each(&list, e) {
		if (!is_digit((u8)e->name[0]))
			continue;
		int pid = (int)str_to_int(e->name, NULL);
		char path[64];
		fmt(path, sizeof path, "/proc/%d/stat", pid);
		char *st = file_read(path, NULL);
		if (!st)
			continue;
		/* "pid (comm) S ppid ..." — comm may contain spaces and parentheses. */
		const char *open = str_chr(st, '('), *close = str_rchr(st, ')');
		if (!open || !close) {
			mem_free(st);
			continue;
		}
		Proc *p = find_proc(pid);
		if (!p) {
			if (pl.nprocs >= MAX_PROCS) {
				mem_free(st);
				continue;
			}
			p = &pl.procs[pl.nprocs++];
			memset(p, 0, sizeof *p);
			p->pid = pid;
		}
		p->seen = true;
		usize nl = MIN((usize)(close - open - 1), sizeof p->name - 1);
		memcpy(p->name, open + 1, nl);
		p->name[nl] = 0;
		const char *q = close + 2;
		p->state = *q;
		/* Fields after the state: skip to utime (14) and stime (15), threads (20), rss (24). */
		u64 f[24] = { 0 };
		q++;
		for (int k = 0; k < 22 && *q; k++)
			f[k] = (u64)str_to_int(q, &q);
		u64 ticks = f[10] + f[11];
		p->prev_ticks = p->ticks ? p->ticks : ticks;
		p->ticks = ticks;
		p->threads = (int)f[16];
		p->rss_kb = f[20] * 4;
		p->cpu = pl.total_ticks_delta ? (f32)(p->ticks - p->prev_ticks) / (f32)pl.total_ticks_delta * (f32)MAX(pl.ncpu, 1) : 0;
		p->cpu = minf(p->cpu, (f32)MAX(pl.ncpu, 1));
		mem_free(st);
		fmt(path, sizeof path, "/proc/%d/status", pid);
		char *status = file_read(path, NULL);
		if (status) {
			const char *u = str_find(status, "Uid:");
			if (u)
				p->uid = (u32)str_to_int(u + 4, NULL);
			mem_free(status);
		}
		user_name(p->uid, p->user, sizeof p->user);
		/* Kernel threads have no command line; prefer the program's file name. */
		fmt(path, sizeof path, "/proc/%d/cmdline", pid);
		char *cmd = file_read(path, NULL);
		p->kernel = !cmd || !cmd[0];
		if (cmd && cmd[0]) {
			const char *base = path_base(cmd);
			if (*base)
				str_copy(p->name, base, sizeof p->name);
		}
		mem_free(cmd);
	}
	dir_list_free(&list);
	/* Drop processes that exited. */
	int w = 0;
	for (int i = 0; i < pl.nprocs; i++)
		if (pl.procs[i].seen)
			pl.procs[w++] = pl.procs[i];
	pl.nprocs = w;
}

static void sample_hardware(void)
{
	struct utsname u;
	if (sys_uname(&u) == 0)
		fmt(pl.kernel, sizeof pl.kernel, "Linux %s", u.release);
	char *ci = file_read("/proc/cpuinfo", NULL);
	if (ci) {
		const char *m = str_find(ci, "model name");
		if (m) {
			m = str_chr(m, ':');
			if (m) {
				m += 2;
				const char *e = str_chr(m, '\n');
				usize n = e ? (usize)(e - m) : strlen(m);
				fmt(pl.cpu_model, sizeof pl.cpu_model, "%.*s", (int)n, m);
			}
		}
		mem_free(ci);
	}
	char *vendor = file_read_line("/sys/class/dmi/id/sys_vendor");
	char *product = file_read_line("/sys/class/dmi/id/product_name");
	if (product)
		fmt(pl.board, sizeof pl.board, "%s %s", vendor ? vendor : "", product);
	mem_free(vendor);
	mem_free(product);
	/* The GPU driving the display, from the DRM device's driver. */
	for (int i = 0; i < 4 && !pl.gpu[0]; i++) {
		char path[96], link[256];
		fmt(path, sizeof path, "/sys/class/drm/card%d/device/driver", i);
		long n = sys_readlink(path, link, sizeof link - 1);
		if (n > 0) {
			link[n] = 0;
			const char *drv = path_base(link);
			const char *name = str_eq(drv, "i915") || str_eq(drv, "xe") ? "Intel Graphics" :
					   str_eq(drv, "amdgpu") ? "AMD Radeon Graphics" :
					   str_eq(drv, "nouveau") ? "NVIDIA Graphics" : drv;
			fmt(pl.gpu, sizeof pl.gpu, "%s", name);
		}
	}
	struct sysinfo si;
	if (sys_sysinfo(&si) == 0)
		pl.uptime = si.uptime;
	pl.battery = -1;
	DirList ps = { 0 };
	if (dir_list("/sys/class/power_supply", &ps, false)) {
		vec_each(&ps, e) {
			char path[128];
			fmt(path, sizeof path, "/sys/class/power_supply/%s/capacity", e->name);
			i64 cap = file_read_int(path, -1);
			if (cap >= 0) {
				pl.battery = (int)cap;
				fmt(path, sizeof path, "/sys/class/power_supply/%s/status", e->name);
				char *s = file_read_line(path);
				str_copy(pl.battery_state, s ? s : "", sizeof pl.battery_state);
				mem_free(s);
			}
		}
		dir_list_free(&ps);
	}
}

static void sample(Ui *ui)
{
	f32 dt = pl.last_sample > 0 ? (f32)(ui->now - pl.last_sample) : 1;
	pl.last_sample = ui->now;
	sample_cpu();
	sample_memory();
	sample_network(dt);
	sample_disk(dt);
	sample_procs();
	if (pl.first)
		sample_hardware();
	else {
		struct sysinfo si;
		if (sys_sysinfo(&si) == 0)
			pl.uptime = si.uptime;
	}
	pl.first = false;
}

/* ---- Formatting -------------------------------------------------------------------- */

static void human_bytes(f64 b, char *out, usize cap)
{
	const char *units[] = { "B", "kB", "MB", "GB", "TB" };
	int u = 0;
	while (b >= 1000 && u < 4) {
		b /= 1024;
		u++;
	}
	fmt(out, cap, u == 0 ? "%.0f %s" : b < 10 ? "%.1f %s" : "%.0f %s", b, units[u]);
}

/* ---- Drawing ----------------------------------------------------------------------- */

static void graph(Ui *ui, Rect r, const f32 *hist, f32 max, Color c, bool fill)
{
	Theme *t = &ui->theme;
	ui_round(ui, r, 6, t->dark ? color_hex("#00000030") : color_hex("#00000008"));
	for (int i = 1; i < 4; i++)
		ui_line(ui, r.x, r.y + r.h * (f32)i / 4, r.x + r.w, r.y + r.h * (f32)i / 4, 1, t->border);
	if (max <= 0)
		max = 1;
	Path p = { 0 }, a = { 0 };
	f32 step = r.w / (HISTORY - 1);
	path_move(&a, r.x, r.y + r.h);
	for (int i = 0; i < HISTORY; i++) {
		f32 x = r.x + step * (f32)i, y = r.y + r.h - clampf(hist[i] / max, 0, 1) * (r.h - 2) - 1;
		if (i == 0)
			path_move(&p, x, y);
		else
			path_line(&p, x, y);
		path_line(&a, x, y);
	}
	if (fill) {
		path_line(&a, r.x + r.w, r.y + r.h);
		path_close(&a);
		Rect d = ui_to_device(ui, r);
		Paint g = paint_linear(0, d.y, 0, d.y + d.h, color_with_alpha(c, 0x70), color_with_alpha(c, 0x08));
		fill_path(ui->canvas, &a, mat_scale(ui->scale, ui->scale), &g, FILL_NONZERO);
	}
	path_free(&a);
	ui_path_stroke(ui, &p, 1.6f, c);
	path_free(&p);
}

static void stat_card(Ui *ui, Rect r, const char *icon, const char *title, const char *value, const char *sub, const f32 *hist,
		      f32 max, Color c, const f32 *hist2, Color c2)
{
	Theme *t = &ui->theme;
	ui_card(ui, r);
	Rect in = pad(r, 16);
	Rect head = cut_top(&in, 24);
	ui_round(ui, rect(head.x, head.y, 24, 24), 6, color_with_alpha(c, 0x30));
	ui_icon(ui, rect(head.x + 4, head.y + 4, 16, 16), icon, c);
	ui_text(ui, ui_font(ui, 13, 600), head.x + 34, head.y + 17, title, t->text_dim);
	Font vf = ui_font(ui, 24, 300);
	ui_text(ui, vf, in.x, in.y + 32, value, t->text);
	ui_text(ui, ui_font(ui, 12, 400), in.x, in.y + 52, sub, t->text_dim);
	Rect g = pad4(in, 62, 0, 0, 0);
	if (hist2)
		graph(ui, g, hist2, max, c2, false);
	graph(ui, g, hist, max, c, true);
}

static void page_overview(Ui *ui, Rect r)
{
	char v[64], sub[128];
	Rect body = pad(r, 18);
	f32 gap = 14;
	f32 cw = (body.w - gap) / 2, ch = (body.h - gap) / 2;
	Color blue = color_hex("#3689e6"), green = color_hex("#68b723"), orange = color_hex("#f37329"), purple = color_hex("#a56de2");
	Color teal = color_hex("#28bca3"), pink = color_hex("#de3e80");

	fmt(v, sizeof v, "%d%%", (int)roundf(pl.cpu_now[0] * 100));
	if (pl.temp_c > 0)
		fmt(sub, sizeof sub, "%d cores · %.1f GHz · %d °C", pl.ncpu, (double)(pl.cpu_mhz / 1000), (int)pl.temp_c);
	else
		fmt(sub, sizeof sub, "%d cores%s", pl.ncpu, pl.cpu_mhz > 0 ? "" : "");
	stat_card(ui, rect(body.x, body.y, cw, ch), "cpu", "Processor", v, sub, pl.cpu_hist, 1, blue, NULL, 0);

	char used[32], total[32];
	human_bytes((f64)(pl.mem_total_kb - pl.mem_avail_kb) * 1024, used, sizeof used);
	human_bytes((f64)pl.mem_total_kb * 1024, total, sizeof total);
	fmt(v, sizeof v, "%s", used);
	fmt(sub, sizeof sub, "of %s in use · %d%%", total, (int)roundf(pl.mem_hist[HISTORY - 1] * 100));
	stat_card(ui, rect(body.x + cw + gap, body.y, cw, ch), "memory", "Memory", v, sub, pl.mem_hist, 1, purple, NULL, 0);

	f32 nmax = 1024;
	for (int i = 0; i < HISTORY; i++)
		nmax = maxf(nmax, maxf(pl.rx_hist[i], pl.tx_hist[i]));
	char rx[32], tx[32];
	human_bytes(pl.rx_rate, rx, sizeof rx);
	human_bytes(pl.tx_rate, tx, sizeof tx);
	fmt(v, sizeof v, "↓ %s/s", rx);
	fmt(sub, sizeof sub, "↑ %s/s sent", tx);
	stat_card(ui, rect(body.x, body.y + ch + gap, cw, ch), "network-wireless", "Network", v, sub, pl.rx_hist, nmax * 1.15f, teal,
		  pl.tx_hist, orange);

	f32 dmax = 1024 * 64;
	for (int i = 0; i < HISTORY; i++)
		dmax = maxf(dmax, maxf(pl.read_hist[i], pl.write_hist[i]));
	char rd[32], wrs[32], free_s[32];
	human_bytes(pl.read_rate, rd, sizeof rd);
	human_bytes(pl.write_rate, wrs, sizeof wrs);
	human_bytes((f64)pl.data_free, free_s, sizeof free_s);
	fmt(v, sizeof v, "%s/s", rd);
	fmt(sub, sizeof sub, "%s/s written · %s free", wrs, free_s);
	stat_card(ui, rect(body.x + cw + gap, body.y + ch + gap, cw, ch), "drive", "Storage", v, sub, pl.read_hist, dmax * 1.15f,
		  green, pl.write_hist, pink);
}

static int cmp_procs(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	const Proc *x = a, *y = b;
	int r = 0;
	switch (pl.sort_col) {
	case 0: r = str_cmp_nocase(x->name, y->name); break;
	case 1: r = x->cpu < y->cpu ? -1 : x->cpu > y->cpu; break;
	case 2: r = x->rss_kb < y->rss_kb ? -1 : x->rss_kb > y->rss_kb; break;
	case 3: r = x->pid - y->pid; break;
	case 4: r = str_cmp(x->user, y->user); break;
	}
	return pl.sort_desc ? -r : r;
}

static void page_processes(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect bar = cut_top(&r, 50);
	Rect b = pad2(bar, 14, 9);
	ui_entry(ui, cut_left(&b, 260), "filter", &pl.filter, "Search processes", ENTRY_SEARCH);
	Proc *sel = find_proc(pl.selected_pid);
	if (sel) {
		if (ui_button_destructive(ui, cut_right(&b, 120), "End Process")) {
			sys_kill(sel->pid, SIGTERM);
			pl.selected_pid = 0;
		}
		cut_right(&b, 8);
		if (ui_button(ui, cut_right(&b, 90), "Force Quit")) {
			sys_kill(sel->pid, SIGKILL);
			pl.selected_pid = 0;
		}
	}
	int visible = 0;
	for (int i = 0; i < pl.nprocs; i++)
		visible += pl.show_kernel || !pl.procs[i].kernel;
	char count[48];
	fmt(count, sizeof count, "%d processes", visible);
	cut_left(&b, 14);
	ui_checkbox(ui, cut_left(&b, 190), "Include kernel threads", &pl.show_kernel);
	ui_label_dim(ui, b, count);

	sort(pl.procs, (usize)pl.nprocs, sizeof(Proc), cmp_procs, NULL);
	Rect head = cut_top(&r, 30);
	ui_fill(ui, head, t->header_bottom);
	ui_separator(ui, head.x, head.y + head.h, head.x + head.w);
	static const char *const cols[] = { "Name", "CPU", "Memory", "PID", "User" };
	f32 cx[6] = { head.x + 16, head.x + head.w - 400, head.x + head.w - 310, head.x + head.w - 200, head.x + head.w - 120, head.x + head.w };
	for (int i = 0; i < 5; i++) {
		Rect cr = rect(cx[i] - 6, head.y, cx[i + 1] - cx[i], head.h);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "col"), cr);
		ui_pop_id(ui);
		if (it.hovered)
			ui_fill(ui, cr, t->hover);
		Font f = ui_font(ui, 12, pl.sort_col == i ? 700 : 600);
		f32 x = ui_text(ui, f, cx[i], head.y + 19, cols[i], pl.sort_col == i ? t->text : t->text_dim);
		if (pl.sort_col == i)
			ui_icon(ui, rect(x + 4, head.y + 9, 12, 12), pl.sort_desc ? "pan-down" : "pan-up", t->text_dim);
		if (it.clicked) {
			if (pl.sort_col == i)
				pl.sort_desc = !pl.sort_desc;
			else {
				pl.sort_col = i;
				pl.sort_desc = i == 1 || i == 2;
			}
		}
	}
	ui_fill(ui, r, t->view);
	const char *q = sb_str(&pl.filter);
	f32 y = ui_scroll_begin(ui, r, "procs");
	for (int i = 0; i < pl.nprocs; i++) {
		Proc *p = &pl.procs[i];
		if ((*q && !str_find_nocase(p->name, q)) || (p->kernel && !pl.show_kernel))
			continue;
		Rect row = rect(r.x, y, r.w, 30);
		y += 30;
		ui_push_id(ui, p->pid);
		UiInteract it = ui_row(ui, row, ui_id(ui, "p"), p->pid == pl.selected_pid);
		ui_pop_id(ui);
		if (it.clicked)
			pl.selected_pid = p->pid;
		Font f = ui_font(ui, 13, 400);
		ui_icon(ui, rect(cx[0], row.y + 7, 16, 16), p->uid == 0 ? "settings" : "apps", t->text_faint);
		ui_push_clip(ui, rect(cx[0], row.y, cx[1] - cx[0] - 10, row.h));
		ui_text(ui, f, cx[0] + 24, row.y + 20, p->name, t->text);
		ui_pop_clip(ui);
		char v[32];
		fmt(v, sizeof v, "%.1f%%", (double)(p->cpu * 100 / (f32)MAX(pl.ncpu, 1)));
		ui_text(ui, f, cx[1], row.y + 20, v, p->cpu > 0.5f ? t->warning : t->text);
		human_bytes((f64)p->rss_kb * 1024, v, sizeof v);
		ui_text(ui, f, cx[2], row.y + 20, p->rss_kb ? v : "—", t->text);
		fmt(v, sizeof v, "%d", p->pid);
		ui_text(ui, f, cx[3], row.y + 20, v, t->text_dim);
		ui_text(ui, f, cx[4], row.y + 20, p->user, t->text_dim);
	}
	ui_scroll_end(ui, y - r.y);
}

static void info_row(Ui *ui, Rect *area, const char *label, const char *value)
{
	Theme *t = &ui->theme;
	Rect row = cut_top(area, 40);
	ui_text(ui, ui_font(ui, 13, 500), row.x, row.y + 25, label, t->text_dim);
	ui_text(ui, ui_font(ui, 13, 500), row.x + 170, row.y + 25, value, t->text);
	ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
}

static void page_hardware(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect body = pad(r, 24);
	body.w = minf(body.w, 640);
	body.x = r.x + (r.w - body.w) / 2;
	Rect top = cut_top(&body, 110);
	ui_app_icon(ui, rect(top.x, top.y + 10, 80, 80), "zurbax");
	ui_text(ui, ui_font(ui, 26, 300), top.x + 100, top.y + 45, pl.board[0] ? pl.board : "This Computer", t->text);
	ui_text(ui, ui_font(ui, 13, 400), top.x + 100, top.y + 70, "Zurbax 1.0", t->text_dim);
	Rect card = cut_top(&body, 40 * 7 + 16);
	ui_card(ui, card);
	Rect in = pad2(card, 18, 8);
	char buf[128], tot[32];
	info_row(ui, &in, "Processor", pl.cpu_model[0] ? pl.cpu_model : "Unknown");
	fmt(buf, sizeof buf, "%d", pl.ncpu);
	info_row(ui, &in, "Cores", buf);
	human_bytes((f64)pl.mem_total_kb * 1024, tot, sizeof tot);
	info_row(ui, &in, "Memory", tot);
	info_row(ui, &in, "Graphics", pl.gpu[0] ? pl.gpu : "Firmware framebuffer");
	char free_s[32];
	human_bytes((f64)pl.data_total, tot, sizeof tot);
	human_bytes((f64)pl.data_free, free_s, sizeof free_s);
	fmt(buf, sizeof buf, "%s (%s free)", tot, free_s);
	info_row(ui, &in, "Storage", buf);
	info_row(ui, &in, "Kernel", pl.kernel);
	i64 up = pl.uptime;
	if (up >= 86400)
		fmt(buf, sizeof buf, "%lld days, %lld h %lld min", (long long)(up / 86400), (long long)(up % 86400 / 3600), (long long)(up % 3600 / 60));
	else
		fmt(buf, sizeof buf, "%lld h %lld min", (long long)(up / 3600), (long long)(up % 3600 / 60));
	info_row(ui, &in, "Running for", buf);
	if (pl.battery >= 0) {
		cut_top(&body, 14);
		Rect bc = cut_top(&body, 70);
		ui_card(ui, bc);
		Rect bi = pad(bc, 16);
		ui_text(ui, ui_font(ui, 13, 600), bi.x, bi.y + 14, "Battery", t->text);
		fmt(buf, sizeof buf, "%d%% · %s", pl.battery, pl.battery_state);
		ui_text(ui, ui_font(ui, 13, 400), bi.x + bi.w - ui_text_width(ui, ui_font(ui, 13, 400), buf), bi.y + 14, buf, t->text_dim);
		ui_progress(ui, rect(bi.x, bi.y + 26, bi.w, 8), (f32)pl.battery / 100);
	}
}

static void tick(Ui *ui)
{
	sample(ui);
	ui_request_frame(ui);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	Rect tools = ui_headerbar(ui, header, "Pulse", HEADER_NO_TITLE);
	static const char *const pages[] = { "Overview", "Processes", "Hardware" };
	ui_segmented(ui, center_in(tools, 330, 30), pages, 3, &pl.page);
	ui_fill(ui, win, t->window);
	switch (pl.page) {
	case 0: page_overview(ui, win); break;
	case 1: page_processes(ui, win); break;
	case 2: page_hardware(ui, win); break;
	}
}

static void init(Ui *ui)
{
	pl.first = true;
	pl.sort_col = 1;
	pl.sort_desc = true;
	sample(ui);
}

int main(void)
{
	UiAppDesc d = { .id = "pulse", .title = "Pulse", .width = 900, .height = 640, .min_width = 700, .min_height = 480,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame, .tick = tick, .tick_ms = 1000 };
	return ui_run(&d);
}
