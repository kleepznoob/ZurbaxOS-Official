/* Tests for libzr. Run with `make test`. */
#include "zr.h"

static int failures, checks;

#define CHECK(cond)                                                          \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			failures++;                                          \
			eprint("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                            \
	} while (0)

static void check_fmt(const char *want, const char *f, ...) PRINTF_LIKE(2, 3);
static void check_fmt(const char *want, const char *f, ...)
{
	char buf[128];
	va_list ap;
	va_start(ap, f);
	fmt_v(buf, sizeof buf, f, ap);
	va_end(ap);
	checks++;
	if (!str_eq(buf, want)) {
		failures++;
		eprint("  FAIL fmt(\"%s\") = \"%s\", want \"%s\"\n", f, buf, want);
	}
}

static bool near(f64 a, f64 b, f64 tol)
{
	return fabs(a - b) <= tol * MAX(1.0, fabs(b));
}

static void test_format(void)
{
	check_fmt("42", "%d", 42);
	check_fmt("-7", "%d", -7);
	check_fmt("  12", "%4d", 12);
	check_fmt("0012", "%04d", 12);
	check_fmt("12  |", "%-4d|", 12);
	check_fmt("ff", "%x", 255);
	check_fmt("0XFF", "%#X", 255);
	check_fmt("18446744073709551615", "%llu", 18446744073709551615ull);
	check_fmt("hello", "%s", "hello");
	check_fmt("hel", "%.3s", "hello");
	check_fmt("  hi", "%4s", "hi");
	check_fmt("x", "%c", 'x');
	check_fmt("3.14", "%.2f", 3.14159);
	check_fmt("2.50", "%.2f", 2.4999999);
	check_fmt("-0.5", "%.1f", -0.5);
	check_fmt("100", "%.0f", 99.5);
	check_fmt("1.500000e+03", "%e", 1500.0);
	check_fmt("0.3", "%.15g", 0.1 + 0.2);
	check_fmt("1e+20", "%g", 1e20);
	check_fmt("0.0001", "%g", 0.0001);
	check_fmt("1e-05", "%g", 0.00001);
	check_fmt("123457", "%g", 123456.7);
	check_fmt("10", "%g", 9.9999999);
	check_fmt("100%", "%d%%", 100);
	char b[32];
	fmt_bytes(b, sizeof b, 1536000);
	CHECK(str_eq(b, "1.5 MB"));
}

static void test_strings(void)
{
	CHECK(str_eq("abc", "abc"));
	CHECK(!str_eq("abc", "abd"));
	CHECK(str_cmp_natural("file2", "file10") < 0);
	CHECK(str_cmp_natural("File10", "file9") > 0);
	CHECK(str_starts("zurbax", "zur"));
	CHECK(str_ends("photo.png", ".png"));
	CHECK(str_find("hello world", "o w") != NULL);
	CHECK(str_find_nocase("Hello World", "WORLD") != NULL);
	CHECK(str_to_int("  -123x", NULL) == -123);
	CHECK(str_to_int("0x1F", NULL) == 31);
	CHECK(near(str_to_float("3.25e2", NULL), 325, 1e-15));
	CHECK(near(str_to_float("-0.001", NULL), -0.001, 1e-15));
	usize n;
	char **parts = str_split("a,b,,c", ',', &n);
	CHECK(n == 4 && str_eq(parts[2], "") && str_eq(parts[3], "c"));
	str_split_free(parts, n);
	char *r = str_replace("one two one", "one", "1");
	CHECK(str_eq(r, "1 two 1"));
	mem_free(r);
	char t[] = "  padded \n";
	CHECK(str_eq(str_trim(t), "padded"));

	u32 cp;
	CHECK(utf8_decode("\xc3\xa9", 2, &cp) == 2 && cp == 0xe9);
	CHECK(utf8_decode("\xe2\x82\xac", 3, &cp) == 3 && cp == 0x20ac);
	char enc[4];
	CHECK(utf8_encode(0x1f600, enc) == 4);
	CHECK(utf8_count("h\xc3\xa9llo", 6) == 5);
	CHECK(utf8_prev("h\xc3\xa9", 3) == 1);

	StrBuf sb = { 0 };
	sb_adds(&sb, "Hello");
	sb_fmt(&sb, ", %s #%d", "Zurbax", 1);
	sb_insert(&sb, 0, ">", 1);
	sb_remove(&sb, 1, 1);
	CHECK(str_eq(sb_str(&sb), ">ello, Zurbax #1"));
	sb_free(&sb);
	/* Inserting into an empty buffer must leave it terminated. */
	StrBuf e = { 0 };
	sb_insert(&e, 0, "ab", 2);
	CHECK(e.len == 2 && strlen(sb_str(&e)) == 2);
	sb_free(&e);
}

static int cmp_int(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	return *(const int *)a - *(const int *)b;
}

static void test_containers(void)
{
	VEC(int) v = { 0 };
	for (int i = 0; i < 1000; i++)
		vec_push(&v, (i * 7919) % 1000);
	sort(v.data, v.len, sizeof(int), cmp_int, NULL);
	bool sorted = true;
	for (usize i = 0; i < v.len; i++)
		sorted &= v.data[i] == (int)i;
	CHECK(sorted);
	vec_remove(&v, 0);
	CHECK(v.data[0] == 1 && v.len == 999);
	vec_insert(&v, 0, 0);
	CHECK(v.data[0] == 0 && v.data[1] == 1);
	vec_free(&v);

	Map m = { 0 };
	char key[16];
	for (int i = 0; i < 500; i++) {
		fmt(key, sizeof key, "k%d", i);
		map_set(&m, key, (void *)(uintptr_t)(i + 1));
	}
	CHECK(m.len == 500);
	CHECK((uintptr_t)map_get(&m, "k123") == 124);
	for (int i = 0; i < 500; i += 2) {
		fmt(key, sizeof key, "k%d", i);
		map_del(&m, key);
	}
	CHECK(m.len == 250);
	CHECK(map_get(&m, "k122") == NULL);
	CHECK((uintptr_t)map_get(&m, "k123") == 124);
	map_free(&m, NULL);

	/* allocator: many sizes, realloc growth */
	void *ptrs[200];
	for (int i = 0; i < 200; i++) {
		ptrs[i] = mem_zalloc((usize)(i * 37 + 1));
		memset(ptrs[i], i, (usize)(i * 37 + 1));
	}
	bool ok = true;
	for (int i = 0; i < 200; i++)
		ok &= ((u8 *)ptrs[i])[i * 37] == (u8)i;
	CHECK(ok);
	for (int i = 0; i < 200; i++)
		mem_free(ptrs[i]);
	char *grow = NULL;
	for (int i = 1; i <= 200000; i *= 3)
		grow = mem_realloc(grow, (usize)i), grow[i - 1] = 'z';
	CHECK(grow[59048] == 'z');
	mem_free(grow);
}

static void test_math(void)
{
	CHECK(near(sin(PI / 6), 0.5, 1e-14));
	CHECK(near(cos(PI / 3), 0.5, 1e-14));
	CHECK(near(sin(100.0), -0.50636564110975879, 1e-12));
	CHECK(near(tan(PI / 4), 1.0, 1e-14));
	CHECK(near(atan(1.0), PI / 4, 1e-15));
	CHECK(near(atan2(-1.0, -1.0), -3 * PI / 4, 1e-15));
	CHECK(near(asin(0.5), PI / 6, 1e-14));
	CHECK(near(exp(1.0), 2.718281828459045, 1e-15));
	CHECK(near(exp(-20.0), 2.061153622438558e-09, 1e-14));
	CHECK(near(log(10.0), 2.302585092994046, 1e-15));
	CHECK(near(log(1e-300), -690.7755278982137, 1e-15));
	CHECK(near(pow(2.0, 0.5), 1.4142135623730951, 1e-15));
	CHECK(pow(2.0, 10.0) == 1024.0);
	CHECK(near(pow(-8.0, 1.0 / 3.0 * 3), -8.0, 1e-12));
	CHECK(near(cbrt(27.0), 3.0, 1e-15));
	CHECK(floor(-2.5) == -3 && ceil(-2.5) == -2 && round(2.5) == 3 && round(-2.5) == -3);
	CHECK(near(fmod(10.5, 3.0), 1.5, 1e-15));
	CHECK(near(hypot(3.0, 4.0), 5.0, 1e-15));
	int e;
	f64 m = frexp(48.0, &e);
	CHECK(m == 0.75 && e == 6);
	CHECK(ldexp(0.75, 6) == 48.0);
	CHECK(is_nan(sqrt(-1.0)));
}

static void test_time(void)
{
	Civil c;
	civil_from_unix(0, &c);
	CHECK(c.year == 1970 && c.month == 1 && c.day == 1 && c.wday == 4);
	civil_from_unix(1772668800, &c); /* 2026-03-05 00:00:00 UTC */
	CHECK(c.year == 2026 && c.month == 3 && c.day == 5 && c.wday == 4);
	CHECK(unix_from_civil(&c) == 1772668800);
	civil_from_unix(951782400, &c); /* 2000-02-29 */
	CHECK(c.month == 2 && c.day == 29);
	civil_from_unix(-86400, &c);
	CHECK(c.year == 1969 && c.month == 12 && c.day == 31);
	CHECK(days_in_month(2024, 2) == 29 && days_in_month(2100, 2) == 28);
	u64 a = time_mono_ns();
	sleep_ms(20);
	u64 b = time_mono_ns();
	CHECK(b - a >= 19000000);
	CHECK(time_unix() > 1700000000);
}

static int loop_ticks;
static void tick(Loop *l, void *data)
{
	UNUSED(data);
	if (++loop_ticks == 3)
		loop_quit(l);
}

static int thread_counter;
static Mutex counter_lock;
static int threads_done;
static void worker(void *arg)
{
	for (int i = 0; i < 10000; i++) {
		mutex_lock(&counter_lock);
		thread_counter++;
		mutex_unlock(&counter_lock);
	}
	atomic_add(&threads_done, 1);
	futex_wake(&threads_done, 1);
	UNUSED(arg);
}

static void test_os(void)
{
	Loop *l = loop_new();
	loop_timer(l, 5, 5, tick, NULL);
	u64 t0 = time_mono_ms();
	loop_run(l);
	CHECK(loop_ticks == 3);
	CHECK(time_mono_ms() - t0 >= 14);
	loop_free(l);

	for (int i = 0; i < 4; i++)
		CHECK(thread_spawn(worker, NULL) > 0);
	while (atomic_load(&threads_done) < 4)
		futex_wait(&threads_done, atomic_load(&threads_done), 100);
	CHECK(thread_counter == 40000);

	char *argv[] = { "/bin/echo", "from child", NULL };
	int status;
	char *out = proc_run_capture(argv, &status);
	CHECK(out && str_eq(out, "from child\n") && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	mem_free(out);

	int sv[2];
	CHECK(sys_socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	int pfd[2];
	sys_pipe2(pfd, O_CLOEXEC);
	CHECK(unix_send(sv[0], "fd!", 3, &pfd[1], 1) == 3);
	char buf[8];
	int got[2], ngot = 0;
	CHECK(unix_recv(sv[1], buf, sizeof buf, got, 2, &ngot) == 3 && ngot == 1);
	sys_write(got[0], "x", 1);
	CHECK(sys_read(pfd[0], buf, 1) == 1 && buf[0] == 'x');

	u32 r1 = rand_u32(), r2 = rand_u32();
	CHECK(r1 != r2);
	u32 v = rand_range(5, 9);
	CHECK(v >= 5 && v <= 9);
}

static void test_files(void)
{
	const char *dir = "/tmp/zr-test";
	path_remove_all(dir);
	CHECK(dir_make("/tmp/zr-test/a/b", 0755));
	CHECK(file_write("/tmp/zr-test/a/file10.txt", "x", 1));
	CHECK(file_write_atomic("/tmp/zr-test/a/file2.txt", "hello\nworld\n", 12));
	usize len;
	char *s = file_read("/tmp/zr-test/a/file2.txt", &len);
	CHECK(s && len == 12 && str_eq(s, "hello\nworld\n"));
	mem_free(s);
	DirList list = { 0 };
	CHECK(dir_list("/tmp/zr-test/a", &list, true));
	CHECK(list.len == 3 && str_eq(list.data[0].name, "b") && str_eq(list.data[1].name, "file2.txt"));
	dir_list_free(&list);
	CHECK(str_eq(path_base("/x/y/photo.jpg"), "photo.jpg"));
	CHECK(str_eq(path_ext("/x/y/photo.jpg"), "jpg"));
	char *n = path_normalize("/a/./b/../c//d/");
	CHECK(str_eq(n, "/a/c/d"));
	mem_free(n);

	Conf *c = conf_load("/tmp/zr-test/settings.conf");
	conf_set(c, "theme", "dark");
	conf_set_int(c, "volume", 70);
	conf_set_bool(c, "wifi", true);
	CHECK(conf_save(c));
	conf_free(c);
	c = conf_load("/tmp/zr-test/settings.conf");
	CHECK(str_eq(conf_get(c, "theme", ""), "dark") && conf_get_int(c, "volume", 0) == 70 && conf_get_bool(c, "wifi", false));
	conf_free(c);
	CHECK(path_remove_all(dir) && !dir_exists(dir));
}

static void test_json(void)
{
	const char *text = "{\"name\":\"Zurbax\",\"version\":1.5,\"tags\":[\"fast\",\"clean\"],"
			   "\"nested\":{\"ok\":true,\"none\":null},\"esc\":\"a\\\"b\\u00e9\\n\"}";
	Json *j = json_parse(text, strlen(text));
	CHECK(j != NULL);
	CHECK(str_eq(json_str(json_get(j, "name"), ""), "Zurbax"));
	CHECK(json_num(json_get(j, "version"), 0) == 1.5);
	CHECK(str_eq(json_str(json_path(j, "tags.1"), ""), "clean"));
	CHECK(json_bool(json_path(j, "nested.ok"), false));
	CHECK(json_path(j, "nested.none")->type == JSON_NULL);
	CHECK(str_eq(json_str(json_get(j, "esc"), ""), "a\"b\xc3\xa9\n"));
	json_free(j);
	CHECK(json_parse("{\"a\":}", 6) == NULL);
	CHECK(json_parse("[1,2", 4) == NULL);
	StrBuf sb = { 0 };
	sb_json_string(&sb, "q\"\n");
	CHECK(str_eq(sb_str(&sb), "\"q\\\"\\n\""));
	sb_free(&sb);
}

static void test_tz(void)
{
	/* Built-in rules (no zone files are installed on the build machine's /system). */
	CHECK(tz_set("Europe/Bucharest"));
	CHECK(tz_offset(1782907200) == 3 * 3600);   /* July: summer time */
	CHECK(tz_offset(1768478400) == 2 * 3600);   /* January */
	CHECK(tz_offset(1774745999) == 2 * 3600);   /* 29 Mar 2026 00:59:59 UTC */
	CHECK(tz_offset(1774746000) == 3 * 3600);   /* 01:00 UTC: clocks go forward */
	CHECK(tz_offset(1792889999) == 3 * 3600);   /* 25 Oct 2026 00:59:59 UTC */
	CHECK(tz_offset(1792890000) == 2 * 3600);   /* 01:00 UTC: back to winter time */
	CHECK(tz_offset_in("America/New_York", 1772953199) == -5 * 3600);
	CHECK(tz_offset_in("America/New_York", 1772953200) == -4 * 3600);
	CHECK(tz_offset_in("Australia/Sydney", 1768478400) == 11 * 3600);
	CHECK(tz_offset_in("Australia/Sydney", 1782907200) == 10 * 3600);
	CHECK(tz_offset_in("Asia/Kolkata", 1782907200) == 5 * 3600 + 1800);
	CHECK(tz_offset_in("Asia/Kathmandu", 1782907200) == 5 * 3600 + 2700);
	CHECK(tz_offset_in("America/Phoenix", 1782907200) == -7 * 3600);
	tz_set("UTC");
}

static void test_crypto(void)
{
	u8 h[32];
	char hex[65];
	sha256("abc", 3, h);
	hex_encode(h, 32, hex);
	CHECK(str_eq(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	sha256("", 0, h);
	hex_encode(h, 32, hex);
	CHECK(str_eq(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	sha256(m, strlen(m), h);
	hex_encode(h, 32, hex);
	CHECK(str_eq(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	hmac_sha256("key", 3, "The quick brown fox jumps over the lazy dog", 43, h);
	hex_encode(h, 32, hex);
	CHECK(str_eq(hex, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"));
	u8 dk[32];
	pbkdf2_sha256("password", 8, "salt", 4, 1, dk, 32);
	hex_encode(dk, 32, hex);
	CHECK(str_eq(hex, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
	pbkdf2_sha256("password", 8, "salt", 4, 4096, dk, 32);
	hex_encode(dk, 32, hex);
	CHECK(str_eq(hex, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
	char *rec = password_hash("correct horse");
	CHECK(password_check("correct horse", rec));
	CHECK(!password_check("wrong horse", rec));
	mem_free(rec);
}

int main(void)
{
	test_crypto();
	test_format();
	test_strings();
	test_containers();
	test_math();
	test_time();
	test_os();
	test_files();
	test_json();
	test_tz();
	print("libzr: %d/%d checks passed\n", checks - failures, checks);
	return failures ? 1 : 0;
}
