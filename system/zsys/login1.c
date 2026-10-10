/*
 * org.freedesktop.login1: the session GNOME runs in, its seat and user,
 * the devices of the seat, and the computer's power.
 *
 * There is one session (the person logged in on the screen), always the
 * active one. GNOME's compositor takes control of it and asks for each
 * device it drives (the graphics card, keyboards, mice, touchpads): zsys
 * opens them as root and hands them over the bus, the graphics card with
 * the right to set modes already its own. Inhibitors (no sleeping while a
 * download runs, the lid switch left to GNOME) are pipes whose closing
 * ends them.
 */
#include "zsys.h"

#define LOGIN1 "org.freedesktop.login1"
#define MANAGER_PATH "/org/freedesktop/login1"
#define MANAGER_IF "org.freedesktop.login1.Manager"
#define SESSION_IF "org.freedesktop.login1.Session"
#define SEAT_IF "org.freedesktop.login1.Seat"
#define USER_IF "org.freedesktop.login1.User"

#define DRM_MAJOR 226
#define INPUT_MAJOR 13
#define DRM_IOCTL_SET_MASTER 0x641e
#define DRM_IOCTL_DROP_MASTER 0x641f
#define EVIOCREVOKE 0x40044591

static char session_path[96], seat_path[96], user_path[96];

/* ---- Devices ---------------------------------------------------------------------------- */

typedef struct {
	u32 major, minor;
	int fd;
} Device;

static Device devices[128];
static int ndevices;
static char controller[64];     /* the bus name that took control of the session */

/* /dev/... for a character device, from what sysfs says about it. */
static bool device_path(u32 major, u32 minor, char *out, usize cap)
{
	char *uevent = str_fmt("/sys/dev/char/%u:%u/uevent", major, minor);
	char *text = file_read(uevent, NULL);
	mem_free(uevent);
	if (!text)
		return false;
	bool ok = false;
	usize n;
	char **lines = str_split(text, '\n', &n);
	for (usize i = 0; i < n; i++)
		if (str_starts(lines[i], "DEVNAME=")) {
			fmt(out, cap, "/dev/%s", lines[i] + 8);
			ok = true;
		}
	str_split_free(lines, n);
	mem_free(text);
	return ok;
}

static void device_release(Device *d)
{
	if (d->major == DRM_MAJOR)
		sys_ioctl(d->fd, DRM_IOCTL_DROP_MASTER, NULL);
	else if (d->major == INPUT_MAJOR)
		sys_ioctl(d->fd, EVIOCREVOKE, NULL);
	sys_close(d->fd);
	*d = devices[--ndevices];
}

static Device *device_find(u32 major, u32 minor)
{
	for (int i = 0; i < ndevices; i++)
		if (devices[i].major == major && devices[i].minor == minor)
			return &devices[i];
	return NULL;
}

static void release_all(void)
{
	while (ndevices)
		device_release(&devices[ndevices - 1]);
	controller[0] = 0;
}

void login1_stop(void)
{
	release_all();
}

/* ---- Inhibitors -------------------------------------------------------------------------- */

typedef struct {
	char what[96], who[96], why[160], mode[16];
	u32 uid, pid;
	int fd;          /* our end: it reports a hang-up when every copy of theirs is closed */
	bool used;
} Inhibitor;

static Inhibitor inhibitors[64];

static void inhibited_changed(void);

static void inhibitor_gone(Loop *loop, int fd, u32 events, void *data)
{
	(void)events;
	Inhibitor *in = data;
	loop_del_fd(loop, fd);
	sys_close(fd);
	in->used = false;
	inhibited_changed();
}

/* "what" lists joined by ':' for the inhibitors of a mode */
static void inhibited(const char *mode, char *out, usize cap)
{
	out[0] = 0;
	static const char *const KINDS[] = { "shutdown", "sleep", "idle", "handle-power-key", "handle-suspend-key",
					     "handle-hibernate-key", "handle-lid-switch" };
	for (usize k = 0; k < ARRAY_LEN(KINDS); k++)
		for (usize i = 0; i < ARRAY_LEN(inhibitors); i++) {
			Inhibitor *in = &inhibitors[i];
			if (!in->used || !str_eq(in->mode, mode))
				continue;
			bool has = false;
			usize n;
			char **parts = str_split(in->what, ':', &n);
			for (usize j = 0; j < n; j++)
				has |= str_eq(parts[j], KINDS[k]);
			str_split_free(parts, n);
			if (has) {
				usize len = strlen(out);
				fmt(out + len, cap - len, "%s%s", len ? ":" : "", KINDS[k]);
				break;
			}
		}
}

