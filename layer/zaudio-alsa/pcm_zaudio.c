/*
 * An ALSA output for the Linux layer that plays through zaudio, Zurbax's sound
 * service, so Steam, Proton (Wine's ALSA driver) and Linux games are heard
 * like Zurbax's own apps, with the same volume control.
 *
 * zaudio owns the sound card; this plugin sends it 16-bit PCM over its socket
 * (/run/zurbax/audio.sock, the protocol in libzui/include/zui/audio.h) and
 * follows the "played" counts zaudio reports every 50 ms to move ALSA's
 * hardware pointer, interpolating in between with the clock. alsa-lib's plug
 * layer in front of it converts other formats and rates (asound.conf):
 *
 *     pcm.!default { type plug slave.pcm "zaudio" }
 *     pcm.zaudio { type zaudio }
 *
 * Unlike the rest of Zurbax this is built against the layer's glibc and
 * alsa-lib, by layer/build.sh, into the layer's alsa-lib plugin folder.
 */
/* a plugin is always a shared object: alsa-lib's symbol macros need to know */
#ifndef PIC
#define PIC
#endif
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCKET_PATH "/run/zurbax/audio.sock"
#define MAX_PACKET 65536

typedef struct {
	snd_pcm_ioplug_t io;
	int fd;
	unsigned int rate, channels, frame_bytes;
	uint64_t written;       /* frames sent since prepare */
	uint64_t played;        /* frames zaudio reported played since prepare */
	uint64_t base;          /* zaudio's count at prepare (its count keeps running) */
	uint64_t zaudio_count;  /* zaudio's latest count */
	double report_time;     /* when the latest count arrived */
	int running;
} Zaudio;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Reads what zaudio sent: the frames played so far. */
static void drain(Zaudio *z)
{
	unsigned char msg[64];
	for (;;) {
		ssize_t n = recv(z->fd, msg, sizeof msg, MSG_DONTWAIT);
		if (n <= 0)
			return;
		if (msg[0] == 'P' && n >= 16) {
			memcpy(&z->zaudio_count, msg + 8, 8);
			z->report_time = now_s();
		}
	}
}

/* Frames gone to the speakers: the last count plus the time since, never
 * ahead of what was sent. */
static uint64_t position(Zaudio *z)
{
	drain(z);
	uint64_t p = z->zaudio_count > z->base ? z->zaudio_count - z->base : 0;
	if (z->running && z->report_time > 0) {
		double extra = (now_s() - z->report_time) * z->rate;
		if (extra > 0 && extra < z->rate * 0.1) /* at most the gap between reports */
			p += (uint64_t)extra;
	}
	if (p > z->written)
		p = z->written;
	if (p > z->played)
		z->played = p;
	return z->played;
}

static int send_all(Zaudio *z, const void *data, size_t len)
{
	while (len) {
		ssize_t n = send(z->fd, data, len, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -errno;
		data = (const char *)data + n;
		len -= (size_t)n;
	}
	return 0;
}

static int zaudio_start(snd_pcm_ioplug_t *io)
{
	Zaudio *z = io->private_data;
	z->running = 1;
	return 0;
}

static int zaudio_stop(snd_pcm_ioplug_t *io)
{
	Zaudio *z = io->private_data;
	z->running = 0;
	unsigned char flush[4] = { 'F' };
	send_all(z, flush, sizeof flush);
	return 0;
}

static snd_pcm_sframes_t zaudio_pointer(snd_pcm_ioplug_t *io)
{
	Zaudio *z = io->private_data;
	if (io->state == SND_PCM_STATE_XRUN)
		return -EPIPE;
	return (snd_pcm_sframes_t)(position(z) % io->buffer_size);
}

static snd_pcm_sframes_t zaudio_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
					 snd_pcm_uframes_t size)
{
	Zaudio *z = io->private_data;
	const char *src = (const char *)areas[0].addr + (areas[0].first + areas[0].step * offset) / 8;
	size_t max_frames = (MAX_PACKET - 4) / z->frame_bytes;
	snd_pcm_uframes_t done = 0;
	while (done < size) {
		size_t frames = size - done < max_frames ? size - done : max_frames;
		unsigned char packet[MAX_PACKET];
		packet[0] = 'D', packet[1] = packet[2] = packet[3] = 0;
		memcpy(packet + 4, src + done * z->frame_bytes, frames * z->frame_bytes);
		int r = send_all(z, packet, 4 + frames * z->frame_bytes);
		if (r < 0)
			return done ? (snd_pcm_sframes_t)done : r;
		done += frames;
	}
	z->written += done;
	return (snd_pcm_sframes_t)done;
}

