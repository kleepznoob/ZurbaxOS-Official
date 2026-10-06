/* Harbor index parsing and signature checks, shared by Harbor and harborctl. */
#include "harbor_repo.h"
#include "zn.h"

static void field(char *dst, usize cap, const char *v)
{
	str_copy(dst, v, cap);
}

bool harbor_parse(const char *text, HarborIndex *out)
{
	memset(out, 0, sizeof *out);
	VEC(HarborApp) v = { 0 };
	HarborApp *cur = NULL;
	char *copy = str_dup(text);
	for (char *line = copy; line && *line;) {
		char *nl = (char *)str_chr(line, '\n');
		if (nl)
			*nl = 0;
		char *s = str_trim(line);
		if (*s == '[') {
			char *e = (char *)str_chr(s, ']');
			if (e) {
				*e = 0;
				HarborApp a = { 0 };
				field(a.id, sizeof a.id, s + 1);
				vec_push(&v, a);
				cur = &vec_last(&v);
			}
		} else if (*s && *s != '#') {
			char *eq = (char *)str_chr(s, '=');
			if (eq) {
				*eq = 0;
				char *k = str_trim(s), *val = str_trim(eq + 1);
				if (!cur) {
					if (str_eq(k, "source"))
						field(out->name, sizeof out->name, val);
				} else if (str_eq(k, "name")) field(cur->name, sizeof cur->name, val);
				else if (str_eq(k, "version")) field(cur->version, sizeof cur->version, val);
				else if (str_eq(k, "category")) field(cur->category, sizeof cur->category, val);
				else if (str_eq(k, "developer")) field(cur->developer, sizeof cur->developer, val);
				else if (str_eq(k, "summary")) field(cur->summary, sizeof cur->summary, val);
				else if (str_eq(k, "file")) field(cur->file, sizeof cur->file, val);
				else if (str_eq(k, "icon")) field(cur->icon, sizeof cur->icon, val);
				else if (str_eq(k, "screenshot")) field(cur->screenshot, sizeof cur->screenshot, val);
				else if (str_eq(k, "size")) cur->size = (u64)str_to_int(val, NULL);
				else if (str_eq(k, "sha256")) hex_decode(val, cur->sha256, 32);
				else if (str_eq(k, "description")) {
					/* "\n" in the index means a new paragraph */
					mem_free(cur->description);
					cur->description = str_replace(val, "\\n", "\n");
				}
			}
		}
		line = nl ? nl + 1 : NULL;
	}
	mem_free(copy);
	/* Only entries with the essentials, and ids safe to use as folder names. */
	int w = 0;
	for (usize i = 0; i < v.len; i++) {
		HarborApp *a = &v.data[i];
		bool ok = a->id[0] && a->name[0] && a->file[0] && a->version[0];
		for (const char *p = a->id; *p; p++)
			if (!is_alnum(*p) && *p != '-' && *p != '_' && *p != '.')
				ok = false;
		if (str_find(a->id, "..") || str_chr(a->file, '/'))
			ok = false;
		if (ok)
			v.data[w++] = *a;
		else
			mem_free(a->description);
	}
	out->apps = v.data;
	out->napps = w;
	return w > 0 || v.len == 0;
}

void harbor_free(HarborIndex *ix)
{
	for (int i = 0; i < ix->napps; i++)
		mem_free(ix->apps[i].description);
	mem_free(ix->apps);
	memset(ix, 0, sizeof *ix);
}

const HarborApp *harbor_find(const HarborIndex *ix, const char *id)
{
	for (int i = 0; i < ix->napps; i++)
		if (str_eq(ix->apps[i].id, id))
			return &ix->apps[i];
	return NULL;
}

bool harbor_verify(const void *data, usize len, const u8 *sig, usize slen, const char *keys_dir)
{
	u8 digest[32];
	sha256(data, len, digest);
	DirList keys = { 0 };
	if (!dir_list(keys_dir, &keys, false))
		return false;
	bool ok = false;
	vec_each(&keys, e)
	{
		if (!str_ends(e->name, ".pub"))
			continue;
		char *path = path_join(keys_dir, e->name);
		char *hex = file_read(path, NULL);
		mem_free(path);
		if (!hex)
			continue;
		u8 point[65];
		char *t = str_trim(hex);
		if (strlen(t) == 130 && hex_decode(t, point, 65) && point[0] == 4 &&
		    ecdsa_verify(CURVE_P256, point, 65, digest, 32, sig, slen))
			ok = true;
		mem_free(hex);
		if (ok)
			break;
	}
	dir_list_free(&keys);
	return ok;
}

int harbor_version_cmp(const char *a, const char *b)
{
	/* Compare dot-separated numbers; a missing part counts as 0. */
	while (*a || *b) {
		i64 x = is_digit(*a) ? str_to_int(a, NULL) : 0, y = is_digit(*b) ? str_to_int(b, NULL) : 0;
		if (x != y)
			return x < y ? -1 : 1;
		while (*a && *a != '.')
			a++;
		while (*b && *b != '.')
			b++;
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
	}
	return 0;
}
