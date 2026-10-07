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

void zr_start(long *sp);
void zr_start(long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	environ = argv + argc + 1;
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
