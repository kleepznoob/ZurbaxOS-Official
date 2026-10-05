/* Time, processes, signals, threads, randomness, event loop and local sockets. */
#pragma once

#include "mem.h"
#include "sys.h"

/* ---- Time --------------------------------------------------------------------------- */

u64 time_mono_ns(void);
static inline u64 time_mono_ms(void) { return time_mono_ns() / 1000000u; }
i64 time_unix(void);             /* seconds since 1970, UTC */
i64 time_unix_ms(void);
void sleep_ms(u32 ms);

typedef struct {
	int year;   /* e.g. 2026 */
	int month;  /* 1..12 */
	int day;    /* 1..31 */
	int hour, min, sec;
	int wday;   /* 0 = Sunday */
	int yday;   /* 0..365 */
} Civil;

void civil_from_unix(i64 t, Civil *out);
i64 unix_from_civil(const Civil *c);
int days_in_month(int year, int month);
extern const char *const MONTH_NAMES[12];
extern const char *const WEEKDAY_NAMES[7];

/* Time zones (reads compiled TZif files). Local time = UTC + tz_offset(). */
bool tz_set(const char *zone_name);          /* e.g. "Europe/Bucharest"; loads /system/zoneinfo */
const char *tz_name(void);
i32 tz_offset(i64 utc);                      /* seconds east of UTC at that instant */
void civil_local(i64 utc, Civil *out);

/* ---- Environment and arguments ------------------------------------------------------- */

extern char **environ;
const char *env_get(const char *name);
void env_set(const char *name, const char *value); /* modifies this process's environ */

/* ---- Processes ------------------------------------------------------------------------- */

typedef struct {
	const char *cwd;
	int stdin_fd, stdout_fd, stderr_fd; /* -1 = inherit */
	bool new_session;
	u32 uid, gid;                       /* used when drop_privileges is set */
	bool drop_privileges;
} SpawnOpts;

/* Starts `path` with argv/envp. envp NULL = inherit. Returns pid or -errno. */
int proc_spawn(const char *path, char *const argv[], char *const envp[], const SpawnOpts *opts);
int proc_wait(int pid, int *status, bool nohang);  /* pid, 0 if still running, or -errno */
char *proc_run_capture(char *const argv[], int *status); /* runs and returns stdout */
const char *proc_self_exe(void);
const char *find_program(const char *name);        /* searches PATH, returns static buffer */

/* ---- Signals ----------------------------------------------------------------------------- */

void sig_handle(int sig, void (*fn)(int));
void sig_ignore(int sig);
void sig_default(int sig);
void sig_block(int sig);
void sig_unblock(int sig);

/* ---- Randomness ---------------------------------------------------------------------------- */

void rand_bytes(void *buf, usize n);  /* cryptographic quality (getrandom) */
u32 rand_u32(void);                   /* fast PRNG seeded from getrandom */
u32 rand_range(u32 lo, u32 hi);       /* inclusive */
f32 rand_f32(void);                   /* [0, 1) */

/* ---- Threads ------------------------------------------------------------------------------- */

typedef struct {
	int state;
} Mutex;

int thread_spawn(void (*fn)(void *), void *arg);  /* returns tid or -errno */
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);
void futex_wait(int *addr, int expected, i64 timeout_ms);
void futex_wake(int *addr, int count);
static inline int atomic_load(const int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void atomic_store(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline int atomic_add(int *p, int v) { return __atomic_add_fetch(p, v, __ATOMIC_ACQ_REL); }

/* ---- Event loop ------------------------------------------------------------------------------ */

typedef struct Loop Loop;
typedef void (*LoopFdFn)(Loop *loop, int fd, u32 events, void *data);
typedef void (*LoopTimerFn)(Loop *loop, void *data);

Loop *loop_new(void);
void loop_free(Loop *loop);
bool loop_add_fd(Loop *loop, int fd, u32 events, LoopFdFn fn, void *data);
void loop_mod_fd(Loop *loop, int fd, u32 events);
void loop_del_fd(Loop *loop, int fd);
/* Timers: one-shot unless repeat_ms > 0. Returns an id for loop_cancel. */
u32 loop_timer(Loop *loop, u32 delay_ms, u32 repeat_ms, LoopTimerFn fn, void *data);
void loop_cancel(Loop *loop, u32 timer_id);
/* Runs one iteration, waiting at most timeout_ms (-1 = until something happens). */
void loop_once(Loop *loop, int timeout_ms);
void loop_run(Loop *loop);        /* until loop_quit */
void loop_quit(Loop *loop);
/* Wakes a blocked loop from another thread; the callback runs on the loop's thread. */
void loop_wake(Loop *loop);
void loop_on_wake(Loop *loop, LoopTimerFn fn, void *data);

/* ---- Local (unix) sockets ------------------------------------------------------------------- */

int unix_listen(const char *path, int type);      /* type: SOCK_STREAM / SOCK_SEQPACKET */
int unix_connect(const char *path, int type);
/* Send/receive a message with up to `max_fds` file descriptors attached. */
long unix_send(int sock, const void *data, usize len, const int *fds, int nfds);
long unix_recv(int sock, void *data, usize cap, int *fds, int max_fds, int *nfds);
bool fd_set_nonblock(int fd, bool on);
bool fd_set_cloexec(int fd, bool on);
bool write_all(int fd, const void *data, usize len);
