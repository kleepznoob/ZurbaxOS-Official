/* Window management: placement, stacking, focus, move/resize, snapping, animation. */
#include "zdisplay.h"

#define APPEAR_SECONDS 0.20f
#define CLOSE_SECONDS 0.16f
#define RECT_ANIM_SECONDS 0.22f
#define MINIMIZE_SECONDS 0.28f

/* Repaints where the window is now and where it was last drawn: an animation
 * can move it further in one frame than the shadow margin covers. */
void window_damage(Window *w)
{
	Rect v = window_visual_rect(w);
	damage_rect(rect(v.x - 50, v.y - 40, v.w + 100, v.h + 100));
	if (!rect_empty(w->shown))
		damage_rect(rect(w->shown.x - 50, w->shown.y - 40, w->shown.w + 100, w->shown.h + 100));
}

void window_configure(Window *w)
{
	if (w->wl) {
		wl_window_configure(w);
		w->configured_once = true;
		return;
	}
	if (!w->client)
		return;
	MsgConfigure m = { .width = (u32)roundf(w->rect.w), .height = (u32)roundf(w->rect.h), .scale = S.scale, .states = w->states };
	if (w == S.focus)
		m.states |= STATE_ACTIVE;
	server_send(w->client, &m, MSG_CONFIGURE, sizeof m, w->client_win, NULL, 0);
	w->configured_once = true;
}

static int stack_index(Window *w)
{
	for (int i = 0; i < S.nwin; i++)
		if (S.windows[i] == w)
			return i;
	return -1;
}

Window *wm_top_window(void)
{
	for (int i = S.nwin - 1; i >= 0; i--) {
		Window *w = S.windows[i];
		if (!w->closing && !w->minimized && w->mapped && !(w->flags & WIN_POPUP))
			return w;
	}
	return NULL;
}

static Rect place_new(Window *w, u32 width, u32 height)
{
	Rect wa = work_area();
	f32 ww = minf((f32)width, wa.w - 40), wh = minf((f32)height, wa.h - 40);
	ww = maxf(ww, (f32)w->min_w), wh = maxf(wh, (f32)w->min_h);
	if (w->parent && (w->flags & WIN_DIALOG)) {
		Rect p = w->parent->rect;
		return rect(p.x + (p.w - ww) / 2, p.y + (p.h - wh) / 3, ww, wh);
	}
	/* Centre, then cascade away from windows of the same app already there. */
	f32 x = wa.x + (wa.w - ww) / 2, y = wa.y + (wa.h - wh) / 2;
	for (int tries = 0; tries < 8; tries++) {
		bool clash = false;
		for (int i = 0; i < S.nwin; i++) {
			Window *o = S.windows[i];
			if (o != w && !o->closing && fabsf(o->rect.x - x) < 8 && fabsf(o->rect.y - y) < 8)
				clash = true;
		}
		if (!clash)
			break;
		x += 28, y += 28;
	}
	x = clampf(x, wa.x, wa.x + wa.w - ww);
	y = clampf(y, wa.y, wa.y + wa.h - wh);
	return rect(roundf(x), roundf(y), ww, wh);
}

Window *window_create(Client *c, const MsgWinCreate *m)
{
	if (S.nwin >= MAX_WINDOWS)
		return NULL;
	Window *w = NEW(Window);
	w->id = ++S.next_window_id;
	w->client = c;
	w->client_win = m->h.win;
	w->flags = m->flags;
	w->front = -1;
	w->release_after_frame = -1;
	w->memfd = -1;
	w->min_w = MAX(m->min_width, 120u);
	w->min_h = MAX(m->min_height, 80u);
	w->radius = m->corner_radius > 0 ? m->corner_radius : 10;
	str_copy(w->title, m->title, sizeof w->title);
	str_copy(w->app_id, c->app_id, sizeof w->app_id);
	for (int i = 0; i < S.nwin; i++)
		if (S.windows[i]->client == c && S.windows[i]->client_win == m->parent && m->parent)
			w->parent = S.windows[i];
	if ((w->flags & WIN_POPUP) && w->parent)
		w->rect = rect(w->parent->rect.x + (f32)m->x, w->parent->rect.y + (f32)m->y, (f32)m->width, (f32)m->height);
	else
		w->rect = place_new(w, m->width ? m->width : 800, m->height ? m->height : 560);
	if (w->flags & WIN_FULLSCREEN_START) {
		w->saved = w->rect;
		w->rect = rect(0, 0, S.sw, S.sh);
		w->states |= STATE_FULLSCREEN;
	}
	hive_window_added(w);
	w->appear = S.set.reduce_motion ? 1 : 0;
	S.windows[S.nwin++] = w;
	if (!c->main_window)
		c->main_window = w;
	window_focus(w);
	window_configure(w);
	return w;
}

