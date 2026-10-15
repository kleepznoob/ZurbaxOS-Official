/* The display protocol, server side: clients, windows, buffers and events. */
#include "zdisplay.h"

static void on_client(Loop *loop, int fd, u32 events, void *data);

void server_send(Client *c, void *msg, u16 type, usize size, u32 win, const int *fds, int nfds)
{
	if (!c || c->fd < 0)
		return;
	MsgHeader *h = msg;
	h->type = type;
	h->size = (u16)size;
	h->win = win;
	long r = unix_send(c->fd, msg, size, fds, nfds);
	if (r < 0 && r != -EAGAIN)
		eprint("zdisplay: send to %s failed: %s\n", c->app_id, err_str(r));
}

static ThemeInfo current_theme(void)
{
	ThemeInfo t = { 0 };
	t.dark = S.set.dark;
	t.reduce_motion = S.set.reduce_motion;
	t.glass = S.set.glass;
	t.accent = S.set.accent;
	t.text_scale = S.set.text_scale > 0 ? S.set.text_scale : 1;
	return t;
}

void server_broadcast_theme(void)
{
	S.theme = current_theme();
	MsgTheme m = { .theme = S.theme };
	for (int i = 0; i < S.nclients; i++)
		server_send(S.clients[i], &m, MSG_THEME, sizeof m, 0, NULL, 0);
}

static Window *find_window(Client *c, u32 client_win)
{
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		if (w->client == c && w->client_win == client_win && !w->closing)
			return w;
	}
	return NULL;
}

static void handle_buffer(Window *w, const MsgWinBuffer *m, int fd)
{
	if (w->map)
		sys_munmap(w->map, w->map_size);
	if (w->memfd >= 0)
		sys_close(w->memfd);
	w->map = NULL;
	w->memfd = fd;
	w->bw = m->width, w->bh = m->height, w->stride = m->stride, w->slot_bytes = m->slot_bytes;
	w->map_size = (usize)m->slot_bytes * 2;
	w->front = -1;
	w->release_after_frame = -1;
	if ((usize)m->stride * m->height * 4 > m->slot_bytes || m->width > 16384 || m->height > 16384)
		return; /* nonsense sizes from a buggy client: ignore the buffer */
	void *p = sys_mmap(NULL, w->map_size, PROT_READ, MAP_SHARED, fd, 0);
	if ((unsigned long)p > -4096ul) {
		eprint("zdisplay: cannot map buffer for %s\n", w->app_id);
		return;
	}
	w->map = p;
}

static void handle_commit(Window *w, const MsgWinCommit *m)
{
	if (!w->map || m->slot > 1)
		return;
	if (w->front >= 0 && w->front != (int)m->slot) {
		/* The old slot may be reused once the new one is on screen. */
		if (w->release_after_frame >= 0 && w->release_after_frame != (int)m->slot) {
			MsgRelease r = { .slot = (u32)w->release_after_frame };
			server_send(w->client, &r, MSG_RELEASE, sizeof r, w->client_win, NULL, 0);
		}
		w->release_after_frame = w->front;
	}
	w->front = (int)m->slot;
	w->want_frame = true;
	/* Only show the window once its buffer matches its size. */
	u32 want_w = (u32)roundf(w->rect.w * S.scale), want_h = (u32)roundf(w->rect.h * S.scale);
	if (!w->mapped && ABS((int)w->bw - (int)want_w) <= 1 && ABS((int)w->bh - (int)want_h) <= 1) {
		w->mapped = true;
		shell_mark_dirty();
	}
	Rect v = window_visual_rect(w);
	if (m->dw > 0)
		damage_physical(rect(v.x * S.scale + (f32)m->dx, v.y * S.scale + (f32)m->dy, (f32)m->dw, (f32)m->dh));
	else
		damage_rect(rect(v.x - 40, v.y - 30, v.w + 80, v.h + 80));
}

