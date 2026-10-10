/* Properties: Get, GetAll and Set from a table, and PropertiesChanged. */
#include "zsys.h"

static const Prop *find(const Prop *props, usize n, const char *name)
{
	for (usize i = 0; i < n; i++)
		if (str_eq(props[i].name, name))
			return &props[i];
	return NULL;
}

static void add_value(DBusMsg *m, const Prop *p, void *obj)
{
	dbus_open_variant(m, p->sig);
	p->get(m, obj);
	dbus_close(m);
}

bool props_call(DBus *bus, DBusMsg *call, const char *iface, const Prop *props, usize n, void *obj)
{
	const char *P = "org.freedesktop.DBus.Properties";
	if (!str_eq(dbus_interface(call) ? dbus_interface(call) : "", P))
		return false;
	if (!str_eq(dbus_str(dbus_arg(call, 0), ""), iface))
		return false;
	if (dbus_is_call(call, P, "Get")) {
		const Prop *p = find(props, n, dbus_str(dbus_arg(call, 1), ""));
		if (!p) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.UnknownProperty", "No such property");
			return true;
		}
		DBusMsg *m = dbus_new_reply(call);
		add_value(m, p, obj);
		dbus_send(bus, m);
		return true;
	}
	if (dbus_is_call(call, P, "GetAll")) {
		DBusMsg *m = dbus_new_reply(call);
		props_add_all(m, props, n, obj);
		dbus_send(bus, m);
		return true;
	}
	if (dbus_is_call(call, P, "Set")) {
		const char *name = dbus_str(dbus_arg(call, 1), "");
		const Prop *p = find(props, n, name);
		if (!p) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.UnknownProperty", "No such property");
		} else if (!p->set) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.PropertyReadOnly", "The property is read-only");
		} else if (!p->set(dbus_unvariant(dbus_arg(call, 2)), obj)) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs", "Can't set that value");
		} else {
			dbus_reply_empty(bus, call);
			const char *names[] = { name, NULL };
			props_changed(bus, dbus_path(call), iface, props, n, obj, names);
		}
		return true;
	}
	return false;
}

void props_add_all(DBusMsg *m, const Prop *props, usize n, void *obj)
{
	dbus_open(m, "a{sv}");
	for (usize i = 0; i < n; i++) {
		dbus_open(m, "{sv}");
		dbus_add(m, "s", props[i].name);
		add_value(m, &props[i], obj);
		dbus_close(m);
	}
	dbus_close(m);
}

void props_changed(DBus *bus, const char *path, const char *iface, const Prop *props, usize n, void *obj,
		   const char *const *names)
{
	if (!bus)
		return;
	DBusMsg *m = dbus_new_signal(path, "org.freedesktop.DBus.Properties", "PropertiesChanged");
	dbus_add(m, "s", iface);
	dbus_open(m, "a{sv}");
	for (const char *const *nm = names; *nm; nm++) {
		const Prop *p = find(props, n, *nm);
		if (!p)
			continue;
		dbus_open(m, "{sv}");
		dbus_add(m, "s", p->name);
		add_value(m, p, obj);
		dbus_close(m);
	}
	dbus_close(m);
	dbus_open(m, "as");
	dbus_close(m);
	dbus_send(bus, m);
}

char *path_escape(const char *s)
{
	StrBuf sb = { 0 };
	for (const u8 *c = (const u8 *)s; *c; c++) {
		if (is_alpha(*c) || (is_digit(*c) && c != (const u8 *)s))
			sb_addc(&sb, (char)*c);
		else
			sb_fmt(&sb, "_%02x", *c);
	}
	return sb_take(&sb);
}
