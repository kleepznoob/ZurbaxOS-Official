/*
 * DRM/KMS output: drives the screen through the kernel's mode-setting
 * interface with two "dumb" buffers and page flipping.
 *
 * The first mode set is delayed until the first frame is ready, and that
 * frame is the boot splash showing the firmware logo in the same place, so
 * the transition from firmware to Zurbax is invisible. When the panel is
 * already running in its native mode (firmware or a fast-booting driver set
 * it) the kernel only swaps the buffer and no blank flash occurs.
 *
 * The uapi structures are written out here; they are the stable kernel ABI.
 */
#include "zdisplay.h"

/* ---- Kernel ABI ------------------------------------------------------------------- */

struct drm_mode_modeinfo {
	u32 clock;
	u16 hdisplay, hsync_start, hsync_end, htotal, hskew;
	u16 vdisplay, vsync_start, vsync_end, vtotal, vscan;
	u32 vrefresh;
	u32 flags;
	u32 type;
	char name[32];
};

struct drm_mode_card_res {
	u64 fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
	u32 count_fbs, count_crtcs, count_connectors, count_encoders;
	u32 min_width, max_width, min_height, max_height;
};

struct drm_mode_crtc {
	u64 set_connectors_ptr;
	u32 count_connectors;
	u32 crtc_id;
	u32 fb_id;
	u32 x, y;
	u32 gamma_size;
	u32 mode_valid;
	struct drm_mode_modeinfo mode;
};

