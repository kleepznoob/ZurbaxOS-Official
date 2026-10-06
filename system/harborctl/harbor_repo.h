/* Harbor repositories: the index of apps a source offers, and its signature.
 *
 * A source is a folder (local, or on a web server) holding:
 *   index        the list of apps, in sections ([id] then "key = value" lines)
 *   index.sig    for web sources: an ECDSA P-256 signature (DER) over the
 *                index's SHA-256, by one of the keys in /system/share/harbor/keys
 *   <id>.svg     each app's icon, and <id>-1.png a screenshot
 *   <file>.zpk   each package: a cpio (newc) archive of the app's folder
 * The source built into the system needs no signature: it is part of /system. */
#pragma once

#include "zr.h"

typedef struct {
	char id[48], name[64], version[24], category[32], developer[64];
	char summary[200];
	char *description;
	char file[128], icon[128], screenshot[128];
	u64 size;
	u8 sha256[32];
} HarborApp;

typedef struct {
	char name[64];
	HarborApp *apps;
	int napps;
} HarborIndex;

bool harbor_parse(const char *text, HarborIndex *out);
void harbor_free(HarborIndex *ix);
const HarborApp *harbor_find(const HarborIndex *ix, const char *id);
/* Checks a detached signature against every *.pub key in keys_dir. */
bool harbor_verify(const void *data, usize len, const u8 *sig, usize slen, const char *keys_dir);
/* "1.10" > "1.9" */
int harbor_version_cmp(const char *a, const char *b);
