/*
 * Music — your songs from the Music folder.
 *
 * Albums as a wall of covers, all songs as a list, artists, and an album
 * page. A player bar along the bottom with the cover, seek bar, shuffle and
 * repeat. The library is read in the background and remembered between
 * starts; playback runs on its own thread so nothing in the window waits.
 */
#include "zui.h"
#include "za.h"

typedef struct {
	char *path;
	char title[160], artist[160], album[160];
	int track, year;
	f32 duration;
	i64 mtime;
	int album_index;
} Track;

typedef struct {
	char title[160], artist[160];
	int year;
	int first_track;         /* index in sorted order */
	int count;
	f32 duration;
	Canvas *cover;
	bool cover_tried;
	Color tint;              /* average of the cover, for the album page */
} Album;

enum { VIEW_ALBUMS, VIEW_SONGS, VIEW_ARTISTS, VIEW_ALBUM };
enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };
enum { CMD_NONE, CMD_PLAY, CMD_SEEK, CMD_PAUSE, CMD_RESUME, CMD_STOP };

typedef struct {
	Ui *ui;
	/* library */
	Track *tracks;
	int ntracks;
	Album *albums;
	int nalbums;
	int unsupported;
	int scan_state;          /* 0 idle, 1 scanning, 2 finished */
	Track *scan_tracks;
	int scan_n, scan_unsupported;
	/* views */
	int view, album_view;    /* the album shown on the album page */
	StrBuf search;
	/* queue */
	int *queue;
	int nqueue, qpos;
	bool shuffle;
	int repeat;
	/* player thread */
	Mutex lock;
	int cmd;
	int cmd_track;
	f64 cmd_seek;
	int playing_track;       /* -1 if none */
	bool paused;
	f64 position, duration;
	bool track_ended;
	f32 volume;
	AudioStream *stream;
	char error[160];
	char *open_path;         /* a file to play once the library is read */
} Music;

static Music mu = { .playing_track = -1, .volume = 1 };

static void play_from(const int *list, int n, int start);

/* ---- Library -------------------------------------------------------------------------------- */

static bool playable(const char *name)
{
	const char *e = path_ext(name);
	return str_eq_nocase(e, "flac") || str_eq_nocase(e, "ogg") || str_eq_nocase(e, "oga") || str_eq_nocase(e, "wav");
}

static bool known_unplayable(const char *name)
{
	const char *e = path_ext(name);
	return str_eq_nocase(e, "mp3") || str_eq_nocase(e, "m4a") || str_eq_nocase(e, "aac") || str_eq_nocase(e, "wma") || str_eq_nocase(e, "opus");
}

static char cache_path[512];

typedef VEC(Track) TrackVec;

/* The tag cache: one line per song, "path\tmtime\tduration\ttrack\tyear\ttitle\tartist\talbum",
 * looked up by path through a small hash table. */
typedef struct {
	char *text;
	char **lines;
	usize n;
	int *slots;
	usize nslots;
} Cache;

static u32 hash_str(const char *s, usize n)
{
	u32 h = 2166136261u;
	for (usize i = 0; i < n; i++)
		h = (h ^ (u8)s[i]) * 16777619u;
	return h;
}

static void cache_load(Cache *c)
{
	memset(c, 0, sizeof *c);
	c->text = file_read(cache_path, NULL);
	if (!c->text)
		return;
	c->lines = str_split(c->text, '\n', &c->n);
	c->nslots = c->n * 2 + 16;
	c->slots = NEW_ARRAY(int, c->nslots);
	for (usize i = 0; i < c->nslots; i++)
		c->slots[i] = -1;
	for (usize i = 0; i < c->n; i++) {
		const char *tab = str_chr(c->lines[i], '\t');
		if (!tab)
			continue;
		usize at = hash_str(c->lines[i], (usize)(tab - c->lines[i])) % c->nslots;
		while (c->slots[at] >= 0)
			at = (at + 1) % c->nslots;
		c->slots[at] = (int)i;
	}
}

static const char *cache_find(const Cache *c, const char *path)
{
	if (!c->nslots)
		return NULL;
	usize len = strlen(path);
	for (usize at = hash_str(path, len) % c->nslots; c->slots[at] >= 0; at = (at + 1) % c->nslots) {
		const char *l = c->lines[c->slots[at]];
		if (memcmp(l, path, len) == 0 && l[len] == '\t')
			return l + len + 1;
	}
	return NULL;
}

static void cache_free(Cache *c)
{
	if (c->lines)
		str_split_free(c->lines, c->n);
	mem_free(c->slots);
	mem_free(c->text);
}

