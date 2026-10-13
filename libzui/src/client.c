/* App runtime: talks to zdisplay, owns the window buffers, runs the loop. */
#include "zui.h"
#include "zui/driver.h"

typedef struct {
	int sock;
	Loop *loop;
	Ui *ui;
	const UiAppDesc *desc;
	u32 win;
	/* buffers */
	int memfd;
	u8 *map;
	usize map_size;
	u32 buf_w, buf_h, stride, slot_bytes;
	bool slot_busy[2];
	int next_slot;
	/* sizing */
	u32 width, height;
	f32 scale;
	bool configured;
	bool waiting_frame;
	u64 commit_time;
	bool dirty;
	bool running;
	u32 last_cursor;
	MsgWelcome welcome;
	char user_name[64];
} Client;

static void send_msg(Client *c, void *msg, u16 type, usize size, const int *fds, int nfds)
{
	MsgHeader *h = msg;
	h->type = type;
	h->size = (u16)size;
	h->win = c->win;
	long r = unix_send(c->sock, msg, size, fds, nfds);
	if (r < 0) {
		eprint("zui: lost connection to the display server (%s)\n", err_str(r));
		c->running = false;
	}
}

static void free_buffers(Client *c)
{
	if (c->map) {
		sys_munmap(c->map, c->map_size);
		c->map = NULL;
	}
	if (c->memfd >= 0) {
		sys_close(c->memfd);
		c->memfd = -1;
	}
}

static bool make_buffers(Client *c)
{
	free_buffers(c);
	c->buf_w = (u32)MAX(1, (int)roundf((f32)c->width * c->scale));
	c->buf_h = (u32)MAX(1, (int)roundf((f32)c->height * c->scale));
	c->stride = c->buf_w;
	c->slot_bytes = (u32)ALIGN_UP((usize)c->stride * c->buf_h * 4, 4096u);
	c->map_size = (usize)c->slot_bytes * 2;
	c->memfd = (int)sys_memfd_create("zui-window", MFD_CLOEXEC);
	if (c->memfd < 0 || sys_ftruncate(c->memfd, (i64)c->map_size) < 0)
		return false;
	c->map = sys_mmap(NULL, c->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, c->memfd, 0);
	if ((unsigned long)c->map > -4096ul) {
		c->map = NULL;
		return false;
	}
	MsgWinBuffer m = { .width = c->buf_w, .height = c->buf_h, .stride = c->stride, .slot_bytes = c->slot_bytes };
	send_msg(c, &m, MSG_WIN_BUFFER, sizeof m, &c->memfd, 1);
	c->slot_busy[0] = c->slot_busy[1] = false;
	c->next_slot = 0;
	return true;
}

