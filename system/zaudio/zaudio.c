/*
 * zaudio — the sound service.
 *
 * Apps connect to /run/zurbax/audio.sock and send 16-bit PCM at any rate;
 * zaudio resamples each stream to 48 kHz, mixes them with their volumes and
 * the master volume (/run/zurbax/volume) and writes to the sound card
 * through the kernel's ALSA PCM interface. At start it unmutes the card's
 * outputs and turns its hardware volumes up, since the volume is applied
 * here. With nothing playing it lets the card stop.
 */
#include "zui/audio.h"

#define PERIOD 480             /* frames per write: 10 ms */
#define PERIODS 4
#define RING_SECONDS 1
#define MAX_CLIENTS 32

/* ---- ALSA ---------------------------------------------------------------------------------- */

#define PCM_HW_PARAMS 0xC2604111u
#define PCM_PREPARE 0x00004140u
#define PCM_DROP 0x00004143u
#define PCM_WRITEI 0x40184150u
#define CTL_ELEM_LIST 0xC0505510u
#define CTL_ELEM_INFO 0xC1105511u
#define CTL_ELEM_READ 0xC4C85512u
#define CTL_ELEM_WRITE 0xC4C85513u

typedef struct {
	u32 min, max;
	u32 flags;
} Interval;

typedef struct {
	u32 flags;
	u32 masks[3][8];
	u32 mres[5][8];
	Interval iv[12];
	Interval ires[9];
	u32 rmask, cmask, info, msbits, rate_num, rate_den;
	u64 fifo_size;
	u8 sync[16];
	u8 reserved[48];
} HwParams;

_Static_assert(sizeof(HwParams) == 608, "snd_pcm_hw_params layout");

enum { IV_SAMPLE_BITS, IV_FRAME_BITS, IV_CHANNELS, IV_RATE, IV_PERIOD_TIME, IV_PERIOD_SIZE, IV_PERIOD_BYTES, IV_PERIODS,
       IV_BUFFER_TIME, IV_BUFFER_SIZE, IV_BUFFER_BYTES, IV_TICK_TIME };

typedef struct {
	i64 result;
	const void *buf;
	u64 frames;
} XferI;

typedef struct {
	u32 numid;
	i32 iface;
	u32 device, subdevice;
	char name[44];
	u32 index;
} ElemId;

typedef struct {
	u32 offset, space, used, count;
	ElemId *pids;
	u8 reserved[50];
	u8 pad[6];
} ElemList;

typedef struct {
	ElemId id;
	i32 type;
	u32 access, count;
	i32 owner;
	union {
		struct {
			i64 min, max, step;
		} integer;
		u8 reserved[128];
	} value;
	u8 reserved[64];
} ElemInfo;

typedef struct {
	ElemId id;
	u32 indirect;
	u32 pad;
	union {
		i64 integer[128];
		u8 reserved[1024];
	} value;
	u8 reserved[128];
} ElemValue;

_Static_assert(sizeof(ElemList) == 80, "snd_ctl_elem_list layout");
_Static_assert(sizeof(ElemInfo) == 272, "snd_ctl_elem_info layout");
_Static_assert(sizeof(ElemValue) == 1224, "snd_ctl_elem_value layout");

static void log_msg(const char *f, ...) PRINTF_LIKE(1, 2);
static void log_msg(const char *f, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, f);
	int n = fmt(line, sizeof line, "<6>zaudio: ");
	n += fmt_v(line + n, sizeof line - (usize)n, f, ap);
	va_end(ap);
	int fd = (int)sys_open("/dev/kmsg", O_WRONLY | O_CLOEXEC, 0);
	if (fd >= 0) {
		sys_write(fd, line, (usize)n);
		sys_close(fd);
	} else {
		eprint("%s\n", line + 3);
	}
}

