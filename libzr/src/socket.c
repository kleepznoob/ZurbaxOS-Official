/* Unix domain sockets with file-descriptor passing. */
#include "zr.h"

static bool make_addr(struct sockaddr_un *a, const char *path)
{
	memset(a, 0, sizeof *a);
	a->sun_family = AF_UNIX;
	return str_copy(a->sun_path, path, sizeof a->sun_path) < sizeof a->sun_path;
}

int unix_listen(const char *path, int type)
{
	struct sockaddr_un a;
	if (!make_addr(&a, path))
		return -ENAMETOOLONG;
	int fd = (int)sys_socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return fd;
	sys_unlink(path);
	long r = sys_bind(fd, &a, sizeof a);
	if (r == 0)
		r = sys_listen(fd, 64);
	if (r < 0) {
		sys_close(fd);
		return (int)r;
	}
	return fd;
}

int unix_connect(const char *path, int type)
{
	struct sockaddr_un a;
	if (!make_addr(&a, path))
		return -ENAMETOOLONG;
	int fd = (int)sys_socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return fd;
	long r = sys_connect(fd, &a, sizeof a);
	if (r < 0) {
		sys_close(fd);
		return (int)r;
	}
	return fd;
}

#define MAX_FDS 8

long unix_send(int sock, const void *data, usize len, const int *fds, int nfds)
{
	struct iovec iov = { (void *)data, len };
	struct msghdr m = { 0 };
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	union {
		struct cmsghdr h;
		u8 buf[CMSG_SPACE(sizeof(int) * MAX_FDS)];
	} ctl;
	if (nfds > 0) {
		if (nfds > MAX_FDS)
			return -EINVAL;
		memset(&ctl, 0, sizeof ctl);
		m.msg_control = &ctl;
		m.msg_controllen = CMSG_SPACE(sizeof(int) * (usize)nfds);
		ctl.h.cmsg_len = CMSG_LEN(sizeof(int) * (usize)nfds);
		ctl.h.cmsg_level = SOL_SOCKET;
		ctl.h.cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(&ctl.h), fds, sizeof(int) * (usize)nfds);
	}
	long r;
	do
		r = sys_sendmsg(sock, &m, MSG_NOSIGNAL);
	while (r == -EINTR);
	return r;
}

long unix_recv(int sock, void *data, usize cap, int *fds, int max_fds, int *nfds)
{
	struct iovec iov = { data, cap };
	struct msghdr m = { 0 };
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	union {
		struct cmsghdr h;
		u8 buf[CMSG_SPACE(sizeof(int) * MAX_FDS)];
	} ctl;
	m.msg_control = &ctl;
	m.msg_controllen = sizeof ctl;
	long r;
	do
		r = sys_recvmsg(sock, &m, MSG_CMSG_CLOEXEC);
	while (r == -EINTR);
	int got = 0;
	if (r >= 0 && m.msg_controllen >= sizeof(struct cmsghdr) && ctl.h.cmsg_level == SOL_SOCKET &&
	    ctl.h.cmsg_type == SCM_RIGHTS) {
		int n = (int)((ctl.h.cmsg_len - CMSG_LEN(0)) / sizeof(int));
		int *in = (int *)CMSG_DATA(&ctl.h);
		for (int i = 0; i < n; i++) {
			if (fds && got < max_fds)
				fds[got++] = in[i];
			else
				sys_close(in[i]); /* more than the caller wanted: don't leak */
		}
	}
	if (nfds)
		*nfds = got;
	return r;
}

bool fd_set_nonblock(int fd, bool on)
{
	long fl = sys_fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return false;
	fl = on ? fl | O_NONBLOCK : fl & ~(long)O_NONBLOCK;
	return sys_fcntl(fd, F_SETFL, fl) == 0;
}

bool fd_set_cloexec(int fd, bool on)
{
	return sys_fcntl(fd, F_SETFD, on ? FD_CLOEXEC : 0) == 0;
}
