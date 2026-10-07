/*
 * Camera — take photos with the webcam.
 *
 * Talks to the camera through the kernel's V4L2 interface: uncompressed
 * YUYV where the camera offers it (no decoding, so any webcam works),
 * motion JPEG otherwise. The preview is mirrored like a mirror; photos are
 * saved the right way round to Pictures/Camera. A timer, a few looks
 * (warm, cool, mono, vivid) and a flash on the screen.
 *
 * ZURBAX_CAMERA_DEMO=1 shows a moving test picture instead of a camera.
 */
#include "zui.h"

/* ---- V4L2 ------------------------------------------------------------------------------------- */

#define VIDIOC_QUERYCAP 0x80685600u
#define VIDIOC_ENUM_FMT 0xC0405602u
#define VIDIOC_S_FMT 0xC0D05605u
#define VIDIOC_REQBUFS 0xC0145608u
#define VIDIOC_QUERYBUF 0xC0585609u
#define VIDIOC_QBUF 0xC058560Fu
#define VIDIOC_DQBUF 0xC0585611u
#define VIDIOC_STREAMON 0x40045612u
#define VIDIOC_STREAMOFF 0x40045613u
#define VIDIOC_ENUM_FRAMESIZES 0xC02C564Au

#define FOURCC(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)
#define FMT_YUYV FOURCC('Y', 'U', 'Y', 'V')
#define FMT_MJPG FOURCC('M', 'J', 'P', 'G')
#define CAP_VIDEO_CAPTURE 0x1u
#define CAP_STREAMING 0x04000000u
#define CAP_DEVICE_CAPS 0x80000000u
#define BUF_CAPTURE 1
#define MEM_MMAP 1

typedef struct {
	u8 driver[16], card[32], bus_info[32];
	u32 version, capabilities, device_caps, reserved[3];
} V4lCap;

typedef struct {
	u32 index, type, flags;
	u8 description[32];
	u32 pixelformat, mbus_code, reserved[3];
} V4lFmtDesc;

typedef struct {
	u32 type;
	u32 pad;
	union {
		struct {
			u32 width, height, pixelformat, field, bytesperline, sizeimage, colorspace, priv, flags, ycbcr_enc, quantization, xfer_func;
		} pix;
		u8 raw[200];
	} fmt;
} V4lFormat;

typedef struct {
	u32 count, type, memory, capabilities;
	u8 flags, reserved[3];
} V4lReqBufs;

typedef struct {
	u32 index, type, bytesused, flags, field;
	u32 pad0;
	i64 ts_sec, ts_usec;
	u32 tc_type, tc_flags;
	u8 tc_frames, tc_seconds, tc_minutes, tc_hours, tc_user[4];
	u32 sequence, memory;
	union {
		u32 offset;
		u64 userptr;
	} m;
	u32 length, reserved2, request_fd;
	u32 pad1;
} V4lBuffer;

typedef struct {
	u32 index, pixel_format, type;
	union {
		struct {
			u32 width, height;
		} discrete;
		struct {
			u32 min_width, max_width, step_width, min_height, max_height, step_height;
		} stepwise;
	} u;
	u32 reserved[2];
} V4lFrameSize;

_Static_assert(sizeof(V4lCap) == 104, "v4l2_capability");
_Static_assert(sizeof(V4lFmtDesc) == 64, "v4l2_fmtdesc");
_Static_assert(sizeof(V4lFormat) == 208, "v4l2_format");
_Static_assert(sizeof(V4lReqBufs) == 20, "v4l2_requestbuffers");
_Static_assert(sizeof(V4lBuffer) == 88, "v4l2_buffer");
_Static_assert(sizeof(V4lFrameSize) == 44, "v4l2_frmsizeenum");

#define NBUF 4

typedef struct {
	char path[32], name[64];
} CamInfo;

static struct {
	Ui *ui;
	CamInfo cams[8];
	int ncams, cam;
	bool demo;
	/* capture */
	int fd;
	u32 width, height, format, stride;
	void *buf[NBUF];
	usize buf_len[NBUF];
	int run;                 /* 1 while the capture thread runs; 0 asks it to stop */
	int stopped;
	Mutex lock;
	Canvas *frame;           /* latest picture, the right way round */
	u64 frame_no;
	u64 shown_no;
	char error[160];
	/* the app */
	int look;                /* LOOK_* */
	int timer;               /* 0, 3 or 10 seconds */
	f64 countdown_until;
	f64 flash_until;
	Canvas *last_photo;
	char last_path[512];
	Canvas *preview;         /* the frame copy drawn this frame */
} cam;

