/* Opening audio files, WAV decoding and tags (ID3v2, ID3v1, Vorbis comments). */
#include "internal.h"

i16 *pcm_reserve(Decoder *d, usize frames)
{
	usize need = (d->pcm_frames + frames) * (usize)d->channels;
	if (need > d->pcm_cap) {
		d->pcm_cap = MAX(need, d->pcm_cap * 2);
		d->pcm = mem_realloc(d->pcm, d->pcm_cap * sizeof(i16));
	}
	return d->pcm + d->pcm_frames * (usize)d->channels;
}

/* ---- Text ------------------------------------------------------------------------------------ */

/* Converts tag text in the given ID3 encoding to UTF-8. */
static void tag_text(int enc, const u8 *p, usize n, char *out, usize cap)
{
	usize o = 0;
	if (enc == 0) { /* ISO-8859-1 */
		for (usize i = 0; i < n && p[i] && o + 3 < cap; i++) {
			char u[4];
			usize k = utf8_encode(p[i], u);
			memcpy(out + o, u, k);
			o += k;
		}
	} else if (enc == 1 || enc == 2) { /* UTF-16 with BOM, or big-endian */
		bool be = enc == 2;
		usize i = 0;
		if (n >= 2 && enc == 1) {
			if (p[0] == 0xfe && p[1] == 0xff)
				be = true, i = 2;
			else if (p[0] == 0xff && p[1] == 0xfe)
				i = 2;
		}
		for (; i + 1 < n && o + 5 < cap; i += 2) {
			u32 c = be ? (u32)(p[i] << 8 | p[i + 1]) : (u32)(p[i] | p[i + 1] << 8);
			if (!c)
				break;
			if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
				u32 lo = be ? (u32)(p[i + 2] << 8 | p[i + 3]) : (u32)(p[i + 2] | p[i + 3] << 8);
				c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
				i += 2;
			}
			char u[4];
			usize k = utf8_encode(c, u);
			memcpy(out + o, u, k);
			o += k;
		}
	} else { /* UTF-8 */
		for (usize i = 0; i < n && p[i] && o + 1 < cap; i++)
			out[o++] = (char)p[i];
	}
	out[o] = 0;
	str_copy(out, str_trim(out), cap);
}

