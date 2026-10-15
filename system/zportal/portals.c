/* The portals (org.freedesktop.portal.*): what a program in Flatpak's
 * sandbox asks the desktop for, on /org/freedesktop/portal/desktop. Each
 * interface is answered the Zurbax way: links open in Drift, emails in Mail,
 * files are picked in Zurbax's own file chooser, the look follows Zurbax's
 * Appearance settings, trash goes to Vault's Trash. */
#include "zportal.h"

#define DESKTOP "/org/freedesktop/portal/desktop"

/* ---- Settings: the look -------------------------------------------------------------------- */

typedef struct {
	bool dark, reduce_motion, high_contrast, clock24;
	Color accent;
	f32 text_scale;
} Look;

static Look look, sent_look;
static bool have_sent;

/* From Zurbax's settings (the same file Settings and zdisplay use). */
static Look read_look(void)
{
	Look l = { 0 };
	char *path = str_fmt("%s/.config/zurbax/settings.conf", P.home);
	Conf *c = conf_load(path);
	mem_free(path);
	l.dark = look_dark(c);
	const char *accent = look_accent(c);
	l.accent = accent[0] == '#' ? color_hex(accent) : theme_accent_named(accent);
	l.reduce_motion = conf_get_bool(c, "reduce_motion", false);
	l.text_scale = (f32)conf_get_float(c, "text_scale", 1.0);
	l.clock24 = conf_get_bool(c, "clock_24h", true);
	conf_free(c);
	return l;
}

typedef struct {
	const char *ns, *key, *sig;
} SettingKey;

static const SettingKey SETTINGS[] = {
	{ "org.freedesktop.appearance", "color-scheme", "u" },
	{ "org.freedesktop.appearance", "accent-color", "(ddd)" },
	{ "org.freedesktop.appearance", "contrast", "u" },
	{ "org.freedesktop.appearance", "reduced-motion", "u" },
	{ "org.gnome.desktop.interface", "color-scheme", "s" },
	{ "org.gnome.desktop.interface", "gtk-theme", "s" },
	{ "org.gnome.desktop.interface", "icon-theme", "s" },
	{ "org.gnome.desktop.interface", "cursor-size", "i" },
	{ "org.gnome.desktop.interface", "clock-format", "s" },
	{ "org.gnome.desktop.interface", "text-scaling-factor", "d" },
	{ "org.gnome.desktop.interface", "enable-animations", "b" },
	{ "org.gnome.desktop.interface", "accent-color", "s" },
	{ "org.gnome.desktop.wm.preferences", "button-layout", "s" },
};

/* GNOME's accent names, nearest to Zurbax's accent */
static const char *gnome_accent(Color c)
{
	static const struct {
		const char *name;
		u32 rgb;
	} names[] = { { "blue", 0x3584e4 }, { "teal", 0x2190a4 }, { "green", 0x3a944a }, { "yellow", 0xc88800 },
		      { "orange", 0xed5b00 }, { "red", 0xe62d42 }, { "pink", 0xd56199 }, { "purple", 0x9141ac },
		      { "slate", 0x6f8396 } };
	const char *best = "blue";
	i32 bd = 1 << 30;
	for (usize i = 0; i < ARRAY_LEN(names); i++) {
		i32 dr = (i32)COLOR_R(c) - (i32)(names[i].rgb >> 16 & 0xff), dg = (i32)COLOR_G(c) - (i32)(names[i].rgb >> 8 & 0xff),
		    db = (i32)COLOR_B(c) - (i32)(names[i].rgb & 0xff);
		i32 d = dr * dr + dg * dg + db * db;
		if (d < bd)
			bd = d, best = names[i].name;
	}
	return best;
}

/* Writes one setting's value (not wrapped in a variant). */
static void add_setting(DBusMsg *m, const SettingKey *k, const Look *l)
{
	const char *key = k->key;
	if (str_eq(k->ns, "org.freedesktop.appearance")) {
		if (str_eq(key, "color-scheme"))
			dbus_add(m, "u", l->dark ? 1u : 2u);
		else if (str_eq(key, "accent-color")) {
			dbus_open(m, "(ddd)");
			dbus_add(m, "ddd", COLOR_R(l->accent) / 255.0, COLOR_G(l->accent) / 255.0, COLOR_B(l->accent) / 255.0);
			dbus_close(m);
		} else if (str_eq(key, "contrast"))
			dbus_add(m, "u", l->high_contrast ? 1u : 0u);
		else
			dbus_add(m, "u", l->reduce_motion ? 1u : 0u);
	} else if (str_eq(key, "color-scheme"))
		dbus_add(m, "s", l->dark ? "prefer-dark" : "default");
	else if (str_eq(key, "gtk-theme"))
		dbus_add(m, "s", l->dark ? "Adwaita-dark" : "Adwaita");
	else if (str_eq(key, "icon-theme"))
		dbus_add(m, "s", "Adwaita");
	else if (str_eq(key, "cursor-size"))
		dbus_add(m, "i", 24);
	else if (str_eq(key, "clock-format"))
		dbus_add(m, "s", l->clock24 ? "24h" : "12h");
	else if (str_eq(key, "text-scaling-factor"))
		dbus_add(m, "d", (f64)(l->text_scale > 0 ? l->text_scale : 1));
	else if (str_eq(key, "enable-animations"))
		dbus_add(m, "b", !l->reduce_motion);
	else if (str_eq(key, "accent-color"))
		dbus_add(m, "s", gnome_accent(l->accent));
	else
		/* Zurbax's windows have minimize, maximize and close at the right */
		dbus_add(m, "s", "appmenu:minimize,maximize,close");
}