static void handle_message(Client *c, const u8 *buf, usize len, int *fds, int nfds)
{
	const MsgHeader *h = (const MsgHeader *)buf;
	if (len < sizeof *h)
		return;
	Ui *ui = c->ui;
	switch (h->type) {
	case MSG_CONFIGURE: {
		const MsgConfigure *m = (const MsgConfigure *)buf;
		bool resized = m->width != c->width || m->height != c->height || m->scale != c->scale;
		c->width = m->width, c->height = m->height, c->scale = m->scale > 0 ? m->scale : 1;
		ui->win_states = m->states;
		ui->window_focused = m->states & STATE_ACTIVE;
		if (resized || !c->map)
			make_buffers(c);
		c->configured = true;
		c->dirty = true;
		break;
	}
	case MSG_RELEASE: {
		const MsgRelease *m = (const MsgRelease *)buf;
		if (m->slot < 2)
			c->slot_busy[m->slot] = false;
		break;
	}
	case MSG_FRAME:
		c->waiting_frame = false;
		break;
	case MSG_POINTER: {
		const MsgPointer *m = (const MsgPointer *)buf;
		ui_input_pointer(ui, m->kind, m->x, m->y, m->button, m->mods);
		c->dirty = true;
		break;
	}
	case MSG_SCROLL: {
		const MsgScroll *m = (const MsgScroll *)buf;
		ui->mx = m->x, ui->my = m->y;
		ui_input_scroll(ui, m->dx, m->dy, m->source, m->phase);
		c->dirty = true;
		break;
	}
	case MSG_KEY: {
		const MsgKey *m = (const MsgKey *)buf;
		ui_input_key(ui, m->code, m->state, m->mods, m->text);
		c->dirty = true;
		break;
	}
	case MSG_GAMEPAD: {
		const MsgGamepad *m = (const MsgGamepad *)buf;
		ui_input_gamepad(ui, m->pad, m->connected, m->buttons, m->axes, m->name);
		if (m->pad < PAD_MAX) {
			ui->pads[m->pad].battery = m->battery;
			ui->pads[m->pad].rumble = m->rumble;
		}
		c->dirty = true;
		break;
	}
	case MSG_FOCUS: {
		const MsgFocus *m = (const MsgFocus *)buf;
		ui_input_focus(ui, m->focused);
		c->dirty = true;
		break;
	}
	case MSG_THEME: {
		const MsgTheme *m = (const MsgTheme *)buf;
		ui_set_theme(ui, &m->theme);
		font_cache_clear();
		c->dirty = true;
		break;
	}
	case MSG_CLOSE_REQUEST:
		if (!c->desc->close || c->desc->close(ui))
			c->running = false;
		break;
	case MSG_CLIPBOARD_DATA: {
		const MsgClipboard *m = (const MsgClipboard *)buf;
		if (str_eq(m->mime, CLIPBOARD_FILES)) {
			/* files asked for with ui_clipboard_files_request */
			mem_free(ui->clip_files);
			ui->clip_files = NULL;
			if (nfds > 0 && m->size && m->size < (16u << 20)) {
				char *text = mem_alloc(m->size + 1);
				long n = sys_pread(fds[0], text, m->size, 0);
				text[n > 0 ? n : 0] = 0;
				ui->clip_files = text;
			} else {
				ui->clip_files = str_dup("");
			}
			c->dirty = true;
			break;
		}
		/* Only text can be pasted into text fields. */
		if (nfds > 0 && m->size < (64u << 20) && (str_starts(m->mime, "text/") || !m->mime[0])) {
			char *text = mem_alloc(m->size + 1);
			long n = sys_pread(fds[0], text, m->size, 0);
			text[n > 0 ? n : 0] = 0;
			ui_input_paste(ui, text);
			mem_free(text);
			c->dirty = true;
		}
		break;
	}
	case MSG_SCREENSHOT_DATA: {
		const MsgScreenshotData *m = (const MsgScreenshotData *)buf;
		canvas_free(ui->screenshot);
		ui->screenshot = NULL;
		ui->screenshot_status = m->status;
		if (m->status == 0 && nfds > 0 && m->width && m->height && m->width <= 16384 && m->height <= 16384) {
			Canvas *img = canvas_new((int)m->width, (int)m->height);
			for (u32 y = 0; y < m->height; y++)
				sys_pread(fds[0], canvas_row(img, (int)y), (usize)m->width * 4, (i64)((u64)y * m->width * 4));
			ui->screenshot = img;
		} else if (m->status == 0) {
			ui->screenshot_status = -1;
		}
		ui->screenshot_ready = true;
		c->dirty = true;
		break;
	}
	case MSG_REPLY: {
		const MsgReply *m = (const MsgReply *)buf;
		ui->reply_seq = m->seq;
		ui->reply_status = m->status;
		str_copy(ui->reply_message, m->message, sizeof ui->reply_message);
		ui->redraw = true;
		break;
	}
	case MSG_POPUP_DONE:
		break;
	case MSG_DROP_FILES: {
		const MsgDropFiles *m = (const MsgDropFiles *)buf;
		ui->drag_over = false;
		if (nfds > 0 && m->size && m->size < (16u << 20)) {
			char *paths = mem_alloc(m->size + 1);
			long n = sys_pread(fds[0], paths, m->size, 0);
			paths[n > 0 ? n : 0] = 0;
			ui->mx = m->x, ui->my = m->y;
			ui->drop_mods = m->mods;
			ui_input_drop(ui, paths);
			mem_free(paths);
		}
		c->dirty = true;
		break;
	}
	case MSG_DRAG_OVER: {
		const MsgDragOver *m = (const MsgDragOver *)buf;
		ui->drag_over = m->kind != DRAG_LEAVE;
		ui->drag_x = m->x, ui->drag_y = m->y;
		ui->drag_count = (int)m->count;
		ui->mods = m->mods;
		str_copy(ui->drag_first, m->first, sizeof ui->drag_first);
		c->dirty = true;
		break;
	}
	}
	for (int i = 0; i < nfds; i++)
		sys_close(fds[i]);
}

