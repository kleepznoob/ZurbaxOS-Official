/* CSS: parsing style sheets, matching selectors and computing styles.
 *
 * Sheets are parsed into rules (one per selector) that point at shared
 * declaration blocks. Values stay as text until the cascade, where they are
 * resolved against the element: custom properties, em and percentages all
 * depend on where a declaration lands. */
#include "drift.h"

/* ---- Properties --------------------------------------------------------------------------------- */

enum {
	P_NONE, P_DISPLAY, P_POSITION, P_FLOAT, P_CLEAR, P_COLOR, P_BACKGROUND, P_BACKGROUND_COLOR, P_BACKGROUND_IMAGE,
	P_FONT, P_FONT_SIZE, P_FONT_WEIGHT, P_FONT_STYLE, P_FONT_FAMILY, P_LINE_HEIGHT, P_TEXT_ALIGN, P_TEXT_DECORATION,
	P_TEXT_TRANSFORM, P_TEXT_INDENT, P_WHITE_SPACE, P_VERTICAL_ALIGN, P_LIST_STYLE, P_LIST_STYLE_TYPE, P_VISIBILITY,
	P_OPACITY, P_OVERFLOW, P_BOX_SIZING, P_MARGIN, P_PADDING, P_BORDER, P_BORDER_WIDTH, P_BORDER_STYLE, P_BORDER_COLOR,
	P_BORDER_RADIUS, P_WIDTH, P_HEIGHT, P_MIN_WIDTH, P_MAX_WIDTH, P_MIN_HEIGHT, P_MAX_HEIGHT, P_FLEX, P_FLEX_DIRECTION,
	P_FLEX_WRAP, P_FLEX_FLOW, P_FLEX_GROW, P_FLEX_SHRINK, P_FLEX_BASIS, P_JUSTIFY_CONTENT, P_ALIGN_ITEMS, P_ALIGN_SELF,
	P_GAP, P_ROW_GAP, P_COLUMN_GAP, P_GRID_TEMPLATE_COLUMNS, P_BORDER_SPACING, P_BORDER_COLLAPSE, P_PLACE_ITEMS,
	P_GRID_TEMPLATE_AREAS, P_GRID_TEMPLATE, P_GRID_AREA, P_GRID_COLUMN, P_GRID_ROW, P_MASK_IMAGE, P_ORDER,
	P_CUSTOM
};

enum { S_T = 1, S_R = 2, S_B = 4, S_L = 8, S_ALL = 15, S_INLINE = S_L | S_R, S_BLOCK = S_T | S_B };

static const struct {
	const char *name;
	u8 prop, sides;
} PROPS[] = {
	{ "display", P_DISPLAY, 0 }, { "position", P_POSITION, 0 }, { "float", P_FLOAT, 0 }, { "clear", P_CLEAR, 0 },
	{ "color", P_COLOR, 0 }, { "background", P_BACKGROUND, 0 }, { "background-color", P_BACKGROUND_COLOR, 0 },
	{ "background-image", P_BACKGROUND_IMAGE, 0 }, { "font", P_FONT, 0 }, { "font-size", P_FONT_SIZE, 0 },
	{ "font-weight", P_FONT_WEIGHT, 0 }, { "font-style", P_FONT_STYLE, 0 }, { "font-family", P_FONT_FAMILY, 0 },
	{ "line-height", P_LINE_HEIGHT, 0 }, { "text-align", P_TEXT_ALIGN, 0 }, { "text-decoration", P_TEXT_DECORATION, 0 },
	{ "text-decoration-line", P_TEXT_DECORATION, 0 }, { "text-transform", P_TEXT_TRANSFORM, 0 },
	{ "text-indent", P_TEXT_INDENT, 0 }, { "white-space", P_WHITE_SPACE, 0 }, { "vertical-align", P_VERTICAL_ALIGN, 0 },
	{ "list-style", P_LIST_STYLE, 0 }, { "list-style-type", P_LIST_STYLE_TYPE, 0 }, { "visibility", P_VISIBILITY, 0 },
	{ "opacity", P_OPACITY, 0 }, { "overflow", P_OVERFLOW, 0 }, { "overflow-x", P_OVERFLOW, 0 }, { "overflow-y", P_OVERFLOW, 0 },
	{ "box-sizing", P_BOX_SIZING, 0 },
	{ "margin", P_MARGIN, S_ALL }, { "margin-top", P_MARGIN, S_T }, { "margin-right", P_MARGIN, S_R },
	{ "margin-bottom", P_MARGIN, S_B }, { "margin-left", P_MARGIN, S_L }, { "margin-inline", P_MARGIN, S_INLINE },
	{ "margin-block", P_MARGIN, S_BLOCK }, { "margin-inline-start", P_MARGIN, S_L }, { "margin-inline-end", P_MARGIN, S_R },
	{ "margin-block-start", P_MARGIN, S_T }, { "margin-block-end", P_MARGIN, S_B },
	{ "padding", P_PADDING, S_ALL }, { "padding-top", P_PADDING, S_T }, { "padding-right", P_PADDING, S_R },
	{ "padding-bottom", P_PADDING, S_B }, { "padding-left", P_PADDING, S_L }, { "padding-inline", P_PADDING, S_INLINE },
	{ "padding-block", P_PADDING, S_BLOCK }, { "padding-inline-start", P_PADDING, S_L }, { "padding-inline-end", P_PADDING, S_R },
	{ "padding-block-start", P_PADDING, S_T }, { "padding-block-end", P_PADDING, S_B },
	{ "border", P_BORDER, S_ALL }, { "border-top", P_BORDER, S_T }, { "border-right", P_BORDER, S_R },
	{ "border-bottom", P_BORDER, S_B }, { "border-left", P_BORDER, S_L }, { "border-inline", P_BORDER, S_INLINE },
	{ "border-block", P_BORDER, S_BLOCK }, { "border-inline-start", P_BORDER, S_L }, { "border-inline-end", P_BORDER, S_R },
	{ "border-block-start", P_BORDER, S_T }, { "border-block-end", P_BORDER, S_B },
	{ "border-width", P_BORDER_WIDTH, S_ALL }, { "border-top-width", P_BORDER_WIDTH, S_T },
	{ "border-right-width", P_BORDER_WIDTH, S_R }, { "border-bottom-width", P_BORDER_WIDTH, S_B },
	{ "border-left-width", P_BORDER_WIDTH, S_L }, { "border-style", P_BORDER_STYLE, S_ALL },
	{ "border-top-style", P_BORDER_STYLE, S_T }, { "border-right-style", P_BORDER_STYLE, S_R },
	{ "border-bottom-style", P_BORDER_STYLE, S_B }, { "border-left-style", P_BORDER_STYLE, S_L },
	{ "border-color", P_BORDER_COLOR, S_ALL }, { "border-top-color", P_BORDER_COLOR, S_T },
	{ "border-right-color", P_BORDER_COLOR, S_R }, { "border-bottom-color", P_BORDER_COLOR, S_B },
	{ "border-left-color", P_BORDER_COLOR, S_L }, { "border-radius", P_BORDER_RADIUS, 0 },
	{ "width", P_WIDTH, 0 }, { "height", P_HEIGHT, 0 }, { "min-width", P_MIN_WIDTH, 0 }, { "max-width", P_MAX_WIDTH, 0 },
	{ "min-height", P_MIN_HEIGHT, 0 }, { "max-height", P_MAX_HEIGHT, 0 }, { "inline-size", P_WIDTH, 0 },
	{ "max-inline-size", P_MAX_WIDTH, 0 }, { "min-inline-size", P_MIN_WIDTH, 0 }, { "block-size", P_HEIGHT, 0 },
	{ "flex", P_FLEX, 0 }, { "flex-direction", P_FLEX_DIRECTION, 0 }, { "flex-wrap", P_FLEX_WRAP, 0 },
	{ "flex-flow", P_FLEX_FLOW, 0 }, { "flex-grow", P_FLEX_GROW, 0 }, { "flex-shrink", P_FLEX_SHRINK, 0 },
	{ "flex-basis", P_FLEX_BASIS, 0 }, { "justify-content", P_JUSTIFY_CONTENT, 0 }, { "align-items", P_ALIGN_ITEMS, 0 },
	{ "align-self", P_ALIGN_SELF, 0 }, { "place-items", P_PLACE_ITEMS, 0 }, { "gap", P_GAP, 0 }, { "grid-gap", P_GAP, 0 },
	{ "row-gap", P_ROW_GAP, 0 }, { "grid-row-gap", P_ROW_GAP, 0 }, { "column-gap", P_COLUMN_GAP, 0 },
	{ "grid-column-gap", P_COLUMN_GAP, 0 }, { "grid-template-columns", P_GRID_TEMPLATE_COLUMNS, 0 },
	{ "border-spacing", P_BORDER_SPACING, 0 }, { "border-collapse", P_BORDER_COLLAPSE, 0 },
	{ "grid-template-areas", P_GRID_TEMPLATE_AREAS, 0 }, { "grid-template", P_GRID_TEMPLATE, 0 }, { "grid-area", P_GRID_AREA, 0 },
	{ "grid-column", P_GRID_COLUMN, 0 }, { "grid-row", P_GRID_ROW, 0 }, { "grid-column-start", P_GRID_COLUMN, 0 },
	{ "grid-row-start", P_GRID_ROW, 0 }, { "mask-image", P_MASK_IMAGE, 0 }, { "-webkit-mask-image", P_MASK_IMAGE, 0 },
	{ "mask", P_MASK_IMAGE, 0 }, { "-webkit-mask", P_MASK_IMAGE, 0 }, { "order", P_ORDER, 0 },
};

static Map prop_map;

static void props_init(void)
{
	if (prop_map.cap)
		return;
	for (usize i = 0; i < ARRAY_LEN(PROPS); i++)
		map_set(&prop_map, PROPS[i].name, (void *)(uintptr_t)(i + 1));
}

/* ---- Sheet structures ------------------------------------------------------------------------------ */

typedef struct Compound Compound;

typedef struct {
	char *name, *value;
	u8 op;                   /* 0 = present, or one of = ~ | ^ $ * */
	bool nocase;
} AttrSel;

enum {
	PC_FIRST_CHILD = 1, PC_LAST_CHILD, PC_ONLY_CHILD, PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_FIRST_OF_TYPE, PC_LAST_OF_TYPE,
	PC_ONLY_OF_TYPE, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE, PC_NOT, PC_IS, PC_LINK, PC_ROOT, PC_EMPTY, PC_CHECKED, PC_DISABLED,
	PC_ENABLED, PC_ALWAYS, PC_NEVER
};

typedef struct {
	u8 kind;
	int a, b;
	Compound *args;
	int nargs;
} PseudoSel;

struct Compound {
	char tag[16];
	char *id;
	char **classes;
	int nclasses;
	AttrSel *attrs;
	int nattrs;
	PseudoSel *pseudos;
	int npseudos;
	char comb;               /* relation to the compound on the left: ' ', '>', '+', '~' */
};

typedef struct {
	u8 prop, sides;
	bool important;
	char *name;              /* custom properties */
	char *value;
} Decl;

typedef struct {
	Decl *decls;
	int n;
} DeclBlock;

typedef struct {
	Compound *parts;
	int nparts;
	u32 spec;
	DeclBlock *block;
	const char *media;       /* condition, NULL = always */
	u32 order;
} Rule;

struct Sheet {
	VEC(Rule) rules;
	VEC(DeclBlock *) blocks;
	VEC(char *) strings;     /* media conditions */
	VEC(char *) imports;
	char *base;
};

/* ---- Text helpers ------------------------------------------------------------------------------------ */

static const char *skip_ws(const char *p)
{
	while (*p && is_space(*p))
		p++;
	return p;
}

static const char *skip_string(const char *p)
{
	char q = *p++;
	while (*p && *p != q) {
		if (*p == '\\' && p[1])
			p++;
		p++;
	}
	return *p ? p + 1 : p;
}

/* From just after a '{' to just after its matching '}'. */
static const char *skip_block(const char *p)
{
	int depth = 1;
	while (*p) {
		if (*p == '\\' && p[1]) {
			p += 2;
			continue;
		}
		if (*p == '"' || *p == '\'') {
			p = skip_string(p);
			continue;
		}
		if (*p == '{')
			depth++;
		else if (*p == '}' && --depth == 0)
			return p + 1;
		p++;
	}
	return p;
}

static char *strip_comments(const char *src, usize len)
{
	char *out = mem_alloc(len + 1);
	usize w = 0;
	for (usize i = 0; i < len;) {
		char c = src[i];
		if (c == '/' && i + 1 < len && src[i + 1] == '*') {
			i += 2;
			while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/'))
				i++;
			i += 2;
			out[w++] = ' ';
			continue;
		}
		if (c == '"' || c == '\'') {
			out[w++] = src[i++];
			while (i < len && src[i] != c && src[i] != '\n') {
				if (src[i] == '\\' && i + 1 < len)
					out[w++] = src[i++];
				out[w++] = src[i++];
			}
			if (i < len)
				out[w++] = src[i++];
			continue;
		}
		out[w++] = c ? c : ' ';
		i++;
	}
	out[w] = 0;
	return out;
}

static bool is_ident_char(int c)
{
	return is_alnum(c) || c == '-' || c == '_' || (u8)c >= 0x80 || c == '\\';
}

/* Reads an identifier with CSS escapes; returns the end. */
static const char *read_ident(const char *p, char *out, usize cap)
{
	usize n = 0;
	while (*p && is_ident_char(*p)) {
		char c = *p++;
		if (c == '\\' && *p) {
			if (is_xdigit(*p)) {
				u32 cp = 0;
				for (int k = 0; k < 6 && is_xdigit(*p); k++, p++)
					cp = cp * 16 + (u32)(is_digit(*p) ? *p - '0' : to_lower(*p) - 'a' + 10);
				if (*p == ' ')
					p++;
				char u[4];
				usize k = utf8_encode(cp, u);
				for (usize i = 0; i < k && n + 1 < cap; i++)
					out[n++] = u[i];
				continue;
			}
			c = *p++;
		}
		if (n + 1 < cap)
			out[n++] = c;
	}
	out[n] = 0;
	return p;
}

