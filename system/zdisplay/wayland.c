/*
 * Wayland: the window protocol standard Linux programs speak, served by
 * zdisplay next to its own. Xwayland (and through it Steam and X11 games)
 * and native Wayland programs connect to $XDG_RUNTIME_DIR/wayland-0.
 *
 * The wire format: each message is the object id, then the size and opcode
 * in one word, then the arguments, every one padded to 4 bytes; file
 * descriptors travel beside the bytes. The opcodes and argument signatures
 * come from the protocol XML (wl_protocol.h, made by tools/wlgen.py).
 *
 * Surfaces with the xdg_toplevel role become ordinary zdisplay windows, so
 * they get the same focus, stacking, animations, dock and full screen as
 * ZOS apps. Pixels come from shared memory (wl_shm). The keyboard layout is
 * described to clients as an XKB keymap made from zdisplay's own layouts.
 */
#include "zdisplay.h"

typedef struct {
	const char *name, *signature, *creates;
	int since;
} WlMessage;

typedef struct {
	const char *name;
	int version;
	const WlMessage *requests;
	int nrequests, nevents;
} WlInterface;

#include "wl_protocol.h"

#define WL_BUF 65536
#define WL_MAX_FDS 28
#define WL_MAX_ARGS 20
#define WL_MAX_CLIENTS 32
#define WL_MAX_IDS (1u << 20)
#define WL_SERVER_ID 0xff000000u
#define FIX(v) ((u32)(i32)roundf((v) * 256.0f))
#define UNFIX(v) ((f32)(i32)(v) / 256.0f)

typedef struct WlClient WlClient;
typedef struct WlResource WlResource;
typedef struct WlXdgSurface WlXdgSurface;

typedef struct {
	u32 u;              /* int, uint, fixed, object and new ids */
	const char *s;
	const void *data;   /* arrays */
	u32 len;
	int fd;
	WlResource *obj;
} WlArg;

typedef void (*WlDispatch)(WlResource *r, u32 op, WlArg *a);

struct WlResource {
	u32 id;
	const WlInterface *iface;
	u32 version;
	WlClient *client;
	void *data;
	WlDispatch dispatch;
	void (*destroy)(WlResource *r);
};

struct WlClient {
	int fd;
	bool dead;
	int pid;
	char app_id[48];     /* from ZOS_APP_ID in its environment, when ZOS started it */
	u8 in[WL_BUF];
	usize in_len;
	int in_fds[WL_MAX_FDS];
	int nin_fds;
	u8 out[WL_BUF];
	usize out_len;
	int out_fds[WL_MAX_FDS];
	int nout_fds;
	WlResource **objs;
	u32 nobjs;
	/* input objects this client asked for */
	WlResource *pointers[8], *keyboards[8], *relative[8];
	int npointers, nkeyboards, nrelative;
	WlResource *outputs[8];
	int noutputs;
};

/* ---- Shared memory and surfaces ------------------------------------------------------- */

typedef struct {
	int fd;
	u8 *map;
	usize size;
	int refs;            /* the pool object plus its buffers */
	WlClient *client;
	bool broken;         /* the client shrank the memory under us (see on_sigbus) */
} WlPool;

typedef struct {
	WlResource *res;     /* NULL once the client destroyed it */
	WlPool *pool;        /* shared memory, or NULL for a GPU buffer */
	i32 offset, width, height, stride;
	u32 format;          /* as wl_shm numbers them: 0 ARGB, 1 XRGB */
	int users;           /* surfaces showing it, and the screen while it's scanned out */
	bool release_after_frame;
	/* GPU buffers (linux-dmabuf) */
	int dmabuf;          /* -1 for shared memory */
	u64 modifier;
	u8 *map;             /* mapped when zdisplay has to read it */
	usize map_size;
	bool map_failed;
	u32 fb;              /* framebuffer for direct scanout, 0 none yet */
	bool no_scanout;     /* the GPU said no */
} WlBuffer;

enum { ROLE_NONE, ROLE_TOPLEVEL, ROLE_POPUP, ROLE_CURSOR, ROLE_SUBSURFACE };

/* A region (input areas): rectangles added and taken away, in order. */
typedef struct {
	i32 x, y, w, h;
	bool subtract;
} RegionOp;

typedef struct {
	RegionOp *ops;
	int n;
} WlRegion;

typedef struct WlSurface WlSurface;

/* A subsurface: a surface drawn as part of another (its parent), at a
 * position in the parent's coordinates. Toolkits put video, web content and
 * GPU-drawn parts of a window in them. */
typedef struct WlSub {
	WlResource *res;
	WlSurface *surface, *parent;
	i32 x, y, px, py;    /* the position, and the one the parent's next commit applies */
	bool sync;
} WlSub;

struct WlSurface {
	WlResource *res;
	WlClient *client;
	/* double-buffered state */
	WlBuffer *pending_buffer;
	bool pending_attach;
	i32 pending_scale;
	WlResource *pending_frames[16];
	int npending_frames;
	WlBuffer *buffer;
	i32 scale;
	WlResource *frames[16];
	int nframes;
	i32 vp_w, vp_h;      /* viewport destination, 0: none */
	int role;
	WlXdgSurface *xdg;   /* its xdg_surface, for toplevels and popups */
	Window *win;
	u32 commits;
	i32 cursor_hx, cursor_hy;
	Canvas *opaque;      /* XRGB pixels with the alpha filled in, when blending needs it */
	u32 opaque_commit;   /* the commit it was made from */
	struct WlConstraint *constraint;
	WlSub *sub;          /* when it's a subsurface: its parent and position */
	WlSub **kids;        /* subsurfaces on it, bottom first */
	int nkids, below;    /* the first `below` of them are under the surface itself */
	WlRegion *input, *pending_input; /* where it takes pointer input; NULL: everywhere */
	bool pending_input_set;
};

typedef struct WlToplevel WlToplevel;
struct WlToplevel {
	WlResource *res;
	WlXdgSurface *xs;
	WlToplevel *parent;
	char title[128], app_id[48];
	i32 min_w, min_h, max_w, max_h;
	bool want_fullscreen, want_maximized;
	/* the last configure sent, to skip repeats */
	i32 sent_w, sent_h;
	u32 sent_states;
	bool sent_any;
};

typedef struct {
	i32 w, h, ax, ay, aw, ah, dx, dy;
	u32 anchor, gravity, adjust;
} WlPositioner;

typedef struct {
	WlResource *res;
	WlXdgSurface *xs;
	WlXdgSurface *parent;
	WlPositioner pos;
	i32 x, y;            /* relative to the parent's window geometry */
	bool grab;           /* a menu: dismissed by clicks elsewhere, takes the keyboard */
	bool dismissed;      /* popup_done sent: it stays hidden until destroyed */
} WlPopup;

struct WlXdgSurface {
	WlResource *res;
	WlSurface *surface;
	WlToplevel *toplevel;
	WlPopup *popup;
	u32 serial;          /* the last configure sent */
	u32 acked;           /* the last one the client acked */
	bool configured;     /* the client acked one */
	bool initial_sent;
	/* window geometry: the part of the surface that is the window, without client-drawn shadows */
	i32 gx, gy, gw, gh, pgx, pgy, pgw, pgh;
	bool pending_geometry;
};

/* A pointer lock or confinement (zwp_pointer_constraints_v1). */
typedef struct WlConstraint {
	WlResource *res;
	WlSurface *surface;
	bool lock;           /* locked: the cursor stays put; confined: it stays inside */
	bool persistent;     /* comes back each time the surface gets the pointer again */
	bool active, defunct;
	f32 hint_x, hint_y;
	bool has_hint;
} WlConstraint;

static struct {
	int listen_fd;
	WlClient *clients[WL_MAX_CLIENTS];
	int nclients;
	u32 serial;
	/* surfaces waiting for the next frame */
	WlSurface *frame_waiters[256];
	int nframe_waiters;
	WlBuffer *to_release[256];
	int nto_release;
	/* the keymap, shared with every keyboard */
	int keymap_fd;
	u32 keymap_size;
	/* keys the focused client saw go down */
	u32 keys_down[32];
	int nkeys_down;
	WlSurface *kb_focus, *ptr_focus;
	int buttons_down;    /* pointer buttons held: the surface they went down on keeps the pointer */
	/* open menus (grabbing popups), innermost last */
	WlPopup *grabs[8];
	int ngrabs;
	/* every shared-memory pool, for the SIGBUS handler */
	WlPool *pools[512];
	int npools;
	/* every xdg_surface and pointer constraint, to fix up references when one goes */
	WlXdgSurface *xdgs[512];
	int nxdgs;
	WlConstraint *constraints[64];
	int nconstraints;
	u32 idle_timer;      /* fires frame callbacks when nothing on screen changes */
	/* GPU buffers on screen: the one flipped to, and the one showing */
	WlBuffer *flip_buffer, *shown;
	int format_table;    /* linux-dmabuf feedback: the formats and modifiers, in a sealed memfd */
	Canvas *cursor;      /* the image of the cursor surface in use */
	WlSurface *cursor_surface;
} W = { .listen_fd = -1, .keymap_fd = -1, .format_table = -1 };

static u32 next_serial(void)
{
	return ++W.serial;
}

static u32 now_ms(void)
{
	return (u32)time_mono_ms();
}

/* ---- Wire --------------------------------------------------------------------------- */

static void put32(u8 *p, u32 v)
{
	memcpy(p, &v, 4);
}

static u32 get32(const u8 *p)
{
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

static u32 wire_pad(u32 n)
{
	return (n + 3) & ~3u;
}

static void client_kill(WlClient *c);

static void flush_client(WlClient *c)
{
	while (!c->dead && (c->out_len || c->nout_fds)) {
		long n = unix_send(c->fd, c->out, c->out_len, c->out_fds, c->nout_fds);
		if (n == -EAGAIN)
			return;
		if (n < 0) {
			client_kill(c);
			return;
		}
		for (int i = 0; i < c->nout_fds; i++)
			sys_close(c->out_fds[i]);
		c->nout_fds = 0;
		memmove(c->out, c->out + n, c->out_len - (usize)n);
		c->out_len -= (usize)n;
	}
}

static void queue(WlClient *c, const u8 *msg, usize n, const int *fds, int nfds)
{
	if (c->dead)
		return;
	if (c->out_len + n > sizeof c->out || c->nout_fds + nfds > WL_MAX_FDS)
		flush_client(c);
	if (c->out_len + n > sizeof c->out || c->nout_fds + nfds > WL_MAX_FDS) {
		/* a client that stopped reading */
		client_kill(c);
		return;
	}
	memcpy(c->out + c->out_len, msg, n);
	c->out_len += n;
	for (int i = 0; i < nfds; i++)
		c->out_fds[c->nout_fds++] = fds[i];
}

/* Sends an event. Arguments by signature: i u f o n as u32 (objects by id),
 * s a string (or NULL), a a pointer and a length, h a file descriptor (sent
 * as a duplicate, so the caller keeps its own). */
static void event(WlResource *r, u32 op, const char *sig, ...)
{
	if (!r || r->client->dead)
		return;
	u8 msg[4096];
	usize n = 8;
	int fds[4], nfds = 0;
	va_list ap;
	va_start(ap, sig);
	for (const char *p = sig; *p; p++) {
		switch (*p) {
		case '?':
			break;
		case 'i': case 'u': case 'f': case 'o': case 'n':
			put32(msg + n, va_arg(ap, u32));
			n += 4;
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			u32 len = s ? (u32)strlen(s) + 1 : 0;
			put32(msg + n, len);
			memset(msg + n + 4, 0, wire_pad(len));
			if (len)
				memcpy(msg + n + 4, s, len);
			n += 4 + wire_pad(len);
			break;
		}
		case 'a': {
			const void *d = va_arg(ap, const void *);
			u32 len = va_arg(ap, u32);
			put32(msg + n, len);
			memset(msg + n + 4, 0, wire_pad(len));
			if (len)
				memcpy(msg + n + 4, d, len);
			n += 4 + wire_pad(len);
			break;
		}
		case 'h': {
			int fd = va_arg(ap, int);
			int d = (int)sys_fcntl(fd, F_DUPFD_CLOEXEC, 0);
			if (d >= 0)
				fds[nfds++] = d;
			break;
		}
		}
	}
	va_end(ap);
	put32(msg, r->id);
	put32(msg + 4, (u32)n << 16 | op);
	queue(r->client, msg, n, fds, nfds);
}

/* ---- Objects -------------------------------------------------------------------------- */

static WlResource *obj(WlClient *c, u32 id)
{
	return id < c->nobjs ? c->objs[id] : NULL;
}

static void protocol_error(WlClient *c, u32 id, u32 code, const char *msg)
{
	WlResource *display = obj(c, 1);
	if (display)
		event(display, WL_DISPLAY_EV_ERROR, "ous", id, code, msg);
	eprint("zdisplay: wayland client error on object %u: %s\n", id, msg);
	flush_client(c);
	client_kill(c);
}

static WlResource *res_new(WlClient *c, u32 id, const WlInterface *iface, u32 version, WlDispatch dispatch, void *data)
{
	if (id == 0 || id >= WL_MAX_IDS || obj(c, id)) {
		protocol_error(c, 1, 0, "invalid object id");
		return NULL;
	}
	if (id >= c->nobjs) {
		u32 n = MAX(64u, c->nobjs);
		while (n <= id)
			n *= 2;
		c->objs = mem_realloc(c->objs, n * sizeof *c->objs);
		memset(c->objs + c->nobjs, 0, (n - c->nobjs) * sizeof *c->objs);
		c->nobjs = n;
	}
	WlResource *r = NEW(WlResource);
	r->id = id, r->iface = iface, r->version = version, r->client = c, r->dispatch = dispatch, r->data = data;
	c->objs[id] = r;
	return r;
}

/* Drops an object: frees its state and tells the client the id is free again. */
static void res_destroy(WlResource *r)
{
	if (!r)
		return;
	WlClient *c = r->client;
	if (r->destroy)
		r->destroy(r);
	if (r->id < c->nobjs && c->objs[r->id] == r)
		c->objs[r->id] = NULL;
	if (r->id < WL_SERVER_ID && !c->dead) {
		WlResource *display = obj(c, 1);
		if (display && display != r)
			event(display, WL_DISPLAY_EV_DELETE_ID, "u", r->id);
	}
	mem_free(r);
}

static void remove_ptr(WlResource **list, int *n, WlResource *r)
{
	for (int i = 0; i < *n; i++)
		if (list[i] == r) {
			list[i] = list[--*n];
			return;
		}
}

/* Destructor requests share opcode 0 on most interfaces; the dispatchers
 * call this for them. */
static void plain_destroy(WlResource *r, u32 op, WlArg *a)
{
	UNUSED(op), UNUSED(a);
	res_destroy(r);
}

/* ---- Buffers and pools ---------------------------------------------------------------- */

static void pool_unref(WlPool *p)
{
	if (--p->refs > 0)
		return;
	for (int i = 0; i < W.npools; i++)
		if (W.pools[i] == p) {
			W.pools[i] = W.pools[--W.npools];
			break;
		}
	if (p->map)
		sys_munmap(p->map, p->size);
	sys_close(p->fd);
	mem_free(p);
}

/* A client can shrink the memory behind a pool while zdisplay reads it, which
 * raises SIGBUS. The handler puts zero-filled pages where the pool was, so the
 * read finishes with blank pixels, and the client is dropped afterwards. */
