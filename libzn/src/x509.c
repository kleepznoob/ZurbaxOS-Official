/* X.509 certificates: parsing, host name checks, chain verification against
 * the trusted roots, and base64 for PEM files. */
#include "internal.h"

/* ---- Base64 ------------------------------------------------------------------------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const void *data, usize n)
{
	const u8 *p = data;
	char *out = mem_alloc((n + 2) / 3 * 4 + 1), *o = out;
	for (usize i = 0; i < n; i += 3) {
		u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
		*o++ = B64[v >> 18];
		*o++ = B64[(v >> 12) & 63];
		*o++ = i + 1 < n ? B64[(v >> 6) & 63] : '=';
		*o++ = i + 2 < n ? B64[v & 63] : '=';
	}
	*o = 0;
	return out;
}

u8 *base64_decode(const char *s, usize n, usize *out_len)
{
	u8 *out = mem_alloc(n / 4 * 3 + 3);
	usize o = 0;
	u32 acc = 0;
	int bits = 0;
	for (usize i = 0; i < n; i++) {
		char c = s[i];
		int v;
		if (c >= 'A' && c <= 'Z')
			v = c - 'A';
		else if (c >= 'a' && c <= 'z')
			v = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			v = c - '0' + 52;
		else if (c == '+' || c == '-')
			v = 62;
		else if (c == '/' || c == '_')
			v = 63;
		else if (c == '=' || is_space(c))
			continue;
		else {
			mem_free(out);
			return NULL;
		}
		acc = acc << 6 | (u32)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (u8)(acc >> bits);
		}
	}
	*out_len = o;
	return out;
}

/* ---- Parsing ---------------------------------------------------------------------------------- */

#define OID(...) ((const u8[]){ __VA_ARGS__ }), sizeof((const u8[]){ __VA_ARGS__ })

static bool oid_is(Der v, const u8 *oid, usize len)
{
	return v.n == len && memcmp(v.p, oid, len) == 0;
}

static int hash_oid(Der v)
{
	if (oid_is(v, OID(0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01)))
		return HASH_SHA256;
	if (oid_is(v, OID(0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02)))
		return HASH_SHA384;
	if (oid_is(v, OID(0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03)))
		return HASH_SHA512;
	return 0;
}

/* AlgorithmIdentifier of a signature -> SIG_* */
static int parse_sig_alg(Der alg)
{
	Der oid;
	if (!der_expect(&alg, DER_OID, &oid))
		return 0;
	static const u8 rsa[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01 };
	static const u8 ec[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03 };
	if (oid.n == 9 && memcmp(oid.p, rsa, 8) == 0) {
		switch (oid.p[8]) {
		case 0x05: return SIG_RSA_SHA1;
		case 0x0b: return SIG_RSA_SHA256;
		case 0x0c: return SIG_RSA_SHA384;
		case 0x0d: return SIG_RSA_SHA512;
		case 0x0a: {
			/* RSASSA-PSS-params: the hash is in [0]; SHA-256 if absent... but
			 * the default is SHA-1, which nobody uses for this. */
			Der params, h, hid;
			int hash = HASH_SHA256;
			if (der_expect(&alg, DER_SEQUENCE, &params) && der_expect(&params, 0xa0, &h) && der_expect(&h, DER_SEQUENCE, &hid) &&
			    der_expect(&hid, DER_OID, &oid))
				hash = hash_oid(oid);
			return hash == HASH_SHA384 ? SIG_RSA_PSS_SHA384 : hash == HASH_SHA512 ? SIG_RSA_PSS_SHA512 : SIG_RSA_PSS_SHA256;
		}
		}
		return 0;
	}
	if (oid.n == 8 && memcmp(oid.p, ec, 7) == 0) {
		switch (oid.p[7]) {
		case 0x02: return SIG_ECDSA_SHA256;
		case 0x03: return SIG_ECDSA_SHA384;
		case 0x04: return SIG_ECDSA_SHA512;
		}
	}
	return 0;
}

