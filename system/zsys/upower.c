/*
 * org.freedesktop.UPower: batteries and the charger, from the kernel's
 * power supply class; and power profiles (org.freedesktop.UPower.PowerProfiles,
 * also under its first name net.hadess.PowerProfiles): power saving,
 * balanced and performance, set through the firmware's platform profile
 * and the processors' energy preference.
 */
#include "zsys.h"

#define UPOWER "org.freedesktop.UPower"
#define UPOWER_PATH "/org/freedesktop/UPower"
#define DEVICE_IF "org.freedesktop.UPower.Device"
#define DISPLAY_PATH "/org/freedesktop/UPower/devices/DisplayDevice"
#define PROFILES_IF "org.freedesktop.UPower.PowerProfiles"
#define PROFILES_PATH "/org/freedesktop/UPower/PowerProfiles"
#define HADESS_IF "net.hadess.PowerProfiles"
#define HADESS_PATH "/net/hadess/PowerProfiles"

enum { TYPE_UNKNOWN, TYPE_LINE_POWER, TYPE_BATTERY, TYPE_UPS };
enum { ST_UNKNOWN, ST_CHARGING, ST_DISCHARGING, ST_EMPTY, ST_FULL, ST_PENDING_CHARGE, ST_PENDING_DISCHARGE };
enum { WARN_UNKNOWN, WARN_NONE, WARN_DISCHARGING, WARN_LOW, WARN_CRITICAL, WARN_ACTION };
enum { TECH_UNKNOWN, TECH_LI_ION, TECH_LI_POLY, TECH_LIFEPO4, TECH_LEAD, TECH_NICD, TECH_NIMH };

typedef struct {
	char name[48];          /* the kernel's: "BAT0", "AC" */
	char path[128];         /* D-Bus object */
	u32 type;
	bool online, present, rechargeable;
	f64 energy, energy_full, energy_design, rate, voltage, percentage, capacity, temperature;
	i64 to_empty, to_full;
	u32 state, technology, warning;
	i32 cycles;
	char vendor[64], model[64], serial[64];
	u64 updated;
	bool threshold_supported, threshold_enabled;
	u32 start_threshold, end_threshold;
	bool used;
} Supply;

static Supply supplies[8];
static Supply display;     /* all the batteries as one */
static bool on_battery;

/* ---- Reading the kernel -------------------------------------------------------------------- */

static char *attr(const char *name, const char *a)
{
	char *p = str_fmt("/sys/class/power_supply/%s/%s", name, a);
	char *v = file_read_line(p);
	mem_free(p);
	return v;
}

static f64 attr_num(const char *name, const char *a, f64 scale, f64 fallback)
{
	char *v = attr(name, a);
	f64 r = v && *v ? (f64)str_to_int(v, NULL) * scale : fallback;
	mem_free(v);
	return r;
}

static void attr_str(const char *name, const char *a, char *out, usize cap)
{
	char *v = attr(name, a);
	str_copy(out, v ? v : "", cap);
	mem_free(v);
}

static u32 icon_level(f64 pct)
{
	return (u32)(pct / 10) * 10;
}