static void on_socket(Loop *loop, int fd, u32 events, void *data)
{
	Client *c = data;
	if (events & (EPOLLHUP | EPOLLERR)) {
		c->running = false;
		loop_quit(loop);
		return;
	}
	u8 buf[ZUI_MAX_MSG];
	for (;;) {
		int fds[4], nfds = 0;
		long n = unix_recv(fd, buf, sizeof buf, fds, 4, &nfds);
		if (n == -EAGAIN)
			break;
		if (n <= 0) {
			c->running = false;
			loop_quit(loop);
			return;
		}
		handle_message(c, buf, (usize)n, fds, nfds);
	}
}

static void process_requests(Client *c)
{
	Ui *ui = c->ui;
	if (ui->begin_move) {
		MsgWinMove m = { 0 };
		send_msg(c, &m, MSG_WIN_MOVE, sizeof m, NULL, 0);
		ui->begin_move = false;
	}
	if (ui->begin_resize) {
		MsgWinMove m = { .edges = ui->begin_resize };
		send_msg(c, &m, MSG_WIN_RESIZE, sizeof m, NULL, 0);
		ui->begin_resize = 0;
	}
	if (ui->action) {
		if (ui->action == ACTION_CLOSE && c->desc->close && !c->desc->close(ui)) {
			/* the app vetoed closing */
		} else if (ui->action == ACTION_CLOSE) {
			c->running = false;
		} else {
			MsgWinAction m = { .action = ui->action, .x = (i32)ui->mx, .y = (i32)ui->my };
			send_msg(c, &m, MSG_WIN_ACTION, sizeof m, NULL, 0);
		}
		ui->action = 0;
	}
	if (ui->cursor != c->last_cursor) {
		MsgCursor m = { .shape = ui->cursor };
		send_msg(c, &m, MSG_CURSOR, sizeof m, NULL, 0);
		c->last_cursor = ui->cursor;
	}
	if (ui->clipboard_out) {
		ui_clipboard_set(ui, ui->clipboard_out);
		mem_free(ui->clipboard_out);
		ui->clipboard_out = NULL;
	}
	if (ui->drag_out) {
		usize n = strlen(ui->drag_out);
		int fds[2] = { (int)sys_memfd_create("zui-drag", MFD_CLOEXEC), -1 };
		MsgDragStart m = { .count = (u32)ui->drag_out_count, .size = (u32)n };
		if (fds[0] >= 0) {
			write_all(fds[0], ui->drag_out, n);
			Canvas *t = ui->drag_out_thumb;
			if (t && (fds[1] = (int)sys_memfd_create("zui-drag-picture", MFD_CLOEXEC)) >= 0) {
				for (int y = 0; y < t->height; y++)
					write_all(fds[1], canvas_row(t, y), (usize)t->width * 4);
				m.thumb_w = (u32)t->width, m.thumb_h = (u32)t->height;
			}
			send_msg(c, &m, MSG_DRAG_START, sizeof m, fds, fds[1] >= 0 ? 2 : 1);
		}
		for (int i = 0; i < 2; i++)
			if (fds[i] >= 0)
				sys_close(fds[i]);
		mem_free(ui->drag_out);
		ui->drag_out = NULL;
		canvas_free(ui->drag_out_thumb);
		ui->drag_out_thumb = NULL;
	}
	if (ui->want_files) {
		MsgClipboard m = { .size = 0 };
		str_copy(m.mime, CLIPBOARD_FILES, sizeof m.mime);
		send_msg(c, &m, MSG_CLIPBOARD_GET, sizeof m, NULL, 0);
		ui->want_files = false;
	}
	if (ui->want_paste) {
		MsgClipboard m = { .size = 0 };
		str_copy(m.mime, "text/plain", sizeof m.mime);
		send_msg(c, &m, MSG_CLIPBOARD_GET, sizeof m, NULL, 0);
		ui->want_paste = false;
	}
	if (ui->quit)
		c->running = false;
}

