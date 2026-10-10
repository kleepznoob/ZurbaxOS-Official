/*
 * The computer itself: org.freedesktop.hostname1 (its name and what kind of
 * machine it is), org.freedesktop.locale1 (the system's language and
 * keyboard) and org.freedesktop.timedate1 (the clock and the time zone).
 *
 * Settings live in /data/system/system.conf, which the rest of Zurbax reads
 * too. The time zone is also the link /run/zurbax/localtime, which the
 * layer's /etc/localtime points to: the session's programs see a new zone
 * the moment it changes.
 */
#include "zsys.h"

#define HOSTNAME_PATH "/org/freedesktop/hostname1"
#define HOSTNAME_IF "org.freedesktop.hostname1"
#define LOCALE_PATH "/org/freedesktop/locale1"
#define LOCALE_IF "org.freedesktop.locale1"
#define TIMEDATE_PATH "/org/freedesktop/timedate1"
#define TIMEDATE_IF "org.freedesktop.timedate1"
#define LOCALTIME_LINK "/run/zurbax/localtime"

static Conf *sysconf(void)
{
	char *p = data_path("system.conf");
	Conf *c = conf_load(p);
	mem_free(p);
	return c;
}

static const char *cget(const char *key, const char *fallback, char *buf, usize cap)
{
	Conf *c = sysconf();
	str_copy(buf, conf_get(c, key, fallback), cap);
	conf_free(c);
	return buf;
}

static bool cset(const char *key, const char *value)
{
	Conf *c = sysconf();
	conf_set(c, key, value);
	bool ok = conf_save(c);
	conf_free(c);
	return ok;
}

static char *dmi(const char *name)
{
	char *p = str_fmt("/sys/class/dmi/id/%s", name);
	char *v = file_read_line(p);
	mem_free(p);
	if (!v)
		return str_dup("");
	/* what firmware makers write when they don't know */
	static const char *const junk[] = { "To Be Filled By O.E.M.", "System manufacturer", "System Product Name", "Default string",
					    "Not Applicable", "O.E.M.", "None" };
	for (usize i = 0; i < ARRAY_LEN(junk); i++)
		if (str_eq(v, junk[i]))
			v[0] = 0;
	return v;
}

/* ---- hostname1 --------------------------------------------------------------------------- */

static const char *chassis(void)
{
	char *t = dmi("chassis_type");
	int n = (int)str_to_int(t, NULL);
	mem_free(t);
	char *vendor = dmi("sys_vendor");
	bool vm = str_find(vendor, "QEMU") || str_find(vendor, "VMware") || str_find(vendor, "innotek") || str_find(vendor, "Microsoft") ||
		  str_find(vendor, "Xen") || str_find(vendor, "Parallels");
	mem_free(vendor);
	if (vm)
		return "vm";
	switch (n) {
	case 8: case 9: case 10: case 14: case 31: case 32: return "laptop";
	case 11: return "handset";
	case 30: return "tablet";
	case 17: case 23: case 25: case 28: case 29: return "server";
	case 3: case 4: case 5: case 6: case 7: case 13: case 15: case 16: case 35: case 36: return "desktop";
	}
	/* no word from the firmware: a battery means something to carry */
	return dir_exists("/sys/class/power_supply/BAT0") ? "laptop" : "desktop";
}

static const char *icon_for(const char *c)
{
	if (str_eq(c, "laptop"))
		return "computer-laptop";
	if (str_eq(c, "tablet"))
		return "computer-tablet";
	if (str_eq(c, "handset"))
		return "phone";
	if (str_eq(c, "vm"))
		return "computer-vm";
	if (str_eq(c, "server"))
		return "computer-server";
	return "computer";
}

/* 16 bytes from hex text, dashes skipped (machine ids, boot ids, UUIDs) */
static void id_bytes(const char *text, u8 out[16])
{
	char hex[33];
	usize n = 0;
	for (const char *c = text ? text : ""; *c && n < 32; c++)
		if (*c != '-')
			hex[n++] = *c;
	hex[n] = 0;
	memset(out, 0, 16);
	if (n == 32)
		hex_decode(hex, out, 16);
}

static void machine_id(u8 out[16])
{
	char *idpath = str_fmt("%s/.local/share/zos/machine-id", Z.home);
	char *id = file_read_line(idpath);
	mem_free(idpath);
	id_bytes(id, out);
	mem_free(id);
}

