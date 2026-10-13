/*
 * Writer — a text editor with tabs.
 *
 * Open, edit and save plain text files; find and replace; word wrap and a
 * monospaced mode for code; a status bar with the caret position and
 * counts. Unsaved changes are never lost silently.
 */
#include "zui.h"

typedef struct {
	char path[1024];      /* empty for a new, unsaved document */
	StrBuf text;
	bool dirty;
	char editor_id[24];
} Doc;

typedef struct {
	Doc docs[24];
	int ndocs, current;
	int untitled_counter;
	bool wrap, mono;
	f32 font_size;
	bool find_open, replace_open;
	StrBuf find, replace;
	int pending_close;    /* tab waiting for the save prompt, -1 none */
	bool quit_after;      /* closing the window */
	char message[160];
	f64 message_until;
} Writer;

static Writer wr;

static Doc *cur(void)
{
	return wr.ndocs ? &wr.docs[wr.current] : NULL;
}

static void doc_name(const Doc *d, char *out, usize cap)
{
	if (d->path[0])
		str_copy(out, path_base(d->path), cap);
	else
		fmt(out, cap, "Untitled");
}

static void flash(Ui *ui, const char *msg)
{
	str_copy(wr.message, msg, sizeof wr.message);
	wr.message_until = ui->now + 3;
	ui->wake_at = wr.message_until;
}

static Doc *new_doc(void)
{
	if (wr.ndocs >= (int)ARRAY_LEN(wr.docs))
		return NULL;
	Doc *d = &wr.docs[wr.ndocs];
	memset(d, 0, sizeof *d);
	fmt(d->editor_id, sizeof d->editor_id, "editor-%d", ++wr.untitled_counter);
	wr.current = wr.ndocs++;
	return d;
}

static bool open_file(Ui *ui, const char *path)
{
	/* Already open? Switch to it. */
	for (int i = 0; i < wr.ndocs; i++)
		if (str_eq(wr.docs[i].path, path)) {
			wr.current = i;
			return true;
		}
	usize len;
	char *data = file_read(path, &len);
	if (!data) {
		flash(ui, "The file could not be opened.");
		return false;
	}
	/* Replace a pristine empty tab rather than adding another. */
	Doc *d = cur();
	if (!(d && !d->path[0] && d->text.len == 0 && !d->dirty))
		d = new_doc();
	if (!d) {
		mem_free(data);
		return false;
	}
	str_copy(d->path, path, sizeof d->path);
	sb_clear(&d->text);
	sb_add(&d->text, data, len);
	d->dirty = false;
	mem_free(data);
	ui_focus(ui, d->editor_id);
	ui_textarea_select(ui, d->editor_id, 0, 0);
	return true;
}

static bool save_doc(Ui *ui, Doc *d)
{
	if (!d->path[0])
		return false;
	if (!file_write_atomic(d->path, d->text.data ? d->text.data : "", d->text.len)) {
		flash(ui, "Saving failed. Check that you can write to this folder.");
		return false;
	}
	d->dirty = false;
	char msg[200];
	fmt(msg, sizeof msg, "Saved %s", path_base(d->path));
	flash(ui, msg);
	return true;
}

static void close_doc(int i)
{
	sb_free(&wr.docs[i].text);
	memmove(&wr.docs[i], &wr.docs[i + 1], sizeof(Doc) * (usize)(wr.ndocs - i - 1));
	wr.ndocs--;
	if (wr.current >= wr.ndocs)
		wr.current = wr.ndocs - 1;
	if (wr.ndocs == 0) {
		new_doc();
	}
}

static void request_close(Ui *ui, int i)
{
	if (wr.docs[i].dirty) {
		wr.pending_close = i;
		wr.current = i;
		ui_dialog_open(ui, "save-changes");
	} else {
		close_doc(i);
	}
}

/* ---- Find and replace ------------------------------------------------------------------- */