struct drm_mode_get_encoder {
	u32 encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct drm_mode_get_connector {
	u64 encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
	u32 count_modes, count_props, count_encoders;
	u32 encoder_id, connector_id, connector_type, connector_type_id;
	u32 connection, mm_width, mm_height, subpixel, pad;
};

struct drm_mode_fb_cmd {
	u32 fb_id, width, height, pitch, bpp, depth, handle;
};

struct drm_mode_crtc_page_flip {
	u32 crtc_id, fb_id, flags, reserved;
	u64 user_data;
};

struct drm_clip_rect {
	u16 x1, y1, x2, y2;
};

struct drm_mode_fb_dirty_cmd {
	u32 fb_id, flags, color, num_clips;
	u64 clips_ptr;
};

struct drm_mode_create_dumb {
	u32 height, width, bpp, flags, handle, pitch;
	u64 size;
};

struct drm_mode_map_dumb {
	u32 handle, pad;
	u64 offset;
};

struct drm_mode_destroy_dumb {
	u32 handle;
};

struct drm_mode_cursor2 {
	u32 flags, crtc_id;
	i32 x, y;
	u32 width, height, handle;
	i32 hot_x, hot_y;
};

struct drm_mode_crtc_lut {
	u32 crtc_id, gamma_size;
	u64 red, green, blue;
};

struct drm_get_cap {
	u64 capability, value;
};

struct drm_version {
	i32 major, minor, patchlevel;
	usize name_len;
	u64 name;
	usize date_len;
	u64 date;
	usize desc_len;
	u64 desc;
};

struct drm_event {
	u32 type, length;
};

struct drm_mode_fb_cmd2 {
	u32 fb_id, width, height, pixel_format, flags;
	u32 handles[4], pitches[4], offsets[4];
	u64 modifier[4];
};

struct drm_prime_handle {
	u32 handle, flags;
	i32 fd;
};

struct drm_gem_close {
	u32 handle, pad;
};

#define DRM_IOCTL_VERSION IOWR('d', 0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP IOWR('d', 0x0c, struct drm_get_cap)
#define DRM_IOCTL_SET_MASTER IO('d', 0x1e)
#define DRM_IOCTL_DROP_MASTER IO('d', 0x1f)
#define DRM_IOCTL_DROP_MASTER IO('d', 0x1f)
#define DRM_IOCTL_MODE_GETRESOURCES IOWR('d', 0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC IOWR('d', 0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC IOWR('d', 0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETGAMMA IOWR('d', 0xA5, struct drm_mode_crtc_lut)
#define DRM_IOCTL_MODE_GETENCODER IOWR('d', 0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR IOWR('d', 0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB IOWR('d', 0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB IOWR('d', 0xAF, u32)
#define DRM_IOCTL_MODE_PAGE_FLIP IOWR('d', 0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_DIRTYFB IOWR('d', 0xB1, struct drm_mode_fb_dirty_cmd)
#define DRM_IOCTL_MODE_CREATE_DUMB IOWR('d', 0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB IOWR('d', 0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB IOWR('d', 0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_CURSOR2 IOWR('d', 0xBB, struct drm_mode_cursor2)
#define DRM_IOCTL_MODE_ADDFB2 IOWR('d', 0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE IOWR('d', 0x2e, struct drm_prime_handle)
#define DRM_IOCTL_GEM_CLOSE IOW('d', 0x09, struct drm_gem_close)
#define DRM_FORMAT_XRGB8888 0x34325258u /* 'XR24' */
#define DRM_MODE_FB_MODIFIERS 2
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull

enum { DRM_CAP_DUMB_BUFFER = 0x1, DRM_CAP_CURSOR_WIDTH = 0x8, DRM_CAP_CURSOR_HEIGHT = 0x9, DRM_CAP_ADDFB2_MODIFIERS = 0x10 };
enum { DRM_MODE_TYPE_PREFERRED = 1 << 3 };
enum { DRM_MODE_CONNECTED = 1 };
enum { DRM_MODE_CURSOR_BO = 1, DRM_MODE_CURSOR_MOVE = 2 };
enum { DRM_MODE_PAGE_FLIP_EVENT = 1 };
enum { DRM_EVENT_FLIP_COMPLETE = 2 };
enum { CONN_LVDS = 7, CONN_EDP = 14, CONN_DSI = 16 };

/* ---- Backend state ---------------------------------------------------------------- */

typedef struct {
	u32 handle, fb, pitch;
	u64 size;
	u32 *map;
} DumbBuf;

typedef struct {
	int fd;
	u32 crtc, connector;
	struct drm_mode_modeinfo mode;
	DumbBuf buf[2];
	int front;                /* buffer on screen */
	bool mode_set;
	bool can_flip;            /* false: draw straight into the front buffer */
	Rect prev_damage[16];     /* what changed in the previous frame */
	int nprev;
	bool prev_full;
	DumbBuf cursor;
	u32 cursor_w, cursor_h;
	int gamma_size;
	char driver[32];
	bool showing_client;      /* a program's buffer is on screen (flip_to) */
	bool modifiers;           /* framebuffers can name a tiling layout */
} Drm;

static int drm_ioctl(int fd, unsigned long req, void *arg)
{
	long r;
	do
		r = sys_ioctl(fd, req, arg);
	while (r == -EINTR || r == -EAGAIN);
	return (int)r;
}

static bool dumb_create(Drm *d, DumbBuf *b, u32 w, u32 h, bool add_fb)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
	if (drm_ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0)
		return false;
	b->handle = c.handle;
	b->pitch = c.pitch;
	b->size = c.size;
	if (add_fb) {
		struct drm_mode_fb_cmd f = { .width = w, .height = h, .pitch = c.pitch, .bpp = 32, .depth = 24,
					     .handle = c.handle };
		if (drm_ioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &f) < 0)
			return false;
		b->fb = f.fb_id;
	}
	struct drm_mode_map_dumb m = { .handle = c.handle };
	if (drm_ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0)
		return false;
	void *p = sys_mmap(NULL, (usize)c.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (i64)m.offset);
	if ((long)p < 0 && (long)p > -4096)
		return false;
	b->map = p;
	return true;
}

static void dumb_destroy(Drm *d, DumbBuf *b)
{
	if (b->map)
		sys_munmap(b->map, (usize)b->size);
	if (b->fb)
		drm_ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &b->fb);
	if (b->handle) {
		struct drm_mode_destroy_dumb x = { b->handle };
		drm_ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &x);
	}
	memset(b, 0, sizeof *b);
}

/* Copies a rectangle of the composited frame into a scanout buffer. */
static void copy_rect(DumbBuf *b, const Canvas *frame, Rect r)
{
	int x0 = (int)r.x, y0 = (int)r.y, x1 = (int)(r.x + r.w), y1 = (int)(r.y + r.h);
	x0 = MAX(x0, 0), y0 = MAX(y0, 0);
	x1 = MIN(x1, frame->width), y1 = MIN(y1, frame->height);
	if (x1 <= x0 || y1 <= y0)
		return;
	usize bytes = (usize)(x1 - x0) * 4;
	for (int y = y0; y < y1; y++)
		memcpy((u8 *)b->map + (usize)y * b->pitch + (usize)x0 * 4, canvas_row(frame, y) + x0, bytes);
}

