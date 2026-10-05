/* SVG subset renderer: a small XML reader, path-data parser and painter. */
#include "zg.h"
#include "zg/svg.h"

/* ---- XML tree ----------------------------------------------------------------- */

typedef struct Node Node;
struct Node {
	char *name;
	VEC(char *) keys;
	VEC(char *) vals;
	VEC(Node *) kids;
};

struct SvgDoc {
	Node *root;
	f32 vb_x, vb_y, vb_w, vb_h;
	f32 width, height;
	Map ids; /* id -> Node* */
};

static const char *attr(const Node *n, const char *key)
{
	for (usize i = 0; i < n->keys.len; i++)
		if (str_eq(n->keys.data[i], key))
			return n->vals.data[i];
	return NULL;
}

static void free_node(Node *n)
{
	if (!n)
		return;
	mem_free(n->name);
	for (usize i = 0; i < n->keys.len; i++) {
		mem_free(n->keys.data[i]);
		mem_free(n->vals.data[i]);
	}
	vec_free(&n->keys);
	vec_free(&n->vals);
	vec_each(&n->kids, k) free_node(*k);
	vec_free(&n->kids);
	mem_free(n);
}

static char *decode_entities(const char *s, usize n)
{
	StrBuf sb = { 0 };
	for (usize i = 0; i < n; i++) {
		if (s[i] == '&') {
			const char *semi = (const char *)memchr(s + i, ';', n - i);
			if (semi) {
				usize len = (usize)(semi - (s + i));
				if (len == 3 && !memcmp(s + i, "&lt", 3)) sb_addc(&sb, '<');
				else if (len == 3 && !memcmp(s + i, "&gt", 3)) sb_addc(&sb, '>');
				else if (len == 4 && !memcmp(s + i, "&amp", 4)) sb_addc(&sb, '&');
				else if (len == 5 && !memcmp(s + i, "&quot", 5)) sb_addc(&sb, '"');
				else if (len == 5 && !memcmp(s + i, "&apos", 5)) sb_addc(&sb, '\'');
				else sb_add(&sb, s + i, len + 1);
				i += len;
				continue;
			}
		}
		sb_addc(&sb, s[i]);
	}
	return sb_take(&sb);
}

static Node *parse_xml(const char *s, usize len)
{
	const char *p = s, *end = s + len;
	Node *root = NULL;
	VEC(Node *) stack = { 0 };
	while (p < end) {
		if (*p != '<') {
			p++;
			continue;
		}
		if (p + 4 <= end && !memcmp(p, "<!--", 4)) {
			const char *e = str_find(p + 4, "-->");
			p = e ? e + 3 : end;
			continue;
		}
		if (p[1] == '?' || p[1] == '!') {
			while (p < end && *p != '>')
				p++;
			p++;
			continue;
		}
		if (p[1] == '/') {
			while (p < end && *p != '>')
				p++;
			p++;
			if (stack.len)
				stack.len--;
			continue;
		}
		p++;
		const char *ns = p;
		while (p < end && !is_space(*p) && *p != '>' && *p != '/')
			p++;
		Node *n = NEW(Node);
		const char *colon = (const char *)memchr(ns, ':', (usize)(p - ns));
		n->name = colon ? str_ndup(colon + 1, (usize)(p - colon - 1)) : str_ndup(ns, (usize)(p - ns));
		bool self_close = false;
		for (;;) {
			while (p < end && is_space(*p))
				p++;
			if (p >= end)
				break;
			if (*p == '/') {
				self_close = true;
				p++;
				continue;
			}
			if (*p == '>') {
				p++;
				break;
			}
			const char *ks = p;
			while (p < end && *p != '=' && !is_space(*p) && *p != '>' && *p != '/')
				p++;
			char *key = str_ndup(ks, (usize)(p - ks));
			while (p < end && is_space(*p))
				p++;
			char *val = str_dup("");
			if (p < end && *p == '=') {
				p++;
				while (p < end && is_space(*p))
					p++;
				if (p < end && (*p == '"' || *p == '\'')) {
					char q = *p++;
					const char *vs = p;
					while (p < end && *p != q)
						p++;
					mem_free(val);
					val = decode_entities(vs, (usize)(p - vs));
					p++;
				}
			}
			vec_push(&n->keys, key);
			vec_push(&n->vals, val);
		}
		if (stack.len)
			vec_push(&vec_last(&stack)->kids, n);
		else if (!root)
			root = n;
		else
			free_node(n);
		if (!self_close)
			vec_push(&stack, n);
	}
	vec_free(&stack);
	return root;
}