static void add_bytes(DBusMsg *m, const u8 *b, usize n)
{
	dbus_open(m, "ay");
	for (usize i = 0; i < n; i++)
		dbus_add(m, "y", b[i]);
	dbus_close(m);
}

#define GET(name) static void name(DBusMsg *m, void *obj)
GET(h_hostname) { (void)obj; struct utsname u; sys_uname(&u); dbus_add(m, "s", u.nodename); }
GET(h_static) { (void)obj; char b[80]; dbus_add(m, "s", cget("hostname", "zurbax", b, sizeof b)); }
GET(h_pretty) { (void)obj; char b[128]; dbus_add(m, "s", cget("pretty_hostname", "", b, sizeof b)); }
GET(h_default) { (void)obj; dbus_add(m, "s", "zurbax"); }
GET(h_source) { (void)obj; dbus_add(m, "s", "static"); }
GET(h_icon) { (void)obj; dbus_add(m, "s", icon_for(chassis())); }
GET(h_chassis) { (void)obj; dbus_add(m, "s", chassis()); }
GET(h_empty) { (void)obj; dbus_add(m, "s", ""); }
GET(h_kernel_name) { (void)obj; struct utsname u; sys_uname(&u); dbus_add(m, "s", u.sysname); }
GET(h_kernel_release) { (void)obj; struct utsname u; sys_uname(&u); dbus_add(m, "s", u.release); }
GET(h_kernel_version) { (void)obj; struct utsname u; sys_uname(&u); dbus_add(m, "s", u.version); }
GET(h_os) { (void)obj; dbus_add(m, "s", OS_PRETTY_NAME); }
GET(h_os_url) { (void)obj; dbus_add(m, "s", "https://github.com/kleepznoob/i-really-wanna-impress-me-parents-by-making-money"); }
GET(h_zero_t) { (void)obj; dbus_add(m, "t", (u64)0); }
GET(h_os_data)
{
	(void)obj;
	dbus_open(m, "as");
	dbus_add(m, "s", "NAME=" OS_NAME);
	dbus_add(m, "s", "VERSION_ID=" OS_VERSION);
	dbus_add(m, "s", "PRETTY_NAME=" OS_PRETTY_NAME);
	dbus_add(m, "s", "ID=zurbax");
	dbus_close(m);
}
GET(h_empty_as) { (void)obj; dbus_open(m, "as"); dbus_close(m); }
GET(h_vendor) { (void)obj; char *v = dmi("sys_vendor"); dbus_add(m, "s", v); mem_free(v); }
GET(h_model)
{
	(void)obj;
	char *v = dmi("product_name"), *ver = dmi("product_version");
	/* Lenovo puts the marketing name in the version */
	char *vendor = dmi("sys_vendor");
	dbus_add(m, "s", str_find(vendor, "LENOVO") && ver[0] ? ver : v);
	mem_free(vendor);
	mem_free(v);
	mem_free(ver);
}
GET(h_sku) { (void)obj; char *v = dmi("product_sku"); dbus_add(m, "s", v); mem_free(v); }
GET(h_hw_version) { (void)obj; char *v = dmi("product_version"); dbus_add(m, "s", v); mem_free(v); }
GET(h_fw_version) { (void)obj; char *v = dmi("bios_version"); dbus_add(m, "s", v); mem_free(v); }
GET(h_fw_vendor) { (void)obj; char *v = dmi("bios_vendor"); dbus_add(m, "s", v); mem_free(v); }
GET(h_fw_date)
{
	(void)obj;
	/* "MM/DD/YYYY" in µs since 1970 */
	char *v = dmi("bios_date");
	u64 t = 0;
	if (strlen(v) == 10) {
		Civil c = { 0 };
		c.month = (int)str_to_int(v, NULL);
		c.day = (int)str_to_int(v + 3, NULL);
		c.year = (int)str_to_int(v + 6, NULL);
		t = (u64)unix_from_civil(&c) * 1000000;
	}
	mem_free(v);
	dbus_add(m, "t", t);
}
GET(h_machine_id) { (void)obj; u8 id[16]; machine_id(id); add_bytes(m, id, 16); }
GET(h_boot_id)
{
	(void)obj;
	char *v = file_read_line("/proc/sys/kernel/random/boot_id");
	u8 id[16];
	id_bytes(v, id);
	mem_free(v);
	add_bytes(m, id, 16);
}
GET(h_vsock) { (void)obj; dbus_add(m, "u", 0xffffffffu); }

