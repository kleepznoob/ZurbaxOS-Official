/* Shared inside libza. */
#pragma once

#include "za.h"

enum { KIND_WAV = 1, KIND_FLAC, KIND_VORBIS };

struct Decoder {
	int kind;
	const u8 *data;          /* the whole file, mapped */
	usize len;
	usize pos;               /* where decoding continues */
	usize audio_start, audio_end;
	int rate, channels;
	u64 frame;               /* next frame to be returned */
	u64 total;
	/* decoded but not yet returned */
	i16 *pcm;
	usize pcm_frames, pcm_off, pcm_cap;
	void *codec;             /* codec state */
	u8 *cover;
	/* WAV */
	int wav_format, wav_bits, wav_block;
};

/* Big- and little-endian reads. */
static inline u32 rd_be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static inline u32 rd_be24(const u8 *p)
{
	return (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
}
static inline u16 rd_be16(const u8 *p)
{
	return (u16)(p[0] << 8 | p[1]);
}
static inline u32 rd_le32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static inline u16 rd_le16(const u8 *p)
{
	return (u16)(p[0] | p[1] << 8);
}

/* Ensures room for `frames` more decoded frames. */
i16 *pcm_reserve(Decoder *d, usize frames);

/* Tags */
usize id3v2_parse(const u8 *p, usize len, AudioInfo *info, bool want_cover); /* returns its size, 0 if none */
void id3v1_parse(const u8 *p, usize len, AudioInfo *info);
void vorbis_comments(const u8 *p, usize len, AudioInfo *info);

/* Codecs */
bool flac_open(Decoder *d, AudioInfo *info, bool want_cover);
bool flac_decode_frame(Decoder *d);
bool flac_seek(Decoder *d, u64 frame);
void flac_free(Decoder *d);
bool vorbis_open(Decoder *d, AudioInfo *info, bool want_cover);
bool vorbis_decode_packet(Decoder *d);
bool vorbis_seek(Decoder *d, u64 frame);
void vorbis_free(Decoder *d);