static bool parse_time(Der v, u8 tag, i64 *out)
{
	usize year_digits = tag == DER_UTC_TIME ? 2 : 4;
	if (v.n < year_digits + 10)
		return false;
	int f[7] = { 0 };
	const u8 *p = v.p;
	for (usize i = 0; i < year_digits; i++) {
		if (!is_digit(p[i]))
			return false;
		f[0] = f[0] * 10 + (p[i] - '0');
	}
	p += year_digits;
	for (int i = 1; i < 6; i++, p += 2) {
		if (!is_digit(p[0]) || !is_digit(p[1]))
			return false;
		f[i] = (p[0] - '0') * 10 + (p[1] - '0');
	}
	if (tag == DER_UTC_TIME)
		f[0] += f[0] >= 50 ? 1900 : 2000;
	Civil c = { .year = f[0], .month = f[1], .day = f[2], .hour = f[3], .min = f[4], .sec = f[5] };
	*out = unix_from_civil(&c);
	return true;
}

static void find_cn(Der name, char *out, usize cap)
{
	/* Name = SEQUENCE OF SET OF SEQUENCE { type OID, value } */
	Der rdn;
	while (der_expect(&name, DER_SET, &rdn)) {
		Der atv, oid, val;
		u8 tag;
		while (der_expect(&rdn, DER_SEQUENCE, &atv)) {
			if (der_expect(&atv, DER_OID, &oid) && oid_is(oid, OID(0x55, 0x04, 0x03)) && der_next(&atv, &tag, &val)) {
				str_copy(out, (const char *)val.p, MIN(cap, val.n + 1));
				return;
			}
		}
	}
}

static bool parse_key(Der spki, Cert *c)
{
	Der alg, oid, bits;
	if (!der_expect(&spki, DER_SEQUENCE, &alg) || !der_expect(&alg, DER_OID, &oid) || !der_expect(&spki, DER_BIT_STRING, &bits) ||
	    bits.n < 2 || bits.p[0] != 0)
		return false;
	bits.p++, bits.n--;
	if (oid_is(oid, OID(0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01))) {
		Der key = bits, seq, n, e;
		if (!der_expect(&key, DER_SEQUENCE, &seq) || !der_expect(&seq, DER_INTEGER, &n) || !der_expect(&seq, DER_INTEGER, &e))
			return false;
		der_strip(&n);
		der_strip(&e);
		c->key_type = KEY_RSA;
		c->key = n.p, c->key_len = n.n;
		c->rsa_e = e.p, c->rsa_e_len = e.n;
		return true;
	}
	if (oid_is(oid, OID(0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01))) {
		Der curve;
		if (!der_expect(&alg, DER_OID, &curve))
			return false;
		if (oid_is(curve, OID(0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07)))
			c->curve = CURVE_P256;
		else if (oid_is(curve, OID(0x2b, 0x81, 0x04, 0x00, 0x22)))
			c->curve = CURVE_P384;
		else
			return true; /* other curves: readable, but nothing it signs can be checked */
		c->key_type = KEY_EC;
		c->key = bits.p, c->key_len = bits.n;
		return true;
	}
	return true; /* other key types: readable, but nothing they sign can be checked */
}

static void parse_extensions(Der exts, Cert *c)
{
	Der seq, ext;
	if (!der_expect(&exts, DER_SEQUENCE, &seq))
		return;
	while (der_expect(&seq, DER_SEQUENCE, &ext)) {
		Der oid, val, b;
		if (!der_expect(&ext, DER_OID, &oid))
			continue;
		u8 tag;
		Der save = ext;
		if (der_next(&ext, &tag, &b) && tag != 0x01) /* optional "critical" */
			ext = save;
		if (!der_expect(&ext, DER_OCTET_STRING, &val))
			continue;
		if (oid_is(oid, OID(0x55, 0x1d, 0x11))) {
			c->san = val.p, c->san_len = val.n;
		} else if (oid_is(oid, OID(0x55, 0x1d, 0x13))) {
			Der bc, flag;
			if (der_expect(&val, DER_SEQUENCE, &bc) && der_expect(&bc, 0x01, &flag) && flag.n == 1)
				c->is_ca = flag.p[0] != 0;
		}
	}
}

