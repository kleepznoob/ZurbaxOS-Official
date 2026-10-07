/* Tests for the network service's Wi-Fi security: keys, frames and parsing. */
#include "../system/znet/wpa.c"

static int checks, failed;
#define CHECK(c)                                                         \
	do {                                                             \
		checks++;                                                \
		if (!(c)) {                                              \
			failed++;                                        \
			eprint("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		}                                                        \
	} while (0)

static void unhex(const char *s, u8 *out)
{
	hex_decode(s, out, strlen(s) / 2);
}

static bool hex_is(const u8 *p, usize n, const char *want)
{
	char buf[256];
	hex_encode(p, n, buf);
	return str_eq(buf, want);
}

static void test_keys(void)
{
	/* IEEE 802.11 PRF test vector */
	u8 key[20], out[64];
	memset(key, 0x0b, sizeof key);
	wpa_prf(key, 20, "prefix", (const u8 *)"Hi There", 8, out, 64);
	CHECK(hex_is(out, 64, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
			      "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"));
	/* the passphrase example, then a PTK checked against an independent implementation */
	u8 pmk[32];
	wpa_pmk("password", (const u8 *)"IEEE", 4, pmk);
	CHECK(hex_is(pmk, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
	u8 aa[6], spa[6], anonce[32], snonce[32];
	unhex("020000000100", aa);
	unhex("020000000000", spa);
	for (int i = 0; i < 32; i++)
		anonce[i] = (u8)i, snonce[i] = (u8)(100 + i);
	Ptk ptk;
	wpa_ptk(pmk, aa, spa, anonce, snonce, &ptk);
	CHECK(hex_is(ptk.kck, 16, "b47cb684c40239ab8b3d9a6820d987d8"));
	CHECK(hex_is(ptk.kek, 16, "fd2be953ebbda94ea23b0a469bbc0caa"));
	CHECK(hex_is(ptk.tk, 16, "8937bc251866e3c9a0beeb95ad7ef0d3"));
	/* the order of addresses and nonces doesn't matter */
	Ptk swapped;
	wpa_ptk(pmk, spa, aa, snonce, anonce, &swapped);
	CHECK(memcmp(&ptk, &swapped, sizeof ptk) == 0);

	/* message 2: MIC over the frame */
	u8 rsn[32];
	usize rl = rsn_element(rsn, SUITE(CIPHER_CCMP));
	CHECK(hex_is(rsn, rl, "30140100000fac040100000fac040100000fac020000"));
	EapolKey k = { .version = 1, .info = KI_VER_SHA1_AES | KI_PAIRWISE | KI_MIC };
	k.replay[7] = 1;
	memcpy(k.nonce, snonce, 32);
	u8 frame[256];
	usize n = eapol_key_build(frame, sizeof frame, &k, rsn, rl, ptk.kck);
	CHECK(n == EAPOL_KEY_HDR + rl);
	CHECK(hex_is(frame + 81, 16, "992427e6d7b7fcf00887bf092a9d1d49"));
	CHECK(eapol_key_mic_ok(frame, n, ptk.kck));
	frame[20] ^= 1;
	CHECK(!eapol_key_mic_ok(frame, n, ptk.kck));
	frame[20] ^= 1;
	EapolKey p;
	CHECK(eapol_key_parse(frame, n, &p) && p.info == k.info && p.data_len == rl && memcmp(p.nonce, snonce, 32) == 0);
	CHECK(!eapol_key_parse(frame, 50, &p));
}

static void test_key_data(void)
{
	u8 kek[16];
	for (int i = 0; i < 16; i++)
		kek[i] = (u8)(0x40 + i);
	/* message 3's key data: our RSN element and the group key */
	u8 plain[64], gtk[16];
	for (int i = 0; i < 16; i++)
		gtk[i] = (u8)(0xa0 + i);
	usize n = rsn_element(plain, SUITE(CIPHER_CCMP));
	u8 kde[] = { 0xdd, 22, 0x00, 0x0f, 0xac, 0x01, 0x01, 0x00 };
	memcpy(plain + n, kde, sizeof kde);
	memcpy(plain + n + sizeof kde, gtk, 16);
	n += sizeof kde + 16;
	u8 wrapped[96], back[96];
	usize wl = wpa_wrap(kek, plain, n, wrapped);
	CHECK(wl % 8 == 0 && wl >= n + 8);
	usize bl;
	CHECK(wpa_unwrap(kek, wrapped, wl, back, &bl) && bl >= n && memcmp(back, plain, n) == 0);
	u8 got[32];
	usize gl;
	int id;
	CHECK(wpa_find_gtk(back, bl, got, &gl, &id) && gl == 16 && id == 1 && memcmp(got, gtk, 16) == 0);
	const u8 *ie = wpa_find_ie(back, bl, 0x30);
	CHECK(ie && ie[1] == 20);
	wrapped[3] ^= 0x80;
	CHECK(!wpa_unwrap(kek, wrapped, wl, back, &bl));
}

static void test_rsn(void)
{
	/* WPA2/WPA3 transition: CCMP, PSK and SAE, management frame protection capable */
	u8 body[] = { 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
		      0x02, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x0f, 0xac, 0x08, 0x80, 0x00 };
	Rsn r;
	CHECK(rsn_parse(body, sizeof body, &r));
	CHECK(r.group == SUITE(CIPHER_CCMP) && r.npairwise == 1 && r.pairwise[0] == SUITE(CIPHER_CCMP));
	CHECK(r.nakm == 2 && r.akm[0] == SUITE(AKM_PSK) && r.akm[1] == SUITE(AKM_SAE) && r.caps == 0x0080);
	/* truncated counts are rejected */
	CHECK(!rsn_parse(body, 14, &r));
	u8 bad[] = { 0x02, 0x00 };
	CHECK(!rsn_parse(bad, 2, &r));
	/* elements in a beacon */
	u8 ies[] = { 0x00, 4, 'h', 'o', 'm', 'e', 0x01, 1, 0x82, 0x30, 2, 1, 0 };
	const u8 *e = wpa_find_ie(ies, sizeof ies, 0x30);
	CHECK(e && e - ies == 9);
	CHECK(!wpa_find_ie(ies, 5, 0x30));
}

int main(void)
{
	test_keys();
	test_key_data();
	test_rsn();
	print("znet: %d/%d checks passed\n", checks - failed, checks);
	return failed ? 1 : 0;
}