/* Unmutes every playback switch and turns playback volumes up, except beeps. */
static void open_mixer(int card)
{
	char path[64];
	fmt(path, sizeof path, "/dev/snd/controlC%d", card);
	int fd = (int)sys_open(path, O_RDWR | O_CLOEXEC, 0);
	if (fd < 0)
		return;
	ElemList list = { 0 };
	if (sys_ioctl(fd, CTL_ELEM_LIST, &list) < 0 || !list.count) {
		sys_close(fd);
		return;
	}
	ElemId *ids = NEW_ARRAY(ElemId, list.count);
	list.space = list.count;
	list.pids = ids;
	if (sys_ioctl(fd, CTL_ELEM_LIST, &list) == 0) {
		for (u32 i = 0; i < list.used; i++) {
			const char *name = ids[i].name;
			if (ids[i].iface != 2 || str_find(name, "Capture") || str_find(name, "Mic") || str_find(name, "Loopback"))
				continue;
			bool sw = str_ends(name, "Playback Switch"), vol = str_ends(name, "Playback Volume");
			if (!sw && !vol)
				continue;
			ElemInfo info = { .id = ids[i] };
			if (sys_ioctl(fd, CTL_ELEM_INFO, &info) < 0)
				continue;
			ElemValue v = { .id = ids[i] };
			bool beep = str_find(name, "Beep") != NULL;
			for (u32 k = 0; k < info.count && k < 128; k++) {
				if (info.type == 1)
					v.value.integer[k] = beep ? 0 : 1;
				else if (info.type == 2)
					v.value.integer[k] = beep ? info.value.integer.min : info.value.integer.max;
			}
			if (info.type == 1 || info.type == 2)
				sys_ioctl(fd, CTL_ELEM_WRITE, &v);
		}
	}
	mem_free(ids);
	sys_close(fd);
}

static void params_any(HwParams *p)
{
	memset(p, 0, sizeof *p);
	for (int m = 0; m < 3; m++)
		memset(p->masks[m], 0xff, sizeof p->masks[m]);
	for (int i = 0; i < 12; i++)
		p->iv[i].max = 0xffffffffu;
	p->rmask = 0xffffffffu;
}

static void mask_only(u32 mask[8], int bit)
{
	memset(mask, 0, 32);
	mask[bit / 32] = 1u << (bit % 32);
}

static void iv_set(HwParams *p, int which, u32 min, u32 max)
{
	p->iv[which].min = min;
	p->iv[which].max = max;
	p->iv[which].flags = 0;
}

typedef struct {
	int fd;
	int card;
	u32 rate;
	bool running;
	char name[80];
} Pcm;

/* The analog output if there is one, else the first playback device. */
static bool find_device(int *card, int *device, char *name, usize cap)
{
	char *pcm = file_read("/proc/asound/pcm", NULL);
	if (!pcm)
		return false;
	bool found = false;
	for (int pass = 0; pass < 2 && !found; pass++) {
		for (const char *line = pcm; line && *line;) {
			const char *nl = str_chr(line, '\n');
			usize len = nl ? (usize)(nl - line) : strlen(line);
			char l[256];
			str_copy(l, line, MIN(sizeof l, len + 1));
			if (str_find(l, "playback") && (pass == 1 || str_find(l, "Analog")) && !str_find(l, "HDMI")) {
				*card = (int)str_to_int(l, NULL);
				*device = (int)str_to_int(l + 3, NULL);
				const char *n = str_find(l, ": ");
				str_copy(name, n ? n + 2 : l, cap);
				char *colon = (char *)str_find(name, " :");
				if (colon)
					*colon = 0;
				found = true;
				break;
			}
			line = nl ? nl + 1 : NULL;
		}
	}
	mem_free(pcm);
	return found;
}

