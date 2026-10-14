/*
 * Clipboard history: the last things copied, text and files, one Super+V
 * away. Picking one puts it back on the clipboard and pastes it into the app
 * in front. Kept in memory only, so nothing copied outlives the session.
 */
#include "zdisplay.h"

#define CLIP_MAX 30
#define CLIP_TEXT_MAX (64u << 10)

typedef struct {
	char mime[48];
	char *data;
	usize len;
	i64 time;
} ClipItem;

static ClipItem items[CLIP_MAX];   /* newest first */
static int nitems;
static bool open_;
static int cursor;
static u64 opened_frame;

bool clip_history_open(void)
{
	return open_;
}

void clip_history_toggle(void)
{
	open_ = !open_;
	cursor = 0;
	opened_frame = 0;
	shell_mark_dirty();
}

static void drop_item(int i)
{
	mem_free(items[i].data);
	memmove(&items[i], &items[i + 1], sizeof(ClipItem) * (usize)(nitems - i - 1));
	nitems--;
	cursor = CLAMP(cursor, 0, MAX(nitems - 1, 0));
}

void clip_history_add(const char *mime, const char *data, usize len)
{
	/* turned off in Settings: nothing is kept, and what was is forgotten */
	if (!S.set.clip_history) {
		while (nitems)
			drop_item(0);
		return;
	}
	bool files = str_eq(mime, CLIPBOARD_FILES);
	if (!data || !len || len > CLIP_TEXT_MAX || (!files && !str_starts(mime, "text/") && mime[0]))
		return;
	/* blank text (an app clearing the clipboard) isn't kept */
	bool blank = true;
	for (usize i = 0; i < len && blank; i++)
		blank = data[i] == ' ' || data[i] == '\n' || data[i] == '\t' || data[i] == '\r';
	if (blank)
		return;
	/* the same again moves to the top */
	for (int i = 0; i < nitems; i++)
		if (items[i].len == len && memcmp(items[i].data, data, len) == 0) {
			drop_item(i);
			break;
		}
	if (nitems == CLIP_MAX)
		drop_item(CLIP_MAX - 1);
	memmove(&items[1], &items[0], sizeof(ClipItem) * (usize)nitems);
	ClipItem *it = &items[0];
	str_copy(it->mime, mime[0] ? mime : "text/plain", sizeof it->mime);
	it->data = mem_alloc(len + 1);
	memcpy(it->data, data, len);
	it->data[len] = 0;
	it->len = len;
	it->time = time_unix();
	nitems++;
	if (open_)
		shell_mark_dirty();
}

/* Back on the clipboard, and pasted into the window in front. */
static void choose(int i)
{
	if (i < 0 || i >= nitems)
		return;
	char mime[48];
	str_copy(mime, items[i].mime, sizeof mime);
	usize len = items[i].len;
	mem_free(S.clipboard);
	S.clipboard = mem_alloc(len + 1);
	memcpy(S.clipboard, items[i].data, len + 1);
	S.clipboard_len = len;
	str_copy(S.clipboard_mime, mime, sizeof S.clipboard_mime);
	clip_history_add(mime, S.clipboard, len); /* to the top */
	wl_clipboard_changed();
	open_ = false;
	shell_mark_dirty();
	Window *w = S.focus;
	if (w && w->client && !w->wl) {
		u32 mods = S.mods;
		S.mods = MOD_CTRL;
		server_key_event(w, KEY_V, KEY_PRESSED, "");
		server_key_event(w, KEY_V, KEY_RELEASED, "");
		S.mods = mods;
	}
}

