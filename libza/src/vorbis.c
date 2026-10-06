/*
 * Ogg Vorbis decoding (Vorbis I): Ogg paging, Huffman codebooks with vector
 * lookup, floor 1, residues 0-2, channel coupling, an FFT-based inverse
 * MDCT and windowed overlap-add. Floor 0, which no encoder has produced in
 * twenty years, is not supported.
 */
#include "internal.h"

/* ---- Ogg ------------------------------------------------------------------------------------- */

typedef struct {
	const u8 *data;
	usize len, pos;          /* next page */
	u32 serial;
	bool have_serial;
	/* the page being read */
	const u8 *seg_table;
	int nseg, seg;
	const u8 *body;
	usize body_off;
	i64 granule;
	bool eos;
	u8 *packet;
	usize plen, pcap;
} Ogg;

static bool next_page(Ogg *o)
{
	while (o->pos + 27 <= o->len) {
		const u8 *p = o->data + o->pos;
		if (memcmp(p, "OggS", 4) != 0) {
			o->pos++;
			continue;
		}
		int nseg = p[26];
		if (o->pos + 27 + (usize)nseg > o->len)
			return false;
		usize body = 0;
		for (int i = 0; i < nseg; i++)
			body += p[27 + i];
		if (o->pos + 27 + (usize)nseg + body > o->len)
			return false;
		u32 serial = rd_le32(p + 14);
		usize here = o->pos;
		o->pos += 27 + (usize)nseg + body;
		if (!o->have_serial) {
			o->serial = serial;
			o->have_serial = true;
		} else if (serial != o->serial) {
			continue; /* another logical stream */
		}
		o->seg_table = p + 27;
		o->nseg = nseg;
		o->seg = 0;
		o->body = o->data + here + 27 + nseg;
		o->body_off = 0;
		o->granule = (i64)((u64)rd_le32(p + 6) | (u64)rd_le32(p + 10) << 32);
		o->eos = p[5] & 4;
		return true;
	}
	return false;
}

/* Assembles the next packet. */
static bool next_packet(Ogg *o)
{
	o->plen = 0;
	for (;;) {
		if (o->seg >= o->nseg) {
			if (!next_page(o))
				return false;
			continue;
		}
		int l = o->seg_table[o->seg++];
		if (o->plen + (usize)l > o->pcap) {
			o->pcap = MAX(o->pcap * 2, o->plen + (usize)l + 4096);
			o->packet = mem_realloc(o->packet, o->pcap);
		}
		memcpy(o->packet + o->plen, o->body + o->body_off, (usize)l);
		o->plen += (usize)l;
		o->body_off += (usize)l;
		if (l < 255)
			return true;
	}
}

/* ---- Bits (least significant first) -------------------------------------------------------------- */

typedef struct {
	const u8 *p;
	usize len, bitpos;
	bool eop;
} Bits;

static u32 getb(Bits *b, int n)
{
	u32 v = 0;
	for (int i = 0; i < n; i++) {
		usize byte = b->bitpos >> 3;
		if (byte >= b->len) {
			b->eop = true;
			return 0;
		}
		v |= (u32)((b->p[byte] >> (b->bitpos & 7)) & 1) << i;
		b->bitpos++;
	}
	return v;
}

/* Up to 24 bits without consuming them (zeros past the end). */
static u32 peekb(Bits *b, int n)
{
	usize byte = b->bitpos >> 3;
	u32 v = 0;
	for (int i = 0; i < 4; i++)
		if (byte + (usize)i < b->len)
			v |= (u32)b->p[byte + (usize)i] << (8 * i);
	v >>= b->bitpos & 7;
	return v & ((1u << n) - 1);
}

static int ilog(u32 v)
{
	int n = 0;
	while (v) {
		n++;
		v >>= 1;
	}
	return n;
}

/* ---- Setup -------------------------------------------------------------------------------------- */

#define FAST_BITS 10

typedef struct {
	int dims, entries;
	u8 *lengths;
	u32 *codes;              /* bit-reversed, for least-significant-first reading */
	i16 fast[1 << FAST_BITS];/* entry for short codes, -1 if longer/none */
	int *sorted;             /* used entries by reversed code, for long codes */
	int nsorted;
	int single;              /* the one used entry of a degenerate book, or -1 */
	f32 *vq;                 /* entries * dims values, if the book has a lookup */
} Book;

typedef struct {
	int partitions;
	u8 pclass[32];
	u8 cdim[16], csub[16];
	i16 cmaster[16], subbooks[16][8];
	int multiplier, values;
	u16 x[65];
	u8 sorted[65];           /* indices of x in increasing order */
	u8 low[65], high[65];    /* neighbours */
} Floor;

typedef struct {
	int type;
	u32 begin, end, psize;
	int classes, classbook;
	i16 books[64][8];
} Residue;

typedef struct {
	int submaps, steps;
	u8 mag[256], ang[256];
	u8 mux[256];
	u8 floor[16], residue[16];
} Mapping;

typedef struct {
	bool block;
	int mapping;
} Mode;

