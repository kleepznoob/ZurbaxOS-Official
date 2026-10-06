/* DNS, TCP connections, URLs and an HTTP/1.1 client (with TLS for https,
 * chunked bodies, gzip, redirects and an optional HTTPS proxy). */
#include "internal.h"
#include "zg.h"

/* ---- DNS ----------------------------------------------------------------------------------- */

static bool parse_ipv4(const char *s, u32 *out)
{
	u8 b[4];
	for (int i = 0; i < 4; i++) {
		if (!is_digit(*s))
			return false;
		int v = 0;
		while (is_digit(*s))
			v = v * 10 + (*s++ - '0');
		if (v > 255 || (i < 3 && *s++ != '.'))
			return false;
		b[i] = (u8)v;
	}
	if (*s)
		return false;
	memcpy(out, b, 4);
	return true;
}

/* Name servers: the network service's, then the host's (development), then public ones. */
static int name_servers(u32 *out, int max)
{
	int n = 0;
	const char *env = env_get("ZURBAX_DNS");
	if (env && parse_ipv4(env, &out[n]))
		n++;
	Conf *c = conf_load("/run/zurbax/network.status");
	const char *dns = conf_get(c, "dns", NULL);
	if (dns) {
		usize k;
		char **parts = str_split(dns, ' ', &k);
		for (usize i = 0; i < k && n < max; i++)
			if (parse_ipv4(parts[i], &out[n]))
				n++;
		str_split_free(parts, k);
	}
	conf_free(c);
	char *rc = file_read("/etc/resolv.conf", NULL);
	for (const char *p = rc; p && *p && n < max;) {
		if (str_starts(p, "nameserver")) {
			char addr[64] = { 0 };
			const char *a = p + 10;
			while (*a == ' ' || *a == '\t')
				a++;
			usize i = 0;
			while (a[i] && !is_space(a[i]) && i < sizeof addr - 1)
				addr[i] = a[i], i++;
			if (parse_ipv4(addr, &out[n]))
				n++;
		}
		p = str_chr(p, '\n');
		if (p)
			p++;
	}
	mem_free(rc);
	static const u8 fallback[2][4] = { { 1, 1, 1, 1 }, { 8, 8, 8, 8 } };
	for (int i = 0; i < 2 && n < max; i++)
		memcpy(&out[n++], fallback[i], 4);
	return n;
}

typedef struct {
	char host[256];
	u32 addr;
	i64 expires;
} DnsCache;

static DnsCache dns_cache[16];

static usize skip_name(const u8 *m, usize len, usize off)
{
	while (off < len) {
		u8 l = m[off];
		if (l == 0)
			return off + 1;
		if ((l & 0xc0) == 0xc0)
			return off + 2;
		off += 1 + l;
	}
	return len;
}