static void scan_dir(const char *dir, TrackVec *out, const Cache *cache, int depth)
{
	if (depth > 12)
		return;
	DirList list = { 0 };
	if (!dir_list(dir, &list, true))
		return;
	for (usize i = 0; i < list.len; i++) {
		DirEntry *e = &list.data[i];
		if (e->name[0] == '.')
			continue;
		char *p = path_join(dir, e->name);
		if (S_ISDIR(e->mode)) {
			scan_dir(p, out, cache, depth + 1);
		} else if (playable(e->name)) {
			Track t = { 0 };
			t.path = p;
			t.mtime = e->mtime;
			const char *c = cache_find(cache, p);
			bool ok = false;
			if (c) {
				usize n;
				char **f = str_split(c, '\t', &n);
				if (n == 7 && str_to_int(f[0], NULL) == e->mtime) {
					t.duration = (f32)str_to_float(f[1], NULL);
					t.track = (int)str_to_int(f[2], NULL);
					t.year = (int)str_to_int(f[3], NULL);
					str_copy(t.title, f[4], sizeof t.title);
					str_copy(t.artist, f[5], sizeof t.artist);
					str_copy(t.album, f[6], sizeof t.album);
					ok = true;
				}
				str_split_free(f, n);
			}
			if (!ok) {
				AudioInfo info;
				if (!audio_probe(p, &info, false)) {
					mem_free(p);
					continue;
				}
				t.duration = (f32)info.duration;
				t.track = info.track;
				t.year = info.year;
				str_copy(t.title, info.title, sizeof t.title);
				str_copy(t.artist, info.artist, sizeof t.artist);
				str_copy(t.album, info.album, sizeof t.album);
			}
			vec_push(out, t);
			continue;
		} else if (known_unplayable(e->name)) {
			mu.scan_unsupported++;
		}
		mem_free(p);
	}
	dir_list_free(&list);
}

static void clean_field(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n')
			*s = ' ';
}

static void scan_thread(void *arg)
{
	UNUSED(arg);
	char *root = path_join(home_dir(), "Music");
	Cache cache;
	cache_load(&cache);
	TrackVec v = { 0 };
	mu.scan_unsupported = 0;
	scan_dir(root, &v, &cache, 0);
	cache_free(&cache);
	mem_free(root);
	/* Remember what was read for next time. */
	StrBuf out = { 0 };
	for (usize i = 0; i < v.len; i++) {
		Track *t = &v.data[i];
		clean_field(t->title), clean_field(t->artist), clean_field(t->album);
		sb_fmt(&out, "%s\t%lld\t%.2f\t%d\t%d\t%s\t%s\t%s\n", t->path, (long long)t->mtime, (double)t->duration, t->track, t->year, t->title, t->artist,
		       t->album);
	}
	char *data_dir = path_dir(cache_path);
	dir_make(data_dir, 0755);
	mem_free(data_dir);
	file_write_atomic(cache_path, out.data ? out.data : "", out.len);
	sb_free(&out);
	mu.scan_tracks = v.data;
	mu.scan_n = (int)v.len;
	atomic_store(&mu.scan_state, 2);
	ui_wake(mu.ui);
}

static int cmp_track(const void *a, const void *b)
{
	const Track *x = a, *y = b;
	const char *ax = x->artist[0] ? x->artist : "\x7f", *ay = y->artist[0] ? y->artist : "\x7f";
	int c = str_cmp_nocase(ax, ay);
	if (c)
		return c;
	c = str_cmp_nocase(x->album, y->album);
	if (c)
		return c;
	if (x->track != y->track)
		return x->track - y->track;
	return str_cmp_natural(x->title, y->title);
}

static void sort_tracks(Track *t, int n)
{
	/* Insertion sort by artist, album and track: fine for a home library. */
	for (int i = 1; i < n; i++) {
		Track k = t[i];
		int j = i - 1;
		while (j >= 0 && cmp_track(&t[j], &k) > 0) {
			t[j + 1] = t[j];
			j--;
		}
		t[j + 1] = k;
	}
}

static void build_albums(void)
{
	for (int i = 0; i < mu.nalbums; i++)
		canvas_free(mu.albums[i].cover);
	mem_free(mu.albums);
	mu.albums = NEW_ARRAY(Album, MAX(mu.ntracks, 1));
	mu.nalbums = 0;
	for (int i = 0; i < mu.ntracks; i++) {
		Track *t = &mu.tracks[i];
		Album *a = mu.nalbums ? &mu.albums[mu.nalbums - 1] : NULL;
		if (!a || !str_eq_nocase(a->title, t->album) || !str_eq_nocase(a->artist, t->artist)) {
			a = &mu.albums[mu.nalbums++];
			memset(a, 0, sizeof *a);
			str_copy(a->title, t->album[0] ? t->album : "Unknown Album", sizeof a->title);
			str_copy(a->artist, t->artist[0] ? t->artist : "Unknown Artist", sizeof a->artist);
			a->first_track = i;
		}
		a->count++;
		a->duration += t->duration;
		if (t->year)
			a->year = t->year;
		t->album_index = mu.nalbums - 1;
	}
}

/* A file opened from Files or dropped on the window: play it, adding it if
 * it's outside the library. */
static void play_opened(void)
{
if (mu.open_path) {
	int found = -1;
	for (int i = 0; i < mu.ntracks; i++)
		if (str_eq(mu.tracks[i].path, mu.open_path))
			found = i;
	AudioInfo info;
	if (found < 0 && audio_probe(mu.open_path, &info, false)) {
		mu.tracks = mem_realloc(mu.tracks, sizeof(Track) * (usize)(mu.ntracks + 1));
		Track *t = &mu.tracks[mu.ntracks];
		memset(t, 0, sizeof *t);
		t->path = str_dup(mu.open_path);
		t->duration = (f32)info.duration;
		t->track = info.track;
		str_copy(t->title, info.title, sizeof t->title);
		str_copy(t->artist, info.artist, sizeof t->artist);
		str_copy(t->album, info.album, sizeof t->album);
		found = mu.ntracks++;
		build_albums();
	}
	if (found >= 0)
		play_from(&found, 1, 0);
	mem_free(mu.open_path);
	mu.open_path = NULL;
}
}

