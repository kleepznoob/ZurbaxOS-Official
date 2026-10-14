/*
 * zupdate — keeps Zurbax up to date.
 *
 * Every release is long-term supported (LTS): security updates come every
 * two weeks until OS_SUPPORT_END, each one a newer kernel from Linux's
 * long-term branch, the system rebuilt with any fixes, and the Linux layer's
 * libraries with theirs.
 *
 * Once a day, and whenever Settings asks, it reads the release channel
 * (OS_UPDATE_CHANNEL): "release", a description of the newest update, and
 * "release.sig", its ECDSA P-256 signature, which must come from one of the
 * keys in /system/share/update-keys or the update is ignored. The
 * description reads:
 *
 *     zurbax-update 1
 *     version 2.1.3
 *     date 2026-10-27
 *     until 2028-10-31
 *     base https://archive.org/download/zurbax-2.1.3/
 *     notes Linux 6.18.60, and fixes for OpenSSL and libxml2.
 *     file zurbax.efi SIZE SHA256
 *     file steam.sfs SIZE SHA256
 *
 * A newer update downloads into /data/system/update, every file checked
 * against its size and SHA-256, then goes in place: the system's EFI file on
 * the EFI system partition (the one before it kept as ZURBAX.OLD when there's
 * room) and layer images in /data/layers, only those already installed and
 * only when they changed. It takes effect at the next restart.
 *
 * Settings talks to it over /run/zurbax/update.sock (datagrams):
 *     check | install | auto on|off
 * and reads /run/zurbax/update.status, "key = value" lines:
 *     state idle|checking|available|downloading|ready|current|error|live
 *     version, available, date, notes, until, progress, message, auto, checked
 */
#include "zn.h"
#include "zui.h"

#define CTL_SOCKET "/run/zurbax/update.sock"
#define STATUS_FILE "/run/zurbax/update.status"
#define ESP_DIR "/run/zupdate/esp"
#define ESP_EFI ESP_DIR "/EFI/ZURBAX/ZURBAX.EFI"
#define ESP_OLD ESP_DIR "/EFI/ZURBAX/ZURBAX.OLD"
#define ESP_NEW ESP_DIR "/EFI/ZURBAX/ZURBAX.NEW"
#define ESP_FALLBACK ESP_DIR "/EFI/BOOT/BOOTX64.EFI"
#define ESP_FALLBACK_NEW ESP_DIR "/EFI/BOOT/BOOTX64.NEW"

#define DAY (24 * 3600)

enum { WORK_NONE, WORK_CHECK, WORK_INSTALL };

typedef struct {
	char name[64];
	u64 size;
	u8 sha[32];
} UpdFile;

typedef struct {
	char version[32], date[16], until[16], base[512], notes[600];
	UpdFile files[16];
	int nfiles;
} Release;

/* What the status file says; the worker thread and the main loop share it. */
static struct {
	Mutex lock;
	char state[16];
	char available[32], date[16], until[16], notes[600], message[200];
	f32 progress;
	i64 checked;
} St;

/* under /data (ZURBAX_DATA in development) */
static char data_dir[256], conf_file[300], work_dir[300], record_file[300], layers_dir[300];

static Conf *conf;
static int busy;          /* a worker thread is running */
static int requested;     /* WORK_* the main loop should start next */
static bool live;

/* ---- Status ---------------------------------------------------------------------------------- */

static void write_status(void)
{
	mutex_lock(&St.lock);
	StrBuf s = { 0 };
	sb_fmt(&s, "state = %s\n", St.state);
	sb_fmt(&s, "version = %s\n", OS_FULL_VERSION);
	sb_fmt(&s, "until = %s\n", St.until[0] ? St.until : OS_SUPPORT_END);
	sb_fmt(&s, "auto = %d\n", conf_get_bool(conf, "auto", true) ? 1 : 0);
	sb_fmt(&s, "checked = %lld\n", (long long)St.checked);
	if (St.available[0])
		sb_fmt(&s, "available = %s\ndate = %s\n", St.available, St.date);
	if (St.notes[0])
		sb_fmt(&s, "notes = %s\n", St.notes);
	if (St.message[0])
		sb_fmt(&s, "message = %s\n", St.message);
	sb_fmt(&s, "progress = %.3f\n", (f64)St.progress);
	file_write_atomic(STATUS_FILE, s.data, s.len);
	sys_chmod(STATUS_FILE, 0644);
	sb_free(&s);
	mutex_unlock(&St.lock);
}

