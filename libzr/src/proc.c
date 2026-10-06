/* Environment, processes, signals, randomness and threads. */
#include "zr.h"

/* ---- Environment ------------------------------------------------------------ */

const char *env_get(const char *name)
{
	usize n = strlen(name);
	for (char **e = environ; e && *e; e++)
		if (memcmp(*e, name, n) == 0 && (*e)[n] == '=')
			return *e + n + 1;
	return NULL;
}

void env_set(const char *name, const char *value)
{
	usize n = strlen(name), count = 0;
	for (char **e = environ; e && *e; e++, count++) {
		if (memcmp(*e, name, n) == 0 && (*e)[n] == '=') {
			*e = str_fmt("%s=%s", name, value);
			return;
		}
	}
	char **env = mem_alloc(sizeof(char *) * (count + 2));
	memcpy(env, environ, sizeof(char *) * count);
	env[count] = str_fmt("%s=%s", name, value);
	env[count + 1] = NULL;
	environ = env;
}

/* ---- Processes ----------------------------------------------------------------- */

static long fork_process(void)
{
	return syscall5(SYS_clone, SIGCHLD, 0, 0, 0, 0);
}

int proc_spawn(const char *path, char *const argv[], char *const envp[], const SpawnOpts *o)
{
	long pid = fork_process();
	if (pid != 0)
		return (int)pid;
	/* Child: only async-safe syscalls from here on. */
	u64 none = 0;
	sys_rt_sigprocmask(SIG_SETMASK, &none, NULL);
	if (o) {
		if (o->new_session)
			sys_setsid();
		if (o->stdin_fd >= 0 && o->stdin_fd != 0)
			sys_dup3(o->stdin_fd, 0, 0);
		if (o->stdout_fd >= 0 && o->stdout_fd != 1)
			sys_dup3(o->stdout_fd, 1, 0);
		if (o->stderr_fd >= 0 && o->stderr_fd != 2)
			sys_dup3(o->stderr_fd, 2, 0);
		if (o->controlling_tty)
			sys_ioctl(0, TIOCSCTTY, 0);
		if (o->cwd)
			sys_chdir(o->cwd);
		if (o->drop_privileges) {
			sys_setgroups(0, NULL);
			if (sys_setresgid(o->gid, o->gid, o->gid) < 0 || sys_setresuid(o->uid, o->uid, o->uid) < 0)
				syscall1(SYS_exit, 126);
		}
	}
	sys_execve(path, argv, envp ? envp : environ);
	syscall1(SYS_exit, 127);
	__builtin_unreachable();
}

int proc_wait(int pid, int *status, bool nohang)
{
	int st = 0;
	long r;
	do
		r = sys_wait4(pid, &st, nohang ? WNOHANG : 0, NULL);
	while (r == -EINTR);
	if (status)
		*status = st;
	return (int)r;
}

char *proc_run_capture(char *const argv[], int *status)
{
	int fds[2];
	if (sys_pipe2(fds, O_CLOEXEC) < 0)
		return NULL;
	const char *path = argv[0][0] == '/' ? argv[0] : find_program(argv[0]);
	SpawnOpts o = { .stdin_fd = -1, .stdout_fd = fds[1], .stderr_fd = -1 };
	int pid = path ? proc_spawn(path, argv, NULL, &o) : -ENOENT;
	sys_close(fds[1]);
	StrBuf out = { 0 };
	char buf[4096];
	for (;;) {
		long n = sys_read(fds[0], buf, sizeof buf);
		if (n == -EINTR)
			continue;
		if (n <= 0)
			break;
		sb_add(&out, buf, (usize)n);
	}
	sys_close(fds[0]);
	int st = -1;
	if (pid > 0)
		proc_wait(pid, &st, false);
	if (status)
		*status = st;
	return sb_take(&out);
}

const char *proc_self_exe(void)
{
	static char path[512];
	long n = sys_readlink("/proc/self/exe", path, sizeof path - 1);
	path[n > 0 ? n : 0] = 0;
	return path;
}

const char *find_program(const char *name)
{
	static char buf[512];
	if (str_chr(name, '/'))
		return name;
	const char *path = env_get("PATH");
	if (!path)
		path = "/system/bin:/data/apps/bin";
	while (*path) {
		const char *end = str_chr(path, ':');
		usize n = end ? (usize)(end - path) : strlen(path);
		if (n && n + strlen(name) + 2 < sizeof buf) {
			memcpy(buf, path, n);
			buf[n] = '/';
			str_copy(buf + n + 1, name, sizeof buf - n - 1);
			if (sys_access(buf, X_OK) == 0)
				return buf;
		}
		if (!end)
			break;
		path = end + 1;
	}
	return NULL;
}

