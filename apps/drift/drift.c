/* Drift: the web browser.
 *
 * The window is ordinary libzui. Pages load on a small pool of worker
 * threads: one job fetches and parses a document and its style sheets, others
 * fetch images, and relayouts (after a resize or when pictures arrive) run
 * there too, so the window never waits on the network or on layout. A
 * finished layout is handed over as a whole Page, which the UI swaps in. */
#include "drift.h"

#define WORKERS 6
#define HOME_URL "drift:home"
#define SEARCH_URL "https://html.duckduckgo.com/html/?q="
#define USER_AGENT "Mozilla/5.0 (X11; Zurbax x86_64) Drift/1.0"

/* ---- Jobs ------------------------------------------------------------------------------------ */

enum { JOB_LOAD, JOB_LAYOUT, JOB_IMAGE, JOB_FETCH };

typedef struct {
	char *url;
	char *text;
	usize len;
	int *pending;
} Fetch;

typedef struct Job Job;
struct Job {
	int kind;
	Doc *doc;
	Image *img;
	char *url, *post, *post_type;
	f32 width, vh;
	bool restyle;
	Fetch *fetch;
	Job *next;
};

static struct {
	Mutex lock;
	Job *hi, *hi_tail, *lo, *lo_tail;
	int seq;
} Q;

static Ui *the_ui;
static const char *start_arg;

static int hexval(const char *h)
{
	int v = 0;
	for (int i = 0; i < 2; i++)
		v = v * 16 + (is_digit(h[i]) ? h[i] - '0' : to_lower(h[i]) - 'a' + 10);
	return v;
}

static void doc_ref(Doc *d)
{
	atomic_add(&d->refs, 1);
}

static void doc_release(Doc *d)
{
	if (!d || atomic_add(&d->refs, -1) > 0)
		return;
	node_free(d->root);
	for (int i = 0; i < d->nsheets; i++)
		sheet_free(d->sheets[i]);
	mem_free(d->sheets);
	for (int i = 0; i < d->nimages; i++) {
		canvas_free(d->images[i]->canvas);
		canvas_free(d->images[i]->tinted);
		mem_free(d->images[i]->url);
		mem_free(d->images[i]);
	}
	mem_free(d->images);
	map_free(&d->image_map, NULL);
	page_free(d->page);
	page_free(d->new_page);
	mem_free(d->url);
	mem_free(d->base);
	mem_free(d->title);
	mem_free(d);
}

static void enqueue(Job *j, bool high)
{
	if (j->doc)
		doc_ref(j->doc);
	mutex_lock(&Q.lock);
	Job **head = high ? &Q.hi : &Q.lo, **tail = high ? &Q.hi_tail : &Q.lo_tail;
	if (j->kind == JOB_FETCH) {
		/* style sheets jump the queue: a page is waiting on them */
		j->next = *head;
		*head = j;
		if (!*tail)
			*tail = j;
	} else {
		j->next = NULL;
		if (*tail)
			(*tail)->next = j;
		else
			*head = j;
		*tail = j;
	}
	atomic_add(&Q.seq, 1);
	mutex_unlock(&Q.lock);
	futex_wake(&Q.seq, 1);
}

static Job *pop_locked(bool fetch_only)
{
	Job **head = &Q.hi, **tail = &Q.hi_tail;
	for (int pass = 0; pass < 2; pass++) {
		if (*head && (!fetch_only || (*head)->kind == JOB_FETCH)) {
			Job *j = *head;
			*head = j->next;
			if (!*head)
				*tail = NULL;
			return j;
		}
		if (fetch_only)
			return NULL;
		head = &Q.lo, tail = &Q.lo_tail;
	}
	return NULL;
}

static void job_free(Job *j)
{
	if (j->doc)
		doc_release(j->doc);
	mem_free(j->url);
	mem_free(j->post);
	mem_free(j->post_type);
	mem_free(j);
}

static void run_job(Job *j);

static void worker(void *arg)
{
	UNUSED(arg);
	for (;;) {
		int seen = atomic_load(&Q.seq);
		mutex_lock(&Q.lock);
		Job *j = pop_locked(false);
		mutex_unlock(&Q.lock);
		if (!j) {
			futex_wait(&Q.seq, seen, 5000);
			continue;
		}
		run_job(j);
		job_free(j);
	}
}

/* ---- Cookies ------------------------------------------------------------------------------------- */

typedef struct {
	char *domain, *path, *name, *value;
	bool host_only, secure;
} Cookie;

static struct {
	Mutex lock;
	VEC(Cookie) list;
} jar;

static bool domain_match(const char *host, const char *domain, bool host_only)
{
	if (str_eq_nocase(host, domain))
		return true;
	if (host_only)
		return false;
	usize hl = strlen(host), dl = strlen(domain);
	return hl > dl && host[hl - dl - 1] == '.' && str_eq_nocase(host + hl - dl, domain);
}

static void cookie_parse(const Url *u, const char *line)
{
	const char *semi = str_chr(line, ';');
	usize first = semi ? (usize)(semi - line) : strlen(line);
	const char *eq = memchr(line, '=', first);
	if (!eq)
		return;
	Cookie c = { 0 };
	c.name = str_trim(str_ndup(line, (usize)(eq - line)));
	char *nm = c.name;
	c.name = str_dup(nm);
	mem_free(nm);
	char *val = str_ndup(eq + 1, first - (usize)(eq - line) - 1);
	c.value = str_dup(str_trim(val));
	mem_free(val);
	c.host_only = true;
	c.domain = str_dup(u->host);
	/* default path: the request's directory */
	char path[1024];
	str_copy(path, u->path, sizeof path);
	char *q = (char *)str_chr(path, '?');
	if (q)
		*q = 0;
	char *slash = (char *)str_rchr(path, '/');
	if (slash && slash != path)
		*slash = 0;
	else
		str_copy(path, "/", sizeof path);
	c.path = str_dup(path);
	bool remove = false;
	for (const char *p = semi; p && *p;) {
		p++;
		const char *e = str_chr(p, ';');
		usize n = e ? (usize)(e - p) : strlen(p);
		char attr[512];
		str_copy(attr, p, MIN(sizeof attr, n + 1));
		char *a = str_trim(attr);
		char *v = (char *)str_chr(a, '=');
		if (v)
			*v++ = 0;
		if (str_eq_nocase(a, "domain") && v) {
			while (*v == '.')
				v++;
			if (*v && domain_match(u->host, v, false)) {
				mem_free(c.domain);
				c.domain = str_dup(v);
				c.host_only = false;
			}
		} else if (str_eq_nocase(a, "path") && v && *v == '/') {
			mem_free(c.path);
			c.path = str_dup(v);
		} else if (str_eq_nocase(a, "max-age") && v && str_to_int(v, NULL) <= 0) {
			remove = true;
		} else if (str_eq_nocase(a, "secure")) {
			c.secure = true;
		}
		p = e;
	}
	mutex_lock(&jar.lock);
	for (usize i = 0; i < jar.list.len; i++) {
		Cookie *o = &jar.list.data[i];
		if (str_eq(o->name, c.name) && str_eq_nocase(o->domain, c.domain) && str_eq(o->path, c.path)) {
			mem_free(o->name), mem_free(o->value), mem_free(o->domain), mem_free(o->path);
			vec_remove(&jar.list, i);
			break;
		}
	}
	if (!remove && jar.list.len < 3000) {
		vec_push(&jar.list, c);
	} else {
		mem_free(c.name), mem_free(c.value), mem_free(c.domain), mem_free(c.path);
	}
	mutex_unlock(&jar.lock);
}

static void cookies_from(void *user, const char *url, int status, const char *headers)
{
	UNUSED(user), UNUSED(status);
	Url u;
	if (!url_parse(url, &u))
		return;
	for (const char *p = headers; *p;) {
		const char *nl = str_chr(p, '\n');
		usize n = nl ? (usize)(nl - p) : strlen(p);
		if (n > 11 && str_eq_nocase_n(p, "set-cookie:", 11)) {
			char *line = str_ndup(p + 11, n - 11);
			cookie_parse(&u, str_trim(line));
			mem_free(line);
		}
		p = nl ? nl + 1 : p + n;
	}
}

static char *cookies_for(void *user, const char *url)
{
	UNUSED(user);
	Url u;
	if (!url_parse(url, &u))
		return NULL;
	bool https = str_eq(u.scheme, "https");
	StrBuf sb = { 0 };
	mutex_lock(&jar.lock);
	vec_each(&jar.list, c)
	{
		if (!domain_match(u.host, c->domain, c->host_only) || !str_starts(u.path, c->path) || (c->secure && !https))
			continue;
		sb_adds(&sb, sb.len ? "; " : "Cookie: ");
		sb_fmt(&sb, "%s=%s", c->name, c->value);
	}
	mutex_unlock(&jar.lock);
	if (!sb.len)
		return NULL;
	sb_adds(&sb, "\r\n");
	return sb_take(&sb);
}

/* ---- Fetching --------------------------------------------------------------------------------------- */

static void on_progress(void *user, usize got, usize total)
{
	Doc *d = user;
	if (!d)
		return;
	int pm = total ? (int)MIN(900, 100 + got * 800 / total) : (int)MIN(850, 150 + got / 3000);
	if (pm > atomic_load(&d->progress)) {
		atomic_store(&d->progress, pm);
		ui_wake(the_ui);
	}
}

static HttpResponse *web_get(const char *url, const char *accept, const char *method, const char *body, const char *ctype,
			     const char *referer, Doc *progress_doc)
{
	HttpOptions o = { .method = method, .user_agent = USER_AGENT, .accept = accept, .hop_headers = cookies_for,
			  .hop_response = cookies_from, .timeout_ms = 20000, .max_body = 48u << 20, .max_redirects = 10 };
	StrBuf h = { 0 };
	sb_adds(&h, "Accept-Language: en-US,en;q=0.8\r\n");
	if (referer)
		sb_fmt(&h, "Referer: %s\r\n", referer);
	if (ctype)
		sb_fmt(&h, "Content-Type: %s\r\n", ctype);
	o.headers = h.data;
	if (body) {
		o.body = body;
		o.body_len = strlen(body);
	}
	if (progress_doc) {
		o.progress = on_progress;
		o.user = progress_doc;
	}
	HttpResponse *r = http_request(url, &o);
	sb_free(&h);
	return r;
}

/* Style sheets are shared between pages of the same site. */
static struct {
	Mutex lock;
	Map map;   /* url -> StrBuf* */
	int count;
} cache;

static char *fetch_text(const char *url, usize *len)
{
	mutex_lock(&cache.lock);
	StrBuf *hit = map_get(&cache.map, url);
	if (hit) {
		char *copy = str_ndup(sb_str(hit), hit->len);
		*len = hit->len;
		mutex_unlock(&cache.lock);
		return copy;
	}
	mutex_unlock(&cache.lock);
	HttpResponse *r = web_get(url, "text/css,*/*;q=0.1", NULL, NULL, NULL, NULL, NULL);
	char *text = NULL;
	*len = 0;
	if (r->status >= 200 && r->status < 300 && r->body) {
		text = str_ndup((char *)r->body, r->body_len);
		*len = r->body_len;
		mutex_lock(&cache.lock);
		if (cache.count > 48) {
			map_free(&cache.map, (void (*)(void *))sb_free);
			cache.count = 0;
		}
		StrBuf *sb = NEW(StrBuf);
		sb_add(sb, text, *len);
		map_set(&cache.map, url, sb);
		cache.count++;
		mutex_unlock(&cache.lock);
	}
	http_response_free(r);
	return text;
}

