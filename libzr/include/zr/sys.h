/*
 * Kernel ABI: constants, structures and thin syscall wrappers.
 * Layouts follow the Linux x86-64 user-space ABI.
 */
#pragma once

#include "base.h"
#include "syscalls.h"

/* ---- Files ----------------------------------------------------------------- */

enum {
	O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2,
	O_CREAT = 0100, O_EXCL = 0200, O_NOCTTY = 0400, O_TRUNC = 01000,
	O_APPEND = 02000, O_NONBLOCK = 04000, O_DIRECTORY = 0200000,
	O_NOFOLLOW = 0400000, O_CLOEXEC = 02000000, O_PATH = 010000000,
};

#define AT_FDCWD (-100)
enum { AT_SYMLINK_NOFOLLOW = 0x100, AT_REMOVEDIR = 0x200, AT_EMPTY_PATH = 0x1000 };
enum { SEEK_SET = 0, SEEK_CUR = 1, SEEK_END = 2 };
enum { F_DUPFD = 0, F_GETFD = 1, F_SETFD = 2, F_GETFL = 3, F_SETFL = 4, FD_CLOEXEC = 1, F_DUPFD_CLOEXEC = 1030, F_ADD_SEALS = 1033 };
enum { F_SEAL_SEAL = 1, F_SEAL_SHRINK = 2, F_SEAL_GROW = 4, F_SEAL_WRITE = 8 };
enum { R_OK = 4, W_OK = 2, X_OK = 1, F_OK = 0 };

enum {
	S_IFMT = 0170000, S_IFSOCK = 0140000, S_IFLNK = 0120000, S_IFREG = 0100000,
	S_IFBLK = 0060000, S_IFDIR = 0040000, S_IFCHR = 0020000, S_IFIFO = 0010000,
};
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)

struct stat {
	u64 st_dev;
	u64 st_ino;
	u64 st_nlink;
	u32 st_mode;
	u32 st_uid;
	u32 st_gid;
	u32 __pad0;
	u64 st_rdev;
	i64 st_size;
	i64 st_blksize;
	i64 st_blocks;
	i64 st_atime;
	i64 st_atime_nsec;
	i64 st_mtime;
	i64 st_mtime_nsec;
	i64 st_ctime;
	i64 st_ctime_nsec;
	i64 __unused[3];
};

struct linux_dirent64 {
	u64 d_ino;
	i64 d_off;
	u16 d_reclen;
	u8 d_type;
	char d_name[];
};
enum { DT_UNKNOWN = 0, DT_FIFO = 1, DT_CHR = 2, DT_DIR = 4, DT_BLK = 6, DT_REG = 8, DT_LNK = 10, DT_SOCK = 12 };

struct statfs {
	i64 f_type;
	i64 f_bsize;
	u64 f_blocks;
	u64 f_bfree;
	u64 f_bavail;
	u64 f_files;
	u64 f_ffree;
	i32 f_fsid[2];
	i64 f_namelen;
	i64 f_frsize;
	i64 f_flags;
	i64 f_spare[4];
};

struct iovec {
	void *iov_base;
	usize iov_len;
};

/* ---- Memory ---------------------------------------------------------------- */

enum { PROT_NONE = 0, PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4 };
enum {
	MAP_SHARED = 0x01, MAP_PRIVATE = 0x02, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20,
	MAP_NORESERVE = 0x4000, MAP_POPULATE = 0x8000, MAP_STACK = 0x20000,
};
#define MAP_FAILED ((void *)-1)
enum { MREMAP_MAYMOVE = 1 };
enum { MADV_DONTNEED = 4, MADV_FREE = 8 };
enum { MFD_CLOEXEC = 1, MFD_ALLOW_SEALING = 2 };

/* ---- Time ------------------------------------------------------------------ */

enum { UTIME_NOW = (1 << 30) - 1, UTIME_OMIT = (1 << 30) - 2 };
struct timespec {
	i64 tv_sec;
	i64 tv_nsec;
};
struct itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};
enum { CLOCK_REALTIME = 0, CLOCK_MONOTONIC = 1, CLOCK_BOOTTIME = 7 };
enum { TFD_NONBLOCK = O_NONBLOCK, TFD_CLOEXEC = O_CLOEXEC, TFD_TIMER_ABSTIME = 1 };

