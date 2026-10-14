/*
 * Bridge — Windows programs and games on Zurbax.
 *
 * Inside it's Proton, in its GE build (the community's, with the newest
 * fixes), run in Valve's Steam Linux Runtime the way Steam runs it, through
 * layerrun in the Linux layer with an Xwayland of its own. DirectX goes
 * through DXVK and VKD3D-Proton to Vulkan, synchronization through the
 * kernel's ntsync, and the anti-cheat runtimes Steam installs (Easy
 * Anti-Cheat, BattlEye) are passed on for games whose makers allow Linux.
 * Every program gets a bottle of its own: a Windows folder tree, registry
 * and settings, so one can't break another.
 *
 * Proton and the runtime are downloaded the first time (about 900 MB), each
 * checked against its published checksum.
 *
 * In ~/.local/share/zurbax/bridge: apps/ID.conf (the library), bottles/ID,
 * proton/GE-Proton*, runtime/SteamLinuxRuntime_sniper.
 *
 *     bridge [FILE.exe]      opens Bridge, and runs FILE in its bottle
 */
#include "zui.h"
#include "zn.h"

#define RUNTIME_URL "https://repo.steampowered.com/steamrt3/images/latest-container-runtime-public-beta/"
#define RUNTIME_FILE "SteamLinuxRuntime_sniper.tar.xz"
#define PROTON_API "https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/latest"

typedef struct {
	char id[48];
	char name[96];
	char exe[512];
	char args[256];
	bool fsr, hud, nvapi, wayland;
	i64 played;
} App;

static App apps[128];
static int napps;
static char base[400];
static int sel = -1;
static StrBuf name_edit;
static char pending_run[512];   /* a file to run once Proton is ready */
static char message[200];

/* the download, on a thread of its own */
static struct {
	Mutex lock;
	int busy;
	char step[120];
	f32 progress;
	char error[200];
} job;

/* what's playing */
static int play_pid = -1;
static char play_id[48];
/* an installer's bottle, before it ran: new programs in it are offered after */
static char **before_exes;
static usize nbefore;
static char install_id[48];
static char found[8][512];
static int nfound;

/* ---- Places ------------------------------------------------------------------------------- */

static char *path_in(const char *rel)
{
	return str_fmt("%s/%s", base, rel);
}

/* The Linux layer, where Proton runs: /data/layers/steam on Zurbax. */
static bool have_layer(void)
{
	char *p = str_fmt("%s/layers/steam/usr/bin/tar", zurbax_data());
	bool ok = file_exists(p);
	mem_free(p);
	return ok;
}

static bool have_runtime(void)
{
	char *p = path_in("runtime/SteamLinuxRuntime_sniper/_v2-entry-point");
	bool ok = file_exists(p);
	mem_free(p);
	return ok;
}

/* The newest Proton downloaded: proton/GE-ProtonN-M with its "proton" script. */
static bool proton_dir(char *out, usize cap)
{
	char *dir = path_in("proton");
	DirList l = { 0 };
	out[0] = 0;
	if (dir_list(dir, &l, false)) {
		vec_each(&l, e)
		{
			char *script = str_fmt("%s/%s/proton", dir, e->name);
			if (file_exists(script) && (!out[0] || str_cmp_natural(path_base(out), e->name) < 0))
				fmt(out, cap, "%s/%s", dir, e->name);
			mem_free(script);
		}
		dir_list_free(&l);
	}
	mem_free(dir);
	return out[0] != 0;
}

/* Steam's anti-cheat runtimes, when Steam has installed them. */
static bool steam_runtime(const char *name, char *out, usize cap)
{
	fmt(out, cap, "%s/.local/share/Steam/steamapps/common/%s", home_dir(), name);
	return dir_exists(out);
}

/* ---- The library ----------------------------------------------------------------------- */

static void save_app(const App *a)
{
	char *path = str_fmt("%s/apps/%s.conf", base, a->id);
	Conf *c = conf_load(path);
	conf_set(c, "name", a->name);
	conf_set(c, "exe", a->exe);
	conf_set(c, "args", a->args);
	conf_set_bool(c, "fsr", a->fsr);
	conf_set_bool(c, "fps", a->hud);
	conf_set_bool(c, "nvapi", a->nvapi);
	conf_set_bool(c, "wayland", a->wayland);
	conf_set_int(c, "played", a->played);
	conf_save(c);
	conf_free(c);
	mem_free(path);
}

static int by_recent(const void *x, const void *y, void *ctx)
{
	const App *a = x, *b = y;
	UNUSED(ctx);
	if (a->played != b->played)
		return a->played > b->played ? -1 : 1;
	return str_cmp_nocase(a->name, b->name);
}

