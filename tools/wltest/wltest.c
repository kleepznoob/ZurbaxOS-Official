/* wltest: a small Wayland program for testing zdisplay's Wayland server
 * inside ZOS, where no other Wayland programs exist.
 *
 *     wltest [dmabuf] [fullscreen] [frames=N] [delay=SECONDS]
 *
 * It opens a window and animates it for N frames, drawing into shared
 * memory, or with "dmabuf" into GPU buffers: dumb buffers on the display's
 * own GPU (the one zdisplay's linux-dmabuf feedback names), shared through
 * PRIME. "fullscreen" asks for the whole screen, where zdisplay can show the
 * buffers directly. The result goes to the kernel log:
 *
 *     wltest: 300 frames in 5012 ms (59.9 fps), 298 releases, dmabuf on 226:0
 *
 * Test builds start it from /system/test with the options in
 * zurbax.wltest=dmabuf,fullscreen,frames=300,delay=20 on the kernel command
 * line; it is never part of a release. */
#include "zr.h"

/* ---- Logging ---------------------------------------------------------------------------- */

static void say(const char *f, ...) PRINTF_LIKE(1, 2);
static void say(const char *f, ...)
{
	char msg[256] = "wltest: ";
	va_list ap;
	va_start(ap, f);
	fmt_v(msg + 8, sizeof msg - 9, f, ap);
	va_end(ap);
	str_copy(msg + strlen(msg), "\n", 2);
	eprint("%s", msg);
	int k = (int)sys_open("/dev/kmsg", O_WRONLY | O_CLOEXEC, 0);
	if (k >= 0) {
		write_all(k, msg, strlen(msg));
		sys_close(k);
	}
}

/* ---- The wire ----------------------------------------------------------------------------- */

static int sock = -1;
static u8 in[65536];
static usize in_len;
static int in_fds[16], nin_fds;
static u32 next_id = 2;

/* A request: i u f o n are 32-bit words, s a string, h a file descriptor
 * (closed here once sent: the server has its own copy). */
static void req(u32 id, u32 op, const char *sig, ...)
{
	u8 msg[512];
	usize n = 8;
	int fds[4], nfds = 0;
	va_list ap;
	va_start(ap, sig);
	for (const char *p = sig; *p; p++) {
		if (*p == 's') {
			const char *s = va_arg(ap, const char *);
			u32 len = (u32)strlen(s) + 1, padded = (len + 3) & ~3u;
			memcpy(msg + n, &len, 4);
			memset(msg + n + 4, 0, padded);
			memcpy(msg + n + 4, s, len);
			n += 4 + padded;
		} else if (*p == 'h') {
			fds[nfds++] = va_arg(ap, int);
		} else {
			u32 v = va_arg(ap, u32);
			memcpy(msg + n, &v, 4);
			n += 4;
		}
	}
	va_end(ap);
	u32 word = (u32)n << 16 | op;
	memcpy(msg, &id, 4);
	memcpy(msg + 4, &word, 4);
	if (unix_send(sock, msg, n, fds, nfds) < 0) {
		say("lost the connection");
		exit(1);
	}
	for (int i = 0; i < nfds; i++)
		sys_close(fds[i]);
}

static u32 new_id(void)
{
	return next_id++;
}

/* ---- State ----------------------------------------------------------------------------------- */

enum { ID_DISPLAY = 1, ID_REGISTRY = 2 };

static u32 compositor, shm, wm_base, dmabuf_global, dmabuf_version;
static u32 surface, xdg_surface, toplevel, feedback;
static u32 sync_cb, frame_cb;
static bool synced, configured, frame_done, closed;
static i32 conf_w, conf_h;
static u64 main_device;
static bool have_device;

typedef struct {
	u32 id;
	bool busy;
	u32 *pixels;
	int width, height, stride; /* stride in pixels */
} Buffer;

static Buffer bufs[2];
static int releases;

/* ---- Events ------------------------------------------------------------------------------------ */