bool cert_parse(const u8 *der, usize len, Cert *c)
{
	memset(c, 0, sizeof *c);
	c->der = der, c->der_len = len;
	Der d = { der, len }, cert, tbs, alg, sigbits;
	if (!der_expect(&d, DER_SEQUENCE, &cert))
		return false;
	const u8 *tbs_start = cert.p;
	if (!der_expect(&cert, DER_SEQUENCE, &tbs))
		return false;
	c->tbs = tbs_start;
	c->tbs_len = (usize)(cert.p - tbs_start);
	if (!der_expect(&cert, DER_SEQUENCE, &alg) || !der_expect(&cert, DER_BIT_STRING, &sigbits) || sigbits.n < 1)
		return false;
	c->sig_alg = parse_sig_alg(alg);
	c->sig = sigbits.p + 1, c->sig_len = sigbits.n - 1;
	Der v, serial, salg, validity, nb, na, spki;
	u8 tag;
	der_expect(&tbs, 0xa0, &v); /* version, optional */
	if (!der_expect(&tbs, DER_INTEGER, &serial) || !der_expect(&tbs, DER_SEQUENCE, &salg))
		return false;
	const u8 *issuer = tbs.p;
	Der name;
	if (!der_expect(&tbs, DER_SEQUENCE, &name))
		return false;
	c->issuer = issuer, c->issuer_len = (usize)(tbs.p - issuer);
	if (!der_expect(&tbs, DER_SEQUENCE, &validity))
		return false;
	if (!der_next(&validity, &tag, &nb) || !parse_time(nb, tag, &c->not_before) || !der_next(&validity, &tag, &na) ||
	    !parse_time(na, tag, &c->not_after))
		return false;
	const u8 *subject = tbs.p;
	if (!der_expect(&tbs, DER_SEQUENCE, &name))
		return false;
	c->subject = subject, c->subject_len = (usize)(tbs.p - subject);
	find_cn(name, c->cn, sizeof c->cn);
	if (!der_expect(&tbs, DER_SEQUENCE, &spki) || !parse_key(spki, c))
		return false;
	Der exts;
	while (der_next(&tbs, &tag, &exts))
		if (tag == 0xa3)
			parse_extensions(exts, c);
	return true;
}

/* ---- Host names ---------------------------------------------------------------------------- */

static bool name_matches(const char *pattern, usize plen, const char *host)
{
	usize hlen = strlen(host);
	if (plen > 2 && pattern[0] == '*' && pattern[1] == '.') {
		/* "*.example.com" covers exactly one label. */
		const char *dot = str_chr(host, '.');
		if (!dot || dot == host)
			return false;
		usize rest = hlen - (usize)(dot - host);
		return rest == plen - 1 && str_eq_nocase_n(dot, pattern + 1, rest);
	}
	return plen == hlen && str_eq_nocase_n(pattern, host, hlen);
}

static bool parse_ipv4(const char *s, u8 out[4])
{
	for (int i = 0; i < 4; i++) {
		if (!is_digit(*s))
			return false;
		int v = 0;
		while (is_digit(*s))
			v = v * 10 + (*s++ - '0');
		if (v > 255 || (i < 3 && *s++ != '.'))
			return false;
		out[i] = (u8)v;
	}
	return *s == 0;
}

bool cert_matches_host(const Cert *c, const char *host)
{
	u8 ip[4];
	bool is_ip = parse_ipv4(host, ip);
	if (c->san) {
		Der d = { c->san, c->san_len }, names, v;
		u8 tag;
		if (!der_expect(&d, DER_SEQUENCE, &names))
			return false;
		while (der_next(&names, &tag, &v)) {
			if (tag == 0x82 && !is_ip && name_matches((const char *)v.p, v.n, host))
				return true;
			if (tag == 0x87 && is_ip && v.n == 4 && memcmp(v.p, ip, 4) == 0)
				return true;
		}
		return false;
	}
	return !is_ip && c->cn[0] && name_matches(c->cn, strlen(c->cn), host);
}

/* ---- Signatures and chains ------------------------------------------------------------------- */

static int sig_hash(int alg)
{
	switch (alg) {
	case SIG_RSA_SHA1: return HASH_SHA1;
	case SIG_RSA_SHA256: case SIG_RSA_PSS_SHA256: case SIG_ECDSA_SHA256: return HASH_SHA256;
	case SIG_RSA_SHA384: case SIG_RSA_PSS_SHA384: case SIG_ECDSA_SHA384: return HASH_SHA384;
	case SIG_RSA_SHA512: case SIG_RSA_PSS_SHA512: case SIG_ECDSA_SHA512: return HASH_SHA512;
	}
	return 0;
}