typedef struct {
	Ogg ogg;
	int bs[2];
	Book *books;
	int nbooks;
	Floor *floors;
	int nfloors;
	Residue *residues;
	int nresidues;
	Mapping *maps;
	int nmaps;
	Mode modes[64];
	int nmodes;
	f32 *window[2];          /* the slope of each block size's window */
	f32 *out[8];             /* this block, per channel */
	f32 *prev[8];            /* the previous block, windowed */
	int prev_n;
	bool have_prev;
	f32 *floorv[8];
	u8 *classify;
	/* MDCT */
	f32 *tw_cos[2], *tw_sin[2], *fft_buf;
	f32 *fft_c[2], *fft_s[2];
	i64 audio_page_pos;      /* where the audio begins in the file */
	u64 decoded;             /* frames returned so far, for trimming at the end */
} Vorbis;

static f32 float32_unpack(u32 x)
{
	f64 mant = x & 0x1fffff;
	int exp = (int)((x & 0x7fe00000) >> 21);
	if (x & 0x80000000)
		mant = -mant;
	return (f32)ldexp(mant, exp - 788);
}

static int lookup1_values(int entries, int dims)
{
	int r = (int)floor(exp(log((f64)entries) / dims));
	while (pow(r + 1, dims) <= entries)
		r++;
	while (r > 0 && pow(r, dims) > entries)
		r--;
	return r;
}

static u32 reverse_bits(u32 v, int n)
{
	u32 r = 0;
	for (int i = 0; i < n; i++)
		r |= ((v >> i) & 1) << (n - 1 - i);
	return r;
}

static bool build_codes(Book *b)
{
	u32 marker[33] = { 0 };
	int used = 0, last = -1;
	for (int i = 0; i < b->entries; i++)
		if (b->lengths[i]) {
			used++;
			last = i;
		}
	b->single = -1;
	for (int i = 0; i < (1 << FAST_BITS); i++)
		b->fast[i] = -1;
	b->codes = NEW_ARRAY(u32, b->entries);
	if (used == 1) {
		b->single = last;
		return true;
	}
	for (int i = 0; i < b->entries; i++) {
		int len = b->lengths[i];
		if (!len)
			continue;
		u32 entry = marker[len];
		if (len < 32 && (entry >> len))
			return false; /* overpopulated tree */
		b->codes[i] = reverse_bits(entry, len);
		for (int j = len; j > 0; j--) {
			if (marker[j] & 1) {
				if (j == 1)
					marker[1]++;
				else
					marker[j] = marker[j - 1] << 1;
				break;
			}
			marker[j]++;
		}
		for (int j = len + 1; j < 33; j++) {
			if ((marker[j] >> 1) == entry) {
				entry = marker[j];
				marker[j] = marker[j - 1] << 1;
			} else {
				break;
			}
		}
	}
	/* The fast table for short codes; longer ones are searched. */
	b->sorted = NEW_ARRAY(int, used);
	for (int i = 0; i < b->entries; i++) {
		int len = b->lengths[i];
		if (!len)
			continue;
		if (len <= FAST_BITS) {
			for (u32 fill = b->codes[i]; fill < (1u << FAST_BITS); fill += 1u << len)
				b->fast[fill] = (i16)i;
		} else {
			b->sorted[b->nsorted++] = i;
		}
	}
	return true;
}

static int decode_scalar(Bits *bits, const Book *b)
{
	if (b->single >= 0) {
		getb(bits, b->lengths[b->single]);
		return b->single;
	}
	u32 peek = peekb(bits, FAST_BITS);
	int e = b->fast[peek];
	if (e >= 0) {
		bits->bitpos += b->lengths[e];
		if ((bits->bitpos >> 3) > bits->len)
			bits->eop = true;
		return e;
	}
	/* Long codes: compare against each candidate (they're rare). */
	u32 window = 0;
	usize byte = bits->bitpos >> 3;
	u64 acc = 0;
	for (int i = 0; i < 5; i++)
		if (byte + (usize)i < bits->len)
			acc |= (u64)bits->p[byte + (usize)i] << (8 * i);
	acc >>= bits->bitpos & 7;
	window = (u32)acc;
	for (int k = 0; k < b->nsorted; k++) {
		int i = b->sorted[k];
		int len = b->lengths[i];
		u32 mask = len >= 32 ? 0xffffffffu : (1u << len) - 1;
		if ((window & mask) == b->codes[i]) {
			bits->bitpos += (usize)len;
			if ((bits->bitpos >> 3) > bits->len)
				bits->eop = true;
			return i;
		}
	}
	bits->eop = true;
	return -1;
}

