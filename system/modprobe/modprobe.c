/*
 * modprobe — puts drivers into the kernel: Ember's modules, from
 * drivers.sfs.
 *
 *     modprobe [-q] [--] NAME [PARAM=VALUE...]
 *                          a module by its name or an alias, with the ones it
 *                          needs first; the kernel runs this when it wants one
 *                          (a file system, a network protocol, a cipher)
 *     modprobe --device MODALIAS...
 *                          the drivers for devices, by the modalias the kernel
 *                          gives each; zinit runs this as devices appear
 *     modprobe --coldplug  the drivers for every device there is (at startup)
 *     modprobe --show MODALIAS|NAME
 *                          what would load, in order, without loading it
 *
 * The modules and the indexes depmod made for them are in
 * /data/layers/drivers/lib/modules/RELEASE (or $ZURBAX_MODULES). Every
 * module is signed with Ember's build key and the kernel refuses any that
 * isn't, so all this does is choose and order: aliases are matched as shell
 * patterns, the modules a module needs load before it (and its soft
 * dependencies around it), and what's built in or already loaded is left
 * alone. Old framebuffer drivers never load for a device: the GPUs' own
 * drivers have the screen.
 *
 * What --coldplug and --device load is noted in /run/zurbax/drivers.log,
 * and in the kernel's log at the info level (quiet on the screen).
 */
#include "zr.h"

#define LOG_FILE "/run/zurbax/drivers.log"

typedef struct {
	char *path;  /* relative to the modules folder */
	char **deps; /* the paths of the modules it needs */
	usize ndeps;
	int state;   /* 0 not tried, 1 loading, 2 loaded, -1 failed */
} Mod;

typedef struct {
	char *pattern, *module;
} Alias;

typedef VEC(Alias) Aliases;

static char dir[600];
static Map mods;      /* name -> Mod* */
static Map builtin;   /* name -> (void *)1 */
static Map softpre, softpost; /* name -> char* (space-separated names) */
static Map by_bus;    /* "pci" -> Aliases*: patterns whose bus is spelled out */
static Aliases wild;  /* patterns that start with a wildcard */
static bool quiet, show;
static int log_fd = -1, kmsg_fd = -1;

static void note(const char *f, ...) PRINTF_LIKE(1, 2);
static void note(const char *f, ...)
{
	if (log_fd < 0)
		return;
	char msg[600];
	va_list ap;
	va_start(ap, f);
	fmt_v(msg, sizeof msg, f, ap);
	va_end(ap);
	write_all(log_fd, msg, strlen(msg));
	if (kmsg_fd >= 0) {
		char line[640];
		fmt(line, sizeof line, "<6>modprobe: %s", msg);
		sys_write(kmsg_fd, line, strlen(line));
	}
}

/* "snd-hda-intel", "kernel/sound/pci/hda/snd-hda-intel.ko": snd_hda_intel */
static void mod_name(const char *s, char *out, usize cap)
{
	const char *b = str_rchr(s, '/');
	b = b ? b + 1 : s;
	usize n = 0;
	for (; *b && n + 1 < cap; b++) {
		if (str_starts(b, ".ko"))
			break;
		out[n++] = *b == '-' ? '_' : *b;
	}
	out[n] = 0;
}

/* ---- depmod's indexes --------------------------------------------------------------------- */

static char *read_index(const char *name)
{
	char *path = str_fmt("%s/%s", dir, name);
	char *text = file_read(path, NULL);
	mem_free(path);
	return text;
}

/* Calls fn for each line of an index. */
static void each_line(char *text, void (*fn)(char *line))
{
	for (char *line = text; line && *line;) {
		char *nl = (char *)str_chr(line, '\n');
		if (nl)
			*nl = 0;
		if (line[0] && line[0] != '#')
			fn(line);
		line = nl ? nl + 1 : NULL;
	}
}

