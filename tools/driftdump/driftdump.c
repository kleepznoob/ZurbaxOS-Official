/* driftdump: loads a page with Drift's engine and prints what layout made of it.
 *   driftdump URL|FILE [width] [boxes|text|all]
 * For working on the engine without a window. Images aren't fetched. */
#include "../../apps/drift/html.c"
#include "../../apps/drift/css.c"
#include "../../apps/drift/layout.c"

Image *doc_image(Doc *d, const char *url)
{
	Image *im = map_get(&d->image_map, url);
	if (!im) {
		im = NEW(Image);
		im->url = str_dup(url);
		map_set(&d->image_map, url, im);
	}
	return im;
}

static char *get(const char *url, usize *len)
{
	if (!str_starts(url, "http"))
		return file_read(url, len);
	HttpOptions o = { .user_agent = "Mozilla/5.0 (X11; Zurbax x86_64) Drift/1.0", .max_redirects = 10 };
	HttpResponse *r = http_request(url, &o);
	char *out = NULL;
	if (r->status == 200 && r->body) {
		out = str_ndup((char *)r->body, r->body_len);
		*len = r->body_len;
	} else {
		eprint("%s: %d %s\n", url, r->status, r->error);
	}
	http_response_free(r);
	return out;
}

static void sheets(Doc *d, Node *n, f32 w)
{
	for (Node *c = n->first; c; c = c->next) {
		if (c->type != NODE_ELEMENT)
			continue;
		Sheet *sh = NULL;
		if (str_eq(c->tag, "style") && css_media_matches(node_attr(c, "media"), w, 800)) {
			StrBuf t = { 0 };
			node_text_content(c, &t);
			sh = css_parse(sb_str(&t), t.len, d->url);
			sb_free(&t);
		} else if (str_eq(c->tag, "link") && node_attr(c, "rel") && str_find(node_attr(c, "rel"), "stylesheet") && node_attr(c, "href") &&
			   css_media_matches(node_attr(c, "media"), w, 800)) {
			char *u = url_resolve(d->url, node_attr(c, "href"));
			usize len;
			char *text = get(u, &len);
			if (text)
				sh = css_parse(text, len, u);
			mem_free(text);
			mem_free(u);
		} else {
			sheets(d, c, w);
		}
		if (sh) {
			d->sheets = mem_realloc(d->sheets, sizeof(Sheet *) * (usize)(d->nsheets + 1));
			d->sheets[d->nsheets++] = sh;
		}
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		eprint("usage: driftdump URL|FILE [width] [boxes|text|all]\n");
		return 2;
	}
	f32 w = argc > 2 ? (f32)str_to_int(argv[2], NULL) : 1180;
	const char *mode = argc > 3 ? argv[3] : "all";
	Doc d = { 0 };
	d.url = str_dup(argv[1]);
	usize len;
	char *html = get(argv[1], &len);
	if (!html)
		return 1;
	d.root = html_parse(html, len);
	i64 t0 = (i64)time_mono_ms();
	sheets(&d, d.root, w);
	i64 t1 = (i64)time_mono_ms();
	style_tree(d.root, d.sheets, d.nsheets, w, 800);
	i64 t2 = (i64)time_mono_ms();
	Page *p = layout_doc(&d, w, 800);
	i64 t3 = (i64)time_mono_ms();
	eprint("sheets %d (%lld ms)  style %lld ms  layout %lld ms  items %d  height %.0f\n", d.nsheets, (long long)(t1 - t0),
	       (long long)(t2 - t1), (long long)(t3 - t2), p->nitems, (double)p->height);
	for (int i = 0; i < p->nitems; i++) {
		Item *it = &p->items[i];
		if (it->kind == ITEM_TEXT && (str_eq(mode, "text") || str_eq(mode, "all")))
			print("T %6.0f %6.0f %5.0f %4.0f  %.*s\n", (double)it->r.x, (double)it->r.y, (double)it->r.w, (double)it->r.h,
			      (int)MIN(it->len, 70), p->text + it->text);
		else if (it->kind != ITEM_TEXT && (str_eq(mode, "boxes") || str_eq(mode, "all"))) {
			const char *cls = it->node ? node_attr(it->node, "class") : NULL;
			print("%c %6.0f %6.0f %5.0f %4.0f  %s.%s\n", "BTIMF"[it->kind], (double)it->r.x, (double)it->r.y, (double)it->r.w,
			      (double)it->r.h, it->node ? it->node->tag : "", cls ? cls : "");
		}
	}
	return 0;
}
