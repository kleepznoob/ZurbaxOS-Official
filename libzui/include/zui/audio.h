/* Sound: the zaudio protocol and the client side. */
#pragma once

#include "zr.h"

#define AUDIO_SOCKET "/run/zurbax/audio.sock"
#define AUDIO_RATE 48000

/* Messages are SEQPACKET datagrams whose first byte is the type. */
enum {
	AUDIO_HELLO = 'H',   /* u8 type, u8 channels, u16 pad, u32 rate, char name[24] */
	AUDIO_DATA = 'D',    /* u8 type, then interleaved signed 16-bit samples (after 3 pad bytes) */
	AUDIO_FLUSH = 'F',   /* drop what is buffered */
	AUDIO_VOLUME = 'V',  /* u8 type, u8 pad, u16 volume 0..1000 */
	AUDIO_PLAYED = 'P',  /* server -> client: u8 type, 7 pad, u64 frames played */
};

typedef struct AudioStream AudioStream;

/* Opens a stream at `rate` Hz with 1 or 2 channels; NULL if there's no sound service. */
AudioStream *audio_open(const char *name, int rate, int channels);
/* Queues frames; blocks while the server's buffer for this stream is full. */
bool audio_write(AudioStream *s, const i16 *samples, usize frames);
/* Like audio_write, but drops what doesn't fit instead of waiting. */
bool audio_write_nowait(AudioStream *s, const i16 *samples, usize frames);
void audio_flush(AudioStream *s);
void audio_set_volume(AudioStream *s, f32 volume);
/* Frames the speakers have actually played (updated as the server reports it). */
u64 audio_played(AudioStream *s);
u64 audio_written(AudioStream *s);
int audio_fd(AudioStream *s);
void audio_close(AudioStream *s);

/* Short interface sounds, synthesized on the spot: "click", "notify",
 * "error", "complete", "camera", "pop", "coin", "hit", "lose", "win". */
void sound_play(const char *name);