static bool read_book(Bits *b, Book *k)
{
	if (getb(b, 24) != 0x564342)
		return false;
	k->dims = (int)getb(b, 16);
	k->entries = (int)getb(b, 24);
	if (k->dims < 1 || k->entries < 1)
		return false;
	k->lengths = NEW_ARRAY(u8, k->entries);
	if (getb(b, 1)) { /* ordered */
		int len = (int)getb(b, 5) + 1, cur = 0;
		while (cur < k->entries) {
			int num = (int)getb(b, ilog((u32)(k->entries - cur)));
			if (cur + num > k->entries || len > 32)
				return false;
			memset(k->lengths + cur, len, (usize)num);
			cur += num;
			len++;
		}
	} else {
		bool sparse = getb(b, 1);
		for (int i = 0; i < k->entries; i++) {
			if (sparse && !getb(b, 1))
				continue;
			k->lengths[i] = (u8)(getb(b, 5) + 1);
		}
	}
	int lookup = (int)getb(b, 4);
	if (lookup == 1 || lookup == 2) {
		f32 minv = float32_unpack(getb(b, 32)), delta = float32_unpack(getb(b, 32));
		int vbits = (int)getb(b, 4) + 1;
		bool seq = getb(b, 1);
		int nvals = lookup == 1 ? lookup1_values(k->entries, k->dims) : k->entries * k->dims;
		if (nvals <= 0 || nvals > 1 << 20)
			return false;
		u32 *mult = NEW_ARRAY(u32, nvals);
		for (int i = 0; i < nvals; i++)
			mult[i] = getb(b, vbits);
		k->vq = NEW_ARRAY(f32, (usize)k->entries * (usize)k->dims);
		for (int e = 0; e < k->entries; e++) {
			f32 last = 0;
			int div = 1;
			for (int d = 0; d < k->dims; d++) {
				int idx = lookup == 1 ? (e / div) % nvals : e * k->dims + d;
				f32 v = (f32)mult[idx] * delta + minv + last;
				k->vq[e * k->dims + d] = v;
				if (seq)
					last = v;
				if (lookup == 1)
					div *= nvals;
			}
		}
		mem_free(mult);
	} else if (lookup != 0) {
		return false;
	}
	return !b->eop && build_codes(k);
}

static bool read_floor(Bits *b, Floor *f, int nbooks)
{
	f->partitions = (int)getb(b, 5);
	int maxc = -1;
	for (int i = 0; i < f->partitions; i++) {
		f->pclass[i] = (u8)getb(b, 4);
		maxc = MAX(maxc, f->pclass[i]);
	}
	for (int c = 0; c <= maxc; c++) {
		f->cdim[c] = (u8)(getb(b, 3) + 1);
		f->csub[c] = (u8)getb(b, 2);
		f->cmaster[c] = f->csub[c] ? (i16)getb(b, 8) : -1;
		if (f->cmaster[c] >= nbooks)
			return false;
		for (int k = 0; k < (1 << f->csub[c]); k++) {
			f->subbooks[c][k] = (i16)((int)getb(b, 8) - 1);
			if (f->subbooks[c][k] >= nbooks)
				return false;
		}
	}
	f->multiplier = (int)getb(b, 2) + 1;
	int rangebits = (int)getb(b, 4);
	f->x[0] = 0;
	f->x[1] = (u16)(1 << rangebits);
	f->values = 2;
	for (int i = 0; i < f->partitions; i++)
		for (int j = 0; j < f->cdim[f->pclass[i]]; j++) {
			if (f->values >= 65)
				return false;
			f->x[f->values++] = (u16)getb(b, rangebits);
		}
	/* Sort order and neighbours, needed for every packet. */
	for (int i = 0; i < f->values; i++)
		f->sorted[i] = (u8)i;
	for (int i = 1; i < f->values; i++)
		for (int j = i; j > 0 && f->x[f->sorted[j - 1]] > f->x[f->sorted[j]]; j--) {
			u8 t = f->sorted[j];
			f->sorted[j] = f->sorted[j - 1];
			f->sorted[j - 1] = t;
		}
	for (int i = 2; i < f->values; i++) {
		int lo = 0, hi = 1, lx = -1, hx = 1 << 30;
		for (int j = 0; j < i; j++) {
			if (f->x[j] < f->x[i] && f->x[j] > lx)
				lx = f->x[j], lo = j;
			if (f->x[j] > f->x[i] && f->x[j] < hx)
				hx = f->x[j], hi = j;
		}
		f->low[i] = (u8)lo;
		f->high[i] = (u8)hi;
	}
	return !b->eop;
}

static bool read_residue(Bits *b, Residue *r, int nbooks)
{
	r->begin = getb(b, 24);
	r->end = getb(b, 24);
	r->psize = getb(b, 24) + 1;
	r->classes = (int)getb(b, 6) + 1;
	r->classbook = (int)getb(b, 8);
	if (r->classbook >= nbooks)
		return false;
	u8 cascade[64];
	for (int i = 0; i < r->classes; i++) {
		int low = (int)getb(b, 3);
		int high = getb(b, 1) ? (int)getb(b, 5) : 0;
		cascade[i] = (u8)(high * 8 + low);
	}
	for (int i = 0; i < r->classes; i++)
		for (int j = 0; j < 8; j++) {
			r->books[i][j] = (cascade[i] >> j) & 1 ? (i16)getb(b, 8) : -1;
			if (r->books[i][j] >= nbooks)
				return false;
		}
	return !b->eop;
}