static const Prop HOSTNAME_PROPS[] = {
	{ "Hostname", "s", h_hostname, NULL },
	{ "StaticHostname", "s", h_static, NULL },
	{ "PrettyHostname", "s", h_pretty, NULL },
	{ "DefaultHostname", "s", h_default, NULL },
	{ "HostnameSource", "s", h_source, NULL },
	{ "IconName", "s", h_icon, NULL },
	{ "Chassis", "s", h_chassis, NULL },
	{ "Deployment", "s", h_empty, NULL },
	{ "Location", "s", h_empty, NULL },
	{ "KernelName", "s", h_kernel_name, NULL },
	{ "KernelRelease", "s", h_kernel_release, NULL },
	{ "KernelVersion", "s", h_kernel_version, NULL },
	{ "OperatingSystemPrettyName", "s", h_os, NULL },
	{ "OperatingSystemCPEName", "s", h_empty, NULL },
	{ "OperatingSystemHomeURL", "s", h_os_url, NULL },
	{ "OperatingSystemSupportEnd", "t", h_zero_t, NULL },
	{ "OperatingSystemReleaseData", "as", h_os_data, NULL },
	{ "MachineInformationData", "as", h_empty_as, NULL },
	{ "HardwareVendor", "s", h_vendor, NULL },
	{ "HardwareModel", "s", h_model, NULL },
	{ "HardwareSKU", "s", h_sku, NULL },
	{ "HardwareVersion", "s", h_hw_version, NULL },
	{ "FirmwareVersion", "s", h_fw_version, NULL },
	{ "FirmwareVendor", "s", h_fw_vendor, NULL },
	{ "FirmwareDate", "t", h_fw_date, NULL },
	{ "ChassisAssetTag", "s", h_empty, NULL },
	{ "MachineID", "ay", h_machine_id, NULL },
	{ "BootID", "ay", h_boot_id, NULL },
	{ "VSockCID", "u", h_vsock, NULL },
};

/* A host name the network accepts: letters, digits and hyphens, 63 at most. */
static bool valid_hostname(const char *s)
{
	usize n = strlen(s);
	if (!n || n > 63 || s[0] == '-' || s[n - 1] == '-')
		return false;
	for (usize i = 0; i < n; i++)
		if (!is_alnum((u8)s[i]) && s[i] != '-')
			return false;
	return true;
}

static void hostname_changed(const char *a, const char *b)
{
	const char *names[] = { a, b, NULL };
	props_changed(Z.bus, HOSTNAME_PATH, HOSTNAME_IF, HOSTNAME_PROPS, ARRAY_LEN(HOSTNAME_PROPS), NULL, names);
}

static bool set_static(const char *name)
{
	if (!valid_hostname(name))
		return false;
	if (!cset("hostname", name))
		return false;
	sys_sethostname(name, strlen(name));
	return true;
}

static bool hostname_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, HOSTNAME_IF, HOSTNAME_PROPS, ARRAY_LEN(HOSTNAME_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call), *arg = dbus_str(dbus_arg(call, 0), "");
	if (str_eq(mbr, "SetHostname") || str_eq(mbr, "SetStaticHostname")) {
		if (!set_static(arg)) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs", "Use letters, digits and hyphens");
			return true;
		}
		dbus_reply_empty(bus, call);
		hostname_changed("Hostname", "StaticHostname");
	} else if (str_eq(mbr, "SetPrettyHostname")) {
		if (str_chr(arg, '\n') || strlen(arg) > 120) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs", "That name is too long");
			return true;
		}
		cset("pretty_hostname", arg);
		dbus_reply_empty(bus, call);
		hostname_changed("PrettyHostname", NULL);
	} else if (str_eq(mbr, "SetIconName") || str_eq(mbr, "SetChassis") || str_eq(mbr, "SetDeployment") ||
		   str_eq(mbr, "SetLocation")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "GetProductUUID")) {
		char *v = dmi("product_uuid");
		u8 id[16];
		id_bytes(v, id);
		mem_free(v);
		DBusMsg *m = dbus_new_reply(call);
		add_bytes(m, id, 16);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "GetHardwareSerial")) {
		char *v = dmi("product_serial");
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "s", v);
		dbus_send(bus, m);
		mem_free(v);
	} else if (str_eq(mbr, "Describe")) {
		struct utsname u;
		sys_uname(&u);
		char b[128];
		StrBuf sb = { 0 };
		sb_adds(&sb, "{\"Hostname\":");
		sb_json_string(&sb, u.nodename);
		sb_adds(&sb, ",\"PrettyHostname\":");
		sb_json_string(&sb, cget("pretty_hostname", "", b, sizeof b));
		sb_adds(&sb, ",\"Chassis\":");
		sb_json_string(&sb, chassis());
		sb_adds(&sb, ",\"OperatingSystemPrettyName\":\"" OS_PRETTY_NAME "\"}");
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "s", sb_str(&sb));
		dbus_send(bus, m);
		sb_free(&sb);
	} else {
		return false;
	}
	return true;
}