static void library_ready(void)
{
	/* Keep playing the same song across a rescan. */
	char *playing = mu.playing_track >= 0 ? str_dup(mu.tracks[mu.playing_track].path) : NULL;
	for (int i = 0; i < mu.ntracks; i++)
		mem_free(mu.tracks[i].path);
	mem_free(mu.tracks);
	mu.tracks = mu.scan_tracks;
	mu.ntracks = mu.scan_n;
	mu.unsupported = mu.scan_unsupported;
	sort_tracks(mu.tracks, mu.ntracks);
	build_albums();
	mutex_lock(&mu.lock);
	mu.playing_track = -1;
	for (int i = 0; playing && i < mu.ntracks; i++)
		if (str_eq(mu.tracks[i].path, playing))
			mu.playing_track = i;
	mutex_unlock(&mu.lock);
	mem_free(playing);
	atomic_store(&mu.scan_state, 0);
	play_opened();
}

static void rescan(void)
{
	if (atomic_load(&mu.scan_state) == 1)
		return;
	atomic_store(&mu.scan_state, 1);
	if (thread_spawn(scan_thread, NULL) < 0)
		atomic_store(&mu.scan_state, 0);
}

/* Cover pictures: embedded, else cover.jpg / folder.jpg next to the songs. */
static void load_cover(Album *a)
{
	a->cover_tried = true;
	Track *t = &mu.tracks[a->first_track];
	AudioInfo info;
	Canvas *img = NULL;
	if (audio_probe(t->path, &info, true) && info.cover) {
		img = image_decode(info.cover, info.cover_len);
		audio_info_free(&info);
	}
	if (!img) {
		char *dir = path_dir(t->path);
		static const char *const names[] = { "cover.jpg", "cover.png", "folder.jpg", "folder.png", "Cover.jpg", "Folder.jpg", "front.jpg" };
		for (usize i = 0; i < ARRAY_LEN(names) && !img; i++) {
			char *p = path_join(dir, names[i]);
			img = image_load_thumb(p, 512);
			mem_free(p);
		}
		mem_free(dir);
	}
	if (!img)
		return;
	int size = 320;
	Canvas *c = canvas_new(size, size);
	f32 s = maxf((f32)size / (f32)img->width, (f32)size / (f32)img->height);
	f32 w = (f32)img->width * s, h = (f32)img->height * s;
	blit_scaled(c, img, rect(0, 0, (f32)img->width, (f32)img->height), rect(((f32)size - w) / 2, ((f32)size - h) / 2, w, h), 1);
	canvas_free(img);
	/* Average colour, for tinting the album page. */
	u64 r = 0, g = 0, b = 0, n = 0;
	for (int y = 0; y < size; y += 8)
		for (int x = 0; x < size; x += 8) {
			u32 px = c->pixels[y * c->stride + x];
			r += (px >> 16) & 255, g += (px >> 8) & 255, b += px & 255, n++;
		}
	a->tint = 0xff000000u | (u32)(r / n) << 16 | (u32)(g / n) << 8 | (u32)(b / n);
	a->cover = c;
}

/* A cover for albums without a picture: a soft gradient from the title. */
static void placeholder_cover(Ui *ui, Rect r, const Album *a, f32 radius)
{
	u32 h = 2166136261u;
	for (const char *p = a->title; *p; p++)
		h = (h ^ (u8)*p) * 16777619u;
	static const char *const pairs[][2] = { { "#ff9a8b", "#ff6a88" }, { "#8ec5fc", "#e0c3fc" }, { "#a1c4fd", "#c2e9fb" }, { "#fbc2eb", "#a6c1ee" },
						{ "#84fab0", "#8fd3f4" }, { "#fccb90", "#d57eeb" }, { "#f6d365", "#fda085" }, { "#5ee7df", "#b490ca" } };
	int k = (int)(h % ARRAY_LEN(pairs));
	ui_gradient(ui, r, radius, color_hex(pairs[k][0]), color_hex(pairs[k][1]));
	ui_icon(ui, center_in(r, r.w * 0.36f, r.w * 0.36f), "music", color_hex("#ffffffd0"));
}

static void draw_cover(Ui *ui, Rect r, Album *a, f32 radius)
{
	if (!a->cover_tried)
		load_cover(a);
	ui_shadow(ui, r, radius, 10, 3, color_hex("#00000030"));
	if (a->cover)
		ui_image(ui, r, a->cover, radius, true);
	else
		placeholder_cover(ui, r, a, radius);
}

/* ---- Player thread ---------------------------------------------------------------------------- */

