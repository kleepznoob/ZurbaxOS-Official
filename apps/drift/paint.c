/* Painting a laid-out page, and the form controls on it. */
#include "drift.h"

static Control *control_of(Node *n)
{
	if (n->control)
		return n->control;
	Control *c = NEW(Control);
	if (str_eq(n->tag, "textarea")) {
		StrBuf t = { 0 };
		node_text_content(n, &t);
		const char *s = sb_str(&t);
		if (*s == '\n')
			s++;
		sb_adds(&c->value, s);
		sb_free(&t);
	} else {
		const char *v = node_attr(n, "value");
		if (v)
			sb_adds(&c->value, v);
	}
	c->checked = node_attr(n, "checked") != NULL;
	c->selected = 0;
	if (str_eq(n->tag, "select")) {
		int i = 0;
		for (Node *o = n->first; o; o = o->next) {
			Node *opts[64];
			int no = 0;
			if (str_eq(o->tag, "optgroup")) {
				for (Node *k = o->first; k && no < 64; k = k->next)
					if (k->type == NODE_ELEMENT && str_eq(k->tag, "option"))
						opts[no++] = k;
			} else if (o->type == NODE_ELEMENT && str_eq(o->tag, "option")) {
				opts[no++] = o;
			}
			for (int k = 0; k < no; k++, i++)
				if (node_attr(opts[k], "selected"))
					c->selected = i;
		}
	}
	n->control = c;
	return c;
}

Control *page_control(Node *n)
{
	return control_of(n);
}

/* Options of a <select>, in order. */
int select_options(Node *sel, Node **out, int max)
{
	int n = 0;
	for (Node *o = sel->first; o && n < max; o = o->next) {
		if (o->type != NODE_ELEMENT)
			continue;
		if (str_eq(o->tag, "optgroup")) {
			for (Node *k = o->first; k && n < max; k = k->next)
				if (k->type == NODE_ELEMENT && str_eq(k->tag, "option"))
					out[n++] = k;
		} else if (str_eq(o->tag, "option")) {
			out[n++] = o;
		}
	}
	return n;
}

static Rect zr(Rect r, Rect view, f32 scroll, f32 z)
{
	return rect(view.x + r.x * z, view.y + (r.y - scroll) * z, r.w * z, r.h * z);
}

static void paint_box(Ui *ui, const Item *it, Rect r, f32 z)
{
	f32 rad = it->radius * z;
	if (COLOR_A(it->background)) {
		if (rad > 0.5f)
			ui_round(ui, r, rad, it->background);
		else
			ui_fill(ui, r, it->background);
	}
	f32 bt = it->border_w[0] * z, br = it->border_w[1] * z, bb = it->border_w[2] * z, bl = it->border_w[3] * z;
	if (bt + br + bb + bl <= 0)
		return;
	bool uniform = bt == br && br == bb && bb == bl && it->border_c[0] == it->border_c[1] && it->border_c[1] == it->border_c[2] &&
		       it->border_c[2] == it->border_c[3];
	if (uniform && (rad > 0.5f || bt >= 1)) {
		if (COLOR_A(it->border_c[0]))
			ui_border(ui, rect_inset(r, bt / 2, bt / 2), maxf(0, rad - bt / 2), bt, it->border_c[0]);
		return;
	}
	if (bt > 0 && COLOR_A(it->border_c[0]))
		ui_fill(ui, rect(r.x, r.y, r.w, maxf(bt, 1 / ui->scale)), it->border_c[0]);
	if (bb > 0 && COLOR_A(it->border_c[2]))
		ui_fill(ui, rect(r.x, r.y + r.h - maxf(bb, 1 / ui->scale), r.w, maxf(bb, 1 / ui->scale)), it->border_c[2]);
	if (bl > 0 && COLOR_A(it->border_c[3]))
		ui_fill(ui, rect(r.x, r.y, maxf(bl, 1 / ui->scale), r.h), it->border_c[3]);
	if (br > 0 && COLOR_A(it->border_c[1]))
		ui_fill(ui, rect(r.x + r.w - maxf(br, 1 / ui->scale), r.y, maxf(br, 1 / ui->scale), r.h), it->border_c[1]);
}

