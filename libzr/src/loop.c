/* epoll-based event loop with millisecond timers and cross-thread wakeups. */
#include "zr.h"

typedef struct {
	int fd;
	LoopFdFn fn;
	void *data;
} FdWatch;

typedef struct {
	u32 id;
	u64 due_ms;
	u32 repeat_ms;
	LoopTimerFn fn;
	void *data;
} Timer;

struct Loop {
	int ep;
	int wake_fd;
	VEC(FdWatch) fds;
	VEC(Timer) timers;
	u32 next_timer_id;
	bool quit;
	LoopTimerFn wake_fn;
	void *wake_data;
};

static void on_wake(Loop *l, int fd, u32 events, void *data)
{
	UNUSED(events), UNUSED(data);
	u64 v;
	sys_read(fd, &v, sizeof v);
	if (l->wake_fn)
		l->wake_fn(l, l->wake_data);
}

Loop *loop_new(void)
{
	Loop *l = NEW(Loop);
	l->ep = (int)sys_epoll_create1(EPOLL_CLOEXEC);
	if (l->ep < 0)
		die("epoll_create1: %s", err_str(l->ep));
	l->wake_fd = (int)sys_eventfd2(0, EFD_CLOEXEC | EFD_NONBLOCK);
	loop_add_fd(l, l->wake_fd, EPOLLIN, on_wake, NULL);
	return l;
}

void loop_free(Loop *l)
{
	sys_close(l->ep);
	sys_close(l->wake_fd);
	vec_free(&l->fds);
	vec_free(&l->timers);
	mem_free(l);
}

bool loop_add_fd(Loop *l, int fd, u32 events, LoopFdFn fn, void *data)
{
	struct epoll_event ev = { events, (u64)fd };
	if (sys_epoll_ctl(l->ep, EPOLL_CTL_ADD, fd, &ev) < 0)
		return false;
	vec_push(&l->fds, (FdWatch){ fd, fn, data });
	return true;
}

void loop_mod_fd(Loop *l, int fd, u32 events)
{
	struct epoll_event ev = { events, (u64)fd };
	sys_epoll_ctl(l->ep, EPOLL_CTL_MOD, fd, &ev);
}

void loop_del_fd(Loop *l, int fd)
{
	sys_epoll_ctl(l->ep, EPOLL_CTL_DEL, fd, NULL);
	for (usize i = 0; i < l->fds.len; i++) {
		if (l->fds.data[i].fd == fd) {
			vec_remove(&l->fds, i);
			return;
		}
	}
}

u32 loop_timer(Loop *l, u32 delay_ms, u32 repeat_ms, LoopTimerFn fn, void *data)
{
	Timer t = { ++l->next_timer_id, time_mono_ms() + delay_ms, repeat_ms, fn, data };
	if (!t.id)
		t.id = ++l->next_timer_id;
	vec_push(&l->timers, t);
	return t.id;
}

void loop_cancel(Loop *l, u32 id)
{
	for (usize i = 0; i < l->timers.len; i++) {
		if (l->timers.data[i].id == id) {
			vec_remove(&l->timers, i);
			return;
		}
	}
}

static void run_timers(Loop *l)
{
	u64 now = time_mono_ms();
	/* Callbacks may add or cancel timers, so re-scan after each one. */
	for (bool again = true; again;) {
		again = false;
		for (usize i = 0; i < l->timers.len; i++) {
			Timer t = l->timers.data[i];
			if (t.due_ms > now)
				continue;
			if (t.repeat_ms) {
				l->timers.data[i].due_ms = MAX(now, t.due_ms + t.repeat_ms);
			} else {
				vec_remove(&l->timers, i);
			}
			t.fn(l, t.data);
			again = true;
			break;
		}
	}
}

void loop_once(Loop *l, int timeout_ms)
{
	if (l->timers.len) {
		u64 now = time_mono_ms(), next = ~0ull;
		vec_each(&l->timers, t) next = MIN(next, t->due_ms);
		int until = next <= now ? 0 : (int)MIN(next - now, 0x7fffffffu);
		if (timeout_ms < 0 || until < timeout_ms)
			timeout_ms = until;
	}
	struct epoll_event evs[32];
	long n = sys_epoll_wait(l->ep, evs, 32, timeout_ms);
	for (long i = 0; i < n; i++) {
		int fd = (int)evs[i].data;
		/* Look the watcher up each time: earlier callbacks may remove it. */
		for (usize k = 0; k < l->fds.len; k++) {
			if (l->fds.data[k].fd == fd) {
				FdWatch w = l->fds.data[k];
				w.fn(l, fd, evs[i].events, w.data);
				break;
			}
		}
	}
	run_timers(l);
}

void loop_run(Loop *l)
{
	l->quit = false;
	while (!l->quit)
		loop_once(l, -1);
}

void loop_quit(Loop *l)
{
	l->quit = true;
	loop_wake(l);
}

void loop_wake(Loop *l)
{
	u64 one = 1;
	sys_write(l->wake_fd, &one, sizeof one);
}

void loop_on_wake(Loop *l, LoopTimerFn fn, void *data)
{
	l->wake_fn = fn;
	l->wake_data = data;
}
