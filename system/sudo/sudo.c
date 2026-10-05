/*
 * sudo — run one command with full rights.
 *
 * Installed setuid root. It asks for the user's own password (checked
 * against the root-only account file), remembers a correct answer for five
 * minutes on that terminal, and then runs the command as root with a clean
 * environment. Nothing from the caller's environment decides which files
 * are trusted.
 */
#include "zr.h"

#define SHADOW "/data/system/shadow"
#define USERS "/data/system/users.conf"
#define STAMP_DIR "/run/sudo"
#define GRACE_SECONDS (5 * 60)

static void fail(const char *msg)
{
	eprint("sudo: %s\n", msg);
	exit(1);
}

static void tty_name(char *out, usize cap)
{
	long n = sys_readlink("/proc/self/fd/0", out, cap - 1);
	out[n > 0 ? n : 0] = 0;
}

static char *read_password(const char *prompt)
{
	int fd = (int)sys_open("/dev/tty", O_RDWR | O_CLOEXEC, 0);
	if (fd < 0)
		fd = 0;
	struct termios old, t;
	bool have = sys_ioctl(fd, TCGETS, &old) == 0;
	if (have) {
		t = old;
		t.c_lflag &= ~(u32)ECHO;
		t.c_lflag |= ICANON;
		sys_ioctl(fd, TCSETS, &t);
	}
	sys_write(fd, prompt, strlen(prompt));
	char buf[256];
	usize n = 0;
	for (;;) {
		char c;
		long r = sys_read(fd, &c, 1);
		if (r == -EINTR)
			continue;
		if (r <= 0 || c == '\n' || c == '\r')
			break;
		if (n < sizeof buf - 1)
			buf[n++] = c;
	}
	buf[n] = 0;
	if (have)
		sys_ioctl(fd, TCSETS, &old);
	sys_write(fd, "\n", 1);
	if (fd != 0)
		sys_close(fd);
	char *pw = str_dup(buf);
	memset(buf, 0, sizeof buf);
	return pw;
}

static bool recently_verified(u32 uid, const char *tty)
{
	char path[128];
	fmt(path, sizeof path, STAMP_DIR "/%u", uid);
	char *saved = file_read(path, NULL);
	bool ok = saved && str_eq(saved, tty) && time_unix() - file_mtime(path) < GRACE_SECONDS;
	mem_free(saved);
	return ok;
}

static void remember(u32 uid, const char *tty)
{
	dir_make(STAMP_DIR, 0700);
	char path[128];
	fmt(path, sizeof path, STAMP_DIR "/%u", uid);
	file_write(path, tty, strlen(tty));
	sys_chmod(path, 0600);
}

/* Quotes an argument for Flint so it arrives unchanged. */
static void quote(StrBuf *sb, const char *a)
{
	sb_addc(sb, '\'');
	for (; *a; a++) {
		if (*a == '\'')
			sb_adds(sb, "'\\''");
		else
			sb_addc(sb, *a);
	}
	sb_addc(sb, '\'');
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		eprint("usage: sudo COMMAND [ARGUMENTS...]\n       sudo -s     open a shell with full rights\n       sudo -k     forget the password now\n");
		return 2;
	}
	u32 uid = (u32)sys_getuid();
	if (sys_geteuid() != 0)
		fail("sudo isn't installed with the right to change users");
	char tty[256];
	tty_name(tty, sizeof tty);
	if (str_eq(argv[1], "-k")) {
		char path[128];
		fmt(path, sizeof path, STAMP_DIR "/%u", uid);
		sys_unlink(path);
		return 0;
	}
	if (uid != 0 && !recently_verified(uid, tty)) {
		Conf *sh = conf_load(SHADOW);
		const char *record = conf_get(sh, "password", "");
		Conf *users = conf_load(USERS);
		const char *name = conf_get(users, "user", "user");
		if (record[0]) {
			bool ok = false;
			for (int attempt = 0; attempt < 3 && !ok; attempt++) {
				char prompt[160];
				fmt(prompt, sizeof prompt, "[sudo] password for %s: ", name);
				char *pw = read_password(prompt);
				ok = password_check(pw, record);
				memset(pw, 0, strlen(pw));
				mem_free(pw);
				if (!ok) {
					sleep_ms(1500);
					eprint("Sorry, that's not the password.\n");
				}
			}
			if (!ok)
				fail("three wrong passwords");
		}
		/* Without a password, the account's owner is the administrator. */
		conf_free(sh);
		conf_free(users);
		remember(uid, tty);
	}
	if (sys_setgroups(0, NULL) < 0 || sys_setresgid(0, 0, 0) < 0 || sys_setresuid(0, 0, 0) < 0)
		fail("couldn't change to the administrator");

	/* A clean environment: only what's needed, with a fixed search path. */
	const char *keep[] = { "TERM", "LANG", "COLORTERM", "HOME", "ZUI_SOCKET", "TZ" };
	char *envp[16];
	int ne = 0;
	envp[ne++] = "PATH=/system/bin:/data/apps/bin";
	envp[ne++] = "USER=root";
	envp[ne++] = "LOGNAME=root";
	char sudo_user[64];
	fmt(sudo_user, sizeof sudo_user, "SUDO_UID=%u", uid);
	envp[ne++] = sudo_user;
	for (usize i = 0; i < ARRAY_LEN(keep); i++) {
		const char *v = env_get(keep[i]);
		if (v)
			envp[ne++] = str_fmt("%s=%s", keep[i], v);
	}
	envp[ne] = NULL;

	if (str_eq(argv[1], "-s")) {
		char *sh[] = { "/system/bin/flint", NULL };
		sys_execve(sh[0], sh, envp);
		fail("couldn't start the shell");
	}
	/* Programs run directly; anything else (Flint's built-in commands) runs through Flint. */
	char path[512];
	bool found = false;
	if (str_chr(argv[1], '/')) {
		str_copy(path, argv[1], sizeof path);
		found = file_exists(path);
	} else {
		const char *dirs[] = { "/system/bin", "/data/apps/bin" };
		for (int i = 0; i < 2 && !found; i++) {
			fmt(path, sizeof path, "%s/%s", dirs[i], argv[1]);
			struct stat st;
			found = sys_stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111) && !str_eq(argv[1], "flint");
		}
	}
	if (found) {
		sys_execve(path, argv + 1, envp);
		fail("couldn't run the command");
	}
	StrBuf line = { 0 };
	for (int i = 1; i < argc; i++) {
		if (i > 1)
			sb_addc(&line, ' ');
		quote(&line, argv[i]);
	}
	char *sh[] = { "/system/bin/flint", "-c", (char *)sb_str(&line), NULL };
	sys_execve(sh[0], sh, envp);
	fail("couldn't start the shell");
	return 1;
}
