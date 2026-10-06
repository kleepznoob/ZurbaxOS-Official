/* Tests for libza against reference decodes made with ffmpeg (tests/data/za). */
#include "za.h"

static int failures, checks;

#define CHECK(cond)                                                          \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			failures++;                                          \
			eprint("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                            \
	} while (0)

static i16 *decode_all(const char *path, AudioInfo *info, usize *frames)
{
	Decoder *d = decoder_open(path, info);
	if (!d)
		return NULL;
	usize cap = 1 << 16, n = 0;
	i16 *out = mem_alloc(cap * 2 * sizeof(i16));
	for (;;) {
		if (n + 1024 > cap) {
			cap *= 2;
			out = mem_realloc(out, cap * 2 * sizeof(i16));
		}
		usize k = decoder_read(d, out + n * (usize)info->channels, 1024);
		if (!k)
			break;
		n += k;
	}
	decoder_close(d);
	*frames = n;
	return out;
}

/* Signal-to-error ratio in dB between two sample runs. */
static f64 snr(const i16 *a, const i16 *b, usize n)
{
	f64 sig = 0, err = 0;
	for (usize i = 0; i < n; i++) {
		sig += (f64)a[i] * a[i];
		err += ((f64)a[i] - b[i]) * ((f64)a[i] - b[i]);
	}
	return err == 0 ? 999 : 10 * log10(sig / err);
}

static void check_file(const char *name, const char *ref_name, f64 min_snr)
{
	char path[256], ref[256];
	fmt(path, sizeof path, "tests/data/za/%s", name);
	fmt(ref, sizeof ref, "tests/data/za/%s", ref_name);
	AudioInfo info;
	usize frames, rlen;
	i16 *pcm = decode_all(path, &info, &frames);
	i16 *expect = (i16 *)file_read(ref, &rlen);
	CHECK(pcm != NULL && expect != NULL);
	if (!pcm || !expect)
		return;
	usize rframes = rlen / 4;
	CHECK(info.channels == 2 && info.rate == 16000);
	/* The stream's own length; ffmpeg cuts a little off the end of short Vorbis files. */
	CHECK(frames == info.frames && frames >= rframes);
	f64 s = snr(expect, pcm, MIN(frames, rframes) * 2);
	if (s < min_snr)
		eprint("    %s: %.1f dB\n", name, s);
	CHECK(s >= min_snr);
	CHECK(str_eq(info.title, "Chord") && str_eq(info.artist, "Zurbax Tests"));

	/* Seeking lands on the same samples. */
	Decoder *d = decoder_open(path, &info);
	u64 at = 5000;
	CHECK(d && decoder_seek(d, at));
	i16 part[512 * 2];
	usize k = d ? decoder_read(d, part, 512) : 0;
	CHECK(k == 512);
	f64 ps = snr(expect + at * 2, part, 512 * 2);
	if (ps < min_snr - 6)
		eprint("    %s seek: %.1f dB\n", name, ps);
	CHECK(ps >= min_snr - 6);
	decoder_close(d);
	mem_free(pcm);
	mem_free(expect);
}

int main(void)
{
	check_file("chord.wav", "chord.flac.raw", 999);
	check_file("chord.flac", "chord.flac.raw", 999);
	check_file("chord.ogg", "chord.ogg.raw", 50);
	AudioInfo info;
	CHECK(audio_probe("tests/data/za/chord.flac", &info, false));
	CHECK(info.track == 7 && info.year == 2026 && str_eq(info.album, "Fixtures") && str_eq(info.codec, "FLAC"));
	CHECK(info.duration > 0.59 && info.duration < 0.61);
	CHECK(!audio_probe("tests/data/za/../zn/msg.txt", &info, false));
	print("libza: %d/%d checks passed\n", checks - failures, checks);
	return failures ? 1 : 0;
}