/* "kernel/a/b.ko: kernel/c.ko kernel/d.ko" */
static void dep_line(char *line)
{
	char *colon = (char *)str_chr(line, ':');
	if (!colon)
		return;
	*colon = 0;
	Mod *m = mem_alloc(sizeof *m);
	memset(m, 0, sizeof *m);
	m->path = str_dup(line);
	usize n = 0;
	char **parts = str_split(str_trim(colon + 1), ' ', &n);
	m->deps = mem_alloc(sizeof(char *) * (n + 1));
	for (usize i = 0; i < n; i++)
		if (parts[i][0])
			m->deps[m->ndeps++] = str_dup(parts[i]);
	str_split_free(parts, n);
	char name[96];
	mod_name(line, name, sizeof name);
	if (!map_has(&mods, name))
		map_set(&mods, name, m);
}

static void builtin_line(char *line)
{
	char name[96];
	mod_name(line, name, sizeof name);
	map_set(&builtin, name, (void *)1);
}

/* "alias pci:v00008086d*sv*sd*bc02sc00i* e1000e" */
static void alias_line(char *line)
{
	if (!str_starts(line, "alias "))
		return;
	char *pat = line + 6;
	char *sp = (char *)str_rchr(pat, ' ');
	if (!sp)
		return;
	*sp = 0;
	char name[96];
	mod_name(sp + 1, name, sizeof name);
	Alias a = { str_dup(pat), str_dup(name) };
	/* indexed by what comes before the first ':', unless that has a wildcard */
	const char *colon = str_chr(pat, ':');
	bool plain = colon != NULL;
	for (const char *p = pat; plain && p < colon; p++)
		if (*p == '*' || *p == '?' || *p == '[')
			plain = false;
	if (!plain) {
		vec_push(&wild, a);
		return;
	}
	char bus[32];
	fmt(bus, sizeof bus, "%.*s", (int)MIN((usize)(colon - pat), sizeof bus - 1), pat);
	Aliases *v = map_get(&by_bus, bus);
	if (!v) {
		v = mem_alloc(sizeof *v);
		memset(v, 0, sizeof *v);
		map_set(&by_bus, bus, v);
	}
	vec_push(v, a);
}

/* "softdep snd_hda_intel pre: snd_hda_codec_hdmi post: x" */
static void softdep_line(char *line)
{
	if (!str_starts(line, "softdep "))
		return;
	usize n = 0;
	char **w = str_split(line + 8, ' ', &n);
	if (n == 0) {
		str_split_free(w, n);
		return;
	}
	char name[96];
	mod_name(w[0], name, sizeof name);
	StrBuf pre = { 0 }, post = { 0 };
	StrBuf *into = NULL;
	for (usize i = 1; i < n; i++) {
		if (str_eq(w[i], "pre:"))
			into = &pre;
		else if (str_eq(w[i], "post:"))
			into = &post;
		else if (into && w[i][0])
			sb_fmt(into, "%s ", w[i]);
	}
	if (pre.data)
		map_set(&softpre, name, pre.data);
	if (post.data)
		map_set(&softpost, name, post.data);
	str_split_free(w, n);
}

static bool load_indexes(void)
{
	const char *over = env_get("ZURBAX_MODULES");
	if (over) {
		str_copy(dir, over, sizeof dir);
	} else {
		struct utsname u;
		sys_uname(&u);
		fmt(dir, sizeof dir, "/data/layers/drivers/lib/modules/%s", u.release);
	}
	char *dep = read_index("modules.dep");
	if (!dep)
		return false;
	each_line(dep, dep_line);
	mem_free(dep);
	char *b = read_index("modules.builtin");
	each_line(b, builtin_line);
	mem_free(b);
	char *a = read_index("modules.alias");
	each_line(a, alias_line);
	mem_free(a);
	char *s = read_index("modules.softdep");
	each_line(s, softdep_line);
	mem_free(s);
	return true;
}

/* ---- Matching ------------------------------------------------------------------------------ */

