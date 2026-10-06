/* Drift's engine: documents, styles, layout and painting. */
#pragma once

#include "zui.h"
#include "zn.h"

/* ---- Document ------------------------------------------------------------------------------- */

enum { NODE_ELEMENT = 1, NODE_TEXT };

typedef struct Style Style;
typedef struct Node Node;

/* Live state of a form control (what the user typed or picked). */
typedef struct {
	StrBuf value;
	bool checked;
	int selected;            /* <select>: index of the chosen option */
} Control;

struct Node {
	int type;
	char tag[16];            /* lower case */
	char *text;              /* text nodes */
	char **attrs;            /* name, value, name, value... */
	int nattrs;
	Node *parent, *first, *last, *next, *prev;
	Style *style;
	Control *control;
	int index;               /* among element siblings, for :nth-child */
};

Node *html_parse(const char *src, usize len);
void node_free(Node *n);
const char *node_attr(const Node *n, const char *name);
bool node_has_class(const Node *n, const char *cls);
Node *node_find(Node *root, const char *tag);          /* first element with this tag */
void node_text_content(const Node *n, StrBuf *out);
/* Decodes &amp; and friends in place. */
void html_decode_entities(char *s);
/* Writes the element and its subtree back out as markup (for inline SVG). */
void node_serialize(const Node *n, StrBuf *out);

/* ---- CSS ------------------------------------------------------------------------------------- */

/* Computed lengths are pixels, a percentage of the containing block (plus a pixel
 * offset, for calc(100% - 20px)), auto or none. */
enum { UNIT_AUTO, UNIT_PX, UNIT_PCT, UNIT_NONE };

typedef struct {
	f32 v;
	f32 add;
	u8 unit;
} Length;

enum { D_NONE, D_INLINE, D_BLOCK, D_INLINE_BLOCK, D_LIST_ITEM, D_TABLE, D_TABLE_ROW, D_TABLE_CELL, D_TABLE_GROUP,
       D_FLEX, D_INLINE_FLEX, D_GRID, D_INLINE_GRID, D_CONTENTS };
enum { TA_LEFT, TA_CENTER, TA_RIGHT, TA_JUSTIFY, TA_CENTER_BLOCKS /* <center>: centers child blocks too */ };
enum { WS_NORMAL, WS_PRE, WS_NOWRAP, WS_PRE_WRAP, WS_PRE_LINE };
enum { LS_NONE, LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_ALPHA, LS_ROMAN };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum { FL_NONE, FL_LEFT, FL_RIGHT };
enum { CL_NONE, CL_LEFT, CL_RIGHT, CL_BOTH };
enum { JC_START, JC_CENTER, JC_END, JC_BETWEEN, JC_AROUND };
enum { AI_STRETCH, AI_START, AI_CENTER, AI_END };
enum { VA_BASELINE, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_SUPER, VA_SUB };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAPITALIZE };

typedef struct Var Var;
struct Var {
	char *name, *value;
	Var *next;
};

struct Style {
	u8 display, position, float_side, clear;
	u8 text_align, white_space, list_style, text_transform, vertical_align;
	bool border_box, visible, underline, strike, mono, italic, overflow_hidden;
	u16 font_weight;
	f32 font_size;           /* px */
	f32 line_height;         /* px, or a factor of the font size if lh_factor; 0 = normal */
	bool lh_factor;
	f32 text_indent;
	Color color, background;
	Length margin[4], padding[4];   /* top right bottom left */
	f32 border_w[4];
	u8 border_style[4];      /* 0 none */
	Color border_c[4];
	u8 border_cur;           /* sides whose color follows `color` */
	f32 radius;
	Length width, height, min_width, max_width, min_height, max_height;
	f32 opacity;
	/* flex and grid */
	bool flex_column, flex_wrap;
	u8 justify, align_items, align_self;
	f32 flex_grow, flex_shrink;
	Length flex_basis;
	i16 order;
	f32 row_gap, col_gap;
	char *grid_cols;         /* grid-template-columns as written */
	char *grid_areas;        /* grid-template-areas */
	char *grid_area, *grid_col, *grid_row;   /* placement of a grid item */
	char *mask_image;        /* url: the element is an icon painted through this */
	/* tables */
	f32 border_spacing;
	bool border_collapse;
	char *bg_image;          /* url, if any */
	Var *vars;               /* custom properties defined on this element */
};

