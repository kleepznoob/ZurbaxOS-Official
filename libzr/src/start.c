/* Program entry, exit and error reporting. */
#include "zr.h"

int main(int argc, char **argv);

char **environ;
static const char *program_name = "?";

/*
 * The kernel enters at _start with the stack holding:
 *   argc, argv[0..argc-1], NULL, envp[...], NULL, auxv pairs...
 * We pass the stack pointer to zr_start in %rdi, aligned for a normal call.
 */
__asm__(".text\n"
	".global _start\n"
	"_start:\n"
	"	xor %rbp, %rbp\n"
	"	mov %rsp, %rdi\n"
	"	and $-16, %rsp\n"
	"	call zr_start\n"
	"	hlt\n");

/* ---- Hardening ----------------------------------------------------------------------
 * Every program is a static position-independent executable, so the kernel
 * puts its code and data at a random address each run (ASLR). Nothing loads
 * it but the kernel, so it applies its own relocations first, before touching
 * any pointer kept in its data; then the relocated tables are made read-only
 * (RELRO), and the stack canary is set from the kernel's random bytes. */

typedef struct {
	i64 tag;
	u64 val;
} ElfDyn;

typedef struct {
	u64 offset, info;
	i64 addend;
} ElfRela;

typedef struct {
	u32 type, flags;
	u64 offset, vaddr, paddr, filesz, memsz, align;
} ElfPhdr;

enum { DT_NULL = 0, DT_RELA = 7, DT_RELASZ = 8, DT_RELAENT = 9, DT_RELRSZ = 35, DT_RELR = 36 };
enum { R_X86_64_RELATIVE = 8, PT_GNU_RELRO = 0x6474e552 };
enum { AT_NULL = 0, AT_PHDR = 3, AT_PHNUM = 5, AT_RANDOM = 25 };

/* both hidden, so they're reached relative to the code, needing no relocation */
extern const ElfDyn _DYNAMIC[] __attribute__((visibility("hidden")));
extern const char __ehdr_start[] __attribute__((visibility("hidden")));

/* -fstack-protector-strong checks this against each protected frame's copy */
unsigned long __stack_chk_guard = 0x5e1f2a7b9c3d4e00ul;

NORETURN void __stack_chk_fail(void);
NORETURN void __stack_chk_fail(void)
{
	static const char msg[] = "fatal: stack smashing detected\n";
	syscall3(SYS_write, 2, (long)msg, sizeof msg - 1);
	sys_kill((int)sys_getpid(), SIGABRT);
	for (;;)
		syscall1(SYS_exit_group, 134);
}

/* No calls and no pointers from data in here: nothing is relocated yet. */
__attribute__((no_stack_protector)) static void relocate(void)
{
	u64 base = (u64)__ehdr_start;
	const ElfRela *rela = NULL;
	u64 relasz = 0, relr = 0, relrsz = 0;
	for (const ElfDyn *d = _DYNAMIC; d->tag != DT_NULL; d++) {
		if (d->tag == DT_RELA)
			rela = (const ElfRela *)(base + d->val);
		else if (d->tag == DT_RELASZ)
			relasz = d->val;
		else if (d->tag == DT_RELR)
			relr = base + d->val;
		else if (d->tag == DT_RELRSZ)
			relrsz = d->val;
	}
	for (u64 i = 0; rela && i < relasz / sizeof(ElfRela); i++)
		if ((u32)rela[i].info == R_X86_64_RELATIVE)
			*(u64 *)(base + rela[i].offset) = base + (u64)rela[i].addend;
	/* packed relative relocations: an address, then bitmaps of the words after it */
	u64 *where = NULL;
	for (u64 i = 0; i < relrsz / 8; i++) {
		u64 e = ((const u64 *)relr)[i];
		if (!(e & 1)) {
			where = (u64 *)(base + e);
			*where++ += base;
		} else {
			for (u64 b = 0; (e >>= 1) != 0; b++)
				if (e & 1)
					where[b] += base;
			where += 63;
		}
	}
}