static bool pcm_open(Pcm *p)
{
	int card, device;
	if (!find_device(&card, &device, p->name, sizeof p->name))
		return false;
	char path[64];
	fmt(path, sizeof path, "/dev/snd/pcmC%dD%dp", card, device);
	p->fd = (int)sys_open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
	if (p->fd < 0)
		return false;
	p->card = card;
	static const u32 rates[] = { AUDIO_RATE, 44100 };
	for (usize i = 0; i < ARRAY_LEN(rates); i++) {
		HwParams hp;
		params_any(&hp);
		mask_only(hp.masks[0], 3); /* interleaved read/write */
		mask_only(hp.masks[1], 2); /* signed 16-bit little-endian */
		mask_only(hp.masks[2], 0);
		iv_set(&hp, IV_CHANNELS, 2, 2);
		iv_set(&hp, IV_RATE, rates[i], rates[i]);
		iv_set(&hp, IV_PERIOD_SIZE, PERIOD, PERIOD);
		iv_set(&hp, IV_PERIODS, PERIODS, PERIODS);
		if (sys_ioctl(p->fd, PCM_HW_PARAMS, &hp) == 0) {
			p->rate = rates[i];
			sys_ioctl(p->fd, PCM_PREPARE, NULL);
			open_mixer(card);
			log_msg("playing on %s at %u Hz", p->name, p->rate);
			return true;
		}
		/* Some devices refuse the exact period: let the kernel choose. */
		params_any(&hp);
		mask_only(hp.masks[0], 3);
		mask_only(hp.masks[1], 2);
		mask_only(hp.masks[2], 0);
		iv_set(&hp, IV_CHANNELS, 2, 2);
		iv_set(&hp, IV_RATE, rates[i], rates[i]);
		iv_set(&hp, IV_BUFFER_TIME, 30000, 80000);
		if (sys_ioctl(p->fd, PCM_HW_PARAMS, &hp) == 0) {
			p->rate = rates[i];
			sys_ioctl(p->fd, PCM_PREPARE, NULL);
			open_mixer(card);
			log_msg("playing on %s at %u Hz", p->name, p->rate);
			return true;
		}
	}
	sys_close(p->fd);
	p->fd = -1;
	return false;
}

/* Writes frames; false if the device has gone away. */
static bool pcm_write(Pcm *p, const i16 *frames, usize n)
{
	while (n) {
		XferI x = { 0, frames, n };
		long r = sys_ioctl(p->fd, PCM_WRITEI, &x);
		if (r == -EPIPE || r == -86 /* ESTRPIPE: resumed from suspend */) {
			sys_ioctl(p->fd, PCM_PREPARE, NULL);
			continue;
		}
		if (r == -EAGAIN)
			return true; /* full: the rest is dropped (only happens on resync) */
		if (r < 0)
			return false;
		p->running = true;
		frames += (usize)x.result * 2;
		n -= (usize)x.result;
	}
	return true;
}

/* ---- Clients ------------------------------------------------------------------------------------- */

typedef struct {
	int fd;
	int rate, channels;
	f32 volume;
	i16 *ring;               /* interleaved, `channels` per frame */
	usize cap, head, count;  /* in frames */
	f64 frac;                /* resampling position between frames */
	u64 played;
	u64 reported_ms;
	char name[24];
} Client;

static Client *clients[MAX_CLIENTS];
static int nclients;

static void client_free(int i)
{
	Client *c = clients[i];
	sys_close(c->fd);
	mem_free(c->ring);
	mem_free(c);
	clients[i] = clients[--nclients];
}

static usize room(const Client *c)
{
	return c->cap - c->count;
}

/* Reads waiting messages; returns false if the client went away. */
static bool client_read(Client *c)
{
	static u8 buf[65536 + 8];
	for (;;) {
		if (c->ring && room(c) < 16384 / 2)
			return true; /* backpressure: the client's sends block meanwhile */
		long n = sys_recvfrom(c->fd, buf, sizeof buf, 0, NULL, NULL);
		if (n == -EAGAIN)
			return true;
		if (n <= 0)
			return false;
		switch (buf[0]) {
		case AUDIO_HELLO:
			if (n >= 8) {
				c->channels = CLAMP(buf[1], 1, 2);
				u32 rate;
				memcpy(&rate, buf + 4, 4);
				c->rate = (int)CLAMP(rate, 8000u, 192000u);
				if (n >= 32)
					str_copy(c->name, (const char *)buf + 8, sizeof c->name);
				mem_free(c->ring);
				c->cap = (usize)c->rate * RING_SECONDS;
				c->ring = NEW_ARRAY(i16, c->cap * (usize)c->channels);
				c->head = c->count = 0;
				c->frac = 0;
			}
			break;
		case AUDIO_DATA: {
			if (!c->ring)
				break;
			usize frames = (usize)(n - 4) / (2 * (usize)c->channels);
			const i16 *s = (const i16 *)(buf + 4);
			frames = MIN(frames, room(c));
			for (usize i = 0; i < frames; i++) {
				usize at = (c->head + c->count) % c->cap;
				for (int ch = 0; ch < c->channels; ch++)
					c->ring[at * (usize)c->channels + (usize)ch] = s[i * (usize)c->channels + (usize)ch];
				c->count++;
			}
			break;
		}
		case AUDIO_FLUSH:
			c->count = 0;
			c->frac = 0;
			break;
		case AUDIO_VOLUME:
			if (n >= 4) {
				u16 v;
				memcpy(&v, buf + 2, 2);
				c->volume = (f32)MIN(v, 1000) / 1000.0f;
			}
			break;
		}
	}
}

