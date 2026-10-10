/* Message bodies: Drift's engine lays out HTML mail, and plain text mail made
 * into a little HTML page. Pictures sent along with the message (cid:) show
 * right away; pictures from the web wait until the reader asks for them,
 * since loading them tells the sender the mail was opened. */
#include "../drift/html.c"
#include "../drift/css.c"
#include "../drift/layout.c"
#include "../drift/paint.c"
#include "mail.h"

/* ---- Helper threads: layout and web pictures ------------------------------------------------ */

enum { VJ_LAYOUT, VJ_IMAGE };

typedef struct VJob VJob;
struct VJob {
	int kind;
	Msg *msg;
	Image *img;
	f32 width;
	VJob *next;
};

static struct {
	Mutex lock;
	VJob *head, *tail;
	int wake;
	Ui *ui;
	bool started;
} V;

static void v_push(VJob *j)
{
	mutex_lock(&V.lock);
	if (V.tail)
		V.tail->next = j;
	else
		V.head = j;
	V.tail = j;
	mutex_unlock(&V.lock);
	atomic_add(&V.wake, 1);
	futex_wake(&V.wake, 1);
}

static Canvas *v_decode(const u8 *data, usize len, const char *type, int *w, int *h)
{
	Canvas *c = NULL;
	*w = *h = 0;
	bool svg = (type && str_find(type, "svg")) || (len > 5 && (memcmp(data, "<svg", 4) == 0 || memcmp(data, "<?xml", 5) == 0));
	if (svg) {
		SvgDoc *doc = svg_parse((const char *)data, len);
		if (doc) {
			f32 sw, sh;
			svg_size(doc, &sw, &sh);
			if (sw <= 0 || sh <= 0)
				sw = 300, sh = 150;
			*w = (int)sw, *h = (int)sh;
			f32 k = minf(2, 2048 / maxf(sw, sh));
			c = svg_rasterize(doc, MAX(1, (int)(sw * k)), MAX(1, (int)(sh * k)), COLOR_BLACK);
			svg_free(doc);
		}
		return c;
	}
	c = image_decode(data, len);
	if (c) {
		*w = c->width, *h = c->height;
		int mx = MAX(c->width, c->height);
		if (mx > 2400) {
			Canvas *s = canvas_resize(c, c->width * 2400 / mx, c->height * 2400 / mx);
			canvas_free(c);
			c = s;
		}
	}
	return c;
}

static void v_set_image(Doc *d, Image *im, Canvas *c, int w, int h)
{
	mutex_lock(&d->lock);
	im->canvas = c;
	im->w = w, im->h = h;
	im->state = c ? IMG_READY : IMG_FAILED;
	d->images_done++;
	mutex_unlock(&d->lock);
}

static void v_fetch(Msg *m, Image *im)
{
	Canvas *c = NULL;
	int w = 0, h = 0;
	if (!atomic_load(&m->doc.cancelled)) {
		/* no referrer and no cookies: the sender learns as little as possible */
		HttpOptions o = { .user_agent = "Mozilla/5.0 (X11; Zurbax x86_64) Mail/1.0", .max_redirects = 5,
				  .max_body = 16 << 20, .timeout_ms = 20000, .accept = "image/png,image/jpeg,image/gif,image/svg+xml,image/*;q=0.5" };
		HttpResponse *r = http_request(im->url, &o);
		if (r->status == 200 && r->body && r->body_len) {
			char type[96] = "";
			http_header(r, "Content-Type", type, sizeof type);
			c = v_decode(r->body, r->body_len, type, &w, &h);
		}
		http_response_free(r);
	}
	v_set_image(&m->doc, im, c, w, h);
	ui_wake(V.ui);
}

static void v_layout(Msg *m, f32 width)
{
	Doc *d = &m->doc;
	mutex_lock(&d->layout_lock);
	style_tree(d->root, d->sheets, d->nsheets, width, 900);
	Page *p = layout_doc(d, width, 900);
	mutex_unlock(&d->layout_lock);
	mutex_lock(&d->lock);
	page_free(d->new_page);
	d->new_page = p;
	mutex_unlock(&d->lock);
	atomic_store(&d->layout_busy, 0);
	ui_wake(V.ui);
}

