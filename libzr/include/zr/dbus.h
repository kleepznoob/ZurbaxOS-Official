/* D-Bus: the message bus Linux desktop programs talk to each other and to
 * the desktop on (notifications, file choosers, media keys, settings...).
 *
 * A connection to a bus, messages built argument by argument, and incoming
 * calls dispatched to the objects a program exports. Everything runs on
 * the caller's thread; dbus_attach() hands the socket to an event loop. */
#pragma once

#include "mem.h"
#include "os.h"

enum { DBUS_CALL = 1, DBUS_RETURN = 2, DBUS_ERROR = 3, DBUS_SIGNAL = 4 };
enum { DBUS_NO_REPLY = 1, DBUS_NO_AUTOSTART = 2 };

/* org.freedesktop.DBus.RequestName flags and replies */
enum { DBUS_NAME_ALLOW_REPLACE = 1, DBUS_NAME_REPLACE = 2, DBUS_NAME_NO_QUEUE = 4 };
enum { DBUS_NAME_OWNER = 1, DBUS_NAME_QUEUED = 2, DBUS_NAME_EXISTS = 3, DBUS_NAME_ALREADY_OWNER = 4 };

/* ---- Values ---------------------------------------------------------------------- */

/* One argument of a received message, or part of one. type is the D-Bus
 * type code: y b n q i u x t d (numbers in n, or d for doubles), s o g
 * (text in s), h (the descriptor in n), a ( { v (parts in items: an
 * array's elements, a structure's fields, a dictionary entry's key and
 * value, a variant's one value). sig is the value's full signature. */
typedef struct DBusVal DBusVal;
struct DBusVal {
	char type;
	u64 n;
	f64 d;
	char *s;
	char *sig;
	DBusVal **items;
	usize count;
};

/* Readers that forgive: a variant is looked through, a missing value or one
 * of another type gives the fallback. */
const char *dbus_str(const DBusVal *v, const char *fallback);
i64 dbus_int(const DBusVal *v, i64 fallback);    /* any integer type */
u64 dbus_uint(const DBusVal *v, u64 fallback);
bool dbus_bool(const DBusVal *v, bool fallback);
f64 dbus_double(const DBusVal *v, f64 fallback);
const DBusVal *dbus_unvariant(const DBusVal *v);
/* In an a{s...} dictionary, the value for key (looked through when a variant). */
const DBusVal *dbus_dict_get(const DBusVal *dict, const char *key);
/* An array of strings (as) as a NULL-terminated vector of borrowed pointers. */
const char **dbus_strv(const DBusVal *v, usize *count);

/* ---- Messages ---------------------------------------------------------------------- */

typedef struct DBusMsg DBusMsg;

DBusMsg *dbus_new_call(const char *dest, const char *path, const char *iface, const char *member);
DBusMsg *dbus_new_signal(const char *path, const char *iface, const char *member);
DBusMsg *dbus_new_reply(const DBusMsg *call);
DBusMsg *dbus_new_error(const DBusMsg *call, const char *name, const char *text);
void dbus_set_destination(DBusMsg *m, const char *dest);   /* signals sent to one peer */
void dbus_set_flags(DBusMsg *m, u8 flags);
DBusMsg *dbus_msg_ref(DBusMsg *m);
void dbus_msg_free(DBusMsg *m);                            /* drops a reference */

int dbus_type(const DBusMsg *m);
u8 dbus_flags(const DBusMsg *m);
u32 dbus_serial(const DBusMsg *m);
u32 dbus_reply_serial(const DBusMsg *m);
const char *dbus_path(const DBusMsg *m);
const char *dbus_interface(const DBusMsg *m);
const char *dbus_member(const DBusMsg *m);
const char *dbus_error_name(const DBusMsg *m);
const char *dbus_sender(const DBusMsg *m);
const char *dbus_destination(const DBusMsg *m);
const char *dbus_signature(const DBusMsg *m);
bool dbus_is_call(const DBusMsg *m, const char *iface, const char *member);

/* The arguments of a received message (NULL past the last one). */
const DBusVal *dbus_arg(DBusMsg *m, usize i);
usize dbus_nargs(DBusMsg *m);
/* A descriptor that came with the message, taken over by the caller (-1 if none). */
int dbus_take_fd(DBusMsg *m, const DBusVal *h);