static void handle_message(Client *c, const u8 *buf, usize len, int *fds, int nfds)
{
	const MsgHeader *h = (const MsgHeader *)buf;
	int used_fd = -1;
	if (len < sizeof *h)
		goto done;
	Window *w = h->win ? find_window(c, h->win) : NULL;
	switch (h->type) {
	case MSG_HELLO: {
		if (len < sizeof(MsgHello))
			break;
		const MsgHello *m = (const MsgHello *)buf;
		str_copy(c->app_id, m->app_id, sizeof c->app_id);
		c->pid = (int)m->pid;
		c->hello = true;
		MsgWelcome wm = { .version = ZUI_PROTOCOL_VERSION, .screen_width = (u32)S.sw, .screen_height = (u32)S.sh,
				  .scale = S.scale, .theme = current_theme(), .uid = S.set.uid };
		str_copy(wm.user_name, S.set.full_name[0] ? S.set.full_name : S.set.user_name, sizeof wm.user_name);
		str_copy(wm.home, S.set.home, sizeof wm.home);
		server_send(c, &wm, MSG_WELCOME, sizeof wm, 0, NULL, 0);
		break;
	}
	case MSG_WIN_CREATE:
		if (len >= sizeof(MsgWinCreate) && !find_window(c, h->win))
			window_create(c, (const MsgWinCreate *)buf);
		break;
	case MSG_WIN_DESTROY:
		if (w)
			window_destroy(w, true);
		break;
	case MSG_WIN_BUFFER:
		if (w && nfds > 0 && len >= sizeof(MsgWinBuffer)) {
			used_fd = fds[0];
			handle_buffer(w, (const MsgWinBuffer *)buf, fds[0]);
		}
		break;
	case MSG_WIN_COMMIT:
		if (w && len >= sizeof(MsgWinCommit))
			handle_commit(w, (const MsgWinCommit *)buf);
		break;
	case MSG_WIN_TITLE:
		if (w && len >= sizeof(MsgWinTitle))
			str_copy(w->title, ((const MsgWinTitle *)buf)->title, sizeof w->title);
		break;
	case MSG_WIN_GLASS:
		if (w && len >= sizeof(MsgWinGlass)) {
			const MsgWinGlass *m = (const MsgWinGlass *)buf;
			w->nglass = (int)MIN(m->count, (u32)WIN_GLASS_MAX);
			for (int i = 0; i < w->nglass; i++)
				w->glass[i] = rect(m->rects[i][0], m->rects[i][1], m->rects[i][2], m->rects[i][3]);
			window_damage(w);
		}
		break;
	case MSG_WIN_ACTION: {
		if (!w || len < sizeof(MsgWinAction))
			break;
		const MsgWinAction *m = (const MsgWinAction *)buf;
		switch (m->action) {
		case ACTION_MAXIMIZE: window_maximize(w, true); break;
		case ACTION_UNMAXIMIZE: window_maximize(w, false); break;
		case ACTION_TOGGLE_MAXIMIZE:
			window_maximize(w, !(w->states & (STATE_MAXIMIZED | STATE_TILED_LEFT | STATE_TILED_RIGHT)));
			break;
		case ACTION_MINIMIZE: window_minimize(w, true); break;
		case ACTION_CLOSE: window_close_request(w); break;
		case ACTION_FULLSCREEN: window_fullscreen(w, true); break;
		case ACTION_UNFULLSCREEN: window_fullscreen(w, false); break;
		case ACTION_RAISE:
			if (w->minimized)
				window_minimize(w, false);
			window_raise(w);
			window_focus(w);
			break;
		}
		break;
	}
	case MSG_WIN_MOVE:
	case MSG_WIN_RESIZE:
		if (w && S.buttons) {
			/* The app saw a press; finish it before the server takes the pointer. */
			server_pointer_event(w, POINTER_UP, BUTTON_LEFT);
			if (h->type == MSG_WIN_MOVE)
				wm_begin_move(w);
			else if (len >= sizeof(MsgWinMove))
				wm_begin_resize(w, ((const MsgWinMove *)buf)->edges);
		}
		break;
	case MSG_CURSOR:
		if (w && len >= sizeof(MsgCursor)) {
			w->cursor = ((const MsgCursor *)buf)->shape;
			if (S.pointer_window == w && S.grab == GRAB_NONE) {
				S.cursor = w->cursor;
				cursor_update();
			}
		}
		break;
	case MSG_RUMBLE:
		/* only the app in front may shake the controllers */
		if (len >= sizeof(MsgRumble) && S.focus && S.focus->client == c && S.set.pad_rumble) {
			const MsgRumble *m = (const MsgRumble *)buf;
			input_pad_rumble(m->pad, (f32)m->strong / 65535, (f32)m->weak / 65535, m->ms);
		}
		break;
	case MSG_NOTIFY:
		if (len >= sizeof(MsgNotify)) {
			const MsgNotify *m = (const MsgNotify *)buf;
			shell_notify(c->app_id, m->icon[0] ? m->icon : c->app_id, m->title, m->body);
		}
		break;
	case MSG_CLIPBOARD_SET:
		if (nfds > 0 && len >= sizeof(MsgClipboard)) {
			const MsgClipboard *m = (const MsgClipboard *)buf;
			if (m->size <= (32u << 20)) {
				mem_free(S.clipboard);
				S.clipboard = mem_alloc(m->size + 1);
				long n = sys_pread(fds[0], S.clipboard, m->size, 0);
				S.clipboard_len = n > 0 ? (usize)n : 0;
				S.clipboard[S.clipboard_len] = 0;
				str_copy(S.clipboard_mime, m->mime, sizeof S.clipboard_mime);
				clip_history_add(S.clipboard_mime, S.clipboard, S.clipboard_len);
				wl_clipboard_changed();
			}
		}
		break;
	case MSG_CLIPBOARD_GET: {
		/* Files asked for: whatever files are there, or nothing. Text asked
		 * for: the text, or the files' paths. */
		bool want_files = len >= sizeof(MsgClipboard) && str_eq(((const MsgClipboard *)buf)->mime, CLIPBOARD_FILES);
		bool have_files = S.clipboard && str_eq(S.clipboard_mime, CLIPBOARD_FILES);
		const char *data = S.clipboard;
		usize n = S.clipboard_len;
		if (want_files && !have_files)
			data = "", n = 0;
		else if (!want_files && have_files) {
			const char *nl = str_chr(S.clipboard, '\n');
			data = nl ? nl + 1 : "";
			n = strlen(data);
		}
		if (!data && !want_files)
			break;
		int fd = (int)sys_memfd_create("clipboard", MFD_CLOEXEC);
		if (fd >= 0) {
			if (n)
				write_all(fd, data, n);
			MsgClipboard r = { .size = (u32)n };
			str_copy(r.mime, want_files ? CLIPBOARD_FILES : have_files ? "text/plain" : S.clipboard_mime, sizeof r.mime);
			server_send(c, &r, MSG_CLIPBOARD_DATA, sizeof r, h->win, &fd, 1);
			sys_close(fd);
		}
		break;
	}
	case MSG_DRAG_START:
		if (w && nfds > 0 && len >= sizeof(MsgDragStart)) {
			const MsgDragStart *m = (const MsgDragStart *)buf;
			if (!m->size || m->size > (16u << 20))
				break;
			char *paths = mem_alloc(m->size + 1);
			long got = sys_pread(fds[0], paths, m->size, 0);
			paths[got > 0 ? got : 0] = 0;
			Canvas *thumb = NULL;
			if (nfds > 1 && m->thumb_w && m->thumb_h && m->thumb_w <= 512 && m->thumb_h <= 512) {
				thumb = canvas_new((int)m->thumb_w, (int)m->thumb_h);
				for (u32 y = 0; y < m->thumb_h; y++)
					sys_pread(fds[1], canvas_row(thumb, (int)y), (usize)m->thumb_w * 4, (i64)((u64)y * m->thumb_w * 4));
			}
			dnd_start(w, paths, (int)m->count, thumb);
			mem_free(paths);
		}
		break;
	case MSG_LAUNCH:
		if (len >= sizeof(MsgLaunch)) {
			const MsgLaunch *m = (const MsgLaunch *)buf;
			shell_launch(m->app_id, m->arg);
		}
		break;
	case MSG_SETTINGS_CHANGED: {
		char old_wall[128];
		str_copy(old_wall, S.set.wallpaper, sizeof old_wall);
		Rect old_area = work_area();
		bool was_hive = hive_on(), was_pixel = S.set.pixel_style;
		settings_load();
		settings_apply();
		hive_settings_changed(was_hive);
		if (was_pixel != S.set.pixel_style)
			compose_reload_cursors();
		if (!str_eq(old_wall, S.set.wallpaper))
			wallpaper_reload();
		Rect area = work_area();
		if (memcmp(&old_area, &area, sizeof area))
			wm_refit();
		server_broadcast_theme();
		damage_all();
		break;
	}
	case MSG_WIN_SET_SIZE:
		if (w && len >= sizeof(MsgWinSetSize) && !(w->states & (STATE_MAXIMIZED | STATE_FULLSCREEN | STATE_TILED_LEFT | STATE_TILED_RIGHT | STATE_TILED))) {
			const MsgWinSetSize *m = (const MsgWinSetSize *)buf;
			Rect wa = work_area();
			f32 nw = clampf((f32)MAX(m->width, w->min_w), 120, wa.w), nh = clampf((f32)MAX(m->height, w->min_h), 80, wa.h);
			/* Grow around the window's centre, then keep it on screen. */
			Rect r = rect(w->rect.x + (w->rect.w - nw) / 2, w->rect.y, nw, nh);
			r.x = clampf(r.x, wa.x, wa.x + wa.w - r.w);
			r.y = clampf(r.y, wa.y, wa.y + wa.h - r.h);
			window_set_rect(w, r, true);
			window_configure(w);
		}
		break;
	case MSG_SCREENSHOT:
		if (len >= sizeof(MsgScreenshot))
			screenshot_request(c, h->win, ((const MsgScreenshot *)buf)->mode);
		break;
	case MSG_ACCOUNT:
		if (len >= sizeof(MsgAccount)) {
			MsgReply r = { .seq = ((const MsgAccount *)buf)->seq };
			r.status = account_update((const MsgAccount *)buf, r.message, sizeof r.message);
			server_send(c, &r, MSG_REPLY, sizeof r, h->win, NULL, 0);
		}
		break;
	case MSG_SESSION:
		if (len >= sizeof(MsgSession))
			shell_session_action(((const MsgSession *)buf)->action);
		break;
	}
done:
	for (int i = 0; i < nfds; i++)
		if (fds[i] != used_fd)
			sys_close(fds[i]);
}

