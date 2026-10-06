/* Format detection and BMP decoding (used for the firmware boot logo). */
#include "zg.h"

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

Canvas *bmp_decode(const u8 *d, usize len)
{
	if (len < 54 || d[0] != 'B' || d[1] != 'M')
		return NULL;
	u32 off = le32(d + 10), hsize = le32(d + 14);
	i32 w = (i32)le32(d + 18), h = (i32)le32(d + 22);
	u16 bpp = le16(d + 28);
	u32 comp = le32(d + 30);
	if (hsize < 40 || w <= 0 || w > 16384 || h == 0 || ABS(h) > 16384 || (comp != 0 && comp != 3))
		return NULL;
	if (bpp != 24 && bpp != 32 && bpp != 8)
		return NULL;
	bool bottom_up = h > 0;
	if (h < 0)
		h = -h;
	usize rowbytes = ((usize)w * bpp / 8 + 3) & ~3u;
	if (off + rowbytes * (usize)h > len)
		return NULL;
	const u8 *pal = d + 14 + hsize;
	Canvas *c = canvas_new(w, h);
	for (int y = 0; y < h; y++) {
		const u8 *src = d + off + rowbytes * (usize)(bottom_up ? h - 1 - y : y);
		u32 *dst = canvas_row(c, y);
		for (int x = 0; x < w; x++) {
			u32 r, g, b, a = 255;
			if (bpp == 8) {
				const u8 *p = pal + src[x] * 4;
				b = p[0], g = p[1], r = p[2];
			} else if (bpp == 24) {
				b = src[x * 3], g = src[x * 3 + 1], r = src[x * 3 + 2];
			} else {
				b = src[x * 4], g = src[x * 4 + 1], r = src[x * 4 + 2], a = src[x * 4 + 3];
			}
			dst[x] = color_premul(RGBA(r, g, b, a));
		}
	}
	/* 32-bit BMPs frequently carry a meaningless zero alpha channel. */
	if (bpp == 32) {
		bool all_zero = true;
		for (int y = 0; y < h && all_zero; y++)
			for (int x = 0; x < w; x++)
				if (canvas_row(c, y)[x] >> 24) {
					all_zero = false;
					break;
				}
		if (all_zero) {
			for (int y = 0; y < h; y++) {
				const u8 *src = d + off + rowbytes * (usize)(bottom_up ? h - 1 - y : y);
				for (int x = 0; x < w; x++)
					canvas_row(c, y)[x] = RGB(src[x * 4 + 2], src[x * 4 + 1], src[x * 4]);
			}
		}
	}
	return c;
}

Canvas *image_decode(const void *data, usize len)
{
	const u8 *d = data;
	if (len >= 8 && d[0] == 137 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
		return png_decode(d, len);
	if (len >= 3 && d[0] == 0xff && d[1] == 0xd8)
		return jpeg_decode(d, len, 1);
	if (len >= 2 && d[0] == 'B' && d[1] == 'M')
		return bmp_decode(d, len);
	if (len >= 6 && memcmp(d, "GIF8", 4) == 0)
		return gif_decode(d, len);
	return NULL;
}

Canvas *image_load(const char *path)
{
	usize len;
	u8 *data = (u8 *)file_read(path, &len);
	if (!data)
		return NULL;
	Canvas *c = image_decode(data, len);
	mem_free(data);
	return c;
}

Canvas *image_load_thumb(const char *path, int max_size)
{
	usize len;
	u8 *data = (u8 *)file_read(path, &len);
	if (!data)
		return NULL;
	Canvas *c;
	if (len >= 3 && data[0] == 0xff && data[1] == 0xd8) {
		/* Decode big photos at 1/2, 1/4 or 1/8 scale straight from the DCT:
		 * a 1/8 decode is nearly free and tells us the full size. */
		c = jpeg_decode(data, len, 8);
		if (c) {
			int full = MAX(c->width, c->height) * 8;
			int scale = 8;
			while (scale > 1 && full / scale < max_size)
				scale /= 2;
			if (scale != 8) {
				canvas_free(c);
				c = jpeg_decode(data, len, scale);
			}
		}
	} else {
		c = image_decode(data, len);
	}
	mem_free(data);
	if (c && MAX(c->width, c->height) > max_size) {
		f32 s = (f32)max_size / (f32)MAX(c->width, c->height);
		Canvas *t = canvas_resize(c, MAX(1, (int)((f32)c->width * s)), MAX(1, (int)((f32)c->height * s)));
		canvas_free(c);
		c = t;
	}
	return c;
}