static void load_library(void)
{
	napps = 0;
	char *dir = path_in("apps");
	DirList l = { 0 };
	if (dir_list(dir, &l, false)) {
		vec_each(&l, e)
		{
			if (!str_ends(e->name, ".conf") || napps >= (int)ARRAY_LEN(apps))
				continue;
			char *path = str_fmt("%s/%s", dir, e->name);
			Conf *c = conf_load(path);
			App *a = &apps[napps];
			memset(a, 0, sizeof *a);
			fmt(a->id, sizeof a->id, "%.*s", (int)(strlen(e->name) - 5), e->name);
			str_copy(a->name, conf_get(c, "name", a->id), sizeof a->name);
			str_copy(a->exe, conf_get(c, "exe", ""), sizeof a->exe);
			str_copy(a->args, conf_get(c, "args", ""), sizeof a->args);
			a->fsr = conf_get_bool(c, "fsr", false);
			a->hud = conf_get_bool(c, "fps", false);
			a->nvapi = conf_get_bool(c, "nvapi", true);
			a->wayland = conf_get_bool(c, "wayland", false);
			a->played = conf_get_int(c, "played", 0);
			if (a->exe[0])
				napps++;
			conf_free(c);
			mem_free(path);
		}
		dir_list_free(&l);
	}
	mem_free(dir);
	sort(apps, (usize)napps, sizeof apps[0], by_recent, NULL);
}

/* "Some Game.exe" becomes "Some Game"; a setup program is named after its folder. */
static void name_from(const char *exe, char *out, usize cap)
{
	const char *b = path_base(exe);
	const char *dot = str_rchr(b, '.');
	fmt(out, cap, "%.*s", (int)(dot ? dot - b : (long)strlen(b)), b);
	for (char *p = out; *p; p++)
		if (*p == '_' || *p == '-')
			*p = ' ';
}

static int find_app(const char *exe)
{
	for (int i = 0; i < napps; i++)
		if (str_eq(apps[i].exe, exe))
			return i;
	return -1;
}

static int add_app(const char *exe, const char *bottle_of)
{
	int i = find_app(exe);
	if (i >= 0)
		return i;
	App a = { 0 };
	str_copy(a.exe, exe, sizeof a.exe);
	name_from(exe, a.name, sizeof a.name);
	a.nvapi = true;
	if (bottle_of) {
		/* a program an installer put in its bottle stays in that bottle */
		str_copy(a.id, bottle_of, sizeof a.id);
		fmt(a.id + strlen(a.id), sizeof a.id - strlen(a.id), "-%u", (u32)(time_unix() % 100000));
	} else {
		usize n = 0;
		for (const char *p = a.name; *p && n + 10 < sizeof a.id; p++)
			if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
				a.id[n++] = *p;
			else if (*p >= 'A' && *p <= 'Z')
				a.id[n++] = (char)(*p + 32);
		fmt(a.id + n, sizeof a.id - n, "-%u", (u32)(time_unix() % 100000));
	}
	save_app(&a);
	load_library();
	return find_app(exe);
}

/* Which bottle an app lives in: an installed program shares its installer's. */
static void bottle_of(const App *a, char *out, usize cap)
{
	char id[48];
	str_copy(id, a->id, sizeof id);
	char *path = str_fmt("%s/apps/%s.conf", base, a->id);
	Conf *c = conf_load(path);
	const char *b = conf_get(c, "bottle", NULL);
	if (b)
		str_copy(id, b, sizeof id);
	conf_free(c);
	mem_free(path);
	fmt(out, cap, "%s/bottles/%s", base, id);
}

/* ---- Getting Proton and the runtime ------------------------------------------------------- */

static void job_step(const char *step, f32 progress)
{
	mutex_lock(&job.lock);
	if (step)
		str_copy(job.step, step, sizeof job.step);
	job.progress = progress;
	mutex_unlock(&job.lock);
}

static void job_fail(const char *msg)
{
	mutex_lock(&job.lock);
	str_copy(job.error, msg, sizeof job.error);
	mutex_unlock(&job.lock);
}

typedef struct {
	int fd;
	Sha256 s256;
	Sha512 s512;
	u64 got, total;
	f32 from, to;
} Dl;

static bool dl_sink(void *user, const u8 *data, usize n)
{
	Dl *d = user;
	sha256_update(&d->s256, data, n);
	sha512_update(&d->s512, data, n);
	if (!write_all(d->fd, data, n))
		return false;
	d->got += n;
	if (d->total)
		job_step(NULL, d->from + (d->to - d->from) * (f32)((f64)d->got / (f64)d->total));
	return true;
}

