/*
 * Notes — quick notes that save themselves.
 *
 * Each note is a plain text file in ~/.local/share/zurbax/notes; the first
 * line is its title. Notes can be pinned to the top and searched.
 */
#include "zui.h"

typedef struct {
	char id[24];         /* file name without .txt */
	StrBuf text;
	i64 modified;
	bool pinned;
	bool dirty;
} Note;

typedef struct {
	Note *notes;
	int n, cap;
	int current;         /* index, -1 none */
	StrBuf search;
	f64 last_edit;
} Notes;

static Notes nt;

static char *notes_dir(void)
{
	return str_fmt("%s/.local/share/zurbax/notes", home_dir());
}

static char *note_path(const Note *n)
{
	char *dir = notes_dir();
	char *p = str_fmt("%s/%s.txt", dir, n->id);
	mem_free(dir);
	return p;
}

static void save_pins(void)
{
	char *dir = notes_dir();
	char *path = str_fmt("%s/pinned", dir);
	StrBuf sb = { 0 };
	for (int i = 0; i < nt.n; i++)
		if (nt.notes[i].pinned)
			sb_fmt(&sb, "%s\n", nt.notes[i].id);
	file_write_atomic(path, sb.data ? sb.data : "", sb.len);
	sb_free(&sb);
	mem_free(path);
	mem_free(dir);
}

static void save_note(Note *n)
{
	if (!n->dirty)
		return;
	char *path = note_path(n);
	if (n->text.len == 0)
		sys_unlink(path); /* empty notes vanish */
	else
		file_write_atomic(path, n->text.data, n->text.len);
	mem_free(path);
	n->dirty = false;
}

static int cmp_notes(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	const Note *x = a, *y = b;
	if (x->pinned != y->pinned)
		return x->pinned ? -1 : 1;
	return x->modified > y->modified ? -1 : x->modified < y->modified ? 1 : 0;
}

static void sort_notes(void)
{
	char cur[24] = { 0 };
	if (nt.current >= 0)
		str_copy(cur, nt.notes[nt.current].id, sizeof cur);
	sort(nt.notes, (usize)nt.n, sizeof(Note), cmp_notes, NULL);
	nt.current = -1;
	for (int i = 0; i < nt.n; i++)
		if (str_eq(nt.notes[i].id, cur))
			nt.current = i;
}

static Note *add_note(void)
{
	if (nt.n == nt.cap) {
		nt.cap = nt.cap ? nt.cap * 2 : 32;
		nt.notes = mem_realloc(nt.notes, sizeof(Note) * (usize)nt.cap);
	}
	Note *n = &nt.notes[nt.n++];
	memset(n, 0, sizeof *n);
	return n;
}

static void load_notes(void)
{
	char *dir = notes_dir();
	dir_make(dir, 0755);
	char *pin_path = str_fmt("%s/pinned", dir);
	char *pins = file_read(pin_path, NULL);
	mem_free(pin_path);
	DirList list = { 0 };
	if (dir_list(dir, &list, true)) {
		vec_each(&list, e) {
			if (!str_ends(e->name, ".txt"))
				continue;
			Note *n = add_note();
			usize len = strlen(e->name) - 4;
			memcpy(n->id, e->name, MIN(len, sizeof n->id - 1));
			char *path = str_fmt("%s/%s", dir, e->name);
			char *text = file_read(path, NULL);
			mem_free(path);
			if (text) {
				sb_adds(&n->text, text);
				mem_free(text);
			}
			n->modified = e->mtime;
			n->pinned = pins && str_find(pins, n->id);
		}
		dir_list_free(&list);
	}
	mem_free(pins);
	mem_free(dir);
	sort_notes();
}

static void new_note(Ui *ui)
{
	if (nt.current >= 0)
		save_note(&nt.notes[nt.current]);
	/* Reuse an untouched empty note instead of piling them up. */
	for (int i = 0; i < nt.n; i++)
		if (nt.notes[i].text.len == 0) {
			nt.current = i;
			ui_focus(ui, "editor");
			return;
		}
	Note *n = add_note();
	fmt(n->id, sizeof n->id, "%lld", (long long)time_unix_ms());
	n->modified = time_unix();
	nt.current = nt.n - 1;
	sort_notes();
	ui_focus(ui, "editor");
}

