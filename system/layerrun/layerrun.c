/*
 * layerrun: starts a program from a Linux layer, a root file system laid out
 * like a standard distribution (glibc, Mesa, X11, Flatpak...) made by
 * layer/build.sh. ZOS itself has none of that; the layer is how Flatpak
 * apps, Steam, Proton and other standard Linux programs run on it.
 *
 *     layerrun [--x11] [--app ID] PROGRAM [ARGS...]
 *     layerrun --session            start the Linux session now, if it isn't running
 *     layerrun --stop               end it
 *     layerrun --isolated [--layer DIR] [--x11] [--sound] PROGRAM [ARGS...]
 *     layerrun --check [--layer DIR]
 *
 * The program sees the layer as its whole file system, with ZOS's /dev,
 * /proc, /sys, /tmp, /run (where the Wayland socket is) and /data (the home
 * folders) passed through, and a passwd, group, hosts and resolv.conf made
 * for it. A user namespace and a mount namespace do this without any
 * privileges, and nothing outside sees a change.
 *
 * The Linux session: every Linux program of a user shares one namespace,
 * made the first time one starts, like the session of a standard desktop.
 * It has what such a desktop's programs expect to find in their runtime
 * folder, /run/user/UID: the D-Bus session bus (bus), PulseAudio (pulse/,
 * playing through zaudio), the Wayland socket (wayland-0) and the document
 * portal's files (doc/, served by zportal). The session process keeps the
 * namespace's privileges, which nothing it starts has, and uses them for
 * one thing: mounting FUSE file systems for the user when the layer's
 * fusermount3 (layer/fusermount) asks over zos-linux/fuse.sock.
 *
 * With --x11 an Xwayland of the program's own runs first, full screen on
 * zdisplay, with zos-xwm arranging its windows (Steam, X11 games). --app
 * names the ZOS app the windows belong to, for the dock. --isolated gives a
 * program a namespace and helpers of its own, the way layerrun worked
 * before sessions. The layer is /data/layers/steam (or --layer).
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

/* Prints what's there and what's missing; 0 when Steam can run. */
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
		{ "the session bus (D-Bus)", "/usr/bin/dbus-daemon", false },
		{ "Flatpak", "/usr/bin/flatpak", false },
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
	print("fuse: %s\n", file_exists("/dev/fuse") ? "yes" : "no");
	return ok && userns ? 0 : 1;
}

/* ---- The runtime folder ------------------------------------------------------------------ */

/* /run/user/UID, which zdisplay makes for the user (owned by them, private);
 * $XDG_RUNTIME_DIR when it is such a folder; else one in /tmp. */
static char rt[200];

static bool private_dir(const char *path)
{
	struct stat st;
	return sys_lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == (u32)sys_getuid() && !(st.st_mode & 077);
}

static void find_runtime(void)
{
	const char *x = env_get("XDG_RUNTIME_DIR");
	if (x && *x && !str_eq(x, "/run/zurbax") && private_dir(x)) {
		str_copy(rt, x, sizeof rt);
		return;
	}
	fmt(rt, sizeof rt, "/run/user/%u", (u32)sys_getuid());
	if (private_dir(rt))
		return;
	sys_mkdir(rt, 0700);
	if (private_dir(rt))
		return;
	fmt(rt, sizeof rt, "/tmp/zos-runtime-%u", (u32)sys_getuid());
	sys_mkdir(rt, 0700);
	if (!private_dir(rt))
		fail("no private runtime folder (%s belongs to someone else)", rt);
}