/* A window for a Wayland program's surface (wayland.c). Wayland dialogs use
 * their parent only to be placed; popups stay attached to it. */
Window *window_create_wl(void *surface, const char *title, const char *app_id, f32 width, f32 height, u32 flags, Window *parent, f32 x, f32 y)
{
	if (S.nwin >= MAX_WINDOWS)
		return NULL;
	Window *w = NEW(Window);
	w->id = ++S.next_window_id;
	w->wl = surface;
	w->flags = flags;
	w->front = -1;
	w->release_after_frame = -1;
	w->memfd = -1;
	w->min_w = w->min_h = 32;
	w->radius = 10;
	str_copy(w->title, title, sizeof w->title);
	str_copy(w->app_id, app_id, sizeof w->app_id);
	w->parent = parent;
	if ((flags & WIN_POPUP) && parent)
		w->rect = rect(parent->rect.x + x, parent->rect.y + y, width, height);
	else
		w->rect = place_new(w, (u32)width, (u32)height);
	if (!(flags & WIN_POPUP))
		w->parent = NULL;
	hive_window_added(w);
	w->appear = S.set.reduce_motion ? 1 : 0;
	w->mapped = true;
	S.windows[S.nwin++] = w;
	wl_window_attach(w);
	if (!(flags & WIN_POPUP)) {
		window_focus(w);
		window_configure(w);
	}
	shell_mark_dirty();
	window_damage(w);
	return w;
}

static void free_window(Window *w)
{
	if (w->map)
		sys_munmap(w->map, w->map_size);
	if (w->memfd >= 0)
		sys_close(w->memfd);
	canvas_free(w->last_frame);
	mem_free(w);
}

void window_destroy(Window *w, bool animate)
{
	shell_mark_dirty(); /* the dock and panel reflect window state */
	if (w->closing)
		return;
	hive_window_removed(w);
	window_damage(w);
	dnd_window_gone(w);
	if (S.focus == w)
		S.focus = NULL;
	if (S.pointer_window == w) {
		/* its cursor goes with it (a hand left over from its close button) */
		S.pointer_window = NULL;
		S.cursor = CURSOR_DEFAULT;
		cursor_update();
	}
	if (S.grab_window == w) {
		S.grab_window = NULL;
		S.grab = GRAB_NONE;
	}
	/* Children go too. */
	for (int i = S.nwin - 1; i >= 0; i--)
		if (S.windows[i]->parent == w)
			window_destroy(S.windows[i], false);
	if (w->client && w->client->main_window == w)
		w->client->main_window = NULL;
	bool can_animate = animate && !S.set.reduce_motion && w->mapped && !w->minimized && (w->wl || (w->front >= 0 && w->map));
	if (w->wl) {
		/* a Wayland surface: keep its picture, then the surface lets go of the window */
		w->last_frame = wl_window_gone(w, can_animate);
		can_animate = w->last_frame != NULL;
	}
	if (can_animate) {
		/* Keep a private copy of the last frame: the app may already be gone. */
		if (!w->last_frame) {
			Canvas src = canvas_wrap(w->map + (usize)w->front * w->slot_bytes, (int)w->bw, (int)w->bh, (int)w->stride);
			w->last_frame = canvas_new((int)w->bw, (int)w->bh);
			copy_pixels(w->last_frame, &src, 0, 0, 0, 0, (int)w->bw, (int)w->bh);
		}
		w->closing = true;
		w->client = NULL;
	} else {
		int idx = stack_index(w);
		if (idx >= 0) {
			memmove(&S.windows[idx], &S.windows[idx + 1], sizeof(Window *) * (usize)(S.nwin - idx - 1));
			S.nwin--;
		}
		free_window(w);
	}
	if (!S.focus)
		window_focus(wm_top_window());
}

