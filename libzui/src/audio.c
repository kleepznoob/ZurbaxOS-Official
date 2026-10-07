/* The zaudio client: streams for apps, and interface sounds synthesized on
 * the spot instead of read from files. */
#include "zui.h"

struct AudioStream {
	int fd;
	int rate, channels;
	u64 written, played;
};

static const char *audio_socket(void)
{
	const char *env = env_get("ZURBAX_AUDIO_SOCKET");
	return env ? env : AUDIO_SOCKET;
}

AudioStream *audio_open(const char *name, int rate, int channels)
{
	int fd = unix_connect(audio_socket(), SOCK_SEQPACKET);
	if (fd < 0)
		return NULL;
	u8 hello[32] = { AUDIO_HELLO, (u8)channels };
	u32 r = (u32)rate;
	memcpy(hello + 4, &r, 4);
	str_copy((char *)hello + 8, name ? name : "", 24);
	if (sys_sendto(fd, hello, sizeof hello, MSG_NOSIGNAL, NULL, 0) < 0) {
		sys_close(fd);
		return NULL;
	}
	AudioStream *s = NEW(AudioStream);
	s->fd = fd;
	s->rate = rate;
	s->channels = channels;
	return s;
}

static void read_reports(AudioStream *s)
{
	u8 msg[16];
	for (;;) {
		long n = sys_recvfrom(s->fd, msg, sizeof msg, MSG_DONTWAIT, NULL, NULL);
		if (n < 16)
			break;
		if (msg[0] == AUDIO_PLAYED)
			memcpy(&s->played, msg + 8, 8);
	}
}

static bool send_frames(AudioStream *s, const i16 *samples, usize frames, int flags)
{
	static u8 buf[4 + 16384];
	buf[0] = AUDIO_DATA;
	usize per = (sizeof buf - 4) / (2 * (usize)s->channels);
	while (frames) {
		usize k = MIN(frames, per), bytes = k * 2 * (usize)s->channels;
		memcpy(buf + 4, samples, bytes);
		long r = sys_sendto(s->fd, buf, 4 + bytes, MSG_NOSIGNAL | flags, NULL, 0);
		if (r == -EAGAIN)
			return true; /* full: the rest is dropped */
		if (r < 0)
			return false;
		s->written += k;
		samples += k * (usize)s->channels;
		frames -= k;
		read_reports(s);
	}
	return true;
}

bool audio_write(AudioStream *s, const i16 *samples, usize frames)
{
	return s && send_frames(s, samples, frames, 0);
}

bool audio_write_nowait(AudioStream *s, const i16 *samples, usize frames)
{
	return s && send_frames(s, samples, frames, MSG_DONTWAIT);
}

void audio_flush(AudioStream *s)
{
	if (!s)
		return;
	u8 m = AUDIO_FLUSH;
	sys_sendto(s->fd, &m, 1, MSG_NOSIGNAL, NULL, 0);
	read_reports(s);
	s->written = s->played;
}

void audio_set_volume(AudioStream *s, f32 volume)
{
	if (!s)
		return;
	u8 m[4] = { AUDIO_VOLUME };
	u16 v = (u16)(clampf(volume, 0, 1) * 1000);
	memcpy(m + 2, &v, 2);
	sys_sendto(s->fd, m, 4, MSG_NOSIGNAL, NULL, 0);
}

u64 audio_played(AudioStream *s)
{
	if (!s)
		return 0;
	read_reports(s);
	return s->played;
}

u64 audio_written(AudioStream *s)
{
	return s ? s->written : 0;
}

int audio_fd(AudioStream *s)
{
	return s ? s->fd : -1;
}

void audio_close(AudioStream *s)
{
	if (!s)
		return;
	sys_close(s->fd);
	mem_free(s);
}

/* ---- Interface sounds ---------------------------------------------------------------------------- */

#define FX_RATE 48000

/* A bell-like note: a sine with a few soft overtones and an exponential fade. */
static void bell(f32 *buf, usize n, f32 at, f32 freq, f32 amp, f32 decay)
{
	usize start = (usize)(at * FX_RATE);
	for (usize i = start; i < n; i++) {
		f32 t = (f32)(i - start) / FX_RATE;
		f32 env = expf(-t * decay) * minf(1, t * 400); /* 2.5 ms attack: no click */
		f32 w = 2 * (f32)PI * freq * t;
		buf[i] += amp * env * (sinf(w) + 0.32f * sinf(2 * w) * expf(-t * decay * 1.6f) + 0.12f * sinf(3.01f * w) * expf(-t * decay * 2.4f));
	}
}