static bool query(u32 server, const char *host, u32 *addr, u32 *ttl)
{
	u8 q[512];
	u16 id = (u16)rand_u32();
	usize n = 0;
	q[n++] = (u8)(id >> 8), q[n++] = (u8)id;
	q[n++] = 0x01, q[n++] = 0x00; /* recursion desired */
	q[n++] = 0, q[n++] = 1, q[n++] = 0, q[n++] = 0, q[n++] = 0, q[n++] = 0, q[n++] = 0, q[n++] = 0;
	usize hl = strlen(host);
	if (hl > 253)
		return false;
	const char *p = host;
	while (*p) {
		const char *dot = str_chr(p, '.');
		usize l = dot ? (usize)(dot - p) : strlen(p);
		if (l == 0 || l > 63)
			return false;
		q[n++] = (u8)l;
		memcpy(q + n, p, l);
		n += l;
		p += l;
		if (*p == '.')
			p++;
	}
	q[n++] = 0;
	q[n++] = 0, q[n++] = 1; /* A */
	q[n++] = 0, q[n++] = 1; /* IN */
	int fd = (int)sys_socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr = { server } };
	bool ok = false;
	for (int attempt = 0; attempt < 2 && !ok; attempt++) {
		if (sys_sendto(fd, q, n, 0, &sa, sizeof sa) < 0)
			break;
		u64 deadline = time_mono_ms() + 1500;
		while (!ok) {
			i64 left = (i64)deadline - (i64)time_mono_ms();
			if (left <= 0)
				break;
			struct pollfd pf = { fd, POLLIN, 0 };
			if (sys_poll(&pf, 1, (int)left) <= 0)
				break;
			u8 r[1500];
			long len = sys_recvfrom(fd, r, sizeof r, 0, NULL, NULL);
			if (len < 12 || r[0] != q[0] || r[1] != q[1])
				continue;
			if ((r[3] & 0x0f) != 0) /* NXDOMAIN and other errors */
				break;
			usize ancount = (usize)r[6] << 8 | r[7], off = 12;
			off = skip_name(r, (usize)len, off) + 4; /* the question */
			for (usize i = 0; i < ancount && off + 10 <= (usize)len; i++) {
				off = skip_name(r, (usize)len, off);
				if (off + 10 > (usize)len)
					break;
				u16 type = (u16)(r[off] << 8 | r[off + 1]);
				u32 t = (u32)r[off + 4] << 24 | (u32)r[off + 5] << 16 | (u32)r[off + 6] << 8 | r[off + 7];
				u16 rdlen = (u16)(r[off + 8] << 8 | r[off + 9]);
				off += 10;
				if (type == 1 && rdlen == 4 && off + 4 <= (usize)len) {
					memcpy(addr, r + off, 4);
					*ttl = t;
					ok = true;
					break;
				}
				off += rdlen;
			}
			if (!ok)
				break;
		}
	}
	sys_close(fd);
	return ok;
}

bool dns_resolve(const char *host, u32 *addr, char *err, usize errcap)
{
	if (parse_ipv4(host, addr))
		return true;
	if (str_eq_nocase(host, "localhost")) {
		*addr = htonl(0x7f000001);
		return true;
	}
	i64 now = time_unix();
	for (usize i = 0; i < ARRAY_LEN(dns_cache); i++)
		if (dns_cache[i].expires > now && str_eq_nocase(dns_cache[i].host, host)) {
			*addr = dns_cache[i].addr;
			return true;
		}
	u32 servers[6];
	int n = name_servers(servers, 6);
	for (int i = 0; i < n; i++) {
		u32 ttl = 0;
		if (query(servers[i], host, addr, &ttl)) {
			DnsCache *slot = &dns_cache[rand_u32() % ARRAY_LEN(dns_cache)];
			str_copy(slot->host, host, sizeof slot->host);
			slot->addr = *addr;
			slot->expires = now + MIN(MAX(ttl, 30u), 3600u);
			return true;
		}
	}
	if (err)
		fmt(err, errcap, "%s couldn't be found", host);
	return false;
}

/* ---- TCP ---------------------------------------------------------------------------------- */

int tcp_connect(const char *host, u16 port, int timeout_ms, char *err, usize errcap)
{
	u32 addr;
	if (!dns_resolve(host, &addr, err, errcap))
		return -1;
	int fd = (int)sys_socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		if (err)
			str_copy(err, "no network connection", errcap);
		return -1;
	}
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = { addr } };
	long r = sys_connect(fd, &sa, sizeof sa);
	if (r == -EINPROGRESS) {
		struct pollfd pf = { fd, POLLOUT, 0 };
		if (sys_poll(&pf, 1, timeout_ms) <= 0) {
			r = -ETIMEDOUT;
		} else {
			int soerr = 0;
			u32 len = sizeof soerr;
			sys_getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
			r = -soerr;
		}
	}
	if (r < 0) {
		if (err)
			fmt(err, errcap, r == -ETIMEDOUT ? "%s took too long to answer" : r == -ECONNREFUSED ? "%s refused the connection" : "%s couldn't be reached", host);
		sys_close(fd);
		return -1;
	}
	fd_set_nonblock(fd, false);
	struct { i64 sec, usec; } tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
	sys_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	sys_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	int one = 1;
	sys_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

/* ---- URLs -------------------------------------------------------------------------------------- */

