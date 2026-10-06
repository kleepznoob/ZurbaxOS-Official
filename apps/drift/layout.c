/* Layout: turns the styled tree into positioned boxes, text runs and images.
 *
 * Blocks stack vertically with collapsing margins; inline content is broken
 * into lines that flow around floats; tables, flex and grid containers size
 * their children first and then place them. Everything is emitted as Items in
 * page coordinates, so painting is a straight walk over a list. Boxes whose
 * final position isn't known yet (inline-blocks, table cells that get aligned)
 * are laid out in place and their items shifted afterwards. */
#include "drift.h"

enum { LB_ABSORB = 1, LB_BFC = 2 };
enum { IF_HIDDEN = 0x80 };

typedef struct {
	f32 x0, x1, y0, y1;      /* margin box */
	u8 side;
} Float;

typedef struct {
	VEC(Float) f;
} FloatCtx;

typedef struct {
	const Node *n;
	f32 mn, mx;
} ISlot;

typedef struct {
	Page *p;
	Doc *doc;
	f32 vw, vh;
	FloatCtx *fc;
	Node *link;
	f32 opacity;
	Node *marker;            /* list item waiting for its marker */
	f32 marker_x;
	int lines;
	f32 last_baseline;
	ISlot *ic;
	usize iccap, iclen;
	Node *canvas_node;       /* html or body: its background is the page's */
	int debug;               /* DRIFT_DEBUG_BOXES=depth: print boxes */
} Layout;

static int depth_of(const Node *n)
{
	int d = 0;
	for (; n->parent; n = n->parent)
		d++;
	return d;
}

typedef struct {
	f32 m[4], b[4], p[4];
	bool auto_l, auto_r;
} Edges;

static f32 layout_box(Layout *L, Node *n, f32 cx, f32 y, f32 cw, u32 flags, f32 forced_w, f32 *mb_out, int *bg_out, Rect *border_out);
static void intrinsic(Layout *L, Node *n, f32 *mn, f32 *mx);

/* ---- Helpers --------------------------------------------------------------------------------- */

static f32 lpx(Length l, f32 base, f32 fallback)
{
	switch (l.unit) {
	case UNIT_PX:
		return l.v;
	case UNIT_PCT:
		return base >= 0 ? base * l.v / 100 + l.add : fallback;
	default:
		return fallback;
	}
}

static void edges(const Style *s, f32 cw, Edges *e)
{
	for (int i = 0; i < 4; i++) {
		e->m[i] = lpx(s->margin[i], cw, 0);
		e->p[i] = maxf(0, lpx(s->padding[i], cw, 0));
		e->b[i] = s->border_w[i];
	}
	e->auto_l = s->margin[3].unit == UNIT_AUTO;
	e->auto_r = s->margin[1].unit == UNIT_AUTO;
}

static f32 edge_h(const Edges *e) { return e->b[1] + e->b[3] + e->p[1] + e->p[3]; }
static f32 edge_v(const Edges *e) { return e->b[0] + e->b[2] + e->p[0] + e->p[2]; }

static Font style_font(const Style *s)
{
	f32 size = maxf(1, s->font_size);
	return s->mono ? font_mono(size, s->font_weight) : font(size, s->font_weight);
}

static f32 style_lh(const Style *s)
{
	if (s->line_height <= 0)
		return s->font_size * 1.25f;
	return s->lh_factor ? s->line_height * s->font_size : s->line_height;
}

static Color fade(Layout *L, Color c)
{
	return L->opacity < 1 ? color_scale_alpha(c, L->opacity) : c;
}

static bool is_tag(const Node *n, const char *t)
{
	return n->type == NODE_ELEMENT && str_eq(n->tag, t);
}

static bool is_replaced(const Node *n)
{
	if (n->type != NODE_ELEMENT)
		return false;
	const char *t = n->tag;
	return str_eq(t, "img") || str_eq(t, "input") || str_eq(t, "select") || str_eq(t, "textarea") || str_eq(t, "svg") ||
	       str_eq(t, "progress") || str_eq(t, "meter");
}

/* The display that layout acts on. Inline elements containing blocks are
 * treated as blocks (a link wrapping a card, say). */
static u8 disp(const Node *n)
{
	const Style *s = n->style;
	if (!s)
		return D_INLINE;
	if (s->position == POS_ABSOLUTE || s->position == POS_FIXED)
		return D_NONE;
	return s->display;
}

static bool block_level(u8 d)
{
	return d == D_BLOCK || d == D_LIST_ITEM || d == D_TABLE || d == D_FLEX || d == D_GRID || d == D_TABLE_ROW ||
	       d == D_TABLE_GROUP || d == D_TABLE_CELL;
}

static bool atomic_inline(const Node *n, u8 d)
{
	return d == D_INLINE_BLOCK || d == D_INLINE_FLEX || d == D_INLINE_GRID || (d == D_INLINE && is_replaced(n));
}

/* Inline elements that contain block-level descendants get blockified. */
static bool mark_blocks(Node *n)
{
	bool any = false;
	for (Node *c = n->first; c; c = c->next) {
		if (c->type != NODE_ELEMENT)
			continue;
		bool inner = mark_blocks(c);
		u8 d = disp(c);
		if (d == D_NONE)
			continue;
		if (block_level(d) && !c->style->float_side)
			any = true;
		else if ((d == D_INLINE || d == D_CONTENTS) && inner && !is_replaced(c)) {
			if (d == D_INLINE)
				c->style->display = D_BLOCK;
			any = true;
		}
	}
	return any;
}

static bool is_ws(const char *s)
{
	for (; *s; s++)
		if (!is_space(*s))
			return false;
	return true;
}

static int item_new(Layout *L, u8 kind)
{
	Page *p = L->p;
	if (p->nitems == p->cap) {
		p->cap = p->cap ? p->cap * 2 : 256;
		p->items = mem_realloc(p->items, sizeof(Item) * (usize)p->cap);
	}
	Item *it = &p->items[p->nitems];
	memset(it, 0, sizeof *it);
	it->kind = kind;
	it->link = L->link;
	return p->nitems++;
}

static u32 page_text(Page *p, const char *s, usize n)
{
	if (p->text_len + n + 1 > p->text_cap) {
		p->text_cap = MAX(p->text_cap * 2, p->text_len + n + 4096);
		p->text = mem_realloc(p->text, p->text_cap);
	}
	u32 off = (u32)p->text_len;
	memcpy(p->text + off, s, n);
	p->text_len += n;
	p->text[p->text_len] = 0;
	return off;
}

static void shift_items(Page *p, int from, int to, f32 dx, f32 dy)
{
	for (int i = from; i < to; i++) {
		p->items[i].r.x += dx;
		p->items[i].r.y += dy;
		p->items[i].baseline += dy;
	}
}

static void anchor(Layout *L, const Node *n, f32 y)
{
	const char *id = node_attr(n, "id");
	if (!id && is_tag(n, "a"))
		id = node_attr(n, "name");
	if (id && *id && !map_has(&L->p->anchors, id))
		map_set(&L->p->anchors, id, (void *)(uintptr_t)(maxf(0, y) + 1));
}

/* ---- Floats -------------------------------------------------------------------------------- */

static void float_span(Layout *L, f32 cx, f32 cw, f32 y0, f32 y1, f32 *x0, f32 *x1)
{
	*x0 = cx, *x1 = cx + cw;
	vec_each(&L->fc->f, f)
	{
		if (f->y1 <= y0 || f->y0 >= y1)
			continue;
		if (f->side == FL_LEFT)
			*x0 = maxf(*x0, f->x1);
		else
			*x1 = minf(*x1, f->x0);
	}
	if (*x1 < *x0)
		*x1 = *x0;
}

static bool floats_at(Layout *L, f32 y0, f32 y1)
{
	vec_each(&L->fc->f, f) if (!(f->y1 <= y0 || f->y0 >= y1)) return true;
	return false;
}

static f32 float_bottom(Layout *L, u8 clear)
{
	f32 b = -1e9f;
	vec_each(&L->fc->f, f)
	{
		if (clear == CL_BOTH || (clear == CL_LEFT && f->side == FL_LEFT) || (clear == CL_RIGHT && f->side == FL_RIGHT))
			b = maxf(b, f->y1);
	}
	return b;
}

static f32 next_float_edge(Layout *L, f32 y)
{
	f32 best = 1e9f;
	vec_each(&L->fc->f, f) if (f->y1 > y && f->y1 < best) best = f->y1;
	return best;
}

/* Shrink-to-fit: as wide as the content wants, within what's available. */
static f32 stf_width(Layout *L, Node *n, f32 avail)
{
	f32 mn, mx;
	intrinsic(L, n, &mn, &mx);
	Edges e;
	edges(n->style, avail, &e);
	f32 m = e.m[1] + e.m[3];
	return maxf(0, minf(maxf(mn, avail), mx) - m);
}

static void place_float(Layout *L, Node *n, f32 cx, f32 cw, f32 y)
{
	Style *s = n->style;
	Edges e;
	edges(s, cw, &e);
	f32 bw;
	f32 spec = lpx(s->width, cw, -1);
	if (spec >= 0 || is_replaced(n))
		bw = -1;
	else
		bw = stf_width(L, n, cw);
	/* Width of the border box, for placement. */
	f32 ow;
	if (bw >= 0) {
		ow = bw + e.m[1] + e.m[3];
	} else {
		f32 mn, mx;
		intrinsic(L, n, &mn, &mx);
		ow = mx;
	}
	ow = minf(ow, cw);
	if (s->clear)
		y = maxf(y, float_bottom(L, s->clear));
	for (int guard = 0; guard < 64; guard++) {
		f32 x0, x1;
		float_span(L, cx, cw, y, y + 1, &x0, &x1);
		if (x1 - x0 >= ow - 0.5f || !floats_at(L, y, y + 1))
			break;
		f32 next = next_float_edge(L, y);
		if (next >= 1e8f)
			break;
		y = next;
	}
	f32 x0, x1;
	float_span(L, cx, cw, y, y + 1, &x0, &x1);
	f32 x = s->float_side == FL_LEFT ? x0 : x1 - ow;
	f32 mb = 0;
	Rect br;
	f32 h = layout_box(L, n, x, y + e.m[0], cw, LB_BFC, bw, &mb, NULL, &br);
	vec_push(&L->fc->f, (Float){ x, x + br.w + e.m[1] + e.m[3], y, y + e.m[0] + h + e.m[2], s->float_side });
}

/* ---- Images and replaced elements ----------------------------------------------------------- */

static const char *img_src(const Node *n)
{
	const char *src = node_attr(n, "src");
	const char *lazy = node_attr(n, "data-src");
	if (!lazy)
		lazy = node_attr(n, "data-lazy-src");
	if ((!src || !*src || str_starts(src, "data:image/gif") || str_starts(src, "data:image/svg")) && lazy && *lazy)
		return lazy;
	if (src && *src)
		return src;
	const char *set = node_attr(n, "srcset");
	if (!set)
		set = node_attr(n, "data-srcset");
	return set;   /* the first candidate (cut at the space below) */
}

static Image *img_for(Layout *L, const Node *n)
{
	const char *src = img_src(n);
	if (!src || !*src)
		return NULL;
	char buf[2048];
	usize k = 0;
	while (src[k] && !is_space(src[k]) && k < sizeof buf - 1)
		k++;
	memcpy(buf, src, k);
	buf[k] = 0;
	if (k && buf[k - 1] == ',')
		buf[k - 1] = 0;
	char *abs = url_resolve(L->p->base ? L->p->base : L->doc->url, buf);
	Image *im = doc_image(L->doc, abs);
	mem_free(abs);
	return im;
}

static bool svg_viewbox(const Node *n, f32 *w, f32 *h)
{
	const char *vb = node_attr(n, "viewbox");
	if (!vb)
		return false;
	f32 v[4];
	const char *p = vb;
	for (int i = 0; i < 4; i++) {
		while (*p && (is_space(*p) || *p == ','))
			p++;
		const char *e;
		v[i] = (f32)str_to_float(p, &e);
		if (e == p)
			return false;
		p = e;
	}
	*w = v[2], *h = v[3];
	return v[2] > 0 && v[3] > 0;
}

static const char *input_type(const Node *n)
{
	const char *t = node_attr(n, "type");
	return t && *t ? t : "text";
}

static bool input_is_button(const Node *n)
{
	const char *t = input_type(n);
	return str_eq_nocase(t, "submit") || str_eq_nocase(t, "button") || str_eq_nocase(t, "reset") || str_eq_nocase(t, "file");
}

const char *input_button_label(const Node *n)
{
	const char *v = node_attr(n, "value");
	if (v && *v)
		return v;
	const char *t = input_type(n);
	return str_eq_nocase(t, "reset") ? "Reset" : str_eq_nocase(t, "file") ? "Choose File…" : str_eq_nocase(t, "submit") ? "Submit" : "";
}