static bool read_setup(Vorbis *v, Bits *b, int channels)
{
	v->nbooks = (int)getb(b, 8) + 1;
	v->books = NEW_ARRAY(Book, v->nbooks);
	for (int i = 0; i < v->nbooks; i++)
		if (!read_book(b, &v->books[i]))
			return false;
	int times = (int)getb(b, 6) + 1;
	for (int i = 0; i < times; i++)
		if (getb(b, 16) != 0)
			return false;
	v->nfloors = (int)getb(b, 6) + 1;
	v->floors = NEW_ARRAY(Floor, v->nfloors);
	for (int i = 0; i < v->nfloors; i++) {
		if (getb(b, 16) != 1)
			return false; /* floor 0 */
		if (!read_floor(b, &v->floors[i], v->nbooks))
			return false;
	}
	v->nresidues = (int)getb(b, 6) + 1;
	v->residues = NEW_ARRAY(Residue, v->nresidues);
	for (int i = 0; i < v->nresidues; i++) {
		v->residues[i].type = (int)getb(b, 16);
		if (v->residues[i].type > 2 || !read_residue(b, &v->residues[i], v->nbooks))
			return false;
	}
	v->nmaps = (int)getb(b, 6) + 1;
	v->maps = NEW_ARRAY(Mapping, v->nmaps);
	for (int i = 0; i < v->nmaps; i++) {
		Mapping *m = &v->maps[i];
		if (getb(b, 16) != 0)
			return false;
		m->submaps = getb(b, 1) ? (int)getb(b, 4) + 1 : 1;
		m->steps = getb(b, 1) ? (int)getb(b, 8) + 1 : 0;
		int cb = ilog((u32)(channels - 1));
		for (int s = 0; s < m->steps; s++) {
			m->mag[s] = (u8)getb(b, cb);
			m->ang[s] = (u8)getb(b, cb);
			if (m->mag[s] >= channels || m->ang[s] >= channels || m->mag[s] == m->ang[s])
				return false;
		}
		if (getb(b, 2) != 0)
			return false;
		for (int c = 0; c < channels; c++)
			m->mux[c] = m->submaps > 1 ? (u8)getb(b, 4) : 0;
		for (int s = 0; s < m->submaps; s++) {
			getb(b, 8);
			m->floor[s] = (u8)getb(b, 8);
			m->residue[s] = (u8)getb(b, 8);
			if (m->floor[s] >= v->nfloors || m->residue[s] >= v->nresidues)
				return false;
		}
	}
	v->nmodes = (int)getb(b, 6) + 1;
	for (int i = 0; i < v->nmodes; i++) {
		v->modes[i].block = getb(b, 1);
		getb(b, 16);
		getb(b, 16);
		v->modes[i].mapping = (int)getb(b, 8);
		if (v->modes[i].mapping >= v->nmaps)
			return false;
	}
	return getb(b, 1) == 1 && !b->eop;
}

/* ---- Inverse MDCT ----------------------------------------------------------------------------------- */

/* In-place radix-2 FFT; tw holds exp(-2*pi*i*k/n) for k < n/2 (cos, then sin). */
static void fft(f32 *re, f32 *im, int n, const f32 *twc, const f32 *tws)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			f32 t = re[i];
			re[i] = re[j], re[j] = t;
			t = im[i];
			im[i] = im[j], im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		int step = n / len;
		for (int i = 0; i < n; i += len)
			for (int k = 0; k < len / 2; k++) {
				f32 cr = twc[k * step], ci = tws[k * step];
				int a = i + k, b = i + k + len / 2;
				f32 xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
				re[b] = re[a] - xr, im[b] = im[a] - xi;
				re[a] += xr, im[a] += xi;
			}
	}
}

/* DCT-IV of size m through an m/2-point complex FFT. */
static void dct4(Vorbis *v, int which, const f32 *x, f32 *u, int m)
{
	int h = m / 2;
	f32 *re = v->fft_buf, *im = v->fft_buf + h;
	const f32 *c = v->tw_cos[which], *s = v->tw_sin[which];
	for (int k = 0; k < h; k++) {
		f32 a = x[2 * k], b = x[m - 1 - 2 * k];
		re[k] = a * c[k] + b * s[k];
		im[k] = b * c[k] - a * s[k];
	}
	fft(re, im, h, v->fft_c[which], v->fft_s[which]);
	for (int k = 0; k < h; k++) {
		f32 a = re[k], b = im[k];
		f32 yr = a * c[k] + b * s[k], yi = b * c[k] - a * s[k];
		u[2 * k] = yr;
		u[m - 1 - 2 * k] = -yi;
	}
}

static void prepare_mdct(Vorbis *v, int which, int n)
{
	int m = n / 2, h = m / 2;
	v->tw_cos[which] = NEW_ARRAY(f32, h);
	v->tw_sin[which] = NEW_ARRAY(f32, h);
	for (int k = 0; k < h; k++) {
		f64 a = PI * (4 * k + 1) / (4.0 * m);
		v->tw_cos[which][k] = (f32)cos(a);
		v->tw_sin[which][k] = (f32)sin(a);
	}
	v->fft_c[which] = NEW_ARRAY(f32, h / 2 + 1);
	v->fft_s[which] = NEW_ARRAY(f32, h / 2 + 1);
	for (int k = 0; k < h / 2; k++) {
		f64 a = -2 * PI * k / h;
		v->fft_c[which][k] = (f32)cos(a);
		v->fft_s[which][k] = (f32)sin(a);
	}
}

