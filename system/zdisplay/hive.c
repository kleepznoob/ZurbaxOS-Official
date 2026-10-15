/*
 * Hive: Zurbax's tiling desktop, made of liquid glass.
 *
 * Windows share the screen between them: each new one splits the window in
 * focus along its longer side (a dwindle tree, one per workspace), with airy
 * gaps between. Every window is a rounded pane with a rim of light along its
 * edge; the one in focus wears a ring running from the theme's one accent to
 * the other and a soft glow of them, which pass from window to window as the
 * focus does. The maximize button zooms a window over the others.
 *
 * Around them, glass: the bar floats as three islands of it (the workspaces,
 * where a bead of the accent flows from one to the next; the clock; the
 * status), and the launcher, the keys and the power menu are panes of it.
 * Nine workspaces; going from one to another slides the screen across.
 * Everything is a keystroke away (Super+Return a terminal, Super+Space the
 * launcher, Super+W closes, Super+1..9 workspaces...), and Super+K shows the
 * keys. Dialogs, menus and windows that can't be resized float above, as
 * Super+V makes any.
 *
 * Its themes (Carbon, black and orange, the default; Midnight, deep blue and
 * violet) colour the rings, the glass and the apps' accent, and
 * each brings a pixel-art wallpaper the system paints for it (libzui's "bit-"
 * ones).
 */
#include "zdisplay.h"

#define WORKSPACES 9
#define GAP_IN 10                  /* between windows */
#define GAP_OUT 14                 /* around them */
#define BORDER 2                   /* room kept for a window's ring */
#define GLOW_PX 10                 /* how far the focus glow reaches */
#define BAR_TOP 8                  /* the bar floats this far down */
#define BAR_PILL 36                /* its islands' height */
#define BAR_H (BAR_TOP + BAR_PILL) /* the strip it keeps for itself */
#define SLIDE_SECONDS 0.30f
#define GLOW_SECONDS 0.24f
#define BEAD_SECONDS 0.42f
#define SETTLE 0.1                 /* a frame or two more, to draw where it ends */

/* ---- Themes ---------------------------------------------------------------------------- */

#define THEMES HIVE_THEMES /* bitart.c in libzui has them, and their wallpapers */

static const HiveTheme *theme(void)
{
	return hive_theme_find(S.set.hive_theme);
}

static int theme_index(void)
{
	return (int)(theme() - THEMES);
}

static void theme_toast(const HiveTheme *t);

/* A theme for everything: the borders and bar here, and through the settings
 * the apps' accent, the dark style and the theme's pixel-art wallpaper. */
static void apply_theme(int i)
{
	const HiveTheme *t = &THEMES[((i % HIVE_THEME_COUNT) + HIVE_THEME_COUNT) % HIVE_THEME_COUNT];
	char wall[64];
	fmt(wall, sizeof wall, "bit-%s", t->id);
	settings_set("hive_theme", t->id);
	settings_set("accent", t->accent);
	settings_set("dark", "true");
	settings_set("wallpaper", wall);
	char old_wall[128];
	str_copy(old_wall, S.set.wallpaper, sizeof old_wall);
	settings_load();
	settings_apply();
	if (!str_eq(old_wall, S.set.wallpaper))
		wallpaper_reload();
	server_broadcast_theme();
	theme_toast(t);
	damage_all();
	shell_mark_dirty();
}

/* ---- The tiling tree ------------------------------------------------------------------- */

typedef struct Node Node;
struct Node {
	Node *parent, *a, *b;
	Window *w;         /* a leaf's window */
	bool side_by_side; /* a split: a left of b, or a above b */
	f32 ratio;         /* a's share */
	Rect r;            /* where it was laid out */
};

static Node *trees[WORKSPACES + 1]; /* 1..9 */
static Window *zoomed[WORKSPACES + 1]; /* a window over the others, or none */
static int current = 1, previous = 1, slide_dir = 1;
static f64 slide_t0 = -10;

bool hive_on(void)
{
	return S.set.hive && !S.setup_mode;
}

f32 hive_bar_height(void)
{
	return BAR_H;
}

/* Windows Hive arranges: ordinary resizable top-level ones, unless floated. */
bool hive_tiled(const Window *w)
{
	return hive_on() && w && w->ws > 0 && !w->floating && !w->minimized && !(w->flags & (WIN_POPUP | WIN_DIALOG)) && !w->closing;
}

static Node *find(Node *n, const Window *w)
{
	if (!n)
		return NULL;
	if (n->w)
		return n->w == w ? n : NULL;
	Node *f = find(n->a, w);
	return f ? f : find(n->b, w);
}

static Node *last_leaf(Node *n)
{
	while (n && !n->w)
		n = n->b;
	return n;
}

static Rect area(void)
{
	Rect wa = work_area();
	Rect a = rect_inset(wa, GAP_OUT, GAP_OUT);
	/* under the floating bar the gap is the one between windows */
	a.y -= GAP_OUT - GAP_IN, a.h += GAP_OUT - GAP_IN;
	return a;
}

static void layout(Node *n, Rect r, bool animate)
{
	if (!n)
		return;
	n->r = r;
	if (n->w) {
		Window *w = n->w;
		if (w->states & STATE_FULLSCREEN)
			return;
		bool zoom = zoomed[w->ws] == w;
		Rect c = rect_inset(zoom ? area() : r, BORDER, BORDER);
		/* on whole pixels: the app's picture is then copied, never scaled */
		c = rect(roundf(c.x), roundf(c.y), roundf(c.x + c.w) - roundf(c.x), roundf(c.y + c.h) - roundf(c.y));
		bool moved = c.x != w->rect.x || c.y != w->rect.y || c.w != w->rect.w || c.h != w->rect.h;
		w->states = (w->states & ~(u32)(STATE_TILED_LEFT | STATE_TILED_RIGHT | STATE_MAXIMIZED)) | STATE_TILED | (zoom ? STATE_MAXIMIZED : 0);
		if (moved)
			window_set_rect(w, c, animate && w->appear >= 1);
		window_configure(w);
		return;
	}
	if (n->side_by_side) {
		f32 aw = roundf((r.w - GAP_IN) * n->ratio);
		layout(n->a, rect(r.x, r.y, aw, r.h), animate);
		layout(n->b, rect(r.x + aw + GAP_IN, r.y, r.w - aw - GAP_IN, r.h), animate);
	} else {
		f32 ah = roundf((r.h - GAP_IN) * n->ratio);
		layout(n->a, rect(r.x, r.y, r.w, ah), animate);
		layout(n->b, rect(r.x, r.y + ah + GAP_IN, r.w, r.h - ah - GAP_IN), animate);
	}
}

static void relayout_ws(int ws, bool animate)
{
	if (ws >= 1 && ws <= WORKSPACES)
		layout(trees[ws], area(), animate);
}