static void dl_progress(void *user, usize got, usize total)
{
	Dl *d = user;
	UNUSED(got);
	if (total)
		d->total = total;
}

/* url to path; the SHA-256 or SHA-512 of what arrived in sum (hex) */
static bool download(const char *url, const char *path, f32 from, f32 to, char *sum256, char *sum512)
{
	char *part = str_fmt("%s.part", path);
	Dl d = { .from = from, .to = to };
	d.fd = (int)sys_open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (d.fd < 0) {
		mem_free(part);
		return false;
	}
	sha256_init(&d.s256);
	sha512_init(&d.s512);
	HttpOptions o = { .timeout_ms = 60000, .sink = dl_sink, .progress = dl_progress, .user = &d };
	HttpResponse *r = http_request(url, &o);
	bool ok = r->status == 200 && d.got > 0;
	http_response_free(r);
	sys_close(d.fd);
	u8 h256[32], h512[64];
	sha256_final(&d.s256, h256);
	sha512_final(&d.s512, h512);
	if (sum256)
		hex_encode(h256, 32, sum256);
	if (sum512)
		hex_encode(h512, 64, sum512);
	if (ok)
		ok = sys_rename(part, path) == 0;
	else
		sys_unlink(part);
	mem_free(part);
	return ok;
}

/* tar, in the Linux layer (Zurbax has none of its own) */
static bool unpack(const char *archive, const char *dir)
{
	dir_make(dir, 0755);
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[] = { layerrun, "tar", "-x", "-f", (char *)archive, "-C", (char *)dir, NULL };
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1 };
	int pid = proc_spawn(layerrun, argv, environ, &o);
	mem_free(layerrun);
	int status = -1;
	if (pid > 0)
		proc_wait(pid, &status, false);
	return status == 0;
}

static bool get_runtime(void)
{
	job_step("Valve's Steam Linux Runtime: checking", 0.02f);
	HttpResponse *r = http_get(RUNTIME_URL "SHA256SUMS");
	char want[65] = "";
	if (r->status == 200 && r->body) {
		char *text = str_ndup((const char *)r->body, r->body_len);
		for (char *line = text; line && *line;) {
			char *nl = (char *)str_chr(line, '\n');
			if (nl)
				*nl = 0;
			if (str_find(line, RUNTIME_FILE) && strlen(line) > 64)
				fmt(want, sizeof want, "%.64s", line);
			line = nl ? nl + 1 : NULL;
		}
		mem_free(text);
	}
	http_response_free(r);
	if (!want[0]) {
		job_fail("Valve's download server couldn't be reached.");
		return false;
	}
	job_step("Downloading the Steam Linux Runtime", 0.03f);
	char *archive = path_in("downloads/" RUNTIME_FILE), got[65];
	bool ok = download(RUNTIME_URL RUNTIME_FILE, archive, 0.03f, 0.35f, got, NULL);
	if (ok && !str_eq_nocase(got, want)) {
		job_fail("The Steam Linux Runtime arrived damaged; try again.");
		ok = false;
	} else if (!ok) {
		job_fail("The Steam Linux Runtime didn't download.");
	}
	if (ok) {
		job_step("Unpacking the Steam Linux Runtime", 0.36f);
		char *dir = path_in("runtime");
		ok = unpack(archive, dir);
		if (!ok)
			job_fail("The Steam Linux Runtime couldn't be unpacked.");
		mem_free(dir);
	}
	sys_unlink(archive);
	mem_free(archive);
	return ok;
}