static void player_thread(void *arg)
{
	UNUSED(arg);
	Decoder *d = NULL;
	AudioInfo info;
	int rate = 0, ch = 0;
	i64 base = 0;            /* the server's frame count at the song's start (can be negative after a seek) */
	static i16 buf[4096 * 8], out[4096 * 2];
	for (;;) {
		mutex_lock(&mu.lock);
		int cmd = mu.cmd;
		mu.cmd = CMD_NONE;
		int track = mu.cmd_track;
		f64 seek = mu.cmd_seek;
		bool paused = mu.paused;
		mutex_unlock(&mu.lock);
		if (cmd == CMD_PLAY || cmd == CMD_STOP) {
			if (d)
				decoder_close(d);
			d = NULL;
			if (mu.stream)
				audio_flush(mu.stream);
			if (cmd == CMD_PLAY && track >= 0 && track < mu.ntracks) {
				d = decoder_open(mu.tracks[track].path, &info);
				if (d) {
					if (!mu.stream || info.rate != rate || MIN(info.channels, 2) != ch) {
						audio_close(mu.stream);
						rate = info.rate, ch = MIN(info.channels, 2);
						mu.stream = audio_open("music", rate, ch);
						audio_set_volume(mu.stream, mu.volume);
					}
					base = (i64)audio_played(mu.stream);
					mutex_lock(&mu.lock);
					mu.playing_track = track;
					mu.duration = info.duration;
					mu.position = 0;
					mu.paused = false;
					mu.error[0] = 0;
					if (!mu.stream)
						str_copy(mu.error, "The sound service isn't running.", sizeof mu.error);
					mutex_unlock(&mu.lock);
				} else {
					mutex_lock(&mu.lock);
					str_copy(mu.error, "This song couldn't be played.", sizeof mu.error);
					mu.track_ended = true;
					mutex_unlock(&mu.lock);
				}
			}
			if (cmd == CMD_STOP) {
				mutex_lock(&mu.lock);
				mu.playing_track = -1;
				mutex_unlock(&mu.lock);
			}
			ui_wake(mu.ui);
			continue;
		}
		if ((cmd == CMD_SEEK || cmd == CMD_RESUME) && d) {
			u64 frame = (u64)(seek * rate);
			decoder_seek(d, frame);
			audio_flush(mu.stream);
			base = (i64)audio_played(mu.stream) - (i64)frame;
			mutex_lock(&mu.lock);
			mu.position = seek;
			if (cmd == CMD_RESUME)
				mu.paused = false;
			mutex_unlock(&mu.lock);
			paused = mu.paused;
		}
		if (cmd == CMD_PAUSE && d) {
			/* Stop at once: drop what's queued, and resume from here later. */
			audio_flush(mu.stream);
			mutex_lock(&mu.lock);
			mu.paused = true;
			mutex_unlock(&mu.lock);
			paused = true;
		}
		if (!d || paused || !mu.stream) {
			sleep_ms(15);
			continue;
		}
		usize n = decoder_read(d, buf, 2048);
		if (n == 0) {
			/* Let the last of it play out, then report the end. */
			if (audio_played(mu.stream) + 64 < audio_written(mu.stream)) {
				sleep_ms(20);
				mutex_lock(&mu.lock);
				mu.position = (f64)((i64)audio_played(mu.stream) - base) / rate;
				mutex_unlock(&mu.lock);
				ui_wake(mu.ui);
				continue;
			}
			decoder_close(d);
			d = NULL;
			mutex_lock(&mu.lock);
			mu.track_ended = true;
			mutex_unlock(&mu.lock);
			ui_wake(mu.ui);
			continue;
		}
		const i16 *src = buf;
		if (info.channels > 2) {
			for (usize i = 0; i < n; i++)
				out[i * 2] = buf[i * (usize)info.channels], out[i * 2 + 1] = buf[i * (usize)info.channels + 1];
			src = out;
		}
		audio_write(mu.stream, src, n);
		mutex_lock(&mu.lock);
		i64 played = (i64)audio_played(mu.stream);
		mu.position = played > base ? (f64)(played - base) / rate : 0;
		mutex_unlock(&mu.lock);
		ui_wake(mu.ui);
	}
}

static void send_cmd(int cmd, int track, f64 seek)
{
	mutex_lock(&mu.lock);
	mu.cmd = cmd;
	mu.cmd_track = track;
	mu.cmd_seek = seek;
	mutex_unlock(&mu.lock);
}

/* ---- Queue ------------------------------------------------------------------------------------- */

static void play_from(const int *list, int n, int start)
{
	mem_free(mu.queue);
	mu.queue = NEW_ARRAY(int, MAX(n, 1));
	memcpy(mu.queue, list, sizeof(int) * (usize)n);
	mu.nqueue = n;
	mu.qpos = start;
	if (mu.shuffle && n > 1) {
		/* Keep the chosen song first, shuffle the rest. */
		int first = mu.queue[start];
		mu.queue[start] = mu.queue[0];
		mu.queue[0] = first;
		for (int i = n - 1; i > 1; i--) {
			int j = 1 + (int)(rand_u32() % (u32)i);
			int t = mu.queue[i];
			mu.queue[i] = mu.queue[j];
			mu.queue[j] = t;
		}
		mu.qpos = 0;
	}
	send_cmd(CMD_PLAY, mu.queue[mu.qpos], 0);
}

static void play_album_track(int album, int track_index)
{
	Album *a = &mu.albums[album];
	int *list = NEW_ARRAY(int, a->count);
	for (int i = 0; i < a->count; i++)
		list[i] = a->first_track + i;
	play_from(list, a->count, track_index);
	mem_free(list);
}

static void play_all(int start)
{
	int *list = NEW_ARRAY(int, MAX(mu.ntracks, 1));
	for (int i = 0; i < mu.ntracks; i++)
		list[i] = i;
	play_from(list, mu.ntracks, start);
	mem_free(list);
}

static void next_track(bool user)
{
	if (!mu.nqueue)
		return;
	if (mu.repeat == REPEAT_ONE && !user) {
		send_cmd(CMD_PLAY, mu.queue[mu.qpos], 0);
		return;
	}
	if (mu.qpos + 1 < mu.nqueue) {
		mu.qpos++;
	} else if (mu.repeat == REPEAT_ALL || user) {
		mu.qpos = 0;
		if (!user && mu.repeat != REPEAT_ALL) {
			send_cmd(CMD_STOP, -1, 0);
			return;
		}
	} else {
		send_cmd(CMD_STOP, -1, 0);
		return;
	}
	send_cmd(CMD_PLAY, mu.queue[mu.qpos], 0);
}