static void delete_current(void)
{
	if (nt.current < 0)
		return;
	Note *n = &nt.notes[nt.current];
	char *path = note_path(n);
	sys_unlink(path);
	mem_free(path);
	sb_free(&n->text);
	memmove(n, n + 1, sizeof(Note) * (usize)(nt.n - nt.current - 1));
	nt.n--;
	nt.current = nt.n ? MIN(nt.current, nt.n - 1) : -1;
	save_pins();
}

/* First non-empty line, and the text after it. */
static void note_title(const Note *n, char *title, usize tcap, char *preview, usize pcap)
{
	const char *s = sb_str(&n->text);
	while (*s == '\n' || *s == ' ')
		s++;
	const char *eol = str_chr(s, '\n');
	usize len = eol ? (usize)(eol - s) : strlen(s);
	if (len == 0)
		str_copy(title, "New Note", tcap);
	else {
		len = MIN(len, tcap - 1);
		memcpy(title, s, len);
		title[len] = 0;
	}
	usize pn = 0;
	if (eol) {
		const char *p = eol + 1;
		while (*p && pn < pcap - 1) {
			char ch = *p++ == '\n' ? ' ' : p[-1];
			if (ch == ' ' && (pn == 0 || preview[pn - 1] == ' '))
				continue;
			preview[pn++] = ch;
		}
	}
	preview[pn] = 0;
}

static void format_when(i64 t, char *out, usize cap, bool long_form)
{
	Civil c, now;
	civil_local(t, &c);
	civil_local(time_unix(), &now);
	static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
					      "September", "October", "November", "December" };
	if (long_form)
		fmt(out, cap, "%d %s %d at %02d:%02d", c.day, months[c.month - 1], c.year, c.hour, c.min);
	else if (c.year == now.year && c.yday == now.yday)
		fmt(out, cap, "%02d:%02d", c.hour, c.min);
	else if (c.year == now.year)
		fmt(out, cap, "%d %s", c.day, mon[c.month - 1]);
	else
		fmt(out, cap, "%d/%d/%d", c.day, c.month, c.year % 100);
}

static const char *group_of(const Note *n)
{
	if (n->pinned)
		return "Pinned";
	i64 age = time_unix() - n->modified;
	Civil c, now;
	civil_local(n->modified, &c);
	civil_local(time_unix(), &now);
	if (c.year == now.year && c.yday == now.yday)
		return "Today";
	if (age < 7 * 86400)
		return "Previous 7 Days";
	if (age < 30 * 86400)
		return "Previous 30 Days";
	return "Earlier";
}

static void sidebar(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->sidebar);
	ui_vseparator(ui, r.x + r.w - 1, r.y, r.y + r.h);
	Rect s = pad4(r, 10, 10, 0, 10);
	ui_entry(ui, cut_top(&s, 32), "search", &nt.search, "Search notes", ENTRY_SEARCH);
	cut_top(&s, 8);
	const char *q = sb_str(&nt.search);
	f32 y = ui_scroll_begin(ui, s, "list");
	const char *last_group = NULL;
	int shown = 0;
	for (int i = 0; i < nt.n; i++) {
		Note *n = &nt.notes[i];
		if (*q && !str_find_nocase(sb_str(&n->text), q))
			continue;
		if (n->text.len == 0 && i != nt.current)
			continue;
		const char *g = group_of(n);
		if (!last_group || !str_eq(g, last_group)) {
			ui_sidebar_heading(ui, rect(s.x, y, s.w, 28), g);
			y += 28;
			last_group = g;
		}
		Rect row = rect(s.x, y, s.w, 62);
		y += 64;
		shown++;
		ui_push_id_str(ui, n->id);
		UiInteract it = ui_row(ui, row, ui_id(ui, "row"), i == nt.current);
		ui_pop_id(ui);
		char title[80], preview[120], when[32];
		note_title(n, title, sizeof title, preview, sizeof preview);
		format_when(n->modified, when, sizeof when, false);
		Rect in = pad2(row, 10, 8);
		ui_push_clip(ui, in);
		ui_text(ui, ui_font(ui, 14, 600), in.x, in.y + 15, title, t->text);
		f32 x = ui_text(ui, ui_font(ui, 12, 500), in.x, in.y + 36, when, t->text);
		ui_text(ui, ui_font(ui, 12, 400), x + 8, in.y + 36, preview[0] ? preview : "No additional text", t->text_dim);
		ui_pop_clip(ui);
		if (n->pinned)
			ui_icon(ui, rect(in.x + in.w - 14, in.y + 3, 14, 14), "pin", t->text_faint);
		if (it.clicked && i != nt.current) {
			if (nt.current >= 0)
				save_note(&nt.notes[nt.current]);
			nt.current = i;
		}
	}
	ui_scroll_end(ui, y - s.y);
	if (shown == 0 && *q)
		ui_placeholder(ui, s, "search", "No Results", "No notes contain that text");
}