static u8 *data_url(const char *url, usize *len, char *type, usize tcap)
{
	const char *comma = str_chr(url, ',');
	if (!comma)
		return NULL;
	const char *meta = url + 5;
	usize ml = (usize)(comma - meta);
	str_copy(type, meta, MIN(tcap, ml + 1));
	char *semi = (char *)str_chr(type, ';');
	if (semi)
		*semi = 0;
	bool b64 = ml >= 7 && memcmp(comma - 7, ";base64", 7) == 0;
	if (b64)
		return base64_decode(comma + 1, strlen(comma + 1), len);
	/* percent-encoded */
	usize n = strlen(comma + 1);
	u8 *out = mem_alloc(n + 1);
	usize w = 0;
	for (const char *p = comma + 1; *p; p++) {
		if (*p == '%' && is_xdigit(p[1]) && is_xdigit(p[2])) {
			out[w++] = (u8)hexval(p + 1);
			p += 2;
		} else {
			out[w++] = (u8)*p;
		}
	}
	out[w] = 0;
	*len = w;
	return out;
}

/* ---- Images ------------------------------------------------------------------------------------------ */

Image *doc_image(Doc *d, const char *url)
{
	mutex_lock(&d->lock);
	Image *im = map_get(&d->image_map, url);
	if (!im) {
		im = NEW(Image);
		im->url = str_dup(url);
		map_set(&d->image_map, url, im);
		if (d->nimages % 64 == 0)
			d->images = mem_realloc(d->images, sizeof(Image *) * (usize)(d->nimages + 64));
		d->images[d->nimages++] = im;
		if (str_starts(url, "http://") || str_starts(url, "https://") || str_starts(url, "data:") || str_starts(url, "file://")) {
			im->state = IMG_LOADING;
			Job *j = NEW(Job);
			j->kind = JOB_IMAGE;
			j->doc = d;
			j->img = im;
			mutex_unlock(&d->lock);
			enqueue(j, false);
			return im;
		}
	}
	mutex_unlock(&d->lock);
	return im;
}

static void run_image(Job *j)
{
	Doc *d = j->doc;
	Image *im = j->img;
	Canvas *c = NULL;
	int w = 0, h = 0;
	if (!atomic_load(&d->cancelled)) {
		u8 *data = NULL;
		usize len = 0;
		char type[96] = "";
		if (str_starts(im->url, "data:")) {
			data = data_url(im->url, &len, type, sizeof type);
		} else if (str_starts(im->url, "file://")) {
			data = (u8 *)file_read(im->url + 7, &len);
		} else {
			HttpResponse *r = web_get(im->url, "image/png,image/jpeg,image/gif,image/svg+xml,image/*;q=0.5", NULL, NULL, NULL, d->url, NULL);
			if (r->status == 200 && r->body) {
				data = r->body;
				len = r->body_len;
				r->body = NULL;
				http_header(r, "Content-Type", type, sizeof type);
			}
			http_response_free(r);
		}
		if (data && len) {
			bool svg = str_find(type, "svg") || str_ends(im->url, ".svg") || (len > 5 && (memcmp(data, "<svg", 4) == 0 || memcmp(data, "<?xml", 5) == 0));
			if (svg) {
				SvgDoc *doc = svg_parse((char *)data, len);
				if (doc) {
					f32 sw, sh;
					svg_size(doc, &sw, &sh);
					if (sw <= 0 || sh <= 0)
						sw = 300, sh = 150;
					w = (int)sw, h = (int)sh;
					f32 k = minf(2, 2048 / maxf(sw, sh));
					c = svg_rasterize(doc, MAX(1, (int)(sw * k)), MAX(1, (int)(sh * k)), COLOR_BLACK);
					svg_free(doc);
				}
			} else {
				c = image_decode(data, len);
				if (c) {
					w = c->width, h = c->height;
					int mx = MAX(c->width, c->height);
					if (mx > 2400) {
						Canvas *s = canvas_resize(c, c->width * 2400 / mx, c->height * 2400 / mx);
						canvas_free(c);
						c = s;
					}
				}
			}
		}
		mem_free(data);
	}
	mutex_lock(&d->lock);
	im->canvas = c;
	im->w = w, im->h = h;
	im->state = c ? IMG_READY : IMG_FAILED;
	d->images_done++;
	mutex_unlock(&d->lock);
	ui_wake(the_ui);
}

/* ---- Loading documents --------------------------------------------------------------------------------- */

static void fail(Doc *d, const char *why)
{
	mutex_lock(&d->lock);
	str_copy(d->error, why, sizeof d->error);
	d->state = DOC_FAILED;
	mutex_unlock(&d->lock);
	ui_wake(the_ui);
}

static char *escape_html(const char *s, usize n)
{
	StrBuf sb = { 0 };
	for (usize i = 0; i < n; i++) {
		char c = s[i];
		if (c == '<')
			sb_adds(&sb, "&lt;");
		else if (c == '>')
			sb_adds(&sb, "&gt;");
		else if (c == '&')
			sb_adds(&sb, "&amp;");
		else if (c == '"')
			sb_adds(&sb, "&quot;");
		else if (c)
			sb_addc(&sb, c);
	}
	return sb_take(&sb);
}

/* Latin-1 and Windows-1252 pages, re-encoded as UTF-8. */
static char *to_utf8(const u8 *s, usize n, usize *out_len)
{
	static const u16 cp1252[32] = { 0x20ac, 0, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17d, 0,
					0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0, 0x17e, 0x178 };
	StrBuf sb = { 0 };
	sb_reserve(&sb, n + n / 8);
	for (usize i = 0; i < n; i++) {
		u8 c = s[i];
		if (c < 0x80)
			sb_addc(&sb, (char)c);
		else if (c < 0xa0 && cp1252[c - 0x80])
			sb_addcp(&sb, cp1252[c - 0x80]);
		else
			sb_addcp(&sb, c);
	}
	*out_len = sb.len;
	return sb_take(&sb);
}

static bool is_latin1(const char *ctype, const u8 *body, usize len)
{
	char low[160];
	str_copy(low, ctype, sizeof low);
	str_lower(low);
	const char *cs = str_find(low, "charset=");
	if (!cs) {
		/* <meta charset=...> near the top */
		usize n = MIN(len, 2048);
		char *head = str_ndup((const char *)body, n);
		str_lower(head);
		const char *m = str_find(head, "charset=");
		bool r = m && (str_starts(m + 8, "iso-8859-1") || str_starts(m + 8, "\"iso-8859-1") || str_starts(m + 8, "windows-1252") ||
			       str_starts(m + 8, "\"windows-1252"));
		mem_free(head);
		return r;
	}
	cs += 8;
	if (*cs == '"')
		cs++;
	return str_starts(cs, "iso-8859-1") || str_starts(cs, "windows-1252") || str_starts(cs, "latin1");
}

static const char *file_ext(const char *url)
{
	const char *q = str_chr(url, '?');
	usize n = q ? (usize)(q - url) : strlen(url);
	for (usize i = n; i > 0; i--) {
		if (url[i - 1] == '/')
			return "";
		if (url[i - 1] == '.')
			return url + i;
	}
	return "";
}

static bool ext_is(const char *ext, const char *e)
{
	usize n = strlen(e);
	return str_eq_nocase_n(ext, e, n) && (!ext[n] || ext[n] == '?' || ext[n] == '#');
}

static void save_download(Doc *d, const char *url, const char *disposition, const u8 *body, usize len)
{
	char name[256] = "";
	const char *fnp = str_find_nocase(disposition, "filename=");
	if (fnp) {
		fnp += 9;
		if (*fnp == '"')
			fnp++;
		usize k = 0;
		while (fnp[k] && fnp[k] != '"' && fnp[k] != ';' && k < sizeof name - 1)
			k++;
		str_copy(name, fnp, k + 1);
	}
	if (!name[0]) {
		Url u;
		if (url_parse(url, &u)) {
			char path[2048];
			str_copy(path, u.path, sizeof path);
			char *q = (char *)str_chr(path, '?');
			if (q)
				*q = 0;
			const char *base = str_rchr(path, '/');
			str_copy(name, base && base[1] ? base + 1 : "download", sizeof name);
		}
	}
	for (char *p = name; *p; p++)
		if (*p == '/' || *p == '\\')
			*p = '_';
	char *dir = path_join(home_dir(), "Downloads");
	dir_make(dir, 0755);
	char *path = path_join(dir, name);
	/* don't overwrite: name (2).ext */
	for (int k = 2; file_exists(path) && k < 100; k++) {
		mem_free(path);
		const char *dot = str_rchr(name, '.');
		char alt[300];
		if (dot && dot != name)
			fmt(alt, sizeof alt, "%.*s (%d)%s", (int)(dot - name), name, k, dot);
		else
			fmt(alt, sizeof alt, "%s (%d)", name, k);
		path = path_join(dir, alt);
	}
	bool ok = file_write(path, body, len);
	mutex_lock(&d->lock);
	if (ok) {
		str_copy(d->error, path, sizeof d->error);
		d->state = DOC_DOWNLOADED;
	} else {
		str_copy(d->error, "The file couldn't be saved to Downloads.", sizeof d->error);
		d->state = DOC_FAILED;
	}
	mutex_unlock(&d->lock);
	mem_free(dir);
	mem_free(path);
	ui_wake(the_ui);
}

static void collect_title(Doc *d)
{
	Node *t = node_find(d->root, "title");
	if (!t)
		return;
	StrBuf sb = { 0 };
	node_text_content(t, &sb);
	/* collapse white space */
	StrBuf out = { 0 };
	bool sp = false;
	for (const char *p = sb_str(&sb); *p; p++) {
		if (is_space(*p)) {
			sp = out.len > 0;
			continue;
		}
		if (sp)
			sb_addc(&out, ' ');
		sp = false;
		sb_addc(&out, *p);
	}
	sb_free(&sb);
	if (out.len)
		d->title = sb_take(&out);
	else
		sb_free(&out);
}

typedef struct {
	Node *node;              /* <style>, or NULL */
	char *url;               /* <link>, or NULL */
	Fetch fetch;
} SheetSrc;

typedef VEC(SheetSrc) SheetList;

static void find_sheets(Node *n, SheetList *out, f32 vw, f32 vh)
{
	for (Node *c = n->first; c; c = c->next) {
		if (c->type != NODE_ELEMENT)
			continue;
		if (str_eq(c->tag, "style")) {
			const char *media = node_attr(c, "media");
			if (css_media_matches(media, vw, vh))
				vec_push(out, (SheetSrc){ .node = c });
			continue;
		}
		if (str_eq(c->tag, "link")) {
			const char *rel = node_attr(c, "rel"), *href = node_attr(c, "href");
			if (rel && href && *href && str_find_nocase(rel, "stylesheet") && !str_find_nocase(rel, "alternate")) {
				const char *media = node_attr(c, "media");
				if (css_media_matches(media, vw, vh))
					vec_push(out, (SheetSrc){ .url = str_dup(href) });
			}
			continue;
		}
		if (str_eq(c->tag, "svg") || str_eq(c->tag, "template"))
			continue;
		find_sheets(c, out, vw, vh);
	}
}