void client_disconnect(Client *c)
{
	screenshot_client_gone(c);
	for (int i = S.nwin - 1; i >= 0; i--)
		if (S.windows[i]->client == c && !S.windows[i]->closing)
			window_destroy(S.windows[i], true);
	loop_del_fd(S.loop, c->fd);
	sys_close(c->fd);
	c->fd = -1;
	for (int i = 0; i < S.nclients; i++) {
		if (S.clients[i] == c) {
			memmove(&S.clients[i], &S.clients[i + 1], sizeof(Client *) * (usize)(S.nclients - i - 1));
			S.nclients--;
			break;
		}
	}
	mem_free(c);
	request_frame();
}

static void on_client(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(loop), UNUSED(fd);
	Client *c = data;
	u8 buf[ZUI_MAX_MSG];
	for (int burst = 0; burst < 64; burst++) {
		int fds[4], nfds = 0;
		long n = unix_recv(c->fd, buf, sizeof buf, fds, 4, &nfds);
		if (n == -EAGAIN)
			break;
		if (n <= 0) {
			client_disconnect(c);
			return;
		}
		handle_message(c, buf, (usize)n, fds, nfds);
	}
	if (events & (EPOLLHUP | EPOLLERR))
		client_disconnect(c);
}

static void on_accept(Loop *loop, int fd, u32 events, void *data)
{
	UNUSED(events), UNUSED(data);
	for (;;) {
		int cfd = (int)sys_accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (cfd < 0)
			break;
		if (S.nclients >= MAX_CLIENTS) {
			sys_close(cfd);
			continue;
		}
		Client *c = NEW(Client);
		c->fd = cfd;
		str_copy(c->app_id, "unknown", sizeof c->app_id);
		S.clients[S.nclients++] = c;
		loop_add_fd(loop, cfd, EPOLLIN, on_client, c);
	}
}

