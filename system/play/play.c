/*
 * play — plays sound files from the command line.
 *
 *   play FILE...            plays WAV, FLAC and Ogg Vorbis files
 *   play --sound NAME       plays an interface sound (notify, complete...)
 *   play --info FILE        shows the tags
 *   play --raw FILE         writes the decoded 16-bit samples to stdout
 */
#include "zui.h"
#include "za.h"

static int raw(const char *path)
{
	AudioInfo info;
	Decoder *d = decoder_open(path, &info);
	if (!d) {
		eprint("play: %s isn't a sound file ZOS can play\n", path);
		return 1;
	}
	static i16 buf[4096 * 8];
	usize n;
	while ((n = decoder_read(d, buf, 4096)) > 0)
		write_all(1, buf, n * (usize)info.channels * sizeof(i16));
	decoder_close(d);
	return 0;
}

static int show_info(const char *path)
{
	AudioInfo info;
	if (!audio_probe(path, &info, true)) {
		eprint("play: %s isn't a sound file ZOS can play\n", path);
		return 1;
	}
	print("Title:    %s\nArtist:   %s\nAlbum:    %s\nTrack:    %d\nYear:     %d\nGenre:    %s\n", info.title, info.artist, info.album, info.track, info.year,
	      info.genre);
	print("Format:   %s, %d Hz, %d channel%s, %d kbit/s\nLength:   %d:%02d\nCover:    %s\n", info.codec, info.rate, info.channels,
	      info.channels == 1 ? "" : "s", info.bitrate, (int)info.duration / 60, (int)info.duration % 60, info.cover ? "yes" : "no");
	audio_info_free(&info);
	return 0;
}

static int play_file(const char *path)
{
	AudioInfo info;
	Decoder *d = decoder_open(path, &info);
	if (!d) {
		eprint("play: %s isn't a sound file ZOS can play\n", path);
		return 1;
	}
	int ch = MIN(info.channels, 2);
	AudioStream *s = audio_open("play", info.rate, ch);
	if (!s) {
		eprint("play: the sound service isn't running\n");
		decoder_close(d);
		return 1;
	}
	print("%s%s%s (%d:%02d)\n", info.artist, info.artist[0] ? " — " : "", info.title, (int)info.duration / 60, (int)info.duration % 60);
	static i16 buf[4096 * 8], out[4096 * 2];
	usize n;
	while ((n = decoder_read(d, buf, 4096)) > 0) {
		/* More than two channels: keep the front pair. */
		const i16 *src = buf;
		if (info.channels > 2) {
			for (usize i = 0; i < n; i++)
				out[i * 2] = buf[i * (usize)info.channels], out[i * 2 + 1] = buf[i * (usize)info.channels + 1];
			src = out;
		}
		if (!audio_write(s, src, n))
			break;
	}
	/* Wait for the end to actually be heard. */
	for (int i = 0; i < 400 && audio_played(s) < audio_written(s); i++)
		sleep_ms(10);
	audio_close(s);
	decoder_close(d);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && str_eq(argv[1], "--sound")) {
		sound_play(argv[2]);
		sleep_ms(1300);
		return 0;
	}
	if (argc == 3 && str_eq(argv[1], "--raw"))
		return raw(argv[2]);
	if (argc == 3 && str_eq(argv[1], "--info"))
		return show_info(argv[2]);
	if (argc < 2) {
		eprint("usage: play FILE... | --sound NAME | --info FILE | --raw FILE\n");
		return 2;
	}
	int rc = 0;
	for (int i = 1; i < argc; i++)
		rc |= play_file(argv[i]);
	return rc;
}