/* ---- Polling and events ---------------------------------------------------- */

struct pollfd {
	i32 fd;
	i16 events;
	i16 revents;
};
enum { POLLIN = 0x001, POLLPRI = 0x002, POLLOUT = 0x004, POLLERR = 0x008, POLLHUP = 0x010, POLLNVAL = 0x020 };

struct epoll_event {
	u32 events;
	u64 data;
} __attribute__((packed));
enum { EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3 };
enum { EPOLLIN = 0x001, EPOLLOUT = 0x004, EPOLLERR = 0x008, EPOLLHUP = 0x010, EPOLLRDHUP = 0x2000, EPOLLET = 1u << 31 };
enum { EPOLL_CLOEXEC = O_CLOEXEC, EFD_CLOEXEC = O_CLOEXEC, EFD_NONBLOCK = O_NONBLOCK };

struct inotify_event {
	i32 wd;
	u32 mask;
	u32 cookie;
	u32 len;
	char name[];
};
enum {
	IN_MODIFY = 0x002, IN_ATTRIB = 0x004, IN_CLOSE_WRITE = 0x008, IN_MOVED_FROM = 0x040,
	IN_MOVED_TO = 0x080, IN_CREATE = 0x100, IN_DELETE = 0x200, IN_DELETE_SELF = 0x400,
	IN_NONBLOCK = O_NONBLOCK, IN_CLOEXEC = O_CLOEXEC,
};

/* ---- Signals --------------------------------------------------------------- */

enum {
	SIGHUP = 1, SIGINT = 2, SIGQUIT = 3, SIGILL = 4, SIGTRAP = 5, SIGABRT = 6, SIGBUS = 7,
	SIGFPE = 8, SIGKILL = 9, SIGUSR1 = 10, SIGSEGV = 11, SIGUSR2 = 12, SIGPIPE = 13,
	SIGALRM = 14, SIGTERM = 15, SIGCHLD = 17, SIGCONT = 18, SIGSTOP = 19, SIGTSTP = 20,
	SIGTTIN = 21, SIGTTOU = 22, SIGWINCH = 28, SIGPWR = 30,
};
enum { SA_NOCLDSTOP = 1, SA_SIGINFO = 4, SA_RESTORER = 0x04000000, SA_ONSTACK = 0x08000000, SA_RESTART = 0x10000000 };
enum { SIG_BLOCK = 0, SIG_UNBLOCK = 1, SIG_SETMASK = 2 };
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

struct kernel_sigaction {
	void (*handler)(int);
	u64 flags;
	void (*restorer)(void);
	u64 mask;
};

struct signalfd_siginfo {
	u32 ssi_signo;
	i32 ssi_errno;
	i32 ssi_code;
	u32 ssi_pid;
	u32 ssi_uid;
	i32 ssi_fd;
	u32 ssi_tid;
	u32 ssi_band;
	u32 ssi_overrun;
	u32 ssi_trapno;
	i32 ssi_status;
	i32 ssi_int;
	u64 ssi_ptr;
	u64 ssi_utime;
	u64 ssi_stime;
	u64 ssi_addr;
	u16 ssi_addr_lsb;
	u8 __pad[46];
};

/* ---- Processes ------------------------------------------------------------- */

enum { WNOHANG = 1, WUNTRACED = 2 };
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s) ((s) & 0x7f)

enum {
	CLONE_VM = 0x100, CLONE_FS = 0x200, CLONE_FILES = 0x400, CLONE_SIGHAND = 0x800,
	CLONE_VFORK = 0x4000, CLONE_PARENT = 0x8000, CLONE_THREAD = 0x10000,
	CLONE_NEWNS = 0x20000, CLONE_SYSVSEM = 0x40000, CLONE_SETTLS = 0x80000,
	CLONE_PARENT_SETTID = 0x100000, CLONE_CHILD_CLEARTID = 0x200000,
	CLONE_NEWUTS = 0x4000000, CLONE_NEWIPC = 0x8000000, CLONE_NEWUSER = 0x10000000,
	CLONE_NEWPID = 0x20000000, CLONE_NEWNET = 0x40000000,
};