static bool set_crtc(Drm *d, DumbBuf *b)
{
	struct drm_mode_crtc c = {
		.set_connectors_ptr = (u64)(usize)&d->connector,
		.count_connectors = 1,
		.crtc_id = d->crtc,
		.fb_id = b->fb,
		.mode_valid = 1,
		.mode = d->mode,
	};
	return drm_ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &c) >= 0;
}

static void dirty_fb(Drm *d, DumbBuf *b, const Rect *damage, int n)
{
	struct drm_clip_rect clips[16];
	for (int i = 0; i < n && i < 16; i++)
		clips[i] = (struct drm_clip_rect){ (u16)damage[i].x, (u16)damage[i].y, (u16)(damage[i].x + damage[i].w),
						   (u16)(damage[i].y + damage[i].h) };
	struct drm_mode_fb_dirty_cmd cmd = { .fb_id = b->fb, .num_clips = (u32)MIN(n, 16),
					     .clips_ptr = (u64)(usize)clips };
	drm_ioctl(d->fd, DRM_IOCTL_MODE_DIRTYFB, &cmd);
}

static bool drm_present(Output *o, const Canvas *frame, const Rect *damage, int n)
{
	Drm *d = o->impl;
	Rect full = rect(0, 0, (f32)o->width, (f32)o->height);
	if (!d->can_flip) {
		DumbBuf *b = &d->buf[0];
		for (int i = 0; i < n; i++)
			copy_rect(b, frame, damage[i]);
		if (!d->mode_set) {
			d->mode_set = set_crtc(d, b);
			if (!d->mode_set)
				eprint("zdisplay: cannot set mode\n");
		}
		/* Drivers that scan out of a shadow copy (simpledrm, virtual GPUs)
		 * need to be told what changed. */
		dirty_fb(d, b, damage, n);
		return true;
	}
	if (o->flip_pending)
		return false;
	if (d->showing_client) {
		/* back from a program's own buffer: both of ours are out of date */
		d->showing_client = false;
		d->prev_full = true;
	}
	int back = d->front ^ 1;
	DumbBuf *b = &d->buf[back];
	/* The back buffer holds the frame before last; bring it up to date with
	 * what changed in the last frame as well as in this one. */
	if (!d->mode_set || d->prev_full) {
		copy_rect(b, frame, full);
	} else {
		for (int i = 0; i < d->nprev; i++)
			copy_rect(b, frame, d->prev_damage[i]);
		for (int i = 0; i < n; i++)
			copy_rect(b, frame, damage[i]);
	}
	d->nprev = MIN(n, 16);
	memcpy(d->prev_damage, damage, sizeof(Rect) * (usize)d->nprev);
	d->prev_full = n > 16;
	if (!d->mode_set) {
		d->mode_set = set_crtc(d, b);
		if (!d->mode_set) {
			eprint("zdisplay: cannot set mode\n");
			return true;
		}
		d->front = back;
		d->prev_full = true;  /* the other buffer has never been drawn */
		return true;
	}
	struct drm_mode_crtc_page_flip f = { .crtc_id = d->crtc, .fb_id = b->fb, .flags = DRM_MODE_PAGE_FLIP_EVENT };
	if (drm_ioctl(d->fd, DRM_IOCTL_MODE_PAGE_FLIP, &f) < 0) {
		/* No page flipping here: switch to drawing into one buffer. */
		set_crtc(d, b);
		d->buf[0] = *b;
		d->can_flip = false;
		d->front = 0;
		return true;
	}
	d->front = back;
	o->flip_pending = true;
	return true;
}

static void drm_on_fd(Output *o)
{
	Drm *d = o->impl;
	u8 buf[1024];
	long n = sys_read(d->fd, buf, sizeof buf);
	for (long off = 0; off + (long)sizeof(struct drm_event) <= n;) {
		struct drm_event *e = (struct drm_event *)(buf + off);
		if (e->length == 0)
			break;
		if (e->type == DRM_EVENT_FLIP_COMPLETE) {
			o->flip_pending = false;
			wl_flip_complete();
			request_frame();
		}
		off += e->length;
	}
}

