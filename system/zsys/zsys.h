/*
 * zsys: Zurbax's system services, for a GNOME session.
 *
 * GNOME expects a Linux system's services on D-Bus's system bus: logind
 * for the session, its devices and power, AccountsService for the user,
 * UPower for batteries, hostnamed, localed and timedated, polkit's
 * authority, NetworkManager. Zurbax has its own pieces for all of that
 * (zinit, znet, its account files), and zsys answers on those interfaces
 * with them. It runs as root, outside the user's session, connected to the
 * session's own system bus, so it can open devices for GNOME's compositor
 * and pass them over the bus, check passwords, and power the computer off.
 */
#pragma once

#include "zr.h"

typedef struct {
	Loop *loop;
	DBus *bus;
	bool lost;
	u32 uid, gid;
	char user[64];
	char full_name[96];
	char home[256];
	char session[16];      /* the session's id ("1") */
	char seat[16];         /* "seat0" */
	char runtime[200];     /* /run/user/UID */
	i64 started_us;        /* when the session began (realtime, µs) */
	i64 started_mono_us;
} Sys;

extern Sys Z;

/* /data/system/NAME (allocated) */
char *data_path(const char *name);
/* users.conf again, after a change */
void account_reload(void);
/* the Linux layer's root as Zurbax sees it ($ZURBAX_LAYER or /data/layers/steam) */
const char *layer_root(void);
/* the account's password (true when it has none) */
bool accounts_check_password(const char *password);

/* ---- Properties (props.c) ---- */

typedef struct {
	const char *name, *sig;
	void (*get)(DBusMsg *m, void *obj);        /* writes one value of type sig */
	bool (*set)(const DBusVal *v, void *obj);  /* NULL: read-only */
} Prop;

/* Answers org.freedesktop.DBus.Properties calls about iface; false if the
 * call isn't one of those. */
bool props_call(DBus *bus, DBusMsg *call, const char *iface, const Prop *props, usize n, void *obj);
/* Every property as an a{sv} dictionary (GetAll's answer, ObjectManager's). */
void props_add_all(DBusMsg *m, const Prop *props, usize n, void *obj);
/* PropertiesChanged for the named properties (a NULL-terminated list). */
void props_changed(DBus *bus, const char *path, const char *iface, const Prop *props, usize n, void *obj,
		   const char *const *names);

/* A D-Bus object path piece for any text, the way systemd escapes it ("1" → "_31"). */
char *path_escape(const char *s);

/* ---- The services ---- */

void login1_start(DBus *bus);
void login1_stop(void);                 /* gives devices back */
void upower_start(DBus *bus);
void accounts_start(DBus *bus);
void hostname_start(DBus *bus);
void polkit_start(DBus *bus);
void network_start(DBus *bus);
void gdm_start(DBus *bus);

/* zinit: "poweroff" or "reboot" */
void power_action(const char *what);