static isize find_from(const Doc *d, usize from, bool forward)
{
	const char *q = sb_str(&wr.find);
	usize ql = wr.find.len;
	if (!ql || d->text.len < ql)
		return -1;
	const char *s = sb_str(&d->text);
	usize n = d->text.len;
	if (forward) {
		for (usize pass = 0; pass < 2; pass++) {
			usize start = pass ? 0 : from, end = pass ? from : n;
			for (usize i = start; i + ql <= n && i < end + (pass ? 0 : ql); i++)
				if (str_eq_nocase_n(s + i, q, ql))
					return (isize)i;
		}
	} else {
		for (usize pass = 0; pass < 2; pass++) {
			isize start = pass ? (isize)(n - ql) : (isize)from - 1;
			isize stop = pass ? (isize)from : 0;
			for (isize i = start; i >= stop; i--)
				if ((usize)i + ql <= n && str_eq_nocase_n(s + i, q, ql))
					return i;
		}
	}
	return -1;
}

static int count_matches(const Doc *d)
{
	usize ql = wr.find.len;
	if (!ql)
		return 0;
	int c = 0;
	const char *s = sb_str(&d->text);
	for (usize i = 0; i + ql <= d->text.len; i++)
		if (str_eq_nocase_n(s + i, sb_str(&wr.find), ql)) {
			c++;
			i += ql - 1;
		}
	return c;
}

static void find_next(Ui *ui, bool forward)
{
	Doc *d = cur();
	if (!d)
		return;
	usize caret = ui_textarea_cursor(ui, d->editor_id);
	isize at = find_from(d, forward ? caret : (caret >= wr.find.len ? caret - wr.find.len : 0), forward);
	if (at < 0) {
		flash(ui, "No matches");
		return;
	}
	ui_textarea_select(ui, d->editor_id, (usize)at, (usize)at + wr.find.len);
}

static void replace_one(Ui *ui)
{
	Doc *d = cur();
	if (!d || !wr.find.len)
		return;
	usize caret = ui_textarea_cursor(ui, d->editor_id);
	/* If the selection is a match, replace it; then move to the next one. */
	if (caret >= wr.find.len && str_eq_nocase_n(sb_str(&d->text) + caret - wr.find.len, sb_str(&wr.find), wr.find.len)) {
		usize at = caret - wr.find.len;
		sb_remove(&d->text, at, wr.find.len);
		sb_insert(&d->text, at, sb_str(&wr.replace), wr.replace.len);
		d->dirty = true;
		ui_textarea_select(ui, d->editor_id, at + wr.replace.len, at + wr.replace.len);
	}
	find_next(ui, true);
}

static void replace_all(Ui *ui)
{
	Doc *d = cur();
	if (!d || !wr.find.len)
		return;
	StrBuf out = { 0 };
	const char *s = sb_str(&d->text);
	int n = 0;
	for (usize i = 0; i < d->text.len;) {
		if (i + wr.find.len <= d->text.len && str_eq_nocase_n(s + i, sb_str(&wr.find), wr.find.len)) {
			sb_add(&out, sb_str(&wr.replace), wr.replace.len);
			i += wr.find.len;
			n++;
		} else {
			sb_addc(&out, s[i++]);
		}
	}
	if (n) {
		sb_free(&d->text);
		d->text = out;
		d->dirty = true;
		ui_textarea_select(ui, d->editor_id, 0, 0);
	} else {
		sb_free(&out);
	}
	char msg[64];
	fmt(msg, sizeof msg, n == 1 ? "Replaced 1 match" : "Replaced %d matches", n);
	flash(ui, msg);
}

/* ---- Interface ------------------------------------------------------------------------- */