static bool drm_cursor_set(Output *o, const Canvas *img, int hot_x, int hot_y)
{
	Drm *d = o->impl;
	if (!img || (u32)img->width > d->cursor_w || (u32)img->height > d->cursor_h)
		return false;
	if (!d->cursor.map && !dumb_create(d, &d->cursor, d->cursor_w, d->cursor_h, false))
		return false;
	memset(d->cursor.map, 0, (usize)d->cursor.size);
	for (int y = 0; y < img->height; y++)
		memcpy((u8 *)d->cursor.map + (usize)y * d->cursor.pitch, canvas_row(img, y), (usize)img->width * 4);
	struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = d->crtc, .width = d->cursor_w,
				      .height = d->cursor_h, .handle = d->cursor.handle, .hot_x = hot_x, .hot_y = hot_y };
	/* Remember the hotspot so moves can place the image's top-left corner. */
	o->cursor_hot_x = hot_x;
	o->cursor_hot_y = hot_y;
	return drm_ioctl(d->fd, DRM_IOCTL_MODE_CURSOR2, &c) >= 0;
}

static void drm_cursor_move(Output *o, int x, int y)
{
	Drm *d = o->impl;
	struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_MOVE, .crtc_id = d->crtc,
				      .x = x - o->cursor_hot_x, .y = y - o->cursor_hot_y };
	drm_ioctl(d->fd, DRM_IOCTL_MODE_CURSOR2, &c);
}

static void drm_set_gamma(Output *o, f32 r, f32 g, f32 b)
{
	Drm *d = o->impl;
	int n = d->gamma_size;
	if (n <= 1)
		return;
	u16 *lut = mem_alloc(sizeof(u16) * (usize)n * 3);
	for (int i = 0; i < n; i++) {
		f32 v = (f32)i / (f32)(n - 1) * 65535;
		lut[i] = (u16)(v * r);
		lut[n + i] = (u16)(v * g);
		lut[2 * n + i] = (u16)(v * b);
	}
	struct drm_mode_crtc_lut l = { .crtc_id = d->crtc, .gamma_size = (u32)n, .red = (u64)(usize)lut,
				       .green = (u64)(usize)(lut + n), .blue = (u64)(usize)(lut + 2 * n) };
	drm_ioctl(d->fd, DRM_IOCTL_MODE_SETGAMMA, &l);
	mem_free(lut);
}

/* ---- Device discovery ------------------------------------------------------------ */

/* ---- Programs' GPU buffers on screen ------------------------------------------------- */

/* Makes a framebuffer from a program's buffer (one plane, 32-bit pixels).
 * The framebuffer keeps the memory alive; 0 if the GPU can't show it. */
static u32 drm_fb_import(Output *o, int dmabuf, u32 width, u32 height, u32 offset, u32 stride, u64 modifier)
{
	Drm *d = o->impl;
	struct drm_prime_handle p = { .fd = dmabuf };
	int r = drm_ioctl(d->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &p);
	if (r < 0) {
		eprint("zdisplay: the GPU can't take a program's buffer: %s\n", err_str(r));
		return 0;
	}
	struct drm_mode_fb_cmd2 f = { .width = width, .height = height, .pixel_format = DRM_FORMAT_XRGB8888 };
	f.handles[0] = p.handle, f.pitches[0] = stride, f.offsets[0] = offset;
	/* linear is what a framebuffer without a modifier is; other layouts need
	 * a driver that understands modifiers */
	if (modifier != DRM_FORMAT_MOD_INVALID && modifier != 0) {
		if (!d->modifiers)
			return 0;
		f.flags = DRM_MODE_FB_MODIFIERS;
		f.modifier[0] = modifier;
	}
	r = drm_ioctl(d->fd, DRM_IOCTL_MODE_ADDFB2, &f);
	struct drm_gem_close c = { .handle = p.handle };
	drm_ioctl(d->fd, DRM_IOCTL_GEM_CLOSE, &c);
	if (r < 0)
		eprint("zdisplay: the display can't show a program's buffer: %s\n", err_str(r));
	return r < 0 ? 0 : f.fb_id;
}

static void drm_fb_release(Output *o, u32 fb)
{
	Drm *d = o->impl;
	drm_ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &fb);
}