static Canvas *tint(const Canvas *src, Color c)
{
	Canvas *out = canvas_new(src->width, src->height);
	u32 p = color_premul(c);
	u32 pa = p >> 24, pr = (p >> 16) & 0xff, pg = (p >> 8) & 0xff, pb = p & 0xff;
	for (int y = 0; y < src->height; y++) {
		const u32 *s = canvas_row(src, y);
		u32 *d = canvas_row(out, y);
		for (int x = 0; x < src->width; x++) {
			u32 a = s[x] >> 24;
			d[x] = ((pa * a / 255) << 24) | ((pr * a / 255) << 16) | ((pg * a / 255) << 8) | (pb * a / 255);
		}
	}
	return out;
}

static void draw_page_image(Ui *ui, const Item *it, Rect r, f32 z)
{
	Image *im = it->image;
	if ((it->flags & IF_MASK) && im->state == IMG_READY && im->canvas) {
		if (!im->tinted || im->tint != it->color) {
			canvas_free(im->tinted);
			im->tinted = tint(im->canvas, it->color);
			im->tint = it->color;
		}
		ui_image(ui, r, im->tinted, 0, false);
		return;
	}
	if (it->flags & IF_MASK)
		return;
	if (im->state == IMG_READY && im->canvas) {
		Rect d = ui_to_device(ui, r);
		const Canvas *c = im->canvas;
		blit_scaled_round(ui->canvas, c, rect(0, 0, (f32)c->width, (f32)c->height), d, it->radius * z * ui->scale, 1);
	} else if (im->state == IMG_LOADING || im->state == IMG_NEW) {
		ui_round(ui, r, minf(4, it->radius * z), 0x10000000);
	}
}

/* Case-insensitive search in n bytes. */
static const char *find_in(const char *s, usize n, const char *needle, usize nl)
{
	if (!nl || nl > n)
		return NULL;
	for (usize i = 0; i + nl <= n; i++)
		if (str_eq_nocase_n(s + i, needle, nl))
			return s + i;
	return NULL;
}