static void on_sigbus(int sig, void *info, void *context)
{
	UNUSED(context);
	u8 *addr;
	memcpy(&addr, (u8 *)info + 16, sizeof addr);
	for (int i = 0; i < W.npools; i++) {
		WlPool *p = W.pools[i];
		if (p->map && addr >= p->map && addr < p->map + p->size) {
			sys_mmap(p->map, p->size, PROT_READ, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
			p->broken = true;
			return;
		}
	}
	/* not ours: crash as usual */
	sig_default(sig);
}

static void buffer_maybe_free(WlBuffer *b)
{
	if (b->res || b->users > 0 || b->release_after_frame)
		return;
	if (b->pool) {
		pool_unref(b->pool);
	} else {
		if (b->map)
			sys_munmap(b->map, b->map_size);
		if (b->fb)
			S.out->fb_release(S.out, b->fb);
		sys_close(b->dmabuf);
	}
	mem_free(b);
}

static void buffer_destroyed(WlResource *r)
{
	WlBuffer *b = r->data;
	b->res = NULL;
	buffer_maybe_free(b);
}

static void buffer_dispatch(WlResource *r, u32 op, WlArg *a)
{
	plain_destroy(r, op, a); /* destroy is its only request */
}

/* A surface stops showing a buffer: it can go back to the client after the next frame. */
static void buffer_unuse(WlBuffer *b)
{
	if (!b)
		return;
	b->users--;
	if (b->users == 0 && !b->release_after_frame && W.nto_release < (int)ARRAY_LEN(W.to_release)) {
		b->release_after_frame = true;
		W.to_release[W.nto_release++] = b;
	}
}

static void pool_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlPool *p = r->data;
	if (op == WL_SHM_POOL_CREATE_BUFFER) {
		i32 offset = (i32)a[1].u, w = (i32)a[2].u, h = (i32)a[3].u, stride = (i32)a[4].u;
		u32 format = a[5].u;
		if (format > 1) {
			protocol_error(r->client, r->id, 0, "unsupported buffer format");
			return;
		}
		if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || stride < w * 4 || offset < 0 ||
		    (u64)offset + (u64)stride * (u64)h > p->size) {
			protocol_error(r->client, r->id, 2, "buffer outside its pool");
			return;
		}
		WlBuffer *b = NEW(WlBuffer);
		b->pool = p, b->offset = offset, b->width = w, b->height = h, b->stride = stride, b->format = format;
		b->dmabuf = -1;
		p->refs++;
		b->res = res_new(r->client, a[0].u, &wl_buffer_interface, 1, buffer_dispatch, b);
		if (b->res)
			b->res->destroy = buffer_destroyed;
	} else if (op == WL_SHM_POOL_DESTROY) {
		res_destroy(r);
	} else if (op == WL_SHM_POOL_RESIZE) {
		i32 size = (i32)a[0].u;
		if (size < (i32)p->size) {
			protocol_error(r->client, r->id, 2, "pools only grow");
			return;
		}
		void *m = sys_mmap(NULL, (usize)size, PROT_READ, MAP_SHARED, p->fd, 0);
		if ((unsigned long)m > -4096ul) {
			protocol_error(r->client, r->id, 2, "cannot map the pool");
			return;
		}
		/* buffers keep offsets into the pool, so they follow the new mapping */
		sys_munmap(p->map, p->size);
		p->map = m;
		p->size = (usize)size;
	}
}

static void pool_destroyed(WlResource *r)
{
	pool_unref(r->data);
}

static void shm_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op != WL_SHM_CREATE_POOL)
		return;
	int fd = a[1].fd;
	i32 size = (i32)a[2].u;
	if (size <= 0) {
		sys_close(fd);
		protocol_error(r->client, r->id, 1, "empty pool");
		return;
	}
	void *m = sys_mmap(NULL, (usize)size, PROT_READ, MAP_SHARED, fd, 0);
	if ((unsigned long)m > -4096ul) {
		sys_close(fd);
		protocol_error(r->client, r->id, 2, "cannot map the pool");
		return;
	}
	if (W.npools >= (int)ARRAY_LEN(W.pools)) {
		sys_munmap(m, (usize)size);
		sys_close(fd);
		protocol_error(r->client, r->id, 2, "too many pools");
		return;
	}
	WlPool *p = NEW(WlPool);
	p->fd = fd, p->map = m, p->size = (usize)size, p->refs = 1, p->client = r->client;
	W.pools[W.npools++] = p;
	WlResource *pr = res_new(r->client, a[0].u, &wl_shm_pool_interface, 1, pool_dispatch, p);
	if (pr)
		pr->destroy = pool_destroyed;
	else
		pool_unref(p);
}

/* ---- Callbacks, regions ---------------------------------------------------------------- */

static void callback_dispatch(WlResource *r, u32 op, WlArg *a)
{
	UNUSED(r), UNUSED(op), UNUSED(a); /* no requests */
}

static void region_free(WlRegion *g)
{
	if (g) {
		mem_free(g->ops);
		mem_free(g);
	}
}

static WlRegion *region_copy(const WlRegion *g)
{
	WlRegion *c = NEW(WlRegion);
	if (g && g->n) {
		c->ops = mem_dup(g->ops, sizeof(RegionOp) * (usize)g->n);
		c->n = g->n;
	}
	return c;
}

static bool region_has(const WlRegion *g, i32 x, i32 y)
{
	if (!g)
		return true;
	bool in = false;
	for (int i = 0; i < g->n; i++) {
		const RegionOp *o = &g->ops[i];
		if (x >= o->x && y >= o->y && x < o->x + o->w && y < o->y + o->h)
			in = !o->subtract;
	}
	return in;
}

static void region_destroyed(WlResource *r)
{
	region_free(r->data);
}

static void region_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlRegion *g = r->data;
	if (op == WL_REGION_DESTROY) {
		res_destroy(r);
	} else if ((op == WL_REGION_ADD || op == WL_REGION_SUBTRACT) && g->n < 4096) {
		g->ops = mem_realloc(g->ops, sizeof(RegionOp) * (usize)(g->n + 1));
		g->ops[g->n++] = (RegionOp){ (i32)a[0].u, (i32)a[1].u, (i32)a[2].u, (i32)a[3].u, op == WL_REGION_SUBTRACT };
	}
}

/* ---- Surfaces ----------------------------------------------------------------------------- */

static void toplevel_commit(WlSurface *s);
static void popup_commit(WlSurface *s);
static void cursor_commit(WlSurface *s);
static void surface_unmap(WlSurface *s);
static void idle_frames(void);
static void drop_broken_clients(void);

i32 wl_surface_width(const WlSurface *s);
i32 wl_surface_height(const WlSurface *s);

i32 wl_surface_width(const WlSurface *s)
{
	if (s->vp_w > 0)
		return s->vp_w;
	return s->buffer ? s->buffer->width / MAX(1, s->scale) : 0;
}

i32 wl_surface_height(const WlSurface *s)
{
	if (s->vp_h > 0)
		return s->vp_h;
	return s->buffer ? s->buffer->height / MAX(1, s->scale) : 0;
}

/* The surface at the bottom of a tree of subsurfaces (the window's own). */
static WlSurface *surface_root(WlSurface *s)
{
	for (int depth = 0; s && s->sub && s->sub->parent && depth < 16; depth++)
		s = s->sub->parent;
	return s;
}

static Window *surface_window(WlSurface *s)
{
	s = surface_root(s);
	return s ? s->win : NULL;
}

/* Where a subsurface is in its root surface's coordinates. */
static void surface_offset(WlSurface *s, i32 *x, i32 *y)
{
	*x = *y = 0;
	for (int depth = 0; s && s->sub && s->sub->parent && depth < 16; depth++) {
		*x += s->sub->x, *y += s->sub->y;
		s = s->sub->parent;
	}
}

static void sub_unlink(WlSub *k)
{
	WlSurface *p = k->parent;
	if (!p)
		return;
	for (int i = 0; i < p->nkids; i++)
		if (p->kids[i] == k) {
			memmove(&p->kids[i], &p->kids[i + 1], sizeof(WlSub *) * (usize)(p->nkids - i - 1));
			p->nkids--;
			if (i < p->below)
				p->below--;
			break;
		}
	k->parent = NULL;
	Window *w = surface_window(p);
	if (w)
		window_damage(w);
}

static void drop_frame_waiter(WlSurface *s)
{
	for (int i = 0; i < W.nframe_waiters; i++)
		if (W.frame_waiters[i] == s)
			W.frame_waiters[i--] = W.frame_waiters[--W.nframe_waiters];
}

static void frame_callback_destroyed(WlResource *r)
{
	/* the surface may still list it */
	WlSurface *s = r->data;
	if (!s)
		return;
	remove_ptr(s->frames, &s->nframes, r);
	remove_ptr(s->pending_frames, &s->npending_frames, r);
}

static void surface_commit(WlSurface *s)
{
	s->commits++;
	if (s->pending_attach) {
		WlBuffer *b = s->pending_buffer;
		if (b != s->buffer) {
			if (b)
				b->users++;
			buffer_unuse(s->buffer);
			s->buffer = b;
		}
		s->pending_attach = false;
		s->pending_buffer = NULL;
	}
	if (s->pending_scale > 0)
		s->scale = s->pending_scale;
	if (s->pending_input_set) {
		region_free(s->input);
		s->input = s->pending_input;
		s->pending_input = NULL;
		s->pending_input_set = false;
	}
	/* subsurfaces move with their parent's commit */
	for (int i = 0; i < s->nkids; i++)
		s->kids[i]->x = s->kids[i]->px, s->kids[i]->y = s->kids[i]->py;
	for (int i = 0; i < s->npending_frames && s->nframes < (int)ARRAY_LEN(s->frames); i++)
		s->frames[s->nframes++] = s->pending_frames[i];
	s->npending_frames = 0;
	if (s->nframes) {
		bool listed = false;
		for (int i = 0; i < W.nframe_waiters; i++)
			listed |= W.frame_waiters[i] == s;
		if (!listed && W.nframe_waiters < (int)ARRAY_LEN(W.frame_waiters))
			W.frame_waiters[W.nframe_waiters++] = s;
		request_frame();
		idle_frames();
	}
	switch (s->role) {
	case ROLE_TOPLEVEL: toplevel_commit(s); break;
	case ROLE_POPUP: popup_commit(s); break;
	case ROLE_CURSOR: cursor_commit(s); break;
	case ROLE_SUBSURFACE: {
		Window *w = surface_window(s);
		if (w)
			window_damage(w);
		break;
	}
	}
}

static void surface_destroyed(WlResource *r)
{
	WlSurface *s = r->data;
	/* the client already forgot the surface: no leave events for it */
	if (W.ptr_focus == s)
		W.ptr_focus = NULL;
	if (W.kb_focus == s)
		W.kb_focus = NULL;
	if (W.cursor_surface == s)
		W.cursor_surface = NULL;
	surface_unmap(s);
	if (s->constraint)
		s->constraint->surface = NULL;
	canvas_free(s->opaque);
	if (s->pending_buffer && s->pending_attach)
		s->pending_buffer = NULL;
	buffer_unuse(s->buffer);
	s->buffer = NULL;
	for (int i = 0; i < s->nframes; i++)
		s->frames[i]->data = NULL;
	for (int i = 0; i < s->npending_frames; i++)
		s->pending_frames[i]->data = NULL;
	drop_frame_waiter(s);
	if (s->xdg)
		s->xdg->surface = NULL;
	if (s->sub) {
		sub_unlink(s->sub);
		s->sub->surface = NULL;
	}
	for (int i = 0; i < s->nkids; i++)
		s->kids[i]->parent = NULL;
	mem_free(s->kids);
	region_free(s->input);
	region_free(s->pending_input);
	mem_free(s);
}

static void surface_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlSurface *s = r->data;
	switch (op) {
	case WL_SURFACE_DESTROY:
		res_destroy(r);
		break;
	case WL_SURFACE_ATTACH:
		s->pending_attach = true;
		s->pending_buffer = a[0].obj ? a[0].obj->data : NULL;
		break;
	case WL_SURFACE_FRAME: {
		WlResource *cb = res_new(r->client, a[0].u, &wl_callback_interface, 1, callback_dispatch, s);
		if (!cb)
			return;
		cb->destroy = frame_callback_destroyed;
		if (s->npending_frames < (int)ARRAY_LEN(s->pending_frames))
			s->pending_frames[s->npending_frames++] = cb;
		else
			res_destroy(cb);
		break;
	}
	case WL_SURFACE_COMMIT:
		surface_commit(s);
		break;
	case WL_SURFACE_SET_INPUT_REGION:
		region_free(s->pending_input);
		s->pending_input = a[0].obj ? region_copy(a[0].obj->data) : NULL;
		s->pending_input_set = true;
		break;
	case WL_SURFACE_SET_BUFFER_SCALE:
		if ((i32)a[0].u < 1) {
			protocol_error(r->client, r->id, 0, "buffer scale below 1");
			return;
		}
		s->pending_scale = (i32)a[0].u;
		break;
	default:
		/* damage, regions, transform, offset: zdisplay redraws whole windows */
		break;
	}
}

static void compositor_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_COMPOSITOR_CREATE_SURFACE) {
		WlSurface *s = NEW(WlSurface);
		s->client = r->client;
		s->scale = 1;
		s->res = res_new(r->client, a[0].u, &wl_surface_interface, r->version, surface_dispatch, s);
		if (!s->res) {
			mem_free(s);
			return;
		}
		s->res->destroy = surface_destroyed;
		/* every surface is on the one output */
		for (int i = 0; i < r->client->noutputs; i++)
			event(s->res, WL_SURFACE_EV_ENTER, "o", r->client->outputs[i]->id);
		if (r->version >= 6) {
			event(s->res, WL_SURFACE_EV_PREFERRED_BUFFER_SCALE, "i", (u32)MAX(1, (i32)ceilf(S.scale - 0.01f)));
			event(s->res, WL_SURFACE_EV_PREFERRED_BUFFER_TRANSFORM, "u", 0);
		}
	} else if (op == WL_COMPOSITOR_CREATE_REGION) {
		WlRegion *g = NEW(WlRegion);
		WlResource *rr = res_new(r->client, a[0].u, &wl_region_interface, 1, region_dispatch, g);
		if (rr)
			rr->destroy = region_destroyed;
		else
			mem_free(g);
	}
}

/* ---- Viewports ---------------------------------------------------------------------------- */

static void viewport_destroyed(WlResource *r)
{
	WlSurface *s = r->data;
	if (s)
		s->vp_w = s->vp_h = 0;
}

static void viewport_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlSurface *s = r->data;
	if (op == WP_VIEWPORT_DESTROY) {
		res_destroy(r);
	} else if (op == WP_VIEWPORT_SET_DESTINATION && s) {
		i32 w = (i32)a[0].u, h = (i32)a[1].u;
		s->vp_w = w > 0 ? w : 0, s->vp_h = h > 0 ? h : 0;
	}
	/* set_source: zdisplay shows the whole buffer */
}

static void viewporter_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WP_VIEWPORTER_DESTROY) {
		res_destroy(r);
	} else if (op == WP_VIEWPORTER_GET_VIEWPORT) {
		WlResource *v = res_new(r->client, a[0].u, &wp_viewport_interface, 1, viewport_dispatch, a[1].obj->data);
		if (v)
			v->destroy = viewport_destroyed;
	}
}

/* ---- Output ----------------------------------------------------------------------------------- */

static void output_send(WlResource *o)
{
	i32 w = S.out ? S.out->width : (i32)(S.sw * S.scale), h = S.out ? S.out->height : (i32)(S.sh * S.scale);
	i32 refresh = S.out && S.out->refresh_mhz > 0 ? S.out->refresh_mhz : 60000;
	f32 dpi = S.out && S.out->dpi > 0 ? S.out->dpi : 96;
	event(o, WL_OUTPUT_EV_GEOMETRY, "iiiiissi", 0, 0, (u32)(i32)((f32)w / dpi * 25.4f), (u32)(i32)((f32)h / dpi * 25.4f), 0,
	      "ZOS", S.out ? S.out->name : "screen", 0);
	event(o, WL_OUTPUT_EV_MODE, "uiii", 3 /* current, preferred */, (u32)w, (u32)h, (u32)refresh);
	if (o->version >= 2)
		event(o, WL_OUTPUT_EV_SCALE, "i", (u32)MAX(1, (i32)ceilf(S.scale - 0.01f)));
	if (o->version >= 4) {
		event(o, WL_OUTPUT_EV_NAME, "s", "ZOS-1");
		event(o, WL_OUTPUT_EV_DESCRIPTION, "s", "The ZOS screen");
	}
	if (o->version >= 2)
		event(o, WL_OUTPUT_EV_DONE, "");
}

static void output_destroyed(WlResource *r)
{
	remove_ptr(r->client->outputs, &r->client->noutputs, r);
}

static void output_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_OUTPUT_RELEASE)
		plain_destroy(r, op, a);
}

static void xdg_output_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == ZXDG_OUTPUT_V1_DESTROY)
		plain_destroy(r, op, a);
}

static void xdg_output_manager_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == ZXDG_OUTPUT_MANAGER_V1_DESTROY) {
		res_destroy(r);
	} else if (op == ZXDG_OUTPUT_MANAGER_V1_GET_XDG_OUTPUT) {
		WlResource *x = res_new(r->client, a[0].u, &zxdg_output_v1_interface, r->version, xdg_output_dispatch, NULL);
		if (!x)
			return;
		event(x, ZXDG_OUTPUT_V1_EV_LOGICAL_POSITION, "ii", 0, 0);
		event(x, ZXDG_OUTPUT_V1_EV_LOGICAL_SIZE, "ii", (u32)(i32)S.sw, (u32)(i32)S.sh);
		if (x->version >= 2) {
			event(x, ZXDG_OUTPUT_V1_EV_NAME, "s", "ZOS-1");
			event(x, ZXDG_OUTPUT_V1_EV_DESCRIPTION, "s", "The ZOS screen");
		}
		if (x->version < 3)
			event(x, ZXDG_OUTPUT_V1_EV_DONE, "");
		else
			event(a[1].obj, WL_OUTPUT_EV_DONE, "");
	}
}

/* ---- Clipboard (accepted, not yet connected to ZOS's) ------------------------------------- */

static void data_source_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_DATA_SOURCE_DESTROY)
		plain_destroy(r, op, a);
}