enum { PR_SET_PDEATHSIG = 1, PR_SET_NAME = 15, PR_SET_NO_NEW_PRIVS = 38, PR_SET_CHILD_SUBREAPER = 36 };
enum { PRIO_PROCESS = 0 };

struct utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

struct sysinfo {
	i64 uptime;
	u64 loads[3];
	u64 totalram;
	u64 freeram;
	u64 sharedram;
	u64 bufferram;
	u64 totalswap;
	u64 freeswap;
	u16 procs;
	u16 pad;
	u64 totalhigh;
	u64 freehigh;
	u32 mem_unit;
	char _f[0];
};

/* reboot(2) */
enum {
	REBOOT_MAGIC1 = (int)0xfee1dead, REBOOT_MAGIC2 = 672274793,
	REBOOT_CMD_RESTART = 0x01234567, REBOOT_CMD_POWER_OFF = 0x4321FEDC,
	REBOOT_CMD_CAD_OFF = 0,
};

/* mount(2) */
enum {
	MS_RDONLY = 1, MS_NOSUID = 2, MS_NODEV = 4, MS_NOEXEC = 8, MS_REMOUNT = 32,
	MS_NOATIME = 1024, MS_BIND = 4096, MS_MOVE = 8192, MS_REC = 16384,
	MS_PRIVATE = 1 << 18, MS_SLAVE = 1 << 19, MS_SHARED = 1 << 20, MS_RELATIME = 1 << 21,
};
enum { MNT_DETACH = 2 };

/* ---- Futex ------------------------------------------------------------------ */

enum { FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_PRIVATE_FLAG = 128 };

/* ---- Sockets ---------------------------------------------------------------- */

enum { AF_UNIX = 1, AF_INET = 2, AF_INET6 = 10, AF_NETLINK = 16, AF_PACKET = 17 };
enum {
	SOCK_STREAM = 1, SOCK_DGRAM = 2, SOCK_RAW = 3, SOCK_SEQPACKET = 5,
	SOCK_NONBLOCK = O_NONBLOCK, SOCK_CLOEXEC = O_CLOEXEC,
};
enum { SOL_SOCKET = 1, IPPROTO_IP = 0, IPPROTO_TCP = 6, IPPROTO_UDP = 17, IPPROTO_IPV6 = 41 };
enum {
	SO_REUSEADDR = 2, SO_ERROR = 4, SO_BROADCAST = 6, SO_SNDBUF = 7, SO_RCVBUF = 8,
	SO_KEEPALIVE = 9, SO_PASSCRED = 16, SO_PEERCRED = 17, SO_RCVTIMEO = 20, SO_SNDTIMEO = 21,
	SO_BINDTODEVICE = 25,
};
enum { TCP_NODELAY = 1 };
enum { MSG_PEEK = 2, MSG_DONTWAIT = 0x40, MSG_NOSIGNAL = 0x4000, MSG_CMSG_CLOEXEC = 0x40000000 };
enum { SHUT_RD = 0, SHUT_WR = 1, SHUT_RDWR = 2 };
enum { SCM_RIGHTS = 1, SCM_CREDENTIALS = 2 };

struct sockaddr {
	u16 sa_family;
	char sa_data[14];
};
struct sockaddr_un {
	u16 sun_family;
	char sun_path[108];
};
struct in_addr {
	u32 s_addr; /* network byte order */
};
struct sockaddr_in {
	u16 sin_family;
	u16 sin_port; /* network byte order */
	struct in_addr sin_addr;
	u8 sin_zero[8];
};
struct sockaddr_in6 {
	u16 sin6_family;
	u16 sin6_port;
	u32 sin6_flowinfo;
	u8 sin6_addr[16];
	u32 sin6_scope_id;
};
struct msghdr {
	void *msg_name;
	u32 msg_namelen;
	struct iovec *msg_iov;
	usize msg_iovlen;
	void *msg_control;
	usize msg_controllen;
	i32 msg_flags;
};
struct cmsghdr {
	usize cmsg_len;
	i32 cmsg_level;
	i32 cmsg_type;
};
struct ucred {
	i32 pid;
	u32 uid;
	u32 gid;
};