bool cert_verify_signature(const Cert *k, int alg, const u8 *data, usize dlen, const u8 *sig, usize slen)
{
	int hash = sig_hash(alg);
	if (!hash)
		return false;
	u8 digest[HASH_MAX];
	hash_once(hash, data, dlen, digest);
	switch (alg) {
	case SIG_RSA_SHA256: case SIG_RSA_SHA384: case SIG_RSA_SHA512:
		return k->key_type == KEY_RSA && rsa_verify_pkcs1(k->key, k->key_len, k->rsa_e, k->rsa_e_len, hash, digest, sig, slen);
	case SIG_RSA_PSS_SHA256: case SIG_RSA_PSS_SHA384: case SIG_RSA_PSS_SHA512:
		return k->key_type == KEY_RSA && rsa_verify_pss(k->key, k->key_len, k->rsa_e, k->rsa_e_len, hash, digest, sig, slen);
	case SIG_ECDSA_SHA256: case SIG_ECDSA_SHA384: case SIG_ECDSA_SHA512:
		return k->key_type == KEY_EC && ecdsa_verify(k->curve, k->key, k->key_len, digest, hash_len(hash), sig, slen);
	}
	return false; /* SHA-1 signatures are not trusted any more */
}

static bool signed_by(const Cert *child, const Cert *issuer)
{
	return child->issuer_len == issuer->subject_len && memcmp(child->issuer, issuer->subject, child->issuer_len) == 0 &&
	       cert_verify_signature(issuer, child->sig_alg, child->tbs, child->tbs_len, child->sig, child->sig_len);
}

/* The trusted roots. */
static Cert *roots;
static int nroots, roots_cap;
static int roots_loaded;      /* atomic */

bool cert_trust_add_pem(const char *pem, usize len)
{
	const char *p = pem, *end = pem + len;
	bool any = false;
	for (;;) {
		const char *b = str_find(p, "-----BEGIN CERTIFICATE-----");
		if (!b || b >= end)
			break;
		b += 27;
		const char *e = str_find(b, "-----END CERTIFICATE-----");
		if (!e || e >= end)
			break;
		usize dlen;
		u8 *der = base64_decode(b, (usize)(e - b), &dlen);
		p = e + 25;
		if (!der)
			continue;
		if (nroots == roots_cap) {
			roots_cap = roots_cap ? roots_cap * 2 : 160;
			roots = mem_realloc(roots, sizeof(Cert) * (usize)roots_cap);
		}
		if (cert_parse(der, dlen, &roots[nroots])) {
			nroots++;
			any = true;
		} else {
			mem_free(der);
		}
	}
	return any;
}

static Mutex roots_lock;

static void add_roots_file(const char *path)
{
	usize len;
	char *pem = file_read(path, &len);
	if (pem) {
		cert_trust_add_pem(pem, len); /* the parsed certificates point into their own DER copies */
		mem_free(pem);
	}
}

/* The system's roots, plus $ZURBAX_CA_FILE (for development servers). Several
 * threads may make their first connection at once, hence the lock. */
static void load_roots(void)
{
	if (atomic_load(&roots_loaded))
		return;
	mutex_lock(&roots_lock);
	if (!roots_loaded) {
		const char *root = env_get("ZURBAX_ROOT");
		char *path = str_fmt("%s/share/certs/roots.pem", root ? root : "/system");
		add_roots_file(path);
		mem_free(path);
		const char *extra = env_get("ZURBAX_CA_FILE");
		if (extra)
			add_roots_file(extra);
		atomic_store(&roots_loaded, 1);
	}
	mutex_unlock(&roots_lock);
}

const char *cert_chain_verify(Cert *chain, int n, const char *host, i64 now)
{
	if (n < 1)
		return "no certificate";
	load_roots();
	if (!cert_matches_host(&chain[0], host))
		return "the certificate is for a different site";
	const Cert *cur = &chain[0];
	bool used[16] = { 0 };
	for (int depth = 0; depth < 10; depth++) {
		if (now < cur->not_before)
			return "the certificate is not valid yet";
		if (now > cur->not_after)
			return "the certificate has expired";
		for (int i = 0; i < nroots; i++)
			if (signed_by(cur, &roots[i]))
				return NULL;
		/* Is the certificate itself a trusted root (sent along by the server)? */
		for (int i = 0; i < nroots; i++)
			if (roots[i].der_len == cur->der_len && memcmp(roots[i].der, cur->der, cur->der_len) == 0)
				return NULL;
		const Cert *next = NULL;
		for (int i = 1; i < n && i < 16 && !next; i++)
			if (!used[i] && chain[i].is_ca && signed_by(cur, &chain[i])) {
				used[i] = true;
				next = &chain[i];
			}
		if (!next)
			return nroots ? "the certificate is not from a trusted authority" : "no trusted certificates are installed";
		cur = next;
	}
	return "the certificate chain is too long";
}