static char *rt_path(const char *name)
{
	return str_fmt("%s/%s", rt, name);
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

/* name servers: what the network service found */
static char *resolv_text(void)
{
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
	return sb_take(&sb);
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
	char *resolv = resolv_text();
	add_etc("resolv.conf", resolv);
	mem_free(resolv);
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
		 * inside it (Steam's runtime, Flatpak's sandboxes) can pivot. */
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

static void copy_environ(void)
{
	for (char **e = environ; *e; e++) {
		env = mem_realloc(env, sizeof(char *) * (nenv + 2));
		env[nenv++] = *e;
		env[nenv] = NULL;
	}
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

/* PulseAudio (what Proton, SDL, Steam's runtime and Flatpak apps play to),
 * with ALSA's zaudio plugin as its only output. Its socket is
 * $XDG_RUNTIME_DIR/pulse/native. Returns its pid once that is up, or 0 when
 * the layer has none. */
static int start_sound(void)
{
	if (!file_exists("/usr/bin/pulseaudio") || !file_exists("/etc/pulse/zos.pa"))
		return 0;
	char *argv[] = { "pulseaudio", "-n", "-F", "/etc/pulse/zos.pa", "--daemonize=no", "--exit-idle-time=-1",
			 "--log-target=stderr", "--log-level=error", NULL };
	int pid = spawn(argv);
	const char *x = env_get("XDG_RUNTIME_DIR");
	char *sock = str_fmt("%s/pulse/native", x ? x : "/.zos/runtime");
	for (int tries = 0; tries < 60 && !file_exists(sock); tries++)
		sleep_ms(50);
	mem_free(sock);
	return pid;
}

/* A D-Bus session bus listening on path. Returns its pid once it listens, or 0. */
static int start_bus(const char *path)
{
	if (!file_exists("/usr/bin/dbus-daemon"))
		return 0;
	sys_unlink(path);
	char *addr = str_fmt("--address=unix:path=%s", path);
	char *argv[] = { "dbus-daemon", "--session", "--nofork", "--nosyslog", "--nopidfile", addr, NULL };
	int pid = spawn(argv);
	for (int tries = 0; tries < 60 && !file_exists(path); tries++)
		sleep_ms(50);
	mem_free(addr);
	return pid;
}

/* ---- The session's environment ------------------------------------------------------------ */

/* What programs of a standard Wayland desktop session find in their
 * environment, with ZOS's runtime folder. */
static void session_env(const char *user, const char *home)
{
	setenv_("PATH", "/usr/local/bin:/usr/bin:/bin");
	setenv_("SHELL", "/bin/bash");
	setenv_("USER", user);
	setenv_("LOGNAME", user);
	setenv_("HOME", home);
	setenv_("XDG_RUNTIME_DIR", rt);
	char *bus = str_fmt("unix:path=%s/bus", rt);
	setenv_("DBUS_SESSION_BUS_ADDRESS", bus);
	mem_free(bus);
	setenv_("WAYLAND_DISPLAY", "wayland-0");
	setenv_("XDG_SESSION_TYPE", "wayland");
	setenv_("XDG_CURRENT_DESKTOP", "ZOS");
	setenv_("XDG_SESSION_DESKTOP", "zos");
	setenv_("DESKTOP_SESSION", "zos");
	/* Flatpak apps' menu entries and icons, then the layer's own */
	char *data = str_fmt("%s/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:/usr/local/share:/usr/share", home);
	setenv_("XDG_DATA_DIRS", data);
	mem_free(data);
	/* toolkits that would otherwise start in X11 mode, use Wayland */
	setenv_("QT_QPA_PLATFORM", "wayland;xcb");
	setenv_("ELECTRON_OZONE_PLATFORM_HINT", "auto");
	setenv_("MOZ_ENABLE_WAYLAND", "1");
	setenv_("_JAVA_AWT_WM_NONREPARENTING", "1");
}

/* The Wayland socket under its standard name in the runtime folder. */
static void link_wayland(const char *socket)
{
	char *link = rt_path("wayland-0");
	char cur[256] = { 0 };
	long n = sys_readlink(link, cur, sizeof cur - 1);
	if (n <= 0 || !str_eq(cur, socket)) {
		sys_unlink(link);
		sys_symlink(socket, link);
	}
	mem_free(link);
}

/* ---- The session ----------------------------------------------------------------------- */

static char *session_file(const char *name)
{
	return str_fmt("%s/zos-linux/%s", rt, name);
}

/* The running session's process, or 0. */
static int session_pid(void)
{
	char *f = session_file("session.pid");
	char *line = file_read_line(f);
	mem_free(f);
	int pid = line ? (int)str_to_int(line, NULL) : 0;
	mem_free(line);
	if (pid <= 0 || sys_kill(pid, 0) < 0)
		return 0;
	char comm[64];
	fmt(comm, sizeof comm, "/proc/%d/comm", pid);
	char *name = file_read_line(comm);
	bool ours = name && str_eq(name, "layerrun");
	mem_free(name);
	return ours ? pid : 0;
}

static volatile int stopping;

static void on_term(int sig)
{
	(void)sig;
	stopping = 1;
}

/* Answers one request on zos-linux/fuse.sock from the user's fusermount3
 * (or zportal):
 *     mount\nMOUNTPOINT\nOPTIONS   ->  "ok", with /dev/fuse's descriptor
 *     umount\nMOUNTPOINT\nlazy     ->  "ok" */
static void serve_fuse(int listen_fd)
{
	int c = (int)sys_accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
	if (c < 0)
		return;
	struct ucred cred = { 0 };
	u32 clen = sizeof cred;
	char req[4096];
	long n = 0;
	const char *answer = "error: not allowed";
	int fuse_fd = -1;
	struct pollfd p = { c, POLLIN, 0 };
	if (sys_getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0 || cred.uid != (u32)sys_getuid())
		goto done;
	if (sys_poll(&p, 1, 2000) <= 0 || (n = sys_read(c, req, sizeof req - 1)) <= 0)
		goto done;
	req[n] = 0;
	usize nl = 0;
	char **lines = str_split(req, '\n', &nl);
	const char *op = nl > 0 ? lines[0] : "", *mp = nl > 1 ? lines[1] : "", *opts = nl > 2 ? lines[2] : "";
	struct stat st;
	/* a FUSE file system whose program went away leaves a dead mount: it goes first */
	if (mp[0] == '/' && sys_lstat(mp, &st) == -ENOTCONN)
		sys_umount(mp, MNT_DETACH);
	if (mp[0] != '/' || sys_lstat(mp, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != cred.uid) {
		answer = "error: the mount point must be a folder of yours";
	} else if (str_eq(op, "mount")) {
		/* the options fusermount allows a user, in the forms the kernel takes */
		unsigned long flags = MS_NOSUID | MS_NODEV;
		StrBuf kopts = { 0 };
		char fsname[128] = "fuse", type[140] = "fuse";
		usize no = 0;
		char **o = str_split(opts, ',', &no);
		for (usize i = 0; i < no; i++) {
			if (str_eq(o[i], "ro"))
				flags |= MS_RDONLY;
			else if (str_eq(o[i], "noexec"))
				flags |= MS_NOEXEC;
			else if (str_eq(o[i], "noatime"))
				flags |= MS_NOATIME;
			else if (str_eq(o[i], "default_permissions"))
				sb_adds(&kopts, ",default_permissions");
			else if (str_starts(o[i], "max_read=") || str_starts(o[i], "blksize="))
				sb_fmt(&kopts, ",%s", o[i]);
			else if (str_starts(o[i], "fsname="))
				str_copy(fsname, o[i] + 7, sizeof fsname);
			else if (str_starts(o[i], "subtype="))
				fmt(type, sizeof type, "fuse.%s", o[i] + 8);
		}
		str_split_free(o, no);
		fuse_fd = (int)sys_open("/dev/fuse", O_RDWR | O_CLOEXEC, 0);
		if (fuse_fd < 0) {
			answer = "error: no /dev/fuse";
		} else {
			char *data = str_fmt("fd=%d,rootmode=%o,user_id=%u,group_id=%u%s", fuse_fd, st.st_mode & S_IFMT, cred.uid,
					     cred.gid, sb_str(&kopts));
			long r = sys_mount(fsname, mp, type, flags, data);
			mem_free(data);
			if (r < 0) {
				sys_close(fuse_fd);
				fuse_fd = -1;
				answer = "error: the kernel refused the mount";
			} else {
				answer = "ok";
			}
		}
		sb_free(&kopts);
	} else if (str_eq(op, "umount")) {
		/* only FUSE file systems of this user */
		char *info = file_read("/proc/self/mountinfo", NULL);
		char *want = str_fmt(" %s ", mp), *uid = str_fmt("user_id=%u", cred.uid);
		bool mine = false;
		usize ln = 0;
		char **ml = str_split(info ? info : "", '\n', &ln);
		for (usize i = 0; i < ln && !mine; i++)
			mine = str_find(ml[i], want) && str_find(ml[i], " - fuse") && str_find(ml[i], uid);
		str_split_free(ml, ln);
		mem_free(info);
		mem_free(want);
		mem_free(uid);
		if (!mine)
			answer = "error: not a FUSE file system of yours";
		else
			answer = sys_umount(mp, str_eq(opts, "lazy") ? MNT_DETACH : 0) < 0 ? "error: busy" : "ok";
	} else {
		answer = "error: unknown request";
	}
	str_split_free(lines, nl);
done:
	unix_send(c, answer, strlen(answer), fuse_fd >= 0 ? &fuse_fd : NULL, fuse_fd >= 0 ? 1 : 0);
	if (fuse_fd >= 0)
		sys_close(fuse_fd);
	sys_close(c);
}

/* Flathub, in the user's own Flatpak installation (once). */
static int add_flathub(const char *home)
{
	if (!file_exists("/usr/bin/flatpak") || !file_exists("/usr/share/zos/flathub.flatpakrepo"))
		return 0;
	char *conf = str_fmt("%s/.local/share/flatpak/repo/config", home);
	char *text = file_read(conf, NULL);
	bool have = text && str_find(text, "[remote \"flathub\"]");
	mem_free(text);
	mem_free(conf);
	if (have)
		return 0;
	char *argv[] = { "flatpak", "remote-add", "--user", "--if-not-exists", "flathub", "/usr/share/zos/flathub.flatpakrepo", NULL };
	return spawn(argv);
}

/* The session itself: makes the namespace, starts the bus and the sound
 * server, and serves FUSE mounts until it is told to stop. */
static int session_main(const char *user, const char *home)
{
	char *dir = session_file("");
	dir_make(dir, 0700);
	mem_free(dir);
	char *log = session_file("session.log");
	int lfd = (int)sys_open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	mem_free(log);
	if (lfd >= 0) {
		sys_dup3(lfd, 1, 0);
		sys_dup3(lfd, 2, 0);
		sys_close(lfd);
	}
	int null = (int)sys_open("/dev/null", O_RDONLY | O_CLOEXEC, 0);
	if (null >= 0) {
		sys_dup3(null, 0, 0);
		sys_close(null);
	}
	sig_handle(SIGTERM, on_term);
	sig_handle(SIGINT, on_term);
	sig_ignore(SIGHUP);
	sig_ignore(SIGPIPE);
	copy_environ();
	build_root(user, home);
	session_env(user, home);
	/* the mount service first: zportal mounts the document store as soon as the bus is there */
	char *fsock = session_file("fuse.sock");
	int fl = unix_listen(fsock, SOCK_SEQPACKET);
	if (fl >= 0)
		sys_chmod(fsock, 0600);
	char *bus_path = rt_path("bus");
	int bus = start_bus(bus_path);
	int sound = start_sound();
	int flathub = add_flathub(home);
	/* the desktop's zportal answers the portals: the first program should
	 * find them, so wait a moment for it to come onto the new bus */
	const char *display = env_get("ZUI_SOCKET");
	if (bus > 0 && file_exists(display && *display ? display : "/run/zurbax/display.sock")) {
		char *addr = str_fmt("unix:path=%s", bus_path);
		DBus *b = dbus_open_address(addr);
		for (int tries = 0; b && tries < 30 && !dbus_name_has_owner(b, "org.freedesktop.portal.Desktop"); tries++)
			sleep_ms(100);
		dbus_close_bus(b);
		mem_free(addr);
	}
	char *pidfile = session_file("session.pid");
	char pidtext[24];
	fmt(pidtext, sizeof pidtext, "%ld\n", sys_getpid());
	file_write_atomic(pidfile, pidtext, strlen(pidtext));
	print("layerrun: Linux session %ld ready (bus %d, sound %d)\n", sys_getpid(), bus, sound);
	i64 net_mtime = file_mtime("/run/zurbax/network.status");
	int restarts = 0;
	u64 window_start = time_mono_ms();
	while (!stopping) {
		struct pollfd p = { fl, POLLIN, 0 };
		long r = sys_poll(&p, fl >= 0 ? 1 : 0, 1000);
		if (r > 0 && (p.revents & POLLIN))
			serve_fuse(fl);
		/* helpers that stop are started again (a few times a minute at most) */
		int st, w;
		while ((w = (int)sys_wait4(-1, &st, WNOHANG, NULL)) > 0) {
			if (time_mono_ms() - window_start > 60000) {
				window_start = time_mono_ms();
				restarts = 0;
			}
			if (w == bus && !stopping && restarts++ < 5) {
				print("layerrun: the session bus stopped (%d); starting it again\n", st);
				bus = start_bus(bus_path);
			} else if (w == sound && !stopping && restarts++ < 5) {
				print("layerrun: the sound server stopped (%d); starting it again\n", st);
				sound = start_sound();
			} else if (w == flathub) {
				flathub = 0;
			}
		}
		/* a new network may have other name servers */
		i64 m = file_mtime("/run/zurbax/network.status");
		if (m != net_mtime) {
			net_mtime = m;
			char *text = resolv_text();
			file_write_str("/.zos/resolv.conf", text);
			mem_free(text);
		}
	}
	print("layerrun: the Linux session ends\n");
	sys_unlink(pidfile);
	int helpers[] = { sound, bus };
	for (usize k = 0; k < ARRAY_LEN(helpers); k++)
		if (helpers[k] > 0) {
			sys_kill(helpers[k], SIGTERM);
			sys_wait4(helpers[k], NULL, 0, NULL);
		}
	return 0;
}

/* Starts the session unless it's running; returns its pid. */
static int ensure_session(void)
{
	int pid = session_pid();
	if (pid)
		return pid;
	char *dir = session_file("");
	dir_make(dir, 0700);
	mem_free(dir);
	/* one starter at a time: the others wait for its session */
	char *lock = session_file("starting");
	struct stat st;
	bool starter = true;
	int fd = (int)sys_open(lock, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) {
		starter = sys_stat(lock, &st) == 0 && time_unix() - st.st_mtime > 30; /* left by a crash */
		if (starter) {
			sys_unlink(lock);
			fd = (int)sys_open(lock, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			starter = fd >= 0;
		}
	}
	if (fd >= 0)
		sys_close(fd);
	if (starter) {
		char *argv[] = { "layerrun", "--session-daemon", NULL };
		SpawnOpts o = { .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .new_session = true };
		int spid = proc_spawn(proc_self_exe(), argv, NULL, &o);
		if (spid < 0) {
			sys_unlink(lock);
			fail("cannot start the Linux session: %s", err_str(spid));
		}
	}
	for (int tries = 0; tries < 400 && !(pid = session_pid()); tries++)
		sleep_ms(50);
	if (starter)
		sys_unlink(lock);
	mem_free(lock);
	if (!pid) {
		char *log = session_file("session.log");
		char *text = file_read(log, NULL);
		eprint("%s", text ? text : "");
		fail("the Linux session didn't start (see %s)", log);
	}
	return pid;
}

/* Moves this process into the session's namespaces. */
static void join(int pid)
{
	char path[64];
	fmt(path, sizeof path, "/proc/%d/ns/user", pid);
	int ufd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	fmt(path, sizeof path, "/proc/%d/ns/mnt", pid);
	int mfd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (ufd < 0 || mfd < 0)
		fail("cannot reach the Linux session");
	long r = sys_getuid() == 0 ? 0 : sys_setns(ufd, CLONE_NEWUSER);
	if (r >= 0)
		r = sys_setns(mfd, CLONE_NEWNS);
	if (r < 0)
		fail("cannot join the Linux session: %s", err_str(r));
	sys_close(ufd);
	sys_close(mfd);
	sys_chdir("/");
}

/* ---- Running a program ----------------------------------------------------------------- */

/* Waits for the program; when its X server goes first (its window was
 * closed), the program goes too. Then the helpers stop. */
static int wait_program(int pid, int xpid, const int *helpers, usize nhelpers)
{
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
	for (usize k = 0; k < nhelpers; k++)
		if (helpers[k] > 0) {
			sys_kill(helpers[k], SIGTERM);
			sys_wait4(helpers[k], NULL, 0, NULL);
		}
	return (status & 0x7f) ? 128 + (status & 0x7f) : (status >> 8) & 0xff;
}

/* Xwayland and its window manager for --x11; DISPLAY for the program. */
static int start_x11(int *wpid)
{
	int xpid = 0;
	char d[16];
	fmt(d, sizeof d, ":%d", start_x(&xpid));
	setenv_("DISPLAY", d);
	setenv_("XDG_SESSION_TYPE", "x11");
	/* the layer's window manager arranges windows in that screen */
	if (file_exists("/usr/bin/zos-xwm")) {
		char *wm[] = { "zos-xwm", NULL };
		*wpid = spawn(wm);
	}
	return xpid;
}

/* The way layerrun worked before sessions: a namespace and helpers of the
 * program's own, gone when it ends. */
static int run_isolated(char **argv, bool x11, bool sound, const char *app, const char *user, const char *home)
{
	const char *wayland = env_get("WAYLAND_DISPLAY");
	char wl[200];
	str_copy(wl, wayland && wayland[0] == '/' ? wayland : "/run/zurbax/wayland-0", sizeof wl);
	copy_environ();
	build_root(user, home);
	setenv_("PATH", "/usr/local/bin:/usr/bin:/bin");
	setenv_("SHELL", "/bin/bash");
	setenv_("LOGNAME", user);
	setenv_("XDG_RUNTIME_DIR", "/.zos/runtime");
	setenv_("WAYLAND_DISPLAY", wl);
	setenv_("XDG_SESSION_TYPE", x11 ? "x11" : "wayland");
	if (app)
		setenv_("ZOS_APP_ID", app);
	int bpid = 0;
	if (x11) {
		bpid = start_bus("/.zos/runtime/bus");
		setenv_("DBUS_SESSION_BUS_ADDRESS", "unix:path=/.zos/runtime/bus");
	}
	int spid = sound ? start_sound() : 0;
	int wpid = 0, xpid = x11 ? start_x11(&wpid) : 0;
	sys_chdir(home);
	int pid = spawn(argv);
	int helpers[] = { wpid, spid, bpid };
	return wait_program(pid, xpid, helpers, ARRAY_LEN(helpers));
}

int main(int argc, char **argv)
{
	bool x11 = false, sound = false, check_only = false, isolated = false;
	const char *app = NULL, *mode = NULL;
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
		else if (str_eq(argv[i], "--isolated"))
			isolated = true;
		else if (str_eq(argv[i], "--session") || str_eq(argv[i], "--stop") || str_eq(argv[i], "--session-daemon"))
			mode = argv[i];
		else
			break;
	}
	if (check_only)
		return check();
	const char *home = env_get("HOME"), *user = env_get("USER");
	if (!home || !*home)
		home = "/";
	if (!user || !*user)
		user = "user";
	/* another layer than the usual one runs on its own */
	if (!str_eq(layer, DEFAULT_LAYER))
		isolated = true;
	if (isolated) {
		if (i >= argc)
			fail("no program to run");
		return run_isolated(argv + i, x11, sound, app, user, home);
	}
	find_runtime();
	if (mode && str_eq(mode, "--session-daemon"))
		return session_main(user, home);
	if (mode && str_eq(mode, "--stop")) {
		int pid = session_pid();
		if (pid)
			sys_kill(pid, SIGTERM);
		return 0;
	}
	if (mode && str_eq(mode, "--session")) {
		print("%d\n", ensure_session());
		return 0;
	}
	if (i >= argc) {
		eprint("usage: layerrun [--x11] [--app ID] PROGRAM [ARGS...]\n"
		       "       layerrun --session | --stop\n"
		       "       layerrun --isolated [--layer DIR] [--x11] [--sound] PROGRAM [ARGS...]\n"
		       "       layerrun --check [--layer DIR]\n");
		return 2;
	}
	const char *wayland = env_get("WAYLAND_DISPLAY");
	link_wayland(wayland && wayland[0] == '/' ? wayland : "/run/zurbax/wayland-0");
	int pid = ensure_session();
	copy_environ();
	join(pid);
	session_env(user, home);
	if (app)
		setenv_("ZOS_APP_ID", app);
	sys_chdir(home);
	if (!x11) {
		/* the program takes this process's place */
		char *path = find(argv[i]);
		if (!path)
			fail("the layer has no %s", argv[i]);
		long r = sys_execve(path, argv + i, env);
		fail("cannot start %s: %s", path, err_str(r));
	}
	int wpid = 0, xpid = start_x11(&wpid);
	int ppid = spawn(argv + i);
	int helpers[] = { wpid };
	return wait_program(ppid, xpid, helpers, ARRAY_LEN(helpers));
}
