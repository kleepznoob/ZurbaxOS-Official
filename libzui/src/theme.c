/* Light and dark themes and the accent color palette. */
#include "zui.h"

const char *const THEME_ACCENT_NAMES[] = { "blue", "teal", "green", "yellow", "orange", "red", "pink", "purple", "graphite" };
const u32 THEME_ACCENT_COLORS[] = {
	0xff3a86e8, 0xff1fa59f, 0xff4aa84a, 0xffd9a521, 0xffef8a2f,
	0xffdf4b4b, 0xffd9578f, 0xff8b5bd4, 0xff6b7280,
};
const int THEME_ACCENT_COUNT = (int)ARRAY_LEN(THEME_ACCENT_COLORS);

Color theme_accent_named(const char *name)
{
	for (int i = 0; i < THEME_ACCENT_COUNT; i++)
		if (str_eq(name, THEME_ACCENT_NAMES[i]))
			return THEME_ACCENT_COLORS[i];
	return THEME_ACCENT_COLORS[0];
}

void theme_build(Theme *t, const ThemeInfo *info)
{
	memset(t, 0, sizeof *t);
	t->dark = info && info->dark;
	t->reduce_motion = info && info->reduce_motion;
	t->text_scale = (info && info->text_scale > 0.5f) ? info->text_scale : 1.0f;
	Color accent = (info && info->accent) ? info->accent | 0xff000000u : THEME_ACCENT_COLORS[0];
	t->accent = accent;
	t->accent_hover = color_lighten(accent, t->dark ? 0.12f : 0.08f);
	t->accent_pressed = color_lighten(accent, -0.12f);
	/* White text on light accents like yellow would be unreadable. */
	f32 h, s, l;
	color_to_hsl(accent, &h, &s, &l);
	t->on_accent = (l > 0.62f || (h > 40 && h < 70 && l > 0.5f)) ? color_hex("#26292e") : COLOR_WHITE;

	if (!t->dark) {
		t->window = color_hex("#f7f7f8");
		t->view = color_hex("#ffffff");
		t->sidebar = color_hex("#efeff1");
		t->header_top = color_hex("#f8f8f9");
		t->header_bottom = color_hex("#ececee");
		t->card = color_hex("#ffffff");
		t->border = color_hex("#0000001c");
		t->border_strong = color_hex("#00000036");
		t->text = color_hex("#2e3036");
		t->text_dim = color_hex("#6b6e76");
		t->text_faint = color_hex("#9a9da4");
		t->hover = color_hex("#0000000c");
		t->press = color_hex("#00000018");
		t->selection = color_with_alpha(accent, 0x2e);
		t->input = color_hex("#ffffff");
		t->button_top = color_hex("#fdfdfd");
		t->button_bottom = color_hex("#f0f0f2");
		t->button_border = color_hex("#00000030");
		t->shadow = color_hex("#00000030");
	} else {
		t->window = color_hex("#2b2c30");
		t->view = color_hex("#24252a");
		t->sidebar = color_hex("#26272b");
		t->header_top = color_hex("#36373c");
		t->header_bottom = color_hex("#2f3035");
		t->card = color_hex("#323338");
		t->border = color_hex("#ffffff17");
		t->border_strong = color_hex("#ffffff2a");
		t->text = color_hex("#eeeff1");
		t->text_dim = color_hex("#a6a8ae");
		t->text_faint = color_hex("#787a81");
		t->hover = color_hex("#ffffff10");
		t->press = color_hex("#ffffff1c");
		t->selection = color_with_alpha(accent, 0x48);
		t->input = color_hex("#1f2024");
		t->button_top = color_hex("#45464c");
		t->button_bottom = color_hex("#3b3c41");
		t->button_border = color_hex("#00000060");
		t->shadow = color_hex("#00000070");
	}
	t->danger = color_hex("#d9433f");
	t->success = color_hex("#3f9e4a");
	t->warning = color_hex("#e0a223");
}