void hive_relayout(bool animate)
{
	if (!hive_on())
		return;
	for (int i = 1; i <= WORKSPACES; i++)
		relayout_ws(i, animate);
}

/* The window on workspace ws that was in focus last. */
static Window *recent_on(int ws, const Window *except)
{
	Window *best = NULL;
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		if (w != except && w->ws == ws && !w->closing && !w->minimized && w->mapped && !(w->flags & WIN_POPUP) &&
		    (!best || w->focus_serial > best->focus_serial))
			best = w;
	}
	return best;
}

static void insert(Window *w, int ws)
{
	zoomed[ws] = NULL; /* a new window is seen */
	Node *leaf = NEW(Node);
	leaf->w = w;
	Node **root = &trees[ws];
	if (!*root) {
		*root = leaf;
		return;
	}
	/* the window in focus there is split, else the newest */
	Window *f = S.focus && S.focus->ws == ws && hive_tiled(S.focus) ? S.focus : recent_on(ws, w);
	Node *at = f ? find(*root, f) : NULL;
	if (!at)
		at = last_leaf(*root);
	Node *split = NEW(Node);
	split->parent = at->parent;
	split->side_by_side = at->r.w >= at->r.h;
	split->ratio = 0.5f;
	if (!at->parent)
		*root = split;
	else if (at->parent->a == at)
		at->parent->a = split;
	else
		at->parent->b = split;
	split->a = at;
	split->b = leaf;
	at->parent = split;
	leaf->parent = split;
	/* until it's laid out, the new leaf takes after the one it split */
	leaf->r = at->r;
}

static void unlink(Window *w)
{
	if (w->ws < 1 || w->ws > WORKSPACES)
		return;
	Node *leaf = find(trees[w->ws], w);
	if (!leaf)
		return;
	if (zoomed[w->ws] == w)
		zoomed[w->ws] = NULL;
	Node *p = leaf->parent;
	if (!p) {
		trees[w->ws] = NULL;
	} else {
		Node *sib = p->a == leaf ? p->b : p->a;
		sib->parent = p->parent;
		if (!p->parent)
			trees[w->ws] = sib;
		else if (p->parent->a == p)
			p->parent->a = sib;
		else
			p->parent->b = sib;
		mem_free(p);
	}
	mem_free(leaf);
}

void hive_window_added(Window *w)
{
	if (!hive_on())
		return;
	/* popups and dialogs go with their window, the rest where you are */
	w->ws = w->parent && w->parent->ws ? w->parent->ws : current;
	if (!(w->flags & WIN_RESIZABLE) && !(w->flags & (WIN_POPUP | WIN_DIALOG)))
		w->floating = true;
	if (!hive_tiled(w))
		return;
	insert(w, w->ws);
	/* the new window starts where it'll be; the others make room */
	layout(trees[w->ws], area(), true);
	Node *leaf = find(trees[w->ws], w);
	if (leaf)
		w->rect = rect_inset(leaf->r, BORDER, BORDER);
}

void hive_window_removed(Window *w)
{
	if (!hive_on() || !find(w->ws >= 1 && w->ws <= WORKSPACES ? trees[w->ws] : NULL, w))
		return;
	int ws = w->ws;
	unlink(w);
	relayout_ws(ws, true);
}

/* ---- Workspaces ------------------------------------------------------------------------ */

static f32 slide_k(void)
{
	f32 t = (f32)((S.now - slide_t0) / SLIDE_SECONDS);
	return t >= 1 ? 1 : ease_out_cubic(clampf(t, 0, 1));
}

static bool sliding(void)
{
	return hive_on() && S.now - slide_t0 < SLIDE_SECONDS;
}

static bool glowing(void);
static bool bar_animating(void);

bool hive_animating(void)
{
	return hive_on() && (S.now - slide_t0 < SLIDE_SECONDS + SETTLE || glowing() || bar_animating());
}

bool hive_window_hidden(const Window *w)
{
	if (!hive_on() || w->ws <= 0)
		return false;
	if (w->ws == current)
		return false;
	return !(sliding() && w->ws == previous);
}

f32 hive_window_offset(const Window *w)
{
	if (!sliding() || w->ws <= 0)
		return 0;
	f32 k = slide_k();
	if (w->ws == current)
		return (f32)slide_dir * (1 - k) * S.sw;
	if (w->ws == previous)
		return -(f32)slide_dir * k * S.sw;
	return 0;
}

static void bead_from_here(void);

static void go_to(int n)
{
	if (n < 1 || n > WORKSPACES || n == current)
		return;
	bead_from_here();
	previous = current;
	current = n;
	slide_dir = n > previous ? 1 : -1;
	slide_t0 = S.set.reduce_motion ? -10 : S.now;
	window_focus(recent_on(current, NULL));
	damage_all();
	shell_mark_dirty();
	request_frame();
}

static void move_to(Window *w, int n)
{
	if (!w || n < 1 || n > WORKSPACES || n == w->ws || (w->flags & WIN_POPUP))
		return;
	int from = w->ws;
	bool tiled = hive_tiled(w);
	if (tiled)
		unlink(w);
	w->ws = n;
	for (int i = 0; i < S.nwin; i++)
		if (S.windows[i]->parent == w)
			S.windows[i]->ws = n;
	if (tiled) {
		insert(w, n);
		relayout_ws(n, false);
		relayout_ws(from, true);
	}
	window_focus(recent_on(from, w));
	damage_all();
	shell_mark_dirty();
}

static bool occupied(int ws)
{
	for (int i = 0; i < S.nwin; i++)
		if (S.windows[i]->ws == ws && !S.windows[i]->closing && !S.windows[i]->minimized && S.windows[i]->mapped)
			return true;
	return false;
}

/* ---- Window commands ------------------------------------------------------------------- */

/* The tiled window from the one in focus in a direction (dx, dy). */
static Window *neighbour(int dx, int dy)
{
	Window *f = S.focus;
	if (!f)
		return recent_on(current, NULL);
	f32 fx = f->rect.x + f->rect.w / 2, fy = f->rect.y + f->rect.h / 2;
	Window *best = NULL;
	f32 best_d = 1e9f;
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		if (w == f || w->ws != current || w->closing || w->minimized || !w->mapped || (w->flags & WIN_POPUP))
			continue;
		f32 wx = w->rect.x + w->rect.w / 2, wy = w->rect.y + w->rect.h / 2;
		f32 along = dx ? (wx - fx) * (f32)dx : (wy - fy) * (f32)dy;
		f32 across = dx ? fabsf(wy - fy) : fabsf(wx - fx);
		if (along <= 1)
			continue;
		f32 d = along + across * 2;
		if (d < best_d)
			best_d = d, best = w;
	}
	return best;
}