static void tab_bar(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->header_bottom);
	ui_separator(ui, r.x, r.y + r.h - 1, r.x + r.w);
	f32 tw = minf(220, (r.w - 40) / (f32)MAX(1, wr.ndocs));
	f32 x = r.x + 6;
	int close = -1;
	for (int i = 0; i < wr.ndocs; i++) {
		Doc *d = &wr.docs[i];
		Rect tab = rect(x, r.y + 4, tw - 4, r.h - 4);
		x += tw;
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "tab"), tab);
		bool active = i == wr.current;
		if (active) {
			ui_round4(ui, tab, 6, 6, 0, 0, t->view);
			ui_border(ui, rect(tab.x, tab.y, tab.w, tab.h + 6), 6, 1, t->border);
			ui_fill(ui, rect(tab.x + 1, tab.y + tab.h - 1, tab.w - 2, 2), t->view);
		} else if (it.hovered) {
			ui_round4(ui, tab, 6, 6, 0, 0, t->hover);
		}
		char name[128];
		doc_name(d, name, sizeof name);
		Rect cb = rect(tab.x + tab.w - 26, tab.y + (tab.h - 20) / 2, 20, 20);
		ui_push_clip(ui, rect(tab.x + 10, tab.y, tab.w - 40, tab.h));
		ui_text(ui, ui_font(ui, 13, active ? 600 : 500), tab.x + 12, tab.y + tab.h / 2 + 5, name, active ? t->text : t->text_dim);
		ui_pop_clip(ui);
		UiInteract ci = ui_interact(ui, ui_id(ui, "close"), cb);
		if (d->dirty && !ci.hovered)
			ui_circle(ui, cb.x + 10, cb.y + 10, 4, t->text_dim);
		else if (ci.hovered || active || it.hovered) {
			if (ci.hovered)
				ui_round(ui, cb, 4, t->hover);
			ui_icon(ui, pad(cb, 4), "close", t->text_dim);
		}
		if (ci.clicked)
			close = i;
		else if (it.clicked)
			wr.current = i;
		if (it.right_clicked || (ui->pressed[BUTTON_MIDDLE] && it.hovered))
			close = i;
		ui_pop_id(ui);
	}
	Rect add = rect(x + 2, r.y + 6, 28, r.h - 10);
	if (ui_icon_button(ui, add, "add", "New Tab (Ctrl+T)")) {
		new_doc();
		ui_focus(ui, cur()->editor_id);
	}
	if (close >= 0)
		request_close(ui, close);
}

static void find_bar(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->header_bottom);
	ui_separator(ui, r.x, r.y, r.x + r.w);
	Rect in = pad2(r, 10, 7);
	Rect row = cut_top(&in, 30);
	int ev = ui_entry(ui, cut_left(&row, 260), "find", &wr.find, "Find", ENTRY_SEARCH);
	if (ev & ENTRY_SUBMIT)
		find_next(ui, !(ui->mods & MOD_SHIFT));
	if (ev & ENTRY_ESCAPE)
		wr.find_open = false;
	cut_left(&row, 6);
	if (ui_icon_button(ui, cut_left(&row, 30), "go-up", "Previous Match (Shift+Enter)"))
		find_next(ui, false);
	if (ui_icon_button(ui, cut_left(&row, 30), "go-down", "Next Match (Enter)"))
		find_next(ui, true);
	cut_left(&row, 8);
	if (wr.find.len) {
		char cnt[32];
		int n = count_matches(cur());
		fmt(cnt, sizeof cnt, n == 1 ? "1 match" : "%d matches", n);
		ui_label_dim(ui, cut_left(&row, 90), cnt);
	}
	if (ui_icon_button(ui, cut_right(&row, 30), "close", "Close"))
		wr.find_open = false;
	if (ui_button_flat(ui, cut_right(&row, 90), wr.replace_open ? "Hide Replace" : "Replace"))
		wr.replace_open = !wr.replace_open;
	if (wr.replace_open) {
		cut_top(&in, 6);
		Rect r2 = cut_top(&in, 30);
		int ev2 = ui_entry(ui, cut_left(&r2, 260), "replace", &wr.replace, "Replace with", 0);
		cut_left(&r2, 6);
		if (ui_button(ui, cut_left(&r2, 90), "Replace") || (ev2 & ENTRY_SUBMIT))
			replace_one(ui);
		cut_left(&r2, 6);
		if (ui_button(ui, cut_left(&r2, 100), "Replace All"))
			replace_all(ui);
	}
}

