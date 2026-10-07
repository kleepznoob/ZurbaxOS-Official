/*
 * TLS 1.3 client (RFC 8446): X25519 key exchange, AES-GCM and
 * ChaCha20-Poly1305, server certificates checked against the trusted
 * roots. No resumption, no early data; KeyUpdate and alerts are handled.
 */
#include "internal.h"

#define REC_MAX (16384 + 256)
enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APP = 23 };
enum { HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8, HS_CERTIFICATE = 11,
       HS_CERTIFICATE_REQUEST = 13, HS_CERTIFICATE_VERIFY = 15, HS_FINISHED = 20, HS_KEY_UPDATE = 24 };

typedef struct {
	u8 key[32], iv[12];
	AesGcm aes;
	u64 seq;
	u8 secret[HASH_MAX];
} Dir;

struct Tls {
	int fd;
	u16 suite;
	int hash;
	usize hlen, klen;
	Dir rd, wr;
	bool encrypted_read, encrypted_write;
	/* raw input */
	u8 in[REC_MAX + 5 + 4096];
	usize in_len;
	/* decrypted records waiting to be used */
	u8 rec[REC_MAX];
	usize rec_len, rec_off;
	int rec_type;
	/* handshake messages being reassembled */
	u8 *hs;
	usize hs_len;
	bool eof, failed;
	char *err;
	usize errcap;
};

static bool fail(Tls *t, const char *msg)
{
	if (!t->failed && t->err)
		str_copy(t->err, msg, t->errcap);
	t->failed = true;
	return false;
}

static void put16(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 8), p[1] = (u8)v;
}

static void put24(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 16), p[1] = (u8)(v >> 8), p[2] = (u8)v;
}

static u32 get16(const u8 *p)
{
	return (u32)p[0] << 8 | p[1];
}