static void set_state(const char *state, const char *message)
{
	mutex_lock(&St.lock);
	str_copy(St.state, state, sizeof St.state);
	str_copy(St.message, message ? message : "", sizeof St.message);
	mutex_unlock(&St.lock);
	write_status();
}

/* The settings file changes from both threads. */
static void conf_put(const char *key, const char *value)
{
	mutex_lock(&St.lock);
	if (value)
		conf_set(conf, key, value);
	else
		conf_unset(conf, key);
	conf_save(conf);
	mutex_unlock(&St.lock);
}

static void log_line(const char *f, ...) PRINTF_LIKE(1, 2);
static void log_line(const char *f, ...)
{
	char line[400];
	va_list ap;
	va_start(ap, f);
	int n = fmt(line, sizeof line, "<6>zupdate: ");
	n += fmt_v(line + n, sizeof line - (usize)n, f, ap);
	va_end(ap);
	int k = (int)sys_open("/dev/kmsg", O_WRONLY | O_CLOEXEC, 0);
	if (k >= 0) {
		sys_write(k, line, (usize)MIN(n, (int)sizeof line - 1));
		sys_close(k);
	}
}

/* ---- Versions and the description ----------------------------------------------------------- */

/* "2.1.10" is newer than "2.1.9": numbers compare as numbers. */
static int version_cmp(const char *a, const char *b)
{
	while (*a || *b) {
		i64 x = str_to_int(a, &a), y = str_to_int(b, &b);
		if (x != y)
			return x < y ? -1 : 1;
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
		if (!(*a >= '0' && *a <= '9') && !(*b >= '0' && *b <= '9'))
			break;
	}
	return 0;
}

static bool signature_ok(const void *data, usize len, const u8 *sig, usize slen)
{
	u8 digest[32];
	sha256(data, len, digest);
	char *dir = zurbax_path("share/update-keys");
	DirList keys = { 0 };
	bool ok = false;
	if (dir_list(dir, &keys, false)) {
		vec_each(&keys, e)
		{
			if (!str_ends(e->name, ".pub"))
				continue;
			char *path = path_join(dir, e->name);
			char *hex = file_read(path, NULL);
			mem_free(path);
			u8 point[65];
			char *t = hex ? str_trim(hex) : NULL;
			if (t && strlen(t) == 130 && hex_decode(t, point, 65) && point[0] == 4 &&
			    ecdsa_verify(CURVE_P256, point, 65, digest, 32, sig, slen))
				ok = true;
			mem_free(hex);
			if (ok)
				break;
		}
		dir_list_free(&keys);
	}
	mem_free(dir);
	return ok;
}

static bool parse_release(char *text, Release *r)
{
	memset(r, 0, sizeof *r);
	bool magic = false;
	for (char *line = text; line && *line;) {
		char *nl = (char *)str_chr(line, '\n');
		if (nl)
			*nl = 0;
		char *v = (char *)str_chr(line, ' ');
		if (v) {
			*v++ = 0;
			v = str_trim(v);
			if (str_eq(line, "zurbax-update"))
				magic = str_to_int(v, NULL) == 1;
			else if (str_eq(line, "version"))
				str_copy(r->version, v, sizeof r->version);
			else if (str_eq(line, "date"))
				str_copy(r->date, v, sizeof r->date);
			else if (str_eq(line, "until"))
				str_copy(r->until, v, sizeof r->until);
			else if (str_eq(line, "base"))
				str_copy(r->base, v, sizeof r->base);
			else if (str_eq(line, "notes"))
				str_copy(r->notes, v, sizeof r->notes);
			else if (str_eq(line, "file") && r->nfiles < (int)ARRAY_LEN(r->files)) {
				UpdFile *f = &r->files[r->nfiles];
				const char *q = v;
				usize i = 0;
				while (*q && *q != ' ' && i + 1 < sizeof f->name)
					f->name[i++] = *q++;
				f->name[i] = 0;
				f->size = (u64)str_to_int(q, &q);
				while (*q == ' ')
					q++;
				/* names are plain file names, never paths */
				if (f->name[0] && f->name[0] != '.' && !str_chr(f->name, '/') && f->size && hex_decode(q, f->sha, 32))
					r->nfiles++;
			}
		}
		line = nl ? nl + 1 : NULL;
	}
	/* downloads come over HTTPS (a server on this computer is allowed, for testing) */
	bool base_ok = str_starts(r->base, "https://") || str_starts(r->base, "http://127.0.0.1:");
	return magic && r->version[0] && base_ok && r->nfiles > 0;
}

