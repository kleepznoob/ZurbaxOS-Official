/* Text entry widgets: single-line fields and a multi-line editor. */
#include "zui.h"

#define CTL_RADIUS_ENTRY 6.0f

typedef struct {
	usize cursor, anchor;   /* byte offsets; selection is [min, max) */
	f32 scroll_x, scroll_y;
	f64 blink_start;
	bool dragging;
	f32 want_x;             /* preferred caret x for vertical movement */
	bool reveal;            /* scroll the caret into view on the next layout */
	f32 menu_x, menu_y;     /* where the right-click menu opened */
} EditState;

static bool is_word(u8 c)
{
	return is_alnum(c) || c == '_' || c >= 0x80;
}

static usize word_left(const char *s, usize pos)
{
	while (pos > 0 && !is_word((u8)s[pos - 1]))
		pos--;
	while (pos > 0 && is_word((u8)s[pos - 1]))
		pos--;
	return pos;
}

static usize word_right(const char *s, usize len, usize pos)
{
	while (pos < len && !is_word((u8)s[pos]))
		pos++;
	while (pos < len && is_word((u8)s[pos]))
		pos++;
	return pos;
}

static void delete_selection(StrBuf *text, EditState *st)
{
	usize a = MIN(st->cursor, st->anchor), b = MAX(st->cursor, st->anchor);
	if (a != b)
		sb_remove(text, a, b - a);
	st->cursor = st->anchor = a;
}

static void insert_text(StrBuf *text, EditState *st, const char *s, usize n)
{
	delete_selection(text, st);
	sb_insert(text, st->cursor, s, n);
	st->cursor += n;
	st->anchor = st->cursor;
}

static char *selection_text(const StrBuf *text, const EditState *st)
{
	usize a = MIN(st->cursor, st->anchor), b = MAX(st->cursor, st->anchor);
	return str_ndup(sb_str(text) + a, b - a);
}