static void status_bar(Ui *ui, Rect r, Doc *d)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->window);
	ui_separator(ui, r.x, r.y, r.x + r.w);
	usize caret = ui_textarea_cursor(ui, d->editor_id);
	int line = 1, col = 1, words = 0;
	const char *s = sb_str(&d->text);
	bool in_word = false;
	for (usize i = 0; i < d->text.len; i++) {
		u8 c = (u8)s[i];
		if (i < caret) {
			if (c == '\n')
				line++, col = 1;
			else if ((c & 0xC0) != 0x80)
				col++;
		}
		bool sp = c == ' ' || c == '\n' || c == '\t';
		if (!sp && !in_word)
			words++;
		in_word = !sp;
	}
	char left[96];
	fmt(left, sizeof left, "Line %d, Column %d", line, col);
	Font f = ui_font(ui, 12, 500);
	ui_text(ui, f, r.x + 14, r.y + 17, left, t->text_dim);
	char right[96];
	fmt(right, sizeof right, "%d words  ·  UTF-8", words);
	if (ui->now < wr.message_until)
		str_copy(right, wr.message, sizeof right);
	ui_text(ui, f, r.x + r.w - 14 - ui_text_width(ui, f, right), r.y + 17, right, ui->now < wr.message_until ? t->accent : t->text_dim);
}

static void menu(Ui *ui, Rect anchor)
{
	Rect c;
	if (!ui_popover_begin(ui, "menu", anchor, 240, 300, &c))
		return;
	Doc *d = cur();
	if (ui_menu_item(ui, cut_top(&c, 34), "document-save", "Save As…", "Shift+Ctrl+S")) {
		ui_popover_close(ui);
		char dir[1024];
		char *pd = d->path[0] ? path_dir(d->path) : str_fmt("%s/Documents", home_dir());
		str_copy(dir, pd, sizeof dir);
		mem_free(pd);
		char name[128];
		doc_name(d, name, sizeof name);
		ui_file_dialog_open(ui, "save", FILE_SAVE, dir, NULL, d->path[0] ? name : "Untitled.txt");
	}
	if (ui_menu_item(ui, cut_top(&c, 34), "search", "Find and Replace", "Ctrl+H")) {
		wr.find_open = wr.replace_open = true;
		ui_focus(ui, "find");
		ui_popover_close(ui);
	}
	ui_separator(ui, c.x, c.y + 4, c.x + c.w);
	cut_top(&c, 9);
	Rect row = cut_top(&c, 34);
	ui_label(ui, pad2(cut_left(&row, row.w - 56), 10, 0), "Word Wrap");
	ui_switch(ui, center_in(row, 46, 24), &wr.wrap);
	row = cut_top(&c, 34);
	ui_label(ui, pad2(cut_left(&row, row.w - 56), 10, 0), "Monospace");
	ui_switch(ui, center_in(row, 46, 24), &wr.mono);
	row = cut_top(&c, 38);
	ui_label(ui, pad2(cut_left(&row, row.w - 110), 10, 0), "Text Size");
	if (ui_icon_button(ui, center_in(cut_left(&row, 34), 30, 30), "zoom-out", "Smaller"))
		wr.font_size = maxf(9, wr.font_size - 1);
	char sz[8];
	fmt(sz, sizeof sz, "%d", (int)wr.font_size);
	ui_text_in(ui, ui_font(ui, 12, 600), cut_left(&row, 30), sz, ui->theme.text, ALIGN_CENTER);
	if (ui_icon_button(ui, center_in(cut_left(&row, 34), 30, 30), "zoom-in", "Larger"))
		wr.font_size = minf(32, wr.font_size + 1);
	ui_popover_end(ui);
}