static void read_supply(Supply *s)
{
	char type[32];
	attr_str(s->name, "type", type, sizeof type);
	s->type = str_eq(type, "Battery") ? TYPE_BATTERY : str_eq(type, "UPS") ? TYPE_UPS : TYPE_LINE_POWER;
	s->updated = (u64)time_unix();
	if (s->type == TYPE_LINE_POWER) {
		s->online = attr_num(s->name, "online", 1, 0) > 0;
		s->present = true;
		return;
	}
	/* a battery that belongs to a mouse or a game pad isn't the computer's */
	char scope[32];
	attr_str(s->name, "scope", scope, sizeof scope);
	s->present = attr_num(s->name, "present", 1, 1) > 0;
	s->rechargeable = true;
	attr_str(s->name, "manufacturer", s->vendor, sizeof s->vendor);
	attr_str(s->name, "model_name", s->model, sizeof s->model);
	attr_str(s->name, "serial_number", s->serial, sizeof s->serial);
	s->voltage = attr_num(s->name, "voltage_now", 1e-6, 0);
	f64 volts = s->voltage > 0 ? s->voltage : attr_num(s->name, "voltage_min_design", 1e-6, 12);
	/* energy in Wh: from energy_* (µWh), or charge_* (µAh) times the voltage */
	s->energy = attr_num(s->name, "energy_now", 1e-6, -1);
	if (s->energy >= 0) {
		s->energy_full = attr_num(s->name, "energy_full", 1e-6, 0);
		s->energy_design = attr_num(s->name, "energy_full_design", 1e-6, 0);
		s->rate = attr_num(s->name, "power_now", 1e-6, 0);
	} else {
		s->energy = attr_num(s->name, "charge_now", 1e-6, 0) * volts;
		s->energy_full = attr_num(s->name, "charge_full", 1e-6, 0) * volts;
		s->energy_design = attr_num(s->name, "charge_full_design", 1e-6, 0) * volts;
		s->rate = attr_num(s->name, "current_now", 1e-6, 0) * volts;
	}
	if (s->rate < 0)
		s->rate = -s->rate;
	f64 cap = attr_num(s->name, "capacity", 1, -1);
	s->percentage = cap >= 0 ? cap : s->energy_full > 0 ? s->energy / s->energy_full * 100 : 0;
	s->percentage = clampf((f32)s->percentage, 0, 100);
	s->capacity = s->energy_design > 0 ? clampf((f32)(s->energy_full / s->energy_design * 100), 0, 100) : 100;
	s->temperature = attr_num(s->name, "temp", 0.1, 0);
	s->cycles = (i32)attr_num(s->name, "cycle_count", 1, -1);
	char status[32], tech[32];
	attr_str(s->name, "status", status, sizeof status);
	attr_str(s->name, "technology", tech, sizeof tech);
	s->state = str_eq(status, "Charging") ? ST_CHARGING : str_eq(status, "Discharging") ? ST_DISCHARGING
		 : str_eq(status, "Full") ? ST_FULL : str_eq(status, "Not charging") ? ST_PENDING_CHARGE : ST_UNKNOWN;
	if (s->state == ST_UNKNOWN && s->percentage >= 99)
		s->state = ST_FULL;
	s->technology = str_eq(tech, "Li-ion") ? TECH_LI_ION : str_eq(tech, "Li-poly") ? TECH_LI_POLY
		      : str_eq(tech, "LiFe") ? TECH_LIFEPO4 : str_eq(tech, "NiCd") ? TECH_NICD
		      : str_eq(tech, "NiMH") ? TECH_NIMH : TECH_UNKNOWN;
	s->to_empty = s->state == ST_DISCHARGING && s->rate > 0.1 ? (i64)(s->energy / s->rate * 3600) : 0;
	s->to_full = s->state == ST_CHARGING && s->rate > 0.1 ? (i64)((s->energy_full - s->energy) / s->rate * 3600) : 0;
	s->warning = s->state != ST_DISCHARGING ? WARN_NONE : s->percentage <= 2 ? WARN_ACTION
		   : s->percentage <= 5 ? WARN_CRITICAL : s->percentage <= 10 ? WARN_LOW : WARN_NONE;
	s->threshold_supported = attr_num(s->name, "charge_control_end_threshold", 1, -1) >= 0;
	s->start_threshold = (u32)attr_num(s->name, "charge_control_start_threshold", 1, 0);
	s->end_threshold = (u32)attr_num(s->name, "charge_control_end_threshold", 1, 100);
	s->threshold_enabled = s->threshold_supported && s->end_threshold < 100;
}

/* The display device: what the top bar shows, every battery together. */
static void compose_display(void)
{
	Supply d = { 0 };
	str_copy(d.path, DISPLAY_PATH, sizeof d.path);
	d.type = TYPE_BATTERY;
	d.updated = (u64)time_unix();
	int n = 0, charging = 0, discharging = 0, full = 0;
	for (usize i = 0; i < ARRAY_LEN(supplies); i++) {
		Supply *s = &supplies[i];
		if (!s->used || s->type != TYPE_BATTERY || !s->present)
			continue;
		n++;
		d.energy += s->energy;
		d.energy_full += s->energy_full;
		d.energy_design += s->energy_design;
		d.rate += s->rate;
		charging += s->state == ST_CHARGING;
		discharging += s->state == ST_DISCHARGING;
		full += s->state == ST_FULL;
		if (n == 1)
			d.percentage = s->percentage;
	}
	d.present = n > 0;
	d.rechargeable = n > 0;
	if (n > 1 && d.energy_full > 0)
		d.percentage = d.energy / d.energy_full * 100;
	d.state = !n ? ST_UNKNOWN : charging ? ST_CHARGING : discharging ? ST_DISCHARGING : full == n ? ST_FULL : ST_PENDING_CHARGE;
	d.to_empty = d.state == ST_DISCHARGING && d.rate > 0.1 ? (i64)(d.energy / d.rate * 3600) : 0;
	d.to_full = d.state == ST_CHARGING && d.rate > 0.1 ? (i64)((d.energy_full - d.energy) / d.rate * 3600) : 0;
	d.warning = d.state != ST_DISCHARGING ? WARN_NONE : d.percentage <= 2 ? WARN_ACTION
		  : d.percentage <= 5 ? WARN_CRITICAL : d.percentage <= 10 ? WARN_LOW : WARN_NONE;
	d.used = true;
	display = d;
}