static void data_device_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_DATA_DEVICE_RELEASE)
		plain_destroy(r, op, a);
}

static void data_device_manager_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_DATA_DEVICE_MANAGER_CREATE_DATA_SOURCE)
		res_new(r->client, a[0].u, &wl_data_source_interface, r->version, data_source_dispatch, NULL);
	else if (op == WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE)
		res_new(r->client, a[0].u, &wl_data_device_interface, r->version, data_device_dispatch, NULL);
}

/* ---- Subsurfaces ---------------------------------------------------------------------------- */

static void subsurface_destroyed(WlResource *r)
{
	WlSub *k = r->data;
	sub_unlink(k);
	if (k->surface)
		k->surface->sub = NULL;
	mem_free(k);
}

/* Moves k just above (or below) sibling, which may be the parent itself. */
static void sub_restack(WlSub *k, WlSurface *sibling, bool above)
{
	WlSurface *p = k->parent;
	if (!p || sibling == k->surface)
		return;
	/* the stack with the parent in it (NULL stands for the parent), without k */
	int n = 0;
	WlSub **order = mem_alloc(sizeof(WlSub *) * (usize)(p->nkids + 1));
	for (int i = 0; i <= p->nkids; i++) {
		if (i == p->below)
			order[n++] = NULL;
		if (i < p->nkids && p->kids[i] != k)
			order[n++] = p->kids[i];
	}
	int at = -1;
	for (int i = 0; i < n && at < 0; i++)
		if (sibling == p ? order[i] == NULL : order[i] && order[i]->surface == sibling)
			at = i;
	if (at < 0) {
		mem_free(order);
		return;
	}
	int pos = above ? at + 1 : at;
	memmove(&order[pos + 1], &order[pos], sizeof(WlSub *) * (usize)(n - pos));
	order[pos] = k;
	n++;
	int kids = 0;
	for (int i = 0; i < n; i++) {
		if (order[i])
			p->kids[kids++] = order[i];
		else
			p->below = kids;
	}
	mem_free(order);
	Window *w = surface_window(p);
	if (w)
		window_damage(w);
}

static void subsurface_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlSub *k = r->data;
	switch (op) {
	case WL_SUBSURFACE_DESTROY:
		res_destroy(r);
		break;
	case WL_SUBSURFACE_SET_POSITION:
		k->px = (i32)a[0].u, k->py = (i32)a[1].u;
		break;
	case WL_SUBSURFACE_PLACE_ABOVE:
	case WL_SUBSURFACE_PLACE_BELOW:
		if (a[0].obj && a[0].obj->data)
			sub_restack(k, a[0].obj->data, op == WL_SUBSURFACE_PLACE_ABOVE);
		break;
	case WL_SUBSURFACE_SET_SYNC:
		k->sync = true;
		break;
	case WL_SUBSURFACE_SET_DESYNC:
		k->sync = false;
		break;
	}
}

static void subcompositor_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_SUBCOMPOSITOR_DESTROY) {
		res_destroy(r);
	} else if (op == WL_SUBCOMPOSITOR_GET_SUBSURFACE) {
		WlSurface *s = a[1].obj->data;
		if (s->role != ROLE_NONE && s->role != ROLE_SUBSURFACE) {
			protocol_error(r->client, r->id, 0, "surface already has a role");
			return;
		}
		WlSurface *parent = a[2].obj->data;
		/* a surface can't be its own ancestor */
		for (WlSurface *p = parent; p; p = p->sub ? p->sub->parent : NULL)
			if (p == s) {
				protocol_error(r->client, r->id, 0, "a surface can't be a subsurface of itself");
				return;
			}
		if (s->sub) {
			protocol_error(r->client, r->id, 0, "surface already is a subsurface");
			return;
		}
		s->role = ROLE_SUBSURFACE;
		WlSub *k = NEW(WlSub);
		k->surface = s, k->parent = parent, k->sync = true;
		k->res = res_new(r->client, a[0].u, &wl_subsurface_interface, 1, subsurface_dispatch, k);
		if (!k->res) {
			mem_free(k);
			return;
		}
		k->res->destroy = subsurface_destroyed;
		s->sub = k;
		/* new subsurfaces go on top of their siblings */
		parent->kids = mem_realloc(parent->kids, sizeof(WlSub *) * (usize)(parent->nkids + 1));
		parent->kids[parent->nkids++] = k;
	}
}

/* ---- Seat: pointer and keyboard ------------------------------------------------------------- */

static void pointer_destroyed(WlResource *r)
{
	remove_ptr(r->client->pointers, &r->client->npointers, r);
}

static void keyboard_destroyed(WlResource *r)
{
	remove_ptr(r->client->keyboards, &r->client->nkeyboards, r);
}

static void cursor_set_for(WlClient *c, WlSurface *cs, i32 hx, i32 hy);

static void pointer_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_POINTER_SET_CURSOR) {
		WlSurface *cs = a[1].obj ? a[1].obj->data : NULL;
		if (cs) {
			if (cs->role != ROLE_NONE && cs->role != ROLE_CURSOR) {
				protocol_error(r->client, r->id, 0, "surface already has a role");
				return;
			}
			cs->role = ROLE_CURSOR;
		}
		cursor_set_for(r->client, cs, (i32)a[2].u, (i32)a[3].u);
	} else if (op == WL_POINTER_RELEASE) {
		res_destroy(r);
	}
}

static void keyboard_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_KEYBOARD_RELEASE)
		plain_destroy(r, op, a);
}

static void touch_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_TOUCH_RELEASE)
		plain_destroy(r, op, a);
}

static u32 xkb_mods_depressed(void);
static u32 xkb_mods_locked(void);

static void keyboard_hello(WlResource *k)
{
	if (W.keymap_fd >= 0)
		event(k, WL_KEYBOARD_EV_KEYMAP, "uhu", 1 /* xkb v1 */, W.keymap_fd, W.keymap_size);
	if (k->version >= 4)
		event(k, WL_KEYBOARD_EV_REPEAT_INFO, "ii", 30, 400);
	/* already focused (a second keyboard object) */
	if (W.kb_focus && W.kb_focus->client == k->client) {
		event(k, WL_KEYBOARD_EV_ENTER, "uoa", next_serial(), W.kb_focus->res->id, W.keys_down, (u32)(W.nkeys_down * 4));
		event(k, WL_KEYBOARD_EV_MODIFIERS, "uuuuu", next_serial(), xkb_mods_depressed(), 0, xkb_mods_locked(), 0);
	}
}

static void seat_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlClient *c = r->client;
	switch (op) {
	case WL_SEAT_GET_POINTER: {
		WlResource *p = res_new(c, a[0].u, &wl_pointer_interface, r->version, pointer_dispatch, NULL);
		if (!p)
			return;
		p->destroy = pointer_destroyed;
		if (c->npointers < (int)ARRAY_LEN(c->pointers))
			c->pointers[c->npointers++] = p;
		break;
	}
	case WL_SEAT_GET_KEYBOARD: {
		WlResource *k = res_new(c, a[0].u, &wl_keyboard_interface, r->version, keyboard_dispatch, NULL);
		if (!k)
			return;
		k->destroy = keyboard_destroyed;
		if (c->nkeyboards < (int)ARRAY_LEN(c->keyboards))
			c->keyboards[c->nkeyboards++] = k;
		keyboard_hello(k);
		break;
	}
	case WL_SEAT_GET_TOUCH:
		res_new(c, a[0].u, &wl_touch_interface, r->version, touch_dispatch, NULL);
		break;
	case WL_SEAT_RELEASE:
		res_destroy(r);
		break;
	}
}

/* ---- The keymap -------------------------------------------------------------------------------- */

/* X keysym names for ASCII; others are written as Unicode (U00E9). */
static const char *const ASCII_SYMS[95] = {
	"space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "apostrophe", "parenleft", "parenright",
	"asterisk", "plus", "comma", "minus", "period", "slash", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "colon",
	"semicolon", "less", "equal", "greater", "question", "at", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",
	"M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright",
	"asciicircum", "underscore", "grave", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p",
	"q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde",
};

static void keysym_name(const char *utf8, char *out, usize cap)
{
	if (!utf8 || !*utf8) {
		str_copy(out, "NoSymbol", cap);
		return;
	}
	u32 cp;
	utf8_decode(utf8, strlen(utf8), &cp);
	if (cp >= 32 && cp < 127)
		str_copy(out, ASCII_SYMS[cp - 32], cap);
	else
		fmt(out, cap, "U%04X", cp);
}

/* Keys that type nothing: evdev code and keysym. */
static const struct { u16 code; const char *sym; } SPECIAL_KEYS[] = {
	{ 1, "Escape" }, { 14, "BackSpace" }, { 15, "Tab" }, { 28, "Return" }, { 29, "Control_L" }, { 42, "Shift_L" },
	{ 54, "Shift_R" }, { 55, "KP_Multiply" }, { 56, "Alt_L" }, { 58, "Caps_Lock" }, { 59, "F1" }, { 60, "F2" },
	{ 61, "F3" }, { 62, "F4" }, { 63, "F5" }, { 64, "F6" }, { 65, "F7" }, { 66, "F8" }, { 67, "F9" }, { 68, "F10" },
	{ 69, "Num_Lock" }, { 70, "Scroll_Lock" }, { 74, "KP_Subtract" }, { 78, "KP_Add" }, { 87, "F11" }, { 88, "F12" },
	{ 96, "KP_Enter" }, { 97, "Control_R" }, { 98, "KP_Divide" }, { 99, "Print" }, { 100, "ISO_Level3_Shift" },
	{ 102, "Home" }, { 103, "Up" }, { 104, "Prior" }, { 105, "Left" }, { 106, "Right" }, { 107, "End" }, { 108, "Down" },
	{ 109, "Next" }, { 110, "Insert" }, { 111, "Delete" }, { 113, "XF86AudioMute" }, { 114, "XF86AudioLowerVolume" },
	{ 115, "XF86AudioRaiseVolume" }, { 116, "XF86PowerOff" }, { 117, "KP_Equal" }, { 119, "Pause" }, { 125, "Super_L" },
	{ 126, "Super_R" }, { 127, "Menu" }, { 163, "XF86AudioNext" }, { 164, "XF86AudioPlay" }, { 165, "XF86AudioPrev" },
	{ 166, "XF86AudioStop" }, { 224, "XF86MonBrightnessDown" }, { 225, "XF86MonBrightnessUp" },
};

/* The keypad: evdev code, with Num Lock, without. */
static const struct { u16 code; const char *on, *off; } KEYPAD[] = {
	{ 71, "KP_7", "KP_Home" }, { 72, "KP_8", "KP_Up" }, { 73, "KP_9", "KP_Prior" }, { 75, "KP_4", "KP_Left" },
	{ 76, "KP_5", "KP_Begin" }, { 77, "KP_6", "KP_Right" }, { 79, "KP_1", "KP_End" }, { 80, "KP_2", "KP_Down" },
	{ 81, "KP_3", "KP_Next" }, { 82, "KP_0", "KP_Insert" }, { 83, "KP_Decimal", "KP_Delete" },
};

static void build_keymap(StrBuf *sb)
{
	sb_adds(sb, "xkb_keymap {\nxkb_keycodes \"zos\" {\n\tminimum = 8;\n\tmaximum = 255;\n");
	for (int k = 1; k < 248; k++)
		sb_fmt(sb, "\t<K%d> = %d;\n", k, k + 8);
	sb_adds(sb, "\tindicator 1 = \"Caps Lock\";\n\tindicator 2 = \"Num Lock\";\n};\n");
	sb_adds(sb, "xkb_types \"zos\" {\n\tvirtual_modifiers NumLock,Alt,LevelThree,Super;\n"
		    "\ttype \"ONE_LEVEL\" { modifiers = none; level_name[Level1] = \"Any\"; };\n"
		    "\ttype \"TWO_LEVEL\" { modifiers = Shift; map[Shift] = Level2; level_name[Level1] = \"Base\"; level_name[Level2] = \"Shift\"; };\n"
		    "\ttype \"ALPHABETIC\" { modifiers = Shift+Lock; map[Shift] = Level2; map[Lock] = Level2;"
		    " level_name[Level1] = \"Base\"; level_name[Level2] = \"Caps\"; };\n"
		    "\ttype \"FOUR_LEVEL\" { modifiers = Shift+LevelThree; map[Shift] = Level2; map[LevelThree] = Level3;"
		    " map[Shift+LevelThree] = Level4; level_name[Level1] = \"Base\"; level_name[Level2] = \"Shift\";"
		    " level_name[Level3] = \"AltGr\"; level_name[Level4] = \"Shift AltGr\"; };\n"
		    "\ttype \"FOUR_LEVEL_ALPHABETIC\" { modifiers = Shift+Lock+LevelThree; map[Shift] = Level2; map[Lock] = Level2;"
		    " map[LevelThree] = Level3; map[Shift+LevelThree] = Level4; map[Lock+LevelThree] = Level4;"
		    " map[Shift+Lock+LevelThree] = Level3; level_name[Level1] = \"Base\"; level_name[Level2] = \"Shift\";"
		    " level_name[Level3] = \"AltGr\"; level_name[Level4] = \"Shift AltGr\"; };\n"
		    "\ttype \"KEYPAD\" { modifiers = Shift+NumLock; map[None] = Level1; map[Shift] = Level2; map[NumLock] = Level2;"
		    " map[Shift+NumLock] = Level1; level_name[Level1] = \"Base\"; level_name[Level2] = \"Number\"; };\n};\n");
	sb_adds(sb, "xkb_compatibility \"zos\" {\n\tvirtual_modifiers NumLock,Alt,LevelThree,Super;\n"
		    "\tinterpret.useModMapMods = AnyLevel;\n\tinterpret.repeat = False;\n"
		    "\tinterpret Shift_L+AnyOf(all) { action = SetMods(modifiers=Shift,clearLocks); };\n"
		    "\tinterpret Shift_R+AnyOf(all) { action = SetMods(modifiers=Shift,clearLocks); };\n"
		    "\tinterpret Control_L+AnyOf(all) { action = SetMods(modifiers=Control,clearLocks); };\n"
		    "\tinterpret Control_R+AnyOf(all) { action = SetMods(modifiers=Control,clearLocks); };\n"
		    "\tinterpret Alt_L+AnyOf(all) { virtualModifier = Alt; action = SetMods(modifiers=modMapMods,clearLocks); };\n"
		    "\tinterpret Super_L+AnyOf(all) { virtualModifier = Super; action = SetMods(modifiers=modMapMods,clearLocks); };\n"
		    "\tinterpret Super_R+AnyOf(all) { virtualModifier = Super; action = SetMods(modifiers=modMapMods,clearLocks); };\n"
		    "\tinterpret ISO_Level3_Shift+AnyOf(all) { virtualModifier = LevelThree; useModMapMods = level1;"
		    " action = SetMods(modifiers=LevelThree,clearLocks); };\n"
		    "\tinterpret Caps_Lock+AnyOfOrNone(all) { action = LockMods(modifiers=Lock); };\n"
		    "\tinterpret Num_Lock+AnyOf(all) { virtualModifier = NumLock; action = LockMods(modifiers=NumLock); };\n"
		    "\tinterpret Any + Any { action = SetMods(modifiers=modMapMods); };\n"
		    "\tindicator \"Caps Lock\" { whichModState = locked; modifiers = Lock; };\n"
		    "\tindicator \"Num Lock\" { whichModState = locked; modifiers = NumLock; };\n};\n");
	sb_adds(sb, "xkb_symbols \"zos\" {\n\tname[group1] = \"ZOS\";\n");
	for (usize i = 0; i < ARRAY_LEN(SPECIAL_KEYS); i++)
		sb_fmt(sb, "\tkey <K%d> { [ %s ] };\n", SPECIAL_KEYS[i].code, SPECIAL_KEYS[i].sym);
	for (usize i = 0; i < ARRAY_LEN(KEYPAD); i++)
		sb_fmt(sb, "\tkey <K%d> { type = \"KEYPAD\", [ %s, %s ] };\n", KEYPAD[i].code, KEYPAD[i].off, KEYPAD[i].on);
	/* the typing keys, from the active layout */
	for (u32 code = 1; code < 128; code++) {
		const char *lo, *up, *gr;
		if (!input_key_texts(code, &lo, &up, &gr) || (!lo && !gr))
			continue;
		bool special = false;
		for (usize i = 0; i < ARRAY_LEN(SPECIAL_KEYS); i++)
			special |= SPECIAL_KEYS[i].code == code;
		if (special)
			continue;
		char l[32], u[32], g[32], gu[32];
		keysym_name(lo, l, sizeof l);
		keysym_name(up ? up : lo, u, sizeof u);
		/* letters follow Caps Lock: lower and upper differ, and lower is a letter */
		bool letter = lo && up && !str_eq(lo, up) && ((u8)lo[0] < 0x80 ? is_alpha((u8)lo[0]) : (u8)up[0] >= 0xC3);
		if (gr) {
			keysym_name(gr, g, sizeof g);
			str_copy(gu, g, sizeof gu);
			sb_fmt(sb, "\tkey <K%u> { type = \"%s\", [ %s, %s, %s, %s ] };\n", code, letter ? "FOUR_LEVEL_ALPHABETIC" : "FOUR_LEVEL", l, u, g,
				gu);
		} else {
			sb_fmt(sb, "\tkey <K%u> { type = \"%s\", [ %s, %s ] };\n", code, letter ? "ALPHABETIC" : "TWO_LEVEL", l, u);
		}
	}
	sb_adds(sb, "\tmodifier_map Shift { <K42>, <K54> };\n\tmodifier_map Lock { <K58> };\n"
		    "\tmodifier_map Control { <K29>, <K97> };\n\tmodifier_map Mod1 { <K56> };\n\tmodifier_map Mod2 { <K69> };\n"
		    "\tmodifier_map Mod4 { <K125>, <K126> };\n\tmodifier_map Mod5 { <K100> };\n};\n};\n");
}