static void prev_track(void)
{
	if (!mu.nqueue)
		return;
	if (mu.position > 3) {
		send_cmd(CMD_SEEK, -1, 0);
		return;
	}
	mu.qpos = mu.qpos > 0 ? mu.qpos - 1 : 0;
	send_cmd(CMD_PLAY, mu.queue[mu.qpos], 0);
}

static void toggle_pause(void)
{
	if (mu.playing_track < 0) {
		if (mu.ntracks)
			play_all(0);
		return;
	}
	if (mu.paused)
		send_cmd(CMD_RESUME, -1, mu.position);
	else
		send_cmd(CMD_PAUSE, -1, 0);
}

/* ---- Drawing ----------------------------------------------------------------------------------- */

static void fmt_duration(char *out, usize cap, f64 s)
{
	int t = (int)(s + 0.5);
	if (t >= 3600)
		fmt(out, cap, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
	else
		fmt(out, cap, "%d:%02d", t / 60, t % 60);
}

static bool matches(const Track *t)
{
	if (!mu.search.len)
		return true;
	const char *q = sb_str(&mu.search);
	return str_find_nocase(t->title, q) || str_find_nocase(t->artist, q) || str_find_nocase(t->album, q);
}

static void view_albums(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect area = pad4(r, 20, 24, 20, 24);
	f32 card = 172, gap = 22;
	int cols = MAX(1, (int)((area.w + gap) / (card + gap)));
	card = (area.w - gap * (f32)(cols - 1)) / (f32)cols;
	f32 cell_h = card + 52;
	int shown = 0;
	for (int i = 0; i < mu.nalbums; i++)
		if (!mu.search.len || str_find_nocase(mu.albums[i].title, sb_str(&mu.search)) || str_find_nocase(mu.albums[i].artist, sb_str(&mu.search)))
			shown++;
	int rows = (shown + cols - 1) / cols;
	f32 y0 = ui_scroll_begin(ui, r, "albums");
	int k = 0;
	for (int i = 0; i < mu.nalbums; i++) {
		Album *a = &mu.albums[i];
		if (mu.search.len && !str_find_nocase(a->title, sb_str(&mu.search)) && !str_find_nocase(a->artist, sb_str(&mu.search)))
			continue;
		int col = k % cols, row = k / cols;
		k++;
		Rect c = rect(area.x + (f32)col * (card + gap), y0 + area.y - r.y + (f32)row * (cell_h + gap), card, cell_h);
		if (c.y > r.y + r.h || c.y + c.h < r.y)
			continue;
		ui_push_id(ui, i);
		UiInteract it = ui_interact(ui, ui_id(ui, "album"), c);
		f32 lift = ui_fade(ui, ui_id(ui, "album"), it.hovered, 0.12f);
		Rect cover = rect(c.x, c.y - lift * 3, card, card);
		draw_cover(ui, cover, a, 8);
		if (lift > 0.01f) {
			/* A play button on hover. */
			Rect pb = rect(cover.x + cover.w - 52, cover.y + cover.h - 52, 40, 40);
			ui_circle(ui, pb.x + 20, pb.y + 20, 20, color_scale_alpha(t->accent, lift));
			ui_icon(ui, center_in(pb, 18, 18), "media-play", color_scale_alpha(COLOR_WHITE, lift));
			if (it.clicked && ui_mouse_in(ui, pb)) {
				play_album_track(i, 0);
				it.clicked = false;
			}
		}
		ui_text_in(ui, ui_font(ui, 14, 600), rect(c.x, c.y + card + 8, card, 20), a->title, t->text, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 13, 400), rect(c.x, c.y + card + 28, card, 18), a->artist, t->text_dim, ALIGN_START);
		if (it.clicked) {
			mu.album_view = i;
			mu.view = VIEW_ALBUM;
		}
		ui_pop_id(ui);
	}
	ui_scroll_end(ui, 40 + (f32)rows * (cell_h + gap));
}

static void song_row(Ui *ui, Rect row, int i, bool show_album, int number)
{
	Theme *t = &ui->theme;
	Track *tr = &mu.tracks[i];
	bool current = i == mu.playing_track;
	UiInteract it = ui_row(ui, row, ui_id_int(ui, i), false);
	Color text = current ? t->accent : t->text;
	Rect c = pad2(row, 12, 0);
	Rect num = cut_left(&c, 36);
	if (current)
		ui_icon(ui, center_in(num, 16, 16), mu.paused ? "media-pause" : "audio-volume-high", t->accent);
	else {
		char n[8];
		fmt(n, sizeof n, "%d", number);
		ui_text_in(ui, ui_font(ui, 13, 400), num, n, t->text_faint, ALIGN_START);
	}
	char dur[16];
	fmt_duration(dur, sizeof dur, tr->duration);
	ui_text_in(ui, ui_font(ui, 13, 400), cut_right(&c, 56), dur, t->text_dim, ALIGN_END);
	if (show_album) {
		f32 w = c.w;
		ui_text_in(ui, ui_font(ui, 14, current ? 600 : 500), cut_left(&c, w * 0.42f), tr->title, text, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 13, 400), cut_left(&c, w * 0.29f), tr->artist[0] ? tr->artist : "Unknown Artist", t->text_dim, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 13, 400), c, tr->album[0] ? tr->album : "Unknown Album", t->text_dim, ALIGN_START);
	} else {
		ui_text_in(ui, ui_font(ui, 14, current ? 600 : 500), c, tr->title, text, ALIGN_START);
	}
	if (it.double_clicked || (it.clicked && !show_album)) {
		if (show_album)
			play_all(i);
		else
			play_album_track(mu.album_view, i - mu.albums[mu.album_view].first_track);
	}
}

