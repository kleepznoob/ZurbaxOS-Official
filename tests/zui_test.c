/* Renders a widget gallery offscreen (light and dark) for visual review. */
#include "zui.h"

typedef struct {
	int page;
	bool wifi, bluetooth, dark;
	bool check1, check2;
	int radio;
	f32 volume;
	int seg;
	int drop;
	int spin;
	StrBuf search, name, notes;
} Gallery;

static void frame(Ui *ui)
{
	Gallery *g = ui->user;
	Theme *t = &ui->theme;
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	Rect tools = ui_headerbar(ui, header, "Widget Gallery", 0);
	ui_entry(ui, center_in(tools, 260, 30), "search", &g->search, "Search settings", ENTRY_SEARCH);

	Rect side = cut_left(&win, 220);
	ui_fill(ui, side, t->sidebar);
	ui_vseparator(ui, side.x + side.w - 1, side.y, side.y + side.h);
	Rect s = pad(side, 8);
	ui_sidebar_heading(ui, cut_top(&s, 28), "NETWORK");
	static const char *icons[] = { "network-wireless", "bluetooth", "network-wired" };
	static const char *labels[] = { "Wi-Fi", "Bluetooth", "Ethernet" };
	for (int i = 0; i < 3; i++) {
		ui_push_id(ui, i);
		if (ui_sidebar_item(ui, cut_top(&s, 34), icons[i], labels[i], g->page == i))
			g->page = i;
		ui_pop_id(ui);
	}
	cut_top(&s, 8);
	ui_sidebar_heading(ui, cut_top(&s, 28), "PERSONAL");
	static const char *icons2[] = { "wallpaper", "display-brightness", "hourglass", "user" };
	static const char *labels2[] = { "Desktop", "Display", "Screen Time", "Users" };
	for (int i = 0; i < 4; i++) {
		ui_push_id(ui, 10 + i);
		if (ui_sidebar_item(ui, cut_top(&s, 34), icons2[i], labels2[i], g->page == 10 + i))
			g->page = 10 + i;
		ui_pop_id(ui);
	}

	Rect body = pad(win, 24);
	ui_title(ui, cut_top(&body, 34), "Wi-Fi");
	ui_label_dim(ui, cut_top(&body, 22), "Connect to wireless networks and manage saved ones.");
	cut_top(&body, 14);

	Rect card = cut_top(&body, 156);
	ui_card(ui, card);
	Rect c = pad2(card, 16, 6);
	const char *names[] = { "Wi-Fi", "Bluetooth", "Dark style" };
	bool *vals[] = { &g->wifi, &g->bluetooth, &g->dark };
	for (int i = 0; i < 3; i++) {
		Rect row = cut_top(&c, 48);
		ui_label(ui, row, names[i]);
		ui_switch(ui, row, vals[i]);
		if (i < 2)
			ui_separator(ui, row.x, row.y + row.h, row.x + row.w);
	}
	cut_top(&body, 16);

	Rect row = cut_top(&body, 34);
	ui_button(ui, cut_left(&row, 110), "Cancel");
	cut_left(&row, 8);
	ui_button_suggested(ui, cut_left(&row, 130), "Connect");
	cut_left(&row, 8);
	ui_button_destructive(ui, cut_left(&row, 110), "Forget");
	cut_left(&row, 12);
	ui_icon_button(ui, cut_left(&row, 34), "refresh", "Refresh");
	ui_icon_button(ui, cut_left(&row, 34), "settings", "Settings");
	cut_top(&body, 16);

	row = cut_top(&body, 32);
	static const char *segs[] = { "Day", "Week", "Month" };
	ui_segmented(ui, cut_left(&row, 240), segs, 3, &g->seg);
	cut_left(&row, 12);
	static const char *drops[] = { "Automatic", "Manual", "Disabled" };
	ui_dropdown(ui, cut_left(&row, 150), "mode", drops, 3, &g->drop);
	cut_left(&row, 12);
	ui_spin(ui, cut_left(&row, 120), "spin", &g->spin, 0, 99);
	cut_top(&body, 16);

	row = cut_top(&body, 28);
	ui_icon(ui, cut_left(&row, 20), "audio-volume-medium", t->text_dim);
	cut_left(&row, 8);
	ui_slider(ui, cut_left(&row, 260), &g->volume, 0, 1);
	cut_left(&row, 16);
	ui_progress(ui, cut_left(&row, 160), 0.62f);
	cut_left(&row, 16);
	ui_spinner(ui, cut_left(&row, 22));
	cut_top(&body, 14);

	row = cut_top(&body, 26);
	ui_checkbox(ui, cut_left(&row, 170), "Connect automatically", &g->check1);
	ui_checkbox(ui, cut_left(&row, 150), "Metered connection", &g->check2);
	if (ui_radio(ui, cut_left(&row, 90), "WPA2", g->radio == 0))
		g->radio = 0;
	if (ui_radio(ui, cut_left(&row, 90), "WPA3", g->radio == 1))
		g->radio = 1;
	cut_top(&body, 14);

	row = cut_top(&body, 32);
	ui_entry(ui, cut_left(&row, 280), "name", &g->name, "Network name", 0);
	cut_left(&row, 12);
	static StrBuf pw;
	if (!pw.len)
		sb_adds(&pw, "secret");
	ui_entry(ui, cut_left(&row, 200), "pw", &pw, "Password", ENTRY_PASSWORD);
	cut_top(&body, 14);
	ui_textarea(ui, cut_top(&body, 90), "notes", &g->notes, 13, 0);
	cut_top(&body, 14);
	row = cut_top(&body, 48);
	ui_avatar(ui, cut_left(&row, 48), "Zurbax User");
	cut_left(&row, 12);
	Rect who = cut_left(&row, 300);
	ui_heading(ui, cut_top(&who, 24), "Zurbax User");
	ui_label_dim(ui, who, "Administrator");
	ui_badge(ui, row.x + 20, row.y + 12, "3");
}

static void init(Ui *ui)
{
	Gallery *g = ui->user;
	g->wifi = true;
	g->volume = 0.6f;
	g->check1 = true;
	g->page = 0;
	sb_adds(&g->name, "Home Network 5G");
	sb_adds(&g->notes, "The quick brown fox jumps over the lazy dog. Text wraps across lines inside the editor, "
			   "and long documents scroll smoothly.\nSecond paragraph.");
	ui_focus(ui, "name");
}

int main(void)
{
	static Gallery g;
	UiAppDesc d = { .id = "gallery", .title = "Widget Gallery", .width = 900, .height = 720, .init = init, .frame = frame, .user = &g };
	bool ok = ui_snapshot(&d, 900, 720, 1, false, "build/tests/zui_light.png");
	memset(&g, 0, sizeof g);
	ok = ui_snapshot(&d, 900, 720, 1, true, "build/tests/zui_dark.png") && ok;
	print("zui: gallery rendered (%s)\n", ok ? "ok" : "failed");
	return ok ? 0 : 1;
}