void wl_keymap_changed(void)
{
	StrBuf sb = { 0 };
	build_keymap(&sb);
	int fd = (int)sys_memfd_create("zos-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		sb_free(&sb);
		return;
	}
	u32 size = (u32)sb.len + 1;
	bool ok = write_all(fd, sb.data, size);
	sb_free(&sb);
	if (!ok) {
		sys_close(fd);
		return;
	}
	sys_fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
	if (W.keymap_fd >= 0)
		sys_close(W.keymap_fd);
	W.keymap_fd = fd;
	W.keymap_size = size;
	for (int i = 0; i < W.nclients; i++)
		for (int k = 0; k < W.clients[i]->nkeyboards; k++)
			event(W.clients[i]->keyboards[k], WL_KEYBOARD_EV_KEYMAP, "uhu", 1, W.keymap_fd, W.keymap_size);
}

/* ---- GPU buffers (linux-dmabuf) --------------------------------------------------------------- */

/* zdisplay composes on the processor, so it asks for linear 32-bit buffers:
 * it can read those, and the display can show them as they are. */
#define FOURCC_ARGB8888 0x34325241u /* 'AR24' */
#define FOURCC_XRGB8888 0x34325258u /* 'XR24' */
#define MOD_LINEAR 0ull
#define MOD_INVALID 0x00ffffffffffffffull /* "whatever the driver does implicitly" */

enum { PARAMS_ALREADY_USED, PARAMS_PLANE_IDX, PARAMS_PLANE_SET, PARAMS_INCOMPLETE, PARAMS_INVALID_FORMAT,
       PARAMS_INVALID_DIMENSIONS, PARAMS_OUT_OF_BOUNDS };

typedef struct {
	int fd;
	u32 offset, stride;
	u64 modifier;
	bool used;
} WlParams;

static void params_destroyed(WlResource *r)
{
	WlParams *p = r->data;
	if (p->fd >= 0)
		sys_close(p->fd);
	mem_free(p);
}

static void params_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlParams *p = r->data;
	switch (op) {
	case ZWP_LINUX_BUFFER_PARAMS_V1_DESTROY:
		res_destroy(r);
		return;
	case ZWP_LINUX_BUFFER_PARAMS_V1_ADD: {
		int fd = a[0].fd;
		u32 code = p->used ? PARAMS_ALREADY_USED : a[1].u != 0 ? PARAMS_PLANE_IDX : p->fd >= 0 ? PARAMS_PLANE_SET : 99;
		if (code != 99) {
			sys_close(fd);
			protocol_error(r->client, r->id, code, "only one plane, set once");
			return;
		}
		p->fd = fd, p->offset = a[2].u, p->stride = a[3].u, p->modifier = (u64)a[4].u << 32 | a[5].u;
		return;
	}
	case ZWP_LINUX_BUFFER_PARAMS_V1_CREATE:
		/* buffers made by the server: programs use create_immed, this says no */
		p->used = true;
		event(r, ZWP_LINUX_BUFFER_PARAMS_V1_EV_FAILED, "");
		return;
	case ZWP_LINUX_BUFFER_PARAMS_V1_CREATE_IMMED: {
		i32 w = (i32)a[1].u, h = (i32)a[2].u;
		u32 format = a[3].u;
		if (p->used || p->fd < 0) {
			protocol_error(r->client, r->id, p->used ? PARAMS_ALREADY_USED : PARAMS_INCOMPLETE, "no plane to make a buffer from");
			return;
		}
		p->used = true;
		if ((format != FOURCC_ARGB8888 && format != FOURCC_XRGB8888) || (p->modifier != MOD_LINEAR && p->modifier != MOD_INVALID)) {
			protocol_error(r->client, r->id, PARAMS_INVALID_FORMAT, "format or modifier not offered");
			return;
		}
		if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || p->stride < (u32)w * 4) {
			protocol_error(r->client, r->id, PARAMS_INVALID_DIMENSIONS, "bad buffer size");
			return;
		}
		long size = sys_lseek(p->fd, 0, SEEK_END);
		if (size >= 0 && (u64)p->offset + (u64)p->stride * (u64)h > (u64)size) {
			protocol_error(r->client, r->id, PARAMS_OUT_OF_BOUNDS, "buffer bigger than its memory");
			return;
		}
		WlBuffer *b = NEW(WlBuffer);
		b->dmabuf = p->fd, b->offset = (i32)p->offset, b->stride = (i32)p->stride, b->width = w, b->height = h;
		b->format = format == FOURCC_XRGB8888 ? 1 : 0;
		b->modifier = p->modifier;
		p->fd = -1;
		b->res = res_new(r->client, a[0].u, &wl_buffer_interface, 1, buffer_dispatch, b);
		if (b->res) {
			b->res->destroy = buffer_destroyed;
		} else {
			sys_close(b->dmabuf);
			mem_free(b);
		}
		return;
	}
	}
}

static void feedback_dispatch(WlResource *r, u32 op, WlArg *a)
{
	plain_destroy(r, op, a); /* destroy is its only request */
}

/* Where to render and in what: the display's GPU, linear XRGB or ARGB. */
static void feedback_send(WlResource *f)
{
	if (W.format_table < 0) {
		struct {
			u32 format, pad;
			u64 modifier;
		} table[2] = { { FOURCC_XRGB8888, 0, MOD_LINEAR }, { FOURCC_ARGB8888, 0, MOD_LINEAR } };
		int fd = (int)sys_memfd_create("zos-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (fd < 0 || !write_all(fd, table, sizeof table)) {
			if (fd >= 0)
				sys_close(fd);
			return;
		}
		sys_fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
		W.format_table = fd;
	}
	u64 dev = S.out->device;
	u16 indices[2] = { 0, 1 };
	event(f, ZWP_LINUX_DMABUF_FEEDBACK_V1_EV_FORMAT_TABLE, "hu", W.format_table, 32u);
	event(f, ZWP_LINUX_DMABUF_FEEDBACK_V1_EV_MAIN_DEVICE, "a", &dev, 8u);
	event(f, ZWP_LINUX_DMABUF_FEEDBACK_V1_EV_TRANCHE_TARGET_DEVICE, "a", &dev, 8u);
	event(f, ZWP_LINUX_DMABUF_FEEDBACK_V1_EV_TRANCHE_FORMATS, "a", indices, (u32)sizeof indices);
	event(f, ZWP_LINUX_DMABUF_FEEDBACK_V1_EV_TRANCHE_FLAGS, "u", 0);
	event(f, ZWP_LINUX_DMABUF_FEEDBACK_V1_EV_TRANCHE_DONE, "");
	event(f, ZWP_LINUX_DMABUF_FEEDBACK_V1_EV_DONE, "");
}

static void dmabuf_dispatch(WlResource *r, u32 op, WlArg *a)
{
	switch (op) {
	case ZWP_LINUX_DMABUF_V1_DESTROY:
		res_destroy(r);
		break;
	case ZWP_LINUX_DMABUF_V1_CREATE_PARAMS: {
		WlParams *p = NEW(WlParams);
		p->fd = -1;
		WlResource *pr = res_new(r->client, a[0].u, &zwp_linux_buffer_params_v1_interface, r->version, params_dispatch, p);
		if (pr)
			pr->destroy = params_destroyed;
		else
			mem_free(p);
		break;
	}
	case ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK:
	case ZWP_LINUX_DMABUF_V1_GET_SURFACE_FEEDBACK: {
		WlResource *f = res_new(r->client, a[0].u, &zwp_linux_dmabuf_feedback_v1_interface, r->version, feedback_dispatch, NULL);
		if (f)
			feedback_send(f);
		break;
	}
	}
}

static void bind_dmabuf(WlClient *c, u32 id, u32 v)
{
	WlResource *r = res_new(c, id, &zwp_linux_dmabuf_v1_interface, v, dmabuf_dispatch, NULL);
	if (!r || v >= 4)
		return; /* version 4 programs ask for feedback instead */
	u32 formats[2] = { FOURCC_XRGB8888, FOURCC_ARGB8888 };
	for (int i = 0; i < 2; i++) {
		if (v >= 3)
			event(r, ZWP_LINUX_DMABUF_V1_EV_MODIFIER, "uuu", formats[i], (u32)(MOD_LINEAR >> 32), (u32)MOD_LINEAR);
		else
			event(r, ZWP_LINUX_DMABUF_V1_EV_FORMAT, "u", formats[i]);
	}
}

/* ---- Globals and the registry ---------------------------------------------------------------- */

static void bind_compositor(WlClient *c, u32 id, u32 v) { res_new(c, id, &wl_compositor_interface, v, compositor_dispatch, NULL); }
static void bind_subcompositor(WlClient *c, u32 id, u32 v) { res_new(c, id, &wl_subcompositor_interface, v, subcompositor_dispatch, NULL); }
static void bind_viewporter(WlClient *c, u32 id, u32 v) { res_new(c, id, &wp_viewporter_interface, v, viewporter_dispatch, NULL); }
static void bind_data_device_manager(WlClient *c, u32 id, u32 v) { res_new(c, id, &wl_data_device_manager_interface, v, data_device_manager_dispatch, NULL); }
static void bind_xdg_output_manager(WlClient *c, u32 id, u32 v) { res_new(c, id, &zxdg_output_manager_v1_interface, v, xdg_output_manager_dispatch, NULL); }

static void bind_shm(WlClient *c, u32 id, u32 v)
{
	WlResource *r = res_new(c, id, &wl_shm_interface, v, shm_dispatch, NULL);
	if (!r)
		return;
	event(r, WL_SHM_EV_FORMAT, "u", 0); /* ARGB8888 */
	event(r, WL_SHM_EV_FORMAT, "u", 1); /* XRGB8888 */
}

static void bind_seat(WlClient *c, u32 id, u32 v)
{
	WlResource *r = res_new(c, id, &wl_seat_interface, v, seat_dispatch, NULL);
	if (!r)
		return;
	event(r, WL_SEAT_EV_CAPABILITIES, "u", 3 /* pointer, keyboard */);
	if (v >= 2)
		event(r, WL_SEAT_EV_NAME, "s", "seat0");
}

static void bind_output(WlClient *c, u32 id, u32 v)
{
	WlResource *r = res_new(c, id, &wl_output_interface, v, output_dispatch, NULL);
	if (!r)
		return;
	r->destroy = output_destroyed;
	if (c->noutputs < (int)ARRAY_LEN(c->outputs))
		c->outputs[c->noutputs++] = r;
	output_send(r);
}

static void bind_xdg_wm_base(WlClient *c, u32 id, u32 v);
static void bind_decoration_manager(WlClient *c, u32 id, u32 v);
static void bind_relative_pointer_manager(WlClient *c, u32 id, u32 v);
static void bind_pointer_constraints(WlClient *c, u32 id, u32 v);

static const struct {
	const WlInterface *iface;
	void (*bind)(WlClient *c, u32 id, u32 version);
} GLOBALS[] = {
	{ &wl_compositor_interface, bind_compositor },
	{ &wl_subcompositor_interface, bind_subcompositor },
	{ &wl_shm_interface, bind_shm },
	{ &wl_seat_interface, bind_seat },
	{ &wl_output_interface, bind_output },
	{ &wl_data_device_manager_interface, bind_data_device_manager },
	{ &xdg_wm_base_interface, bind_xdg_wm_base },
	{ &wp_viewporter_interface, bind_viewporter },
	{ &zxdg_output_manager_v1_interface, bind_xdg_output_manager },
	{ &zxdg_decoration_manager_v1_interface, bind_decoration_manager },
	{ &zwp_relative_pointer_manager_v1_interface, bind_relative_pointer_manager },
	{ &zwp_pointer_constraints_v1_interface, bind_pointer_constraints },
	{ &zwp_linux_dmabuf_v1_interface, bind_dmabuf },
};

/* GPU buffers only when the display runs on a GPU driver. */
static bool global_offered(u32 i)
{
	return GLOBALS[i].iface != &zwp_linux_dmabuf_v1_interface || (S.out && S.out->device);
}

static void registry_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op != WL_REGISTRY_BIND)
		return;
	u32 name = a[0].u, version = a[2].u, id = a[3].u;
	if (name == 0 || name > ARRAY_LEN(GLOBALS) || !global_offered(name - 1) || !a[1].s || !str_eq(a[1].s, GLOBALS[name - 1].iface->name) ||
	    version == 0 || version > (u32)GLOBALS[name - 1].iface->version) {
		protocol_error(r->client, r->id, 0, "invalid global");
		return;
	}
	GLOBALS[name - 1].bind(r->client, id, version);
}

static void display_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == WL_DISPLAY_SYNC) {
		WlResource *cb = res_new(r->client, a[0].u, &wl_callback_interface, 1, callback_dispatch, NULL);
		if (cb) {
			event(cb, WL_CALLBACK_EV_DONE, "u", next_serial());
			res_destroy(cb);
		}
	} else if (op == WL_DISPLAY_GET_REGISTRY) {
		WlResource *reg = res_new(r->client, a[0].u, &wl_registry_interface, 1, registry_dispatch, NULL);
		if (!reg)
			return;
		for (u32 i = 0; i < ARRAY_LEN(GLOBALS); i++)
			if (global_offered(i))
				event(reg, WL_REGISTRY_EV_GLOBAL, "usu", i + 1, GLOBALS[i].iface->name, (u32)GLOBALS[i].iface->version);
	}
}

/* ---- Reading requests ---------------------------------------------------------------------------- */

static bool take_fd(WlClient *c, int *fd)
{
	if (c->nin_fds == 0)
		return false;
	*fd = c->in_fds[0];
	memmove(c->in_fds, c->in_fds + 1, sizeof(int) * (usize)--c->nin_fds);
	return true;
}

/* Handles one message; returns its size, 0 if it isn't all here yet, -1 on error. */
static long handle_one(WlClient *c, const u8 *p, usize avail)
{
	if (avail < 8)
		return 0;
	u32 id = get32(p), word = get32(p + 4);
	u32 size = word >> 16, op = word & 0xffff;
	if (size < 8 || size % 4)
		return -1;
	if (size > avail)
		return 0;
	WlResource *r = obj(c, id);
	if (!r) {
		/* a request on an object we already dropped: the client couldn't know yet */
		return (long)size;
	}
	if ((int)op >= r->iface->nrequests) {
		protocol_error(c, id, 1, "unknown request");
		return -1;
	}
	const WlMessage *m = &r->iface->requests[op];
	WlArg args[WL_MAX_ARGS] = { 0 };
	int na = 0;
	usize at = 8;
	bool nullable = false;
	for (const char *s = m->signature; *s && na < WL_MAX_ARGS; s++) {
		if (*s == '?') {
			nullable = true;
			continue;
		}
		WlArg *a = &args[na++];
		a->fd = -1;
		if (*s == 'h') {
			if (!take_fd(c, &a->fd)) {
				protocol_error(c, id, 1, "missing file descriptor");
				return -1;
			}
			nullable = false;
			continue;
		}
		if (at + 4 > size)
			goto short_msg;
		u32 v = get32(p + at);
		at += 4;
		switch (*s) {
		case 'i': case 'u': case 'f': case 'n':
			a->u = v;
			break;
		case 'o':
			a->u = v;
			if (v) {
				a->obj = obj(c, v);
				if (!a->obj) {
					/* a just-dropped object: treat as null where allowed */
					if (!nullable) {
						protocol_error(c, id, 1, "unknown object");
						return -1;
					}
				}
			} else if (!nullable) {
				protocol_error(c, id, 1, "null object");
				return -1;
			}
			break;
		case 's':
			if (v == 0) {
				if (!nullable) {
					protocol_error(c, id, 1, "null string");
					return -1;
				}
				a->s = NULL;
				break;
			}
			if (at + wire_pad(v) > size || p[at + v - 1] != 0)
				goto short_msg;
			a->s = (const char *)p + at;
			at += wire_pad(v);
			break;
		case 'a':
			if (at + wire_pad(v) > size)
				goto short_msg;
			a->data = p + at;
			a->len = v;
			at += wire_pad(v);
			break;
		}
		nullable = false;
	}
	r->dispatch(r, op, args);
	return (long)size;
short_msg:
	protocol_error(c, id, 1, "malformed request");
	return -1;
}