static const SettingKey *find_setting(const char *ns, const char *key)
{
	for (usize i = 0; i < ARRAY_LEN(SETTINGS); i++)
		if (str_eq(SETTINGS[i].ns, ns) && str_eq(SETTINGS[i].key, key))
			return &SETTINGS[i];
	return NULL;
}

/* "org.freedesktop.*" style patterns of ReadAll */
static bool ns_wanted(const DBusVal *patterns, const char *ns)
{
	if (!patterns || !patterns->count)
		return true;
	for (usize i = 0; i < patterns->count; i++) {
		const char *p = dbus_str(patterns->items[i], "");
		usize n = strlen(p);
		if (!*p || str_eq(p, ns) || (n && p[n - 1] == '*' && str_eq_nocase_n(p, ns, n - 1) && strlen(ns) >= n - 1))
			return true;
	}
	return false;
}

static bool settings_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 2))
		return true;
	bool one = dbus_is_call(call, "org.freedesktop.portal.Settings", "ReadOne");
	if (one || dbus_is_call(call, "org.freedesktop.portal.Settings", "Read")) {
		const SettingKey *k = find_setting(dbus_str(dbus_arg(call, 0), ""), dbus_str(dbus_arg(call, 1), ""));
		if (!k) {
			dbus_reply_error(bus, call, "org.freedesktop.portal.Error.NotFound", "Requested setting not found");
			return true;
		}
		DBusMsg *r = dbus_new_reply(call);
		/* Read, the old way, wraps the value in one variant more */
		if (!one)
			dbus_open_variant(r, "v");
		dbus_open_variant(r, k->sig);
		add_setting(r, k, &look);
		dbus_close(r);
		if (!one)
			dbus_close(r);
		dbus_send(bus, r);
		return true;
	}
	if (dbus_is_call(call, "org.freedesktop.portal.Settings", "ReadAll")) {
		const DBusVal *patterns = dbus_arg(call, 0);
		DBusMsg *r = dbus_new_reply(call);
		dbus_open(r, "a{sa{sv}}");
		const char *done[8] = { 0 };
		int ndone = 0;
		for (usize i = 0; i < ARRAY_LEN(SETTINGS); i++) {
			const char *ns = SETTINGS[i].ns;
			bool seen = false;
			for (int d = 0; d < ndone; d++)
				seen |= str_eq(done[d], ns);
			if (seen || !ns_wanted(patterns, ns))
				continue;
			done[ndone++] = ns;
			dbus_open(r, "{sa{sv}}");
			dbus_add(r, "s", ns);
			dbus_open(r, "a{sv}");
			for (usize k = 0; k < ARRAY_LEN(SETTINGS); k++) {
				if (!str_eq(SETTINGS[k].ns, ns))
					continue;
				dbus_open(r, "{sv}");
				dbus_add(r, "s", SETTINGS[k].key);
				dbus_open_variant(r, SETTINGS[k].sig);
				add_setting(r, &SETTINGS[k], &look);
				dbus_close(r);
				dbus_close(r);
			}
			dbus_close(r);
			dbus_close(r);
		}
		dbus_close(r);
		dbus_send(bus, r);
		return true;
	}
	return false;
}

/* Tells programs about settings that changed (from zdisplay's theme message or the file). */
void settings_poll(void)
{
	char *path = str_fmt("%s/.config/zurbax/settings.conf", P.home);
	i64 m = file_mtime(path);
	mem_free(path);
	if (have_sent && m == P.settings_mtime)
		return;
	P.settings_mtime = m;
	look = read_look();
	if (!P.bus) {
		have_sent = false;
		return;
	}
	if (have_sent) {
		for (usize i = 0; i < ARRAY_LEN(SETTINGS); i++) {
			/* compare the encodings of the old and new value */
			DBusMsg *a = dbus_new_signal("/", "x.y", "Z"), *b = dbus_new_signal("/", "x.y", "Z");
			add_setting(a, &SETTINGS[i], &sent_look);
			add_setting(b, &SETTINGS[i], &look);
			usize la, lb;
			u8 *wa = dbus_msg_encode(a, 1, &la), *wb = dbus_msg_encode(b, 1, &lb);
			bool same = la == lb && memcmp(wa, wb, la) == 0;
			mem_free(wa);
			mem_free(wb);
			dbus_msg_free(a);
			dbus_msg_free(b);
			if (same)
				continue;
			DBusMsg *s = dbus_new_signal(DESKTOP, "org.freedesktop.portal.Settings", "SettingChanged");
			dbus_add(s, "ss", SETTINGS[i].ns, SETTINGS[i].key);
			dbus_open_variant(s, SETTINGS[i].sig);
			add_setting(s, &SETTINGS[i], &look);
			dbus_close(s);
			dbus_send(P.bus, s);
		}
	}
	sent_look = look;
	have_sent = true;
}

/* ---- OpenURI, Email ------------------------------------------------------------------------------- */

static char *fd_path(int fd)
{
	char link[64], target[1024] = { 0 };
	fmt(link, sizeof link, "/proc/self/fd/%d", fd);
	long n = sys_readlink(link, target, sizeof target - 1);
	return n > 0 ? str_dup(target) : NULL;
}