bool url_parse(const char *s, Url *u)
{
	memset(u, 0, sizeof *u);
	while (is_space(*s))
		s++;
	const char *colon = str_find(s, "://");
	if (!colon || colon - s >= (long)sizeof u->scheme)
		return false;
	for (const char *p = s; p < colon; p++)
		u->scheme[p - s] = (char)to_lower(*p);
	const char *h = colon + 3;
	const char *end = h;
	while (*end && *end != '/' && *end != '?' && *end != '#')
		end++;
	const char *at = NULL;
	for (const char *p = h; p < end; p++)
		if (*p == '@')
			at = p;
	if (at)
		h = at + 1; /* user info is ignored */
	const char *port = NULL;
	for (const char *p = h; p < end; p++)
		if (*p == ':')
			port = p;
	const char *hend = port ? port : end;
	if (hend == h || (usize)(hend - h) >= sizeof u->host)
		return false;
	for (const char *p = h; p < hend; p++)
		u->host[p - h] = (char)to_lower(*p);
	if (port) {
		i64 v = str_to_int(port + 1, NULL);
		if (v <= 0 || v > 65535)
			return false;
		u->port = (u16)v;
	} else {
		u->port = str_eq(u->scheme, "https") ? 443 : 80;
	}
	const char *frag = str_chr(end, '#');
	usize plen = frag ? (usize)(frag - end) : strlen(end);
	if (plen == 0 || end[0] != '/') {
		u->path[0] = '/';
		str_copy(u->path + 1, end, MIN(sizeof u->path - 1, plen + 1));
	} else {
		str_copy(u->path, end, MIN(sizeof u->path, plen + 1));
	}
	if (frag)
		str_copy(u->fragment, frag + 1, sizeof u->fragment);
	return true;
}

/* "https:", "mailto:"... */
static bool has_scheme(const char *s)
{
	if (!is_alpha(*s))
		return false;
	while (is_alnum(*s) || *s == '+' || *s == '-' || *s == '.')
		s++;
	return *s == ':';
}

char *url_resolve(const char *base, const char *ref)
{
	while (is_space(*ref))
		ref++;
	if (has_scheme(ref))
		return str_dup(ref);
	Url b;
	if (!url_parse(base, &b))
		return str_dup(ref);
	bool default_port = (str_eq(b.scheme, "https") && b.port == 443) || (str_eq(b.scheme, "http") && b.port == 80);
	char origin[320];
	if (default_port)
		fmt(origin, sizeof origin, "%s://%s", b.scheme, b.host);
	else
		fmt(origin, sizeof origin, "%s://%s:%u", b.scheme, b.host, b.port);
	if (ref[0] == '/' && ref[1] == '/')
		return str_fmt("%s:%s", b.scheme, ref);
	if (ref[0] == '/')
		return str_fmt("%s%s", origin, ref);
	if (ref[0] == '#')
		return str_fmt("%s%s%s", origin, b.path, ref);
	if (ref[0] == '?') {
		char path[2048];
		str_copy(path, b.path, sizeof path);
		char *q = (char *)str_chr(path, '?');
		if (q)
			*q = 0;
		return str_fmt("%s%s%s", origin, path, ref);
	}
	/* Relative to the base's folder, then tidy "." and "..". */
	char dir[2048];
	str_copy(dir, b.path, sizeof dir);
	char *q = (char *)str_chr(dir, '?');
	if (q)
		*q = 0;
	char *slash = (char *)str_rchr(dir, '/');
	if (slash)
		slash[1] = 0;
	char *joined = str_fmt("%s%s", dir, ref);
	char *qpart = (char *)str_chr(joined, '?');
	char *query = NULL;
	if (qpart) {
		query = str_dup(qpart);
		*qpart = 0;
	}
	usize n;
	char **parts = str_split(joined + 1, '/', &n);
	StrBuf path = { 0 };
	usize *stack = NEW_ARRAY(usize, n + 1);
	usize depth = 0;
	for (usize i = 0; i < n; i++) {
		if (str_eq(parts[i], ".") || (parts[i][0] == 0 && i + 1 < n))
			continue;
		if (str_eq(parts[i], "..")) {
			if (depth)
				depth--;
			continue;
		}
		stack[depth++] = i;
	}
	for (usize i = 0; i < depth; i++) {
		sb_addc(&path, '/');
		sb_adds(&path, parts[stack[i]]);
	}
	if (n && (str_eq(parts[n - 1], "..") || str_eq(parts[n - 1], ".") || parts[n - 1][0] == 0))
		sb_addc(&path, '/');
	if (!path.len)
		sb_addc(&path, '/');
	char *out = str_fmt("%s%s%s", origin, sb_str(&path), query ? query : "");
	sb_free(&path);
	mem_free(stack);
	str_split_free(parts, n);
	mem_free(joined);
	mem_free(query);
	return out;
}

