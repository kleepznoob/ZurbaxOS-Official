/*
 * A Windows program's resources: the tree of icons, version information and
 * so on inside a .exe, the same Explorer shows. Only the section holding
 * them is read, however big the program; every offset in it is checked, as
 * the file can be anything.
 */
#include "pe.h"

#define RT_ICON 3
#define RT_GROUP_ICON 14
#define RT_VERSION 16
#define MAX_SECTION (64u << 20)

typedef struct {
	const u8 *d; /* the section with the resources */
	usize n;
	u32 rva;     /* where the section sits in memory: data entries point there */
	u32 dir;     /* the root directory's offset in d; directory offsets count from it */
} Rsrc;

static inline u16 rd16(const u8 *p)
{
	return (u16)(p[0] | p[1] << 8);
}

static inline u32 rd32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static bool in(const Rsrc *r, u64 off, u64 len)
{
	return off <= r->n && len <= r->n - off;
}

/* In the directory at dir: the entry for id, or the first entry. */
static bool entry(const Rsrc *r, u32 dir, u32 id, bool first, u32 *off, bool *sub)
{
	if (!in(r, dir, 16))
		return false;
	u32 n = (u32)rd16(r->d + dir + 12) + rd16(r->d + dir + 14);
	for (u32 i = 0; i < n && i < 4096; i++) {
		u64 e = (u64)dir + 16 + (u64)i * 8;
		if (!in(r, e, 8))
			return false;
		u32 name = rd32(r->d + e), o = rd32(r->d + e + 4);
		if (first || (!(name & 0x80000000u) && name == id)) {
			*off = o & 0x7fffffffu;
			*sub = (o & 0x80000000u) != 0;
			return true;
		}
	}
	return false;
}

/* A resource's bytes: type, then id (or the first, when id < 0), then any language. */
static const u8 *resource(const Rsrc *r, u32 type, i64 id, u32 *len)
{
	u32 off;
	bool sub;
	if (!entry(r, r->dir, type, false, &off, &sub) || !sub)
		return NULL;
	if (!entry(r, r->dir + off, (u32)id, id < 0, &off, &sub) || !sub)
		return NULL;
	if (!entry(r, r->dir + off, 0, true, &off, &sub) || sub)
		return NULL;
	u64 de = (u64)r->dir + off;
	if (!in(r, de, 16))
		return NULL;
	u32 at = rd32(r->d + de), size = rd32(r->d + de + 4);
	if (at < r->rva || !in(r, at - r->rva, size))
		return NULL;
	*len = size;
	return r->d + (at - r->rva);
}