static bool drm_flip_to(Output *o, u32 fb)
{
	Drm *d = o->impl;
	if (!d->can_flip || !d->mode_set || o->flip_pending)
		return false;
	struct drm_mode_crtc_page_flip f = { .crtc_id = d->crtc, .fb_id = fb, .flags = DRM_MODE_PAGE_FLIP_EVENT };
	int r = drm_ioctl(d->fd, DRM_IOCTL_MODE_PAGE_FLIP, &f);
	if (r < 0) {
		eprint("zdisplay: flipping to a program's buffer failed: %s\n", err_str(r));
		return false;
	}
	d->showing_client = true;
	o->flip_pending = true;
	return true;
}

static void driver_name(int fd, char *out, usize cap)
{
	char name[32] = { 0 };
	struct drm_version v = { .name_len = sizeof name - 1, .name = (u64)(usize)name };
	out[0] = 0;
	if (drm_ioctl(fd, DRM_IOCTL_VERSION, &v) >= 0)
		str_copy(out, name, cap);
}

/* Picks a connected connector (the built-in panel first), its best mode and a CRTC. */
static bool pick_pipe(Drm *d, f32 *dpi)
{
	struct drm_mode_card_res res = { 0 };
	if (drm_ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || res.count_connectors == 0)
		return false;
	u32 *conns = mem_zalloc(sizeof(u32) * res.count_connectors);
	u32 *crtcs = mem_zalloc(sizeof(u32) * (res.count_crtcs + 1));
	u32 *encs = mem_zalloc(sizeof(u32) * (res.count_encoders + 1));
	res.connector_id_ptr = (u64)(usize)conns;
	res.crtc_id_ptr = (u64)(usize)crtcs;
	res.encoder_id_ptr = (u64)(usize)encs;
	res.count_fbs = 0;
	bool ok = false;
	if (drm_ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		goto out;
	int best_score = -1;
	for (u32 i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector c = { .connector_id = conns[i] };
		if (drm_ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || c.connection != DRM_MODE_CONNECTED ||
		    c.count_modes == 0)
			continue;
		struct drm_mode_modeinfo *modes = mem_zalloc(sizeof *modes * c.count_modes);
		u32 *cencs = mem_zalloc(sizeof(u32) * (c.count_encoders + 1));
		struct drm_mode_get_connector q = { .connector_id = conns[i], .count_modes = c.count_modes,
						    .modes_ptr = (u64)(usize)modes, .count_encoders = c.count_encoders,
						    .encoders_ptr = (u64)(usize)cencs };
		if (drm_ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &q) < 0) {
			mem_free(modes);
			mem_free(cencs);
			continue;
		}
		bool internal = q.connector_type == CONN_EDP || q.connector_type == CONN_LVDS || q.connector_type == CONN_DSI;
		int score = internal ? 2 : 1;
		if (score > best_score) {
			/* Preferred mode, otherwise the largest. */
			int m = 0;
			for (u32 k = 0; k < q.count_modes; k++) {
				if (modes[k].type & DRM_MODE_TYPE_PREFERRED) {
					m = (int)k;
					break;
				}
				if ((u32)modes[k].hdisplay * modes[k].vdisplay > (u32)modes[m].hdisplay * modes[m].vdisplay)
					m = (int)k;
			}
			/* A CRTC: the encoder's current one, else any it can use. */
			u32 crtc = 0;
			struct drm_mode_get_encoder e = { .encoder_id = q.encoder_id };
			if (q.encoder_id && drm_ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &e) >= 0 && e.crtc_id)
				crtc = e.crtc_id;
			for (u32 k = 0; !crtc && k < q.count_encoders; k++) {
				struct drm_mode_get_encoder e2 = { .encoder_id = cencs[k] };
				if (drm_ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &e2) < 0)
					continue;
				for (u32 j = 0; j < res.count_crtcs; j++)
					if (e2.possible_crtcs & (1u << j)) {
						crtc = crtcs[j];
						break;
					}
			}
			if (crtc) {
				best_score = score;
				d->connector = conns[i];
				d->crtc = crtc;
				d->mode = modes[m];
				*dpi = q.mm_width > 0 ? modes[m].hdisplay / (q.mm_width / 25.4f) : 96;
				ok = true;
			}
		}
		mem_free(modes);
		mem_free(cencs);
	}
