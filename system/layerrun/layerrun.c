/*
 * layerrun: starts a program from a Linux layer, a root file system laid out
 * like a standard distribution (glibc, Mesa, X11...) made by layer/build.sh.
 * ZOS itself has none of that; the layer is how Steam, Proton and other
 * standard Linux programs run on it.
 *
 *     layerrun [--layer DIR] [--x11] [--sound] [--app ID] PROGRAM [ARGS...]
 *     layerrun --check [--layer DIR]
 *
 * The program sees the layer as its whole file system, with ZOS's /dev,
 * /proc, /sys, /tmp, /run (where the Wayland socket is) and /data (the home
 * folders) passed through, and a passwd, group, hosts and resolv.conf made
 * for it. A user namespace and a mount namespace do this without any
 * privileges, and nothing outside the program sees a change.
 *
 * With --x11 the layer's Xwayland runs first, full screen on zdisplay, and
 * the program gets it as its DISPLAY (Steam and X11 games), along with the
 * layer's sound server (PulseAudio playing through zaudio; --sound alone
 * starts just that). --app names the
 * ZOS app the windows belong to, for the dock. The default layer is
 * /data/layers/steam.
 */
#include "zr.h"

#define DEFAULT_LAYER "/data/layers/steam"

static const char *layer = DEFAULT_LAYER;

static void fail(const char *f, ...) PRINTF_LIKE(1, 2);
static void fail(const char *f, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, f);
	fmt_v(msg, sizeof msg, f, ap);
	va_end(ap);
	eprint("layerrun: %s\n", msg);
	exit(1);
}

static char *in_layer(const char *path)
{
	return str_fmt("%s%s", layer, path);
}

/* ---- What the layer has ---------------------------------------------------------------- */

/* Any of the paths, separated by spaces (distributions put libraries in different places). */
static bool layer_has(const char *paths)
{
	bool ok = false;
	usize n = 0;
	char **alts = str_split(paths, ' ', &n);
	for (usize i = 0; i < n && !ok; i++) {
		char *p = in_layer(alts[i]);
		ok = file_exists(p);
		mem_free(p);
	}
	str_split_free(alts, n);
	return ok;
}

/* The dynamic linkers sit at paths fixed by the processor ABIs. */
#define LD64 "/lib64/ld-linux-x86-64.so.2"
#define LD32 "/lib/ld-linux.so.2"

/* Prints what's there and what's missing for Steam; 0 when it can run. */
static int check(void)
{
	struct {
		const char *what, *path;
		bool needed;
	} items[] = {
		{ "the C library (64-bit)", LD64, true },
		{ "the C library (32-bit, for the Steam client)", LD32, true },
		{ "a shell", "/usr/bin/bash /bin/bash", true },
		{ "Xwayland", "/usr/bin/Xwayland", true },
		{ "the Vulkan loader", "/usr/lib/libvulkan.so.1 /usr/lib/x86_64-linux-gnu/libvulkan.so.1", true },
		{ "Vulkan drivers (Mesa)", "/usr/share/vulkan/icd.d", true },
		{ "the Steam launcher", "/usr/bin/steam /usr/games/steam", true },
		{ "sound for games (PulseAudio)", "/usr/bin/pulseaudio", false },
	};
	bool ok = dir_exists(layer);
	print("layer: %s%s\n", layer, ok ? "" : " (missing)");
	for (usize i = 0; i < ARRAY_LEN(items) && dir_exists(layer); i++) {
		bool have = layer_has(items[i].path);
		print("  %s %s\n", have ? "yes" : "no ", items[i].what);
		ok &= have || !items[i].needed;
	}
	char *ns = file_read_line("/proc/sys/user/max_user_namespaces");
	bool userns = ns && str_to_int(ns, NULL) > 0;
	mem_free(ns);
	print("user namespaces: %s\n", userns ? "yes" : "no");
	print("ntsync: %s\n", file_exists("/dev/ntsync") ? "yes" : "no");
	return ok && userns ? 0 : 1;
}

/* ---- The namespace --------------------------------------------------------------------- */

static void must_mount(const char *src, const char *dst, const char *type, unsigned long flags, const char *data)
{
	long r = sys_mount(src, dst, type, flags, data);
	if (r < 0)
		fail("cannot mount %s on %s: %s", src ? src : type, dst, err_str(r));
}