static u32 word(const u8 *p)
{
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

static void on_event(u32 id, u32 op, const u8 *p, u32 len)
{
	if (id == ID_DISPLAY && op == 0) {
		u32 slen = word(p + 8);
		say("protocol error %u on object %u: %.*s", word(p + 4), word(p), (int)slen, (const char *)p + 12);
		exit(1);
	}
	if (id == ID_REGISTRY && op == 0) {
		u32 name = word(p), slen = word(p + 4);
		const char *iface = (const char *)p + 8;
		u32 version = word(p + 8 + ((slen + 3) & ~3u));
		if (str_eq(iface, "wl_compositor"))
			req(ID_REGISTRY, 0, "usun", name, iface, 4u, compositor = new_id());
		else if (str_eq(iface, "wl_shm"))
			req(ID_REGISTRY, 0, "usun", name, iface, 1u, shm = new_id());
		else if (str_eq(iface, "xdg_wm_base"))
			req(ID_REGISTRY, 0, "usun", name, iface, 1u, wm_base = new_id());
		else if (str_eq(iface, "zwp_linux_dmabuf_v1") && version >= 4)
			req(ID_REGISTRY, 0, "usun", name, iface, dmabuf_version = 4, dmabuf_global = new_id());
		return;
	}
	if (id == sync_cb && op == 0) {
		synced = true;
		return;
	}
	if (id == frame_cb && op == 0) {
		frame_done = true;
		return;
	}
	if (id == wm_base && op == 0) {
		req(wm_base, 3, "u", word(p)); /* pong */
		return;
	}
	if (id == xdg_surface && op == 0) {
		req(xdg_surface, 4, "u", word(p)); /* ack_configure */
		configured = true;
		return;
	}
	if (id == toplevel && op == 0) {
		conf_w = (i32)word(p), conf_h = (i32)word(p + 4);
		return;
	}
	if (id == toplevel && op == 1) {
		closed = true;
		return;
	}
	if (id == feedback && op == 2 && len >= 12 && word(p) == 8) {
		memcpy(&main_device, p + 4, 8); /* main_device */
		have_device = true;
		return;
	}
	if (id == feedback && op == 1 && nin_fds > 0) {
		/* format_table: we know what zdisplay offers; just close the table */
		sys_close(in_fds[0]);
		memmove(in_fds, in_fds + 1, sizeof(int) * (usize)--nin_fds);
		return;
	}
	for (int i = 0; i < 2; i++)
		if (bufs[i].id && id == bufs[i].id && op == 0) {
			bufs[i].busy = false;
			releases++;
		}
}

/* Reads what the server sent and handles every complete event. */
static void dispatch(void)
{
	int fds[8], nfds = 0;
	long n = unix_recv(sock, in + in_len, sizeof in - in_len, fds, 8, &nfds);
	if (n <= 0) {
		say("server closed the connection");
		exit(1);
	}
	for (int i = 0; i < nfds && nin_fds < 16; i++)
		in_fds[nin_fds++] = fds[i];
	in_len += (usize)n;
	usize at = 0;
	while (in_len - at >= 8) {
		u32 id = word(in + at), w = word(in + at + 4), size = w >> 16;
		if (size < 8 || in_len - at < size)
			break;
		on_event(id, w & 0xffff, in + at + 8, size - 8);
		at += size;
	}
	memmove(in, in + at, in_len - at);
	in_len -= at;
}

static void roundtrip(void)
{
	synced = false;
	req(ID_DISPLAY, 0, "n", sync_cb = new_id());
	while (!synced)
		dispatch();
}

/* ---- Buffers ------------------------------------------------------------------------------------ */

static bool shm_buffers(int w, int h)
{
	usize one = (usize)w * (usize)h * 4;
	int fd = (int)sys_memfd_create("wltest", MFD_CLOEXEC);
	if (fd < 0 || sys_ftruncate(fd, (i64)(one * 2)) < 0)
		return false;
	u8 *map = sys_mmap(NULL, one * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if ((unsigned long)map > -4096ul)
		return false;
	u32 pool = new_id();
	req(shm, 0, "nhi", pool, fd, (u32)(one * 2));
	for (int i = 0; i < 2; i++) {
		bufs[i] = (Buffer){ .id = new_id(), .pixels = (u32 *)(map + one * (usize)i), .width = w, .height = h, .stride = w };
		req(pool, 0, "niiiiu", bufs[i].id, (u32)(one * (usize)i), (u32)w, (u32)h, (u32)(w * 4), 1u /* XRGB */);
	}
	req(pool, 1, ""); /* the buffers keep the memory */
	return true;
}

struct drm_mode_create_dumb {
	u32 height, width, bpp, flags, handle, pitch;
	u64 size;
};
struct drm_mode_map_dumb {
	u32 handle, pad;
	u64 offset;
};
struct drm_prime_handle {
	u32 handle, flags;
	i32 fd;
};
#define DRM_IOCTL_MODE_CREATE_DUMB IOWR('d', 0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB IOWR('d', 0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD IOWR('d', 0x2d, struct drm_prime_handle)

/* The GPU zdisplay named: the /dev/dri node with that device number. */
static int open_gpu(void)
{
	for (int i = 0; i < 8; i++) {
		char path[32];
		fmt(path, sizeof path, "/dev/dri/card%d", i);
		struct stat st;
		if (sys_stat(path, &st) == 0 && st.st_rdev == main_device)
			return (int)sys_open(path, O_RDWR | O_CLOEXEC, 0);
	}
	return -1;
}

static bool dmabuf_buffers(int w, int h)
{
	feedback = new_id();
	req(dmabuf_global, 2, "n", feedback); /* get_default_feedback */
	roundtrip();
	if (!have_device) {
		say("no main device in the dmabuf feedback");
		return false;
	}
	int gpu = open_gpu();
	if (gpu < 0) {
		say("cannot open the GPU %u:%u", (u32)(main_device >> 8) & 0xfff, (u32)(main_device & 0xff));
		return false;
	}
	for (int i = 0; i < 2; i++) {
		struct drm_mode_create_dumb c = { .width = (u32)w, .height = (u32)h, .bpp = 32 };
		struct drm_mode_map_dumb m = { 0 };
		struct drm_prime_handle ph = { 0 };
		if (sys_ioctl(gpu, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) {
			say("cannot make a GPU buffer");
			return false;
		}
		m.handle = c.handle;
		ph.handle = c.handle, ph.flags = O_CLOEXEC | O_RDWR;
		if (sys_ioctl(gpu, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0 || sys_ioctl(gpu, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) < 0) {
			say("cannot share a GPU buffer");
			return false;
		}
		u32 *px = sys_mmap(NULL, (usize)c.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu, (i64)m.offset);
		if ((unsigned long)px > -4096ul)
			return false;
		u32 params = new_id();
		bufs[i] = (Buffer){ .id = new_id(), .pixels = px, .width = w, .height = h, .stride = (int)(c.pitch / 4) };
		req(dmabuf_global, 1, "n", params);
		req(params, 1, "huuuuu", ph.fd, 0u, 0u, c.pitch, 0u, 0u); /* linear */
		req(params, 3, "niiuu", bufs[i].id, (u32)w, (u32)h, 0x34325258u /* XR24 */, 0u);
		req(params, 0, "");
	}
	return true;
}

/* A gradient with a bar sweeping across, different every frame. */
static void draw(Buffer *b, int frame)
{
	int bar = (frame * 8) % b->width;
	for (int y = 0; y < b->height; y++) {
		u32 *row = b->pixels + (usize)y * (usize)b->stride;
		u32 g = (u32)(y * 255 / b->height);
		for (int x = 0; x < b->width; x++) {
			bool on_bar = x >= bar && x < bar + 40;
			row[x] = on_bar ? 0xffffd040u : 0xff000000u | (u32)(x * 255 / b->width) << 16 | g << 8 | 0x60;
		}
	}
}

/* ---- Main ------------------------------------------------------------------------------------------------- */

static bool option(const char *opts, const char *name)
{
	usize n = strlen(name);
	for (const char *p = opts; (p = str_find(p, name)); p += n)
		if ((p == opts || p[-1] == ',' || p[-1] == ' ') && (p[n] == 0 || p[n] == ',' || p[n] == ' ' || p[n] == '='))
			return true;
	return false;
}

static long option_int(const char *opts, const char *name, long fallback)
{
	char key[32];
	fmt(key, sizeof key, "%s=", name);
	const char *p = str_find(opts, key);
	return p ? str_to_int(p + strlen(key), NULL) : fallback;
}

int main(int argc, char **argv)
{
	/* options from the arguments, or from the kernel command line as a test hook */
	char opts[256] = "";
	for (int i = 1; i < argc; i++) {
		str_copy(opts + strlen(opts), argv[i], sizeof opts - strlen(opts));
		str_copy(opts + strlen(opts), ",", sizeof opts - strlen(opts));
	}
	if (argc == 1) {
		char *cmdline = file_read("/proc/cmdline", NULL);
		const char *p = cmdline ? str_find(cmdline, "zurbax.wltest=") : NULL;
		if (!p) {
			mem_free(cmdline);
			return 0; /* not asked for */
		}
		str_copy(opts, p + strlen("zurbax.wltest="), sizeof opts);
		mem_free(cmdline);
	}
	bool use_dmabuf = option(opts, "dmabuf"), fullscreen = option(opts, "fullscreen");
	long frames = option_int(opts, "frames", 120), delay = option_int(opts, "delay", 0);
	if (delay > 0)
		sleep_ms((u64)delay * 1000);

	char path[160];
	const char *disp = env_get("WAYLAND_DISPLAY"), *rt = env_get("XDG_RUNTIME_DIR");
	if (disp && disp[0] == '/')
		str_copy(path, disp, sizeof path);
	else if (rt && *rt)
		fmt(path, sizeof path, "%s/%s", rt, disp && *disp ? disp : "wayland-0");
	else
		str_copy(path, "/run/zurbax/wayland-0", sizeof path);
	for (int tries = 0; (sock = unix_connect(path, SOCK_STREAM)) < 0; tries++) {
		if (tries == 30) {
			say("no Wayland server at %s", path);
			return 1;
		}
		sleep_ms(1000);
	}
	req(ID_DISPLAY, 1, "n", ID_REGISTRY);
	next_id = 3;
	roundtrip();
	if (!compositor || !wm_base || !shm) {
		say("the server lacks the basics");
		return 1;
	}
	if (use_dmabuf && !dmabuf_global) {
		say("no linux-dmabuf: the display isn't on a GPU driver");
		return 1;
	}
	req(compositor, 0, "n", surface = new_id());
	req(wm_base, 2, "no", xdg_surface = new_id(), surface);
	req(xdg_surface, 1, "n", toplevel = new_id());
	req(toplevel, 2, "s", "wltest");
	req(toplevel, 3, "s", "wltest");
	if (fullscreen)
		req(toplevel, 11, "o", 0u);
	req(surface, 6, ""); /* the initial commit */
	while (!configured)
		dispatch();
	int w = conf_w > 0 ? conf_w : 480, h = conf_h > 0 ? conf_h : 320;
	if (!(use_dmabuf ? dmabuf_buffers(w, h) : shm_buffers(w, h)))
		return 1;
	u64 t0 = time_mono_ms();
	int shown = 0;
	for (int f = 0; f < frames && !closed; f++) {
		Buffer *b = !bufs[0].busy ? &bufs[0] : !bufs[1].busy ? &bufs[1] : NULL;
		while (!b) {
			dispatch();
			b = !bufs[0].busy ? &bufs[0] : !bufs[1].busy ? &bufs[1] : NULL;
		}
		draw(b, f);
		req(surface, 1, "oii", b->id, 0u, 0u);
		req(surface, 2, "iiii", 0u, 0u, (u32)w, (u32)h);
		req(surface, 3, "n", frame_cb = new_id());
		req(surface, 6, "");
		b->busy = true;
		frame_done = false;
		while (!frame_done && !closed)
			dispatch();
		shown++;
	}
	u64 ms = time_mono_ms() - t0;
	char dev[32] = "";
	if (use_dmabuf)
		fmt(dev, sizeof dev, " on %u:%u", (u32)(main_device >> 8) & 0xfff, (u32)(main_device & 0xff));
	say("%d frames of %dx%d in %u ms (%.1f fps), %d releases, %s%s%s", shown, w, h, (u32)ms,
	    ms ? (double)shown * 1000.0 / (double)ms : 0.0, releases, use_dmabuf ? "dmabuf" : "shm", dev,
	    fullscreen ? ", full screen" : "");
	return 0;
}