static void swap_with(int dx, int dy)
{
	Window *f = S.focus, *o = neighbour(dx, dy);
	if (!hive_tiled(f) || !hive_tiled(o))
		return;
	Node *a = find(trees[current], f), *b = find(trees[current], o);
	if (!a || !b)
		return;
	a->w = o, b->w = f;
	relayout_ws(current, true);
}

static void toggle_split(void)
{
	Node *n = hive_tiled(S.focus) ? find(trees[current], S.focus) : NULL;
	if (n && n->parent) {
		n->parent->side_by_side = !n->parent->side_by_side;
		relayout_ws(current, true);
	}
}

static void grow(int dx, int dy)
{
	Node *n = hive_tiled(S.focus) ? find(trees[current], S.focus) : NULL;
	/* the nearest split across that direction */
	for (Node *c = n; c && c->parent; c = c->parent) {
		Node *p = c->parent;
		if (p->side_by_side != (dx != 0))
			continue;
		f32 step = 0.05f * (f32)(dx ? dx : dy);
		p->ratio = clampf(p->ratio + (p->a == c ? step : -step), 0.15f, 0.85f);
		relayout_ws(current, true);
		return;
	}
}

static void toggle_float(Window *w)
{
	if (!w || (w->flags & (WIN_POPUP | WIN_DIALOG)) || w->ws <= 0)
		return;
	if (hive_tiled(w)) {
		unlink(w);
		w->floating = true;
		w->states &= ~(u32)(STATE_MAXIMIZED | STATE_TILED);
		Rect a = area();
		window_set_rect(w, rect(a.x + a.w * 0.2f, a.y + a.h * 0.15f, a.w * 0.6f, a.h * 0.7f), true);
		window_raise(w);
	} else {
		w->floating = false;
		insert(w, w->ws);
	}
	relayout_ws(w->ws, true);
	window_configure(w);
	shell_mark_dirty();
}

/* The maximize button, a double click on the title or Super+Z: the window
 * fills the workspace, over the others, till it's done again (or another
 * window comes, or the focus moves on). */
void hive_zoom(Window *w, bool on)
{
	if (!hive_tiled(w))
		return;
	if (on == (zoomed[w->ws] == w))
		return;
	zoomed[w->ws] = on ? w : NULL;
	if (on)
		window_raise(w);
	relayout_ws(w->ws, true);
	shell_mark_dirty();
}

/* Minimized, a window leaves the tiling for the tray in the bar, and the
 * others close over its place; back, it comes to the workspace you're on. */
void hive_window_minimized(Window *w, bool on)
{
	if (!hive_on() || w->ws <= 0 || (w->flags & (WIN_POPUP | WIN_DIALOG)))
		return;
	if (on) {
		int ws = w->ws;
		if (find(trees[ws], w)) {
			unlink(w);
			relayout_ws(ws, true);
		}
	} else {
		w->ws = current;
		for (int i = 0; i < S.nwin; i++)
			if (S.windows[i]->parent == w)
				S.windows[i]->ws = current;
		if (!w->floating) {
			insert(w, current);
			relayout_ws(current, true);
		}
	}
	shell_mark_dirty();
}

/* Hive turned on or off in Settings. */
void hive_settings_changed(bool was_on)
{
	bool on = hive_on();
	if (on == was_on)
		return;
	for (int i = 1; i <= WORKSPACES; i++)
		while (trees[i]) {
			Window *w = last_leaf(trees[i])->w;
			unlink(w);
		}
	current = previous = 1;
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		w->ws = 0;
		w->floating = false;
		if (on && !w->closing && !(w->flags & WIN_POPUP))
			hive_window_added(w);
		else if (!on)
			w->states &= ~(u32)(STATE_MAXIMIZED | STATE_TILED);
	}
	if (on)
		hive_relayout(true);
	else
		wm_refit();
	damage_all();
	shell_mark_dirty();
}

/* ---- Frames ------------------------------------------------------------------------------ */

/* The window in focus takes the ring and its glow over a moment, and the one
 * it took them from gives them up. (These are only compared with, never
 * followed: the window may be gone.) */
static const Window *glow_cur, *glow_prev;
static f64 glow_t0 = -10;

static void glow_track(void)
{
	if (S.focus == glow_cur)
		return;
	glow_prev = glow_cur;
	glow_cur = S.focus;
	glow_t0 = S.set.reduce_motion ? -10 : S.now;
}

static bool glowing(void)
{
	return S.now - glow_t0 < GLOW_SECONDS + SETTLE;
}

static f32 glow_of(const Window *w)
{
	f32 k = ease_out_cubic(clampf((f32)((S.now - glow_t0) / GLOW_SECONDS), 0, 1));
	return w == glow_cur ? k : w == glow_prev ? 1 - k : 0;
}

/* signed distance from (px, py) to a rounded rectangle: negative inside */
static inline f32 rr_dist(f32 px, f32 py, f32 cx, f32 cy, f32 hw, f32 hh, f32 rad)
{
	f32 qx = fabsf(px - cx) - hw + rad, qy = fabsf(py - cy) - hh + rad;
	f32 ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	return sqrtf(ox * ox + oy * oy) + minf(maxf(qx, qy), 0) - rad;
}

/* Around a window's picture pr (device pixels): a rim like the edge of a pane
 * of glass, catching the light at the top; on the window in focus, a ring
 * running from the theme's one accent to the other, and their glow. */
