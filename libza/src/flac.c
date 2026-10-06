/* FLAC decoding: fixed and LPC predictors, Rice-coded residuals, stereo
 * decorrelation, any sample size, seeking through the seek table. */
#include "internal.h"

typedef struct {
	int bps;                 /* bits per sample from STREAMINFO */
	int max_block;
	u64 *seek_sample, *seek_offset;
	int nseek;
	i32 *buf[8];             /* one block per channel */
	int buf_cap;
} Flac;

/* ---- Bits --------------------------------------------------------------------------------------- */

typedef struct {
	const u8 *p, *end;
	u64 cache;
	int bits;
	bool bad;
} Bits;

static void bits_init(Bits *b, const u8 *p, const u8 *end)
{
	b->p = p, b->end = end, b->cache = 0, b->bits = 0, b->bad = false;
}

static void refill(Bits *b)
{
	while (b->bits <= 56) {
		u8 byte = 0;
		if (b->p < b->end)
			byte = *b->p++;
		else
			b->bad = b->bits == 0 ? true : b->bad;
		b->cache |= (u64)byte << (56 - b->bits);
		b->bits += 8;
	}
}

static u32 get(Bits *b, int n)
{
	if (n == 0)
		return 0;
	if (b->bits < n)
		refill(b);
	u32 v = (u32)(b->cache >> (64 - n));
	b->cache <<= n;
	b->bits -= n;
	return v;
}

static i32 get_signed(Bits *b, int n)
{
	if (n == 0)
		return 0;
	u32 v = get(b, n);
	return (i32)(v << (32 - n)) >> (32 - n);
}

static u32 unary(Bits *b)
{
	u32 n = 0;
	for (;;) {
		if (b->bits == 0)
			refill(b);
		if (b->cache == 0) {
			n += (u32)b->bits;
			b->bits = 0;
			if (b->p >= b->end) {
				b->bad = true;
				return n;
			}
			continue;
		}
		int z = __builtin_clzll(b->cache);
		if (z < b->bits) {
			n += (u32)z;
			b->cache <<= z + 1;
			b->bits -= z + 1;
			return n;
		}
		n += (u32)b->bits;
		b->cache = 0;
		b->bits = 0;
	}
}

/* Position of the next unread byte (after aligning). */
static const u8 *bits_byte_pos(Bits *b)
{
	int drop = b->bits % 8;
	b->cache <<= drop;
	b->bits -= drop;
	return b->p - b->bits / 8;
}

/* ---- Metadata ----------------------------------------------------------------------------------- */

bool flac_open(Decoder *d, AudioInfo *info, bool want_cover)
{
	const u8 *p = d->data;
	usize off = d->pos + 4;
	Flac *f = NEW(Flac);
	d->codec = f;
	bool have_info = false, last = false;
	while (!last && off + 4 <= d->len) {
		last = p[off] & 0x80;
		int type = p[off] & 0x7f;
		u32 len = rd_be24(p + off + 1);
		const u8 *b = p + off + 4;
		if (off + 4 + len > d->len)
			break;
		if (type == 0 && len >= 34) {
			f->max_block = rd_be16(b + 2);
			d->rate = (int)(rd_be32(b + 10) >> 12);
			d->channels = ((b[12] >> 1) & 7) + 1;
			f->bps = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
			d->total = ((u64)(b[13] & 15) << 32) | rd_be32(b + 14);
			have_info = true;
		} else if (type == 3) {
			int n = (int)(len / 18);
			f->seek_sample = NEW_ARRAY(u64, n);
			f->seek_offset = NEW_ARRAY(u64, n);
			for (int i = 0; i < n; i++) {
				u64 s = (u64)rd_be32(b + i * 18) << 32 | rd_be32(b + i * 18 + 4);
				if (s == 0xffffffffffffffffull)
					continue; /* placeholder */
				f->seek_sample[f->nseek] = s;
				f->seek_offset[f->nseek] = (u64)rd_be32(b + i * 18 + 8) << 32 | rd_be32(b + i * 18 + 12);
				f->nseek++;
			}
		} else if (type == 4) {
			vorbis_comments(b, len, info);
		} else if (type == 6 && want_cover && !info->cover && len > 32) {
			u32 q = 4;
			u32 ml = rd_be32(b + q);
			q += 4 + ml;
			if (q + 4 <= len) {
				u32 dl = rd_be32(b + q);
				q += 4 + dl + 16;
				if (q + 4 <= len) {
					u32 il = rd_be32(b + q);
					q += 4;
					if (q + il <= len) {
						info->cover = mem_dup(b + q, il);
						info->cover_len = il;
					}
				}
			}
		}
		off += 4 + len;
	}
	if (!have_info || d->rate <= 0 || f->bps < 4 || f->bps > 32) {
		flac_free(d);
		return false;
	}
	d->audio_start = d->pos = off;
	d->audio_end = d->len;
	info->codec = "FLAC";
	if (d->total && d->rate)
		info->bitrate = (int)((f64)(d->len - off) * 8 / ((f64)d->total / d->rate) / 1000);
	return true;
}