static void save_changes_dialog(Ui *ui)
{
	Rect c;
	if (!ui_dialog_begin(ui, "save-changes", 420, 190, &c))
		return;
	Doc *d = &wr.docs[wr.pending_close >= 0 ? wr.pending_close : wr.current];
	char name[128], title[200];
	doc_name(d, name, sizeof name);
	fmt(title, sizeof title, "Save changes to “%s”?", name);
	Rect top = cut_top(&c, 60);
	ui_icon(ui, rect(top.x, top.y + 4, 40, 40), "document-save", ui->theme.warning);
	Rect tx = pad4(top, 0, 0, 0, 54);
	ui_heading(ui, cut_top(&tx, 26), title);
	ui_label_dim(ui, tx, "Your changes will be lost if you don't save them.");
	Rect b = cut_bottom(&c, 34);
	int idx = wr.pending_close >= 0 ? wr.pending_close : wr.current;
	if (ui_button_suggested(ui, cut_right(&b, 100), "Save")) {
		ui_dialog_close(ui);
		if (d->path[0]) {
			if (save_doc(ui, d)) {
				close_doc(idx);
				if (wr.quit_after)
					ui->quit = true;
			}
		} else {
			char *docs = str_fmt("%s/Documents", home_dir());
			ui_file_dialog_open(ui, "save", FILE_SAVE, docs, NULL, "Untitled.txt");
			mem_free(docs);
		}
	}
	cut_right(&b, 8);
	if (ui_button(ui, cut_right(&b, 90), "Cancel")) {
		wr.pending_close = -1;
		wr.quit_after = false;
		ui_dialog_close(ui);
	}
	if (ui_button_destructive(ui, cut_left(&b, 130), "Don't Save")) {
		ui_dialog_close(ui);
		close_doc(idx);
		wr.pending_close = -1;
		if (wr.quit_after) {
			/* Ask about the next unsaved document, if any. */
			int next = -1;
			for (int i = 0; i < wr.ndocs; i++)
				if (wr.docs[i].dirty)
					next = i;
			if (next >= 0)
				request_close(ui, next);
			else
				ui->quit = true;
		}
	}
	ui_dialog_end(ui);
}

static void shortcuts(Ui *ui)
{
	if (ui_key_pressed(ui, KEY_S, MOD_CTRL)) {
		Doc *d = cur();
		if (d->path[0])
			save_doc(ui, d);
		else {
			char *docs = str_fmt("%s/Documents", home_dir());
			ui_file_dialog_open(ui, "save", FILE_SAVE, docs, NULL, "Untitled.txt");
			mem_free(docs);
		}
	}
	if (ui_key_pressed(ui, KEY_S, MOD_CTRL | MOD_SHIFT)) {
		char *docs = str_fmt("%s/Documents", home_dir());
		ui_file_dialog_open(ui, "save", FILE_SAVE, docs, NULL, "Untitled.txt");
		mem_free(docs);
	}
	if (ui_key_pressed(ui, KEY_O, MOD_CTRL))
		ui_file_dialog_open(ui, "open", FILE_OPEN, NULL, NULL, NULL);
	if (ui_key_pressed(ui, KEY_T, MOD_CTRL) || ui_key_pressed(ui, KEY_N, MOD_CTRL)) {
		new_doc();
		ui_focus(ui, cur()->editor_id);
	}
	if (ui_key_pressed(ui, KEY_W, MOD_CTRL))
		request_close(ui, wr.current);
	if (ui_key_pressed(ui, KEY_F, MOD_CTRL)) {
		wr.find_open = true;
		ui_focus(ui, "find");
	}
	if (ui_key_pressed(ui, KEY_H, MOD_CTRL)) {
		wr.find_open = wr.replace_open = true;
		ui_focus(ui, "find");
	}
	if (ui_key_pressed(ui, KEY_EQUAL, MOD_CTRL))
		wr.font_size = minf(32, wr.font_size + 1);
	if (ui_key_pressed(ui, KEY_MINUS, MOD_CTRL))
		wr.font_size = maxf(9, wr.font_size - 1);
	if (ui_key_pressed(ui, KEY_TAB, MOD_CTRL) && wr.ndocs > 1)
		wr.current = (wr.current + 1) % wr.ndocs;
	if (ui_key_pressed(ui, KEY_F3, 0))
		find_next(ui, true);
}