void window_raise(Window *w)
{
	int idx = stack_index(w);
	if (idx < 0)
		return;
	memmove(&S.windows[idx], &S.windows[idx + 1], sizeof(Window *) * (usize)(S.nwin - idx - 1));
	S.windows[S.nwin - 1] = w;
	/* Keep this window's popups and dialogs above it. */
	for (int i = 0; i < S.nwin - 1; i++) {
		Window *c = S.windows[i];
		if (c->parent == w) {
			memmove(&S.windows[i], &S.windows[i + 1], sizeof(Window *) * (usize)(S.nwin - i - 1));
			S.windows[S.nwin - 1] = c;
			i--;
		}
	}
	window_damage(w);
}

void window_focus(Window *w)
{
	shell_mark_dirty(); /* the dock and panel reflect window state */
	if (w == S.focus)
		return;
	Window *old = S.focus;
	S.focus = w;
	if (old && !old->closing) {
		server_focus_event(old, false);
		window_configure(old);
		window_damage(old);
	}
	if (w) {
		desktop_unfocus();
		w->focus_serial = ++S.focus_serial;
		server_focus_event(w, true);
		if (w->configured_once)
			window_configure(w);
		window_damage(w);
	}
}

void window_set_rect(Window *w, Rect r, bool animate)
{
	r.w = maxf(r.w, (f32)w->min_w);
	r.h = maxf(r.h, (f32)w->min_h);
	r.x = roundf(r.x), r.y = roundf(r.y), r.w = roundf(r.w), r.h = roundf(r.h);
	window_damage(w);
	bool resized = r.w != w->rect.w || r.h != w->rect.h;
	if (animate && !S.set.reduce_motion) {
		w->anim_from = window_visual_rect(w);
		w->anim_to = r;
		w->anim_t0 = S.now;
		w->anim_dur = RECT_ANIM_SECONDS;
		w->animating_rect = true;
	}
	w->rect = r;
	if (resized)
		window_configure(w);
	window_damage(w);
}

void window_fullscreen(Window *w, bool on)
{
	if (on == !!(w->states & STATE_FULLSCREEN))
		return;
	if (on) {
		w->saved = w->rect;
		w->states |= STATE_FULLSCREEN;
		window_set_rect(w, rect(0, 0, S.sw, S.sh), false);
	} else {
		w->states &= ~(u32)STATE_FULLSCREEN;
		window_set_rect(w, w->saved, false);
	}
	window_configure(w);
	damage_all();
	game_mode_update();
}

void window_maximize(Window *w, bool on)
{
	shell_mark_dirty(); /* the dock and panel reflect window state */
	if (hive_tiled(w)) {
		hive_zoom(w, on);
		return;
	}
	if (!(w->flags & WIN_RESIZABLE) && on)
		return;
	if (on && !(w->states & STATE_MAXIMIZED)) {
		if (!(w->states & (STATE_TILED_LEFT | STATE_TILED_RIGHT)))
			w->saved = w->rect;
		w->states = (w->states & ~(u32)(STATE_TILED_LEFT | STATE_TILED_RIGHT)) | STATE_MAXIMIZED;
		window_set_rect(w, work_area(), true);
	} else if (!on && (w->states & (STATE_MAXIMIZED | STATE_TILED_LEFT | STATE_TILED_RIGHT))) {
		w->states &= ~(u32)(STATE_MAXIMIZED | STATE_TILED_LEFT | STATE_TILED_RIGHT);
		window_set_rect(w, w->saved, true);
	}
	window_configure(w);
}