/* ---- Files ----------------------------------------------------------------------------------- */

static bool hash_file(const char *path, u8 out[32])
{
	int fd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return false;
	Sha256 s;
	sha256_init(&s);
	u8 *buf = mem_alloc(1 << 20);
	long n;
	while ((n = sys_read(fd, buf, 1 << 20)) > 0)
		sha256_update(&s, buf, (usize)n);
	mem_free(buf);
	sys_close(fd);
	sha256_final(&s, out);
	return n == 0;
}

static u64 size_of(const char *path)
{
	struct stat st;
	return sys_stat(path, &st) == 0 ? (u64)st.st_size : 0;
}

/* What's installed, as "name sha" lines: files are hashed once, not every check. */
static bool recorded(const char *name, u8 sha[32])
{
	Conf *c = conf_load(record_file);
	const char *hex = conf_get(c, name, NULL);
	bool ok = hex && hex_decode(hex, sha, 32);
	conf_free(c);
	return ok;
}

static void record(const char *name, const u8 sha[32])
{
	Conf *c = conf_load(record_file);
	char hex[65];
	hex_encode(sha, 32, hex);
	conf_set(c, name, hex);
	conf_save(c);
	conf_free(c);
}

static bool copy_file(const char *from, const char *to)
{
	usize len;
	char *data = file_read(from, &len);
	if (!data)
		return false;
	int fd = (int)sys_open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	bool ok = fd >= 0 && write_all(fd, data, len) && sys_fsync(fd) == 0;
	if (fd >= 0)
		sys_close(fd);
	mem_free(data);
	return ok;
}

/* ---- The EFI system partition ---------------------------------------------------------------- */

/* The one on the disk that holds /data with Zurbax's EFI file in it. */
static bool mount_esp(bool writable)
{
	struct stat st;
	if (sys_stat(data_dir, &st) < 0)
		return false;
	u32 major = (u32)(((st.st_dev >> 8) & 0xfff) | ((st.st_dev >> 32) & ~0xfffu));
	u32 minor = (u32)((st.st_dev & 0xff) | ((st.st_dev >> 12) & ~0xffu));
	char link[64], target[512];
	fmt(link, sizeof link, "/sys/dev/block/%u:%u", major, minor);
	long n = sys_readlink(link, target, sizeof target - 1);
	if (n <= 0)
		return false;
	target[n] = 0;
	/* .../block/DISK/PART: the disk is the folder above the partition */
	char *slash = (char *)str_rchr(target, '/');
	if (!slash)
		return false;
	*slash = 0;
	const char *disk = path_base(target);
	char dir[128];
	fmt(dir, sizeof dir, "/sys/block/%s", disk);
	DirList parts = { 0 };
	if (!dir_list(dir, &parts, false))
		return false;
	dir_make(ESP_DIR, 0700);
	bool found = false;
	vec_each(&parts, e)
	{
		if (found || !str_starts(e->name, disk) || str_eq(e->name, disk))
			continue;
		char dev[96];
		fmt(dev, sizeof dev, "/dev/%s", e->name);
		if (sys_mount(dev, ESP_DIR, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC | (writable ? 0 : MS_RDONLY), NULL) < 0)
			continue;
		if (file_exists(ESP_EFI))
			found = true;
		else
			sys_umount(ESP_DIR, 0);
	}
	dir_list_free(&parts);
	return found;
}

static void unmount_esp(void)
{
	sys_sync();
	sys_umount(ESP_DIR, 0);
}

/* The new system goes in place, the running one kept as ZURBAX.OLD when the
 * partition has room for both; the firmware's fallback copy is replaced too,
 * but only if it was Zurbax's (it's Windows' on a shared partition). */