static bool get_proton(void)
{
	job_step("Proton: finding the newest version", 0.42f);
	HttpOptions o = { .timeout_ms = 20000, .accept = "application/vnd.github+json" };
	HttpResponse *r = http_request(PROTON_API, &o);
	char tag[64] = "", tar_url[600] = "", sum_url[600] = "";
	Json *j = r->status == 200 && r->body ? json_parse((const char *)r->body, r->body_len) : NULL;
	if (j) {
		str_copy(tag, json_str(json_get(j, "tag_name"), ""), sizeof tag);
		Json *assets = json_get(j, "assets");
		for (usize i = 0; assets && i < assets->count; i++) {
			const char *n = json_str(json_get(assets->items[i], "name"), "");
			const char *u = json_str(json_get(assets->items[i], "browser_download_url"), "");
			if (str_ends(n, ".tar.gz"))
				str_copy(tar_url, u, sizeof tar_url);
			else if (str_ends(n, ".sha512sum"))
				str_copy(sum_url, u, sizeof sum_url);
		}
		json_free(j);
	}
	http_response_free(r);
	if (!tar_url[0] || !sum_url[0] || !str_starts(tar_url, "https://")) {
		job_fail("GitHub, where Proton GE is published, couldn't be reached.");
		return false;
	}
	char want[129] = "";
	r = http_get(sum_url);
	if (r->status == 200 && r->body && r->body_len >= 128)
		fmt(want, sizeof want, "%.128s", (const char *)r->body);
	http_response_free(r);
	char step[120];
	fmt(step, sizeof step, "Downloading Proton %s", tag);
	job_step(step, 0.44f);
	char *archive = path_in("downloads/proton.tar.gz"), got[129];
	bool ok = want[0] && download(tar_url, archive, 0.44f, 0.92f, NULL, got);
	if (ok && !str_eq_nocase(got, want)) {
		job_fail("Proton arrived damaged; try again.");
		ok = false;
	} else if (!ok) {
		job_fail("Proton didn't download.");
	}
	if (ok) {
		job_step("Unpacking Proton", 0.93f);
		char *dir = path_in("proton");
		ok = unpack(archive, dir);
		if (!ok)
			job_fail("Proton couldn't be unpacked.");
		mem_free(dir);
	}
	sys_unlink(archive);
	mem_free(archive);
	return ok;
}

static void get_ready(void *arg)
{
	UNUSED(arg);
	char p[600];
	bool ok = (have_runtime() || get_runtime()) && (proton_dir(p, sizeof p) || get_proton());
	if (ok)
		job_step("Ready", 1);
	atomic_store(&job.busy, 0);
}

static void start_job(void)
{
	if (atomic_add(&job.busy, 0))
		return;
	job.error[0] = 0;
	atomic_store(&job.busy, 1);
	if (thread_spawn(get_ready, NULL) < 0)
		atomic_store(&job.busy, 0);
}

/* ---- Playing ------------------------------------------------------------------------------- */

static void list_exes(const char *dir, char ***out, usize *n, int depth)
{
	DirList l = { 0 };
	if (depth > 4 || !dir_list(dir, &l, false))
		return;
	vec_each(&l, e)
	{
		char *p = path_join(dir, e->name);
		if (e->type == DT_DIR)
			list_exes(p, out, n, depth + 1);
		else if (str_eq_nocase(path_ext(p), "exe") && !str_find_nocase(e->name, "unins") && !str_find_nocase(e->name, "crash")) {
			*out = mem_realloc(*out, sizeof(char *) * (*n + 1));
			(*out)[(*n)++] = p;
			continue;
		}
		mem_free(p);
	}
	dir_list_free(&l);
}

static void bottle_exes(const char *bottle, char ***out, usize *n)
{
	static const char *const places[] = { "pfx/drive_c/Program Files", "pfx/drive_c/Program Files (x86)", "pfx/drive_c/Games" };
	for (usize i = 0; i < ARRAY_LEN(places); i++) {
		char *d = path_join(bottle, places[i]);
		list_exes(d, out, n, 0);
		mem_free(d);
	}
}

static bool looks_like_installer(const char *exe)
{
	const char *b = path_base(exe);
	return str_eq_nocase(path_ext(b), "msi") || str_find_nocase(b, "setup") || str_find_nocase(b, "install");
}