void window_tile(Window *w, int side)
{
	shell_mark_dirty(); /* the dock and panel reflect window state */
	if (!(w->flags & WIN_RESIZABLE) || hive_tiled(w))
		return;
	if (!(w->states & (STATE_MAXIMIZED | STATE_TILED_LEFT | STATE_TILED_RIGHT)))
		w->saved = w->rect;
	Rect wa = work_area();
	w->states &= ~(u32)(STATE_MAXIMIZED | STATE_TILED_LEFT | STATE_TILED_RIGHT);
	w->states |= side < 0 ? STATE_TILED_LEFT : STATE_TILED_RIGHT;
	Rect r = side < 0 ? rect(wa.x, wa.y, floorf(wa.w / 2), wa.h) : rect(wa.x + floorf(wa.w / 2), wa.y, ceilf(wa.w / 2), wa.h);
	window_set_rect(w, r, true);
	window_configure(w);
}

/* The work area changed (the top bar or the taskbar came, went or changed
 * size): maximized and tiled windows follow it. */
void wm_refit(void)
{
	if (hive_on()) {
		hive_relayout(true);
		return;
	}
	Rect wa = work_area();
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		if (w->closing || (w->flags & WIN_POPUP) || (w->states & STATE_FULLSCREEN))
			continue;
		Rect r;
		if (w->states & STATE_MAXIMIZED)
			r = wa;
		else if (w->states & STATE_TILED_LEFT)
			r = rect(wa.x, wa.y, floorf(wa.w / 2), wa.h);
		else if (w->states & STATE_TILED_RIGHT)
			r = rect(wa.x + floorf(wa.w / 2), wa.y, ceilf(wa.w / 2), wa.h);
		else
			continue;
		window_set_rect(w, r, true);
		window_configure(w);
	}
}

void window_minimize(Window *w, bool on)
{
	shell_mark_dirty(); /* the dock and panel reflect window state */
	if (on == w->minimized)
		return;
	w->minimized = on;
	w->states = on ? (w->states | STATE_MINIMIZED) : (w->states & ~(u32)STATE_MINIMIZED);
	hive_window_minimized(w, on);
	if (S.set.reduce_motion)
		w->minimize_k = on ? 1 : 0;
	window_damage(w);
	if (on && S.focus == w) {
		S.focus = NULL;
		server_focus_event(w, false);
		window_configure(w);
		Window *next = NULL;
		for (int i = S.nwin - 1; i >= 0; i--) {
			Window *o = S.windows[i];
			if (o != w && !o->closing && !o->minimized && o->mapped && !(o->flags & WIN_POPUP)) {
				next = o;
				break;
			}
		}
		window_focus(next);
	} else if (!on) {
		window_raise(w);
		window_focus(w);
	}
}

void window_close_request(Window *w)
{
	if (w->wl) {
		wl_window_close(w);
		return;
	}
	if (!w->client)
		return;
	MsgHeader m = { 0 };
	server_send(w->client, &m, MSG_CLOSE_REQUEST, sizeof m, w->client_win, NULL, 0);
}

void wm_show_desktop(bool on)
{
	S.show_desktop = on;
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		if (!w->closing && !(w->flags & WIN_POPUP))
			window_minimize(w, on);
	}
}

/* ---- Geometry with animation ---------------------------------------------------------- */

bool pixel_motion(void)
{
	return S.set.pixel_style && !S.set.reduce_motion;
}

Rect window_visual_rect(const Window *w)
{
	Rect r = w->rect;
	if (w->animating_rect) {
		f32 t = ease_out_cubic(clampf((f32)((S.now - w->anim_t0) / w->anim_dur), 0, 1));
		r = rect(lerpf(w->anim_from.x, w->anim_to.x, t), lerpf(w->anim_from.y, w->anim_to.y, t),
			 lerpf(w->anim_from.w, w->anim_to.w, t), lerpf(w->anim_from.h, w->anim_to.h, t));
	}
	r.x += hive_window_offset(w);
	f32 s = 1;
	/* (in pixels, coming and going is done block by block, in place) */
	if (w->appear < 1 && !pixel_motion())
		s *= 0.94f + 0.06f * ease_out_cubic(w->appear);
	if (w->closing && !pixel_motion())
		s *= 1 - 0.05f * w->vanish;
	if (s != 1) {
		f32 nw = r.w * s, nh = r.h * s;
		r = rect(r.x + (r.w - nw) / 2, r.y + (r.h - nh) / 2 + (1 - s) * 40, nw, nh);
	}
	if (w->minimize_k > 0) {
		Rect dock = hive_on() ? hive_tray_rect(w) : shell_dock_icon_rect(w->app_id);
		f32 k = ease_in_out_cubic(w->minimize_k);
		r = rect(lerpf(r.x, dock.x, k), lerpf(r.y, dock.y, k), lerpf(r.w, dock.w, k), lerpf(r.h, dock.h, k));
	}
	return r;
}