/* Content-box size of a replaced element. */
static void replaced_size(Layout *L, Node *n, f32 cw, f32 *ow, f32 *oh)
{
	Style *s = n->style;
	Edges e;
	edges(s, cw, &e);
	f32 iw = 0, ih = 0;
	bool ratio = false;
	Font f = style_font(s);
	f32 line = maxf(style_lh(s), s->font_size * 1.2f);
	if (is_tag(n, "img")) {
		Image *im = img_for(L, n);
		if (im && im->w > 0 && im->h > 0) {
			iw = (f32)im->w, ih = (f32)im->h;
			ratio = true;
		}
	} else if (is_tag(n, "svg")) {
		f32 vw, vh;
		if (svg_viewbox(n, &vw, &vh)) {
			iw = vw, ih = vh, ratio = true;
		} else {
			iw = 300, ih = 150;
		}
	} else if (is_tag(n, "input")) {
		const char *t = input_type(n);
		if (str_eq_nocase(t, "checkbox") || str_eq_nocase(t, "radio")) {
			iw = ih = 14;
		} else if (input_is_button(n)) {
			iw = text_width_z(f, input_button_label(n));
			ih = line;
		} else if (str_eq_nocase(t, "range")) {
			iw = 130, ih = 16;
		} else if (str_eq_nocase(t, "color")) {
			iw = 40, ih = line;
		} else {
			iw = 170, ih = line;
		}
	} else if (is_tag(n, "select")) {
		f32 w = 40;
		for (Node *o = n->first; o; o = o->next) {
			Node *list[2] = { o, NULL };
			if (is_tag(o, "optgroup"))
				list[0] = o->first;
			for (Node *opt = list[0]; opt; opt = (o != opt) ? opt->next : NULL) {
				if (is_tag(opt, "option")) {
					StrBuf t = { 0 };
					node_text_content(opt, &t);
					w = maxf(w, text_width(f, sb_str(&t), t.len) + 26);
					sb_free(&t);
				}
				if (opt == o)
					break;
			}
		}
		iw = minf(w, 400), ih = line;
	} else if (is_tag(n, "textarea")) {
		iw = 240, ih = line * 3;
	} else {
		iw = 160, ih = 12;   /* progress, meter */
	}
	f32 w = lpx(s->width, cw, -1), h = lpx(s->height, -1, -1);
	if (s->border_box) {
		if (w >= 0)
			w = maxf(0, w - edge_h(&e));
		if (h >= 0)
			h = maxf(0, h - edge_v(&e));
	}
	bool auto_h = h < 0;
	if (w < 0 && h < 0)
		w = iw, h = ih;
	else if (w < 0)
		w = ratio && ih > 0 ? h * iw / ih : iw;
	else if (h < 0)
		h = ratio && iw > 0 ? w * ih / iw : ih;
	f32 mw = lpx(s->max_width, cw, -1);
	if (mw >= 0 && s->border_box)
		mw = maxf(0, mw - edge_h(&e));
	if (mw >= 0 && w > mw) {
		if (ratio && auto_h && w > 0)
			h = h * mw / w;
		w = mw;
	}
	f32 mh = lpx(s->max_height, -1, -1);
	if (mh >= 0 && h > mh) {
		if (ratio && h > 0 && lpx(s->width, cw, -1) < 0)
			w = w * mh / h;
		h = mh;
	}
	f32 minw = lpx(s->min_width, cw, -1);
	if (minw > w)
		w = minw;
	*ow = maxf(0, w);
	*oh = maxf(0, h);
}

static void emit_replaced(Layout *L, Node *n, Rect content, Rect padbox)
{
	Style *s = n->style;
	if (!s->visible)
		return;
	if (is_tag(n, "img")) {
		Image *im = img_for(L, n);
		if (!im || content.w < 1 || content.h < 1)
			return;
		int i = item_new(L, ITEM_IMAGE);
		Item *it = &L->p->items[i];
		it->r = content;
		it->image = im;
		it->radius = s->radius;
		it->node = n;
		return;
	}
	if (is_tag(n, "svg")) {
		if (content.w < 1 || content.h < 1)
			return;
		char key[96];
		Color col = s->color;
		fmt(key, sizeof key, "svg:%p:%d:%d:%08x", (void *)n, (int)content.w, (int)content.h, col);
		Image *im = doc_image(L->doc, key);
		mutex_lock(&L->doc->lock);
		bool fresh = im->state == IMG_NEW;
		if (fresh)
			im->state = IMG_LOADING;
		mutex_unlock(&L->doc->lock);
		if (fresh) {
			StrBuf sb = { 0 };
			node_serialize(n, &sb);
			SvgDoc *doc = svg_parse(sb_str(&sb), sb.len);
			sb_free(&sb);
			Canvas *c = doc ? svg_rasterize(doc, (int)ceilf(content.w * 2), (int)ceilf(content.h * 2), col) : NULL;
			if (doc)
				svg_free(doc);
			mutex_lock(&L->doc->lock);
			im->canvas = c;
			im->w = (int)content.w, im->h = (int)content.h;
			im->state = c ? IMG_READY : IMG_FAILED;
			mutex_unlock(&L->doc->lock);
		}
		int i = item_new(L, ITEM_IMAGE);
		L->p->items[i].r = content;
		L->p->items[i].image = im;
		L->p->items[i].node = n;
		return;
	}
	int i = item_new(L, ITEM_INPUT);
	Item *it = &L->p->items[i];
	it->r = padbox;
	it->node = n;
	it->color = fade(L, s->color);
	it->font = style_font(s);
	it->baseline = content.y + (content.h + font_cap_height(it->font)) / 2;
}

/* ---- Intrinsic widths ---------------------------------------------------------------------------- */

typedef struct {
	f32 mn, mx, line;
	f32 word;                /* the unbreakable piece being measured */
	bool space;
} IState;

static ISlot *icache_find(Layout *L, const Node *n, bool insert)
{
	if (insert && L->iclen * 2 >= L->iccap) {
		usize ncap = L->iccap ? L->iccap * 2 : 1024;
		ISlot *old = L->ic;
		usize ocap = L->iccap;
		L->ic = NEW_ARRAY(ISlot, ncap);
		L->iccap = ncap;
		L->iclen = 0;
		for (usize i = 0; i < ocap; i++)
			if (old[i].n) {
				ISlot *s = icache_find(L, old[i].n, true);
				*s = old[i];
			}
		mem_free(old);
	}
	if (!L->iccap)
		return NULL;
	usize h = ((uintptr_t)n >> 4) * 0x9E3779B97F4A7C15ull;
	for (usize i = 0; i < L->iccap; i++) {
		ISlot *s = &L->ic[(h + i) & (L->iccap - 1)];
		if (s->n == n)
			return s;
		if (!s->n) {
			if (!insert)
				return NULL;
			s->n = n;
			L->iclen++;
			return s;
		}
	}
	return NULL;
}

static void text_intrinsic(IState *st, const char *t, const Style *s)
{
	Font f = style_font(s);
	f32 sw = text_width(f, " ", 1);
	bool pre = s->white_space == WS_PRE || s->white_space == WS_PRE_WRAP;
	bool nowrap = s->white_space == WS_NOWRAP || s->white_space == WS_PRE;
	f32 k = s->text_transform == TT_UPPER ? 1.15f : 1;
	const char *p = t;
	while (*p) {
		if (*p == '\n' && (pre || s->white_space == WS_PRE_LINE)) {
			st->mx = maxf(st->mx, st->line);
			st->line = st->word = 0;
			p++;
			continue;
		}
		if (is_space(*p) && !pre) {
			if (st->line > 0 && !st->space) {
				st->line += sw;
				st->space = true;
				if (nowrap)
					st->word += sw;
				else
					st->word = 0;
			}
			p++;
			continue;
		}
		const char *q = p;
		if (pre)
			while (*q && *q != '\n')
				q++;
		else
			while (*q && !is_space(*q))
				q++;
		f32 w = text_width(f, p, (usize)(q - p)) * k;
		st->line += w;
		st->word += w;
		st->mn = maxf(st->mn, st->word);
		st->space = false;
		p = q;
	}
}

static void inline_intrinsic(Layout *L, IState *st, Node *n)
{
	for (Node *c = n->first; c; c = c->next) {
		if (c->type == NODE_TEXT) {
			text_intrinsic(st, c->text, n->style);
			continue;
		}
		u8 d = disp(c);
		if (d == D_NONE)
			continue;
		if (is_tag(c, "br")) {
			st->mx = maxf(st->mx, st->line);
			st->line = st->word = 0;
			continue;
		}
		if (block_level(d) || c->style->float_side || atomic_inline(c, d)) {
			f32 mn, mx;
			intrinsic(L, c, &mn, &mx);
			st->mn = maxf(st->mn, mn);
			if (block_level(d) && !c->style->float_side) {
				st->mx = maxf(st->mx, maxf(st->line, mx));
				st->line = 0;
			} else {
				st->line += mx;
			}
			st->word = 0;
			st->space = false;
			continue;
		}
		Edges e;
		edges(c->style, -1, &e);
		st->line += e.m[3] + e.b[3] + e.p[3];
		inline_intrinsic(L, st, c);
		st->line += e.m[1] + e.b[1] + e.p[1];
	}
}

static void table_intrinsic(Layout *L, Node *tbl, f32 *mn, f32 *mx);

static void intrinsic(Layout *L, Node *n, f32 *omn, f32 *omx)
{
	ISlot *slot = icache_find(L, n, false);
	if (slot) {
		*omn = slot->mn, *omx = slot->mx;
		return;
	}
	Style *s = n->style;
	Edges e;
	edges(s, -1, &e);
	f32 mn = 0, mx = 0;
	f32 spec = lpx(s->width, -1, -1);
	u8 d = disp(n);
	if (is_replaced(n)) {
		f32 w, h;
		replaced_size(L, n, -1, &w, &h);
		mn = mx = w;
		if (lpx(s->max_width, -1, -1) < 0 && s->max_width.unit == UNIT_PCT)
			mn = 0;   /* max-width: 100% images shrink with their container */
	} else if (d == D_TABLE) {
		table_intrinsic(L, n, &mn, &mx);
		if (spec >= 0) {
			f32 w = s->border_box ? maxf(0, spec - edge_h(&e)) : spec;
			mn = mx = maxf(w, mn);
		}
	} else if (spec >= 0) {
		mn = mx = s->border_box ? maxf(0, spec - edge_h(&e)) : spec;
	} else if ((d == D_FLEX || d == D_INLINE_FLEX) && !s->flex_column) {
		int k = 0;
		for (Node *c = n->first; c; c = c->next) {
			if (c->type == NODE_TEXT) {
				if (!is_ws(c->text)) {
					IState st = { 0 };
					text_intrinsic(&st, c->text, s);
					mx += maxf(st.mx, st.line);
					mn = s->flex_wrap ? maxf(mn, st.mn) : mn + st.mn;
				}
				continue;
			}
			if (disp(c) == D_NONE)
				continue;
			f32 cmn, cmx;
			intrinsic(L, c, &cmn, &cmx);
			mx += cmx;
			mn = s->flex_wrap ? maxf(mn, cmn) : mn + cmn;
			k++;
		}
		if (k > 1) {
			mx += s->col_gap * (f32)(k - 1);
			if (!s->flex_wrap)
				mn += s->col_gap * (f32)(k - 1);
		}
	} else {
		IState st = { 0 };
		inline_intrinsic(L, &st, n);
		mn = st.mn;
		mx = maxf(st.mx, st.line);
		if (d == D_GRID && s->grid_cols) {
			/* columns side by side: a rough guess */
			int cols = 1;
			for (const char *p = s->grid_cols; *p; p++)
				cols += *p == ' ' && p[1] && p[1] != ' ';
			if (!str_find(s->grid_cols, "repeat"))
				mx *= (f32)MIN(cols, 4);
		}
	}
	if (!is_replaced(n)) {
		f32 minw = lpx(s->min_width, -1, -1), maxw = lpx(s->max_width, -1, -1);
		if (maxw >= 0) {
			if (s->border_box)
				maxw = maxf(0, maxw - edge_h(&e));
			mx = minf(mx, maxw);
			mn = minf(mn, maxw);
		}
		if (minw >= 0) {
			if (s->border_box)
				minw = maxf(0, minw - edge_h(&e));
			mn = maxf(mn, minw);
		}
	}
	if (s->overflow_hidden && spec < 0)
		mn = minf(mn, 0);
	mx = maxf(mx, mn);
	f32 add = edge_h(&e) + e.m[1] + e.m[3];
	*omn = ceilf(mn + add);
	*omx = ceilf(mx + add);
	slot = icache_find(L, n, true);
	if (slot)
		slot->mn = *omn, slot->mx = *omx;
}

/* ---- Inline content ---------------------------------------------------------------------------- */

enum { A_WORD, A_BOX, A_BREAK, A_OPEN, A_CLOSE, A_FLOAT };

typedef struct {
	u8 kind, flags, valign;
	bool space;              /* a collapsible space follows */
	bool nowrap;
	Node *node, *link;
	u32 off, len;            /* word text, in Run.text */
	f32 w, space_w;
	Font font;
	Color color;
	f32 asc, desc, lh;       /* text metrics, or the box above/below its baseline */
	f32 voff;                /* raise (negative) or lower, for sup/sub */
	int i0, i1;              /* A_BOX: its items, laid out with the top at y=0, x=0 */
	f32 edge, margin;        /* A_OPEN/A_CLOSE: margin+border+padding, and margin alone */
} Atom;

typedef struct {
	VEC(Atom) a;
	StrBuf text;
	f32 cw;
} Run;

static int last_word(Run *r)
{
	for (int i = (int)r->a.len - 1; i >= 0; i--) {
		u8 k = r->a.data[i].kind;
		if (k == A_WORD || k == A_BOX)
			return i;
		if (k == A_BREAK)
			return -1;
	}
	return -1;
}