void server_init(void)
{
	const char *path = env_get("ZUI_SOCKET");
	if (!path || !*path)
		path = ZUI_SOCKET_PATH;
	char *dir = path_dir(path);
	dir_make(dir, 0755);
	mem_free(dir);
	S.listen_fd = unix_listen(path, SOCK_SEQPACKET | SOCK_NONBLOCK);
	if (S.listen_fd < 0)
		die("cannot listen on %s: %s", path, err_str(S.listen_fd));
	sys_chmod(path, 0666);
	loop_add_fd(S.loop, S.listen_fd, EPOLLIN, on_accept, NULL);
	S.theme = current_theme();
}

void server_frame_done(void)
{
	wl_frame_done();
	MsgFrame f = { .time_ns = time_mono_ns() };
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.windows[i];
		if (!w->client)
			continue;
		if (w->want_frame) {
			server_send(w->client, &f, MSG_FRAME, sizeof f, w->client_win, NULL, 0);
			w->want_frame = false;
		}
		if (w->release_after_frame >= 0) {
			MsgRelease r = { .slot = (u32)w->release_after_frame };
			server_send(w->client, &r, MSG_RELEASE, sizeof r, w->client_win, NULL, 0);
			w->release_after_frame = -1;
		}
	}
}

void server_pointer_event(Window *w, u32 kind, u32 button)
{
	if (w && w->wl) {
		wl_pointer_event(w, kind, button);
		return;
	}
	if (!w || !w->client)
		return;
	MsgPointer m = { .kind = kind, .button = button, .mods = S.mods, .x = S.px - w->rect.x, .y = S.py - w->rect.y,
			 .time_ms = time_mono_ms() };
	server_send(w->client, &m, MSG_POINTER, sizeof m, w->client_win, NULL, 0);
}