/* An icon image stored as a bitmap: the colours bottom-up, then a 1-bit mask. */
static Canvas *dib_decode(const u8 *p, u32 n)
{
	if (n < 40)
		return NULL;
	u32 hs = rd32(p), comp = rd32(p + 16), used = rd32(p + 32);
	i32 w = (i32)rd32(p + 4), h = (i32)rd32(p + 8) / 2;
	u16 bpp = rd16(p + 14);
	if (hs < 40 || hs > n || w <= 0 || w > 1024 || h <= 0 || h > 1024 || comp != 0)
		return NULL;
	if (bpp != 32 && bpp != 24 && bpp != 8 && bpp != 4 && bpp != 1)
		return NULL;
	u32 colors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
	if (colors > 256)
		return NULL;
	u64 xs = ((u64)w * bpp + 31) / 32 * 4, ms = ((u64)w + 31) / 32 * 4;
	u64 xor_at = hs + (u64)colors * 4, mask_at = xor_at + xs * (u64)h;
	if (mask_at > n)
		return NULL;
	bool has_mask = mask_at + ms * (u64)h <= n;
	/* 32-bit icons carry alpha, unless an old tool left it all zero */
	bool alpha = false;
	if (bpp == 32)
		for (i32 y = 0; y < h && !alpha; y++)
			for (i32 x = 0; x < w && !alpha; x++)
				alpha = p[xor_at + xs * (u64)y + (u64)x * 4 + 3] != 0;
	Canvas *c = canvas_new(w, h);
	for (i32 y = 0; y < h; y++) {
		const u8 *row = p + xor_at + xs * (u64)(h - 1 - y);
		const u8 *mrow = has_mask ? p + mask_at + ms * (u64)(h - 1 - y) : NULL;
		u32 *out = canvas_row(c, y);
		for (i32 x = 0; x < w; x++) {
			u32 b, g, r, a = 255;
			if (bpp == 32) {
				b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
				if (alpha)
					a = row[x * 4 + 3];
			} else if (bpp == 24) {
				b = row[x * 3], g = row[x * 3 + 1], r = row[x * 3 + 2];
			} else {
				u32 i = bpp == 8 ? row[x] : bpp == 4 ? (u32)(row[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (u32)(row[x / 8] >> (7 - (x & 7))) & 1;
				const u8 *q = p + hs + (u64)(i < colors ? i : 0) * 4;
				b = q[0], g = q[1], r = q[2];
			}
			if (!alpha && mrow && (mrow[x / 8] >> (7 - (x & 7)) & 1))
				a = 0;
			out[x] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
		}
	}
	return c;
}

/* The first icon group (the one Explorer shows), its largest, most colourful image. */
static Canvas *main_icon(const Rsrc *r)
{
	u32 len;
	const u8 *g = resource(r, RT_GROUP_ICON, -1, &len);
	if (!g || len < 6)
		return NULL;
	u32 count = rd16(g + 4), best = 0, best_id = 0;
	for (u32 i = 0; i < count && 6 + (i + 1) * 14 <= len; i++) {
		const u8 *e = g + 6 + i * 14;
		u32 w = e[0] ? e[0] : 256, bits = rd16(e + 6);
		u32 score = w * 64 + MIN(bits, 32u);
		if (score > best)
			best = score, best_id = rd16(e + 12);
	}
	if (!best)
		return NULL;
	const u8 *img = resource(r, RT_ICON, best_id, &len);
	if (!img || len < 8)
		return NULL;
	static const u8 PNG[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	if (!memcmp(img, PNG, 8))
		return png_decode(img, len);
	return dib_decode(img, len);
}

/* A string from the version information: the UTF-16 key, then its value, 32-bit aligned. */
static void version_string(const Rsrc *r, const char *key, char *out, usize cap)
{
	out[0] = 0;
	u32 n;
	const u8 *v = resource(r, RT_VERSION, -1, &n);
	if (!v)
		return;
	usize kl = strlen(key);
	for (u32 i = 0; i + (kl + 1) * 2 <= n; i += 2) {
		bool match = true;
		for (usize k = 0; k <= kl && match; k++)
			match = v[i + k * 2] == (u8)(k < kl ? key[k] : 0) && v[i + k * 2 + 1] == 0;
		if (!match)
			continue;
		u32 at = (i + (u32)(kl + 1) * 2 + 3) & ~3u;
		StrBuf sb = { 0 };
		for (; at + 1 < n && sb.len + 4 < cap; at += 2) {
			u32 ch = rd16(v + at);
			if (!ch)
				break;
			/* no trademark signs, and nothing outside the basic plane */
			if ((ch >= 0xd800 && ch <= 0xdfff) || ch == 0x2122 || ch == 0xae || ch == 0xa9 || ch < 0x20)
				continue;
			sb_addcp(&sb, ch);
		}
		if (sb.data) {
			str_copy(out, str_trim(sb.data), cap);
			sb_free(&sb);
		}
		return;
	}
}

bool pe_read(const char *path, PeInfo *out)
{
	memset(out, 0, sizeof *out);
	int fd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return false;
	u8 h[4096];
	long got = sys_pread(fd, h, sizeof h, 0);
	bool ok = false;
	u8 *sect = NULL;
	if (got < 64 || h[0] != 'M' || h[1] != 'Z')
		goto done;
	u32 pe = rd32(h + 0x3c);
	if (pe > (u32)got - 24 || memcmp(h + pe, "PE\0\0", 4))
		goto done;
	u32 nsect = rd16(h + pe + 6), optsize = rd16(h + pe + 20);
	u32 opt = pe + 24;
	if (opt + optsize > (u32)got || optsize < 2)
		goto done;
	u16 magic = rd16(h + opt);
	u32 dirs = magic == 0x20b ? 112 : 96, ndirs_at = magic == 0x20b ? 108 : 92;
	if (optsize < dirs + 3 * 8 || rd32(h + opt + ndirs_at) < 3)
		goto done;
	u32 rsrc = rd32(h + opt + dirs + 2 * 8);
	ok = true; /* a Windows program, with or without resources */
	if (!rsrc)
		goto done;
	u32 table = opt + optsize;
	for (u32 i = 0; i < nsect && i < 96; i++) {
		u32 s = table + i * 40;
		if (s + 40 > (u32)got)
			break;
		u32 va = rd32(h + s + 12), vsize = rd32(h + s + 8), raw = rd32(h + s + 16), at = rd32(h + s + 20);
		if (rsrc < va || rsrc >= va + MAX(vsize, raw))
			continue;
		raw = MIN(raw, MAX_SECTION);
		sect = mem_alloc(raw ? raw : 1);
		usize have = 0;
		while (have < raw) {
			long k = sys_pread(fd, sect + have, raw - have, (i64)at + (i64)have);
			if (k <= 0)
				break;
			have += (usize)k;
		}
		Rsrc r = { .d = sect, .n = have, .rva = va, .dir = rsrc - va };
		version_string(&r, "ProductName", out->product, sizeof out->product);
		out->icon = main_icon(&r);
		break;
	}
done:
	mem_free(sect);
	sys_close(fd);
	return ok;
}

void pe_info_free(PeInfo *info)
{
	if (info->icon)
		canvas_free(info->icon);
	info->icon = NULL;
}