static bool delay_pending(const char *what)
{
	for (usize i = 0; i < ARRAY_LEN(inhibitors); i++)
		if (inhibitors[i].used && str_eq(inhibitors[i].mode, "delay") && str_find(inhibitors[i].what, what))
			return true;
	return false;
}

static bool blocked(const char *what)
{
	for (usize i = 0; i < ARRAY_LEN(inhibitors); i++)
		if (inhibitors[i].used && str_eq(inhibitors[i].mode, "block") && str_find(inhibitors[i].what, what))
			return true;
	return false;
}

/* ---- Session state ------------------------------------------------------------------------- */

static bool idle_hint, locked_hint;
static i64 idle_since_us, idle_since_mono_us;
static bool preparing_sleep, preparing_shutdown;

static i64 now_us(void)
{
	return time_unix_ms() * 1000;
}

static i64 mono_us(void)
{
	return (i64)(time_mono_ns() / 1000);
}

/* ---- Property getters ---------------------------------------------------------------------- */

#define GET(name) static void name(DBusMsg *m, void *obj)
GET(p_false) { (void)obj; dbus_add(m, "b", false); }
GET(p_true) { (void)obj; dbus_add(m, "b", true); }
GET(p_empty_s) { (void)obj; dbus_add(m, "s", ""); }
GET(p_zero_t) { (void)obj; dbus_add(m, "t", (u64)0); }
GET(p_zero_u) { (void)obj; dbus_add(m, "u", 0u); }
GET(p_empty_as) { (void)obj; dbus_open(m, "as"); dbus_close(m); }
GET(p_ignore) { (void)obj; dbus_add(m, "s", "ignore"); }
GET(p_idle) { (void)obj; dbus_add(m, "b", idle_hint); }
GET(p_idle_since) { (void)obj; dbus_add(m, "t", (u64)idle_since_us); }
GET(p_idle_since_mono) { (void)obj; dbus_add(m, "t", (u64)idle_since_mono_us); }
GET(p_block) { (void)obj; char s[200]; inhibited("block", s, sizeof s); dbus_add(m, "s", s); }
GET(p_delay) { (void)obj; char s[200]; inhibited("delay", s, sizeof s); dbus_add(m, "s", s); }
GET(p_delay_max) { (void)obj; dbus_add(m, "t", (u64)5000000); }
GET(p_preparing_sleep) { (void)obj; dbus_add(m, "b", preparing_sleep); }
GET(p_preparing_shutdown) { (void)obj; dbus_add(m, "b", preparing_shutdown); }
GET(p_suspend) { (void)obj; dbus_add(m, "s", "suspend"); }
GET(p_poweroff) { (void)obj; dbus_add(m, "s", "poweroff"); }
GET(p_scheduled) { (void)obj; dbus_open(m, "(st)"); dbus_add(m, "st", "", (u64)0); dbus_close(m); }

/* the lid, docking and the charger, from the kernel */
static bool lid_closed(void)
{
	DirList l = { 0 };
	bool closed = false;
	if (dir_list("/proc/acpi/button/lid", &l, false)) {
		vec_each(&l, e)
		{
			char *p = str_fmt("/proc/acpi/button/lid/%s/state", e->name);
			char *t = file_read(p, NULL);
			closed |= t && str_find(t, "closed");
			mem_free(t);
			mem_free(p);
		}
		dir_list_free(&l);
	}
	return closed;
}

static bool on_external_power(void)
{
	DirList l = { 0 };
	bool online = true, any = false;
	if (dir_list("/sys/class/power_supply", &l, false)) {
		vec_each(&l, e)
		{
			char *tp = str_fmt("/sys/class/power_supply/%s/type", e->name);
			char *type = file_read(tp, NULL);
			if (type && str_starts(type, "Mains")) {
				char *op = str_fmt("/sys/class/power_supply/%s/online", e->name);
				char *o = file_read(op, NULL);
				if (!any)
					online = false;
				any = true;
				online |= o && o[0] == '1';
				mem_free(o);
				mem_free(op);
			}
			mem_free(type);
			mem_free(tp);
		}
		dir_list_free(&l);
	}
	return online;
}

GET(p_lid_closed) { (void)obj; dbus_add(m, "b", lid_closed()); }
GET(p_external) { (void)obj; dbus_add(m, "b", on_external_power()); }