static void load_sheets(Doc *d, f32 vw, f32 vh)
{
	SheetList src = { 0 };
	find_sheets(d->root, &src, vw, vh);
	int pending = 0;
	const char *base = d->base ? d->base : d->url;
	vec_each(&src, s)
	{
		if (!s->url)
			continue;
		char *abs = url_resolve(base, s->url);
		mem_free(s->url);
		s->url = abs;
		s->fetch.url = abs;
		s->fetch.pending = &pending;
		atomic_add(&pending, 1);
		Job *j = NEW(Job);
		j->kind = JOB_FETCH;
		j->fetch = &s->fetch;
		enqueue(j, true);
	}
	/* Help with the fetches while waiting: all workers might be loaders. */
	for (;;) {
		int left = atomic_load(&pending);
		if (left <= 0)
			break;
		mutex_lock(&Q.lock);
		Job *j = pop_locked(true);
		mutex_unlock(&Q.lock);
		if (j) {
			run_job(j);
			job_free(j);
			continue;
		}
		futex_wait(&pending, left, 200);
	}
	VEC(Sheet *) sheets = { 0 };
	vec_each(&src, s)
	{
		Sheet *sh = NULL;
		const char *sbase = s->url ? s->url : base;
		if (s->node) {
			StrBuf t = { 0 };
			node_text_content(s->node, &t);
			sh = css_parse(sb_str(&t), t.len, base);
			sb_free(&t);
		} else if (s->fetch.text) {
			sh = css_parse(s->fetch.text, s->fetch.len, sbase);
		}
		if (!sh)
			continue;
		/* @import, one level deep, goes in front of the sheet that asked. */
		char **imports;
		int ni = sheet_imports(sh, &imports);
		for (int i = 0; i < ni && i < 16 && !atomic_load(&d->cancelled); i++) {
			usize len;
			char *text = fetch_text(imports[i], &len);
			if (text) {
				vec_push(&sheets, css_parse(text, len, imports[i]));
				mem_free(text);
			}
		}
		vec_push(&sheets, sh);
	}
	vec_each(&src, s)
	{
		mem_free(s->url);
		mem_free(s->fetch.text);
	}
	vec_free(&src);
	d->sheets = sheets.data;
	d->nsheets = (int)sheets.len;
}

static void publish(Doc *d, Page *p)
{
	mutex_lock(&d->lock);
	page_free(d->new_page);
	d->new_page = p;
	d->state = DOC_READY;
	mutex_unlock(&d->lock);
	atomic_store(&d->layout_busy, 0);
	ui_wake(the_ui);
}

static void run_load(Job *j)
{
	Doc *d = j->doc;
	char *url = str_dup(j->url);
	char *post = j->post ? str_dup(j->post) : NULL;
	char *html = NULL;
	usize hlen = 0;
	for (int hop = 0; hop < 4; hop++) {
		u8 *body = NULL;
		usize blen = 0;
		char ctype[160] = "", disp[512] = "";
		char *final = NULL;
		const char *ext = file_ext(url);
		if (str_starts(url, "file://")) {
			body = (u8 *)file_read(url + 7, &blen);
			if (!body) {
				fail(d, "That file doesn't exist, or can't be read.");
				goto out;
			}
			final = str_dup(url);
			if (ext_is(ext, "html") || ext_is(ext, "htm"))
				str_copy(ctype, "text/html", sizeof ctype);
			else if (ext_is(ext, "png") || ext_is(ext, "jpg") || ext_is(ext, "jpeg") || ext_is(ext, "gif") || ext_is(ext, "svg") || ext_is(ext, "bmp"))
				str_copy(ctype, "image/any", sizeof ctype);
			else
				str_copy(ctype, "text/plain", sizeof ctype);
		} else if (str_starts(url, "data:")) {
			body = data_url(url, &blen, ctype, sizeof ctype);
			final = str_dup(url);
			if (!body) {
				fail(d, "This address can't be shown.");
				goto out;
			}
		} else {
			HttpResponse *r = web_get(url, "text/html,application/xhtml+xml,*/*;q=0.8", post ? "POST" : NULL, post,
						  post ? (j->post_type ? j->post_type : "application/x-www-form-urlencoded") : NULL, NULL, d);
			if (atomic_load(&d->cancelled)) {
				http_response_free(r);
				goto out;
			}
			if (!r->status) {
				char why[256];
				fmt(why, sizeof why, "%c%s.", to_upper(r->error[0]), r->error[0] ? r->error + 1 : "The site didn't answer");
				fail(d, why);
				http_response_free(r);
				goto out;
			}
			http_header(r, "Content-Type", ctype, sizeof ctype);
			http_header(r, "Content-Disposition", disp, sizeof disp);
			body = r->body;
			blen = r->body_len;
			r->body = NULL;
			final = str_dup(r->final_url);
			http_response_free(r);
		}
		mem_free(post);
		post = NULL;
		str_lower(ctype);
		bool is_html = str_starts(ctype, "text/html") || str_find(ctype, "xhtml") || (!ctype[0] && blen && body[0] == '<');
		bool is_image = str_starts(ctype, "image/");
		bool is_text = str_starts(ctype, "text/") || str_find(ctype, "json") || str_find(ctype, "javascript") || str_find(ctype, "xml");
		if (str_find_nocase(disp, "attachment") || (!is_html && !is_image && !is_text)) {
			save_download(d, final, disp, body, blen);
			mem_free(body);
			mem_free(final);
			goto out;
		}
		mem_free(d->url);
		d->url = final;
		mem_free(html);
		if (is_image) {
			char *e = escape_html(final, strlen(final));
			html = str_fmt("<html><head><title>%s</title></head><body style=\"margin:0;background:#262626;text-align:center\">"
				       "<img src=\"%s\" style=\"max-width:100%%;margin-top:16px\"></body></html>",
				       str_rchr(final, '/') ? str_rchr(final, '/') + 1 : final, e);
			mem_free(e);
			hlen = strlen(html);
			mem_free(body);
		} else if (!is_html) {
			char *e = escape_html((char *)body, blen);
			html = str_fmt("<html><body style=\"margin:16px\"><pre style=\"white-space:pre-wrap\">%s</pre></body></html>", e);
			hlen = strlen(html);
			mem_free(e);
			mem_free(body);
		} else if (is_latin1(ctype, body, blen)) {
			html = to_utf8(body, blen, &hlen);
			mem_free(body);
		} else {
			html = (char *)body;
			hlen = blen;
			if (hlen >= 3 && (u8)html[0] == 0xef && (u8)html[1] == 0xbb && (u8)html[2] == 0xbf)
				memmove(html, html + 3, hlen -= 3);
		}
		Node *root = html_parse(html, hlen);
		/* <meta http-equiv="refresh" content="0; url=..."> */
		char *refresh = NULL;
		for (Node *m = node_find(root, "meta"); m; m = NULL) {
			(void)m;
		}
		{
			Node *head = node_find(root, "head");
			for (Node *m = head ? head->first : NULL; m; m = m->next) {
				const char *he = m->type == NODE_ELEMENT && str_eq(m->tag, "meta") ? node_attr(m, "http-equiv") : NULL;
				const char *content = he && str_eq_nocase(he, "refresh") ? node_attr(m, "content") : NULL;
				if (!content)
					continue;
				const char *e;
				i64 delay = str_to_int(content, &e);
				const char *u = str_find_nocase(content, "url=");
				if (u && delay <= 2) {
					u += 4;
					char tmp[2048];
					str_copy(tmp, u, sizeof tmp);
					char *t = str_trim(tmp);
					if (*t == '\'' || *t == '"') {
						t++;
						usize l = strlen(t);
						if (l && (t[l - 1] == '\'' || t[l - 1] == '"'))
							t[l - 1] = 0;
					}
					refresh = url_resolve(d->url, t);
				}
				break;
			}
		}
		if (refresh && hop < 3 && !str_eq(refresh, d->url)) {
			node_free(root);
			mem_free(url);
			url = refresh;
			continue;
		}
		mem_free(refresh);
		d->root = root;
		break;
	}
	if (!d->root || atomic_load(&d->cancelled))
		goto out;
	{
		/* Theme switchers normally put "light" or "dark" on <html> from a
		 * script; without scripts, say light so only one theme's parts show. */
		Node *html_el = node_find(d->root, "html");
		if (html_el && !node_has_class(html_el, "dark") && !node_has_class(html_el, "light")) {
			const char *cls = node_attr(html_el, "class");
			char *v = str_fmt("%s%slight", cls ? cls : "", cls && *cls ? " " : "");
			bool set = false;
			for (int i = 0; i < html_el->nattrs; i++)
				if (str_eq(html_el->attrs[i * 2], "class")) {
					mem_free(html_el->attrs[i * 2 + 1]);
					html_el->attrs[i * 2 + 1] = v;
					set = true;
				}
			if (!set) {
				html_el->attrs = mem_realloc(html_el->attrs, sizeof(char *) * (usize)(html_el->nattrs + 1) * 2);
				html_el->attrs[html_el->nattrs * 2] = str_dup("class");
				html_el->attrs[html_el->nattrs * 2 + 1] = v;
				html_el->nattrs++;
			}
		}
	}
	collect_title(d);
	{
		Node *b = node_find(d->root, "base");
		const char *href = b ? node_attr(b, "href") : NULL;
		if (href && *href)
			d->base = url_resolve(d->url, href);
		Node *head = node_find(d->root, "head");
		for (Node *m = head ? head->first : NULL; m; m = m->next) {
			const char *name = m->type == NODE_ELEMENT && str_eq(m->tag, "meta") ? node_attr(m, "name") : NULL;
			if (name && str_eq_nocase(name, "theme-color")) {
				Color c;
				const char *content = node_attr(m, "content");
				if (content && css_parse_color(content, &c))
					d->theme = c;
			}
		}
	}
	atomic_store(&d->progress, MAX(atomic_load(&d->progress), 900));
	ui_wake(the_ui);
	load_sheets(d, j->width, j->vh);
	if (atomic_load(&d->cancelled))
		goto out;
	mutex_lock(&d->layout_lock);
	style_tree(d->root, d->sheets, d->nsheets, j->width, j->vh);
	Page *p = layout_doc(d, j->width, j->vh);
	mutex_unlock(&d->layout_lock);
	publish(d, p);
out:
	mem_free(url);
	mem_free(post);
	if (html && d->root)
		mem_free(html);
	else
		mem_free(html);
}

static void run_layout(Job *j)
{
	Doc *d = j->doc;
	if (!atomic_load(&d->cancelled) && d->root) {
		mutex_lock(&d->layout_lock);
		if (j->restyle)
			style_tree(d->root, d->sheets, d->nsheets, j->width, j->vh);
		Page *p = layout_doc(d, j->width, j->vh);
		mutex_unlock(&d->layout_lock);
		publish(d, p);
	}
	atomic_store(&d->layout_busy, 0);
}

static void run_job(Job *j)
{
	switch (j->kind) {
	case JOB_LOAD:
		run_load(j);
		break;
	case JOB_LAYOUT:
		run_layout(j);
		break;
	case JOB_IMAGE:
		run_image(j);
		break;
	case JOB_FETCH:
		j->fetch->text = fetch_text(j->fetch->url, &j->fetch->len);
		atomic_add(j->fetch->pending, -1);
		futex_wake(j->fetch->pending, 1);
		break;
	}
}