/* Passes a ZOS directory through to the same place in the layer. */
static void pass_through(const char *dir)
{
	if (!dir_exists(dir))
		return;
	char *dst = in_layer(dir);
	dir_make(dst, 0755);
	must_mount(dir, dst, NULL, MS_BIND | MS_REC, NULL);
	mem_free(dst);
}

static void enter_namespaces(void)
{
	u32 uid = (u32)sys_getuid(), gid = (u32)sys_getgid();
	long r = sys_unshare(uid == 0 ? CLONE_NEWNS : CLONE_NEWUSER | CLONE_NEWNS);
	if (r < 0)
		fail("cannot make a namespace (%s); the kernel needs CONFIG_USER_NS", err_str(r));
	if (uid != 0) {
		/* the same user inside as outside */
		char map[64];
		file_write_str("/proc/self/setgroups", "deny");
		fmt(map, sizeof map, "%u %u 1\n", uid, uid);
		if (!file_write_str("/proc/self/uid_map", map))
			fail("cannot map the user into the namespace");
		fmt(map, sizeof map, "%u %u 1\n", gid, gid);
		if (!file_write_str("/proc/self/gid_map", map))
			fail("cannot map the group into the namespace");
	}
	/* mounts from here on stay in this namespace */
	must_mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
}

/* Small files the layer's programs expect in /etc, written for this session
 * into a private tmpfs (the layer's .zos folder) and laid over its own. */
static void add_etc(const char *name, const char *text)
{
	char *made = str_fmt("%s/.zos/%s", layer, name), *dst = str_fmt("%s/etc/%s", layer, name);
	if (!file_write_str(made, text))
		fail("cannot write %s", made);
	if (!file_exists(dst))
		file_write_str(dst, ""); /* a writable layer gets the spot; a read-only one has it already */
	if (file_exists(dst))
		must_mount(made, dst, NULL, MS_BIND, NULL);
	mem_free(made);
	mem_free(dst);
}

static void make_etc(const char *user, const char *home)
{
	u32 uid = (u32)sys_getuid(), gid = (u32)sys_getgid();
	char text[1024];
	fmt(text, sizeof text, "root:x:0:0:root:/root:/bin/bash\n%s:x:%u:%u:%s:%s:/bin/bash\nnobody:x:65534:65534:nobody:/:/bin/false\n",
	    user, uid, gid, user, home);
	add_etc("passwd", text);
	fmt(text, sizeof text, "root:x:0:\naudio:x:%u:%s\nvideo:x:%u:%s\ninput:x:%u:%s\n%s:x:%u:\nnobody:x:65534:\n", GID_AUDIO, user,
	    GID_VIDEO, user, GID_INPUT, user, user, gid);
	add_etc("group", text);
	add_etc("hosts", "127.0.0.1 localhost\n::1 localhost\n127.0.1.1 zos\n");
	add_etc("hostname", "zos\n");
	/* name servers: what the network service found */
	StrBuf sb = { 0 };
	Conf *c = conf_load("/run/zurbax/network.status");
	const char *dns = conf_get(c, "dns", NULL);
	usize n = 0;
	char **parts = dns ? str_split(dns, ' ', &n) : NULL;
	for (usize i = 0; i < n; i++)
		if (parts[i][0])
			sb_fmt(&sb, "nameserver %s\n", parts[i]);
	if (!sb.len)
		sb_adds(&sb, "nameserver 1.1.1.1\nnameserver 8.8.8.8\n");
	str_split_free(parts, n);
	conf_free(c);
	add_etc("resolv.conf", sb.data);
	sb_free(&sb);
	/* a stable machine id (D-Bus and Steam want one), kept with the user's files */
	char *idpath = str_fmt("%s/.local/share/zos/machine-id", home);
	char *id = file_read_line(idpath);
	if (!id || strlen(id) != 32) {
		u8 raw[16];
		rand_bytes(raw, sizeof raw);
		mem_free(id);
		id = mem_alloc(33);
		for (int i = 0; i < 16; i++)
			fmt(id + i * 2, 3, "%02x", raw[i]);
		char *dir = path_dir(idpath);
		dir_make(dir, 0700);
		mem_free(dir);
		file_write_str(idpath, id);
	}
	fmt(text, sizeof text, "%s\n", id);
	add_etc("machine-id", text);
	mem_free(id);
	mem_free(idpath);
}

