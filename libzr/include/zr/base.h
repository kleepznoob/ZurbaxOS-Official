/*
 * libzr — the Zurbax runtime.
 *
 * Zurbax does not use a C library. Everything a program needs from the
 * operating system goes through this runtime, which talks to the Linux
 * kernel directly with the `syscall` instruction.
 *
 * Conventions:
 *   - Calls that wrap a syscall return the kernel's result: >= 0 on success,
 *     a negative errno value (e.g. -ENOENT) on failure. There is no global errno.
 *   - Allocation failure is fatal (the process prints a message and exits);
 *     callers never need to check for NULL from the allocator.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;
typedef size_t usize;
typedef ptrdiff_t isize;

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define ABS(x) ((x) < 0 ? -(x) : (x))
#define UNUSED(x) ((void)(x))

/* The system's name and release, as people see it. (Inside, programs,
 * paths and settings keep the "zurbax" name it was started under.) */
#define OS_NAME "Zurbax"
#define OS_VERSION "2.1"
#define OS_PRETTY_NAME OS_NAME " " OS_VERSION
/* Each release is long-term supported: security updates every two weeks
 * until OS_SUPPORT_END, each one numbered (2.1.1, 2.1.2, ...). The update
 * builds set OS_UPDATE. */
#ifndef OS_UPDATE
#define OS_UPDATE 0
#endif
#define OS_STR_(x) #x
#define OS_STR(x) OS_STR_(x)
#define OS_FULL_VERSION OS_VERSION "." OS_STR(OS_UPDATE)
#define OS_SUPPORT_END "2028-10-31"
#define OS_UPDATE_CHANNEL "https://www.zurbax.com/updates/" OS_VERSION "/"
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define NORETURN __attribute__((noreturn))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define CONTAINER_OF(p, T, m) ((T *)((char *)(p) - offsetof(T, m)))

/* ---- Raw system calls ----------------------------------------------------- */

static inline long syscall0(long n)
{
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
	return r;
}

static inline long syscall1(long n, long a)
{
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
	return r;
}

static inline long syscall2(long n, long a, long b)
{
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
	return r;
}

static inline long syscall3(long n, long a, long b, long c)
{
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
	return r;
}

static inline long syscall4(long n, long a, long b, long c, long d)
{
	long r;
	register long r10 __asm__("r10") = d;
	__asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
	return r;
}

static inline long syscall5(long n, long a, long b, long c, long d, long e)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
			 : "rcx", "r11", "memory");
	return r;
}

static inline long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	register long r9 __asm__("r9") = f;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return r;
}

/* ---- Error numbers -------------------------------------------------------- */

enum {
	EPERM = 1, ENOENT = 2, ESRCH = 3, EINTR = 4, EIO = 5, ENXIO = 6, E2BIG = 7,
	ENOEXEC = 8, EBADF = 9, ECHILD = 10, EAGAIN = 11, ENOMEM = 12, EACCES = 13,
	EFAULT = 14, EBUSY = 16, EEXIST = 17, EXDEV = 18, ENODEV = 19, ENOTDIR = 20,
	EISDIR = 21, EINVAL = 22, ENFILE = 23, EMFILE = 24, ENOTTY = 25, EFBIG = 27,
	ENOSPC = 28, ESPIPE = 29, EROFS = 30, EMLINK = 31, EPIPE = 32, ERANGE = 34,
	ENAMETOOLONG = 36, ENOSYS = 38, ENOTEMPTY = 39, ELOOP = 40, ENODATA = 61,
	EPROTO = 71, EOVERFLOW = 75, ENOTSOCK = 88, EMSGSIZE = 90, EOPNOTSUPP = 95,
	EADDRINUSE = 98, EADDRNOTAVAIL = 99, ENETDOWN = 100, ENETUNREACH = 101,
	ECONNABORTED = 103, ECONNRESET = 104, ENOBUFS = 105, EISCONN = 106,
	ENOTCONN = 107, ETIMEDOUT = 110, ECONNREFUSED = 111, EHOSTUNREACH = 113,
	EALREADY = 114, EINPROGRESS = 115,
};

const char *err_str(long err);

/* ---- Process exit / fatal errors ------------------------------------------ */

NORETURN void exit(int code);
NORETURN void die(const char *fmt, ...) PRINTF_LIKE(1, 2);
