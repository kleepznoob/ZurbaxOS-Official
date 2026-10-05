/*
 * Calendar — a month view with an agenda for the selected day.
 *
 * Events can last all day or have times, repeat daily, weekly, monthly or
 * yearly (birthdays), and carry a colour, place and notes. They are saved
 * as JSON in ~/.local/share/zurbax/calendar.json.
 */
#include "zui.h"

typedef enum { REPEAT_NONE, REPEAT_DAILY, REPEAT_WEEKLY, REPEAT_MONTHLY, REPEAT_YEARLY } Repeat;

typedef struct {
	char title[96];
	int y, m, d;
	bool allday;
	int sh, sm, eh, em;  /* start and end time */
	char location[96];
	char notes[512];
	int color;
	Repeat repeat;
} Event;

typedef struct {
	Event *events;
	int nevents, cap;
	int view_y, view_m;  /* month shown */
	int sel_y, sel_m, sel_d;
	/* editor */
	int edit_index;      /* -1 new */
	Event edit;
	StrBuf e_title, e_location, e_notes;
	int e_repeat;
	StrBuf search;
} Cal;

static Cal cal;

static const u32 EVENT_COLORS[] = { 0x3689e6, 0x28bca3, 0x68b723, 0xf9c440, 0xf37329, 0xc6262e, 0xa56de2, 0x8a715e };
static const char *const MONTHS[] = { "January", "February", "March", "April", "May", "June", "July", "August",
				      "September", "October", "November", "December" };