/* Handles keys common to both widgets. Returns ENTRY_* flags. */
static int edit_keys(Ui *ui, StrBuf *text, EditState *st, bool multiline, bool readonly, bool password)
{
	int events = 0;
	for (int i = 0; i < ui->nkeys; i++) {
		UiKey *k = &ui->keys[i];
		if (!k->pressed)
			continue; /* code 0 = text without a key (compose, input methods) */
		bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL;
		usize len = text->len;
		const char *s = sb_str(text);
		bool handled = true, moved = false;
		switch (k->code) {
		case KEY_LEFT:
			if (!shift && st->cursor != st->anchor)
				st->cursor = st->anchor = MIN(st->cursor, st->anchor);
			else
				st->cursor = ctrl ? word_left(s, st->cursor) : utf8_prev(s, st->cursor);
			moved = true;
			break;
		case KEY_RIGHT:
			if (!shift && st->cursor != st->anchor)
				st->cursor = st->anchor = MAX(st->cursor, st->anchor);
			else
				st->cursor = ctrl ? word_right(s, len, st->cursor) : utf8_next(s, len, st->cursor);
			moved = true;
			break;
		case KEY_HOME:
			if (multiline && !ctrl)
				while (st->cursor > 0 && s[st->cursor - 1] != '\n')
					st->cursor--;
			else
				st->cursor = 0;
			moved = true;
			break;
		case KEY_END:
			if (multiline && !ctrl)
				while (st->cursor < len && s[st->cursor] != '\n')
					st->cursor++;
			else
				st->cursor = len;
			moved = true;
			break;
		case KEY_BACKSPACE:
			if (readonly)
				break;
			if (st->cursor == st->anchor)
				st->anchor = ctrl ? word_left(s, st->cursor) : utf8_prev(s, st->cursor);
			delete_selection(text, st);
			events |= ENTRY_CHANGED;
			break;
		case KEY_DELETE:
			if (readonly)
				break;
			if (st->cursor == st->anchor)
				st->anchor = ctrl ? word_right(s, len, st->cursor) : utf8_next(s, len, st->cursor);
			delete_selection(text, st);
			events |= ENTRY_CHANGED;
			break;
		case KEY_ENTER:
		case KEY_KPENTER:
			if (multiline && !readonly && !ctrl) {
				insert_text(text, st, "\n", 1);
				events |= ENTRY_CHANGED;
			} else {
				events |= ENTRY_SUBMIT;
			}
			break;
		case KEY_ESC:
			events |= ENTRY_ESCAPE;
			break;
		case KEY_TAB:
			if (multiline && !readonly) {
				insert_text(text, st, "    ", 4);
				events |= ENTRY_CHANGED;
			} else {
				handled = false;
			}
			break;
		case KEY_UP:
			if (!multiline)
				events |= ENTRY_UP;
			else
				handled = false; /* handled by the multi-line widget with layout info */
			break;
		case KEY_DOWN:
			if (!multiline)
				events |= ENTRY_DOWN;
			else
				handled = false;
			break;
		default:
			handled = false;
		}
		if (!handled && ctrl) {
			handled = true;
			switch (k->code) {
			case KEY_A:
				st->anchor = 0;
				st->cursor = len;
				break;
			case KEY_C:
				if (st->cursor != st->anchor && !password) {
					mem_free(ui->clipboard_out);
					ui->clipboard_out = selection_text(text, st);
				}
				break;
			case KEY_X:
				if (st->cursor != st->anchor && !readonly && !password) {
					mem_free(ui->clipboard_out);
					ui->clipboard_out = selection_text(text, st);
					delete_selection(text, st);
					events |= ENTRY_CHANGED;
				}
				break;
			case KEY_V:
				if (!readonly)
					ui->want_paste = true;
				break;
			default:
				handled = false;
			}
		}
		if (!handled && k->text[0] && !(k->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) && !readonly) {
			if ((u8)k->text[0] >= 0x20 && k->text[0] != 0x7f) {
				insert_text(text, st, k->text, strlen(k->text));
				events |= ENTRY_CHANGED;
				handled = true;
			}
		}
		if (moved && !shift)
			st->anchor = st->cursor;
		if (handled) {
			k->code = 0, k->text[0] = 0;
			st->blink_start = ui->now;
		}
	}
	if (ui->paste_in && !readonly) {
		/* Single-line fields flatten pasted newlines into spaces. */
		char *p = str_dup(ui->paste_in);
		if (!multiline)
			for (char *q = p; *q; q++)
				if (*q == '\n' || *q == '\r' || *q == '\t')
					*q = ' ';
		insert_text(text, st, p, strlen(p));
		mem_free(p);
		mem_free(ui->paste_in);
		ui->paste_in = NULL;
		events |= ENTRY_CHANGED;
	}
	return events;
}

/* A menu item that can't be used right now: shown, dimmed, not clickable. */
static void menu_item_off(Ui *ui, Rect r, const char *icon, const char *label, const char *keys)
{
	Theme *t = &ui->theme;
	ui_icon(ui, rect(r.x + 10, r.y + (r.h - 16) / 2, 16, 16), icon, t->text_faint);
	ui_text_in(ui, ui_font(ui, 13, 500), rect(r.x + 36, r.y, r.w - 120, r.h), label, t->text_faint, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 12, 400), rect(r.x + r.w - 80, r.y, 70, r.h), keys, t->text_faint, ALIGN_END);
}