/* Text files dragged onto the window open in tabs. */
static void dropped(Ui *ui)
{
	if (!ui_drop_zone(ui, rect(0, 46, ui->width, ui->height - 46), "text-file", "Drop to open"))
		return;
	usize n;
	char **lines = str_split(ui->drop_paths, '\n', &n);
	for (usize i = 0; i < n && i < 12; i++) {
		const char *p = str_trim(lines[i]);
		if (*p == '/' && !dir_exists(p))
			open_file(ui, p);
	}
	str_split_free(lines, n);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	shortcuts(ui);
	Doc *d = cur();
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	char name[128], title[160];
	doc_name(d, name, sizeof name);
	fmt(title, sizeof title, "%s%s", d->dirty ? "• " : "", name);
	Rect tools = ui_headerbar(ui, header, title, 0);
	if (ui_button_icon_label(ui, center_in(cut_left(&tools, 90), 86, 30), "document-open", "Open"))
		ui_file_dialog_open(ui, "open", FILE_OPEN, NULL, NULL, NULL);
	cut_left(&tools, 4);
	if (ui_icon_button(ui, cut_left(&tools, 34), "document-save", "Save (Ctrl+S)")) {
		if (d->path[0])
			save_doc(ui, d);
		else {
			char *docs = str_fmt("%s/Documents", home_dir());
			ui_file_dialog_open(ui, "save", FILE_SAVE, docs, NULL, "Untitled.txt");
			mem_free(docs);
		}
	}
	Rect menu_btn = cut_right(&tools, 34);
	if (ui_icon_button(ui, menu_btn, "menu", "Menu"))
		ui_popover_toggle(ui, "menu");
	if (ui_icon_toggle(ui, cut_right(&tools, 34), "search", wr.find_open, "Find (Ctrl+F)")) {
		wr.find_open = !wr.find_open;
		if (wr.find_open)
			ui_focus(ui, "find");
	}
	if (d->path[0]) {
		char *dir = path_dir(d->path);
		const char *home = home_dir();
		char shown[512];
		if (str_starts(dir, home))
			fmt(shown, sizeof shown, "~%s", dir + strlen(home));
		else
			str_copy(shown, dir, sizeof shown);
		mem_free(dir);
		ui_text_in(ui, ui_font(ui, 11, 400), rect(header.x, header.y + 30, header.w, 14), shown, t->text_faint, ALIGN_CENTER);
	}

	if (wr.ndocs > 1)
		tab_bar(ui, cut_top(&win, 36));
	Rect status = cut_bottom(&win, 26);
	if (wr.find_open)
		find_bar(ui, cut_bottom(&win, wr.replace_open ? 86 : 44));
	ui_fill(ui, win, t->view);
	u32 flags = TEXT_NO_FRAME | (wr.wrap ? 0 : TEXT_NO_WRAP) | (wr.mono ? TEXT_MONO : 0);
	int ev = ui_textarea(ui, pad4(win, 10, 6, 6, 18), d->editor_id, &d->text, wr.font_size, flags);
	if (ev & ENTRY_CHANGED)
		d->dirty = true;
	status_bar(ui, status, d);
	menu(ui, menu_btn);
	save_changes_dialog(ui);

	char path[1024];
	if (ui_file_dialog(ui, "open", path, sizeof path))
		open_file(ui, path);
	if (ui_file_dialog(ui, "save", path, sizeof path)) {
		Doc *target = wr.pending_close >= 0 ? &wr.docs[wr.pending_close] : cur();
		str_copy(target->path, path, sizeof target->path);
		if (save_doc(ui, target) && wr.pending_close >= 0) {
			close_doc(wr.pending_close);
			wr.pending_close = -1;
			if (wr.quit_after)
				ui->quit = true;
		}
	}
	char wt[200];
	fmt(wt, sizeof wt, "%s — Writer", name);
	ui_set_title(ui, wt);
	dropped(ui);
}

static bool on_close(Ui *ui)
{
	for (int i = 0; i < wr.ndocs; i++)
		if (wr.docs[i].dirty) {
			wr.quit_after = true;
			request_close(ui, i);
			return false;
		}
	return true;
}

static char **start_files;
static int nstart_files;

static void init(Ui *ui)
{
	wr.wrap = true;
	wr.font_size = 14;
	wr.pending_close = -1;
	new_doc();
	for (int i = 0; i < nstart_files; i++)
		open_file(ui, start_files[i]);
	ui_focus(ui, cur()->editor_id);
}

int main(int argc, char **argv)
{
	start_files = argv + 1;
	nstart_files = argc - 1;
	UiAppDesc d = { .id = "writer", .title = "Writer", .width = 860, .height = 620, .min_width = 480, .min_height = 320,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame, .close = on_close };
	return ui_run(&d);
}