/* ---- Properties ------------------------------------------------------------------------------ */

#define GET(name) static void name(DBusMsg *m, void *obj)
#define S ((Supply *)obj)
GET(d_native) { dbus_add(m, "s", S->name); }
GET(d_vendor) { dbus_add(m, "s", S->vendor); }
GET(d_model) { dbus_add(m, "s", S->model); }
GET(d_serial) { dbus_add(m, "s", S->serial); }
GET(d_updated) { dbus_add(m, "t", S->updated); }
GET(d_type) { dbus_add(m, "u", S->type); }
GET(d_supply) { dbus_add(m, "b", S->type == TYPE_LINE_POWER || (S->type == TYPE_BATTERY && S != &display) || S == &display); }
GET(d_false) { (void)obj; dbus_add(m, "b", false); }
GET(d_online) { dbus_add(m, "b", S->online); }
GET(d_energy) { dbus_add(m, "d", S->energy); }
GET(d_zero) { (void)obj; dbus_add(m, "d", 0.0); }
GET(d_energy_full) { dbus_add(m, "d", S->energy_full); }
GET(d_energy_design) { dbus_add(m, "d", S->energy_design); }
GET(d_rate) { dbus_add(m, "d", S->rate); }
GET(d_voltage) { dbus_add(m, "d", S->voltage); }
GET(d_cycles) { dbus_add(m, "i", S->cycles); }
GET(d_to_empty) { dbus_add(m, "x", S->to_empty); }
GET(d_to_full) { dbus_add(m, "x", S->to_full); }
GET(d_percentage) { dbus_add(m, "d", S->percentage); }
GET(d_temperature) { dbus_add(m, "d", S->temperature); }
GET(d_present) { dbus_add(m, "b", S->present); }
GET(d_state) { dbus_add(m, "u", S->state); }
GET(d_rechargeable) { dbus_add(m, "b", S->rechargeable); }
GET(d_capacity) { dbus_add(m, "d", S->capacity); }
GET(d_technology) { dbus_add(m, "u", S->technology); }
GET(d_warning) { dbus_add(m, "u", S->warning); }
GET(d_level) { (void)obj; dbus_add(m, "u", 1u); }
GET(d_icon)
{
	char icon[64];
	if (S->type == TYPE_LINE_POWER)
		str_copy(icon, "ac-adapter-symbolic", sizeof icon);
	else if (!S->present)
		str_copy(icon, S == &display ? "" : "battery-missing-symbolic", sizeof icon);
	else if (S->state == ST_FULL)
		str_copy(icon, "battery-level-100-charged-symbolic", sizeof icon);
	else
		fmt(icon, sizeof icon, "battery-level-%u%s-symbolic", icon_level(S->percentage), S->state == ST_CHARGING ? "-charging" : "");
	dbus_add(m, "s", icon);
}
GET(d_start_threshold) { dbus_add(m, "u", S->start_threshold); }
GET(d_end_threshold) { dbus_add(m, "u", S->end_threshold); }
GET(d_threshold_enabled) { dbus_add(m, "b", S->threshold_enabled); }
GET(d_threshold_supported) { dbus_add(m, "b", S->threshold_supported); }
GET(d_threshold_settings) { dbus_add(m, "u", S->threshold_supported ? 3u : 0u); }
GET(d_capacity_level) { (void)obj; dbus_add(m, "s", ""); }
#undef S