/* ---- Browser state ------------------------------------------------------------------------------------- */

typedef struct {
	char *url, *title;
	f32 scroll;
} Hist;

typedef struct {
	VEC(Hist) hist;
	int pos;                 /* current entry */
	Doc *doc;                /* NULL on the start page */
	f32 scroll, target;
	f32 laid_w, want_w;
	f64 resized_at, laid_at;
	int laid_images;
	char *anchor;            /* scroll here once laid out */
	bool restore_scroll;
	StrBuf addr;
	bool addr_dirty;         /* the user is typing in the address bar */
} Tab;

typedef struct {
	char *url, *title;
} Mark;

typedef VEC(Mark) MarkList;

static struct {
	VEC(Tab *) tabs;
	int cur;
	MarkList marks;          /* bookmarks */
	MarkList visited;        /* newest last */
	bool find_open;
	StrBuf find;
	int find_cur, find_matches;
	f32 zoom;
	const Node *hover;
	char hover_url[1024];
	StrBuf home_q;
	f32 view_h;
	int suggest_sel;
	Conf *conf;
	char *dir;
} B;

static Tab *cur_tab(void)
{
	return B.tabs.data[B.cur];
}

static void save_marks(const char *file, MarkList *list)
{
	StrBuf sb = { 0 };
	vec_each(list, m) sb_fmt(&sb, "%s\t%s\n", m->url, m->title ? m->title : "");
	char *path = path_join(B.dir, file);
	file_write_atomic(path, sb_str(&sb), sb.len);
	mem_free(path);
	sb_free(&sb);
}

static void load_marks(const char *file, MarkList *list)
{
	char *path = path_join(B.dir, file);
	usize len;
	char *text = file_read(path, &len);
	mem_free(path);
	if (!text)
		return;
	for (char *line = text; *line;) {
		char *nl = (char *)str_chr(line, '\n');
		if (nl)
			*nl = 0;
		char *tab = (char *)str_chr(line, '\t');
		if (tab) {
			*tab = 0;
			if (*line)
				vec_push(list, (Mark){ str_dup(line), str_dup(tab + 1) });
		}
		if (!nl)
			break;
		line = nl + 1;
	}
	mem_free(text);
}

static int mark_index(const char *url)
{
	for (usize i = 0; i < B.marks.len; i++)
		if (str_eq(B.marks.data[i].url, url))
			return (int)i;
	return -1;
}

static void remember_visit(const char *url, const char *title)
{
	if (!url || str_starts(url, "drift:") || str_starts(url, "data:"))
		return;
	for (usize i = 0; i < B.visited.len; i++)
		if (str_eq(B.visited.data[i].url, url)) {
			mem_free(B.visited.data[i].url);
			mem_free(B.visited.data[i].title);
			vec_remove(&B.visited, i);
			break;
		}
	vec_push(&B.visited, (Mark){ str_dup(url), str_dup(title ? title : "") });
	while (B.visited.len > 400) {
		mem_free(B.visited.data[0].url);
		mem_free(B.visited.data[0].title);
		vec_remove(&B.visited, 0);
	}
	save_marks("history.tsv", &B.visited);
}

static const char *tab_url(Tab *t)
{
	if (t->doc && t->doc->url)
		return t->doc->url;
	return t->hist.len ? t->hist.data[t->pos].url : HOME_URL;
}

static const char *tab_title(Tab *t)
{
	if (!t->doc)
		return "New Tab";
	if (t->doc->title && t->doc->title[0])
		return t->doc->title;
	Url u;
	static char host[256];
	if (t->doc->url && url_parse(t->doc->url, &u)) {
		str_copy(host, str_starts(u.host, "www.") ? u.host + 4 : u.host, sizeof host);
		return host;
	}
	return "Loading…";
}

static void start_load(Tab *t, const char *url, const char *post, const char *post_type)
{
	if (t->doc) {
		atomic_store(&t->doc->cancelled, 1);
		doc_release(t->doc);
		t->doc = NULL;
	}
	mem_free(t->anchor);
	t->anchor = NULL;
	if (str_eq(url, HOME_URL))
		return;
	const char *hash = str_chr(url, '#');
	if (hash && hash[1])
		t->anchor = str_dup(hash + 1);
	Doc *d = NEW(Doc);
	d->refs = 1;
	d->url = str_ndup(url, hash ? (usize)(hash - url) : strlen(url));
	d->state = DOC_LOADING;
	d->progress = 40;
	t->doc = d;
	t->scroll = t->target = 0;
	Job *j = NEW(Job);
	j->kind = JOB_LOAD;
	j->doc = d;
	j->url = str_dup(d->url);
	j->post = post ? str_dup(post) : NULL;
	j->post_type = post_type ? str_dup(post_type) : NULL;
	j->width = t->want_w > 100 ? t->want_w : 1000;
	j->vh = B.view_h > 100 ? B.view_h : 700;
	t->laid_w = j->width;
	t->laid_images = 0;
	enqueue(j, true);
}

/* The part of a URL before the fragment. */
static usize url_base_len(const char *u)
{
	const char *h = str_chr(u, '#');
	return h ? (usize)(h - u) : strlen(u);
}

static void navigate(Tab *t, const char *url, const char *post, const char *post_type)
{
	/* DuckDuckGo's result links bounce through a redirector; skip it. */
	char *direct = NULL;
	if (str_find(url, "duckduckgo.com/l/?")) {
		const char *u = str_find(url, "uddg=");
		if (u) {
			u += 5;
			StrBuf sb = { 0 };
			for (const char *p = u; *p && *p != '&'; p++) {
				if (*p == '%' && is_xdigit(p[1]) && is_xdigit(p[2])) {
					sb_addc(&sb, (char)hexval(p + 1));
					p += 2;
				} else {
					sb_addc(&sb, *p == '+' ? ' ' : *p);
				}
			}
			direct = sb_take(&sb);
			url = direct;
		}
	}
	/* Same page, different fragment: just scroll. */
	const char *cur = t->doc && t->doc->url ? t->doc->url : NULL;
	const char *hash = str_chr(url, '#');
	if (!post && cur && hash && url_base_len(url) == strlen(cur) && memcmp(url, cur, strlen(cur)) == 0 && t->doc->page) {
		f32 y = page_anchor(t->doc->page, hash + 1);
		if (t->hist.len)
			t->hist.data[t->pos].scroll = t->scroll;
		t->target = y >= 0 ? y : 0;
		goto push;
	}
	if (t->hist.len)
		t->hist.data[t->pos].scroll = t->scroll;
	start_load(t, url, post, post_type);
push:
	/* Drop the forward history and add this. */
	while ((int)t->hist.len > t->pos + 1) {
		Hist h = vec_pop(&t->hist);
		mem_free(h.url);
		mem_free(h.title);
	}
	vec_push(&t->hist, (Hist){ str_dup(url), NULL, 0 });
	t->pos = (int)t->hist.len - 1;
	t->addr_dirty = false;
	mem_free(direct);
}

static void go_history(Tab *t, int delta)
{
	int np = t->pos + delta;
	if (np < 0 || np >= (int)t->hist.len)
		return;
	t->hist.data[t->pos].scroll = t->scroll;
	t->pos = np;
	start_load(t, t->hist.data[np].url, NULL, NULL);
	t->restore_scroll = true;
	t->addr_dirty = false;
}

static void reload(Tab *t)
{
	if (!t->hist.len)
		return;
	f32 keep = t->scroll;
	start_load(t, t->hist.data[t->pos].url, NULL, NULL);
	t->hist.data[t->pos].scroll = keep;
	t->restore_scroll = true;
}

static Tab *new_tab(const char *url, bool focus)
{
	Tab *t = NEW(Tab);
	vec_push(&B.tabs, t);
	if (focus)
		B.cur = (int)B.tabs.len - 1;
	t->want_w = B.tabs.len > 1 ? B.tabs.data[0]->want_w : 0;
	if (url && !str_eq(url, HOME_URL))
		navigate(t, url, NULL, NULL);
	else
		vec_push(&t->hist, (Hist){ str_dup(HOME_URL), NULL, 0 });
	return t;
}

static void close_tab(Ui *ui, int i)
{
	Tab *t = B.tabs.data[i];
	if (t->doc) {
		atomic_store(&t->doc->cancelled, 1);
		doc_release(t->doc);
	}
	vec_each(&t->hist, h)
	{
		mem_free(h->url);
		mem_free(h->title);
	}
	vec_free(&t->hist);
	sb_free(&t->addr);
	mem_free(t->anchor);
	mem_free(t);
	vec_remove(&B.tabs, (usize)i);
	if (!B.tabs.len) {
		ui->quit = true;
		new_tab(HOME_URL, true);
		return;
	}
	if (B.cur >= (int)B.tabs.len)
		B.cur = (int)B.tabs.len - 1;
	else if (B.cur > i)
		B.cur--;
}

/* What the address bar or the start page's search box was given. */
static char *typed_to_url(const char *in)
{
	char buf[2048];
	str_copy(buf, in, sizeof buf);
	char *s = str_trim(buf);
	if (!*s)
		return NULL;
	if (str_starts(s, "http://") || str_starts(s, "https://") || str_starts(s, "file://") || str_starts(s, "drift:") ||
	    str_starts(s, "data:"))
		return str_dup(s);
	if (s[0] == '/')
		return str_fmt("file://%s", s);
	bool spaces = str_chr(s, ' ') != NULL;
	bool dotted = str_chr(s, '.') != NULL;
	bool port = str_starts(s, "localhost") || (str_chr(s, ':') && is_digit(str_chr(s, ':')[1]));
	if (!spaces && (dotted || port)) {
		bool local = str_starts(s, "localhost") || str_starts(s, "10.") || str_starts(s, "192.168.") || str_starts(s, "127.");
		return str_fmt("%s://%s", local ? "http" : "https", s);
	}
	char *q = url_encode(s);
	char *u = str_fmt(SEARCH_URL "%s", q);
	mem_free(q);
	return u;
}

/* ---- Forms ---------------------------------------------------------------------------------------------- */

static Node *form_of(Node *n)
{
	for (Node *p = n; p; p = p->parent)
		if (p->type == NODE_ELEMENT && str_eq(p->tag, "form"))
			return p;
	return NULL;
}

static void add_field(StrBuf *sb, const char *name, const char *value)
{
	if (!name || !*name)
		return;
	char *k = url_encode(name), *v = url_encode(value ? value : "");
	if (sb->len)
		sb_addc(sb, '&');
	sb_fmt(sb, "%s=%s", k, v);
	mem_free(k);
	mem_free(v);
}