static void v_worker(void *arg)
{
	for (;;) {
		int seen = atomic_load(&V.wake);
		mutex_lock(&V.lock);
		VJob *j = V.head;
		if (j) {
			V.head = j->next;
			if (!V.head)
				V.tail = NULL;
		}
		mutex_unlock(&V.lock);
		if (!j) {
			futex_wait(&V.wake, seen, -1);
			continue;
		}
		if (j->kind == VJ_LAYOUT)
			v_layout(j->msg, j->width);
		else
			v_fetch(j->msg, j->img);
		msg_release(j->msg);
		mem_free(j);
	}
}

void view_start(Ui *ui)
{
	V.ui = ui;
	if (V.started)
		return;
	V.started = true;
	for (int i = 0; i < 3; i++)
		thread_spawn(v_worker, NULL);
}

/* ---- Pictures ----------------------------------------------------------------------------------- */

static bool v_remote(const char *url)
{
	return str_starts(url, "http://") || str_starts(url, "https://");
}

static int v_hex2(const char *h)
{
	int v = 0;
	for (int i = 0; i < 2; i++)
		v = v * 16 + (is_digit(h[i]) ? h[i] - '0' : (h[i] | 32) - 'a' + 10);
	return v;
}

static u8 *v_data_url(const char *url, usize *len, char *type, usize tcap)
{
	const char *comma = str_chr(url, ',');
	if (!comma)
		return NULL;
	const char *meta = url + 5;
	usize ml = (usize)(comma - meta);
	usize tl = 0;
	while (tl < ml && meta[tl] != ';' && tl < tcap - 1) {
		type[tl] = meta[tl];
		tl++;
	}
	type[tl] = 0;
	bool b64 = ml >= 7 && str_eq_nocase_n(comma - 7, ";base64", 7);
	const char *s = comma + 1;
	if (b64)
		return base64_decode(s, strlen(s), len);
	/* percent-encoded */
	usize n = strlen(s), w = 0;
	u8 *out = mem_alloc(n + 1);
	for (usize i = 0; i < n; i++) {
		if (s[i] == '%' && i + 2 < n && is_xdigit(s[i + 1]) && is_xdigit(s[i + 2])) {
			out[w++] = (u8)v_hex2(s + i + 1);
			i += 2;
		} else {
			out[w++] = (u8)s[i];
		}
	}
	*len = w;
	return out;
}

/* A picture sent inside the message (cid:) or written into it (data:). */
static Canvas *v_local(Msg *m, const char *url, int *w, int *h)
{
	u8 *data = NULL;
	usize len = 0;
	char type[96] = "";
	if (str_starts(url, "data:")) {
		data = v_data_url(url, &len, type, sizeof type);
	} else {
		char cid[200];
		usize k = 0;
		for (const char *p = url + 4; *p && k < sizeof cid - 1; p++) {
			if (*p == '%' && is_xdigit(p[1]) && is_xdigit(p[2])) {
				cid[k++] = (char)v_hex2(p + 1);
				p += 2;
			} else {
				cid[k++] = *p;
			}
		}
		cid[k] = 0;
		const Part *p = mime_find_cid(m->mime, cid);
		if (p) {
			data = (u8 *)mime_body(p, &len);
			str_copy(type, p->type, sizeof type);
		}
	}
	Canvas *c = data && len ? v_decode(data, len, type, w, h) : NULL;
	mem_free(data);
	return c;
}

Image *doc_image(Doc *d, const char *url)
{
	Msg *m = (Msg *)d;
	mutex_lock(&d->lock);
	Image *im = map_get(&d->image_map, url);
	if (im) {
		mutex_unlock(&d->lock);
		return im;
	}
	im = NEW(Image);
	im->url = str_dup(url);
	map_set(&d->image_map, url, im);
	if (d->nimages % 64 == 0)
		d->images = mem_realloc(d->images, sizeof(Image *) * (usize)(d->nimages + 64));
	d->images[d->nimages++] = im;
	if (str_starts(url, "cid:") || str_starts(url, "data:")) {
		im->state = IMG_LOADING;
		mutex_unlock(&d->lock);
		int w, h;
		Canvas *c = v_local(m, url, &w, &h);
		v_set_image(d, im, c, w, h);
		return im;
	}
	if (v_remote(url)) {
		if (m->allow_remote) {
			im->state = IMG_LOADING;
			mutex_unlock(&d->lock);
			msg_ref(m);
			VJob *j = NEW(VJob);
			j->kind = VJ_IMAGE;
			j->msg = m;
			j->img = im;
			v_push(j);
			return im;
		}
		im->state = IMG_FAILED;
		atomic_add(&m->remote, 1);
	} else if (!str_starts(url, "svg:")) {
		im->state = IMG_FAILED;   /* nothing else can be shown */
	}
	mutex_unlock(&d->lock);
	return im;
}

