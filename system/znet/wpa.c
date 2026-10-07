/* WPA2-Personal keys and EAPOL-Key frames. */
#include "wpa.h"

static u16 be16(const u8 *p)
{
	return (u16)(p[0] << 8 | p[1]);
}

static void put_be16(u8 *p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

void wpa_pmk(const char *pass, const u8 *ssid, usize ssid_len, u8 pmk[32])
{
	pbkdf2_sha1(pass, strlen(pass), ssid, ssid_len, 4096, pmk, 32);
}

void wpa_prf(const u8 *key, usize klen, const char *label, const u8 *data, usize dlen, u8 *out, usize olen)
{
	usize ll = strlen(label);
	u8 *msg = mem_alloc(ll + 1 + dlen + 1);
	memcpy(msg, label, ll);
	msg[ll] = 0;
	memcpy(msg + ll + 1, data, dlen);
	u8 h[20];
	for (usize done = 0, i = 0; done < olen; i++) {
		msg[ll + 1 + dlen] = (u8)i;
		hmac(HASH_SHA1, key, klen, msg, ll + 2 + dlen, h);
		usize take = MIN(20, olen - done);
		memcpy(out + done, h, take);
		done += take;
	}
	mem_free(msg);
}

void wpa_ptk(const u8 pmk[32], const u8 aa[6], const u8 spa[6], const u8 anonce[32], const u8 snonce[32], Ptk *out)
{
	u8 data[76];
	bool a_first = memcmp(aa, spa, 6) < 0;
	memcpy(data, a_first ? aa : spa, 6);
	memcpy(data + 6, a_first ? spa : aa, 6);
	bool an_first = memcmp(anonce, snonce, 32) < 0;
	memcpy(data + 12, an_first ? anonce : snonce, 32);
	memcpy(data + 44, an_first ? snonce : anonce, 32);
	u8 ptk[48];
	wpa_prf(pmk, 32, "Pairwise key expansion", data, sizeof data, ptk, sizeof ptk);
	memcpy(out->kck, ptk, 16);
	memcpy(out->kek, ptk + 16, 16);
	memcpy(out->tk, ptk + 32, 16);
}

/* Frame layout after the 4-byte EAPOL header: type(1) info(2) key_len(2)
 * replay(8) nonce(32) iv(16) rsc(8) reserved(8) mic(16) data_len(2) data. */
enum { O_TYPE = 4, O_INFO = 5, O_KEYLEN = 7, O_REPLAY = 9, O_NONCE = 17, O_IV = 49, O_RSC = 65, O_MIC = 81, O_DLEN = 97 };

bool eapol_key_parse(const u8 *f, usize n, EapolKey *k)
{
	if (n < EAPOL_KEY_HDR || f[1] != 3 /* EAPOL-Key */)
		return false;
	usize body = be16(f + 2);
	if (body + 4 > n || body + 4 < EAPOL_KEY_HDR)
		return false;
	if (f[O_TYPE] != 2 /* RSN */)
		return false;
	memset(k, 0, sizeof *k);
	k->version = f[0];
	k->info = be16(f + O_INFO);
	k->key_len = be16(f + O_KEYLEN);
	memcpy(k->replay, f + O_REPLAY, 8);
	memcpy(k->nonce, f + O_NONCE, 32);
	memcpy(k->rsc, f + O_RSC, 8);
	memcpy(k->mic, f + O_MIC, 16);
	k->data_len = be16(f + O_DLEN);
	if ((usize)EAPOL_KEY_HDR + k->data_len > body + 4)
		return false;
	k->data = f + EAPOL_KEY_HDR;
	return true;
}

static void mic(const u8 *frame, usize n, const u8 kck[16], u8 out[16])
{
	u8 *copy = mem_alloc(n);
	memcpy(copy, frame, n);
	memset(copy + O_MIC, 0, 16);
	u8 h[20];
	hmac(HASH_SHA1, kck, 16, copy, n, h);
	memcpy(out, h, 16);
	mem_free(copy);
}

usize eapol_key_build(u8 *out, usize cap, const EapolKey *k, const u8 *data, usize dlen, const u8 *kck)
{
	usize n = EAPOL_KEY_HDR + dlen;
	if (n > cap)
		return 0;
	memset(out, 0, EAPOL_KEY_HDR);
	out[0] = k->version ? k->version : 2;   /* 802.1X-2004 unless the peer used another */
	out[1] = 3;   /* EAPOL-Key */
	put_be16(out + 2, (u16)(n - 4));
	out[O_TYPE] = 2;
	put_be16(out + O_INFO, k->info);
	put_be16(out + O_KEYLEN, k->key_len);
	memcpy(out + O_REPLAY, k->replay, 8);
	memcpy(out + O_NONCE, k->nonce, 32);
	memcpy(out + O_RSC, k->rsc, 8);
	put_be16(out + O_DLEN, (u16)dlen);
	if (dlen)
		memcpy(out + EAPOL_KEY_HDR, data, dlen);
	if (kck)
		mic(out, n, kck, out + O_MIC);
	return n;
}

bool eapol_key_mic_ok(const u8 *frame, usize n, const u8 kck[16])
{
	if (n < EAPOL_KEY_HDR)
		return false;
	u8 want[16];
	mic(frame, n, kck, want);
	return mem_equal_ct(want, frame + O_MIC, 16);
}

bool wpa_unwrap(const u8 kek[16], const u8 *in, usize n, u8 *out, usize *out_len)
{
	if (n < 24 || n % 8)
		return false;
	if (!aes_key_unwrap(kek, 16, in, n, out))
		return false;
	*out_len = n - 8;
	return true;
}

usize wpa_wrap(const u8 kek[16], const u8 *in, usize n, u8 *out)
{
	/* padded with 0xdd then zeros to a multiple of 8, at least 16 bytes */
	u8 buf[512];
	if (n > sizeof buf - 16)
		return 0;
	memcpy(buf, in, n);
	usize p = n;
	if (p < 16 || p % 8) {
		buf[p++] = 0xdd;
		while (p < 16 || p % 8)
			buf[p++] = 0;
	}
	aes_key_wrap(kek, 16, buf, p, out);
	return p + 8;
}

const u8 *wpa_find_ie(const u8 *ies, usize n, u8 id)
{
	usize off = 0;
	while (off + 2 <= n) {
		u8 len = ies[off + 1];
		if (off + 2 + len > n)
			break;
		if (ies[off] == id)
			return ies + off;
		off += 2u + len;
	}
	return NULL;
}

bool wpa_find_gtk(const u8 *data, usize n, u8 *gtk, usize *gtk_len, int *key_id)
{
	usize off = 0;
	while (off + 2 <= n) {
		u8 id = data[off], len = data[off + 1];
		if (id == 0xdd && len == 0)
			break;   /* padding */
		if (off + 2 + len > n)
			break;
		const u8 *e = data + off + 2;
		/* vendor KDE 00-0F-AC type 1: key id byte, reserved byte, the key */
		if (id == 0xdd && len >= 6 + 16 && e[0] == 0x00 && e[1] == 0x0f && e[2] == 0xac && e[3] == 1) {
			*key_id = e[4] & 3;
			*gtk_len = len - 6u;
			if (*gtk_len > 32)
				return false;
			memcpy(gtk, e + 6, *gtk_len);
			return true;
		}
		off += 2u + len;
	}
	return false;
}

static u32 suite(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

bool rsn_parse(const u8 *b, usize n, Rsn *r)
{
	memset(r, 0, sizeof *r);
	if (n < 2 || b[0] != 1 || b[1] != 0)
		return false;
	/* defaults when the element stops early: CCMP and PSK... strictly, 802.1X */
	r->group = SUITE(CIPHER_CCMP);
	r->pairwise[r->npairwise++] = SUITE(CIPHER_CCMP);
	r->akm[r->nakm++] = SUITE(AKM_8021X);
	usize off = 2;
	if (off + 4 > n)
		return true;
	r->group = suite(b + off);
	off += 4;
	if (off + 2 > n)
		return true;
	u16 cnt = (u16)(b[off] | b[off + 1] << 8);
	off += 2;
	if (off + 4u * cnt > n)
		return false;
	r->npairwise = 0;
	for (u16 i = 0; i < cnt; i++, off += 4)
		if (r->npairwise < 4)
			r->pairwise[r->npairwise++] = suite(b + off);
	if (off + 2 > n)
		return true;
	cnt = (u16)(b[off] | b[off + 1] << 8);
	off += 2;
	if (off + 4u * cnt > n)
		return false;
	r->nakm = 0;
	for (u16 i = 0; i < cnt; i++, off += 4)
		if (r->nakm < 4)
			r->akm[r->nakm++] = suite(b + off);
	if (off + 2 <= n)
		r->caps = (u16)(b[off] | b[off + 1] << 8);
	return true;
}

usize rsn_element(u8 *out, u32 group_cipher)
{
	static const u8 tmpl[22] = {
		0x30, 20, 0x01, 0x00,             /* RSN, version 1 */
		0x00, 0x0f, 0xac, 0x04,           /* group cipher (patched below) */
		0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,   /* one pairwise cipher: CCMP */
		0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,   /* one key management: PSK */
		0x00, 0x00,                       /* capabilities */
	};
	memcpy(out, tmpl, sizeof tmpl);
	out[7] = (u8)group_cipher;
	return sizeof tmpl;
}