static void on_wl_client(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(loop), UNUSED(fd);
	WlClient *c = data;
	if (c->dead)
		return;
	for (int burst = 0; burst < 16 && !c->dead; burst++) {
		int fds[WL_MAX_FDS], nfds = 0;
		long n = unix_recv(c->fd, c->in + c->in_len, sizeof c->in - c->in_len, fds, WL_MAX_FDS - c->nin_fds, &nfds);
		for (int i = 0; i < nfds; i++)
			c->in_fds[c->nin_fds++] = fds[i];
		if (n == -EAGAIN)
			break;
		if (n <= 0) {
			client_kill(c);
			break;
		}
		c->in_len += (usize)n;
		usize at = 0;
		while (!c->dead) {
			long used = handle_one(c, c->in + at, c->in_len - at);
			if (used < 0) {
				client_kill(c);
				break;
			}
			if (used == 0)
				break;
			at += (usize)used;
		}
		if (c->dead)
			break;
		memmove(c->in, c->in + at, c->in_len - at);
		c->in_len -= at;
		if (c->in_len == sizeof c->in) {
			client_kill(c); /* one message bigger than the buffer */
			break;
		}
	}
	if (!c->dead && (events & (EPOLLHUP | EPOLLERR)))
		client_kill(c);
	if (!c->dead)
		flush_client(c);
}

static void client_kill(WlClient *c)
{
	if (c->dead)
		return;
	c->dead = true;
	loop_del_fd(S.loop, c->fd);
	/* objects go newest first, so surfaces leave before what they reference */
	for (u32 id = c->nobjs; id-- > 0;)
		if (c->objs[id])
			res_destroy(c->objs[id]);
	mem_free(c->objs);
	c->objs = NULL;
	c->nobjs = 0;
	for (int i = 0; i < c->nin_fds; i++)
		sys_close(c->in_fds[i]);
	for (int i = 0; i < c->nout_fds; i++)
		sys_close(c->out_fds[i]);
	c->nin_fds = c->nout_fds = 0;
	sys_close(c->fd);
	for (int i = 0; i < W.nclients; i++)
		if (W.clients[i] == c) {
			W.clients[i] = W.clients[--W.nclients];
			break;
		}
	/* freed on the next flush, once nothing is iterating over it */
	request_frame();
}

static WlClient *graveyard[WL_MAX_CLIENTS * 2];
static int ngraveyard;

/* Programs ZOS starts carry ZOS_APP_ID, so their windows join the right dock icon. */
static void client_identify(WlClient *c)
{
	struct ucred cred = { 0 };
	u32 len = sizeof cred;
	if (sys_getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.pid <= 0)
		return;
	c->pid = cred.pid;
	char path[64];
	fmt(path, sizeof path, "/proc/%d/environ", cred.pid);
	usize n = 0;
	char *env = file_read(path, &n);
	for (usize i = 0; env && i < n; i += strlen(env + i) + 1)
		if (str_starts(env + i, "ZOS_APP_ID="))
			str_copy(c->app_id, env + i + 11, sizeof c->app_id);
	mem_free(env);
}

static void on_wl_accept(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(events), UNUSED(data);
	for (;;) {
		int cfd = (int)sys_accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (cfd < 0)
			break;
		if (W.nclients >= WL_MAX_CLIENTS) {
			sys_close(cfd);
			continue;
		}
		WlClient *c = NEW(WlClient);
		c->fd = cfd;
		client_identify(c);
		W.clients[W.nclients++] = c;
		res_new(c, 1, &wl_display_interface, 1, display_dispatch, NULL);
		loop_add_fd(loop, cfd, EPOLLIN, on_wl_client, c);
	}
}

void wl_flush(void)
{
	drop_broken_clients();
	for (int i = 0; i < W.nclients; i++)
		flush_client(W.clients[i]);
	/* clients that died during the last round */
	for (int i = 0; i < ngraveyard; i++)
		mem_free(graveyard[i]);
	ngraveyard = 0;
	for (int i = 0; i < W.nclients; i++)
		if (W.clients[i]->dead && ngraveyard < (int)ARRAY_LEN(graveyard))
			graveyard[ngraveyard++] = W.clients[i];
}

/* After each composed frame: frame callbacks fire and replaced buffers go back. */
void wl_frame_done(void)
{
	u32 t = now_ms();
	for (int i = 0; i < W.nframe_waiters; i++) {
		WlSurface *s = W.frame_waiters[i];
		for (int k = 0; k < s->nframes; k++) {
			WlResource *cb = s->frames[k];
			cb->data = NULL;
			event(cb, WL_CALLBACK_EV_DONE, "u", t);
			res_destroy(cb);
		}
		s->nframes = 0;
	}
	W.nframe_waiters = 0;
	for (int i = 0; i < W.nto_release; i++) {
		WlBuffer *b = W.to_release[i];
		b->release_after_frame = false;
		if (b->users == 0 && b->res)
			event(b->res, WL_BUFFER_EV_RELEASE, "");
		buffer_maybe_free(b);
	}
	W.nto_release = 0;
}

static char socket_path[160];

const char *wl_socket_path(void)
{
	return socket_path;
}

void wl_server_init(void)
{
	const char *path = env_get("ZOS_WAYLAND_SOCKET");
	if (!path || !*path) {
		const char *rt = env_get("XDG_RUNTIME_DIR");
		fmt(socket_path, sizeof socket_path, "%s/wayland-0", rt && *rt ? rt : "/run/zurbax");
	} else {
		str_copy(socket_path, path, sizeof socket_path);
	}
	path = socket_path;
	char *dir = path_dir(path);
	dir_make(dir, 0755);
	mem_free(dir);
	sys_unlink(path);
	W.listen_fd = unix_listen(path, SOCK_STREAM | SOCK_NONBLOCK);
	if (W.listen_fd < 0) {
		eprint("zdisplay: no Wayland socket at %s: %s\n", path, err_str(W.listen_fd));
		return;
	}
	sys_chmod(path, 0666);
	sig_handle_info(SIGBUS, on_sigbus);
	loop_add_fd(S.loop, W.listen_fd, EPOLLIN, on_wl_accept, NULL);
	wl_keymap_changed();
}


/* ---- Window geometry and pictures ------------------------------------------------------------ */

/* The window geometry in surface coordinates: the part of the surface that is
 * the window, without shadows the client drew around it. */
static void geometry(const WlSurface *s, i32 *x, i32 *y, i32 *w, i32 *h)
{
	i32 sw = wl_surface_width(s), sh = wl_surface_height(s);
	*x = 0, *y = 0, *w = sw, *h = sh;
	const WlXdgSurface *xs = s->xdg;
	if (!xs || xs->gw <= 0 || xs->gh <= 0)
		return;
	i32 x0 = MAX(xs->gx, 0), y0 = MAX(xs->gy, 0), x1 = MIN(xs->gx + xs->gw, sw), y1 = MIN(xs->gy + xs->gh, sh);
	if (x1 > x0 && y1 > y0)
		*x = x0, *y = y0, *w = x1 - x0, *h = y1 - y0;
}

/* Where the window's picture goes on screen (logical). A full-screen game
 * that keeps a smaller picture is shown centred at its aspect ratio. */
static Rect content_rect(const Window *w, Rect r)
{
	const WlSurface *s = w->wl;
	if (!s || !(w->states & STATE_FULLSCREEN))
		return r;
	i32 gx, gy, gw, gh;
	geometry(s, &gx, &gy, &gw, &gh);
	if (gw <= 0 || gh <= 0 || (fabsf((f32)gw - r.w) < 1 && fabsf((f32)gh - r.h) < 1))
		return r;
	f32 k = minf(r.w / (f32)gw, r.h / (f32)gh);
	f32 cw = (f32)gw * k, ch = (f32)gh * k;
	return rect(r.x + (r.w - cw) / 2, r.y + (r.h - ch) / 2, cw, ch);
}

/* Screen position to surface coordinates. */
static void to_surface(Window *w, f32 x, f32 y, f32 *sx, f32 *sy)
{
	WlSurface *s = w->wl;
	i32 gx, gy, gw, gh;
	geometry(s, &gx, &gy, &gw, &gh);
	Rect c = content_rect(w, w->rect);
	*sx = (f32)gx + (x - c.x) * (c.w > 0 ? (f32)gw / c.w : 1);
	*sy = (f32)gy + (y - c.y) * (c.h > 0 ? (f32)gh / c.h : 1);
}

struct dma_buf_sync {
	u64 flags;
};
#define DMA_BUF_IOCTL_SYNC IOW('b', 0, struct dma_buf_sync)
enum { DMA_BUF_SYNC_READ = 1, DMA_BUF_SYNC_START = 0, DMA_BUF_SYNC_END = 4 };

/* The buffer's first pixel, mapping a GPU buffer on first use. */
static u8 *buffer_pixels(WlBuffer *b)
{
	if (b->pool)
		return b->pool->map ? b->pool->map + b->offset : NULL;
	if (!b->map && !b->map_failed) {
		usize size = (usize)b->offset + (usize)b->stride * (usize)b->height;
		void *m = sys_mmap(NULL, size, PROT_READ, MAP_SHARED, b->dmabuf, 0);
		if ((unsigned long)m > -4096ul) {
			b->map_failed = true;
			eprint("zdisplay: cannot read a program's GPU buffer\n");
		} else {
			b->map = m, b->map_size = size;
		}
	}
	return b->map ? b->map + b->offset : NULL;
}

/* Reading a GPU buffer: the driver finishes rendering into it and makes it
 * coherent for the processor first. */