static void transform(char *s, usize n, u8 tt, bool *word_start)
{
	for (usize i = 0; i < n; i++) {
		char c = s[i];
		if (tt == TT_UPPER)
			s[i] = (char)to_upper(c);
		else if (tt == TT_LOWER)
			s[i] = (char)to_lower(c);
		else if (tt == TT_CAPITALIZE && *word_start)
			s[i] = (char)to_upper(c);
		*word_start = !is_alnum(c) && c != '\'';
	}
}

static void push_word(Layout *L, Run *r, const Style *s, const char *w, usize n, Font f, f32 lh, Color col, u8 flags, f32 voff)
{
	Atom a = { .kind = A_WORD, .flags = flags, .link = L->link, .font = f, .color = col, .lh = lh, .voff = voff,
		   .nowrap = s->white_space == WS_NOWRAP || s->white_space == WS_PRE };
	a.off = (u32)r->text.len;
	a.len = (u32)n;
	sb_add(&r->text, w, n);
	if (s->text_transform) {
		bool ws = true;
		transform(r->text.data + a.off, n, s->text_transform, &ws);
	}
	a.w = text_width(f, r->text.data + a.off, n);
	a.space_w = text_width(f, " ", 1);
	a.asc = font_ascent(f);
	a.desc = font_descent(f);
	vec_push(&r->a, a);
}

static f32 valign_offset(const Style *s, const Style *ps)
{
	if (s->vertical_align == VA_SUPER)
		return -ps->font_size * 0.38f;
	if (s->vertical_align == VA_SUB)
		return ps->font_size * 0.22f;
	return 0;
}

static void gather_text(Layout *L, Run *r, const char *t, const Style *s, f32 voff)
{
	if (s->font_size < 0.5f)
		return;
	Font f = style_font(s);
	f32 lh = style_lh(s);
	Color col = fade(L, s->color);
	u8 flags = (u8)((s->underline ? IF_UNDERLINE : 0) | (s->strike ? IF_STRIKE : 0) | (s->visible ? 0 : IF_HIDDEN));
	bool pre = s->white_space == WS_PRE || s->white_space == WS_PRE_WRAP;
	bool keep_nl = pre || s->white_space == WS_PRE_LINE;
	const char *p = t;
	while (*p) {
		if (*p == '\n' && keep_nl) {
			Atom a = { .kind = A_BREAK, .font = f, .lh = lh, .asc = font_ascent(f), .desc = font_descent(f) };
			vec_push(&r->a, a);
			p++;
			continue;
		}
		if (*p == '\r') {
			p++;
			continue;
		}
		if (pre) {
			/* A whole line, tabs expanded. */
			char buf[1024];
			usize n = 0, col_no = 0;
			while (*p && *p != '\n' && n < sizeof buf - 8) {
				if (*p == '\t') {
					do
						buf[n++] = ' ';
					while (++col_no % 8 && n < sizeof buf - 1);
				} else if (*p != '\r') {
					buf[n++] = *p;
					if (((u8)*p & 0xc0) != 0x80)
						col_no++;
				}
				p++;
			}
			if (n)
				push_word(L, r, s, buf, n, f, lh, col, flags, voff);
			continue;
		}
		if (is_space(*p)) {
			int lw = last_word(r);
			if (lw >= 0)
				r->a.data[lw].space = true;
			while (*p && is_space(*p) && !(*p == '\n' && keep_nl))
				p++;
			continue;
		}
		const char *q = p;
		while (*q && !is_space(*q))
			q++;
		push_word(L, r, s, p, (usize)(q - p), f, lh, col, flags, voff);
		p = q;
	}
}

static void gather_atomic(Layout *L, Run *r, Node *n, f32 voff)
{
	Style *s = n->style;
	Edges e;
	edges(s, r->cw, &e);
	int i0 = L->p->nitems;
	f32 fw = -1;
	if (!is_replaced(n) && lpx(s->width, r->cw, -1) < 0)
		fw = stf_width(L, n, r->cw);
	int lines0 = L->lines;
	f32 mb = 0;
	Rect br;
	f32 bh = layout_box(L, n, 0, e.m[0], r->cw, LB_BFC, fw, &mb, NULL, &br);
	f32 h = e.m[0] + bh + mb;
	Atom a = { .kind = A_BOX, .node = n, .link = L->link, .i0 = i0, .i1 = L->p->nitems, .voff = voff };
	a.w = br.x + br.w + e.m[1];   /* br.x is the left margin */
	f32 base = h - mb;
	if (L->lines > lines0 && !s->overflow_hidden && !is_replaced(n))
		base = minf(L->last_baseline, h);
	if (is_tag(n, "input") || is_tag(n, "select") || is_tag(n, "textarea")) {
		Font f = style_font(s);
		base = e.m[0] + e.b[0] + e.p[0] + (br.h - edge_v(&e) + font_cap_height(f)) / 2;
		if (is_tag(n, "textarea"))
			base = h - mb;
	}
	a.valign = s->vertical_align;
	a.asc = base;
	a.desc = h - base;
	a.space_w = text_width(style_font(n->parent && n->parent->style ? n->parent->style : s), " ", 1);
	vec_push(&r->a, a);
}

static void gather_inline(Layout *L, Run *r, Node *n, f32 voff);

static void gather_children(Layout *L, Run *r, Node *n, f32 voff)
{
	for (Node *c = n->first; c; c = c->next) {
		if (c->type == NODE_TEXT) {
			gather_text(L, r, c->text, n->style, voff);
			continue;
		}
		u8 d = disp(c);
		if (d == D_NONE)
			continue;
		if (d == D_CONTENTS) {
			gather_children(L, r, c, voff);
			continue;
		}
		gather_inline(L, r, c, voff);
	}
}

static void gather_inline(Layout *L, Run *r, Node *n, f32 voff)
{
	Style *s = n->style;
	u8 d = disp(n);
	voff += valign_offset(s, n->parent && n->parent->style ? n->parent->style : s);
	if (is_tag(n, "br")) {
		Font f = style_font(s);
		Atom a = { .kind = A_BREAK, .font = f, .lh = style_lh(s), .asc = font_ascent(f), .desc = font_descent(f) };
		vec_push(&r->a, a);
		return;
	}
	if (s->float_side) {
		Atom a = { .kind = A_FLOAT, .node = n, .link = L->link };
		vec_push(&r->a, a);
		return;
	}
	Node *saved = L->link;
	if ((is_tag(n, "a") && node_attr(n, "href")) || is_tag(n, "button"))
		L->link = n;
	f32 so = L->opacity;
	L->opacity *= s->opacity;
	if (atomic_inline(n, d) || block_level(d)) {
		gather_atomic(L, r, n, voff);
	} else {
		Edges e;
		edges(s, r->cw, &e);
		Atom o = { .kind = A_OPEN, .node = n, .link = L->link, .edge = e.m[3] + e.b[3] + e.p[3], .margin = e.m[3] };
		vec_push(&r->a, o);
		gather_children(L, r, n, voff);
		Atom c = { .kind = A_CLOSE, .node = n, .link = L->link, .edge = e.m[1] + e.b[1] + e.p[1], .margin = e.m[1] };
		vec_push(&r->a, c);
	}
	L->opacity = so;
	L->link = saved;
}

/* ---- Line breaking -------------------------------------------------------------------------------- */

typedef struct {
	Atom *a;
	u32 off, len;
	f32 x, w;
} Ent;

typedef struct {
	Layout *L;
	Run *run;
	const Style *cs;
	f32 cx, cw;
	f32 y;
	f32 x0, x1;              /* this line's span, after floats */
	f32 x;                   /* pen, from x0 */
	VEC(Ent) ents;
	VEC(Node *) open;        /* inline boxes still open from earlier lines */
	VEC(Node *) deferred;    /* floats to place after this line */
	f32 strut_above, strut_below;
	bool first, nowrap, hide_text;
	int words;
} Lines;

static void line_begin(Lines *lc)
{
	Layout *L = lc->L;
	f32 h = lc->strut_above + lc->strut_below;
	for (int guard = 0; guard < 64; guard++) {
		float_span(L, lc->cx, lc->cw, lc->y, lc->y + h, &lc->x0, &lc->x1);
		if (lc->x1 - lc->x0 >= minf(40, lc->cw) || !floats_at(L, lc->y, lc->y + h))
			break;
		f32 next = next_float_edge(L, lc->y);
		if (next >= 1e8f)
			break;
		lc->y = next;
	}
	lc->x = lc->first ? lc->cs->text_indent : 0;
	if (lc->x < -500) {
		lc->hide_text = true;
		lc->x = 0;
	}
	lc->words = 0;
}

static void emit_text(Lines *lc, Ent *from, Ent *to, f32 ox, f32 baseline)
{
	Layout *L = lc->L;
	Atom *a = from->a;
	if ((a->flags & IF_HIDDEN) || lc->hide_text || COLOR_A(a->color) == 0)
		return;
	StrBuf sb = { 0 };
	for (Ent *e = from; e < to; e++) {
		if (e > from && (e - 1)->a->space && (e - 1)->off + (e - 1)->len == (e - 1)->a->off + (e - 1)->a->len)
			sb_addc(&sb, ' ');
		sb_add(&sb, lc->run->text.data + e->off, e->len);
	}
	int i = item_new(L, ITEM_TEXT);
	Item *it = &L->p->items[i];
	it->link = a->link;
	it->font = a->font;
	it->color = a->color;
	it->flags = a->flags & (IF_UNDERLINE | IF_STRIKE);
	f32 x = ox + from->x;
	f32 w = (to - 1)->x + (to - 1)->w - from->x;
	f32 bl = baseline + a->voff;
	it->r = rect(x, bl - a->asc, w, a->asc + a->desc);
	it->baseline = bl;
	it->text = page_text(L->p, sb_str(&sb), sb.len);
	it->len = (u32)sb.len;
	sb_free(&sb);
}

static bool same_run(const Atom *a, const Atom *b)
{
	return a->font.size == b->font.size && a->font.weight == b->font.weight && a->font.mono == b->font.mono &&
	       a->color == b->color && a->flags == b->flags && a->link == b->link && a->voff == b->voff;
}