void hive_draw_border(Canvas *frame, Window *w, Rect pr, f32 opacity)
{
	if (!hive_on() || (w->flags & WIN_POPUP) || (w->states & STATE_FULLSCREEN) || w->ws <= 0 || opacity <= 0.01f)
		return;
	glow_track();
	const HiveTheme *t = theme();
	f32 g = glow_of(w), sc = S.scale, k = clampf(opacity, 0, 1);
	/* the accents' gradient, and the ring's colour along it, as tables */
	enum { STEPS = 64 };
	u32 acc[STEPS], ring_c[STEPS];
	Color c1 = color_hex(t->accent), c2 = color_hex(t->accent2);
	for (int i = 0; i < STEPS; i++) {
		Color a = color_mix(c1, c2, (f32)i / (STEPS - 1));
		acc[i] = color_premul(a);
		ring_c[i] = color_premul(color_mix(COLOR_WHITE, a, g));
	}
	f32 bw = (1 + g) * sc; /* the ring: a pixel wide, two in focus */
	f32 halo = GLOW_PX * sc;
	f32 rad = (w->wl ? 0 : w->radius) * sc;
	f32 cx = pr.x + pr.w / 2, cy = pr.y + pr.h / 2, hw = pr.w / 2, hh = pr.h / 2;
	f32 reach = bw + (g > 0 ? halo : 0) + 1;
	Rect o = rect_inset(pr, -reach, -reach), clip = frame->clip;
	int x0 = MAX((int)floorf(o.x), (int)clip.x), y0 = MAX((int)floorf(o.y), (int)clip.y);
	int x1 = MIN((int)ceilf(o.x + o.w), (int)(clip.x + clip.w)), y1 = MIN((int)ceilf(o.y + o.h), (int)(clip.y + clip.h));
	f32 gx = (STEPS - 1) / (2 * maxf(pr.w, 1)), gy = (STEPS - 1) / (2 * maxf(pr.h, 1));
	for (int y = y0; y < y1; y++) {
		u32 *row = canvas_row(frame, y);
		f32 py = (f32)y + 0.5f;
		/* rows along the straight sides: what's well inside is skipped */
		bool middle = py > pr.y + rad + 2 && py < pr.y + pr.h - rad - 2;
		bool corners = py < pr.y + rad || py > pr.y + pr.h - rad;
		f32 dy = py < cy ? pr.y - py : py - (pr.y + pr.h); /* to the top or bottom edge */
		f32 rim = 0.17f - 0.11f * clampf((py - pr.y) / maxf(pr.h, 1), 0, 1);
		f32 ring_a = rim + (1 - rim) * g;
		f32 gbase = (py - pr.y) * gy;
		for (int x = x0; x < x1; x++) {
			f32 px = (f32)x + 0.5f;
			if (middle && px > pr.x + 2 && px < pr.x + pr.w - 2) {
				x = MAX(x, (int)ceilf(pr.x + pr.w - 2.5f) - 1); /* never back */
				continue;
			}
			f32 d;
			if (px > pr.x + rad && px < pr.x + pr.w - rad)
				d = dy;
			else if (!corners)
				d = px < cx ? pr.x - px : px - (pr.x + pr.w);
			else
				d = rr_dist(px, py, cx, cy, hw, hh, rad);
			if (d < -1 || d > reach)
				continue;
			f32 ring = clampf(0.5f - (d - bw), 0, 1) - clampf(0.5f - d, 0, 1);
			int gi = CLAMP((int)(gbase + (px - pr.x) * gx + 0.5f), 0, STEPS - 1);
			if (g > 0 && d > 0) {
				f32 u = clampf(1 - (d - bw * 0.5f) / halo, 0, 1);
				f32 a = u * u * 0.45f * g * (1 - ring);
				if (a > 0.004f)
					row[x] = blend_premul(row[x], acc[gi], (u32)(a * k * 256));
			}
			if (ring > 0)
				row[x] = blend_premul(row[x], ring_c[gi], (u32)(ring * ring_a * k * 256));
		}
	}
}

/* Every frame, before it's drawn: what moves is drawn again. */
void hive_animate(void)
{
	if (!hive_on())
		return;
	glow_track();
	if (S.now - slide_t0 < SLIDE_SECONDS + SETTLE)
		damage_all();
	else if (glowing())
		for (int i = 0; i < S.nwin; i++)
			if (S.windows[i] == glow_cur || S.windows[i] == glow_prev)
				window_damage(S.windows[i]);
}

/* ---- Keys -------------------------------------------------------------------------------- */

static bool launcher_open, help_open, power_open;
static StrBuf query;
static int pick;

static int number_key(u32 code)
{
	if (code >= KEY_1 && code <= KEY_9)
		return (int)(code - KEY_1) + 1;
	return 0;
}

static void close_overlays(void)
{
	launcher_open = help_open = power_open = false;
}

static void open_launcher(void)
{
	close_overlays();
	launcher_open = true;
	sb_clear(&query);
	pick = 0;
	shell_reload_apps();
}

bool hive_wants_pointer(void)
{
	return hive_on() && (launcher_open || help_open || power_open || S.py < BAR_H);
}

static bool launcher_matches(const AppInfo *a)
{
	const char *q = sb_str(&query);
	return !a->hidden && (!q[0] || str_find_nocase(a->name, q) || str_find_nocase(a->keywords, q) || str_find_nocase(a->id, q));
}

static void launch_pick(void)
{
	int n = 0;
	for (int i = 0; i < S.napps; i++) {
		if (!launcher_matches(&S.apps[i]))
			continue;
		if (n++ == pick) {
			shell_launch_new(S.apps[i].id);
			break;
		}
	}
	launcher_open = false;
}

bool hive_key(u32 code, u32 state, u32 mods, const char *text)
{
	if (!hive_on() || S.locked)
		return false;
	bool pressed = state != KEY_RELEASED;
	/* the launcher takes the typing */
	if (launcher_open) {
		if (!pressed)
			return true;
		if (code == KEY_ESC || ((mods & MOD_SUPER) && code == KEY_SPACE))
			launcher_open = false;
		else if (code == KEY_ENTER || code == KEY_KPENTER)
			launch_pick();
		else if (code == KEY_UP)
			pick = MAX(0, pick - 1);
		else if (code == KEY_DOWN)
			pick++;
		else if (code == KEY_BACKSPACE && query.len) {
			/* the last character, however many bytes */
			usize at = query.len - 1;
			while (at > 0 && ((u8)query.data[at] & 0xc0) == 0x80)
				at--;
			sb_remove(&query, at, query.len - at);
		}
		else if (text && text[0] && (u8)text[0] >= 32 && !(mods & (MOD_CTRL | MOD_SUPER | MOD_ALT))) {
			sb_adds(&query, text);
			pick = 0;
		}
		shell_mark_dirty();
		return true;
	}
	if ((help_open || power_open) && pressed && code == KEY_ESC) {
		close_overlays();
		shell_mark_dirty();
		return true;
	}
	if (!pressed || !(mods & MOD_SUPER))
		return false;
	bool shift = mods & MOD_SHIFT, ctrl = mods & MOD_CTRL;
	Window *f = S.focus;
	int n = number_key(code);
	if (n) {
		if (shift)
			move_to(f, n);
		else
			go_to(n);
		return true;
	}
	switch (code) {
	case KEY_ENTER: shell_launch_new("forge"); return true;
	case KEY_SPACE:
		if (ctrl && shift)
			apply_theme(theme_index() + 1);
		else if (ctrl)
			apply_theme(theme_index() + 1);
		else
			open_launcher();
		shell_mark_dirty();
		return true;
	case KEY_W:
	case KEY_Q:
		if (f)
			window_close_request(f);
		return true;
	case KEY_V:
		if (ctrl)
			return false; /* Super+Ctrl+V: what was copied, as elsewhere */
		toggle_float(f);
		return true;
	case KEY_J: toggle_split(); return true;
	case KEY_Z:
		if (f)
			hive_zoom(f, zoomed[current] != f);
		return true;
	case KEY_K:
		close_overlays();
		help_open = true;
		shell_mark_dirty();
		return true;
	case KEY_ESC:
		close_overlays();
		power_open = true;
		shell_mark_dirty();
		return true;
	case KEY_B: shell_launch_new("drift"); return true;
	case KEY_F: shell_launch_new("vault"); return true;
	case KEY_T: shell_launch_new("pulse"); return true;
	case KEY_N: shell_launch_new("writer"); return true;
	case KEY_M: shell_launch_new("music"); return true;
	case KEY_G: shell_launch_new("bridge"); return true;
	case KEY_COMMA: shell_launch_new("settings"); return true;
	case KEY_LEFT:
	case KEY_RIGHT:
	case KEY_UP:
	case KEY_DOWN:
	case KEY_H:
	case KEY_L: {
		int dx = code == KEY_LEFT || code == KEY_H ? -1 : code == KEY_RIGHT || code == KEY_L ? 1 : 0;
		int dy = code == KEY_UP ? -1 : code == KEY_DOWN ? 1 : 0;
		if (zoomed[current])
			hive_zoom(zoomed[current], false);
		if (ctrl)
			grow(dx, dy);
		else if (shift)
			swap_with(dx, dy);
		else {
			Window *o = neighbour(dx, dy);
			if (o) {
				window_focus(o);
				if (!hive_tiled(o))
					window_raise(o);
			}
		}
		return true;
	}
	default: return false;
	}
}