static const char *const WEEKDAYS[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static Color event_color(int i)
{
	return 0xff000000u | EVENT_COLORS[(usize)i % ARRAY_LEN(EVENT_COLORS)];
}

static i64 day_number(int y, int m, int d)
{
	Civil c = { .year = y, .month = m, .day = d };
	return unix_from_civil(&c) / 86400;
}

static int weekday(int y, int m, int d)
{
	return (int)((day_number(y, m, d) % 7 + 11) % 7); /* 1970-01-01 was a Thursday */
}

static bool occurs_on(const Event *e, int y, int m, int d)
{
	i64 base = day_number(e->y, e->m, e->d), t = day_number(y, m, d);
	if (t < base)
		return false;
	switch (e->repeat) {
	case REPEAT_NONE: return t == base;
	case REPEAT_DAILY: return true;
	case REPEAT_WEEKLY: return (t - base) % 7 == 0;
	case REPEAT_MONTHLY: return d == e->d;
	case REPEAT_YEARLY: return m == e->m && d == e->d;
	}
	return false;
}

/* ---- Storage ------------------------------------------------------------------------- */

static char *data_path(void)
{
	return str_fmt("%s/.local/share/zurbax/calendar.json", home_dir());
}

static void save_events(void)
{
	StrBuf sb = { 0 };
	sb_adds(&sb, "[\n");
	static const char *const reps[] = { "none", "daily", "weekly", "monthly", "yearly" };
	for (int i = 0; i < cal.nevents; i++) {
		Event *e = &cal.events[i];
		sb_adds(&sb, "  {\"title\": ");
		sb_json_string(&sb, e->title);
		sb_fmt(&sb, ", \"date\": \"%04d-%02d-%02d\", \"allday\": %s, \"start\": \"%02d:%02d\", \"end\": \"%02d:%02d\", ", e->y,
		       e->m, e->d, e->allday ? "true" : "false", e->sh, e->sm, e->eh, e->em);
		sb_adds(&sb, "\"location\": ");
		sb_json_string(&sb, e->location);
		sb_adds(&sb, ", \"notes\": ");
		sb_json_string(&sb, e->notes);
		sb_fmt(&sb, ", \"color\": %d, \"repeat\": \"%s\"}%s\n", e->color, reps[e->repeat], i + 1 < cal.nevents ? "," : "");
	}
	sb_adds(&sb, "]\n");
	char *path = data_path();
	char *dir = path_dir(path);
	dir_make(dir, 0755);
	file_write_atomic(path, sb.data, sb.len);
	mem_free(dir);
	mem_free(path);
	sb_free(&sb);
}

static void add_event(const Event *e)
{
	if (cal.nevents == cal.cap) {
		cal.cap = cal.cap ? cal.cap * 2 : 32;
		cal.events = mem_realloc(cal.events, sizeof(Event) * (usize)cal.cap);
	}
	cal.events[cal.nevents++] = *e;
}

static void parse_hm(const char *s, int *h, int *m)
{
	*h = (int)str_to_int(s, &s);
	*m = *s == ':' ? (int)str_to_int(s + 1, NULL) : 0;
}

static void load_events(void)
{
	char *path = data_path();
	usize len;
	char *text = file_read(path, &len);
	mem_free(path);
	if (!text)
		return;
	Json *j = json_parse(text, len);
	mem_free(text);
	if (!j)
		return;
	for (usize i = 0; i < j->count; i++) {
		Json *o = j->items[i];
		Event e = { 0 };
		str_copy(e.title, json_str(json_get(o, "title"), ""), sizeof e.title);
		const char *date = json_str(json_get(o, "date"), "2026-01-01");
		e.y = (int)str_to_int(date, &date);
		e.m = (int)str_to_int(date + 1, &date);
		e.d = (int)str_to_int(date + 1, NULL);
		e.allday = json_bool(json_get(o, "allday"), true);
		parse_hm(json_str(json_get(o, "start"), "09:00"), &e.sh, &e.sm);
		parse_hm(json_str(json_get(o, "end"), "10:00"), &e.eh, &e.em);
		str_copy(e.location, json_str(json_get(o, "location"), ""), sizeof e.location);
		str_copy(e.notes, json_str(json_get(o, "notes"), ""), sizeof e.notes);
		e.color = (int)json_num(json_get(o, "color"), 0);
		const char *r = json_str(json_get(o, "repeat"), "none");
		e.repeat = str_eq(r, "daily") ? REPEAT_DAILY : str_eq(r, "weekly") ? REPEAT_WEEKLY : str_eq(r, "monthly") ? REPEAT_MONTHLY :
			   str_eq(r, "yearly") ? REPEAT_YEARLY : REPEAT_NONE;
		add_event(&e);
	}
	json_free(j);
}

/* Events on a day, ordered: all-day first, then by start time. */
static int events_on(int y, int m, int d, int *out, int max)
{
	int n = 0;
	for (int i = 0; i < cal.nevents && n < max; i++)
		if (occurs_on(&cal.events[i], y, m, d))
			out[n++] = i;
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0; b--) {
			Event *x = &cal.events[out[b - 1]], *y2 = &cal.events[out[b]];
			int kx = x->allday ? -1 : x->sh * 60 + x->sm, ky = y2->allday ? -1 : y2->sh * 60 + y2->sm;
			if (kx <= ky)
				break;
			int t = out[b];
			out[b] = out[b - 1];
			out[b - 1] = t;
		}
	return n;
}

/* ---- Editor ---------------------------------------------------------------------------- */

static void open_editor(Ui *ui, int index, int y, int m, int d)
{
	cal.edit_index = index;
	if (index >= 0) {
		cal.edit = cal.events[index];
	} else {
		Civil now;
		civil_local(time_unix(), &now);
		int h = MIN(now.hour + 1, 22);
		cal.edit = (Event){ .y = y, .m = m, .d = d, .allday = false, .sh = h, .sm = 0, .eh = h + 1, .em = 0 };
	}
	sb_clear(&cal.e_title);
	sb_adds(&cal.e_title, cal.edit.title);
	sb_clear(&cal.e_location);
	sb_adds(&cal.e_location, cal.edit.location);
	sb_clear(&cal.e_notes);
	sb_adds(&cal.e_notes, cal.edit.notes);
	cal.e_repeat = (int)cal.edit.repeat;
	ui_dialog_open(ui, "event");
	ui_focus(ui, "ev-title");
}