/* ---- locale1 -------------------------------------------------------------------------- */

GET(l_locale)
{
	(void)obj;
	char lang[64], fmt_[64];
	cget("language", "en_US.UTF-8", lang, sizeof lang);
	cget("formats", "", fmt_, sizeof fmt_);
	dbus_open(m, "as");
	char kv[96];
	fmt(kv, sizeof kv, "LANG=%s", lang);
	dbus_add(m, "s", kv);
	if (fmt_[0]) {
		static const char *const cats[] = { "LC_NUMERIC", "LC_TIME", "LC_MONETARY", "LC_MEASUREMENT", "LC_PAPER" };
		for (usize i = 0; i < ARRAY_LEN(cats); i++) {
			fmt(kv, sizeof kv, "%s=%s", cats[i], fmt_);
			dbus_add(m, "s", kv);
		}
	}
	dbus_close(m);
}
GET(l_keymap) { (void)obj; char b[32]; dbus_add(m, "s", cget("keyboard_layout", "us", b, sizeof b)); }
GET(l_layout) { (void)obj; char b[64]; dbus_add(m, "s", cget("keyboard_layout", "us", b, sizeof b)); }
GET(l_model) { (void)obj; dbus_add(m, "s", "pc105"); }
GET(l_variant) { (void)obj; char b[64]; dbus_add(m, "s", cget("keyboard_variant", "", b, sizeof b)); }
GET(l_options) { (void)obj; char b[128]; dbus_add(m, "s", cget("keyboard_options", "", b, sizeof b)); }

static const Prop LOCALE_PROPS[] = {
	{ "Locale", "as", l_locale, NULL },
	{ "VConsoleKeymap", "s", l_keymap, NULL },
	{ "VConsoleKeymapToggle", "s", h_empty, NULL },
	{ "X11Layout", "s", l_layout, NULL },
	{ "X11Model", "s", l_model, NULL },
	{ "X11Variant", "s", l_variant, NULL },
	{ "X11Options", "s", l_options, NULL },
};

static bool safe_word(const char *s)
{
	for (; *s; s++)
		if (!is_alnum((u8)*s) && !str_chr("_-.@,:+", *s))
			return false;
	return true;
}

static bool locale_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, LOCALE_IF, LOCALE_PROPS, ARRAY_LEN(LOCALE_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "SetLocale")) {
		usize n = 0;
		const char **kv = dbus_strv(dbus_arg(call, 0), &n);
		for (usize i = 0; i < n; i++) {
			const char *eq = str_chr(kv[i], '=');
			if (!eq || !safe_word(eq + 1))
				continue;
			if (str_starts(kv[i], "LANG="))
				cset("language", eq + 1);
			else if (str_starts(kv[i], "LC_TIME=") || str_starts(kv[i], "LC_NUMERIC="))
				cset("formats", eq + 1);
		}
		mem_free(kv);
		dbus_reply_empty(bus, call);
		const char *names[] = { "Locale", NULL };
		props_changed(bus, LOCALE_PATH, LOCALE_IF, LOCALE_PROPS, ARRAY_LEN(LOCALE_PROPS), NULL, names);
	} else if (str_eq(mbr, "SetX11Keyboard") || str_eq(mbr, "SetVConsoleKeyboard")) {
		const char *layout = dbus_str(dbus_arg(call, 0), "");
		if (!*layout || !safe_word(layout)) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs", "No such keyboard layout");
			return true;
		}
		cset("keyboard_layout", layout);
		if (str_eq(mbr, "SetX11Keyboard")) {
			const char *variant = dbus_str(dbus_arg(call, 2), ""), *options = dbus_str(dbus_arg(call, 3), "");
			if (safe_word(variant))
				cset("keyboard_variant", variant);
			if (safe_word(options))
				cset("keyboard_options", options);
		}
		dbus_reply_empty(bus, call);
		const char *names[] = { "X11Layout", "X11Variant", "X11Options", "VConsoleKeymap", NULL };
		props_changed(bus, LOCALE_PATH, LOCALE_IF, LOCALE_PROPS, ARRAY_LEN(LOCALE_PROPS), NULL, names);
	} else {
		return false;
	}
	return true;
}