#define CMSG_ALIGN(len) (((len) + sizeof(usize) - 1) & ~(sizeof(usize) - 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(len) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_DATA(c) ((unsigned char *)(c) + CMSG_ALIGN(sizeof(struct cmsghdr)))

static inline u16 htons(u16 v) { return __builtin_bswap16(v); }
static inline u16 ntohs(u16 v) { return __builtin_bswap16(v); }
static inline u32 htonl(u32 v) { return __builtin_bswap32(v); }
static inline u32 ntohl(u32 v) { return __builtin_bswap32(v); }

/* ---- ioctl encoding --------------------------------------------------------- */

#define IOC_NONE 0u
#define IOC_WRITE 1u
#define IOC_READ 2u
#define IOC(dir, type, nr, size) (((dir) << 30) | ((u32)(size) << 16) | ((u32)(type) << 8) | (u32)(nr))
#define IO(type, nr) IOC(IOC_NONE, type, nr, 0)
#define IOR(type, nr, T) IOC(IOC_READ, type, nr, sizeof(T))
#define IOW(type, nr, T) IOC(IOC_WRITE, type, nr, sizeof(T))
#define IOWR(type, nr, T) IOC(IOC_READ | IOC_WRITE, type, nr, sizeof(T))

/* Terminal ioctls used by the terminal app and the shell. */
enum {
	TCGETS = 0x5401, TCSETS = 0x5402, TIOCSCTTY = 0x540E, TIOCGPGRP = 0x540F,
	TIOCSPGRP = 0x5410, TIOCGWINSZ = 0x5413, TIOCSWINSZ = 0x5414, FIONREAD = 0x541B,
	TIOCGPTN = 0x80045430, TIOCSPTLCK = 0x40045431,
};
struct winsize {
	u16 ws_row;
	u16 ws_col;
	u16 ws_xpixel;
	u16 ws_ypixel;
};
struct termios {
	u32 c_iflag;
	u32 c_oflag;
	u32 c_cflag;
	u32 c_lflag;
	u8 c_line;
	u8 c_cc[19];
};
enum { ICANON = 0000002, ECHO = 0000010, ISIG = 0000001, IEXTEN = 0100000, ECHONL = 0000100 };
enum { ICRNL = 0000400, IXON = 0002000, OPOST = 0000001 };
enum { VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4, VTIME = 5, VMIN = 6, VSUSP = 10 };

/* ---- Syscall wrappers -------------------------------------------------------- */

#define SC(n, ...) syscall##n(__VA_ARGS__)