out:
	mem_free(conns);
	mem_free(crtcs);
	mem_free(encs);
	return ok;
}

static void drm_pause(Output *o, bool paused)
{
	Drm *d = o->impl;
	if (paused) {
		drm_ioctl(d->fd, DRM_IOCTL_DROP_MASTER, NULL);
		return;
	}
	drm_ioctl(d->fd, DRM_IOCTL_SET_MASTER, NULL);
	/* whatever the other compositor showed is gone: set our mode and redraw everything */
	d->mode_set = false;
	d->prev_full = true;
	d->showing_client = false;
	o->flip_pending = false;
}

static Output *try_card(const char *path, bool allow_simple)
{
	int fd = (int)sys_open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK, 0);
	if (fd < 0)
		return NULL;
	Drm *d = NEW(Drm);
	d->fd = fd;
	driver_name(fd, d->driver, sizeof d->driver);
	struct drm_get_cap cap = { .capability = DRM_CAP_DUMB_BUFFER };
	f32 dpi = 96;
	if ((!allow_simple && str_eq(d->driver, "simpledrm")) || drm_ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0 ||
	    !cap.value || !pick_pipe(d, &dpi))
		goto fail;
	drm_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL);
	u32 w = d->mode.hdisplay, h = d->mode.vdisplay;
	if (!dumb_create(d, &d->buf[0], w, h, true))
		goto fail;
	d->can_flip = dumb_create(d, &d->buf[1], w, h, true);
	d->front = 1;
	struct drm_mode_crtc cur = { .crtc_id = d->crtc };
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &cur) >= 0)
		d->gamma_size = (int)cur.gamma_size;
	cap = (struct drm_get_cap){ .capability = DRM_CAP_CURSOR_WIDTH };
	d->cursor_w = drm_ioctl(fd, DRM_IOCTL_GET_CAP, &cap) >= 0 && cap.value ? (u32)cap.value : 64;
	cap = (struct drm_get_cap){ .capability = DRM_CAP_CURSOR_HEIGHT };
	d->cursor_h = drm_ioctl(fd, DRM_IOCTL_GET_CAP, &cap) >= 0 && cap.value ? (u32)cap.value : 64;

	Output *o = NEW(Output);
	o->name = str_dup(d->driver[0] ? d->driver : "drm");
	o->width = (int)w;
	o->height = (int)h;
	u32 total = (u32)d->mode.htotal * d->mode.vtotal;
	o->refresh_mhz = total ? (int)((u64)d->mode.clock * 1000000 / total) : 60000;
	o->dpi = dpi;
	o->impl = d;
	o->present = drm_present;
	o->cursor_set = drm_cursor_set;
	o->cursor_move = drm_cursor_move;
	o->set_gamma = drm_set_gamma;
	o->fd = fd;
	o->on_fd = drm_on_fd;
	o->pause = drm_pause;
	/* a real GPU driver can take programs' buffers (the firmware framebuffer can't) */
	cap = (struct drm_get_cap){ .capability = DRM_CAP_ADDFB2_MODIFIERS };
	d->modifiers = drm_ioctl(fd, DRM_IOCTL_GET_CAP, &cap) >= 0 && cap.value;
	struct stat st;
	if (!str_eq(d->driver, "simpledrm") && sys_fstat(fd, &st) == 0) {
		o->device = st.st_rdev;
		o->fb_import = drm_fb_import;
		o->fb_release = drm_fb_release;
		o->flip_to = drm_flip_to;
	}
	return o;
fail:
	dumb_destroy(d, &d->buf[0]);
	dumb_destroy(d, &d->buf[1]);
	sys_close(fd);
	mem_free(d);
	return NULL;
}

Output *output_drm_open(void)
{
	/* Real GPU drivers replace the firmware framebuffer driver as soon as
	 * they load; prefer them, and use simpledrm only when nothing else drives
	 * the screen (unsupported GPU, or still probing after a short wait). */
	for (int attempt = 0; attempt < 2; attempt++) {
		for (int i = 0; i < 8; i++) {
			char path[32];
			fmt(path, sizeof path, "/dev/dri/card%d", i);
			Output *o = try_card(path, attempt == 1);
			if (o)
				return o;
		}
		if (attempt == 0)
			sleep_ms(300);
	}
	return NULL;
}