static bool openuri_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 5))
		return true;
	if (dbus_is_call(call, "org.freedesktop.portal.OpenURI", "SchemeSupported")) {
		const char *scheme = dbus_str(dbus_arg(call, 0), "");
		bool ok = str_eq(scheme, "http") || str_eq(scheme, "https") || str_eq(scheme, "mailto") || str_eq(scheme, "file");
		DBusMsg *r = dbus_new_reply(call);
		dbus_add(r, "b", ok);
		dbus_send(bus, r);
		return true;
	}
	bool uri = dbus_is_call(call, "org.freedesktop.portal.OpenURI", "OpenURI");
	bool file = dbus_is_call(call, "org.freedesktop.portal.OpenURI", "OpenFile");
	bool dir = dbus_is_call(call, "org.freedesktop.portal.OpenURI", "OpenDirectory");
	if (!uri && !file && !dir)
		return false;
	Request *r = request_new(call, dbus_arg(call, 2));
	request_reply_handle(r, call);
	u32 code = 0;
	if (uri) {
		const char *u = dbus_str(dbus_arg(call, 1), "");
		char *path = uri_to_path(u);
		display_launch(NULL, path ? path : u);
		mem_free(path);
	} else {
		int fd = dbus_take_fd(call, dbus_arg(call, 1));
		char *path = fd >= 0 ? fd_path(fd) : NULL;
		if (fd >= 0)
			sys_close(fd);
		if (!path) {
			code = 2;
		} else if (dir) {
			char *folder = dir_exists(path) ? str_dup(path) : path_dir(path);
			display_launch("vault", folder);
			mem_free(folder);
		} else {
			display_launch(NULL, path);
		}
		mem_free(path);
	}
	request_respond_empty(r, code);
	return true;
}

static void mailto_add(StrBuf *sb, bool *first, const char *key, const char *value)
{
	if (!value || !*value)
		return;
	sb_addc(sb, *first ? '?' : '&');
	*first = false;
	sb_fmt(sb, "%s=", key);
	for (const u8 *c = (const u8 *)value; *c; c++) {
		if (is_alnum(*c) || str_chr("-._~@", *c))
			sb_addc(sb, (char)*c);
		else
			sb_fmt(sb, "%%%02X", *c);
	}
}

static bool email_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 4))
		return true;
	if (!dbus_is_call(call, "org.freedesktop.portal.Email", "ComposeEmail"))
		return false;
	const DBusVal *o = dbus_arg(call, 1);
	Request *r = request_new(call, o);
	request_reply_handle(r, call);
	StrBuf to = { 0 };
	const char *one = dbus_str(dbus_dict_get(o, "address"), NULL);
	if (one)
		sb_adds(&to, one);
	usize n = 0;
	const char **many = dbus_strv(dbus_dict_get(o, "addresses"), &n);
	for (usize i = 0; i < n; i++) {
		if (to.len)
			sb_addc(&to, ',');
		sb_adds(&to, many[i]);
	}
	mem_free(many);
	StrBuf uri = { 0 };
	sb_fmt(&uri, "mailto:%s", sb_str(&to));
	bool first = true;
	mailto_add(&uri, &first, "subject", dbus_str(dbus_dict_get(o, "subject"), NULL));
	mailto_add(&uri, &first, "body", dbus_str(dbus_dict_get(o, "body"), NULL));
	display_launch("mail", uri.data);
	sb_free(&uri);
	sb_free(&to);
	request_respond_empty(r, 0);
	return true;
}

/* ---- FileChooser: Zurbax's own file chooser ----------------------------------------------------------- */

typedef struct {
	bool save, sandboxed, writable;
	char app[128];
} Pick;

/* a byte array (ay) holding a NUL-terminated path */
static char *bytes_str(const DBusVal *v)
{
	v = dbus_unvariant(v);
	if (!v || v->type != 'a')
		return NULL;
	char *s = mem_alloc(v->count + 1);
	usize n = 0;
	for (usize i = 0; i < v->count && v->items[i]->n; i++)
		s[n++] = (char)v->items[i]->n;
	s[n] = 0;
	return s;
}

/* Extensions from file filters, a(sa(us)): globs like "*.png", and MIME types. */
static void filter_exts(const DBusVal *filter, StrBuf *out, bool *all)
{
	static const struct {
		const char *mime, *exts;
	} mimes[] = { { "image/png", "png" }, { "image/jpeg", "jpg,jpeg" }, { "image/gif", "gif" }, { "image/webp", "webp" },
		      { "image/svg+xml", "svg" }, { "text/plain", "txt" }, { "text/html", "html,htm" }, { "application/pdf", "pdf" },
		      { "audio/mpeg", "mp3" }, { "audio/flac", "flac" }, { "video/mp4", "mp4" }, { "application/json", "json" } };
	filter = dbus_unvariant(filter);
	if (!filter || filter->count < 2)
		return;
	const DBusVal *rules = filter->items[1];
	for (usize i = 0; rules && i < rules->count; i++) {
		const DBusVal *rule = rules->items[i];
		if (rule->count < 2)
			continue;
		u64 kind = rule->items[0]->n;
		const char *pat = dbus_str(rule->items[1], "");
		const char *ext = NULL;
		if (kind == 0) {
			if (str_eq(pat, "*") || str_eq(pat, "*.*"))
				*all = true;
			else if (str_starts(pat, "*."))
				ext = pat + 2;
		} else {
			if (str_eq(pat, "application/octet-stream") || str_ends(pat, "/*"))
				*all = true;
			for (usize m = 0; m < ARRAY_LEN(mimes); m++)
				if (str_eq(pat, mimes[m].mime))
					ext = mimes[m].exts;
		}
		if (ext && *ext && !str_chr(ext, '*')) {
			if (out->len)
				sb_addc(out, ',');
			sb_adds(out, ext);
		}
	}
}

static Request *picks[16];
static int npicks;

/* The program closed the request: the chooser goes, and the request with it. */
static void pick_closed(Request *r)
{
	for (int i = 0; i < npicks; i++)
		if (picks[i] == r)
			picks[i--] = picks[--npicks];
	if (r->pid > 0) {
		sys_kill(r->pid, SIGTERM);
		proc_wait(r->pid, NULL, false);
		r->pid = 0;
	}
	mem_free(r->data);
	r->data = NULL;
}

