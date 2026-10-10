/*
 * Zurbax's libsystemd: the parts of systemd's library GNOME calls, for a
 * system without systemd.
 *
 * GNOME asks libsystemd which login session it runs in, on which seat, of
 * what type, and whether it's the active one; Mutter won't even build
 * without an answer. On Zurbax a GNOME session is started by Zurbax for the
 * person logged in, and that start sets the session's details in the
 * environment (XDG_SESSION_ID, XDG_SEAT, XDG_SESSION_TYPE,
 * XDG_SESSION_CLASS, XDG_VTNR). This library answers from those. The rest
 * (the journal, service notifications, socket activation) is either written
 * to standard error or politely declined.
 *
 * Built against the layer's glibc (layer/gnome.sh), unlike the rest of
 * Zurbax.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

static const char *env_or(const char *name, const char *fallback)
{
	const char *v = getenv(name);
	return v && *v ? v : fallback;
}

static int dup_out(const char *v, char **out)
{
	if (!out)
		return 0;
	*out = strdup(v);
	return *out ? 0 : -ENOMEM;
}

/* Only the session Zurbax started exists: its id, for anyone asking. */
static const char *session_id(void)
{
	return env_or("XDG_SESSION_ID", "1");
}

static int is_ours(const char *session)
{
	return !session || strcmp(session, session_id()) == 0;
}

/* ---- sd-login ---------------------------------------------------------------------- */

EXPORT int sd_pid_get_session(pid_t pid, char **session)
{
	(void)pid;
	return dup_out(session_id(), session);
}

EXPORT int sd_pid_get_owner_uid(pid_t pid, uid_t *uid)
{
	(void)pid;
	if (uid)
		*uid = getuid();
	return 0;
}

EXPORT int sd_pid_get_unit(pid_t pid, char **unit)
{
	(void)pid, (void)unit;
	return -ENODATA;
}

EXPORT int sd_pid_get_user_unit(pid_t pid, char **unit)
{
	(void)pid, (void)unit;
	return -ENODATA;
}

EXPORT int sd_pid_get_slice(pid_t pid, char **slice)
{
	(void)pid, (void)slice;
	return -ENODATA;
}

EXPORT int sd_pid_get_user_slice(pid_t pid, char **slice)
{
	(void)pid, (void)slice;
	return -ENODATA;
}

EXPORT int sd_pid_get_machine_name(pid_t pid, char **name)
{
	(void)pid, (void)name;
	return -ENODATA;
}

EXPORT int sd_pid_get_cgroup(pid_t pid, char **cgroup)
{
	(void)pid, (void)cgroup;
	return -ENODATA;
}

EXPORT int sd_pidfd_get_session(int pidfd, char **session)
{
	(void)pidfd;
	return dup_out(session_id(), session);
}

EXPORT int sd_pidfd_get_owner_uid(int pidfd, uid_t *uid)
{
	return sd_pid_get_owner_uid(0, uid + 0 * pidfd);
}

EXPORT int sd_pidfd_get_user_unit(int pidfd, char **unit)
{
	(void)pidfd, (void)unit;
	return -ENODATA;
}

EXPORT int sd_peer_get_session(int fd, char **session)
{
	(void)fd;
	return dup_out(session_id(), session);
}

EXPORT int sd_peer_get_owner_uid(int fd, uid_t *uid)
{
	(void)fd;
	if (uid)
		*uid = getuid();
	return 0;
}

EXPORT int sd_session_is_active(const char *session)
{
	return is_ours(session) ? 1 : -ENXIO;
}

EXPORT int sd_session_is_remote(const char *session)
{
	return is_ours(session) ? 0 : -ENXIO;
}

EXPORT int sd_session_get_state(const char *session, char **state)
{
	return is_ours(session) ? dup_out("active", state) : -ENXIO;
}