void style_free_parts(Style *s);

typedef struct Sheet Sheet;

Sheet *css_parse(const char *src, usize len, const char *base_url);
void sheet_free(Sheet *s);
/* @import urls found while parsing, for the loader. */
int sheet_imports(const Sheet *s, char ***urls);
/* Computes styles for the whole tree from the sheets (in order), presentational
 * attributes and style attributes. */
void style_tree(Node *root, Sheet **sheets, int nsheets, f32 viewport_w, f32 viewport_h);
/* Pages without a doctype get the old "quirks mode" table rules. */
bool html_quirks(const Node *root);
/* Whether a <link media=...> or @import media list applies to a screen this wide. */
bool css_media_matches(const char *media, f32 viewport_w, f32 viewport_h);
bool css_parse_color(const char *s, Color *out);

/* ---- Documents and images ------------------------------------------------------------------- */

enum { IMG_NEW, IMG_LOADING, IMG_READY, IMG_FAILED };

typedef struct Image {
	char *url;
	Canvas *canvas;          /* set once, by a loader thread */
	Canvas *tinted;          /* for masks: the picture in one color (UI thread) */
	Color tint;
	int w, h;                /* intrinsic size in CSS pixels once known */
	int state;
} Image;

typedef struct Page Page;

typedef struct Doc {
	int refs;
	int cancelled;
	char *url;               /* after redirects */
	char *base;              /* <base href>, or NULL */
	char *title;
	Node *root;
	Sheet **sheets;
	int nsheets;
	Mutex lock;              /* guards images and page */
	Image **images;
	int nimages;
	Map image_map;
	int image_wake;          /* futex: bumped when images are wanted */
	int images_done;         /* bumped when one arrives, for relayout */
	Page *page;              /* what the UI shows (UI thread only) */
	Page *new_page;          /* a finished layout waiting for the UI */
	Color theme;             /* <meta name="theme-color">, or 0 */
	int state;               /* DOC_* */
	char error[256];         /* why it failed, or where a download went */
	int progress;            /* per mille, while loading */
	int layout_busy;
	Mutex layout_lock;       /* style and layout run one at a time */
} Doc;

enum { DOC_LOADING, DOC_READY, DOC_FAILED, DOC_DOWNLOADED };

Image *doc_image(Doc *d, const char *url);   /* finds or adds; thread-safe */

/* ---- Layout ---------------------------------------------------------------------------------- */

enum { ITEM_BOX, ITEM_TEXT, ITEM_IMAGE, ITEM_MARKER, ITEM_INPUT };
enum { IF_UNDERLINE = 1, IF_STRIKE = 2, IF_MASK = 4 };
enum { MK_DISC, MK_CIRCLE, MK_SQUARE };

/* What gets painted: boxes, text runs, images and controls at page positions. */
typedef struct {
	u8 kind;
	u8 flags;
	u8 marker;
	Rect r;
	Color color, background;
	f32 border_w[4];
	Color border_c[4];
	f32 radius;
	Font font;
	u32 text, len;           /* into Page.text */
	f32 baseline;
	Image *image;
	Node *link;              /* the <a> this belongs to, for clicks */
	Node *node;              /* the element (controls) */
} Item;

struct Page {
	Item *items;
	int nitems, cap;
	char *text;
	usize text_len, text_cap;
	f32 width, height;
	Color background;
	Map anchors;             /* id -> y (as a pointer-sized integer) */
	char *base;              /* for resolving image addresses */
};

Page *layout_doc(Doc *d, f32 width, f32 viewport_h);
const char *input_button_label(const Node *n);
void page_free(Page *p);
f32 page_anchor(const Page *p, const char *id);  /* -1 if missing */

/* ---- Painting ---------------------------------------------------------------------------------- */

typedef struct {
	const Node *hover_link;
	const char *find;        /* highlight matches of this text */
	int find_current;
	f32 zoom;
	/* out */
	int matches;
	f32 find_y;              /* where the current match is */
	Node *submit;            /* a submit button was clicked, or Enter in a field */
	Node *radio;             /* a radio button was picked */
	bool focused;
} PaintOpts;

void paint_page(Ui *ui, Page *p, Rect view, f32 scroll, PaintOpts *o);
Control *page_control(Node *n);
int select_options(Node *sel, Node **out, int max);
const Item *page_hit(const Page *p, f32 x, f32 y);
