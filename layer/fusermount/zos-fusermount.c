/* fusermount3 for the Linux layer on Zurbax.
 *
 * On other systems fusermount3 is a setuid-root program that mounts FUSE
 * file systems for users. The layer has no root to become: here it asks the
 * user's Linux session (layerrun), which holds the mount rights of the
 * session's namespace, over $XDG_RUNTIME_DIR/zos-linux/fuse.sock. The
 * session checks the mount point is the user's, mounts /dev/fuse there and
 * sends the descriptor back, which this hands to libfuse the usual way
 * (over the socket in $_FUSE_COMMFD).
 *
 *     fusermount3 [-o OPTIONS] -- MOUNTPOINT          mount (libfuse runs this)
 *     fusermount3 --auto-unmount -- MOUNTPOINT        unmount when the caller goes
 *     fusermount3 -u [-q] [-z] [--] MOUNTPOINT        unmount
 *
 * The document portal and AppImages use it; see system/layerrun. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int quiet;

static void die(const char *what)
{
	if (!quiet)
		fprintf(stderr, "fusermount3: %s\n", what);
	exit(1);
}

/* Sends a request to the session; returns its answer, and the descriptor
 * that came with it in *fd (or -1). */
static char *ask(const char *request, int *fd)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt || !*rt)
		die("XDG_RUNTIME_DIR isn't set (run this in the Zurbax Linux session)");
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	if (snprintf(a.sun_path, sizeof a.sun_path, "%s/zos-linux/fuse.sock", rt) >= (int)sizeof a.sun_path)
		die("runtime folder path too long");
	int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) < 0)
		die("cannot reach the Zurbax Linux session");
	if (send(s, request, strlen(request), MSG_NOSIGNAL) < 0)
		die("cannot talk to the Zurbax Linux session");
	static char answer[512];
	struct iovec iov = { answer, sizeof answer - 1 };
	union {
		struct cmsghdr h;
		char buf[CMSG_SPACE(sizeof(int))];
	} ctl;
	struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = &ctl, .msg_controllen = sizeof ctl };
	ssize_t n;
	while ((n = recvmsg(s, &m, MSG_CMSG_CLOEXEC)) < 0 && errno == EINTR)
		;
	close(s);
	if (n <= 0)
		die("no answer from the Zurbax Linux session");
	answer[n] = 0;
	*fd = -1;
	struct cmsghdr *c = CMSG_FIRSTHDR(&m);
	if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
		memcpy(fd, CMSG_DATA(c), sizeof(int));
	return answer;
}

/* Hands /dev/fuse's descriptor to libfuse, as fusermount3 does. */
static void send_fd(int commfd, int fd)
{
	char byte = 0;
	struct iovec iov = { &byte, 1 };
	union {
		struct cmsghdr h;
		char buf[CMSG_SPACE(sizeof(int))];
	} ctl;
	memset(&ctl, 0, sizeof ctl);
	struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = &ctl, .msg_controllen = sizeof ctl };
	struct cmsghdr *c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &fd, sizeof(int));
	while (sendmsg(commfd, &m, MSG_NOSIGNAL) < 0)
		if (errno != EINTR)
			die("cannot hand the file system to its program");
}

static int commfd(void)
{
	const char *s = getenv("_FUSE_COMMFD");
	if (!s || !*s)
		die("this is for libfuse to run ($_FUSE_COMMFD isn't set)");
	return atoi(s);
}

static void unmount(const char *mp, int lazy)
{
	char req[PATH_MAX + 32];
	snprintf(req, sizeof req, "umount\n%s\n%s", mp, lazy ? "lazy" : "");
	int fd;
	const char *a = ask(req, &fd);
	if (strcmp(a, "ok") != 0)
		die(a);
}

/* Waits until the program that mounted the file system goes, then unmounts it. */
static void auto_unmount(int cfd, const char *mp)
{
	for (;;) {
		struct pollfd p = { cfd, POLLIN, 0 };
		if (poll(&p, 1, -1) < 0 && errno == EINTR)
			continue;
		char buf[64];
		ssize_t n = recv(cfd, buf, sizeof buf, 0);
		if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
			break;
	}
	quiet = 1;
	unmount(mp, 1);
}

int main(int argc, char **argv)
{
	const char *opts = "", *mp = NULL;
	int un = 0, lazy = 0, autoun = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--")) {
			if (i + 1 < argc)
				mp = argv[++i];
			break;
		} else if (!strcmp(a, "-o") && i + 1 < argc) {
			opts = argv[++i];
		} else if (!strcmp(a, "-u") || !strcmp(a, "--unmount")) {
			un = 1;
		} else if (!strcmp(a, "-z") || !strcmp(a, "--lazy")) {
			lazy = 1;
		} else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
			quiet = 1;
		} else if (!strcmp(a, "--auto-unmount")) {
			autoun = 1;
		} else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
			puts("fusermount3 version: 3.18 (Zurbax)");
			return 0;
		} else if (a[0] != '-') {
			mp = a;
		}
	}
	if (!mp)
		die("no mount point");
	char real[PATH_MAX];
	if (!realpath(mp, real))
		die("the mount point doesn't exist");
	if (un) {
		unmount(real, lazy);
		return 0;
	}
	int cfd = commfd();
	if (autoun) {
		auto_unmount(cfd, real);
		return 0;
	}
	char req[PATH_MAX + 4200];
	snprintf(req, sizeof req, "mount\n%s\n%s", real, opts);
	int fd;
	const char *a = ask(req, &fd);
	if (strcmp(a, "ok") != 0 || fd < 0)
		die(a);
	send_fd(cfd, fd);
	close(fd);
	/* with auto_unmount the caller expects this to stay until it's gone */
	if (strstr(opts, "auto_unmount"))
		auto_unmount(cfd, real);
	return 0;
}