static void collect_fields(Node *n, Node *submitter, StrBuf *sb)
{
	for (Node *c = n->first; c; c = c->next) {
		if (c->type != NODE_ELEMENT)
			continue;
		const char *name = node_attr(c, "name");
		if (str_eq(c->tag, "input") && name && !node_attr(c, "disabled")) {
			const char *type = node_attr(c, "type");
			if (!type || !*type)
				type = "text";
			Control *ctl = page_control(c);
			if (str_eq_nocase(type, "checkbox") || str_eq_nocase(type, "radio")) {
				if (ctl->checked) {
					const char *v = node_attr(c, "value");
					add_field(sb, name, v ? v : "on");
				}
			} else if (str_eq_nocase(type, "submit") || str_eq_nocase(type, "image") || str_eq_nocase(type, "button")) {
				if (c == submitter)
					add_field(sb, name, node_attr(c, "value") ? node_attr(c, "value") : "");
			} else if (!str_eq_nocase(type, "file") && !str_eq_nocase(type, "reset")) {
				add_field(sb, name, sb_str(&ctl->value));
			}
		} else if (str_eq(c->tag, "select") && name) {
			Node *opts[256];
			int no = select_options(c, opts, 256);
			Control *ctl = page_control(c);
			if (no && ctl->selected < no) {
				Node *o = opts[ctl->selected];
				const char *v = node_attr(o, "value");
				StrBuf t = { 0 };
				if (!v) {
					node_text_content(o, &t);
					v = str_trim(t.data ? t.data : (char *)"");
				}
				add_field(sb, name, v);
				sb_free(&t);
			}
			continue;
		} else if (str_eq(c->tag, "textarea") && name) {
			add_field(sb, name, sb_str(&page_control(c)->value));
			continue;
		} else if (str_eq(c->tag, "button") && name && c == submitter) {
			add_field(sb, name, node_attr(c, "value") ? node_attr(c, "value") : "");
		}
		collect_fields(c, submitter, sb);
	}
}

static void submit_form(Tab *t, Node *control, Node *submitter, bool new_tab_wanted)
{
	Node *form = form_of(control);
	if (!form || !t->doc)
		return;
	StrBuf data = { 0 };
	collect_fields(form, submitter, &data);
	const char *action = node_attr(form, "action");
	if (submitter && node_attr(submitter, "formaction"))
		action = node_attr(submitter, "formaction");
	const char *base = t->doc->base ? t->doc->base : t->doc->url;
	char *target = url_resolve(base, action && *action ? action : t->doc->url);
	const char *method = node_attr(form, "method");
	bool post = method && str_eq_nocase(method, "post");
	Tab *dest = new_tab_wanted ? new_tab(NULL, true) : t;
	if (post) {
		navigate(dest, target, sb_str(&data), "application/x-www-form-urlencoded");
	} else {
		char *q = (char *)str_chr(target, '?');
		if (q)
			*q = 0;
		char *hash = (char *)str_chr(target, '#');
		if (hash)
			*hash = 0;
		char *full = str_fmt("%s?%s", target, sb_str(&data));
		navigate(dest, full, NULL, NULL);
		mem_free(full);
	}
	mem_free(target);
	sb_free(&data);
}

static void pick_radio(Node *radio)
{
	const char *name = node_attr(radio, "name");
	Node *scope = form_of(radio);
	if (!scope)
		for (scope = radio; scope->parent; scope = scope->parent)
			;
	/* uncheck the others in the group */
	Node *stack[512];
	int sp = 0;
	stack[sp++] = scope;
	while (sp) {
		Node *n = stack[--sp];
		for (Node *c = n->first; c; c = c->next) {
			if (c->type != NODE_ELEMENT)
				continue;
			if (str_eq(c->tag, "input") && node_attr(c, "type") && str_eq_nocase(node_attr(c, "type"), "radio")) {
				const char *nm = node_attr(c, "name");
				if (name && nm && str_eq(nm, name))
					page_control(c)->checked = false;
			}
			if (sp < 512)
				stack[sp++] = c;
		}
	}
	page_control(radio)->checked = true;
}

/* ---- Following links ------------------------------------------------------------------------------------ */

static char *link_url(Tab *t, const Node *a)
{
	const char *href = node_attr(a, "href");
	if (!href || !t->doc)
		return NULL;
	if (str_starts(href, "javascript:"))
		return NULL;
	return url_resolve(t->doc->base ? t->doc->base : t->doc->url, href);
}

static void follow(Ui *ui, Tab *t, Node *link, bool background)
{
	if (str_eq(link->tag, "button")) {
		const char *type = node_attr(link, "type");
		if (!type || str_eq_nocase(type, "submit"))
			submit_form(t, link, link, background);
		return;
	}
	char *url = link_url(t, link);
	if (!url)
		return;
	if (str_starts(url, "mailto:")) {
		ui_launch(ui, "mail", url);
	} else {
		const char *target = node_attr(link, "target");
		if (background) {
			new_tab(url, false);
		} else if (target && str_eq(target, "_blank")) {
			new_tab(url, true);
		} else {
			navigate(t, url, NULL, NULL);
		}
	}
	mem_free(url);
}

/* ---- Drawing --------------------------------------------------------------------------------------------- */

static Color site_color(const char *url)
{
	Url u;
	const char *key = url_parse(url, &u) ? u.host : url;
	u64 h = hash_bytes(key, strlen(key));
	return color_hsl((f32)(h % 360), 0.55f, 0.48f, 255);
}

static void site_letter(const char *url, const char *title, char *out, usize cap)
{
	Url u;
	const char *s = title && *title ? title : (url_parse(url, &u) ? (str_starts(u.host, "www.") ? u.host + 4 : u.host) : url);
	u32 cp;
	usize k = utf8_decode(s, strlen(s), &cp);
	if (cp < 128)
		fmt(out, cap, "%c", to_upper((int)cp));
	else
		str_copy(out, s, MIN(cap, k + 1));
}

static const struct {
	const char *title, *url;
} FAVORITES[] = {
	{ "Wikipedia", "https://en.wikipedia.org/wiki/Main_Page" },
	{ "DuckDuckGo", "https://html.duckduckgo.com/html/" },
	{ "Hacker News", "https://news.ycombinator.com/" },
	{ "CNN Lite", "https://lite.cnn.com/" },
	{ "NPR Text", "https://text.npr.org/" },
	{ "Lobsters", "https://lobste.rs/" },
	{ "Gutenberg", "https://www.gutenberg.org/" },
	{ "Wiktionary", "https://en.wiktionary.org/wiki/Wiktionary:Main_Page" },
};

static bool tile(Ui *ui, Rect r, const char *url, const char *title, int idx)
{
	Theme *th = &ui->theme;
	ui_push_id(ui, idx);
	UiId id = ui_id(ui, "tile");
	UiInteract in = ui_interact(ui, id, r);
	f32 hv = ui_fade(ui, id, in.hovered, 0.15f);
	if (hv > 0)
		ui_round(ui, r, 12, color_scale_alpha(th->hover, hv));
	Rect icon = rect(r.x + (r.w - 56) / 2, r.y + 12, 56, 56);
	Color c = site_color(url);
	ui_shadow(ui, icon, 14, 8, 2, 0x28000000);
	ui_gradient(ui, icon, 14, color_lighten(c, 0.12f), color_lighten(c, -0.08f));
	ui_line(ui, icon.x + 12, icon.y + 1.5f, icon.x + icon.w - 12, icon.y + 1.5f, 1, 0x40ffffff);
	char letter[8];
	site_letter(url, title, letter, sizeof letter);
	ui_text_in(ui, ui_font(ui, 24, 700), icon, letter, COLOR_WHITE, ALIGN_CENTER);
	ui_text_in(ui, ui_font(ui, 12, 500), rect(r.x + 4, icon.y + icon.h + 8, r.w - 8, 18), title, th->text, ALIGN_CENTER);
	if (in.hovered)
		ui_set_cursor(ui, CURSOR_POINTER);
	ui_pop_id(ui);
	return in.clicked;
}

static void draw_home(Ui *ui, Tab *t, Rect area)
{
	Theme *th = &ui->theme;
	ui_fill(ui, area, th->view);
	/* a soft wash of color at the top */
	ui_gradient(ui, rect(area.x, area.y, area.w, 220), 0, color_with_alpha(th->accent, th->dark ? 0x30 : 0x1c), color_with_alpha(th->accent, 0));
	f32 colw = minf(760, area.w - 48);
	Rect col = rect(area.x + (area.w - colw) / 2, area.y, colw, area.h);
	f32 y = area.y + maxf(36, minf(110, area.h * 0.12f));
	ui_app_icon(ui, rect(col.x + (col.w - 64) / 2, y, 64, 64), "drift");
	y += 76;
	ui_text_in(ui, ui_font(ui, 26, 700), rect(col.x, y, col.w, 32), "Where to?", th->text, ALIGN_CENTER);
	y += 48;
	Rect sr = rect(col.x + (col.w - minf(col.w, 600)) / 2, y, minf(col.w, 600), 46);
	ui_shadow(ui, sr, 23, 14, 3, 0x22000000);
	int ev = ui_entry(ui, sr, "homeq", &B.home_q, "Search the web or type an address", ENTRY_SEARCH | (t->hist.len <= 1 ? ENTRY_AUTOFOCUS : 0));
	if (ev & ENTRY_SUBMIT) {
		char *u = typed_to_url(sb_str(&B.home_q));
		if (u) {
			navigate(t, u, NULL, NULL);
			mem_free(u);
			sb_clear(&B.home_q);
		}
	}
	y += 46 + 40;
	/* Bookmarks first, then a few good starting points. */
	struct { const char *url, *title; } list[24];
	int n = 0;
	for (int i = (int)B.marks.len - 1; i >= 0 && n < 12; i--)
		list[n].url = B.marks.data[i].url, list[n].title = B.marks.data[i].title, n++;
	for (usize i = 0; i < ARRAY_LEN(FAVORITES) && n < 16; i++) {
		bool dup = false;
		for (int k = 0; k < n; k++)
			dup |= str_eq(list[k].url, FAVORITES[i].url);
		if (!dup)
			list[n].url = FAVORITES[i].url, list[n].title = FAVORITES[i].title, n++;
	}
	ui_text(ui, ui_font(ui, 12, 700), col.x + 6, y, B.marks.len ? "BOOKMARKS & FAVORITES" : "FAVORITES", th->text_dim);
	y += 12;
	int cols = MAX(2, MIN(6, (int)(col.w / 120)));
	f32 tw = col.w / (f32)cols;
	for (int i = 0; i < n; i++) {
		Rect r = rect(col.x + tw * (f32)(i % cols), y + 108 * (f32)(i / cols), tw, 104);
		if (r.y + r.h > area.y + area.h)
			break;
		if (tile(ui, pad(r, 4), list[i].url, list[i].title, i))
			navigate(t, list[i].url, NULL, NULL);
	}
	y += 108 * (f32)((n + cols - 1) / cols) + 20;
	/* Recently visited */
	if (B.visited.len && y + 80 < area.y + area.h) {
		ui_text(ui, ui_font(ui, 12, 700), col.x + 6, y, "RECENTLY VISITED", th->text_dim);
		y += 10;
		for (int i = (int)B.visited.len - 1, k = 0; i >= 0 && k < 6 && y + 34 < area.y + area.h; i--, k++) {
			Mark *m = &B.visited.data[i];
			Rect r = rect(col.x, y, col.w, 34);
			ui_push_id(ui, 1000 + i);
			UiInteract in = ui_row(ui, r, ui_id(ui, "recent"), false);
			ui_pop_id(ui);
			Color c = site_color(m->url);
			ui_round(ui, rect(r.x + 8, r.y + 7, 20, 20), 6, c);
			char letter[8];
			site_letter(m->url, m->title, letter, sizeof letter);
			ui_text_in(ui, ui_font(ui, 11, 700), rect(r.x + 8, r.y + 7, 20, 20), letter, COLOR_WHITE, ALIGN_CENTER);
			f32 tw2 = minf(ui_text_width(ui, ui_font(ui, 13, 500), m->title[0] ? m->title : m->url), r.w * 0.55f);
			ui_text_in(ui, ui_font(ui, 13, 500), rect(r.x + 38, r.y, tw2, r.h), m->title[0] ? m->title : m->url, th->text, ALIGN_START);
			ui_text_in(ui, ui_font(ui, 12, 400), rect(r.x + 48 + tw2, r.y, r.w - tw2 - 56, r.h), m->url, th->text_faint, ALIGN_START);
			if (in.clicked) {
				char *u = str_dup(m->url);
				navigate(t, u, NULL, NULL);
				mem_free(u);
				break;
			}
			y += 34;
		}
	}
}