static void sweep(f32 *buf, usize n, f32 at, f32 dur, f32 f0, f32 f1, f32 amp, bool square)
{
	usize start = (usize)(at * FX_RATE), len = (usize)(dur * FX_RATE);
	f32 phase = 0;
	for (usize i = 0; i < len && start + i < n; i++) {
		f32 k = (f32)i / (f32)len;
		f32 f = f0 + (f1 - f0) * k;
		phase += 2 * (f32)PI * f / FX_RATE;
		f32 env = minf(1, (f32)i / 96) * (1 - k);
		f32 v = sinf(phase);
		if (square)
			v = clampf(v * 3, -1, 1) * 0.7f;
		buf[start + i] += amp * env * v;
	}
}

static void noise(f32 *buf, usize n, f32 at, f32 dur, f32 amp, f32 smooth)
{
	usize start = (usize)(at * FX_RATE), len = (usize)(dur * FX_RATE);
	f32 y = 0;
	u32 seed = 0x9e3779b9u;
	for (usize i = 0; i < len && start + i < n; i++) {
		seed = seed * 1664525u + 1013904223u;
		f32 x = (f32)(seed >> 8) / 8388608.0f - 1;
		y += (x - y) * smooth; /* one-pole low-pass */
		f32 k = (f32)i / (f32)len;
		buf[start + i] += amp * y * (1 - k) * (1 - k);
	}
}

static usize synth(const char *name, f32 *buf, usize cap)
{
	memset(buf, 0, cap * sizeof *buf);
	f32 len = 0;
	if (str_eq(name, "click")) {
		noise(buf, cap, 0, 0.012f, 0.5f, 0.6f);
		bell(buf, cap, 0, 2200, 0.12f, 120);
		len = 0.05f;
	} else if (str_eq(name, "tick")) {
		bell(buf, cap, 0, 1760, 0.08f, 160);
		len = 0.03f;
	} else if (str_eq(name, "pop")) {
		sweep(buf, cap, 0, 0.07f, 720, 320, 0.5f, false);
		len = 0.08f;
	} else if (str_eq(name, "notify")) {
		bell(buf, cap, 0, 1318.5f, 0.28f, 7);
		bell(buf, cap, 0.13f, 1760, 0.24f, 6);
		len = 0.9f;
	} else if (str_eq(name, "error")) {
		bell(buf, cap, 0, 466, 0.3f, 9);
		bell(buf, cap, 0.12f, 349, 0.3f, 8);
		len = 0.6f;
	} else if (str_eq(name, "complete")) {
		bell(buf, cap, 0, 1046.5f, 0.22f, 6);
		bell(buf, cap, 0.09f, 1318.5f, 0.22f, 6);
		bell(buf, cap, 0.18f, 1568, 0.24f, 5);
		len = 1.0f;
	} else if (str_eq(name, "camera")) {
		noise(buf, cap, 0, 0.03f, 0.6f, 0.5f);
		noise(buf, cap, 0.07f, 0.05f, 0.5f, 0.35f);
		len = 0.14f;
	} else if (str_eq(name, "coin")) {
		sweep(buf, cap, 0, 0.07f, 988, 988, 0.22f, true);
		sweep(buf, cap, 0.07f, 0.3f, 1319, 1319, 0.22f, true);
		len = 0.38f;
	} else if (str_eq(name, "hit")) {
		noise(buf, cap, 0, 0.09f, 0.7f, 0.15f);
		sweep(buf, cap, 0, 0.08f, 180, 60, 0.4f, false);
		len = 0.1f;
	} else if (str_eq(name, "lose")) {
		sweep(buf, cap, 0, 0.5f, 520, 130, 0.3f, true);
		len = 0.5f;
	} else if (str_eq(name, "win")) {
		static const f32 notes[] = { 784, 988, 1175, 1568 };
		for (int i = 0; i < 4; i++)
			bell(buf, cap, 0.08f * (f32)i, notes[i], 0.2f, 5);
		len = 1.1f;
	}
	return MIN(cap, (usize)(len * FX_RATE));
}

void sound_play(const char *name)
{
	static AudioStream *pool[4];
	static f32 buf[FX_RATE * 2];
	static i16 pcm[FX_RATE * 2];
	usize n = synth(name, buf, ARRAY_LEN(buf));
	if (!n)
		return;
	/* An idle stream from the pool, so overlapping sounds mix. */
	AudioStream *s = NULL;
	for (int i = 0; i < 4 && !s; i++) {
		if (!pool[i])
			pool[i] = audio_open("sounds", FX_RATE, 1);
		if (pool[i] && audio_played(pool[i]) >= audio_written(pool[i]))
			s = pool[i];
	}
	if (!s)
		s = pool[0];
	if (!s)
		return;
	for (usize i = 0; i < n; i++)
		pcm[i] = (i16)(clampf(buf[i], -1, 1) * 30000);
	if (!audio_write_nowait(s, pcm, n)) {
		/* The service restarted: reconnect next time. */
		for (int i = 0; i < 4; i++)
			if (pool[i] == s)
				pool[i] = NULL;
		audio_close(s);
	}
}