static void build_root(const char *user, const char *home)
{
	if (!dir_exists(layer))
		fail("no layer at %s (layer/build.sh makes one)", layer);
	if (!layer_has(LD64))
		fail("%s isn't a Linux layer: it has no C library", layer);
	enter_namespaces();
	/* the layer becomes a mount of its own, to be the new root */
	must_mount(layer, layer, NULL, MS_BIND | MS_REC, NULL);
	char *zos = in_layer("/.zos");
	dir_make(zos, 0755);
	if (!dir_exists(zos))
		fail("the layer has no .zos folder (layer/build.sh makes it)");
	must_mount("tmpfs", zos, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
	char *runtime = str_fmt("%s/runtime", zos);
	dir_make(runtime, 0700);
	mem_free(runtime);
	mem_free(zos);
	make_etc(user, home);
	static const char *const through[] = { "/dev", "/proc", "/sys", "/tmp", "/run", "/data" };
	for (usize i = 0; i < ARRAY_LEN(through); i++)
		pass_through(through[i]);
	long r = sys_chdir(layer);
	if (r >= 0)
		r = sys_pivot_root(".", ".");
	if (r >= 0) {
		sys_umount(".", MNT_DETACH); /* ZOS's own root, now under the layer */
	} else if (r == -EINVAL) {
		/* ZOS runs from the initial RAM file system, which can't be pivoted
		 * away from: the layer moves over it instead, as switch_root does.
		 * The layer's root is then a mount of its own, so containers made
		 * inside it (Steam's runtime) can pivot. */
		r = sys_mount(".", "/", NULL, MS_MOVE, NULL);
		if (r >= 0)
			r = sys_chroot(".");
	}
	if (r < 0)
		fail("cannot switch to the layer: %s", err_str(r));
	sys_chdir("/");
}

/* ---- Starting programs ---------------------------------------------------------------- */

static char **env;
static usize nenv;

static void setenv_(const char *name, const char *value)
{
	usize n = strlen(name);
	char *kv = str_fmt("%s=%s", name, value);
	for (usize i = 0; i < nenv; i++)
		if (str_starts(env[i], name) && env[i][n] == '=') {
			env[i] = kv;
			return;
		}
	env = mem_realloc(env, sizeof(char *) * (nenv + 2));
	env[nenv++] = kv;
	env[nenv] = NULL;
}

/* PROGRAM in the layer's PATH, unless it has a slash. */
static char *find(const char *program)
{
	if (str_chr(program, '/'))
		return str_dup(program);
	static const char *const dirs[] = { "/usr/local/bin", "/usr/bin", "/bin" };
	for (usize i = 0; i < ARRAY_LEN(dirs); i++) {
		char *p = str_fmt("%s/%s", dirs[i], program);
		if (file_exists(p))
			return p;
		mem_free(p);
	}
	return NULL;
}

static int spawn(char **argv)
{
	char *path = find(argv[0]);
	if (!path)
		fail("the layer has no %s", argv[0]);
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1 };
	int pid = proc_spawn(path, argv, env, &o);
	if (pid < 0)
		fail("cannot start %s: %s", path, err_str(pid));
	mem_free(path);
	return pid;
}

/* The screen's real size (from zdisplay), so the X server matches it pixel for pixel. */
static bool screen_size(char *out, usize cap)
{
	const char *s = env_get("ZOS_SCREEN");
	if (!s || !*s)
		return false;
	str_copy(out, s, cap);
	return true;
}

/* Starts the layer's Xwayland full screen and returns its display number. */
static int start_x(int *xpid)
{
	int fds[2];
	if (sys_pipe2(fds, O_CLOEXEC) < 0)
		fail("cannot make a pipe");
	/* Xwayland writes its display number to this descriptor once it's ready
	 * (a copy without close-on-exec, so it gets it) */
	int keep = (int)sys_fcntl(fds[1], F_DUPFD, 10);
	char fdarg[16], size[48];
	fmt(fdarg, sizeof fdarg, "%d", keep);
	char *argv[12];
	int n = 0;
	argv[n++] = "Xwayland";
	argv[n++] = "-displayfd";
	argv[n++] = fdarg;
	argv[n++] = "-fullscreen";
	argv[n++] = "-noreset";
	argv[n++] = "-nolisten";
	argv[n++] = "tcp";
	if (screen_size(size, sizeof size)) {
		argv[n++] = "-geometry";
		argv[n++] = size;
	}
	argv[n] = NULL;
	*xpid = spawn(argv);
	sys_close(keep);
	sys_close(fds[1]);
	char buf[16] = { 0 };
	long got = 0;
	for (int tries = 0; tries < 200 && got <= 0; tries++) {
		got = sys_read(fds[0], buf, sizeof buf - 1);
		if (got < 0 && got != -EINTR)
			break;
	}
	sys_close(fds[0]);
	if (got <= 0)
		fail("Xwayland didn't start");
	return (int)str_to_int(buf, NULL);
}