static inline long sys_read(int fd, void *buf, usize n) { return SC(3, SYS_read, fd, (long)buf, (long)n); }
static inline long sys_write(int fd, const void *buf, usize n) { return SC(3, SYS_write, fd, (long)buf, (long)n); }
static inline long sys_pread(int fd, void *buf, usize n, i64 off) { return SC(4, SYS_pread64, fd, (long)buf, (long)n, off); }
static inline long sys_pwrite(int fd, const void *buf, usize n, i64 off) { return SC(4, SYS_pwrite64, fd, (long)buf, (long)n, off); }
static inline long sys_writev(int fd, const struct iovec *iov, int n) { return SC(3, SYS_writev, fd, (long)iov, n); }
static inline long sys_open(const char *path, int flags, int mode) { return SC(4, SYS_openat, AT_FDCWD, (long)path, flags, mode); }
static inline long sys_openat(int dir, const char *path, int flags, int mode) { return SC(4, SYS_openat, dir, (long)path, flags, mode); }
static inline long sys_close(int fd) { return SC(1, SYS_close, fd); }
static inline long sys_lseek(int fd, i64 off, int whence) { return SC(3, SYS_lseek, fd, off, whence); }
static inline long sys_ioctl(int fd, unsigned long req, void *arg) { return SC(3, SYS_ioctl, fd, (long)req, (long)arg); }
static inline long sys_fcntl(int fd, int cmd, long arg) { return SC(3, SYS_fcntl, fd, cmd, arg); }
static inline long sys_dup3(int old, int new_, int flags) { return SC(3, SYS_dup3, old, new_, flags); }
static inline long sys_pipe2(int fds[2], int flags) { return SC(2, SYS_pipe2, (long)fds, flags); }
static inline long sys_fstat(int fd, struct stat *st) { return SC(2, SYS_fstat, fd, (long)st); }
static inline long sys_stat(const char *path, struct stat *st) { return SC(4, SYS_newfstatat, AT_FDCWD, (long)path, (long)st, 0); }
static inline long sys_lstat(const char *path, struct stat *st) { return SC(4, SYS_newfstatat, AT_FDCWD, (long)path, (long)st, AT_SYMLINK_NOFOLLOW); }
static inline long sys_access(const char *path, int mode) { return SC(4, SYS_faccessat2, AT_FDCWD, (long)path, mode, 0); }
static inline long sys_getdents64(int fd, void *buf, usize n) { return SC(3, SYS_getdents64, fd, (long)buf, (long)n); }
static inline long sys_mkdir(const char *path, int mode) { return SC(3, SYS_mkdirat, AT_FDCWD, (long)path, mode); }
static inline long sys_rmdir(const char *path) { return SC(3, SYS_unlinkat, AT_FDCWD, (long)path, AT_REMOVEDIR); }
static inline long sys_unlink(const char *path) { return SC(3, SYS_unlinkat, AT_FDCWD, (long)path, 0); }
static inline long sys_rename(const char *from, const char *to) { return SC(5, SYS_renameat2, AT_FDCWD, (long)from, AT_FDCWD, (long)to, 0); }
static inline long sys_symlink(const char *target, const char *path) { return SC(3, SYS_symlinkat, (long)target, AT_FDCWD, (long)path); }
static inline long sys_readlink(const char *path, char *buf, usize n) { return SC(4, SYS_readlinkat, AT_FDCWD, (long)path, (long)buf, (long)n); }
static inline long sys_chmod(const char *path, int mode) { return SC(3, SYS_fchmodat, AT_FDCWD, (long)path, mode); }
static inline long sys_chown(const char *path, u32 uid, u32 gid) { return SC(5, SYS_fchownat, AT_FDCWD, (long)path, uid, gid, 0); }
static inline long sys_lchown(const char *path, u32 uid, u32 gid) { return SC(5, SYS_fchownat, AT_FDCWD, (long)path, uid, gid, AT_SYMLINK_NOFOLLOW); }
static inline long sys_ftruncate(int fd, i64 len) { return SC(2, SYS_ftruncate, fd, len); }
static inline long sys_truncate(const char *path, i64 len) { return SC(2, SYS_truncate, (long)path, len); }
static inline long sys_fchmod(int fd, u32 mode) { return SC(2, SYS_fchmod, fd, mode); }
static inline long sys_rename2(const char *from, const char *to, u32 flags) { return SC(5, SYS_renameat2, AT_FDCWD, (long)from, AT_FDCWD, (long)to, flags); }
static inline long sys_fsync(int fd) { return SC(1, SYS_fsync, fd); }
static inline long sys_sync(void) { return SC(0, SYS_sync); }
static inline long sys_chdir(const char *path) { return SC(1, SYS_chdir, (long)path); }
static inline long sys_getcwd(char *buf, usize n) { return SC(2, SYS_getcwd, (long)buf, (long)n); }
static inline long sys_umask(int mask) { return SC(1, SYS_umask, mask); }
static inline long sys_statfs(const char *path, struct statfs *st) { return SC(2, SYS_statfs, (long)path, (long)st); }
static inline long sys_utimensat(int dir, const char *path, const struct timespec t[2], int flags) { return SC(4, SYS_utimensat, dir, (long)path, (long)t, flags); }
static inline long sys_copy_file_range(int in, int out, usize len) { return SC(6, SYS_copy_file_range, in, 0, out, 0, (long)len, 0); }