/* y[0..n) from n/2 coefficients, by the DCT-IV and its symmetries. */
static void imdct(Vorbis *v, int which, const f32 *x, f32 *y, int n)
{
	int m = n / 2;
	f32 *u = y + m; /* scratch: the second half is overwritten last */
	f32 tmp[4096];
	dct4(v, which, x, tmp, m);
	UNUSED(u);
	for (int i = 0; i < n; i++) {
		int k = i + m / 2;
		f32 val;
		if (k < m)
			val = tmp[k];
		else if (k < 2 * m)
			val = -tmp[2 * m - 1 - k];
		else
			val = -tmp[k - 2 * m];
		y[i] = val;
	}
}

/* ---- Packets ------------------------------------------------------------------------------------- */

static const f32 *inverse_db(void)
{
	static f32 table[256];
	if (!table[255])
		for (int i = 0; i < 256; i++)
			table[i] = (f32)pow(10.0, -7.0 * (255 - i) / 256.0);
	return table;
}

static int render_point(int x0, int y0, int x1, int y1, int x)
{
	int dy = y1 - y0, adx = x1 - x0, ady = dy < 0 ? -dy : dy;
	int off = ady * (x - x0) / adx;
	return dy < 0 ? y0 - off : y0 + off;
}

static void render_line(int x0, int y0, int x1, int y1, f32 *v, int n)
{
	const f32 *db = inverse_db();
	int dy = y1 - y0, adx = x1 - x0, ady = dy < 0 ? -dy : dy;
	int base = dy / adx, sy = dy < 0 ? base - 1 : base + 1;
	int absbase = base < 0 ? -base : base;
	ady -= absbase * adx;
	int y = y0, err = 0;
	if (x0 < n)
		v[x0] *= db[CLAMP(y, 0, 255)];
	for (int x = x0 + 1; x < x1 && x < n; x++) {
		err += ady;
		if (err >= adx) {
			err -= adx;
			y += sy;
		} else {
			y += base;
		}
		v[x] *= db[CLAMP(y, 0, 255)];
	}
}

/* Decodes a channel's floor; false if the channel is silent in this packet. */
static bool floor_decode(Vorbis *v, Bits *b, const Floor *f, int *y)
{
	if (!getb(b, 1))
		return false;
	static const int ranges[] = { 256, 128, 86, 64 };
	int range = ranges[f->multiplier - 1];
	int rb = ilog((u32)(range - 1));
	y[0] = (int)getb(b, rb);
	y[1] = (int)getb(b, rb);
	int off = 2;
	for (int i = 0; i < f->partitions; i++) {
		int c = f->pclass[i], cdim = f->cdim[c], cbits = f->csub[c], csub = (1 << cbits) - 1, cval = 0;
		if (cbits) {
			cval = decode_scalar(b, &v->books[f->cmaster[c]]);
			if (cval < 0)
				return false;
		}
		for (int j = 0; j < cdim; j++) {
			int book = f->subbooks[c][cval & csub];
			cval >>= cbits;
			y[off + j] = book >= 0 ? decode_scalar(b, &v->books[book]) : 0;
			if (y[off + j] < 0)
				return false;
		}
		off += cdim;
	}
	return !b->eop;
}

static void floor_curve(const Floor *f, int *y, f32 *out, int n)
{
	static const int ranges[] = { 256, 128, 86, 64 };
	int range = ranges[f->multiplier - 1];
	bool step2[65];
	int fy[65];
	step2[0] = step2[1] = true;
	fy[0] = y[0], fy[1] = y[1];
	for (int i = 2; i < f->values; i++) {
		int lo = f->low[i], hi = f->high[i];
		int pred = render_point(f->x[lo], fy[lo], f->x[hi], fy[hi], f->x[i]);
		int val = y[i], highroom = range - pred, lowroom = pred;
		int room = (highroom < lowroom ? highroom : lowroom) * 2;
		if (val) {
			step2[lo] = step2[hi] = step2[i] = true;
			if (val >= room)
				fy[i] = highroom > lowroom ? val - lowroom + pred : pred - val + highroom - 1;
			else
				fy[i] = (val & 1) ? pred - (val + 1) / 2 : pred + val / 2;
		} else {
			step2[i] = false;
			fy[i] = pred;
		}
	}
	int lx = 0, ly = fy[f->sorted[0]] * f->multiplier, hx = 0, hy = 0;
	for (int k = 1; k < f->values; k++) {
		int i = f->sorted[k];
		if (step2[i]) {
			hy = fy[i] * f->multiplier;
			hx = f->x[i];
			render_line(lx, ly, hx, hy, out, n);
			lx = hx, ly = hy;
		}
	}
	if (hx < n) {
		const f32 *db = inverse_db();
		for (int x = hx; x < n; x++)
			out[x] *= db[CLAMP(hy, 0, 255)];
	}
}