/* Focus follows the pointer, as in Hyprland. */
void hive_pointer_motion(void)
{
	if (!hive_on() || S.grab != GRAB_NONE || launcher_open || help_open || power_open || S.py < BAR_H || zoomed[current])
		return;
	u32 edges;
	Window *w = window_at(S.px, S.py, &edges);
	if (w && hive_tiled(w) && w != S.focus && !(S.focus && !hive_tiled(S.focus) && S.focus->ws == current))
		window_focus(w);
}

/* ---- Glass: the bar, the launcher, the keys, the power menu ------------------------------ */

static int cpu_percent(void)
{
	static u64 last_total, last_idle;
	static f64 last_at;
	static int value;
	if (S.now - last_at < 2)
		return value;
	last_at = S.now;
	char *s = file_read_line("/proc/stat");
	if (s && str_starts(s, "cpu ")) {
		u64 v[8] = { 0 }, total = 0;
		const char *p = s + 4;
		for (int i = 0; i < 8; i++) {
			char *end;
			v[i] = (u64)str_to_int(p, (const char **)&end);
			total += v[i];
			p = end;
		}
		u64 idle = v[3] + v[4];
		if (last_total && total > last_total)
			value = (int)(100 - (idle - last_idle) * 100 / (total - last_total));
		last_total = total, last_idle = idle;
	}
	mem_free(s);
	return value;
}

/* A piece of liquid glass, a breath of the theme's colour in it: the bar's
 * islands and the panes over the desktop. alpha fades it all. */
static void glass(Ui *ui, Rect r, f32 radius, const HiveTheme *t, f32 alpha)
{
	shell_surface(r);
	ui_shadow(ui, r, radius, 18, 6, color_scale_alpha(color_hex("#00000070"), alpha));
	glass_panel_style(ui, r, radius, alpha, true);
	ui_round(ui, r, radius, color_scale_alpha(color_with_alpha(color_hex(t->bg), 0x30), alpha));
	ui_border(ui, r, radius, 1, color_scale_alpha(color_hex("#ffffff1a"), alpha));
}

/* ---- The bead: the current workspace, a drop of the accent that flows ---------------------- */

static f32 bead_from = -1; /* where it set off, in workspace slots (0 the first) */
static f64 bead_t0 = -10;

/* Its left and right ends, in slots: going to another workspace, its leading
 * end runs ahead and the other catches up, so it stretches and gathers
 * again, like a drop. */
static void bead_edges(f32 *l, f32 *r)
{
	f32 to = (f32)(current - 1);
	f32 t = (f32)((S.now - bead_t0) / BEAD_SECONDS);
	if (bead_from < 0 || t >= 1) {
		*l = *r = to;
		return;
	}
	f32 lead = ease_out_cubic(clampf(t / 0.6f, 0, 1)), trail = ease_out_cubic(clampf((t - 0.22f) / 0.78f, 0, 1));
	f32 a = bead_from + (to - bead_from) * lead, b = bead_from + (to - bead_from) * trail;
	*l = minf(a, b), *r = maxf(a, b);
}

static void bead_from_here(void)
{
	f32 l, r;
	bead_edges(&l, &r);
	bead_from = (l + r) / 2;
	bead_t0 = S.set.reduce_motion ? -10 : S.now;
}

/* ---- A theme's name, shown as it comes ------------------------------------------------------ */

#define TOAST_SECONDS 1.6

static char toast_text[64];
static f64 toast_t0 = -10;

static void theme_toast(const HiveTheme *t)
{
	str_copy(toast_text, t->name, sizeof toast_text);
	toast_t0 = S.now;
}

static bool bar_animating(void)
{
	return S.now - bead_t0 < BEAD_SECONDS + SETTLE || S.now - toast_t0 < TOAST_SECONDS + SETTLE;
}

static void toast(Ui *ui, const HiveTheme *t)
{
	f32 age = (f32)(S.now - toast_t0);
	if (age >= TOAST_SECONDS)
		return;
	f32 a = clampf(minf(age / 0.15f, (TOAST_SECONDS - age) / 0.4f), 0, 1);
	Font f = ui_font(ui, 15, 700);
	const char *dots[] = { t->accent, t->accent2, t->fg, t->muted, t->inactive };
	bool px = S.set.pixel_style;
	f32 w = 22 + 5 * 14 + 10 + (px ? ui_pixel_text_width(toast_text, 2) : ui_text_width(ui, f, toast_text)) + 24;
	Rect r = rect(roundf((S.sw - w) / 2), roundf(S.sh - 120), w, 48);
	glass(ui, r, 24, t, a);
	for (int i = 0; i < 5; i++)
		ui_circle(ui, r.x + 28 + (f32)i * 14, r.y + 24, 5, color_scale_alpha(color_hex(dots[i]), a));
	if (px)
		ui_pixel_text(ui, r.x + 22 + 5 * 14 + 10, r.y + 17, toast_text, 2, color_scale_alpha(color_hex(t->fg), a));
	else
		ui_text(ui, f, r.x + 22 + 5 * 14 + 10, r.y + roundf((48 + font_cap_height(f)) / 2), toast_text,
			color_scale_alpha(color_hex(t->fg), a));
}

/* ---- The bar: three islands ------------------------------------------------------------------ */

/* where the tray's windows were drawn, for them to fly to and from */
static struct {
	const Window *w;
	Rect r;
} tray_at[8];
static int ntray;