/* Splits at top-level separators (outside parentheses and quotes). Returns the count. */
static int split_top(const char *s, char sep, char out[][256], int max)
{
	int n = 0, depth = 0;
	usize w = 0;
	const char *p = s;
	for (;; p++) {
		char c = *p;
		bool end = !c;
		bool split = end || (depth == 0 && (sep == ' ' ? is_space(c) : c == sep));
		if (split) {
			out[n][w] = 0;
			if (w || sep != ' ') {
				char *t = str_trim(out[n]);
				if (t != out[n])
					memmove(out[n], t, strlen(t) + 1);
				if (out[n][0] || sep != ' ')
					n++;
			}
			w = 0;
			if (end || n >= max)
				break;
			continue;
		}
		if (c == '(')
			depth++;
		else if (c == ')' && depth > 0)
			depth--;
		else if (c == '"' || c == '\'') {
			const char *e = skip_string(p);
			while (p < e - 1 && w < 255)
				out[n][w++] = *p++;
			if (w < 255)
				out[n][w++] = *p;
			continue;
		}
		if (w < 255)
			out[n][w++] = c;
	}
	return n;
}

/* ---- Selectors ----------------------------------------------------------------------------------------- */

static void compound_free(Compound *c)
{
	mem_free(c->id);
	for (int i = 0; i < c->nclasses; i++)
		mem_free(c->classes[i]);
	mem_free(c->classes);
	for (int i = 0; i < c->nattrs; i++) {
		mem_free(c->attrs[i].name);
		mem_free(c->attrs[i].value);
	}
	mem_free(c->attrs);
	for (int i = 0; i < c->npseudos; i++) {
		for (int k = 0; k < c->pseudos[i].nargs; k++)
			compound_free(&c->pseudos[i].args[k]);
		mem_free(c->pseudos[i].args);
	}
	mem_free(c->pseudos);
}

static bool parse_nth(const char *s, int *a, int *b)
{
	s = skip_ws(s);
	if (str_starts(s, "odd")) {
		*a = 2, *b = 1;
		return true;
	}
	if (str_starts(s, "even")) {
		*a = 2, *b = 0;
		return true;
	}
	int sign = 1;
	if (*s == '+')
		s++;
	else if (*s == '-')
		sign = -1, s++;
	const char *e;
	i64 num = str_to_int(s, &e);
	bool had_num = e != s;
	s = e;
	if (*s == 'n' || *s == 'N') {
		*a = sign * (had_num ? (int)num : 1);
		s = skip_ws(s + 1);
		int bs = 1;
		if (*s == '+')
			s = skip_ws(s + 1);
		else if (*s == '-')
			bs = -1, s = skip_ws(s + 1);
		*b = bs * (int)str_to_int(s, NULL);
		return true;
	}
	if (!had_num)
		return false;
	*a = 0, *b = sign * (int)num;
	return true;
}

static bool parse_compound(const char **pp, Compound *c, u32 *spec);

/* :not(a, b) and :is(a, b): lists of compound selectors. */
static bool parse_compound_list(const char *s, Compound **out, int *n, u32 *spec_max)
{
	char parts[16][256];
	int np = split_top(s, ',', parts, 16);
	*out = NULL, *n = 0;
	*spec_max = 0;
	for (int i = 0; i < np; i++) {
		Compound c = { 0 };
		u32 spec = 0;
		const char *p = skip_ws(parts[i]);
		if (!parse_compound(&p, &c, &spec) || *skip_ws(p)) {
			compound_free(&c);
			continue;   /* complex selectors inside are skipped */
		}
		*out = mem_realloc(*out, sizeof(Compound) * (usize)(*n + 1));
		(*out)[(*n)++] = c;
		*spec_max = MAX(*spec_max, spec);
	}
	return *n > 0;
}

static bool parse_compound(const char **pp, Compound *c, u32 *spec)
{
	const char *p = *pp;
	char buf[256];
	bool any = false;
	if (*p == '*') {
		p++;
		any = true;
	} else if (is_ident_char(*p)) {
		p = read_ident(p, buf, sizeof buf);
		str_copy(c->tag, str_lower(buf), sizeof c->tag);
		*spec += 1;
		any = true;
	}
	if (*p == '|') {   /* namespaces: ignore the prefix */
		p++;
		if (*p == '*')
			p++, c->tag[0] = 0;
		else {
			p = read_ident(p, buf, sizeof buf);
			str_copy(c->tag, str_lower(buf), sizeof c->tag);
		}
	}
	for (;;) {
		if (*p == '#') {
			p = read_ident(p + 1, buf, sizeof buf);
			mem_free(c->id);
			c->id = str_dup(buf);
			*spec += 1 << 16;
		} else if (*p == '.') {
			p = read_ident(p + 1, buf, sizeof buf);
			if (!buf[0])
				return false;
			c->classes = mem_realloc(c->classes, sizeof(char *) * (usize)(c->nclasses + 1));
			c->classes[c->nclasses++] = str_dup(buf);
			*spec += 1 << 8;
		} else if (*p == '[') {
			p = skip_ws(p + 1);
			AttrSel a = { 0 };
			p = read_ident(p, buf, sizeof buf);
			if (*p == '|' && p[1] != '=') {
				p = read_ident(p + 1, buf, sizeof buf);
			}
			a.name = str_dup(str_lower(buf));
			p = skip_ws(p);
			if (*p && str_chr("~|^$*", *p) && p[1] == '=')
				a.op = (u8)*p, p += 2;
			else if (*p == '=')
				a.op = '=', p++;
			if (a.op) {
				p = skip_ws(p);
				if (*p == '"' || *p == '\'') {
					const char *e = skip_string(p);
					a.value = str_ndup(p + 1, (usize)(e - p - 2 > 0 ? e - p - 2 : 0));
					p = e;
				} else {
					p = read_ident(p, buf, sizeof buf);
					a.value = str_dup(buf);
				}
				p = skip_ws(p);
				if (*p == 'i' || *p == 'I')
					a.nocase = true, p++;
				else if (*p == 's' || *p == 'S')
					p++;
				p = skip_ws(p);
			}
			if (*p != ']') {
				mem_free(a.name);
				mem_free(a.value);
				return false;
			}
			p++;
			c->attrs = mem_realloc(c->attrs, sizeof(AttrSel) * (usize)(c->nattrs + 1));
			c->attrs[c->nattrs++] = a;
			*spec += 1 << 8;
		} else if (*p == ':') {
			p++;
			if (*p == ':')
				return false;   /* pseudo-elements aren't generated */
			p = read_ident(p, buf, sizeof buf);
			str_lower(buf);
			PseudoSel ps = { 0 };
			char arg[512] = "";
			if (*p == '(') {
				int depth = 1;
				const char *s = ++p;
				while (*p && depth) {
					if (*p == '(')
						depth++;
					else if (*p == ')')
						depth--;
					if (depth)
						p++;
				}
				str_copy(arg, s, MIN(sizeof arg, (usize)(p - s) + 1));
				if (*p)
					p++;
			}
			static const char *const elements[] = { "before", "after", "first-line", "first-letter", "marker", "placeholder", "selection" };
			for (usize i = 0; i < ARRAY_LEN(elements); i++)
				if (str_eq(buf, elements[i]))
					return false;
			u32 add = 1 << 8;
			if (str_eq(buf, "first-child")) ps.kind = PC_FIRST_CHILD;
			else if (str_eq(buf, "last-child")) ps.kind = PC_LAST_CHILD;
			else if (str_eq(buf, "only-child")) ps.kind = PC_ONLY_CHILD;
			else if (str_eq(buf, "first-of-type")) ps.kind = PC_FIRST_OF_TYPE;
			else if (str_eq(buf, "last-of-type")) ps.kind = PC_LAST_OF_TYPE;
			else if (str_eq(buf, "only-of-type")) ps.kind = PC_ONLY_OF_TYPE;
			else if (str_eq(buf, "nth-child") || str_eq(buf, "nth-last-child") || str_eq(buf, "nth-of-type") || str_eq(buf, "nth-last-of-type")) {
				ps.kind = str_eq(buf, "nth-child") ? PC_NTH_CHILD : str_eq(buf, "nth-last-child") ? PC_NTH_LAST_CHILD
					  : str_eq(buf, "nth-of-type")                         ? PC_NTH_OF_TYPE
											      : PC_NTH_LAST_OF_TYPE;
				if (!parse_nth(arg, &ps.a, &ps.b))
					ps.kind = PC_NEVER;
			} else if (str_eq(buf, "not") || str_eq(buf, "is") || str_eq(buf, "where") || str_eq(buf, "matches") || str_eq(buf, "-webkit-any") || str_eq(buf, "-moz-any")) {
				bool not = str_eq(buf, "not");
				u32 inner = 0;
				if (parse_compound_list(arg, &ps.args, &ps.nargs, &inner)) {
					ps.kind = not ? PC_NOT : PC_IS;
					add = str_eq(buf, "where") ? 0 : inner;
				} else {
					ps.kind = not ? PC_ALWAYS : PC_NEVER;
					add = 0;
				}
			} else if (str_eq(buf, "link") || str_eq(buf, "any-link")) ps.kind = PC_LINK;
			else if (str_eq(buf, "root")) ps.kind = PC_ROOT;
			else if (str_eq(buf, "empty")) ps.kind = PC_EMPTY;
			else if (str_eq(buf, "checked")) ps.kind = PC_CHECKED;
			else if (str_eq(buf, "disabled")) ps.kind = PC_DISABLED;
			else if (str_eq(buf, "enabled")) ps.kind = PC_ENABLED;
			else if (str_eq(buf, "lang") || str_eq(buf, "dir") || str_eq(buf, "defined") || str_eq(buf, "scope")) ps.kind = PC_ALWAYS;
			else ps.kind = PC_NEVER;   /* :hover, :focus, :visited and friends */
			*spec += add;
			c->pseudos = mem_realloc(c->pseudos, sizeof(PseudoSel) * (usize)(c->npseudos + 1));
			c->pseudos[c->npseudos++] = ps;
		} else {
			break;
		}
		any = true;
	}
	*pp = p;
	return any;
}

static bool parse_selector(const char *s, Rule *r)
{
	const char *p = skip_ws(s);
	char comb = 0;
	while (*p) {
		Compound c = { 0 };
		c.comb = comb;
		if (!parse_compound(&p, &c, &r->spec)) {
			compound_free(&c);
			return false;
		}
		r->parts = mem_realloc(r->parts, sizeof(Compound) * (usize)(r->nparts + 1));
		r->parts[r->nparts++] = c;
		const char *q = skip_ws(p);
		if (!*q)
			break;
		if (*q == '>' || *q == '+' || *q == '~') {
			comb = *q;
			p = skip_ws(q + 1);
		} else if (q != p) {
			comb = ' ';
			p = q;
		} else {
			return false;   /* something we don't understand */
		}
	}
	return r->nparts > 0;
}

static bool match_compound(const Compound *c, const Node *n);

static bool attr_matches(const AttrSel *a, const Node *n)
{
	const char *v = node_attr(n, a->name);
	if (!v)
		return false;
	if (!a->op)
		return true;
	const char *want = a->value;
	usize wl = strlen(want), vl = strlen(v);
	bool (*eqn)(const char *, const char *, usize) = a->nocase ? str_eq_nocase_n : NULL;
#define EQN(x, y, k) (eqn ? eqn(x, y, k) : memcmp(x, y, k) == 0)
	switch (a->op) {
	case '=':
		return vl == wl && EQN(v, want, wl);
	case '^':
		return wl && vl >= wl && EQN(v, want, wl);
	case '$':
		return wl && vl >= wl && EQN(v + vl - wl, want, wl);
	case '*':
		return wl && (a->nocase ? str_find_nocase(v, want) : str_find(v, want)) != NULL;
	case '|':
		return (vl == wl && EQN(v, want, wl)) || (vl > wl && EQN(v, want, wl) && v[wl] == '-');
	case '~':
		for (const char *p = v; *p;) {
			while (*p && is_space(*p))
				p++;
			const char *s = p;
			while (*p && !is_space(*p))
				p++;
			if ((usize)(p - s) == wl && wl && EQN(s, want, wl))
				return true;
		}
		return false;
	}
#undef EQN
	return false;
}

static bool is_el(const Node *n)
{
	return n && n->type == NODE_ELEMENT && n->tag[0] != '#';
}

static bool nth(int a, int b, int pos)
{
	if (a == 0)
		return pos == b;
	int k = pos - b;
	return k % a == 0 && k / a >= 0;
}

static bool pseudo_matches(const PseudoSel *ps, const Node *n)
{
	switch (ps->kind) {
	case PC_FIRST_CHILD:
		return n->index == 0;
	case PC_LAST_CHILD:
	case PC_ONLY_CHILD:
		for (Node *s = n->next; s; s = s->next)
			if (s->type == NODE_ELEMENT)
				return false;
		return ps->kind == PC_LAST_CHILD || n->index == 0;
	case PC_NTH_CHILD:
		return nth(ps->a, ps->b, n->index + 1);
	case PC_NTH_LAST_CHILD: {
		int k = 1;
		for (Node *s = n->next; s; s = s->next)
			k += s->type == NODE_ELEMENT;
		return nth(ps->a, ps->b, k);
	}
	case PC_FIRST_OF_TYPE:
	case PC_LAST_OF_TYPE:
	case PC_ONLY_OF_TYPE:
	case PC_NTH_OF_TYPE:
	case PC_NTH_LAST_OF_TYPE: {
		int before = 0, after = 0;
		for (Node *s = n->prev; s; s = s->prev)
			before += s->type == NODE_ELEMENT && str_eq(s->tag, n->tag);
		if (ps->kind == PC_FIRST_OF_TYPE)
			return before == 0;
		if (ps->kind == PC_NTH_OF_TYPE)
			return nth(ps->a, ps->b, before + 1);
		for (Node *s = n->next; s; s = s->next)
			after += s->type == NODE_ELEMENT && str_eq(s->tag, n->tag);
		if (ps->kind == PC_LAST_OF_TYPE)
			return after == 0;
		if (ps->kind == PC_ONLY_OF_TYPE)
			return before == 0 && after == 0;
		return nth(ps->a, ps->b, after + 1);
	}
	case PC_NOT:
		for (int i = 0; i < ps->nargs; i++)
			if (match_compound(&ps->args[i], n))
				return false;
		return true;
	case PC_IS:
		for (int i = 0; i < ps->nargs; i++)
			if (match_compound(&ps->args[i], n))
				return true;
		return false;
	case PC_LINK:
		return (str_eq(n->tag, "a") || str_eq(n->tag, "area")) && node_attr(n, "href");
	case PC_ROOT:
		return !is_el(n->parent);
	case PC_EMPTY:
		for (Node *c = n->first; c; c = c->next)
			if (c->type == NODE_ELEMENT || (c->text && c->text[0]))
				return false;
		return true;
	case PC_CHECKED:
		return n->control ? n->control->checked : node_attr(n, "checked") || node_attr(n, "selected");
	case PC_DISABLED:
		return node_attr(n, "disabled") != NULL;
	case PC_ENABLED:
		return node_attr(n, "disabled") == NULL;
	case PC_ALWAYS:
		return true;
	}
	return false;
}

