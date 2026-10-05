/* Renders every icon with the SVG renderer into build/tests/icons.png. */
#include "zg.h"

static int count_failures;

static void sheet(const char *dir, int size, int cols, int y0, Canvas *c, Color tint, bool labels)
{
	DirList list = { 0 };
	if (!dir_list(dir, &list, false)) {
		eprint("cannot list %s\n", dir);
		count_failures++;
		return;
	}
	int cell = labels ? size + 40 : size + 16;
	int i = 0;
	vec_each(&list, e)
	{
		if (!str_ends(e->name, ".svg"))
			continue;
		char *path = path_join(dir, e->name);
		SvgDoc *doc = svg_load(path);
		if (!doc) {
			eprint("  FAIL parsing %s\n", path);
			count_failures++;
		} else {
			int x = 24 + (i % cols) * cell, y = y0 + (i / cols) * (cell + (labels ? 22 : 8));
			if (labels)
				draw_shadow(c, rect((f32)x + size * 0.12f, (f32)y + size * 0.16f, size * 0.76f, size * 0.76f), size * 0.2f, 6, 0, 3, color_hex("#00000045"));
			svg_render(doc, c, rect((f32)x, (f32)y, (f32)size, (f32)size), tint);
			if (labels) {
				char name[64];
				str_copy(name, e->name, sizeof name);
				name[strlen(name) - 4] = 0;
				draw_text_in(c, font(12, 500), rect((f32)x - 20, (f32)(y + size + 4), (f32)size + 40, 18), name, color_hex("#333333"), ALIGN_CENTER);
			}
			svg_free(doc);
		}
		mem_free(path);
		i++;
	}
	dir_list_free(&list);
}

int main(void)
{
	Canvas *c = canvas_new(1240, 1060);
	canvas_clear(c, color_hex("#f6f6f7"));
	sheet("data/icons/apps", 80, 9, 24, c, COLOR_BLACK, true);
	fill_rect(c, rect(0, 470, 1240, 590), color_hex("#2b2c30"));
	sheet("data/icons/symbolic", 24, 30, 500, c, color_hex("#ececef"), false);
	png_save(c, "build/tests/icons.png");
	canvas_free(c);
	print("svg: icon sheet rendered (%d failures)\n", count_failures);
	return count_failures ? 1 : 0;
}