static const Prop MANAGER_PROPS[] = {
	{ "EnableWallMessages", "b", p_false, NULL },
	{ "WallMessage", "s", p_empty_s, NULL },
	{ "NAutoVTs", "u", p_zero_u, NULL },
	{ "KillOnlyUsers", "as", p_empty_as, NULL },
	{ "KillExcludeUsers", "as", p_empty_as, NULL },
	{ "KillUserProcesses", "b", p_false, NULL },
	{ "RebootToFirmwareSetup", "b", p_false, NULL },
	{ "IdleHint", "b", p_idle, NULL },
	{ "IdleSinceHint", "t", p_idle_since, NULL },
	{ "IdleSinceHintMonotonic", "t", p_idle_since_mono, NULL },
	{ "BlockInhibited", "s", p_block, NULL },
	{ "DelayInhibited", "s", p_delay, NULL },
	{ "InhibitDelayMaxUSec", "t", p_delay_max, NULL },
	{ "UserStopDelayUSec", "t", p_zero_t, NULL },
	{ "HandlePowerKey", "s", p_poweroff, NULL },
	{ "HandleSuspendKey", "s", p_suspend, NULL },
	{ "HandleHibernateKey", "s", p_ignore, NULL },
	{ "HandleLidSwitch", "s", p_suspend, NULL },
	{ "HandleLidSwitchExternalPower", "s", p_suspend, NULL },
	{ "HandleLidSwitchDocked", "s", p_ignore, NULL },
	{ "IdleAction", "s", p_ignore, NULL },
	{ "IdleActionUSec", "t", p_zero_t, NULL },
	{ "PreparingForShutdown", "b", p_preparing_shutdown, NULL },
	{ "PreparingForSleep", "b", p_preparing_sleep, NULL },
	{ "ScheduledShutdown", "(st)", p_scheduled, NULL },
	{ "Docked", "b", p_false, NULL },
	{ "LidClosed", "b", p_lid_closed, NULL },
	{ "OnExternalPower", "b", p_external, NULL },
	{ "RemoveIPC", "b", p_false, NULL },
};

GET(p_id) { (void)obj; dbus_add(m, "s", Z.session); }
GET(p_user) { (void)obj; dbus_open(m, "(uo)"); dbus_add(m, "uo", Z.uid, user_path); dbus_close(m); }
GET(p_name) { (void)obj; dbus_add(m, "s", Z.user); }
GET(p_timestamp) { (void)obj; dbus_add(m, "t", (u64)Z.started_us); }
GET(p_timestamp_mono) { (void)obj; dbus_add(m, "t", (u64)Z.started_mono_us); }
GET(p_vtnr) { (void)obj; dbus_add(m, "u", 1u); }
GET(p_seat) { (void)obj; dbus_open(m, "(so)"); dbus_add(m, "so", Z.seat, seat_path); dbus_close(m); }
GET(p_tty) { (void)obj; dbus_add(m, "s", "tty1"); }
GET(p_service) { (void)obj; dbus_add(m, "s", "zurbax"); }
GET(p_desktop) { (void)obj; dbus_add(m, "s", "GNOME"); }
GET(p_wayland) { (void)obj; dbus_add(m, "s", "wayland"); }
GET(p_class) { (void)obj; dbus_add(m, "s", "user"); }
GET(p_active_state) { (void)obj; dbus_add(m, "s", "active"); }
GET(p_locked) { (void)obj; dbus_add(m, "b", locked_hint); }
GET(p_scope) { (void)obj; dbus_add(m, "s", "session-1.scope"); }

static const Prop SESSION_PROPS[] = {
	{ "Id", "s", p_id, NULL },
	{ "User", "(uo)", p_user, NULL },
	{ "Name", "s", p_name, NULL },
	{ "Timestamp", "t", p_timestamp, NULL },
	{ "TimestampMonotonic", "t", p_timestamp_mono, NULL },
	{ "VTNr", "u", p_vtnr, NULL },
	{ "Seat", "(so)", p_seat, NULL },
	{ "TTY", "s", p_tty, NULL },
	{ "Display", "s", p_empty_s, NULL },
	{ "Remote", "b", p_false, NULL },
	{ "RemoteHost", "s", p_empty_s, NULL },
	{ "RemoteUser", "s", p_empty_s, NULL },
	{ "Service", "s", p_service, NULL },
	{ "Desktop", "s", p_desktop, NULL },
	{ "Scope", "s", p_scope, NULL },
	{ "Leader", "u", p_zero_u, NULL },
	{ "Audit", "u", p_zero_u, NULL },
	{ "Type", "s", p_wayland, NULL },
	{ "Class", "s", p_class, NULL },
	{ "Active", "b", p_true, NULL },
	{ "State", "s", p_active_state, NULL },
	{ "IdleHint", "b", p_idle, NULL },
	{ "IdleSinceHint", "t", p_idle_since, NULL },
	{ "IdleSinceHintMonotonic", "t", p_idle_since_mono, NULL },
	{ "CanIdle", "b", p_true, NULL },
	{ "CanLock", "b", p_true, NULL },
	{ "LockedHint", "b", p_locked, NULL },
};

