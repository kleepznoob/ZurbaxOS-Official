/*
 * libza — audio files: WAV, FLAC and Ogg Vorbis decoding, and their tags
 * (ID3, Vorbis comments) with embedded cover pictures.
 */
#pragma once

#include "zr.h"

typedef struct {
	int rate, channels;
	u64 frames;              /* total length, 0 if unknown */
	f64 duration;            /* seconds */
	int bitrate;             /* kbit/s, if known */
	const char *codec;       /* "WAV", "FLAC", "Vorbis" */
	char title[160], artist[160], album[160], genre[64];
	int track, year;
	u8 *cover;               /* embedded picture (JPEG or PNG bytes), owned by the decoder */
	usize cover_len;
} AudioInfo;

typedef struct Decoder Decoder;

/* Opens a file by looking at its contents. NULL if it isn't audio we know. */
Decoder *decoder_open(const char *path, AudioInfo *info);
/* Decodes up to `frames` interleaved 16-bit frames; 0 at the end. */
usize decoder_read(Decoder *d, i16 *out, usize frames);
bool decoder_seek(Decoder *d, u64 frame);
void decoder_close(Decoder *d);

/* Reads the tags (and length) only, which is quick enough for a whole library. */
bool audio_probe(const char *path, AudioInfo *info, bool want_cover);
void audio_info_free(AudioInfo *info); /* frees the cover from audio_probe */
