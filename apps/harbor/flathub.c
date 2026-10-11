/*
 * Flathub in Harbor: the apps of Flathub (flathub.org), installed with
 * Flatpak into the person's own Flatpak installation in the Linux layer
 * (layerrun runs flatpak there, with Flathub already added as a remote).
 *
 * The app lists, details and pictures come from Flathub's web API; what is
 * installed, the updates, and the installing itself come from flatpak.
 * Everything slow runs on threads. The lists and the app table are behind
 * one lock, which the window holds while it draws them (flathub_lock); the
 * threads hold it only to swap results in, never across a download.
 */
#include "flathub.h"
#include "zn.h"

#define API "https://flathub.org/api/v2"
#define MAX_LIST 60

static struct {
	Ui *ui;
	char cache[512];            /* icons, by app id */
	Mutex lock;                 /* the lists and the app table */
	Map apps;                   /* id -> FlatApp* (kept for the life of the window) */
	FlatApp *popular[MAX_LIST], *results[MAX_LIST], *installed[256], *gaming[16];
	int npopular, nresults, ninstalled, ngaming, updates;
	int loading, searching;     /* atomic: loading counts the threads at it */
	int list_loading;           /* atomic: Flathub's list is on its way */
	int available;              /* -1 until known */
	char error[200];
	char query[128];
	int search_gen;             /* atomic: a newer search makes older answers stale */
	/* icons to fetch, newest first */
	Mutex qlock;
	FlatApp *queue[256];
	int nqueue;
	int icon_worker;            /* atomic: how many fetch icons */
	/* the running job */
	int busy, job_done;         /* atomic */
	FlatApp *job;               /* NULL: updating everything */
	FlatApp all;                /* the progress of "update everything" */
	int job_op;
	char job_result[200];
} F = { .available = -1 };

static const char *layer_dir(void)
{
	const char *l = env_get("ZURBAX_LAYER");
	return l && *l ? l : "/data/layers/steam";
}

void flathub_lock(void)
{
	mutex_lock(&F.lock);
}

void flathub_unlock(void)
{
	mutex_unlock(&F.lock);
}

/* The table's entry for an app (under the lock). */
static FlatApp *app_for(const char *id)
{
	FlatApp *a = map_get(&F.apps, id);
	if (!a) {
		a = NEW(FlatApp);
		str_copy(a->id, id, sizeof a->id);
		a->fraction = -1;
		map_set(&F.apps, id, a);
	}
	return a;
}

static void set_error(const char *e)
{
	mutex_lock(&F.lock);
	str_copy(F.error, e, sizeof F.error);
	mutex_unlock(&F.lock);
}

static Json *get_json(const char *url, const char *post)
{
	HttpOptions o = { .timeout_ms = 20000, .accept = "application/json" };
	if (post) {
		o.method = "POST";
		o.headers = "Content-Type: application/json\r\n";
		o.body = post;
		o.body_len = strlen(post);
	}
	HttpResponse *r = http_request(url, &o);
	Json *j = r->status == 200 && r->body ? json_parse((const char *)r->body, r->body_len) : NULL;
	if (!j)
		set_error(r->status ? "Flathub didn't answer as expected." : "Couldn't reach Flathub. Check the connection.");
	http_response_free(r);
	return j;
}

/* Flathub's main categories, as people say them. */
static const char *category_name(const char *c)
{
	static const char *const names[][2] = {
		{ "audiovideo", "Audio & Video" }, { "development", "Developer Tools" }, { "education", "Education" },
		{ "game", "Games" }, { "graphics", "Graphics & Photos" }, { "network", "Internet" }, { "office", "Office" },
		{ "science", "Science" }, { "system", "System" }, { "utility", "Utilities" },
	};
	for (usize i = 0; i < ARRAY_LEN(names); i++)
		if (str_eq_nocase(c, names[i][0]))
			return names[i][1];
	return c;
}

static void copy_str(char *dst, usize cap, const Json *j)
{
	const char *s = json_str(j, NULL);
	if (s && *s)
		str_copy(dst, s, cap);
}