static bool match_compound(const Compound *c, const Node *n)
{
	if (c->tag[0] && !str_eq(c->tag, n->tag))
		return false;
	if (c->id) {
		const char *id = node_attr(n, "id");
		if (!id || !str_eq(id, c->id))
			return false;
	}
	for (int i = 0; i < c->nclasses; i++)
		if (!node_has_class(n, c->classes[i]))
			return false;
	for (int i = 0; i < c->nattrs; i++)
		if (!attr_matches(&c->attrs[i], n))
			return false;
	for (int i = 0; i < c->npseudos; i++)
		if (!pseudo_matches(&c->pseudos[i], n))
			return false;
	return true;
}

static bool match_from(const Rule *r, int i, const Node *n)
{
	if (!match_compound(&r->parts[i], n))
		return false;
	if (i == 0)
		return true;
	switch (r->parts[i].comb) {
	case '>':
		return is_el(n->parent) && match_from(r, i - 1, n->parent);
	case ' ':
		for (const Node *a = n->parent; is_el(a); a = a->parent)
			if (match_from(r, i - 1, a))
				return true;
		return false;
	case '+':
		for (const Node *s = n->prev; s; s = s->prev)
			if (s->type == NODE_ELEMENT)
				return match_from(r, i - 1, s);
		return false;
	case '~':
		for (const Node *s = n->prev; s; s = s->prev)
			if (s->type == NODE_ELEMENT && match_from(r, i - 1, s))
				return true;
		return false;
	}
	return false;
}

/* ---- Parsing sheets ----------------------------------------------------------------------------------------- */

static int parse_decls(const char *s, Decl **out)
{
	VEC(Decl) v = { 0 };
	props_init();
	const char *p = s;
	while (*p) {
		p = skip_ws(p);
		const char *start = p;
		int depth = 0;
		bool nested = false;
		while (*p && !(depth == 0 && *p == ';')) {
			if (*p == '\\' && p[1]) {
				p += 2;
				continue;
			}
			if (*p == '"' || *p == '\'') {
				p = skip_string(p);
				continue;
			}
			if (*p == '(')
				depth++;
			else if (*p == ')' && depth)
				depth--;
			else if (*p == '{') {
				/* A nested rule: skip it. */
				p = skip_block(p + 1);
				nested = true;
				break;
			}
			p++;
		}
		const char *stop = p;
		if (*p == ';')
			p++;
		if (nested)
			continue;
		const char *colon = start;
		while (colon < stop && *colon != ':')
			colon++;
		if (colon >= stop)
			continue;
		char name[96];
		usize nl = (usize)(colon - start);
		while (nl && is_space(start[nl - 1]))
			nl--;
		if (!nl || nl >= sizeof name)
			continue;
		memcpy(name, start, nl);
		name[nl] = 0;
		char *value = str_ndup(colon + 1, (usize)(stop - colon - 1));
		char *t = str_trim(value);
		bool important = false;
		usize vl = strlen(t);
		const char *bang = vl ? str_rchr(t, '!') : NULL;
		if (bang && str_eq_nocase(skip_ws(bang + 1), "important")) {
			important = true;
			vl = (usize)(bang - t);
			while (vl && is_space(t[vl - 1]))
				vl--;
			t[vl] = 0;
		}
		Decl d = { .important = important };
		if (name[0] == '-' && name[1] == '-') {
			d.prop = P_CUSTOM;
			d.name = str_dup(name);
		} else {
			str_lower(name);
			uintptr_t idx = (uintptr_t)map_get(&prop_map, name);
			if (!idx) {
				mem_free(value);
				continue;
			}
			d.prop = PROPS[idx - 1].prop;
			d.sides = PROPS[idx - 1].sides;
		}
		d.value = str_dup(t);
		mem_free(value);
		vec_push(&v, d);
	}
	*out = v.data;
	return (int)v.len;
}

static void decls_free(Decl *d, int n)
{
	for (int i = 0; i < n; i++) {
		mem_free(d[i].name);
		mem_free(d[i].value);
	}
	mem_free(d);
}

static const char *join_media(Sheet *sh, const char *outer, const char *cond)
{
	char *c = outer ? str_fmt("%s&&%s", outer, cond) : str_dup(cond);
	vec_push(&sh->strings, c);
	return c;
}

static char *read_url(const char *p);

/* url(...) in a sheet is relative to the sheet, not the page: make it absolute now. */
static char *absolute_urls(const char *v, const char *base)
{
	StrBuf sb = { 0 };
	const char *p = v;
	for (;;) {
		const char *u = str_find(p, "url(");
		if (!u) {
			sb_adds(&sb, p);
			break;
		}
		sb_add(&sb, p, (usize)(u - p));
		char *raw = read_url(u);
		const char *close = str_chr(u, ')');
		if (!raw || !close) {
			mem_free(raw);
			sb_adds(&sb, u);
			break;
		}
		if (str_starts(raw, "data:") || str_starts(raw, "#")) {
			sb_add(&sb, u, (usize)(close - u + 1));
		} else {
			char *abs = url_resolve(base, raw);
			sb_fmt(&sb, "url(\"%s\")", abs);
			mem_free(abs);
		}
		mem_free(raw);
		/* a quoted data: url can contain ')' — find the real end */
		const char *q = skip_ws(u + 4);
		if (*q == '"' || *q == '\'') {
			q = skip_string(q);
			close = str_chr(q, ')');
			if (!close)
				break;
		}
		p = close + 1;
	}
	return sb_take(&sb);
}

static void parse_rule(Sheet *sh, const char *prelude, const char *body, usize blen, const char *media)
{
	char *b = str_ndup(body, blen);
	Decl *decls;
	int nd = parse_decls(b, &decls);
	mem_free(b);
	if (sh->base[0])
		for (int i = 0; i < nd; i++)
			if (str_find(decls[i].value, "url(")) {
				char *v = absolute_urls(decls[i].value, sh->base);
				mem_free(decls[i].value);
				decls[i].value = v;
			}
	if (!nd) {
		mem_free(decls);
		return;
	}
	DeclBlock *blk = NEW(DeclBlock);
	blk->decls = decls;
	blk->n = nd;
	char parts[64][256];
	int np = split_top(prelude, ',', parts, 64);
	bool used = false;
	for (int i = 0; i < np; i++) {
		Rule r = { .block = blk, .media = media };
		if (!parse_selector(parts[i], &r)) {
			for (int k = 0; k < r.nparts; k++)
				compound_free(&r.parts[k]);
			mem_free(r.parts);
			continue;
		}
		r.order = (u32)sh->rules.len;
		vec_push(&sh->rules, r);
		used = true;
	}
	if (used) {
		vec_push(&sh->blocks, blk);
	} else {
		decls_free(blk->decls, blk->n);
		mem_free(blk);
	}
}

static char *read_url(const char *p)
{
	p = skip_ws(p);
	if (str_starts(p, "url(")) {
		p = skip_ws(p + 4);
		if (*p == '"' || *p == '\'') {
			const char *e = skip_string(p);
			return str_ndup(p + 1, (usize)MAX(0, e - p - 2));
		}
		const char *e = p;
		while (*e && *e != ')' && !is_space(*e))
			e++;
		return str_ndup(p, (usize)(e - p));
	}
	if (*p == '"' || *p == '\'') {
		const char *e = skip_string(p);
		return str_ndup(p + 1, (usize)MAX(0, e - p - 2));
	}
	return NULL;
}

static bool supports_ok(const char *cond)
{
	cond = skip_ws(cond);
	if (str_starts(cond, "not") || str_find(cond, "selector("))
		return false;
	return true;
}

static void parse_rules(Sheet *sh, const char **pp, const char *media, bool nested);

static void parse_at(Sheet *sh, const char **pp, const char *media)
{
	const char *p = *pp + 1;
	char name[64];
	p = read_ident(p, name, sizeof name);
	str_lower(name);
	const char *s = p;
	while (*p && *p != '{' && *p != ';') {
		if (*p == '"' || *p == '\'')
			p = skip_string(p);
		else
			p++;
	}
	char *prelude = str_ndup(s, (usize)(p - s));
	char *pre = str_trim(prelude);
	if (*p == ';' || !*p) {
		if (str_eq(name, "import")) {
			char *url = read_url(pre);
			if (url) {
				vec_push(&sh->imports, url_resolve(sh->base, url));
				mem_free(url);
			}
		}
		if (*p)
			p++;
	} else {
		p++;   /* past '{' */
		if (str_eq(name, "media")) {
			parse_rules(sh, &p, join_media(sh, media, pre), true);
		} else if (str_eq(name, "supports")) {
			if (supports_ok(pre))
				parse_rules(sh, &p, media, true);
			else
				p = skip_block(p);
		} else if (str_eq(name, "layer") || str_eq(name, "document") || str_eq(name, "-moz-document") || str_eq(name, "scope")) {
			parse_rules(sh, &p, media, true);
		} else {
			p = skip_block(p);   /* @font-face, @keyframes, @page, @container... */
		}
	}
	mem_free(prelude);
	*pp = p;
}

static void parse_rules(Sheet *sh, const char **pp, const char *media, bool nested)
{
	const char *p = *pp;
	for (;;) {
		p = skip_ws(p);
		if (!*p)
			break;
		if (*p == '}') {
			p++;
			if (nested)
				break;
			continue;
		}
		if (*p == '@') {
			parse_at(sh, &p, media);
			continue;
		}
		if (*p == '<' || *p == '-') {   /* <!-- and --> around old inline sheets */
			if (str_starts(p, "<!--")) {
				p += 4;
				continue;
			}
			if (str_starts(p, "-->")) {
				p += 3;
				continue;
			}
		}
		const char *s = p;
		while (*p && *p != '{' && *p != '}') {
			if (*p == '\\' && p[1])
				p += 2;
			else if (*p == '"' || *p == '\'')
				p = skip_string(p);
			else
				p++;
		}
		if (*p != '{')
			continue;
		char *prelude = str_ndup(s, (usize)(p - s));
		const char *body = ++p;
		p = skip_block(p);
		usize blen = (usize)(p - body);
		if (blen && body[blen - 1] == '}')
			blen--;
		parse_rule(sh, prelude, body, blen, media);
		mem_free(prelude);
	}
	*pp = p;
}

Sheet *css_parse(const char *src, usize len, const char *base_url)
{
	Sheet *sh = NEW(Sheet);
	sh->base = str_dup(base_url ? base_url : "");
	char *text = strip_comments(src, len);
	const char *p = text;
	parse_rules(sh, &p, NULL, false);
	mem_free(text);
	return sh;
}

void sheet_free(Sheet *sh)
{
	if (!sh)
		return;
	vec_each(&sh->rules, r)
	{
		for (int k = 0; k < r->nparts; k++)
			compound_free(&r->parts[k]);
		mem_free(r->parts);
	}
	vec_free(&sh->rules);
	vec_each(&sh->blocks, b)
	{
		decls_free((*b)->decls, (*b)->n);
		mem_free(*b);
	}
	vec_free(&sh->blocks);
	vec_each(&sh->strings, s) mem_free(*s);
	vec_free(&sh->strings);
	vec_each(&sh->imports, s) mem_free(*s);
	vec_free(&sh->imports);
	mem_free(sh->base);
	mem_free(sh);
}

int sheet_imports(const Sheet *s, char ***urls)
{
	*urls = s->imports.data;
	return (int)s->imports.len;
}

/* ---- Media queries ---------------------------------------------------------------------------------------- */

static f32 media_len(const char *v)
{
	const char *e;
	f64 x = str_to_float(skip_ws(v), &e);
	if (str_starts(e, "em") || str_starts(e, "rem"))
		x *= 16;
	return (f32)x;
}

static bool media_feature(const char *f, f32 vw, f32 vh)
{
	/* f is the inside of the parentheses */
	char name[64] = "";
	const char *p = skip_ws(f);
	usize n = 0;
	while (*p && (is_alnum(*p) || *p == '-') && n < sizeof name - 1)
		name[n++] = (char)to_lower(*p++);
	name[n] = 0;
	p = skip_ws(p);
	if (*p == '<' || *p == '>' || *p == '=') {
		/* Range syntax: (width >= 600px) */
		bool lt = *p == '<', gt = *p == '>';
		bool eq = p[1] == '=' || *p == '=';
		p += p[1] == '=' ? 2 : 1;
		f32 x = media_len(p), v = str_eq(name, "width") ? vw : str_eq(name, "height") ? vh : -1;
		if (v < 0)
			return false;
		return lt ? (eq ? v <= x : v < x) : gt ? (eq ? v >= x : v > x) : v == x;
	}
	const char *val = *p == ':' ? skip_ws(p + 1) : NULL;
	if (str_eq(name, "min-width")) return val && vw >= media_len(val);
	if (str_eq(name, "max-width")) return val && vw <= media_len(val);
	if (str_eq(name, "min-height")) return val && vh >= media_len(val);
	if (str_eq(name, "max-height")) return val && vh <= media_len(val);
	if (str_eq(name, "min-device-width")) return val && vw >= media_len(val);
	if (str_eq(name, "max-device-width")) return val && vw <= media_len(val);
	if (str_eq(name, "orientation")) return val && (str_starts(val, "landscape") ? vw >= vh : vw < vh);
	if (str_eq(name, "prefers-color-scheme")) return val && str_starts(val, "light");
	if (str_eq(name, "hover") || str_eq(name, "any-hover")) return !val || str_starts(val, "hover");
	if (str_eq(name, "pointer") || str_eq(name, "any-pointer")) return !val || str_starts(val, "fine");
	if (str_eq(name, "color")) return true;
	if (str_eq(name, "prefers-reduced-motion")) return val && str_starts(val, "no-preference");
	return false;
}