enum { LOOK_NATURAL, LOOK_WARM, LOOK_COOL, LOOK_MONO, LOOK_VIVID };

static int xioctl(int fd, u32 req, void *arg)
{
	for (;;) {
		long r = sys_ioctl(fd, req, arg);
		if (r != -EINTR)
			return (int)r;
	}
}

static void find_cameras(void)
{
	cam.ncams = 0;
	for (int i = 0; i < 16 && cam.ncams < (int)ARRAY_LEN(cam.cams); i++) {
		char path[32];
		fmt(path, sizeof path, "/dev/video%d", i);
		int fd = (int)sys_open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK, 0);
		if (fd < 0)
			continue;
		V4lCap c = { 0 };
		if (xioctl(fd, VIDIOC_QUERYCAP, &c) == 0) {
			u32 caps = (c.capabilities & CAP_DEVICE_CAPS) ? c.device_caps : c.capabilities;
			/* Webcams expose a second node for metadata; only capture nodes count. */
			if ((caps & CAP_VIDEO_CAPTURE) && (caps & CAP_STREAMING)) {
				CamInfo *ci = &cam.cams[cam.ncams++];
				str_copy(ci->path, path, sizeof ci->path);
				str_copy(ci->name, (const char *)c.card, sizeof ci->name);
			}
		}
		sys_close(fd);
	}
}

/* ---- Pictures ----------------------------------------------------------------------------------- */

static inline u32 pack(int r, int g, int b)
{
	return 0xff000000u | (u32)CLAMP(r, 0, 255) << 16 | (u32)CLAMP(g, 0, 255) << 8 | (u32)CLAMP(b, 0, 255);
}

/* YUYV (BT.601, limited range) to pixels. */
static void yuyv_to_canvas(const u8 *src, u32 w, u32 h, u32 stride, Canvas *dst)
{
	for (u32 y = 0; y < h; y++) {
		const u8 *s = src + (usize)y * stride;
		u32 *d = canvas_row(dst, (int)y);
		for (u32 x = 0; x + 1 < w; x += 2, s += 4) {
			int u = s[1] - 128, v = s[3] - 128;
			int ru = 409 * v, gu = -100 * u - 208 * v, bu = 516 * u;
			int c0 = 298 * (s[0] - 16), c1 = 298 * (s[2] - 16);
			d[x] = pack((c0 + ru + 128) >> 8, (c0 + gu + 128) >> 8, (c0 + bu + 128) >> 8);
			d[x + 1] = pack((c1 + ru + 128) >> 8, (c1 + gu + 128) >> 8, (c1 + bu + 128) >> 8);
		}
	}
}

/* A test picture for the demo: colour bars and a moving circle, as YUYV. */
static void demo_frame(u8 *out, u32 w, u32 h, f64 t)
{
	static const u8 bars[8][3] = { { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
				       { 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 } };
	f32 cx = (f32)w * (0.5f + 0.3f * (f32)sin(t * 1.3)), cy = (f32)h * (0.5f + 0.25f * (f32)cos(t * 0.9)), r = (f32)h * 0.12f;
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x += 2) {
			const u8 *c = bars[x * 8 / w];
			u8 Y = c[0], U = c[1], V = c[2];
			f32 dx = (f32)x - cx, dy = (f32)y - cy;
			if (dx * dx + dy * dy < r * r)
				Y = 235, U = 128, V = 128;
			if (y > h * 3 / 4)
				Y = (u8)(16 + (x * 219 / w)), U = V = 128;
			u8 *p = out + ((usize)y * w + x) * 2;
			p[0] = Y, p[1] = U, p[2] = Y, p[3] = V;
		}
}

static void publish(Canvas *c)
{
	mutex_lock(&cam.lock);
	canvas_free(cam.frame);
	cam.frame = c;
	cam.frame_no++;
	mutex_unlock(&cam.lock);
	ui_wake(cam.ui);
}