void msg_allow_remote(Msg *m)
{
	Doc *d = &m->doc;
	mutex_lock(&d->lock);
	m->allow_remote = true;
	atomic_store(&m->remote, 0);
	for (int i = 0; i < d->nimages; i++) {
		Image *im = d->images[i];
		if (!v_remote(im->url) || im->state != IMG_FAILED || im->canvas)
			continue;
		im->state = IMG_LOADING;
		msg_ref(m);
		VJob *j = NEW(VJob);
		j->kind = VJ_IMAGE;
		j->msg = m;
		j->img = im;
		v_push(j);
	}
	mutex_unlock(&d->lock);
}

/* ---- Building the page ------------------------------------------------------------------------------ */

static void css_color(char *out, usize cap, Color c)
{
	fmt(out, cap, "#%02x%02x%02x", (c >> 16) & 255, (c >> 8) & 255, c & 255);
}

static void add_escaped(StrBuf *sb, const char *s, usize n)
{
	for (usize i = 0; i < n; i++) {
		char c = s[i];
		if (c == '&') sb_adds(sb, "&amp;");
		else if (c == '<') sb_adds(sb, "&lt;");
		else if (c == '>') sb_adds(sb, "&gt;");
		else if (c == '"') sb_adds(sb, "&quot;");
		else if (c != '\r') sb_addc(sb, c);
	}
}

/* One line of plain text with its web addresses made into links. */
static void add_line(StrBuf *sb, const char *s, usize n)
{
	usize i = 0, from = 0;
	while (i < n) {
		bool url = (n - i > 8 && (str_eq_nocase_n(s + i, "https://", 8))) || (n - i > 7 && str_eq_nocase_n(s + i, "http://", 7)) ||
			   (n - i > 4 && str_eq_nocase_n(s + i, "www.", 4) && (i == 0 || !is_alnum(s[i - 1])));
		if (!url) {
			i++;
			continue;
		}
		usize e = i;
		while (e < n && !is_space(s[e]) && s[e] != '<' && s[e] != '>' && s[e] != '"')
			e++;
		while (e > i && str_chr(".,;:!?)'", s[e - 1]))
			e--;
		add_escaped(sb, s + from, i - from);
		sb_adds(sb, "<a href=\"");
		if (s[i] == 'w' || s[i] == 'W')
			sb_adds(sb, "https://");
		add_escaped(sb, s + i, e - i);
		sb_adds(sb, "\">");
		add_escaped(sb, s + i, e - i);
		sb_adds(sb, "</a>");
		i = from = e;
	}
	add_escaped(sb, s + from, n - from);
}

static char *text_page(const char *text, const MsgColors *c)
{
	char fg[16], bg[16], link[16], quote[16];
	css_color(fg, sizeof fg, c->text);
	css_color(bg, sizeof bg, c->background);
	css_color(link, sizeof link, c->link);
	css_color(quote, sizeof quote, c->quote);
	StrBuf sb = { 0 };
	sb_fmt(&sb,
	       "<!doctype html><html><head><style>"
	       "body{margin:0;padding:4px 0 8px;font-family:sans-serif;font-size:14.5px;line-height:1.5;color:%s;background:%s}"
	       ".t{white-space:pre-wrap}"
	       "blockquote{margin:10px 0 4px;padding:0 0 0 12px;border-left:3px solid %s;color:%s}"
	       "a{color:%s}"
	       "</style></head><body><div class=\"t\">",
	       fg, bg, quote, quote, link);
	const char *p = text;
	bool in_quote = false, first = true;
	while (*p) {
		const char *eol = str_chr(p, '\n');
		usize n = eol ? (usize)(eol - p) : strlen(p);
		bool q = n && p[0] == '>';
		if (q != in_quote) {
			sb_adds(&sb, q ? "<blockquote>" : "</blockquote>");
			in_quote = q;
			first = true;
		}
		if (!first)
			sb_addc(&sb, '\n');
		first = false;
		if (q) {
			usize skip = (n > 1 && p[1] == ' ') ? 2 : 1;
			add_line(&sb, p + skip, n - skip);
		} else {
			add_line(&sb, p, n);
		}
		p += n;
		if (*p == '\n')
			p++;
	}
	if (in_quote)
		sb_adds(&sb, "</blockquote>");
	sb_adds(&sb, "</div></body></html>");
	return sb_take(&sb);
}