/* One query: [not|only] type [and (feature)]... */
static bool media_query(const char *q, f32 vw, f32 vh)
{
	const char *p = skip_ws(q);
	bool negate = false;
	if (str_starts(p, "not ")) {
		negate = true;
		p = skip_ws(p + 4);
	} else if (str_starts(p, "only ")) {
		p = skip_ws(p + 5);
	}
	bool ok = true;
	while (*p) {
		if (*p == '(') {
			int depth = 1;
			const char *s = ++p;
			while (*p && depth) {
				if (*p == '(')
					depth++;
				else if (*p == ')')
					depth--;
				if (depth)
					p++;
			}
			char inner[256];
			str_copy(inner, s, MIN(sizeof inner, (usize)(p - s) + 1));
			if (*p)
				p++;
			bool r;
			if (str_find(inner, " or ")) {
				r = false;   /* (a) or (b) inside a group is rare: treat each as its own query */
				char parts[8][256];
				int np = split_top(inner, ' ', parts, 8);
				for (int i = 0; i < np; i++)
					if (parts[i][0] == '(') {
						parts[i][strlen(parts[i]) - 1] = 0;
						r |= media_feature(parts[i] + 1, vw, vh);
					}
			} else if (inner[0] == '(') {
				r = media_query(inner, vw, vh);
			} else {
				r = media_feature(inner, vw, vh);
			}
			ok = ok && r;
		} else if (is_alpha(*p)) {
			char word[32];
			usize n = 0;
			while (*p && (is_alnum(*p) || *p == '-') && n < sizeof word - 1)
				word[n++] = (char)to_lower(*p++);
			word[n] = 0;
			if (str_eq(word, "and") || str_eq(word, "only"))
				continue;
			if (str_eq(word, "not")) {
				negate = !negate;
				continue;
			}
			if (str_eq(word, "screen") || str_eq(word, "all"))
				continue;
			ok = false;   /* print, speech, tv... */
		} else {
			p++;
		}
	}
	return negate ? !ok : ok;
}

bool css_media_matches(const char *media, f32 vw, f32 vh)
{
	if (!media || !*skip_ws(media))
		return true;
	/* "a&&b" means both (nested @media); commas mean either. */
	const char *amp = str_find(media, "&&");
	if (amp) {
		char *first = str_ndup(media, (usize)(amp - media));
		bool r = css_media_matches(first, vw, vh) && css_media_matches(amp + 2, vw, vh);
		mem_free(first);
		return r;
	}
	char parts[16][256];
	int n = split_top(media, ',', parts, 16);
	for (int i = 0; i < n; i++)
		if (media_query(parts[i], vw, vh))
			return true;
	return false;
}

/* ---- Values -------------------------------------------------------------------------------------------------- */

static const char NAMED_COLORS[] =
	"aliceblue f0f8ff antiquewhite faebd7 aqua 00ffff aquamarine 7fffd4 azure f0ffff beige f5f5dc bisque ffe4c4 black 000000 "
	"blanchedalmond ffebcd blue 0000ff blueviolet 8a2be2 brown a52a2a burlywood deb887 cadetblue 5f9ea0 chartreuse 7fff00 "
	"chocolate d2691e coral ff7f50 cornflowerblue 6495ed cornsilk fff8dc crimson dc143c cyan 00ffff darkblue 00008b "
	"darkcyan 008b8b darkgoldenrod b8860b darkgray a9a9a9 darkgreen 006400 darkgrey a9a9a9 darkkhaki bdb76b darkmagenta 8b008b "
	"darkolivegreen 556b2f darkorange ff8c00 darkorchid 9932cc darkred 8b0000 darksalmon e9967a darkseagreen 8fbc8f "
	"darkslateblue 483d8b darkslategray 2f4f4f darkslategrey 2f4f4f darkturquoise 00ced1 darkviolet 9400d3 deeppink ff1493 "
	"deepskyblue 00bfff dimgray 696969 dimgrey 696969 dodgerblue 1e90ff firebrick b22222 floralwhite fffaf0 forestgreen 228b22 "
	"fuchsia ff00ff gainsboro dcdcdc ghostwhite f8f8ff gold ffd700 goldenrod daa520 gray 808080 green 008000 greenyellow adff2f "
	"grey 808080 honeydew f0fff0 hotpink ff69b4 indianred cd5c5c indigo 4b0082 ivory fffff0 khaki f0e68c lavender e6e6fa "
	"lavenderblush fff0f5 lawngreen 7cfc00 lemonchiffon fffacd lightblue add8e6 lightcoral f08080 lightcyan e0ffff "
	"lightgoldenrodyellow fafad2 lightgray d3d3d3 lightgreen 90ee90 lightgrey d3d3d3 lightpink ffb6c1 lightsalmon ffa07a "
	"lightseagreen 20b2aa lightskyblue 87cefa lightslategray 778899 lightslategrey 778899 lightsteelblue b0c4de "
	"lightyellow ffffe0 lime 00ff00 limegreen 32cd32 linen faf0e6 magenta ff00ff maroon 800000 mediumaquamarine 66cdaa "
	"mediumblue 0000cd mediumorchid ba55d3 mediumpurple 9370db mediumseagreen 3cb371 mediumslateblue 7b68ee "
	"mediumspringgreen 00fa9a mediumturquoise 48d1cc mediumvioletred c71585 midnightblue 191970 mintcream f5fffa "
	"mistyrose ffe4e1 moccasin ffe4b5 navajowhite ffdead navy 000080 oldlace fdf5e6 olive 808000 olivedrab 6b8e23 "
	"orange ffa500 orangered ff4500 orchid da70d6 palegoldenrod eee8aa palegreen 98fb98 paleturquoise afeeee "
	"palevioletred db7093 papayawhip ffefd5 peachpuff ffdab9 peru cd853f pink ffc0cb plum dda0dd powderblue b0e0e6 "
	"purple 800080 rebeccapurple 663399 red ff0000 rosybrown bc8f8f royalblue 4169e1 saddlebrown 8b4513 salmon fa8072 "
	"sandybrown f4a460 seagreen 2e8b57 seashell fff5ee sienna a0522d silver c0c0c0 skyblue 87ceeb slateblue 6a5acd "
	"slategray 708090 slategrey 708090 snow fffafa springgreen 00ff7f steelblue 4682b4 tan d2b48c teal 008080 "
	"thistle d8bfd8 tomato ff6347 turquoise 40e0d0 violet ee82ee wheat f5deb3 white ffffff whitesmoke f5f5f5 yellow ffff00 "
	"yellowgreen 9acd32 canvas ffffff canvastext 000000 linktext 1a5fb4 visitedtext 613583 buttonface efefef "
	"buttontext 000000 buttonborder 767676 field ffffff fieldtext 000000 graytext 808080 highlight 3584e4 "
	"highlighttext ffffff window ffffff windowtext 000000 ";

static f32 num_arg(const char *s, f32 pct_scale, bool *pct)
{
	const char *e;
	f32 v = (f32)str_to_float(skip_ws(s), &e);
	if (*e == '%') {
		if (pct)
			*pct = true;
		return v * pct_scale / 100;
	}
	if (str_starts(e, "deg"))
		return v;
	if (str_starts(e, "turn"))
		return v * 360;
	if (str_starts(e, "rad"))
		return v * 180 / PI_F;
	return v;
}

/* Splits the arguments of rgb()/hsl(), with commas or spaces and an optional "/ alpha". */
static int color_args(const char *s, f32 *out, char kinds[4])
{
	const char *open = str_chr(s, '(');
	if (!open)
		return 0;
	char inner[256];
	str_copy(inner, open + 1, sizeof inner);
	char *close = (char *)str_rchr(inner, ')');
	if (close)
		*close = 0;
	for (char *p = inner; *p; p++)
		if (*p == ',' || *p == '/')
			*p = ' ';
	char parts[6][256];
	int n = split_top(inner, ' ', parts, 6);
	for (int i = 0; i < n && i < 4; i++) {
		kinds[i] = str_chr(parts[i], '%') ? '%' : 'n';
		out[i] = (f32)str_to_float(parts[i], NULL);
		if (str_find(parts[i], "turn"))
			out[i] *= 360;
	}
	return MIN(n, 4);
}

bool css_parse_color(const char *v, Color *out)
{
	char s[256];
	str_copy(s, skip_ws(v), sizeof s);
	str_trim(s);
	if (s[0] == '#') {
		usize n = strlen(s + 1);
		for (usize i = 1; i <= n; i++)
			if (!is_xdigit(s[i]))
				return false;
		if (n == 3 || n == 4 || n == 6 || n == 8) {
			*out = color_hex(s);
			if (n == 4) {
				/* #rgba */
				u32 a = (u32)(is_digit(s[4]) ? s[4] - '0' : to_lower(s[4]) - 'a' + 10) * 17;
				char t[5] = { '#', s[1], s[2], s[3], 0 };
				*out = color_with_alpha(color_hex(t), (u8)a);
			}
			return true;
		}
		return false;
	}
	str_lower(s);
	if (str_starts(s, "rgb")) {
		f32 a[4] = { 0, 0, 0, 1 };
		char k[4] = { 0 };
		int n = color_args(s, a, k);
		if (n < 3)
			return false;
		for (int i = 0; i < 3; i++)
			if (k[i] == '%')
				a[i] = a[i] * 255 / 100;
		if (n == 4 && k[3] == '%')
			a[3] /= 100;
		*out = RGBA((u8)clampf(a[0], 0, 255), (u8)clampf(a[1], 0, 255), (u8)clampf(a[2], 0, 255), (u8)(clampf(a[3], 0, 1) * 255 + 0.5f));
		return true;
	}
	if (str_starts(s, "hsl")) {
		f32 a[4] = { 0, 0, 0, 1 };
		char k[4] = { 0 };
		int n = color_args(s, a, k);
		if (n < 3)
			return false;
		if (n == 4 && k[3] == '%')
			a[3] /= 100;
		*out = color_hsl(fmodf(a[0] + 360, 360), clampf(a[1] / 100, 0, 1), clampf(a[2] / 100, 0, 1), (u8)(clampf(a[3], 0, 1) * 255 + 0.5f));
		return true;
	}
	if (str_starts(s, "light-dark(")) {
		char parts[2][256];
		char inner[256];
		str_copy(inner, s + 11, sizeof inner);
		char *close = (char *)str_rchr(inner, ')');
		if (close)
			*close = 0;
		return split_top(inner, ',', parts, 2) >= 1 && css_parse_color(parts[0], out);
	}
	if (str_eq(s, "transparent")) {
		*out = COLOR_CLEAR;
		return true;
	}
	usize l = strlen(s);
	if (!l || l > 24)
		return false;
	for (const char *p = NAMED_COLORS; *p;) {
		const char *sp = str_chr(p, ' ');
		if ((usize)(sp - p) == l && memcmp(p, s, l) == 0) {
			char hex[8] = "#";
			memcpy(hex + 1, sp + 1, 6);
			hex[7] = 0;
			*out = color_hex(hex);
			return true;
		}
		p = sp + 8;
	}
	return false;
}

typedef struct {
	f32 vw, vh, rem;
} Ctx;

/* calc(): values are px plus a percentage. */
typedef struct {
	f32 px, pct;
	bool has_pct, number;
} CalcVal;

static bool calc_expr(const char **pp, f32 em, const Ctx *c, CalcVal *out);

static bool calc_atom(const char **pp, f32 em, const Ctx *c, CalcVal *out)
{
	const char *p = skip_ws(*pp);
	*out = (CalcVal){ 0 };
	if (*p == '(') {
		p++;
		if (!calc_expr(&p, em, c, out))
			return false;
		p = skip_ws(p);
		if (*p == ')')
			p++;
		*pp = p;
		return true;
	}
	if (str_starts(p, "calc(")) {
		p += 4;
		return calc_atom(&p, em, c, out) && (*pp = p, true);
	}
	if (str_starts(p, "min(") || str_starts(p, "max(") || str_starts(p, "clamp(")) {
		bool is_min = p[1] == 'i', is_clamp = p[0] == 'c';
		p = str_chr(p, '(') + 1;
		CalcVal vals[3];
		int n = 0;
		for (;;) {
			CalcVal v;
			if (!calc_expr(&p, em, c, &v))
				return false;
			if (n < 3)
				vals[n++] = v;
			p = skip_ws(p);
			if (*p == ',') {
				p++;
				continue;
			}
			if (*p == ')')
				p++;
			break;
		}
		/* With percentages we can't compare yet: prefer the pixel bound, which is
		 * what these usually express (a cap on a fluid width). */
		if (is_clamp && n == 3) {
			CalcVal v = vals[1];
			if (!v.has_pct && !vals[0].has_pct && !vals[2].has_pct)
				v.px = clampf(v.px, vals[0].px, vals[2].px);
			else if (v.has_pct)
				v = vals[2].has_pct ? vals[0] : vals[2];
			*out = v;
		} else {
			*out = vals[0];
			for (int i = 1; i < n; i++) {
				if (out->has_pct != vals[i].has_pct) {
					if (out->has_pct)
						*out = vals[i];
					continue;
				}
				bool less = vals[i].px + vals[i].pct < out->px + out->pct;
				if (less == is_min)
					*out = vals[i];
			}
		}
		*pp = p;
		return true;
	}
	const char *e;
	f64 x = str_to_float(p, &e);
	if (e == p)
		return false;
	p = e;
	if (*p == '%') {
		out->pct = (f32)x, out->has_pct = true;
		p++;
	} else if (is_alpha(*p)) {
		char unit[8];
		usize n = 0;
		while (is_alpha(*p) && n < sizeof unit - 1)
			unit[n++] = (char)to_lower(*p++);
		unit[n] = 0;
		f32 px;
		if (str_eq(unit, "px")) px = (f32)x;
		else if (str_eq(unit, "em")) px = (f32)x * em;
		else if (str_eq(unit, "rem")) px = (f32)x * c->rem;
		else if (str_eq(unit, "vw")) px = (f32)x * c->vw / 100;
		else if (str_eq(unit, "vh") || str_eq(unit, "svh") || str_eq(unit, "dvh") || str_eq(unit, "lvh")) px = (f32)x * c->vh / 100;
		else if (str_eq(unit, "vmin")) px = (f32)x * minf(c->vw, c->vh) / 100;
		else if (str_eq(unit, "vmax")) px = (f32)x * maxf(c->vw, c->vh) / 100;
		else if (str_eq(unit, "pt")) px = (f32)x * 4 / 3;
		else if (str_eq(unit, "pc")) px = (f32)x * 16;
		else if (str_eq(unit, "in")) px = (f32)x * 96;
		else if (str_eq(unit, "cm")) px = (f32)x * 96 / 2.54f;
		else if (str_eq(unit, "mm")) px = (f32)x * 96 / 25.4f;
		else if (str_eq(unit, "q")) px = (f32)x * 96 / 101.6f;
		else if (str_eq(unit, "ch") || str_eq(unit, "ex")) px = (f32)x * em * 0.5f;
		else if (str_eq(unit, "lh")) px = (f32)x * em * 1.2f;
		else return false;
		out->px = px;
	} else {
		out->px = (f32)x;
		out->number = true;
	}
	*pp = p;
	return true;
}