static bool install_efi(const char *path, const u8 sha[32])
{
	if (!mount_esp(true))
		return false;
	u8 cur[32], fb[32];
	bool ours_fallback = hash_file(ESP_EFI, cur) && hash_file(ESP_FALLBACK, fb) && memcmp(cur, fb, 32) == 0;
	u64 size = size_of(path);
	sys_unlink(ESP_OLD);
	sys_unlink(ESP_NEW);
	struct statfs fs;
	u64 room = sys_statfs(ESP_DIR, &fs) == 0 ? fs.f_bavail * (u64)fs.f_bsize : 0;
	bool keep = room >= size + (1u << 20);
	if (!keep)
		sys_unlink(ESP_EFI); /* only room for one */
	bool ok = copy_file(path, keep ? ESP_NEW : ESP_EFI);
	if (ok && keep)
		ok = sys_rename(ESP_EFI, ESP_OLD) == 0 && sys_rename(ESP_NEW, ESP_EFI) == 0;
	u8 check[32];
	ok = ok && hash_file(ESP_EFI, check) && memcmp(check, sha, 32) == 0;
	if (ok && ours_fallback) {
		room = sys_statfs(ESP_DIR, &fs) == 0 ? fs.f_bavail * (u64)fs.f_bsize : 0;
		if (room >= size + (1u << 20)) {
			if (copy_file(path, ESP_FALLBACK_NEW))
				sys_rename(ESP_FALLBACK_NEW, ESP_FALLBACK);
		} else {
			/* no room for a second copy: the old one would start the old system */
			sys_unlink(ESP_FALLBACK);
			copy_file(path, ESP_FALLBACK);
		}
	}
	unmount_esp();
	return ok;
}

/* ---- Downloads ------------------------------------------------------------------------------- */

typedef struct {
	int fd;
	Sha256 sha;
	u64 got, done, all;
} Dl;

static bool dl_sink(void *user, const u8 *data, usize n)
{
	Dl *d = user;
	sha256_update(&d->sha, data, n);
	if (!write_all(d->fd, data, n))
		return false;
	d->got += n;
	f32 p = (f32)((f64)(d->done + d->got) / (f64)MAX(d->all, 1));
	if (p - St.progress >= 0.005f || p >= 1) {
		mutex_lock(&St.lock);
		St.progress = p;
		mutex_unlock(&St.lock);
		write_status();
	}
	return true;
}

static bool download(const Release *r, const UpdFile *f, Dl *d)
{
	char *dest = str_fmt("%s/%s", work_dir, f->name), *part = str_fmt("%s/%s.part", work_dir, f->name);
	bool ok = false;
	for (int attempt = 0; attempt < 3 && !ok; attempt++) {
		d->fd = (int)sys_open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (d->fd < 0)
			break;
		sha256_init(&d->sha);
		d->got = 0;
		char *url = str_fmt("%s%s%s", r->base, str_ends(r->base, "/") ? "" : "/", f->name);
		HttpOptions o = { .timeout_ms = 60000, .sink = dl_sink, .user = d };
		HttpResponse *resp = http_request(url, &o);
		mem_free(url);
		u8 sum[32];
		sha256_final(&d->sha, sum);
		ok = resp->status == 200 && d->got == f->size && memcmp(sum, f->sha, 32) == 0 && sys_fsync(d->fd) == 0;
		http_response_free(resp);
		sys_close(d->fd);
		if (ok)
			ok = sys_rename(part, dest) == 0;
		else
			sys_unlink(part);
	}
	if (ok)
		d->done += f->size;
	mem_free(dest);
	mem_free(part);
	return ok;
}

/* ---- The work: checking, and installing what's new ------------------------------------------ */

static bool fetch_release(Release *r)
{
	char *url = str_fmt("%srelease", conf_get(conf, "channel", OS_UPDATE_CHANNEL));
	char *surl = str_fmt("%s.sig", url);
	HttpOptions o = { .timeout_ms = 20000, .max_body = 64 << 10 };
	HttpResponse *a = http_request(url, &o), *b = http_request(surl, &o);
	mem_free(url);
	mem_free(surl);
	bool ok = false;
	if (a->status == 200 && b->status == 200 && a->body && b->body) {
		if (!signature_ok(a->body, a->body_len, (const u8 *)b->body, b->body_len)) {
			log_line("the update description's signature didn't check out; ignoring it");
			set_state("error", "An update was offered without a valid signature, so it was ignored.");
		} else {
			char *text = mem_alloc(a->body_len + 1);
			memcpy(text, a->body, a->body_len);
			text[a->body_len] = 0;
			ok = parse_release(text, r);
			mem_free(text);
			if (!ok)
				set_state("error", "The update server's answer couldn't be read.");
		}
	} else {
		set_state("error", "The update server couldn't be reached. Zurbax will try again later.");
	}
	http_response_free(a);
	http_response_free(b);
	return ok;
}