static void on_pick_output(Loop *loop, int fd, u32 events, void *data)
{
	(void)loop;
	(void)events;
	Request *r = data;
	char buf[4096];
	long n = sys_read(fd, buf, sizeof buf);
	if (n > 0)
		sb_add(&r->out, buf, (usize)n);
	if (n <= 0) {
		loop_del_fd(P.loop, fd);
		sys_close(fd);
		r->out_fd = -1;
	}
}

/* The chooser ended: its output (a path a line) answers the request. */
void helpers_reap(void)
{
	for (int i = 0; i < npicks; i++) {
		Request *r = picks[i];
		int st = 0;
		if (r->pid > 0 && proc_wait(r->pid, &st, true) == 0)
			continue;
		if (r->out_fd >= 0) {
			/* the rest of the output first */
			on_pick_output(P.loop, r->out_fd, 0, r);
			if (r->out_fd >= 0)
				continue;
		}
		picks[i--] = picks[--npicks];
		Pick *p = r->data;
		usize nl = 0;
		char **lines = str_split(sb_str(&r->out), '\n', &nl);
		u32 code = (r->pid > 0 && ((st >> 8) & 0xff) == 0 && nl && lines[0][0]) ? 0 : 1;
		DBusMsg *m = request_response(r, code);
		if (code == 0) {
			dbus_open(m, "{sv}");
			dbus_add(m, "s", "uris");
			dbus_open_variant(m, "as");
			dbus_open(m, "as");
			for (usize k = 0; k < nl; k++) {
				if (!lines[k][0])
					continue;
				char *doc = p && p->sandboxed ? document_export(lines[k], p->app, p->save || p->writable) : NULL;
				char *uri = path_to_uri(doc ? doc : lines[k]);
				dbus_add(m, "s", uri);
				mem_free(uri);
				mem_free(doc);
			}
			dbus_close(m);
			dbus_close(m);
			dbus_close(m);
			dbus_dict_add(m, "writable", "b", p ? (p->save || p->writable) : 1);
		}
		str_split_free(lines, nl);
		mem_free(p);
		r->data = NULL;
		r->pid = 0;
		request_finish(r, m);
	}
}

static bool filechooser_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 4))
		return true;
	bool open = dbus_is_call(call, "org.freedesktop.portal.FileChooser", "OpenFile");
	bool save = dbus_is_call(call, "org.freedesktop.portal.FileChooser", "SaveFile");
	bool saves = dbus_is_call(call, "org.freedesktop.portal.FileChooser", "SaveFiles");
	if (!open && !save && !saves)
		return false;
	const DBusVal *o = dbus_arg(call, 2);
	Request *r = request_new(call, o);
	request_reply_handle(r, call);
	Pick *p = NEW(Pick);
	p->save = save || saves;
	str_copy(p->app, caller_app(dbus_sender(call), &p->sandboxed), sizeof p->app);
	p->writable = dbus_bool(dbus_dict_get(o, "writable"), false);
	r->data = p;
	r->closed = pick_closed;
	/* the chooser's command line */
	const char *mode = saves || dbus_bool(dbus_dict_get(o, "directory"), false) ? "folder" : save ? "save" : "open";
	StrBuf exts = { 0 };
	bool all = false;
	const DBusVal *cur = dbus_dict_get(o, "current_filter");
	if (cur) {
		filter_exts(cur, &exts, &all);
	} else {
		const DBusVal *fs = dbus_dict_get(o, "filters");
		for (usize i = 0; fs && i < fs->count; i++)
			filter_exts(fs->items[i], &exts, &all);
	}
	char *folder = bytes_str(dbus_dict_get(o, "current_folder"));
	char *file = bytes_str(dbus_dict_get(o, "current_file"));
	const char *name = dbus_str(dbus_dict_get(o, "current_name"), NULL);
	char *argv[20];
	int n = 0;
	argv[n++] = "zportal";
	argv[n++] = "pick";
	argv[n++] = (char *)mode;
	argv[n++] = "--title";
	argv[n++] = (char *)dbus_str(dbus_arg(call, 1), "");
	if (dbus_bool(dbus_dict_get(o, "multiple"), false))
		argv[n++] = "--multiple";
	const char *accept = dbus_str(dbus_dict_get(o, "accept_label"), NULL);
	if (accept) {
		argv[n++] = "--accept";
		argv[n++] = (char *)accept;
	}
	if (!all && exts.len) {
		argv[n++] = "--filters";
		argv[n++] = exts.data;
	}
	char *file_dir = file ? path_dir(file) : NULL;
	if (folder || file_dir) {
		argv[n++] = "--folder";
		argv[n++] = folder ? folder : file_dir;
	}
	if (name || file) {
		argv[n++] = "--name";
		argv[n++] = (char *)(name ? name : path_base(file));
	}
	argv[n] = NULL;
	int fds[2];
	if (sys_pipe2(fds, O_CLOEXEC) == 0) {
		SpawnOpts so = { .stdin_fd = -1, .stdout_fd = fds[1], .stderr_fd = -1 };
		r->pid = proc_spawn(proc_self_exe(), argv, NULL, &so);
		sys_close(fds[1]);
		if (r->pid > 0) {
			r->out_fd = fds[0];
			fd_set_nonblock(fds[0], true);
			loop_add_fd(P.loop, fds[0], EPOLLIN, on_pick_output, r);
		} else {
			sys_close(fds[0]);
			r->pid = 0;
		}
	}
	mem_free(folder);
	mem_free(file);
	mem_free(file_dir);
	sb_free(&exts);
	if (r->pid > 0 && npicks < (int)ARRAY_LEN(picks)) {
		picks[npicks++] = r;
	} else {
		if (r->pid > 0)
			sys_kill(r->pid, SIGTERM);
		r->pid = 0;
		mem_free(r->data);
		r->data = NULL;
		r->closed = NULL;
		request_respond_empty(r, 2);
	}
	return true;
}