static void editor(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->view);
	if (nt.current < 0) {
		ui_placeholder(ui, r, "notes", "No Note Selected", "Create a note with the button in the header");
		return;
	}
	Note *n = &nt.notes[nt.current];
	char when[64];
	format_when(n->modified, when, sizeof when, true);
	Rect top = cut_top(&r, 34);
	ui_text_in(ui, ui_font(ui, 12, 400), top, when, t->text_faint, ALIGN_CENTER);
	Rect area = pad4(r, 4, 40, 20, 40);
	int ev = ui_textarea(ui, area, "editor", &n->text, 15, TEXT_NO_FRAME);
	if (ev & ENTRY_CHANGED) {
		n->dirty = true;
		n->modified = time_unix();
		nt.last_edit = ui->now;
	}
	/* Save a moment after typing stops. */
	if (n->dirty) {
		if (ui->now - nt.last_edit > 1.0) {
			save_note(n);
			sort_notes();
		} else if (ui->wake_at == 0 || ui->wake_at > nt.last_edit + 1.05) {
			ui->wake_at = nt.last_edit + 1.05;
		}
	}
	/* Word and character count. */
	int words = 0, chars = 0;
	bool in_word = false;
	for (usize i = 0; i < n->text.len; i++) {
		u8 ch = (u8)n->text.data[i];
		if ((ch & 0xC0) != 0x80)
			chars++;
		bool space = ch == ' ' || ch == '\n' || ch == '\t';
		if (!space && !in_word)
			words++;
		in_word = !space;
	}
	char count[48];
	fmt(count, sizeof count, "%d words · %d characters", words, chars);
	Font cf = ui_font(ui, 11, 500);
	ui_text(ui, cf, r.x + r.w - 16 - ui_text_width(ui, cf, count), r.y + r.h - 8, count, t->text_faint);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	Rect tools = ui_headerbar(ui, header, "Notes", HEADER_NO_TITLE);
	Rect side = cut_left(&win, 290);
	cut_left(&tools, 290 - 50);
	if (ui_icon_button(ui, cut_left(&tools, 34), "edit", "New Note (Ctrl+N)") || ui_key_pressed(ui, KEY_N, MOD_CTRL))
		new_note(ui);
	if (nt.current >= 0) {
		Note *n = &nt.notes[nt.current];
		if (ui_icon_button(ui, cut_right(&tools, 34), "trash", "Delete Note"))
			delete_current();
		else if (ui_icon_toggle(ui, cut_right(&tools, 34), "pin", n->pinned, n->pinned ? "Unpin" : "Pin")) {
			n->pinned = !n->pinned;
			save_pins();
			sort_notes();
		}
	}
	if (ui_key_pressed(ui, KEY_F, MOD_CTRL))
		ui_focus(ui, "search");
	sidebar(ui, side);
	editor(ui, win);
}

static bool on_close(Ui *ui)
{
	UNUSED(ui);
	for (int i = 0; i < nt.n; i++)
		save_note(&nt.notes[i]);
	return true;
}

static void init(Ui *ui)
{
	load_notes();
	if (nt.n == 0) {
		/* A first note that explains itself. */
		Note *n = add_note();
		fmt(n->id, sizeof n->id, "%lld", (long long)time_unix_ms());
		sb_adds(&n->text, "Welcome to Notes\n\nNotes save themselves as you type. The first line becomes the title.\n\n"
				  "Pin important notes to keep them at the top, and use the search box to find anything.");
		n->modified = time_unix();
		n->dirty = true;
		save_note(n);
	}
	nt.current = 0;
	ui_focus(ui, "editor");
}

int main(void)
{
	nt.current = -1;
	UiAppDesc d = { .id = "notes", .title = "Notes", .width = 900, .height = 600, .min_width = 640, .min_height = 400,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame, .close = on_close };
	return ui_run(&d);
}