static f32 step_toward(f32 v, f32 target, f32 dt, f32 seconds)
{
	f32 d = dt / seconds;
	return v < target ? minf(target, v + d) : maxf(target, v - d);
}

void wm_animate(f64 dt_d)
{
	f32 dt = (f32)clampf((f32)dt_d, 0, 0.1f);
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		bool moving = false;
		if (w->mapped && w->appear < 1) {
			w->appear = step_toward(w->appear, 1, dt, pixel_motion() ? 0.30f : APPEAR_SECONDS);
			moving = true;
		}
		if (w->closing) {
			w->vanish = step_toward(w->vanish, 1, dt, pixel_motion() ? 0.26f : CLOSE_SECONDS);
			moving = true;
		}
		f32 mk = w->minimized ? 1.0f : 0.0f;
		if (w->minimize_k != mk) {
			window_damage(w);
			w->minimize_k = step_toward(w->minimize_k, mk, dt, MINIMIZE_SECONDS);
			moving = true;
		}
		if (w->animating_rect) {
			moving = true;
			if (S.now - w->anim_t0 >= w->anim_dur)
				w->animating_rect = false;
		}
		if (moving)
			window_damage(w);
	}
	/* Free windows whose closing animation finished. */
	for (int i = S.nwin - 1; i >= 0; i--) {
		Window *w = S.windows[i];
		if (w->closing && w->vanish >= 1) {
			window_damage(w);
			memmove(&S.windows[i], &S.windows[i + 1], sizeof(Window *) * (usize)(S.nwin - i - 1));
			S.nwin--;
			free_window(w);
		}
	}
	f32 target = S.snap_zone ? 1.0f : 0.0f;
	if (S.snap_k != target) {
		damage_rect(S.snap_preview);
		S.snap_k = step_toward(S.snap_k, target, dt, 0.15f);
		damage_rect(S.snap_preview);
	}
	hive_animate();
}

bool wm_any_animation(void)
{
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		if ((w->mapped && w->appear < 1) || w->closing || w->animating_rect || w->minimize_k != (w->minimized ? 1.0f : 0.0f))
			return true;
	}
	return S.snap_k != (S.snap_zone ? 1.0f : 0.0f) || hive_animating();
}

/* ---- Hit testing and pointer handling ----------------------------------------------------- */

Window *window_at(f32 x, f32 y, u32 *edges)
{
	*edges = 0;
	/* in Hive, what floats is above the tiles whatever the stack says */
	for (int pass = hive_on() ? 0 : 1; pass < 2; pass++)
	for (int i = S.nwin - 1; i >= 0; i--) {
		Window *w = S.windows[i];
		if (w->closing || w->minimized || !w->mapped || hive_window_hidden(w) || (pass == 0 && hive_tiled(w)))
			continue;
		Rect r = w->rect;
		if (rect_contains(r, x, y))
			return w;
		bool resizable = (w->flags & WIN_RESIZABLE) && !(w->states & (STATE_MAXIMIZED | STATE_FULLSCREEN)) && !hive_tiled(w);
		if (resizable && rect_contains(rect_inset(r, -RESIZE_MARGIN, -RESIZE_MARGIN), x, y)) {
			u32 e = 0;
			if (x < r.x + 12) e |= EDGE_LEFT;
			if (x > r.x + r.w - 12) e |= EDGE_RIGHT;
			if (y < r.y + 12) e |= EDGE_TOP;
			if (y > r.y + r.h - 12) e |= EDGE_BOTTOM;
			*edges = e;
			return w;
		}
	}
	return NULL;
}