static void residue_decode(Vorbis *v, Bits *b, const Residue *r, f32 **vecs, int nvec, const bool *skip, int n)
{
	int ch = nvec;
	usize actual = (usize)n / 2;
	f32 *single = NULL;
	f32 *rv[8];
	bool rskip[8];
	if (r->type == 2) {
		bool all = true;
		for (int i = 0; i < nvec; i++)
			all &= skip[i];
		if (all)
			return;
		actual *= (usize)nvec;
		single = mem_zalloc(sizeof(f32) * actual);
		rv[0] = single;
		rskip[0] = false;
		ch = 1;
	} else {
		for (int i = 0; i < nvec; i++)
			rv[i] = vecs[i], rskip[i] = skip[i];
	}
	usize lb = MIN(r->begin, (u32)actual), le = MIN(r->end, (u32)actual);
	const Book *cb = &v->books[r->classbook];
	int cpw = cb->dims;
	usize nread = le > lb ? le - lb : 0;
	int parts = (int)(nread / r->psize);
	if (parts > 0) {
		u8 *cls = mem_alloc((usize)ch * (usize)(parts + cpw));
		for (int pass = 0; pass < 8 && !b->eop; pass++) {
			int pc = 0;
			while (pc < parts && !b->eop) {
				if (pass == 0)
					for (int j = 0; j < ch; j++) {
						if (rskip[j])
							continue;
						int temp = decode_scalar(b, cb);
						if (temp < 0)
							goto done;
						for (int i = cpw - 1; i >= 0; i--) {
							cls[j * (parts + cpw) + i + pc] = (u8)(temp % r->classes);
							temp /= r->classes;
						}
					}
				for (int i = 0; i < cpw && pc < parts; i++, pc++)
					for (int j = 0; j < ch; j++) {
						if (rskip[j])
							continue;
						int book = r->books[cls[j * (parts + cpw) + pc]][pass];
						if (book < 0)
							continue;
						const Book *bk = &v->books[book];
						if (!bk->vq)
							continue;
						usize off = lb + (usize)pc * r->psize;
						f32 *out = rv[j] + off;
						int dims = bk->dims;
						if (r->type == 0) {
							int step = (int)r->psize / dims;
							for (int s = 0; s < step; s++) {
								int e = decode_scalar(b, bk);
								if (e < 0)
									goto done;
								for (int d = 0; d < dims; d++)
									out[s + d * step] += bk->vq[e * dims + d];
							}
						} else {
							for (usize s = 0; s < r->psize;) {
								int e = decode_scalar(b, bk);
								if (e < 0)
									goto done;
								for (int d = 0; d < dims && s < r->psize; d++)
									out[s++] += bk->vq[e * dims + d];
							}
						}
					}
			}
		}
	done:
		mem_free(cls);
	}
	if (single) {
		for (usize i = 0; i < actual; i++)
			vecs[i % (usize)nvec][i / (usize)nvec] += single[i];
		mem_free(single);
	}
}

static f32 slope(int i, int n)
{
	f64 x = sin((i + 0.5) / n * PI / 2);
	return (f32)sin(PI / 2 * x * x);
}

static bool decode_audio(Decoder *d, Vorbis *v, const u8 *pkt, usize plen)
{
	Bits b = { pkt, plen, 0, false };
	if (getb(&b, 1) != 0)
		return false;
	int mode = (int)getb(&b, ilog((u32)(v->nmodes - 1)));
	if (mode >= v->nmodes)
		return false;
	Mode *md = &v->modes[mode];
	int n = v->bs[md->block], half = n / 2;
	bool prev_long = true, next_long = true;
	if (md->block) {
		prev_long = getb(&b, 1);
		next_long = getb(&b, 1);
	}
	Mapping *mp = &v->maps[md->mapping];
	int ch = d->channels;
	bool silent[8];
	int y[65];
	for (int c = 0; c < ch; c++) {
		memset(v->out[c], 0, sizeof(f32) * (usize)n);
		const Floor *f = &v->floors[mp->floor[mp->mux[c]]];
		silent[c] = !floor_decode(v, &b, f, y);
		/* Keep the floor values to apply after the residue. */
		if (!silent[c]) {
			for (int i = 0; i < half; i++)
				v->floorv[c][i] = 1;
			floor_curve(f, y, v->floorv[c], half);
		}
	}
	bool skip[8];
	for (int c = 0; c < ch; c++)
		skip[c] = silent[c];
	for (int s = 0; s < mp->steps; s++)
		if (!skip[mp->mag[s]] || !skip[mp->ang[s]])
			skip[mp->mag[s]] = skip[mp->ang[s]] = false;
	for (int sm = 0; sm < mp->submaps; sm++) {
		f32 *vecs[8];
		bool sk[8];
		int nv = 0;
		for (int c = 0; c < ch; c++)
			if (mp->mux[c] == sm) {
				vecs[nv] = v->out[c];
				sk[nv++] = skip[c];
			}
		residue_decode(v, &b, &v->residues[mp->residue[sm]], vecs, nv, sk, n);
	}
	for (int s = mp->steps - 1; s >= 0; s--) {
		f32 *m = v->out[mp->mag[s]], *a = v->out[mp->ang[s]];
		for (int i = 0; i < half; i++) {
			f32 mv = m[i], av = a[i], nm, na;
			if (mv > 0) {
				if (av > 0)
					nm = mv, na = mv - av;
				else
					na = mv, nm = mv + av;
			} else {
				if (av > 0)
					nm = mv, na = mv + av;
				else
					na = mv, nm = mv - av;
			}
			m[i] = nm, a[i] = na;
		}
	}
	/* Floor times residue, then back to the time domain and windowed. */
	int bs0 = v->bs[0];
	int ls = 0, le = half, lw = half, rs = half, re = n, rw = half;
	if (md->block && !prev_long)
		ls = n / 4 - bs0 / 4, le = n / 4 + bs0 / 4, lw = bs0 / 2;
	if (md->block && !next_long)
		rs = n * 3 / 4 - bs0 / 4, re = n * 3 / 4 + bs0 / 4, rw = bs0 / 2;
	f32 spec[4096];
	for (int c = 0; c < ch; c++) {
		f32 *o = v->out[c];
		if (silent[c]) {
			memset(o, 0, sizeof(f32) * (usize)n);
		} else {
			for (int i = 0; i < half; i++)
				spec[i] = o[i] * v->floorv[c][i];
			imdct(v, md->block, spec, o, n);
		}
		for (int i = 0; i < ls; i++)
			o[i] = 0;
		for (int i = ls; i < le; i++)
			o[i] *= slope(i - ls, lw);
		for (int i = rs; i < re; i++)
			o[i] *= slope(rw - 1 - (i - rs), rw);
		for (int i = re; i < n; i++)
			o[i] = 0;
	}
	/* Overlap with the previous block: out[i] = prev[pn/2 + i] + cur[i + n/4 - pn/4]. */
	if (v->have_prev) {
		int pn = v->prev_n, count = pn / 4 + n / 4;
		i16 *pcm = pcm_reserve(d, (usize)count);
		for (int i = 0; i < count; i++)
			for (int c = 0; c < ch; c++) {
				f32 s = 0;
				if (pn / 2 + i < pn)
					s += v->prev[c][pn / 2 + i];
				int ci = i + n / 4 - pn / 4;
				if (ci >= 0 && ci < n)
					s += v->out[c][ci];
				pcm[i * ch + c] = (i16)CLAMP((int)roundf(s * 32767.0f), -32768, 32767);
			}
		usize keep = (usize)count;
		if (d->total && v->decoded + keep > d->total)
			keep = d->total > v->decoded ? (usize)(d->total - v->decoded) : 0;
		v->decoded += keep;
		d->pcm_frames += keep;
	}
	for (int c = 0; c < ch; c++) {
		f32 *t = v->prev[c];
		v->prev[c] = v->out[c];
		v->out[c] = t;
	}
	v->prev_n = n;
	v->have_prev = true;
	return true;
}