/* The right-click menu of a text field: Cut, Copy, Paste, Select All. */
static int edit_menu(Ui *ui, UiId eid, UiInteract it, StrBuf *text, EditState *st, bool readonly, bool password)
{
	int events = 0;
	ui_push_id(ui, (i64)eid);
	if (it.right_clicked) {
		ui->focus = eid;
		st->menu_x = ui->mx - ui->ox, st->menu_y = ui->my - ui->oy;
		if (!ui_popover_is_open(ui, "__edit"))
			ui_popover_toggle(ui, "__edit");
	}
	Rect c;
	if (ui_popover_begin(ui, "__edit", rect(st->menu_x, st->menu_y - 6, 1, 1), 220, 12 + 34 * 4 + 9, &c)) {
		bool sel = st->cursor != st->anchor;
		Rect r = cut_top(&c, 34);
		if (sel && !readonly && !password) {
			if (ui_menu_item(ui, r, "edit-cut", "Cut", "Ctrl+X")) {
				mem_free(ui->clipboard_out);
				ui->clipboard_out = selection_text(text, st);
				delete_selection(text, st);
				events |= ENTRY_CHANGED;
			}
		} else {
			menu_item_off(ui, r, "edit-cut", "Cut", "Ctrl+X");
		}
		r = cut_top(&c, 34);
		if (sel && !password) {
			if (ui_menu_item(ui, r, "edit-copy", "Copy", "Ctrl+C")) {
				mem_free(ui->clipboard_out);
				ui->clipboard_out = selection_text(text, st);
			}
		} else {
			menu_item_off(ui, r, "edit-copy", "Copy", "Ctrl+C");
		}
		r = cut_top(&c, 34);
		if (!readonly) {
			if (ui_menu_item(ui, r, "edit-paste", "Paste", "Ctrl+V"))
				ui->want_paste = true;
		} else {
			menu_item_off(ui, r, "edit-paste", "Paste", "Ctrl+V");
		}
		Rect sp = cut_top(&c, 9);
		ui_separator(ui, sp.x + 8, sp.y + 4, sp.x + sp.w - 8);
		if (ui_menu_item(ui, cut_top(&c, 34), "selection-mode", "Select All", "Ctrl+A")) {
			st->anchor = 0;
			st->cursor = text->len;
		}
		ui_popover_end(ui);
	}
	ui_pop_id(ui);
	if (events)
		st->blink_start = ui->now;
	return events;
}

static void schedule_blink(Ui *ui, EditState *st)
{
	f64 phase = fmod(ui->now - st->blink_start, 1.0);
	f64 next = ui->now + (phase < 0.5 ? 0.5 - phase : 1.0 - phase) + 0.001;
	if (!ui->wake_at || next < ui->wake_at)
		ui->wake_at = next;
}

static bool caret_visible(Ui *ui, EditState *st)
{
	return fmod(ui->now - st->blink_start, 1.0) < 0.5;
}

/* ---- Single line ------------------------------------------------------------------------- */

void ui_focus(Ui *ui, const char *id)
{
	ui->focus = ui_id(ui, id);
	ui->redraw = true;
}

usize ui_textarea_cursor(Ui *ui, const char *id)
{
	EditState *st = ui_state(ui, ui_id(ui, id), sizeof *st);
	return st->cursor;
}

void ui_textarea_select(Ui *ui, const char *id, usize start, usize end)
{
	EditState *st = ui_state(ui, ui_id(ui, id), sizeof *st);
	st->anchor = start;
	st->cursor = end;
	st->reveal = true;
	st->blink_start = ui->now;
	ui->redraw = true;
}

bool ui_is_focused(Ui *ui, const char *id)
{
	return ui->focus == ui_id(ui, id);
}

/* Builds the displayed string (bullets for passwords). */
static const char *display_text(const StrBuf *text, bool password, StrBuf *tmp)
{
	if (!password)
		return sb_str(text);
	sb_clear(tmp);
	usize n = utf8_count(sb_str(text), text->len);
	for (usize i = 0; i < n; i++)
		sb_adds(tmp, "\xe2\x80\xa2");
	return sb_str(tmp);
}

/* Maps a byte offset in the real text to one in the displayed text. */
static usize display_offset(const StrBuf *text, usize off, bool password)
{
	return password ? utf8_count(sb_str(text), off) * 3 : off;
}

static usize real_offset(const StrBuf *text, usize disp_off, bool password)
{
	if (!password)
		return disp_off;
	usize chars = disp_off / 3, pos = 0;
	while (chars-- && pos < text->len)
		pos = utf8_next(sb_str(text), text->len, pos);
	return pos;
}