/* ---- Notification ----------------------------------------------------------------------------------- */

static bool notification_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 2))
		return true;
	if (dbus_is_call(call, "org.freedesktop.portal.Notification", "AddNotification")) {
		const DBusVal *n = dbus_arg(call, 1);
		const char *app = caller_app(dbus_sender(call), NULL);
		display_notify(app && *app ? app : "notification", dbus_str(dbus_dict_get(n, "title"), ""),
			       dbus_str(dbus_dict_get(n, "body"), ""));
		dbus_reply_empty(bus, call);
		return true;
	}
	if (dbus_is_call(call, "org.freedesktop.portal.Notification", "RemoveNotification")) {
		dbus_reply_empty(bus, call);
		return true;
	}
	return false;
}

/* ---- Inhibit ------------------------------------------------------------------------------------------ */

typedef struct {
	u32 cookie;
} Hold;

static void hold_closed(Request *r)
{
	Hold *h = r->data;
	if (h) {
		inhibit_remove(h->cookie);
		mem_free(h);
		r->data = NULL;
	}
}

static bool inhibit_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 3))
		return true;
	if (dbus_is_call(call, "org.freedesktop.portal.Inhibit", "Inhibit")) {
		/* the request stays until the program closes it */
		Request *r = request_new(call, dbus_arg(call, 2));
		Hold *h = NEW(Hold);
		h->cookie = inhibit_add();
		r->data = h;
		r->closed = hold_closed;
		request_reply_handle(r, call);
		return true;
	}
	if (dbus_is_call(call, "org.freedesktop.portal.Inhibit", "CreateMonitor")) {
		dbus_reply_error(bus, call, "org.freedesktop.portal.Error.NotAllowed", "Zurbax has no session end to watch for");
		return true;
	}
	return false;
}

/* ---- Account, Trash, Secret, Wallpaper, Background ----------------------------------------------------- */

static bool account_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 1))
		return true;
	if (!dbus_is_call(call, "org.freedesktop.portal.Account", "GetUserInformation"))
		return false;
	Request *r = request_new(call, dbus_arg(call, 1));
	request_reply_handle(r, call);
	DBusMsg *m = request_response(r, 0);
	dbus_dict_add(m, "id", "s", P.user);
	dbus_dict_add(m, "name", "s", P.full_name);
	dbus_dict_add(m, "image", "s", "");
	request_finish(r, m);
	return true;
}

/* Vault's Trash: the freedesktop.org trash, with a .trashinfo for each file. */
static bool trash_path(const char *path)
{
	char *files = str_fmt("%s/.local/share/Trash/files", P.home), *info = str_fmt("%s/.local/share/Trash/info", P.home);
	dir_make(files, 0700);
	dir_make(info, 0700);
	const char *base = path_base(path);
	char *dst = NULL, *meta = NULL;
	for (int i = 0; i < 1000; i++) {
		mem_free(dst);
		mem_free(meta);
		dst = i ? str_fmt("%s/%s.%d", files, base, i) : str_fmt("%s/%s", files, base);
		meta = i ? str_fmt("%s/%s.%d.trashinfo", info, base, i) : str_fmt("%s/%s.trashinfo", info, base);
		if (!file_exists(dst) && !file_exists(meta))
			break;
	}
	bool ok = sys_rename(path, dst) == 0;
	if (ok) {
		Civil c;
		civil_local(time_unix(), &c);
		char *uri = path_to_uri(path);
		char *text = str_fmt("[Trash Info]\nPath=%s\nDeletionDate=%04d-%02d-%02dT%02d:%02d:%02d\n", uri + 7, c.year, c.month, c.day,
				     c.hour, c.min, c.sec);
		file_write_str(meta, text);
		mem_free(text);
		mem_free(uri);
	}
	mem_free(dst);
	mem_free(meta);
	mem_free(files);
	mem_free(info);
	return ok;
}

static bool trash_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 1))
		return true;
	if (!dbus_is_call(call, "org.freedesktop.portal.Trash", "TrashFile"))
		return false;
	int fd = dbus_take_fd(call, dbus_arg(call, 0));
	char *path = fd >= 0 ? fd_path(fd) : NULL;
	if (fd >= 0)
		sys_close(fd);
	DBusMsg *r = dbus_new_reply(call);
	dbus_add(r, "u", path && trash_path(path) ? 1u : 0u);
	dbus_send(bus, r);
	mem_free(path);
	return true;
}

/* Secret: a key of each app's own, which libsecret in the sandbox encrypts
 * the app's passwords with (kept in the user's files, readable by them only). */
static bool secret_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 1))
		return true;
	if (!dbus_is_call(call, "org.freedesktop.portal.Secret", "RetrieveSecret"))
		return false;
	int fd = dbus_take_fd(call, dbus_arg(call, 0));
	Request *r = request_new(call, dbus_arg(call, 1));
	request_reply_handle(r, call);
	bool sandboxed = false;
	const char *app = caller_app(dbus_sender(call), &sandboxed);
	u32 code = 2;
	if (fd >= 0 && app && *app) {
		char *dir = str_fmt("%s/.local/share/zos/secrets", P.home);
		dir_make(dir, 0700);
		char *path = str_fmt("%s/%s", dir, app);
		usize len = 0;
		char *key = file_read(path, &len);
		if (!key || len < 64) {
			mem_free(key);
			key = mem_alloc(64);
			rand_bytes(key, 64);
			len = 64;
			int kf = (int)sys_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
			if (kf >= 0) {
				write_all(kf, key, 64);
				sys_close(kf);
			}
		}
		if (write_all(fd, key, len))
			code = 0;
		memset(key, 0, len);
		mem_free(key);
		mem_free(path);
		mem_free(dir);
	}
	if (fd >= 0)
		sys_close(fd);
	request_respond_empty(r, code);
	return true;
}