/* Is this file of the update new for this computer? Files never installed
 * here (a layer left out) are skipped. */
static bool needed(const UpdFile *f, bool *have_esp)
{
	u8 sha[32];
	if (str_eq(f->name, "zurbax.efi")) {
		if (!recorded(f->name, sha)) {
			if (!mount_esp(false))
				return false;
			*have_esp = true;
			bool hashed = hash_file(ESP_EFI, sha);
			unmount_esp();
			if (!hashed)
				return false;
			record(f->name, sha);
		}
		return memcmp(sha, f->sha, 32) != 0;
	}
	if (str_ends(f->name, ".sfs")) {
		char *path = str_fmt("%s/%s", layers_dir, f->name);
		bool there = file_exists(path);
		if (there && !recorded(f->name, sha)) {
			there = hash_file(path, sha);
			if (there)
				record(f->name, sha);
		}
		mem_free(path);
		return there && memcmp(sha, f->sha, 32) != 0;
	}
	return false;
}

static void work(void *arg)
{
	int what = (int)(usize)arg;
	set_state("checking", NULL);
	Release r;
	bool fetched = fetch_release(&r);
	i64 now = time_unix();
	mutex_lock(&St.lock);
	St.checked = now;
	mutex_unlock(&St.lock);
	write_status();
	char when[24];
	fmt(when, sizeof when, "%lld", (long long)now);
	conf_put("checked", when);
	if (!fetched) {
		atomic_store(&busy, 0);
		return;
	}
	mutex_lock(&St.lock);
	str_copy(St.until, r.until, sizeof St.until);
	mutex_unlock(&St.lock);
	if (version_cmp(r.version, OS_FULL_VERSION) <= 0) {
		mutex_lock(&St.lock);
		St.available[0] = St.notes[0] = 0;
		mutex_unlock(&St.lock);
		set_state("current", NULL);
		atomic_store(&busy, 0);
		return;
	}
	mutex_lock(&St.lock);
	str_copy(St.available, r.version, sizeof St.available);
	str_copy(St.date, r.date, sizeof St.date);
	str_copy(St.notes, r.notes, sizeof St.notes);
	St.progress = 0;
	mutex_unlock(&St.lock);
	/* already downloaded and put in place: waiting for a restart */
	if (str_eq(conf_get(conf, "pending", ""), r.version)) {
		set_state("ready", NULL);
		atomic_store(&busy, 0);
		return;
	}
	if (what != WORK_INSTALL && !conf_get_bool(conf, "auto", true)) {
		set_state("available", NULL);
		atomic_store(&busy, 0);
		return;
	}
	log_line("installing update %s", r.version);
	set_state("downloading", NULL);
	dir_make(work_dir, 0700);
	bool esp_seen = false, fail = false;
	UNUSED(esp_seen);
	bool need[16] = { 0 };
	Dl d = { 0 };
	for (int i = 0; i < r.nfiles; i++)
		if ((need[i] = needed(&r.files[i], &esp_seen)))
			d.all += r.files[i].size;
	struct statfs fs;
	u64 room = sys_statfs(data_dir, &fs) == 0 ? fs.f_bavail * (u64)fs.f_bsize : 0;
	if (room < d.all + (256ull << 20)) {
		char msg[200];
		fmt(msg, sizeof msg, "The update needs %llu MB of free space.", (unsigned long long)((d.all >> 20) + 256));
		set_state("error", msg);
		atomic_store(&busy, 0);
		return;
	}
	for (int i = 0; i < r.nfiles && !fail; i++)
		if (need[i] && !download(&r, &r.files[i], &d)) {
			log_line("%s didn't download", r.files[i].name);
			set_state("error", "The update didn't download completely. Zurbax will try again later.");
			fail = true;
		}
	/* everything's here and checked: into place */
	for (int i = 0; i < r.nfiles && !fail; i++) {
		if (!need[i])
			continue;
		const UpdFile *f = &r.files[i];
		char *src = str_fmt("%s/%s", work_dir, f->name);
		bool ok;
		if (str_eq(f->name, "zurbax.efi")) {
			ok = install_efi(src, f->sha);
			if (!ok)
				log_line("the EFI system partition couldn't be found or written");
			sys_unlink(src);
		} else {
			/* the running layer keeps its old copy open until the restart */
			char *dst = str_fmt("%s/%s", layers_dir, f->name);
			ok = sys_rename(src, dst) == 0;
			mem_free(dst);
		}
		mem_free(src);
		if (ok) {
			record(f->name, f->sha);
		} else {
			log_line("%s couldn't be put in place", f->name);
			set_state("error", "The update couldn't be installed. Nothing was changed that would stop Zurbax starting.");
			fail = true;
		}
	}
	if (!fail) {
		conf_put("pending", r.version);
		log_line("update %s is ready; it starts with the next restart", r.version);
		set_state("ready", NULL);
	}
	atomic_store(&busy, 0);
}

