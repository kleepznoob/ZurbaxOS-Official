/* Notifications from Linux programs (the Desktop Notifications
 * Specification, 1.2) on Zurbax's notification banners and in its notification
 * center; and org.freedesktop.ScreenSaver, which video players and games
 * use to keep the screen on. */
#include "zportal.h"

static const char NOTIFY_XML[] =
	"<interface name=\"org.freedesktop.Notifications\">"
	"<method name=\"GetCapabilities\"><arg type=\"as\" direction=\"out\"/></method>"
	"<method name=\"Notify\"><arg type=\"s\" name=\"app_name\" direction=\"in\"/><arg type=\"u\" name=\"replaces_id\" direction=\"in\"/>"
	"<arg type=\"s\" name=\"app_icon\" direction=\"in\"/><arg type=\"s\" name=\"summary\" direction=\"in\"/>"
	"<arg type=\"s\" name=\"body\" direction=\"in\"/><arg type=\"as\" name=\"actions\" direction=\"in\"/>"
	"<arg type=\"a{sv}\" name=\"hints\" direction=\"in\"/><arg type=\"i\" name=\"expire_timeout\" direction=\"in\"/>"
	"<arg type=\"u\" direction=\"out\"/></method>"
	"<method name=\"CloseNotification\"><arg type=\"u\" name=\"id\" direction=\"in\"/></method>"
	"<method name=\"GetServerInformation\"><arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
	"<arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/></method>"
	"<signal name=\"NotificationClosed\"><arg type=\"u\"/><arg type=\"u\"/></signal>"
	"<signal name=\"ActionInvoked\"><arg type=\"u\"/><arg type=\"s\"/></signal></interface>";

static u32 next_id;

/* Bodies may carry a little HTML (b, i, u, a, img): Zurbax shows plain text. */
static char *plain_text(const char *s)
{
	StrBuf sb = { 0 };
	while (*s) {
		if (*s == '<') {
			const char *end = str_chr(s, '>');
			if (end) {
				s = end + 1;
				continue;
			}
		}
		if (*s == '&') {
			static const struct {
				const char *ent, *ch;
			} ents[] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" } };
			bool done = false;
			for (usize i = 0; i < ARRAY_LEN(ents) && !done; i++)
				if (str_starts(s, ents[i].ent)) {
					sb_adds(&sb, ents[i].ch);
					s += strlen(ents[i].ent);
					done = true;
				}
			if (done)
				continue;
		}
		sb_addc(&sb, *s++);
	}
	return sb_take(&sb);
}

/* The icon Zurbax shows: the app's own (its menu entry's id), from the hints,
 * the caller's Flatpak id, or what the program called itself. */
static void notice_icon(const char *sender, const char *app_name, const DBusVal *hints, char *out, usize cap)
{
	const char *entry = dbus_str(dbus_dict_get(hints, "desktop-entry"), NULL);
	const char *app = caller_app(sender, NULL);
	if (entry && *entry)
		str_copy(out, entry, cap);
	else if (app && *app)
		str_copy(out, app, cap);
	else {
		str_copy(out, app_name && *app_name ? app_name : "notification", cap);
		str_lower(out);
	}
}