GET(p_seat_id) { (void)obj; dbus_add(m, "s", Z.seat); }
GET(p_active_session) { (void)obj; dbus_open(m, "(so)"); dbus_add(m, "so", Z.session, session_path); dbus_close(m); }
GET(p_sessions) { (void)obj; dbus_open(m, "a(so)"); dbus_open(m, "(so)"); dbus_add(m, "so", Z.session, session_path); dbus_close(m); dbus_close(m); }

static const Prop SEAT_PROPS[] = {
	{ "Id", "s", p_seat_id, NULL },
	{ "ActiveSession", "(so)", p_active_session, NULL },
	{ "CanTTY", "b", p_false, NULL },
	{ "CanGraphical", "b", p_true, NULL },
	{ "Sessions", "a(so)", p_sessions, NULL },
	{ "IdleHint", "b", p_idle, NULL },
	{ "IdleSinceHint", "t", p_idle_since, NULL },
	{ "IdleSinceHintMonotonic", "t", p_idle_since_mono, NULL },
};

GET(p_uid) { (void)obj; dbus_add(m, "u", Z.uid); }
GET(p_gid) { (void)obj; dbus_add(m, "u", Z.gid); }
GET(p_runtime) { (void)obj; dbus_add(m, "s", Z.runtime); }
GET(p_slice) { (void)obj; char s[64]; fmt(s, sizeof s, "user-%u.slice", Z.uid); dbus_add(m, "s", s); }
GET(p_service_unit) { (void)obj; char s[64]; fmt(s, sizeof s, "user@%u.service", Z.uid); dbus_add(m, "s", s); }

static const Prop USER_PROPS[] = {
	{ "UID", "u", p_uid, NULL },
	{ "GID", "u", p_gid, NULL },
	{ "Name", "s", p_name, NULL },
	{ "Timestamp", "t", p_timestamp, NULL },
	{ "TimestampMonotonic", "t", p_timestamp_mono, NULL },
	{ "RuntimePath", "s", p_runtime, NULL },
	{ "Service", "s", p_service_unit, NULL },
	{ "Slice", "s", p_slice, NULL },
	{ "Display", "(so)", p_active_session, NULL },
	{ "State", "s", p_active_state, NULL },
	{ "Sessions", "a(so)", p_sessions, NULL },
	{ "IdleHint", "b", p_idle, NULL },
	{ "IdleSinceHint", "t", p_idle_since, NULL },
	{ "IdleSinceHintMonotonic", "t", p_idle_since_mono, NULL },
	{ "Linger", "b", p_false, NULL },
};

static void inhibited_changed(void)
{
	static const char *const names[] = { "BlockInhibited", "DelayInhibited", NULL };
	props_changed(Z.bus, MANAGER_PATH, MANAGER_IF, MANAGER_PROPS, ARRAY_LEN(MANAGER_PROPS), NULL, names);
}

/* ---- Power ------------------------------------------------------------------------------- */

static void signal_bool(const char *member, bool v)
{
	DBusMsg *m = dbus_new_signal(MANAGER_PATH, MANAGER_IF, member);
	dbus_add(m, "b", v);
	dbus_send(Z.bus, m);
}

static void manager_changed(const char *name)
{
	const char *names[] = { name, NULL };
	props_changed(Z.bus, MANAGER_PATH, MANAGER_IF, MANAGER_PROPS, ARRAY_LEN(MANAGER_PROPS), NULL, names);
}

static const char *pending_power;    /* "suspend", "poweroff" or "reboot" once delays are over */
static u32 power_timer;

static void do_power(Loop *loop, void *data)
{
	(void)loop, (void)data;
	power_timer = 0;
	const char *what = pending_power;
	pending_power = NULL;
	if (!what)
		return;
	if (str_eq(what, "suspend")) {
		file_write_str("/sys/power/state", "mem");
		/* back: tell everyone, so they wake up too */
		preparing_sleep = false;
		manager_changed("PreparingForSleep");
		signal_bool("PrepareForSleep", false);
	} else {
		power_action(what);
	}
}