Rect hive_tray_rect(const Window *w)
{
	for (int i = 0; i < ntray; i++)
		if (tray_at[i].w == w)
			return tray_at[i].r;
	return rect(GAP_OUT + 200, BAR_TOP + 10, 16, 16); /* till the bar is drawn with it */
}

static void bar(Ui *ui, const HiveTheme *t)
{
	if (S.focus && (S.focus->states & STATE_FULLSCREEN) && S.focus->ws == current)
		return;
	Color fg = color_hex(t->fg), muted = color_hex(t->muted), accent = color_hex(t->accent), bg = color_hex(t->bg);
	Color hover = color_hex("#ffffff1a");
	Font f = ui_font(ui, 13, 600), small = ui_font(ui, 12, 500), bold = ui_font(ui, 14, 700);
	f32 y = BAR_TOP, h = BAR_PILL, base = y + roundf((h + font_cap_height(f)) / 2);

	/* the left island: the logo, the workspaces with the bead among them, and
	 * the window in focus */
	const f32 slot = 26;
	int last = 5;
	for (int i = 6; i <= WORKSPACES; i++)
		if (occupied(i) || i == current)
			last = i;
	const char *name = NULL, *icon = NULL;
	if (S.focus && S.focus->ws == current && !S.focus->closing) {
		const AppInfo *a = shell_app(S.focus->app_id);
		name = S.focus->title[0] ? S.focus->title : a ? a->name : S.focus->app_id;
		icon = a ? a->icon : S.focus->app_id;
	}
	f32 name_w = name ? minf(ui_text_width(ui, small, name), 260) : 0;
	/* the tray: windows put away, waiting to come back */
	Window *tray[8];
	int nt = 0;
	for (int i = 0; i < S.nwin && nt < (int)ARRAY_LEN(tray); i++) {
		Window *w = S.windows[i];
		if (w->minimized && !w->closing && w->ws > 0 && !(w->flags & WIN_POPUP))
			tray[nt++] = w;
	}
	Rect L = rect(GAP_OUT, y, 42 + slot * (f32)last + (name ? 16 + 24 + name_w : 0) + (nt ? 14 + 26 * (f32)nt : 0) + 12, h);
	glass(ui, L, h / 2, t, 1);
	ui_app_icon(ui, rect(L.x + 12, y + (h - 20) / 2, 20, 20), "zurbax");
	f32 wx = L.x + 40, bl, br;
	bead_edges(&bl, &br);
	f32 bh = 24 - minf(6, (br - bl) * 6); /* thinner as it stretches */
	Rect bead = rect(wx + bl * slot + 1, y + (h - bh) / 2, (br - bl) * slot + slot - 2, bh);
	ui_shadow(ui, bead, bh / 2, 12, 0, color_with_alpha(accent, 0x90));
	ui_gradient(ui, bead, bh / 2, color_lighten(accent, 0.18f), color_mix(accent, color_hex(t->accent2), 0.35f));
	ui_round(ui, rect(bead.x + 5, bead.y + 2, bead.w - 10, roundf(bh * 0.36f)), bh * 0.18f, color_hex("#ffffff3a")); /* its shine */
	for (int i = 1; i <= last; i++) {
		Rect cell = rect(wx + (f32)(i - 1) * slot, y + 5, slot, h - 10);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "ws"), cell);
		ui_pop_id(ui);
		f32 c = (f32)(i - 1);
		bool under = c > bl - 0.5f && c < br + 0.5f;
		if (it.hovered && !under)
			ui_round(ui, rect_inset(cell, 2, 1), (h - 12) / 2, hover);
		char num[4];
		fmt(num, sizeof num, "%d", i);
		Color nc = under ? bg : occupied(i) ? fg : color_with_alpha(muted, 0xa0);
		if (S.set.pixel_style) /* the numbers in pixels */
			ui_pixel_text(ui, roundf(cell.x + (cell.w - ui_pixel_text_width(num, 2)) / 2), roundf(y + (h - 14) / 2), num, 2, nc);
		else
			ui_text_in(ui, f, cell, num, nc, ALIGN_CENTER);
		if (it.clicked)
			go_to(i);
	}
	f32 tx = wx + slot * (f32)last;
	if (name) {
		f32 x = tx + 8;
		ui_fill(ui, rect(x, y + 11, 1, h - 22), color_hex("#ffffff26"));
		x += 9;
		ui_app_icon(ui, rect(x, y + (h - 18) / 2, 18, 18), icon);
		ui_text_in(ui, small, rect(x + 24, y, name_w + 2, h), name, fg, ALIGN_START);
		tx = x + 24 + name_w;
	}
	ntray = 0;
	if (nt) {
		f32 x = tx + 8;
		ui_fill(ui, rect(x, y + 11, 1, h - 22), color_hex("#ffffff26"));
		x += 6;
		for (int i = 0; i < nt; i++, x += 26) {
			Rect chip = rect(x, y + 5, 26, h - 10);
			ui_push_id(ui, 200 + i);
			UiInteract it = ui_interact(ui, ui_id(ui, "tray"), chip);
			ui_pop_id(ui);
			if (it.hovered)
				ui_round(ui, chip, 8, hover);
			const AppInfo *a = shell_app(tray[i]->app_id);
			Rect ic = rect(x + 5, y + (h - 16) / 2, 16, 16);
			ui_app_icon(ui, ic, a ? a->icon : tray[i]->app_id);
			tray_at[ntray].w = tray[i], tray_at[ntray++].r = ic;
			if (it.clicked)
				window_minimize(tray[i], false);
		}
	}

	/* the middle island: the time and the date; a click shows what's new */
	Civil c;
	civil_local(time_unix(), &c);
	char tm[16], date[32];
	int hour = S.set.clock_24h ? c.hour : (c.hour % 12 ? c.hour % 12 : 12);
	fmt(tm, sizeof tm, S.set.clock_24h ? "%02d:%02d" : "%d:%02d", hour, c.min);
	fmt(date, sizeof date, "%s%.3s %d %.3s", S.set.clock_24h ? "" : c.hour < 12 ? "am  " : "pm  ", WEEKDAY_NAMES[c.wday], c.day,
	    MONTH_NAMES[c.month - 1]);
	bool px = S.set.pixel_style; /* the time in pixels */
	f32 tw = px ? ui_pixel_text_width(tm, 2) : ui_text_width(ui, bold, tm), dw = ui_text_width(ui, small, date);
	Rect C = rect(roundf((S.sw - (tw + dw + 50)) / 2), y, tw + dw + 50, h);
	glass(ui, C, h / 2, t, 1);
	if (ui_interact(ui, ui_id(ui, "hive-clock"), C).clicked)
		S.notifications_open = !S.notifications_open;
	if (px)
		ui_pixel_text(ui, C.x + 18, roundf(y + (h - 14) / 2), tm, 2, fg);
	else
		ui_text(ui, bold, C.x + 18, base, tm, fg);
	ui_circle(ui, C.x + 18 + tw + 7, y + h / 2, 2, accent);
	ui_text(ui, small, C.x + 18 + tw + 14, base, date, muted);

	/* the right island: how busy the computer is, then the usual status */
	const char *icons[8];
	int bat, ni = shell_status_icons(icons, &bat);
	int cpu = cpu_percent();
	char cpus[8], pct[8] = "";
	fmt(cpus, sizeof cpus, "%d%%", cpu);
	if (bat >= 0 && S.battery >= 0)
		fmt(pct, sizeof pct, "%d%%", S.battery);
	f32 rw = 16 + 30 + ui_text_width(ui, small, cpus) + 12 + (f32)ni * 26 + (pct[0] ? ui_text_width(ui, small, pct) + 4 : 0) + 10;
	Rect R = rect(S.sw - GAP_OUT - rw, y, rw, h);
	glass(ui, R, h / 2, t, 1);
	f32 x = R.x + 16;
	Rect track = rect(x, y + h / 2 - 3, 24, 6);
	ui_round(ui, track, 3, color_hex("#ffffff24"));
	ui_round(ui, rect(track.x, track.y, maxf(6, 24 * (f32)cpu / 100), 6), 3, accent);
	x += 30;
	ui_text(ui, small, x, base, cpus, muted);
	x += ui_text_width(ui, small, cpus) + 12;
	for (int i = 0; i < ni; i++) {
		Rect hit = rect(x, y + 5, 26, h - 10);
		ui_push_id(ui, 100 + i);
		UiInteract it = ui_interact(ui, ui_id(ui, "st"), hit);
		ui_pop_id(ui);
		if (it.hovered)
			ui_round(ui, hit, 13, hover);
		ui_icon(ui, rect(x + 5, y + (h - 16) / 2, 16, 16), icons[i], it.hovered ? accent : fg);
		if (it.clicked) {
			if (str_eq(icons[i], "system-shutdown"))
				close_overlays(), power_open = true;
			else
				S.notifications_open = !S.notifications_open;
		}
		x += 26;
		if (i == bat && pct[0]) {
			ui_text(ui, small, x, base, pct, fg);
			x += ui_text_width(ui, small, pct) + 4;
		}
	}
	ui->wake_at = ui->wake_at > 0 && ui->wake_at < S.now + 2 ? ui->wake_at : S.now + 2;
}

