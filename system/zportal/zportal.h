/* zportal: ZOS's side of the freedesktop.org desktop services, for the
 * Linux programs in the user's Linux session (Flatpak apps and others). */
#pragma once

#include "zr.h"
#include "zui.h"

typedef struct {
	Loop *loop;
	DBus *bus;
	bool bus_lost;         /* freed by the next check (not from inside its own dispatch) */
	int display;           /* zdisplay's app socket, or -1 */
	char rt[200];          /* the runtime folder, /run/user/UID */
	char home[256];
	char user[64];
	char full_name[64];
	u32 uid, gid;
	u32 next_token;
	i64 settings_mtime, network_mtime;
} Portal;

extern Portal P;

/* zdisplay */
void display_notify(const char *icon, const char *title, const char *body);
void display_launch(const char *app_id, const char *arg);
void display_settings_changed(void);

/* The app behind a D-Bus caller: its Flatpak id ("" for programs outside
 * Flatpak's sandbox), and whether it is sandboxed. */
const char *caller_app(const char *sender, bool *sandboxed);

/* Portal requests: the object a call answers with at once, whose Response
 * signal brings the result later. */
typedef struct Request Request;
struct Request {
	char *path, *sender;
	void (*closed)(Request *r);   /* the caller gave up (Close): stop what's running */
	void *data;
	int pid;                      /* a helper's process, if one runs for it */
	int out_fd;                   /* its output */
	StrBuf out;
};
Request *request_new(DBusMsg *call, const DBusVal *options);
void request_reply_handle(Request *r, DBusMsg *call);
/* The Response signal with the code (0 done, 1 cancelled, 2 failed) and an
 * open a{sv} for the results; request_finish closes and sends it. */
DBusMsg *request_response(Request *r, u32 code);
void request_finish(Request *r, DBusMsg *response);
void request_respond_empty(Request *r, u32 code);

/* "file:///path" with the bytes URIs can't hold escaped, and back. */
char *path_to_uri(const char *path);
char *uri_to_path(const char *uri);

/* Pieces of the services, started when the bus comes */
void notifications_start(DBus *bus);
void portals_start(DBus *bus);
void documents_start(DBus *bus);
void documents_tick(DBus *bus);
void settings_poll(void);
u32 inhibit_add(void);
void inhibit_remove(u32 cookie);
void network_poll(void);
/* Documents: a file made reachable for a sandboxed app; the path it sees. */
char *document_export(const char *path, const char *app_id, bool writable);

/* zportal pick ...: the file chooser window (picker.c) */
int picker_main(int argc, char **argv);

/* Property "version" of a portal interface (Properties.Get and GetAll). */
bool reply_version(DBus *bus, DBusMsg *call, u32 version);