static void buffer_access(WlBuffer *b, bool start)
{
	if (b && !b->pool && b->map) {
		struct dma_buf_sync sync = { DMA_BUF_SYNC_READ | (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) };
		sys_ioctl(b->dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
	}
}

/* The geometry's pixels in the buffer. */
static bool buffer_area(const WlSurface *s, Canvas *src, i32 *x, i32 *y, i32 *w, i32 *h)
{
	WlBuffer *b = s->buffer;
	u8 *pixels = b ? buffer_pixels(b) : NULL;
	if (!pixels)
		return false;
	i32 sw = wl_surface_width(s), sh = wl_surface_height(s);
	if (sw <= 0 || sh <= 0)
		return false;
	i32 gx, gy, gw, gh;
	geometry(s, &gx, &gy, &gw, &gh);
	f32 kx = (f32)b->width / (f32)sw, ky = (f32)b->height / (f32)sh;
	*x = CLAMP((i32)roundf((f32)gx * kx), 0, b->width - 1);
	*y = CLAMP((i32)roundf((f32)gy * ky), 0, b->height - 1);
	*w = CLAMP((i32)roundf((f32)gw * kx), 1, b->width - *x);
	*h = CLAMP((i32)roundf((f32)gh * ky), 1, b->height - *y);
	*src = canvas_wrap(pixels, b->width, b->height, b->stride / 4);
	return true;
}

/* XRGB buffers leave the alpha byte undefined: blending needs it filled in. */
static const Canvas *opaque_copy(WlSurface *s, const Canvas *src, i32 x, i32 y, i32 w, i32 h)
{
	if (!s->opaque || s->opaque->width != w || s->opaque->height != h) {
		canvas_free(s->opaque);
		s->opaque = canvas_new(w, h);
	} else if (s->opaque_commit == s->commits) {
		return s->opaque; /* already made from this commit */
	}
	s->opaque_commit = s->commits;
	for (i32 r = 0; r < h; r++) {
		const u32 *in = canvas_row(src, y + r) + x;
		u32 *out = canvas_row(s->opaque, r);
		for (i32 i = 0; i < w; i++)
			out[i] = in[i] | 0xff000000u;
	}
	return s->opaque;
}

/* One surface of a tree, its whole buffer at (fx, fy) in the frame; k maps
 * surface coordinates to frame pixels. */
static void draw_one(Canvas *frame, WlSurface *s, f32 fx, f32 fy, f32 kx, f32 ky, Rect clip, f32 opacity)
{
	WlBuffer *b = s->buffer;
	if (b && !b->pool)
		buffer_pixels(b);
	u8 *pixels = b ? buffer_pixels(b) : NULL;
	i32 sw = wl_surface_width(s), sh = wl_surface_height(s);
	if (!pixels || sw <= 0 || sh <= 0)
		return;
	buffer_access(b, true);
	Canvas src = canvas_wrap(pixels, b->width, b->height, b->stride / 4);
	const Canvas *img = b->format == 1 ? opaque_copy(s, &src, 0, 0, b->width, b->height) : &src;
	Rect dst = rect(fx, fy, (f32)sw * kx, (f32)sh * ky);
	Rect saved = frame->clip;
	frame->clip = rect_intersect(saved, clip);
	if (!rect_empty(frame->clip)) {
		if (fabsf(dst.w - (f32)b->width) < 0.5f && fabsf(dst.h - (f32)b->height) < 0.5f)
			blit(frame, img, (int)roundf(dst.x), (int)roundf(dst.y), opacity);
		else
			blit_scaled(frame, img, rect(0, 0, (f32)b->width, (f32)b->height), dst, opacity);
	}
	frame->clip = saved;
	buffer_access(b, false);
}

/* A surface and its subsurfaces, bottom first. */
static void draw_tree(Canvas *frame, WlSurface *s, f32 fx, f32 fy, f32 kx, f32 ky, Rect clip, f32 opacity, int depth)
{
	if (depth > 8)
		return;
	for (int i = 0; i < s->below && i < s->nkids; i++) {
		WlSub *k = s->kids[i];
		if (k->surface)
			draw_tree(frame, k->surface, fx + (f32)k->x * kx, fy + (f32)k->y * ky, kx, ky, clip, opacity, depth + 1);
	}
	draw_one(frame, s, fx, fy, kx, ky, clip, opacity);
	for (int i = s->below; i < s->nkids; i++) {
		WlSub *k = s->kids[i];
		if (k->surface)
			draw_tree(frame, k->surface, fx + (f32)k->x * kx, fy + (f32)k->y * ky, kx, ky, clip, opacity, depth + 1);
	}
}

static void draw_surface(Canvas *frame, Window *w, Rect pr, f32 opacity)
{
	WlSurface *s = w->wl;
	Canvas src;
	i32 x, y, bw, bh;
	if (!s || !buffer_area(s, &src, &x, &y, &bw, &bh))
		return;
	Rect dst = pr;
	if (w->states & STATE_FULLSCREEN) {
		Rect logical = content_rect(w, rect(pr.x / S.scale, pr.y / S.scale, pr.w / S.scale, pr.h / S.scale));
		if (logical.w * S.scale < pr.w - 0.5f || logical.h * S.scale < pr.h - 0.5f) {
			fill_rect(frame, pr, color_scale_alpha(COLOR_BLACK, opacity));
			dst = rect(roundf(logical.x * S.scale), roundf(logical.y * S.scale), roundf(logical.w * S.scale), roundf(logical.h * S.scale));
		}
	}
	if (s->nkids) {
		/* a window made of several surfaces: all of them, cut to the window */
		i32 gx, gy, gw, gh;
		geometry(s, &gx, &gy, &gw, &gh);
		f32 kx = dst.w / (f32)MAX(gw, 1), ky = dst.h / (f32)MAX(gh, 1);
		draw_tree(frame, s, dst.x - (f32)gx * kx, dst.y - (f32)gy * ky, kx, ky, dst, opacity, 0);
		return;
	}
	bool opaque = s->buffer->format == 1;
	bool exact = fabsf(dst.w - (f32)bw) < 0.5f && fabsf(dst.h - (f32)bh) < 0.5f;
	if (exact && opaque && opacity >= 0.999f) {
		/* the common case for games: a straight copy, alpha filled in */
		Rect c = rect_intersect(frame->clip, rect(dst.x, dst.y, (f32)bw, (f32)bh));
		if (rect_empty(c))
			return;
		int x0 = (int)c.x, y0 = (int)c.y, x1 = (int)(c.x + c.w), y1 = (int)(c.y + c.h), dx = (int)dst.x, dy = (int)dst.y;
		for (int r = y0; r < y1; r++) {
			const u32 *in = canvas_row(&src, y + r - dy) + x + (x0 - dx);
			u32 *out = canvas_row(frame, r) + x0;
			for (int i = 0; i < x1 - x0; i++)
				out[i] = in[i] | 0xff000000u;
		}
		return;
	}
	const Canvas *img = &src;
	Rect sr = rect((f32)x, (f32)y, (f32)bw, (f32)bh);
	if (opaque) {
		img = opaque_copy(s, &src, x, y, bw, bh);
		sr = rect(0, 0, (f32)bw, (f32)bh);
	}
	if (exact) {
		Canvas part = canvas_wrap(img->pixels + (usize)sr.y * (usize)img->stride + (usize)sr.x, bw, bh, img->stride);
		blit(frame, &part, (int)dst.x, (int)dst.y, opacity);
	} else {
		blit_scaled(frame, img, sr, dst, opacity);
	}
}

void wl_window_draw(Canvas *frame, Window *w, Rect pr, f32 opacity)
{
	WlSurface *s = w->wl;
	WlBuffer *b = s ? s->buffer : NULL;
	if (b && !b->pool)
		buffer_pixels(b);
	buffer_access(b, true);
	draw_surface(frame, w, pr, opacity);
	buffer_access(b, false);
}

/* A copy of the window's picture, for the closing animation. */
static Canvas *window_snapshot(Window *w)
{
	WlSurface *s = w->wl;
	Canvas src;
	i32 x, y, bw, bh;
	if (!s || !buffer_area(s, &src, &x, &y, &bw, &bh))
		return NULL;
	Canvas *c = canvas_new(bw, bh);
	bool opaque = s->buffer->format == 1;
	buffer_access(s->buffer, true);
	for (i32 r = 0; r < bh; r++) {
		const u32 *in = canvas_row(&src, y + r) + x;
		u32 *out = canvas_row(c, r);
		for (i32 i = 0; i < bw; i++)
			out[i] = opaque ? in[i] | 0xff000000u : in[i];
	}
	buffer_access(s->buffer, false);
	return c;
}

/* ---- Keyboard focus ------------------------------------------------------------------------------ */

static bool is_modifier(u32 code)
{
	return code == 29 || code == 97 || code == 42 || code == 54 || code == 56 || code == 100 || code == 125 || code == 126;
}

static u32 xkb_mods_depressed(void)
{
	u32 m = 0;
	for (int i = 0; i < W.nkeys_down; i++) {
		switch (W.keys_down[i]) {
		case 42: case 54: m |= 1; break;      /* Shift */
		case 29: case 97: m |= 4; break;      /* Control */
		case 56: m |= 8; break;               /* Alt (Mod1) */
		case 125: case 126: m |= 64; break;   /* Super (Mod4) */
		case 100: m |= 128; break;            /* AltGr (Mod5) */
		}
	}
	return m;
}

static u32 xkb_mods_locked(void)
{
	return ((S.mods & MOD_CAPS) ? 2 : 0) | 16; /* Caps Lock as zdisplay has it, Num Lock on */
}

static void send_modifiers(WlClient *c)
{
	u32 serial = next_serial();
	for (int k = 0; k < c->nkeyboards; k++)
		event(c->keyboards[k], WL_KEYBOARD_EV_MODIFIERS, "uuuuu", serial, xkb_mods_depressed(), 0, xkb_mods_locked(), 0);
}

static void constraints_update(void);

static void kb_set_focus(WlSurface *s)
{
	if (W.kb_focus == s)
		return;
	WlSurface *old = W.kb_focus;
	if (old) {
		u32 serial = next_serial();
		for (int k = 0; k < old->client->nkeyboards; k++)
			event(old->client->keyboards[k], WL_KEYBOARD_EV_LEAVE, "uo", serial, old->res->id);
	}
	W.kb_focus = s;
	W.nkeys_down = 0;
	if (s) {
		u32 serial = next_serial();
		for (int k = 0; k < s->client->nkeyboards; k++)
			event(s->client->keyboards[k], WL_KEYBOARD_EV_ENTER, "uoa", serial, s->res->id, W.keys_down, 0);
		send_modifiers(s->client);
	}
	constraints_update();
}

/* The keyboard goes to the innermost open menu, else the focused window. */
static void kb_refocus(void)
{
	WlSurface *target = NULL;
	if (W.ngrabs && W.grabs[W.ngrabs - 1]->xs)
		target = W.grabs[W.ngrabs - 1]->xs->surface;
	if (!target && S.focus && S.focus->wl)
		target = S.focus->wl;
	kb_set_focus(target);
}

void wl_key_event(Window *w, u32 code, u32 state)
{
	if (state == KEY_REPEATED)
		return; /* clients repeat keys themselves (repeat_info) */
	WlSurface *s = w->wl;
	if (!s)
		return;
	if (W.kb_focus && W.kb_focus->client == s->client)
		s = W.kb_focus; /* an open menu */
	if (W.kb_focus != s)
		kb_set_focus(s);
	int at = -1;
	for (int i = 0; i < W.nkeys_down; i++)
		if (W.keys_down[i] == code)
			at = i;
	if (state == KEY_PRESSED) {
		if (at < 0 && W.nkeys_down < (int)ARRAY_LEN(W.keys_down))
			W.keys_down[W.nkeys_down++] = code;
	} else {
		if (at < 0)
			return; /* the press went to the shell */
		W.keys_down[at] = W.keys_down[--W.nkeys_down];
	}
	u32 serial = next_serial(), t = now_ms();
	for (int k = 0; k < s->client->nkeyboards; k++)
		event(s->client->keyboards[k], WL_KEYBOARD_EV_KEY, "uuuu", serial, t, code, state == KEY_PRESSED ? 1 : 0);
	if (is_modifier(code) || code == 58 /* Caps Lock */)
		send_modifiers(s->client);
}

/* ---- Pointer ----------------------------------------------------------------------------------------- */

static void pointer_frame(WlClient *c)
{
	for (int i = 0; i < c->npointers; i++)
		if (c->pointers[i]->version >= 5)
			event(c->pointers[i], WL_POINTER_EV_FRAME, "");
}

static void ptr_leave(void)
{
	WlSurface *s = W.ptr_focus;
	if (!s)
		return;
	W.ptr_focus = NULL;
	u32 serial = next_serial();
	for (int i = 0; i < s->client->npointers; i++)
		event(s->client->pointers[i], WL_POINTER_EV_LEAVE, "uo", serial, s->res->id);
	pointer_frame(s->client);
	constraints_update();
}

static void ptr_enter(WlSurface *s, f32 sx, f32 sy)
{
	if (W.ptr_focus == s)
		return;
	ptr_leave();
	W.ptr_focus = s;
	u32 serial = next_serial();
	for (int i = 0; i < s->client->npointers; i++)
		event(s->client->pointers[i], WL_POINTER_EV_ENTER, "uoff", serial, s->res->id, FIX(sx), FIX(sy));
	pointer_frame(s->client);
	constraints_update();
}

static bool locked(void);

static u32 linux_button(u32 b)
{
	switch (b) {
	case BUTTON_RIGHT: return 0x111;
	case BUTTON_MIDDLE: return 0x112;
	case BUTTON_BACK: return 0x113;
	case BUTTON_FORWARD: return 0x114;
	default: return 0x110;
	}
}

/* The surface of a tree under (x, y), in s's coordinates (topmost first,
 * where its input region says it takes input); its own coordinates in lx, ly. */
static WlSurface *hit_surface(WlSurface *s, f32 x, f32 y, f32 *lx, f32 *ly, int depth)
{
	if (depth > 8)
		return NULL;
	for (int i = s->nkids - 1; i >= 0; i--) {
		if (i == s->below - 1 && x >= 0 && y >= 0 && x < (f32)wl_surface_width(s) && y < (f32)wl_surface_height(s) &&
		    region_has(s->input, (i32)floorf(x), (i32)floorf(y))) {
			*lx = x, *ly = y;
			return s;
		}
		WlSub *k = s->kids[i];
		WlSurface *hit = k->surface && k->surface->buffer
					 ? hit_surface(k->surface, x - (f32)k->x, y - (f32)k->y, lx, ly, depth + 1) : NULL;
		if (hit)
			return hit;
	}
	if (s->below <= 0 && x >= 0 && y >= 0 && x < (f32)wl_surface_width(s) && y < (f32)wl_surface_height(s) &&
	    region_has(s->input, (i32)floorf(x), (i32)floorf(y))) {
		*lx = x, *ly = y;
		return s;
	}
	return NULL;
}

/* The surface of window w that gets the pointer at the window coordinates
 * (sx, sy): the one under it, or the one buttons went down on. */
static WlSurface *pointer_target(Window *w, f32 sx, f32 sy, f32 *lx, f32 *ly)
{
	WlSurface *root = w->wl;
	if (W.buttons_down > 0 && W.ptr_focus && surface_root(W.ptr_focus) == root) {
		i32 ox, oy;
		surface_offset(W.ptr_focus, &ox, &oy);
		*lx = sx - (f32)ox, *ly = sy - (f32)oy;
		return W.ptr_focus;
	}
	WlSurface *s = root->nkids ? hit_surface(root, sx, sy, lx, ly, 0) : NULL;
	if (!s) {
		s = root;
		*lx = sx, *ly = sy;
	}
	return s;
}

void wl_pointer_event(Window *w, u32 kind, u32 button)
{
	if (!w->wl)
		return;
	f32 wx, wy, sx, sy;
	to_surface(w, S.px, S.py, &wx, &wy);
	if (kind == POINTER_LEAVE) {
		if (W.ptr_focus && surface_root(W.ptr_focus) == w->wl)
			ptr_leave();
		W.buttons_down = 0;
		return;
	}
	WlSurface *s = pointer_target(w, wx, wy, &sx, &sy);
	if (kind == POINTER_DOWN)
		W.buttons_down++;
	else if (kind == POINTER_UP && W.buttons_down > 0)
		W.buttons_down--;
	if (W.ptr_focus != s) {
		ptr_enter(s, sx, sy);
		if (kind == POINTER_ENTER)
			return;
	}
	WlClient *c = s->client;
	u32 t = now_ms();
	if (kind == POINTER_MOTION && !locked()) {
		for (int i = 0; i < c->npointers; i++)
			event(c->pointers[i], WL_POINTER_EV_MOTION, "uff", t, FIX(sx), FIX(sy));
	} else if (kind == POINTER_DOWN || kind == POINTER_UP) {
		u32 serial = next_serial();
		for (int i = 0; i < c->npointers; i++)
			event(c->pointers[i], WL_POINTER_EV_BUTTON, "uuuu", serial, t, linux_button(button), kind == POINTER_DOWN ? 1 : 0);
	} else {
		return;
	}
	pointer_frame(c);
}

void wl_scroll_event(Window *w, f32 dx, f32 dy, u32 source, u32 phase)
{
	if (!w->wl)
		return;
	f32 wx, wy, sx, sy;
	to_surface(w, S.px, S.py, &wx, &wy);
	WlSurface *s = pointer_target(w, wx, wy, &sx, &sy);
	if (W.ptr_focus != s)
		ptr_enter(s, sx, sy);
	WlClient *c = s->client;
	u32 t = now_ms();
	/* zdisplay's sign is the wire's (positive scrolls down or right), natural scrolling already applied */
	f32 v[2] = { dy, dx };
	bool wheel = source == SCROLL_WHEEL;
	for (int i = 0; i < c->npointers; i++) {
		WlResource *p = c->pointers[i];
		if (p->version >= 5)
			event(p, WL_POINTER_EV_AXIS_SOURCE, "u", wheel ? 0 : 1);
		for (u32 axis = 0; axis < 2; axis++) {
			if (v[axis] == 0)
				continue;
			if (wheel) {
				f32 steps = v[axis] / 48;
				if (p->version >= 8)
					event(p, WL_POINTER_EV_AXIS_VALUE120, "ui", axis, (u32)(i32)roundf(steps * 120));
				else if (p->version >= 5)
					event(p, WL_POINTER_EV_AXIS_DISCRETE, "ui", axis, (u32)(i32)roundf(steps));
				event(p, WL_POINTER_EV_AXIS, "uuf", t, axis, FIX(steps * 15));
			} else {
				event(p, WL_POINTER_EV_AXIS, "uuf", t, axis, FIX(v[axis]));
			}
		}
		if (phase == SCROLL_PHASE_END && p->version >= 5) {
			event(p, WL_POINTER_EV_AXIS_STOP, "uu", t, 0);
			event(p, WL_POINTER_EV_AXIS_STOP, "uu", t, 1);
		}
	}
	pointer_frame(c);
}

/* ---- Cursors ------------------------------------------------------------------------------------------ */

/* Turns a cursor surface into an image at the screen's scale. */
static void cursor_image(WlSurface *cs)
{
	canvas_free(W.cursor);
	W.cursor = NULL;
	Canvas src;
	i32 x, y, bw, bh;
	if (!cs || !buffer_area(cs, &src, &x, &y, &bw, &bh))
		return;
	i32 sw = wl_surface_width(cs), sh = wl_surface_height(cs);
	int w = MAX(1, (int)roundf((f32)sw * S.scale)), h = MAX(1, (int)roundf((f32)sh * S.scale));
	if (w > 256 || h > 256)
		return;
	Canvas *full = canvas_new(bw, bh);
	for (i32 r = 0; r < bh; r++) {
		const u32 *in = canvas_row(&src, y + r) + x;
		u32 *out = canvas_row(full, r);
		for (i32 i = 0; i < bw; i++)
			out[i] = cs->buffer->format == 1 ? in[i] | 0xff000000u : in[i];
	}
	if (w == bw && h == bh) {
		W.cursor = full;
	} else {
		W.cursor = canvas_new(w, h);
		blit_scaled(W.cursor, full, rect(0, 0, (f32)bw, (f32)bh), rect(0, 0, (f32)w, (f32)h), 1);
		canvas_free(full);
	}
}

static void cursor_apply(void)
{
	WlSurface *s = W.ptr_focus;
	Window *w = s ? surface_window(s) : NULL;
	if (!w)
		return;
	u32 shape = CURSOR_HIDDEN;
	if (W.cursor_surface && W.cursor) {
		shape = CURSOR_CUSTOM;
		S.cursor_custom = W.cursor;
		S.cursor_custom_hx = (int)roundf((f32)W.cursor_surface->cursor_hx * S.scale);
		S.cursor_custom_hy = (int)roundf((f32)W.cursor_surface->cursor_hy * S.scale);
		S.cursor_custom_serial++;
	}
	w->cursor = shape;
	if (S.pointer_window == w && S.grab == GRAB_NONE) {
		S.cursor = shape;
		cursor_update();
	}
}

static void cursor_set_for(WlClient *c, WlSurface *cs, i32 hx, i32 hy)
{
	/* only the program under the pointer picks the cursor */
	if (!W.ptr_focus || W.ptr_focus->client != c)
		return;
	W.cursor_surface = cs;
	if (cs) {
		cs->cursor_hx = hx, cs->cursor_hy = hy;
		cursor_image(cs);
	}
	cursor_apply();
}

static void cursor_commit(WlSurface *s)
{
	if (W.cursor_surface != s || !W.ptr_focus || W.ptr_focus->client != s->client)
		return;
	cursor_image(s);
	cursor_apply();
}

/* ---- xdg-shell: positioners ------------------------------------------------------------------------- */

enum { XDG_WM_BASE_ERROR_ROLE = 0, XDG_WM_BASE_ERROR_INVALID_POSITIONER = 5 };
enum { XDG_SURFACE_ERROR_NOT_CONSTRUCTED = 1, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED = 2, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER = 3,
       XDG_SURFACE_ERROR_INVALID_SIZE = 5 };

static void positioner_destroyed(WlResource *r)
{
	mem_free(r->data);
}

static void positioner_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlPositioner *p = r->data;
	switch (op) {
	case XDG_POSITIONER_DESTROY:
		res_destroy(r);
		break;
	case XDG_POSITIONER_SET_SIZE:
		if ((i32)a[0].u <= 0 || (i32)a[1].u <= 0) {
			protocol_error(r->client, r->id, 0, "positioner size must be positive");
			return;
		}
		p->w = (i32)a[0].u, p->h = (i32)a[1].u;
		break;
	case XDG_POSITIONER_SET_ANCHOR_RECT:
		if ((i32)a[2].u < 0 || (i32)a[3].u < 0) {
			protocol_error(r->client, r->id, 0, "negative anchor rectangle");
			return;
		}
		p->ax = (i32)a[0].u, p->ay = (i32)a[1].u, p->aw = (i32)a[2].u, p->ah = (i32)a[3].u;
		break;
	case XDG_POSITIONER_SET_ANCHOR:
		p->anchor = a[0].u;
		break;
	case XDG_POSITIONER_SET_GRAVITY:
		p->gravity = a[0].u;
		break;
	case XDG_POSITIONER_SET_CONSTRAINT_ADJUSTMENT:
		p->adjust = a[0].u;
		break;
	case XDG_POSITIONER_SET_OFFSET:
		p->dx = (i32)a[0].u, p->dy = (i32)a[1].u;
		break;
	}
	/* reactive, parent size and parent configure: popups are placed once */
}

/* Anchor and gravity values: none, top, bottom, left, right, top-left,
 * bottom-left, top-right, bottom-right. -1, 0 or 1 along each axis. */
static int dir_x(u32 v)
{
	return (v == 3 || v == 5 || v == 6) ? -1 : (v == 4 || v == 7 || v == 8) ? 1 : 0;
}