/* Shell patterns as depmod writes them: *, ? and [...] with ranges and ! or ^. */
static bool glob(const char *p, const char *s)
{
	while (*p) {
		if (*p == '*') {
			while (*p == '*')
				p++;
			if (!*p)
				return true;
			for (; *s; s++)
				if (glob(p, s))
					return true;
			return false;
		}
		if (!*s)
			return false;
		if (*p == '?') {
			p++, s++;
			continue;
		}
		if (*p == '[') {
			const char *q = p + 1;
			bool neg = *q == '!' || *q == '^';
			if (neg)
				q++;
			bool hit = false;
			for (bool first = true; *q && (first || *q != ']'); first = false) {
				if (q[1] == '-' && q[2] && q[2] != ']') {
					hit |= *s >= q[0] && *s <= q[2];
					q += 3;
				} else {
					hit |= *s == *q;
					q++;
				}
			}
			if (*q != ']')
				return false; /* no closing bracket: no match */
			if (hit == neg)
				return false;
			p = q + 1, s++;
			continue;
		}
		if (*p != *s)
			return false;
		p++, s++;
	}
	return !*s;
}

/* The modules whose aliases match, each once. */
static usize matching(const char *alias, char **out, usize cap)
{
	usize n = 0;
	Aliases *lists[2] = { &wild, NULL };
	const char *colon = str_chr(alias, ':');
	if (colon) {
		char bus[32];
		fmt(bus, sizeof bus, "%.*s", (int)MIN((usize)(colon - alias), sizeof bus - 1), alias);
		lists[1] = map_get(&by_bus, bus);
	}
	for (int l = 0; l < 2; l++) {
		if (!lists[l])
			continue;
		vec_each(lists[l], a)
		{
			if (!glob(a->pattern, alias))
				continue;
			bool dup = false;
			for (usize i = 0; i < n && !dup; i++)
				dup = str_eq(out[i], a->module);
			if (!dup && n < cap)
				out[n++] = a->module;
		}
	}
	return n;
}

/* ---- Loading ------------------------------------------------------------------------------- */

static bool is_loaded(const char *name)
{
	char path[160];
	fmt(path, sizeof path, "/sys/module/%s/initstate", name);
	char *s = file_read_line(path);
	bool live = s && (str_eq(s, "live") || str_eq(s, "coming"));
	mem_free(s);
	return live;
}

/* framebuffer drivers: not for devices; the GPUs' DRM drivers have the screen */
static bool unwanted(const Mod *m)
{
	return str_find(m->path, "/drivers/video/fbdev/") != NULL;
}

static bool load(const char *want, const char *params, bool for_device, int depth);

static void load_list(const char *names)
{
	if (!names)
		return;
	usize n = 0;
	char **w = str_split(names, ' ', &n);
	for (usize i = 0; i < n; i++)
		if (w[i][0])
			load(w[i], "", false, 1);
	str_split_free(w, n);
}

static bool load_mod(const char *name, Mod *m, const char *params, int depth)
{
	if (m->state == 2 || m->state == 1)
		return true;
	if (m->state == -1)
		return false;
	m->state = 1;
	load_list(map_get(&softpre, name));
	bool ok = true;
	for (usize i = 0; i < m->ndeps && ok; i++) {
		char dn[96];
		mod_name(m->deps[i], dn, sizeof dn);
		ok = load(dn, "", false, depth + 1);
	}
	if (ok && show) {
		print("%s%s\n", m->path, is_loaded(name) ? " (loaded)" : "");
	} else if (ok && !is_loaded(name)) {
		char *path = str_fmt("%s/%s", dir, m->path);
		int fd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
		long r = fd >= 0 ? syscall3(SYS_finit_module, fd, (long)params, 0) : fd;
		if (fd >= 0)
			sys_close(fd);
		if (r < 0 && r != -EEXIST) {
			ok = false;
			if (!quiet)
				eprint("modprobe: %s: %s\n", name, err_str(r));
			note("  %s failed: %s\n", name, err_str(r));
		}
		mem_free(path);
	}
	m->state = ok ? 2 : -1;
	if (ok)
		load_list(map_get(&softpost, name));
	return ok;
}