/* Writing arguments. dbus_add takes simple types only, one per letter of
 * sig: y b n q i u x t (as ints, x and t as i64/u64), d (double), s o g
 * (strings), h (a descriptor, duplicated). Containers are opened and closed:
 *     dbus_open(m, "a{sv}");
 *         dbus_dict_add(m, "color-scheme", "u", 1);
 *     dbus_close(m);
 * dbus_open takes the container's whole signature ("a{sv}", "(ss)", "{sv}");
 * a variant is opened with dbus_open_variant and the signature it holds. */
void dbus_add(DBusMsg *m, const char *sig, ...);
void dbus_open(DBusMsg *m, const char *sig);
void dbus_open_variant(DBusMsg *m, const char *contained);
void dbus_close(DBusMsg *m);
/* {sv} entry of a dictionary: key, then one simple value of type sig. */
void dbus_dict_add(DBusMsg *m, const char *key, const char *sig, ...);
/* Copies a received value (any type) into the message being written. */
void dbus_add_val(DBusMsg *m, const DBusVal *v);

/* The message as it travels (header and body), and back: for tests and tools. */
u8 *dbus_msg_encode(DBusMsg *m, u32 serial, usize *len);
DBusMsg *dbus_msg_decode(const u8 *data, usize len);

/* ---- Connections ------------------------------------------------------------------------ */

typedef struct DBus DBus;

DBus *dbus_open_address(const char *address);   /* "unix:path=..." */
DBus *dbus_open_session(void);                  /* $DBUS_SESSION_BUS_ADDRESS */
void dbus_close_bus(DBus *bus);
const char *dbus_unique_name(const DBus *bus);
int dbus_fd(const DBus *bus);
bool dbus_connected(const DBus *bus);
/* Called once the connection is lost (from the loop). */
void dbus_on_disconnect(DBus *bus, void (*fn)(DBus *bus, void *data), void *data);

/* Sends m (and drops the caller's reference). Returns its serial, 0 on failure. */
u32 dbus_send(DBus *bus, DBusMsg *m);
/* Sends a call and waits for its reply (or an error message); NULL when the
 * time runs out or the bus goes. Messages that arrive meanwhile wait for
 * the next dispatch. */
DBusMsg *dbus_call(DBus *bus, DBusMsg *call, int timeout_ms);
typedef void (*DBusReplyFn)(DBus *bus, DBusMsg *reply, void *data); /* reply NULL on failure */
void dbus_call_async(DBus *bus, DBusMsg *call, DBusReplyFn fn, void *data);

/* Reads what has arrived and dispatches it. False once disconnected. */
bool dbus_dispatch(DBus *bus);
void dbus_attach(DBus *bus, Loop *loop);

/* The bus itself. */
int dbus_request_name(DBus *bus, const char *name, u32 flags);  /* DBUS_NAME_*, or -1 */
bool dbus_release_name(DBus *bus, const char *name);
bool dbus_add_match(DBus *bus, const char *rule);
bool dbus_name_has_owner(DBus *bus, const char *name);
/* The process id behind a unique name (0 if unknown). */
u32 dbus_peer_pid(DBus *bus, const char *name);

/* Objects this program serves. fn gets the calls to iface on path (or on
 * any path below it when path ends in "/" followed by "*", and the Properties calls about
 * iface); it returns false for a method it doesn't have. xml describes the
 * interface for Introspect (an <interface> element, or NULL). Peer and
 * Introspectable are answered for every exported path. */
typedef bool (*DBusMethodFn)(DBus *bus, DBusMsg *call, void *data);
void dbus_export(DBus *bus, const char *path, const char *iface, DBusMethodFn fn, void *data, const char *xml);
void dbus_unexport(DBus *bus, const char *path, const char *iface);

/* Signals: any of sender, path, iface, member may be NULL (anything). */
typedef void (*DBusSignalFn)(DBus *bus, DBusMsg *sig, void *data);
void dbus_subscribe(DBus *bus, const char *sender, const char *path, const char *iface, const char *member,
		    DBusSignalFn fn, void *data);

/* Replies: answers a call (dropping nothing: the call is still the caller's). */
void dbus_reply_empty(DBus *bus, const DBusMsg *call);
void dbus_reply_error(DBus *bus, const DBusMsg *call, const char *name, const char *text);

/* org.freedesktop.DBus.Properties.Get's answer: one simple value in a variant. */
void dbus_reply_property(DBus *bus, const DBusMsg *call, const char *sig, ...);
