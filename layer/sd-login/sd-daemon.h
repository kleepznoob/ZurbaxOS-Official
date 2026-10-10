/* Zurbax's libsystemd (sd-login.c): there is no service manager to talk to. */
#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_LISTEN_FDS_START 3

int sd_notify(int unset_environment, const char *state);
int sd_notifyf(int unset_environment, const char *format, ...);
int sd_pid_notify(pid_t pid, int unset_environment, const char *state);
int sd_booted(void);
int sd_listen_fds(int unset_environment);
int sd_listen_fds_with_names(int unset_environment, char ***names);
int sd_watchdog_enabled(int unset_environment, unsigned long long *usec);

#ifdef __cplusplus
}
#endif