static int dir_y(u32 v)
{
	return (v == 1 || v == 5 || v == 7) ? -1 : (v == 2 || v == 6 || v == 8) ? 1 : 0;
}

static i32 place_axis(i32 a0, i32 alen, int anchor, int gravity, i32 size, i32 offset)
{
	i32 at = anchor < 0 ? a0 : anchor > 0 ? a0 + alen : a0 + alen / 2;
	i32 x = gravity < 0 ? at - size : gravity > 0 ? at : at - size / 2;
	return x + offset;
}

/* Places a popup next to its anchor, relative to the parent's window
 * geometry; flips or slides it to keep it on screen when allowed. */
static void popup_place(WlPopup *p, f32 ox, f32 oy)
{
	WlPositioner *q = &p->pos;
	Rect wa = rect(0, 0, S.sw, S.sh);
	for (int axis = 0; axis < 2; axis++) {
		bool hx = axis == 0;
		i32 a0 = hx ? q->ax : q->ay, alen = hx ? q->aw : q->ah, size = hx ? q->w : q->h, off = hx ? q->dx : q->dy;
		int an = hx ? dir_x(q->anchor) : dir_y(q->anchor), gr = hx ? dir_x(q->gravity) : dir_y(q->gravity);
		f32 o = hx ? ox : oy, lo = hx ? wa.x : wa.y, hi = hx ? wa.x + wa.w : wa.y + wa.h;
		i32 v = place_axis(a0, alen, an, gr, size, off);
		bool out = o + (f32)v < lo || o + (f32)(v + size) > hi;
		u32 flip = hx ? 4 : 8, slide = hx ? 1 : 2;
		if (out && (q->adjust & flip)) {
			i32 f = place_axis(a0, alen, -an, -gr, size, -off);
			if (o + (f32)f >= lo && o + (f32)(f + size) <= hi)
				v = f, out = false;
		}
		if (out && (q->adjust & slide)) {
			if (o + (f32)(v + size) > hi)
				v = (i32)(hi - o) - size;
			if (o + (f32)v < lo)
				v = (i32)(lo - o);
		}
		if (hx)
			p->x = v;
		else
			p->y = v;
	}
}

/* ---- xdg-shell: toplevels ------------------------------------------------------------------------------ */

enum { XS_MAXIMIZED = 1, XS_FULLSCREEN, XS_RESIZING, XS_ACTIVATED, XS_TILED_LEFT, XS_TILED_RIGHT, XS_TILED_TOP, XS_TILED_BOTTOM };

static void xdg_configure(WlXdgSurface *xs)
{
	xs->serial = next_serial();
	event(xs->res, XDG_SURFACE_EV_CONFIGURE, "u", xs->serial);
}

static void toplevel_configure(WlToplevel *t, bool force)
{
	WlXdgSurface *xs = t->xs;
	if (!xs)
		return;
	Window *w = xs->surface ? xs->surface->win : NULL;
	i32 width = 0, height = 0;
	u32 states = 0;
	if (w) {
		width = (i32)roundf(w->rect.w), height = (i32)roundf(w->rect.h);
		if (w->states & STATE_MAXIMIZED)
			states |= 1u << XS_MAXIMIZED;
		if (w->states & STATE_FULLSCREEN)
			states |= 1u << XS_FULLSCREEN;
		if (w->states & (STATE_TILED_LEFT | STATE_TILED_RIGHT))
			states |= 1u << (w->states & STATE_TILED_LEFT ? XS_TILED_LEFT : XS_TILED_RIGHT) | 1u << XS_TILED_TOP | 1u << XS_TILED_BOTTOM;
		if (w == S.focus)
			states |= 1u << XS_ACTIVATED;
		if (S.grab == GRAB_RESIZE && S.grab_window == w)
			states |= 1u << XS_RESIZING;
	} else if (t->want_fullscreen) {
		width = (i32)S.sw, height = (i32)S.sh;
		states |= 1u << XS_FULLSCREEN | 1u << XS_ACTIVATED;
	} else if (t->want_maximized) {
		Rect wa = work_area();
		width = (i32)wa.w, height = (i32)wa.h;
		states |= 1u << XS_MAXIMIZED | 1u << XS_ACTIVATED;
	}
	if (!force && t->sent_any && width == t->sent_w && height == t->sent_h && states == t->sent_states)
		return;
	t->sent_any = true, t->sent_w = width, t->sent_h = height, t->sent_states = states;
	u32 list[8];
	u32 n = 0;
	for (u32 st = XS_MAXIMIZED; st <= XS_TILED_BOTTOM; st++)
		if (states & (1u << st))
			list[n++] = st;
	event(t->res, XDG_TOPLEVEL_EV_CONFIGURE, "iia", (u32)width, (u32)height, list, n * 4);
	xdg_configure(xs);
}

void wl_window_configure(Window *w)
{
	WlSurface *s = w->wl;
	if (s && s->xdg && s->xdg->toplevel)
		toplevel_configure(s->xdg->toplevel, false);
}

void wl_window_close(Window *w)
{
	WlSurface *s = w->wl;
	if (!s || !s->xdg)
		return;
	if (s->xdg->toplevel)
		event(s->xdg->toplevel->res, XDG_TOPLEVEL_EV_CLOSE, "");
}

static void surface_unmap(WlSurface *s)
{
	Window *w = s->win;
	if (!w)
		return;
	if (W.ptr_focus && surface_root(W.ptr_focus) == s)
		ptr_leave();
	window_destroy(w, true); /* calls wl_window_gone */
	if (s->xdg) {
		s->xdg->initial_sent = s->xdg->configured = false;
		if (s->xdg->toplevel)
			s->xdg->toplevel->sent_any = false;
	}
}

static Window *parent_window(WlXdgSurface *xs)
{
	return xs && xs->surface ? xs->surface->win : NULL;
}

static void apply_geometry(WlXdgSurface *xs)
{
	if (!xs->pending_geometry)
		return;
	xs->gx = xs->pgx, xs->gy = xs->pgy, xs->gw = xs->pgw, xs->gh = xs->pgh;
	xs->pending_geometry = false;
}

static void toplevel_commit(WlSurface *s)
{
	WlXdgSurface *xs = s->xdg;
	WlToplevel *t = xs ? xs->toplevel : NULL;
	if (!t)
		return;
	apply_geometry(xs);
	if (!xs->initial_sent) {
		if (s->buffer) {
			protocol_error(s->client, xs->res->id, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "buffer before the first configure");
			return;
		}
		xs->initial_sent = true;
		if (t->res->version >= 4) {
			Rect wa = work_area();
			event(t->res, XDG_TOPLEVEL_EV_CONFIGURE_BOUNDS, "ii", (u32)(i32)wa.w, (u32)(i32)wa.h);
		}
		if (t->res->version >= 5) {
			u32 caps[] = { 2 /* maximize */, 3 /* fullscreen */, 4 /* minimize */ };
			event(t->res, XDG_TOPLEVEL_EV_WM_CAPABILITIES, "a", caps, (u32)sizeof caps);
		}
		toplevel_configure(t, true);
		return;
	}
	if (!s->buffer) {
		surface_unmap(s);
		return;
	}
	if (!xs->configured) {
		protocol_error(s->client, xs->res->id, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "buffer before acking a configure");
		return;
	}
	i32 gx, gy, gw, gh;
	geometry(s, &gx, &gy, &gw, &gh);
	if (gw <= 0 || gh <= 0)
		return;
	Window *w = s->win;
	if (!w) {
		u32 flags = WIN_RESIZABLE;
		if (t->max_w > 0 && t->max_w == t->min_w && t->max_h > 0 && t->max_h == t->min_h)
			flags = 0;
		Window *parent = t->parent ? parent_window(t->parent->xs) : NULL;
		if (parent)
			flags |= WIN_DIALOG;
		const char *app = s->client->app_id[0] ? s->client->app_id : t->app_id;
		w = window_create_wl(s, t->title, app, (f32)gw, (f32)gh, flags, parent, 0, 0);
		if (!w)
			return;
		w->min_w = (u32)MAX(t->min_w, 32), w->min_h = (u32)MAX(t->min_h, 32);
		if (t->want_fullscreen)
			window_fullscreen(w, true);
		else if (t->want_maximized)
			window_maximize(w, true);
		toplevel_configure(t, false);
		if (S.splash_wait_window)
			S.splash_wait_window = false;
		return;
	}
	/* the client picked its own size (a terminal snapping to whole cells, a
	 * fixed-size game): the window follows, unless a newer size is on its way */
	if (xs->acked == xs->serial && !(w->states & STATE_FULLSCREEN) &&
	    (fabsf((f32)gw - w->rect.w) >= 1 || fabsf((f32)gh - w->rect.h) >= 1)) {
		window_damage(w);
		if (S.grab == GRAB_RESIZE && S.grab_window == w) {
			if (S.grab_edges & EDGE_LEFT)
				w->rect.x += w->rect.w - (f32)gw;
			if (S.grab_edges & EDGE_TOP)
				w->rect.y += w->rect.h - (f32)gh;
		}
		w->rect.w = (f32)gw, w->rect.h = (f32)gh;
	}
	window_damage(w);
}

static void toplevel_destroyed(WlResource *r)
{
	WlToplevel *t = r->data;
	WlXdgSurface *xs = t->xs;
	if (xs) {
		if (xs->surface)
			surface_unmap(xs->surface);
		xs->toplevel = NULL;
		xs->initial_sent = xs->configured = false;
	}
	for (int i = 0; i < W.nxdgs; i++)
		if (W.xdgs[i]->toplevel && W.xdgs[i]->toplevel->parent == t)
			W.xdgs[i]->toplevel->parent = NULL;
	mem_free(t);
}

static void toplevel_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlToplevel *t = r->data;
	Window *w = t->xs ? parent_window(t->xs) : NULL;
	switch (op) {
	case XDG_TOPLEVEL_DESTROY:
		res_destroy(r);
		return;
	case XDG_TOPLEVEL_SET_PARENT:
		t->parent = a[0].obj ? a[0].obj->data : NULL;
		if (t->parent == t)
			t->parent = NULL;
		return;
	case XDG_TOPLEVEL_SET_TITLE:
		str_copy(t->title, a[0].s, sizeof t->title);
		if (w) {
			str_copy(w->title, t->title, sizeof w->title);
			shell_mark_dirty();
		}
		return;
	case XDG_TOPLEVEL_SET_APP_ID:
		str_copy(t->app_id, a[0].s, sizeof t->app_id);
		if (w && !r->client->app_id[0]) {
			str_copy(w->app_id, t->app_id, sizeof w->app_id);
			shell_mark_dirty();
		}
		return;
	case XDG_TOPLEVEL_MOVE:
	case XDG_TOPLEVEL_RESIZE:
		if (!w || !S.buttons || S.grab != GRAB_NONE)
			return;
		/* the window manager takes the pointer until the button comes up */
		if (W.ptr_focus && surface_root(W.ptr_focus) == w->wl)
			ptr_leave();
		W.buttons_down = 0;
		S.pointer_window = NULL;
		if (op == XDG_TOPLEVEL_MOVE)
			wm_begin_move(w);
		else
			wm_begin_resize(w, a[2].u & (EDGE_TOP | EDGE_BOTTOM | EDGE_LEFT | EDGE_RIGHT));
		toplevel_configure(t, false);
		return;
	case XDG_TOPLEVEL_SET_MAX_SIZE:
		t->max_w = (i32)a[0].u, t->max_h = (i32)a[1].u;
		return;
	case XDG_TOPLEVEL_SET_MIN_SIZE:
		t->min_w = (i32)a[0].u, t->min_h = (i32)a[1].u;
		if (w)
			w->min_w = (u32)MAX(t->min_w, 32), w->min_h = (u32)MAX(t->min_h, 32);
		return;
	case XDG_TOPLEVEL_SET_MAXIMIZED:
	case XDG_TOPLEVEL_UNSET_MAXIMIZED:
		t->want_maximized = op == XDG_TOPLEVEL_SET_MAXIMIZED;
		if (w)
			window_maximize(w, t->want_maximized);
		if (t->xs && t->xs->initial_sent)
			toplevel_configure(t, true); /* answered even when nothing changed */
		return;
	case XDG_TOPLEVEL_SET_FULLSCREEN:
	case XDG_TOPLEVEL_UNSET_FULLSCREEN:
		t->want_fullscreen = op == XDG_TOPLEVEL_SET_FULLSCREEN;
		if (w)
			window_fullscreen(w, t->want_fullscreen);
		if (t->xs && t->xs->initial_sent)
			toplevel_configure(t, true);
		return;
	case XDG_TOPLEVEL_SET_MINIMIZED:
		if (w)
			window_minimize(w, true);
		return;
	}
	/* show_window_menu: no window menu yet */
}

/* ---- xdg-shell: popups ---------------------------------------------------------------------------------- */

static void popup_dismiss(WlPopup *p)
{
	if (p->dismissed)
		return;
	p->dismissed = true;
	for (int i = 0; i < W.ngrabs; i++)
		if (W.grabs[i] == p) {
			memmove(W.grabs + i, W.grabs + i + 1, sizeof *W.grabs * (usize)(W.ngrabs - i - 1));
			W.ngrabs--;
			break;
		}
	if (p->res)
		event(p->res, XDG_POPUP_EV_POPUP_DONE, "");
	p->grab = false;
	if (p->xs && p->xs->surface)
		surface_unmap(p->xs->surface);
	kb_refocus();
}

static void popup_commit(WlSurface *s)
{
	WlXdgSurface *xs = s->xdg;
	WlPopup *p = xs ? xs->popup : NULL;
	if (!p)
		return;
	apply_geometry(xs);
	Window *pw = parent_window(p->parent);
	if (!xs->initial_sent) {
		if (s->buffer) {
			protocol_error(s->client, xs->res->id, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "buffer before the first configure");
			return;
		}
		xs->initial_sent = true;
		if (!pw || p->dismissed) {
			popup_dismiss(p);
			return;
		}
		popup_place(p, pw->rect.x, pw->rect.y);
		event(p->res, XDG_POPUP_EV_CONFIGURE, "iiii", (u32)p->x, (u32)p->y, (u32)p->pos.w, (u32)p->pos.h);
		xdg_configure(xs);
		return;
	}
	if (!s->buffer || p->dismissed || !pw) {
		surface_unmap(s);
		return;
	}
	if (!xs->configured)
		return;
	i32 gx, gy, gw, gh;
	geometry(s, &gx, &gy, &gw, &gh);
	Window *w = s->win;
	if (!w) {
		w = window_create_wl(s, "", pw->app_id, (f32)gw, (f32)gh, WIN_POPUP, pw, (f32)p->x, (f32)p->y);
		if (!w)
			return;
		if (p->grab)
			kb_refocus();
		return;
	}
	if (fabsf((f32)gw - w->rect.w) >= 1 || fabsf((f32)gh - w->rect.h) >= 1) {
		window_damage(w);
		w->rect.w = (f32)gw, w->rect.h = (f32)gh;
	}
	window_damage(w);
}

static void popup_destroyed(WlResource *r)
{
	WlPopup *p = r->data;
	p->res = NULL;
	for (int i = 0; i < W.ngrabs; i++)
		if (W.grabs[i] == p) {
			memmove(W.grabs + i, W.grabs + i + 1, sizeof *W.grabs * (usize)(W.ngrabs - i - 1));
			W.ngrabs--;
			break;
		}
	if (p->xs) {
		if (p->xs->surface)
			surface_unmap(p->xs->surface);
		p->xs->popup = NULL;
		p->xs->initial_sent = p->xs->configured = false;
	}
	mem_free(p);
	kb_refocus();
}

static void popup_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlPopup *p = r->data;
	if (op == XDG_POPUP_DESTROY) {
		res_destroy(r);
	} else if (op == XDG_POPUP_GRAB) {
		/* a menu: only on top of the window in front or another open menu */
		WlXdgSurface *parent = p->parent;
		bool ok = parent && (parent->toplevel || (parent->popup && parent->popup->grab));
		if (!ok || p->grab || (p->xs && p->xs->initial_sent && p->xs->surface && p->xs->surface->win)) {
			popup_dismiss(p);
			return;
		}
		p->grab = true;
		if (W.ngrabs < (int)ARRAY_LEN(W.grabs))
			W.grabs[W.ngrabs++] = p;
	} else if (op == XDG_POPUP_REPOSITION) {
		WlPositioner *q = a[0].obj->data;
		Window *pw = parent_window(p->parent);
		p->pos = *q;
		if (!pw || !p->xs)
			return;
		popup_place(p, pw->rect.x, pw->rect.y);
		event(r, XDG_POPUP_EV_REPOSITIONED, "u", a[1].u);
		event(r, XDG_POPUP_EV_CONFIGURE, "iiii", (u32)p->x, (u32)p->y, (u32)p->pos.w, (u32)p->pos.h);
		xdg_configure(p->xs);
		Window *w = p->xs->surface ? p->xs->surface->win : NULL;
		if (w) {
			window_damage(w);
			w->rect.x = pw->rect.x + (f32)p->x, w->rect.y = pw->rect.y + (f32)p->y;
			window_damage(w);
		}
	}
}