/* An app as Flathub's lists and search describe it (under the lock). */
static FlatApp *from_hit(const Json *h)
{
	const char *id = json_str(json_get(h, "app_id"), "");
	if (!*id)
		return NULL;
	FlatApp *a = app_for(id);
	copy_str(a->name, sizeof a->name, json_get(h, "name"));
	copy_str(a->summary, sizeof a->summary, json_get(h, "summary"));
	copy_str(a->developer, sizeof a->developer, json_get(h, "developer_name"));
	copy_str(a->license, sizeof a->license, json_get(h, "project_license"));
	copy_str(a->icon_url, sizeof a->icon_url, json_get(h, "icon"));
	Json *cat = json_get(h, "main_categories");
	if (cat && cat->type == JSON_ARRAY)
		cat = json_at(cat, 0);
	if (json_str(cat, NULL))
		str_copy(a->category, category_name(json_str(cat, "")), sizeof a->category);
	a->verified = json_bool(json_get(h, "verification_verified"), a->verified);
	if (!a->name[0])
		str_copy(a->name, id, sizeof a->name);
	return a;
}

/* flatpak ARGS... in the Linux session; its output. */
static char *flatpak(char **args, int *status)
{
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[16];
	int n = 0;
	argv[n++] = layerrun;
	argv[n++] = "flatpak";
	for (int i = 0; args[i] && n < 15; i++)
		argv[n++] = args[i];
	argv[n] = NULL;
	char *out = proc_run_capture(argv, status);
	mem_free(layerrun);
	return out;
}

/* ---- What is installed ------------------------------------------------------------------------- */

static void fill_from_appstream(FlatApp *a, const Json *j);

static void read_installed(void)
{
	int st = -1, st2 = -1;
	char *list_args[] = { "list", "--user", "--app", "--columns=application,version", NULL };
	char *out = flatpak(list_args, &st);
	char *upd_args[] = { "remote-ls", "--user", "--updates", "--app", "--columns=application", NULL };
	char *upd = st == 0 ? flatpak(upd_args, &st2) : NULL;
	FlatApp *unknown[32];
	int nunknown = 0;
	mutex_lock(&F.lock);
	for (int i = 0; i < F.ninstalled; i++)
		F.installed[i]->installed[0] = 0, F.installed[i]->update = false;
	F.ninstalled = 0;
	F.updates = 0;
	for (char *line = st == 0 ? out : NULL; line && *line;) {
		char *nl = (char *)str_chr(line, '\n');
		if (nl)
			*nl = 0;
		char *tab = (char *)str_chr(line, '\t');
		if (tab)
			*tab = 0;
		char *id = str_trim(line);
		/* app ids have dots and no spaces (a heading wouldn't) */
		if (str_chr(id, '.') && !str_chr(id, ' ') && F.ninstalled < (int)ARRAY_LEN(F.installed)) {
			FlatApp *a = app_for(id);
			str_copy(a->installed, tab && *str_trim(tab + 1) ? str_trim(tab + 1) : "installed", sizeof a->installed);
			if (!a->name[0] && nunknown < (int)ARRAY_LEN(unknown))
				unknown[nunknown++] = a;
			F.installed[F.ninstalled++] = a;
		}
		line = nl ? nl + 1 : NULL;
	}
	for (char *line = st2 == 0 ? upd : NULL; line && *line;) {
		char *nl = (char *)str_chr(line, '\n');
		if (nl)
			*nl = 0;
		FlatApp *a = map_get(&F.apps, str_trim(line));
		if (a && a->installed[0] && !a->update)
			a->update = true, F.updates++;
		line = nl ? nl + 1 : NULL;
	}
	mutex_unlock(&F.lock);
	mem_free(out);
	mem_free(upd);
	/* apps installed some other way (or long ago): their names from Flathub */
	for (int i = 0; i < nunknown; i++) {
		char url[256];
		fmt(url, sizeof url, API "/appstream/%s", unknown[i]->id);
		Json *j = get_json(url, NULL);
		if (j) {
			mutex_lock(&F.lock);
			fill_from_appstream(unknown[i], j);
			mutex_unlock(&F.lock);
			json_free(j);
		}
	}
}

static void installed_thread(void *arg);

/* Zurbax's picks for playing: launchers for every store, Windows games,
 * Proton versions, emulators, and streaming games to and from this PC */
static const char *const GAMING[] = {
	"com.heroicgameslauncher.hgl", "net.lutris.Lutris", "com.usebottles.bottles", "com.vysp3r.ProtonPlus",
	"net.davidotek.pupgui2", "org.libretro.RetroArch", "org.prismlauncher.PrismLauncher",
	"dev.lizardbyte.app.Sunshine", "com.moonlight_stream.Moonlight",
};