int ui_entry(Ui *ui, Rect r, const char *id, StrBuf *text, const char *placeholder, u32 flags)
{
	Theme *t = &ui->theme;
	UiId eid = ui_id(ui, id);
	EditState *st = ui_state(ui, eid, sizeof *st);
	bool password = flags & ENTRY_PASSWORD, search = flags & ENTRY_SEARCH;
	int events = 0;
	if (st->cursor > text->len)
		st->cursor = st->anchor = text->len;
	if ((flags & ENTRY_AUTOFOCUS) && !ui->focus)
		ui->focus = eid;

	f32 radius = search ? r.h / 2 : CTL_RADIUS_ENTRY;
	Rect inner = pad2(r, 10, 0);
	if (search)
		inner = pad4(r, 0, 30, 0, 32);

	UiInteract it = ui_interact(ui, eid, r);
	if (it.hovered)
		ui_set_cursor(ui, CURSOR_TEXT);
	bool focused = ui->focus == eid && ui->window_focused;
	Font f = ui_font(ui, 13, 400);
	StrBuf tmp = { 0 };
	const char *shown = display_text(text, password, &tmp);
	usize shown_len = strlen(shown);

	if (it.pressed_now) {
		if (ui->focus != eid)
			events |= ENTRY_FOCUSED;
		ui->focus = eid;
		ui->focus_claimed = true;
		usize hit = text_hit(font(f.size * ui->scale, f.weight), shown, shown_len,
				     (ui->mx - ui->ox - inner.x + st->scroll_x) * ui->scale);
		usize pos = real_offset(text, hit, password);
		if (ui->click_count == 2 && !password) {
			st->anchor = word_left(sb_str(text), pos);
			st->cursor = word_right(sb_str(text), text->len, pos);
		} else if (ui->click_count >= 3) {
			st->anchor = 0;
			st->cursor = text->len;
		} else {
			st->cursor = pos;
			if (!(ui->mods & MOD_SHIFT))
				st->anchor = pos;
			st->dragging = true;
		}
		st->blink_start = ui->now;
	} else if (it.pressed_now == false && ui->pressed[BUTTON_LEFT] && ui->focus == eid && !it.hovered) {
		/* clicked elsewhere: focus handling happens in ui_frame */
	}
	if (st->dragging) {
		if (ui->down[BUTTON_LEFT]) {
			usize hit = text_hit(font(f.size * ui->scale, f.weight), shown, shown_len,
					     (ui->mx - ui->ox - inner.x + st->scroll_x) * ui->scale);
			st->cursor = real_offset(text, hit, password);
		} else {
			st->dragging = false;
		}
	}
	if (ui->focus == eid) {
		events |= edit_keys(ui, text, st, false, false, password);
		shown = display_text(text, password, &tmp);
		shown_len = strlen(shown);
		if (search && (events & ENTRY_ESCAPE) && text->len) {
			sb_clear(text);
			st->cursor = st->anchor = 0;
			events |= ENTRY_CHANGED;
		}
	}

	/* Frame */
	if (!(flags & ENTRY_FLAT)) {
		ui_round(ui, r, radius, t->input);
		if (focused) {
			ui_border(ui, rect_inset(r, -2, -2), radius + 2, 2, color_with_alpha(t->accent, 0x40));
			ui_border(ui, r, radius, 1, t->accent);
		} else {
			ui_border(ui, r, radius, 1, it.hovered ? color_mix(t->border_strong, t->text_faint, 0.4f) : t->border_strong);
		}
	}
	if (search) {
		ui_icon(ui, rect(r.x + 10, r.y + (r.h - 16) / 2, 16, 16), "search", t->text_faint);
		if (text->len) {
			Rect clear = { r.x + r.w - 28, r.y + (r.h - 22) / 2, 22, 22 };
			UiId cid = ui_id(ui, "__clear");
			UiInteract ci = ui_interact(ui, cid ^ eid, clear);
			ui_icon(ui, pad(clear, 4), "close", ci.hovered ? t->text : t->text_faint);
			if (ci.clicked) {
				sb_clear(text);
				st->cursor = st->anchor = 0;
				events |= ENTRY_CHANGED;
				ui->focus = eid;
				ui->focus_claimed = true;
				shown = display_text(text, password, &tmp);
				shown_len = strlen(shown);
			}
		}
	}

	/* Keep the caret in view. */
	Font df = font(f.size * ui->scale, f.weight);
	f32 caret_x = text_width(df, shown, display_offset(text, st->cursor, password)) / ui->scale;
	if (caret_x - st->scroll_x > inner.w - 2)
		st->scroll_x = caret_x - inner.w + 2;
	if (caret_x - st->scroll_x < 0)
		st->scroll_x = caret_x;
	f32 total_w = text_width(df, shown, shown_len) / ui->scale;
	if (total_w - st->scroll_x < inner.w)
		st->scroll_x = maxf(0, total_w - inner.w + 2);

	ui_push_clip(ui, inner);
	f32 baseline = r.y + (r.h + font_cap_height(f)) / 2;
	f32 x0 = inner.x - st->scroll_x;
	if (!text->len && placeholder)
		ui_text(ui, f, inner.x, baseline, placeholder, t->text_faint);
	if (focused && st->cursor != st->anchor) {
		usize a = MIN(st->cursor, st->anchor), b = MAX(st->cursor, st->anchor);
		f32 sx0 = text_width(df, shown, display_offset(text, a, password)) / ui->scale;
		f32 sx1 = text_width(df, shown, display_offset(text, b, password)) / ui->scale;
		ui_round(ui, rect(x0 + sx0, r.y + (r.h - 20) / 2, sx1 - sx0, 20), 3, color_with_alpha(t->accent, 0x55));
	}
	ui_text_n(ui, f, x0, baseline, shown, shown_len, t->text);
	if (focused) {
		if (caret_visible(ui, st))
			ui_fill(ui, rect(x0 + caret_x - 0.5f, r.y + (r.h - 18) / 2, 1.5f, 18), t->accent);
		schedule_blink(ui, st);
	}
	ui_pop_clip(ui);
	sb_free(&tmp);
	events |= edit_menu(ui, eid, it, text, st, false, password);
	return events;
}

