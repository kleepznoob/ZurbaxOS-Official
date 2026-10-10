/* Light and dark themes and the accent color palette.
 *
 * Zurbax's look is quiet and flat: neutral greys with a trace of warmth, one
 * accent color for what matters, hairlines instead of bevels and no
 * gradients on controls. Iris, a blue-violet, is the accent out of the box. */
#include "zui.h"

const char *const THEME_ACCENT_NAMES[] = { "iris", "blue", "teal", "green", "gold", "orange", "red", "pink", "purple", "graphite" };
const u32 THEME_ACCENT_COLORS[] = {
	0xff5b5fe0, 0xff2f7fe0, 0xff15a199, 0xff3ca34d, 0xffd9a21b,
	0xffeb7330, 0xffe0443e, 0xffd9508a, 0xff9a5bd4, 0xff6b7280,
};
const int THEME_ACCENT_COUNT = (int)ARRAY_LEN(THEME_ACCENT_COLORS);

Color theme_accent_named(const char *name)
{
	if (name && str_eq(name, "yellow")) /* the name before Zurbax 2.0's palette */
		name = "gold";
	for (int i = 0; i < THEME_ACCENT_COUNT; i++)
		if (name && str_eq(name, THEME_ACCENT_NAMES[i]))
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
	t->accent_hover = color_lighten(accent, t->dark ? 0.1f : 0.06f);
	t->accent_pressed = color_lighten(accent, -0.1f);
	/* White text on light accents like gold would be unreadable. */
	f32 h, s, l;
	color_to_hsl(accent, &h, &s, &l);
	t->on_accent = (l > 0.62f || (h > 38 && h < 70 && l > 0.45f)) ? color_hex("#1d1e22") : COLOR_WHITE;

	if (!t->dark) {
		t->window = color_hex("#f7f7f6");
		t->view = color_hex("#ffffff");
		t->sidebar = color_hex("#efefed");
		t->header_top = t->header_bottom = color_hex("#efefed");
		t->card = color_hex("#ffffff");
		t->border = color_hex("#00000014");
		t->border_strong = color_hex("#00000026");
		t->text = color_hex("#1d1e22");
		t->text_dim = color_hex("#5d5f66");
		t->text_faint = color_hex("#9b9da3");
		t->hover = color_hex("#0000000c");
		t->press = color_hex("#00000017");
		t->selection = color_with_alpha(accent, 0x26);
		t->input = color_hex("#ffffff");
		t->button_top = t->button_bottom = color_hex("#e9e9e7");
		t->button_border = color_hex("#00000000");
		t->shadow = color_hex("#00000026");
	} else {
		t->window = color_hex("#1e1f22");
		t->view = color_hex("#18191b");
		t->sidebar = color_hex("#232427");
		t->header_top = t->header_bottom = color_hex("#252629");
		t->card = color_hex("#28292d");
		t->border = color_hex("#ffffff12");
		t->border_strong = color_hex("#ffffff22");
		t->text = color_hex("#ededef");
		t->text_dim = color_hex("#a2a4aa");
		t->text_faint = color_hex("#727479");
		t->hover = color_hex("#ffffff0e");
		t->press = color_hex("#ffffff1a");
		t->selection = color_with_alpha(accent, 0x40);
		t->input = color_hex("#151618");
		t->button_top = t->button_bottom = color_hex("#323337");
		t->button_border = color_hex("#00000000");
		t->shadow = color_hex("#00000070");
	}
	t->danger = color_hex("#e0443e");
	t->success = color_hex("#2f9e55");
	t->warning = color_hex("#e3a21a");
}