static bool calc_term(const char **pp, f32 em, const Ctx *c, CalcVal *out)
{
	if (!calc_atom(pp, em, c, out))
		return false;
	for (;;) {
		const char *p = skip_ws(*pp);
		if (*p != '*' && *p != '/')
			return true;
		char op = *p++;
		CalcVal r;
		if (!calc_atom(&p, em, c, &r))
			return false;
		if (op == '*') {
			f32 k = r.number ? r.px : out->px;
			if (r.number) {
				out->px *= k, out->pct *= k;
			} else if (out->number) {
				r.px *= k, r.pct *= k;
				*out = r;
			}
		} else if (r.number && r.px != 0) {
			out->px /= r.px, out->pct /= r.px;
		}
		*pp = p;
	}
}

static bool calc_expr(const char **pp, f32 em, const Ctx *c, CalcVal *out)
{
	if (!calc_term(pp, em, c, out))
		return false;
	for (;;) {
		const char *p = skip_ws(*pp);
		if (*p != '+' && *p != '-')
			return true;
		char op = *p++;
		CalcVal r;
		if (!calc_term(&p, em, c, &r))
			return false;
		f32 s = op == '-' ? -1.0f : 1.0f;
		out->px += s * r.px;
		out->pct += s * r.pct;
		out->has_pct |= r.has_pct;
		out->number = out->number && r.number;
		*pp = p;
	}
}

static bool parse_len(const char *v, f32 em, const Ctx *c, Length *out)
{
	v = skip_ws(v);
	if (str_eq_nocase(v, "auto") || str_eq_nocase(v, "fit-content") || str_eq_nocase(v, "max-content") || str_eq_nocase(v, "min-content")) {
		*out = (Length){ 0, 0, UNIT_AUTO };
		return true;
	}
	if (str_eq_nocase(v, "none")) {
		*out = (Length){ 0, 0, UNIT_NONE };
		return true;
	}
	const char *p = v;
	CalcVal cv;
	if (!calc_expr(&p, em, c, &cv) || *skip_ws(p))
		return false;
	if (cv.number && cv.px != 0)
		return false;   /* unitless lengths aren't valid (except 0) */
	if (cv.has_pct)
		*out = (Length){ cv.pct, cv.px, UNIT_PCT };
	else
		*out = (Length){ cv.px, 0, UNIT_PX };
	return true;
}

static f32 parse_px(const char *v, f32 em, const Ctx *c, f32 fallback)
{
	Length l;
	if (!parse_len(v, em, c, &l) || l.unit != UNIT_PX)
		return fallback;
	return l.v;
}

/* ---- The user agent style sheet --------------------------------------------------------------------------------- */

static const char UA_CSS[] =
	"html, body, div, p, address, blockquote, center, dl, dd, dt, figure, figcaption, form, h1, h2, h3, h4, h5, h6, hr,"
	" ol, ul, menu, dir, pre, header, footer, nav, section, article, aside, main, hgroup, fieldset, legend, details, summary,"
	" listing, xmp, plaintext, optgroup, search, dialog[open], frameset, frame { display: block }"
	"head, script, style, title, meta, link, base, template, area, datalist, noembed, noframes, param, rp, source, track,"
	" iframe, object, embed, canvas, audio, video, map, dialog, [hidden], input[type=hidden], col, colgroup, rt, option,"
	" select > optgroup { display: none }"
	"li { display: list-item }"
	"table { display: table; border-spacing: 2px; border-collapse: separate; text-indent: 0 }"
	"caption { display: block; text-align: center }"
	"tr { display: table-row }"
	"thead, tbody, tfoot { display: table-row-group }"
	"td, th { display: table-cell; padding: 1px; vertical-align: middle }"
	"th { font-weight: bold; text-align: center }"
	"body { margin: 8px }"
	"p, dl, figure, blockquote, pre, ul, ol, menu, dir, listing, xmp { margin-top: 1em; margin-bottom: 1em }"
	"blockquote, figure { margin-left: 40px; margin-right: 40px }"
	"dd { margin-left: 40px }"
	"ul, ol, menu, dir { padding-left: 40px }"
	"ul ul, ul ol, ol ul, ol ol, ul menu, ul dir { margin-top: 0; margin-bottom: 0 }"
	"ul, menu, dir { list-style-type: disc }"
	"ol { list-style-type: decimal }"
	"ul ul, ol ul, menu ul, ul menu { list-style-type: circle }"
	"ul ul ul, ul ol ul, ol ul ul, ol ol ul { list-style-type: square }"
	"h1 { font-size: 2em; margin: .67em 0 }"
	"h2 { font-size: 1.5em; margin: .83em 0 }"
	"h3 { font-size: 1.17em; margin: 1em 0 }"
	"h4 { margin: 1.33em 0 }"
	"h5 { font-size: .83em; margin: 1.67em 0 }"
	"h6 { font-size: .67em; margin: 2.33em 0 }"
	"h1, h2, h3, h4, h5, h6, b, strong, legend, summary { font-weight: bold }"
	"i, em, cite, var, dfn, address { font-style: italic }"
	"u, ins { text-decoration: underline }"
	"s, strike, del { text-decoration: line-through }"
	"pre, code, kbd, samp, tt, listing, xmp, plaintext { font-family: monospace }"
	"pre, code, kbd, samp, tt { font-size: .875em }"
	"pre code, pre kbd, pre samp, pre tt { font-size: 1em }"
	"pre, listing, xmp, plaintext { white-space: pre }"
	"small { font-size: smaller }"
	"big { font-size: larger }"
	"sub { vertical-align: sub; font-size: smaller }"
	"sup { vertical-align: super; font-size: smaller }"
	"a:link { color: #1a5fb4; text-decoration: underline }"
	"mark { background-color: #ffe48a; color: black }"
	"hr { border-top: 1px solid #c8c8c8; margin: .5em 0 }"
	"center { text-align: -webkit-center }"
	"nobr { white-space: nowrap }"
	"input, select, textarea, button { display: inline-block; font-size: 13.333px; text-indent: 0; line-height: normal }"
	"button, input[type=submit], input[type=button], input[type=reset], select { padding: 3px 10px;"
	" border: 1px solid #b6b6b6; border-radius: 5px; background-color: #f4f4f4; color: #202020; text-align: center }"
	"input, textarea { border: 1px solid #b6b6b6; border-radius: 5px; background-color: white; color: #202020; padding: 3px 6px }"
	"input[type=checkbox], input[type=radio], input[type=image] { border: 0; padding: 0; background-color: transparent }"
	"fieldset { margin: 0 2px; padding: .35em .75em .625em; border: 1px solid #c8c8c8 }"
	"legend { padding: 0 2px }"
	"details:not([open]) > :not(summary) { display: none }"
	"abbr[title] { text-decoration: none }"
	"ruby > rt { display: none }";

static const char QUIRKS_CSS[] =
	"table { text-align: left; white-space: normal; line-height: normal; font-weight: normal; font-size: medium; font-style: normal }"
	"td, th { text-align: inherit }"
	"th { font-weight: bold; text-align: center }";

static Sheet *ua_sheet, *quirks_sheet;

/* ---- The cascade ------------------------------------------------------------------------------------------------ */

static const Style INITIAL = {
	.display = D_INLINE,
	.visible = true,
	.font_weight = 400,
	.font_size = 16,
	.color = 0xff1f1f1f,
	.list_style = LS_DISC,
	.width = { 0, 0, UNIT_AUTO },
	.height = { 0, 0, UNIT_AUTO },
	.min_width = { 0, 0, UNIT_AUTO },
	.max_width = { 0, 0, UNIT_NONE },
	.min_height = { 0, 0, UNIT_AUTO },
	.max_height = { 0, 0, UNIT_NONE },
	.margin = { { 0, 0, UNIT_PX }, { 0, 0, UNIT_PX }, { 0, 0, UNIT_PX }, { 0, 0, UNIT_PX } },
	.padding = { { 0, 0, UNIT_PX }, { 0, 0, UNIT_PX }, { 0, 0, UNIT_PX }, { 0, 0, UNIT_PX } },
	.border_w = { 3, 3, 3, 3 },
	.border_cur = 15,
	.opacity = 1,
	.flex_shrink = 1,
	.flex_basis = { 0, 0, UNIT_AUTO },
	.border_spacing = 0,
};

void style_free_parts(Style *s)
{
	mem_free(s->bg_image);
	mem_free(s->grid_cols);
	mem_free(s->grid_areas);
	mem_free(s->grid_area);
	mem_free(s->grid_col);
	mem_free(s->grid_row);
	mem_free(s->mask_image);
	s->grid_areas = s->grid_area = s->grid_col = s->grid_row = s->mask_image = NULL;
	Var *v = s->vars;
	while (v) {
		Var *next = v->next;
		mem_free(v->name);
		mem_free(v->value);
		mem_free(v);
		v = next;
	}
	s->bg_image = s->grid_cols = NULL;
	s->vars = NULL;
}

static const char *find_var(const Node *n, const char *name, usize len)
{
	for (; n; n = n->parent) {
		if (!n->style)
			continue;
		for (Var *v = n->style->vars; v; v = v->next)
			if (strlen(v->name) == len && memcmp(v->name, name, len) == 0)
				return v->value;
	}
	return NULL;
}

/* Replaces var(--x, fallback) references. False if one can't be resolved. */
static bool resolve_vars(const char *v, const Node *n, StrBuf *out, int depth)
{
	if (depth > 8)
		return false;
	const char *p = v;
	for (;;) {
		const char *at = str_find(p, "var(");
		if (!at) {
			sb_adds(out, p);
			return true;
		}
		sb_add(out, p, (usize)(at - p));
		const char *q = skip_ws(at + 4);
		const char *name = q;
		while (*q && !is_space(*q) && *q != ',' && *q != ')')
			q++;
		usize nl = (usize)(q - name);
		q = skip_ws(q);
		const char *fb = NULL;
		usize fbl = 0;
		if (*q == ',') {
			fb = ++q;
		}
		int d = 1;
		while (*q && d) {
			if (*q == '(')
				d++;
			else if (*q == ')')
				d--;
			if (d)
				q++;
		}
		if (fb)
			fbl = (usize)(q - fb);
		const char *val = find_var(n, name, nl);
		if (val) {
			if (!resolve_vars(val, n, out, depth + 1))
				return false;
		} else if (fb) {
			char *f = str_ndup(fb, fbl);
			bool ok = resolve_vars(str_trim(f), n, out, depth + 1);
			mem_free(f);
			if (!ok)
				return false;
		} else {
			return false;
		}
		p = *q ? q + 1 : q;
	}
}

static u8 parse_display(const char *v, u8 cur)
{
	char parts[3][256];
	int n = split_top(v, ' ', parts, 3);
	if (!n)
		return cur;
	bool inl = false;
	const char *inner = parts[0];
	if (n == 2) {
		inl = str_eq(parts[0], "inline");
		inner = parts[1];
	}
	if (str_eq(inner, "none")) return D_NONE;
	if (str_eq(inner, "inline")) return D_INLINE;
	if (str_eq(inner, "block") || str_eq(inner, "flow-root") || str_eq(inner, "flow") || str_eq(inner, "run-in")) return inl ? D_INLINE_BLOCK : D_BLOCK;
	if (str_eq(inner, "inline-block")) return D_INLINE_BLOCK;
	if (str_eq(inner, "list-item")) return D_LIST_ITEM;
	if (str_eq(inner, "table") || str_eq(inner, "inline-table")) return D_TABLE;
	if (str_eq(inner, "table-row")) return D_TABLE_ROW;
	if (str_eq(inner, "table-cell")) return D_TABLE_CELL;
	if (str_eq(inner, "table-row-group") || str_eq(inner, "table-header-group") || str_eq(inner, "table-footer-group")) return D_TABLE_GROUP;
	if (str_eq(inner, "table-caption")) return D_BLOCK;
	if (str_eq(inner, "table-column") || str_eq(inner, "table-column-group")) return D_NONE;
	if (str_eq(inner, "flex") || str_eq(inner, "-webkit-box") || str_eq(inner, "-webkit-flex") || str_eq(inner, "-ms-flexbox")) return inl ? D_INLINE_FLEX : D_FLEX;
	if (str_eq(inner, "inline-flex") || str_eq(inner, "-webkit-inline-box")) return D_INLINE_FLEX;
	if (str_eq(inner, "grid")) return inl ? D_INLINE_GRID : D_GRID;
	if (str_eq(inner, "inline-grid")) return D_INLINE_GRID;
	if (str_eq(inner, "contents")) return D_CONTENTS;
	return cur;
}

static bool is_mono_family(const char *v)
{
	char low[256];
	str_copy(low, v, sizeof low);
	str_lower(low);
	/* Only the first family counts. */
	char *comma = (char *)str_chr(low, ',');
	if (comma)
		*comma = 0;
	return str_find(low, "mono") || str_find(low, "courier") || str_find(low, "consolas") || str_find(low, "menlo") ||
	       str_find(low, "monaco") || str_find(low, "code") || str_find(low, "fixed");
}