static void render(Client *c)
{
	int slot = -1;
	for (int i = 0; i < 2; i++) {
		int s = (c->next_slot + i) % 2;
		if (!c->slot_busy[s]) {
			slot = s;
			break;
		}
	}
	if (slot < 0 || !c->map)
		return;
	Canvas canvas = canvas_wrap(c->map + (usize)slot * c->slot_bytes, (int)c->buf_w, (int)c->buf_h, (int)c->stride);
	memset(canvas.pixels, 0, (usize)c->stride * c->buf_h * 4);
	ui_attach(c->ui, &canvas, (f32)c->width, (f32)c->height, c->scale);
	c->ui->redraw = false;
	ui_frame(c->ui, c->desc->frame);
	c->dirty = c->ui->redraw;
	MsgWinCommit m = { .slot = (u32)slot };
	send_msg(c, &m, MSG_WIN_COMMIT, sizeof m, NULL, 0);
	c->slot_busy[slot] = true;
	c->next_slot = (slot + 1) % 2;
	c->waiting_frame = true;
	c->commit_time = time_mono_ms();
	process_requests(c);
}

static void on_tick(Loop *loop, void *data)
{
	UNUSED(loop);
	Client *c = data;
	if (c->desc->tick)
		c->desc->tick(c->ui);
	c->dirty = true;
}

static void on_wake(Loop *loop, void *data)
{
	UNUSED(loop);
	((Client *)data)->dirty = true;
}

static Client *current;

Loop *ui_loop(Ui *ui)
{
	Client *c = ui->driver;
	return c ? c->loop : NULL;
}

void ui_wake(Ui *ui)
{
	Client *c = ui->driver;
	if (c)
		loop_wake(c->loop);
}

void ui_set_title(Ui *ui, const char *title)
{
	if (str_eq(ui->title, title))
		return;
	str_copy(ui->title, title, sizeof ui->title);
	Client *c = ui->driver;
	if (!c)
		return;
	MsgWinTitle m = { 0 };
	str_copy(m.title, title, sizeof m.title);
	send_msg(c, &m, MSG_WIN_TITLE, sizeof m, NULL, 0);
}

void ui_window_action(Ui *ui, u32 action)
{
	ui->action = action;
}

void ui_window_resize(Ui *ui, int width, int height)
{
	Client *c = ui->driver;
	if (!c)
		return;
	MsgWinSetSize m = { .width = (u32)width, .height = (u32)height };
	send_msg(c, &m, MSG_WIN_SET_SIZE, sizeof m, NULL, 0);
}

void ui_notify(Ui *ui, const char *title, const char *body, const char *icon)
{
	Client *c = ui->driver;
	if (!c)
		return;
	MsgNotify m = { 0 };
	str_copy(m.icon, icon ? icon : (ui->app_id ? ui->app_id : ""), sizeof m.icon);
	str_copy(m.title, title, sizeof m.title);
	str_copy(m.body, body ? body : "", sizeof m.body);
	send_msg(c, &m, MSG_NOTIFY, sizeof m, NULL, 0);
}

void ui_pad_rumble(Ui *ui, int pad, f32 strong, f32 weak, u32 ms)
{
	Client *c = ui->driver;
	if (!c || !ui->window_focused)
		return;
	MsgRumble m = { .pad = pad, .strong = (u16)(clampf(strong, 0, 1) * 65535), .weak = (u16)(clampf(weak, 0, 1) * 65535), .ms = ms };
	send_msg(c, &m, MSG_RUMBLE, sizeof m, NULL, 0);
}

void ui_launch(Ui *ui, const char *app_id, const char *arg)
{
	Client *c = ui->driver;
	if (!c)
		return;
	MsgLaunch m = { 0 };
	str_copy(m.app_id, app_id ? app_id : "", sizeof m.app_id);
	str_copy(m.arg, arg ? arg : "", sizeof m.arg);
	send_msg(c, &m, MSG_LAUNCH, sizeof m, NULL, 0);
}