void flac_free(Decoder *d)
{
	Flac *f = d->codec;
	if (!f)
		return;
	for (int i = 0; i < 8; i++)
		mem_free(f->buf[i]);
	mem_free(f->seek_sample);
	mem_free(f->seek_offset);
	mem_free(f);
	d->codec = NULL;
}

/* ---- Frames ------------------------------------------------------------------------------------- */

static u8 crc8(const u8 *p, usize n)
{
	u8 c = 0;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (u8)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
	}
	return c;
}

typedef struct {
	int block, rate, channels, assign, bps;
	u64 number;
	bool variable;
	usize header_len;
} FrameHeader;

static bool parse_header(const Decoder *d, const Flac *f, const u8 *p, usize avail, FrameHeader *h)
{
	if (avail < 6 || p[0] != 0xff || (p[1] & 0xfe) != 0xf8)
		return false;
	h->variable = p[1] & 1;
	int bs = p[2] >> 4, sr = p[2] & 15, ch = p[3] >> 4, ss = (p[3] >> 1) & 7;
	if (bs == 0 || sr == 15 || ch > 10 || ss == 3 || (p[3] & 1))
		return false;
	usize i = 4;
	/* The frame or sample number, UTF-8 style. */
	u8 lead = p[i++];
	int extra = 0;
	u64 v;
	if (!(lead & 0x80))
		v = lead;
	else if ((lead & 0xe0) == 0xc0)
		v = lead & 0x1f, extra = 1;
	else if ((lead & 0xf0) == 0xe0)
		v = lead & 0x0f, extra = 2;
	else if ((lead & 0xf8) == 0xf0)
		v = lead & 0x07, extra = 3;
	else if ((lead & 0xfc) == 0xf8)
		v = lead & 0x03, extra = 4;
	else if ((lead & 0xfe) == 0xfc)
		v = lead & 0x01, extra = 5;
	else if (lead == 0xfe)
		v = 0, extra = 6;
	else
		return false;
	if (i + (usize)extra + 4 > avail)
		return false;
	for (int k = 0; k < extra; k++) {
		if ((p[i] & 0xc0) != 0x80)
			return false;
		v = v << 6 | (p[i++] & 0x3f);
	}
	h->number = v;
	if (bs == 1)
		h->block = 192;
	else if (bs <= 5)
		h->block = 576 << (bs - 2);
	else if (bs == 6)
		h->block = p[i++] + 1;
	else if (bs == 7)
		h->block = rd_be16(p + i) + 1, i += 2;
	else
		h->block = 256 << (bs - 8);
	static const int rates[] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
	if (sr == 0)
		h->rate = d->rate;
	else if (sr < 12)
		h->rate = rates[sr];
	else if (sr == 12)
		h->rate = p[i++] * 1000;
	else if (sr == 13)
		h->rate = rd_be16(p + i), i += 2;
	else
		h->rate = rd_be16(p + i) * 10, i += 2;
	static const int sizes[] = { 0, 8, 12, 0, 16, 20, 24, 32 };
	h->bps = ss ? sizes[ss] : f->bps;
	h->assign = ch;
	h->channels = ch < 8 ? ch + 1 : 2;
	if (i >= avail || crc8(p, i) != p[i])
		return false;
	h->header_len = i + 1;
	return h->channels == d->channels && h->block <= 65535;
}