static void emit_marker(Layout *L, f32 baseline, f32 x_text)
{
	Node *li = L->marker;
	L->marker = NULL;
	Style *s = li->style;
	if (s->list_style == LS_NONE || !s->visible)
		return;
	Font f = style_font(s);
	Color col = fade(L, s->color);
	if (s->list_style == LS_DECIMAL || s->list_style == LS_ALPHA || s->list_style == LS_ROMAN) {
		int k = 1;
		if (li->parent) {
			const char *st = node_attr(li->parent, "start");
			if (st)
				k = (int)str_to_int(st, NULL);
		}
		const char *v = node_attr(li, "value");
		if (v)
			k = (int)str_to_int(v, NULL);
		else
			for (Node *p = li->prev; p; p = p->prev)
				k += is_tag(p, "li");
		char num[24];
		if (s->list_style == LS_ALPHA && k >= 1 && k <= 26)
			fmt(num, sizeof num, "%c.", 'a' + k - 1);
		else if (s->list_style == LS_ROMAN && k >= 1 && k < 40) {
			static const char *const r[] = { "", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix" };
			static const char *const t[] = { "", "x", "xx", "xxx" };
			fmt(num, sizeof num, "%s%s.", t[k / 10], r[k % 10]);
		} else
			fmt(num, sizeof num, "%d.", k);
		f32 w = text_width_z(f, num);
		int i = item_new(L, ITEM_TEXT);
		Item *it = &L->p->items[i];
		it->link = NULL;
		it->font = f;
		it->color = col;
		it->r = rect(x_text - w - 6, baseline - font_ascent(f), w, font_ascent(f) + font_descent(f));
		it->baseline = baseline;
		it->text = page_text(L->p, num, strlen(num));
		it->len = (u32)strlen(num);
		return;
	}
	f32 sz = maxf(4, s->font_size * 0.36f);
	int i = item_new(L, ITEM_MARKER);
	Item *it = &L->p->items[i];
	it->link = NULL;
	it->marker = s->list_style == LS_CIRCLE ? MK_CIRCLE : s->list_style == LS_SQUARE ? MK_SQUARE : MK_DISC;
	it->color = col;
	it->r = rect(x_text - sz - s->font_size * 0.5f, baseline - font_x_height(f) / 2 - sz / 2, sz, sz);
}

static void line_flush(Lines *lc, bool forced)
{
	Layout *L = lc->L;
	if (!lc->ents.len && !forced && !lc->deferred.len)
		return;
	/* Carry trailing open boxes to the next line. */
	int keep = 0;
	if (!forced)
		while (keep < (int)lc->ents.len && lc->ents.data[lc->ents.len - 1 - keep].a->kind == A_OPEN)
			keep++;
	int n = (int)lc->ents.len - keep;
	bool content = false;
	for (int i = 0; i < n; i++)
		content |= lc->ents.data[i].a->kind == A_WORD || lc->ents.data[i].a->kind == A_BOX;
	if (!content && !forced) {
		/* only inline-box edges: no line box, but keep the boxes open */
		for (int i = 0; i < n; i++) {
			Atom *a = lc->ents.data[i].a;
			if (a->kind == A_OPEN)
				vec_push(&lc->open, a->node);
			else if (a->kind == A_CLOSE && lc->open.len)
				lc->open.len--;
		}
		memmove(lc->ents.data, lc->ents.data + n, sizeof(Ent) * (usize)keep);
		lc->ents.len = (usize)keep;
		return;
	}
	/* Vertical metrics. */
	f32 above = lc->strut_above, below = lc->strut_below;
	if (!content && forced && n == 0 && lc->ents.len == 0) {
		/* an empty line from <br>: the strut alone */
	}
	f32 xh = font_x_height(style_font(lc->cs));
	for (int i = 0; i < n; i++) {
		Atom *a = lc->ents.data[i].a;
		if (a->kind == A_WORD) {
			f32 hl = (a->lh - (a->asc + a->desc)) / 2;
			above = maxf(above, a->asc + hl - a->voff);
			below = maxf(below, a->desc + hl + a->voff);
		} else if (a->kind == A_BOX) {
			f32 h = a->asc + a->desc;
			if (a->valign == VA_MIDDLE) {
				above = maxf(above, h / 2 + xh / 2);
				below = maxf(below, h / 2 - xh / 2);
			} else if (a->valign == VA_TOP || a->valign == VA_BOTTOM) {
				above = maxf(above, h - lc->strut_below);
			} else {
				above = maxf(above, a->asc - a->voff);
				below = maxf(below, a->desc + a->voff);
			}
		}
	}
	f32 baseline = lc->y + above;
	f32 height = above + below;
	/* Alignment. */
	f32 end = 0;
	for (int i = n - 1; i >= 0; i--) {
		Ent *e = &lc->ents.data[i];
		end = maxf(end, e->x + e->w);
		if (e->a->kind == A_WORD || e->a->kind == A_BOX)
			break;
	}
	f32 avail = lc->x1 - lc->x0;
	f32 ox = lc->x0;
	if (lc->cs->text_align == TA_CENTER || lc->cs->text_align == TA_CENTER_BLOCKS)
		ox += maxf(0, (avail - end) / 2);
	else if (lc->cs->text_align == TA_RIGHT)
		ox += maxf(0, avail - end);
	/* Inline box decorations (backgrounds and borders of spans and links),
	 * outermost first so inner ones paint on top. */
	{
		Node *stack[64];
		f32 start[64];
		int order[64];
		int depth = 0, seq = 0;
		vec_each(&lc->open, o) if (depth < 64)
		{
			stack[depth] = *o;
			order[depth] = seq++;
			start[depth++] = 0;
		}
		struct { Node *n; f32 sx, ex; int order; bool closed; } decos[64];
		int nd = 0;
		for (int i = 0; i < n; i++) {
			Ent *en = &lc->ents.data[i];
			Atom *a = en->a;
			if (a->kind == A_OPEN) {
				if (depth < 64) {
					stack[depth] = a->node;
					order[depth] = seq++;
					start[depth++] = en->x + a->margin;
				}
				anchor(L, a->node, lc->y);
			} else if (a->kind == A_CLOSE && depth > 0) {
				depth--;
				if (nd < 64)
					decos[nd++] = (__typeof__(decos[0])){ stack[depth], start[depth], en->x + en->w - a->margin, order[depth], true };
			}
		}
		for (int k = depth - 1; k >= 0; k--)
			if (nd < 64)
				decos[nd++] = (__typeof__(decos[0])){ stack[k], start[k], end, order[k], false };
		for (int i = 1; i < nd; i++)
			for (int j = i; j > 0 && decos[j - 1].order > decos[j].order; j--) {
				__typeof__(decos[0]) t = decos[j];
				decos[j] = decos[j - 1];
				decos[j - 1] = t;
			}
		for (int i = 0; i < nd; i++) {
			Style *bs = decos[i].n->style;
			f32 sx = decos[i].sx, ex = decos[i].ex;
			bool deco = COLOR_A(bs->background) || bs->border_w[0] + bs->border_w[1] + bs->border_w[2] + bs->border_w[3] > 0;
			if (!deco || !bs->visible || ex <= sx)
				continue;
			Edges e;
			edges(bs, lc->cw, &e);
			Font bf = style_font(bs);
			int it = item_new(L, ITEM_BOX);
			Item *b = &L->p->items[it];
			b->link = NULL;
			f32 top = baseline - font_ascent(bf) - e.p[0] - e.b[0];
			f32 bot = baseline + font_descent(bf) + e.p[2] + e.b[2];
			b->r = rect(ox + sx, top, ex - sx, bot - top);
			b->background = fade(L, bs->background);
			for (int k = 0; k < 4; k++) {
				b->border_w[k] = e.b[k];
				b->border_c[k] = fade(L, bs->border_c[k]);
			}
			if (!decos[i].closed)
				b->border_w[1] = 0;
			b->radius = bs->radius;
		}
		vec_clear(&lc->open);
		for (int k = 0; k < depth; k++)
			vec_push(&lc->open, stack[k]);
	}
	/* Text runs and boxes. */
	for (int i = 0; i < n;) {
		Ent *e = &lc->ents.data[i];
		if (e->a->kind == A_BOX) {
			Atom *a = e->a;
			f32 top;
			f32 h = a->asc + a->desc;
			if (a->valign == VA_MIDDLE)
				top = baseline - xh / 2 - h / 2;
			else if (a->valign == VA_TOP)
				top = lc->y;
			else if (a->valign == VA_BOTTOM)
				top = lc->y + height - h;
			else
				top = baseline - a->asc + a->voff;
			shift_items(L->p, a->i0, a->i1, ox + e->x, top);
			i++;
			continue;
		}
		if (e->a->kind != A_WORD) {
			i++;
			continue;
		}
		int j = i + 1;
		while (j < n && lc->ents.data[j].a->kind == A_WORD && same_run(lc->ents.data[j].a, e->a))
			j++;
		/* A run stops at boxes and edges too (they move the pen). */
		emit_text(lc, e, &lc->ents.data[j], ox, baseline);
		i = j;
	}
	if (L->marker)
		emit_marker(L, baseline, L->marker_x);
	L->lines++;
	L->last_baseline = baseline;
	lc->y += height;
	lc->first = false;
	memmove(lc->ents.data, lc->ents.data + n, sizeof(Ent) * (usize)keep);
	lc->ents.len = (usize)keep;
	/* Floats that came up mid-line. */
	vec_each(&lc->deferred, f) place_float(L, *f, lc->cx, lc->cw, lc->y);
	vec_clear(&lc->deferred);
	line_begin(lc);
	for (usize k = 0; k < lc->ents.len; k++) {
		lc->ents.data[k].x = lc->x;
		lc->x += lc->ents.data[k].w;
	}
}

static void line_add(Lines *lc, Atom *a, u32 off, u32 len, f32 w)
{
	Ent e = { a, off, len, lc->x, w };
	vec_push(&lc->ents, e);
	lc->x += w;
	if (a->kind == A_WORD || a->kind == A_BOX)
		lc->words++;
}

static f32 layout_run(Layout *L, Run *r, const Style *cs, f32 cx, f32 y, f32 cw)
{
	if (!r->a.len)
		return 0;
	bool any = false;
	vec_each(&r->a, a) if (a->kind != A_OPEN && a->kind != A_CLOSE) any = true;
	if (!any) {
		/* Empty inline elements still mark anchors. */
		vec_each(&r->a, a) if (a->kind == A_OPEN) anchor(L, a->node, y);
		return 0;
	}
	Lines lc = { .L = L, .run = r, .cs = cs, .cx = cx, .cw = cw, .y = y, .first = true };
	Font sf = style_font(cs);
	f32 lh = style_lh(cs);
	f32 hl = (lh - (font_ascent(sf) + font_descent(sf))) / 2;
	lc.strut_above = font_ascent(sf) + hl;
	lc.strut_below = font_descent(sf) + hl;
	lc.nowrap = cs->white_space == WS_NOWRAP;
	line_begin(&lc);
	for (usize i = 0; i < r->a.len; i++) {
		Atom *a = &r->a.data[i];
		switch (a->kind) {
		case A_BREAK:
			line_flush(&lc, true);
			break;
		case A_OPEN:
		case A_CLOSE:
			line_add(&lc, a, 0, 0, a->edge);
			break;
		case A_FLOAT:
			if (!lc.words) {
				place_float(L, a->node, cx, cw, lc.y);
				line_begin(&lc);
				for (usize k = 0; k < lc.ents.len; k++) {
					lc.ents.data[k].x = lc.x;
					lc.x += lc.ents.data[k].w;
				}
			} else {
				vec_push(&lc.deferred, a->node);
			}
			break;
		case A_WORD:
		case A_BOX: {
			f32 avail = lc.x1 - lc.x0;
			/* Words inside white-space: nowrap stick together. */
			bool glue = lc.nowrap;
			if (a->nowrap && lc.ents.len) {
				for (int k = (int)lc.ents.len - 1; k >= 0; k--) {
					Atom *pa = lc.ents.data[k].a;
					if (pa->kind == A_WORD || pa->kind == A_BOX) {
						glue = pa->nowrap;
						break;
					}
				}
			}
			if (!glue && lc.words && lc.x + a->w > avail + 0.5f)
				line_flush(&lc, false);
			avail = lc.x1 - lc.x0;
			if (a->kind == A_WORD && lc.x + a->w > avail + 0.5f && !glue) {
				/* Too long for any line: break it where it fits. */
				u32 off = a->off, rem = a->len;
				while (rem) {
					f32 room = maxf(0, avail - lc.x);
					usize k = text_fit(a->font, r->text.data + off, rem, room);
					if (k == 0)
						k = lc.words ? 0 : utf8_next(r->text.data + off, rem, 0);
					if (k == 0) {
						line_flush(&lc, false);
						avail = lc.x1 - lc.x0;
						continue;
					}
					f32 w = text_width(a->font, r->text.data + off, k);
					line_add(&lc, a, off, (u32)k, w);
					off += (u32)k, rem -= (u32)k;
					if (rem) {
						line_flush(&lc, false);
						avail = lc.x1 - lc.x0;
					}
				}
			} else {
				line_add(&lc, a, a->off, a->len, a->w);
			}
			if (a->space)
				lc.x += a->space_w;
			break;
		}
		}
	}
	if (lc.ents.len || lc.deferred.len)
		line_flush(&lc, lc.ents.len > 0);
	f32 h = lc.y - y;
	vec_free(&lc.ents);
	vec_free(&lc.open);
	vec_free(&lc.deferred);
	return h;
}

static void run_reset(Run *r)
{
	vec_clear(&r->a);
	sb_clear(&r->text);
}

/* ---- Block flow ------------------------------------------------------------------------------------ */

static bool has_top_edge(const Style *s, f32 cw)
{
	return s->border_w[0] > 0 || lpx(s->padding[0], cw, 0) > 0;
}

static bool makes_bfc(const Node *n)
{
	const Style *s = n->style;
	u8 d = s->display;
	return s->overflow_hidden || s->float_side || d == D_TABLE || d == D_FLEX || d == D_GRID || d == D_INLINE_BLOCK ||
	       d == D_TABLE_CELL || d == D_INLINE_FLEX || d == D_INLINE_GRID || is_replaced(n);
}

/* Inline elements with nothing visible in them (anchors, empty spans). */
static bool inline_empty(const Node *n)
{
	if (is_replaced(n) || is_tag(n, "br"))
		return false;
	for (const Node *c = n->first; c; c = c->next) {
		if (c->type == NODE_TEXT) {
			if (!is_ws(c->text))
				return false;
		} else if (disp(c) != D_NONE && (disp(c) != D_INLINE || !inline_empty(c))) {
			return false;
		}
	}
	return true;
}

/* The top margin of a block, collapsed with its first child's when nothing separates them. */
static f32 collapsed_mt(Node *n, f32 cw, int depth)
{
	Style *s = n->style;
	f32 mt = lpx(s->margin[0], cw, 0);
	if (depth > 32 || has_top_edge(s, cw) || makes_bfc(n) || is_replaced(n))
		return mt;
	u8 d = disp(n);
	if (d != D_BLOCK && d != D_LIST_ITEM)
		return mt;
	if (d == D_LIST_ITEM && s->list_style != LS_NONE)
		return mt;
	for (Node *c = n->first; c; c = c->next) {
		if (c->type == NODE_TEXT) {
			if (is_ws(c->text))
				continue;
			return mt;
		}
		u8 cd = disp(c);
		if (cd == D_NONE)
			continue;
		if (c->style->float_side || (cd == D_INLINE && inline_empty(c)))
			continue;
		if (!block_level(cd) || cd == D_TABLE_CELL)
			return mt;
		f32 cm = collapsed_mt(c, cw, depth + 1);
		return (mt >= 0 && cm >= 0) ? maxf(mt, cm) : mt + cm;
	}
	return mt;
}

static f32 collapse(f32 a, f32 b)
{
	if (a >= 0 && b >= 0)
		return maxf(a, b);
	if (a < 0 && b < 0)
		return minf(a, b);
	return a + b;
}

typedef struct {
	Layout *L;
	Node *parent;
	f32 cx, cw, y0;
	f32 y, pending;
	bool absorb;             /* the next block's top margin is already counted */
	bool started;            /* something has been placed */
	Run run;
} Flow;

static void flow_flush(Flow *f)
{
	if (!f->run.a.len)
		return;
	f32 h0 = f->y;
	bool any = false;
	vec_each(&f->run.a, a) if (a->kind == A_WORD || a->kind == A_BOX || a->kind == A_BREAK) any = true;
	if (any) {
		f->y += f->pending;
		f->pending = 0;
		f->absorb = false;
		f->started = true;
	}
	f->y += layout_run(f->L, &f->run, f->parent->style, f->cx, f->y, f->cw);
	if (f->y > h0)
		f->started = true;
	run_reset(&f->run);
}

static void flow_block(Flow *f, Node *c)
{
	Layout *L = f->L;
	Style *s = c->style;
	flow_flush(f);
	f32 mt = f->absorb ? 0 : collapsed_mt(c, f->cw, 0);
	bool absorb_child = !has_top_edge(s, f->cw) && !makes_bfc(c);
	f32 top = f->y + collapse(f->pending, mt);
	if (s->clear) {
		f32 fb = float_bottom(L, s->clear);
		if (fb > top)
			top = fb;
	}
	/* Boxes that start a formatting context sit beside floats rather than under them. */
	f32 cx = f->cx, cw = f->cw;
	if (makes_bfc(c) && floats_at(L, top, top + 1)) {
		f32 x0, x1;
		float_span(L, f->cx, f->cw, top, top + 1, &x0, &x1);
		f32 mn, mx;
		intrinsic(L, c, &mn, &mx);
		if (x1 - x0 < mn && x1 - x0 < f->cw) {
			top = maxf(top, next_float_edge(L, top));
			float_span(L, f->cx, f->cw, top, top + 1, &x0, &x1);
		}
		cx = x0, cw = x1 - x0;
	}
	f32 mb = 0;
	Rect br;
	int items0 = L->p->nitems;
	f32 h = layout_box(L, c, cx, top, cw, absorb_child ? LB_ABSORB : 0, -1, &mb, NULL, &br);
	Edges e;
	edges(s, f->cw, &e);
	bool empty = h <= 0 && L->p->nitems == items0 && e.b[0] + e.b[2] + e.p[0] + e.p[2] == 0;
	if (empty) {
		f->pending = collapse(collapse(f->pending, mt), mb);
		return;
	}
	f->y = top + h;
	f->pending = mb;
	f->absorb = false;
	f->started = true;
}

static void flow_children(Flow *f, Node *n);

static void flow_child(Flow *f, Node *c)
{
	Layout *L = f->L;
	if (c->type == NODE_TEXT) {
		gather_text(L, &f->run, c->text, f->parent->style, 0);
		return;
	}
	u8 d = disp(c);
	if (d == D_NONE)
		return;
	if (d == D_CONTENTS) {
		flow_children(f, c);
		return;
	}
	if (c->style->float_side) {
		bool inline_content = false;
		vec_each(&f->run.a, a) if (a->kind == A_WORD || a->kind == A_BOX) inline_content = true;
		if (inline_content) {
			gather_inline(L, &f->run, c, 0);
		} else {
			flow_flush(f);
			place_float(L, c, f->cx, f->cw, f->y + (f->pending > 0 ? f->pending : 0));
		}
		return;
	}
	if (block_level(d)) {
		flow_block(f, c);
		return;
	}
	gather_inline(L, &f->run, c, 0);
}

static void flow_children(Flow *f, Node *n)
{
	for (Node *c = n->first; c; c = c->next)
		flow_child(f, c);
}

/* Lays out the children of a block container. Returns the content height;
 * *trail gets a bottom margin that may collapse through the parent. */
static f32 layout_flow(Layout *L, Node *n, f32 cx, f32 cy, f32 cw, bool absorb, f32 *trail)
{
	Flow f = { .L = L, .parent = n, .cx = cx, .cw = cw, .y0 = cy, .y = cy, .absorb = absorb };
	f.run.cw = cw;
	flow_children(&f, n);
	flow_flush(&f);
	vec_free(&f.run.a);
	sb_free(&f.run.text);
	*trail = f.pending;
	return f.y - cy;
}

/* ---- Tables -------------------------------------------------------------------------------------------- */

typedef struct {
	Node *n;
	int row, col, span, rspan;
	f32 mn, mx;
	f32 y, h;
	int i0, i1, bg;
} Cell;

typedef struct {
	Node *n;
	int c0, nc;
	f32 y, h;
} Row;

typedef struct {
	VEC(Row) rows;
	VEC(Cell) cells;
	int ncols;
	f32 *cmin, *cmax, *cpct, *w, *x;
	bool *cfixed;
	f32 spacing;
} Table;

static void table_add_row(Table *t, Node *tr)
{
	Row r = { .n = tr, .c0 = (int)t->cells.len };
	for (Node *c = tr->first; c; c = c->next) {
		if (c->type != NODE_ELEMENT || disp(c) == D_NONE)
			continue;
		Cell cell = { .n = c, .row = (int)t->rows.len, .span = 1, .rspan = 1 };
		const char *v = node_attr(c, "colspan");
		if (v)
			cell.span = (int)CLAMP(str_to_int(v, NULL), 1, 64);
		v = node_attr(c, "rowspan");
		if (v)
			cell.rspan = (int)CLAMP(str_to_int(v, NULL), 1, 64);
		vec_push(&t->cells, cell);
	}
	r.nc = (int)t->cells.len - r.c0;
	vec_push(&t->rows, r);
}

static void table_collect(Layout *L, Node *tbl, Table *t)
{
	for (Node *c = tbl->first; c; c = c->next) {
		if (c->type != NODE_ELEMENT)
			continue;
		u8 d = disp(c);
		if (d == D_TABLE_GROUP) {
			for (Node *r = c->first; r; r = r->next)
				if (r->type == NODE_ELEMENT && disp(r) != D_NONE && (disp(r) == D_TABLE_ROW || is_tag(r, "tr")))
					table_add_row(t, r);
		} else if (d == D_TABLE_ROW) {
			table_add_row(t, c);
		}
	}
	/* Columns, skipping slots taken by row spans. */
	int busy[256] = { 0 };
	t->ncols = 0;
	vec_each(&t->rows, r)
	{
		int col = 0;
		for (int i = 0; i < r->nc; i++) {
			Cell *cell = &t->cells.data[r->c0 + i];
			while (col < 256 && busy[col] > 0)
				col++;
			cell->col = MIN(col, 255);
			for (int k = 0; k < cell->span && col + k < 256; k++)
				busy[col + k] = cell->rspan;
			col += cell->span;
			t->ncols = MAX(t->ncols, MIN(col, 256));
		}
		for (int k = 0; k < 256; k++)
			if (busy[k] > 0)
				busy[k]--;
	}
	int nc = MAX(t->ncols, 1);
	t->cmin = NEW_ARRAY(f32, nc);
	t->cmax = NEW_ARRAY(f32, nc);
	t->cpct = NEW_ARRAY(f32, nc);
	t->w = NEW_ARRAY(f32, nc);
	t->x = NEW_ARRAY(f32, nc + 1);
	t->cfixed = NEW_ARRAY(bool, nc);
	t->spacing = tbl->style->border_collapse ? 0 : tbl->style->border_spacing;
	/* Single-column cells first, then spans. */
	for (int pass = 0; pass < 2; pass++) {
		vec_each(&t->cells, cell)
		{
			if ((cell->span > 1) != (pass == 1))
				continue;
			if (pass == 0)
				intrinsic(L, cell->n, &cell->mn, &cell->mx);
			else
				intrinsic(L, cell->n, &cell->mn, &cell->mx);
			Style *cs = cell->n->style;
			int c0 = cell->col, span = MIN(cell->span, nc - c0);
			if (span <= 0)
				continue;
			if (span == 1) {
				t->cmin[c0] = maxf(t->cmin[c0], cell->mn);
				f32 w = lpx(cs->width, -1, -1);
				if (w >= 0) {
					Edges e;
					edges(cs, -1, &e);
					if (!cs->border_box)
						w += edge_h(&e);
					t->cfixed[c0] = true;
					t->cmax[c0] = maxf(t->cmax[c0], maxf(w, t->cmin[c0]));
				} else if (!t->cfixed[c0]) {
					t->cmax[c0] = maxf(t->cmax[c0], cell->mx);
				}
				if (cs->width.unit == UNIT_PCT)
					t->cpct[c0] = maxf(t->cpct[c0], cs->width.v);
			} else {
				f32 smin = 0, smax = 0;
				for (int k = 0; k < span; k++)
					smin += t->cmin[c0 + k], smax += t->cmax[c0 + k];
				f32 sp = t->spacing * (f32)(span - 1);
				if (cell->mn > smin + sp)
					for (int k = 0; k < span; k++)
						t->cmin[c0 + k] += (cell->mn - smin - sp) / (f32)span;
				if (cell->mx > smax + sp)
					for (int k = 0; k < span; k++)
						t->cmax[c0 + k] += (cell->mx - smax - sp) / (f32)span;
			}
		}
	}
	for (int i = 0; i < nc; i++)
		t->cmax[i] = maxf(t->cmax[i], t->cmin[i]);
}

static void table_free(Table *t)
{
	vec_free(&t->rows);
	vec_free(&t->cells);
	mem_free(t->cmin);
	mem_free(t->cmax);
	mem_free(t->cpct);
	mem_free(t->w);
	mem_free(t->x);
	mem_free(t->cfixed);
}

static void table_intrinsic(Layout *L, Node *tbl, f32 *mn, f32 *mx)
{
	Table t = { 0 };
	table_collect(L, tbl, &t);
	f32 a = 0, b = 0;
	for (int i = 0; i < t.ncols; i++)
		a += t.cmin[i], b += t.cmax[i];
	f32 sp = t.spacing * (f32)(t.ncols + 1);
	/* captions */
	for (Node *c = tbl->first; c; c = c->next)
		if (c->type == NODE_ELEMENT && disp(c) == D_BLOCK) {
			f32 cmn, cmx;
			intrinsic(L, c, &cmn, &cmx);
			a = maxf(a, cmn - sp);
		}
	*mn = a + sp;
	*mx = b + sp;
	table_free(&t);
}

static f32 layout_table(Layout *L, Node *tbl, f32 cx, f32 cy, f32 cw)
{
	Table t = { 0 };
	table_collect(L, tbl, &t);
	f32 y = cy;
	/* Captions go on top. */
	for (Node *c = tbl->first; c; c = c->next) {
		if (c->type != NODE_ELEMENT || disp(c) != D_BLOCK)
			continue;
		f32 mb = 0;
		Rect br;
		y += layout_box(L, c, cx, y, cw, LB_BFC, -1, &mb, NULL, &br) + mb;
	}
	int nc = t.ncols;
	if (!nc) {
		table_free(&t);
		return y - cy;
	}
	f32 avail = maxf(0, cw - t.spacing * (f32)(nc + 1));
	/* Percent columns first. */
	f32 used = 0, smin = 0, smax = 0;
	for (int i = 0; i < nc; i++) {
		if (t.cpct[i] > 0) {
			t.w[i] = maxf(t.cmin[i], avail * t.cpct[i] / 100);
			used += t.w[i];
		} else {
			smin += t.cmin[i], smax += t.cmax[i];
		}
	}
	f32 rem = maxf(0, avail - used);
	if (smax <= rem) {
		f32 extra = rem - smax;
		f32 weight = 0;
		int flexible = 0;
		for (int i = 0; i < nc; i++)
			if (t.cpct[i] <= 0 && !t.cfixed[i])
				weight += t.cmax[i], flexible++;
		for (int i = 0; i < nc; i++) {
			if (t.cpct[i] > 0)
				continue;
			t.w[i] = t.cmax[i];
			if (!t.cfixed[i] && flexible)
				t.w[i] += weight > 0 ? extra * t.cmax[i] / weight : extra / (f32)flexible;
			else if (!flexible && t.cpct[i] <= 0 && smax > 0)
				t.w[i] += extra * t.cmax[i] / smax;
		}
	} else if (smin >= rem) {
		for (int i = 0; i < nc; i++)
			if (t.cpct[i] <= 0)
				t.w[i] = t.cmin[i];
	} else {
		f32 k = (rem - smin) / (smax - smin);
		for (int i = 0; i < nc; i++)
			if (t.cpct[i] <= 0)
				t.w[i] = t.cmin[i] + (t.cmax[i] - t.cmin[i]) * k;
	}
	f32 x = cx + t.spacing;
	for (int i = 0; i < nc; i++) {
		t.x[i] = x;
		x += t.w[i] + t.spacing;
	}
	t.x[nc] = x;
	y += t.spacing;
	vec_each(&t.rows, r)
	{
		Style *rs = r->n->style;
		int bg = -1;
		if (COLOR_A(rs->background) && rs->visible) {
			bg = item_new(L, ITEM_BOX);
			L->p->items[bg].background = fade(L, rs->background);
		}
		f32 rh = maxf(0, lpx(rs->height, -1, 0));
		for (int i = 0; i < r->nc; i++) {
			Cell *c = &t.cells.data[r->c0 + i];
			int c0 = MIN(c->col, nc - 1), span = MAX(1, MIN(c->span, nc - c0));
			f32 cwid = t.x[c0 + span] - t.spacing - t.x[c0];
			c->i0 = L->p->nitems;
			f32 mb = 0;
			Rect br;
			c->h = layout_box(L, c->n, t.x[c0], y, cwid, LB_BFC, cwid, &mb, &c->bg, &br);
			c->i1 = L->p->nitems;
			c->y = y;
			if (c->rspan == 1)
				rh = maxf(rh, c->h);
		}
		r->y = y;
		r->h = rh;
		for (int i = 0; i < r->nc; i++) {
			Cell *c = &t.cells.data[r->c0 + i];
			if (c->rspan != 1)
				continue;
			u8 va = c->n->style->vertical_align;
			if (node_attr(r->n, "valign") && !node_attr(c->n, "valign"))
				va = rs->vertical_align;
			f32 extra = rh - c->h;
			if (extra > 0.5f) {
				f32 dy = va == VA_MIDDLE ? extra / 2 : va == VA_BOTTOM ? extra : 0;
				int from = c->bg >= 0 ? c->bg + 1 : c->i0;
				if (dy > 0)
					shift_items(L->p, from, c->i1, 0, dy);
				if (c->bg >= 0)
					L->p->items[c->bg].r.h = rh;
			}
		}
		if (bg >= 0)
			L->p->items[bg].r = rect(t.x[0], y, t.x[nc] - t.spacing - t.x[0], rh);
		y += rh + t.spacing;
	}
	/* Cells spanning rows: stretch their backgrounds over the rows they cover. */
	vec_each(&t.cells, c)
	{
		if (c->rspan <= 1)
			continue;
		int last = MIN(c->row + c->rspan, (int)t.rows.len) - 1;
		f32 bottom = t.rows.data[last].y + t.rows.data[last].h;
		f32 target = bottom - c->y;
		if (c->bg >= 0 && target > L->p->items[c->bg].r.h)
			L->p->items[c->bg].r.h = target;
		if (target > c->h && c->n->style->vertical_align == VA_MIDDLE)
			shift_items(L->p, c->bg >= 0 ? c->bg + 1 : c->i0, c->i1, 0, (target - c->h) / 2);
	}
	table_free(&t);
	return y - cy;
}

/* ---- Flex and grid ---------------------------------------------------------------------------------------- */

typedef struct {
	Node *n;                 /* element, or text node for anonymous items */
	f32 basis, mn, mx, grow, shrink, w;
	f32 m[4];
	bool auto_l, auto_r;
	f32 h, x;
	int i0, i1, bg;
} FlexItem;

static f32 layout_anon_text(Layout *L, Node *text, const Style *s, f32 x, f32 y, f32 w)
{
	Run r = { .cw = w };
	gather_text(L, &r, text->text, s, 0);
	f32 h = layout_run(L, &r, s, x, y, w);
	vec_free(&r.a);
	sb_free(&r.text);
	return h;
}

static int item_order(const FlexItem *it)
{
	return it->n->type == NODE_ELEMENT ? it->n->style->order : 0;
}

static int flex_items(Layout *L, Node *n, f32 cw, FlexItem **out)
{
	VEC(FlexItem) v = { 0 };
	for (Node *c = n->first; c; c = c->next) {
		FlexItem it = { .n = c, .bg = -1, .shrink = 1 };
		if (c->type == NODE_TEXT) {
			if (is_ws(c->text))
				continue;
			IState st = { 0 };
			text_intrinsic(&st, c->text, n->style);
			it.mn = st.mn;
			it.mx = maxf(st.mx, st.line);
			it.basis = it.mx;
			vec_push(&v, it);
			continue;
		}
		u8 d = disp(c);
		if (d == D_NONE)
			continue;
		if (d == D_CONTENTS) {
			FlexItem *sub;
			int k = flex_items(L, c, cw, &sub);
			for (int i = 0; i < k; i++)
				vec_push(&v, sub[i]);
			mem_free(sub);
			continue;
		}
		Style *s = c->style;
		Edges e;
		edges(s, cw, &e);
		memcpy(it.m, e.m, sizeof it.m);
		it.auto_l = e.auto_l, it.auto_r = e.auto_r;
		f32 mn, mx;
		intrinsic(L, c, &mn, &mx);
		f32 marg = e.m[1] + e.m[3];
		it.mn = maxf(0, mn - marg);
		it.mx = maxf(0, mx - marg);
		it.grow = s->flex_grow;
		it.shrink = s->flex_shrink;
		f32 basis = lpx(s->flex_basis, cw, -1);
		if (basis >= 0 && !s->border_box && s->flex_basis.unit != UNIT_AUTO && !(basis == 0 && s->flex_basis.unit == UNIT_PX))
			basis += edge_h(&e);
		if (basis < 0) {
			f32 w = lpx(s->width, cw, -1);
			if (w >= 0)
				basis = s->border_box ? w : w + edge_h(&e);
			else
				basis = it.mx;
		}
		it.basis = basis;
		f32 minw = lpx(s->min_width, cw, -1);
		if (s->overflow_hidden)
			it.mn = 0;
		if (minw >= 0)
			it.mn = s->border_box ? minw : minw + edge_h(&e);
		vec_push(&v, it);
	}
	/* order: a stable sort keeps document order among equals */
	for (usize i = 1; i < v.len; i++)
		for (usize j = i; j > 0 && item_order(&v.data[j - 1]) > item_order(&v.data[j]); j--) {
			FlexItem t = v.data[j];
			v.data[j] = v.data[j - 1];
			v.data[j - 1] = t;
		}
	*out = v.data;
	return (int)v.len;
}

static f32 place_item(Layout *L, Node *parent, FlexItem *it, f32 x, f32 y, f32 w, f32 cw)
{
	it->i0 = L->p->nitems;
	it->x = x;
	if (it->n->type == NODE_TEXT) {
		it->h = layout_anon_text(L, it->n, parent->style, x, y, w);
		it->i1 = L->p->nitems;
		return it->h;
	}
	f32 mb = 0;
	Rect br;
	it->h = layout_box(L, it->n, x - it->m[3], y + it->m[0], cw, LB_BFC, w, &mb, &it->bg, &br);
	it->i1 = L->p->nitems;
	return it->h;
}

static f32 layout_flex_row(Layout *L, Node *n, f32 cx, f32 cy, f32 cw)
{
	Style *s = n->style;
	FlexItem *items;
	int count = flex_items(L, n, cw, &items);
	f32 gap = s->col_gap, rgap = s->row_gap;
	f32 y = cy;
	int i = 0;
	while (i < count) {
		/* One line's worth. */
		int j = i;
		f32 sum = 0;
		while (j < count) {
			FlexItem *it = &items[j];
			f32 outer = maxf(it->basis, it->mn) + it->m[1] + it->m[3] + (j > i ? gap : 0);
			if (s->flex_wrap && j > i && sum + outer > cw + 0.5f)
				break;
			sum += outer;
			j++;
		}
		/* Resolve flexible lengths: grow or shrink, freezing items that hit
		 * their limits and sharing the rest among the others. */
		f32 gaps = gap * (f32)(j - i - 1);
		bool frozen[256] = { 0 };
		int nk0 = MIN(j - i, 256);
		for (int k = 0; k < nk0; k++) {
			FlexItem *it = &items[i + k];
			it->w = maxf(it->basis, it->mn);
		}
		f32 used = gaps;
		for (int k = 0; k < nk0; k++)
			used += items[i + k].w + items[i + k].m[1] + items[i + k].m[3];
		bool growing = used < cw;
		for (int k = 0; k < nk0; k++) {
			FlexItem *it = &items[i + k];
			if ((growing && it->grow <= 0) || (!growing && it->shrink <= 0))
				frozen[k] = true;
		}
		for (int iter = 0; iter < 10; iter++) {
			f32 space = cw - gaps, factor = 0;
			for (int k = 0; k < nk0; k++) {
				FlexItem *it = &items[i + k];
				space -= it->m[1] + it->m[3] + (frozen[k] ? it->w : it->basis);
				if (!frozen[k])
					factor += growing ? it->grow : it->shrink * it->basis;
			}
			if (factor <= 0)
				break;
			if (growing && factor < 1)
				space *= factor;   /* grow factors below 1 take only part */
			bool changed = false;
			for (int k = 0; k < nk0; k++) {
				FlexItem *it = &items[i + k];
				if (frozen[k])
					continue;
				f32 share = growing ? it->grow / factor : it->shrink * it->basis / factor;
				f32 w = it->basis + space * share;
				if (!growing && w < it->mn) {
					w = it->mn;
					frozen[k] = true;
					changed = true;
				}
				it->w = maxf(0, w);
			}
			if (!changed)
				break;
		}
		f32 free = cw - gaps;
		for (int k = i; k < j; k++)
			free -= items[k].w + items[k].m[1] + items[k].m[3];
		if (free < 0)
			free = 0;
		/* Auto margins take the free space first, then justify-content. */
		int autos = 0;
		for (int k = i; k < j; k++)
			autos += items[k].auto_l + items[k].auto_r;
		f32 x = cx, between = gap;
		if (autos && free > 0) {
			f32 per = free / (f32)autos;
			for (int k = i; k < j; k++) {
				if (items[k].auto_l)
					items[k].m[3] += per;
				if (items[k].auto_r)
					items[k].m[1] += per;
			}
			free = 0;
		}
		int nk = j - i;
		switch (s->justify) {
		case JC_CENTER: x += free / 2; break;
		case JC_END: x += free; break;
		case JC_BETWEEN: if (nk > 1) between += free / (f32)(nk - 1); break;
		case JC_AROUND: between += free / (f32)nk; x += free / (f32)nk / 2; break;
		}
		if (L->debug) {
			eprint("flex %s.%.40s cw %.0f free %.0f\n", n->tag, node_attr(n, "class") ? node_attr(n, "class") : "", (double)cw, (double)free);
			for (int k = i; k < j; k++)
				eprint("   %s.%.30s basis %.0f mn %.0f mx %.0f grow %.1f shrink %.1f w %.0f\n", items[k].n->tag,
				       items[k].n->type == NODE_ELEMENT && node_attr(items[k].n, "class") ? node_attr(items[k].n, "class") : "",
				       (double)items[k].basis, (double)items[k].mn, (double)items[k].mx, (double)items[k].grow, (double)items[k].shrink, (double)items[k].w);
		}
		f32 line_h = 0;
		for (int k = i; k < j; k++) {
			FlexItem *it = &items[k];
			x += it->m[3];
			place_item(L, n, it, x, y, it->w, cw);
			x += it->w + it->m[1] + between;
			line_h = maxf(line_h, it->h + it->m[0] + it->m[2]);
		}
		/* Cross-axis alignment. */
		for (int k = i; k < j; k++) {
			FlexItem *it = &items[k];
			u8 al = s->align_items;
			if (it->n->type == NODE_ELEMENT && it->n->style->align_self)
				al = it->n->style->align_self - 1;
			f32 room = line_h - (it->h + it->m[0] + it->m[2]);
			if (room <= 0.5f)
				continue;
			if (al == AI_STRETCH && it->n->type == NODE_ELEMENT && it->n->style->height.unit == UNIT_AUTO) {
				if (it->bg >= 0)
					L->p->items[it->bg].r.h += room;
			} else if (al == AI_CENTER) {
				shift_items(L->p, it->i0, it->i1, 0, room / 2);
			} else if (al == AI_END) {
				shift_items(L->p, it->i0, it->i1, 0, room);
			}
		}
		y += line_h + (j < count ? rgap : 0);
		i = j;
	}
	mem_free(items);
	return y - cy;
}

static f32 layout_flex_column(Layout *L, Node *n, f32 cx, f32 cy, f32 cw)
{
	Style *s = n->style;
	FlexItem *items;
	int count = flex_items(L, n, cw, &items);
	f32 y = cy;
	for (int i = 0; i < count; i++) {
		FlexItem *it = &items[i];
		u8 al = s->align_items;
		if (it->n->type == NODE_ELEMENT && it->n->style->align_self)
			al = it->n->style->align_self - 1;
		f32 avail = cw - it->m[1] - it->m[3];
		f32 w;
		bool spec = it->n->type == NODE_ELEMENT && lpx(it->n->style->width, cw, -1) >= 0;
		if (al == AI_STRETCH && !spec)
			w = avail;
		else
			w = minf(maxf(it->mn, minf(it->mx, avail)), maxf(avail, it->mn));
		if (spec) {
			Style *is = it->n->style;
			Edges e;
			edges(is, cw, &e);
			w = lpx(is->width, cw, avail);
			if (!is->border_box)
				w += edge_h(&e);
		}
		f32 x = cx + it->m[3];
		if (it->auto_l && it->auto_r)
			x = cx + (cw - w) / 2;
		else if (al == AI_CENTER)
			x = cx + (cw - w) / 2;
		else if (al == AI_END)
			x = cx + cw - w - it->m[1];
		if (i)
			y += s->row_gap;
		place_item(L, n, it, x, y, w, cw);
		y += it->m[0] + it->h + it->m[2];
	}
	mem_free(items);
	return y - cy;
}

/* grid-template-columns: fixed tracks, fr tracks, content-sized tracks and repeat(). */
enum { TK_FIXED, TK_FR, TK_AUTO, TK_MIN, TK_RANGE /* minmax(a, b): b if there's room, down to a */ };

static f32 track_len(const char *v, f32 cw, f32 em)
{
	const char *e;
	f32 x = (f32)str_to_float(v, &e);
	if (str_starts(e, "%"))
		return cw * x / 100;
	if (str_starts(e, "em") || str_starts(e, "rem"))
		return x * em;
	return x;
}

static int grid_tracks(const char *spec, f32 cw, f32 gap, f32 em, f32 *size, u8 *kind, f32 *lo, int max)
{
	int n = 0;
	const char *p = spec;
	while (*p && n < max) {
		while (*p && is_space(*p))
			p++;
		if (!*p)
			break;
		if (*p == '[') {   /* line names */
			while (*p && *p != ']')
				p++;
			if (*p)
				p++;
			continue;
		}
		const char *s = p;
		int depth = 0;
		while (*p && (depth || !is_space(*p))) {
			if (*p == '(')
				depth++;
			else if (*p == ')')
				depth--;
			p++;
		}
		char tok[256];
		str_copy(tok, s, MIN(sizeof tok, (usize)(p - s) + 1));
		lo[n] = 0;
		if (str_starts(tok, "repeat(")) {
			char *inner = tok + 7;
			char *comma = (char *)str_chr(inner, ',');
			if (!comma)
				continue;
			*comma = 0;
			char *body = comma + 1;
			char *close = (char *)str_rchr(body, ')');
			if (close)
				*close = 0;
			int count;
			f32 sub_size[16], sub_lo[16];
			u8 sub_kind[16];
			int k = grid_tracks(body, cw, gap, em, sub_size, sub_kind, sub_lo, 16);
			if (!k)
				continue;
			if (str_starts(inner, "auto-fill") || str_starts(inner, "auto-fit")) {
				f32 track = 0;
				for (int i = 0; i < k; i++)
					track += maxf(sub_size[i], sub_kind[i] == TK_FIXED ? 0 : 1);
				track = maxf(track, 40);
				count = MAX(1, (int)((cw + gap) / (track + gap * (f32)k)));
			} else {
				count = (int)str_to_int(inner, NULL);
			}
			for (int c = 0; c < count && n < max; c++)
				for (int i = 0; i < k && n < max; i++) {
					size[n] = sub_size[i];
					lo[n] = sub_lo[i];
					kind[n++] = sub_kind[i];
				}
			continue;
		}
		if (str_starts(tok, "minmax(")) {
			char *inner = tok + 7;
			char *comma = (char *)str_chr(inner, ',');
			if (comma) {
				*comma = 0;
				char *mx = comma + 1;
				while (is_space(*mx))
					mx++;
				char *close = (char *)str_rchr(mx, ')');
				if (close)
					*close = 0;
				f32 mnv = is_digit(*inner) ? track_len(inner, cw, em) : 0;
				lo[n] = mnv;
				if (str_find(mx, "fr")) {
					size[n] = mnv;
					kind[n++] = TK_FR;
				} else if (str_starts(mx, "auto") || str_starts(mx, "max-content") || str_starts(mx, "min-content")) {
					size[n] = mnv;
					kind[n++] = TK_AUTO;
				} else {
					size[n] = maxf(mnv, track_len(mx, cw, em));
					kind[n++] = TK_RANGE;
				}
			}
			continue;
		}
		if (str_starts(tok, "fit-content(")) {
			size[n] = 0;
			kind[n++] = TK_AUTO;
			continue;
		}
		const char *e;
		f32 v = (f32)str_to_float(tok, &e);
		if (str_starts(e, "fr")) {
			size[n] = 0;
			kind[n++] = TK_FR;
		} else if (e != tok && (str_starts(e, "px") || str_starts(e, "%") || str_starts(e, "em") || str_starts(e, "rem") || !*e)) {
			size[n] = track_len(tok, cw, em);
			kind[n++] = TK_FIXED;
			UNUSED(v);
		} else if (str_starts(tok, "min-content")) {
			size[n] = 0;
			kind[n++] = TK_MIN;
		} else {
			size[n] = 0;   /* auto, max-content */
			kind[n++] = TK_AUTO;
		}
	}
	return n;
}

typedef struct {
	char name[48];
	int r0, c0, r1, c1;      /* [r0, r1) x [c0, c1), zero-based */
} Area;

/* grid-template-areas: "head head" "side main" */
static int parse_areas(const char *spec, Area *areas, int max, int *rows, int *cols)
{
	int n = 0, row = 0;
	*cols = 0;
	for (const char *p = spec; *p; p++) {
		if (*p != '"' && *p != '\'')
			continue;
		char q = *p++;
		int col = 0;
		while (*p && *p != q) {
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == q || !*p)
				break;
			char name[48];
			usize k = 0;
			while (*p && *p != q && *p != ' ' && *p != '\t') {
				if (k < sizeof name - 1)
					name[k++] = *p;
				p++;
			}
			name[k] = 0;
			if (name[0] != '.') {
				int i;
				for (i = 0; i < n; i++)
					if (str_eq(areas[i].name, name))
						break;
				if (i == n && n < max) {
					str_copy(areas[n].name, name, sizeof areas[n].name);
					areas[n].r0 = row, areas[n].c0 = col, areas[n].r1 = row + 1, areas[n].c1 = col + 1;
					n++;
				} else if (i < n) {
					areas[i].r1 = MAX(areas[i].r1, row + 1);
					areas[i].c1 = MAX(areas[i].c1, col + 1);
					areas[i].r0 = MIN(areas[i].r0, row);
					areas[i].c0 = MIN(areas[i].c0, col);
				}
			}
			col++;
		}
		*cols = MAX(*cols, col);
		row++;
		if (!*p)
			break;
	}
	*rows = row;
	return n;
}