void ui_settings_changed(Ui *ui)
{
	Client *c = ui->driver;
	if (!c)
		return;
	MsgHeader m = { 0 };
	send_msg(c, &m, MSG_SETTINGS_CHANGED, sizeof m, NULL, 0);
}

u32 ui_account_update(Ui *ui, u32 flags, const char *full_name, const char *old_password, const char *new_password)
{
	static u32 seq;
	Client *c = ui->driver;
	if (!c)
		return 0;
	MsgAccount m = { .seq = ++seq, .flags = flags };
	str_copy(m.full_name, full_name ? full_name : "", sizeof m.full_name);
	str_copy(m.old_password, old_password ? old_password : "", sizeof m.old_password);
	str_copy(m.new_password, new_password ? new_password : "", sizeof m.new_password);
	send_msg(c, &m, MSG_ACCOUNT, sizeof m, NULL, 0);
	/* Don't leave passwords lying around in memory. */
	memset(m.old_password, 0, sizeof m.old_password);
	memset(m.new_password, 0, sizeof m.new_password);
	return seq;
}

void ui_session(Ui *ui, u32 action)
{
	Client *c = ui->driver;
	if (!c)
		return;
	MsgSession m = { .action = action };
	send_msg(c, &m, MSG_SESSION, sizeof m, NULL, 0);
}

void ui_clipboard_set_data(Ui *ui, const char *mime, const void *data, usize n)
{
	Client *c = ui->driver;
	if (!c)
		return;
	int fd = (int)sys_memfd_create("zui-clipboard", MFD_CLOEXEC);
	if (fd < 0)
		return;
	write_all(fd, data, n);
	MsgClipboard m = { .size = (u32)n };
	str_copy(m.mime, mime, sizeof m.mime);
	send_msg(c, &m, MSG_CLIPBOARD_SET, sizeof m, &fd, 1);
	sys_close(fd);
}

void ui_clipboard_set(Ui *ui, const char *text)
{
	ui_clipboard_set_data(ui, "text/plain", text, strlen(text));
}

void ui_clipboard_set_files(Ui *ui, const char *paths, bool cut)
{
	char *data = str_fmt("%s\n%s", cut ? "cut" : "copy", paths);
	ui_clipboard_set_data(ui, CLIPBOARD_FILES, data, strlen(data));
	mem_free(data);
}

void ui_clipboard_files_request(Ui *ui)
{
	ui->want_files = true;
	ui->redraw = true;
}

void ui_screenshot_request(Ui *ui, u32 mode)
{
	Client *c = ui->driver;
	if (!c)
		return;
	MsgScreenshot m = { .mode = mode };
	send_msg(c, &m, MSG_SCREENSHOT, sizeof m, NULL, 0);
}

const char *ui_user_name(Ui *ui)
{
	Client *c = ui->driver;
	if (c && c->user_name[0])
		return c->user_name;
	const char *u = env_get("USER");
	return u ? u : "User";
}

/* Parses ZUI_SNAPSHOT="path[:WxH][:scale][:dark]". */
static int run_snapshot(const UiAppDesc *desc, const char *spec)
{
	char path[512];
	int w = desc->width, h = desc->height;
	f32 scale = 1;
	bool dark = false;
	usize n = 0;
	while (spec[n] && spec[n] != ':' && n < sizeof path - 1) {
		path[n] = spec[n];
		n++;
	}
	path[n] = 0;
	const char *p = spec + n;
	while (*p == ':') {
		p++;
		if (str_starts(p, "dark")) {
			dark = true;
			p += 4;
		} else if (str_chr(p, 'x') && is_digit(*p) && (str_chr(p, ':') == NULL || str_chr(p, 'x') < str_chr(p, ':'))) {
			w = (int)str_to_int(p, &p);
			if (*p == 'x')
				h = (int)str_to_int(p + 1, &p);
		} else {
			scale = (f32)str_to_float(p, &p);
		}
		while (*p && *p != ':')
			p++;
	}
	return ui_snapshot(desc, w, h, scale > 0 ? scale : 1, dark, path) ? 0 : 1;
}