static bool residual(Bits *b, i32 *out, int block, int order)
{
	int method = (int)get(b, 2);
	if (method > 1)
		return false;
	int pbits = method ? 5 : 4, escape = method ? 31 : 15;
	int porder = (int)get(b, 4);
	int parts = 1 << porder;
	if ((block >> porder) < order)
		return false;
	int i = order;
	for (int part = 0; part < parts; part++) {
		int n = (block >> porder) - (part == 0 ? order : 0);
		int k = (int)get(b, pbits);
		if (k == escape) {
			int raw = (int)get(b, 5);
			for (int j = 0; j < n; j++)
				out[i++] = get_signed(b, raw);
		} else {
			for (int j = 0; j < n; j++) {
				u32 q = unary(b);
				u32 v = (q << k) | get(b, k);
				out[i++] = (i32)(v >> 1) ^ -(i32)(v & 1);
			}
		}
		if (b->bad)
			return false;
	}
	return true;
}

static bool subframe(Bits *b, i32 *s, int block, int bps)
{
	if (get(b, 1))
		return false;
	int type = (int)get(b, 6);
	int wasted = 0;
	if (get(b, 1))
		wasted = (int)unary(b) + 1;
	bps -= wasted;
	if (bps <= 0 || bps > 33)
		return false;
	if (type == 0) {
		i32 v = get_signed(b, bps);
		for (int i = 0; i < block; i++)
			s[i] = v;
	} else if (type == 1) {
		for (int i = 0; i < block; i++)
			s[i] = get_signed(b, bps);
	} else if (type >= 8 && type <= 12) {
		int order = type - 8;
		if (order > block)
			return false;
		for (int i = 0; i < order; i++)
			s[i] = get_signed(b, bps);
		if (!residual(b, s, block, order))
			return false;
		switch (order) {
		case 1: for (int i = 1; i < block; i++) s[i] += s[i - 1]; break;
		case 2: for (int i = 2; i < block; i++) s[i] += 2 * s[i - 1] - s[i - 2]; break;
		case 3: for (int i = 3; i < block; i++) s[i] += 3 * s[i - 1] - 3 * s[i - 2] + s[i - 3]; break;
		case 4: for (int i = 4; i < block; i++) s[i] += 4 * s[i - 1] - 6 * s[i - 2] + 4 * s[i - 3] - s[i - 4]; break;
		}
	} else if (type >= 32) {
		int order = type - 31;
		if (order > block)
			return false;
		for (int i = 0; i < order; i++)
			s[i] = get_signed(b, bps);
		int precision = (int)get(b, 4) + 1;
		if (precision == 16)
			return false;
		int shift = get_signed(b, 5);
		if (shift < 0)
			return false;
		i32 coef[32];
		for (int i = 0; i < order; i++)
			coef[i] = get_signed(b, precision);
		if (!residual(b, s, block, order))
			return false;
		for (int i = order; i < block; i++) {
			i64 sum = 0;
			for (int j = 0; j < order; j++)
				sum += (i64)coef[j] * s[i - 1 - j];
			s[i] += (i32)(sum >> shift);
		}
	} else {
		return false;
	}
	if (wasted)
		for (int i = 0; i < block; i++)
			s[i] = (i32)((u32)s[i] << wasted);
	return !b->bad;
}

