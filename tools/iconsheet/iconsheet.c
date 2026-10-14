/*
 * iconsheet: draws icons side by side into one PNG, the way Zurbax draws
 * them, to look a set over.
 *
 *     iconsheet OUT.png SIZE COLUMNS BACKGROUND ICON.svg...
 *
 * BACKGROUND is a colour such as "#20232a".
 */
#include "zr.h"
#include "zg.h"
#include "zg/svg.h"

int main(int argc, char **argv)
{
	if (argc < 6) {
		eprint("usage: iconsheet OUT.png SIZE COLUMNS BACKGROUND ICON.svg...\n");
		return 2;
	}
	int size = (int)str_to_int(argv[2], NULL), cols = (int)str_to_int(argv[3], NULL);
	int n = argc - 5, pad = size / 4;
	if (size < 8 || size > 1024 || cols < 1)
		return 2;
	int rows = (n + cols - 1) / cols;
	Canvas *c = canvas_new(cols * (size + pad) + pad, rows * (size + pad) + pad);
	canvas_clear(c, color_hex(argv[4]));
	for (int i = 0; i < n; i++) {
		SvgDoc *d = svg_load(argv[5 + i]);
		if (!d) {
			eprint("iconsheet: couldn't read %s\n", argv[5 + i]);
			continue;
		}
		f32 x = (f32)(pad + (i % cols) * (size + pad)), y = (f32)(pad + (i / cols) * (size + pad));
		svg_render(d, c, rect(x, y, (f32)size, (f32)size), color_hex("#ffffff"));
		svg_free(d);
	}
	bool ok = png_save(c, argv[1]);
	canvas_free(c);
	return ok ? 0 : 1;
}