static void view_songs(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect head = cut_top(&r, 36);
	Rect hc = pad2(head, 24 + 12, 0);
	cut_left(&hc, 36);
	cut_right(&hc, 56);
	f32 w = hc.w;
	Font hf = ui_font(ui, 12, 600);
	ui_text_in(ui, hf, cut_left(&hc, w * 0.42f), "Title", t->text_dim, ALIGN_START);
	ui_text_in(ui, hf, cut_left(&hc, w * 0.29f), "Artist", t->text_dim, ALIGN_START);
	ui_text_in(ui, hf, hc, "Album", t->text_dim, ALIGN_START);
	ui_separator(ui, r.x, r.y, r.x + r.w);
	f32 y = ui_scroll_begin(ui, r, "songs");
	int k = 0;
	for (int i = 0; i < mu.ntracks; i++) {
		if (!matches(&mu.tracks[i]))
			continue;
		Rect row = rect(r.x + 24, y + (f32)k * 40, r.w - 48, 40);
		k++;
		if (row.y > r.y + r.h || row.y + row.h < r.y)
			continue;
		song_row(ui, row, i, true, k);
	}
	ui_scroll_end(ui, (f32)k * 40 + 20);
}

static void view_artists(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	f32 y = ui_scroll_begin(ui, r, "artists");
	f32 at = y + 16;
	for (int i = 0; i < mu.nalbums;) {
		const char *artist = mu.albums[i].artist;
		int j = i;
		while (j < mu.nalbums && str_eq_nocase(mu.albums[j].artist, artist))
			j++;
		if (!mu.search.len || str_find_nocase(artist, sb_str(&mu.search))) {
			ui_avatar(ui, rect(r.x + 24, at, 44, 44), artist);
			ui_text_in(ui, ui_font(ui, 17, 700), rect(r.x + 80, at + 2, r.w - 100, 24), artist, t->text, ALIGN_START);
			char sub[64];
			fmt(sub, sizeof sub, "%d album%s", j - i, j - i == 1 ? "" : "s");
			ui_text_in(ui, ui_font(ui, 13, 400), rect(r.x + 80, at + 24, r.w - 100, 18), sub, t->text_dim, ALIGN_START);
			at += 58;
			f32 x = r.x + 80;
			for (int k = i; k < j; k++) {
				if (x + 120 > r.x + r.w - 20) {
					x = r.x + 80;
					at += 160;
				}
				ui_push_id(ui, k);
				Rect c = rect(x, at, 120, 150);
				UiInteract it = ui_interact(ui, ui_id(ui, "a"), c);
				draw_cover(ui, rect(x, at, 120, 120), &mu.albums[k], 6);
				ui_text_in(ui, ui_font(ui, 12, 600), rect(x, at + 124, 120, 18), mu.albums[k].title, t->text, ALIGN_START);
				if (it.clicked) {
					mu.album_view = k;
					mu.view = VIEW_ALBUM;
				}
				ui_pop_id(ui);
				x += 136;
			}
			at += 168;
		}
		i = j;
	}
	ui_scroll_end(ui, at - y + 20);
}

static void view_album(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	if (mu.album_view < 0 || mu.album_view >= mu.nalbums) {
		mu.view = VIEW_ALBUMS;
		return;
	}
	Album *a = &mu.albums[mu.album_view];
	f32 y = ui_scroll_begin(ui, r, "album");
	/* A header washed in the cover's colour. */
	Rect hero = rect(r.x, y, r.w, 260);
	Color tint = a->cover ? a->tint : t->accent;
	ui_gradient(ui, hero, 0, color_mix(tint, t->window, t->dark ? 0.55f : 0.45f), t->window);
	Rect cov = rect(hero.x + 28, hero.y + 28, 204, 204);
	draw_cover(ui, cov, a, 10);
	Rect info = rect(cov.x + cov.w + 28, hero.y + 60, hero.w - cov.w - 90, 170);
	ui_text_in(ui, ui_font(ui, 12, 700), cut_top(&info, 18), "ALBUM", t->text_dim, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 30, 700), cut_top(&info, 42), a->title, t->text, ALIGN_START);
	char sub[200], dur[24];
	fmt_duration(dur, sizeof dur, a->duration);
	if (a->year)
		fmt(sub, sizeof sub, "%s · %d · %d song%s, %s", a->artist, a->year, a->count, a->count == 1 ? "" : "s", dur);
	else
		fmt(sub, sizeof sub, "%s · %d song%s, %s", a->artist, a->count, a->count == 1 ? "" : "s", dur);
	ui_text_in(ui, ui_font(ui, 15, 500), cut_top(&info, 24), sub, t->text_dim, ALIGN_START);
	cut_top(&info, 18);
	Rect btns = cut_top(&info, 38);
	if (ui_button_icon_label(ui, cut_left(&btns, 110), "media-play", "Play"))
		play_album_track(mu.album_view, 0);
	cut_left(&btns, 10);
	if (ui_button_icon_label(ui, cut_left(&btns, 120), "media-shuffle", "Shuffle")) {
		mu.shuffle = true;
		play_album_track(mu.album_view, (int)(rand_u32() % (u32)a->count));
	}
	f32 ty = hero.y + hero.h + 8;
	for (int i = 0; i < a->count; i++) {
		int ti = a->first_track + i;
		Track *tr = &mu.tracks[ti];
		song_row(ui, rect(r.x + 24, ty + (f32)i * 40, r.w - 48, 40), ti, false, tr->track ? tr->track : i + 1);
	}
	ui_scroll_end(ui, hero.h + 8 + (f32)a->count * 40 + 24);
}