static void gaming_thread(void *arg)
{
	UNUSED(arg);
	for (usize i = 0; i < ARRAY_LEN(GAMING); i++) {
		char url[256];
		fmt(url, sizeof url, API "/appstream/%s", GAMING[i]);
		Json *j = get_json(url, NULL);
		if (!j)
			continue;
		mutex_lock(&F.lock);
		FlatApp *a = app_for(GAMING[i]);
		fill_from_appstream(a, j);
		str_copy(a->category, "Games", sizeof a->category);
		F.gaming[F.ngaming++] = a;
		mutex_unlock(&F.lock);
		json_free(j);
		ui_wake(F.ui);
	}
	atomic_add(&F.loading, -1);
}

static void load_thread(void *arg)
{
	UNUSED(arg);
	/* what's installed comes from flatpak, which can take a while to start:
	 * on its own thread, while Flathub's list loads */
	atomic_add(&F.loading, 1);
	thread_spawn(installed_thread, NULL);
	if (!F.ngaming) {
		atomic_add(&F.loading, 1);
		thread_spawn(gaming_thread, NULL);
	}
	Json *j = get_json(API "/collection/popular?page=1&per_page=60", NULL);
	if (j) {
		Json *hits = json_get(j, "hits");
		mutex_lock(&F.lock);
		F.npopular = 0;
		for (usize i = 0; hits && i < hits->count && F.npopular < MAX_LIST; i++) {
			FlatApp *a = from_hit(hits->items[i]);
			if (a)
				F.popular[F.npopular++] = a;
		}
		F.error[0] = 0;
		mutex_unlock(&F.lock);
		json_free(j);
	}
	atomic_add(&F.loading, -1);
	atomic_store(&F.list_loading, 0);
	ui_wake(F.ui);
}

static void installed_thread(void *arg)
{
	UNUSED(arg);
	read_installed();
	atomic_add(&F.loading, -1);
	ui_wake(F.ui);
}

bool flathub_available(void)
{
	if (F.available < 0) {
		char *fp = str_fmt("%s/usr/bin/flatpak", layer_dir());
		F.available = file_exists(fp);
		mem_free(fp);
	}
	return F.available == 1;
}

void flathub_refresh(void)
{
	if (!flathub_available() || atomic_load(&F.loading))
		return;
	atomic_store(&F.loading, 1);
	atomic_store(&F.list_loading, 1);
	thread_spawn(load_thread, NULL);
}

void flathub_init(Ui *ui, const char *cache_dir)
{
	F.ui = ui;
	fmt(F.cache, sizeof F.cache, "%s/flathub", cache_dir);
	dir_make(F.cache, 0700);
	flathub_refresh();
}

/* ---- Search ----------------------------------------------------------------------------------- */

static void search_thread(void *arg)
{
	int gen = (int)(isize)arg;
	/* typing: wait for a pause */
	sleep_ms(300);
	if (gen != atomic_load(&F.search_gen))
		return;
	char q[128];
	mutex_lock(&F.lock);
	str_copy(q, F.query, sizeof q);
	mutex_unlock(&F.lock);
	StrBuf body = { 0 };
	sb_adds(&body, "{\"query\":");
	sb_json_string(&body, q);
	sb_adds(&body, ",\"filters\":[]}");
	Json *j = get_json(API "/search", sb_str(&body));
	sb_free(&body);
	if (gen == atomic_load(&F.search_gen)) {
		Json *hits = json_get(j, "hits");
		mutex_lock(&F.lock);
		F.nresults = 0;
		for (usize i = 0; hits && i < hits->count && F.nresults < MAX_LIST; i++) {
			const char *type = json_str(json_get(hits->items[i], "type"), "desktop-application");
			if (!str_eq(type, "desktop-application"))
				continue;
			FlatApp *a = from_hit(hits->items[i]);
			if (a)
				F.results[F.nresults++] = a;
		}
		mutex_unlock(&F.lock);
		atomic_store(&F.searching, 0);
		ui_wake(F.ui);
	}
	json_free(j);
}

void flathub_search(const char *query)
{
	int gen = atomic_add(&F.search_gen, 1);
	mutex_lock(&F.lock);
	str_copy(F.query, query, sizeof F.query);
	if (!*query)
		F.nresults = 0;
	mutex_unlock(&F.lock);
	if (!*query || !flathub_available()) {
		atomic_store(&F.searching, 0);
		return;
	}
	atomic_store(&F.searching, 1);
	thread_spawn(search_thread, (void *)(isize)gen);
}

/* ---- Details ---------------------------------------------------------------------------------- */