static void play(Ui *ui, int i)
{
	UNUSED(ui);
	App *a = &apps[i];
	char proton[600], bottle[600];
	if (!have_runtime() || !proton_dir(proton, sizeof proton)) {
		str_copy(pending_run, a->exe, sizeof pending_run);
		start_job();
		return;
	}
	bottle_of(a, bottle, sizeof bottle);
	dir_make(bottle, 0755);
	char *runtime = path_in("runtime/SteamLinuxRuntime_sniper");
	char *entry = str_fmt("%s/_v2-entry-point", runtime);
	char *script = str_fmt("%s/proton", proton);
	char *where = path_dir(a->exe);
	/* an installer: what's in its bottle now, to find what it adds */
	for (usize k = 0; k < nbefore; k++)
		mem_free(before_exes[k]);
	mem_free(before_exes);
	before_exes = NULL, nbefore = 0, install_id[0] = 0, nfound = 0;
	if (looks_like_installer(a->exe)) {
		bottle_exes(bottle, &before_exes, &nbefore);
		str_copy(install_id, a->id, sizeof install_id);
	}
	char *vars[32];
	int nv = 0;
#define VAR(...)                                       \
	do {                                           \
		vars[nv++] = str_fmt(__VA_ARGS__);     \
	} while (0)
	VAR("STEAM_COMPAT_DATA_PATH=%s", bottle);
	VAR("STEAM_COMPAT_CLIENT_INSTALL_PATH=%s", base);
	VAR("STEAM_COMPAT_INSTALL_PATH=%s", where);
	VAR("STEAM_COMPAT_TOOL_PATHS=%s:%s", proton, runtime);
	VAR("STEAM_COMPAT_LIBRARY_PATHS=%s:%s", where, base);
	VAR("STEAM_COMPAT_APP_ID=0");
	VAR("SteamAppId=0");
	VAR("SteamGameId=bridge-%s", a->id);
	VAR("PROTON_VERB=waitforexitandrun");
	VAR("PRESSURE_VESSEL_FILESYSTEMS_RW=%s:%s", where, base);
	if (file_exists("/dev/ntsync"))
		VAR("PROTON_USE_NTSYNC=1");
	if (a->fsr)
		VAR("WINE_FULLSCREEN_FSR=1");
	if (a->hud)
		VAR("DXVK_HUD=fps");
	if (a->nvapi)
		VAR("PROTON_ENABLE_NVAPI=1");
	if (a->wayland)
		VAR("PROTON_ENABLE_WAYLAND=1");
	char rt[600];
	if (steam_runtime("Proton EasyAntiCheat Runtime", rt, sizeof rt))
		VAR("PROTON_EAC_RUNTIME=%s", rt);
	if (steam_runtime("Proton BattlEye Runtime", rt, sizeof rt))
		VAR("PROTON_BATTLEYE_RUNTIME=%s", rt);
#undef VAR
	/* this process's environment, then Proton's */
	usize ne = 0;
	while (environ[ne])
		ne++;
	char **envp = mem_alloc(sizeof(char *) * (ne + (usize)nv + 1));
	usize k = 0;
	for (usize e = 0; e < ne; e++)
		envp[k++] = environ[e];
	for (int v = 0; v < nv; v++)
		envp[k++] = vars[v];
	envp[k] = NULL;
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[24];
	int n = 0;
	argv[n++] = layerrun;
	argv[n++] = "--x11";
	argv[n++] = "--app";
	argv[n++] = "bridge";
	argv[n++] = entry;
	argv[n++] = "--verb=waitforexitandrun";
	argv[n++] = "--";
	argv[n++] = script;
	argv[n++] = "waitforexitandrun";
	argv[n++] = a->exe;
	usize na = 0;
	char **extra = a->args[0] ? str_split(a->args, ' ', &na) : NULL;
	for (usize e = 0; e < na && n < 22; e++)
		if (*extra[e])
			argv[n++] = extra[e];
	argv[n] = NULL;
	SpawnOpts o = { .cwd = where, .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .new_session = true };
	play_pid = proc_spawn(layerrun, argv, envp, &o);
	if (play_pid > 0) {
		str_copy(play_id, a->id, sizeof play_id);
		a->played = time_unix();
		save_app(a);
		fmt(message, sizeof message, "Starting %s… the first start of a bottle takes a minute.", a->name);
	} else {
		fmt(message, sizeof message, "%s couldn't start.", a->name);
	}
	if (extra)
		str_split_free(extra, na);
	for (int v = 0; v < nv; v++)
		mem_free(vars[v]);
	mem_free(envp);
	mem_free(layerrun);
	mem_free(runtime);
	mem_free(entry);
	mem_free(script);
	mem_free(where);
}

/* Programs an installer added to its bottle, the likely main one first (the biggest). */
static void after_install(void)
{
	if (!install_id[0])
		return;
	int i = -1;
	for (int k = 0; k < napps; k++)
		if (str_eq(apps[k].id, install_id))
			i = k;
	if (i < 0)
		return;
	char bottle[600];
	bottle_of(&apps[i], bottle, sizeof bottle);
	char **now = NULL;
	usize nnow = 0;
	bottle_exes(bottle, &now, &nnow);
	nfound = 0;
	u64 sizes[8] = { 0 };
	for (usize a = 0; a < nnow; a++) {
		bool old = false;
		for (usize b = 0; b < nbefore && !old; b++)
			old = str_eq(now[a], before_exes[b]);
		if (old || find_app(now[a]) >= 0)
			continue;
		struct stat st;
		u64 size = sys_stat(now[a], &st) == 0 ? (u64)st.st_size : 0;
		/* keep the eight biggest */
		int at = nfound < 8 ? nfound++ : 7;
		if (at == 7 && nfound == 8 && size <= sizes[7])
			continue;
		str_copy(found[at], now[a], sizeof found[0]);
		sizes[at] = size;
		for (int z = at; z > 0 && sizes[z] > sizes[z - 1]; z--) {
			u64 ts = sizes[z];
			sizes[z] = sizes[z - 1], sizes[z - 1] = ts;
			char tp[512];
			str_copy(tp, found[z], sizeof tp);
			str_copy(found[z], found[z - 1], sizeof found[0]);
			str_copy(found[z - 1], tp, sizeof found[0]);
		}
	}
	for (usize a = 0; a < nnow; a++)
		mem_free(now[a]);
	mem_free(now);
}