bool vorbis_decode_packet(Decoder *d)
{
	Vorbis *v = d->codec;
	while (next_packet(&v->ogg)) {
		if (v->ogg.plen == 0 || (v->ogg.packet[0] & 1))
			continue; /* empty, or a stray header */
		usize before = d->pcm_frames;
		if (!decode_audio(d, v, v->ogg.packet, v->ogg.plen))
			continue; /* a broken packet: skip it */
		if (d->pcm_frames > before)
			return true;
		if (d->total && v->decoded >= d->total)
			return false;
	}
	return false;
}

/* ---- Opening, seeking ---------------------------------------------------------------------------- */

static void picture_comment(const u8 *c, usize len, AudioInfo *info)
{
	/* METADATA_BLOCK_PICTURE=<base64 FLAC picture block> */
	static const char key[] = "METADATA_BLOCK_PICTURE=";
	if (len < sizeof key || !str_eq_nocase_n((const char *)c, key, sizeof key - 1) || info->cover)
		return;
	const u8 *s = c + sizeof key - 1;
	usize n = len - (sizeof key - 1);
	u8 *raw = mem_alloc(n);
	usize o = 0;
	u32 acc = 0;
	int bits = 0;
	for (usize i = 0; i < n; i++) {
		int v = s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' : s[i] >= 'a' && s[i] <= 'z' ? s[i] - 'a' + 26 : s[i] >= '0' && s[i] <= '9' ? s[i] - '0' + 52 :
			s[i] == '+' ? 62 : s[i] == '/' ? 63 : -1;
		if (v < 0)
			continue;
		acc = acc << 6 | (u32)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			raw[o++] = (u8)(acc >> bits);
		}
	}
	u32 q = 4;
	if (q + 4 <= o) {
		q += 4 + rd_be32(raw + q);
		if (q + 4 <= o) {
			q += 4 + rd_be32(raw + q) + 16;
			if (q + 4 <= o) {
				u32 il = rd_be32(raw + q);
				q += 4;
				if (q + il <= o) {
					info->cover = mem_dup(raw + q, il);
					info->cover_len = il;
				}
			}
		}
	}
	mem_free(raw);
}

static void comments_with_pictures(const u8 *p, usize len, AudioInfo *info, bool want_cover)
{
	vorbis_comments(p, len, info);
	if (!want_cover || len < 8)
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
		picture_comment(p + off, l, info);
		off += l;
	}
}

