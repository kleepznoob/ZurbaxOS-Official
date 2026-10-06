/* Image decoding and encoding. Decoded images are canvases (premultiplied). */
#pragma once

#include "canvas.h"

/* Detects the format from the data and decodes it. NULL on failure. */
Canvas *image_decode(const void *data, usize len);
Canvas *image_load(const char *path);
/* Fast thumbnail load: JPEGs decode at reduced scale when possible. */
Canvas *image_load_thumb(const char *path, int max_size);

Canvas *png_decode(const u8 *data, usize len);
Canvas *jpeg_decode(const u8 *data, usize len, int scale_down /* 1, 2, 4 or 8 */);
Canvas *bmp_decode(const u8 *data, usize len);
Canvas *gif_decode(const u8 *data, usize len);   /* the first frame */

/* Encodes as PNG; returns an allocated buffer. */
u8 *png_encode(const Canvas *c, usize *out_len);
bool png_save(const Canvas *c, const char *path);

/* zlib / DEFLATE (also used by the network stack). */
u8 *zlib_inflate(const u8 *data, usize len, usize *out_len, bool has_header);
u8 *zlib_deflate(const u8 *data, usize len, usize *out_len);
u32 crc32_update(u32 crc, const void *data, usize len);
u32 adler32_update(u32 adler, const void *data, usize len);