EXPORT int sd_session_get_uid(const char *session, uid_t *uid)
{
	if (!is_ours(session))
		return -ENXIO;
	if (uid)
		*uid = getuid();
	return 0;
}

EXPORT int sd_session_get_username(const char *session, char **username)
{
	return is_ours(session) ? dup_out(env_or("USER", "user"), username) : -ENXIO;
}

EXPORT int sd_session_get_seat(const char *session, char **seat)
{
	return is_ours(session) ? dup_out(env_or("XDG_SEAT", "seat0"), seat) : -ENXIO;
}

EXPORT int sd_session_get_service(const char *session, char **service)
{
	return is_ours(session) ? dup_out("zurbax", service) : -ENXIO;
}

EXPORT int sd_session_get_type(const char *session, char **type)
{
	return is_ours(session) ? dup_out(env_or("XDG_SESSION_TYPE", "wayland"), type) : -ENXIO;
}

EXPORT int sd_session_get_class(const char *session, char **clazz)
{
	return is_ours(session) ? dup_out(env_or("XDG_SESSION_CLASS", "user"), clazz) : -ENXIO;
}

EXPORT int sd_session_get_desktop(const char *session, char **desktop)
{
	return is_ours(session) ? dup_out(env_or("XDG_CURRENT_DESKTOP", "GNOME"), desktop) : -ENXIO;
}

EXPORT int sd_session_get_display(const char *session, char **display)
{
	(void)display;
	return is_ours(session) ? -ENODATA : -ENXIO;
}

EXPORT int sd_session_get_remote_user(const char *session, char **user)
{
	(void)user;
	return is_ours(session) ? -ENODATA : -ENXIO;
}

EXPORT int sd_session_get_remote_host(const char *session, char **host)
{
	(void)host;
	return is_ours(session) ? -ENODATA : -ENXIO;
}

EXPORT int sd_session_get_tty(const char *session, char **tty)
{
	(void)tty;
	return is_ours(session) ? -ENODATA : -ENXIO;
}

EXPORT int sd_session_get_vt(const char *session, unsigned *vt)
{
	if (!is_ours(session))
		return -ENXIO;
	const char *v = getenv("XDG_VTNR");
	if (!v || !*v)
		return -ENODATA;
	if (vt)
		*vt = (unsigned)atoi(v);
	return 0;
}

EXPORT int sd_session_get_leader(const char *session, pid_t *leader)
{
	(void)leader;
	return is_ours(session) ? -ENODATA : -ENXIO;
}

EXPORT int sd_session_get_start_time(const char *session, unsigned long long *usec)
{
	(void)usec;
	return is_ours(session) ? -ENODATA : -ENXIO;
}

EXPORT int sd_session_can_multi_seat(const char *session)
{
	return is_ours(session) ? 0 : -ENXIO;
}

EXPORT int sd_uid_get_state(uid_t uid, char **state)
{
	return uid == getuid() ? dup_out("active", state) : -ENXIO;
}

EXPORT int sd_uid_get_display(uid_t uid, char **session)
{
	return uid == getuid() ? dup_out(session_id(), session) : -ENXIO;
}

EXPORT int sd_uid_is_on_seat(uid_t uid, int require_active, const char *seat)
{
	(void)require_active;
	return uid == getuid() && (!seat || strcmp(seat, env_or("XDG_SEAT", "seat0")) == 0);
}

/* A NULL-terminated list of one string. */
static int one_list(const char *v, char ***out)
{
	if (!out)
		return 1;
	char **l = calloc(2, sizeof *l);
	if (!l || !(l[0] = strdup(v))) {
		free(l);
		return -ENOMEM;
	}
	*out = l;
	return 1;
}

EXPORT int sd_uid_get_sessions(uid_t uid, int require_active, char ***sessions)
{
	(void)require_active;
	if (uid != getuid()) {
		if (sessions)
			*sessions = NULL;
		return 0;
	}
	return one_list(session_id(), sessions);
}

