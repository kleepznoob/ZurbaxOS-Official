/*
 * wallpaper: renders Zurbax's wallpapers to PNGs.
 *
 *     wallpaper NAME OUT.png [WIDTH HEIGHT]
 *     wallpaper --gnome DIR XML INSTALLED_DIR [WIDTH HEIGHT]
 *
 * The layer build uses it for GNOME's backgrounds, so both desktops open
 * on the same picture and offer the same ones: --gnome renders every
 * wallpaper into DIR and writes the list GNOME's Appearance settings read,
 * naming the pictures where they'll be (INSTALLED_DIR).
 */
#include "zr.h"
#include "zg.h"
#include "zui.h"

static bool render(const char *name, const char *out, int w, int h)
{
	Canvas *c = wallpaper_render(name, w, h);
	bool ok = c && png_save(c, out);
	if (c)
		canvas_free(c);
	if (!ok)
		eprint("wallpaper: couldn't write %s\n", out);
	return ok;
}

static int gnome(const char *dir, const char *xml, const char *installed, int w, int h)
{
	StrBuf b = { 0 };
	sb_adds(&b, "<?xml version=\"1.0\"?>\n<!DOCTYPE wallpapers SYSTEM \"gnome-wp-list.dtd\">\n<wallpapers>\n");
	for (int i = 0; i < WALLPAPER_COUNT; i++) {
		char *out = str_fmt("%s/%s.png", dir, WALLPAPER_NAMES[i]);
		if (!render(WALLPAPER_NAMES[i], out, w, h))
			return 1;
		mem_free(out);
		out = str_fmt("%s/%s.png", installed, WALLPAPER_NAMES[i]);
		sb_fmt(&b,
			"  <wallpaper deleted=\"false\">\n    <name>%s</name>\n    <filename>%s</filename>\n"
			"    <filename-dark>%s</filename-dark>\n    <options>zoom</options>\n    <shade_type>solid</shade_type>\n"
			"    <pcolor>#2c001e</pcolor>\n    <scolor>#2c001e</scolor>\n  </wallpaper>\n",
			WALLPAPER_TITLES[i], out, out);
		mem_free(out);
	}
	sb_adds(&b, "</wallpapers>\n");
	if (!file_write(xml, b.data, b.len)) {
		eprint("wallpaper: couldn't write %s\n", xml);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	bool all = argc > 1 && str_eq(argv[1], "--gnome");
	int first = all ? 3 : 1;
	if (argc < first + 2) {
		eprint("usage: wallpaper NAME OUT.png [WIDTH HEIGHT]\n       wallpaper --gnome DIR XML INSTALLED_DIR [WIDTH HEIGHT]\n");
		return 2;
	}
	int w = 3840, h = 2160;
	if (argc > first + 3)
		w = (int)str_to_int(argv[first + 2], NULL), h = (int)str_to_int(argv[first + 3], NULL);
	if (w < 16 || h < 16 || w > 16384 || h > 16384) {
		eprint("wallpaper: %dx%d isn't a size for a picture\n", w, h);
		return 2;
	}
	if (all)
		return gnome(argv[2], argv[3], argv[4], w, h);
	return render(argv[1], argv[2], w, h) ? 0 : 1;
}