static void report(Client *c, u64 now)
{
	if (now - c->reported_ms < 50)
		return;
	c->reported_ms = now;
	u8 msg[16] = { AUDIO_PLAYED };
	memcpy(msg + 8, &c->played, 8);
	sys_sendto(c->fd, msg, sizeof msg, MSG_DONTWAIT, NULL, 0);
}

/* ---- Mixing --------------------------------------------------------------------------------------- */

static f32 master_gain(void)
{
	static u64 checked;
	static f32 gain = 0.36f;
	u64 now = time_mono_ms();
	if (now - checked > 200) {
		checked = now;
		i64 v = file_read_int("/run/zurbax/volume", 60);
		f32 k = (f32)CLAMP(v, 0, 100) / 100.0f;
		gain = k * k; /* loudness follows roughly the square */
	}
	return gain;
}

/* Mixes one period into out (stereo); returns true if anything played. */
static bool mix(i16 *out, u32 out_rate)
{
	static f32 acc[PERIOD * 2];
	memset(acc, 0, sizeof acc);
	bool any = false;
	for (int i = 0; i < nclients; i++) {
		Client *c = clients[i];
		if (!c->ring || !c->count)
			continue;
		any = true;
		f64 step = (f64)c->rate / (f64)out_rate;
		f32 vol = c->volume;
		usize used = 0;
		for (int f = 0; f < PERIOD; f++) {
			usize i0 = (usize)c->frac;
			if (i0 + 1 >= c->count)
				break;
			f32 t = (f32)(c->frac - (f64)i0);
			usize a = (c->head + i0) % c->cap, b = (c->head + i0 + 1) % c->cap;
			f32 l, r;
			if (c->channels == 2) {
				l = (f32)c->ring[a * 2] * (1 - t) + (f32)c->ring[b * 2] * t;
				r = (f32)c->ring[a * 2 + 1] * (1 - t) + (f32)c->ring[b * 2 + 1] * t;
			} else {
				l = r = (f32)c->ring[a] * (1 - t) + (f32)c->ring[b] * t;
			}
			acc[f * 2] += l * vol;
			acc[f * 2 + 1] += r * vol;
			c->frac += step;
			used = (usize)c->frac;
		}
		/* The last frame of a finished sound would never get a partner: let it go. */
		if (c->count == 1)
			used = 1;
		used = MIN(used, c->count);
		c->head = (c->head + used) % c->cap;
		c->count -= used;
		c->frac -= (f64)used;
		c->played += used;
	}
	f32 g = master_gain();
	for (int i = 0; i < PERIOD * 2; i++) {
		f32 v = acc[i] * g;
		/* Soft limit near full scale instead of hard clipping. */
		if (v > 24000)
			v = 24000 + (v - 24000) / (1 + (v - 24000) / 8767.0f);
		else if (v < -24000)
			v = -24000 + (v + 24000) / (1 - (v + 24000) / 8767.0f);
		out[i] = (i16)CLAMP(v, -32767.0f, 32767.0f);
	}
	return any;
}