static void player_bar(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_fill(ui, r, t->sidebar);
	ui_separator(ui, r.x, r.y, r.x + r.w);
	mutex_lock(&mu.lock);
	int track = mu.playing_track;
	f64 pos = mu.position, dur = mu.duration;
	bool paused = mu.paused;
	mutex_unlock(&mu.lock);
	Rect c = pad2(r, 16, 12);
	/* Now playing. */
	Rect now = cut_left(&c, minf(320, c.w * 0.3f));
	if (track >= 0 && track < mu.ntracks) {
		Track *tr = &mu.tracks[track];
		Album *a = &mu.albums[tr->album_index];
		draw_cover(ui, rect(now.x, now.y + (now.h - 52) / 2, 52, 52), a, 6);
		ui_text_in(ui, ui_font(ui, 14, 600), rect(now.x + 64, now.y + now.h / 2 - 20, now.w - 64, 20), tr->title, t->text, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 13, 400), rect(now.x + 64, now.y + now.h / 2 + 2, now.w - 64, 18), tr->artist[0] ? tr->artist : "Unknown Artist",
			   t->text_dim, ALIGN_START);
		UiInteract it = ui_interact(ui, ui_id(ui, "now"), rect(now.x, now.y, 52, now.h));
		if (it.clicked) {
			mu.album_view = tr->album_index;
			mu.view = VIEW_ALBUM;
		}
	} else {
		ui_text_in(ui, ui_font(ui, 13, 400), now, mu.ntracks ? "Pick something to play" : "", t->text_dim, ALIGN_START);
	}
	/* Volume on the right. */
	Rect vol = cut_right(&c, minf(170, c.w * 0.22f));
	ui_icon(ui, center_in(cut_left(&vol, 22), 16, 16), mu.volume < 0.01f ? "audio-volume-muted" : "audio-volume-medium", t->text_dim);
	if (ui_slider(ui, center_in(vol, vol.w - 8, 24), &mu.volume, 0, 1))
		audio_set_volume(mu.stream, mu.volume);
	/* Controls and seek bar in the middle. */
	Rect mid = pad2(c, 24, 0);
	Rect ctl = center_in(cut_top(&mid, 36), 230, 36);
	if (ui_icon_toggle(ui, cut_left(&ctl, 36), "media-shuffle", mu.shuffle, "Shuffle"))
		mu.shuffle = !mu.shuffle;
	cut_left(&ctl, 6);
	if (ui_icon_button(ui, cut_left(&ctl, 36), "media-previous", "Previous"))
		prev_track();
	cut_left(&ctl, 6);
	Rect pp = cut_left(&ctl, 38);
	UiInteract pit = ui_interact(ui, ui_id(ui, "pp"), pp);
	ui_circle(ui, pp.x + 19, pp.y + 18, 18, pit.hovered ? t->accent_hover : t->accent);
	ui_icon(ui, center_in(pp, 18, 18), track >= 0 && !paused ? "media-pause" : "media-play", t->on_accent);
	if (pit.clicked)
		toggle_pause();
	cut_left(&ctl, 6);
	if (ui_icon_button(ui, cut_left(&ctl, 36), "media-next", "Next"))
		next_track(true);
	cut_left(&ctl, 6);
	static const char *const repeat_icons[] = { "media-repeat", "media-repeat", "media-repeat-one" };
	if (ui_icon_toggle(ui, cut_left(&ctl, 36), repeat_icons[mu.repeat], mu.repeat != REPEAT_OFF,
			   mu.repeat == REPEAT_OFF ? "Repeat" : mu.repeat == REPEAT_ALL ? "Repeating all" : "Repeating this song"))
		mu.repeat = (mu.repeat + 1) % 3;
	Rect seek = cut_top(&mid, 22);
	char a[16], b[16];
	fmt_duration(a, sizeof a, pos);
	fmt_duration(b, sizeof b, dur);
	ui_text_in(ui, ui_font(ui, 11, 500), cut_left(&seek, 42), track >= 0 ? a : "", t->text_dim, ALIGN_START);
	ui_text_in(ui, ui_font(ui, 11, 500), cut_right(&seek, 42), track >= 0 ? b : "", t->text_dim, ALIGN_END);
	f32 v = dur > 0 ? (f32)(pos / dur) : 0;
	if (ui_slider(ui, seek, &v, 0, 1) && track >= 0 && dur > 0) {
		mutex_lock(&mu.lock);
		mu.position = v * dur;
		mutex_unlock(&mu.lock);
		send_cmd(paused ? CMD_NONE : CMD_SEEK, -1, v * dur);
	}
}