/* "2", "1 / 3", "1 / -1", "span 2", "2 / span 3": start and end lines, 1-based (0 = auto). */
static void parse_lines(const char *v, int nlines, int *start, int *end, int *span)
{
	*start = *end = 0;
	*span = 1;
	char a[64] = "", b[64] = "";
	const char *slash = str_chr(v, '/');
	if (slash) {
		str_copy(a, v, MIN(sizeof a, (usize)(slash - v) + 1));
		str_copy(b, slash + 1, sizeof b);
	} else {
		str_copy(a, v, sizeof a);
	}
	char *pa = str_trim(a), *pb = str_trim(b);
	if (str_starts(pa, "span")) {
		*span = MAX(1, (int)str_to_int(pa + 4, NULL));
	} else if (is_digit(*pa) || *pa == '-') {
		int x = (int)str_to_int(pa, NULL);
		*start = x < 0 ? nlines + 1 + x : x;
	}
	if (str_starts(pb, "span")) {
		*span = MAX(1, (int)str_to_int(pb + 4, NULL));
	} else if (is_digit(*pb) || *pb == '-') {
		int x = (int)str_to_int(pb, NULL);
		*end = x < 0 ? nlines + 1 + x : x;
		if (*start && *end > *start)
			*span = *end - *start;
		else if (!*start && *end > 1)
			*start = *end - *span;
	}
}