static u32 cursor_for_edges(u32 e)
{
	if ((e & EDGE_TOP && e & EDGE_LEFT) || (e & EDGE_BOTTOM && e & EDGE_RIGHT))
		return CURSOR_RESIZE_NWSE;
	if ((e & EDGE_TOP && e & EDGE_RIGHT) || (e & EDGE_BOTTOM && e & EDGE_LEFT))
		return CURSOR_RESIZE_NESW;
	if (e & (EDGE_LEFT | EDGE_RIGHT))
		return CURSOR_RESIZE_EW;
	return CURSOR_RESIZE_NS;
}

void wm_begin_move(Window *w)
{
	/* tiles stay in their place: Super+Shift+arrows swaps them */
	if (!w || (w->states & STATE_FULLSCREEN) || hive_tiled(w))
		return;
	S.grab = GRAB_MOVE;
	S.grab_window = w;
	S.grab_x = S.px, S.grab_y = S.py;
	S.grab_rect = w->rect;
	S.cursor = CURSOR_DEFAULT;
}

void wm_begin_resize(Window *w, u32 edges)
{
	if (!w || !(w->flags & WIN_RESIZABLE))
		return;
	S.grab = GRAB_RESIZE;
	S.grab_window = w;
	S.grab_edges = edges;
	S.grab_x = S.px, S.grab_y = S.py;
	S.grab_rect = w->rect;
	S.cursor = cursor_for_edges(edges);
}

static void update_snap(void)
{
	int zone = 0;
	Rect wa = work_area();
	if (S.py <= wa.y + 4)
		zone = 1;
	else if (S.px <= 2)
		zone = 2;
	else if (S.px >= S.sw - 3)
		zone = 3;
	if (!(S.grab_window->flags & WIN_RESIZABLE))
		zone = 0;
	if (zone != S.snap_zone) {
		damage_rect(S.snap_preview);
		S.snap_zone = zone;
		if (zone == 1)
			S.snap_preview = wa;
		else if (zone == 2)
			S.snap_preview = rect(wa.x, wa.y, floorf(wa.w / 2), wa.h);
		else if (zone == 3)
			S.snap_preview = rect(wa.x + floorf(wa.w / 2), wa.y, ceilf(wa.w / 2), wa.h);
		damage_rect(S.snap_preview);
	}
}

void wm_pointer_motion(void)
{
	if (desktop_grabbing()) {
		desktop_pointer(POINTER_MOTION, 0);
		return;
	}
	Window *w = S.grab_window;
	if (S.grab == GRAB_MOVE && w) {
		f32 dx = S.px - S.grab_x, dy = S.py - S.grab_y;
		if (w->states & (STATE_MAXIMIZED | STATE_TILED_LEFT | STATE_TILED_RIGHT)) {
			/* Dragging a maximized window restores it under the pointer. */
			if (fabsf(dx) + fabsf(dy) < 12)
				return;
			f32 rel = (S.grab_x - w->rect.x) / w->rect.w;
			w->states &= ~(u32)(STATE_MAXIMIZED | STATE_TILED_LEFT | STATE_TILED_RIGHT);
			Rect r = w->saved;
			r.x = S.px - rel * r.w;
			r.y = S.py - 20;
			window_set_rect(w, r, false);
			window_configure(w);
			S.grab_rect = w->rect;
			S.grab_x = S.px, S.grab_y = S.py;
			return;
		}
		Rect r = S.grab_rect;
		r.x += dx;
		r.y = maxf(work_area().y, r.y + dy);
		window_damage(w);
		w->rect.x = roundf(r.x);
		w->rect.y = roundf(r.y);
		window_damage(w);
		update_snap();
		return;
	}
	if (S.grab == GRAB_RESIZE && w) {
		f32 dx = S.px - S.grab_x, dy = S.py - S.grab_y;
		Rect r = S.grab_rect;
		if (S.grab_edges & EDGE_LEFT) {
			f32 nw = maxf((f32)w->min_w, r.w - dx);
			r.x += r.w - nw;
			r.w = nw;
		}
		if (S.grab_edges & EDGE_RIGHT)
			r.w = maxf((f32)w->min_w, r.w + dx);
		if (S.grab_edges & EDGE_TOP) {
			f32 nh = maxf((f32)w->min_h, r.h - dy);
			r.y += r.h - nh;
			r.h = nh;
		}
		if (S.grab_edges & EDGE_BOTTOM)
			r.h = maxf((f32)w->min_h, r.h + dy);
		window_set_rect(w, r, false);
		return;
	}
	/* Not grabbing: route motion to the window under the pointer. */
	hive_pointer_motion();
	u32 edges;
	Window *under = S.grab_window ? S.grab_window : window_at(S.px, S.py, &edges);
	if (!S.grab_window && under && edges) {
		S.cursor = cursor_for_edges(edges);
		if (S.pointer_window && S.pointer_window != under)
			server_pointer_event(S.pointer_window, POINTER_LEAVE, 0);
		S.pointer_window = NULL;
		return;
	}
	desktop_pointer(under ? POINTER_LEAVE : POINTER_MOTION, 0);
	if (under != S.pointer_window) {
		if (S.pointer_window)
			server_pointer_event(S.pointer_window, POINTER_LEAVE, 0);
		S.pointer_window = under;
		if (under)
			server_pointer_event(under, POINTER_ENTER, 0);
	}
	if (under) {
		S.cursor = under->cursor;
		server_pointer_event(under, POINTER_MOTION, 0);
	} else {
		S.cursor = CURSOR_DEFAULT;
	}
}