static const Prop DEVICE_PROPS[] = {
	{ "NativePath", "s", d_native, NULL },
	{ "Vendor", "s", d_vendor, NULL },
	{ "Model", "s", d_model, NULL },
	{ "Serial", "s", d_serial, NULL },
	{ "UpdateTime", "t", d_updated, NULL },
	{ "Type", "u", d_type, NULL },
	{ "PowerSupply", "b", d_supply, NULL },
	{ "HasHistory", "b", d_false, NULL },
	{ "HasStatistics", "b", d_false, NULL },
	{ "Online", "b", d_online, NULL },
	{ "Energy", "d", d_energy, NULL },
	{ "EnergyEmpty", "d", d_zero, NULL },
	{ "EnergyFull", "d", d_energy_full, NULL },
	{ "EnergyFullDesign", "d", d_energy_design, NULL },
	{ "EnergyRate", "d", d_rate, NULL },
	{ "Voltage", "d", d_voltage, NULL },
	{ "ChargeCycles", "i", d_cycles, NULL },
	{ "Luminosity", "d", d_zero, NULL },
	{ "TimeToEmpty", "x", d_to_empty, NULL },
	{ "TimeToFull", "x", d_to_full, NULL },
	{ "Percentage", "d", d_percentage, NULL },
	{ "Temperature", "d", d_temperature, NULL },
	{ "IsPresent", "b", d_present, NULL },
	{ "State", "u", d_state, NULL },
	{ "IsRechargeable", "b", d_rechargeable, NULL },
	{ "Capacity", "d", d_capacity, NULL },
	{ "Technology", "u", d_technology, NULL },
	{ "WarningLevel", "u", d_warning, NULL },
	{ "BatteryLevel", "u", d_level, NULL },
	{ "IconName", "s", d_icon, NULL },
	{ "ChargeStartThreshold", "u", d_start_threshold, NULL },
	{ "ChargeEndThreshold", "u", d_end_threshold, NULL },
	{ "ChargeThresholdEnabled", "b", d_threshold_enabled, NULL },
	{ "ChargeThresholdSupported", "b", d_threshold_supported, NULL },
	{ "ChargeThresholdSettingsSupported", "u", d_threshold_settings, NULL },
	{ "VoltageMinDesign", "d", d_zero, NULL },
	{ "VoltageMaxDesign", "d", d_zero, NULL },
	{ "CapacityLevel", "s", d_capacity_level, NULL },
};

GET(u_version) { (void)obj; dbus_add(m, "s", "1.91.0"); }
GET(u_on_battery) { (void)obj; dbus_add(m, "b", on_battery); }
GET(u_lid_present) { (void)obj; dbus_add(m, "b", dir_exists("/proc/acpi/button/lid")); }
GET(u_lid_closed)
{
	(void)obj;
	DirList l = { 0 };
	bool closed = false;
	if (dir_list("/proc/acpi/button/lid", &l, false)) {
		for (usize i = 0; i < l.len; i++) {
			char *p = str_fmt("/proc/acpi/button/lid/%s/state", l.data[i].name);
			char *t = file_read(p, NULL);
			closed |= t && str_find(t, "closed");
			mem_free(t);
			mem_free(p);
		}
		dir_list_free(&l);
	}
	dbus_add(m, "b", closed);
}

static const Prop UPOWER_PROPS[] = {
	{ "DaemonVersion", "s", u_version, NULL },
	{ "OnBattery", "b", u_on_battery, NULL },
	{ "LidIsClosed", "b", u_lid_closed, NULL },
	{ "LidIsPresent", "b", u_lid_present, NULL },
};

/* ---- Methods ------------------------------------------------------------------------------- */

static void scan(bool announce);

static bool set_threshold(Supply *s, bool on)
{
	if (!s->threshold_supported)
		return false;
	char *p = str_fmt("/sys/class/power_supply/%s/charge_control_end_threshold", s->name);
	bool ok = file_write_str(p, on ? "80" : "100");
	mem_free(p);
	if (ok) {
		p = str_fmt("/sys/class/power_supply/%s/charge_control_start_threshold", s->name);
		file_write_str(p, on ? "75" : "0");
		mem_free(p);
	}
	return ok;
}

static bool device_method(DBus *bus, DBusMsg *call, void *data)
{
	Supply *s = data;
	if (props_call(bus, call, DEVICE_IF, DEVICE_PROPS, ARRAY_LEN(DEVICE_PROPS), s))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "Refresh")) {
		scan(true);
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "GetHistory") || str_eq(mbr, "GetStatistics")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, str_eq(mbr, "GetHistory") ? "a(udu)" : "a(dd)");
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "EnableChargeThreshold")) {
		if (set_threshold(s, dbus_bool(dbus_arg(call, 0), false))) {
			dbus_reply_empty(bus, call);
			scan(true);
		} else {
			dbus_reply_error(bus, call, "org.freedesktop.UPower.GeneralError", "This battery has no charge limit");
		}
	} else {
		return false;
	}
	return true;
}