/* Starts a power action: those holding delay inhibitors get a moment
 * (until they let go, or five seconds) to get ready. */
static void begin_power(const char *what)
{
	bool sleep = str_eq(what, "suspend");
	if (sleep) {
		preparing_sleep = true;
		manager_changed("PreparingForSleep");
		signal_bool("PrepareForSleep", true);
	} else {
		preparing_shutdown = true;
		manager_changed("PreparingForShutdown");
		signal_bool("PrepareForShutdown", true);
	}
	pending_power = what;
	if (power_timer)
		loop_cancel(Z.loop, power_timer);
	power_timer = loop_timer(Z.loop, delay_pending(sleep ? "sleep" : "shutdown") ? 5000 : 200, 0, do_power, NULL);
}

static bool can_suspend(void)
{
	char *t = file_read("/sys/power/state", NULL);
	bool ok = t && str_find(t, "mem");
	mem_free(t);
	return ok;
}

/* ---- Methods ---------------------------------------------------------------------------- */

static void reply_object(DBus *bus, DBusMsg *call, const char *path)
{
	DBusMsg *m = dbus_new_reply(call);
	dbus_add(m, "o", path);
	dbus_send(bus, m);
}

static void reply_string(DBus *bus, DBusMsg *call, const char *s)
{
	DBusMsg *m = dbus_new_reply(call);
	dbus_add(m, "s", s);
	dbus_send(bus, m);
}

static bool is_ours(const char *id)
{
	return !*id || str_eq(id, Z.session) || str_eq(id, "self") || str_eq(id, "auto");
}

static void no_such(DBus *bus, DBusMsg *call, const char *what)
{
	dbus_reply_error(bus, call, "org.freedesktop.login1.NoSuchSession", what);
}

static void emit_session(const char *member)
{
	dbus_send(Z.bus, dbus_new_signal(session_path, SESSION_IF, member));
}