/* ---- The window -------------------------------------------------------------------------- */

static Color tile_color(const char *name)
{
	u32 h = 2166136261u;
	for (const char *p = name; *p; p++)
		h = (h ^ (u8)*p) * 16777619u;
	return color_hsl((f32)(h % 360), 0.55f, 0.46f, 255);
}

static void monogram(Ui *ui, Rect r, const char *name)
{
	ui_round(ui, r, 14, tile_color(name));
	ui_gradient(ui, rect(r.x, r.y, r.w, r.h * 0.55f), 14, color_hex("#ffffff30"), COLOR_CLEAR);
	ui_border(ui, r, 14, 1, color_hex("#ffffff30"));
	char m[4] = { name[0] ? name[0] : '?', 0 };
	if (m[0] >= 'a' && m[0] <= 'z')
		m[0] = (char)(m[0] - 32);
	ui_text_in(ui, ui_font(ui, r.h * 0.42f, 700), r, m, COLOR_WHITE, ALIGN_CENTER);
}

static bool option_row(Ui *ui, Rect *c, const char *label, const char *sub, bool *v)
{
	Theme *t = &ui->theme;
	Rect row = cut_top(c, 52);
	ui_text(ui, ui_font(ui, 13, 600), row.x, row.y + 21, label, t->text);
	ui_text(ui, ui_font(ui, 11, 400), row.x, row.y + 38, sub, t->text_dim);
	ui_push_id_str(ui, label);
	bool changed = ui_switch(ui, center_in(cut_right(&row, 50), 46, 26), v);
	ui_pop_id(ui);
	return changed;
}

static void details(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	App *a = &apps[sel];
	ui_card(ui, r);
	Rect c = pad(r, 18);
	Rect top = cut_top(&c, 64);
	monogram(ui, rect(top.x, top.y, 64, 64), a->name);
	Rect nm = rect(top.x + 78, top.y + 6, top.w - 78, 32);
	int ev = ui_entry(ui, nm, "bridge-name", &name_edit, "Name", ENTRY_FLAT);
	if (ev & (ENTRY_CHANGED | ENTRY_SUBMIT) && sb_str(&name_edit)[0]) {
		str_copy(a->name, sb_str(&name_edit), sizeof a->name);
		save_app(a);
	}
	ui_text_in(ui, ui_font(ui, 11, 400), rect(top.x + 80, top.y + 40, top.w - 80, 18), a->exe, t->text_faint, ALIGN_START);
	cut_top(&c, 12);
	bool playing = play_pid > 0 && str_eq(play_id, a->id);
	Rect btns = cut_top(&c, 36);
	if (playing) {
		if (ui_button_destructive(ui, cut_left(&btns, 130), "Stop")) {
			sys_kill(-play_pid, SIGTERM);
			str_copy(message, "Stopping…", sizeof message);
		}
	} else if (ui_button_suggested(ui, cut_left(&btns, 130), looks_like_installer(a->exe) ? "Install" : "Play")) {
		play(ui, sel);
	}
	cut_left(&btns, 8);
	if (ui_button(ui, cut_left(&btns, 120), "Show Files")) {
		char bottle[600];
		bottle_of(a, bottle, sizeof bottle);
		char *drive = str_fmt("%s/pfx/drive_c", bottle);
		ui_launch(ui, "vault", dir_exists(drive) ? drive : bottle);
		mem_free(drive);
	}
	cut_top(&c, 14);
	ui_heading(ui, cut_top(&c, 26), "Options");
	bool ch = false;
	ch |= option_row(ui, &c, "FidelityFX upscaling (FSR)", "Sharper full-screen games at lower resolutions", &a->fsr);
	ch |= option_row(ui, &c, "Frame rate counter", "Frames per second in the corner", &a->hud);
	ch |= option_row(ui, &c, "NVIDIA DLSS and Reflex", "For games that support them, on NVIDIA cards", &a->nvapi);
	ch |= option_row(ui, &c, "Native Wayland (experimental)", "Skips Xwayland; some games run smoother", &a->wayland);
	if (ch)
		save_app(a);
	cut_top(&c, 8);
	char rt[600];
	bool eac = steam_runtime("Proton EasyAntiCheat Runtime", rt, sizeof rt), be = steam_runtime("Proton BattlEye Runtime", rt, sizeof rt);
	char ac[240];
	fmt(ac, sizeof ac, "Anti-cheat: Easy Anti-Cheat %s, BattlEye %s. They work in games whose makers allow Linux.",
	    eac ? "ready" : "not installed", be ? "ready" : "not installed");
	ui_text_wrapped(ui, ui_font(ui, 11, 400), cut_top(&c, 34), ac, t->text_dim, ALIGN_START);
	Rect bottom = cut_bottom(&c, 34);
	if (ui_button_flat(ui, cut_right(&bottom, 150), "Remove from Bridge")) {
		char *path = str_fmt("%s/apps/%s.conf", base, a->id);
		sys_unlink(path);
		mem_free(path);
		sel = -1;
		load_library();
	}
}