static void set_wallpaper(const char *picture)
{
	/* a copy of its own, as Settings keeps a chosen picture */
	char *dir = str_fmt("%s/.local/share/zos/wallpapers", P.home);
	dir_make(dir, 0700);
	char *dst = str_fmt("%s/%s", dir, path_base(picture));
	if (!str_eq(dst, picture))
		file_copy(picture, dst);
	char *path = str_fmt("%s/.config/zurbax/settings.conf", P.home);
	Conf *c = conf_load(path);
	conf_set(c, "wallpaper", dst);
	conf_save(c);
	conf_free(c);
	display_settings_changed();
	mem_free(path);
	mem_free(dst);
	mem_free(dir);
}

static bool wallpaper_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 1))
		return true;
	bool by_uri = dbus_is_call(call, "org.freedesktop.portal.Wallpaper", "SetWallpaperURI");
	if (!by_uri && !dbus_is_call(call, "org.freedesktop.portal.Wallpaper", "SetWallpaperFile"))
		return false;
	Request *r = request_new(call, dbus_arg(call, 2));
	request_reply_handle(r, call);
	char *path = NULL;
	if (by_uri) {
		path = uri_to_path(dbus_str(dbus_arg(call, 1), ""));
	} else {
		int fd = dbus_take_fd(call, dbus_arg(call, 1));
		path = fd >= 0 ? fd_path(fd) : NULL;
		if (fd >= 0)
			sys_close(fd);
	}
	if (path && file_exists(path))
		set_wallpaper(path);
	request_respond_empty(r, path ? 0 : 2);
	mem_free(path);
	return true;
}

static bool background_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 2))
		return true;
	if (dbus_is_call(call, "org.freedesktop.portal.Background", "RequestBackground")) {
		Request *r = request_new(call, dbus_arg(call, 1));
		request_reply_handle(r, call);
		DBusMsg *m = request_response(r, 0);
		dbus_dict_add(m, "background", "b", 1);
		dbus_dict_add(m, "autostart", "b", 0);
		request_finish(r, m);
		return true;
	}
	if (dbus_is_call(call, "org.freedesktop.portal.Background", "SetStatus")) {
		dbus_reply_empty(bus, call);
		return true;
	}
	return false;
}

/* ---- Network, proxies, memory, power, game mode ---------------------------------------------------------- */

static bool net_available(u32 *connectivity)
{
	Conf *c = conf_load("/run/zurbax/network.status");
	bool up = str_eq(conf_get(c, "state", "disconnected"), "connected");
	conf_free(c);
	if (connectivity)
		*connectivity = up ? 4 : 1; /* full, or local only */
	return up;
}

static bool network_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 3))
		return true;
	u32 conn;
	bool up = net_available(&conn);
	DBusMsg *r = NULL;
	if (dbus_is_call(call, "org.freedesktop.portal.NetworkMonitor", "GetAvailable") ||
	    dbus_is_call(call, "org.freedesktop.portal.NetworkMonitor", "CanReach")) {
		r = dbus_new_reply(call);
		dbus_add(r, "b", up);
	} else if (dbus_is_call(call, "org.freedesktop.portal.NetworkMonitor", "GetMetered")) {
		r = dbus_new_reply(call);
		dbus_add(r, "b", 0);
	} else if (dbus_is_call(call, "org.freedesktop.portal.NetworkMonitor", "GetConnectivity")) {
		r = dbus_new_reply(call);
		dbus_add(r, "u", conn);
	} else if (dbus_is_call(call, "org.freedesktop.portal.NetworkMonitor", "GetStatus")) {
		r = dbus_new_reply(call);
		dbus_open(r, "a{sv}");
		dbus_dict_add(r, "available", "b", up);
		dbus_dict_add(r, "metered", "b", 0);
		dbus_dict_add(r, "connectivity", "u", conn);
		dbus_close(r);
	} else {
		return false;
	}
	dbus_send(bus, r);
	return true;
}

void network_poll(void)
{
	i64 m = file_mtime("/run/zurbax/network.status");
	if (m == P.network_mtime)
		return;
	P.network_mtime = m;
	if (P.bus)
		dbus_send(P.bus, dbus_new_signal(DESKTOP, "org.freedesktop.portal.NetworkMonitor", "changed"));
}

static bool proxy_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (reply_version(bus, call, 1))
		return true;
	if (!dbus_is_call(call, "org.freedesktop.portal.ProxyResolver", "Lookup"))
		return false;
	DBusMsg *r = dbus_new_reply(call);
	dbus_open(r, "as");
	dbus_add(r, "s", "direct://");
	dbus_close(r);
	dbus_send(bus, r);
	return true;
}

static bool monitor_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	const char *iface = dbus_str(dbus_arg(call, 0), "");
	if (str_eq(iface, "org.freedesktop.portal.PowerProfileMonitor") &&
	    dbus_is_call(call, "org.freedesktop.DBus.Properties", "Get") &&
	    str_eq(dbus_str(dbus_arg(call, 1), ""), "power-saver-enabled")) {
		dbus_reply_property(bus, call, "b", 0);
		return true;
	}
	if (dbus_is_call(call, "org.freedesktop.DBus.Properties", "GetAll") &&
	    str_eq(iface, "org.freedesktop.portal.PowerProfileMonitor")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "a{sv}");
		dbus_dict_add(m, "version", "u", 1u);
		dbus_dict_add(m, "power-saver-enabled", "b", 0);
		dbus_close(m);
		dbus_send(bus, m);
		return true;
	}
	return reply_version(bus, call, 1);
}