static void empty_state(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect c = center_in(r, minf(440, r.w), 260);
	Rect ir = center_in(cut_top(&c, 110), 110, 110);
	ui_gradient(ui, ir, 26, color_hex("#ff8fa3"), color_hex("#c06cff"));
	ui_icon(ui, center_in(ir, 54, 54), "music", COLOR_WHITE);
	cut_top(&c, 18);
	bool scanning = atomic_load(&mu.scan_state) == 1;
	ui_text_in(ui, ui_font(ui, 20, 700), cut_top(&c, 30), scanning ? "Looking for music…" : "Your music goes here", t->text, ALIGN_CENTER);
	ui_text_wrapped(ui, ui_font(ui, 14, 400), cut_top(&c, 44),
			"Put FLAC, Ogg or WAV files in the Music folder in your home, and they show up here by album.", t->text_dim, ALIGN_CENTER);
	cut_top(&c, 10);
	if (!scanning && ui_button_suggested(ui, center_in(cut_top(&c, 36), 200, 36), "Open Music Folder")) {
		char *dir = path_join(home_dir(), "Music");
		dir_make(dir, 0755);
		ui_launch(ui, "vault", dir);
		mem_free(dir);
	}
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	if (atomic_load(&mu.scan_state) == 2)
		library_ready();
	mutex_lock(&mu.lock);
	bool ended = mu.track_ended;
	mu.track_ended = false;
	mutex_unlock(&mu.lock);
	if (ended)
		next_track(false);
	/* Keyboard: space plays/pauses, arrows move through the song. */
	if (!ui_is_focused(ui, "search")) {
		if (ui_key_pressed(ui, KEY_SPACE, 0))
			toggle_pause();
		if (ui_key_pressed(ui, KEY_RIGHT, 0) && mu.playing_track >= 0)
			send_cmd(CMD_SEEK, -1, minf((f32)(mu.position + 10), (f32)mu.duration));
		if (ui_key_pressed(ui, KEY_LEFT, 0) && mu.playing_track >= 0)
			send_cmd(CMD_SEEK, -1, maxf((f32)(mu.position - 10), 0));
	}
	if (ui_key_pressed(ui, KEY_PLAYPAUSE, 0))
		toggle_pause();
	if (ui_key_pressed(ui, KEY_NEXTSONG, 0))
		next_track(true);
	if (ui_key_pressed(ui, KEY_PREVIOUSSONG, 0))
		prev_track();

	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	Rect hb = ui_headerbar(ui, header, NULL, 0);
	/* View switcher in the middle of the header, search on the right. */
	const char *views[] = { "Albums", "Songs", "Artists" };
	int sel = mu.view == VIEW_ALBUM ? VIEW_ALBUMS : mu.view;
	if (mu.view == VIEW_ALBUM && ui_button_icon_label(ui, rect(hb.x, hb.y + 7, 96, 32), "go-back", "Albums"))
		mu.view = VIEW_ALBUMS;
	if (ui_segmented(ui, center_in(hb, 270, 32), views, 3, &sel))
		mu.view = sel;
	ui_entry(ui, rect(hb.x + hb.w - 220, hb.y + 7, 220, 32), "search", &mu.search, "Search music", ENTRY_SEARCH);
	Rect bar = cut_bottom(&win, 76);
	if (!mu.ntracks) {
		empty_state(ui, win);
	} else {
		if (mu.unsupported && mu.view != VIEW_ALBUM) {
			Rect note = cut_top(&win, 30);
			ui_fill(ui, note, color_with_alpha(t->warning, 0x22));
			char m[160];
			fmt(m, sizeof m, "%d song%s in MP3 or AAC can't be played yet.", mu.unsupported, mu.unsupported == 1 ? "" : "s");
			ui_text_in(ui, ui_font(ui, 12, 500), note, m, t->text_dim, ALIGN_CENTER);
		}
		switch (mu.view) {
		case VIEW_ALBUMS: view_albums(ui, win); break;
		case VIEW_SONGS: view_songs(ui, win); break;
		case VIEW_ARTISTS: view_artists(ui, win); break;
		case VIEW_ALBUM: view_album(ui, win); break;
		}
	}
	player_bar(ui, bar);
	if (mu.error[0]) {
		Font f = ui_font(ui, 13, 600);
		f32 w = ui_text_width(ui, f, mu.error) + 30;
		Rect m = rect((ui->width - w) / 2, bar.y - 46, w, 34);
		ui_round(ui, m, 17, color_hex("#000000c8"));
		ui_text_in(ui, f, m, mu.error, COLOR_WHITE, ALIGN_CENTER);
	}
	if (mu.playing_track >= 0 && !mu.paused)
		ui->wake_at = ui->now + 0.25;
	/* songs dropped on the window play */
	if (ui_drop_zone(ui, rect(0, 0, ui->width, ui->height), "music", "Drop to play")) {
		char *p = ui_dropped_file(ui, "mp3,flac,ogg,oga,opus,wav,m4a");
		if (p) {
			mem_free(mu.open_path);
			mu.open_path = p;
			if (atomic_load(&mu.scan_state) == 0)
				play_opened();
		}
	}
}

static void init(Ui *ui)
{
	mu.ui = ui;
	fmt(cache_path, sizeof cache_path, "%s/.cache/zurbax/music-library.tsv", home_dir());
	thread_spawn(player_thread, NULL);
	rescan();
}

int main(int argc, char **argv)
{
	if (argc > 1 && file_exists(argv[1]))
		mu.open_path = str_dup(argv[1]);
	UiAppDesc d = { .id = "music", .title = "Music", .width = 1040, .height = 700, .min_width = 720, .min_height = 480, .flags = WIN_RESIZABLE,
			.init = init, .frame = frame };
	return ui_run(&d);
}
