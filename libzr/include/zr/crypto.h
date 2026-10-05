/* Hashing: SHA-256, HMAC-SHA-256, PBKDF2 and password records. */
#pragma once

#include "base.h"

typedef struct {
	u32 h[8];
	u64 len;
	u8 buf[64];
	usize fill;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, usize n);
void sha256_final(Sha256 *s, u8 out[32]);
void sha256(const void *data, usize n, u8 out[32]);
void hmac_sha256(const void *key, usize klen, const void *msg, usize mlen, u8 out[32]);
void pbkdf2_sha256(const void *pw, usize plen, const void *salt, usize slen, u32 iterations, u8 *out, usize olen);

/* Password records look like "pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>". */
char *password_hash(const char *password);           /* allocated */
bool password_check(const char *password, const char *record);
bool mem_equal_ct(const void *a, const void *b, usize n); /* constant-time compare */
void hex_encode(const u8 *data, usize n, char *out);  /* out: 2n+1 bytes */
bool hex_decode(const char *s, u8 *out, usize n);