static bool notifications_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (dbus_is_call(call, "org.freedesktop.Notifications", "GetCapabilities")) {
		DBusMsg *r = dbus_new_reply(call);
		dbus_open(r, "as");
		dbus_add(r, "s", "body");
		dbus_add(r, "s", "persistence");
		dbus_add(r, "s", "icon-static");
		dbus_close(r);
		dbus_send(bus, r);
	} else if (dbus_is_call(call, "org.freedesktop.Notifications", "Notify")) {
		const char *app_name = dbus_str(dbus_arg(call, 0), "");
		u32 replaces = (u32)dbus_uint(dbus_arg(call, 1), 0);
		const char *summary = dbus_str(dbus_arg(call, 3), "");
		char *body = plain_text(dbus_str(dbus_arg(call, 4), ""));
		char icon[96];
		notice_icon(dbus_sender(call), app_name, dbus_arg(call, 6), icon, sizeof icon);
		display_notify(icon, summary, body);
		mem_free(body);
		DBusMsg *r = dbus_new_reply(call);
		dbus_add(r, "u", replaces ? replaces : ++next_id);
		dbus_send(bus, r);
	} else if (dbus_is_call(call, "org.freedesktop.Notifications", "CloseNotification")) {
		dbus_reply_empty(bus, call);
		DBusMsg *s = dbus_new_signal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "NotificationClosed");
		dbus_set_destination(s, dbus_sender(call));
		dbus_add(s, "uu", (u32)dbus_uint(dbus_arg(call, 0), 0), 3u /* closed by a call */);
		dbus_send(bus, s);
	} else if (dbus_is_call(call, "org.freedesktop.Notifications", "GetServerInformation")) {
		DBusMsg *r = dbus_new_reply(call);
		dbus_add(r, "ssss", "Zurbax", "Zurbax", "2.1", "1.2");
		dbus_send(bus, r);
	} else {
		return false;
	}
	return true;
}

/* ---- Keeping the screen on ----------------------------------------------------------------------- */

static const char SCREENSAVER_XML[] =
	"<interface name=\"org.freedesktop.ScreenSaver\">"
	"<method name=\"Inhibit\"><arg type=\"s\" name=\"application_name\" direction=\"in\"/>"
	"<arg type=\"s\" name=\"reason_for_inhibit\" direction=\"in\"/><arg type=\"u\" name=\"cookie\" direction=\"out\"/></method>"
	"<method name=\"UnInhibit\"><arg type=\"u\" name=\"cookie\" direction=\"in\"/></method>"
	"<method name=\"GetActive\"><arg type=\"b\" direction=\"out\"/></method></interface>";

static u32 inhibitors[64];
static int ninhibitors;
static u32 next_cookie;

/* zdisplay reads this before dimming or locking an idle screen. */
static void write_inhibit(void)
{
	char *path = str_fmt("%s/zos-linux/inhibit", P.rt);
	if (ninhibitors > 0)
		file_write_str(path, "1\n");
	else
		sys_unlink(path);
	mem_free(path);
}

u32 inhibit_add(void)
{
	u32 cookie = ++next_cookie;
	if (ninhibitors < (int)ARRAY_LEN(inhibitors))
		inhibitors[ninhibitors++] = cookie;
	write_inhibit();
	return cookie;
}

void inhibit_remove(u32 cookie)
{
	for (int i = 0; i < ninhibitors; i++)
		if (inhibitors[i] == cookie)
			inhibitors[i--] = inhibitors[--ninhibitors];
	write_inhibit();
}

static bool screensaver_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (dbus_is_call(call, "org.freedesktop.ScreenSaver", "Inhibit")) {
		DBusMsg *r = dbus_new_reply(call);
		dbus_add(r, "u", inhibit_add());
		dbus_send(bus, r);
	} else if (dbus_is_call(call, "org.freedesktop.ScreenSaver", "UnInhibit")) {
		inhibit_remove((u32)dbus_uint(dbus_arg(call, 0), 0));
		dbus_reply_empty(bus, call);
	} else if (dbus_is_call(call, "org.freedesktop.ScreenSaver", "GetActive")) {
		DBusMsg *r = dbus_new_reply(call);
		dbus_add(r, "b", 0);
		dbus_send(bus, r);
	} else {
		return false;
	}
	return true;
}

void notifications_start(DBus *bus)
{
	ninhibitors = 0;
	write_inhibit();
	dbus_export(bus, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", notifications_method, NULL, NOTIFY_XML);
	dbus_export(bus, "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", screensaver_method, NULL, SCREENSAVER_XML);
	dbus_export(bus, "/ScreenSaver", "org.freedesktop.ScreenSaver", screensaver_method, NULL, SCREENSAVER_XML);
	dbus_request_name(bus, "org.freedesktop.Notifications", DBUS_NAME_REPLACE);
	dbus_request_name(bus, "org.freedesktop.ScreenSaver", DBUS_NAME_REPLACE);
}