int ui_run(const UiAppDesc *desc)
{
	/* The desktop passes the user's time zone. */
	const char *tz = env_get("TZ");
	if (tz && *tz)
		tz_set(tz);
	const char *snap = env_get("ZUI_SNAPSHOT");
	if (snap && *snap)
		return run_snapshot(desc, snap);

	sig_ignore(SIGPIPE);
	Client *c = NEW(Client);
	current = c;
	c->desc = desc;
	c->memfd = -1;
	c->scale = 1;
	const char *path = env_get("ZUI_SOCKET");
	c->sock = unix_connect(path && *path ? path : ZUI_SOCKET_PATH, SOCK_SEQPACKET);
	if (c->sock < 0) {
		eprint("%s: cannot connect to the display server: %s\n", desc->id, err_str(c->sock));
		return 1;
	}
	c->ui = ui_new();
	c->ui->driver = c;
	c->ui->user = desc->user;
	c->ui->app_id = desc->id;
	str_copy(c->ui->title, desc->title ? desc->title : desc->id, sizeof c->ui->title);

	MsgHello hello = { .version = ZUI_PROTOCOL_VERSION, .pid = (u32)sys_getpid() };
	str_copy(hello.app_id, desc->id, sizeof hello.app_id);
	send_msg(c, &hello, MSG_HELLO, sizeof hello, NULL, 0);

	/* The first reply is the welcome with theme and screen information. */
	u8 buf[ZUI_MAX_MSG];
	int fds[4], nfds = 0;
	long n = unix_recv(c->sock, buf, sizeof buf, fds, 4, &nfds);
	if (n < (long)sizeof(MsgWelcome) || ((MsgHeader *)buf)->type != MSG_WELCOME) {
		eprint("%s: unexpected reply from the display server\n", desc->id);
		return 1;
	}
	memcpy(&c->welcome, buf, sizeof c->welcome);
	ui_set_theme(c->ui, &c->welcome.theme);
	str_copy(c->user_name, c->welcome.user_name, sizeof c->user_name);
	c->scale = c->welcome.scale > 0 ? c->welcome.scale : 1;

	MsgWinCreate wc = { .flags = desc->flags, .width = (u32)desc->width, .height = (u32)desc->height,
			    .min_width = (u32)desc->min_width, .min_height = (u32)desc->min_height, .corner_radius = 10 };
	str_copy(wc.title, c->ui->title, sizeof wc.title);
	c->win = 1;
	send_msg(c, &wc, MSG_WIN_CREATE, sizeof wc, NULL, 0);

	c->loop = loop_new();
	fd_set_nonblock(c->sock, true);
	loop_add_fd(c->loop, c->sock, EPOLLIN, on_socket, c);
	loop_on_wake(c->loop, on_wake, c);
	if (desc->tick && desc->tick_ms)
		loop_timer(c->loop, desc->tick_ms, desc->tick_ms, on_tick, c);
	if (desc->init)
		desc->init(c->ui);

	c->running = true;
	c->dirty = true;
	while (c->running) {
		bool can_draw = c->configured && (!c->waiting_frame || time_mono_ms() - c->commit_time > 100);
		if (c->dirty && can_draw && (!c->slot_busy[0] || !c->slot_busy[1])) {
			render(c);
			if (!c->running)
				break;
		}
		int timeout = -1;
		if (c->dirty)
			timeout = c->waiting_frame ? 100 : 0;
		if (c->ui->wake_at > 0) {
			f64 dt = c->ui->wake_at - (f64)time_mono_ns() / 1e9;
			int ms = dt <= 0 ? 0 : (int)(dt * 1000) + 1;
			if (timeout < 0 || ms < timeout)
				timeout = ms;
		}
		loop_once(c->loop, timeout);
		if (c->ui->wake_at > 0 && (f64)time_mono_ns() / 1e9 >= c->ui->wake_at) {
			c->ui->wake_at = 0;
			c->dirty = true;
		}
		if (c->ui->redraw)
			c->dirty = true;
	}
	MsgHeader bye = { 0 };
	send_msg(c, &bye, MSG_WIN_DESTROY, sizeof bye, NULL, 0);
	sys_close(c->sock);
	return 0;
}