EXPORT int sd_uid_get_seats(uid_t uid, int require_active, char ***seats)
{
	(void)require_active;
	if (uid != getuid()) {
		if (seats)
			*seats = NULL;
		return 0;
	}
	return one_list(env_or("XDG_SEAT", "seat0"), seats);
}

EXPORT int sd_seat_get_active(const char *seat, char **session, uid_t *uid)
{
	(void)seat;
	if (uid)
		*uid = getuid();
	return dup_out(session_id(), session);
}

EXPORT int sd_seat_get_sessions(const char *seat, char ***sessions, uid_t **uids, unsigned *n_uids)
{
	(void)seat;
	if (uids) {
		*uids = malloc(sizeof **uids);
		if (!*uids)
			return -ENOMEM;
		(*uids)[0] = getuid();
	}
	if (n_uids)
		*n_uids = 1;
	return one_list(session_id(), sessions);
}

EXPORT int sd_seat_can_multi_session(const char *seat)
{
	(void)seat;
	return 0;
}

EXPORT int sd_seat_can_tty(const char *seat)
{
	(void)seat;
	return 0;
}

EXPORT int sd_seat_can_graphical(const char *seat)
{
	(void)seat;
	return 1;
}

EXPORT int sd_get_seats(char ***seats)
{
	return one_list(env_or("XDG_SEAT", "seat0"), seats);
}

EXPORT int sd_get_sessions(char ***sessions)
{
	return one_list(session_id(), sessions);
}

EXPORT int sd_get_uids(uid_t **users)
{
	if (users) {
		*users = malloc(sizeof **users);
		if (!*users)
			return -ENOMEM;
		(*users)[0] = getuid();
	}
	return 1;
}

EXPORT int sd_get_machine_names(char ***machines)
{
	if (machines)
		*machines = NULL;
	return 0;
}

/* Login changes: nothing ever changes, so the descriptor never wakes. */
typedef struct sd_login_monitor sd_login_monitor;

EXPORT int sd_login_monitor_new(const char *category, sd_login_monitor **ret)
{
	(void)category;
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd < 0)
		return -errno;
	*ret = (sd_login_monitor *)(long)(fd + 1);
	return 0;
}

EXPORT sd_login_monitor *sd_login_monitor_unref(sd_login_monitor *m)
{
	if (m)
		close((int)(long)m - 1);
	return NULL;
}

EXPORT int sd_login_monitor_flush(sd_login_monitor *m)
{
	(void)m;
	return 0;
}

EXPORT int sd_login_monitor_get_fd(sd_login_monitor *m)
{
	return m ? (int)(long)m - 1 : -EINVAL;
}

EXPORT int sd_login_monitor_get_events(sd_login_monitor *m)
{
	(void)m;
	return 1; /* POLLIN */
}

EXPORT int sd_login_monitor_get_timeout(sd_login_monitor *m, unsigned long long *timeout_usec)
{
	(void)m;
	if (timeout_usec)
		*timeout_usec = (unsigned long long)-1;
	return 0;
}

/* ---- the journal: standard error ----------------------------------------------------- */

static const char *const LEVELS[] = { "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug" };

EXPORT int sd_journal_printv(int priority, const char *format, va_list ap)
{
	if (priority < LOG_DEBUG || getenv("ZURBAX_DEBUG")) {
		fprintf(stderr, "[%s] ", LEVELS[priority & 7]);
		vfprintf(stderr, format, ap);
		fputc('\n', stderr);
	}
	return 0;
}

EXPORT int sd_journal_print(int priority, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int r = sd_journal_printv(priority, format, ap);
	va_end(ap);
	return r;
}

EXPORT int sd_journal_print_with_location(int priority, const char *file, const char *line, const char *func, const char *format, ...)
{
	(void)file, (void)line, (void)func;
	va_list ap;
	va_start(ap, format);
	int r = sd_journal_printv(priority, format, ap);
	va_end(ap);
	return r;
}

