/* Zurbax's libsystemd (sd-login.c): what programs include to call it. */
#pragma once
#include <stdarg.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sd_login_monitor sd_login_monitor;

int sd_pid_get_session(pid_t pid, char **session);
int sd_pid_get_owner_uid(pid_t pid, uid_t *uid);
int sd_pid_get_unit(pid_t pid, char **unit);
int sd_pid_get_user_unit(pid_t pid, char **unit);
int sd_pid_get_slice(pid_t pid, char **slice);
int sd_pid_get_user_slice(pid_t pid, char **slice);
int sd_pid_get_machine_name(pid_t pid, char **machine);
int sd_pid_get_cgroup(pid_t pid, char **cgroup);
int sd_pidfd_get_session(int pidfd, char **session);
int sd_pidfd_get_owner_uid(int pidfd, uid_t *uid);
int sd_pidfd_get_user_unit(int pidfd, char **unit);
int sd_peer_get_session(int fd, char **session);
int sd_peer_get_owner_uid(int fd, uid_t *uid);
int sd_session_is_active(const char *session);
int sd_session_is_remote(const char *session);
int sd_session_get_state(const char *session, char **state);
int sd_session_get_uid(const char *session, uid_t *uid);
int sd_session_get_username(const char *session, char **username);
int sd_session_get_seat(const char *session, char **seat);
int sd_session_get_service(const char *session, char **service);
int sd_session_get_type(const char *session, char **type);
int sd_session_get_class(const char *session, char **clazz);
int sd_session_get_desktop(const char *session, char **desktop);
int sd_session_get_display(const char *session, char **display);
int sd_session_get_remote_user(const char *session, char **remote_user);
int sd_session_get_remote_host(const char *session, char **remote_host);
int sd_session_get_tty(const char *session, char **display);
int sd_session_get_vt(const char *session, unsigned *vtnr);
int sd_session_get_leader(const char *session, pid_t *leader);
int sd_session_get_start_time(const char *session, unsigned long long *usec);
int sd_session_can_multi_seat(const char *session);
int sd_uid_get_state(uid_t uid, char **state);
int sd_uid_get_display(uid_t uid, char **session);
int sd_uid_is_on_seat(uid_t uid, int require_active, const char *seat);
int sd_uid_get_sessions(uid_t uid, int require_active, char ***sessions);
int sd_uid_get_seats(uid_t uid, int require_active, char ***seats);
int sd_seat_get_active(const char *seat, char **session, uid_t *uid);
int sd_seat_get_sessions(const char *seat, char ***sessions, uid_t **uid, unsigned *n_uids);
int sd_seat_can_multi_session(const char *seat);
int sd_seat_can_tty(const char *seat);
int sd_seat_can_graphical(const char *seat);
int sd_get_seats(char ***seats);
int sd_get_sessions(char ***sessions);
int sd_get_uids(uid_t **users);
int sd_get_machine_names(char ***machines);
int sd_login_monitor_new(const char *category, sd_login_monitor **ret);
sd_login_monitor *sd_login_monitor_unref(sd_login_monitor *m);
int sd_login_monitor_flush(sd_login_monitor *m);
int sd_login_monitor_get_fd(sd_login_monitor *m);
int sd_login_monitor_get_events(sd_login_monitor *m);
int sd_login_monitor_get_timeout(sd_login_monitor *m, unsigned long long *timeout_usec);

#ifdef __cplusplus
}
#endif