/* ---- Panes ------------------------------------------------------------------------------------ */

static Rect centre(f32 w, f32 h)
{
	return rect(roundf((S.sw - w) / 2), roundf(BAR_H + (S.sh - BAR_H - h) / 3), w, h);
}

/* Keys drawn as keycaps, "Super|Shift|S"; returns where they end. */
static f32 keycaps(Ui *ui, f32 x, f32 cy, const char *combo, const HiveTheme *t)
{
	Font f = ui_font(ui, 12, 600);
	char buf[64];
	str_copy(buf, combo, sizeof buf);
	for (char *k = buf; k && *k;) {
		char *sep = (char *)str_chr(k, '|');
		if (sep)
			*sep = 0;
		f32 w = maxf(ui_text_width(ui, f, k) + 14, 24);
		Rect cap = rect(x, cy - 11, w, 22);
		ui_round(ui, cap, 6, color_hex("#ffffff14"));
		ui_border(ui, cap, 6, 1, color_hex("#ffffff2a"));
		ui_fill(ui, rect(cap.x + 4, cap.y + cap.h - 1, cap.w - 8, 1), color_hex("#00000050"));
		ui_text_in(ui, f, cap, k, color_hex(t->fg), ALIGN_CENTER);
		x += w + 4;
		k = sep ? sep + 1 : NULL;
	}
	return x;
}

static f32 sel_y = -1; /* the launcher's highlight, gliding to the row picked */
static f64 sel_at;

static void launcher(Ui *ui, const HiveTheme *t)
{
	Rect r = centre(600, 484);
	glass(ui, r, 26, t, 1);
	Color fg = color_hex(t->fg), muted = color_hex(t->muted), accent = color_hex(t->accent);
	Font big = ui_font(ui, 18, 500), f = ui_font(ui, 14, 600), small = ui_font(ui, 12, 500);
	/* the search field: a well in the glass */
	Rect field = rect(r.x + 16, r.y + 16, r.w - 32, 46);
	ui_round(ui, field, 23, color_hex("#00000048"));
	ui_border(ui, field, 23, 1, color_hex("#ffffff1e"));
	ui_icon(ui, rect(field.x + 18, field.y + 15, 16, 16), "search", muted);
	const char *q = sb_str(&query);
	f32 tx = field.x + 44, tb = field.y + roundf((field.h + font_cap_height(big)) / 2);
	ui_text(ui, big, tx, tb, q[0] ? q : "Search apps", q[0] ? fg : muted);
	if (fmodf((f32)S.now, 1.0f) < 0.55f)
		ui_fill(ui, rect(tx + (q[0] ? ui_text_width(ui, big, q) + 1 : 0), field.y + 13, 2, 20), accent);
	ui->wake_at = S.now + 0.5;
	f32 top = field.y + field.h + 12, row_h = 40;
	int n = 0;
	for (int i = 0; i < S.napps; i++)
		if (launcher_matches(&S.apps[i]))
			n++;
	pick = n ? MIN(pick, n - 1) : 0;
	int first = MAX(0, pick - 8);
	/* the highlight glides to the row picked */
	f32 target = top + (f32)(pick - first) * row_h, dt = (f32)clampf((f32)(S.now - sel_at), 0, 0.05f);
	sel_at = S.now;
	sel_y = sel_y < 0 || S.set.reduce_motion ? target : sel_y + (target - sel_y) * minf(1, dt * 22);
	if (fabsf(target - sel_y) > 0.5f)
		ui->redraw = true;
	else
		sel_y = target;
	if (n) {
		Rect hl = rect(r.x + 12, sel_y, r.w - 24, row_h - 2);
		ui_round(ui, hl, 13, color_with_alpha(accent, 0x30));
		ui_border(ui, hl, 13, 1, color_with_alpha(accent, 0x55));
	}
	f32 y = top;
	for (int i = 0, k = 0, shown = 0; i < S.napps && shown < 9; i++) {
		const AppInfo *a = &S.apps[i];
		if (!launcher_matches(a))
			continue;
		if (k++ < first)
			continue;
		Rect row = rect(r.x + 12, y, r.w - 24, row_h - 2);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "app"), row);
		ui_pop_id(ui);
		bool sel = k - 1 == pick;
		if (it.hovered && !sel)
			ui_round(ui, row, 13, color_hex("#ffffff12"));
		ui_app_icon(ui, rect(row.x + 12, row.y + 6, 26, 26), a->icon);
		ui_text_in(ui, f, rect(row.x + 50, row.y, row.w - 160, row.h), a->name, sel ? color_lighten(accent, 0.25f) : fg, ALIGN_START);
		if (a->category[0])
			ui_text_in(ui, small, rect(row.x + row.w - 130, row.y, 116, row.h), a->category, muted, ALIGN_END);
		if (it.clicked) {
			shell_launch_new(a->id);
			launcher_open = false;
		}
		y += row_h, shown++;
	}
	if (!n)
		ui_text_in(ui, f, rect(r.x, top + 30, r.w, 30), "Nothing by that name", muted, ALIGN_CENTER);
	/* what the keys do here */
	f32 fy = r.y + r.h - 22, fx = r.x + 22;
	fx = keycaps(ui, fx, fy, "Enter", t);
	ui_text(ui, small, fx + 2, fy + 4, "open", muted);
	fx = keycaps(ui, fx + 44, fy, "Up|Down", t);
	ui_text(ui, small, fx + 2, fy + 4, "choose", muted);
	fx = keycaps(ui, fx + 58, fy, "Esc", t);
	ui_text(ui, small, fx + 2, fy + 4, "close", muted);
}