/* Fields as "KEY=value": the message, with its priority. */
EXPORT int sd_journal_sendv(const struct iovec *iov, int n)
{
	int priority = LOG_INFO;
	const char *msg = NULL;
	int msg_len = 0;
	for (int i = 0; i < n; i++) {
		const char *f = iov[i].iov_base;
		if (iov[i].iov_len > 9 && strncmp(f, "PRIORITY=", 9) == 0)
			priority = f[9] - '0';
		else if (iov[i].iov_len > 8 && strncmp(f, "MESSAGE=", 8) == 0)
			msg = f + 8, msg_len = (int)iov[i].iov_len - 8;
	}
	if (msg && (priority < LOG_DEBUG || getenv("ZURBAX_DEBUG")))
		fprintf(stderr, "[%s] %.*s\n", LEVELS[priority & 7], msg_len, msg);
	return 0;
}

EXPORT int sd_journal_send(const char *format, ...)
{
	struct iovec iov[64];
	char *bufs[64];
	int n = 0;
	va_list ap;
	va_start(ap, format);
	for (const char *f = format; f && n < 64; f = va_arg(ap, const char *)) {
		va_list copy;
		va_copy(copy, ap);
		if (vasprintf(&bufs[n], f, copy) < 0)
			bufs[n] = NULL;
		va_end(copy);
		/* skip this field's own printf arguments: count conversions */
		for (const char *p = f; *p; p++)
			if (*p == '%' && p[1] && p[1] != '%') {
				while (*++p && strchr("-+ #0123456789.lhzjt", *p))
					;
				if (*p == 's' || *p == 'p')
					(void)va_arg(ap, void *);
				else if (*p == 'f' || *p == 'g' || *p == 'e')
					(void)va_arg(ap, double);
				else
					(void)va_arg(ap, long);
			} else if (*p == '%') {
				p++;
			}
		if (bufs[n]) {
			iov[n].iov_base = bufs[n];
			iov[n].iov_len = strlen(bufs[n]);
			n++;
		}
	}
	va_end(ap);
	int r = sd_journal_sendv(iov, n);
	for (int i = 0; i < n; i++)
		free(bufs[i]);
	return r;
}

EXPORT int sd_journal_sendv_with_location(const char *file, const char *line, const char *func, const struct iovec *iov, int n)
{
	(void)file, (void)line, (void)func;
	return sd_journal_sendv(iov, n);
}

EXPORT int sd_journal_stream_fd(const char *identifier, int priority, int level_prefix)
{
	(void)identifier, (void)priority, (void)level_prefix;
	return dup(STDERR_FILENO);
}

/* ---- service manager: there is none ------------------------------------------------ */

EXPORT int sd_notify(int unset_environment, const char *state)
{
	(void)state;
	if (unset_environment)
		unsetenv("NOTIFY_SOCKET");
	return 0;
}

EXPORT int sd_notifyf(int unset_environment, const char *format, ...)
{
	(void)format;
	return sd_notify(unset_environment, NULL);
}

EXPORT int sd_pid_notify(pid_t pid, int unset_environment, const char *state)
{
	(void)pid;
	return sd_notify(unset_environment, state);
}

EXPORT int sd_booted(void)
{
	return 0;
}

EXPORT int sd_listen_fds(int unset_environment)
{
	if (unset_environment) {
		unsetenv("LISTEN_PID");
		unsetenv("LISTEN_FDS");
		unsetenv("LISTEN_FDNAMES");
	}
	return 0;
}

EXPORT int sd_listen_fds_with_names(int unset_environment, char ***names)
{
	if (names)
		*names = NULL;
	return sd_listen_fds(unset_environment);
}

EXPORT int sd_watchdog_enabled(int unset_environment, unsigned long long *usec)
{
	(void)unset_environment, (void)usec;
	return 0;
}
