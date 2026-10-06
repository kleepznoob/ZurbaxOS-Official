/* HTML parsing: a forgiving tokenizer and tree builder. Real pages are
 * often not well formed; this follows the common HTML rules for implied
 * end tags, void and raw-text elements, and ignores what it can't place. */
#include "drift.h"

static Node *new_node(int type, const char *tag)
{
	Node *n = NEW(Node);
	n->type = type;
	if (tag)
		str_copy(n->tag, tag, sizeof n->tag);
	return n;
}

static void append(Node *parent, Node *child)
{
	child->parent = parent;
	child->prev = parent->last;
	if (parent->last)
		parent->last->next = child;
	else
		parent->first = child;
	parent->last = child;
	if (child->type == NODE_ELEMENT) {
		int i = 0;
		for (Node *s = child->prev; s; s = s->prev)
			if (s->type == NODE_ELEMENT)
				i++;
		child->index = i;
	}
}

void node_free(Node *n)
{
	if (!n)
		return;
	Node *c = n->first;
	while (c) {
		Node *next = c->next;
		node_free(c);
		c = next;
	}
	for (int i = 0; i < n->nattrs * 2; i++)
		mem_free(n->attrs[i]);
	mem_free(n->attrs);
	mem_free(n->text);
	if (n->style)
		style_free_parts(n->style);
	mem_free(n->style);
	if (n->control)
		sb_free(&n->control->value);
	mem_free(n->control);
	mem_free(n);
}

const char *node_attr(const Node *n, const char *name)
{
	for (int i = 0; i < n->nattrs; i++)
		if (str_eq(n->attrs[i * 2], name))
			return n->attrs[i * 2 + 1];
	return NULL;
}

bool node_has_class(const Node *n, const char *cls)
{
	const char *c = node_attr(n, "class");
	if (!c)
		return false;
	usize l = strlen(cls);
	for (const char *p = c; *p;) {
		while (*p && is_space(*p))
			p++;
		const char *s = p;
		while (*p && !is_space(*p))
			p++;
		if ((usize)(p - s) == l && memcmp(s, cls, l) == 0)
			return true;
	}
	return false;
}

Node *node_find(Node *root, const char *tag)
{
	if (!root)
		return NULL;
	if (root->type == NODE_ELEMENT && str_eq(root->tag, tag))
		return root;
	for (Node *c = root->first; c; c = c->next) {
		Node *f = node_find(c, tag);
		if (f)
			return f;
	}
	return NULL;
}

void node_text_content(const Node *n, StrBuf *out)
{
	if (n->type == NODE_TEXT) {
		sb_adds(out, n->text);
		return;
	}
	for (Node *c = n->first; c; c = c->next)
		node_text_content(c, out);
}

/* ---- Entities ------------------------------------------------------------------------------------ */