/* A module by name, or the modules an alias names. */
static bool load(const char *want, const char *params, bool for_device, int depth)
{
	if (depth > 32)
		return false;
	char name[96];
	mod_name(want, name, sizeof name);
	if (map_has(&builtin, name))
		return true;
	Mod *m = map_get(&mods, name);
	if (m)
		return load_mod(name, m, params, depth);
	char *found[32];
	usize n = matching(want, found, ARRAY_LEN(found));
	bool any = false;
	for (usize i = 0; i < n; i++) {
		if (map_has(&builtin, found[i])) {
			any = true;
			continue;
		}
		Mod *fm = map_get(&mods, found[i]);
		if (!fm || (for_device && unwanted(fm)))
			continue;
		bool was = fm->state == 2 || is_loaded(found[i]);
		if (load_mod(found[i], fm, params, depth)) {
			any = true;
			if (for_device && !was && !show)
				note("%s for %s\n", found[i], want);
		}
	}
	return any;
}

/* ---- Every device ---------------------------------------------------------------------- */

static void coldplug(void)
{
	DirList buses = { 0 };
	if (!dir_list("/sys/bus", &buses, false))
		return;
	Map seen = { 0 };
	int devices = 0;
	vec_each(&buses, b)
	{
		char *devs = str_fmt("/sys/bus/%s/devices", b->name);
		DirList dl = { 0 };
		if (dir_list(devs, &dl, false)) {
			vec_each(&dl, d)
			{
				char *path = str_fmt("%s/%s/modalias", devs, d->name);
				char *alias = file_read_line(path);
				mem_free(path);
				if (alias && alias[0] && !map_has(&seen, alias)) {
					map_set(&seen, alias, (void *)1);
					devices++;
					load(alias, "", true, 0);
				}
				mem_free(alias);
			}
			dir_list_free(&dl);
		}
		mem_free(devs);
	}
	dir_list_free(&buses);
	map_free(&seen, NULL);
	note("checked %d devices\n", devices);
}

int main(int argc, char **argv)
{
	int i = 1;
	bool device = false, cold = false;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (str_eq(argv[i], "--")) {
			i++;
			break;
		}
		if (str_eq(argv[i], "-q") || str_eq(argv[i], "--quiet"))
			quiet = true;
		else if (str_eq(argv[i], "--device"))
			device = quiet = true;
		else if (str_eq(argv[i], "--coldplug"))
			cold = quiet = true;
		else if (str_eq(argv[i], "--show"))
			show = true;
		else if (str_eq(argv[i], "-b") || str_eq(argv[i], "--use-blacklist"))
			; /* what the kernel's request_module passes on some versions */
		else {
			eprint("usage: modprobe [-q] [--] NAME [PARAM=VALUE...] | --device MODALIAS... | --coldplug | --show NAME\n");
			return 2;
		}
	}
	if (!load_indexes()) {
		if (!quiet)
			eprint("modprobe: no drivers in %s\n", dir);
		return 1;
	}
	if (cold || device) {
		log_fd = (int)sys_open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		kmsg_fd = (int)sys_open("/dev/kmsg", O_WRONLY | O_CLOEXEC, 0);
	}
	if (cold) {
		coldplug();
		return 0;
	}
	if (i >= argc)
		return 2;
	if (device) {
		for (; i < argc; i++)
			load(argv[i], "", true, 0);
		return 0;
	}
	StrBuf params = { 0 };
	for (int k = i + 1; k < argc; k++)
		sb_fmt(&params, "%s%s", k > i + 1 ? " " : "", argv[k]);
	bool ok = load(argv[i], params.data ? params.data : "", false, 0);
	if (!ok && !quiet)
		eprint("modprobe: %s: no such driver\n", argv[i]);
	return ok ? 0 : 1;
}