static u32 get24(const u8 *p)
{
	return (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
}

/* ---- Key schedule ------------------------------------------------------------------------- */

static void expand_label(Tls *t, const u8 *secret, const char *label, const u8 *ctx, usize clen, u8 *out, usize olen)
{
	u8 info[2 + 1 + 6 + 32 + 1 + HASH_MAX];
	usize ll = strlen(label), n = 0;
	put16(info, (u32)olen);
	n = 2;
	info[n++] = (u8)(6 + ll);
	memcpy(info + n, "tls13 ", 6);
	n += 6;
	memcpy(info + n, label, ll);
	n += ll;
	info[n++] = (u8)clen;
	memcpy(info + n, ctx, clen);
	n += clen;
	hkdf_expand(t->hash, secret, info, n, out, olen);
}

static void set_keys(Tls *t, Dir *d, const u8 *secret)
{
	memcpy(d->secret, secret, t->hlen);
	expand_label(t, secret, "key", NULL, 0, d->key, t->klen);
	expand_label(t, secret, "iv", NULL, 0, d->iv, 12);
	if (t->suite != 0x1303)
		aes_init(&d->aes, d->key, t->klen);
	d->seq = 0;
}

static void nonce_for(const Dir *d, u8 nonce[12])
{
	memcpy(nonce, d->iv, 12);
	for (int i = 0; i < 8; i++)
		nonce[11 - i] ^= (u8)(d->seq >> (8 * i));
}

/* ---- Records ------------------------------------------------------------------------------ */

static bool send_all(Tls *t, const u8 *p, usize n)
{
	if (!write_all(t->fd, p, n))
		return fail(t, "the connection was lost");
	return true;
}

static bool send_record(Tls *t, int type, const u8 *data, usize n)
{
	u8 rec[5 + REC_MAX];
	while (n || type != CT_APP) {
		usize k = MIN(n, (usize)16384);
		if (!t->encrypted_write) {
			rec[0] = (u8)type;
			put16(rec + 1, 0x0303);
			put16(rec + 3, (u32)k);
			memcpy(rec + 5, data, k);
			if (!send_all(t, rec, 5 + k))
				return false;
		} else {
			u8 plain[16385];
			memcpy(plain, data, k);
			plain[k] = (u8)type;
			usize clen = k + 1 + 16;
			rec[0] = CT_APP;
			put16(rec + 1, 0x0303);
			put16(rec + 3, (u32)clen);
			u8 nonce[12];
			nonce_for(&t->wr, nonce);
			if (t->suite == 0x1303)
				chacha20_poly1305_seal(t->wr.key, nonce, rec, 5, plain, k + 1, rec + 5);
			else
				aes_gcm_seal(&t->wr.aes, nonce, rec, 5, plain, k + 1, rec + 5);
			t->wr.seq++;
			if (!send_all(t, rec, 5 + clen))
				return false;
		}
		data += k, n -= k;
		if (type != CT_APP)
			break;
	}
	return true;
}

static bool fill(Tls *t, usize need)
{
	while (t->in_len < need) {
		long r = sys_read(t->fd, t->in + t->in_len, sizeof t->in - t->in_len);
		if (r == -EINTR)
			continue;
		if (r == -EAGAIN)
			return fail(t, "the site took too long to answer");
		if (r <= 0)
			return fail(t, "the connection was closed");
		t->in_len += (usize)r;
	}
	return true;
}

/* Reads one record and decrypts it into t->rec. */
static bool read_record(Tls *t)
{
	for (;;) {
		if (!fill(t, 5))
			return false;
		usize len = get16(t->in + 3);
		if (len > REC_MAX)
			return fail(t, "the site sent a record that is too large");
		if (!fill(t, 5 + len))
			return false;
		u8 type = t->in[0];
		u8 hdr[5];
		memcpy(hdr, t->in, 5);
		const u8 *body = t->in + 5;
		bool ok = true;
		if (type == CT_CCS) {
			/* Sent for middlebox compatibility; meaningless in TLS 1.3. */
		} else if (!t->encrypted_read || type != CT_APP) {
			if (t->encrypted_read && type != CT_ALERT) {
				ok = fail(t, "the site sent an unencrypted record");
			} else {
				memcpy(t->rec, body, len);
				t->rec_len = len, t->rec_off = 0, t->rec_type = type;
			}
		} else {
			u8 nonce[12];
			nonce_for(&t->rd, nonce);
			bool good = len >= 17 && (t->suite == 0x1303 ? chacha20_poly1305_open(t->rd.key, nonce, hdr, 5, body, len, t->rec)
								  : aes_gcm_open(&t->rd.aes, nonce, hdr, 5, body, len, t->rec));
			t->rd.seq++;
			if (!good) {
				ok = fail(t, "a record failed its integrity check");
			} else {
				/* Strip the padding; the last non-zero byte is the real type. */
				usize n = len - 16;
				while (n && t->rec[n - 1] == 0)
					n--;
				if (!n) {
					ok = fail(t, "the site sent an empty record");
				} else {
					t->rec_type = t->rec[n - 1];
					t->rec_len = n - 1, t->rec_off = 0;
				}
			}
		}
		memmove(t->in, t->in + 5 + len, t->in_len - 5 - len);
		t->in_len -= 5 + len;
		if (!ok)
			return false;
		if (type == CT_CCS)
			continue;
		if (t->rec_type == CT_ALERT) {
			if (t->rec_len >= 2 && t->rec[1] == 0) { /* close_notify */
				t->eof = true;
				return true;
			}
			char m[64];
			fmt(m, sizeof m, "the site refused the connection (alert %d)", t->rec_len >= 2 ? t->rec[1] : -1);
			return fail(t, m);
		}
		return true;
	}
}

/* Returns the next complete handshake message (type, body) from the stream. */
static bool next_handshake(Tls *t, u8 *type, u8 **body, usize *len)
{
	for (;;) {
		if (t->hs_len >= 4) {
			usize mlen = get24(t->hs + 1);
			if (mlen > 1 << 20)
				return fail(t, "the site sent a handshake message that is too large");
			if (t->hs_len >= 4 + mlen) {
				*type = t->hs[0];
				*body = t->hs;
				*len = 4 + mlen;
				return true;
			}
		}
		if (!read_record(t))
			return false;
		if (t->eof)
			return fail(t, "the connection was closed during the handshake");
		if (t->rec_type != CT_HANDSHAKE)
			return fail(t, "the site sent something unexpected during the handshake");
		t->hs = mem_realloc(t->hs, t->hs_len + t->rec_len);
		memcpy(t->hs + t->hs_len, t->rec, t->rec_len);
		t->hs_len += t->rec_len;
		t->rec_len = 0;
	}
}

static void consume_handshake(Tls *t, usize len)
{
	memmove(t->hs, t->hs + len, t->hs_len - len);
	t->hs_len -= len;
}

/* ---- The handshake ------------------------------------------------------------------------------- */

static usize client_hello(u8 *m, const char *host, const u8 pub[32], const u8 random[32], const u8 session[32])
{
	usize n = 4;
	put16(m + n, 0x0303), n += 2;
	memcpy(m + n, random, 32), n += 32;
	m[n++] = 32;
	memcpy(m + n, session, 32), n += 32;
	static const u8 suites[] = { 0, 6, 0x13, 0x01, 0x13, 0x03, 0x13, 0x02 };
	memcpy(m + n, suites, sizeof suites), n += sizeof suites;
	m[n++] = 1, m[n++] = 0;
	usize ext_at = n;
	n += 2;
	/* server_name, unless the host is an address */
	bool numeric = true;
	for (const char *p = host; *p; p++)
		if (!is_digit(*p) && *p != '.')
			numeric = false;
	usize hl = strlen(host);
	if (!numeric) {
		put16(m + n, 0), put16(m + n + 2, (u32)hl + 5), put16(m + n + 4, (u32)hl + 3);
		m[n + 6] = 0;
		put16(m + n + 7, (u32)hl);
		memcpy(m + n + 9, host, hl);
		n += 9 + hl;
	}
	static const u8 fixed[] = {
		0x00, 0x0a, 0x00, 0x04, 0x00, 0x02, 0x00, 0x1d,                    /* supported_groups: x25519 */
		0x00, 0x0d, 0x00, 0x14, 0x00, 0x12, 0x04, 0x03, 0x05, 0x03, 0x06, 0x03, /* signature_algorithms */
		0x08, 0x04, 0x08, 0x05, 0x08, 0x06, 0x04, 0x01, 0x05, 0x01, 0x06, 0x01,
		0x00, 0x2b, 0x00, 0x03, 0x02, 0x03, 0x04,                          /* supported_versions: 1.3 */
		0x00, 0x2d, 0x00, 0x02, 0x01, 0x01,                                /* psk_key_exchange_modes */
		0x00, 0x10, 0x00, 0x0b, 0x00, 0x09, 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1', /* ALPN */
	};
	memcpy(m + n, fixed, sizeof fixed), n += sizeof fixed;
	/* key_share */
	put16(m + n, 51), put16(m + n + 2, 38), put16(m + n + 4, 36), put16(m + n + 6, 0x1d), put16(m + n + 8, 32);
	memcpy(m + n + 10, pub, 32);
	n += 42;
	put16(m + ext_at, (u32)(n - ext_at - 2));
	m[0] = HS_CLIENT_HELLO;
	put24(m + 1, (u32)(n - 4));
	return n;
}

static const u8 HRR_RANDOM[32] = { 0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
				   0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c };

static bool parse_server_hello(Tls *t, const u8 *m, usize len, u8 server_pub[32])
{
	if (len < 4 + 2 + 32 + 1)
		return fail(t, "the site's hello was malformed");
	const u8 *p = m + 4, *end = m + len;
	if (memcmp(p + 2, HRR_RANDOM, 32) == 0)
		return fail(t, "the site asked for a key exchange ZOS doesn't support yet");
	p += 34;
	usize sid = *p++;
	if (p + sid + 3 > end)
		return fail(t, "the site's hello was malformed");
	p += sid;
	t->suite = (u16)get16(p);
	p += 3; /* suite and compression */
	if (t->suite == 0x1301)
		t->hash = HASH_SHA256, t->klen = 16;
	else if (t->suite == 0x1302)
		t->hash = HASH_SHA384, t->klen = 32;
	else if (t->suite == 0x1303)
		t->hash = HASH_SHA256, t->klen = 32;
	else
		return fail(t, "the site chose an unknown cipher");
	t->hlen = hash_len(t->hash);
	bool tls13 = false, have_key = false;
	if (p + 2 <= end) {
		usize elen = get16(p);
		p += 2;
		const u8 *eend = MIN(p + elen, end);
		while (p + 4 <= eend) {
			u32 type = get16(p), l = get16(p + 2);
			const u8 *v = p + 4;
			if (v + l > eend)
				break;
			if (type == 43 && l == 2 && get16(v) == 0x0304)
				tls13 = true;
			if (type == 51 && l == 36 && get16(v) == 0x1d && get16(v + 2) == 32) {
				memcpy(server_pub, v + 4, 32);
				have_key = true;
			}
			p = v + l;
		}
	}
	if (!tls13)
		return fail(t, "the site only supports older TLS versions");
	if (!have_key)
		return fail(t, "the site didn't send a key share");
	return true;
}

static int scheme_to_sig(u32 scheme)
{
	switch (scheme) {
	case 0x0403: return SIG_ECDSA_SHA256;
	case 0x0503: return SIG_ECDSA_SHA384;
	case 0x0603: return SIG_ECDSA_SHA512;
	case 0x0804: return SIG_RSA_PSS_SHA256;
	case 0x0805: return SIG_RSA_PSS_SHA384;
	case 0x0806: return SIG_RSA_PSS_SHA512;
	}
	return 0;
}

static void transcript(const Hash *h, u8 *out)
{
	Hash copy = *h;
	hash_final(&copy, out);
}

static bool handshake(Tls *t, const char *host)
{
	u8 priv[32], pub[32], random[32], session[32];
	rand_bytes(priv, 32);
	rand_bytes(random, 32);
	rand_bytes(session, 32);
	x25519_base(pub, priv);
	u8 ch[1024];
	usize chlen = client_hello(ch, host, pub, random, session);
	if (!send_record(t, CT_HANDSHAKE, ch, chlen))
		return false;

	u8 type, *m;
	usize len;
	if (!next_handshake(t, &type, &m, &len))
		return false;
	if (type != HS_SERVER_HELLO)
		return fail(t, "the site didn't answer with a hello");
	u8 server_pub[32];
	if (!parse_server_hello(t, m, len, server_pub))
		return false;
	Hash th;
	hash_init(&th, t->hash);
	hash_update(&th, ch, chlen);
	hash_update(&th, m, len);
	consume_handshake(t, len);

	/* Handshake secrets. */
	u8 shared[32], zero[HASH_MAX] = { 0 }, early[HASH_MAX], derived[HASH_MAX], hs_secret[HASH_MAX], empty_hash[HASH_MAX], th_now[HASH_MAX];
	x25519(shared, priv, server_pub);
	u8 acc = 0;
	for (int i = 0; i < 32; i++)
		acc |= shared[i];
	if (!acc)
		return fail(t, "the site sent an invalid key");
	hkdf_extract(t->hash, NULL, 0, zero, t->hlen, early);
	hash_once(t->hash, "", 0, empty_hash);
	expand_label(t, early, "derived", empty_hash, t->hlen, derived, t->hlen);
	hkdf_extract(t->hash, derived, t->hlen, shared, 32, hs_secret);
	transcript(&th, th_now);
	u8 c_hs[HASH_MAX], s_hs[HASH_MAX];
	expand_label(t, hs_secret, "c hs traffic", th_now, t->hlen, c_hs, t->hlen);
	expand_label(t, hs_secret, "s hs traffic", th_now, t->hlen, s_hs, t->hlen);
	set_keys(t, &t->rd, s_hs);
	t->encrypted_read = true;

	/* EncryptedExtensions, (CertificateRequest), Certificate, CertificateVerify, Finished. */
	Cert chain[8];
	int nchain = 0;
	u8 *cert_msg = NULL;
	bool cert_requested = false, verified = false;
	for (;;) {
		if (!next_handshake(t, &type, &m, &len))
			break;
		if (type == HS_ENCRYPTED_EXTENSIONS) {
			/* nothing we need */
		} else if (type == HS_CERTIFICATE_REQUEST) {
			cert_requested = true;
		} else if (type == HS_CERTIFICATE) {
			cert_msg = mem_dup(m, len);
			const u8 *p = cert_msg + 4, *end = cert_msg + len;
			if (p >= end)
				break;
			p += 1 + *p; /* request context */
			if (p + 3 > end)
				break;
			const u8 *lend = MIN(p + 3 + get24(p), end);
			p += 3;
			while (p + 3 <= lend && nchain < 8) {
				usize cl = get24(p);
				p += 3;
				if (p + cl + 2 > lend)
					break;
				if (!cert_parse(p, cl, &chain[nchain])) {
					if (nchain == 0) {
						mem_free(cert_msg);
						return fail(t, "the site's certificate couldn't be read");
					}
				} else {
					nchain++;
				}
				p += cl;
				p += 2 + get16(p);
			}
			if (nchain == 0) {
				mem_free(cert_msg);
				return fail(t, "the site sent no certificate");
			}
			const char *why = cert_chain_verify(chain, nchain, host, time_unix());
			if (why) {
				mem_free(cert_msg);
				return fail(t, why);
			}
		} else if (type == HS_CERTIFICATE_VERIFY) {
			if (!nchain || len < 8)
				break;
			u32 scheme = get16(m + 4), slen = get16(m + 6);
			if (8 + slen > len)
				break;
			u8 content[64 + 34 + HASH_MAX];
			memset(content, 0x20, 64);
			memcpy(content + 64, "TLS 1.3, server CertificateVerify", 34); /* includes the 0 byte */
			transcript(&th, content + 98);
			int sig = scheme_to_sig(scheme);
			if (!sig || !cert_verify_signature(&chain[0], sig, content, 98 + t->hlen, m + 8, slen)) {
				mem_free(cert_msg);
				return fail(t, "the site's signature didn't check out");
			}
			verified = true;
		} else if (type == HS_FINISHED) {
			if (!verified) {
				mem_free(cert_msg);
				return fail(t, "the site didn't prove its identity");
			}
			u8 fkey[HASH_MAX], expect[HASH_MAX];
			expand_label(t, s_hs, "finished", NULL, 0, fkey, t->hlen);
			transcript(&th, th_now);
			hmac(t->hash, fkey, t->hlen, th_now, t->hlen, expect);
			if (len != 4 + t->hlen || !mem_equal_ct(expect, m + 4, t->hlen)) {
				mem_free(cert_msg);
				return fail(t, "the handshake was tampered with");
			}
			hash_update(&th, m, len);
			consume_handshake(t, len);
			break;
		} else {
			mem_free(cert_msg);
			return fail(t, "the site sent an unexpected handshake message");
		}
		hash_update(&th, m, len);
		consume_handshake(t, len);
	}
	mem_free(cert_msg);
	if (t->failed)
		return false;
	if (!verified)
		return fail(t, "the handshake ended early");

	/* Application secrets from the transcript through the server's Finished. */
	u8 master[HASH_MAX], c_ap[HASH_MAX], s_ap[HASH_MAX];
	expand_label(t, hs_secret, "derived", empty_hash, t->hlen, derived, t->hlen);
	hkdf_extract(t->hash, derived, t->hlen, zero, t->hlen, master);
	transcript(&th, th_now);
	expand_label(t, master, "c ap traffic", th_now, t->hlen, c_ap, t->hlen);
	expand_label(t, master, "s ap traffic", th_now, t->hlen, s_ap, t->hlen);

	/* Our side: compatibility ChangeCipherSpec, an empty Certificate if one
	 * was asked for, and Finished. */
	static const u8 ccs = 1;
	if (!send_record(t, CT_CCS, &ccs, 1))
		return false;
	set_keys(t, &t->wr, c_hs);
	t->encrypted_write = true;
	if (cert_requested) {
		u8 empty[8] = { HS_CERTIFICATE, 0, 0, 4, 0, 0, 0, 0 };
		hash_update(&th, empty, 8);
		if (!send_record(t, CT_HANDSHAKE, empty, 8))
			return false;
	}
	u8 fkey[HASH_MAX], fin[4 + HASH_MAX];
	expand_label(t, c_hs, "finished", NULL, 0, fkey, t->hlen);
	transcript(&th, th_now);
	fin[0] = HS_FINISHED;
	put24(fin + 1, (u32)t->hlen);
	hmac(t->hash, fkey, t->hlen, th_now, t->hlen, fin + 4);
	if (!send_record(t, CT_HANDSHAKE, fin, 4 + t->hlen))
		return false;
	set_keys(t, &t->wr, c_ap);
	set_keys(t, &t->rd, s_ap);
	return true;
}

Tls *tls_connect(int fd, const char *host, char *err, usize errcap)
{
	Tls *t = mem_zalloc(sizeof *t);
	t->fd = fd;
	t->err = err, t->errcap = errcap;
	if (err && errcap)
		err[0] = 0;
	if (!handshake(t, host)) {
		mem_free(t->hs);
		mem_free(t);
		return NULL;
	}
	t->err = NULL;
	return t;
}

static bool key_update(Tls *t, Dir *d)
{
	u8 next[HASH_MAX];
	expand_label(t, d->secret, "traffic upd", NULL, 0, next, t->hlen);
	set_keys(t, d, next);
	return true;
}

long tls_read(Tls *t, void *buf, usize n)
{
	while (t->rec_off >= t->rec_len || t->rec_type != CT_APP) {
		if (t->eof)
			return 0;
		if (t->failed)
			return -1;
		/* Post-handshake messages first. */
		if (t->rec_type == CT_HANDSHAKE && t->rec_off < t->rec_len) {
			t->hs = mem_realloc(t->hs, t->hs_len + t->rec_len - t->rec_off);
			memcpy(t->hs + t->hs_len, t->rec + t->rec_off, t->rec_len - t->rec_off);
			t->hs_len += t->rec_len - t->rec_off;
			t->rec_off = t->rec_len;
		}
		while (t->hs_len >= 4 && t->hs_len >= 4 + get24(t->hs + 1)) {
			usize mlen = 4 + get24(t->hs + 1);
			if (t->hs[0] == HS_KEY_UPDATE && mlen == 5) {
				bool requested = t->hs[4] == 1;
				key_update(t, &t->rd);
				if (requested) {
					u8 ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
					if (!send_record(t, CT_HANDSHAKE, ku, 5))
						return -1;
					key_update(t, &t->wr);
				}
			}
			/* NewSessionTicket and anything else: not used. */
			consume_handshake(t, mlen);
		}
		t->rec_len = t->rec_off = 0;
		if (!read_record(t))
			return -1;
	}
	usize k = MIN(n, t->rec_len - t->rec_off);
	memcpy(buf, t->rec + t->rec_off, k);
	t->rec_off += k;
	return (long)k;
}

bool tls_write(Tls *t, const void *buf, usize n)
{
	if (t->failed)
		return false;
	return send_record(t, CT_APP, buf, n);
}

void tls_close(Tls *t)
{
	if (!t)
		return;
	if (!t->failed) {
		static const u8 close_notify[2] = { 1, 0 };
		send_record(t, CT_ALERT, close_notify, 2);
	}
	sys_close(t->fd);
	mem_free(t->hs);
	memset(t, 0, sizeof *t);
	mem_free(t);
}