static void reply_paths(DBus *bus, DBusMsg *call)
{
	DBusMsg *m = dbus_new_reply(call);
	dbus_open(m, "ao");
	for (usize i = 0; i < ARRAY_LEN(supplies); i++)
		if (supplies[i].used)
			dbus_add(m, "o", supplies[i].path);
	dbus_close(m);
	dbus_send(bus, m);
}

static bool upower_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, UPOWER, UPOWER_PROPS, ARRAY_LEN(UPOWER_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "EnumerateDevices")) {
		reply_paths(bus, call);
	} else if (str_eq(mbr, "GetDisplayDevice")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "o", DISPLAY_PATH);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "GetCriticalAction")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "s", "PowerOff");
		dbus_send(bus, m);
	} else {
		return false;
	}
	return true;
}

/* ---- Watching ------------------------------------------------------------------------------- */

static const char DEVICE_XML[] =
	"<interface name=\"org.freedesktop.UPower.Device\">"
	"<method name=\"Refresh\"/>"
	"<method name=\"GetHistory\"><arg type=\"s\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/>"
	"<arg type=\"a(udu)\" direction=\"out\"/></method>"
	"<method name=\"GetStatistics\"><arg type=\"s\" direction=\"in\"/><arg type=\"a(dd)\" direction=\"out\"/></method>"
	"<method name=\"EnableChargeThreshold\"><arg type=\"b\" direction=\"in\"/></method>"
	"</interface>";

static bool same(const Supply *a, const Supply *b)
{
	return a->online == b->online && a->present == b->present && a->state == b->state && a->warning == b->warning &&
	       (i32)(a->percentage * 10) == (i32)(b->percentage * 10) && a->to_empty / 60 == b->to_empty / 60 &&
	       a->to_full / 60 == b->to_full / 60 && a->threshold_enabled == b->threshold_enabled;
}

static void announce_device(const Supply *s)
{
	static const char *const names[] = { "Online", "IsPresent", "State", "WarningLevel", "Percentage", "Energy", "EnergyRate",
					     "TimeToEmpty", "TimeToFull", "IconName", "UpdateTime", "ChargeThresholdEnabled",
					     "ChargeEndThreshold", "ChargeStartThreshold", NULL };
	props_changed(Z.bus, s->path, DEVICE_IF, DEVICE_PROPS, ARRAY_LEN(DEVICE_PROPS), (void *)s, names);
}

static void scan(bool announce)
{
	DirList l = { 0 };
	dir_list("/sys/class/power_supply", &l, false);
	/* gone */
	for (usize i = 0; i < ARRAY_LEN(supplies); i++) {
		Supply *s = &supplies[i];
		if (!s->used)
			continue;
		bool still = false;
		for (usize k = 0; k < l.len; k++)
			still |= str_eq(l.data[k].name, s->name);
		if (!still) {
			s->used = false;
			dbus_unexport(Z.bus, s->path, DEVICE_IF);
			DBusMsg *m = dbus_new_signal(UPOWER_PATH, UPOWER, "DeviceRemoved");
			dbus_add(m, "o", s->path);
			dbus_send(Z.bus, m);
		}
	}
	bool any_mains = false, mains_online = false, battery_discharging = false;
	for (usize k = 0; k < l.len; k++) {
		const char *name = l.data[k].name;
		/* batteries of mice and game pads have their own owners */
		char scope[32];
		attr_str(name, "scope", scope, sizeof scope);
		if (str_eq(scope, "Device"))
			continue;
		Supply *s = NULL, *free_slot = NULL;
		for (usize i = 0; i < ARRAY_LEN(supplies); i++) {
			if (supplies[i].used && str_eq(supplies[i].name, name))
				s = &supplies[i];
			else if (!supplies[i].used && !free_slot)
				free_slot = &supplies[i];
		}
		bool fresh = !s;
		if (fresh) {
			if (!free_slot)
				continue;
			s = free_slot;
			*s = (Supply){ 0 };
			str_copy(s->name, name, sizeof s->name);
		}
		Supply before = *s;
		read_supply(s);
		if (s->type == TYPE_LINE_POWER) {
			any_mains = true;
			mains_online |= s->online;
		} else if (s->state == ST_DISCHARGING) {
			battery_discharging = true;
		}
		if (fresh) {
			char *esc = path_escape(name);
			fmt(s->path, sizeof s->path, "/org/freedesktop/UPower/devices/%s_%s",
			    s->type == TYPE_LINE_POWER ? "line_power" : s->type == TYPE_UPS ? "ups" : "battery", esc);
			mem_free(esc);
			s->used = true;
			dbus_export(Z.bus, s->path, DEVICE_IF, device_method, s, DEVICE_XML);
			if (announce) {
				DBusMsg *m = dbus_new_signal(UPOWER_PATH, UPOWER, "DeviceAdded");
				dbus_add(m, "o", s->path);
				dbus_send(Z.bus, m);
			}
		} else if (announce && !same(&before, s)) {
			announce_device(s);
		}
	}
	dir_list_free(&l);
	Supply before = display;
	compose_display();
	if (announce && !same(&before, &display))
		announce_device(&display);
	bool was = on_battery;
	on_battery = any_mains ? !mains_online : battery_discharging;
	if (announce && was != on_battery) {
		static const char *const names[] = { "OnBattery", NULL };
		props_changed(Z.bus, UPOWER_PATH, UPOWER, UPOWER_PROPS, ARRAY_LEN(UPOWER_PROPS), NULL, names);
	}
}