static bool manager_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, MANAGER_IF, MANAGER_PROPS, ARRAY_LEN(MANAGER_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (!str_eq(dbus_interface(call) ? dbus_interface(call) : MANAGER_IF, MANAGER_IF))
		return false;
	if (str_eq(mbr, "GetSession")) {
		if (is_ours(dbus_str(dbus_arg(call, 0), "")))
			reply_object(bus, call, session_path);
		else
			no_such(bus, call, "No such session");
	} else if (str_eq(mbr, "GetSessionByPID") || str_eq(mbr, "GetSessionByPIDFD")) {
		reply_object(bus, call, session_path);
	} else if (str_eq(mbr, "GetUser") || str_eq(mbr, "GetUserByPID")) {
		if (str_eq(mbr, "GetUser") && dbus_uint(dbus_arg(call, 0), 0) != Z.uid)
			dbus_reply_error(bus, call, "org.freedesktop.login1.NoSuchUser", "No such user");
		else
			reply_object(bus, call, user_path);
	} else if (str_eq(mbr, "GetSeat")) {
		const char *s = dbus_str(dbus_arg(call, 0), "");
		if (!*s || str_eq(s, Z.seat) || str_eq(s, "self") || str_eq(s, "auto"))
			reply_object(bus, call, seat_path);
		else
			dbus_reply_error(bus, call, "org.freedesktop.login1.NoSuchSeat", "No such seat");
	} else if (str_eq(mbr, "ListSessions")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "a(susso)");
		dbus_open(m, "(susso)");
		dbus_add(m, "susso", Z.session, Z.uid, Z.user, Z.seat, session_path);
		dbus_close(m);
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "ListUsers")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "a(uso)");
		dbus_open(m, "(uso)");
		dbus_add(m, "uso", Z.uid, Z.user, user_path);
		dbus_close(m);
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "ListSeats")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "a(so)");
		dbus_open(m, "(so)");
		dbus_add(m, "so", Z.seat, seat_path);
		dbus_close(m);
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "ListInhibitors")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "a(ssssuu)");
		for (usize i = 0; i < ARRAY_LEN(inhibitors); i++) {
			Inhibitor *in = &inhibitors[i];
			if (!in->used)
				continue;
			dbus_open(m, "(ssssuu)");
			dbus_add(m, "ssssuu", in->what, in->who, in->why, in->mode, in->uid, in->pid);
			dbus_close(m);
		}
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "Inhibit")) {
		Inhibitor *in = NULL;
		for (usize i = 0; i < ARRAY_LEN(inhibitors) && !in; i++)
			if (!inhibitors[i].used)
				in = &inhibitors[i];
		int fds[2];
		if (!in || sys_pipe2(fds, O_CLOEXEC) < 0) {
			dbus_reply_error(bus, call, "org.freedesktop.login1.TooManyInhibitors", "Too many inhibitors");
			return true;
		}
		str_copy(in->what, dbus_str(dbus_arg(call, 0), ""), sizeof in->what);
		str_copy(in->who, dbus_str(dbus_arg(call, 1), ""), sizeof in->who);
		str_copy(in->why, dbus_str(dbus_arg(call, 2), ""), sizeof in->why);
		str_copy(in->mode, dbus_str(dbus_arg(call, 3), "block"), sizeof in->mode);
		in->uid = Z.uid;
		in->pid = dbus_peer_pid(bus, dbus_sender(call));
		in->fd = fds[0];
		in->used = true;
		loop_add_fd(Z.loop, fds[0], EPOLLIN, inhibitor_gone, in);
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "h", fds[1]);
		dbus_send(bus, m);
		sys_close(fds[1]);
		inhibited_changed();
	} else if (str_eq(mbr, "CanPowerOff") || str_eq(mbr, "CanReboot")) {
		reply_string(bus, call, blocked("shutdown") ? "challenge" : "yes");
	} else if (str_eq(mbr, "CanSuspend")) {
		reply_string(bus, call, can_suspend() ? (blocked("sleep") ? "challenge" : "yes") : "na");
	} else if (str_starts(mbr, "Can")) {
		reply_string(bus, call, "na");   /* hibernation and the boot loader's tricks */
	} else if (str_eq(mbr, "PowerOff") || str_eq(mbr, "PowerOffWithFlags")) {
		dbus_reply_empty(bus, call);
		begin_power("poweroff");
	} else if (str_eq(mbr, "Reboot") || str_eq(mbr, "RebootWithFlags")) {
		dbus_reply_empty(bus, call);
		begin_power("reboot");
	} else if (str_eq(mbr, "Suspend") || str_eq(mbr, "SuspendWithFlags")) {
		if (!can_suspend()) {
			dbus_reply_error(bus, call, "org.freedesktop.login1.SleepVerbNotSupported", "This computer can't sleep");
			return true;
		}
		dbus_reply_empty(bus, call);
		begin_power("suspend");
	} else if (str_eq(mbr, "LockSession") || str_eq(mbr, "LockSessions")) {
		dbus_reply_empty(bus, call);
		emit_session("Lock");
	} else if (str_eq(mbr, "UnlockSession") || str_eq(mbr, "UnlockSessions")) {
		dbus_reply_empty(bus, call);
		emit_session("Unlock");
	} else if (str_eq(mbr, "ActivateSession") || str_eq(mbr, "ActivateSessionOnSeat")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "TerminateSession") || str_eq(mbr, "TerminateUser") || str_eq(mbr, "KillSession") ||
		   str_eq(mbr, "KillUser")) {
		dbus_reply_empty(bus, call);
		release_all();
	} else if (str_eq(mbr, "SetRebootToFirmwareSetup") || str_eq(mbr, "SetWallMessage") || str_eq(mbr, "ScheduleShutdown") ||
		   str_eq(mbr, "CancelScheduledShutdown")) {
		dbus_reply_empty(bus, call);
	} else {
		return false;
	}
	return true;
}

static void session_changed(const char *name)
{
	const char *names[] = { name, NULL };
	props_changed(Z.bus, session_path, SESSION_IF, SESSION_PROPS, ARRAY_LEN(SESSION_PROPS), NULL, names);
}

/* Brightness: SetBrightness("backlight", "intel_backlight", level) */
static bool set_brightness(const char *subsystem, const char *name, u32 value)
{
	if (!str_eq(subsystem, "backlight") && !str_eq(subsystem, "leds"))
		return false;
	if (str_chr(name, '/') || str_eq(name, "..") || str_eq(name, "."))
		return false;
	char *path = str_fmt("/sys/class/%s/%s/brightness", subsystem, name);
	char v[16];
	fmt(v, sizeof v, "%u", value);
	bool ok = file_write_str(path, v);
	mem_free(path);
	return ok;
}