static int zaudio_prepare(snd_pcm_ioplug_t *io)
{
	Zaudio *z = io->private_data;
	z->rate = io->rate, z->channels = io->channels, z->frame_bytes = io->channels * 2;
	/* a fresh stream: tell zaudio the format and start counting from here */
	unsigned char hello[32] = { 'H', (unsigned char)z->channels };
	memcpy(hello + 4, &z->rate, 4);
	snprintf((char *)hello + 8, 24, "%s", "linux");
	int r = send_all(z, hello, sizeof hello);
	drain(z);
	z->base = z->zaudio_count;
	z->written = z->played = 0;
	z->report_time = 0;
	z->running = 0;
	return r;
}

static int zaudio_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds, unsigned short *revents)
{
	(void)pfd, (void)nfds;
	Zaudio *z = io->private_data;
	position(z);
	snd_pcm_sframes_t avail = snd_pcm_ioplug_avail(io, z->played % io->buffer_size, io->appl_ptr);
	*revents = avail >= (snd_pcm_sframes_t)io->period_size ? POLLOUT : 0;
	return 0;
}

static int zaudio_close(snd_pcm_ioplug_t *io)
{
	Zaudio *z = io->private_data;
	close(z->fd);
	free(z);
	return 0;
}

static const snd_pcm_ioplug_callback_t callbacks = {
	.start = zaudio_start,
	.stop = zaudio_stop,
	.pointer = zaudio_pointer,
	.transfer = zaudio_transfer,
	.prepare = zaudio_prepare,
	.poll_revents = zaudio_poll_revents,
	.close = zaudio_close,
};

/* 16-bit, mono or stereo, any rate zaudio can resample (8-192 kHz); a buffer
 * of 50 ms to 1 s, since the hardware pointer moves every 50 ms or so. */
static int constrain(Zaudio *z)
{
	static const unsigned int access[] = { SND_PCM_ACCESS_RW_INTERLEAVED };
	static const unsigned int formats[] = { SND_PCM_FORMAT_S16_LE };
	int r;
	if ((r = snd_pcm_ioplug_set_param_list(&z->io, SND_PCM_IOPLUG_HW_ACCESS, 1, access)) < 0 ||
	    (r = snd_pcm_ioplug_set_param_list(&z->io, SND_PCM_IOPLUG_HW_FORMAT, 1, formats)) < 0 ||
	    (r = snd_pcm_ioplug_set_param_minmax(&z->io, SND_PCM_IOPLUG_HW_CHANNELS, 1, 2)) < 0 ||
	    (r = snd_pcm_ioplug_set_param_minmax(&z->io, SND_PCM_IOPLUG_HW_RATE, 8000, 192000)) < 0 ||
	    (r = snd_pcm_ioplug_set_param_minmax(&z->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 512, 64 * 1024)) < 0 ||
	    (r = snd_pcm_ioplug_set_param_minmax(&z->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 64)) < 0 ||
	    (r = snd_pcm_ioplug_set_param_minmax(&z->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 16 * 1024, 768 * 1024)) < 0)
		return r;
	return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(zaudio)
{
	(void)root, (void)conf;
	if (stream != SND_PCM_STREAM_PLAYBACK) {
		SNDERR("zaudio plays sound; it doesn't record");
		return -EINVAL;
	}
	Zaudio *z = calloc(1, sizeof *z);
	if (!z)
		return -ENOMEM;
	z->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", getenv("ZURBAX_AUDIO_SOCKET") ? getenv("ZURBAX_AUDIO_SOCKET") : SOCKET_PATH);
	if (z->fd < 0 || connect(z->fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		SNDERR("no Zurbax sound service at %s", sa.sun_path);
		if (z->fd >= 0)
			close(z->fd);
		free(z);
		return -ENOENT;
	}
	z->io.version = SND_PCM_IOPLUG_VERSION;
	z->io.name = "Zurbax sound (zaudio)";
	z->io.callback = &callbacks;
	z->io.private_data = z;
	z->io.poll_fd = z->fd;
	z->io.poll_events = POLLIN;
	z->io.mmap_rw = 0;
	int r = snd_pcm_ioplug_create(&z->io, name, stream, mode);
	if (r < 0) {
		close(z->fd);
		free(z);
		return r;
	}
	if ((r = constrain(z)) < 0) {
		snd_pcm_ioplug_delete(&z->io);
		return r;
	}
	*pcmp = z->io.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(zaudio);
