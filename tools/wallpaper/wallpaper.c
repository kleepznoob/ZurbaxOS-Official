/*
 * wallpaper: renders one of Zurbax's wallpapers to a PNG.
 *
 *     wallpaper NAME OUT.png [WIDTH HEIGHT]
 *
 * The layer build uses it for GNOME's background, so both desktops open
 * on the same picture.
 */
#include "zr.h"
#include "zg.h"
#include "zui.h"

int main(int argc, char **argv)
{
	if (argc < 3) {
		eprint("usage: wallpaper NAME OUT.png [WIDTH HEIGHT]\n");
		return 2;
	}
	int w = argc > 4 ? (int)str_to_int(argv[3], NULL) : 3840, h = argc > 4 ? (int)str_to_int(argv[4], NULL) : 2160;
	if (w < 16 || h < 16 || w > 16384 || h > 16384) {
		eprint("wallpaper: %dx%d isn't a size for a picture\n", w, h);
		return 2;
	}
	Canvas *c = wallpaper_render(argv[1], w, h);
	if (!c || !png_save(c, argv[2])) {
		eprint("wallpaper: couldn't write %s\n", argv[2]);
		return 1;
	}
	return 0;
}