static void start_work(int what)
{
	if (live) {
		set_state("live", NULL);
		return;
	}
	if (atomic_add(&busy, 0))
		return;
	atomic_store(&busy, 1);
	if (thread_spawn(work, (void *)(usize)what) < 0)
		atomic_store(&busy, 0);
}

/* ---- Main ------------------------------------------------------------------------------------ */

static void command(const char *cmd)
{
	if (str_eq(cmd, "check"))
		requested = WORK_CHECK;
	else if (str_eq(cmd, "install"))
		requested = WORK_INSTALL;
	else if (str_eq(cmd, "auto on") || str_eq(cmd, "auto off")) {
		conf_put("auto", str_eq(cmd, "auto on") ? "1" : "0");
		write_status();
	}
}

int main(void)
{
	sig_ignore(SIGPIPE);
	dir_make("/run/zurbax", 0755);
	live = file_exists("/run/zurbax-live");
	str_copy(data_dir, zurbax_data(), sizeof data_dir);
	fmt(conf_file, sizeof conf_file, "%s/system/update.conf", data_dir);
	fmt(work_dir, sizeof work_dir, "%s/system/update", data_dir);
	fmt(record_file, sizeof record_file, "%s/system/update/files", data_dir);
	fmt(layers_dir, sizeof layers_dir, "%s/layers", data_dir);
	dir_make(work_dir, 0700);
	conf = conf_load(conf_file);
	St.checked = conf_get_int(conf, "checked", 0);
	/* started after an update: it's done */
	const char *pending = conf_get(conf, "pending", "");
	if (pending[0] && version_cmp(OS_FULL_VERSION, pending) >= 0) {
		log_line("now running %s", OS_FULL_VERSION);
		conf_put("pending", NULL);
		set_state("current", "Zurbax was updated to " OS_FULL_VERSION ".");
	} else {
		set_state(live ? "live" : "idle", NULL);
	}

	int ctl = (int)sys_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (ctl >= 0) {
		struct sockaddr_un a = { .sun_family = AF_UNIX };
		str_copy(a.sun_path, CTL_SOCKET, sizeof a.sun_path);
		sys_unlink(CTL_SOCKET);
		if (sys_bind(ctl, &a, sizeof a) == 0)
			sys_chmod(CTL_SOCKET, 0666); /* Settings, as the person, asks for checks */
	}
	i64 started = time_unix(), last_try = 0;
	for (;;) {
		struct pollfd p = { ctl, POLLIN, 0 };
		sys_poll(&p, ctl >= 0 ? 1 : 0, 60000);
		char buf[64];
		long n;
		while (ctl >= 0 && (n = sys_recvfrom(ctl, buf, sizeof buf - 1, 0, NULL, NULL)) > 0) {
			buf[n] = 0;
			command(str_trim(buf));
		}
		i64 now = time_unix();
		/* on its own: a few minutes after starting, then daily; an hour after a failed try */
		if (!requested && conf_get_bool(conf, "auto", true) && now - started > 180 && now - last_try > 3600 &&
		    now - St.checked >= DAY)
			requested = WORK_CHECK;
		if (requested && !atomic_add(&busy, 0)) {
			last_try = now;
			start_work(requested);
			requested = WORK_NONE;
		}
	}
}