static void event_dialog(Ui *ui)
{
	Rect c;
	f32 h = cal.edit.allday ? 470 : 520;
	if (!ui_dialog_begin(ui, "event", 460, h, &c))
		return;
	Theme *t = &ui->theme;
	char head[96];
	fmt(head, sizeof head, "%s, %d %s %d", WEEKDAYS[weekday(cal.edit.y, cal.edit.m, cal.edit.d)], cal.edit.d,
	    MONTHS[cal.edit.m - 1], cal.edit.y);
	ui_heading(ui, cut_top(&c, 26), cal.edit_index >= 0 ? "Edit Event" : "New Event");
	ui_label_dim(ui, cut_top(&c, 20), head);
	cut_top(&c, 10);
	int r = ui_entry(ui, cut_top(&c, 34), "ev-title", &cal.e_title, "Event name", 0);
	cut_top(&c, 12);
	Rect row = cut_top(&c, 30);
	ui_label(ui, cut_left(&row, 120), "All day");
	ui_switch(ui, cut_left(&row, 50), &cal.edit.allday);
	cut_top(&c, 8);
	if (!cal.edit.allday) {
		row = cut_top(&c, 36);
		ui_label_dim(ui, cut_left(&row, 48), "From");
		ui_spin(ui, cut_left(&row, 96), "ev-sh", &cal.edit.sh, 0, 23);
		ui_spin(ui, cut_left(&row, 96), "ev-sm", &cal.edit.sm, 0, 59);
		cut_left(&row, 10);
		ui_label_dim(ui, cut_left(&row, 24), "to");
		ui_spin(ui, cut_left(&row, 96), "ev-eh", &cal.edit.eh, 0, 23);
		ui_spin(ui, cut_left(&row, 80), "ev-em", &cal.edit.em, 0, 59);
		cut_top(&c, 10);
	}
	row = cut_top(&c, 32);
	ui_label_dim(ui, cut_left(&row, 70), "Repeat");
	static const char *const reps[] = { "Never", "Every day", "Every week", "Every month", "Every year" };
	ui_dropdown(ui, cut_left(&row, 150), "ev-repeat", reps, 5, &cal.e_repeat);
	cut_left(&row, 20);
	/* Colour swatches. */
	for (int i = 0; i < (int)ARRAY_LEN(EVENT_COLORS); i++) {
		Rect sw = cut_left(&row, 26);
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "col"), sw);
		ui_pop_id(ui);
		f32 cx = sw.x + sw.w / 2, cy = sw.y + sw.h / 2;
		ui_circle(ui, cx, cy, it.hovered ? 10 : 9, event_color(i));
		if (cal.edit.color == i)
			ui_ring(ui, cx, cy, 12, 2, event_color(i));
		if (it.clicked)
			cal.edit.color = i;
	}
	cut_top(&c, 12);
	ui_entry(ui, cut_top(&c, 32), "ev-loc", &cal.e_location, "Location", 0);
	cut_top(&c, 10);
	Rect buttons = cut_bottom(&c, 34);
	cut_bottom(&c, 12);
	ui_textarea(ui, c, "ev-notes", &cal.e_notes, 13, 0);
	if (sb_str(&cal.e_notes)[0] == 0 && !ui_is_focused(ui, "ev-notes"))
		ui_text(ui, ui_font(ui, 13, 400), c.x + 10, c.y + 20, "Notes", t->text_faint);
	bool save = (r & ENTRY_SUBMIT) != 0;
	if (cal.edit_index >= 0 && ui_button_destructive(ui, cut_left(&buttons, 90), "Delete")) {
		memmove(&cal.events[cal.edit_index], &cal.events[cal.edit_index + 1], sizeof(Event) * (usize)(cal.nevents - cal.edit_index - 1));
		cal.nevents--;
		save_events();
		ui_dialog_close(ui);
	}
	if (ui_button_suggested(ui, cut_right(&buttons, 100), cal.edit_index >= 0 ? "Save" : "Create"))
		save = true;
	cut_right(&buttons, 8);
	if (ui_button(ui, cut_right(&buttons, 90), "Cancel"))
		ui_dialog_close(ui);
	if (save && cal.e_title.len) {
		Event *e = &cal.edit;
		str_copy(e->title, sb_str(&cal.e_title), sizeof e->title);
		str_copy(e->location, sb_str(&cal.e_location), sizeof e->location);
		str_copy(e->notes, sb_str(&cal.e_notes), sizeof e->notes);
		e->repeat = (Repeat)cal.e_repeat;
		if (!e->allday && e->eh * 60 + e->em < e->sh * 60 + e->sm) {
			e->eh = e->sh;
			e->em = e->sm;
		}
		if (cal.edit_index >= 0)
			cal.events[cal.edit_index] = *e;
		else
			add_event(e);
		save_events();
		ui_dialog_close(ui);
	}
	ui_dialog_end(ui);
}