/* Flathub's descriptions are a little HTML: paragraphs and lists. */
static char *html_text(const char *h)
{
	StrBuf sb = { 0 };
	bool space = false;
	for (const char *p = h; *p;) {
		if (*p == '<') {
			const char *e = str_chr(p, '>');
			if (!e)
				break;
			const char *tag = p + 1 + (p[1] == '/');
			bool block = str_starts(tag, "p") || str_starts(tag, "ul") || str_starts(tag, "ol");
			if (block && sb.len && !str_ends(sb_str(&sb), "\n\n"))
				sb_adds(&sb, str_ends(sb_str(&sb), "\n") ? "\n" : "\n\n");
			else if (str_starts(p, "<li"))
				sb_adds(&sb, sb.len && !str_ends(sb_str(&sb), "\n") ? "\n• " : "• ");
			else if (str_starts(tag, "br"))
				sb_addc(&sb, '\n');
			p = e + 1;
			space = false;
			continue;
		}
		if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') {
			space = true;
			p++;
			continue;
		}
		if (space && sb.len && !str_ends(sb_str(&sb), "\n") && !str_ends(sb_str(&sb), " "))
			sb_addc(&sb, ' ');
		space = false;
		static const char *const ents[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
						      { "&#39;", "'" }, { "&apos;", "'" }, { "&nbsp;", " " } };
		bool ent = false;
		for (usize i = 0; *p == '&' && i < ARRAY_LEN(ents) && !ent; i++)
			if (str_starts(p, ents[i][0])) {
				sb_adds(&sb, ents[i][1]);
				p += strlen(ents[i][0]);
				ent = true;
			}
		if (!ent)
			sb_addc(&sb, *p++);
	}
	while (sb.len && (sb.data[sb.len - 1] == '\n' || sb.data[sb.len - 1] == ' '))
		sb.data[--sb.len] = 0;
	return sb_take(&sb);
}

static void fill_from_appstream(FlatApp *a, const Json *j)
{
	copy_str(a->name, sizeof a->name, json_get(j, "name"));
	copy_str(a->summary, sizeof a->summary, json_get(j, "summary"));
	copy_str(a->developer, sizeof a->developer, json_get(j, "developer_name"));
	copy_str(a->license, sizeof a->license, json_get(j, "project_license"));
	if (!a->icon_url[0])
		copy_str(a->icon_url, sizeof a->icon_url, json_get(j, "icon"));
	copy_str(a->version, sizeof a->version, json_path(j, "releases.0.version"));
	if (!a->name[0])
		str_copy(a->name, a->id, sizeof a->name);
}

static Canvas *fetch_image(const char *url)
{
	if (!*url)
		return NULL;
	HttpOptions o = { .timeout_ms = 30000 };
	HttpResponse *r = http_request(url, &o);
	Canvas *c = r->status == 200 && r->body ? image_decode(r->body, r->body_len) : NULL;
	http_response_free(r);
	return c;
}

static void details_thread(void *arg)
{
	FlatApp *a = arg;
	char url[256];
	fmt(url, sizeof url, API "/appstream/%s", a->id);
	Json *j = get_json(url, NULL);
	if (!j) {
		atomic_store(&a->details, 3);
		ui_wake(F.ui);
		return;
	}
	fmt(url, sizeof url, API "/summary/%s", a->id);
	Json *s = get_json(url, NULL);
	char *desc = html_text(json_str(json_get(j, "description"), ""));
	/* the screenshot nearest 752 pixels wide */
	char shot[400] = "";
	Json *sizes = json_path(j, "screenshots.0.sizes");
	f64 best = 1e9;
	for (usize i = 0; sizes && i < sizes->count; i++) {
		f64 w = str_to_float(json_str(json_get(sizes->items[i], "width"), "0"), NULL);
		if (w > 0 && fabs(w - 752) < best) {
			best = fabs(w - 752);
			str_copy(shot, json_str(json_get(sizes->items[i], "src"), ""), sizeof shot);
		}
	}
	Canvas *pic = fetch_image(shot);
	mutex_lock(&F.lock);
	fill_from_appstream(a, j);
	str_copy(a->shot_url, shot, sizeof a->shot_url);
	mem_free(a->description);
	a->description = desc;
	if (pic)
		a->shot = pic;
	if (s) {
		a->download_size = (u64)json_num(json_get(s, "download_size"), 0);
		a->installed_size = (u64)json_num(json_get(s, "installed_size"), 0);
		const char *rt = json_str(json_path(s, "metadata.runtime"), "");
		/* "org.gnome.Platform/x86_64/51" -> "org.gnome.Platform 51" */
		char tmp[96];
		str_copy(tmp, rt, sizeof tmp);
		char *first = (char *)str_chr(tmp, '/'), *last = first ? (char *)str_rchr(tmp, '/') : NULL;
		if (first && last && last != first) {
			*first = 0;
			fmt(a->runtime, sizeof a->runtime, "%s %s", tmp, last + 1);
		} else {
			str_copy(a->runtime, tmp, sizeof a->runtime);
		}
	}
	mutex_unlock(&F.lock);
	json_free(j);
	json_free(s);
	atomic_store(&a->details, 2);
	ui_wake(F.ui);
}