static void tick(Loop *loop, void *data)
{
	(void)loop, (void)data;
	scan(true);
}

/* ---- Power profiles ---------------------------------------------------------------------------- */

static char active_profile[16] = "balanced";
static u32 next_cookie = 1;

typedef struct {
	u32 cookie;
	char profile[16], reason[128], app[128], owner[64];
} Hold;

static Hold holds[16];
static int nholds;

static bool platform_has(const char *choice)
{
	char *t = file_read("/sys/firmware/acpi/platform_profile_choices", NULL);
	bool ok = t && str_find(t, choice);
	mem_free(t);
	return ok;
}

static bool epp_available(void)
{
	return file_exists("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference");
}

static bool performance_supported(void)
{
	return platform_has("performance") || epp_available();
}

static const char *driver_name(void)
{
	return file_exists("/sys/firmware/acpi/platform_profile") ? "platform_profile" : epp_available() ? "intel_pstate" : "placeholder";
}

/* Puts the computer into a profile. */
static void apply_profile(const char *p)
{
	if (file_exists("/sys/firmware/acpi/platform_profile")) {
		const char *v = str_eq(p, "power-saver") ? (platform_has("low-power") ? "low-power" : "quiet")
			      : str_eq(p, "performance") ? "performance" : "balanced";
		file_write_str("/sys/firmware/acpi/platform_profile", v);
	}
	const char *epp = str_eq(p, "power-saver") ? "power" : str_eq(p, "performance") ? "performance" : "balance_performance";
	DirList l = { 0 };
	if (dir_list("/sys/devices/system/cpu", &l, false)) {
		for (usize i = 0; i < l.len; i++) {
			const char *n = l.data[i].name;
			if (!str_starts(n, "cpu") || !is_digit((u8)n[3]))
				continue;
			char *path = str_fmt("/sys/devices/system/cpu/%s/cpufreq/energy_performance_preference", n);
			if (file_exists(path)) {
				/* the preference can only change under the powersave governor */
				char *gov = str_fmt("/sys/devices/system/cpu/%s/cpufreq/scaling_governor", n);
				file_write_str(gov, "powersave");
				mem_free(gov);
				file_write_str(path, epp);
			}
			mem_free(path);
		}
		dir_list_free(&l);
	}
	/* AMD's and others' processors without the energy preference: the governor */
	if (!epp_available()) {
		const char *gov = str_eq(p, "power-saver") ? "powersave" : str_eq(p, "performance") ? "performance" : "schedutil";
		DirList c = { 0 };
		if (dir_list("/sys/devices/system/cpu/cpufreq", &c, false)) {
			for (usize i = 0; i < c.len; i++) {
				char *path = str_fmt("/sys/devices/system/cpu/cpufreq/%s/scaling_governor", c.data[i].name);
				file_write_str(path, gov);
				mem_free(path);
			}
			dir_list_free(&c);
		}
	}
}

static void save_profile(void)
{
	char *path = data_path("system.conf");
	Conf *c = conf_load(path);
	conf_set(c, "power_profile", active_profile);
	conf_save(c);
	conf_free(c);
	mem_free(path);
}

static const char *effective_profile(void)
{
	/* a hold wins over the chosen profile; power saving wins over performance */
	const char *p = NULL;
	for (int i = 0; i < nholds; i++)
		if (!p || str_eq(holds[i].profile, "power-saver"))
			p = holds[i].profile;
	return p ? p : active_profile;
}

