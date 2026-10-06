/*
 * libzn — networking: the cryptography TLS needs, certificates, TLS 1.3,
 * DNS, HTTP/1.1 and URLs. Everything here is written for Zurbax; the only
 * outside data is the list of trusted root certificates.
 */
#pragma once

#include "zr.h"

/* ---- Hashes ----------------------------------------------------------------------------- */

typedef struct {
	u32 h[5];
	u64 len;
	u8 buf[64];
	usize fill;
} Sha1;

typedef struct {
	u64 h[8];
	u64 len;
	u8 buf[128];
	usize fill;
	usize out_len;          /* 48 for SHA-384, 64 for SHA-512 */
} Sha512;

void sha1_init(Sha1 *s);
void sha1_update(Sha1 *s, const void *data, usize n);
void sha1_final(Sha1 *s, u8 out[20]);
void sha512_init(Sha512 *s);
void sha384_init(Sha512 *s);
void sha512_update(Sha512 *s, const void *data, usize n);
void sha512_final(Sha512 *s, u8 *out);

enum { HASH_SHA1 = 1, HASH_SHA256, HASH_SHA384, HASH_SHA512 };
#define HASH_MAX 64

typedef struct {
	int kind;
	union {
		Sha1 sha1;
		Sha256 sha256;
		Sha512 sha512;
	} u;
} Hash;

usize hash_len(int kind);
void hash_init(Hash *h, int kind);
void hash_update(Hash *h, const void *data, usize n);
void hash_final(Hash *h, u8 *out);
void hash_once(int kind, const void *data, usize n, u8 *out);
void hmac(int kind, const void *key, usize klen, const void *msg, usize mlen, u8 *out);
void hkdf_extract(int kind, const void *salt, usize slen, const void *ikm, usize ilen, u8 *prk);
void hkdf_expand(int kind, const u8 *prk, const void *info, usize ilen, u8 *out, usize olen);
void pbkdf2_sha1(const void *pw, usize plen, const void *salt, usize slen, u32 iterations, u8 *out, usize olen);

/* ---- Ciphers -------------------------------------------------------------------------- */

/* ChaCha20-Poly1305 (RFC 8439). `out` gets n bytes plus the 16-byte tag. */
void chacha20_poly1305_seal(const u8 key[32], const u8 nonce[12], const u8 *aad, usize alen, const u8 *in, usize n, u8 *out);
/* `in` holds n bytes including the tag; writes n - 16 bytes. False if forged. */
bool chacha20_poly1305_open(const u8 key[32], const u8 nonce[12], const u8 *aad, usize alen, const u8 *in, usize n, u8 *out);

typedef struct {
	u32 rk[60];
	int rounds;
	u8 h[16];               /* GHASH key */
} AesGcm;

void aes_init(AesGcm *a, const u8 *key, usize klen);      /* 16 or 32 bytes */
void aes_encrypt_block(const AesGcm *a, const u8 in[16], u8 out[16]);
void aes_gcm_seal(const AesGcm *a, const u8 nonce[12], const u8 *aad, usize alen, const u8 *in, usize n, u8 *out);
bool aes_gcm_open(const AesGcm *a, const u8 nonce[12], const u8 *aad, usize alen, const u8 *in, usize n, u8 *out);
/* RFC 3394 key unwrap (Wi-Fi group keys). False if the integrity check fails. */
bool aes_key_unwrap(const u8 *kek, usize klen, const u8 *in, usize n, u8 *out);

/* ---- Public keys ------------------------------------------------------------------------- */

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);
void x25519_base(u8 out[32], const u8 scalar[32]);

enum { CURVE_P256 = 1, CURVE_P384 };
/* `pub` is an uncompressed point (04 || X || Y); `sig` is a DER ECDSA-Sig-Value. */
bool ecdsa_verify(int curve, const u8 *pub, usize plen, const u8 *digest, usize dlen, const u8 *sig, usize slen);
/* RSA with modulus n and exponent e (big-endian bytes). */
bool rsa_verify_pkcs1(const u8 *n, usize nlen, const u8 *e, usize elen, int hash, const u8 *digest, const u8 *sig, usize slen);
bool rsa_verify_pss(const u8 *n, usize nlen, const u8 *e, usize elen, int hash, const u8 *digest, const u8 *sig, usize slen);

/* ---- Encodings ---------------------------------------------------------------------------- */