/* ---- Signals --------------------------------------------------------------------- */

void zr_sigreturn(void);
__asm__(".text\n"
	".global zr_sigreturn\n"
	"zr_sigreturn:\n"
	"	mov $15, %eax\n" /* rt_sigreturn */
	"	syscall\n");

void sig_handle(int sig, void (*fn)(int))
{
	struct kernel_sigaction sa = { 0 };
	sa.handler = fn;
	sa.flags = SA_RESTORER | SA_RESTART;
	sa.restorer = zr_sigreturn;
	sys_rt_sigaction(sig, &sa, NULL);
}

void sig_ignore(int sig)
{
	sig_handle(sig, SIG_IGN);
}

void sig_default(int sig)
{
	sig_handle(sig, SIG_DFL);
}

void sig_block(int sig)
{
	u64 set = 1ull << (sig - 1);
	sys_rt_sigprocmask(SIG_BLOCK, &set, NULL);
}

void sig_unblock(int sig)
{
	u64 set = 1ull << (sig - 1);
	sys_rt_sigprocmask(SIG_UNBLOCK, &set, NULL);
}

/* ---- Randomness ------------------------------------------------------------------ */

void rand_bytes(void *buf, usize n)
{
	u8 *p = buf;
	while (n) {
		long r = sys_getrandom(p, n, 0);
		if (r == -EINTR)
			continue;
		if (r <= 0)
			die("getrandom failed: %s", err_str(r));
		p += r;
		n -= (usize)r;
	}
}

static u64 prng_state;

u32 rand_u32(void)
{
	if (!prng_state)
		rand_bytes(&prng_state, sizeof prng_state), prng_state |= 1;
	/* xorshift64* */
	prng_state ^= prng_state >> 12;
	prng_state ^= prng_state << 25;
	prng_state ^= prng_state >> 27;
	return (u32)((prng_state * 0x2545f4914f6cdd1dull) >> 32);
}

u32 rand_range(u32 lo, u32 hi)
{
	if (hi <= lo)
		return lo;
	return lo + (u32)(((u64)rand_u32() * (hi - lo + 1)) >> 32);
}

f32 rand_f32(void)
{
	return (f32)(rand_u32() >> 8) / (f32)(1u << 24);
}

/* ---- Threads -------------------------------------------------------------------- */

/* long zr_clone(flags, stack_top, fn, arg): the child calls fn(arg) and exits. */
long zr_clone(unsigned long flags, void *stack, void (*fn)(void *), void *arg);
__asm__(".text\n"
	".global zr_clone\n"
	"zr_clone:\n"
	"	sub $16, %rsi\n"
	"	mov %rdx, 0(%rsi)\n"  /* fn  */
	"	mov %rcx, 8(%rsi)\n"  /* arg */
	"	mov $56, %eax\n"      /* clone */
	"	xor %edx, %edx\n"
	"	xor %r10d, %r10d\n"
	"	xor %r8d, %r8d\n"
	"	syscall\n"
	"	test %rax, %rax\n"
	"	jnz 1f\n"
	"	xor %ebp, %ebp\n"
	"	pop %rax\n"
	"	pop %rdi\n"
	"	call *%rax\n"
	"	mov $60, %eax\n"      /* exit (this thread only) */
	"	xor %edi, %edi\n"
	"	syscall\n"
	"1:	ret\n");

int thread_spawn(void (*fn)(void *), void *arg)
{
	const usize size = 8u << 20; /* reserved, not committed: deep recursion is fine */
	u8 *stack = sys_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if ((unsigned long)stack > -4096ul)
		return (int)(long)stack;
	unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
	return (int)zr_clone(flags, stack + size, fn, arg);
}

void futex_wait(int *addr, int expected, i64 timeout_ms)
{
	struct timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000 };
	sys_futex(addr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected, timeout_ms >= 0 ? &ts : NULL);
}

void futex_wake(int *addr, int count)
{
	sys_futex(addr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, count, NULL);
}

/* Mutex states: 0 unlocked, 1 locked, 2 locked with waiters. */
void mutex_lock(Mutex *m)
{
	int c = 0;
	if (__atomic_compare_exchange_n(&m->state, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	if (c != 2)
		c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
	while (c != 0) {
		futex_wait(&m->state, 2, -1);
		c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
	}
}

void mutex_unlock(Mutex *m)
{
	if (__atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE) == 2)
		futex_wake(&m->state, 1);
}