static void draw_message(Ui *ui, Tab *t, Rect area, const char *icon, const char *title, const char *body, const char *button)
{
	Theme *th = &ui->theme;
	ui_fill(ui, area, th->view);
	f32 w = minf(520, area.w - 48);
	Rect c = rect(area.x + (area.w - w) / 2, area.y + maxf(40, area.h * 0.2f), w, 300);
	ui_icon(ui, rect(c.x + (c.w - 64) / 2, c.y, 64, 64), icon, th->text_faint);
	ui_text_in(ui, ui_font(ui, 22, 700), rect(c.x, c.y + 80, c.w, 30), title, th->text, ALIGN_CENTER);
	f32 h = ui_text_wrapped(ui, ui_font(ui, 14, 400), rect(c.x, c.y + 120, c.w, 120), body, th->text_dim, ALIGN_CENTER);
	if (button && ui_button_suggested(ui, rect(c.x + (c.w - 140) / 2, c.y + 136 + h, 140, 36), button)) {
		if (t->doc && t->doc->state == DOC_DOWNLOADED) {
			char dir[512];
			str_copy(dir, t->doc->error, sizeof dir);
			char *slash = (char *)str_rchr(dir, '/');
			if (slash)
				*slash = 0;
			ui_launch(ui, "vault", dir);
		} else {
			reload(t);
		}
	}
}

static bool header_icon(Ui *ui, Rect r, const char *icon, const char *tip, bool enabled)
{
	if (!enabled) {
		ui_icon(ui, center_in(r, 16, 16), icon, ui->theme.text_faint);
		return false;
	}
	return ui_icon_button(ui, r, icon, tip);
}

static void draw_tabs(Ui *ui, Rect bar)
{
	Theme *th = &ui->theme;
	ui_fill(ui, bar, color_mix(th->window, th->text, th->dark ? 0.04f : 0.035f));
	ui_fill(ui, rect(bar.x, bar.y + bar.h - 1, bar.w, 1), th->border);
	Rect add = cut_right(&bar, 36);
	if (ui_icon_button(ui, center_in(add, 28, 28), "tab-new", "New Tab"))
		new_tab(HOME_URL, true);
	int n = (int)B.tabs.len;
	f32 tw = minf(240, bar.w / (f32)n);
	for (int i = 0; i < n; i++) {
		Tab *t = B.tabs.data[i];
		Rect r = rect(bar.x + tw * (f32)i, bar.y + 4, tw, bar.h - 4);
		bool active = i == B.cur;
		ui_push_id(ui, i);
		UiId id = ui_id(ui, "tab");
		UiInteract in = ui_interact(ui, id, r);
		if (active) {
			ui_round4(ui, r, 8, 8, 0, 0, th->view);
			ui_fill(ui, rect(r.x, r.y + r.h - 1, r.w, 1), th->view);
		} else if (in.hovered) {
			ui_round4(ui, pad4(r, 0, 2, 2, 2), 8, 8, 0, 0, th->hover);
		}
		if (!active && i + 1 != B.cur && i + 1 < n)
			ui_vseparator(ui, r.x + r.w - 0.5f, r.y + 8, r.y + r.h - 8);
		Rect inner = pad2(r, 10, 0);
		bool loading = t->doc && t->doc->state == DOC_LOADING;
		Rect ic = cut_left(&inner, 16);
		if (loading)
			ui_spinner(ui, center_in(ic, 14, 14));
		else if (t->doc)
			ui_icon(ui, center_in(ic, 16, 16), t->doc->url && str_starts(t->doc->url, "https:") ? "globe" : "globe", th->text_dim);
		else
			ui_icon(ui, center_in(ic, 16, 16), "go-home", th->text_dim);
		cut_left(&inner, 8);
		Rect close = cut_right(&inner, 22);
		ui_text_in(ui, ui_font(ui, 13, active ? 600 : 500), inner, tab_title(t), active ? th->text : th->text_dim, ALIGN_START);
		bool closed = false;
		if (in.hovered || active) {
			if (ui_icon_button(ui, center_in(close, 22, 22), "window-close", "Close Tab")) {
				close_tab(ui, i);
				closed = true;
			}
		}
		if (!closed && in.clicked)
			B.cur = i;
		if (!closed && in.hovered && ui->released[BUTTON_MIDDLE]) {
			close_tab(ui, i);
			closed = true;
		}
		ui_pop_id(ui);
		if (closed)
			break;
	}
}

static void draw_suggestions(Ui *ui, Tab *t, Rect entry)
{
	Theme *th = &ui->theme;
	const char *q = sb_str(&t->addr);
	if (!*q)
		return;
	Mark *found[7];
	int n = 0;
	for (int pass = 0; pass < 2; pass++) {
		MarkList *list = pass == 0 ? &B.marks : &B.visited;
		for (int i = (int)list->len - 1; i >= 0 && n < 6; i--) {
			Mark *m = &list->data[i];
			if (!str_find_nocase(m->url, q) && !str_find_nocase(m->title, q))
				continue;
			bool dup = false;
			for (int k = 0; k < n; k++)
				dup |= str_eq(found[k]->url, m->url);
			if (!dup)
				found[n++] = m;
		}
	}
	f32 h = 40 * (f32)(n + 1) + 12;
	Rect pop = rect(entry.x, entry.y + entry.h + 6, entry.w, h);
	ui_shadow(ui, pop, 10, 18, 6, 0x40000000);
	ui_round(ui, pop, 10, th->card);
	ui_border(ui, pop, 10, 1, th->border);
	Rect row = rect(pop.x + 6, pop.y + 6, pop.w - 12, 40);
	ui_push_id(ui, 77);
	/* first row: search for what was typed */
	{
		UiInteract in = ui_row(ui, row, ui_id(ui, "sq"), B.suggest_sel == 0);
		ui_icon(ui, rect(row.x + 10, row.y + 12, 16, 16), "search", th->text_dim);
		char label[300];
		fmt(label, sizeof label, "%s — search with DuckDuckGo", q);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(row.x + 36, row.y, row.w - 44, row.h), label, th->text, ALIGN_START);
		if (in.pressed_now) {
			char *e = url_encode(q);
			char *u = str_fmt(SEARCH_URL "%s", e);
			navigate(t, u, NULL, NULL);
			mem_free(u);
			mem_free(e);
			ui->focus = 0;
			ui_pop_id(ui);
			return;
		}
		row.y += 40;
	}
	for (int i = 0; i < n; i++) {
		ui_push_id(ui, i);
		UiInteract in = ui_row(ui, row, ui_id(ui, "s"), B.suggest_sel == i + 1);
		ui_pop_id(ui);
		Mark *m = found[i];
		ui_icon(ui, rect(row.x + 10, row.y + 12, 16, 16), mark_index(m->url) >= 0 ? "starred" : "history", th->text_dim);
		Font f = ui_font(ui, 13, 500);
		f32 tw = minf(ui_text_width(ui, f, m->title[0] ? m->title : m->url), row.w * 0.5f);
		ui_text_in(ui, f, rect(row.x + 36, row.y, tw, row.h), m->title[0] ? m->title : m->url, th->text, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 12, 400), rect(row.x + 46 + tw, row.y, row.w - tw - 54, row.h), m->url, th->text_faint, ALIGN_START);
		if (in.pressed_now) {
			char *u = str_dup(m->url);
			navigate(t, u, NULL, NULL);
			mem_free(u);
			ui->focus = 0;
			break;
		}
		row.y += 40;
	}
	ui_pop_id(ui);
}

static void pretty_url(const char *url, char *out, usize cap)
{
	const char *s = url;
	if (str_starts(s, "https://"))
		s += 8;
	else if (str_starts(s, "http://"))
		s += 7;
	if (str_starts(s, "www."))
		s += 4;
	str_copy(out, s, cap);
	usize n = strlen(out);
	if (n && out[n - 1] == '/')
		out[n - 1] = 0;
}

static void schedule_layout(Ui *ui, Tab *t)
{
	Doc *d = t->doc;
	if (!d || d->state != DOC_READY || !d->root)
		return;
	bool width = fabsf(t->want_w - t->laid_w) > 0.5f;
	bool images = d->images_done != t->laid_images;
	if (!width && !images)
		return;
	if (atomic_load(&d->layout_busy)) {
		ui->wake_at = ui->now + 0.1;
		return;
	}
	f64 wait = width ? 0.12 : 0.45;
	f64 since = width ? ui->now - t->resized_at : ui->now - t->laid_at;
	if (since < wait) {
		f64 at = ui->now + (wait - since) + 0.01;
		ui->wake_at = ui->wake_at ? minf((f32)ui->wake_at, (f32)at) : at;
		return;
	}
	Job *j = NEW(Job);
	j->kind = JOB_LAYOUT;
	j->doc = d;
	j->width = t->want_w;
	j->vh = B.view_h;
	j->restyle = width;
	atomic_store(&d->layout_busy, 1);
	t->laid_w = t->want_w;
	t->laid_images = d->images_done;
	t->laid_at = ui->now;
	enqueue(j, true);
}

static void take_page(Tab *t)
{
	Doc *d = t->doc;
	if (!d)
		return;
	mutex_lock(&d->lock);
	Page *np = d->new_page;
	d->new_page = NULL;
	mutex_unlock(&d->lock);
	if (!np)
		return;
	bool first = !d->page;
	page_free(d->page);
	d->page = np;
	if (first) {
		if (t->hist.len) {
			Hist *h = &t->hist.data[t->pos];
			mem_free(h->title);
			h->title = str_dup(d->title ? d->title : "");
		}
		remember_visit(d->url, d->title);
	}
	if (t->anchor) {
		f32 y = page_anchor(np, t->anchor);
		if (y >= 0) {
			t->scroll = t->target = y;
			if (first || np->height > y + 200) {
				mem_free(t->anchor);
				t->anchor = NULL;
			}
		}
	} else if (t->restore_scroll && t->hist.len) {
		f32 want = t->hist.data[t->pos].scroll;
		t->scroll = t->target = minf(want, maxf(0, np->height - B.view_h));
		if (np->height >= want + B.view_h || d->images_done >= d->nimages)
			t->restore_scroll = false;
	}
}

static void find_step(int delta)
{
	if (!B.find_matches)
		return;
	B.find_cur = (B.find_cur + delta + B.find_matches) % B.find_matches;
}