static void paint_input(Ui *ui, Item *it, Rect r, f32 z, PaintOpts *o)
{
	Node *n = it->node;
	Control *c = control_of(n);
	Theme *t = &ui->theme;
	char id[40];
	fmt(id, sizeof id, "drift-%p", (void *)n);
	Font f = it->font;
	f.size *= z;
	if (str_eq(n->tag, "input")) {
		const char *type = node_attr(n, "type");
		if (!type || !*type)
			type = "text";
		UiId uid = ui_id(ui, id);
		if (str_eq_nocase(type, "checkbox") || str_eq_nocase(type, "radio")) {
			bool radio = str_eq_nocase(type, "radio");
			UiInteract in = ui_interact(ui, uid, r);
			Rect b = center_in(r, 14 * z, 14 * z);
			Color fill = c->checked ? t->accent : 0xffffffff;
			if (radio) {
				ui_circle(ui, b.x + b.w / 2, b.y + b.h / 2, b.w / 2, fill);
				ui_ring(ui, b.x + b.w / 2, b.y + b.h / 2, b.w / 2 - 0.5f, 1, c->checked ? t->accent : (in.hovered ? 0xff6a6a6a : 0xff8c8c8c));
				if (c->checked)
					ui_circle(ui, b.x + b.w / 2, b.y + b.h / 2, b.w * 0.2f, 0xffffffff);
			} else {
				ui_round(ui, b, 3 * z, fill);
				ui_border(ui, rect_inset(b, 0.5f, 0.5f), 2.5f * z, 1, c->checked ? t->accent : (in.hovered ? 0xff6a6a6a : 0xff8c8c8c));
				if (c->checked) {
					ui_line(ui, b.x + b.w * 0.24f, b.y + b.h * 0.52f, b.x + b.w * 0.43f, b.y + b.h * 0.72f, 1.8f * z, 0xffffffff);
					ui_line(ui, b.x + b.w * 0.43f, b.y + b.h * 0.72f, b.x + b.w * 0.78f, b.y + b.h * 0.3f, 1.8f * z, 0xffffffff);
				}
			}
			if (in.clicked && !node_attr(n, "disabled")) {
				if (radio) {
					o->radio = n;
				} else {
					c->checked = !c->checked;
				}
			}
			return;
		}
		if (str_eq_nocase(type, "submit") || str_eq_nocase(type, "button") || str_eq_nocase(type, "reset") ||
		    str_eq_nocase(type, "file") || str_eq_nocase(type, "image")) {
			UiInteract in = ui_interact(ui, uid, r);
			if (in.hovered) {
				ui_round(ui, r, 3 * z, 0x0c000000);
				ui_set_cursor(ui, CURSOR_POINTER);
			}
			if (in.held)
				ui_round(ui, r, 3 * z, 0x14000000);
			const char *label = input_button_label(n);
			ui_text_in(ui, f, r, label, it->color, ALIGN_CENTER);
			if (in.clicked && str_eq_nocase(type, "submit"))
				o->submit = n;
			return;
		}
		if (str_eq_nocase(type, "range")) {
			f32 cy = r.y + r.h / 2;
			ui_round(ui, rect(r.x, cy - 2 * z, r.w, 4 * z), 2 * z, 0xffc8c8c8);
			const char *v = node_attr(n, "value");
			f32 mn = 0, mx = 100;
			if (node_attr(n, "min"))
				mn = (f32)str_to_float(node_attr(n, "min"), NULL);
			if (node_attr(n, "max"))
				mx = (f32)str_to_float(node_attr(n, "max"), NULL);
			f32 val = c->value.len ? (f32)str_to_float(sb_str(&c->value), NULL) : v ? (f32)str_to_float(v, NULL) : (mn + mx) / 2;
			f32 k = mx > mn ? clampf((val - mn) / (mx - mn), 0, 1) : 0.5f;
			ui_circle(ui, r.x + r.w * k, cy, 7 * z, t->accent);
			return;
		}
		u32 flags = ENTRY_FLAT;
		if (str_eq_nocase(type, "password"))
			flags |= ENTRY_PASSWORD;
		const char *ph = node_attr(n, "placeholder");
		int ev = ui_entry(ui, r, id, &c->value, ph ? ph : "", flags);
		if (ui_is_focused(ui, id))
			ui_border(ui, rect_inset(r, -1, -1), 5 * z, 2, color_with_alpha(t->accent, 0xaa));
		if (ev & ENTRY_SUBMIT)
			o->submit = n;
		if (ev & ENTRY_FOCUSED)
			o->focused = true;
		return;
	}
	if (str_eq(n->tag, "select")) {
		Node *opts[256];
		int no = select_options(n, opts, 256);
		const char *labels[256];
		StrBuf text[256];
		for (int i = 0; i < no; i++) {
			memset(&text[i], 0, sizeof text[i]);
			node_text_content(opts[i], &text[i]);
			char *s = text[i].data ? str_trim(text[i].data) : "";
			labels[i] = s;
		}
		if (no)
			ui_dropdown(ui, r, id, labels, no, &c->selected);
		for (int i = 0; i < no; i++)
			sb_free(&text[i]);
		return;
	}
	if (str_eq(n->tag, "textarea")) {
		ui_textarea(ui, r, id, &c->value, maxf(11, f.size), TEXT_NO_FRAME);
		if (ui_is_focused(ui, id))
			ui_border(ui, rect_inset(r, -1, -1), 5 * z, 2, color_with_alpha(t->accent, 0xaa));
		return;
	}
	/* progress and meter */
	f32 mx = node_attr(n, "max") ? (f32)str_to_float(node_attr(n, "max"), NULL) : 1;
	f32 v = node_attr(n, "value") ? (f32)str_to_float(node_attr(n, "value"), NULL) : 0;
	ui_round(ui, r, r.h / 2, 0xffdedede);
	if (mx > 0 && v > 0)
		ui_round(ui, rect(r.x, r.y, r.w * clampf(v / mx, 0, 1), r.h), r.h / 2, t->accent);
}