GET(pp_active) { (void)obj; dbus_add(m, "s", effective_profile()); }
GET(pp_inhibited) { (void)obj; dbus_add(m, "s", ""); }
GET(pp_degraded)
{
	(void)obj;
	char *t = file_read_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
	dbus_add(m, "s", t && t[0] == '1' ? "high-operating-temperature" : "");
	mem_free(t);
}
GET(pp_profiles)
{
	(void)obj;
	static const char *const ALL[] = { "power-saver", "balanced", "performance" };
	dbus_open(m, "aa{sv}");
	for (usize i = 0; i < ARRAY_LEN(ALL); i++) {
		if (i == 2 && !performance_supported())
			continue;
		dbus_open(m, "a{sv}");
		dbus_dict_add(m, "Profile", "s", ALL[i]);
		dbus_dict_add(m, "Driver", "s", driver_name());
		dbus_dict_add(m, "PlatformDriver", "s", file_exists("/sys/firmware/acpi/platform_profile") ? "platform_profile" : "placeholder");
		dbus_dict_add(m, "CpuDriver", "s", epp_available() ? "intel_pstate" : "cpufreq");
		dbus_close(m);
	}
	dbus_close(m);
}
GET(pp_actions) { (void)obj; dbus_open(m, "as"); dbus_close(m); }
GET(pp_holds)
{
	(void)obj;
	dbus_open(m, "aa{sv}");
	for (int i = 0; i < nholds; i++) {
		dbus_open(m, "a{sv}");
		dbus_dict_add(m, "Profile", "s", holds[i].profile);
		dbus_dict_add(m, "Reason", "s", holds[i].reason);
		dbus_dict_add(m, "ApplicationId", "s", holds[i].app);
		dbus_close(m);
	}
	dbus_close(m);
}
GET(pp_version) { (void)obj; dbus_add(m, "s", "0.30"); }
GET(pp_battery_aware) { (void)obj; dbus_add(m, "b", false); }

static void profiles_changed(void);

static bool pp_set_active(const DBusVal *v, void *obj)
{
	(void)obj;
	const char *p = dbus_str(v, "");
	if (!str_eq(p, "power-saver") && !str_eq(p, "balanced") && !(str_eq(p, "performance") && performance_supported()))
		return false;
	str_copy(active_profile, p, sizeof active_profile);
	nholds = 0;   /* choosing a profile ends the holds, as power-profiles-daemon does */
	apply_profile(active_profile);
	save_profile();
	return true;
}

static const Prop PROFILE_PROPS[] = {
	{ "ActiveProfile", "s", pp_active, pp_set_active },
	{ "PerformanceInhibited", "s", pp_inhibited, NULL },
	{ "PerformanceDegraded", "s", pp_degraded, NULL },
	{ "Profiles", "aa{sv}", pp_profiles, NULL },
	{ "Actions", "as", pp_actions, NULL },
	{ "ActiveProfileHolds", "aa{sv}", pp_holds, NULL },
	{ "Version", "s", pp_version, NULL },
	{ "BatteryAware", "b", pp_battery_aware, NULL },
};

static void profiles_changed(void)
{
	static const char *const names[] = { "ActiveProfile", "ActiveProfileHolds", NULL };
	props_changed(Z.bus, PROFILES_PATH, PROFILES_IF, PROFILE_PROPS, ARRAY_LEN(PROFILE_PROPS), NULL, names);
	props_changed(Z.bus, HADESS_PATH, HADESS_IF, PROFILE_PROPS, ARRAY_LEN(PROFILE_PROPS), NULL, names);
}

static void release_hold(int i, bool tell)
{
	u32 cookie = holds[i].cookie;
	char owner[64];
	str_copy(owner, holds[i].owner, sizeof owner);
	holds[i] = holds[--nholds];
	apply_profile(effective_profile());
	if (tell) {
		DBusMsg *m = dbus_new_signal(PROFILES_PATH, PROFILES_IF, "ProfileReleased");
		dbus_add(m, "u", cookie);
		dbus_set_destination(m, owner);
		dbus_send(Z.bus, m);
	}
	profiles_changed();
}