/* A click: open menus close unless it landed on one of them. */
void wl_pointer_press(Window *w)
{
	int keep = -1;
	for (int i = 0; i < W.ngrabs; i++) {
		WlPopup *p = W.grabs[i];
		if (w && p->xs && p->xs->surface && p->xs->surface->win == w)
			keep = i;
	}
	while (W.ngrabs > keep + 1)
		popup_dismiss(W.grabs[W.ngrabs - 1]);
}

/* ---- xdg-shell: xdg_surface and xdg_wm_base -------------------------------------------------------------- */

static void xdg_surface_destroyed(WlResource *r)
{
	WlXdgSurface *xs = r->data;
	if (xs->toplevel)
		xs->toplevel->xs = NULL;
	if (xs->popup)
		xs->popup->xs = NULL;
	if (xs->surface) {
		surface_unmap(xs->surface);
		xs->surface->xdg = NULL;
	}
	for (int i = 0; i < W.nxdgs; i++) {
		WlXdgSurface *o = W.xdgs[i];
		if (o == xs) {
			W.xdgs[i--] = W.xdgs[--W.nxdgs];
			continue;
		}
		if (o->popup && o->popup->parent == xs) {
			o->popup->parent = NULL;
			popup_dismiss(o->popup);
		}
	}
	mem_free(xs);
}

static void xdg_surface_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlXdgSurface *xs = r->data;
	WlSurface *s = xs->surface;
	switch (op) {
	case XDG_SURFACE_DESTROY:
		res_destroy(r);
		break;
	case XDG_SURFACE_GET_TOPLEVEL:
	case XDG_SURFACE_GET_POPUP: {
		int role = op == XDG_SURFACE_GET_TOPLEVEL ? ROLE_TOPLEVEL : ROLE_POPUP;
		if (!s || xs->toplevel || xs->popup) {
			protocol_error(r->client, r->id, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "xdg_surface already has a role object");
			return;
		}
		if (s->role != ROLE_NONE && s->role != role) {
			protocol_error(r->client, r->id, XDG_WM_BASE_ERROR_ROLE, "surface already has another role");
			return;
		}
		if (role == ROLE_TOPLEVEL) {
			WlToplevel *t = NEW(WlToplevel);
			t->xs = xs;
			t->res = res_new(r->client, a[0].u, &xdg_toplevel_interface, r->version, toplevel_dispatch, t);
			if (!t->res) {
				mem_free(t);
				return;
			}
			t->res->destroy = toplevel_destroyed;
			xs->toplevel = t;
		} else {
			WlPositioner *q = a[2].obj->data;
			if (q->w <= 0) {
				protocol_error(r->client, r->id, XDG_WM_BASE_ERROR_INVALID_POSITIONER, "positioner without a size");
				return;
			}
			WlPopup *p = NEW(WlPopup);
			p->xs = xs;
			p->parent = a[1].obj ? a[1].obj->data : NULL;
			p->pos = *q;
			p->res = res_new(r->client, a[0].u, &xdg_popup_interface, r->version, popup_dispatch, p);
			if (!p->res) {
				mem_free(p);
				return;
			}
			p->res->destroy = popup_destroyed;
			xs->popup = p;
		}
		s->role = role;
		break;
	}
	case XDG_SURFACE_SET_WINDOW_GEOMETRY:
		if ((i32)a[2].u <= 0 || (i32)a[3].u <= 0) {
			protocol_error(r->client, r->id, XDG_SURFACE_ERROR_INVALID_SIZE, "window geometry must have a size");
			return;
		}
		xs->pgx = (i32)a[0].u, xs->pgy = (i32)a[1].u, xs->pgw = (i32)a[2].u, xs->pgh = (i32)a[3].u;
		xs->pending_geometry = true;
		break;
	case XDG_SURFACE_ACK_CONFIGURE:
		xs->acked = a[0].u;
		xs->configured = true;
		break;
	}
}

static void wm_base_dispatch(WlResource *r, u32 op, WlArg *a)
{
	switch (op) {
	case XDG_WM_BASE_DESTROY:
		res_destroy(r);
		break;
	case XDG_WM_BASE_CREATE_POSITIONER: {
		WlPositioner *p = NEW(WlPositioner);
		WlResource *pr = res_new(r->client, a[0].u, &xdg_positioner_interface, r->version, positioner_dispatch, p);
		if (pr)
			pr->destroy = positioner_destroyed;
		else
			mem_free(p);
		break;
	}
	case XDG_WM_BASE_GET_XDG_SURFACE: {
		WlSurface *s = a[1].obj->data;
		if (s->xdg || s->buffer || W.nxdgs >= (int)ARRAY_LEN(W.xdgs)) {
			protocol_error(r->client, r->id, 4 /* invalid_surface_state */, "surface already has a buffer or an xdg_surface");
			return;
		}
		WlXdgSurface *xs = NEW(WlXdgSurface);
		xs->surface = s;
		xs->res = res_new(r->client, a[0].u, &xdg_surface_interface, r->version, xdg_surface_dispatch, xs);
		if (!xs->res) {
			mem_free(xs);
			return;
		}
		xs->res->destroy = xdg_surface_destroyed;
		s->xdg = xs;
		W.xdgs[W.nxdgs++] = xs;
		break;
	}
	}
	/* pong: zdisplay doesn't ping */
}

static void bind_xdg_wm_base(WlClient *c, u32 id, u32 v)
{
	res_new(c, id, &xdg_wm_base_interface, v, wm_base_dispatch, NULL);
}

/* ---- Decorations: ZOS windows draw their own, and so do Wayland programs ---------------------------------- */

static void decoration_dispatch(WlResource *r, u32 op, WlArg *a)
{
	UNUSED(a);
	if (op == ZXDG_TOPLEVEL_DECORATION_V1_DESTROY) {
		res_destroy(r);
		return;
	}
	event(r, ZXDG_TOPLEVEL_DECORATION_V1_EV_CONFIGURE, "u", 1 /* client side */);
	WlToplevel *t = r->data;
	if (t && t->xs && t->xs->initial_sent)
		toplevel_configure(t, true);
}

static void decoration_manager_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == ZXDG_DECORATION_MANAGER_V1_DESTROY) {
		res_destroy(r);
	} else if (op == ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION) {
		WlResource *d = res_new(r->client, a[0].u, &zxdg_toplevel_decoration_v1_interface, 1, decoration_dispatch, NULL);
		if (d)
			event(d, ZXDG_TOPLEVEL_DECORATION_V1_EV_CONFIGURE, "u", 1);
	}
}

static void bind_decoration_manager(WlClient *c, u32 id, u32 v)
{
	res_new(c, id, &zxdg_decoration_manager_v1_interface, v, decoration_manager_dispatch, NULL);
}

/* ---- Relative motion and pointer locks, for games ---------------------------------------------------------- */

static void relative_destroyed(WlResource *r)
{
	remove_ptr(r->client->relative, &r->client->nrelative, r);
}

static void relative_dispatch(WlResource *r, u32 op, WlArg *a)
{
	plain_destroy(r, op, a); /* destroy is its only request */
}

static void relative_manager_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == ZWP_RELATIVE_POINTER_MANAGER_V1_DESTROY) {
		res_destroy(r);
	} else if (op == ZWP_RELATIVE_POINTER_MANAGER_V1_GET_RELATIVE_POINTER) {
		WlClient *c = r->client;
		WlResource *rp = res_new(c, a[0].u, &zwp_relative_pointer_v1_interface, 1, relative_dispatch, NULL);
		if (!rp)
			return;
		rp->destroy = relative_destroyed;
		if (c->nrelative < (int)ARRAY_LEN(c->relative))
			c->relative[c->nrelative++] = rp;
	}
}

static void bind_relative_pointer_manager(WlClient *c, u32 id, u32 v)
{
	res_new(c, id, &zwp_relative_pointer_manager_v1_interface, v, relative_manager_dispatch, NULL);
}

void wl_relative_motion(f32 dx, f32 dy, f32 raw_dx, f32 raw_dy)
{
	WlSurface *s = W.ptr_focus;
	if (!s || !s->client->nrelative)
		return;
	u64 us = time_mono_ns() / 1000;
	for (int i = 0; i < s->client->nrelative; i++)
		event(s->client->relative[i], ZWP_RELATIVE_POINTER_V1_EV_RELATIVE_MOTION, "uuffff", (u32)(us >> 32), (u32)us, FIX(dx),
		      FIX(dy), FIX(raw_dx), FIX(raw_dy));
	pointer_frame(s->client);
}

/* A lock or confinement holds while its surface has the pointer and the keyboard. */
static void constraints_update(void)
{
	for (int i = 0; i < W.nconstraints; i++) {
		WlConstraint *c = W.constraints[i];
		WlSurface *s = c->surface;
		bool want = !c->defunct && s && s->win && W.ptr_focus == s && W.kb_focus && W.kb_focus->client == s->client;
		if (want == c->active)
			continue;
		c->active = want;
		if (c->lock)
			event(c->res, want ? ZWP_LOCKED_POINTER_V1_EV_LOCKED : ZWP_LOCKED_POINTER_V1_EV_UNLOCKED, "");
		else
			event(c->res, want ? ZWP_CONFINED_POINTER_V1_EV_CONFINED : ZWP_CONFINED_POINTER_V1_EV_UNCONFINED, "");
		if (!want) {
			if (!c->persistent)
				c->defunct = true;
			/* leaving a lock: the cursor reappears where the program asked */
			if (c->lock && c->has_hint && s && s->win) {
				Rect r = content_rect(s->win, s->win->rect);
				S.px = clampf(r.x + c->hint_x, 0, S.sw - 1), S.py = clampf(r.y + c->hint_y, 0, S.sh - 1);
				cursor_update();
			}
		}
	}
}

static bool locked(void)
{
	for (int i = 0; i < W.nconstraints; i++)
		if (W.constraints[i]->active && W.constraints[i]->lock)
			return true;
	return false;
}

bool wl_pointer_locked(void)
{
	return locked();
}

/* Keeps a confined pointer inside its window. */
void wl_confine(f32 *x, f32 *y)
{
	for (int i = 0; i < W.nconstraints; i++) {
		WlConstraint *c = W.constraints[i];
		if (!c->active || c->lock || !c->surface || !c->surface->win)
			continue;
		Rect r = content_rect(c->surface->win, c->surface->win->rect);
		*x = clampf(*x, r.x, r.x + r.w - 1);
		*y = clampf(*y, r.y, r.y + r.h - 1);
	}
}

static void constraint_destroyed(WlResource *r)
{
	WlConstraint *c = r->data;
	if (c->surface)
		c->surface->constraint = NULL;
	for (int i = 0; i < W.nconstraints; i++)
		if (W.constraints[i] == c) {
			W.constraints[i] = W.constraints[--W.nconstraints];
			break;
		}
	mem_free(c);
}

static void constraint_dispatch(WlResource *r, u32 op, WlArg *a)
{
	WlConstraint *c = r->data;
	if (op == 0) { /* destroy, on both interfaces */
		res_destroy(r);
	} else if (c->lock && op == ZWP_LOCKED_POINTER_V1_SET_CURSOR_POSITION_HINT) {
		c->hint_x = UNFIX(a[0].u), c->hint_y = UNFIX(a[1].u);
		c->has_hint = true;
	}
	/* set_region: the whole window counts */
}

static void constraints_dispatch(WlResource *r, u32 op, WlArg *a)
{
	if (op == ZWP_POINTER_CONSTRAINTS_V1_DESTROY) {
		res_destroy(r);
		return;
	}
	WlSurface *s = a[1].obj->data;
	if (s->constraint) {
		protocol_error(r->client, r->id, 1 /* already_constrained */, "surface already has a pointer constraint");
		return;
	}
	if (W.nconstraints >= (int)ARRAY_LEN(W.constraints))
		return;
	bool lock = op == ZWP_POINTER_CONSTRAINTS_V1_LOCK_POINTER;
	WlConstraint *c = NEW(WlConstraint);
	c->surface = s;
	c->lock = lock;
	c->persistent = a[4].u == 2;
	c->res = res_new(r->client, a[0].u, lock ? &zwp_locked_pointer_v1_interface : &zwp_confined_pointer_v1_interface, 1,
			 constraint_dispatch, c);
	if (!c->res) {
		mem_free(c);
		return;
	}
	c->res->destroy = constraint_destroyed;
	s->constraint = c;
	W.constraints[W.nconstraints++] = c;
	constraints_update();
}

static void bind_pointer_constraints(WlClient *c, u32 id, u32 v)
{
	res_new(c, id, &zwp_pointer_constraints_v1_interface, v, constraints_dispatch, NULL);
}

/* ---- Window manager hooks -------------------------------------------------------------------------------------- */

void wl_focus_event(Window *w, bool focused)
{
	WlSurface *s = w->wl;
	if (!s)
		return;
	if (focused) {
		kb_refocus();
	} else {
		/* menus close when their window goes to the back */
		while (W.ngrabs)
			popup_dismiss(W.grabs[W.ngrabs - 1]);
		if (W.kb_focus && W.kb_focus->client == s->client)
			kb_set_focus(NULL);
	}
}

/* A new window for a surface: the surface learns it before anything is sent. */
void wl_window_attach(Window *w)
{
	WlSurface *s = w->wl;
	if (s)
		s->win = w;
}

/* The window manager is done with a window (closed, or its parent went). */
Canvas *wl_window_gone(Window *w, bool keep_picture)
{
	WlSurface *s = w->wl;
	if (!s)
		return NULL;
	Canvas *pic = keep_picture ? window_snapshot(w) : NULL;
	w->wl = NULL;
	s->win = NULL;
	if (W.ptr_focus && surface_root(W.ptr_focus) == s)
		ptr_leave();
	if (s->xdg && s->xdg->popup)
		popup_dismiss(s->xdg->popup);
	constraints_update();
	return pic;
}

/* Frame callbacks of surfaces that aren't on screen (not mapped yet, or
 * nothing changed) still fire, at about 60 per second. */
static void on_idle_frames(Loop *loop, void *data)
{
	UNUSED(loop), UNUSED(data);
	W.idle_timer = 0;
	if (W.nframe_waiters && !S.full_damage && !S.ndamage)
		wl_frame_done();
	else if (W.nframe_waiters)
		idle_frames();
}

static void idle_frames(void)
{
	if (!W.idle_timer)
		W.idle_timer = loop_timer(S.loop, 16, 0, on_idle_frames, NULL);
}

/* Clients whose memory went away under zdisplay are dropped. */
static void drop_broken_clients(void)
{
	for (int i = 0; i < W.npools; i++)
		if (W.pools[i]->broken && !W.pools[i]->client->dead) {
			W.pools[i]->broken = false;
			protocol_error(W.pools[i]->client, 1, 2 /* wl_shm invalid_fd */, "shared memory shrank under the compositor");
			i = -1; /* the pool list changed */
		}
}

/* ---- Programs' GPU buffers straight to the screen ------------------------------------------------- */

/* The full-screen window alone on screen shows its own buffer: no copy, no
 * compositing, which is what a game wants. False: compose as usual. */
bool wl_window_scanout(Window *w)
{
	WlSurface *s = w->wl;
	WlBuffer *b = s ? s->buffer : NULL;
	if (!b || b->pool || b->no_scanout || !S.out->flip_to || W.flip_buffer)
		return false;
	if (b->width != S.out->width || b->height != S.out->height || content_rect(w, w->rect).w < w->rect.w - 0.5f)
		return false;
	i32 gx, gy, gw, gh;
	geometry(s, &gx, &gy, &gw, &gh);
	if (gx != 0 || gy != 0 || gw != wl_surface_width(s) || gh != wl_surface_height(s))
		return false;
	if (b == W.shown)
		return true; /* already there */
	if (!b->fb) {
		b->fb = S.out->fb_import(S.out, b->dmabuf, (u32)b->width, (u32)b->height, (u32)b->offset, (u32)b->stride, b->modifier);
		if (!b->fb) {
			b->no_scanout = true;
			return false;
		}
	}
	if (!S.out->flip_to(S.out, b->fb))
		return false;
	b->users++; /* the screen holds it until the next flip is done */
	W.flip_buffer = b;
	return true;
}

/* A flip finished: the buffer that was on screen before goes back to its program. */
void wl_flip_complete(void)
{
	WlBuffer *old = W.shown;
	W.shown = W.flip_buffer; /* NULL after a composed frame */
	W.flip_buffer = NULL;
	if (!old || old == W.shown)
		return;
	old->users--;
	if (old->users == 0 && !old->release_after_frame) {
		if (old->res)
			event(old->res, WL_BUFFER_EV_RELEASE, "");
		buffer_maybe_free(old);
	}
}