static f32 layout_grid(Layout *L, Node *n, f32 cx, f32 cy, f32 cw)
{
	Style *s = n->style;
	FlexItem *items;
	int count = flex_items(L, n, cw, &items);
	f32 size[64], lo[64];
	u8 kind[64];
	int ncol = s->grid_cols ? grid_tracks(s->grid_cols, cw, s->col_gap, s->font_size, size, kind, lo, 64) : 0;
	Area areas[32];
	int arows = 0, acols = 0;
	int nareas = s->grid_areas ? parse_areas(s->grid_areas, areas, 32, &arows, &acols) : 0;
	for (int i = ncol; i < acols && i < 64; i++)
		size[i] = 0, lo[i] = 0, kind[i] = TK_FR;
	ncol = MAX(ncol, MIN(acols, 64));
	if (ncol <= 0) {
		ncol = 1;
		size[0] = 0, kind[0] = TK_FR;
	}
	/* Place every item: named areas, explicit lines, then the free cells in order. */
	typedef struct { int r0, c0, r1, c1; } Span;
	Span *sp = NEW_ARRAY(Span, MAX(count, 1));
	enum { MAXR = 256 };
	u64 busy[MAXR];
	memset(busy, 0, sizeof busy);
	int maxrow = arows;
	for (int pass = 0; pass < 2; pass++) {
		int cur_r = 0, cur_c = 0;
		for (int i = 0; i < count; i++) {
			Style *is = items[i].n->type == NODE_ELEMENT ? items[i].n->style : NULL;
			bool explicit_pos = false;
			Span p = { 0, 0, 1, 1 };
			if (is && is->grid_area && !is_digit(is->grid_area[0])) {
				for (int k = 0; k < nareas; k++)
					if (str_eq(areas[k].name, is->grid_area)) {
						p = (Span){ areas[k].r0, areas[k].c0, areas[k].r1, areas[k].c1 };
						explicit_pos = true;
					}
				if (!explicit_pos) {
					/* an area that isn't in the template: hide it */
					p = (Span){ -1, -1, -1, -1 };
					explicit_pos = true;
				}
			} else if (is && (is->grid_col || is->grid_row)) {
				int cs = 0, ce = 0, cspan = 1, rs = 0, re = 0, rspan = 1;
				if (is->grid_col)
					parse_lines(is->grid_col, ncol + 1, &cs, &ce, &cspan);
				if (is->grid_row)
					parse_lines(is->grid_row, MAX(arows, 1) + 1, &rs, &re, &rspan);
				cspan = MIN(cspan, ncol);
				if (cs >= 1 && rs >= 1) {
					p = (Span){ rs - 1, MIN(cs - 1, ncol - 1), rs - 1 + rspan, MIN(cs - 1 + cspan, ncol) };
					explicit_pos = true;
				} else if (cs >= 1) {
					/* fixed column, next free row */
					int c0 = MIN(cs - 1, ncol - 1), c1 = MIN(c0 + cspan, ncol);
					int r = 0;
					u64 mask = ((c1 - c0) >= 64 ? ~0ull : ((1ull << (c1 - c0)) - 1)) << c0;
					while (r < MAXR - 1 && (busy[r] & mask))
						r++;
					p = (Span){ r, c0, r + rspan, c1 };
					explicit_pos = true;
				} else {
					p = (Span){ 0, 0, rspan, cspan };   /* auto with a span */
				}
			}
			if (pass == 0) {
				if (explicit_pos) {
					sp[i] = p;
					if (p.r0 >= 0)
						for (int r = p.r0; r < p.r1 && r < MAXR; r++)
							for (int c = p.c0; c < p.c1 && c < 64; c++)
								busy[r] |= 1ull << c;
				} else {
					sp[i] = (Span){ -2, p.c1 - p.c0, 0, p.r1 - p.r0 };   /* placed in pass 1 */
				}
				continue;
			}
			if (sp[i].r0 != -2)
				continue;
			int cspan = MAX(1, MIN(sp[i].c0, ncol)), rspan = MAX(1, sp[i].r1);
			for (;;) {
				if (cur_c + cspan > ncol) {
					cur_c = 0;
					cur_r++;
				}
				if (cur_r >= MAXR - rspan)
					break;
				bool free_cell = true;
				for (int r = cur_r; r < cur_r + rspan && free_cell; r++)
					for (int c = cur_c; c < cur_c + cspan; c++)
						if (busy[r] & (1ull << c))
							free_cell = false;
				if (free_cell)
					break;
				cur_c++;
			}
			sp[i] = (Span){ cur_r, cur_c, cur_r + rspan, cur_c + cspan };
			for (int r = sp[i].r0; r < sp[i].r1 && r < MAXR; r++)
				for (int c = sp[i].c0; c < sp[i].c1; c++)
					busy[r] |= 1ull << c;
			cur_c += cspan;
		}
	}
	for (int i = 0; i < count; i++)
		maxrow = MAX(maxrow, sp[i].r1);
	/* Column sizes: fixed tracks; content-sized tracks from the items in
	 * them; then fr shares of what's left. */
	for (int i = 0; i < count; i++) {
		if (sp[i].r0 < 0 || sp[i].c1 - sp[i].c0 != 1 || sp[i].c0 >= ncol)
			continue;
		int c = sp[i].c0;
		if (kind[c] == TK_AUTO)
			size[c] = maxf(size[c], items[i].mx + items[i].m[1] + items[i].m[3]);
		else if (kind[c] == TK_MIN)
			size[c] = maxf(size[c], items[i].mn + items[i].m[1] + items[i].m[3]);
	}
	f32 fixed = s->col_gap * (f32)(ncol - 1);
	f32 frs = 0, autos = 0;
	for (int i = 0; i < ncol; i++) {
		if (kind[i] == TK_FR)
			frs += 1;
		else
			fixed += size[i];
		if (kind[i] == TK_AUTO)
			autos += size[i];
	}
	/* minmax(a, b) tracks give way first, then content-sized ones */
	for (int i = 0; i < ncol && fixed > cw; i++) {
		if (kind[i] != TK_RANGE)
			continue;
		f32 cut = minf(fixed - cw, size[i] - lo[i]);
		size[i] -= cut;
		fixed -= cut;
	}
	if (fixed > cw && autos > 0) {
		f32 k = maxf(0, 1 - (fixed - cw) / autos);
		for (int i = 0; i < ncol; i++)
			if (kind[i] == TK_AUTO) {
				fixed -= size[i] * (1 - k);
				size[i] *= k;
			}
	}
	f32 frw = frs > 0 ? maxf(0, (cw - fixed) / frs) : 0;
	f32 xs[65];
	f32 x = cx;
	for (int i = 0; i < ncol; i++) {
		xs[i] = x;
		size[i] = kind[i] == TK_FR ? maxf(frw, size[i]) : size[i];
		x += size[i] + s->col_gap;
	}
	xs[ncol] = x;
	if (L->debug) {
		eprint("grid %s.%s cw %.0f cols '%s' areas '%s':", n->tag, node_attr(n, "class") ? node_attr(n, "class") : "", (double)cw,
		       s->grid_cols ? s->grid_cols : "", s->grid_areas ? s->grid_areas : "");
		for (int i = 0; i < ncol; i++)
			eprint(" %d:%.0f", kind[i], (double)size[i]);
		eprint("\n");
		for (int i = 0; i < count; i++)
			eprint("   item %s.%.30s r%d-%d c%d-%d\n", items[i].n->tag, items[i].n->type == NODE_ELEMENT && node_attr(items[i].n, "class") ? node_attr(items[i].n, "class") : "",
			       sp[i].r0, sp[i].r1, sp[i].c0, sp[i].c1);
	}
	/* Lay out at the top, then move rows into place. */
	int nrows = MAX(1, MIN(maxrow, MAXR));
	f32 *rowh = NEW_ARRAY(f32, nrows + 1);
	for (int i = 0; i < count; i++) {
		FlexItem *it = &items[i];
		if (sp[i].r0 < 0) {
			it->i0 = it->i1 = L->p->nitems;
			it->h = 0;
			continue;
		}
		int c0 = MIN(sp[i].c0, ncol - 1), c1 = MAX(c0 + 1, MIN(sp[i].c1, ncol));
		f32 w = xs[c1] - s->col_gap - xs[c0] - it->m[1] - it->m[3];
		place_item(L, n, it, xs[c0] + it->m[3], cy, maxf(0, w), xs[c1] - xs[c0]);
		if (sp[i].r1 - sp[i].r0 == 1)
			rowh[sp[i].r0] = maxf(rowh[sp[i].r0], it->h + it->m[0] + it->m[2]);
	}
	for (int i = 0; i < count; i++) {
		if (sp[i].r0 < 0 || sp[i].r1 - sp[i].r0 <= 1)
			continue;
		f32 have = s->row_gap * (f32)(sp[i].r1 - sp[i].r0 - 1);
		for (int r = sp[i].r0; r < sp[i].r1 && r < nrows; r++)
			have += rowh[r];
		f32 need = items[i].h + items[i].m[0] + items[i].m[2];
		if (need > have)
			rowh[MIN(sp[i].r1, nrows) - 1] += need - have;
	}
	f32 *rowy = NEW_ARRAY(f32, nrows + 1);
	f32 y = cy;
	for (int r = 0; r < nrows; r++) {
		rowy[r] = y;
		y += rowh[r] + (rowh[r] > 0 && r + 1 < nrows ? s->row_gap : 0);
	}
	rowy[nrows] = y;
	for (int i = 0; i < count; i++) {
		FlexItem *it = &items[i];
		if (sp[i].r0 < 0)
			continue;
		int r0 = MIN(sp[i].r0, nrows - 1), r1 = MIN(sp[i].r1, nrows);
		f32 top = rowy[r0];
		f32 span_h = rowy[r1] - top - (r1 < nrows && rowh[r1 - 1] > 0 ? s->row_gap : 0);
		f32 room = span_h - (it->h + it->m[0] + it->m[2]);
		f32 dy = top - cy;
		if (room > 0.5f && s->align_items == AI_CENTER)
			dy += room / 2;
		else if (room > 0.5f && s->align_items == AI_END)
			dy += room;
		if (dy != 0)
			shift_items(L->p, it->i0, it->i1, 0, dy);
		if (room > 0.5f && it->bg >= 0 && s->align_items == AI_STRETCH)
			L->p->items[it->bg].r.h += room;
	}
	mem_free(rowh);
	mem_free(rowy);
	mem_free(sp);
	mem_free(items);
	return y - cy;
}

