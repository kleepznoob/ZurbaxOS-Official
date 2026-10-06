/*
 * fetch — downloads a web address.
 *
 *   fetch URL                 prints the page
 *   fetch -o FILE URL         saves it
 *   fetch -I URL              shows the status and headers only
 */
#include "zn.h"

static void progress(void *user, usize got, usize total)
{
	UNUSED(user);
	if (total)
		eprint("\r  %llu of %llu KB", (unsigned long long)(got >> 10), (unsigned long long)(total >> 10));
	else
		eprint("\r  %llu KB", (unsigned long long)(got >> 10));
}

int main(int argc, char **argv)
{
	const char *out = NULL, *url = NULL;
	bool head = false;
	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (str_eq(argv[i], "-I"))
			head = true;
		else
			url = argv[i];
	}
	if (!url) {
		eprint("usage: fetch [-o FILE] [-I] URL\n");
		return 2;
	}
	char *full = str_find(url, "://") ? str_dup(url) : str_fmt("https://%s", url);
	HttpOptions o = { 0 };
	if (out)
		o.progress = progress;
	HttpResponse *r = http_request(full, &o);
	if (out)
		eprint("\n");
	if (!r->status) {
		eprint("fetch: %s\n", r->error);
		return 1;
	}
	if (head) {
		print("%d %s\n%s", r->status, r->final_url, r->headers);
	} else if (out) {
		if (!file_write(out, r->body, r->body_len)) {
			eprint("fetch: cannot write %s\n", out);
			return 1;
		}
		eprint("fetch: saved %llu bytes to %s\n", (unsigned long long)r->body_len, out);
	} else {
		write_all(1, r->body, r->body_len);
	}
	int code = r->status >= 400 ? 1 : 0;
	http_response_free(r);
	mem_free(full);
	return code;
}