bool flac_decode_frame(Decoder *d)
{
	Flac *f = d->codec;
	for (;;) {
		/* Find the next frame (normally right here). */
		while (d->pos + 2 <= d->len && !(d->data[d->pos] == 0xff && (d->data[d->pos + 1] & 0xfe) == 0xf8))
			d->pos++;
		if (d->pos + 16 > d->len)
			return false;
		FrameHeader h;
		if (!parse_header(d, f, d->data + d->pos, d->len - d->pos, &h)) {
			d->pos++;
			continue;
		}
		if (h.block > f->buf_cap) {
			f->buf_cap = h.block;
			for (int c = 0; c < 8; c++)
				f->buf[c] = mem_realloc(f->buf[c], sizeof(i32) * (usize)h.block);
		}
		Bits b;
		bits_init(&b, d->data + d->pos + h.header_len, d->data + d->len);
		bool ok = true;
		for (int c = 0; c < h.channels && ok; c++) {
			int bps = h.bps;
			/* The side channel needs one extra bit. */
			if ((h.assign == 8 && c == 1) || (h.assign == 9 && c == 0) || (h.assign == 10 && c == 1))
				bps++;
			ok = subframe(&b, f->buf[c], h.block, bps);
		}
		if (!ok) {
			d->pos++; /* a false sync or damage: look for the next frame */
			continue;
		}
		const u8 *end = bits_byte_pos(&b);
		d->pos = (usize)(end - d->data) + 2; /* CRC-16 */
		i32 *l = f->buf[0], *r = f->buf[1];
		if (h.assign == 8)
			for (int i = 0; i < h.block; i++) r[i] = l[i] - r[i];
		else if (h.assign == 9)
			for (int i = 0; i < h.block; i++) l[i] += r[i];
		else if (h.assign == 10)
			for (int i = 0; i < h.block; i++) {
				i32 mid = (i32)((u32)l[i] << 1) | (r[i] & 1), side = r[i];
				l[i] = (mid + side) >> 1;
				r[i] = (mid - side) >> 1;
			}
		i16 *out = pcm_reserve(d, (usize)h.block);
		int shift = h.bps - 16;
		for (int i = 0; i < h.block; i++)
			for (int c = 0; c < h.channels; c++) {
				i32 v = f->buf[c][i];
				v = shift > 0 ? v >> shift : (i32)((u32)v << -shift);
				out[i * h.channels + c] = (i16)CLAMP(v, -32768, 32767);
			}
		d->pcm_frames += (usize)h.block;
		return true;
	}
}

bool flac_seek(Decoder *d, u64 frame)
{
	Flac *f = d->codec;
	usize start = d->audio_start;
	u64 at = 0;
	for (int i = 0; i < f->nseek; i++)
		if (f->seek_sample[i] <= frame && d->audio_start + f->seek_offset[i] < d->len) {
			start = d->audio_start + (usize)f->seek_offset[i];
			at = f->seek_sample[i];
		}
	if (!f->nseek && d->total) {
		/* No table: jump proportionally a little early, then decode forward. */
		u64 early = frame > (u64)d->rate * 2 ? frame - (u64)d->rate * 2 : 0;
		start = d->audio_start + (usize)((f64)(d->len - d->audio_start) * ((f64)early / (f64)d->total));
		at = (u64)-1;
	}
	d->pos = start;
	d->pcm_frames = d->pcm_off = 0;
	/* Find out where we landed from the next frame header. */
	while (d->pos + 16 < d->len) {
		FrameHeader h;
		if (d->data[d->pos] == 0xff && parse_header(d, f, d->data + d->pos, d->len - d->pos, &h)) {
			at = h.variable ? h.number : h.number * (u64)h.block;
			break;
		}
		d->pos++;
	}
	if (at == (u64)-1)
		return false;
	/* Decode and drop up to the wanted frame. */
	while (at < frame) {
		d->pcm_frames = d->pcm_off = 0;
		if (!flac_decode_frame(d))
			break;
		if (at + d->pcm_frames > frame) {
			d->pcm_off = (usize)(frame - at);
			at = frame;
			break;
		}
		at += d->pcm_frames;
	}
	if (d->pcm_off == 0)
		d->pcm_frames = 0; /* landed on a frame boundary: start with the next frame */
	d->frame = frame;
	return true;
}