char *url_encode(const char *s)
{
	StrBuf b = { 0 };
	for (const u8 *p = (const u8 *)s; *p; p++) {
		if (is_alnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
			sb_addc(&b, (char)*p);
		else
			sb_fmt(&b, "%%%02X", *p);
	}
	return b.data ? sb_take(&b) : str_dup("");
}

/* ---- HTTP ---------------------------------------------------------------------------------------- */

typedef struct {
	int fd;
	Tls *tls;
	u8 buf[16384];
	usize len, off;
} Conn;

static long conn_read(Conn *c, void *out, usize n)
{
	if (c->off < c->len) {
		usize k = MIN(n, c->len - c->off);
		memcpy(out, c->buf + c->off, k);
		c->off += k;
		return (long)k;
	}
	if (c->tls)
		return tls_read(c->tls, out, n);
	for (;;) {
		long r = sys_read(c->fd, out, n);
		if (r != -EINTR)
			return r;
	}
}

static bool conn_fill(Conn *c)
{
	if (c->off < c->len)
		return true;
	long r = c->tls ? tls_read(c->tls, c->buf, sizeof c->buf) : sys_read(c->fd, c->buf, sizeof c->buf);
	if (r <= 0)
		return false;
	c->len = (usize)r, c->off = 0;
	return true;
}

static bool conn_line(Conn *c, StrBuf *line)
{
	sb_clear(line);
	for (;;) {
		if (!conn_fill(c))
			return line->len > 0;
		u8 ch = c->buf[c->off++];
		if (ch == '\n') {
			if (line->len && line->data[line->len - 1] == '\r')
				line->data[--line->len] = 0;
			return true;
		}
		sb_addc(line, (char)ch);
		if (line->len > 65536)
			return false;
	}
}

static bool conn_write(Conn *c, const void *data, usize n)
{
	return c->tls ? tls_write(c->tls, data, n) : write_all(c->fd, data, n);
}

static void conn_close(Conn *c)
{
	if (c->tls)
		tls_close(c->tls);
	else if (c->fd >= 0)
		sys_close(c->fd);
	c->tls = NULL, c->fd = -1;
}

const char *http_header(const HttpResponse *r, const char *name, char *out, usize cap)
{
	usize nl = strlen(name);
	for (const char *p = r->headers; p && *p;) {
		const char *eol = str_chr(p, '\n');
		usize len = eol ? (usize)(eol - p) : strlen(p);
		if (len > nl && p[nl] == ':' && str_eq_nocase_n(p, name, nl)) {
			const char *v = p + nl + 1;
			while (*v == ' ')
				v++;
			str_copy(out, v, MIN(cap, (usize)(p + len - v) + 1));
			return out;
		}
		p = eol ? eol + 1 : NULL;
	}
	return NULL;
}

static bool append_body(HttpResponse *r, usize *cap, const u8 *data, usize n, usize max)
{
	if (r->body_len + n > max) {
		str_copy(r->error, "the download is too large", sizeof r->error);
		return false;
	}
	if (r->body_len + n + 1 > *cap) {
		*cap = MAX(*cap * 2, r->body_len + n + 1);
		r->body = mem_realloc(r->body, *cap);
	}
	memcpy(r->body + r->body_len, data, n);
	r->body_len += n;
	r->body[r->body_len] = 0;
	return true;
}

static u8 *gunzip(const u8 *d, usize n, usize *out_len)
{
	if (n < 18 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8)
		return NULL;
	u8 flags = d[3];
	usize off = 10;
	if (flags & 4)
		off += 2 + (usize)(d[off] | d[off + 1] << 8);
	if (flags & 8)
		while (off < n && d[off++])
			;
	if (flags & 16)
		while (off < n && d[off++])
			;
	if (flags & 2)
		off += 2;
	if (off >= n)
		return NULL;
	return zlib_inflate(d + off, n - off, out_len, false);
}

static bool open_conn(Conn *c, const Url *u, int timeout, char *err, usize errcap)
{
	memset(c, 0, sizeof *c);
	c->fd = -1;
	bool https = str_eq(u->scheme, "https");
	/* An HTTPS proxy from the environment, used for https only. */
	const char *proxy = https ? env_get("https_proxy") : NULL;
	if (!proxy && https)
		proxy = env_get("HTTPS_PROXY");
	if (proxy && *proxy) {
		Url pu;
		if (!url_parse(proxy, &pu)) {
			str_copy(err, "the proxy setting is invalid", errcap);
			return false;
		}
		c->fd = tcp_connect(pu.host, pu.port, timeout, err, errcap);
		if (c->fd < 0)
			return false;
		char req[600];
		int n = fmt(req, sizeof req, "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n\r\n", u->host, u->port, u->host, u->port);
		StrBuf line = { 0 };
		bool ok = write_all(c->fd, req, (usize)n) && conn_line(c, &line) && str_find(sb_str(&line), " 200");
		while (ok && conn_line(c, &line) && line.len)
			;
		sb_free(&line);
		if (!ok) {
			str_copy(err, "the proxy refused the connection", errcap);
			conn_close(c);
			return false;
		}
	} else {
		c->fd = tcp_connect(u->host, u->port, timeout, err, errcap);
		if (c->fd < 0)
			return false;
	}
	if (https) {
		c->tls = tls_connect(c->fd, u->host, err, errcap);
		if (!c->tls) {
			sys_close(c->fd);
			c->fd = -1;
			return false;
		}
	}
	return true;
}

HttpResponse *http_request(const char *url, const HttpOptions *opts)
{
	HttpOptions o = opts ? *opts : (HttpOptions){ 0 };
	if (!o.method)
		o.method = "GET";
	if (!o.timeout_ms)
		o.timeout_ms = 15000;
	if (!o.max_body)
		o.max_body = 64u << 20;
	if (!o.max_redirects)
		o.max_redirects = 5;
	HttpResponse *r = NEW(HttpResponse);
	str_copy(r->final_url, url, sizeof r->final_url);
	for (int hop = 0; hop <= o.max_redirects; hop++) {
		Url u;
		if (!url_parse(r->final_url, &u) || (!str_eq(u.scheme, "http") && !str_eq(u.scheme, "https"))) {
			str_copy(r->error, "the address is not valid", sizeof r->error);
			return r;
		}
		Conn c;
		if (!open_conn(&c, &u, o.timeout_ms, r->error, sizeof r->error))
			return r;
		StrBuf req = { 0 };
		bool default_port = u.port == (str_eq(u.scheme, "https") ? 443 : 80);
		sb_fmt(&req, "%s %s HTTP/1.1\r\nHost: %s", o.method, u.path, u.host);
		if (!default_port)
			sb_fmt(&req, ":%u", u.port);
		sb_fmt(&req, "\r\nUser-Agent: %s\r\nAccept: %s\r\nAccept-Encoding: gzip\r\nConnection: close\r\n",
		       o.user_agent ? o.user_agent : "Zurbax/1.0", o.accept ? o.accept : "*/*");
		if (o.hop_headers) {
			char *extra = o.hop_headers(o.user, r->final_url);
			if (extra)
				sb_adds(&req, extra);
			mem_free(extra);
		}
		if (o.body)
			sb_fmt(&req, "Content-Length: %llu\r\n", (unsigned long long)o.body_len);
		if (o.headers)
			sb_adds(&req, o.headers);
		sb_adds(&req, "\r\n");
		bool ok = conn_write(&c, req.data, req.len) && (!o.body || conn_write(&c, o.body, o.body_len));
		sb_free(&req);
		StrBuf line = { 0 };
		if (!ok || !conn_line(&c, &line) || !str_starts(sb_str(&line), "HTTP/")) {
			str_copy(r->error, "the site didn't answer", sizeof r->error);
			sb_free(&line);
			conn_close(&c);
			return r;
		}
		const char *sp = str_chr(sb_str(&line), ' ');
		int status = sp ? (int)str_to_int(sp + 1, NULL) : 0;
		StrBuf headers = { 0 };
		while (conn_line(&c, &line) && line.len) {
			sb_adds(&headers, sb_str(&line));
			sb_addc(&headers, '\n');
		}
		sb_free(&line);
		mem_free(r->headers);
		r->headers = headers.data ? sb_take(&headers) : str_dup("");
		if (o.hop_response)
			o.hop_response(o.user, r->final_url, status, r->headers);
		char value[2048];
		if (status >= 300 && status < 400 && status != 304 && http_header(r, "Location", value, sizeof value)) {
			char *next = url_resolve(r->final_url, value);
			str_copy(r->final_url, next, sizeof r->final_url);
			mem_free(next);
			conn_close(&c);
			if (status == 303 || ((status == 301 || status == 302) && str_eq(o.method, "POST")))
				o.method = "GET", o.body = NULL;
			continue;
		}
		/* The body. */
		usize cap = 0, total = 0;
		mem_free(r->body);
		r->body = NULL, r->body_len = 0;
		bool chunked = http_header(r, "Transfer-Encoding", value, sizeof value) && str_find_nocase(value, "chunked");
		if (http_header(r, "Content-Length", value, sizeof value))
			total = (usize)str_to_int(value, NULL);
		bool good = true;
		u8 buf[16384];
		if (str_eq(o.method, "HEAD") || status == 204 || status == 304) {
			/* no body */
		} else if (chunked) {
			StrBuf cl = { 0 };
			for (;;) {
				if (!conn_line(&c, &cl)) {
					good = false;
					break;
				}
				usize size = 0;
				for (const char *p = sb_str(&cl); is_xdigit(*p); p++)
					size = size * 16 + (usize)(is_digit(*p) ? *p - '0' : (to_lower(*p) - 'a' + 10));
				if (size == 0)
					break;
				while (size) {
					long k = conn_read(&c, buf, MIN(size, sizeof buf));
					if (k <= 0 || !append_body(r, &cap, buf, (usize)k, o.max_body)) {
						good = false;
						break;
					}
					size -= (usize)k;
					if (o.progress)
						o.progress(o.user, r->body_len, 0);
				}
				if (!good || !conn_line(&c, &cl))
					break;
			}
			sb_free(&cl);
		} else {
			for (;;) {
				if (total && r->body_len >= total)
					break;
				long k = conn_read(&c, buf, sizeof buf);
				if (k == 0)
					break;
				if (k < 0) {
					good = r->body_len > 0 && !total;
					break;
				}
				if (!append_body(r, &cap, buf, (usize)k, o.max_body)) {
					good = false;
					break;
				}
				if (o.progress)
					o.progress(o.user, r->body_len, total);
			}
		}
		conn_close(&c);
		if (!good) {
			if (!r->error[0])
				str_copy(r->error, "the download was interrupted", sizeof r->error);
			return r;
		}
		if (http_header(r, "Content-Encoding", value, sizeof value) && str_find_nocase(value, "gzip")) {
			usize n;
			u8 *plain = gunzip(r->body, r->body_len, &n);
			if (!plain) {
				str_copy(r->error, "the site sent a damaged response", sizeof r->error);
				return r;
			}
			mem_free(r->body);
			r->body = mem_realloc(plain, n + 1);
			r->body[n] = 0;
			r->body_len = n;
		}
		r->status = status;
		return r;
	}
	str_copy(r->error, "the site redirected too many times", sizeof r->error);
	return r;
}

HttpResponse *http_get(const char *url)
{
	return http_request(url, NULL);
}

void http_response_free(HttpResponse *r)
{
	if (!r)
		return;
	mem_free(r->headers);
	mem_free(r->body);
	mem_free(r);
}