/* ---- Month view -------------------------------------------------------------------------- */

static void shift_month(int delta)
{
	cal.view_m += delta;
	while (cal.view_m < 1) {
		cal.view_m += 12;
		cal.view_y--;
	}
	while (cal.view_m > 12) {
		cal.view_m -= 12;
		cal.view_y++;
	}
}

static void month_grid(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Civil today;
	civil_local(time_unix(), &today);
	/* Weekday names, Monday first. */
	Rect names = cut_top(&r, 30);
	f32 cw = r.w / 7;
	static const char *const short_days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	for (int i = 0; i < 7; i++)
		ui_text_in(ui, ui_font(ui, 12, 600), rect(names.x + cw * (f32)i, names.y, cw, names.h), short_days[i],
			   i >= 5 ? t->text_faint : t->text_dim, ALIGN_CENTER);
	ui_separator(ui, r.x, r.y, r.x + r.w);
	int first_wd = (weekday(cal.view_y, cal.view_m, 1) + 6) % 7; /* 0 = Monday */
	i64 start = day_number(cal.view_y, cal.view_m, 1) - first_wd;
	f32 ch = r.h / 6;
	for (int cell = 0; cell < 42; cell++) {
		Civil c;
		civil_from_unix((start + cell) * 86400, &c);
		int col = cell % 7, row = cell / 7;
		Rect cr = rect(r.x + cw * (f32)col, r.y + ch * (f32)row, cw, ch);
		bool in_month = c.month == cal.view_m;
		bool is_today = c.year == today.year && c.month == today.month && c.day == today.day;
		bool selected = c.year == cal.sel_y && c.month == cal.sel_m && c.day == cal.sel_d;
		ui_push_id(ui, cell);
		UiInteract it = ui_interact(ui, ui_id(ui, "cell"), cr);
		ui_pop_id(ui);
		if (!in_month)
			ui_fill(ui, cr, t->dark ? color_hex("#00000022") : color_hex("#00000008"));
		if (selected)
			ui_fill(ui, cr, color_with_alpha(t->accent, 0x1c));
		else if (it.hovered)
			ui_fill(ui, cr, t->hover);
		if (col > 0)
			ui_vseparator(ui, cr.x, cr.y, cr.y + cr.h);
		if (row > 0)
			ui_separator(ui, cr.x, cr.y, cr.x + cr.w);
		char num[8];
		fmt(num, sizeof num, "%d", c.day);
		Font nf = ui_font(ui, 13, is_today ? 700 : 500);
		f32 nw = ui_text_width(ui, nf, num);
		f32 nx = cr.x + 10, ny = cr.y + 20;
		if (is_today) {
			ui_circle(ui, nx + nw / 2, ny - 5, 12, t->accent);
			ui_text(ui, nf, nx, ny, num, t->on_accent);
		} else {
			ui_text(ui, nf, nx, ny, num, in_month ? (col >= 5 ? t->text_dim : t->text) : t->text_faint);
		}
		/* Event chips. */
		int ids[16];
		int n = events_on(c.year, c.month, c.day, ids, 16);
		f32 y = cr.y + 32;
		int fit = MAX(0, (int)((cr.h - 34) / 20));
		for (int k = 0; k < n; k++) {
			if (k == fit - 1 && n > fit) {
				char more[24];
				fmt(more, sizeof more, "%d more", n - k);
				ui_text(ui, ui_font(ui, 11, 600), cr.x + 8, y + 13, more, t->text_dim);
				break;
			}
			if (k >= fit)
				break;
			Event *e = &cal.events[ids[k]];
			Rect chip = rect(cr.x + 4, y, cr.w - 8, 18);
			Color col_e = event_color(e->color);
			ui_push_clip(ui, chip);
			if (e->allday) {
				ui_round(ui, chip, 4, color_with_alpha(col_e, t->dark ? 0x70 : 0x38));
				ui_text(ui, ui_font(ui, 11, 600), chip.x + 6, chip.y + 13, e->title, t->text);
			} else {
				ui_circle(ui, chip.x + 6, chip.y + 9, 3.5f, col_e);
				char tm[8];
				fmt(tm, sizeof tm, "%d:%02d", e->sh, e->sm);
				f32 x = ui_text(ui, ui_font(ui, 11, 400), chip.x + 14, chip.y + 13, tm, t->text_dim);
				ui_text(ui, ui_font(ui, 11, 500), x + 5, chip.y + 13, e->title, t->text);
			}
			ui_pop_clip(ui);
			y += 20;
		}
		if (it.clicked) {
			cal.sel_y = c.year, cal.sel_m = c.month, cal.sel_d = c.day;
			if (!in_month) {
				cal.view_y = c.year;
				cal.view_m = c.month;
			}
		}
		if (it.double_clicked)
			open_editor(ui, -1, c.year, c.month, c.day);
	}
}