static const struct { const char *name; u32 cp; } ENTITIES[] = {
	{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xa0 }, { "copy", 0xa9 },
	{ "reg", 0xae }, { "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "lsquo", 0x2018 },
	{ "rsquo", 0x2019 }, { "ldquo", 0x201c }, { "rdquo", 0x201d }, { "sbquo", 0x201a }, { "bdquo", 0x201e }, { "laquo", 0xab },
	{ "raquo", 0xbb }, { "middot", 0xb7 }, { "bull", 0x2022 }, { "euro", 0x20ac }, { "pound", 0xa3 }, { "yen", 0xa5 },
	{ "cent", 0xa2 }, { "deg", 0xb0 }, { "times", 0xd7 }, { "divide", 0xf7 }, { "plusmn", 0xb1 }, { "frac12", 0xbd },
	{ "frac14", 0xbc }, { "frac34", 0xbe }, { "para", 0xb6 }, { "sect", 0xa7 }, { "larr", 0x2190 }, { "rarr", 0x2192 },
	{ "uarr", 0x2191 }, { "darr", 0x2193 }, { "harr", 0x2194 }, { "shy", 0xad }, { "zwj", 0x200d }, { "zwnj", 0x200c },
	{ "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "iexcl", 0xa1 }, { "iquest", 0xbf }, { "ordm", 0xba },
	{ "ordf", 0xaa }, { "sup2", 0xb2 }, { "sup3", 0xb3 }, { "micro", 0xb5 }, { "dagger", 0x2020 }, { "prime", 0x2032 },
	{ "minus", 0x2212 }, { "le", 0x2264 }, { "ge", 0x2265 }, { "ne", 0x2260 }, { "infin", 0x221e }, { "pi", 0x3c0 },
	{ "auml", 0xe4 }, { "ouml", 0xf6 }, { "uuml", 0xfc }, { "Auml", 0xc4 }, { "Ouml", 0xd6 }, { "Uuml", 0xdc }, { "szlig", 0xdf },
	{ "eacute", 0xe9 }, { "egrave", 0xe8 }, { "ecirc", 0xea }, { "aacute", 0xe1 }, { "agrave", 0xe0 }, { "acirc", 0xe2 },
	{ "iacute", 0xed }, { "icirc", 0xee }, { "oacute", 0xf3 }, { "ocirc", 0xf4 }, { "uacute", 0xfa }, { "ccedil", 0xe7 },
	{ "ntilde", 0xf1 }, { "atilde", 0xe3 }, { "otilde", 0xf5 }, { "Eacute", 0xc9 }, { "aring", 0xe5 }, { "oslash", 0xf8 },
	{ "check", 0x2713 }, { "star", 0x2606 },
};

void html_decode_entities(char *s)
{
	char *w = s;
	for (const char *r = s; *r;) {
		if (*r != '&') {
			*w++ = *r++;
			continue;
		}
		const char *semi = r + 1;
		while (*semi && *semi != ';' && semi - r < 12 && (is_alnum(*semi) || *semi == '#'))
			semi++;
		u32 cp = 0;
		bool ok = false;
		if (r[1] == '#') {
			const char *d = r + 2;
			if (*d == 'x' || *d == 'X') {
				d++;
				while (is_xdigit(*d))
					cp = cp * 16 + (u32)(is_digit(*d) ? *d - '0' : to_lower(*d) - 'a' + 10), d++;
			} else {
				while (is_digit(*d))
					cp = cp * 10 + (u32)(*d - '0'), d++;
			}
			ok = d > r + 2 && cp > 0 && cp < 0x110000;
			semi = d;
		} else {
			usize l = (usize)(semi - r - 1);
			for (usize i = 0; i < ARRAY_LEN(ENTITIES) && l; i++)
				if (strlen(ENTITIES[i].name) == l && memcmp(ENTITIES[i].name, r + 1, l) == 0) {
					cp = ENTITIES[i].cp;
					ok = true;
					break;
				}
		}
		if (!ok) {
			*w++ = *r++;
			continue;
		}
		if (cp == 0xad || cp == 0x200c || cp == 0x200d) {
			/* invisible */
		} else {
			char u[4];
			usize k = utf8_encode(cp, u);
			/* Never longer than the entity it replaces. */
			memcpy(w, u, k);
			w += k;
		}
		r = *semi == ';' ? semi + 1 : semi;
	}
	*w = 0;
}

/* ---- Tree building ---------------------------------------------------------------------------------- */

static bool is_void(const char *t)
{
	static const char *const v[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr" };
	for (usize i = 0; i < ARRAY_LEN(v); i++)
		if (str_eq(t, v[i]))
			return true;
	return false;
}

static bool closes_p(const char *t)
{
	static const char *const v[] = { "address", "article", "aside", "blockquote", "details", "div", "dl", "fieldset", "figcaption", "figure",
					 "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "main", "nav", "ol", "p",
					 "pre", "section", "table", "ul" };
	for (usize i = 0; i < ARRAY_LEN(v); i++)
		if (str_eq(t, v[i]))
			return true;
	return false;
}

typedef struct {
	Node *stack[256];
	int depth;
} Builder;

static Node *current(Builder *b)
{
	return b->stack[b->depth - 1];
}

static int find_open(Builder *b, const char *tag, const char *const *stop, int nstop)
{
	for (int i = b->depth - 1; i > 0; i--) {
		if (str_eq(b->stack[i]->tag, tag))
			return i;
		for (int k = 0; k < nstop; k++)
			if (str_eq(b->stack[i]->tag, stop[k]))
				return -1;
	}
	return -1;
}

static void close_to(Builder *b, int index)
{
	if (index > 0)
		b->depth = index;
}

static void open_element(Builder *b, Node *el)
{
	const char *t = el->tag;
	static const char *const list_scope[] = { "ul", "ol", "table", "div" };
	static const char *const table_scope[] = { "table" };
	if (closes_p(t))
		close_to(b, find_open(b, "p", list_scope, 0));
	if (str_eq(t, "li"))
		close_to(b, find_open(b, "li", list_scope, 3));
	if (str_eq(t, "dt") || str_eq(t, "dd")) {
		static const char *const dl[] = { "dl" };
		int i = find_open(b, "dd", dl, 1), j = find_open(b, "dt", dl, 1);
		close_to(b, MAX(i, j));
	}
	if (str_eq(t, "tr")) {
		int i = find_open(b, "tr", table_scope, 1);
		close_to(b, i);
	}
	if (str_eq(t, "td") || str_eq(t, "th")) {
		static const char *const row[] = { "tr", "table" };
		int i = find_open(b, "td", row, 2), j = find_open(b, "th", row, 2);
		close_to(b, MAX(i, j));
	}
	if (str_eq(t, "thead") || str_eq(t, "tbody") || str_eq(t, "tfoot")) {
		int i = MAX(find_open(b, "thead", table_scope, 1), MAX(find_open(b, "tbody", table_scope, 1), find_open(b, "tfoot", table_scope, 1)));
		close_to(b, i);
	}
	if (str_eq(t, "option")) {
		static const char *const sel[] = { "select" };
		close_to(b, find_open(b, "option", sel, 1));
	}
	if (str_eq(t, "a"))
		close_to(b, find_open(b, "a", NULL, 0)); /* links don't nest */
	append(current(b), el);
	if (!is_void(t) && b->depth < (int)ARRAY_LEN(b->stack))
		b->stack[b->depth++] = el;
}

static void close_element(Builder *b, const char *tag)
{
	if (str_eq(tag, "p") && find_open(b, "p", NULL, 0) < 0) {
		/* </p> without <p> makes an empty paragraph. */
		append(current(b), new_node(NODE_ELEMENT, "p"));
		return;
	}
	static const char *const scope[] = { "table", "html" };
	int i = find_open(b, tag, str_eq(tag, "table") ? NULL : scope, str_eq(tag, "table") ? 0 : 2);
	if (i > 0)
		b->depth = i;
}

static void add_text(Builder *b, const char *s, usize n)
{
	if (!n)
		return;
	Node *cur = current(b);
	/* Join with the previous text node. */
	if (cur->last && cur->last->type == NODE_TEXT) {
		usize old = strlen(cur->last->text);
		cur->last->text = mem_realloc(cur->last->text, old + n + 1);
		memcpy(cur->last->text + old, s, n);
		cur->last->text[old + n] = 0;
		return;
	}
	Node *t = new_node(NODE_TEXT, NULL);
	t->text = str_ndup(s, n);
	append(cur, t);
}

static void decode_last_text(Builder *b)
{
	Node *cur = current(b);
	if (cur->last && cur->last->type == NODE_TEXT)
		html_decode_entities(cur->last->text);
}

bool html_quirks(const Node *root)
{
	return root && root->nattrs == 0 && str_eq(root->tag, "#document");
}

Node *html_parse(const char *src, usize len)
{
	Node *doc = new_node(NODE_ELEMENT, "#document");
	/* A doctype means standards mode; the root carries it as an attribute. */
	for (const char *p = src; p < src + len && p < src + 4096; p++) {
		if (*p == '<' && p + 9 < src + len && str_eq_nocase_n(p, "<!doctype", 9)) {
			doc->attrs = mem_alloc(sizeof(char *) * 2);
			doc->attrs[0] = str_dup("doctype");
			doc->attrs[1] = str_dup("html");
			doc->nattrs = 1;
			break;
		}
		if (*p == '<' && is_alpha(p[1]))
			break;
	}
	Builder b = { .depth = 1 };
	b.stack[0] = doc;
	const char *p = src, *end = src + len;
	while (p < end) {
		if (*p != '<') {
			const char *q = p;
			while (q < end && *q != '<')
				q++;
			add_text(&b, p, (usize)(q - p));
			decode_last_text(&b);
			p = q;
			continue;
		}
		/* Comments, doctype, CDATA. */
		if (end - p >= 4 && memcmp(p, "<!--", 4) == 0) {
			const char *e = str_find(p + 4, "-->");
			p = e && e < end ? e + 3 : end;
			continue;
		}
		if (end - p >= 2 && (p[1] == '!' || p[1] == '?')) {
			while (p < end && *p != '>')
				p++;
			p++;
			continue;
		}
		bool closing = end - p >= 2 && p[1] == '/';
		const char *n = p + (closing ? 2 : 1);
		if (n >= end || !is_alpha(*n)) {
			add_text(&b, p, 1);
			p++;
			continue;
		}
		char tag[16];
		usize tl = 0;
		while (n < end && (is_alnum(*n) || *n == '-' || *n == ':')) {
			if (tl < sizeof tag - 1)
				tag[tl++] = (char)to_lower(*n);
			n++;
		}
		tag[tl] = 0;
		if (closing) {
			while (n < end && *n != '>')
				n++;
			p = n + 1;
			close_element(&b, tag);
			continue;
		}
		Node *el = new_node(NODE_ELEMENT, tag);
		/* Attributes. */
		bool self_close = false;
		for (;;) {
			while (n < end && (is_space(*n) || *n == '/')) {
				if (*n == '/' && n + 1 < end && n[1] == '>')
					self_close = true;
				n++;
			}
			if (n >= end || *n == '>')
				break;
			const char *an = n;
			while (n < end && !is_space(*n) && *n != '=' && *n != '>' && !(*n == '/' && n + 1 < end && n[1] == '>'))
				n++;
			char *name = str_ndup(an, (usize)(n - an));
			str_lower(name);
			char *value = NULL;
			while (n < end && is_space(*n))
				n++;
			if (n < end && *n == '=') {
				n++;
				while (n < end && is_space(*n))
					n++;
				if (n < end && (*n == '"' || *n == '\'')) {
					char q = *n++;
					const char *vs = n;
					while (n < end && *n != q)
						n++;
					value = str_ndup(vs, (usize)(n - vs));
					if (n < end)
						n++;
				} else {
					const char *vs = n;
					while (n < end && !is_space(*n) && *n != '>')
						n++;
					value = str_ndup(vs, (usize)(n - vs));
				}
				html_decode_entities(value);
			}
			el->attrs = mem_realloc(el->attrs, sizeof(char *) * (usize)(el->nattrs + 1) * 2);
			el->attrs[el->nattrs * 2] = name;
			el->attrs[el->nattrs * 2 + 1] = value ? value : str_dup("");
			el->nattrs++;
		}
		p = n < end ? n + 1 : end;
		open_element(&b, el);
		/* In SVG and MathML, <x/> is a complete element. */
		if (self_close && !is_void(tag) && b.depth > 1 && current(&b) == el) {
			for (int i = b.depth - 1; i > 0; i--)
				if (str_eq(b.stack[i]->tag, "svg") || str_eq(b.stack[i]->tag, "math")) {
					b.depth--;
					break;
				}
		}
		/* Raw text: everything up to the matching end tag. */
		bool raw = str_eq(tag, "script") || str_eq(tag, "style") || str_eq(tag, "textarea") || str_eq(tag, "title") || str_eq(tag, "noscript");
		if (raw) {
			char endtag[24];
			fmt(endtag, sizeof endtag, "</%s", tag);
			const char *e = p;
			while (e < end && !(e[0] == '<' && e[1] == '/' && str_eq_nocase_n(e, endtag, strlen(endtag))))
				e++;
			if (!str_eq(tag, "noscript")) {
				add_text(&b, p, (usize)(e - p));
				if (str_eq(tag, "textarea") || str_eq(tag, "title"))
					decode_last_text(&b);
				while (e < end && *e != '>')
					e++;
				p = e < end ? e + 1 : end;
				close_element(&b, tag);
			} else {
				/* Without scripts, what's in <noscript> is meant for us: parse it normally. */
			}
		}
	}
	return doc;
}

/* ---- Serializing (inline SVG) -------------------------------------------------------------------- */

/* The parser lower-cases names; SVG needs some of them back in camel case. */
static const char *svg_name(const char *s)
{
	static const char *const names[] = { "viewBox", "preserveAspectRatio", "linearGradient", "radialGradient", "gradientUnits",
					     "gradientTransform", "clipPath", "clipPathUnits", "patternUnits", "stdDeviation", "textLength",
					     "spreadMethod", "markerWidth", "markerHeight", "refX", "refY", "foreignObject", "patternTransform" };
	for (usize i = 0; i < ARRAY_LEN(names); i++)
		if (str_eq_nocase(s, names[i]))
			return names[i];
	return s;
}

static void escape_into(StrBuf *out, const char *s, bool attr)
{
	for (; *s; s++) {
		if (*s == '&')
			sb_adds(out, "&amp;");
		else if (*s == '<')
			sb_adds(out, "&lt;");
		else if (*s == '"' && attr)
			sb_adds(out, "&quot;");
		else
			sb_addc(out, *s);
	}
}

void node_serialize(const Node *n, StrBuf *out)
{
	if (n->type == NODE_TEXT) {
		escape_into(out, n->text, false);
		return;
	}
	sb_fmt(out, "<%s", svg_name(n->tag));
	for (int i = 0; i < n->nattrs; i++) {
		sb_fmt(out, " %s=\"", svg_name(n->attrs[i * 2]));
		escape_into(out, n->attrs[i * 2 + 1], true);
		sb_addc(out, '"');
	}
	if (!n->first) {
		sb_adds(out, "/>");
		return;
	}
	sb_addc(out, '>');
	for (const Node *c = n->first; c; c = c->next)
		node_serialize(c, out);
	sb_fmt(out, "</%s>", svg_name(n->tag));
}
