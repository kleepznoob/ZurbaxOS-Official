/*
 * fetch — downloads a web address.
 *
 *   fetch URL                 prints the page
 *   fetch -o FILE URL         saves it, straight to disk, any size
 *   fetch -I URL              shows the status and headers only
 */
#include "zn.h"

/* a saved download goes to FILE.part as it arrives, then takes FILE's name */
static bool to_disk(void *user, const u8 *data, usize n)
{
	return write_all(*(int *)user, data, n);
}

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
	HttpOptions o = { .timeout_ms = 60000 };
	int fd = -1;
	char *part = NULL;
	if (out && !head) {
		part = str_fmt("%s.part", out);
		fd = (int)sys_open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd < 0) {
			eprint("fetch: cannot write %s\n", out);
			return 1;
		}
		o.progress = progress;
		o.sink = to_disk;
		o.user = &fd;
	}
	HttpResponse *r = http_request(full, &o);
	if (fd >= 0) {
		eprint("\n");
		/* a connection that drops part way leaves less than the site promised */
		char len[32];
		bool whole = !http_header(r, "content-length", len, sizeof len) || (u64)str_to_int(len, NULL) == (u64)r->body_len;
		bool ok = r->status && r->status < 400 && whole && sys_fsync(fd) == 0;
		sys_close(fd);
		if (!ok || sys_rename(part, out) != 0) {
			sys_unlink(part);
			eprint("fetch: %s\n", !r->status ? r->error : r->status >= 400 ? "the site said no" : !whole ? "the download was cut off; run it again" : "cannot write the file");
			return 1;
		}
		eprint("fetch: saved %llu bytes to %s\n", (unsigned long long)r->body_len, out);
		return 0;
	}
	if (!r->status) {
		eprint("fetch: %s\n", r->error);
		return 1;
	}
	if (head) {
		print("%d %s\n%s", r->status, r->final_url, r->headers);
	} else {
		write_all(1, r->body, r->body_len);
	}
	int code = r->status >= 400 ? 1 : 0;
	http_response_free(r);
	mem_free(full);
	return code;
}