/* GameMode: games ask for it; Zurbax keeps a count (its own game mode comes
 * with full screen). */
static int games;

static bool gamemode_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	const char *m = dbus_member(call);
	if (dbus_is_call(call, "org.freedesktop.DBus.Properties", "Get") && str_eq(dbus_str(dbus_arg(call, 1), ""), "Active")) {
		dbus_reply_property(bus, call, "b", games > 0);
		return true;
	}
	if (reply_version(bus, call, 4))
		return true;
	if (!m || (!str_starts(m, "Register") && !str_starts(m, "Unregister") && !str_starts(m, "QueryStatus")))
		return false;
	if (str_starts(m, "Register"))
		games++;
	else if (str_starts(m, "Unregister") && games > 0)
		games--;
	/* close the descriptors of the ...ByPIDFd forms */
	for (usize i = 0; i < dbus_nargs(call); i++)
		if (dbus_arg(call, i)->type == 'h') {
			int fd = dbus_take_fd(call, dbus_arg(call, i));
			if (fd >= 0)
				sys_close(fd);
		}
	DBusMsg *r = dbus_new_reply(call);
	dbus_add(r, "i", str_starts(m, "QueryStatus") ? (games > 0 ? 2 : 0) : 0);
	dbus_send(bus, r);
	return true;
}

/* ---- Registration ------------------------------------------------------------------------------------------- */

#define ARG(t, n, d) "<arg type=\"" t "\" name=\"" n "\" direction=\"" d "\"/>"
#define PROP_VERSION "<property name=\"version\" type=\"u\" access=\"read\"/>"

static const char XML_SETTINGS[] =
	"<interface name=\"org.freedesktop.portal.Settings\">"
	"<method name=\"ReadAll\">" ARG("as", "namespaces", "in") ARG("a{sa{sv}}", "value", "out") "</method>"
	"<method name=\"Read\">" ARG("s", "namespace", "in") ARG("s", "key", "in") ARG("v", "value", "out") "</method>"
	"<method name=\"ReadOne\">" ARG("s", "namespace", "in") ARG("s", "key", "in") ARG("v", "value", "out") "</method>"
	"<signal name=\"SettingChanged\"><arg type=\"s\"/><arg type=\"s\"/><arg type=\"v\"/></signal>" PROP_VERSION "</interface>";
static const char XML_OPENURI[] =
	"<interface name=\"org.freedesktop.portal.OpenURI\">"
	"<method name=\"OpenURI\">" ARG("s", "parent_window", "in") ARG("s", "uri", "in") ARG("a{sv}", "options", "in")
	ARG("o", "handle", "out") "</method>"
	"<method name=\"OpenFile\">" ARG("s", "parent_window", "in") ARG("h", "fd", "in") ARG("a{sv}", "options", "in")
	ARG("o", "handle", "out") "</method>"
	"<method name=\"OpenDirectory\">" ARG("s", "parent_window", "in") ARG("h", "fd", "in") ARG("a{sv}", "options", "in")
	ARG("o", "handle", "out") "</method>"
	"<method name=\"SchemeSupported\">" ARG("s", "scheme", "in") ARG("a{sv}", "options", "in") ARG("b", "supported", "out")
	"</method>" PROP_VERSION "</interface>";
static const char XML_EMAIL[] =
	"<interface name=\"org.freedesktop.portal.Email\"><method name=\"ComposeEmail\">" ARG("s", "parent_window", "in")
	ARG("a{sv}", "options", "in") ARG("o", "handle", "out") "</method>" PROP_VERSION "</interface>";
static const char XML_FILECHOOSER[] =
	"<interface name=\"org.freedesktop.portal.FileChooser\">"
	"<method name=\"OpenFile\">" ARG("s", "parent_window", "in") ARG("s", "title", "in") ARG("a{sv}", "options", "in")
	ARG("o", "handle", "out") "</method>"
	"<method name=\"SaveFile\">" ARG("s", "parent_window", "in") ARG("s", "title", "in") ARG("a{sv}", "options", "in")
	ARG("o", "handle", "out") "</method>"
	"<method name=\"SaveFiles\">" ARG("s", "parent_window", "in") ARG("s", "title", "in") ARG("a{sv}", "options", "in")
	ARG("o", "handle", "out") "</method>" PROP_VERSION "</interface>";
static const char XML_NOTIFICATION[] =
	"<interface name=\"org.freedesktop.portal.Notification\">"
	"<method name=\"AddNotification\">" ARG("s", "id", "in") ARG("a{sv}", "notification", "in") "</method>"
	"<method name=\"RemoveNotification\">" ARG("s", "id", "in") "</method>"
	"<signal name=\"ActionInvoked\"><arg type=\"s\"/><arg type=\"s\"/><arg type=\"av\"/></signal>" PROP_VERSION "</interface>";
static const char XML_INHIBIT[] =
	"<interface name=\"org.freedesktop.portal.Inhibit\"><method name=\"Inhibit\">" ARG("s", "window", "in")
	ARG("u", "flags", "in") ARG("a{sv}", "options", "in") ARG("o", "handle", "out") "</method>" PROP_VERSION "</interface>";
static const char XML_ACCOUNT[] =
	"<interface name=\"org.freedesktop.portal.Account\"><method name=\"GetUserInformation\">" ARG("s", "window", "in")
	ARG("a{sv}", "options", "in") ARG("o", "handle", "out") "</method>" PROP_VERSION "</interface>";
