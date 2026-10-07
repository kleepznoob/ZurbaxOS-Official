/* Tests for libzn against published test vectors and certificates made with
 * OpenSSL (tests/data/zn). Run with `make test`. */
#include "zn.h"

static int failures, checks;

#define CHECK(cond)                                                          \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			failures++;                                          \
			eprint("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                            \
	} while (0)

static bool hex_is(const u8 *data, usize n, const char *want)
{
	char *got = mem_alloc(2 * n + 1);
	hex_encode(data, n, got);
	bool ok = str_eq_nocase(got, want);
	if (!ok)
		eprint("    got  %s\n    want %s\n", got, want);
	mem_free(got);
	return ok;
}

static usize unhex(const char *s, u8 *out)
{
	usize n = strlen(s) / 2;
	hex_decode(s, out, n);
	return n;
}

static void test_hashes(void)
{
	u8 out[64];
	hash_once(HASH_SHA1, "abc", 3, out);
	CHECK(hex_is(out, 20, "a9993e364706816aba3e25717850c26c9cd0d89d"));
	hash_once(HASH_SHA384, "abc", 3, out);
	CHECK(hex_is(out, 48, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"));
	hash_once(HASH_SHA512, "abc", 3, out);
	CHECK(hex_is(out, 64,
		     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
	/* Two blocks' worth. */
	const char *long_msg = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
	hash_once(HASH_SHA512, long_msg, strlen(long_msg), out);
	CHECK(hex_is(out, 64,
		     "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"));
	hmac(HASH_SHA256, "Jefe", 4, "what do ya want for nothing?", 28, out);
	CHECK(hex_is(out, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
	hmac(HASH_SHA384, "Jefe", 4, "what do ya want for nothing?", 28, out);
	CHECK(hex_is(out, 48, "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e8e2240ca5e69e2c78b3239ecfab21649"));
	/* RFC 5869 case 1 */
	u8 ikm[22], salt[13], info[10], prk[32], okm[42];
	memset(ikm, 0x0b, sizeof ikm);
	for (int i = 0; i < 13; i++)
		salt[i] = (u8)i;
	for (int i = 0; i < 10; i++)
		info[i] = (u8)(0xf0 + i);
	hkdf_extract(HASH_SHA256, salt, 13, ikm, 22, prk);
	CHECK(hex_is(prk, 32, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));
	hkdf_expand(HASH_SHA256, prk, info, 10, okm, 42);
	CHECK(hex_is(okm, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
	/* RFC 6070 and the IEEE 802.11i passphrase example. */
	pbkdf2_sha1("password", 8, "salt", 4, 2, out, 20);
	CHECK(hex_is(out, 20, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"));
	pbkdf2_sha1("password", 8, "IEEE", 4, 4096, out, 32);
	CHECK(hex_is(out, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
}

static void test_ciphers(void)
{
	/* RFC 8439 section 2.8.2 */
	u8 key[32], nonce[12], aad[12];
	for (int i = 0; i < 32; i++)
		key[i] = (u8)(0x80 + i);
	unhex("070000004041424344454647", nonce);
	unhex("50515253c0c1c2c3c4c5c6c7", aad);
	const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
	usize n = strlen(pt);
	u8 ct[256], back[256];
	chacha20_poly1305_seal(key, nonce, aad, 12, (const u8 *)pt, n, ct);
	CHECK(hex_is(ct, 16, "d31a8d34648e60db7b86afbc53ef7ec2"));
	CHECK(hex_is(ct + n, 16, "1ae10b594f09e26a7e902ecbd0600691"));
	CHECK(chacha20_poly1305_open(key, nonce, aad, 12, ct, n + 16, back) && memcmp(back, pt, n) == 0);
	ct[5] ^= 1;
	CHECK(!chacha20_poly1305_open(key, nonce, aad, 12, ct, n + 16, back));

	/* FIPS-197 */
	AesGcm a;
	u8 k[32], block[16], out[16];
	for (int i = 0; i < 32; i++)
		k[i] = (u8)i;
	unhex("00112233445566778899aabbccddeeff", block);
	aes_init(&a, k, 16);
	aes_encrypt_block(&a, block, out);
	CHECK(hex_is(out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"));
	aes_init(&a, k, 32);
	aes_encrypt_block(&a, block, out);
	CHECK(hex_is(out, 16, "8ea2b7ca516745bfeafc49904b496089"));

	/* GCM test cases 2 and 4 from the original GCM paper. */
	u8 zero[16] = { 0 }, gct[128];
	aes_init(&a, zero, 16);
	aes_gcm_seal(&a, zero, NULL, 0, zero, 16, gct);
	CHECK(hex_is(gct, 16, "0388dace60b6a392f328c2b971b2fe78"));
	CHECK(hex_is(gct + 16, 16, "ab6e47d42cec13bdf53a67b21257bddf"));
	u8 k4[16], iv4[12], p4[60], a4[20];
	unhex("feffe9928665731c6d6a8f9467308308", k4);
	unhex("cafebabefacedbaddecaf888", iv4);
	unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", p4);
	unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", a4);
	aes_init(&a, k4, 16);
	aes_gcm_seal(&a, iv4, a4, 20, p4, 60, gct);
	CHECK(hex_is(gct + 60, 16, "5bc94fbc3221a5db94fae95ae7121a47"));
	u8 p4b[60];
	CHECK(aes_gcm_open(&a, iv4, a4, 20, gct, 76, p4b) && memcmp(p4b, p4, 60) == 0);

	/* RFC 3394 section 4.1 */
	u8 kek[16], wrapped[24], unwrapped[16];
	unhex("000102030405060708090a0b0c0d0e0f", kek);
	unhex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", wrapped);
	CHECK(aes_key_unwrap(kek, 16, wrapped, 24, unwrapped) && hex_is(unwrapped, 16, "00112233445566778899aabbccddeeff"));
	u8 rewrapped[24];
	aes_key_wrap(kek, 16, unwrapped, 16, rewrapped);
	CHECK(hex_is(rewrapped, 24, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"));
}

static void test_x25519(void)
{
	u8 s[32], u[32], out[32];
	unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", s);
	unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
	x25519(out, s, u);
	CHECK(hex_is(out, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));
	u8 alice[32], bob[32], apub[32], bpub[32], s1[32], s2[32];
	unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice);
	unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bob);
	x25519_base(apub, alice);
	x25519_base(bpub, bob);
	CHECK(hex_is(apub, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
	x25519(s1, alice, bpub);
	x25519(s2, bob, apub);
	CHECK(memcmp(s1, s2, 32) == 0 && hex_is(s1, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
}

static u8 *pem_cert(const char *path, usize *len)
{
	char *pem = file_read(path, NULL);
	if (!pem)
		return NULL;
	const char *b = str_find(pem, "-----BEGIN CERTIFICATE-----"), *e = str_find(pem, "-----END");
	u8 *der = b && e ? base64_decode(b + 27, (usize)(e - b - 27), len) : NULL;
	mem_free(pem);
	return der;
}

static void test_certs(void)
{
	const char *dir = "tests/data/zn";
	char path[256];
	usize len;
	Cert leaf_rsa, leaf_ec, leaf_pss, ca_rsa, ca_384;
	u8 *d_rsa, *d_ec, *d_pss, *d_ca, *d_384;
	fmt(path, sizeof path, "%s/leaf-rsa.pem", dir);
	d_rsa = pem_cert(path, &len);
	CHECK(d_rsa && cert_parse(d_rsa, len, &leaf_rsa));
	fmt(path, sizeof path, "%s/leaf-ec.pem", dir);
	d_ec = pem_cert(path, &len);
	CHECK(d_ec && cert_parse(d_ec, len, &leaf_ec));
	fmt(path, sizeof path, "%s/leaf-ec-pss.pem", dir);
	d_pss = pem_cert(path, &len);
	CHECK(d_pss && cert_parse(d_pss, len, &leaf_pss));
	fmt(path, sizeof path, "%s/ca-rsa.pem", dir);
	d_ca = pem_cert(path, &len);
	CHECK(d_ca && cert_parse(d_ca, len, &ca_rsa));
	fmt(path, sizeof path, "%s/ca-p384.pem", dir);
	d_384 = pem_cert(path, &len);
	CHECK(d_384 && cert_parse(d_384, len, &ca_384));
	if (!d_rsa || !d_ec || !d_pss || !d_ca || !d_384)
		return;
	CHECK(leaf_rsa.key_type == KEY_RSA && leaf_rsa.sig_alg == SIG_RSA_SHA256);
	CHECK(leaf_ec.key_type == KEY_EC && leaf_ec.curve == CURVE_P256 && leaf_ec.sig_alg == SIG_ECDSA_SHA384);
	CHECK(leaf_pss.sig_alg == SIG_RSA_PSS_SHA256);
	CHECK(ca_rsa.is_ca && ca_384.is_ca && !leaf_rsa.is_ca);
	CHECK(str_eq(ca_rsa.cn, "Zurbax Test Root RSA"));

	/* Host names. */
	CHECK(cert_matches_host(&leaf_rsa, "localhost"));
	CHECK(cert_matches_host(&leaf_rsa, "www.zurbax.test"));
	CHECK(cert_matches_host(&leaf_rsa, "WWW.Zurbax.Test"));
	CHECK(!cert_matches_host(&leaf_rsa, "a.b.zurbax.test"));
	CHECK(!cert_matches_host(&leaf_rsa, "zurbax.test"));
	CHECK(cert_matches_host(&leaf_rsa, "127.0.0.1"));
	CHECK(!cert_matches_host(&leaf_rsa, "127.0.0.2"));
	CHECK(!cert_matches_host(&leaf_rsa, "rsa.test")); /* the CN doesn't count when there are SANs */

	/* Signatures made by openssl over msg.txt. */
	fmt(path, sizeof path, "%s/msg.txt", dir);
	usize mlen;
	char *msg = file_read(path, &mlen);
	struct { const char *file; const Cert *key; int alg; } sigs[] = {
		{ "msg.p256.sig", &leaf_ec, SIG_ECDSA_SHA256 },
		{ "msg.rsa.sig", &leaf_rsa, SIG_RSA_SHA256 },
		{ "msg.pss.sig", &leaf_rsa, SIG_RSA_PSS_SHA256 },
		{ "msg.p384.sig", &ca_384, SIG_ECDSA_SHA384 },
	};
	for (usize i = 0; i < ARRAY_LEN(sigs); i++) {
		fmt(path, sizeof path, "%s/%s", dir, sigs[i].file);
		usize slen;
		char *sig = file_read(path, &slen);
		CHECK(sig && cert_verify_signature(sigs[i].key, sigs[i].alg, (const u8 *)msg, mlen, (const u8 *)sig, slen));
		/* A different message must fail. */
		CHECK(sig && !cert_verify_signature(sigs[i].key, sigs[i].alg, (const u8 *)"Zurbax signs that.", mlen, (const u8 *)sig, slen));
		mem_free(sig);
	}
	mem_free(msg);

	/* Chains: RSA root signs an RSA leaf (PKCS #1) and an EC leaf (PSS);
	 * the P-384 root signs an EC leaf with ECDSA-SHA384. */
	i64 now = time_unix();
	CHECK(cert_chain_verify(&leaf_rsa, 1, "localhost", now) != NULL); /* nothing trusted yet */
	char *pem = file_read("tests/data/zn/ca-rsa.pem", &len);
	cert_trust_add_pem(pem, len);
	mem_free(pem);
	pem = file_read("tests/data/zn/ca-p384.pem", &len);
	cert_trust_add_pem(pem, len);
	mem_free(pem);
	CHECK(cert_chain_verify(&leaf_rsa, 1, "localhost", now) == NULL);
	CHECK(cert_chain_verify(&leaf_ec, 1, "localhost", now) == NULL);
	CHECK(cert_chain_verify(&leaf_pss, 1, "localhost", now) == NULL);
	CHECK(cert_chain_verify(&leaf_rsa, 1, "example.com", now) != NULL);
	CHECK(cert_chain_verify(&leaf_rsa, 1, "localhost", now + 20ll * 365 * 86400) != NULL);
	/* A tampered certificate must not verify. */
	u8 *bad = mem_dup(d_rsa, leaf_rsa.der_len);
	Cert badc;
	bad[leaf_rsa.tbs - leaf_rsa.der + 40] ^= 1;
	CHECK(!cert_parse(bad, leaf_rsa.der_len, &badc) || cert_chain_verify(&badc, 1, "localhost", now) != NULL);
	mem_free(bad);
}

static void test_urls(void)
{
	Url u;
	CHECK(url_parse("https://Example.com/a/b?c=d#top", &u) && str_eq(u.host, "example.com") && u.port == 443 && str_eq(u.path, "/a/b?c=d") &&
	      str_eq(u.fragment, "top"));
	CHECK(url_parse("http://localhost:8080", &u) && u.port == 8080 && str_eq(u.path, "/"));
	CHECK(url_parse("http://h?x=1", &u) && str_eq(u.path, "/?x=1"));
	CHECK(!url_parse("example.com", &u));
	char *r;
	r = url_resolve("https://a.com/x/y/z.html", "../img/p.png");
	CHECK(str_eq(r, "https://a.com/x/img/p.png"));
	mem_free(r);
	r = url_resolve("https://a.com/x/y/z.html", "/root.css");
	CHECK(str_eq(r, "https://a.com/root.css"));
	mem_free(r);
	r = url_resolve("https://a.com/x/", "//cdn.b.com/s.js");
	CHECK(str_eq(r, "https://cdn.b.com/s.js"));
	mem_free(r);
	r = url_resolve("https://a.com/x/page?q=1", "?q=2");
	CHECK(str_eq(r, "https://a.com/x/page?q=2"));
	mem_free(r);
	r = url_resolve("http://a.com:8080/x", "y");
	CHECK(str_eq(r, "http://a.com:8080/y"));
	mem_free(r);
	r = url_resolve("https://a.com/", "mailto:me@a.com");
	CHECK(str_eq(r, "mailto:me@a.com"));
	mem_free(r);
	r = url_encode("a b&c=d/é");
	CHECK(str_eq(r, "a%20b%26c%3Dd%2F%C3%A9"));
	mem_free(r);
	char *b = base64_encode("hello!?", 7);
	CHECK(str_eq(b, "aGVsbG8hPw=="));
	usize n;
	u8 *d = base64_decode(b, strlen(b), &n);
	CHECK(d && n == 7 && memcmp(d, "hello!?", 7) == 0);
	mem_free(d);
	mem_free(b);
}

/* Every root in the shipped trust store must parse. */
static void test_roots(void)
{
	usize len;
	char *pem = file_read("data/certs/roots.pem", &len);
	CHECK(pem != NULL);
	if (!pem)
		return;
	int total = 0, parsed = 0;
	for (const char *p = pem;;) {
		const char *b = str_find(p, "-----BEGIN CERTIFICATE-----");
		if (!b)
			break;
		const char *e = str_find(b, "-----END CERTIFICATE-----");
		usize dlen;
		u8 *der = base64_decode(b + 27, (usize)(e - b - 27), &dlen);
		Cert c;
		total++;
		if (der && cert_parse(der, dlen, &c))
			parsed++;
		else
			eprint("    unparsed root #%d\n", total);
		mem_free(der);
		p = e + 25;
	}
	CHECK(total > 100 && parsed == total);
	mem_free(pem);
}

int main(void)
{
	test_hashes();
	test_ciphers();
	test_x25519();
	test_certs();
	test_urls();
	test_roots();
	print("libzn: %d/%d checks passed\n", checks - failures, checks);
	return failures ? 1 : 0;
}