static void toggle_bookmark(Tab *t)
{
	if (!t->doc || !t->doc->url)
		return;
	int i = mark_index(t->doc->url);
	if (i >= 0) {
		mem_free(B.marks.data[i].url);
		mem_free(B.marks.data[i].title);
		vec_remove(&B.marks, (usize)i);
	} else {
		vec_push(&B.marks, (Mark){ str_dup(t->doc->url), str_dup(tab_title(t)) });
	}
	save_marks("bookmarks.tsv", &B.marks);
}

static void set_zoom(f32 z)
{
	B.zoom = clampf(roundf(z * 10) / 10, 0.5f, 2.5f);
	conf_set(B.conf, "zoom", NULL);
	char v[16];
	fmt(v, sizeof v, "%.1f", (double)B.zoom);
	conf_set(B.conf, "zoom", v);
	conf_save(B.conf);
}

static void menu(Ui *ui, Rect anchor, Tab *t)
{
	Rect c;
	if (!ui_popover_begin(ui, "menu", anchor, 250, 248, &c))
		return;
	Theme *th = &ui->theme;
	if (ui_menu_item(ui, cut_top(&c, 34), "tab-new", "New Tab", "Ctrl+T")) {
		new_tab(HOME_URL, true);
		ui_popover_close(ui);
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "search", "Find in Page", "Ctrl+F")) {
		B.find_open = true;
		ui_focus(ui, "find");
		ui_popover_close(ui);
	}
	bool marked = t->doc && t->doc->url && mark_index(t->doc->url) >= 0;
	if (ui_menu_item(ui, cut_top(&c, 34), marked ? "starred" : "star", marked ? "Remove Bookmark" : "Bookmark This Page", "Ctrl+D")) {
		toggle_bookmark(t);
		ui_popover_close(ui);
	}
	cut_top(&c, 4);
	ui_separator(ui, c.x, c.y, c.x + c.w);
	cut_top(&c, 6);
	/* Zoom row */
	Rect zr = cut_top(&c, 34);
	ui_text_in(ui, ui_font(ui, 13, 500), rect(zr.x + 12, zr.y, 80, zr.h), "Zoom", th->text, ALIGN_START);
	Rect zb = cut_right(&zr, 120);
	if (ui_icon_button(ui, cut_left(&zb, 34), "zoom-out", "Zoom Out"))
		set_zoom(B.zoom - 0.1f);
	char pct[16];
	fmt(pct, sizeof pct, "%d%%", (int)roundf(B.zoom * 100));
	Rect pr = cut_left(&zb, 52);
	UiInteract pi = ui_interact(ui, ui_id(ui, "zoomreset"), pr);
	ui_text_in(ui, ui_font(ui, 13, 600), pr, pct, pi.hovered ? th->accent : th->text, ALIGN_CENTER);
	if (pi.clicked)
		set_zoom(1);
	if (ui_icon_button(ui, cut_left(&zb, 34), "zoom-in", "Zoom In"))
		set_zoom(B.zoom + 0.1f);
	cut_top(&c, 4);
	ui_separator(ui, c.x, c.y, c.x + c.w);
	cut_top(&c, 6);
	if (ui_menu_item(ui, cut_top(&c, 34), "download", "Downloads", NULL)) {
		char *dir = path_join(home_dir(), "Downloads");
		ui_launch(ui, "vault", dir);
		mem_free(dir);
		ui_popover_close(ui);
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "trash", "Clear History & Cookies", NULL)) {
		vec_each(&B.visited, m)
		{
			mem_free(m->url);
			mem_free(m->title);
		}
		vec_clear(&B.visited);
		save_marks("history.tsv", &B.visited);
		mutex_lock(&jar.lock);
		vec_each(&jar.list, ck)
		{
			mem_free(ck->name), mem_free(ck->value), mem_free(ck->domain), mem_free(ck->path);
		}
		vec_clear(&jar.list);
		mutex_unlock(&jar.lock);
		ui_notify(ui, "Browsing data cleared", "History and cookies are gone.", "drift");
		ui_popover_close(ui);
	}
	ui_popover_end(ui);
}

static void keys(Ui *ui, Tab *t, Rect view)
{
	bool typing = ui->focus != 0;
	if (ui_key_pressed(ui, KEY_L, MOD_CTRL) || ui_key_pressed(ui, KEY_F6, 0)) {
		ui_focus(ui, "addr");
		t->addr_dirty = false;
	}
	if (ui_key_pressed(ui, KEY_T, MOD_CTRL)) {
		new_tab(HOME_URL, true);
		ui_focus(ui, "homeq");
	}
	if (ui_key_pressed(ui, KEY_W, MOD_CTRL)) {
		close_tab(ui, B.cur);
		return;
	}
	if (ui_key_pressed(ui, KEY_TAB, MOD_CTRL))
		B.cur = (B.cur + 1) % (int)B.tabs.len;
	if (ui_key_pressed(ui, KEY_TAB, MOD_CTRL | MOD_SHIFT))
		B.cur = (B.cur + (int)B.tabs.len - 1) % (int)B.tabs.len;
	for (int k = 0; k < 9; k++)
		if (ui_key_pressed(ui, (u32)(KEY_1 + k), MOD_ALT) && k < (int)B.tabs.len)
			B.cur = k;
	t = cur_tab();
	if (ui_key_pressed(ui, KEY_R, MOD_CTRL) || ui_key_pressed(ui, KEY_F5, 0) || ui_key_pressed(ui, KEY_REFRESH, 0))
		reload(t);
	if (ui_key_pressed(ui, KEY_LEFT, MOD_ALT) || ui_key_pressed(ui, KEY_BACK, 0) || ui->released[BUTTON_BACK])
		go_history(t, -1);
	if (ui_key_pressed(ui, KEY_RIGHT, MOD_ALT) || ui_key_pressed(ui, KEY_FORWARD, 0) || ui->released[BUTTON_FORWARD])
		go_history(t, 1);
	if (ui_key_pressed(ui, KEY_F, MOD_CTRL)) {
		B.find_open = true;
		ui_focus(ui, "find");
	}
	if (ui_key_pressed(ui, KEY_D, MOD_CTRL))
		toggle_bookmark(t);
	if (ui_key_pressed(ui, KEY_EQUAL, MOD_CTRL) || ui_key_pressed(ui, KEY_EQUAL, MOD_CTRL | MOD_SHIFT) || ui_key_pressed(ui, KEY_KPPLUS, MOD_CTRL))
		set_zoom(B.zoom + 0.1f);
	if (ui_key_pressed(ui, KEY_MINUS, MOD_CTRL) || ui_key_pressed(ui, KEY_KPMINUS, MOD_CTRL))
		set_zoom(B.zoom - 0.1f);
	if (ui_key_pressed(ui, KEY_0, MOD_CTRL))
		set_zoom(1);
	if (ui_key_pressed(ui, KEY_F3, 0))
		find_step(1);
	if (ui_key_pressed(ui, KEY_HOMEPAGE, 0))
		navigate(t, HOME_URL, NULL, NULL);
	if (typing || !t->doc || !t->doc->page)
		return;
	f32 page = view.h / B.zoom - 48;
	f32 maxs = maxf(0, t->doc->page->height - view.h / B.zoom);
	if (ui_key_pressed(ui, KEY_DOWN, 0))
		t->target += 60;
	if (ui_key_pressed(ui, KEY_UP, 0))
		t->target -= 60;
	if (ui_key_pressed(ui, KEY_PAGEDOWN, 0) || ui_key_pressed(ui, KEY_SPACE, 0))
		t->target += page;
	if (ui_key_pressed(ui, KEY_PAGEUP, 0) || ui_key_pressed(ui, KEY_SPACE, MOD_SHIFT))
		t->target -= page;
	if (ui_key_pressed(ui, KEY_HOME, 0) || ui_key_pressed(ui, KEY_HOME, MOD_CTRL))
		t->target = 0;
	if (ui_key_pressed(ui, KEY_END, 0) || ui_key_pressed(ui, KEY_END, MOD_CTRL))
		t->target = maxs;
	if (ui_key_pressed(ui, KEY_BACKSPACE, 0))
		go_history(t, -1);
	t->target = clampf(t->target, 0, maxs);
}

static void draw_page(Ui *ui, Tab *t, Rect view)
{
	Doc *d = t->doc;
	Page *p = d->page;
	f32 z = B.zoom;
	f32 maxs = maxf(0, p->height - view.h / z);
	/* Scrolling: the wheel moves directly, keys glide. */
	if (ui_mouse_in(ui, view) && ui->scroll_dy != 0) {
		t->scroll = clampf(t->scroll + ui->scroll_dy / z, 0, maxs);
		t->target = t->scroll;
		ui->scroll_dy = 0;
		if (t->anchor) {
			mem_free(t->anchor);
			t->anchor = NULL;
		}
		t->restore_scroll = false;
	}
	t->target = clampf(t->target, 0, maxs);
	if (fabsf(t->target - t->scroll) > 0.5f) {
		f32 k = ui->theme.reduce_motion ? 1 : minf(1, ui->dt * 16);
		t->scroll += (t->target - t->scroll) * k;
		ui->redraw = true;
	} else {
		t->scroll = t->target;
	}
	t->scroll = clampf(t->scroll, 0, maxs);
	/* Clicks: registered before the page's own controls, which win. */
	UiId pid = ui_id(ui, "page");
	UiInteract in = ui_interact(ui, pid, view);
	f32 px = (ui->mx - view.x) / z, py = (ui->my - view.y) / z + t->scroll;
	const Item *hit = in.hovered ? page_hit(p, px, py) : NULL;
	Node *link = hit && hit->link ? hit->link : NULL;
	B.hover = link;
	B.hover_url[0] = 0;
	if (link) {
		ui_set_cursor(ui, CURSOR_POINTER);
		if (str_eq(link->tag, "a")) {
			char *u = link_url(t, link);
			if (u)
				str_copy(B.hover_url, u, sizeof B.hover_url);
			mem_free(u);
		}
	}
	PaintOpts o = { .hover_link = link, .zoom = z, .find = B.find_open && B.find.len ? sb_str(&B.find) : NULL, .find_current = B.find_cur };
	paint_page(ui, p, view, t->scroll, &o);
	if (o.find && o.matches != B.find_matches) {
		B.find_matches = o.matches;
		if (B.find_cur >= o.matches)
			B.find_cur = 0;
	}
	if (o.find && o.matches) {
		static int last_cur = -1;
		static const char *last_q;
		if (last_cur != B.find_cur || last_q != o.find) {
			if (o.find_y < t->target || o.find_y > t->target + view.h / z - 60)
				t->target = maxf(0, o.find_y - view.h / z / 3);
			last_cur = B.find_cur;
			last_q = o.find;
		}
	}
	if (o.radio)
		pick_radio(o.radio);
	if (o.submit) {
		submit_form(t, o.submit, str_eq(o.submit->tag, "input") && node_attr(o.submit, "type") && !str_eq_nocase(node_attr(o.submit, "type"), "text") &&
					   !str_eq_nocase(node_attr(o.submit, "type"), "search") ? o.submit : NULL, false);
		return;
	}
	if (link && (in.clicked || (in.hovered && ui->released[BUTTON_MIDDLE]))) {
		bool bg = ui->released[BUTTON_MIDDLE] || (ui->mods & MOD_CTRL);
		follow(ui, t, link, bg);
		return;
	}
	if (in.pressed_now)
		ui->focus = 0;
	/* Overlay scroll bar. */
	if (p->height > view.h / z + 1) {
		Rect zone = rect(view.x + view.w - 14, view.y, 14, view.h);
		UiId sid = ui_id(ui, "vscroll");
		UiInteract si = ui_interact(ui, sid, zone);
		f32 frac = (view.h / z) / p->height;
		f32 bh = maxf(36, view.h * frac);
		f32 by = view.y + (view.h - bh) * (maxs > 0 ? t->scroll / maxs : 0);
		static f32 grab = -1;
		if (si.pressed_now) {
			grab = (ui->my >= by && ui->my <= by + bh) ? ui->my - by : bh / 2;
		}
		if (si.held && grab >= 0) {
			f32 k = (ui->my - grab - view.y) / maxf(1, view.h - bh);
			t->scroll = t->target = clampf(k, 0, 1) * maxs;
		}
		if (!si.held)
			grab = -1;
		bool near = si.hovered || si.held;
		f32 w = near ? 8 : 5;
		ui_round(ui, rect(view.x + view.w - w - 3, by + 2, w, bh - 4), w / 2, near ? 0x88000000 : 0x55000000);
	}
}