static bool tag_is(const Node *n, const char *const *list)
{
	for (; *list; list++)
		if (str_eq(n->tag, *list))
			return true;
	return false;
}

/* The words of an HTML body with line breaks at blocks, for quoting in replies. */
static void dom_text(const Node *n, StrBuf *out, bool *space)
{
	static const char *const skip[] = { "style", "script", "head", "title", "template", NULL };
	static const char *const block[] = { "p", "div", "tr", "li", "h1", "h2", "h3", "h4", "h5", "h6", "table", "ul", "ol",
					     "blockquote", "section", "article", "header", "footer", "hr", NULL };
	for (const Node *c = n->first; c; c = c->next) {
		if (c->type == NODE_TEXT) {
			for (const char *p = c->text; *p; p++) {
				if (is_space(*p)) {
					*space = true;
					continue;
				}
				if (*space && out->len && out->data[out->len - 1] != '\n')
					sb_addc(out, ' ');
				*space = false;
				sb_addc(out, *p);
			}
			continue;
		}
		if (c->type != NODE_ELEMENT || tag_is(c, skip))
			continue;
		if (str_eq(c->tag, "br")) {
			sb_addc(out, '\n');
			*space = false;
			continue;
		}
		bool b = tag_is(c, block);
		if (b && out->len && out->data[out->len - 1] != '\n')
			sb_addc(out, '\n');
		if (b)
			*space = false;
		dom_text(c, out, space);
		if (b && out->len && out->data[out->len - 1] != '\n')
			sb_addc(out, '\n');
	}
}

static char *tidy_text(const char *s)
{
	/* at most one empty line in a row, no trailing white space */
	StrBuf out = { 0 };
	int newlines = 0;
	for (const char *p = s; *p; p++) {
		if (*p == '\r')
			continue;
		if (*p == '\n') {
			while (out.len && (out.data[out.len - 1] == ' ' || out.data[out.len - 1] == '\t'))
				out.len--;
			if (++newlines <= 2 && out.len)
				sb_addc(&out, '\n');
			continue;
		}
		newlines = 0;
		sb_addc(&out, *p);
	}
	while (out.len && is_space(out.data[out.len - 1]))
		out.len--;
	return sb_take(&out);
}

static const Part *find_plain(const Part *p)
{
	if (str_eq(p->type, "text/plain") && !p->attachment && !p->filename[0])
		return p;
	for (int i = 0; i < p->nkids; i++) {
		if (str_eq(p->kids[i].type, "message/rfc822"))
			continue;
		const Part *f = find_plain(&p->kids[i]);
		if (f)
			return f;
	}
	return NULL;
}

static void find_style_sheets(Node *n, Doc *d)
{
	for (Node *c = n->first; c; c = c->next) {
		if (c->type != NODE_ELEMENT)
			continue;
		if (str_eq(c->tag, "style")) {
			/* <link> sheets stay unloaded: fetching them would give the mail away */
			if (!css_media_matches(node_attr(c, "media"), 800, 900))
				continue;
			StrBuf t = { 0 };
			node_text_content(c, &t);
			Sheet *sh = css_parse(sb_str(&t), t.len, d->url);
			sb_free(&t);
			if (sh) {
				d->sheets = mem_realloc(d->sheets, sizeof(Sheet *) * (usize)(d->nsheets + 1));
				d->sheets[d->nsheets++] = sh;
			}
			continue;
		}
		if (str_eq(c->tag, "svg") || str_eq(c->tag, "template"))
			continue;
		find_style_sheets(c, d);
	}
}

static void header_into(Msg *m, const char *name, char *out, usize cap)
{
	out[0] = 0;
	char *v = mime_header(m->mime->headers, m->mime->headers_len, name);
	if (!v)
		return;
	char *dec = mime_words(str_trim(v));
	str_copy(out, dec, cap);
	mem_free(dec);
	mem_free(v);
	/* decoded words can hide line breaks; they must never reach a reply's headers */
	for (char *p = out; *p; p++)
		if ((u8)*p < 0x20 || *p == 0x7f)
			*p = ' ';
}