/* ---- Boxes ---------------------------------------------------------------------------------------------- */

static void drop_overflow(Page *p, int from, f32 bottom)
{
	int w = from;
	for (int i = from; i < p->nitems; i++)
		if (p->items[i].r.y < bottom - 1)
			p->items[w++] = p->items[i];
	p->nitems = w;
}

static f32 layout_box(Layout *L, Node *n, f32 cx, f32 y, f32 cw, u32 flags, f32 forced_w, f32 *mb_out, int *bg_out, Rect *border_out)
{
	Style *s = n->style;
	u8 d = disp(n);
	Edges e;
	edges(s, cw, &e);
	f32 eh = edge_h(&e), ev = edge_v(&e);
	bool replaced = is_replaced(n);
	f32 rw = 0, rh = 0;
	if (replaced)
		replaced_size(L, n, cw, &rw, &rh);
	f32 content_w;
	f32 ml = e.m[3], mr = e.m[1];
	if (forced_w >= 0) {
		content_w = maxf(0, forced_w - eh);
		if (replaced && lpx(s->width, cw, -1) < 0 && forced_w < rw + eh)
			rh = rw > 0 ? rh * content_w / rw : rh;
	} else {
		f32 spec = lpx(s->width, cw, -1);
		if (replaced && spec < 0)
			content_w = rw;
		else if (spec >= 0)
			content_w = s->border_box ? spec - eh : spec;
		else if (d == D_TABLE) {
			f32 mn, mx;
			intrinsic(L, n, &mn, &mx);
			f32 avail = cw - ml - mr;
			content_w = minf(maxf(mn, avail), mx) - eh;
		} else {
			content_w = cw - ml - mr - eh;
		}
		f32 maxw = lpx(s->max_width, cw, -1);
		if (maxw >= 0) {
			if (s->border_box)
				maxw -= eh;
			if (content_w > maxw) {
				if (replaced && rw > 0 && lpx(s->height, -1, -1) < 0)
					rh = rh * maxw / rw;
				content_w = maxw;
			}
		}
		f32 minw = lpx(s->min_width, cw, -1);
		if (minw >= 0) {
			if (s->border_box)
				minw -= eh;
			content_w = maxf(content_w, minw);
		}
		if (d == D_TABLE) {
			f32 mn, mx;
			intrinsic(L, n, &mn, &mx);
			content_w = maxf(content_w, mn - ml - mr - eh);
		}
		/* Auto margins center or push a box narrower than its container. */
		f32 free = cw - (content_w + eh + (e.auto_l ? 0 : ml) + (e.auto_r ? 0 : mr));
		if (e.auto_l && e.auto_r) {
			ml = mr = maxf(0, free / 2);
		} else if (e.auto_l) {
			ml = maxf(0, free);
		} else if (free > 0 && n->parent && n->parent->style && n->parent->style->text_align == TA_CENTER_BLOCKS && !s->float_side &&
			   s->margin[3].v == 0 && s->margin[1].v == 0) {
			ml = free / 2;   /* <center> and align=center center their blocks */
		}
	}
	content_w = maxf(0, content_w);
	f32 bx = cx + ml, by = y;
	int bg = -1;
	bool masked = s->mask_image && s->visible;
	bool paint_bg = !masked && s->visible && n != L->canvas_node && !is_tag(n, "html") &&
			(COLOR_A(s->background) || e.b[0] + e.b[1] + e.b[2] + e.b[3] > 0);
	if (paint_bg)
		bg = item_new(L, ITEM_BOX);
	int first_item = L->p->nitems;
	Node *saved_link = L->link;
	if ((is_tag(n, "a") && node_attr(n, "href")) || is_tag(n, "button"))
		L->link = n;
	f32 saved_op = L->opacity;
	L->opacity *= s->opacity;
	anchor(L, n, by);
	bool bfc = (flags & LB_BFC) || makes_bfc(n) || !n->parent || is_tag(n, "html");
	FloatCtx fc = { 0 }, *saved_fc = L->fc;
	if (bfc)
		L->fc = &fc;
	f32 ix = bx + e.b[3] + e.p[3], iy = by + e.b[0] + e.p[0];
	f32 h = 0, trail = 0;
	Node *saved_marker = L->marker;
	if (replaced) {
		h = rh;
		emit_replaced(L, n, rect(ix, iy, content_w, rh), rect(bx + e.b[3], by + e.b[0], content_w + e.p[1] + e.p[3], rh + e.p[0] + e.p[2]));
	} else if (d == D_TABLE) {
		h = layout_table(L, n, ix, iy, content_w);
	} else if (d == D_FLEX || d == D_INLINE_FLEX) {
		h = s->flex_column ? layout_flex_column(L, n, ix, iy, content_w) : layout_flex_row(L, n, ix, iy, content_w);
	} else if (d == D_GRID || d == D_INLINE_GRID) {
		h = layout_grid(L, n, ix, iy, content_w);
	} else {
		if (d == D_LIST_ITEM) {
			L->marker = n;
			L->marker_x = ix;
		}
		bool absorb = (flags & LB_ABSORB) && !has_top_edge(s, cw) && !bfc;
		h = layout_flow(L, n, ix, iy, content_w, absorb, &trail);
		if (d == D_LIST_ITEM && L->marker == n)
			emit_marker(L, iy + font_ascent(style_font(s)), ix);
		L->marker = d == D_LIST_ITEM ? saved_marker : L->marker;
	}
	bool bottom_edge = e.b[2] > 0 || e.p[2] > 0;
	bool auto_h = s->height.unit == UNIT_AUTO || (s->height.unit == UNIT_PCT && !replaced);
	if (!bottom_edge && auto_h && !bfc) {
		*mb_out = collapse(e.m[2], trail);
	} else {
		h += trail;
		*mb_out = e.m[2];
	}
	if (bfc) {
		vec_each(&fc.f, f) h = maxf(h, f->y1 - iy);
		vec_free(&fc.f);
		L->fc = saved_fc;
	}
	if (!replaced) {
		f32 spec_h = lpx(s->height, -1, -1);
		if (s->height.unit == UNIT_PCT && s->height.v >= 100 && (is_tag(n, "html") || is_tag(n, "body")))
			spec_h = -1;
		if (spec_h >= 0) {
			if (s->border_box)
				spec_h = maxf(0, spec_h - ev);
			if (s->overflow_hidden && spec_h < h)
				drop_overflow(L->p, first_item, iy + spec_h);
			h = spec_h;
		}
		f32 maxh = lpx(s->max_height, -1, -1);
		if (maxh >= 0) {
			if (s->border_box)
				maxh = maxf(0, maxh - ev);
			if (h > maxh) {
				if (s->overflow_hidden)
					drop_overflow(L->p, first_item, iy + maxh);
				h = maxh;
			}
		}
		f32 minh = lpx(s->min_height, -1, -1);
		if (s->min_height.unit == UNIT_PCT && s->min_height.v >= 100 && (is_tag(n, "html") || is_tag(n, "body")))
			minh = -1;
		if (minh >= 0) {
			if (s->border_box)
				minh = maxf(0, minh - ev);
			h = maxf(h, minh);
		}
	}
	f32 bw = content_w + eh, bh = h + ev;
	if (bg >= 0) {
		Item *it = &L->p->items[bg];
		it->link = L->link;
		it->r = rect(bx, by, bw, bh);
		it->background = fade(L, s->background);
		for (int i = 0; i < 4; i++) {
			it->border_w[i] = e.b[i];
			it->border_c[i] = fade(L, s->border_c[i]);
		}
		it->radius = minf(s->radius, minf(bw, bh) / 2);
		it->node = n;
	}
	if (L->debug && depth_of(n) <= L->debug) {
		const char *cls = node_attr(n, "class");
		eprint("%*s%s.%.50s  x%.0f y%.0f w%.0f h%.0f  cw%.0f d%d\n", depth_of(n), "", n->tag, cls ? cls : "", (double)bx, (double)by,
		       (double)bw, (double)bh, (double)cw, d);
	}
	if (masked && bw >= 1 && bh >= 1) {
		/* An icon: the picture's shape filled with the background color. */
		char *abs = url_resolve(L->p->base, s->mask_image);
		Image *im = doc_image(L->doc, abs);
		mem_free(abs);
		int mi = item_new(L, ITEM_IMAGE);
		Item *it = &L->p->items[mi];
		it->r = rect(bx, by, bw, bh);
		it->image = im;
		it->flags = IF_MASK;
		it->color = fade(L, COLOR_A(s->background) ? s->background : s->color);
		it->node = n;
	}
	if (bg_out)
		*bg_out = bg;
	if (border_out)
		*border_out = rect(bx, by, bw, bh);
	L->opacity = saved_op;
	L->link = saved_link;
	return bh;
}