static void preview(const ClipItem *it, char *out, usize cap, int *lines_out)
{
	out[0] = 0;
	int lines = 0;
	if (str_eq(it->mime, CLIPBOARD_FILES)) {
		usize n;
		char **parts = str_split(it->data, '\n', &n);
		int count = 0;
		StrBuf names = { 0 };
		for (usize k = 1; k < n; k++) {
			const char *p = str_trim(parts[k]);
			if (*p != '/')
				continue;
			if (count < 3)
				sb_fmt(&names, "%s%s", count ? ", " : "", path_base(p));
			count++;
		}
		fmt(out, cap, "%d %s%s: %s%s", count, count == 1 ? "item" : "items", str_starts(it->data, "cut") ? " to move" : "",
		    sb_str(&names), count > 3 ? "…" : "");
		sb_free(&names);
		str_split_free(parts, n);
		*lines_out = 1;
		return;
	}
	/* the first few lines of the text, tidied */
	usize o = 0;
	const char *s = it->data;
	while (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r')
		s++;
	for (; *s && o + 4 < cap && lines < 3; s++) {
		if (*s == '\n') {
			lines++;
			if (lines < 3)
				out[o++] = '\n';
			while (s[1] == '\n' || s[1] == '\r')
				s++;
		} else if (*s == '\t' || *s == '\r') {
			out[o++] = ' ';
		} else {
			out[o++] = *s;
		}
	}
	out[o] = 0;
	*lines_out = MIN(lines + 1, 3);
}

static void ago(i64 when, char *out, usize cap)
{
	i64 d = time_unix() - when;
	if (d < 60)
		str_copy(out, "Just now", cap);
	else if (d < 3600)
		fmt(out, cap, "%d min ago", (int)(d / 60));
	else
		fmt(out, cap, "%d h ago", (int)(d / 3600));
}

void clip_history_draw(Ui *ui)
{
	if (!open_)
		return;
	Theme *t = &ui->theme;
	/* as tall as what's in it, up to a point */
	f32 content = 0;
	for (int i = 0; i < nitems; i++) {
		char text[400];
		int lines;
		preview(&items[i], text, sizeof text, &lines);
		content += 30 + 18 * (f32)lines + 8;
	}
	f32 w = 440, h = minf(minf(520, S.sh - shell_panel_height() - shell_taskbar_reserved() - 40), 14 + 34 + 8 + maxf(content, 170) + 14);
	Rect r = rect(roundf((S.sw - w) / 2), S.sh - shell_taskbar_reserved() - h - 14, w, h);
	ui_shadow(ui, r, 14, 26, 10, color_hex("#00000060"));
	ui_round(ui, r, 14, t->card);
	ui_border(ui, r, 14, 1, t->border);
	shell_surface(r);
	Rect c = pad(r, 14);
	Rect head = cut_top(&c, 34);
	ui_icon(ui, rect(head.x, head.y + 8, 18, 18), "edit-paste", t->accent);
	ui_text(ui, ui_font(ui, 16, 700), head.x + 28, head.y + 23, "Clipboard", t->text);
	if (nitems && ui_button_flat(ui, cut_right(&head, 96), "Clear All")) {
		while (nitems)
			drop_item(0);
	}
	cut_top(&c, 8);
	if (!S.set.clip_history) {
		while (nitems)
			drop_item(0);
		ui_placeholder(ui, c, "edit-paste", "Clipboard history is off", "Turn it on in Settings, under Privacy & Security.");
	} else if (!nitems) {
		ui_placeholder(ui, c, "edit-paste", "Nothing copied yet", "Text and files you copy show up here.");
	} else {
		f32 y = ui_scroll_begin(ui, c, "clip-scroll");
		for (int i = 0; i < nitems; i++) {
			char text[400], when[32];
			int lines;
			preview(&items[i], text, sizeof text, &lines);
			ago(items[i].time, when, sizeof when);
			f32 rh = 30 + 18 * (f32)lines;
			Rect row = rect(c.x, y, c.w, rh);
			y += rh + 8;
			ui_push_id(ui, i);
			UiInteract in = ui_interact(ui, ui_id(ui, "clip"), row);
			Rect del = rect(row.x + row.w - 34, row.y + 6, 26, 26);
			UiInteract dx = ui_interact(ui, ui_id(ui, "del"), del);
			ui_pop_id(ui);
			if (in.hovered)
				cursor = i;
			bool sel = i == cursor;
			ui_round(ui, row, 10, sel ? color_with_alpha(t->accent, 0x26) : (t->dark ? color_hex("#ffffff0a") : color_hex("#0000000a")));
			if (sel)
				ui_border(ui, row, 10, 1, color_with_alpha(t->accent, 0x90));
			bool files = str_eq(items[i].mime, CLIPBOARD_FILES);
			ui_push_clip(ui, rect(row.x + 12, row.y, row.w - 52, row.h));
			ui_text_wrapped(ui, ui_font(ui, 13, files ? 600 : 400), rect(row.x + 12, row.y + 10, row.w - 52, 18 * (f32)lines + 2), text, t->text,
					ALIGN_START);
			ui_pop_clip(ui);
			ui_text(ui, ui_font(ui, 11, 500), row.x + 12, row.y + row.h - 9, when, t->text_faint);
			if (in.hovered || sel) {
				if (dx.hovered)
					ui_round(ui, del, 6, t->hover);
				ui_icon(ui, pad(del, 6), "close", t->text_dim);
			}
			if (dx.clicked) {
				drop_item(i);
				break;
			}
			if (in.clicked && !dx.hovered) {
				choose(i);
				break;
			}
		}
		ui_scroll_end(ui, y - c.y);
	}
	/* keys: arrows, Enter, Delete, Esc */
	if (ui_key_pressed(ui, KEY_DOWN, 0))
		cursor = MIN(cursor + 1, nitems - 1);
	if (ui_key_pressed(ui, KEY_UP, 0))
		cursor = MAX(cursor - 1, 0);
	if (ui_key_pressed(ui, KEY_ENTER, 0) || ui_key_pressed(ui, KEY_KPENTER, 0))
		choose(cursor);
	if (ui_key_pressed(ui, KEY_DELETE, 0) && nitems)
		drop_item(cursor);
	bool fresh = opened_frame == 0;
	if (fresh)
		opened_frame = ui->frame_no;
	if (open_ && (ui_key_pressed(ui, KEY_ESC, 0) ||
		      (ui->frame_no != opened_frame && (ui->pressed[BUTTON_LEFT] || ui->pressed[BUTTON_RIGHT]) &&
		       !rect_contains(rect_inset(r, -2, -2), ui->mx, ui->my)))) {
		open_ = false;
		shell_mark_dirty();
	}
}
