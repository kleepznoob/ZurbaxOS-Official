/*
 * Bridge — Windows programs and games on Zurbax.
 *
 * Three shelves: the Windows programs people add (here), the games Steam
 * installed (steam.c) and games played in the cloud (cloud.c), for the few
 * whose anti-cheat turns Linux away.
 *
 * Inside it's Proton, in its GE build (the community's, with the newest
 * fixes), run in Valve's Steam Linux Runtime the way Steam runs it, through
 * layerrun in the Linux layer with an Xwayland of its own. DirectX goes
 * through DXVK and VKD3D-Proton to Vulkan, synchronization through the
 * kernel's ntsync, and the anti-cheat runtimes Steam installs (Easy
 * Anti-Cheat, BattlEye) are passed on for games whose makers allow Linux.
 * Every program gets a bottle of its own: a Windows folder tree, registry
 * and settings, so one can't break another. The first time a program runs,
 * Bridge asks whether it may use the person's files: allowed, its
 * Documents, Downloads, Pictures, Music, Videos and Desktop are the real
 * ones, and USB drives get drive letters (D: on); kept apart, it has its
 * own. It can be changed in its options.
 *
 * Proton and the runtime are downloaded the first time (about 900 MB), each
 * checked against its published checksum.
 *
 * Every program in the library has a cover. A game's comes from its store
 * page on Steam, found by the name the program gives itself in its version
 * information; anything else gets one made of its own icon, read from the
 * .exe, and its name. People can choose a picture of their own instead.
 *
 * In ~/.local/share/zurbax/bridge: apps/ID.conf (the library), bottles/ID,
 * covers/ID and covers/ID-icon.png, proton/GE-Proton*,
 * runtime/SteamLinuxRuntime_sniper.
 *
 *     bridge [FILE.exe]      opens Bridge, and runs FILE in its bottle
 */
#include "bridge.h"
#include "pe.h"

#define RUNTIME_URL "https://repo.steampowered.com/steamrt3/images/latest-container-runtime-public-beta/"
#define RUNTIME_FILE "SteamLinuxRuntime_sniper.tar.xz"
#define PROTON_API "https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/latest"
#define STEAM_SEARCH "https://store.steampowered.com/api/storesearch/?l=english&cc=us&term="
#define STEAM_ART "https://cdn.cloudflare.steamstatic.com/steam/apps/"

typedef struct {
	char id[48];
	char name[96];
	char exe[512];
	char args[256];
	bool fsr, hud, nvapi, wayland;
	int files; /* the person's files: -1 not asked yet, 0 kept apart, 1 allowed */
	i64 played;
	/* where its cover came from: "steam:APPID" (portrait art), "wide:APPID"
	 * (a store banner), "file" (the person's), "icon" or "none"; "" when
	 * nobody has looked yet */
	char cover[24];
} App;

static App apps[128];
static int napps;
char bridge_base[400];
static int sel = -1;
static StrBuf name_edit;
static int tab; /* the shelf: 0 Windows, 1 Steam, 2 Cloud */
static char pending_run[512];   /* a file to run once Proton is ready */
char bridge_msg[200];

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

/* covers, loaded when first drawn */
static Cover covers[200];
static int ncovers;

/* finding a cover, on a thread of its own: one program at a time */
static struct {
	int busy; /* 1 looking, 2 done: the results wait to be taken */
	char id[48], name[96], exe[512];
	bool installer, named;
	char found_name[96], src[24]; /* the name it goes by on Steam or calls itself, and the cover's source */
} cj;

/* ---- Places ------------------------------------------------------------------------------- */

static char *path_in(const char *rel)
{
	return str_fmt("%s/%s", bridge_base, rel);
}

/* The Linux layer, where Proton runs: /data/layers/steam on Zurbax. */
static bool have_layer(void)
{
	char *p = str_fmt("%s/layers/steam/usr/bin/tar", zurbax_data());
	bool ok = file_exists(p);
	mem_free(p);
	return ok;
}