static void frame(Ui *ui)
{
	Theme *th = &ui->theme;
	Tab *t = cur_tab();
	take_page(t);
	ui_window_background(ui, th->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 48);
	Rect hb = ui_headerbar(ui, header, NULL, HEADER_NO_TITLE);
	hb = pad2(hb, 4, 9);
	/* Navigation */
	bool can_back = t->pos > 0, can_fwd = t->pos + 1 < (int)t->hist.len;
	if (header_icon(ui, cut_left(&hb, 32), "go-back", "Back (Alt+Left)", can_back))
		go_history(t, -1);
	if (header_icon(ui, cut_left(&hb, 32), "go-forward", "Forward (Alt+Right)", can_fwd))
		go_history(t, 1);
	bool loading = t->doc && t->doc->state == DOC_LOADING;
	if (loading) {
		if (ui_icon_button(ui, cut_left(&hb, 32), "window-close", "Stop")) {
			atomic_store(&t->doc->cancelled, 1);
			fail(t->doc, "Loading was stopped.");
		}
	} else if (header_icon(ui, cut_left(&hb, 32), "refresh", "Reload (Ctrl+R)", t->doc != NULL)) {
		reload(t);
	}
	Rect menu_r = cut_right(&hb, 32);
	if (ui_icon_button(ui, menu_r, "menu", "Menu"))
		ui_popover_toggle(ui, "menu");
	if (B.tabs.len == 1 && ui_icon_button(ui, cut_right(&hb, 32), "tab-new", "New Tab (Ctrl+T)"))
		new_tab(HOME_URL, true);
	t = cur_tab();
	/* Address bar */
	cut_left(&hb, 8);
	cut_right(&hb, 8);
	f32 aw = minf(hb.w, 820);
	Rect addr = rect(hb.x + (hb.w - aw) / 2, hb.y, aw, hb.h);
	bool focused = ui_is_focused(ui, "addr");
	if (!focused && !t->addr_dirty) {
		sb_clear(&t->addr);
		const char *u = tab_url(t);
		if (!str_eq(u, HOME_URL)) {
			char pretty[2048];
			pretty_url(u, pretty, sizeof pretty);
			sb_adds(&t->addr, pretty);
		}
	}
	static bool was_focused;
	if (focused && !was_focused) {
		/* show the whole address while editing */
		sb_clear(&t->addr);
		const char *u = tab_url(t);
		if (!str_eq(u, HOME_URL))
			sb_adds(&t->addr, u);
		ui_textarea_select(ui, "addr", 0, t->addr.len);
		B.suggest_sel = 0;
	}
	was_focused = focused;
	Rect entry = addr;
	bool secure = t->doc && t->doc->url && str_starts(t->doc->url, "https://");
	bool has_url = t->doc && t->doc->url;
	/* Our own frame, so the lock and the star sit inside it. */
	ui_round(ui, entry, 8, th->input);
	ui_line(ui, entry.x + 8, entry.y + 1.5f, entry.x + entry.w - 8, entry.y + 1.5f, 1, th->dark ? 0x40000000 : 0x0f000000);
	if (focused) {
		ui_border(ui, rect_inset(entry, -2, -2), 10, 2, color_with_alpha(th->accent, 0x55));
		ui_border(ui, entry, 8, 1, th->accent);
	} else {
		ui_border(ui, entry, 8, 1, th->border_strong);
	}
	Rect field = entry;
	if (!focused) {
		Rect ic = cut_left(&field, 30);
		const char *icon = !has_url ? "search" : secure ? "lock" : "globe";
		ui_icon(ui, rect(ic.x + 11, ic.y + (ic.h - 14) / 2, 14, 14), icon, secure ? th->text_dim : th->text_faint);
		field.x -= 6, field.w += 6;
	}
	if (has_url && !focused) {
		bool marked = mark_index(t->doc->url) >= 0;
		Rect sr = cut_right(&field, 32);
		if (ui_icon_button(ui, center_in(sr, 26, 26), marked ? "starred" : "star", marked ? "Remove Bookmark" : "Bookmark (Ctrl+D)"))
			toggle_bookmark(t);
	}
	int ev = ui_entry(ui, field, "addr", &t->addr, "Search or enter address", ENTRY_URL | ENTRY_FLAT);
	if (ev & ENTRY_CHANGED)
		t->addr_dirty = true, B.suggest_sel = 0;
	if (ev & ENTRY_SUBMIT) {
		char *u = typed_to_url(sb_str(&t->addr));
		if (u) {
			navigate(t, u, NULL, NULL);
			mem_free(u);
		}
		ui->focus = 0;
		t->addr_dirty = false;
	}
	if (ev & ENTRY_ESCAPE) {
		ui->focus = 0;
		t->addr_dirty = false;
	}
	/* Loading progress, along the bottom of the address bar */
	if (loading) {
		f32 pm = (f32)atomic_load(&t->doc->progress) / 1000.0f;
		Rect bar = rect(entry.x + 6, entry.y + entry.h - 3, (entry.w - 12) * pm, 2);
		ui_round(ui, bar, 1, th->accent);
		ui->wake_at = ui->now + 0.05;
	}
	/* Tabs */
	if (B.tabs.len > 1)
		draw_tabs(ui, cut_top(&win, 38));
	else
		ui_fill(ui, rect(win.x, win.y - 1, win.w, 1), th->border);
	t = cur_tab();
	/* Find bar */
	Rect content = win;
	if (B.find_open) {
		Rect fb = cut_bottom(&content, 46);
		ui_fill(ui, fb, th->window);
		ui_fill(ui, rect(fb.x, fb.y, fb.w, 1), th->border);
		Rect r = pad2(fb, 12, 7);
		if (ui_icon_button(ui, cut_right(&r, 32), "window-close", "Close"))
			B.find_open = false;
		cut_right(&r, 6);
		if (ui_icon_button(ui, cut_right(&r, 32), "go-down", "Next (Enter)"))
			find_step(1);
		if (ui_icon_button(ui, cut_right(&r, 32), "go-up", "Previous"))
			find_step(-1);
		char cnt[48] = "";
		if (B.find.len)
			fmt(cnt, sizeof cnt, B.find_matches ? "%d of %d" : "No matches", B.find_matches ? B.find_cur + 1 : 0, B.find_matches);
		ui_text_in(ui, ui_font(ui, 12, 500), cut_right(&r, 90), cnt, th->text_dim, ALIGN_END);
		cut_right(&r, 8);
		Rect fe = cut_left(&r, minf(320, r.w));
		int fev = ui_entry(ui, fe, "find", &B.find, "Find in page", ENTRY_SEARCH);
		if (fev & ENTRY_CHANGED)
			B.find_cur = 0;
		if (fev & ENTRY_SUBMIT)
			find_step((ui->mods & MOD_SHIFT) ? -1 : 1);
		if (fev & ENTRY_ESCAPE) {
			B.find_open = false;
			ui->focus = 0;
		}
	}
	B.view_h = content.h;
	f32 want = floorf(content.w / B.zoom);
	if (fabsf(want - t->want_w) > 0.5f) {
		t->want_w = want;
		t->resized_at = ui->now;
	}
	keys(ui, t, content);
	if (ui->quit)
		return;
	t = cur_tab();
	if (!t->doc) {
		draw_home(ui, t, content);
	} else if (t->doc->state == DOC_FAILED && !t->doc->page) {
		draw_message(ui, t, content, "warning", "Can't open this page", t->doc->error, "Try Again");
	} else if (t->doc->state == DOC_DOWNLOADED) {
		char body[600];
		const char *name = str_rchr(t->doc->error, '/');
		fmt(body, sizeof body, "%s was saved to your Downloads folder.", name ? name + 1 : t->doc->error);
		draw_message(ui, t, content, "download", "Download complete", body, "Show in Files");
	} else if (t->doc->page) {
		schedule_layout(ui, t);
		draw_page(ui, t, content);
		if (t->anchor || t->restore_scroll)
			ui->wake_at = ui->now + 0.2;
	} else {
		ui_fill(ui, content, th->view);
	}
	t = cur_tab();
	/* Hovered link address, bottom left */
	if (B.hover_url[0] && t->doc) {
		char pretty[1024];
		pretty_url(B.hover_url, pretty, sizeof pretty);
		Font f = ui_font(ui, 12, 500);
		f32 w = minf(ui_text_width(ui, f, pretty) + 20, content.w * 0.6f);
		Rect r = rect(content.x + 8, content.y + content.h - 32, w, 26);
		ui_shadow(ui, r, 8, 10, 2, 0x30000000);
		ui_round(ui, r, 8, th->card);
		ui_border(ui, r, 8, 1, th->border);
		ui_text_in(ui, f, pad2(r, 10, 0), pretty, th->text_dim, ALIGN_START);
	}
	if (ui_is_focused(ui, "addr") && t->addr_dirty)
		draw_suggestions(ui, t, entry);
	menu(ui, menu_r, t);
	/* title */
	char title[300];
	fmt(title, sizeof title, "%s", t->doc ? tab_title(t) : "Drift");
	if (!str_eq(title, ui->title))
		ui_set_title(ui, title);
}

static void init(Ui *ui)
{
	the_ui = ui;
	for (int i = 0; i < WORKERS; i++)
		thread_spawn(worker, NULL);
	B.dir = path_join(home_dir(), ".local/share/zurbax/drift");
	dir_make(B.dir, 0755);
	char *conf = path_join(B.dir, "drift.conf");
	B.conf = conf_load(conf);
	mem_free(conf);
	B.zoom = clampf((f32)conf_get_float(B.conf, "zoom", 1), 0.5f, 2.5f);
	load_marks("bookmarks.tsv", &B.marks);
	load_marks("history.tsv", &B.visited);
	const char *arg = start_arg ? start_arg : env_get("DRIFT_URL");
	if (arg && *arg) {
		char *u = arg[0] == '/' ? str_fmt("file://%s", arg) : typed_to_url(arg);
		new_tab(u, true);
		mem_free(u);
	} else {
		new_tab(HOME_URL, true);
	}
}

int main(int argc, char **argv)
{
	if (argc > 1)
		start_arg = argv[1];
	UiAppDesc d = { .id = "drift", .title = "Drift", .width = 1180, .height = 800, .min_width = 480, .min_height = 360,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
