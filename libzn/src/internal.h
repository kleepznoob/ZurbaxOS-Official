/* Shared inside libzn: a small DER reader. */
#pragma once

#include "zn.h"

typedef struct {
	const u8 *p;
	usize n;
} Der;

/* Reads one element: its tag and contents. False at the end or if malformed. */
static inline bool der_next(Der *d, u8 *tag, Der *content)
{
	if (d->n < 2)
		return false;
	u8 t = d->p[0];
	usize len = d->p[1], hdr = 2;
	if (len & 0x80) {
		usize nb = len & 0x7f;
		if (nb == 0 || nb > 4 || d->n < 2 + nb)
			return false;
		len = 0;
		for (usize i = 0; i < nb; i++)
			len = len << 8 | d->p[2 + i];
		hdr += nb;
	}
	if (len > d->n - hdr)
		return false;
	if (tag)
		*tag = t;
	if (content) {
		content->p = d->p + hdr;
		content->n = len;
	}
	d->p += hdr + len;
	d->n -= hdr + len;
	return true;
}

/* Reads an element that must have the given tag. */
static inline bool der_expect(Der *d, u8 tag, Der *content)
{
	u8 t;
	Der save = *d;
	if (!der_next(d, &t, content) || t != tag) {
		*d = save;
		return false;
	}
	return true;
}

/* An INTEGER's magnitude without leading zero bytes. */
static inline void der_strip(Der *v)
{
	while (v->n > 1 && v->p[0] == 0)
		v->p++, v->n--;
}

enum { DER_INTEGER = 0x02, DER_BIT_STRING = 0x03, DER_OCTET_STRING = 0x04, DER_NULL = 0x05, DER_OID = 0x06,
       DER_UTF8 = 0x0c, DER_PRINTABLE = 0x13, DER_IA5 = 0x16, DER_UTC_TIME = 0x17, DER_GENERALIZED_TIME = 0x18,
       DER_SEQUENCE = 0x30, DER_SET = 0x31 };