bool layer_has(const char *path)
{
	char *p = str_fmt("%s/layers/steam%s", zurbax_data(), path);
	bool ok = file_exists(p);
	mem_free(p);
	return ok;
}

/* what layer_start started, to collect when each ends */
static int started[16];
static int nstarted;

int layer_start(char **args, bool new_session)
{
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[24];
	int n = 0;
	argv[n++] = layerrun;
	for (int i = 0; args[i] && n < 23; i++)
		argv[n++] = args[i];
	argv[n] = NULL;
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .new_session = new_session };
	int pid = proc_spawn(layerrun, argv, environ, &o);
	mem_free(layerrun);
	if (pid > 0 && nstarted < (int)ARRAY_LEN(started))
		started[nstarted++] = pid;
	return pid;
}

void wake_within(Ui *ui, f64 secs)
{
	if (ui->wake_at <= ui->now || ui->wake_at > ui->now + secs)
		ui->wake_at = ui->now + secs;
}

static void reap_started(void)
{
	for (int i = 0; i < nstarted;) {
		int st;
		if (proc_wait(started[i], &st, true) == started[i])
			started[i] = started[--nstarted];
		else
			i++;
	}
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
	char *path = str_fmt("%s/apps/%s.conf", bridge_base, a->id);
	Conf *c = conf_load(path);
	conf_set(c, "name", a->name);
	conf_set(c, "exe", a->exe);
	conf_set(c, "args", a->args);
	conf_set_bool(c, "fsr", a->fsr);
	conf_set_bool(c, "fps", a->hud);
	conf_set_bool(c, "nvapi", a->nvapi);
	conf_set_bool(c, "wayland", a->wayland);
	conf_set(c, "files", a->files < 0 ? "ask" : a->files ? "yes" : "no");
	conf_set_int(c, "played", a->played);
	conf_set(c, "cover", a->cover);
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
			const char *files = conf_get(c, "files", "ask");
			a->files = str_eq(files, "yes") ? 1 : str_eq(files, "no") ? 0 : -1;
			a->played = conf_get_int(c, "played", 0);
			str_copy(a->cover, conf_get(c, "cover", ""), sizeof a->cover);
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
	a.files = -1;
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
	char *path = str_fmt("%s/apps/%s.conf", bridge_base, a->id);
	Conf *c = conf_load(path);
	const char *b = conf_get(c, "bottle", NULL);
	if (b)
		str_copy(id, b, sizeof id);
	conf_free(c);
	mem_free(path);
	fmt(out, cap, "%s/bottles/%s", bridge_base, id);
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

/* ---- The person's files -------------------------------------------------------------------- */

static const char *const USER_FOLDERS[] = { "Documents", "Downloads", "Pictures", "Music", "Videos", "Desktop" };
static int ask_for = -1; /* the program whose first start waits for the answer */

/* A program allowed the person's files: the Windows user's folders in its
 * bottle become links to the real ones (an empty folder of its own makes way;
 * one with files in it stays), and each USB drive gets a letter. Kept apart,
 * the links go and it has empty folders of its own again. */
static void share_files(const App *a, const char *bottle)
{
	char *user = str_fmt("%s/pfx/drive_c/users/steamuser", bottle);
	if (dir_exists(user)) {
		for (usize i = 0; i < ARRAY_LEN(USER_FOLDERS); i++) {
			char *mine = str_fmt("%s/%s", user, USER_FOLDERS[i]);
			char *real = str_fmt("%s/%s", home_dir(), USER_FOLDERS[i]);
			struct stat st;
			bool link = sys_lstat(mine, &st) == 0 && S_ISLNK(st.st_mode);
			if (a->files == 1 && !link && dir_exists(real)) {
				sys_rmdir(mine); /* only when it's empty */
				if (!dir_exists(mine))
					sys_symlink(real, mine);
			} else if (a->files != 1 && link) {
				sys_unlink(mine);
				dir_make(mine, 0755);
			}
			mem_free(mine);
			mem_free(real);
		}
	}
	mem_free(user);
	/* drive letters for the drives in /media, D: on; the old ones go first */
	char *dosdev = str_fmt("%s/pfx/dosdevices", bottle);
	if (dir_exists(dosdev)) {
		for (char l = 'd'; l <= 'y'; l++) {
			char *p = str_fmt("%s/%c:", dosdev, l);
			char target[512] = { 0 };
			if (sys_readlink(p, target, sizeof target - 1) > 0 && str_starts(target, "/media/"))
				sys_unlink(p);
			mem_free(p);
		}
		DirList l = { 0 };
		if (a->files == 1 && dir_list("/media", &l, false)) {
			char letter = 'd';
			vec_each(&l, e) {
				if (letter > 'y')
					break;
				char *p, *target = str_fmt("/media/%s", e->name);
				struct stat st;
				for (p = str_fmt("%s/%c:", dosdev, letter); sys_lstat(p, &st) == 0 && letter < 'y';) {
					mem_free(p);
					p = str_fmt("%s/%c:", dosdev, ++letter);
				}
				if (sys_lstat(p, &st) < 0)
					sys_symlink(target, p);
				letter++;
				mem_free(p);
				mem_free(target);
			}
			dir_list_free(&l);
		}
	}
	mem_free(dosdev);
}

static void play(Ui *ui, int i)
{
	App *a = &apps[i];
	char proton[600], bottle[600];
	if (a->files < 0) {
		ask_for = i;
		ui_dialog_open(ui, "bridge-files");
		return;
	}
	if (!have_runtime() || !proton_dir(proton, sizeof proton)) {
		str_copy(pending_run, a->exe, sizeof pending_run);
		start_job();
		return;
	}
	bottle_of(a, bottle, sizeof bottle);
	dir_make(bottle, 0755);
	share_files(a, bottle);
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
	VAR("STEAM_COMPAT_CLIENT_INSTALL_PATH=%s", bridge_base);
	VAR("STEAM_COMPAT_INSTALL_PATH=%s", where);
	VAR("STEAM_COMPAT_TOOL_PATHS=%s:%s", proton, runtime);
	VAR("STEAM_COMPAT_LIBRARY_PATHS=%s:%s", where, bridge_base);
	VAR("STEAM_COMPAT_APP_ID=0");
	VAR("SteamAppId=0");
	VAR("SteamGameId=bridge-%s", a->id);
	VAR("PROTON_VERB=waitforexitandrun");
	if (a->files == 1)
		VAR("PRESSURE_VESSEL_FILESYSTEMS_RW=%s:%s:/media", where, bridge_base);
	else
		VAR("PRESSURE_VESSEL_FILESYSTEMS_RW=%s:%s", where, bridge_base);
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
		fmt(bridge_msg, sizeof bridge_msg, "Starting %s… the first start of a bottle takes a minute.", a->name);
	} else {
		fmt(bridge_msg, sizeof bridge_msg, "%s couldn't start.", a->name);
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

/* ---- Covers ------------------------------------------------------------------------------ */

char *cover_path(const char *id, const char *suffix)
{
	return str_fmt("%s/covers/%s%s", bridge_base, id, suffix);
}

/* Letters and digits, lower case: "ELDEN RING™" and "eldenring" are one name. */
static void squash(const char *s, char *out, usize cap)
{
	usize n = 0;
	for (; *s && n + 1 < cap; s++)
		if ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9'))
			out[n++] = *s;
		else if (*s >= 'A' && *s <= 'Z')
			out[n++] = (char)(*s + 32);
	out[n] = 0;
}

/* Is a program's ProductName its name, or an engine's or installer's? */
static bool real_product(const char *p)
{
	static const char *const generic[] = { "unrealgame", "ue4game", "ue5game", "epicgames", "unityplayer", "unity",
					       "microsoftwindowsoperatingsystem", "windowsinstaller", "setup", "installer",
					       "launcher", "game", "application", "app", "innosetup", "nsis", "electron" };
	char q[96];
	squash(p, q, sizeof q);
	if (strlen(q) < 2 || strlen(p) > 60)
		return false;
	for (usize i = 0; i < ARRAY_LEN(generic); i++)
		if (str_eq(q, generic[i]))
			return false;
	return true;
}

/* The name Bridge gave a program from its file name, before anyone renamed it. */
static bool default_name(const App *a)
{
	char n[96];
	name_from(a->exe, n, sizeof n);
	return str_eq(n, a->name);
}

/* Without the trademark signs some names carry. */
static void strip_marks(char *s)
{
	char *w = s;
	for (const char *r = s; *r;) {
		if (!memcmp(r, "\xe2\x84\xa2", 3))
			r += 3;
		else if (!memcmp(r, "\xc2\xae", 2) || !memcmp(r, "\xc2\xa9", 2))
			r += 2;
		else
			*w++ = *r++;
	}
	*w = 0;
	str_copy(s, str_trim(s), strlen(s) + 1);
}

/* A game on Steam by this name: its app id, or 0, and the name it has there. */
static u32 steam_find(const char *name, char *official, usize cap)
{
	char want[96];
	squash(name, want, sizeof want);
	if (strlen(want) < 2)
		return 0;
	char *q = url_encode(name);
	char *url = str_fmt(STEAM_SEARCH "%s", q);
	HttpResponse *r = http_get(url);
	mem_free(q);
	mem_free(url);
	u32 id = 0, close = 0;
	Json *j = r->status == 200 && r->body ? json_parse((const char *)r->body, r->body_len) : NULL;
	Json *items = j ? json_get(j, "items") : NULL;
	for (usize i = 0; items && i < items->count && !id; i++) {
		char got[96];
		squash(json_str(json_get(items->items[i], "name"), ""), got, sizeof got);
		u32 app = (u32)json_num(json_get(items->items[i], "id"), 0);
		bool exact = str_eq(got, want);
		/* "Hollow Knight" for "hollowknight_x64", say: a long enough start, most of the name */
		bool near = !close && strlen(want) >= 6 && (str_starts(got, want) || str_starts(want, got)) &&
			    MIN(strlen(got), strlen(want)) * 10 >= MAX(strlen(got), strlen(want)) * 7;
		if (exact || near) {
			if (exact)
				id = app;
			else
				close = app;
			str_copy(official, json_str(json_get(items->items[i], "name"), ""), cap);
		}
	}
	if (j)
		json_free(j);
	http_response_free(r);
	return id ? id : close;
}

/* Steam's art for an app: the tall library picture, else the store banner. */
bool steam_art(u32 app, const char *path, char *src, usize cap)
{
	static const char *const kinds[] = { "library_600x900.jpg", "header.jpg" };
	for (usize k = 0; k < ARRAY_LEN(kinds); k++) {
		char *url = str_fmt(STEAM_ART "%u/%s", app, kinds[k]);
		HttpResponse *r = http_get(url);
		mem_free(url);
		Canvas *img = r->status == 200 && r->body ? image_decode(r->body, r->body_len) : NULL;
		bool ok = img && file_write_atomic(path, r->body, r->body_len);
		if (img)
			canvas_free(img);
		http_response_free(r);
		if (ok) {
			fmt(src, cap, "%s:%u", k == 0 ? "steam" : "wide", app);
			return true;
		}
	}
	return false;
}

static void find_cover(void *arg)
{
	UNUSED(arg);
	PeInfo pi;
	char product[96] = "";
	cj.found_name[0] = 0;
	bool icon = false;
	if (pe_read(cj.exe, &pi)) {
		if (real_product(pi.product))
			str_copy(product, pi.product, sizeof product);
		if (pi.icon) {
			char *path = cover_path(cj.id, "-icon.png");
			icon = png_save(pi.icon, path);
			mem_free(path);
		}
		pe_info_free(&pi);
	}
	/* on Steam: by the name someone gave it; else the program's own, then its file's */
	const char *tries[2];
	int nt = 0;
	if (cj.named) {
		tries[nt++] = cj.name;
	} else {
		if (product[0])
			tries[nt++] = product;
		if (!cj.installer)
			tries[nt++] = cj.name;
	}
	str_copy(cj.src, icon ? "icon" : "none", sizeof cj.src);
	char *art = cover_path(cj.id, "");
	for (int i = 0; i < nt; i++) {
		char official[96] = "";
		u32 app = steam_find(tries[i], official, sizeof official);
		if (app && steam_art(app, art, cj.src, sizeof cj.src)) {
			strip_marks(official);
			str_copy(cj.found_name, official, sizeof cj.found_name);
			break;
		}
	}
	if (!cj.found_name[0])
		str_copy(cj.found_name, product, sizeof cj.found_name);
	mem_free(art);
	atomic_store(&cj.busy, 2);
}

void forget_cover(const char *id)
{
	for (int i = 0; i < ncovers; i++)
		if (str_eq(covers[i].id, id)) {
			if (covers[i].art)
				canvas_free(covers[i].art);
			if (covers[i].icon)
				canvas_free(covers[i].icon);
			covers[i] = covers[--ncovers];
			return;
		}
}

Cover *cover_slot(const char *key)
{
	for (int i = 0; i < ncovers; i++)
		if (str_eq(covers[i].id, key))
			return &covers[i];
	if (ncovers == (int)ARRAY_LEN(covers))
		forget_cover(covers[0].id);
	Cover *c = &covers[ncovers++];
	memset(c, 0, sizeof *c);
	str_copy(c->id, key, sizeof c->id);
	return c;
}

/* One program at a time, the next without a cover; then its results, here on the UI's thread. */
static void cover_tick(Ui *ui)
{
	int st = atomic_add(&cj.busy, 0);
	if (st == 2) {
		int i = -1;
		for (int k = 0; k < napps; k++)
			if (str_eq(apps[k].id, cj.id))
				i = k;
		if (i >= 0) {
			App *a = &apps[i];
			if (cj.found_name[0] && default_name(a)) {
				str_copy(a->name, cj.found_name, sizeof a->name);
				if (sel == i) {
					sb_clear(&name_edit);
					sb_adds(&name_edit, a->name);
				}
			}
			str_copy(a->cover, cj.src, sizeof a->cover);
			save_app(a);
			forget_cover(a->id);
		}
		atomic_store(&cj.busy, 0);
		st = 0;
	}
	if (st == 0) {
		for (int i = 0; i < napps; i++) {
			App *a = &apps[i];
			if (a->cover[0])
				continue;
			str_copy(cj.id, a->id, sizeof cj.id);
			str_copy(cj.name, a->name, sizeof cj.name);
			str_copy(cj.exe, a->exe, sizeof cj.exe);
			cj.installer = looks_like_installer(a->exe);
			cj.named = !default_name(a);
			atomic_store(&cj.busy, 1);
			if (thread_spawn(find_cover, NULL) < 0) {
				atomic_store(&cj.busy, 0);
				str_copy(a->cover, "none", sizeof a->cover);
				save_app(a);
			}
			break;
		}
	}
	if (atomic_add(&cj.busy, 0))
		ui->wake_at = ui->now + 0.3;
}

/* A picture someone chose for a program. */
static void set_cover_file(App *a, const char *picked)
{
	usize len;
	char *data = file_read(picked, &len);
	Canvas *img = data ? image_decode(data, len) : NULL;
	if (img) {
		char *path = cover_path(a->id, "");
		if (file_write_atomic(path, data, len)) {
			str_copy(a->cover, "file", sizeof a->cover);
			save_app(a);
			forget_cover(a->id);
		}
		mem_free(path);
		canvas_free(img);
	} else {
		fmt(bridge_msg, sizeof bridge_msg, "%s isn't a picture Bridge can show.", path_base(picked));
	}
	mem_free(data);
}

/* ---- The window -------------------------------------------------------------------------- */

Color tile_color(const char *name)
{
	u32 h = 2166136261u;
	for (const char *p = name; *p; p++)
		h = (h ^ (u8)*p) * 16777619u;
	return color_hsl((f32)(h % 360), 0.55f, 0.46f, 255);
}

/* A cover: tall art as it is; else one made of a banner, an icon or the initial, and the name. */
void draw_cover_img(Ui *ui, Rect r, const Canvas *art, bool wide, const Canvas *icon, const char *name, Color tone, f32 radius)
{
	if (art && !wide) {
		ui_image(ui, r, art, radius, true);
		ui_gradient(ui, rect(r.x, r.y, r.w, r.h * 0.4f), radius, color_hex("#ffffff1c"), COLOR_CLEAR);
		ui_border(ui, r, radius, 1, color_hex("#ffffff26"));
		return;
	}
	if (!tone)
		tone = tile_color(name);
	ui_gradient(ui, r, radius, color_lighten(tone, 0.08f), color_lighten(tone, -0.5f));
	ui_gradient(ui, rect(r.x, r.y, r.w, r.h * 0.5f), radius, color_hex("#ffffff2a"), COLOR_CLEAR);
	f32 mid = r.y + r.h * 0.36f;
	if (art) {
		/* a store banner, across the middle */
		f32 h = r.w * 215 / 460;
		ui_shadow(ui, rect(r.x, mid - h / 2, r.w, h), 0, 18, 6, color_hex("#00000060"));
		ui_image(ui, rect(r.x, mid - h / 2, r.w, h), art, 0, true);
	} else if (icon) {
		f32 s = roundf(r.w * 0.44f);
		ui_shadow(ui, rect(r.x + (r.w - s) / 2 + s * 0.1f, mid - s / 2 + s * 0.1f, s * 0.8f, s * 0.8f), s * 0.2f, 18, 8, color_hex("#00000070"));
		ui_image(ui, rect(roundf(r.x + (r.w - s) / 2), roundf(mid - s / 2), s, s), icon, 0, false);
	} else {
		char m[4] = { name[0] ? name[0] : '?', 0 };
		if (m[0] >= 'a' && m[0] <= 'z')
			m[0] = (char)(m[0] - 32);
		ui_text_in(ui, ui_font(ui, r.w * 0.36f, 700), rect(r.x, mid - r.w * 0.25f, r.w, r.w * 0.5f), m, color_hex("#ffffffe0"), ALIGN_CENTER);
	}
	if (r.h >= 140)
		ui_text_wrapped(ui, ui_font(ui, maxf(12, r.w * 0.095f), 700), rect(r.x + 12, r.y + r.h * 0.64f, r.w - 24, r.h * 0.3f), name,
				COLOR_WHITE, ALIGN_CENTER);
	ui_border(ui, r, radius, 1, color_hex("#ffffff30"));
}

/* A program's cover: art from Steam or the person; else one made of its icon, or initial, and its name. */
static void draw_cover(Ui *ui, Rect r, const App *a, f32 radius)
{
	Cover *c = cover_slot(a->id);
	if (!c->loaded) {
		c->loaded = true;
		char *art = cover_path(a->id, ""), *icon = cover_path(a->id, "-icon.png");
		bool has_art = str_starts(a->cover, "steam:") || str_starts(a->cover, "wide:") || str_eq(a->cover, "file");
		c->art = has_art ? image_load_thumb(art, 900) : NULL;
		c->icon = image_load(icon);
		mem_free(art);
		mem_free(icon);
	}
	draw_cover_img(ui, r, c->art, str_starts(a->cover, "wide:"), c->icon, a->name, 0, radius);
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
	Rect top = cut_top(&c, 108);
	draw_cover(ui, rect(top.x, top.y, 72, 108), a, 8);
	Rect nm = rect(top.x + 86, top.y + 4, top.w - 86, 32);
	int ev = ui_entry(ui, nm, "bridge-name", &name_edit, "Name", ENTRY_FLAT);
	if (ev & (ENTRY_CHANGED | ENTRY_SUBMIT) && sb_str(&name_edit)[0]) {
		str_copy(a->name, sb_str(&name_edit), sizeof a->name);
		/* a new name: look for its cover again, unless the person chose one */
		if (ev & ENTRY_SUBMIT && !str_eq(a->cover, "file"))
			a->cover[0] = 0;
		save_app(a);
	}
	ui_text_in(ui, ui_font(ui, 11, 400), rect(top.x + 88, top.y + 38, top.w - 88, 18), a->exe, t->text_faint, ALIGN_START);
	if (ui_button_flat(ui, rect(top.x + 82, top.y + 66, 130, 30), "Change Cover…"))
		ui_file_dialog_open(ui, "bridge-cover", FILE_OPEN, home_dir(), "png,jpg,jpeg", NULL);
	char picked[512];
	if (ui_file_dialog(ui, "bridge-cover", picked, sizeof picked))
		set_cover_file(a, picked);
	cut_top(&c, 12);
	bool playing = play_pid > 0 && str_eq(play_id, a->id);
	Rect btns = cut_top(&c, 36);
	if (playing) {
		if (ui_button_destructive(ui, cut_left(&btns, 130), "Stop")) {
			sys_kill(-play_pid, SIGTERM);
			str_copy(bridge_msg, "Stopping…", sizeof bridge_msg);
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
	bool files = a->files == 1;
	if (option_row(ui, &c, "Your files and USB drives", "Documents, Downloads, Pictures and the rest are your real ones", &files)) {
		a->files = files;
		char bottle[600];
		bottle_of(a, bottle, sizeof bottle);
		share_files(a, bottle);
		ch = true;
	}
	if (ch)
		save_app(a);
	cut_top(&c, 8);
	char rt[600];
	bool eac = steam_runtime("Proton EasyAntiCheat Runtime", rt, sizeof rt), be = steam_runtime("Proton BattlEye Runtime", rt, sizeof rt);
	char ac[240];
	fmt(ac, sizeof ac, "Anti-cheat: Easy Anti-Cheat %s, BattlEye %s. They work in games whose makers allow Linux.",
	    eac ? "ready" : "not installed", be ? "ready" : "not installed");
	if (str_find_nocase(a->name, "fortnite") || str_find_nocase(a->exe, "fortnite")) {
		/* Epic keeps Easy Anti-Cheat's Linux support off for Fortnite: it can't play here, only in the cloud */
		ui_text_wrapped(ui, ui_font(ui, 11, 400), cut_top(&c, 34),
				"Fortnite's anti-cheat turns every Linux system away, by Epic's choice. Play it free in the cloud instead.",
				t->danger, ALIGN_START);
		if (ui_button(ui, cut_top(&c, 32), "Play Fortnite in the Cloud"))
			tab = 2;
	} else {
		ui_text_wrapped(ui, ui_font(ui, 11, 400), cut_top(&c, 34), ac, t->text_dim, ALIGN_START);
	}
	Rect bottom = cut_bottom(&c, 34);
	if (ui_button_flat(ui, cut_right(&bottom, 150), "Remove from Bridge")) {
		char *path = str_fmt("%s/apps/%s.conf", bridge_base, a->id);
		sys_unlink(path);
		mem_free(path);
		path = cover_path(a->id, "");
		sys_unlink(path);
		mem_free(path);
		path = cover_path(a->id, "-icon.png");
		sys_unlink(path);
		mem_free(path);
		forget_cover(a->id);
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
	Rect head = cut_top(&win, 46);
	Rect tools = ui_headerbar(ui, head, "", 0);
	static const char *const tabs[] = { "Windows", "Steam", "Cloud" };
	if (ui_segmented(ui, center_in(head, 300, 32), tabs, 3, &tab))
		bridge_msg[0] = 0;
	reap_started();
	cover_tick(ui);
	/* what's playing ended */
	if (play_pid > 0) {
		int st;
		if (proc_wait(play_pid, &st, true) == play_pid) {
			play_pid = -1;
			bridge_msg[0] = 0;
			/* a bottle the first start made: its folders now */
			for (int k = 0; k < napps; k++)
				if (str_eq(apps[k].id, play_id)) {
					char bottle[600];
					bottle_of(&apps[k], bottle, sizeof bottle);
					share_files(&apps[k], bottle);
				}
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
	if (tab == 1) {
		steam_tools(ui, &tools);
		steam_tab(ui, pad2(win, 24, 18));
		return;
	}
	if (tab == 2) {
		cloud_tab(ui, pad2(win, 24, 18));
		return;
	}
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
					char *path = str_fmt("%s/apps/%s.conf", bridge_base, apps[k].id);
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
	/* the library: covers in rows, as on a shelf */
	f32 tw = 150, ch = 225, th = ch + 48, gap = 22;
	int per = MAX(1, (int)((b.w + gap) / (tw + gap)));
	f32 y = ui_scroll_begin(ui, b, "bridge-lib");
	for (int i = 0; i < napps; i++) {
		Rect cell = rect(b.x + (f32)(i % per) * (tw + gap), y + 8 + (f32)(i / per) * (th + gap), tw, th);
		ui_push_id_str(ui, apps[i].id);
		UiInteract it = ui_interact(ui, ui_id(ui, "tile"), cell);
		/* the cover rises a little under the pointer */
		f32 lift = ui_anim(ui, ui_id(ui, "lift"), it.hovered, 14) * 5;
		Rect art = rect(cell.x, cell.y - lift, tw, ch);
		ui_shadow(ui, art, 12, 14 + lift * 3, 4 + lift, color_hex("#00000058"));
		draw_cover(ui, art, &apps[i], 12);
		if (sel == i)
			ui_border(ui, rect_inset(art, -4, -4), 15, 2.5f, t->accent);
		bool playing = play_pid > 0 && str_eq(play_id, apps[i].id);
		ui_text_in(ui, ui_font(ui, 13, 600), rect(cell.x, cell.y + ch + 8, tw, 20), apps[i].name, t->text, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 11, 400), rect(cell.x, cell.y + ch + 27, tw, 16), playing ? "Playing" : looks_like_installer(apps[i].exe) ? "Installer" : "Windows",
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
	ui_scroll_end(ui, 8 + (f32)((napps + per - 1) / per) * (th + gap));
	if (bridge_msg[0])
		ui_text(ui, ui_font(ui, 12, 400), b.x, b.y + b.h - 4, bridge_msg, t->text_dim);
	Rect c;
	if (ui_dialog_begin(ui, "bridge-files", 460, 230, &c)) {
		App *a = ask_for >= 0 && ask_for < napps ? &apps[ask_for] : NULL;
		char title[160];
		fmt(title, sizeof title, "Let %s use your files?", a ? a->name : "it");
		ui_heading(ui, cut_top(&c, 28), title);
		cut_top(&c, 8);
		ui_text_wrapped(ui, ui_font(ui, 13, 400), cut_top(&c, 96),
				"Windows programs run in a bottle of their own. Allowed, this one opens and saves in your Documents, "
				"Downloads, Pictures, Music, Videos and Desktop, and sees USB drives as D:, E:... Kept apart, it has "
				"folders of its own. You can change this in its options.",
				t->text_dim, ALIGN_START);
		Rect btn = cut_bottom(&c, 34);
		int answer = -1;
		if (ui_button_suggested(ui, cut_right(&btn, 110), "Allow"))
			answer = 1;
		cut_right(&btn, 8);
		if (ui_button(ui, cut_right(&btn, 130), "Keep It Apart"))
			answer = 0;
		if (answer >= 0) {
			ui_dialog_close(ui);
			if (a) {
				a->files = answer;
				save_app(a);
				play(ui, ask_for);
			}
			ask_for = -1;
		}
		ui_dialog_end(ui);
	}
}

static const char *open_arg;

static void init(Ui *ui)
{
	UNUSED(ui);
	fmt(bridge_base, sizeof bridge_base, "%s/.local/share/zurbax/bridge", home_dir());
	char *dirs[] = { path_in("apps"), path_in("bottles"), path_in("proton"), path_in("runtime"), path_in("downloads"),
			path_in("covers") };
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