static bool session_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, SESSION_IF, SESSION_PROPS, ARRAY_LEN(SESSION_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "TakeControl")) {
		if (controller[0] && !str_eq(controller, dbus_sender(call)) && !dbus_bool(dbus_arg(call, 0), false)) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.AccessDenied", "Another program controls this session");
			return true;
		}
		release_all();
		str_copy(controller, dbus_sender(call), sizeof controller);
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "ReleaseControl")) {
		release_all();
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "TakeDevice")) {
		u32 major = (u32)dbus_uint(dbus_arg(call, 0), 0), minor = (u32)dbus_uint(dbus_arg(call, 1), 0);
		if (!str_eq(controller, dbus_sender(call))) {
			dbus_reply_error(bus, call, "org.freedesktop.login1.NotInControl", "Take control of the session first");
			return true;
		}
		if (major != DRM_MAJOR && major != INPUT_MAJOR && major != 29 /* fbdev */ && major != 10 /* misc (uinput) */) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.AccessDenied", "Not a device of this seat");
			return true;
		}
		Device *d = device_find(major, minor);
		if (d) {
			dbus_reply_error(bus, call, "org.freedesktop.login1.DeviceIsTaken", "The device is already taken");
			return true;
		}
		char path[128];
		int fd = -1;
		if (device_path(major, minor, path, sizeof path))
			fd = (int)sys_open(path, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK, 0);
		if (fd < 0 || ndevices >= (int)ARRAY_LEN(devices)) {
			if (fd >= 0)
				sys_close(fd);
			dbus_reply_error(bus, call, "org.freedesktop.login1.DeviceNotFound", "Can't open the device");
			return true;
		}
		if (major == DRM_MAJOR)
			sys_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL);
		devices[ndevices++] = (Device){ major, minor, fd };
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "hb", fd, false);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "ReleaseDevice")) {
		Device *d = device_find((u32)dbus_uint(dbus_arg(call, 0), 0), (u32)dbus_uint(dbus_arg(call, 1), 0));
		if (d)
			device_release(d);
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "PauseDeviceComplete")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "SetIdleHint")) {
		bool v = dbus_bool(dbus_arg(call, 0), false);
		dbus_reply_empty(bus, call);
		if (v != idle_hint) {
			idle_hint = v;
			idle_since_us = now_us();
			idle_since_mono_us = mono_us();
			session_changed("IdleHint");
			manager_changed("IdleHint");
		}
	} else if (str_eq(mbr, "SetLockedHint")) {
		locked_hint = dbus_bool(dbus_arg(call, 0), false);
		dbus_reply_empty(bus, call);
		session_changed("LockedHint");
	} else if (str_eq(mbr, "Lock")) {
		dbus_reply_empty(bus, call);
		emit_session("Lock");
	} else if (str_eq(mbr, "Unlock")) {
		dbus_reply_empty(bus, call);
		emit_session("Unlock");
	} else if (str_eq(mbr, "Activate") || str_eq(mbr, "SetType") || str_eq(mbr, "SetClass") || str_eq(mbr, "SetDisplay") ||
		   str_eq(mbr, "SetTTY")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "Terminate") || str_eq(mbr, "Kill")) {
		dbus_reply_empty(bus, call);
		release_all();
	} else if (str_eq(mbr, "SetBrightness")) {
		if (set_brightness(dbus_str(dbus_arg(call, 0), ""), dbus_str(dbus_arg(call, 1), ""), (u32)dbus_uint(dbus_arg(call, 2), 0)))
			dbus_reply_empty(bus, call);
		else
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs", "No such backlight");
	} else {
		return false;
	}
	return true;
}

static bool seat_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, SEAT_IF, SEAT_PROPS, ARRAY_LEN(SEAT_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "ActivateSession") || str_eq(mbr, "Terminate")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "SwitchTo") || str_eq(mbr, "SwitchToNext") || str_eq(mbr, "SwitchToPrevious")) {
		dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.NotSupported", "This seat has one session");
	} else {
		return false;
	}
	return true;
}

static bool user_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, USER_IF, USER_PROPS, ARRAY_LEN(USER_PROPS), NULL))
		return true;
	if (str_eq(dbus_member(call), "Terminate") || str_eq(dbus_member(call), "Kill")) {
		dbus_reply_empty(bus, call);
		return true;
	}
	return false;
}

/* The controller left the bus: its devices come back. */
static void on_name_owner(DBus *bus, DBusMsg *sig, void *data)
{
	(void)bus, (void)data;
	const char *name = dbus_str(dbus_arg(sig, 0), ""), *now = dbus_str(dbus_arg(sig, 2), "");
	if (controller[0] && str_eq(name, controller) && !*now)
		release_all();
}