void server_scroll_event(Window *w, f32 dx, f32 dy, u32 source, u32 phase)
{
	if (w && w->wl) {
		wl_scroll_event(w, dx, dy, source, phase);
		return;
	}
	if (!w || !w->client)
		return;
	MsgScroll m = { .dx = dx, .dy = dy, .source = source, .phase = phase, .mods = S.mods,
			.x = S.px - w->rect.x, .y = S.py - w->rect.y };
	server_send(w->client, &m, MSG_SCROLL, sizeof m, w->client_win, NULL, 0);
}

void server_key_event(Window *w, u32 code, u32 state, const char *text)
{
	if (w && w->wl) {
		wl_key_event(w, code, state);
		return;
	}
	if (!w || !w->client)
		return;
	MsgKey m = { .code = code, .state = state, .mods = S.mods, .time_ms = time_mono_ms() };
	str_copy(m.text, text ? text : "", sizeof m.text);
	server_send(w->client, &m, MSG_KEY, sizeof m, w->client_win, NULL, 0);
}

void server_focus_event(Window *w, bool focused)
{
	if (w && w->wl) {
		wl_focus_event(w, focused);
		return;
	}
	if (!w || !w->client)
		return;
	MsgFocus m = { .focused = focused };
	server_send(w->client, &m, MSG_FOCUS, sizeof m, w->client_win, NULL, 0);
	input_gamepads_focus(w, focused);
}

void server_gamepad_event(Window *w, const MsgGamepad *m)
{
	if (!w || !w->client)
		return;
	MsgGamepad copy = *m;   /* server_send fills in the header */
	server_send(w->client, &copy, MSG_GAMEPAD, sizeof copy, w->client_win, NULL, 0);
}