static void ready_banner(Ui *ui, Rect *b)
{
	Theme *t = &ui->theme;
	char proton[600];
	bool ready = have_runtime() && proton_dir(proton, sizeof proton);
	if (ready && !atomic_add(&job.busy, 0))
		return;
	Rect card = cut_top(b, 118);
	cut_top(b, 16);
	ui_card(ui, card);
	Rect c = pad(card, 18);
	ui_icon(ui, rect(c.x, c.y + 4, 36, 36), "bridge", t->accent);
	Rect words = pad4(c, 0, 0, 0, 52);
	ui_text(ui, ui_font(ui, 16, 700), words.x, words.y + 18, "Run Windows apps and games", t->text);
	mutex_lock(&job.lock);
	char step[120], error[200];
	str_copy(step, job.step, sizeof step);
	str_copy(error, job.error, sizeof error);
	f32 progress = job.progress;
	mutex_unlock(&job.lock);
	if (atomic_add(&job.busy, 0)) {
		ui_text(ui, ui_font(ui, 12, 400), words.x, words.y + 40, step, t->text_dim);
		ui_progress(ui, rect(words.x, words.y + 54, words.w, 6), progress);
		ui->wake_at = ui->now + 0.25;
	} else {
		ui_text_wrapped(ui, ui_font(ui, 12, 400), rect(words.x, words.y + 26, words.w, 34),
				error[0] ? error : "Bridge needs Proton and Valve's Steam Linux Runtime, about 900 MB, downloaded once.",
				error[0] ? t->danger : t->text_dim, ALIGN_START);
		if (ui_button_suggested(ui, rect(words.x, words.y + 60, 160, 32), error[0] ? "Try Again" : "Get Ready"))
			start_job();
	}
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect tools = ui_headerbar(ui, cut_top(&win, 46), "Bridge", 0);
	if (ui_button(ui, center_in(cut_right(&tools, 138), 130, 32), "Add Program"))
		ui_file_dialog_open(ui, "bridge-add", FILE_OPEN, home_dir(), "exe,msi,bat", NULL);
	char picked[512];
	if (ui_file_dialog(ui, "bridge-add", picked, sizeof picked))
		sel = add_app(picked, NULL);
	char *dropped = ui_dropped_file(ui, "exe,msi,bat");
	if (dropped) {
		sel = add_app(dropped, NULL);
		mem_free(dropped);
	}
	/* what's playing ended */
	if (play_pid > 0) {
		int st;
		if (proc_wait(play_pid, &st, true) == play_pid) {
			play_pid = -1;
			message[0] = 0;
			after_install();
		}
		ui->wake_at = ui->now + 1;
	}
	/* a program to run once everything's here */
	if (pending_run[0] && !atomic_add(&job.busy, 0)) {
		char proton[600];
		if (have_runtime() && proton_dir(proton, sizeof proton)) {
			int i = find_app(pending_run);
			pending_run[0] = 0;
			if (i >= 0)
				play(ui, i);
		}
	}
	Rect b = pad2(win, 24, 18);
	if (!have_layer()) {
		ui_placeholder(ui, b, "bridge", "The Linux layer is needed",
			       "Bridge runs Windows programs with Proton, which lives in the Linux layer. Install Zurbax to get it.");
		return;
	}
	Rect side = sel >= 0 && sel < napps ? cut_right(&b, 360) : (Rect){ 0 };
	if (side.w > 0) {
		cut_right(&b, 18);
		details(ui, side);
	}
	ready_banner(ui, &b);
	/* found after an installer ran */
	if (nfound > 0) {
		Rect card = cut_top(&b, 54 + (f32)MIN(nfound, 3) * 40);
		cut_top(&b, 16);
		ui_card(ui, card);
		Rect c = pad(card, 14);
		ui_text(ui, ui_font(ui, 14, 700), c.x, c.y + 16, "The installer added these programs", t->text);
		cut_top(&c, 28);
		for (int i = 0; i < MIN(nfound, 3); i++) {
			Rect row = cut_top(&c, 40);
			ui_text_in(ui, ui_font(ui, 13, 500), rect(row.x, row.y, row.w - 120, row.h), path_base(found[i]), t->text, ALIGN_START);
			ui_push_id(ui, i);
			if (ui_button(ui, rect(row.x + row.w - 110, row.y + 4, 110, 32), "Add")) {
				char owner[48];
				str_copy(owner, install_id, sizeof owner);
				int k = add_app(found[i], NULL);
				if (k >= 0) {
					/* it lives in the installer's bottle */
					char *path = str_fmt("%s/apps/%s.conf", base, apps[k].id);
					Conf *cf = conf_load(path);
					conf_set(cf, "bottle", owner);
					conf_save(cf);
					conf_free(cf);
					mem_free(path);
					sel = k;
				}
				nfound = 0;
			}
			ui_pop_id(ui);
		}
	}
	if (!napps) {
		ui_drop_zone(ui, b, "bridge", "Drop a Windows program here");
		ui_placeholder(ui, b, "bridge", "No Windows programs yet",
			       "Drop a game or an installer (.exe or .msi) here, choose Add Program, or open one from Files.");
		return;
	}
	/* the library: tiles in rows */
	f32 tw = 168, th = 196, gap = 16;
	int per = MAX(1, (int)((b.w + gap) / (tw + gap)));
	f32 y = ui_scroll_begin(ui, b, "bridge-lib");
	for (int i = 0; i < napps; i++) {
		Rect cell = rect(b.x + (f32)(i % per) * (tw + gap), y + (f32)(i / per) * (th + gap), tw, th);
		ui_push_id_str(ui, apps[i].id);
		UiInteract it = ui_interact(ui, ui_id(ui, "tile"), cell);
		if (it.hovered || sel == i)
			ui_round(ui, rect_inset(cell, -6, -6), 18, sel == i ? color_with_alpha(t->accent, 0x30) : t->hover);
		monogram(ui, rect(cell.x + 14, cell.y + 6, tw - 28, tw - 28), apps[i].name);
		bool playing = play_pid > 0 && str_eq(play_id, apps[i].id);
		ui_text_in(ui, ui_font(ui, 13, 600), rect(cell.x, cell.y + tw - 14, tw, 20), apps[i].name, t->text, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 11, 400), rect(cell.x, cell.y + tw + 6, tw, 16), playing ? "Playing" : looks_like_installer(apps[i].exe) ? "Installer" : "Windows",
			   playing ? t->accent : t->text_faint, ALIGN_CENTER);
		if (it.clicked) {
			sel = i;
			sb_clear(&name_edit);
			sb_adds(&name_edit, apps[i].name);
		}
		if (it.double_clicked)
			play(ui, i);
		ui_pop_id(ui);
	}
	ui_scroll_end(ui, (f32)((napps + per - 1) / per) * (th + gap));
	if (message[0])
		ui_text(ui, ui_font(ui, 12, 400), b.x, b.y + b.h - 4, message, t->text_dim);
}

static const char *open_arg;

static void init(Ui *ui)
{
	UNUSED(ui);
	fmt(base, sizeof base, "%s/.local/share/zurbax/bridge", home_dir());
	char *dirs[] = { path_in("apps"), path_in("bottles"), path_in("proton"), path_in("runtime"), path_in("downloads") };
	for (usize i = 0; i < ARRAY_LEN(dirs); i++) {
		dir_make(dirs[i], 0755);
		mem_free(dirs[i]);
	}
	load_library();
	if (open_arg && open_arg[0] == '/' && file_exists(open_arg)) {
		sel = add_app(open_arg, NULL);
		if (sel >= 0) {
			sb_adds(&name_edit, apps[sel].name);
			if (have_layer())
				play(ui, sel);
		}
	}
}

int main(int argc, char **argv)
{
	if (argc > 1)
		open_arg = argv[1];
	UiAppDesc d = { .id = "bridge", .title = "Bridge", .width = 1000, .height = 660, .min_width = 760, .min_height = 520,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