__attribute__((no_stack_protector)) void zr_start(long *sp);
__attribute__((no_stack_protector)) void zr_start(long *sp)
{
	relocate();
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	environ = argv + argc + 1;
	char **e = environ;
	while (*e)
		e++;
	const u64 *aux = (const u64 *)(e + 1);
	const ElfPhdr *phdr = NULL;
	u64 phnum = 0;
	for (; aux[0] != AT_NULL; aux += 2) {
		if (aux[0] == AT_RANDOM) {
			u64 r;
			memcpy(&r, (const void *)aux[1], sizeof r);
			/* a zero low byte stops string functions reading past it */
			__stack_chk_guard = r & ~0xfful;
		} else if (aux[0] == AT_PHDR) {
			phdr = (const ElfPhdr *)aux[1];
		} else if (aux[0] == AT_PHNUM) {
			phnum = aux[1];
		}
	}
	/* relocated pointers and constant tables can't be changed from now on */
	for (u64 i = 0; phdr && i < phnum; i++)
		if (phdr[i].type == PT_GNU_RELRO) {
			u64 start = ((u64)__ehdr_start + phdr[i].vaddr) & ~4095ul;
			u64 end = ((u64)__ehdr_start + phdr[i].vaddr + phdr[i].memsz) & ~4095ul;
			if (end > start)
				sys_mprotect((void *)start, end - start, PROT_READ);
		}
	const char *poison = env_get("ZR_MEM_POISON");
	mem_poison = poison && str_eq(poison, "1");
	if (argc > 0 && argv[0]) {
		const char *slash = str_rchr(argv[0], '/');
		program_name = slash ? slash + 1 : argv[0];
	}
	exit(main(argc, argv));
}

NORETURN void exit(int code)
{
	for (;;)
		syscall1(SYS_exit_group, code);
}

NORETURN void die(const char *f, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, f);
	fmt_v(msg, sizeof msg, f, ap);
	va_end(ap);
	fd_print(2, "%s: fatal: %s\n", program_name, msg);
	exit(1);
}

const char *err_str(long err)
{
	if (err < 0)
		err = -err;
	switch (err) {
	case 0: return "success";
	case EPERM: return "operation not permitted";
	case ENOENT: return "no such file or directory";
	case ESRCH: return "no such process";
	case EINTR: return "interrupted";
	case EIO: return "input/output error";
	case ENXIO: return "no such device or address";
	case E2BIG: return "argument list too long";
	case ENOEXEC: return "not an executable";
	case EBADF: return "bad file descriptor";
	case ECHILD: return "no child processes";
	case EAGAIN: return "try again";
	case ENOMEM: return "out of memory";
	case EACCES: return "permission denied";
	case EFAULT: return "bad address";
	case EBUSY: return "device or resource busy";
	case EEXIST: return "already exists";
	case EXDEV: return "cross-device link";
	case ENODEV: return "no such device";
	case ENOTDIR: return "not a directory";
	case EISDIR: return "is a directory";
	case EINVAL: return "invalid argument";
	case EMFILE: return "too many open files";
	case ENOTTY: return "not a terminal";
	case EFBIG: return "file too large";
	case ENOSPC: return "no space left on device";
	case ESPIPE: return "illegal seek";
	case EROFS: return "read-only file system";
	case EPIPE: return "broken pipe";
	case ERANGE: return "result out of range";
	case ENAMETOOLONG: return "name too long";
	case ENOSYS: return "not implemented";
	case ENOTEMPTY: return "directory not empty";
	case ELOOP: return "too many symbolic links";
	case EADDRINUSE: return "address in use";
	case ENETDOWN: return "network is down";
	case ENETUNREACH: return "network unreachable";
	case ECONNRESET: return "connection reset";
	case ENOTCONN: return "not connected";
	case ETIMEDOUT: return "timed out";
	case ECONNREFUSED: return "connection refused";
	case EHOSTUNREACH: return "host unreachable";
	case EINPROGRESS: return "in progress";
	default: return "unknown error";
	}
}