void wm_pointer_button(u32 button, bool pressed)
{
	if (!pressed && desktop_grabbing()) {
		desktop_pointer(POINTER_UP, button);
		return;
	}
	if (!pressed) {
		if (S.grab == GRAB_MOVE && S.grab_window) {
			Window *w = S.grab_window;
			int zone = S.snap_zone;
			S.snap_zone = 0;
			if (zone == 1) {
				w->saved = S.grab_rect;
				window_maximize(w, true);
			} else if (zone == 2 || zone == 3) {
				w->saved = S.grab_rect;
				window_tile(w, zone == 2 ? -1 : 1);
			}
		}
		if (S.grab != GRAB_NONE) {
			S.grab = GRAB_NONE;
			S.grab_window = NULL;
			S.cursor = CURSOR_DEFAULT;
			wm_pointer_motion();
			return;
		}
		if (S.grab_window) {
			server_pointer_event(S.grab_window, POINTER_UP, button);
			if (S.buttons == 0)
				S.grab_window = NULL;
		}
		return;
	}
	u32 edges;
	Window *w = window_at(S.px, S.py, &edges);
	wl_pointer_press(w);
	if (!w) {
		/* Click on the desktop: drop focus from popups; the icons have it. */
		for (int i = S.nwin - 1; i >= 0; i--) {
			Window *p = S.windows[i];
			if ((p->flags & WIN_POPUP) && p->client) {
				MsgHeader m = { 0 };
				server_send(p->client, &m, MSG_POPUP_DONE, sizeof m, p->client_win, NULL, 0);
			}
		}
		desktop_pointer(POINTER_DOWN, button);
		return;
	}
	if (!(w->flags & WIN_POPUP)) {
		/* Close other apps' popups when clicking elsewhere. */
		for (int i = S.nwin - 1; i >= 0; i--) {
			Window *p = S.windows[i];
			if ((p->flags & WIN_POPUP) && p->parent != w && p->client) {
				MsgHeader m = { 0 };
				server_send(p->client, &m, MSG_POPUP_DONE, sizeof m, p->client_win, NULL, 0);
			}
		}
		window_raise(w);
		window_focus(w);
	}
	if (edges && button == BUTTON_LEFT) {
		wm_begin_resize(w, edges);
		return;
	}
	/* Super + drag moves any window. */
	if ((S.mods & MOD_SUPER) && button == BUTTON_LEFT) {
		wm_begin_move(w);
		return;
	}
	S.grab_window = w;
	server_pointer_event(w, POINTER_DOWN, button);
}