static const char XML_TRASH[] =
	"<interface name=\"org.freedesktop.portal.Trash\"><method name=\"TrashFile\">" ARG("h", "fd", "in") ARG("u", "result", "out")
	"</method>" PROP_VERSION "</interface>";
static const char XML_SECRET[] =
	"<interface name=\"org.freedesktop.portal.Secret\"><method name=\"RetrieveSecret\">" ARG("h", "fd", "in")
	ARG("a{sv}", "options", "in") ARG("o", "handle", "out") "</method>" PROP_VERSION "</interface>";
static const char XML_WALLPAPER[] =
	"<interface name=\"org.freedesktop.portal.Wallpaper\">"
	"<method name=\"SetWallpaperURI\">" ARG("s", "parent_window", "in") ARG("s", "uri", "in") ARG("a{sv}", "options", "in")
	ARG("o", "handle", "out") "</method>"
	"<method name=\"SetWallpaperFile\">" ARG("s", "parent_window", "in") ARG("h", "fd", "in") ARG("a{sv}", "options", "in")
	ARG("o", "handle", "out") "</method>" PROP_VERSION "</interface>";
static const char XML_BACKGROUND[] =
	"<interface name=\"org.freedesktop.portal.Background\">"
	"<method name=\"RequestBackground\">" ARG("s", "parent_window", "in") ARG("a{sv}", "options", "in") ARG("o", "handle", "out")
	"</method><method name=\"SetStatus\">" ARG("a{sv}", "options", "in") "</method>" PROP_VERSION "</interface>";
static const char XML_NETWORK[] =
	"<interface name=\"org.freedesktop.portal.NetworkMonitor\">"
	"<method name=\"GetAvailable\">" ARG("b", "available", "out") "</method>"
	"<method name=\"GetMetered\">" ARG("b", "metered", "out") "</method>"
	"<method name=\"GetConnectivity\">" ARG("u", "connectivity", "out") "</method>"
	"<method name=\"GetStatus\">" ARG("a{sv}", "status", "out") "</method>"
	"<method name=\"CanReach\">" ARG("s", "hostname", "in") ARG("u", "port", "in") ARG("b", "reachable", "out") "</method>"
	"<signal name=\"changed\"/>" PROP_VERSION "</interface>";
static const char XML_PROXY[] =
	"<interface name=\"org.freedesktop.portal.ProxyResolver\"><method name=\"Lookup\">" ARG("s", "uri", "in")
	ARG("as", "proxies", "out") "</method>" PROP_VERSION "</interface>";
static const char XML_MEMORY[] =
	"<interface name=\"org.freedesktop.portal.MemoryMonitor\"><signal name=\"LowMemoryWarning\"><arg type=\"y\"/></signal>"
	PROP_VERSION "</interface>";
static const char XML_POWER[] =
	"<interface name=\"org.freedesktop.portal.PowerProfileMonitor\">"
	"<property name=\"power-saver-enabled\" type=\"b\" access=\"read\"/>" PROP_VERSION "</interface>";
static const char XML_GAMEMODE[] =
	"<interface name=\"org.freedesktop.portal.GameMode\">"
	"<method name=\"QueryStatus\">" ARG("i", "pid", "in") ARG("i", "result", "out") "</method>"
	"<method name=\"RegisterGame\">" ARG("i", "pid", "in") ARG("i", "result", "out") "</method>"
	"<method name=\"UnregisterGame\">" ARG("i", "pid", "in") ARG("i", "result", "out") "</method>"
	"<property name=\"Active\" type=\"b\" access=\"read\"/>" PROP_VERSION "</interface>";

void portals_start(DBus *bus)
{
	have_sent = false;
	settings_poll();
	struct {
		const char *iface;
		DBusMethodFn fn;
		const char *xml;
	} list[] = {
		{ "org.freedesktop.portal.Settings", settings_method, XML_SETTINGS },
		{ "org.freedesktop.portal.OpenURI", openuri_method, XML_OPENURI },
		{ "org.freedesktop.portal.Email", email_method, XML_EMAIL },
		{ "org.freedesktop.portal.FileChooser", filechooser_method, XML_FILECHOOSER },
		{ "org.freedesktop.portal.Notification", notification_method, XML_NOTIFICATION },
		{ "org.freedesktop.portal.Inhibit", inhibit_method, XML_INHIBIT },
		{ "org.freedesktop.portal.Account", account_method, XML_ACCOUNT },
		{ "org.freedesktop.portal.Trash", trash_method, XML_TRASH },
		{ "org.freedesktop.portal.Secret", secret_method, XML_SECRET },
		{ "org.freedesktop.portal.Wallpaper", wallpaper_method, XML_WALLPAPER },
		{ "org.freedesktop.portal.Background", background_method, XML_BACKGROUND },
		{ "org.freedesktop.portal.NetworkMonitor", network_method, XML_NETWORK },
		{ "org.freedesktop.portal.ProxyResolver", proxy_method, XML_PROXY },
		{ "org.freedesktop.portal.MemoryMonitor", monitor_method, XML_MEMORY },
		{ "org.freedesktop.portal.PowerProfileMonitor", monitor_method, XML_POWER },
		{ "org.freedesktop.portal.GameMode", gamemode_method, XML_GAMEMODE },
	};
	for (usize i = 0; i < ARRAY_LEN(list); i++)
		dbus_export(bus, DESKTOP, list[i].iface, list[i].fn, NULL, list[i].xml);
	dbus_request_name(bus, "org.freedesktop.portal.Desktop", DBUS_NAME_REPLACE);
}