char *base64_encode(const void *data, usize n);            /* allocated */
u8 *base64_decode(const char *s, usize n, usize *out_len); /* ignores whitespace; NULL if invalid */

/* ---- Certificates ------------------------------------------------------------------------- */

typedef struct {
	const u8 *der;
	usize der_len;
	const u8 *tbs;          /* the signed part */
	usize tbs_len;
	const u8 *issuer, *subject; /* raw DER names, for chaining */
	usize issuer_len, subject_len;
	i64 not_before, not_after;
	int key_type;           /* KEY_RSA, KEY_EC */
	int curve;
	const u8 *key, *rsa_e;  /* RSA: modulus and exponent; EC: the point */
	usize key_len, rsa_e_len;
	int sig_alg;            /* SIG_* */
	const u8 *sig;
	usize sig_len;
	bool is_ca;
	const u8 *san;          /* the subjectAltName extension's value, or NULL */
	usize san_len;
	char cn[128];           /* subject common name, as a fallback */
} Cert;

enum { KEY_RSA = 1, KEY_EC };
enum { SIG_RSA_SHA256 = 1, SIG_RSA_SHA384, SIG_RSA_SHA512, SIG_RSA_SHA1, SIG_RSA_PSS_SHA256, SIG_RSA_PSS_SHA384, SIG_RSA_PSS_SHA512,
       SIG_ECDSA_SHA256, SIG_ECDSA_SHA384, SIG_ECDSA_SHA512 };

bool cert_parse(const u8 *der, usize len, Cert *out);
bool cert_matches_host(const Cert *c, const char *host);
/* Checks `sig` over `data` with the certificate's key, for the given SIG_* algorithm. */
bool cert_verify_signature(const Cert *key_owner, int sig_alg, const u8 *data, usize dlen, const u8 *sig, usize slen);
/* Verifies a chain (leaf first) for `host` against the trusted roots.
 * Returns NULL if trusted, else a short reason. */
const char *cert_chain_verify(Cert *chain, int n, const char *host, i64 now);
/* Trusted roots come from /system/share/certs/roots.pem; tests may add their own. */
bool cert_trust_add_pem(const char *pem, usize len);

/* ---- TLS 1.3 ---------------------------------------------------------------------------------- */

typedef struct Tls Tls;

/* Runs the handshake on a connected socket. On failure returns NULL and
 * writes a reason to err. */
Tls *tls_connect(int fd, const char *host, char *err, usize errcap);
long tls_read(Tls *t, void *buf, usize n);        /* 0 at the end, negative on error */
bool tls_write(Tls *t, const void *buf, usize n);
void tls_close(Tls *t);                            /* also closes the socket */

/* ---- DNS and connections ----------------------------------------------------------------------- */

/* Resolves a host name to an IPv4 address (network byte order). Numbers pass through. */
bool dns_resolve(const char *host, u32 *addr, char *err, usize errcap);
/* Connects over TCP with a timeout. Returns the socket or -1. */
int tcp_connect(const char *host, u16 port, int timeout_ms, char *err, usize errcap);

/* ---- URLs and HTTP --------------------------------------------------------------------------- */

typedef struct {
	char scheme[16];
	char host[256];
	u16 port;
	char path[2048];        /* with the query, at least "/" */
	char fragment[256];
} Url;

bool url_parse(const char *s, Url *out);
char *url_resolve(const char *base, const char *ref);       /* allocated absolute URL */
char *url_encode(const char *s);                             /* percent-encodes a query value */

typedef struct {
	int status;             /* 200..., or 0 if the request failed */
	char error[160];
	char *headers;          /* "Name: value\n" lines */
	u8 *body;
	usize body_len;
	char final_url[2048];   /* after redirects */
} HttpResponse;

typedef struct {
	const char *method;     /* default GET */
	const char *headers;    /* extra "Name: value\r\n" lines */
	const void *body;
	usize body_len;
	int timeout_ms;         /* default 15000 */
	usize max_body;         /* default 64 MiB */
	int max_redirects;      /* default 5 */
	/* called as data arrives; total is 0 when unknown */
	void (*progress)(void *user, usize got, usize total);
	void *user;
} HttpOptions;

HttpResponse *http_request(const char *url, const HttpOptions *opts); /* never NULL */
HttpResponse *http_get(const char *url);
const char *http_header(const HttpResponse *r, const char *name, char *out, usize cap);
void http_response_free(HttpResponse *r);