/* The layer's sound server for the session: PulseAudio (what Proton, SDL
 * and Steam's runtime play to), with ALSA's zaudio plugin as its only output.
 * Returns its pid once its socket is up, or 0 when the layer has none. */
static int start_sound(void)
{
	if (!file_exists("/usr/bin/pulseaudio") || !file_exists("/etc/pulse/zos.pa"))
		return 0;
	char *argv[] = { "pulseaudio", "-n", "-F", "/etc/pulse/zos.pa", "--daemonize=no", "--exit-idle-time=-1",
			 "--log-target=stderr", "--log-level=error", NULL };
	int pid = spawn(argv);
	for (int tries = 0; tries < 40 && !file_exists("/.zos/runtime/pulse/native"); tries++)
		sleep_ms(50);
	return pid;
}

int main(int argc, char **argv)
{
	bool x11 = false, sound = false, check_only = false;
	const char *app = NULL;
	int i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (str_eq(argv[i], "--layer") && i + 1 < argc)
			layer = argv[++i];
		else if (str_eq(argv[i], "--x11"))
			x11 = sound = true;
		else if (str_eq(argv[i], "--sound"))
			sound = true;
		else if (str_eq(argv[i], "--app") && i + 1 < argc)
			app = argv[++i];
		else if (str_eq(argv[i], "--check"))
			check_only = true;
		else
			break;
	}
	if (check_only)
		return check();
	if (i >= argc) {
		eprint("usage: layerrun [--layer DIR] [--x11] [--sound] [--app ID] PROGRAM [ARGS...]\n       layerrun --check [--layer DIR]\n");
		return 2;
	}
	const char *home = env_get("HOME"), *user = env_get("USER");
	if (!home || !*home)
		home = "/";
	if (!user || !*user)
		user = "user";
	/* the environment of a standard Linux session, plus ZOS's display */
	for (char **e = environ; *e; e++) {
		env = mem_realloc(env, sizeof(char *) * (nenv + 2));
		env[nenv++] = *e;
		env[nenv] = NULL;
	}
	const char *wayland = env_get("WAYLAND_DISPLAY");
	char wl[200];
	str_copy(wl, wayland && wayland[0] == '/' ? wayland : "/run/zurbax/wayland-0", sizeof wl);
	build_root(user, home);
	setenv_("PATH", "/usr/local/bin:/usr/bin:/bin");
	setenv_("SHELL", "/bin/bash");
	setenv_("LOGNAME", user);
	setenv_("XDG_RUNTIME_DIR", "/.zos/runtime");
	setenv_("WAYLAND_DISPLAY", wl);
	setenv_("XDG_SESSION_TYPE", x11 ? "x11" : "wayland");
	if (app)
		setenv_("ZOS_APP_ID", app);
	int spid = sound ? start_sound() : 0;
	int xpid = 0;
	if (x11) {
		char d[16];
		fmt(d, sizeof d, ":%d", start_x(&xpid));
		setenv_("DISPLAY", d);
	}
	sys_chdir(home);
	int pid = spawn(argv + i);
	/* until the program ends; when the X server goes first (its window was
	 * closed), the program goes too */
	int status = 0;
	for (;;) {
		int st = 0;
		long w = sys_wait4(-1, &st, 0, NULL);
		if (w < 0 && w != -EINTR)
			break;
		if (w == pid) {
			status = st;
			break;
		}
		if (w == xpid) {
			xpid = 0;
			sys_kill(pid, SIGTERM);
		}
	}
	if (xpid > 0) {
		sys_kill(xpid, SIGTERM);
		sys_wait4(xpid, NULL, 0, NULL);
	}
	if (spid > 0) {
		sys_kill(spid, SIGTERM);
		sys_wait4(spid, NULL, 0, NULL);
	}
	return (status & 0x7f) ? 128 + (status & 0x7f) : (status >> 8) & 0xff;
}