static void help(Ui *ui, const HiveTheme *t)
{
	static const char *const KEYS[][2] = {
		{ "Super|Return", "Terminal" },
		{ "Super|Space", "Launcher" },
		{ "Super|W", "Close the window" },
		{ "Super|Z", "Zoom the window over the rest" },
		{ "Super|V", "Float the window, or tile it" },
		{ "Super|J", "Turn the split" },
		{ "Super|1-9", "Go to a workspace" },
		{ "Super|Shift|1-9", "Move the window there" },
		{ "Super|Arrows", "Focus a window" },
		{ "Super|Shift|Arrows", "Swap windows" },
		{ "Super|Ctrl|Arrows", "Resize" },
		{ "F11", "Full screen" },
		{ "Super|B", "Browser" },
		{ "Super|F", "Files" },
		{ "Super|N", "Writer" },
		{ "Super|M", "Music" },
		{ "Super|G", "Bridge: games and Windows apps" },
		{ "Super|T", "Activity" },
		{ "Super|Comma", "Settings" },
		{ "Super|Ctrl|Space", "Next theme" },
		{ "Super|Ctrl|V", "What was copied" },
		{ "Super|Shift|S", "Screenshot of an area" },
		{ "Super|Esc", "Lock, sleep, restart, shut down" },
		{ "Super|K", "These keys" },
	};
	int n = (int)ARRAY_LEN(KEYS), rows = (n + 1) / 2;
	Rect r = centre(940, 100 + (f32)rows * 32);
	glass(ui, r, 26, t, 1);
	Font h = ui_font(ui, 18, 700), f = ui_font(ui, 13, 500), small = ui_font(ui, 12, 500);
	ui_text(ui, h, r.x + 30, r.y + 42, "Keys", color_hex(t->fg));
	ui_text(ui, small, r.x + 30 + ui_text_width(ui, h, "Keys") + 12, r.y + 42, "Hive does everything from the keyboard", color_hex(t->muted));
	for (int i = 0; i < n; i++) {
		f32 x = r.x + 30 + (f32)(i / rows) * 455, cy = r.y + 80 + (f32)(i % rows) * 32;
		keycaps(ui, x, cy, KEYS[i][0], t);
		ui_text(ui, f, x + 200, cy + roundf(font_cap_height(f) / 2), KEYS[i][1], color_hex(t->muted));
	}
}

static void power(Ui *ui, const HiveTheme *t)
{
	static const struct {
		const char *label, *icon;
		u32 action;
	} ITEMS[] = {
		{ "Lock", "lock", SESSION_LOCK },
		{ "Sleep", "system-sleep", SESSION_SLEEP },
		{ "Restart", "system-restart", SESSION_RESTART },
		{ "Shut down", "system-shutdown", SESSION_SHUTDOWN },
	};
	int n = (int)ARRAY_LEN(ITEMS);
	Rect r = centre(540, 196);
	glass(ui, r, 30, t, 1);
	Color fg = color_hex(t->fg), accent = color_hex(t->accent);
	f32 w = (r.w - 40) / (f32)n;
	for (int i = 0; i < n; i++) {
		Rect cell = rect(r.x + 20 + (f32)i * w, r.y + 20, w, r.h - 40);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "pw"), cell);
		ui_pop_id(ui);
		f32 cx = cell.x + cell.w / 2, cy = cell.y + 42, rad = 32;
		Rect disc = rect(cx - rad, cy - rad, rad * 2, rad * 2);
		if (it.hovered) {
			ui_shadow(ui, disc, rad, 16, 0, color_with_alpha(accent, 0xa0));
			ui_gradient(ui, disc, rad, color_lighten(accent, 0.18f), color_mix(accent, color_hex(t->accent2), 0.35f));
		} else {
			ui_round(ui, disc, rad, color_hex("#ffffff16"));
			ui_border(ui, disc, rad, 1, color_hex("#ffffff2a"));
		}
		ui_round(ui, rect(cx - rad * 0.55f, cy - rad + 4, rad * 1.1f, rad * 0.5f), rad * 0.25f, color_hex("#ffffff1c")); /* shine */
		ui_icon(ui, rect(cx - 14, cy - 14, 28, 28), ITEMS[i].icon, it.hovered ? color_hex(t->bg) : fg);
		ui_text_in(ui, ui_font(ui, 13, 600), rect(cell.x, cy + rad + 8, cell.w, 22), ITEMS[i].label, fg, ALIGN_CENTER);
		if (it.clicked) {
			power_open = false;
			shell_session_action(ITEMS[i].action);
		}
	}
}

void hive_frame(Ui *ui)
{
	const HiveTheme *t = theme();
	bar(ui, t);
	/* a click outside them closes them (Hive has the pointer while they're
	 * open; nothing is drawn over the screen, which would cost a redraw of
	 * all of it every time) */
	if ((launcher_open || help_open || power_open) && ui_interact(ui, ui_id(ui, "hive-outside"), rect(0, 0, S.sw, S.sh)).clicked)
		close_overlays();
	if (!launcher_open)
		sel_y = -1;
	if (launcher_open)
		launcher(ui, t);
	if (help_open)
		help(ui, t);
	if (power_open)
		power(ui, t);
	toast(ui, t);
	if (hive_animating())
		ui->redraw = true;
}

/* The themes, for Settings to list. */
int hive_theme_count(void)
{
	return HIVE_THEME_COUNT;
}