bool vorbis_open(Decoder *d, AudioInfo *info, bool want_cover)
{
	Vorbis *v = NEW(Vorbis);
	d->codec = v;
	v->ogg.data = d->data;
	v->ogg.len = d->len;
	/* Identification, comment and setup headers. */
	for (int h = 0; h < 3; h++) {
		if (!next_packet(&v->ogg) || v->ogg.plen < 7 || v->ogg.packet[0] != 1 + 2 * h || memcmp(v->ogg.packet + 1, "vorbis", 6) != 0) {
			vorbis_free(d);
			return false;
		}
		const u8 *p = v->ogg.packet + 7;
		usize len = v->ogg.plen - 7;
		if (h == 0) {
			if (len < 23 || rd_le32(p) != 0) {
				vorbis_free(d);
				return false;
			}
			d->channels = p[4];
			d->rate = (int)rd_le32(p + 5);
			info->bitrate = (int)rd_le32(p + 13) / 1000;
			v->bs[0] = 1 << (p[21] & 15);
			v->bs[1] = 1 << (p[21] >> 4);
			if (d->channels < 1 || d->channels > 8 || d->rate <= 0 || v->bs[0] < 64 || v->bs[1] > 8192 || v->bs[0] > v->bs[1]) {
				vorbis_free(d);
				return false;
			}
		} else if (h == 1) {
			comments_with_pictures(p, len, info, want_cover);
		} else {
			Bits b = { p, len, 0, false };
			if (!read_setup(v, &b, d->channels)) {
				vorbis_free(d);
				return false;
			}
		}
	}
	for (int c = 0; c < d->channels; c++) {
		v->out[c] = NEW_ARRAY(f32, v->bs[1]);
		v->prev[c] = NEW_ARRAY(f32, v->bs[1]);
		v->floorv[c] = NEW_ARRAY(f32, v->bs[1] / 2);
	}
	prepare_mdct(v, 0, v->bs[0]);
	prepare_mdct(v, 1, v->bs[1]);
	v->fft_buf = NEW_ARRAY(f32, v->bs[1] / 2);
	/* The length: the granule position of the last page. */
	usize stop = d->len > 70000 ? d->len - 70000 : 0;
	for (usize p = d->len - 27; p > stop; p--) {
		if (memcmp(d->data + p, "OggS", 4) == 0 && rd_le32(d->data + p + 14) == v->ogg.serial) {
			d->total = (u64)rd_le32(d->data + p + 6) | (u64)rd_le32(d->data + p + 10) << 32;
			break;
		}
	}
	v->audio_page_pos = (i64)v->ogg.pos;
	info->codec = "Vorbis";
	return true;
}

bool vorbis_seek(Decoder *d, u64 frame)
{
	Vorbis *v = d->codec;
	/* Bisect pages by granule position for the last page ending before the target. */
	usize lo = (usize)v->audio_page_pos, hi = d->len, best = lo;
	u64 best_granule = 0;
	for (int iter = 0; iter < 40 && hi > lo + 4096; iter++) {
		usize mid = lo + (hi - lo) / 2, p = mid;
		while (p + 27 < d->len && (memcmp(d->data + p, "OggS", 4) != 0 || rd_le32(d->data + p + 14) != v->ogg.serial))
			p++;
		if (p + 27 >= d->len) {
			hi = mid;
			continue;
		}
		u64 g = (u64)rd_le32(d->data + p + 6) | (u64)rd_le32(d->data + p + 10) << 32;
		if (g < frame) {
			best = p, best_granule = g;
			lo = p + 1;
		} else {
			hi = mid;
		}
	}
	/* Decoding restarts after that page: its packets end at best_granule. */
	v->ogg.pos = best;
	v->ogg.seg = v->ogg.nseg = 0;
	if (best != (usize)v->audio_page_pos)
		next_page(&v->ogg); /* skip that page's (possibly partial) packets */
	else
		best_granule = 0;
	v->have_prev = false;
	v->decoded = best_granule;
	d->pcm_frames = d->pcm_off = 0;
	/* Prime with one packet, then drop samples up to the target. */
	u64 at = best_granule;
	while (at < frame) {
		d->pcm_frames = d->pcm_off = 0;
		if (!vorbis_decode_packet(d))
			break;
		if (at + d->pcm_frames > frame) {
			d->pcm_off = (usize)(frame - at);
			at = frame;
			break;
		}
		at += d->pcm_frames;
	}
	if (d->pcm_off == 0)
		d->pcm_frames = 0;
	d->frame = frame;
	return true;
}

void vorbis_free(Decoder *d)
{
	Vorbis *v = d->codec;
	if (!v)
		return;
	for (int i = 0; i < v->nbooks; i++) {
		mem_free(v->books[i].lengths);
		mem_free(v->books[i].codes);
		mem_free(v->books[i].sorted);
		mem_free(v->books[i].vq);
	}
	mem_free(v->books);
	mem_free(v->floors);
	mem_free(v->residues);
	mem_free(v->maps);
	for (int c = 0; c < 8; c++) {
		mem_free(v->out[c]);
		mem_free(v->prev[c]);
		mem_free(v->floorv[c]);
	}
	for (int i = 0; i < 2; i++) {
		mem_free(v->tw_cos[i]);
		mem_free(v->tw_sin[i]);
		mem_free(v->fft_c[i]);
		mem_free(v->fft_s[i]);
	}
	mem_free(v->fft_buf);
	mem_free(v->ogg.packet);
	mem_free(v);
	d->codec = NULL;
}