/* ---- Values ----------------------------------------------------------------------- */

static const char *skip_sep(const char *s)
{
	while (*s && (is_space(*s) || *s == ','))
		s++;
	return s;
}

static bool parse_num(const char **s, f32 *out)
{
	const char *p = skip_sep(*s), *e;
	f64 v = str_to_float(p, &e);
	if (e == p)
		return false;
	*out = (f32)v;
	*s = e;
	return true;
}

static f32 num_attr(const Node *n, const char *key, f32 fallback, f32 percent_of)
{
	const char *v = attr(n, key);
	if (!v)
		return fallback;
	const char *e;
	f32 x = (f32)str_to_float(v, &e);
	if (e == v)
		return fallback;
	if (*e == '%')
		x = x / 100 * percent_of;
	return x;
}

static const struct {
	const char *name;
	u32 rgb;
} NAMED[] = {
	{ "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 },
	{ "blue", 0x0000ff }, { "yellow", 0xffff00 }, { "orange", 0xffa500 }, { "purple", 0x800080 },
	{ "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xc0c0c0 }, { "navy", 0x000080 },
	{ "teal", 0x008080 }, { "lime", 0x00ff00 }, { "maroon", 0x800000 }, { "pink", 0xffc0cb },
};

typedef enum { PAINT_NONE, PAINT_COLOR, PAINT_CURRENT, PAINT_URL } PaintSpecKind;

typedef struct {
	PaintSpecKind kind;
	Color color;
	char id[64];
} PaintSpec;

static bool parse_color(const char *v, Color *out)
{
	while (is_space(*v))
		v++;
	if (*v == '#') {
		*out = color_hex(v);
		return true;
	}
	if (str_starts(v, "rgb")) {
		const char *p = str_chr(v, '(');
		if (!p)
			return false;
		p++;
		f32 c[4] = { 0, 0, 0, 1 };
		for (int i = 0; i < 4; i++) {
			if (!parse_num(&p, &c[i]))
				break;
			p = skip_sep(p);
			if (*p == '%') {
				c[i] = c[i] * 2.55f;
				p++;
			}
		}
		*out = RGBA((u32)clampf(c[0], 0, 255), (u32)clampf(c[1], 0, 255), (u32)clampf(c[2], 0, 255),
			    (u32)(clampf(c[3], 0, 1) * 255));
		return true;
	}
	for (usize i = 0; i < ARRAY_LEN(NAMED); i++) {
		if (str_eq_nocase(v, NAMED[i].name)) {
			*out = 0xff000000u | NAMED[i].rgb;
			return true;
		}
	}
	return false;
}

static PaintSpec parse_paint(const char *v)
{
	PaintSpec ps = { PAINT_NONE, 0, { 0 } };
	while (is_space(*v))
		v++;
	if (str_eq(v, "none") || str_eq(v, "transparent"))
		return ps;
	if (str_eq(v, "currentColor")) {
		ps.kind = PAINT_CURRENT;
		return ps;
	}
	if (str_starts(v, "url(")) {
		const char *h = str_chr(v, '#');
		if (h) {
			usize n = 0;
			h++;
			while (h[n] && h[n] != ')' && n < sizeof ps.id - 1)
				n++;
			memcpy(ps.id, h, n);
			ps.id[n] = 0;
			ps.kind = PAINT_URL;
		}
		return ps;
	}
	if (parse_color(v, &ps.color))
		ps.kind = PAINT_COLOR;
	return ps;
}

static Mat parse_transform(const char *s)
{
	Mat m = mat_identity();
	while (s && *s) {
		s = skip_sep(s);
		const char *name = s;
		while (is_alpha(*s))
			s++;
		usize nl = (usize)(s - name);
		s = skip_sep(s);
		if (*s != '(')
			break;
		s++;
		f32 a[6] = { 0 };
		int n = 0;
		while (n < 6 && parse_num(&s, &a[n]))
			n++;
		s = str_chr(s, ')');
		if (s)
			s++;
		Mat t = mat_identity();
		if (nl == 6 && !memcmp(name, "matrix", 6) && n == 6)
			t = (Mat){ a[0], a[1], a[2], a[3], a[4], a[5] };
		else if (nl == 9 && !memcmp(name, "translate", 9))
			t = mat_translate(a[0], n > 1 ? a[1] : 0);
		else if (nl == 5 && !memcmp(name, "scale", 5))
			t = mat_scale(a[0], n > 1 ? a[1] : a[0]);
		else if (nl == 6 && !memcmp(name, "rotate", 6)) {
			t = mat_rotate(a[0] * PI_F / 180);
			if (n == 3)
				t = mat_mul(mat_translate(a[1], a[2]), mat_mul(t, mat_translate(-a[1], -a[2])));
		} else if (nl == 5 && !memcmp(name, "skewX", 5))
			t = (Mat){ 1, 0, tanf(a[0] * PI_F / 180), 1, 0, 0 };
		else if (nl == 5 && !memcmp(name, "skewY", 5))
			t = (Mat){ 1, tanf(a[0] * PI_F / 180), 0, 1, 0, 0 };
		m = mat_mul(m, t);
	}
	return m;
}

/* ---- Path data ----------------------------------------------------------------------- */

static bool parse_flag(const char **s, bool *out)
{
	const char *p = skip_sep(*s);
	if (*p != '0' && *p != '1')
		return false;
	*out = *p == '1';
	*s = p + 1;
	return true;
}

static void parse_path_data(Path *path, const char *d)
{
	const char *s = d;
	char cmd = 0;
	f32 cx = 0, cy = 0, sx = 0, sy = 0; /* current, subpath start */
	f32 lcx = 0, lcy = 0;               /* last control point */
	char last = 0;
	for (;;) {
		s = skip_sep(s);
		if (!*s)
			break;
		if (is_alpha(*s) && *s != 'e' && *s != 'E')
			cmd = *s++;
		else if (!cmd)
			break;
		bool rel = cmd >= 'a';
		char up = (char)to_upper(cmd);
		f32 a[7];
		switch (up) {
		case 'M':
			if (!parse_num(&s, &a[0]) || !parse_num(&s, &a[1]))
				return;
			if (rel)
				a[0] += cx, a[1] += cy;
			path_move(path, a[0], a[1]);
			cx = sx = a[0], cy = sy = a[1];
			cmd = rel ? 'l' : 'L'; /* further pairs are implicit line-tos */
			break;
		case 'L':
			if (!parse_num(&s, &a[0]) || !parse_num(&s, &a[1]))
				return;
			if (rel)
				a[0] += cx, a[1] += cy;
			path_line(path, a[0], a[1]);
			cx = a[0], cy = a[1];
			break;
		case 'H':
			if (!parse_num(&s, &a[0]))
				return;
			cx = rel ? cx + a[0] : a[0];
			path_line(path, cx, cy);
			break;
		case 'V':
			if (!parse_num(&s, &a[0]))
				return;
			cy = rel ? cy + a[0] : a[0];
			path_line(path, cx, cy);
			break;
		case 'C':
			for (int i = 0; i < 6; i++)
				if (!parse_num(&s, &a[i]))
					return;
			if (rel)
				for (int i = 0; i < 6; i += 2)
					a[i] += cx, a[i + 1] += cy;
			path_cubic(path, a[0], a[1], a[2], a[3], a[4], a[5]);
			lcx = a[2], lcy = a[3], cx = a[4], cy = a[5];
			break;
		case 'S': {
			for (int i = 0; i < 4; i++)
				if (!parse_num(&s, &a[i]))
					return;
			if (rel)
				for (int i = 0; i < 4; i += 2)
					a[i] += cx, a[i + 1] += cy;
			char lu = (char)to_upper(last);
			f32 c1x = (lu == 'C' || lu == 'S') ? 2 * cx - lcx : cx;
			f32 c1y = (lu == 'C' || lu == 'S') ? 2 * cy - lcy : cy;
			path_cubic(path, c1x, c1y, a[0], a[1], a[2], a[3]);
			lcx = a[0], lcy = a[1], cx = a[2], cy = a[3];
			break;
		}
		case 'Q':
			for (int i = 0; i < 4; i++)
				if (!parse_num(&s, &a[i]))
					return;
			if (rel)
				for (int i = 0; i < 4; i += 2)
					a[i] += cx, a[i + 1] += cy;
			path_quad(path, a[0], a[1], a[2], a[3]);
			lcx = a[0], lcy = a[1], cx = a[2], cy = a[3];
			break;
		case 'T': {
			if (!parse_num(&s, &a[0]) || !parse_num(&s, &a[1]))
				return;
			if (rel)
				a[0] += cx, a[1] += cy;
			char lu = (char)to_upper(last);
			f32 qx = (lu == 'Q' || lu == 'T') ? 2 * cx - lcx : cx;
			f32 qy = (lu == 'Q' || lu == 'T') ? 2 * cy - lcy : cy;
			path_quad(path, qx, qy, a[0], a[1]);
			lcx = qx, lcy = qy, cx = a[0], cy = a[1];
			break;
		}
		case 'A': {
			bool large, sweep;
			if (!parse_num(&s, &a[0]) || !parse_num(&s, &a[1]) || !parse_num(&s, &a[2]) ||
			    !parse_flag(&s, &large) || !parse_flag(&s, &sweep) || !parse_num(&s, &a[5]) ||
			    !parse_num(&s, &a[6]))
				return;
			if (rel)
				a[5] += cx, a[6] += cy;
			path_arc_to(path, a[0], a[1], a[2], large, sweep, a[5], a[6]);
			cx = a[5], cy = a[6];
			break;
		}
		case 'Z':
			path_close(path);
			cx = sx, cy = sy;
			break;
		default:
			return;
		}
		last = cmd;
	}
}

static void parse_points(Path *path, const char *s, bool close)
{
	f32 x, y;
	bool first = true;
	while (parse_num(&s, &x) && parse_num(&s, &y)) {
		if (first)
			path_move(path, x, y);
		else
			path_line(path, x, y);
		first = false;
	}
	if (close && !first)
		path_close(path);
}

/* ---- Rendering ---------------------------------------------------------------------------- */

typedef struct {
	PaintSpec fill, stroke;
	f32 fill_opacity, stroke_opacity, opacity;
	f32 stroke_width;
	LineCap cap;
	LineJoin join;
	FillRule rule;
} Style;

typedef struct {
	const SvgDoc *doc;
	Canvas *canvas;
	Color current;
} Ctx;

/* Reads presentation attributes and the style="" attribute into st. */
static void apply_style(const Node *n, Style *st)
{
	char buf[1024];
	const char *style = attr(n, "style");
	for (int pass = 0; pass < 2; pass++) {
		for (usize i = 0; pass == 0 && i < n->keys.len; i++) {
			const char *k = n->keys.data[i], *v = n->vals.data[i];
			if (str_eq(k, "fill")) st->fill = parse_paint(v);
			else if (str_eq(k, "stroke")) st->stroke = parse_paint(v);
			else if (str_eq(k, "fill-opacity")) st->fill_opacity = (f32)str_to_float(v, NULL);
			else if (str_eq(k, "stroke-opacity")) st->stroke_opacity = (f32)str_to_float(v, NULL);
			else if (str_eq(k, "opacity")) st->opacity *= (f32)str_to_float(v, NULL);
			else if (str_eq(k, "stroke-width")) st->stroke_width = (f32)str_to_float(v, NULL);
			else if (str_eq(k, "stroke-linecap")) st->cap = str_eq(v, "round") ? CAP_ROUND : str_eq(v, "square") ? CAP_SQUARE : CAP_BUTT;
			else if (str_eq(k, "stroke-linejoin")) st->join = str_eq(v, "round") ? JOIN_ROUND : str_eq(v, "bevel") ? JOIN_BEVEL : JOIN_MITER;
			else if (str_eq(k, "fill-rule")) st->rule = str_eq(v, "evenodd") ? FILL_EVENODD : FILL_NONZERO;
		}
		if (pass == 1 && style) {
			str_copy(buf, style, sizeof buf);
			usize count;
			char **decls = str_split(buf, ';', &count);
			for (usize i = 0; i < count; i++) {
				char *colon = (char *)str_chr(decls[i], ':');
				if (!colon)
					continue;
				*colon = 0;
				char *k = str_trim(decls[i]), *v = str_trim(colon + 1);
				Node fake = { 0 };
				vec_push(&fake.keys, k);
				vec_push(&fake.vals, v);
				apply_style(&fake, st);
				vec_free(&fake.keys);
				vec_free(&fake.vals);
			}
			str_split_free(decls, count);
		}
	}
}

static const Node *resolve_gradient(const SvgDoc *doc, const char *id)
{
	return map_get(&doc->ids, id);
}

/* Collects stops, following href links to inherited gradients. */
static void gradient_stops(const SvgDoc *doc, const Node *g, Paint *p, f32 opacity, int depth)
{
	bool any = false;
	vec_each(&g->kids, kp)
	{
		const Node *k = *kp;
		if (!str_eq(k->name, "stop"))
			continue;
		any = true;
		f32 off = num_attr(k, "offset", 0, 1);
		Color c = COLOR_BLACK;
		f32 so = 1;
		const char *sc = attr(k, "stop-color");
		if (sc)
			parse_color(sc, &c);
		const char *sop = attr(k, "stop-opacity");
		if (sop)
			so = (f32)str_to_float(sop, NULL);
		const char *style = attr(k, "style");
		if (style) {
			const char *q = str_find(style, "stop-color:");
			if (q)
				parse_color(q + 11, &c);
			q = str_find(style, "stop-opacity:");
			if (q)
				so = (f32)str_to_float(q + 13, NULL);
		}
		paint_add_stop(p, off, color_scale_alpha(c, so * opacity));
	}
	if (!any && depth < 4) {
		const char *href = attr(g, "href");
		if (href && href[0] == '#') {
			const Node *base = resolve_gradient(doc, href + 1);
			if (base)
				gradient_stops(doc, base, p, opacity, depth + 1);
		}
	}
}

static bool make_paint(Ctx *cx, const PaintSpec *ps, f32 opacity, const Path *path, Mat m, Paint *out)
{
	switch (ps->kind) {
	case PAINT_NONE:
		return false;
	case PAINT_COLOR:
		*out = paint_solid(color_scale_alpha(ps->color, opacity));
		return true;
	case PAINT_CURRENT:
		*out = paint_solid(color_scale_alpha(cx->current, opacity));
		return true;
	case PAINT_URL: {
		const Node *g = resolve_gradient(cx->doc, ps->id);
		if (!g)
			return false;
		bool radial = str_eq(g->name, "radialGradient");
		const char *units = attr(g, "gradientUnits");
		bool user = units && str_eq(units, "userSpaceOnUse");
		Rect bb = path_bounds(path);
		Mat gm = attr(g, "gradientTransform") ? parse_transform(attr(g, "gradientTransform")) : mat_identity();
		Mat to_user = user ? gm : mat_mul(mat_mul(mat_translate(bb.x, bb.y), mat_scale(bb.w, bb.h)), gm);
		Mat full = mat_mul(m, to_user);
		Paint p;
		if (radial) {
			f32 rcx = num_attr(g, "cx", user ? 0 : 0.5f, user ? cx->doc->vb_w : 1);
			f32 rcy = num_attr(g, "cy", user ? 0 : 0.5f, user ? cx->doc->vb_h : 1);
			f32 r = num_attr(g, "r", user ? 0 : 0.5f, user ? cx->doc->vb_w : 1);
			Vec2 c = mat_apply(full, (Vec2){ rcx, rcy });
			p = (Paint){ .kind = PAINT_RADIAL, .p0 = c, .radius = r * mat_scale_factor(full) };
		} else {
			f32 x1 = num_attr(g, "x1", 0, user ? cx->doc->vb_w : 1), y1 = num_attr(g, "y1", 0, user ? cx->doc->vb_h : 1);
			f32 x2 = num_attr(g, "x2", user ? cx->doc->vb_w : 1, user ? cx->doc->vb_w : 1);
			f32 y2 = num_attr(g, "y2", 0, user ? cx->doc->vb_h : 1);
			Vec2 a = mat_apply(full, (Vec2){ x1, y1 }), b = mat_apply(full, (Vec2){ x2, y2 });
			p = (Paint){ .kind = PAINT_LINEAR, .p0 = a, .p1 = b };
		}
		gradient_stops(cx->doc, g, &p, opacity, 0);
		if (!p.nstops)
			return false;
		*out = p;
		return true;
	}
	}
	return false;
}

static void draw_shape(Ctx *cx, const Path *path, Mat m, const Style *st)
{
	Paint p;
	if (make_paint(cx, &st->fill, st->fill_opacity * st->opacity, path, m, &p))
		fill_path(cx->canvas, path, m, &p, st->rule);
	if (st->stroke_width > 0 && make_paint(cx, &st->stroke, st->stroke_opacity * st->opacity, path, m, &p)) {
		StrokeStyle ss = { st->stroke_width, st->cap, st->join, 4 };
		stroke_path(cx->canvas, path, m, &p, &ss);
	}
}

static void render_node(Ctx *cx, const Node *n, Mat m, Style st, int depth)
{
	if (depth > 32)
		return;
	const char *name = n->name;
	if (str_eq(name, "defs") || str_eq(name, "linearGradient") || str_eq(name, "radialGradient") ||
	    str_eq(name, "clipPath") || str_eq(name, "mask") || str_eq(name, "title") || str_eq(name, "desc") ||
	    str_eq(name, "metadata") || str_eq(name, "style"))
		return;
	const char *disp = attr(n, "display");
	if (disp && str_eq(disp, "none"))
		return;
	apply_style(n, &st);
	const char *tr = attr(n, "transform");
	if (tr)
		m = mat_mul(m, parse_transform(tr));

	Path path = { 0 };
	bool shape = true;
	if (str_eq(name, "path")) {
		const char *d = attr(n, "d");
		if (d)
			parse_path_data(&path, d);
	} else if (str_eq(name, "rect")) {
		f32 x = num_attr(n, "x", 0, cx->doc->vb_w), y = num_attr(n, "y", 0, cx->doc->vb_h);
		f32 w = num_attr(n, "width", 0, cx->doc->vb_w), h = num_attr(n, "height", 0, cx->doc->vb_h);
		f32 rx = num_attr(n, "rx", -1, cx->doc->vb_w), ry = num_attr(n, "ry", -1, cx->doc->vb_h);
		if (rx < 0)
			rx = ry;
		if (rx < 0)
			rx = 0;
		if (w > 0 && h > 0)
			path_round_rect(&path, rect(x, y, w, h), rx);
	} else if (str_eq(name, "circle")) {
		f32 r = num_attr(n, "r", 0, cx->doc->vb_w);
		if (r > 0)
			path_circle(&path, num_attr(n, "cx", 0, cx->doc->vb_w), num_attr(n, "cy", 0, cx->doc->vb_h), r);
	} else if (str_eq(name, "ellipse")) {
		f32 rx = num_attr(n, "rx", 0, cx->doc->vb_w), ry = num_attr(n, "ry", 0, cx->doc->vb_h);
		if (rx > 0 && ry > 0)
			path_ellipse(&path, num_attr(n, "cx", 0, cx->doc->vb_w), num_attr(n, "cy", 0, cx->doc->vb_h), rx, ry);
	} else if (str_eq(name, "line")) {
		path_move(&path, num_attr(n, "x1", 0, 1), num_attr(n, "y1", 0, 1));
		path_line(&path, num_attr(n, "x2", 0, 1), num_attr(n, "y2", 0, 1));
	} else if (str_eq(name, "polyline") || str_eq(name, "polygon")) {
		const char *pts = attr(n, "points");
		if (pts)
			parse_points(&path, pts, str_eq(name, "polygon"));
	} else if (str_eq(name, "use")) {
		const char *href = attr(n, "href");
		shape = false;
		if (href && href[0] == '#') {
			const Node *target = map_get(&cx->doc->ids, href + 1);
			if (target) {
				Mat um = mat_mul(m, mat_translate(num_attr(n, "x", 0, 1), num_attr(n, "y", 0, 1)));
				render_node(cx, target, um, st, depth + 1);
			}
		}
	} else {
		shape = false;
	}
	if (shape && path.verbs.len)
		draw_shape(cx, &path, m, &st);
	path_free(&path);
	if (str_eq(name, "g") || str_eq(name, "svg") || str_eq(name, "a") || str_eq(name, "symbol") || str_eq(name, "switch"))
		vec_each(&n->kids, k) render_node(cx, *k, m, st, depth + 1);
}

static void index_ids(SvgDoc *doc, Node *n)
{
	const char *id = attr(n, "id");
	if (id)
		map_set(&doc->ids, id, n);
	vec_each(&n->kids, k) index_ids(doc, *k);
}

SvgDoc *svg_parse(const char *text, usize len)
{
	Node *root = parse_xml(text, len);
	if (!root || !str_eq(root->name, "svg")) {
		free_node(root);
		return NULL;
	}
	SvgDoc *doc = NEW(SvgDoc);
	doc->root = root;
	index_ids(doc, root);
	doc->width = num_attr(root, "width", 0, 100);
	doc->height = num_attr(root, "height", 0, 100);
	const char *vb = attr(root, "viewBox");
	f32 v[4];
	const char *p = vb;
	if (vb && parse_num(&p, &v[0]) && parse_num(&p, &v[1]) && parse_num(&p, &v[2]) && parse_num(&p, &v[3]) && v[2] > 0 && v[3] > 0) {
		doc->vb_x = v[0], doc->vb_y = v[1], doc->vb_w = v[2], doc->vb_h = v[3];
	} else {
		doc->vb_w = doc->width > 0 ? doc->width : 100;
		doc->vb_h = doc->height > 0 ? doc->height : 100;
	}
	if (doc->width <= 0)
		doc->width = doc->vb_w;
	if (doc->height <= 0)
		doc->height = doc->vb_h;
	return doc;
}

SvgDoc *svg_load(const char *path)
{
	usize len;
	char *text = file_read(path, &len);
	if (!text)
		return NULL;
	SvgDoc *d = svg_parse(text, len);
	mem_free(text);
	return d;
}

void svg_free(SvgDoc *doc)
{
	if (!doc)
		return;
	free_node(doc->root);
	map_free(&doc->ids, NULL);
	mem_free(doc);
}

void svg_size(const SvgDoc *doc, f32 *w, f32 *h)
{
	*w = doc->width;
	*h = doc->height;
}

void svg_render(const SvgDoc *doc, Canvas *c, Rect dst, Color current)
{
	f32 s = minf(dst.w / doc->vb_w, dst.h / doc->vb_h);
	f32 ox = dst.x + (dst.w - doc->vb_w * s) / 2, oy = dst.y + (dst.h - doc->vb_h * s) / 2;
	Mat m = mat_mul(mat_translate(ox, oy), mat_mul(mat_scale(s, s), mat_translate(-doc->vb_x, -doc->vb_y)));
	Style st = { { PAINT_COLOR, COLOR_BLACK, { 0 } }, { PAINT_NONE, 0, { 0 } }, 1, 1, 1, 1, CAP_BUTT, JOIN_MITER, FILL_NONZERO };
	Ctx cx = { doc, c, current };
	render_node(&cx, doc->root, m, st, 0);
}

Canvas *svg_rasterize(const SvgDoc *doc, int w, int h, Color current)
{
	Canvas *c = canvas_new(w, h);
	svg_render(doc, c, rect(0, 0, (f32)w, (f32)h), current);
	return c;
}