/* ---- Main ------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	/* --null: no sound card, just keep time (testing, or machines without sound). */
	bool null_out = argc > 1 && str_eq(argv[1], "--null");
	const char *sock = env_get("ZURBAX_AUDIO_SOCKET");
	if (!sock)
		sock = AUDIO_SOCKET;
	sig_ignore(SIGPIPE);
	dir_make("/run/zurbax", 0755);
	sys_unlink(sock);
	int ls = unix_listen(sock, SOCK_SEQPACKET);
	if (ls < 0) {
		log_msg("cannot listen on %s", sock);
		return 1;
	}
	sys_chmod(sock, 0666);
	fd_set_nonblock(ls, true);
	Pcm pcm = { .fd = -1 };
	u64 next_try = 0, idle_since = 0, null_next = 0;
	i16 period[PERIOD * 2];
	for (;;) {
		u64 now = time_mono_ms();
		if (!null_out && pcm.fd < 0 && now >= next_try) {
			if (!pcm_open(&pcm))
				next_try = now + 5000;
		}
		/* No card (or --null): keep time anyway, so players move on rather than hang. */
		bool keep_time = null_out || pcm.fd < 0;
		if (keep_time) {
			/* A pretend card: one period every 10 ms. */
			if (now >= null_next) {
				mix(period, AUDIO_RATE);
				null_next = (null_next ? null_next : now) + 10;
				if (null_next < now)
					null_next = now + 10;
			}
		}
		struct pollfd pfd[MAX_CLIENTS + 2];
		int np = 0;
		pfd[np++] = (struct pollfd){ ls, POLLIN, 0 };
		bool want_pcm = false;
		for (int i = 0; i < nclients; i++)
			if (clients[i]->count > 1)
				want_pcm = true;
		/* Keep the card going a little after the last sound, so short sounds don't pop. */
		if (pcm.running && !want_pcm) {
			if (!idle_since)
				idle_since = now;
			if (now - idle_since > 1500) {
				sys_ioctl(pcm.fd, PCM_DROP, NULL);
				sys_ioctl(pcm.fd, PCM_PREPARE, NULL);
				pcm.running = false;
			} else {
				want_pcm = true;
			}
		} else if (want_pcm) {
			idle_since = 0;
		}
		int pcm_index = -1;
		if (pcm.fd >= 0 && want_pcm) {
			pcm_index = np;
			pfd[np++] = (struct pollfd){ pcm.fd, POLLOUT, 0 };
		}
		int first_client = np;
		for (int i = 0; i < nclients; i++)
			pfd[np++] = (struct pollfd){ clients[i]->fd, (short)(room(clients[i]) >= 8192 || !clients[i]->ring ? POLLIN : 0), 0 };
		int wait = pcm.fd < 0 ? 1000 : want_pcm ? 50 : 1000;
		if (keep_time)
			wait = (int)MIN(wait, MAX(1, (i64)null_next - (i64)now));
		sys_poll(pfd, (usize)np, wait);
		now = time_mono_ms();
		if (pfd[0].revents & POLLIN) {
			for (;;) {
				int c = (int)sys_accept4(ls, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
				if (c < 0)
					break;
				if (nclients == MAX_CLIENTS) {
					sys_close(c);
					continue;
				}
				Client *cl = NEW(Client);
				cl->fd = c;
				cl->volume = 1;
				clients[nclients++] = cl;
			}
		}
		for (int i = nclients - 1; i >= 0; i--) {
			int k = first_client + i;
			if (k < np && (pfd[k].revents & (POLLIN | POLLHUP | POLLERR))) {
				if (!client_read(clients[i]))
					client_free(i);
			}
		}
		if (pcm_index >= 0 && (pfd[pcm_index].revents & (POLLOUT | POLLERR))) {
			mix(period, pcm.rate);
			if (!pcm_write(&pcm, period, PERIOD)) {
				log_msg("lost %s", pcm.name);
				sys_close(pcm.fd);
				pcm.fd = -1;
				pcm.running = false;
				next_try = now + 2000;
			}
		}
		for (int i = 0; i < nclients; i++)
			report(clients[i], now);
	}
}
