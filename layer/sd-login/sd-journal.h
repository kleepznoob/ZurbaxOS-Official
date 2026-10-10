/* Zurbax's libsystemd (sd-login.c): the journal, which goes to standard error. */
#pragma once
#include <stdarg.h>
#include <sys/uio.h>
#include <syslog.h>

#ifdef __cplusplus
extern "C" {
#endif

int sd_journal_print(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
int sd_journal_printv(int priority, const char *format, va_list ap);
int sd_journal_send(const char *format, ...);
int sd_journal_sendv(const struct iovec *iov, int n);
int sd_journal_print_with_location(int priority, const char *file, const char *line, const char *func, const char *format, ...);
int sd_journal_sendv_with_location(const char *file, const char *line, const char *func, const struct iovec *iov, int n);
int sd_journal_stream_fd(const char *identifier, int priority, int level_prefix);

#ifdef __cplusplus
}
#endif