/* ---- Capture ------------------------------------------------------------------------------------- */

static void close_camera(void)
{
	if (cam.fd > 0) {
		int type = BUF_CAPTURE;
		xioctl(cam.fd, VIDIOC_STREAMOFF, &type);
		for (int i = 0; i < NBUF; i++)
			if (cam.buf[i])
				sys_munmap(cam.buf[i], cam.buf_len[i]);
		sys_close(cam.fd);
	}
	memset(cam.buf, 0, sizeof cam.buf);
	cam.fd = 0;
}

static bool open_camera(const char *path)
{
	cam.error[0] = 0;
	cam.fd = (int)sys_open(path, O_RDWR | O_CLOEXEC, 0);
	if (cam.fd < 0) {
		cam.fd = 0;
		str_copy(cam.error, "The camera couldn't be opened. Another app may be using it.", sizeof cam.error);
		return false;
	}
	/* YUYV if offered, else MJPEG. */
	bool yuyv = false, mjpg = false;
	for (u32 i = 0; i < 64; i++) {
		V4lFmtDesc d = { .index = i, .type = BUF_CAPTURE };
		if (xioctl(cam.fd, VIDIOC_ENUM_FMT, &d) < 0)
			break;
		yuyv |= d.pixelformat == FMT_YUYV;
		mjpg |= d.pixelformat == FMT_MJPG;
	}
	if (!yuyv && !mjpg) {
		str_copy(cam.error, "This camera sends pictures in a format ZOS doesn't read yet.", sizeof cam.error);
		close_camera();
		return false;
	}
	u32 want = yuyv ? FMT_YUYV : FMT_MJPG;
	/* The largest size up to 1280x720 (YUYV gets slow beyond that over USB 2). */
	u32 bw = 640, bh = 480;
	for (u32 i = 0; i < 64; i++) {
		V4lFrameSize fs = { .index = i, .pixel_format = want };
		if (xioctl(cam.fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0)
			break;
		u32 w = fs.type == 1 ? fs.u.discrete.width : MIN(fs.u.stepwise.max_width, 1280u);
		u32 h = fs.type == 1 ? fs.u.discrete.height : MIN(fs.u.stepwise.max_height, 720u);
		if (w <= 1280 && h <= 720 && w * h > bw * bh)
			bw = w, bh = h;
	}
	V4lFormat f = { .type = BUF_CAPTURE };
	f.fmt.pix.width = bw;
	f.fmt.pix.height = bh;
	f.fmt.pix.pixelformat = want;
	f.fmt.pix.field = 1; /* progressive */
	if (xioctl(cam.fd, VIDIOC_S_FMT, &f) < 0) {
		str_copy(cam.error, "The camera refused the picture size.", sizeof cam.error);
		close_camera();
		return false;
	}
	cam.width = f.fmt.pix.width;
	cam.height = f.fmt.pix.height;
	cam.format = f.fmt.pix.pixelformat;
	cam.stride = f.fmt.pix.bytesperline ? f.fmt.pix.bytesperline : cam.width * 2;
	V4lReqBufs rb = { .count = NBUF, .type = BUF_CAPTURE, .memory = MEM_MMAP };
	if (xioctl(cam.fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
		str_copy(cam.error, "The camera couldn't start.", sizeof cam.error);
		close_camera();
		return false;
	}
	for (u32 i = 0; i < rb.count && i < NBUF; i++) {
		V4lBuffer b = { .index = i, .type = BUF_CAPTURE, .memory = MEM_MMAP };
		if (xioctl(cam.fd, VIDIOC_QUERYBUF, &b) < 0)
			break;
		void *m = sys_mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam.fd, b.m.offset);
		if ((long)m < 0 && (long)m > -4096)
			break;
		cam.buf[i] = m;
		cam.buf_len[i] = b.length;
		xioctl(cam.fd, VIDIOC_QBUF, &b);
	}
	int type = BUF_CAPTURE;
	if (xioctl(cam.fd, VIDIOC_STREAMON, &type) < 0) {
		str_copy(cam.error, "The camera couldn't start streaming.", sizeof cam.error);
		close_camera();
		return false;
	}
	return true;
}

static void capture_thread(void *arg)
{
	UNUSED(arg);
	u8 *demo = NULL;
	f64 t0 = (f64)time_mono_ns() / 1e9;
	if (cam.demo) {
		cam.width = 1280, cam.height = 720, cam.format = FMT_YUYV;
		demo = mem_alloc((usize)cam.width * cam.height * 2);
	}
	while (atomic_load(&cam.run)) {
		Canvas *c = NULL;
		if (cam.demo) {
			demo_frame(demo, cam.width, cam.height, (f64)time_mono_ns() / 1e9 - t0);
			c = canvas_new((int)cam.width, (int)cam.height);
			yuyv_to_canvas(demo, cam.width, cam.height, cam.width * 2, c);
			sleep_ms(33);
		} else {
			struct pollfd p = { cam.fd, POLLIN, 0 };
			if (sys_poll(&p, 1, 200) <= 0)
				continue;
			V4lBuffer b = { .type = BUF_CAPTURE, .memory = MEM_MMAP };
			if (xioctl(cam.fd, VIDIOC_DQBUF, &b) < 0) {
				str_copy(cam.error, "The camera stopped sending pictures.", sizeof cam.error);
				ui_wake(cam.ui);
				break;
			}
			if (b.index < NBUF && cam.buf[b.index] && b.bytesused) {
				if (cam.format == FMT_YUYV) {
					c = canvas_new((int)cam.width, (int)cam.height);
					yuyv_to_canvas(cam.buf[b.index], cam.width, cam.height, cam.stride, c);
				} else {
					c = jpeg_decode(cam.buf[b.index], b.bytesused, 1);
				}
			}
			xioctl(cam.fd, VIDIOC_QBUF, &b);
		}
		if (c)
			publish(c);
	}
	mem_free(demo);
	atomic_store(&cam.stopped, 1);
}

static void start_capture(void)
{
	if (!cam.demo) {
		if (cam.ncams == 0 || !open_camera(cam.cams[cam.cam].path))
			return;
	}
	atomic_store(&cam.run, 1);
	atomic_store(&cam.stopped, 0);
	thread_spawn(capture_thread, NULL);
}

static void stop_capture(void)
{
	if (!atomic_load(&cam.run))
		return;
	atomic_store(&cam.run, 0);
	for (int i = 0; i < 100 && !atomic_load(&cam.stopped); i++)
		sleep_ms(10);
	close_camera();
}

/* ---- Looks and photos ------------------------------------------------------------------------- */

static void apply_look(Canvas *c, int look)
{
	if (look == LOOK_NATURAL)
		return;
	for (int y = 0; y < c->height; y++) {
		u32 *row = canvas_row(c, y);
		for (int x = 0; x < c->width; x++) {
			u32 p = row[x];
			int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
			switch (look) {
			case LOOK_WARM: r = r * 110 / 100 + 6, g = g * 102 / 100, b = b * 86 / 100; break;
			case LOOK_COOL: r = r * 88 / 100, g = g * 100 / 100 + 2, b = b * 112 / 100 + 6; break;
			case LOOK_MONO: {
				int l = (r * 77 + g * 150 + b * 29) >> 8;
				l = (l - 128) * 112 / 100 + 128;
				r = g = b = l;
				break;
			}
			case LOOK_VIVID: {
				int l = (r * 77 + g * 150 + b * 29) >> 8;
				r = l + (r - l) * 145 / 100, g = l + (g - l) * 145 / 100, b = l + (b - l) * 145 / 100;
				r = (r - 128) * 108 / 100 + 128, g = (g - 128) * 108 / 100 + 128, b = (b - 128) * 108 / 100 + 128;
				break;
			}
			}
			row[x] = pack(r, g, b);
		}
	}
}

static void take_photo(Ui *ui)
{
	mutex_lock(&cam.lock);
	Canvas *src = cam.frame;
	Canvas *photo = NULL;
	if (src) {
		photo = canvas_new(src->width, src->height);
		for (int y = 0; y < src->height; y++)
			memcpy(canvas_row(photo, y), canvas_row(src, y), sizeof(u32) * (usize)src->width);
	}
	mutex_unlock(&cam.lock);
	if (!photo)
		return;
	apply_look(photo, cam.look);
	Civil c;
	civil_local(time_unix(), &c);
	char *dir = str_fmt("%s/Pictures/Camera", home_dir());
	dir_make(dir, 0755);
	fmt(cam.last_path, sizeof cam.last_path, "%s/Photo %04d-%02d-%02d %02d.%02d.%02d.png", dir, c.year, c.month, c.day, c.hour, c.min, c.sec);
	mem_free(dir);
	png_save(photo, cam.last_path);
	canvas_free(cam.last_photo);
	cam.last_photo = photo;
	cam.flash_until = ui->now + 0.35;
	sound_play("camera");
}

/* ---- Window ----------------------------------------------------------------------------------- */

static bool round_button(Ui *ui, Rect r, const char *id, const char *icon, bool on)
{
	UiInteract it = ui_interact(ui, ui_id(ui, id), r);
	f32 h = ui_fade(ui, ui_id(ui, id), it.hovered, 0.12f);
	ui_circle(ui, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, color_mix(color_hex(on ? "#ffffff40" : "#ffffff18"), color_hex("#ffffff50"), h));
	ui_icon(ui, center_in(r, r.w * 0.48f, r.w * 0.48f), icon, COLOR_WHITE);
	return it.clicked;
}

static void frame(Ui *ui)
{
	ui_window_background(ui, color_hex("#000000"));
	Rect win = rect(0, 0, ui->width, ui->height);
	ui_fill(ui, win, color_hex("#0b0b0e"));
	/* The picture fills the window, mirrored. */
	mutex_lock(&cam.lock);
	if (cam.frame && cam.frame_no != cam.shown_no) {
		canvas_free(cam.preview);
		cam.preview = canvas_new(cam.frame->width, cam.frame->height);
		for (int y = 0; y < cam.frame->height; y++) {
			const u32 *s = canvas_row(cam.frame, y);
			u32 *d = canvas_row(cam.preview, y);
			for (int x = 0; x < cam.frame->width; x++)
				d[x] = s[cam.frame->width - 1 - x];
		}
		cam.shown_no = cam.frame_no;
		apply_look(cam.preview, cam.look);
	}
	mutex_unlock(&cam.lock);
	Rect controls = cut_bottom(&win, 110);
	Rect view = win;
	if (cam.preview) {
		f32 s = minf(view.w / (f32)cam.preview->width, view.h / (f32)cam.preview->height);
		Rect pr = center_in(view, (f32)cam.preview->width * s, (f32)cam.preview->height * s);
		ui_image(ui, pr, cam.preview, 0, false);
	} else {
		Rect c = center_in(view, minf(420, view.w), 160);
		ui_icon(ui, center_in(cut_top(&c, 70), 64, 64), "camera", color_hex("#ffffff80"));
		const char *title = cam.error[0] ? "The camera isn't working" : cam.ncams || cam.demo ? "Starting the camera…" : "No camera found";
		const char *sub = cam.error[0] ? cam.error : cam.ncams || cam.demo ? "" : "Plug in a webcam and it shows up here.";
		ui_text_in(ui, ui_font(ui, 20, 700), cut_top(&c, 30), title, COLOR_WHITE, ALIGN_CENTER);
		ui_text_wrapped(ui, ui_font(ui, 14, 500), c, sub, color_hex("#ffffffa0"), ALIGN_CENTER);
		if (!cam.ncams && !cam.demo && ui->now > 1) {
			/* Look for a camera now and then. */
			static f64 next_look;
			if (ui->now >= next_look) {
				next_look = ui->now + 2;
				find_cameras();
				if (cam.ncams)
					start_capture();
			}
			ui->wake_at = next_look;
		}
	}
	/* The header floats over the picture. */
	Theme saved = ui->theme;
	ui->theme.text = ui->theme.text_dim = COLOR_WHITE;
	ui->theme.hover = color_hex("#ffffff30");
	ui_gradient(ui, rect(0, 0, ui->width, 70), 0, color_hex("#00000090"), color_hex("#00000000"));
	ui_headerbar(ui, rect(0, 0, ui->width, 46), "", HEADER_TRANSPARENT | HEADER_FLAT | HEADER_NO_TITLE);
	ui->theme = saved;
	/* Countdown and flash. */
	if (cam.countdown_until > 0) {
		f64 left = cam.countdown_until - ui->now;
		if (left <= 0) {
			cam.countdown_until = 0;
			take_photo(ui);
		} else {
			char n[8];
			fmt(n, sizeof n, "%d", (int)ceil(left));
			ui_text_in(ui, ui_font(ui, 140, 800), view, n, color_hex("#ffffffd0"), ALIGN_CENTER);
			ui->redraw = true;
		}
	}
	if (ui->now < cam.flash_until) {
		f32 k = (f32)((cam.flash_until - ui->now) / 0.35);
		ui_fill(ui, view, color_scale_alpha(COLOR_WHITE, k * k));
		ui->redraw = true;
	}
	/* Controls: last photo, looks, shutter, timer. */
	ui_fill(ui, controls, color_hex("#0b0b0e"));
	f32 cy = controls.y + controls.h / 2;
	Rect shutter = rect(controls.x + controls.w / 2 - 34, cy - 34, 68, 68);
	UiInteract sit = ui_interact(ui, ui_id(ui, "shutter"), shutter);
	ui_ring(ui, shutter.x + 34, cy, 32, 4, COLOR_WHITE);
	ui_circle(ui, shutter.x + 34, cy, sit.held ? 23 : 26, color_hex(cam.countdown_until > 0 ? "#ff5a5f" : "#ffffff"));
	if ((sit.clicked || ui_key_pressed(ui, KEY_SPACE, 0) || ui_key_pressed(ui, KEY_ENTER, 0)) && cam.preview) {
		if (cam.countdown_until > 0)
			cam.countdown_until = 0; /* a second press cancels the timer */
		else if (cam.timer)
			cam.countdown_until = ui->now + cam.timer;
		else
			take_photo(ui);
	}
	Rect thumb = rect(controls.x + 30, cy - 28, 56, 56);
	if (cam.last_photo) {
		ui_image(ui, thumb, cam.last_photo, 10, true);
		ui_border(ui, thumb, 10, 2, color_hex("#ffffff60"));
		if (ui_interact(ui, ui_id(ui, "thumb"), thumb).clicked)
			ui_launch(ui, "photos", cam.last_path);
	}
	static const char *const looks[] = { "Natural", "Warm", "Cool", "Mono", "Vivid" };
	Rect lr = rect(thumb.x + thumb.w + 30, cy - 17, minf(360, shutter.x - thumb.x - thumb.w - 60), 34);
	if (lr.w > 200)
		ui_segmented(ui, lr, looks, 5, &cam.look);
	Rect tr = rect(shutter.x + shutter.w + 40, cy - 22, 44, 44);
	if (round_button(ui, tr, "timer", "timer", cam.timer > 0))
		cam.timer = cam.timer == 0 ? 3 : cam.timer == 3 ? 10 : 0;
	if (cam.timer) {
		char tl[8];
		fmt(tl, sizeof tl, "%ds", cam.timer);
		ui_text_in(ui, ui_font(ui, 13, 700), rect(tr.x + tr.w + 8, tr.y, 40, tr.h), tl, COLOR_WHITE, ALIGN_START);
	}
	if (cam.ncams > 1) {
		Rect sw = rect(controls.x + controls.w - 74, cy - 22, 44, 44);
		if (round_button(ui, sw, "switch", "refresh", false)) {
			stop_capture();
			cam.cam = (cam.cam + 1) % cam.ncams;
			start_capture();
		}
	}
	if (atomic_load(&cam.run))
		ui->wake_at = ui->now + 0.05;
}

static bool on_close(Ui *ui)
{
	UNUSED(ui);
	stop_capture();
	return true;
}

static void init(Ui *ui)
{
	cam.ui = ui;
	cam.demo = env_get("ZURBAX_CAMERA_DEMO") != NULL;
	if (!cam.demo)
		find_cameras();
	start_capture();
}

int main(void)
{
	UiAppDesc d = { .id = "camera", .title = "Camera", .width = 960, .height = 680, .min_width = 640, .min_height = 480, .flags = WIN_RESIZABLE,
			.init = init, .frame = frame, .close = on_close };
	return ui_run(&d);
}