void flathub_details(FlatApp *a)
{
	if (a && __atomic_exchange_n(&a->details, 1, __ATOMIC_ACQ_REL) != 1)
		thread_spawn(details_thread, a);
}

/* ---- Icons ------------------------------------------------------------------------------------ */

static Canvas *load_icon(FlatApp *a)
{
	char path[700];
	fmt(path, sizeof path, "%s/%s.png", F.cache, a->id);
	usize len;
	u8 *png = (u8 *)file_read(path, &len);
	if (!png && a->icon_url[0]) {
		HttpResponse *r = http_get(a->icon_url);
		if (r->status == 200 && r->body) {
			file_write(path, r->body, r->body_len);
			png = r->body, len = r->body_len;
			r->body = NULL;
		}
		http_response_free(r);
	}
	Canvas *c = png ? image_decode(png, len) : NULL;
	mem_free(png);
	return c;
}

static void icon_thread(void *arg)
{
	UNUSED(arg);
	for (int idle = 0; idle < 100;) {
		FlatApp *a = NULL;
		mutex_lock(&F.qlock);
		if (F.nqueue)
			a = F.queue[--F.nqueue];
		mutex_unlock(&F.qlock);
		if (!a) {
			sleep_ms(30);
			idle++;
			continue;
		}
		idle = 0;
		a->icon = load_icon(a);
		atomic_store(&a->icon_state, 2);
		ui_wake(F.ui);
	}
	atomic_add(&F.icon_worker, -1);
}