static inline void *sys_mmap(void *addr, usize len, int prot, int flags, int fd, i64 off)
{
	return (void *)SC(6, SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
}
static inline long sys_munmap(void *addr, usize len) { return SC(2, SYS_munmap, (long)addr, (long)len); }
static inline long sys_mprotect(void *addr, usize len, int prot) { return SC(3, SYS_mprotect, (long)addr, (long)len, prot); }
static inline void *sys_mremap(void *old, usize old_len, usize new_len, int flags) { return (void *)SC(4, SYS_mremap, (long)old, (long)old_len, (long)new_len, flags); }
static inline long sys_madvise(void *addr, usize len, int advice) { return SC(3, SYS_madvise, (long)addr, (long)len, advice); }
static inline long sys_memfd_create(const char *name, unsigned flags) { return SC(2, SYS_memfd_create, (long)name, flags); }

static inline long sys_poll(struct pollfd *fds, usize n, int timeout_ms) { return SC(3, SYS_poll, (long)fds, (long)n, timeout_ms); }
static inline long sys_epoll_create1(int flags) { return SC(1, SYS_epoll_create1, flags); }
static inline long sys_epoll_ctl(int ep, int op, int fd, struct epoll_event *ev) { return SC(4, SYS_epoll_ctl, ep, op, fd, (long)ev); }
static inline long sys_epoll_wait(int ep, struct epoll_event *evs, int n, int timeout_ms) { return SC(4, SYS_epoll_wait, ep, (long)evs, n, timeout_ms); }
static inline long sys_timerfd_create(int clock, int flags) { return SC(2, SYS_timerfd_create, clock, flags); }
static inline long sys_timerfd_settime(int fd, int flags, const struct itimerspec *v) { return SC(4, SYS_timerfd_settime, fd, flags, (long)v, 0); }
static inline long sys_eventfd2(unsigned init, int flags) { return SC(2, SYS_eventfd2, init, flags); }
static inline long sys_signalfd4(int fd, const u64 *mask, int flags) { return SC(4, SYS_signalfd4, fd, (long)mask, 8, flags); }
static inline long sys_inotify_init1(int flags) { return SC(1, SYS_inotify_init1, flags); }
static inline long sys_inotify_add_watch(int fd, const char *path, u32 mask) { return SC(3, SYS_inotify_add_watch, fd, (long)path, mask); }
static inline long sys_inotify_rm_watch(int fd, int wd) { return SC(2, SYS_inotify_rm_watch, fd, wd); }

static inline long sys_clock_gettime(int clock, struct timespec *ts) { return SC(2, SYS_clock_gettime, clock, (long)ts); }
static inline long sys_clock_settime(int clock, const struct timespec *ts) { return SC(2, SYS_clock_settime, clock, (long)ts); }
static inline long sys_nanosleep(const struct timespec *req, struct timespec *rem) { return SC(2, SYS_nanosleep, (long)req, (long)rem); }
static inline long sys_getrandom(void *buf, usize n, unsigned flags) { return SC(3, SYS_getrandom, (long)buf, (long)n, flags); }

static inline long sys_getpid(void) { return SC(0, SYS_getpid); }
static inline long sys_getppid(void) { return SC(0, SYS_getppid); }
static inline long sys_gettid(void) { return SC(0, SYS_gettid); }
static inline long sys_getuid(void) { return SC(0, SYS_getuid); }
static inline long sys_geteuid(void) { return SC(0, SYS_geteuid); }
static inline long sys_getgid(void) { return SC(0, SYS_getgid); }
static inline long sys_setuid(u32 uid) { return SC(1, SYS_setuid, uid); }
static inline long sys_setgid(u32 gid) { return SC(1, SYS_setgid, gid); }
static inline long sys_setresuid(u32 r, u32 e, u32 s) { return SC(3, SYS_setresuid, r, e, s); }
static inline long sys_setresgid(u32 r, u32 e, u32 s) { return SC(3, SYS_setresgid, r, e, s); }
static inline long sys_setgroups(usize n, const u32 *list) { return SC(2, SYS_setgroups, (long)n, (long)list); }
static inline long sys_setsid(void) { return SC(0, SYS_setsid); }
static inline long sys_setpgid(int pid, int pgid) { return SC(2, SYS_setpgid, pid, pgid); }
static inline long sys_kill(int pid, int sig) { return SC(2, SYS_kill, pid, sig); }
static inline long sys_wait4(int pid, int *status, int options, void *rusage) { return SC(4, SYS_wait4, pid, (long)status, options, (long)rusage); }
static inline long sys_execve(const char *path, char *const argv[], char *const envp[]) { return SC(3, SYS_execve, (long)path, (long)argv, (long)envp); }
static inline long sys_prctl(int op, long a, long b, long c, long d) { return SC(5, SYS_prctl, op, a, b, c, d); }
static inline long sys_uname(struct utsname *u) { return SC(1, SYS_uname, (long)u); }
static inline long sys_sysinfo(struct sysinfo *s) { return SC(1, SYS_sysinfo, (long)s); }
static inline long sys_sethostname(const char *name, usize n) { return SC(2, SYS_sethostname, (long)name, (long)n); }
static inline long sys_setpriority(int which, int who, int prio) { return SC(3, SYS_setpriority, which, who, prio); }
static inline long sys_sched_yield(void) { return SC(0, SYS_sched_yield); }
static inline long sys_sched_getaffinity(int pid, usize size, void *mask) { return SC(3, SYS_sched_getaffinity, pid, (long)size, (long)mask); }
static inline long sys_rt_sigaction(int sig, const struct kernel_sigaction *act, struct kernel_sigaction *old) { return SC(4, SYS_rt_sigaction, sig, (long)act, (long)old, 8); }
static inline long sys_rt_sigprocmask(int how, const u64 *set, u64 *old) { return SC(4, SYS_rt_sigprocmask, how, (long)set, (long)old, 8); }
static inline long sys_futex(int *addr, int op, int val, const struct timespec *timeout) { return SC(4, SYS_futex, (long)addr, op, val, (long)timeout); }
static inline long sys_mount(const char *src, const char *dst, const char *type, unsigned long flags, const void *data) { return SC(5, SYS_mount, (long)src, (long)dst, (long)type, (long)flags, (long)data); }
static inline long sys_umount(const char *path, int flags) { return SC(2, SYS_umount2, (long)path, flags); }
static inline long sys_unshare(int flags) { return SC(1, SYS_unshare, flags); }
static inline long sys_setns(int fd, int nstype) { return SC(2, SYS_setns, fd, nstype); }
static inline long sys_pivot_root(const char *new_root, const char *put_old) { return SC(2, SYS_pivot_root, (long)new_root, (long)put_old); }
static inline long sys_chroot(const char *path) { return SC(1, SYS_chroot, (long)path); }
static inline long sys_reboot(int cmd) { return SC(4, SYS_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, cmd, 0); }

static inline long sys_socket(int domain, int type, int proto) { return SC(3, SYS_socket, domain, type, proto); }
static inline long sys_socketpair(int domain, int type, int proto, int fds[2]) { return SC(4, SYS_socketpair, domain, type, proto, (long)fds); }
static inline long sys_connect(int fd, const void *addr, u32 len) { return SC(3, SYS_connect, fd, (long)addr, len); }
static inline long sys_bind(int fd, const void *addr, u32 len) { return SC(3, SYS_bind, fd, (long)addr, len); }
static inline long sys_listen(int fd, int backlog) { return SC(2, SYS_listen, fd, backlog); }
static inline long sys_accept4(int fd, void *addr, u32 *len, int flags) { return SC(4, SYS_accept4, fd, (long)addr, (long)len, flags); }
static inline long sys_getsockname(int fd, void *addr, u32 *len) { return SC(3, SYS_getsockname, fd, (long)addr, (long)len); }
static inline long sys_sendmsg(int fd, const struct msghdr *m, int flags) { return SC(3, SYS_sendmsg, fd, (long)m, flags); }
static inline long sys_recvmsg(int fd, struct msghdr *m, int flags) { return SC(3, SYS_recvmsg, fd, (long)m, flags); }
static inline long sys_sendto(int fd, const void *buf, usize n, int flags, const void *addr, u32 alen) { return SC(6, SYS_sendto, fd, (long)buf, (long)n, flags, (long)addr, alen); }
static inline long sys_recvfrom(int fd, void *buf, usize n, int flags, void *addr, u32 *alen) { return SC(6, SYS_recvfrom, fd, (long)buf, (long)n, flags, (long)addr, (long)alen); }
static inline long sys_setsockopt(int fd, int level, int name, const void *val, u32 len) { return SC(5, SYS_setsockopt, fd, level, name, (long)val, len); }
static inline long sys_getsockopt(int fd, int level, int name, void *val, u32 *len) { return SC(5, SYS_getsockopt, fd, level, name, (long)val, (long)len); }
static inline long sys_shutdown(int fd, int how) { return SC(2, SYS_shutdown, fd, how); }

#undef SC