/* ---- Pages ---------------------------------------------------------------------------------------------- */

void page_free(Page *p)
{
	if (!p)
		return;
	mem_free(p->items);
	mem_free(p->text);
	map_free(&p->anchors, NULL);
	mem_free(p->base);
	mem_free(p);
}

f32 page_anchor(const Page *p, const char *id)
{
	uintptr_t v = (uintptr_t)map_get(&p->anchors, id);
	return v ? (f32)(v - 1) : -1;
}

Page *layout_doc(Doc *d, f32 width, f32 vh)
{
	Page *p = NEW(Page);
	p->base = str_dup(d->base ? d->base : d->url);
	Layout L = { .p = p, .doc = d, .vw = width, .vh = vh, .opacity = 1 };
	const char *dbg = env_get("DRIFT_DEBUG_BOXES");
	L.debug = dbg ? (int)str_to_int(dbg, NULL) : 0;
	FloatCtx root = { 0 };
	L.fc = &root;
	mark_blocks(d->root);
	/* The page background comes from html, or body if html has none. */
	Node *html = node_find(d->root, "html");
	Node *body = node_find(d->root, "body");
	p->background = COLOR_WHITE;
	if (html && html->style && COLOR_A(html->style->background)) {
		p->background = html->style->background;
		L.canvas_node = html;
	} else if (body && body->style && COLOR_A(body->style->background)) {
		p->background = body->style->background;
		L.canvas_node = body;
	}
	if (COLOR_A(p->background) < 255)
		p->background = color_mix(COLOR_WHITE, color_with_alpha(p->background, 255), COLOR_A(p->background) / 255.0f);
	f32 trail = 0;
	f32 h = layout_flow(&L, d->root, 0, 0, width, false, &trail);
	vec_each(&root.f, f) h = maxf(h, f->y1);
	vec_free(&root.f);
	p->width = width;
	p->height = h + maxf(0, trail);
	mem_free(L.ic);
	return p;
}