/* ---- timedate1 -------------------------------------------------------------------------- */

/* The zone's file in the layer, as the session sees it. */
static void link_localtime(const char *zone)
{
	char *target = str_fmt("/usr/share/zoneinfo/%s", zone);
	char *tmp = str_fmt("%s.new", LOCALTIME_LINK);
	dir_make("/run/zurbax", 0755);
	sys_unlink(tmp);
	if (sys_symlink(target, tmp) == 0)
		sys_rename(tmp, LOCALTIME_LINK);
	mem_free(tmp);
	mem_free(target);
}

static char zone_now[64];

GET(t_zone) { (void)obj; dbus_add(m, "s", zone_now); }
GET(t_ntp) { (void)obj; char b[8]; dbus_add(m, "b", !str_eq(cget("ntp", "true", b, sizeof b), "false")); }
GET(t_synced) { (void)obj; dbus_add(m, "b", file_exists("/run/zurbax/time.synced")); }
GET(t_now) { (void)obj; dbus_add(m, "t", (u64)time_unix_ms() * 1000); }
GET(t_true) { (void)obj; dbus_add(m, "b", true); }
GET(t_false) { (void)obj; dbus_add(m, "b", false); }

static const Prop TIMEDATE_PROPS[] = {
	{ "Timezone", "s", t_zone, NULL },
	{ "LocalRTC", "b", t_false, NULL },
	{ "CanNTP", "b", t_true, NULL },
	{ "NTP", "b", t_ntp, NULL },
	{ "NTPSynchronized", "b", t_synced, NULL },
	{ "TimeUSec", "t", t_now, NULL },
	{ "RTCTimeUSec", "t", t_now, NULL },
};

/* The layer's time zone database (what the session's clocks read), or Zurbax's own list. */
static bool zone_exists(const char *zone)
{
	if (!*zone || str_find(zone, "..") || zone[0] == '/')
		return false;
	char *p = str_fmt("%s/usr/share/zoneinfo/%s", layer_root(), zone);
	bool ok = file_exists(p) && !dir_exists(p);
	mem_free(p);
	return ok || tz_find(zone) != NULL || str_eq(zone, "UTC");
}

/* Every zone, from the database's zone1970.tab (columns: countries, place, zone). */
static void add_zones(DBusMsg *m)
{
	char *p = str_fmt("%s/usr/share/zoneinfo/zone1970.tab", layer_root());
	char *text = file_read(p, NULL);
	mem_free(p);
	dbus_open(m, "as");
	if (text) {
		dbus_add(m, "s", "UTC");
		usize n = 0;
		char **lines = str_split(text, '\n', &n);
		for (usize i = 0; i < n; i++) {
			if (lines[i][0] == '#' || !lines[i][0])
				continue;
			usize nf = 0;
			char **f = str_split(lines[i], '\t', &nf);
			if (nf >= 3)
				dbus_add(m, "s", f[2]);
			str_split_free(f, nf);
		}
		str_split_free(lines, n);
		mem_free(text);
	} else {
		usize n = 0;
		const ZoneInfo *zones = tz_zones(&n);
		for (usize i = 0; i < n; i++)
			dbus_add(m, "s", zones[i].zone);
	}
	dbus_close(m);
}