static bool parse_font_size(const char *v, const Style *ps, const Ctx *c, f32 *out)
{
	static const struct { const char *n; f32 px; } KW[] = {
		{ "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
		{ "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
	};
	for (usize i = 0; i < ARRAY_LEN(KW); i++)
		if (str_eq(v, KW[i].n)) {
			*out = KW[i].px;
			return true;
		}
	if (str_eq(v, "smaller")) {
		*out = ps->font_size / 1.2f;
		return true;
	}
	if (str_eq(v, "larger")) {
		*out = ps->font_size * 1.2f;
		return true;
	}
	Length l;
	if (!parse_len(v, ps->font_size, c, &l))
		return false;
	if (l.unit == UNIT_PCT)
		*out = ps->font_size * l.v / 100 + l.add;
	else if (l.unit == UNIT_PX)
		*out = l.v;
	else
		return false;
	*out = clampf(*out, 0, 400);
	return true;
}

static bool parse_weight(const char *v, const Style *ps, u16 *out)
{
	if (str_eq(v, "normal")) *out = 400;
	else if (str_eq(v, "bold")) *out = 700;
	else if (str_eq(v, "bolder")) *out = ps->font_weight >= 600 ? 900 : ps->font_weight >= 400 ? 700 : 400;
	else if (str_eq(v, "lighter")) *out = ps->font_weight >= 600 ? 400 : 300;
	else if (is_digit(v[0])) *out = (u16)CLAMP(str_to_int(v, NULL), 100, 900);
	else return false;
	return true;
}

static void set_line_height(Style *s, const char *v, const Ctx *c)
{
	if (str_eq(v, "normal")) {
		s->line_height = 0, s->lh_factor = false;
		return;
	}
	const char *e;
	f64 x = str_to_float(v, &e);
	if (e != v && !*skip_ws(e)) {
		s->line_height = (f32)x, s->lh_factor = true;
		return;
	}
	Length l;
	if (!parse_len(v, s->font_size, c, &l))
		return;
	if (l.unit == UNIT_PCT)
		s->line_height = s->font_size * l.v / 100, s->lh_factor = false;
	else if (l.unit == UNIT_PX)
		s->line_height = l.v, s->lh_factor = false;
}

static f32 border_width_kw(const char *v, f32 em, const Ctx *c, bool *ok)
{
	*ok = true;
	if (str_eq(v, "thin")) return 1;
	if (str_eq(v, "medium")) return 3;
	if (str_eq(v, "thick")) return 5;
	Length l;
	if (parse_len(v, em, c, &l) && l.unit == UNIT_PX)
		return maxf(0, l.v);
	*ok = false;
	return 0;
}

static bool is_border_style(const char *v)
{
	static const char *const st[] = { "none", "hidden", "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset" };
	for (usize i = 0; i < ARRAY_LEN(st); i++)
		if (str_eq(v, st[i]))
			return true;
	return false;
}

/* For shorthands with one to four values: which value each side takes. */
static void expand_sides(int n, u8 sides, int idx[4])
{
	for (int i = 0; i < 4; i++)
		idx[i] = -1;
	if (sides == S_ALL) {
		static const int map[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
		n = CLAMP(n, 1, 4);
		for (int i = 0; i < 4; i++)
			idx[i] = map[n - 1][i];
	} else if (sides == S_INLINE) {
		idx[3] = 0, idx[1] = n > 1 ? 1 : 0;
	} else if (sides == S_BLOCK) {
		idx[0] = 0, idx[2] = n > 1 ? 1 : 0;
	} else {
		for (int i = 0; i < 4; i++)
			if (sides & (1 << i))
				idx[i] = 0;
	}
}

static void apply_background(Style *s, const char *v, bool shorthand)
{
	char parts[12][256];
	int n = split_top(v, ' ', parts, 12);
	if (shorthand) {
		s->background = COLOR_CLEAR;
		mem_free(s->bg_image);
		s->bg_image = NULL;
	}
	for (int i = 0; i < n; i++) {
		Color col;
		if (str_starts(parts[i], "url(")) {
			char *u = read_url(parts[i]);
			mem_free(s->bg_image);
			s->bg_image = u;
		} else if (str_find(parts[i], "gradient(")) {
			/* Approximate a gradient by blending its first and last colors. */
			char inner[256];
			const char *open = str_chr(parts[i], '(');
			str_copy(inner, open + 1, sizeof inner);
			char *close = (char *)str_rchr(inner, ')');
			if (close)
				*close = 0;
			char stops[12][256];
			int ns = split_top(inner, ',', stops, 12);
			Color first = 0, last = 0;
			bool got = false;
			for (int k = 0; k < ns; k++) {
				char tok[4][256];
				int nt = split_top(stops[k], ' ', tok, 4);
				Color cc;
				if (nt && css_parse_color(tok[0], &cc)) {
					if (!got)
						first = cc, got = true;
					last = cc;
				}
			}
			if (got && COLOR_A(s->background) == 0)
				s->background = color_mix(first, last, 0.5f);
		} else if (shorthand && css_parse_color(parts[i], &col)) {
			s->background = col;
		}
	}
}

static void apply_border(Style *s, const char *v, u8 sides, const Ctx *c)
{
	char parts[6][256];
	int n = split_top(v, ' ', parts, 6);
	f32 w = 3;
	u8 style = 0;
	Color col = 0;
	bool has_col = false;
	for (int i = 0; i < n; i++) {
		bool ok;
		f32 bw = border_width_kw(parts[i], s->font_size, c, &ok);
		if (ok) {
			w = bw;
			continue;
		}
		if (is_border_style(parts[i])) {
			style = !str_eq(parts[i], "none") && !str_eq(parts[i], "hidden");
			continue;
		}
		if (css_parse_color(parts[i], &col))
			has_col = true;
		else if (str_eq(parts[i], "currentcolor"))
			has_col = false;
	}
	int idx[4];
	expand_sides(1, sides, idx);
	for (int i = 0; i < 4; i++) {
		if (idx[i] < 0)
			continue;
		s->border_w[i] = w;
		s->border_style[i] = style;
		if (has_col) {
			s->border_c[i] = col;
			s->border_cur &= (u8) ~(1 << i);
		} else {
			s->border_cur |= (u8)(1 << i);
		}
	}
}

static u8 parse_list_type(const char *v, u8 cur)
{
	if (str_eq(v, "none")) return LS_NONE;
	if (str_eq(v, "disc")) return LS_DISC;
	if (str_eq(v, "circle")) return LS_CIRCLE;
	if (str_eq(v, "square")) return LS_SQUARE;
	if (str_eq(v, "decimal") || str_eq(v, "decimal-leading-zero")) return LS_DECIMAL;
	if (str_eq(v, "lower-alpha") || str_eq(v, "upper-alpha") || str_eq(v, "lower-latin") || str_eq(v, "upper-latin")) return LS_ALPHA;
	if (str_eq(v, "lower-roman") || str_eq(v, "upper-roman")) return LS_ROMAN;
	return cur;
}

static u8 parse_align(const char *v, u8 cur)
{
	if (str_eq(v, "stretch") || str_eq(v, "normal")) return AI_STRETCH;
	if (str_eq(v, "flex-start") || str_eq(v, "start") || str_eq(v, "self-start") || str_eq(v, "baseline") || str_eq(v, "first") || str_eq(v, "left")) return AI_START;
	if (str_eq(v, "center")) return AI_CENTER;
	if (str_eq(v, "flex-end") || str_eq(v, "end") || str_eq(v, "self-end") || str_eq(v, "last") || str_eq(v, "right")) return AI_END;
	return cur;
}

static void apply_flex(Style *s, const char *v, const Ctx *c)
{
	char parts[3][256];
	int n = split_top(v, ' ', parts, 3);
	if (n == 1 && str_eq(parts[0], "none")) {
		s->flex_grow = 0, s->flex_shrink = 0, s->flex_basis = (Length){ 0, 0, UNIT_AUTO };
		return;
	}
	if (n == 1 && str_eq(parts[0], "auto")) {
		s->flex_grow = 1, s->flex_shrink = 1, s->flex_basis = (Length){ 0, 0, UNIT_AUTO };
		return;
	}
	s->flex_grow = 1, s->flex_shrink = 1, s->flex_basis = (Length){ 0, 0, UNIT_PX };
	int nums = 0;
	for (int i = 0; i < n; i++) {
		const char *e;
		f64 x = str_to_float(parts[i], &e);
		if (e != parts[i] && !*e && nums < 2) {
			if (nums == 0)
				s->flex_grow = (f32)x;
			else
				s->flex_shrink = (f32)x;
			nums++;
		} else {
			Length l;
			if (parse_len(parts[i], s->font_size, c, &l))
				s->flex_basis = l;
			else if (str_eq(parts[i], "content"))
				s->flex_basis = (Length){ 0, 0, UNIT_AUTO };
		}
	}
}

/* Copies one property (group) from another style: inherit / initial. */
static void copy_prop(Style *s, const Style *from, u8 prop, u8 sides)
{
	switch (prop) {
	case P_DISPLAY: s->display = from->display; break;
	case P_POSITION: s->position = from->position; break;
	case P_FLOAT: s->float_side = from->float_side; break;
	case P_CLEAR: s->clear = from->clear; break;
	case P_COLOR: s->color = from->color; break;
	case P_BACKGROUND:
	case P_BACKGROUND_COLOR: s->background = from->background; break;
	case P_FONT:
		s->font_size = from->font_size, s->font_weight = from->font_weight, s->italic = from->italic, s->mono = from->mono;
		s->line_height = from->line_height, s->lh_factor = from->lh_factor;
		break;
	case P_FONT_SIZE: s->font_size = from->font_size; break;
	case P_FONT_WEIGHT: s->font_weight = from->font_weight; break;
	case P_FONT_STYLE: s->italic = from->italic; break;
	case P_FONT_FAMILY: s->mono = from->mono; break;
	case P_LINE_HEIGHT: s->line_height = from->line_height, s->lh_factor = from->lh_factor; break;
	case P_TEXT_ALIGN: s->text_align = from->text_align; break;
	case P_TEXT_DECORATION: s->underline = from->underline, s->strike = from->strike; break;
	case P_TEXT_TRANSFORM: s->text_transform = from->text_transform; break;
	case P_TEXT_INDENT: s->text_indent = from->text_indent; break;
	case P_WHITE_SPACE: s->white_space = from->white_space; break;
	case P_VERTICAL_ALIGN: s->vertical_align = from->vertical_align; break;
	case P_LIST_STYLE:
	case P_LIST_STYLE_TYPE: s->list_style = from->list_style; break;
	case P_VISIBILITY: s->visible = from->visible; break;
	case P_OPACITY: s->opacity = from->opacity; break;
	case P_OVERFLOW: s->overflow_hidden = from->overflow_hidden; break;
	case P_BOX_SIZING: s->border_box = from->border_box; break;
	case P_MARGIN:
	case P_PADDING:
	case P_BORDER:
	case P_BORDER_WIDTH:
	case P_BORDER_STYLE:
	case P_BORDER_COLOR:
		for (int i = 0; i < 4; i++) {
			if (!(sides & (1 << i)))
				continue;
			if (prop == P_MARGIN) s->margin[i] = from->margin[i];
			if (prop == P_PADDING) s->padding[i] = from->padding[i];
			if (prop == P_BORDER || prop == P_BORDER_WIDTH) s->border_w[i] = from->border_w[i];
			if (prop == P_BORDER || prop == P_BORDER_STYLE) s->border_style[i] = from->border_style[i];
			if (prop == P_BORDER || prop == P_BORDER_COLOR) {
				s->border_c[i] = from->border_c[i];
				s->border_cur = (u8)((s->border_cur & ~(1 << i)) | (from->border_cur & (1 << i)));
			}
		}
		break;
	case P_BORDER_RADIUS: s->radius = from->radius; break;
	case P_WIDTH: s->width = from->width; break;
	case P_HEIGHT: s->height = from->height; break;
	case P_MIN_WIDTH: s->min_width = from->min_width; break;
	case P_MAX_WIDTH: s->max_width = from->max_width; break;
	case P_MIN_HEIGHT: s->min_height = from->min_height; break;
	case P_MAX_HEIGHT: s->max_height = from->max_height; break;
	case P_BORDER_SPACING: s->border_spacing = from->border_spacing; break;
	case P_BORDER_COLLAPSE: s->border_collapse = from->border_collapse; break;
	default: break;
	}
}

static bool inherited_prop(u8 prop)
{
	switch (prop) {
	case P_COLOR: case P_FONT: case P_FONT_SIZE: case P_FONT_WEIGHT: case P_FONT_STYLE: case P_FONT_FAMILY:
	case P_LINE_HEIGHT: case P_TEXT_ALIGN: case P_TEXT_TRANSFORM: case P_TEXT_INDENT: case P_WHITE_SPACE:
	case P_LIST_STYLE: case P_LIST_STYLE_TYPE: case P_VISIBILITY: case P_BORDER_SPACING: case P_BORDER_COLLAPSE:
		return true;
	}
	return false;
}

static void apply(Style *s, const Style *ps, u8 prop, u8 sides, const char *v, const Ctx *c)
{
	if (str_eq(v, "inherit")) {
		copy_prop(s, ps, prop, sides ? sides : S_ALL);
		return;
	}
	if (str_eq(v, "initial") || str_eq(v, "unset") || str_eq(v, "revert") || str_eq(v, "revert-layer")) {
		bool inh = inherited_prop(prop) && !str_eq(v, "initial");
		copy_prop(s, inh ? ps : &INITIAL, prop, sides ? sides : S_ALL);
		return;
	}
	f32 em = s->font_size;
	switch (prop) {
	case P_DISPLAY:
		s->display = parse_display(v, s->display);
		break;
	case P_POSITION:
		s->position = str_eq(v, "absolute") ? POS_ABSOLUTE : str_eq(v, "fixed") ? POS_FIXED : str_eq(v, "relative") ? POS_RELATIVE
			      : str_starts(v, "sticky") || str_starts(v, "-webkit-sticky") ? POS_STICKY
											: str_eq(v, "static") ? POS_STATIC : s->position;
		break;
	case P_FLOAT:
		s->float_side = str_eq(v, "left") || str_eq(v, "inline-start") ? FL_LEFT : str_eq(v, "right") || str_eq(v, "inline-end") ? FL_RIGHT : FL_NONE;
		break;
	case P_CLEAR:
		s->clear = str_eq(v, "left") ? CL_LEFT : str_eq(v, "right") ? CL_RIGHT : str_eq(v, "both") ? CL_BOTH : CL_NONE;
		break;
	case P_COLOR:
		if (str_eq(v, "currentcolor"))
			s->color = ps->color;
		else
			css_parse_color(v, &s->color);
		break;
	case P_BACKGROUND_COLOR:
		if (str_eq(v, "currentcolor"))
			s->background = s->color;
		else
			css_parse_color(v, &s->background);
		break;
	case P_BACKGROUND:
		if (str_eq(v, "none"))
			s->background = COLOR_CLEAR;
		else
			apply_background(s, v, true);
		break;
	case P_BACKGROUND_IMAGE:
		if (str_eq(v, "none")) {
			mem_free(s->bg_image);
			s->bg_image = NULL;
		} else {
			apply_background(s, v, false);
		}
		break;
	case P_FONT: {
		char parts[12][256];
		int n = split_top(v, ' ', parts, 12);
		if (n == 1)
			break;   /* system fonts: caption, menu... */
		s->italic = false, s->font_weight = 400;
		s->line_height = 0, s->lh_factor = false;
		for (int i = 0; i < n; i++) {
			if (str_eq(parts[i], "italic") || str_eq(parts[i], "oblique")) {
				s->italic = true;
				continue;
			}
			if (str_eq(parts[i], "normal") || str_eq(parts[i], "small-caps"))
				continue;
			u16 w;
			if (parse_weight(parts[i], ps, &w) && !str_chr(parts[i], '%') && !str_find(parts[i], "px") && !str_find(parts[i], "em")) {
				s->font_weight = w;
				continue;
			}
			/* size[/line-height], then the family list */
			char size[256];
			str_copy(size, parts[i], sizeof size);
			char *slash = (char *)str_chr(size, '/');
			const char *lh = NULL;
			if (slash) {
				*slash = 0;
				lh = slash + 1;
				if (!*lh && i + 1 < n)
					lh = parts[++i];
			} else if (i + 1 < n && parts[i + 1][0] == '/') {
				lh = parts[i + 1][1] ? parts[i + 1] + 1 : (i + 2 < n ? parts[i + 2] : NULL);
				i += parts[i + 1][1] ? 1 : 2;
			}
			f32 fs;
			if (parse_font_size(size, ps, c, &fs))
				s->font_size = fs;
			if (lh)
				set_line_height(s, lh, c);
			StrBuf fam = { 0 };
			for (int k = i + 1; k < n; k++) {
				sb_adds(&fam, parts[k]);
				sb_addc(&fam, ' ');
			}
			s->mono = is_mono_family(sb_str(&fam));
			sb_free(&fam);
			break;
		}
		break;
	}
	case P_FONT_SIZE: {
		f32 fs;
		if (parse_font_size(v, ps, c, &fs))
			s->font_size = fs;
		break;
	}
	case P_FONT_WEIGHT:
		parse_weight(v, ps, &s->font_weight);
		break;
	case P_FONT_STYLE:
		s->italic = str_eq(v, "italic") || str_starts(v, "oblique");
		break;
	case P_FONT_FAMILY:
		s->mono = is_mono_family(v);
		break;
	case P_LINE_HEIGHT:
		set_line_height(s, v, c);
		break;
	case P_TEXT_ALIGN:
		if (str_eq(v, "left") || str_eq(v, "start") || str_eq(v, "-webkit-left")) s->text_align = TA_LEFT;
		else if (str_eq(v, "center")) s->text_align = TA_CENTER;
		else if (str_eq(v, "-webkit-center") || str_eq(v, "-moz-center")) s->text_align = TA_CENTER_BLOCKS;
		else if (str_eq(v, "right") || str_eq(v, "end") || str_eq(v, "-webkit-right")) s->text_align = TA_RIGHT;
		else if (str_eq(v, "justify")) s->text_align = TA_JUSTIFY;
		break;
	case P_TEXT_DECORATION:
		if (str_find(v, "none")) {
			s->underline = s->strike = false;
		} else {
			if (str_find(v, "underline"))
				s->underline = true;
			if (str_find(v, "line-through"))
				s->strike = true;
		}
		break;
	case P_TEXT_TRANSFORM:
		s->text_transform = str_eq(v, "uppercase") ? TT_UPPER : str_eq(v, "lowercase") ? TT_LOWER : str_eq(v, "capitalize") ? TT_CAPITALIZE : TT_NONE;
		break;
	case P_TEXT_INDENT:
		s->text_indent = parse_px(v, em, c, 0);
		break;
	case P_WHITE_SPACE:
		if (str_eq(v, "normal")) s->white_space = WS_NORMAL;
		else if (str_eq(v, "nowrap")) s->white_space = WS_NOWRAP;
		else if (str_eq(v, "pre")) s->white_space = WS_PRE;
		else if (str_eq(v, "pre-wrap") || str_eq(v, "break-spaces")) s->white_space = WS_PRE_WRAP;
		else if (str_eq(v, "pre-line")) s->white_space = WS_PRE_LINE;
		break;
	case P_VERTICAL_ALIGN:
		if (str_eq(v, "top") || str_eq(v, "text-top")) s->vertical_align = VA_TOP;
		else if (str_eq(v, "middle")) s->vertical_align = VA_MIDDLE;
		else if (str_eq(v, "bottom") || str_eq(v, "text-bottom")) s->vertical_align = VA_BOTTOM;
		else if (str_eq(v, "super")) s->vertical_align = VA_SUPER;
		else if (str_eq(v, "sub")) s->vertical_align = VA_SUB;
		else s->vertical_align = VA_BASELINE;
		break;
	case P_LIST_STYLE: {
		char parts[4][256];
		int n = split_top(v, ' ', parts, 4);
		for (int i = 0; i < n; i++)
			s->list_style = parse_list_type(parts[i], s->list_style);
		break;
	}
	case P_LIST_STYLE_TYPE:
		s->list_style = parse_list_type(v, s->list_style);
		break;
	case P_VISIBILITY:
		s->visible = !(str_eq(v, "hidden") || str_eq(v, "collapse"));
		break;
	case P_OPACITY: {
		bool pct = false;
		s->opacity = clampf(num_arg(v, 1, &pct), 0, 1);
		break;
	}
	case P_OVERFLOW:
		s->overflow_hidden = !str_eq(v, "visible");
		break;
	case P_BOX_SIZING:
		s->border_box = str_eq(v, "border-box");
		break;
	case P_MARGIN:
	case P_PADDING: {
		char parts[4][256];
		int n = split_top(v, ' ', parts, 4);
		if (!n)
			break;
		Length ls[4];
		for (int i = 0; i < n; i++)
			if (!parse_len(parts[i], em, c, &ls[i]) || ls[i].unit == UNIT_NONE)
				return;
		int idx[4];
		expand_sides(n, sides, idx);
		for (int i = 0; i < 4; i++) {
			if (idx[i] < 0)
				continue;
			if (prop == P_MARGIN)
				s->margin[i] = ls[idx[i]];
			else if (ls[idx[i]].unit != UNIT_AUTO)
				s->padding[i] = ls[idx[i]];
		}
		break;
	}
	case P_BORDER:
		apply_border(s, v, sides, c);
		break;
	case P_BORDER_WIDTH:
	case P_BORDER_STYLE:
	case P_BORDER_COLOR: {
		char parts[4][256];
		int n = split_top(v, ' ', parts, 4);
		if (!n)
			break;
		int idx[4];
		expand_sides(n, sides, idx);
		for (int i = 0; i < 4; i++) {
			if (idx[i] < 0)
				continue;
			const char *pv = parts[idx[i]];
			if (prop == P_BORDER_WIDTH) {
				bool ok;
				f32 w = border_width_kw(pv, em, c, &ok);
				if (ok)
					s->border_w[i] = w;
			} else if (prop == P_BORDER_STYLE) {
				if (is_border_style(pv))
					s->border_style[i] = !str_eq(pv, "none") && !str_eq(pv, "hidden");
			} else {
				Color col;
				if (str_eq(pv, "currentcolor")) {
					s->border_cur |= (u8)(1 << i);
				} else if (css_parse_color(pv, &col)) {
					s->border_c[i] = col;
					s->border_cur &= (u8) ~(1 << i);
				}
			}
		}
		break;
	}
	case P_BORDER_RADIUS: {
		char parts[4][256];
		int n = split_top(v, ' ', parts, 4);
		if (!n)
			break;
		Length l;
		if (parse_len(parts[0], em, c, &l))
			s->radius = l.unit == UNIT_PCT ? (l.v >= 50 ? 9999 : l.v * 0.4f) : maxf(0, l.v);
		break;
	}
	case P_WIDTH: parse_len(v, em, c, &s->width); break;
	case P_HEIGHT: parse_len(v, em, c, &s->height); break;
	case P_MIN_WIDTH: parse_len(v, em, c, &s->min_width); break;
	case P_MAX_WIDTH: parse_len(v, em, c, &s->max_width); break;
	case P_MIN_HEIGHT: parse_len(v, em, c, &s->min_height); break;
	case P_MAX_HEIGHT: parse_len(v, em, c, &s->max_height); break;
	case P_FLEX:
		apply_flex(s, v, c);
		break;
	case P_FLEX_DIRECTION:
		s->flex_column = str_starts(v, "column");
		break;
	case P_FLEX_WRAP:
		s->flex_wrap = str_starts(v, "wrap");
		break;
	case P_FLEX_FLOW: {
		char parts[2][256];
		int n = split_top(v, ' ', parts, 2);
		for (int i = 0; i < n; i++) {
			if (str_starts(parts[i], "column"))
				s->flex_column = true;
			else if (str_starts(parts[i], "row"))
				s->flex_column = false;
			else if (str_starts(parts[i], "wrap"))
				s->flex_wrap = true;
			else if (str_eq(parts[i], "nowrap"))
				s->flex_wrap = false;
		}
		break;
	}
	case P_FLEX_GROW: s->flex_grow = (f32)str_to_float(v, NULL); break;
	case P_FLEX_SHRINK: s->flex_shrink = (f32)str_to_float(v, NULL); break;
	case P_FLEX_BASIS:
		if (str_eq(v, "content"))
			s->flex_basis = (Length){ 0, 0, UNIT_AUTO };
		else
			parse_len(v, em, c, &s->flex_basis);
		break;
	case P_JUSTIFY_CONTENT:
		if (str_eq(v, "center")) s->justify = JC_CENTER;
		else if (str_eq(v, "flex-end") || str_eq(v, "end") || str_eq(v, "right")) s->justify = JC_END;
		else if (str_eq(v, "space-between")) s->justify = JC_BETWEEN;
		else if (str_eq(v, "space-around") || str_eq(v, "space-evenly")) s->justify = JC_AROUND;
		else s->justify = JC_START;
		break;
	case P_ALIGN_ITEMS: s->align_items = parse_align(v, s->align_items); break;
	case P_PLACE_ITEMS: {
		char parts[2][256];
		if (split_top(v, ' ', parts, 2))
			s->align_items = parse_align(parts[0], s->align_items);
		break;
	}
	case P_ALIGN_SELF: s->align_self = str_eq(v, "auto") ? 0 : (u8)(parse_align(v, 0) + 1); break;
	case P_GAP: {
		char parts[2][256];
		int n = split_top(v, ' ', parts, 2);
		if (n) {
			s->row_gap = parse_px(parts[0], em, c, s->row_gap);
			s->col_gap = parse_px(parts[n > 1 ? 1 : 0], em, c, s->col_gap);
		}
		break;
	}
	case P_ROW_GAP: s->row_gap = parse_px(v, em, c, s->row_gap); break;
	case P_COLUMN_GAP: s->col_gap = parse_px(v, em, c, s->col_gap); break;
	case P_GRID_TEMPLATE_COLUMNS:
		mem_free(s->grid_cols);
		s->grid_cols = str_eq(v, "none") ? NULL : str_dup(v);
		break;
	case P_BORDER_SPACING: {
		char parts[2][256];
		if (split_top(v, ' ', parts, 2))
			s->border_spacing = parse_px(parts[0], em, c, s->border_spacing);
		break;
	}
	case P_BORDER_COLLAPSE:
		s->border_collapse = str_eq(v, "collapse");
		break;
	case P_GRID_TEMPLATE_AREAS:
		mem_free(s->grid_areas);
		s->grid_areas = str_eq(v, "none") ? NULL : str_dup(v);
		break;
	case P_GRID_TEMPLATE: {
		/* [areas and row sizes] / columns */
		const char *slash = NULL;
		int depth = 0;
		for (const char *p = v; *p; p++) {
			if (*p == '(')
				depth++;
			else if (*p == ')')
				depth--;
			else if (*p == '"' || *p == '\'') {
				p = skip_string(p) - 1;
			} else if (*p == '/' && !depth)
				slash = p;
		}
		if (str_chr(v, '"') || str_chr(v, '\'')) {
			StrBuf areas = { 0 };
			for (const char *p = v; *p && (!slash || p < slash); p++) {
				if (*p == '"' || *p == '\'') {
					const char *e = skip_string(p);
					sb_add(&areas, p, (usize)(e - p));
					sb_addc(&areas, ' ');
					p = e - 1;
				}
			}
			mem_free(s->grid_areas);
			s->grid_areas = sb_take(&areas);
		}
		if (slash) {
			char *tmp = str_dup(slash + 1);
			mem_free(s->grid_cols);
			s->grid_cols = str_dup(str_trim(tmp));
			mem_free(tmp);
		}
		break;
	}
	case P_GRID_AREA:
		mem_free(s->grid_area);
		s->grid_area = str_eq(v, "auto") ? NULL : str_dup(v);
		break;
	case P_GRID_COLUMN:
		mem_free(s->grid_col);
		s->grid_col = str_eq(v, "auto") ? NULL : str_dup(v);
		break;
	case P_GRID_ROW:
		mem_free(s->grid_row);
		s->grid_row = str_eq(v, "auto") ? NULL : str_dup(v);
		break;
	case P_ORDER:
		s->order = (i16)CLAMP(str_to_int(v, NULL), -9999, 9999);
		break;
	case P_MASK_IMAGE: {
		mem_free(s->mask_image);
		s->mask_image = NULL;
		const char *u = str_find(v, "url(");
		if (u)
			s->mask_image = read_url(u);
		break;
	}
	}
}

/* ---- Rule index ------------------------------------------------------------------------------------------------- */

typedef VEC(Rule *) RuleList;

typedef struct {
	Map by_key;              /* "#id", ".class", "tag" -> RuleList* */
	RuleList universal;
	Ctx ctx;
} Index;

static void index_add(Index *ix, Rule *r)
{
	const Compound *c = &r->parts[r->nparts - 1];
	char key[256];
	if (c->id)
		fmt(key, sizeof key, "#%s", c->id);
	else if (c->nclasses)
		fmt(key, sizeof key, ".%s", c->classes[0]);
	else if (c->tag[0])
		str_copy(key, c->tag, sizeof key);
	else {
		vec_push(&ix->universal, r);
		return;
	}
	RuleList *l = map_get(&ix->by_key, key);
	if (!l) {
		l = NEW(RuleList);
		map_set(&ix->by_key, key, l);
	}
	vec_push(l, r);
}

static void rulelist_free(void *p)
{
	RuleList *l = p;
	vec_free(l);
	mem_free(l);
}

typedef struct {
	u64 key;
	const Decl *d;
} Match;

typedef VEC(Match) MatchList;

static int match_cmp(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	u64 x = ((const Match *)a)->key, y = ((const Match *)b)->key;
	return x < y ? -1 : x > y;
}

/* Old-fashioned attributes like bgcolor and width, as CSS. */
static void presentational(const Node *n, StrBuf *out)
{
	const char *t = n->tag, *v;
	if ((v = node_attr(n, "bgcolor")))
		sb_fmt(out, "background-color:%s;", v);
	if (str_eq(t, "body")) {
		if ((v = node_attr(n, "text")))
			sb_fmt(out, "color:%s;", v);
		if ((v = node_attr(n, "marginwidth")) || (v = node_attr(n, "leftmargin")))
			sb_fmt(out, "margin-left:%spx;margin-right:%spx;", v, v);
		if ((v = node_attr(n, "marginheight")) || (v = node_attr(n, "topmargin")))
			sb_fmt(out, "margin-top:%spx;margin-bottom:%spx;", v, v);
	}
	if (str_eq(t, "font")) {
		if ((v = node_attr(n, "color")))
			sb_fmt(out, "color:%s;", v);
		if ((v = node_attr(n, "face")) && is_mono_family(v))
			sb_adds(out, "font-family:monospace;");
		if ((v = node_attr(n, "size"))) {
			static const char *const sizes[] = { "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large" };
			int k = (int)str_to_int(v + (*v == '+' || *v == '-'), NULL);
			int sz = *v == '+' ? 3 + k : *v == '-' ? 3 - k : k;
			sb_fmt(out, "font-size:%s;", sizes[CLAMP(sz, 1, 7) - 1]);
		}
	}
	bool sized = str_eq(t, "img") || str_eq(t, "table") || str_eq(t, "td") || str_eq(t, "th") || str_eq(t, "hr") ||
		     str_eq(t, "iframe") || str_eq(t, "svg") || str_eq(t, "col") || str_eq(t, "video") || str_eq(t, "canvas");
	if (sized) {
		if ((v = node_attr(n, "width")) && is_digit(*v))
			sb_fmt(out, "width:%s%s;", v, str_chr(v, '%') ? "" : "px");
		if ((v = node_attr(n, "height")) && is_digit(*v) && !str_eq(t, "table") && !str_eq(t, "tr"))
			sb_fmt(out, "height:%s%s;", v, str_chr(v, '%') ? "" : "px");
	}
	if ((v = node_attr(n, "align"))) {
		bool img = str_eq(t, "img") || str_eq(t, "table") || str_eq(t, "iframe");
		if (img && str_eq_nocase(v, "left"))
			sb_adds(out, "float:left;");
		else if (img && str_eq_nocase(v, "right"))
			sb_adds(out, "float:right;");
		else if (str_eq(t, "table") && str_eq_nocase(v, "center"))
			sb_adds(out, "margin-left:auto;margin-right:auto;");
		else if (str_eq(t, "img") && (str_eq_nocase(v, "middle") || str_eq_nocase(v, "absmiddle")))
			sb_adds(out, "vertical-align:middle;");
		else if (!img)
			sb_fmt(out, "text-align:%s;", str_eq_nocase(v, "center") || str_eq_nocase(v, "middle") ? "-webkit-center" : v);
	}
	if ((v = node_attr(n, "valign")))
		sb_fmt(out, "vertical-align:%s;", v);
	if (node_attr(n, "nowrap") && (str_eq(t, "td") || str_eq(t, "th")))
		sb_adds(out, "white-space:nowrap;");
	if (str_eq(t, "table")) {
		if ((v = node_attr(n, "cellspacing")))
			sb_fmt(out, "border-spacing:%spx;", v);
		if ((v = node_attr(n, "border")) && str_to_int(v, NULL) > 0)
			sb_fmt(out, "border:%lldpx outset #a0a0a0;", (long long)str_to_int(v, NULL));
	}
	if (str_eq(t, "td") || str_eq(t, "th")) {
		const Node *tbl = n->parent;
		while (tbl && !str_eq(tbl->tag, "table"))
			tbl = tbl->parent;
		if (tbl) {
			if ((v = node_attr(tbl, "cellpadding")))
				sb_fmt(out, "padding:%spx;", v);
			if ((v = node_attr(tbl, "border")) && str_to_int(v, NULL) > 0)
				sb_adds(out, "border:1px inset #a0a0a0;");
		}
	}
	if (str_eq(t, "img")) {
		if ((v = node_attr(n, "border")))
			sb_fmt(out, "border:%spx solid;", v);
		if ((v = node_attr(n, "hspace")))
			sb_fmt(out, "margin-left:%spx;margin-right:%spx;", v, v);
		if ((v = node_attr(n, "vspace")))
			sb_fmt(out, "margin-top:%spx;margin-bottom:%spx;", v, v);
	}
	if (str_eq(t, "hr")) {
		if ((v = node_attr(n, "color")))
			sb_fmt(out, "border-top-color:%s;", v);
		if ((v = node_attr(n, "size")) && str_to_int(v, NULL) > 1)
			sb_fmt(out, "border-top-width:%spx;", v);
	}
	if (str_eq(t, "input") && (v = node_attr(n, "size")) && str_to_int(v, NULL) > 0)
		sb_fmt(out, "width:%lldpx;", (long long)str_to_int(v, NULL) * 7 + 16);
	if (str_eq(t, "textarea")) {
		if ((v = node_attr(n, "cols")))
			sb_fmt(out, "width:%lldpx;", (long long)str_to_int(v, NULL) * 7 + 16);
		if ((v = node_attr(n, "rows")))
			sb_fmt(out, "height:%lldpx;", (long long)str_to_int(v, NULL) * 16 + 8);
	}
	if ((v = node_attr(n, "hidden")) && !str_eq(t, "input"))
		sb_adds(out, "display:none;");
}

static void compute_style(Node *n, Style *s, const Style *ps, Index *ix, MatchList *ml);

static void style_node(Node *n, const Style *ps, Index *ix, MatchList *ml)
{
	if (n->type != NODE_ELEMENT)
		return;
	if (!n->style)
		n->style = NEW(Style);
	Style *s = n->style;
	style_free_parts(s);
	*s = INITIAL;
	/* Inherited properties. */
	s->color = ps->color;
	s->font_size = ps->font_size;
	s->font_weight = ps->font_weight;
	s->italic = ps->italic;
	s->mono = ps->mono;
	s->line_height = ps->line_height;
	s->lh_factor = ps->lh_factor;
	s->text_align = ps->text_align;
	s->white_space = ps->white_space;
	s->list_style = ps->list_style;
	s->text_transform = ps->text_transform;
	s->text_indent = ps->text_indent;
	s->visible = ps->visible;
	s->underline = ps->underline;
	s->strike = ps->strike;
	s->border_spacing = ps->border_spacing;
	s->border_collapse = ps->border_collapse;
	if (n->tag[0] == '#') {
		for (Node *c = n->first; c; c = c->next)
			style_node(c, s, ix, ml);
		return;
	}

	compute_style(n, s, ps, ix, ml);
	for (Node *c = n->first; c; c = c->next)
		style_node(c, s, ix, ml);
}

/* Kept out of style_node so the recursion's frames stay small. */
static __attribute__((noinline)) void compute_style(Node *n, Style *s, const Style *ps, Index *ix, MatchList *ml)
{
	vec_clear(ml);
	char key[256];
	const char *id = node_attr(n, "id");
	/* Candidate rules: by id, each class, tag, and the universal ones; then
	 * sort what matches by origin, specificity and order. */
	RuleList *lists[40];
	int nl = 0;
	if (id) {
		fmt(key, sizeof key, "#%s", id);
		lists[nl] = map_get(&ix->by_key, key);
		nl += lists[nl] != NULL;
	}
	const char *cls = node_attr(n, "class");
	if (cls) {
		for (const char *p = cls; *p && nl < 36;) {
			while (*p && is_space(*p))
				p++;
			const char *st = p;
			while (*p && !is_space(*p))
				p++;
			if (p > st && (usize)(p - st) < sizeof key - 2) {
				key[0] = '.';
				memcpy(key + 1, st, (usize)(p - st));
				key[p - st + 1] = 0;
				RuleList *l = map_get(&ix->by_key, key);
				/* a class listed twice must not count twice */
				bool dup = false;
				for (int i = 0; i < nl; i++)
					dup |= lists[i] == l;
				if (l && !dup)
					lists[nl++] = l;
			}
		}
	}
	lists[nl] = map_get(&ix->by_key, n->tag);
	nl += lists[nl] != NULL;
	lists[nl++] = &ix->universal;
	for (int i = 0; i < nl; i++) {
		RuleList *l = lists[i];
		vec_each(l, rp)
		{
			Rule *r = *rp;
			if (!match_from(r, r->nparts - 1, n))
				continue;
			/* Which sheet: rules carry their sheet's base in the top bits of order. */
			u32 origin = r->order >> 20 == 0 ? 0 : 2;
			for (int k = 0; k < r->block->n; k++) {
				const Decl *d = &r->block->decls[k];
				u64 mk = ((u64)d->important << 58) | ((u64)origin << 56) | ((u64)(r->spec & 0xffffff) << 32) |
					 ((u64)(r->order & 0xfffff) << 12) | (u64)MIN(k, 4095);
				vec_push(ml, (Match){ mk, d });
			}
		}
	}

	/* Presentational attributes and the style attribute. */
	Decl *pdecls = NULL, *sdecls = NULL;
	int npd = 0, nsd = 0;
	StrBuf pres = { 0 };
	presentational(n, &pres);
	if (pres.len) {
		npd = parse_decls(pres.data, &pdecls);
		for (int k = 0; k < npd; k++)
			vec_push(ml, (Match){ ((u64)1 << 56) | (u64)k, &pdecls[k] });
	}
	sb_free(&pres);
	const char *sa = node_attr(n, "style");
	if (sa) {
		nsd = parse_decls(sa, &sdecls);
		for (int k = 0; k < nsd; k++)
			vec_push(ml, (Match){ ((u64)sdecls[k].important << 58) | ((u64)3 << 56) | (u64)k, &sdecls[k] });
	}
	if (ml->len > 1)
		sort(ml->data, ml->len, sizeof(Match), match_cmp, NULL);

	/* Custom properties first: other values may refer to them. */
	vec_each(ml, m)
	{
		if (m->d->prop != P_CUSTOM)
			continue;
		Var *v = NULL;
		for (Var *x = s->vars; x; x = x->next)
			if (str_eq(x->name, m->d->name))
				v = x;
		if (!v) {
			v = NEW(Var);
			v->name = str_dup(m->d->name);
			v->next = s->vars;
			s->vars = v;
		} else {
			mem_free(v->value);
		}
		v->value = str_dup(m->d->value);
	}
	/* font-size first too: em values depend on it. */
	StrBuf tmp = { 0 };
	for (int pass = 0; pass < 2; pass++) {
		vec_each(ml, m)
		{
			const Decl *d = m->d;
			if (d->prop == P_CUSTOM)
				continue;
			bool fontish = d->prop == P_FONT_SIZE || d->prop == P_FONT;
			if (fontish != (pass == 0))
				continue;
			const char *v = d->value;
			if (str_find(v, "var(")) {
				sb_clear(&tmp);
				if (!resolve_vars(v, n, &tmp, 0))
					continue;
				v = sb_str(&tmp);
			}
			char low[512];
			bool keep_case = d->prop == P_BACKGROUND_IMAGE || d->prop == P_BACKGROUND || d->prop == P_FONT_FAMILY || d->prop == P_FONT ||
					 d->prop == P_MASK_IMAGE || d->prop == P_GRID_TEMPLATE_AREAS || d->prop == P_GRID_TEMPLATE || d->prop == P_GRID_AREA ||
					 d->prop == P_GRID_COLUMN || d->prop == P_GRID_ROW;
			if (!keep_case) {
				str_copy(low, v, sizeof low);
				str_lower(low);
				v = low;
			}
			apply(s, ps, d->prop, d->sides, v, &ix->ctx);
		}
	}
	sb_free(&tmp);
	decls_free(pdecls, npd);
	decls_free(sdecls, nsd);

	/* Finishing touches. */
	for (int i = 0; i < 4; i++) {
		if (s->border_cur & (1 << i))
			s->border_c[i] = s->color;
		if (!s->border_style[i])
			s->border_w[i] = 0;
	}
	if (str_eq(n->tag, "html"))
		ix->ctx.rem = s->font_size;
	if (s->position == POS_STICKY || s->position == POS_RELATIVE)
		s->position = POS_STATIC;
}

void style_tree(Node *root, Sheet **sheets, int nsheets, f32 vw, f32 vh)
{
	if (!ua_sheet)
		ua_sheet = css_parse(UA_CSS, sizeof UA_CSS - 1, "");
	Index ix = { .ctx = { vw, vh, 16 } };
	/* Rules get a global order: UA first (sheet 0), then the page's sheets.
	 * The sheet number goes in the top bits so UA rules can be told apart. */
	u32 next = 0;
	Sheet *list[2 + 256];
	int n = 0;
	list[n++] = ua_sheet;
	if (html_quirks(root)) {
		if (!quirks_sheet)
			quirks_sheet = css_parse(QUIRKS_CSS, sizeof QUIRKS_CSS - 1, "");
		list[n++] = quirks_sheet;
	}
	for (int i = 0; i < nsheets && n < (int)ARRAY_LEN(list); i++)
		if (sheets[i])
			list[n++] = sheets[i];
	for (int s = 0; s < n; s++) {
		vec_each(&list[s]->rules, r)
		{
			if (r->media && !css_media_matches(r->media, vw, vh))
				continue;
			/* order: the UA sheets keep < 2^20; page sheets start at 2^20 */
			if (list[s] == ua_sheet)
				r->order = (u32)(r - list[s]->rules.data);
			else if (list[s] == quirks_sheet)
				r->order = (u32)(list[0]->rules.len + (usize)(r - list[s]->rules.data));
			else
				r->order = (1u << 20) | (next++ & 0xfffff);
			index_add(&ix, r);
		}
	}
	Style base = INITIAL;
	base.border_spacing = 0;
	MatchList ml = { 0 };
	style_node(root, &base, &ix, &ml);
	vec_free(&ml);
	map_free(&ix.by_key, rulelist_free);
	vec_free(&ix.universal);
}