/* ---- Multi-line editor ------------------------------------------------------------------- */

typedef struct {
	usize start, len; /* byte range of a visual line */
} VLine;

int ui_textarea(Ui *ui, Rect r, const char *id, StrBuf *text, f32 font_size, u32 flags)
{
	Theme *t = &ui->theme;
	UiId eid = ui_id(ui, id);
	EditState *st = ui_state(ui, eid, sizeof *st);
	bool readonly = flags & TEXT_READONLY, wrap = !(flags & TEXT_NO_WRAP);
	int events = 0;
	if (st->cursor > text->len)
		st->cursor = st->anchor = text->len;
	Font f = (flags & TEXT_MONO) ? ui_font_mono(ui, font_size, 400) : ui_font(ui, font_size, 400);
	Font df = f;
	df.size *= ui->scale;
	f32 lh = roundf(font_line_height(f) * 1.1f);
	if (!(flags & TEXT_NO_FRAME)) {
		ui_round(ui, r, CTL_RADIUS_ENTRY, t->view);
		ui_border(ui, r, CTL_RADIUS_ENTRY, 1, ui->focus == eid ? t->accent : t->border_strong);
	}
	Rect inner = pad(r, (flags & TEXT_NO_FRAME) ? 0 : 10);
	f32 wrap_w = wrap ? (inner.w - 10) * ui->scale : 1e9f;

	/* Lay out visual lines. */
	VEC(VLine) lines = { 0 };
	const char *s = sb_str(text);
	usize pos = 0;
	for (;;) {
		usize eol = pos;
		while (eol < text->len && s[eol] != '\n')
			eol++;
		if (eol == pos) {
			vec_push(&lines, ((VLine){ pos, 0 }));
		} else {
			int n = text_wrap(df, s + pos, eol - pos, wrap_w, NULL, 0);
			TextLine *tl = mem_alloc(sizeof(TextLine) * (usize)MAX(n, 1));
			text_wrap(df, s + pos, eol - pos, wrap_w, tl, n);
			for (int i = 0; i < n; i++) {
				usize start = pos + tl[i].start;
				usize end = i + 1 < n ? pos + tl[i + 1].start : eol;
				vec_push(&lines, ((VLine){ start, end - start }));
			}
			mem_free(tl);
		}
		if (eol >= text->len)
			break;
		pos = eol + 1;
	}

	/* Locate the caret's visual line. */
	usize cline = 0;
	for (usize i = 0; i < lines.len; i++)
		if (st->cursor >= lines.data[i].start)
			cline = i;
	f32 content_h = (f32)lines.len * lh;

	UiInteract it = ui_interact(ui, eid, inner);
	if (it.hovered)
		ui_set_cursor(ui, CURSOR_TEXT);
	if (it.hovered && ui->scroll_dy != 0) {
		st->scroll_y = clampf(st->scroll_y + ui->scroll_dy, 0, maxf(0, content_h - inner.h));
		ui->scroll_dy = 0;
	}
	/* Hit testing a point to a byte offset. */
	#define HIT(px, py) ({                                                                   \
		f32 _ly = (py) - ui->oy - inner.y + st->scroll_y;                                \
		isize _li = (isize)(_ly / lh);                                                   \
		_li = CLAMP(_li, 0, (isize)lines.len - 1);                                       \
		VLine _vl = lines.data[_li];                                                     \
		usize _n = _vl.len;                                                              \
		if (_n && s[_vl.start + _n - 1] == ' ' && (usize)_li + 1 < lines.len) _n--;       \
		_vl.start + text_hit(df, s + _vl.start, _n, ((px) - ui->ox - inner.x) * ui->scale); \
	})
	if (it.pressed_now) {
		ui->focus = eid;
		ui->focus_claimed = true;
		usize p = HIT(ui->mx, ui->my);
		if (ui->click_count == 2) {
			st->anchor = word_left(s, p);
			st->cursor = word_right(s, text->len, p);
		} else if (ui->click_count >= 3) {
			st->anchor = p;
			while (st->anchor > 0 && s[st->anchor - 1] != '\n')
				st->anchor--;
			st->cursor = p;
			while (st->cursor < text->len && s[st->cursor] != '\n')
				st->cursor++;
		} else {
			st->cursor = p;
			if (!(ui->mods & MOD_SHIFT))
				st->anchor = p;
			st->dragging = true;
		}
		st->blink_start = ui->now;
		st->want_x = -1;
	}
	if (st->dragging) {
		if (ui->down[BUTTON_LEFT]) {
			st->cursor = HIT(ui->mx, ui->my);
			if (ui->my - ui->oy > inner.y + inner.h)
				st->scroll_y = minf(st->scroll_y + 8, maxf(0, content_h - inner.h)), ui->redraw = true;
			if (ui->my - ui->oy < inner.y)
				st->scroll_y = maxf(0, st->scroll_y - 8), ui->redraw = true;
		} else {
			st->dragging = false;
		}
	}
	if (ui->focus == eid) {
		/* Vertical movement needs the layout. */
		for (int i = 0; i < ui->nkeys; i++) {
			UiKey *k = &ui->keys[i];
			if (!k->pressed || (k->code != KEY_UP && k->code != KEY_DOWN && k->code != KEY_PAGEUP && k->code != KEY_PAGEDOWN))
				continue;
			int dir = (k->code == KEY_UP || k->code == KEY_PAGEUP) ? -1 : 1;
			int step = (k->code == KEY_PAGEUP || k->code == KEY_PAGEDOWN) ? (int)(inner.h / lh) : 1;
			VLine cur = lines.data[cline];
			if (st->want_x < 0)
				st->want_x = text_width(df, s + cur.start, st->cursor - cur.start) / ui->scale;
			isize target = (isize)cline + dir * step;
			target = CLAMP(target, 0, (isize)lines.len - 1);
			VLine tl = lines.data[target];
			usize n = tl.len;
			if (n && s[tl.start + n - 1] == ' ' && (usize)target + 1 < lines.len)
				n--;
			st->cursor = tl.start + text_hit(df, s + tl.start, n, st->want_x * ui->scale);
			if (!(k->mods & MOD_SHIFT))
				st->anchor = st->cursor;
			k->code = 0, k->text[0] = 0;
			st->blink_start = ui->now;
		}
		int ev = edit_keys(ui, text, st, true, readonly, false);
		if (ev)
			st->want_x = -1;
		events |= ev;
		if (ev & ENTRY_CHANGED) {
			vec_free(&lines);
			ui->redraw = true; /* re-layout next frame */
			return events;
		}
		for (usize i = 0; i < lines.len; i++)
			if (st->cursor >= lines.data[i].start)
				cline = i;
		/* Keep the caret visible. */
		f32 cy = (f32)cline * lh;
		if (cy < st->scroll_y)
			st->scroll_y = cy;
		if (cy + lh > st->scroll_y + inner.h)
			st->scroll_y = cy + lh - inner.h;
	}
	if (st->reveal) {
		st->reveal = false;
		usize cl = 0;
		for (usize i = 0; i < lines.len; i++)
			if (st->cursor >= lines.data[i].start)
				cl = i;
		f32 cy = (f32)cl * lh;
		if (cy < st->scroll_y || cy + lh > st->scroll_y + inner.h)
			st->scroll_y = cy - inner.h / 3;
	}
	st->scroll_y = clampf(st->scroll_y, 0, maxf(0, content_h - inner.h));

	ui_push_clip(ui, inner);
	usize a = MIN(st->cursor, st->anchor), b = MAX(st->cursor, st->anchor);
	bool focused = ui->focus == eid && ui->window_focused;
	isize first = (isize)(st->scroll_y / lh);
	isize last = (isize)((st->scroll_y + inner.h) / lh) + 1;
	for (isize i = MAX(first, 0); i < (isize)lines.len && i <= last; i++) {
		VLine vl = lines.data[i];
		f32 y = inner.y + (f32)i * lh - st->scroll_y;
		f32 baseline = y + (lh + font_cap_height(f)) / 2;
		usize ls = vl.start, le = vl.start + vl.len;
		if (a != b && b > ls && a <= le) {
			usize sa = MAX(a, ls), sb = MIN(b, le);
			f32 x0 = text_width(df, s + ls, sa - ls) / ui->scale;
			f32 x1 = text_width(df, s + ls, sb - ls) / ui->scale;
			if (b > le && (le < text->len && s[le] == '\n'))
				x1 += 6;
			ui_fill(ui, rect(inner.x + x0, y, maxf(x1 - x0, 2), lh), color_with_alpha(t->accent, focused ? 0x50 : 0x28));
		}
		usize n = vl.len;
		while (n && (s[ls + n - 1] == '\n'))
			n--;
		ui_text_n(ui, f, inner.x, baseline, s + ls, n, t->text);
		if (focused && (usize)i == cline && caret_visible(ui, st)) {
			f32 cx = text_width(df, s + ls, st->cursor - ls) / ui->scale;
			ui_fill(ui, rect(inner.x + cx - 0.5f, y + 2, 1.5f, lh - 4), t->accent);
		}
	}
	if (focused)
		schedule_blink(ui, st);
	ui_pop_clip(ui);
	if (content_h > inner.h) {
		f32 bar_h = maxf(30, inner.h * inner.h / content_h);
		f32 bar_y = inner.y + (inner.h - bar_h) * (st->scroll_y / (content_h - inner.h));
		ui_round(ui, rect(r.x + r.w - 7, bar_y, 4, bar_h), 2, color_hex(t->dark ? "#ffffff40" : "#00000035"));
	}
	vec_free(&lines);
	#undef HIT
	events |= edit_menu(ui, eid, it, text, st, readonly, false);
	return events;
}