static bool profiles_method(DBus *bus, DBusMsg *call, void *data)
{
	const char *iface = data;
	if (props_call(bus, call, iface, PROFILE_PROPS, ARRAY_LEN(PROFILE_PROPS), NULL)) {
		if (dbus_is_call(call, "org.freedesktop.DBus.Properties", "Set"))
			profiles_changed();
		return true;
	}
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "HoldProfile")) {
		const char *p = dbus_str(dbus_arg(call, 0), "");
		if ((!str_eq(p, "power-saver") && !str_eq(p, "performance")) || nholds >= (int)ARRAY_LEN(holds)) {
			dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs", "Only power-saver and performance can be held");
			return true;
		}
		Hold *h = &holds[nholds++];
		h->cookie = next_cookie++;
		str_copy(h->profile, p, sizeof h->profile);
		str_copy(h->reason, dbus_str(dbus_arg(call, 1), ""), sizeof h->reason);
		str_copy(h->app, dbus_str(dbus_arg(call, 2), ""), sizeof h->app);
		str_copy(h->owner, dbus_sender(call), sizeof h->owner);
		apply_profile(effective_profile());
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "u", h->cookie);
		dbus_send(bus, m);
		profiles_changed();
	} else if (str_eq(mbr, "ReleaseProfile")) {
		u32 cookie = (u32)dbus_uint(dbus_arg(call, 0), 0);
		for (int i = 0; i < nholds; i++)
			if (holds[i].cookie == cookie) {
				dbus_reply_empty(bus, call);
				release_hold(i, false);
				return true;
			}
		dbus_reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs", "No such hold");
	} else if (str_eq(mbr, "SetActionEnabled")) {
		dbus_reply_empty(bus, call);
	} else {
		return false;
	}
	return true;
}

/* Holders that leave the bus let go. */
static void on_name_owner(DBus *bus, DBusMsg *sig, void *data)
{
	(void)bus, (void)data;
	const char *name = dbus_str(dbus_arg(sig, 0), ""), *now = dbus_str(dbus_arg(sig, 2), "");
	if (*now)
		return;
	for (int i = nholds - 1; i >= 0; i--)
		if (str_eq(holds[i].owner, name))
			release_hold(i, false);
}

static const char PROFILES_XML[] =
	"<interface name=\"%s\">"
	"<method name=\"HoldProfile\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
	"<arg type=\"u\" direction=\"out\"/></method>"
	"<method name=\"ReleaseProfile\"><arg type=\"u\" direction=\"in\"/></method>"
	"<signal name=\"ProfileReleased\"><arg type=\"u\"/></signal>"
	"<property name=\"ActiveProfile\" type=\"s\" access=\"readwrite\"/>"
	"<property name=\"Profiles\" type=\"aa{sv}\" access=\"read\"/>"
	"</interface>";

static const char UPOWER_XML[] =
	"<interface name=\"org.freedesktop.UPower\">"
	"<method name=\"EnumerateDevices\"><arg type=\"ao\" direction=\"out\"/></method>"
	"<method name=\"GetDisplayDevice\"><arg type=\"o\" direction=\"out\"/></method>"
	"<method name=\"GetCriticalAction\"><arg type=\"s\" direction=\"out\"/></method>"
	"<signal name=\"DeviceAdded\"><arg type=\"o\"/></signal>"
	"<signal name=\"DeviceRemoved\"><arg type=\"o\"/></signal>"
	"</interface>";

void upower_start(DBus *bus)
{
	scan(false);
	dbus_export(bus, UPOWER_PATH, UPOWER, upower_method, NULL, UPOWER_XML);
	dbus_export(bus, DISPLAY_PATH, DEVICE_IF, device_method, &display, DEVICE_XML);
	dbus_request_name(bus, UPOWER, DBUS_NAME_NO_QUEUE);
	loop_timer(Z.loop, 10000, 10000, tick, NULL);

	char *path = data_path("system.conf");
	Conf *c = conf_load(path);
	str_copy(active_profile, conf_get(c, "power_profile", "balanced"), sizeof active_profile);
	conf_free(c);
	mem_free(path);
	if (str_eq(active_profile, "performance") && !performance_supported())
		str_copy(active_profile, "balanced", sizeof active_profile);
	apply_profile(active_profile);
	static char xml1[sizeof PROFILES_XML + 64], xml2[sizeof PROFILES_XML + 64];
	fmt(xml1, sizeof xml1, PROFILES_XML, PROFILES_IF);
	fmt(xml2, sizeof xml2, PROFILES_XML, HADESS_IF);
	dbus_export(bus, PROFILES_PATH, PROFILES_IF, profiles_method, (void *)PROFILES_IF, xml1);
	dbus_export(bus, HADESS_PATH, HADESS_IF, profiles_method, (void *)HADESS_IF, xml2);
	dbus_subscribe(bus, "org.freedesktop.DBus", NULL, "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner, NULL);
	dbus_request_name(bus, PROFILES_IF, DBUS_NAME_NO_QUEUE);
	dbus_request_name(bus, HADESS_IF, DBUS_NAME_NO_QUEUE);
}