static bool set_zone(const char *zone)
{
	if (!zone_exists(zone))
		return false;
	cset("timezone", zone);
	/* the person's own Zurbax setting follows, so both desktops agree */
	char *p = str_fmt("%s/.config/zurbax/settings.conf", Z.home);
	Conf *c = conf_load(p);
	if (conf_get(c, "timezone", NULL)) {
		conf_set(c, "timezone", zone);
		conf_save(c);
	}
	conf_free(c);
	mem_free(p);
	str_copy(zone_now, zone, sizeof zone_now);
	link_localtime(zone);
	return true;
}

static bool timedate_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, TIMEDATE_IF, TIMEDATE_PROPS, ARRAY_LEN(TIMEDATE_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "SetTimezone")) {
		if (!set_zone(dbus_str(dbus_arg(call, 0), ""))) {
			dbus_reply_error(bus, call, "org.freedesktop.timedate1.InvalidTimezone", "No such time zone");
			return true;
		}
		dbus_reply_empty(bus, call);
		const char *names[] = { "Timezone", NULL };
		props_changed(bus, TIMEDATE_PATH, TIMEDATE_IF, TIMEDATE_PROPS, ARRAY_LEN(TIMEDATE_PROPS), NULL, names);
	} else if (str_eq(mbr, "SetNTP")) {
		cset("ntp", dbus_bool(dbus_arg(call, 0), true) ? "true" : "false");
		dbus_reply_empty(bus, call);
		const char *names[] = { "NTP", NULL };
		props_changed(bus, TIMEDATE_PATH, TIMEDATE_IF, TIMEDATE_PROPS, ARRAY_LEN(TIMEDATE_PROPS), NULL, names);
	} else if (str_eq(mbr, "SetTime")) {
		i64 us = dbus_int(dbus_arg(call, 0), 0);
		bool relative = dbus_bool(dbus_arg(call, 1), false);
		i64 now_us = time_unix_ms() * 1000;
		i64 t = relative ? now_us + us : us;
		struct timespec ts = { t / 1000000, (t % 1000000) * 1000 };
		if (sys_clock_settime(CLOCK_REALTIME, &ts) < 0) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.Failed", "The clock refused the time");
			return true;
		}
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "SetLocalRTC")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "ListTimezones")) {
		DBusMsg *m = dbus_new_reply(call);
		add_zones(m);
		dbus_send(bus, m);
	} else {
		return false;
	}
	return true;
}

static const char HOSTNAME_XML[] =
	"<interface name=\"org.freedesktop.hostname1\">"
	"<method name=\"SetHostname\"><arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"SetStaticHostname\"><arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"SetPrettyHostname\"><arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"GetProductUUID\"><arg type=\"b\" direction=\"in\"/><arg type=\"ay\" direction=\"out\"/></method>"
	"<method name=\"Describe\"><arg type=\"s\" direction=\"out\"/></method>"
	"</interface>";

static const char LOCALE_XML[] =
	"<interface name=\"org.freedesktop.locale1\">"
	"<method name=\"SetLocale\"><arg type=\"as\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"SetVConsoleKeyboard\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"b\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"SetX11Keyboard\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"</interface>";

static const char TIMEDATE_XML[] =
	"<interface name=\"org.freedesktop.timedate1\">"
	"<method name=\"SetTime\"><arg type=\"x\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"SetTimezone\"><arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"SetLocalRTC\"><arg type=\"b\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"SetNTP\"><arg type=\"b\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/></method>"
	"<method name=\"ListTimezones\"><arg type=\"as\" direction=\"out\"/></method>"
	"</interface>";

void hostname_start(DBus *bus)
{
	char zone[64];
	cget("timezone", "UTC", zone, sizeof zone);
	str_copy(zone_now, zone_exists(zone) ? zone : "UTC", sizeof zone_now);
	link_localtime(zone_now);
	dbus_export(bus, HOSTNAME_PATH, HOSTNAME_IF, hostname_method, NULL, HOSTNAME_XML);
	dbus_export(bus, LOCALE_PATH, LOCALE_IF, locale_method, NULL, LOCALE_XML);
	dbus_export(bus, TIMEDATE_PATH, TIMEDATE_IF, timedate_method, NULL, TIMEDATE_XML);
	dbus_request_name(bus, HOSTNAME_IF, DBUS_NAME_NO_QUEUE);
	dbus_request_name(bus, LOCALE_IF, DBUS_NAME_NO_QUEUE);
	dbus_request_name(bus, TIMEDATE_IF, DBUS_NAME_NO_QUEUE);
}