static void agenda(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->sidebar);
	ui_vseparator(ui, r.x, r.y, r.y + r.h);
	Rect in = pad(r, 18);
	char big[8];
	fmt(big, sizeof big, "%d", cal.sel_d);
	Rect head = cut_top(&in, 70);
	ui_text(ui, ui_font(ui, 44, 300), head.x, head.y + 46, big, t->accent);
	f32 bw = ui_text_width(ui, ui_font(ui, 44, 300), big);
	ui_text(ui, ui_font(ui, 15, 600), head.x + bw + 12, head.y + 26, WEEKDAYS[weekday(cal.sel_y, cal.sel_m, cal.sel_d)], t->text);
	char my[32];
	fmt(my, sizeof my, "%s %d", MONTHS[cal.sel_m - 1], cal.sel_y);
	ui_text(ui, ui_font(ui, 13, 400), head.x + bw + 12, head.y + 45, my, t->text_dim);
	ui_separator(ui, in.x, in.y, in.x + in.w);
	cut_top(&in, 12);
	int ids[32];
	int n = events_on(cal.sel_y, cal.sel_m, cal.sel_d, ids, 32);
	if (n == 0) {
		Rect p = cut_top(&in, 160);
		ui_placeholder(ui, p, "calendar", "No Events", "Double-click a day or press + to add one");
		return;
	}
	f32 y = ui_scroll_begin(ui, in, "agenda");
	for (int k = 0; k < n; k++) {
		Event *e = &cal.events[ids[k]];
		f32 hgt = 54 + (e->location[0] ? 18 : 0);
		Rect card = rect(in.x, y, in.w, hgt);
		y += hgt + 8;
		ui_push_id(ui, ids[k]);
		UiInteract it = ui_interact(ui, ui_id(ui, "ev"), card);
		ui_pop_id(ui);
		ui_card(ui, card);
		if (it.hovered)
			ui_round(ui, card, 8, t->hover);
		Color col_e = event_color(e->color);
		ui_round4(ui, rect(card.x, card.y, 4, card.h), 8, 0, 0, 8, col_e);
		Rect ci = pad4(card, 10, 12, 8, 16);
		ui_push_clip(ui, card);
		ui_text(ui, ui_font(ui, 14, 600), ci.x, ci.y + 14, e->title, t->text);
		char when[48];
		if (e->allday)
			str_copy(when, "All day", sizeof when);
		else
			fmt(when, sizeof when, "%02d:%02d – %02d:%02d", e->sh, e->sm, e->eh, e->em);
		if (e->repeat != REPEAT_NONE) {
			static const char *const rp[] = { "", " · daily", " · weekly", " · monthly", " · yearly" };
			str_copy(when + strlen(when), rp[e->repeat], sizeof when - strlen(when));
		}
		ui_text(ui, ui_font(ui, 12, 400), ci.x, ci.y + 33, when, t->text_dim);
		if (e->location[0]) {
			ui_icon(ui, rect(ci.x - 1, ci.y + 40, 13, 13), "location", t->text_faint);
			ui_text(ui, ui_font(ui, 12, 400), ci.x + 16, ci.y + 51, e->location, t->text_dim);
		}
		ui_pop_clip(ui);
		if (it.clicked)
			open_editor(ui, ids[k], e->y, e->m, e->d);
	}
	ui_scroll_end(ui, y - in.y);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	char title[48];
	fmt(title, sizeof title, "%s %d", MONTHS[cal.view_m - 1], cal.view_y);
	Rect tools = ui_headerbar(ui, header, title, 0);
	if (ui_icon_button(ui, cut_left(&tools, 34), "go-back", "Previous month"))
		shift_month(-1);
	if (ui_icon_button(ui, cut_left(&tools, 34), "go-forward", "Next month"))
		shift_month(1);
	cut_left(&tools, 6);
	if (ui_button(ui, center_in(cut_left(&tools, 74), 70, 30), "Today")) {
		Civil now;
		civil_local(time_unix(), &now);
		cal.view_y = cal.sel_y = now.year;
		cal.view_m = cal.sel_m = now.month;
		cal.sel_d = now.day;
	}
	if (ui_icon_button(ui, cut_right(&tools, 34), "add", "New Event"))
		open_editor(ui, -1, cal.sel_y, cal.sel_m, cal.sel_d);
	/* Keyboard: arrows move the selection, PageUp/PageDown change month. */
	for (int i = 0; i < ui->nkeys && !ui->open_dialog; i++) {
		UiKey *k = &ui->keys[i];
		if (!k->pressed)
			continue;
		int dd = k->code == KEY_LEFT ? -1 : k->code == KEY_RIGHT ? 1 : k->code == KEY_UP ? -7 : k->code == KEY_DOWN ? 7 : 0;
		if (dd) {
			Civil c;
			civil_from_unix((day_number(cal.sel_y, cal.sel_m, cal.sel_d) + dd) * 86400, &c);
			cal.sel_y = c.year, cal.sel_m = c.month, cal.sel_d = c.day;
			cal.view_y = c.year, cal.view_m = c.month;
		} else if (k->code == KEY_PAGEUP) {
			shift_month(-1);
		} else if (k->code == KEY_PAGEDOWN) {
			shift_month(1);
		} else if (k->code == KEY_ENTER) {
			open_editor(ui, -1, cal.sel_y, cal.sel_m, cal.sel_d);
		}
	}
	Rect side = cut_right(&win, minf(320, win.w * 0.34f));
	ui_fill(ui, win, t->view);
	month_grid(ui, win);
	agenda(ui, side);
	event_dialog(ui);
}

static void init(Ui *ui)
{
	UNUSED(ui);
	Civil now;
	civil_local(time_unix(), &now);
	cal.view_y = cal.sel_y = now.year;
	cal.view_m = cal.sel_m = now.month;
	cal.sel_d = now.day;
	load_events();
}

int main(void)
{
	UiAppDesc d = { .id = "calendar", .title = "Calendar", .width = 1060, .height = 700, .min_width = 760, .min_height = 520,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