Msg *msg_new(const char *raw, usize len, const MsgColors *colors)
{
	Msg *m = NEW(Msg);
	m->doc.refs = 1;
	m->raw = (char *)raw;
	m->len = len;
	m->mime = mime_parse(raw, len);
	char from[256], date[96];
	header_into(m, "Subject", m->subject, sizeof m->subject);
	header_into(m, "From", from, sizeof from);
	mime_address(from, m->from_name, sizeof m->from_name, m->from_addr, sizeof m->from_addr);
	header_into(m, "Reply-To", m->reply_to, sizeof m->reply_to);
	header_into(m, "To", m->to, sizeof m->to);
	header_into(m, "Cc", m->cc, sizeof m->cc);
	header_into(m, "Message-ID", m->message_id, sizeof m->message_id);
	header_into(m, "References", m->references, sizeof m->references);
	header_into(m, "Date", date, sizeof date);
	m->date = mime_date(date);
	mime_attachments(m->mime, m->atts, &m->natts, (int)ARRAY_LEN(m->atts));

	bool html = false;
	const Part *best = mime_best_text(m->mime, &html);
	usize blen = 0;
	char *body = best ? mime_body(best, &blen) : str_dup("");
	char *source;
	if (html) {
		source = body;
		const Part *plain = find_plain(m->mime);
		if (plain) {
			usize pl;
			char *t = mime_body(plain, &pl);
			m->text = tidy_text(t);
			mem_free(t);
		}
	} else {
		m->text = tidy_text(body);
		source = text_page(m->text, colors);
		mem_free(body);
	}
	m->html = html;
	Doc *d = &m->doc;
	d->url = str_dup("about:mail");
	d->root = html_parse(source, strlen(source));
	mem_free(source);
	if (html) {
		/* a quiet default look for mail that brings none of its own */
		static const char base_css[] = "body{margin:0;padding:12px 4px;font-family:sans-serif;font-size:14px;line-height:1.45;"
					       "color:#202124;background:#ffffff}"
					       "blockquote{margin:4px 0 4px 4px;padding-left:12px;border-left:3px solid #c8c8cc;color:#5f6368}";
		d->sheets = mem_alloc(sizeof(Sheet *));
		d->sheets[0] = css_parse(base_css, sizeof base_css - 1, d->url);
		d->nsheets = 1;
		if (!m->text) {
			StrBuf t = { 0 };
			bool space = false;
			Node *b = node_find(d->root, "body");
			dom_text(b ? b : d->root, &t, &space);
			m->text = tidy_text(sb_str(&t));
			sb_free(&t);
		}
	}
	find_style_sheets(d->root, d);
	d->state = DOC_READY;
	return m;
}

void msg_ref(Msg *m)
{
	atomic_add(&m->doc.refs, 1);
}

void msg_release(Msg *m)
{
	if (!m || atomic_add(&m->doc.refs, -1) > 0)
		return;
	Doc *d = &m->doc;
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
	mime_free(m->mime);
	mem_free(m->raw);
	mem_free(m->text);
	mem_free(m);
}

void msg_layout(Ui *ui, Msg *m, f32 width)
{
	Doc *d = &m->doc;
	width = floorf(width);
	bool resized = fabsf(width - m->laid_w) > 0.5f;
	bool images = d->images_done != m->laid_images;
	if (!resized && !images)
		return;
	if (atomic_load(&d->layout_busy)) {
		ui->wake_at = ui->now + 0.05;
		return;
	}
	/* pictures arriving one by one: lay out a few at a time */
	if (!resized && d->page && ui->now - m->laid_at < 0.3) {
		f64 at = m->laid_at + 0.31;
		ui->wake_at = ui->wake_at ? minf((f32)ui->wake_at, (f32)at) : at;
		return;
	}
	m->laid_w = width;
	m->laid_images = d->images_done;
	m->laid_at = ui->now;
	atomic_store(&d->layout_busy, 1);
	msg_ref(m);
	VJob *j = NEW(VJob);
	j->kind = VJ_LAYOUT;
	j->msg = m;
	j->width = width;
	v_push(j);
}

bool msg_take_page(Msg *m)
{
	Doc *d = &m->doc;
	mutex_lock(&d->lock);
	Page *np = d->new_page;
	d->new_page = NULL;
	mutex_unlock(&d->lock);
	if (!np)
		return false;
	page_free(d->page);
	d->page = np;
	return true;
}