static u32 syncsafe(const u8 *p)
{
	return (u32)(p[0] & 0x7f) << 21 | (u32)(p[1] & 0x7f) << 14 | (u32)(p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

/* Length of a string in the given encoding, including its terminator. */
static usize text_len(int enc, const u8 *p, usize n)
{
	usize i = 0;
	if (enc == 1 || enc == 2) {
		while (i + 1 < n && (p[i] || p[i + 1]))
			i += 2;
		return MIN(n, i + 2);
	}
	while (i < n && p[i])
		i++;
	return MIN(n, i + 1);
}

static void set_tag(AudioInfo *info, const char *id, int enc, const u8 *p, usize n)
{
	char text[256];
	tag_text(enc, p, n, text, sizeof text);
	if (str_eq(id, "TIT2") || str_eq(id, "TT2"))
		str_copy(info->title, text, sizeof info->title);
	else if (str_eq(id, "TPE1") || str_eq(id, "TP1"))
		str_copy(info->artist, text, sizeof info->artist);
	else if ((str_eq(id, "TPE2") || str_eq(id, "TP2")) && !info->artist[0])
		str_copy(info->artist, text, sizeof info->artist);
	else if (str_eq(id, "TALB") || str_eq(id, "TAL"))
		str_copy(info->album, text, sizeof info->album);
	else if (str_eq(id, "TRCK") || str_eq(id, "TRK"))
		info->track = (int)str_to_int(text, NULL);
	else if (str_eq(id, "TYER") || str_eq(id, "TDRC") || str_eq(id, "TYE"))
		info->year = (int)str_to_int(text, NULL);
	else if (str_eq(id, "TCON") || str_eq(id, "TCO")) {
		/* "(17)Rock" or "Rock" */
		const char *g = text;
		if (g[0] == '(') {
			const char *e = str_chr(g, ')');
			if (e && e[1])
				g = e + 1;
		}
		str_copy(info->genre, g, sizeof info->genre);
	}
}

usize id3v2_parse(const u8 *p, usize len, AudioInfo *info, bool want_cover)
{
	if (len < 10 || memcmp(p, "ID3", 3) != 0)
		return 0;
	int ver = p[3];
	u32 size = syncsafe(p + 6) + 10;
	if (p[5] & 0x10)
		size += 10; /* footer */
	if (size > len)
		return 0;
	usize off = 10;
	if (p[5] & 0x40 && ver >= 3) /* extended header */
		off += ver == 4 ? syncsafe(p + 10) : rd_be32(p + 10) + 4;
	int hdr = ver == 2 ? 6 : 10;
	while (off + (usize)hdr <= size) {
		char id[5] = { 0 };
		memcpy(id, p + off, ver == 2 ? 3 : 4);
		if (!id[0])
			break;
		u32 fsize = ver == 2 ? rd_be24(p + off + 3) : ver == 4 ? syncsafe(p + off + 4) : rd_be32(p + off + 4);
		const u8 *f = p + off + hdr;
		if (off + (usize)hdr + fsize > size)
			break;
		if (fsize > 1 && (id[0] == 'T')) {
			set_tag(info, id, f[0], f + 1, fsize - 1);
		} else if (want_cover && !info->cover && fsize > 16 && (str_eq(id, "APIC") || str_eq(id, "PIC"))) {
			int enc = f[0];
			usize q = 1;
			if (ver == 2)
				q += 3; /* image format */
			else
				q += text_len(0, f + q, fsize - q); /* MIME type */
			q += 1; /* picture type */
			q += text_len(enc, f + q, fsize - q);
			if (q < fsize) {
				info->cover = mem_dup(f + q, fsize - q);
				info->cover_len = fsize - q;
			}
		}
		off += (usize)hdr + fsize;
	}
	return size;
}

void id3v1_parse(const u8 *p, usize len, AudioInfo *info)
{
	if (len < 128)
		return;
	const u8 *t = p + len - 128;
	if (memcmp(t, "TAG", 3) != 0)
		return;
	if (!info->title[0])
		tag_text(0, t + 3, 30, info->title, sizeof info->title);
	if (!info->artist[0])
		tag_text(0, t + 33, 30, info->artist, sizeof info->artist);
	if (!info->album[0])
		tag_text(0, t + 63, 30, info->album, sizeof info->album);
	if (!info->year) {
		char y[8];
		tag_text(0, t + 93, 4, y, sizeof y);
		info->year = (int)str_to_int(y, NULL);
	}
	if (!info->track && t[125] == 0 && t[126])
		info->track = t[126];
}

void vorbis_comments(const u8 *p, usize len, AudioInfo *info)
{
	if (len < 8)
		return;
	usize off = 4 + rd_le32(p);
	if (off + 4 > len)
		return;
	u32 n = rd_le32(p + off);
	off += 4;
	for (u32 i = 0; i < n && off + 4 <= len; i++) {
		u32 l = rd_le32(p + off);
		off += 4;
		if (off + l > len)
			break;
		const char *s = (const char *)p + off;
		const char *eq = memchr(s, '=', l);
		if (eq) {
			usize kl = (usize)(eq - s), vl = l - kl - 1;
			char val[256];
			str_copy(val, eq + 1, MIN(sizeof val, vl + 1));
			if (kl == 5 && str_eq_nocase_n(s, "TITLE", 5))
				str_copy(info->title, val, sizeof info->title);
			else if (kl == 6 && str_eq_nocase_n(s, "ARTIST", 6))
				str_copy(info->artist, val, sizeof info->artist);
			else if (kl == 11 && str_eq_nocase_n(s, "ALBUMARTIST", 11) && !info->artist[0])
				str_copy(info->artist, val, sizeof info->artist);
			else if (kl == 5 && str_eq_nocase_n(s, "ALBUM", 5))
				str_copy(info->album, val, sizeof info->album);
			else if (kl == 11 && str_eq_nocase_n(s, "TRACKNUMBER", 11))
				info->track = (int)str_to_int(val, NULL);
			else if (kl == 4 && str_eq_nocase_n(s, "DATE", 4))
				info->year = (int)str_to_int(val, NULL);
			else if (kl == 5 && str_eq_nocase_n(s, "GENRE", 5))
				str_copy(info->genre, val, sizeof info->genre);
		}
		off += l;
	}
}

/* ---- WAV ----------------------------------------------------------------------------------------- */

static bool wav_open(Decoder *d, AudioInfo *info)
{
	const u8 *p = d->data;
	usize off = 12;
	bool have_fmt = false;
	while (off + 8 <= d->len) {
		u32 size = rd_le32(p + off + 4);
		const u8 *c = p + off + 8;
		if (memcmp(p + off, "fmt ", 4) == 0 && size >= 16 && off + 8 + size <= d->len) {
			d->wav_format = rd_le16(c);
			d->channels = rd_le16(c + 2);
			d->rate = (int)rd_le32(c + 4);
			d->wav_block = rd_le16(c + 12);
			d->wav_bits = rd_le16(c + 14);
			if (d->wav_format == 0xfffe && size >= 26)
				d->wav_format = rd_le16(c + 24); /* the sub-format's first two bytes */
			have_fmt = true;
		} else if (memcmp(p + off, "LIST", 4) == 0 && size >= 4 && off + 8 + size <= d->len && memcmp(c, "INFO", 4) == 0) {
			/* RIFF INFO tags: INAM title, IART artist, IPRD album, ITRK track, ICRD date, IGNR genre */
			usize q = 4;
			while (q + 8 <= size) {
				u32 l = rd_le32(c + q + 4);
				if (q + 8 + l > size)
					break;
				char v[160];
				str_copy(v, (const char *)c + q + 8, MIN(sizeof v, (usize)l + 1));
				const u8 *id = c + q;
				if (memcmp(id, "INAM", 4) == 0)
					str_copy(info->title, v, sizeof info->title);
				else if (memcmp(id, "IART", 4) == 0)
					str_copy(info->artist, v, sizeof info->artist);
				else if (memcmp(id, "IPRD", 4) == 0)
					str_copy(info->album, v, sizeof info->album);
				else if (memcmp(id, "ITRK", 4) == 0)
					info->track = (int)str_to_int(v, NULL);
				else if (memcmp(id, "ICRD", 4) == 0)
					info->year = (int)str_to_int(v, NULL);
				else if (memcmp(id, "IGNR", 4) == 0)
					str_copy(info->genre, v, sizeof info->genre);
				q += 8 + l + (l & 1);
			}
		} else if (memcmp(p + off, "data", 4) == 0 && have_fmt) {
			d->audio_start = off + 8;
			d->audio_end = MIN(d->len, off + 8 + (usize)size);
			break;
		}
		off += 8 + size + (size & 1);
	}
	if (!have_fmt || !d->audio_start || d->channels < 1 || d->channels > 8 || d->rate <= 0 || d->wav_block <= 0)
		return false;
	if (!(d->wav_format == 1 && (d->wav_bits == 8 || d->wav_bits == 16 || d->wav_bits == 24 || d->wav_bits == 32)) &&
	    !(d->wav_format == 3 && d->wav_bits == 32))
		return false;
	d->pos = d->audio_start;
	d->total = (d->audio_end - d->audio_start) / (usize)d->wav_block;
	info->codec = "WAV";
	info->bitrate = d->rate * d->channels * d->wav_bits / 1000;
	return true;
}

static usize wav_read(Decoder *d, i16 *out, usize frames)
{
	usize n = 0;
	int ch = d->channels, bytes = d->wav_bits / 8;
	while (n < frames && d->pos + (usize)d->wav_block <= d->audio_end) {
		const u8 *s = d->data + d->pos;
		for (int c = 0; c < ch; c++) {
			const u8 *x = s + c * bytes;
			i32 v;
			switch (d->wav_bits) {
			case 8: v = (i32)(x[0] - 128) << 8; break;
			case 16: v = (i16)rd_le16(x); break;
			case 24: v = ((i32)(x[0] << 8 | x[1] << 16 | (u32)x[2] << 24)) >> 16; break;
			default:
				if (d->wav_format == 3) {
					f32 fv;
					memcpy(&fv, x, 4);
					v = (i32)(clampf(fv, -1, 1) * 32767);
				} else {
					v = (i32)rd_le32(x) >> 16;
				}
			}
			out[n * (usize)ch + (usize)c] = (i16)v;
		}
		d->pos += (usize)d->wav_block;
		n++;
	}
	return n;
}

/* ---- Common --------------------------------------------------------------------------------------- */

static bool map_file(const char *path, const u8 **data, usize *len)
{
	int fd = (int)sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return false;
	struct stat st;
	if (sys_fstat(fd, &st) < 0 || st.st_size < 16) {
		sys_close(fd);
		return false;
	}
	void *m = sys_mmap(NULL, (usize)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	sys_close(fd);
	if ((long)m < 0 && (long)m > -4096)
		return false;
	*data = m;
	*len = (usize)st.st_size;
	return true;
}

static Decoder *open_any(const char *path, AudioInfo *info, bool want_cover)
{
	memset(info, 0, sizeof *info);
	Decoder *d = NEW(Decoder);
	if (!map_file(path, &d->data, &d->len)) {
		mem_free(d);
		return NULL;
	}
	bool ok = false;
	const u8 *p = d->data;
	if (memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WAVE", 4) == 0) {
		d->kind = KIND_WAV;
		ok = wav_open(d, info);
	} else {
		usize skip = id3v2_parse(p, d->len, info, want_cover);
		if (skip + 4 <= d->len && memcmp(p + skip, "fLaC", 4) == 0) {
			d->kind = KIND_FLAC;
			d->pos = skip;
			ok = flac_open(d, info, want_cover);
		} else if (memcmp(p, "OggS", 4) == 0) {
			d->kind = KIND_VORBIS;
			ok = vorbis_open(d, info, want_cover);
		}
	}
	if (!ok) {
		mem_free(info->cover);
		info->cover = NULL;
		sys_munmap((void *)d->data, d->len);
		mem_free(d);
		return NULL;
	}
	d->cover = info->cover;
	info->rate = d->rate;
	info->channels = d->channels;
	info->frames = d->total;
	info->duration = d->rate ? (f64)d->total / d->rate : 0;
	if (!info->title[0]) {
		/* The file name without its extension. */
		const char *base = path_base(path);
		str_copy(info->title, base, sizeof info->title);
		char *dot = (char *)str_rchr(info->title, '.');
		if (dot && dot != info->title)
			*dot = 0;
	}
	return d;
}

Decoder *decoder_open(const char *path, AudioInfo *info)
{
	return open_any(path, info, true);
}

usize decoder_read(Decoder *d, i16 *out, usize frames)
{
	if (d->kind == KIND_WAV) {
		usize n = wav_read(d, out, frames);
		d->frame += n;
		return n;
	}
	usize done = 0;
	while (done < frames) {
		if (d->pcm_off >= d->pcm_frames) {
			d->pcm_frames = d->pcm_off = 0;
			bool ok = d->kind == KIND_FLAC ? flac_decode_frame(d) : vorbis_decode_packet(d);
			if (!ok || !d->pcm_frames)
				break;
		}
		usize k = MIN(frames - done, d->pcm_frames - d->pcm_off);
		memcpy(out + done * (usize)d->channels, d->pcm + d->pcm_off * (usize)d->channels, k * (usize)d->channels * sizeof(i16));
		d->pcm_off += k;
		done += k;
	}
	d->frame += done;
	return done;
}

bool decoder_seek(Decoder *d, u64 frame)
{
	if (d->total && frame > d->total)
		frame = d->total;
	d->pcm_frames = d->pcm_off = 0;
	bool ok;
	if (d->kind == KIND_WAV) {
		d->pos = d->audio_start + (usize)frame * (usize)d->wav_block;
		d->frame = frame;
		ok = true;
	} else {
		ok = d->kind == KIND_FLAC ? flac_seek(d, frame) : vorbis_seek(d, frame);
	}
	return ok;
}

void decoder_close(Decoder *d)
{
	if (!d)
		return;
	if (d->kind == KIND_FLAC)
		flac_free(d);
	else if (d->kind == KIND_VORBIS)
		vorbis_free(d);
	sys_munmap((void *)d->data, d->len);
	mem_free(d->pcm);
	mem_free(d->cover);
	mem_free(d);
}

bool audio_probe(const char *path, AudioInfo *info, bool want_cover)
{
	Decoder *d = open_any(path, info, want_cover);
	if (!d)
		return false;
	d->cover = NULL; /* the caller owns it now */
	decoder_close(d);
	return true;
}

void audio_info_free(AudioInfo *info)
{
	mem_free(info->cover);
	info->cover = NULL;
	info->cover_len = 0;
}