void paint_page(Ui *ui, Page *p, Rect view, f32 scroll, PaintOpts *o)
{
	f32 z = o->zoom > 0 ? o->zoom : 1;
	ui_push_clip(ui, view);
	ui_fill(ui, view, p->background);
	f32 top = scroll, bottom = scroll + view.h / z;
	usize fl = o->find ? strlen(o->find) : 0;
	int match = 0;
	o->radio = NULL;
	o->submit = NULL;
	o->focused = false;
	for (int i = 0; i < p->nitems; i++) {
		Item *it = &p->items[i];
		bool visible = !(it->r.y > bottom || it->r.y + it->r.h < top);
		if (!visible && !(fl && it->kind == ITEM_TEXT))
			continue;
		Rect r = zr(it->r, view, scroll, z);
		switch (it->kind) {
		case ITEM_BOX:
			paint_box(ui, it, r, z);
			break;
		case ITEM_TEXT: {
			const char *s = p->text + it->text;
			if (fl) {
				const char *at = s;
				while ((at = find_in(at, it->len - (usize)(at - s), o->find, fl))) {
					if (visible) {
						f32 x0 = text_width(it->font, s, (usize)(at - s));
						f32 w = text_width(it->font, at, fl);
						Rect hr = rect(r.x + x0 * z - 1, r.y - 1, w * z + 2, r.h + 2);
						ui_round(ui, hr, 3, match == o->find_current ? 0xffff9f2e : 0xfffde36a);
					}
					if (match == o->find_current)
						o->find_y = it->r.y;
					match++;
					at += fl;
				}
				if (!visible)
					break;
			}
			Font f = it->font;
			f.size *= z;
			f32 bl = view.y + (it->baseline - scroll) * z;
			ui_text_n(ui, f, r.x, bl, s, it->len, it->color);
			bool hover = o->hover_link && it->link == o->hover_link && str_eq(it->link->tag, "a");
			if ((it->flags & IF_UNDERLINE) || hover) {
				f32 th = maxf(1, f.size / 14);
				ui_fill(ui, rect(r.x, bl + maxf(1.5f, f.size * 0.12f), r.w, th), it->color);
			}
			if (it->flags & IF_STRIKE) {
				f32 th = maxf(1, f.size / 14);
				ui_fill(ui, rect(r.x, bl - font_x_height(f) * 0.5f, r.w, th), it->color);
			}
			break;
		}
		case ITEM_IMAGE:
			draw_page_image(ui, it, r, z);
			break;
		case ITEM_MARKER: {
			f32 cx = r.x + r.w / 2, cy = r.y + r.h / 2;
			if (it->marker == MK_DISC)
				ui_circle(ui, cx, cy, r.w / 2, it->color);
			else if (it->marker == MK_CIRCLE)
				ui_ring(ui, cx, cy, r.w / 2 - 0.5f, 1, it->color);
			else
				ui_fill(ui, r, it->color);
			break;
		}
		case ITEM_INPUT:
			if (visible)
				paint_input(ui, it, r, z, o);
			break;
		}
	}
	o->matches = match;
	ui_pop_clip(ui);
}

const Item *page_hit(const Page *p, f32 x, f32 y)
{
	for (int i = p->nitems - 1; i >= 0; i--) {
		const Item *it = &p->items[i];
		if (!rect_contains(it->r, x, y))
			continue;
		if (it->link || it->kind == ITEM_INPUT)
			return it;
	}
	return NULL;
}
