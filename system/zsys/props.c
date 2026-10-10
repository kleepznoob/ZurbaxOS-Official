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

/* ---- Objects of an ObjectManager --------------------------------------------------------- */

u64 iface_hash(const Iface *in, void *obj)
{
	DBusMsg *m = dbus_new_signal("/", "x.y", "z");
	props_add_all(m, in->props, in->n, obj);
	usize len = 0;
	u8 *bytes = dbus_msg_encode(m, 1, &len);
	u8 h[32];
	sha256(bytes, len, h);
	mem_free(bytes);
	dbus_msg_free(m);
	u64 v;
	memcpy(&v, h, 8);
	return v | 1;   /* never 0, which means "not announced yet" */
}

void iface_sync(DBus *bus, const char *path, const Iface *in, void *obj, u64 *hash)
{
	u64 h = iface_hash(in, obj);
	if (h == *hash)
		return;
	bool first = *hash == 0;
	*hash = h;
	if (first)
		return;
	DBusMsg *m = dbus_new_signal(path, "org.freedesktop.DBus.Properties", "PropertiesChanged");
	dbus_add(m, "s", in->iface);
	props_add_all(m, in->props, in->n, obj);
	dbus_open(m, "as");
	dbus_close(m);
	dbus_send(bus, m);
}

void iface_add(DBusMsg *m, const Iface *in, void *obj)
{
	dbus_open(m, "{sa{sv}}");
	dbus_add(m, "s", in->iface);
	props_add_all(m, in->props, in->n, obj);
	dbus_close(m);
}

void om_added(DBus *bus, const char *om_path, const char *path, const Iface *const *ifaces, usize n, void *obj)
{
	DBusMsg *m = dbus_new_signal(om_path, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded");
	dbus_add(m, "o", path);
	dbus_open(m, "a{sa{sv}}");
	for (usize i = 0; i < n; i++)
		iface_add(m, ifaces[i], obj);
	dbus_close(m);
	dbus_send(bus, m);
}

void om_removed(DBus *bus, const char *om_path, const char *path, const char *const *names, usize n)
{
	DBusMsg *m = dbus_new_signal(om_path, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved");
	dbus_add(m, "o", path);
	dbus_open(m, "as");
	for (usize i = 0; i < n; i++)
		dbus_add(m, "s", names[i]);
	dbus_close(m);
	dbus_send(bus, m);
}