static const char MANAGER_XML[] =
	"<interface name=\"org.freedesktop.login1.Manager\">"
	"<method name=\"GetSession\"><arg type=\"s\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"GetSessionByPID\"><arg type=\"u\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"GetUser\"><arg type=\"u\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"GetSeat\"><arg type=\"s\" direction=\"in\"/><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"ListSessions\"><arg type=\"a(susso)\" direction=\"out\"/></method>"
	"<method name=\"ListUsers\"><arg type=\"a(uso)\" direction=\"out\"/></method>"
	"<method name=\"ListSeats\"><arg type=\"a(so)\" direction=\"out\"/></method>"
	"<method name=\"ListInhibitors\"><arg type=\"a(ssssuu)\" direction=\"out\"/></method>"
	"<method name=\"Inhibit\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"s\" direction=\"in\"/><arg type=\"h\" direction=\"out\"/></method>"
	"<method name=\"PowerOff\"><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"Reboot\"><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"Suspend\"><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"CanPowerOff\"><arg type=\"s\" direction=\"out\"/></method>"
	"<method name=\"CanReboot\"><arg type=\"s\" direction=\"out\"/></method>"
	"<method name=\"CanSuspend\"><arg type=\"s\" direction=\"out\"/></method>"
	"<method name=\"CanHibernate\"><arg type=\"s\" direction=\"out\"/></method>"
	"<method name=\"LockSession\"><arg type=\"s\" direction=\"in\"/></method>"
	"<method name=\"UnlockSession\"><arg type=\"s\" direction=\"in\"/></method>"
	"<signal name=\"PrepareForSleep\"><arg type=\"b\"/></signal>"
	"<signal name=\"PrepareForShutdown\"><arg type=\"b\"/></signal>"
	"</interface>";

static const char SESSION_XML[] =
	"<interface name=\"org.freedesktop.login1.Session\">"
	"<method name=\"TakeControl\"><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"ReleaseControl\"/>"
	"<method name=\"TakeDevice\"><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
	"<arg type=\"h\" direction=\"out\"/><arg type=\"b\" direction=\"out\"/></method>"
	"<method name=\"ReleaseDevice\"><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method>"
	"<method name=\"PauseDeviceComplete\"><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method>"
	"<method name=\"SetIdleHint\"><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"SetLockedHint\"><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"SetBrightness\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method>"
	"<method name=\"Lock\"/><method name=\"Unlock\"/><method name=\"Activate\"/><method name=\"Terminate\"/>"
	"<signal name=\"PauseDevice\"><arg type=\"u\"/><arg type=\"u\"/><arg type=\"s\"/></signal>"
	"<signal name=\"ResumeDevice\"><arg type=\"u\"/><arg type=\"u\"/><arg type=\"h\"/></signal>"
	"<signal name=\"Lock\"/><signal name=\"Unlock\"/>"
	"</interface>";

void login1_start(DBus *bus)
{
	char *sid = path_escape(Z.session), *seat = path_escape(Z.seat);
	fmt(session_path, sizeof session_path, "/org/freedesktop/login1/session/%s", sid);
	fmt(seat_path, sizeof seat_path, "/org/freedesktop/login1/seat/%s", seat);
	fmt(user_path, sizeof user_path, "/org/freedesktop/login1/user/_%u", Z.uid);
	mem_free(sid);
	mem_free(seat);
	dbus_export(bus, MANAGER_PATH, MANAGER_IF, manager_method, NULL, MANAGER_XML);
	dbus_export(bus, session_path, SESSION_IF, session_method, NULL, SESSION_XML);
	dbus_export(bus, "/org/freedesktop/login1/session/self", SESSION_IF, session_method, NULL, SESSION_XML);
	dbus_export(bus, "/org/freedesktop/login1/session/auto", SESSION_IF, session_method, NULL, SESSION_XML);
	dbus_export(bus, seat_path, SEAT_IF, seat_method, NULL, NULL);
	dbus_export(bus, "/org/freedesktop/login1/seat/self", SEAT_IF, seat_method, NULL, NULL);
	dbus_export(bus, "/org/freedesktop/login1/seat/auto", SEAT_IF, seat_method, NULL, NULL);
	dbus_export(bus, user_path, USER_IF, user_method, NULL, NULL);
	dbus_export(bus, "/org/freedesktop/login1/user/self", USER_IF, user_method, NULL, NULL);
	dbus_add_match(bus, "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged'");
	dbus_subscribe(bus, "org.freedesktop.DBus", NULL, "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner, NULL);
	dbus_request_name(bus, LOGIN1, DBUS_NAME_NO_QUEUE);
}