void flathub_icon(Ui *ui, Rect r, FlatApp *a)
{
	if (atomic_load(&a->icon_state) == 2 && a->icon) {
		ui_image(ui, r, a->icon, 0, false);
		return;
	}
	ui_round(ui, r, r.w * 0.22f, ui->theme.hover);
	if (atomic_load(&a->icon_state) == 0 && (a->icon_url[0] || a->installed[0])) {
		mutex_lock(&F.qlock);
		if (F.nqueue < (int)ARRAY_LEN(F.queue)) {
			atomic_store(&a->icon_state, 1);
			F.queue[F.nqueue++] = a;
		}
		mutex_unlock(&F.qlock);
		/* three at a time: each is a short download of its own */
		int w = atomic_load(&F.icon_worker);
		if (w < 3 && __atomic_compare_exchange_n(&F.icon_worker, &w, w + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			thread_spawn(icon_thread, NULL);
	}
}

/* ---- Installing, removing, updating ----------------------------------------------------------- */

/* flatpak's progress, as it prints it: "Installing 1/2… 45%" */
static void progress_line(FlatApp *a, char *seg)
{
	char *t = str_trim(seg);
	if (!*t)
		return;
	const char *pct = NULL;
	for (const char *p = t; (p = str_chr(p, '%')); p++)
		pct = p;
	if (pct) {
		const char *d = pct;
		while (d > t && d[-1] >= '0' && d[-1] <= '9')
			d--;
		if (d < pct)
			a->fraction = (f32)str_to_int(d, NULL) / 100.0f;
	}
	if (str_starts(t, "Installing") || str_starts(t, "Updating") || str_starts(t, "Uninstalling")) {
		str_copy(a->progress, t, sizeof a->progress);
		/* up to the dots: the bar and the numbers come from the fraction */
		char *dots = (char *)str_find(a->progress, "…");
		if (dots)
			dots[strlen("…")] = 0;
	}
	if (str_starts(t, "error:") || str_starts(t, "Error:"))
		str_copy(F.job_result, t, sizeof F.job_result);
}

static void job_thread(void *arg)
{
	UNUSED(arg);
	FlatApp *a = F.job ? F.job : &F.all;
	char *layerrun = zurbax_path("bin/layerrun");
	char *argv[12];
	int n = 0;
	argv[n++] = layerrun;
	argv[n++] = "flatpak";
	if (F.job_op == FLAT_INSTALL)
		argv[n++] = "install";
	else if (F.job_op == FLAT_REMOVE)
		argv[n++] = "uninstall";
	else
		argv[n++] = "update";
	argv[n++] = "--user";
	argv[n++] = "--noninteractive";
	argv[n++] = "-y";
	if (F.job_op == FLAT_INSTALL)
		argv[n++] = "flathub";
	if (F.job)
		argv[n++] = F.job->id;
	argv[n] = NULL;
	F.job_result[0] = 0;
	int fds[2] = { -1, -1 }, status = -1;
	int pid = sys_pipe2(fds, O_CLOEXEC) < 0 ? -1 : 0;
	if (pid == 0) {
		SpawnOpts o = { .stdin_fd = -1, .stdout_fd = fds[1], .stderr_fd = fds[1] };
		pid = proc_spawn(layerrun, argv, NULL, &o);
		sys_close(fds[1]);
		char buf[2048], seg[512];
		usize len = 0;
		for (;;) {
			long got = sys_read(fds[0], buf, sizeof buf);
			if (got == -EINTR)
				continue;
			if (got <= 0)
				break;
			for (long i = 0; i < got; i++) {
				if (buf[i] == '\n' || buf[i] == '\r') {
					seg[len] = 0;
					progress_line(a, seg);
					len = 0;
				} else if (len + 1 < sizeof seg) {
					seg[len++] = buf[i];
				}
			}
			ui_wake(F.ui);
		}
		seg[len] = 0;
		progress_line(a, seg);
		sys_close(fds[0]);
		if (pid > 0)
			proc_wait(pid, &status, false);
	}
	mem_free(layerrun);
	if (status == 0)
		str_copy(F.job_result, "done", sizeof F.job_result);
	else if (!F.job_result[0])
		str_copy(F.job_result, pid > 0 ? "Flatpak couldn't finish." : "Flatpak couldn't be started.", sizeof F.job_result);
	atomic_store(&F.job_done, 1);
	ui_wake(F.ui);
}

void flathub_job(FlatApp *a, int op)
{
	if (__atomic_exchange_n(&F.busy, 1, __ATOMIC_ACQ_REL))
		return;
	FlatApp *t = a ? a : &F.all;
	t->op = op;
	t->error[0] = 0;
	t->progress[0] = 0;
	t->fraction = -1;
	F.job = a;
	F.job_op = op;
	thread_spawn(job_thread, NULL);
}

void flathub_poll(Ui *ui)
{
	if (!atomic_load(&F.job_done))
		return;
	atomic_store(&F.job_done, 0);
	FlatApp *a = F.job ? F.job : &F.all;
	int op = F.job_op;
	a->op = FLAT_IDLE;
	a->fraction = -1;
	if (str_eq(F.job_result, "done")) {
		if (op == FLAT_INSTALL && F.job) {
			char body[200];
			fmt(body, sizeof body, "%s is in your Applications.", a->name);
			ui_notify(ui, "App installed", body, "harbor");
			sound_play("complete");
		}
	} else {
		const char *why = F.job_result;
		if (str_eq_nocase_n(why, "error: ", 7))
			why += 7;
		fmt(a->error, sizeof a->error, "%c%s", to_upper(why[0]), why[0] ? why + 1 : "");
		sound_play("error");
	}
	atomic_store(&F.busy, 0);
	/* what's installed changed */
	atomic_add(&F.loading, 1);
	thread_spawn(installed_thread, NULL);
}

/* ---- For the window (under flathub_lock) ------------------------------------------------------ */

FlatApp **flathub_popular(int *n)
{
	*n = F.npopular;
	return F.popular;
}

FlatApp **flathub_gaming(int *n)
{
	*n = F.ngaming;
	return F.gaming;
}

FlatApp **flathub_results(int *n)
{
	*n = F.nresults;
	return F.results;
}

FlatApp **flathub_installed(int *n)
{
	*n = F.ninstalled;
	return F.installed;
}

FlatApp *flathub_all(void)
{
	return &F.all;
}

bool flathub_loading(void)
{
	return atomic_load(&F.list_loading) != 0;
}

bool flathub_searching(void)
{
	return atomic_load(&F.searching) != 0;
}

bool flathub_busy(void)
{
	return atomic_load(&F.busy) != 0;
}

const char *flathub_error(void)
{
	return F.error;
}

int flathub_updates(void)
{
	return F.updates;
}
