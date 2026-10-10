/*
 * org.freedesktop.NetworkManager, answered from znet: GNOME's network
 * menu, its Wi-Fi password dialog and the Network and Wi-Fi pages of its
 * Settings show and drive Zurbax's own network service.
 *
 * znet does the work (DHCP, Wi-Fi scanning and WPA2) and reports in
 * /run/zurbax/network.status and /run/zurbax/wifi.networks; requests go
 * to it over /run/zurbax/wifi.sock. zsys reads those every second and
 * keeps NetworkManager's objects (devices, access points, saved
 * connections, the active ones and their addresses) in step, announcing
 * each change the way NetworkManager does. Saved Wi-Fi networks are znet's
 * (/data/system/wifi); wired links are always automatic.
 */
#include "zsys.h"

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define NM_IF NM
#define SETTINGS_PATH NM_PATH "/Settings"
#define SETTINGS_IF NM ".Settings"
#define CONN_IF NM ".Settings.Connection"
#define DEVICE_IF NM ".Device"
#define WIRED_IF NM ".Device.Wired"
#define WIRELESS_IF NM ".Device.Wireless"
#define AP_IF NM ".AccessPoint"
#define ACTIVE_IF NM ".Connection.Active"
#define IP4_IF NM ".IP4Config"
#define AGENTS_PATH NM_PATH "/AgentManager"
#define AGENTS_IF NM ".AgentManager"
#define AGENT_IF NM ".SecretAgent"
#define AGENT_PATH "/org/freedesktop/NetworkManager/SecretAgent"
#define OM_PATH "/org/freedesktop"
#define OM_IF "org.freedesktop.DBus.ObjectManager"

#define STATUS_FILE "/run/zurbax/network.status"
#define NETWORKS_FILE "/run/zurbax/wifi.networks"
#define WIFI_SOCK "/run/zurbax/wifi.sock"
#define WIFI_DIR "/data/system/wifi"

enum { NM_STATE_DISCONNECTED = 20, NM_STATE_CONNECTING = 40, NM_STATE_CONNECTED_GLOBAL = 70 };
enum { DEV_UNAVAILABLE = 20, DEV_DISCONNECTED = 30, DEV_PREPARE = 40, DEV_CONFIG = 50, DEV_IP_CONFIG = 70, DEV_ACTIVATED = 100,
       DEV_FAILED = 120 };
enum { REASON_NONE = 0, REASON_UNKNOWN = 1, REASON_NO_SECRETS = 7, REASON_SUPPLICANT_FAILED = 9, REASON_CARRIER = 40,
       REASON_USER = 39 };
enum { AC_ACTIVATING = 1, AC_ACTIVATED = 2 };
enum { TYPE_ETHERNET = 1, TYPE_WIFI = 2 };

/* ---- The model ---------------------------------------------------------------------------- */

typedef struct {
	bool used, seen;
	u32 num;
	char name[16], mac[18], udi[160], driver[32];
	bool wifi, carrier;
	u32 speed, mtu, state, reason;
	u64 hash[3];
} Dev;

typedef struct {
	bool used, seen;
	u32 num;
	u8 ssid[32];
	int len;
	char hex[65], sec[16];
	u8 strength;
	bool saved, connected;
	u64 hash;
} Ap;

typedef struct {
	bool used, seen;
	u32 num;
	bool wifi, unsaved;
	char id[96], uuid[40], iface[16], hex[65], psk[128];
	u8 ssid[32];
	int len;
	u64 hash;
} Conn;

typedef struct {
	bool used;
	u32 num;
	Conn *conn;
	Dev *dev;
	Ap *ap;
	u32 state;
	u64 hash[2];     /* the active connection, its addresses */
} Active;

static Dev devs[8];
static Ap aps[64];
static Conn conns[64];
static Active actives[4];
static u32 next_num = 1;
static u64 root_hash, settings_hash;
static u32 nm_state = NM_STATE_DISCONNECTED;
static bool radio_on = true;
static char agent_owner[64];

/* what znet says */
static struct {
	bool connected, connecting;
	char kind[16], iface[16], address[48], gateway[48], dns[160];
	char wifi_state[16], wifi_hex[65], wifi_error[96];
} st;

static char *path_of(const char *kind, u32 num)
{
	return str_fmt(NM_PATH "/%s/%u", kind, num);
}

#define DEV_PATH(d) path_of("Devices", (d)->num)
#define AP_PATH(a) path_of("AccessPoint", (a)->num)
#define CONN_PATH(c) path_of("Settings", (c)->num)
#define ACTIVE_PATH(a) path_of("ActiveConnection", (a)->num)
#define IP4_PATH(a) path_of("IP4Config", (a)->num)

static Active *active_on(const Dev *d)
{
	for (usize i = 0; i < ARRAY_LEN(actives); i++)
		if (actives[i].used && actives[i].dev == d)
			return &actives[i];
	return NULL;
}

static Dev *wifi_dev(void)
{
	for (usize i = 0; i < ARRAY_LEN(devs); i++)
		if (devs[i].used && devs[i].wifi)
			return &devs[i];
	return NULL;
}

/* ---- Talking to znet ---------------------------------------------------------------------- */

static void hex_of(const u8 *b, int n, char *out)
{
	for (int i = 0; i < n; i++)
		fmt(out + i * 2, 3, "%02x", b[i]);
	out[n * 2] = 0;
}

static void znet(const char *cmd)
{
	int fd = (int)sys_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	str_copy(a.sun_path, WIFI_SOCK, sizeof a.sun_path);
	sys_sendto(fd, cmd, strlen(cmd), 0, &a, sizeof a);
	sys_close(fd);
}

static void znet_connect(const char *hex, const char *password)
{
	char cmd[512];
	if (password && *password) {
		char phex[260];
		hex_of((const u8 *)password, (int)MIN(strlen(password), 127), phex);
		fmt(cmd, sizeof cmd, "connect %s %s", hex, phex);
	} else {
		fmt(cmd, sizeof cmd, "connect %s", hex);
	}
	znet(cmd);
	memset(cmd, 0, sizeof cmd);
}

/* ---- Reading the state -------------------------------------------------------------------- */

static char *net_attr(const char *dev, const char *a)
{
	char *p = str_fmt("/sys/class/net/%s/%s", dev, a);
	char *v = file_read_line(p);
	mem_free(p);
	return v;
}

static Dev *find_dev(const char *name)
{
	for (usize i = 0; i < ARRAY_LEN(devs); i++)
		if (devs[i].used && str_eq(devs[i].name, name))
			return &devs[i];
	return NULL;
}

static void read_devices(void)
{
	for (usize i = 0; i < ARRAY_LEN(devs); i++)
		devs[i].seen = false;
	DirList l = { 0 };
	if (!dir_list("/sys/class/net", &l, false))
		return;
	for (usize k = 0; k < l.len; k++) {
		const char *name = l.data[k].name;
		char *devlink = str_fmt("/sys/class/net/%s/device", name);
		bool physical = file_exists(devlink);
		mem_free(devlink);
		char *type = net_attr(name, "type");
		bool ether = type && str_eq(type, "1");
		mem_free(type);
		if (!physical || !ether)
			continue;
		Dev *d = find_dev(name);
		if (!d) {
			for (usize i = 0; i < ARRAY_LEN(devs) && !d; i++)
				if (!devs[i].used)
					d = &devs[i];
			if (!d)
				continue;
			*d = (Dev){ .used = true, .num = next_num++ };
			str_copy(d->name, name, sizeof d->name);
			char *p = str_fmt("/sys/class/net/%s", name);
			char target[160] = { 0 };
			long n = sys_readlink(p, target, sizeof target - 1);
			fmt(d->udi, sizeof d->udi, "/sys/class/net/%s", name);
			if (n > 0 && str_starts(target, "../../devices"))
				fmt(d->udi, sizeof d->udi, "/sys%s", target + 5);
			mem_free(p);
			p = str_fmt("/sys/class/net/%s/device/driver", name);
			n = sys_readlink(p, target, sizeof target - 1);
			target[n > 0 ? n : 0] = 0;
			str_copy(d->driver, path_base(target), sizeof d->driver);
			mem_free(p);
			char *w = str_fmt("/sys/class/net/%s/wireless", name), *phy = str_fmt("/sys/class/net/%s/phy80211", name);
			d->wifi = dir_exists(w) || file_exists(phy);
			mem_free(w);
			mem_free(phy);
		}
		d->seen = true;
		char *mac = net_attr(name, "address");
		str_copy(d->mac, mac ? mac : "", sizeof d->mac);
		mem_free(mac);
		for (char *c = d->mac; *c; c++)
			*c = (char)to_upper((u8)*c);
		char *carrier = net_attr(name, "carrier");
		d->carrier = carrier && carrier[0] == '1';
		mem_free(carrier);
		char *speed = net_attr(name, "speed");
		i64 sp = speed ? str_to_int(speed, NULL) : 0;
		d->speed = sp > 0 ? (u32)sp : 0;
		mem_free(speed);
		char *mtu = net_attr(name, "mtu");
		d->mtu = mtu ? (u32)str_to_int(mtu, NULL) : 1500;
		mem_free(mtu);
	}
	dir_list_free(&l);
}

static void read_status(void)
{
	memset(&st, 0, sizeof st);
	Conf *c = conf_load(STATUS_FILE);
	const char *state = conf_get(c, "state", "disconnected");
	st.connected = str_eq(state, "connected");
	st.connecting = str_eq(state, "connecting");
	str_copy(st.kind, conf_get(c, "kind", ""), sizeof st.kind);
	str_copy(st.iface, conf_get(c, "iface", ""), sizeof st.iface);
	str_copy(st.address, conf_get(c, "address", ""), sizeof st.address);
	str_copy(st.gateway, conf_get(c, "gateway", ""), sizeof st.gateway);
	str_copy(st.dns, conf_get(c, "dns", ""), sizeof st.dns);
	str_copy(st.wifi_state, conf_get(c, "wifi_state", "idle"), sizeof st.wifi_state);
	str_copy(st.wifi_hex, conf_get(c, "wifi_ssid_hex", ""), sizeof st.wifi_hex);
	str_copy(st.wifi_error, conf_get(c, "wifi_error", ""), sizeof st.wifi_error);
	radio_on = !str_eq(conf_get(c, "wifi", "on"), "off");
	conf_free(c);
}

static Ap *find_ap(const char *hex)
{
	for (usize i = 0; i < ARRAY_LEN(aps); i++)
		if (aps[i].used && str_eq(aps[i].hex, hex))
			return &aps[i];
	return NULL;
}

static int unhex(const char *hex, u8 *out, int cap)
{
	int n = (int)MIN(strlen(hex) / 2, (usize)cap);
	return hex_decode(hex, out, (usize)n) ? n : 0;
}

static void read_networks(void)
{
	for (usize i = 0; i < ARRAY_LEN(aps); i++)
		aps[i].seen = false;
	char *text = file_read(NETWORKS_FILE, NULL);
	usize n = 0;
	char **lines = str_split(text ? text : "", '\n', &n);
	for (usize i = 0; i < n; i++) {
		usize nf = 0;
		char **f = str_split(lines[i], '\t', &nf);
		if (nf >= 5 && f[4][0] && strlen(f[4]) <= 64) {
			Ap *a = find_ap(f[4]);
			if (!a) {
				for (usize k = 0; k < ARRAY_LEN(aps) && !a; k++)
					if (!aps[k].used)
						a = &aps[k];
			}
			if (a) {
				if (!a->used) {
					*a = (Ap){ .used = true, .num = next_num++ };
					str_copy(a->hex, f[4], sizeof a->hex);
					a->len = unhex(a->hex, a->ssid, 32);
				}
				a->seen = true;
				a->strength = (u8)CLAMP(str_to_int(f[0], NULL), 0, 100);
				str_copy(a->sec, f[1], sizeof a->sec);
				a->saved = str_eq(f[2], "1");
				a->connected = str_eq(f[3], "1");
			}
		}
		str_split_free(f, nf);
	}
	str_split_free(lines, n);
	mem_free(text);
}

/* ---- Connections (saved settings) ------------------------------------------------------------ */

static void make_uuid(const char *key, char out[40])
{
	u8 h[32];
	sha256(key, strlen(key), h);
	h[6] = (h[6] & 0x0f) | 0x50;
	h[8] = (h[8] & 0x3f) | 0x80;
	fmt(out, 40, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", h[0], h[1], h[2], h[3], h[4], h[5], h[6],
	    h[7], h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
}

static Conn *find_conn_key(bool wifi, const char *what)
{
	for (usize i = 0; i < ARRAY_LEN(conns); i++)
		if (conns[i].used && conns[i].wifi == wifi && str_eq(wifi ? conns[i].hex : conns[i].iface, what))
			return &conns[i];
	return NULL;
}

static Conn *add_conn(bool wifi, const char *what)
{
	Conn *c = find_conn_key(wifi, what);
	if (c)
		return c;
	for (usize i = 0; i < ARRAY_LEN(conns) && !c; i++)
		if (!conns[i].used)
			c = &conns[i];
	if (!c)
		return NULL;
	*c = (Conn){ .used = true, .num = next_num++, .wifi = wifi };
	char key[96];
	if (wifi) {
		str_copy(c->hex, what, sizeof c->hex);
		c->len = unhex(what, c->ssid, 32);
		memcpy(c->id, c->ssid, (usize)c->len);
		c->id[c->len] = 0;
		fmt(key, sizeof key, "wifi:%s", what);
	} else {
		str_copy(c->iface, what, sizeof c->iface);
		int wired = 0;
		for (usize i = 0; i < ARRAY_LEN(conns); i++)
			wired += conns[i].used && !conns[i].wifi;
		fmt(c->id, sizeof c->id, "Wired connection %d", wired);
		fmt(key, sizeof key, "ethernet:%s", what);
	}
	make_uuid(key, c->uuid);
	return c;
}

static void read_connections(void)
{
	for (usize i = 0; i < ARRAY_LEN(conns); i++)
		conns[i].seen = conns[i].used && conns[i].unsaved;   /* made here, waiting to be used */
	for (usize i = 0; i < ARRAY_LEN(devs); i++)
		if (devs[i].used && !devs[i].wifi) {
			Conn *c = add_conn(false, devs[i].name);
			if (c)
				c->seen = true;
		}
	DirList l = { 0 };
	if (dir_list(WIFI_DIR, &l, false)) {
		for (usize k = 0; k < l.len; k++) {
			const char *name = l.data[k].name;
			usize n = strlen(name);
			if (n < 6 || !str_ends(name, ".conf") || n - 5 > 64)
				continue;
			char hex[65];
			str_copy(hex, name, n - 5 + 1);
			Conn *c = add_conn(true, hex);
			if (c) {
				c->seen = true;
				c->unsaved = false;
			}
		}
		dir_list_free(&l);
	}
}

/* ---- Property getters --------------------------------------------------------------------------- */

#define GET(name) static void name(DBusMsg *m, void *obj)
#define D ((Dev *)obj)
#define A ((Ap *)obj)
#define C ((Conn *)obj)
#define AC ((Active *)obj)

static void add_path(DBusMsg *m, char *p)
{
	dbus_add(m, "o", p);
	mem_free(p);
}

static void add_paths_devices(DBusMsg *m)
{
	dbus_open(m, "ao");
	for (usize i = 0; i < ARRAY_LEN(devs); i++)
		if (devs[i].used)
			add_path(m, DEV_PATH(&devs[i]));
	dbus_close(m);
}

static Active *primary(void)
{
	for (usize i = 0; i < ARRAY_LEN(actives); i++)
		if (actives[i].used && actives[i].state == AC_ACTIVATED)
			return &actives[i];
	return NULL;
}

static Active *activating(void)
{
	for (usize i = 0; i < ARRAY_LEN(actives); i++)
		if (actives[i].used && actives[i].state == AC_ACTIVATING)
			return &actives[i];
	return NULL;
}

GET(nm_devices) { (void)obj; add_paths_devices(m); }
GET(nm_empty_ao) { (void)obj; dbus_open(m, "ao"); dbus_close(m); }
GET(nm_true) { (void)obj; dbus_add(m, "b", true); }
GET(nm_false) { (void)obj; dbus_add(m, "b", false); }
GET(nm_wireless) { (void)obj; dbus_add(m, "b", radio_on); }
GET(nm_wireless_hw) { (void)obj; dbus_add(m, "b", wifi_dev() != NULL); }
GET(nm_actives)
{
	(void)obj;
	dbus_open(m, "ao");
	for (usize i = 0; i < ARRAY_LEN(actives); i++)
		if (actives[i].used)
			add_path(m, ACTIVE_PATH(&actives[i]));
	dbus_close(m);
}
GET(nm_primary) { (void)obj; Active *a = primary(); if (a) add_path(m, ACTIVE_PATH(a)); else dbus_add(m, "o", "/"); }
GET(nm_primary_type) { (void)obj; Active *a = primary(); dbus_add(m, "s", !a ? "" : a->conn->wifi ? "802-11-wireless" : "802-3-ethernet"); }
GET(nm_activating) { (void)obj; Active *a = activating(); if (a) add_path(m, ACTIVE_PATH(a)); else dbus_add(m, "o", "/"); }
GET(nm_zero_u) { (void)obj; dbus_add(m, "u", 0u); }
GET(nm_version) { (void)obj; dbus_add(m, "s", "1.56.0"); }
GET(nm_version_info) { (void)obj; dbus_open(m, "au"); dbus_add(m, "u", (1u << 16) | (56u << 8)); dbus_close(m); }
GET(nm_caps) { (void)obj; dbus_open(m, "au"); dbus_close(m); }
GET(nm_state_get) { (void)obj; dbus_add(m, "u", nm_state); }
GET(nm_connectivity) { (void)obj; dbus_add(m, "u", nm_state == NM_STATE_CONNECTED_GLOBAL ? 4u : 1u); }
GET(nm_empty_s) { (void)obj; dbus_add(m, "s", ""); }
GET(nm_empty_dict) { (void)obj; dbus_open(m, "a{sv}"); dbus_close(m); }
GET(nm_radio_flags) { (void)obj; dbus_add(m, "u", wifi_dev() ? 1u : 0u); }

static bool nm_set_wireless(const DBusVal *v, void *obj)
{
	(void)obj;
	bool on = dbus_bool(v, true);
	znet(on ? "radio on" : "radio off");
	radio_on = on;
	return true;
}

static const Prop NM_PROPS[] = {
	{ "Devices", "ao", nm_devices, NULL },
	{ "AllDevices", "ao", nm_devices, NULL },
	{ "Checkpoints", "ao", nm_empty_ao, NULL },
	{ "NetworkingEnabled", "b", nm_true, NULL },
	{ "WirelessEnabled", "b", nm_wireless, nm_set_wireless },
	{ "WirelessHardwareEnabled", "b", nm_wireless_hw, NULL },
	{ "WwanEnabled", "b", nm_false, NULL },
	{ "WwanHardwareEnabled", "b", nm_false, NULL },
	{ "WimaxEnabled", "b", nm_false, NULL },
	{ "WimaxHardwareEnabled", "b", nm_false, NULL },
	{ "RadioFlags", "u", nm_radio_flags, NULL },
	{ "ActiveConnections", "ao", nm_actives, NULL },
	{ "PrimaryConnection", "o", nm_primary, NULL },
	{ "PrimaryConnectionType", "s", nm_primary_type, NULL },
	{ "Metered", "u", nm_zero_u, NULL },
	{ "ActivatingConnection", "o", nm_activating, NULL },
	{ "Startup", "b", nm_false, NULL },
	{ "Version", "s", nm_version, NULL },
	{ "VersionInfo", "au", nm_version_info, NULL },
	{ "Capabilities", "au", nm_caps, NULL },
	{ "State", "u", nm_state_get, NULL },
	{ "Connectivity", "u", nm_connectivity, NULL },
	{ "ConnectivityCheckAvailable", "b", nm_false, NULL },
	{ "ConnectivityCheckEnabled", "b", nm_false, NULL },
	{ "ConnectivityCheckUri", "s", nm_empty_s, NULL },
	{ "GlobalDnsConfiguration", "a{sv}", nm_empty_dict, NULL },
};

GET(s_connections)
{
	(void)obj;
	dbus_open(m, "ao");
	for (usize i = 0; i < ARRAY_LEN(conns); i++)
		if (conns[i].used)
			add_path(m, CONN_PATH(&conns[i]));
	dbus_close(m);
}
GET(s_hostname) { (void)obj; struct utsname u; sys_uname(&u); dbus_add(m, "s", u.nodename); }
GET(s_version) { (void)obj; dbus_add(m, "t", (u64)next_num); }

static const Prop SETTINGS_PROPS[] = {
	{ "Connections", "ao", s_connections, NULL },
	{ "Hostname", "s", s_hostname, NULL },
	{ "CanModify", "b", nm_true, NULL },
	{ "VersionId", "t", s_version, NULL },
};

GET(c_unsaved) { dbus_add(m, "b", C->unsaved); }
GET(c_flags) { dbus_add(m, "u", C->unsaved ? 1u : 0u); }
GET(c_filename) { if (C->wifi && !C->unsaved) { char *p = str_fmt(WIFI_DIR "/%s.conf", C->hex); dbus_add(m, "s", p); mem_free(p); } else dbus_add(m, "s", ""); }

static const Prop CONN_PROPS[] = {
	{ "Unsaved", "b", c_unsaved, NULL },
	{ "Flags", "u", c_flags, NULL },
	{ "Filename", "s", c_filename, NULL },
};

GET(d_udi) { dbus_add(m, "s", D->udi); }
GET(d_path) { dbus_add(m, "s", ""); }
GET(d_iface) { dbus_add(m, "s", D->name); }
GET(d_driver) { dbus_add(m, "s", D->driver); }
GET(d_empty) { (void)obj; dbus_add(m, "s", ""); }
GET(d_caps) { (void)obj; dbus_add(m, "u", 3u); }
GET(d_zero_u) { (void)obj; dbus_add(m, "u", 0u); }
GET(d_state) { dbus_add(m, "u", D->state); }
GET(d_reason) { dbus_open(m, "(uu)"); dbus_add(m, "uu", D->state, D->reason); dbus_close(m); }
GET(d_active) { Active *a = active_on(D); if (a) add_path(m, ACTIVE_PATH(a)); else dbus_add(m, "o", "/"); }
GET(d_ip4) { Active *a = active_on(D); if (a && a->state == AC_ACTIVATED) add_path(m, IP4_PATH(a)); else dbus_add(m, "o", "/"); }
GET(d_none_o) { (void)obj; dbus_add(m, "o", "/"); }
GET(d_true) { (void)obj; dbus_add(m, "b", true); }
GET(d_false) { (void)obj; dbus_add(m, "b", false); }
GET(d_type) { dbus_add(m, "u", D->wifi ? (u32)TYPE_WIFI : (u32)TYPE_ETHERNET); }
GET(d_available)
{
	dbus_open(m, "ao");
	for (usize i = 0; i < ARRAY_LEN(conns); i++) {
		Conn *c = &conns[i];
		if (!c->used || c->wifi != D->wifi || (!c->wifi && !str_eq(c->iface, D->name)))
			continue;
		if (c->wifi && !find_ap(c->hex))
			continue;
		add_path(m, CONN_PATH(c));
	}
	dbus_close(m);
}
GET(d_mtu) { dbus_add(m, "u", D->mtu); }
GET(d_lldp) { (void)obj; dbus_open(m, "aa{sv}"); dbus_close(m); }
GET(d_connectivity) { dbus_add(m, "u", D->state == DEV_ACTIVATED ? 4u : 1u); }
GET(d_flags) { dbus_add(m, "u", 1u | (D->carrier ? 0x10000u : 0u)); }
GET(d_mac) { dbus_add(m, "s", D->mac); }
GET(d_speed) { dbus_add(m, "u", D->speed); }
GET(d_carrier) { dbus_add(m, "b", D->carrier); }
GET(d_empty_as) { (void)obj; dbus_open(m, "as"); dbus_close(m); }
GET(d_ip4addr)
{
	Active *a = active_on(D);
	u32 v = 0;
	if (a && a->state == AC_ACTIVATED && st.address[0]) {
		u32 parts[4] = { 0 };
		usize n = 0;
		char **p = str_split(st.address, '.', &n);
		for (usize i = 0; i < n && i < 4; i++)
			parts[i] = (u32)str_to_int(p[i], NULL);
		str_split_free(p, n);
		v = parts[0] | parts[1] << 8 | parts[2] << 16 | parts[3] << 24;
	}
	dbus_add(m, "u", v);
}

static const Prop DEVICE_PROPS[] = {
	{ "Udi", "s", d_udi, NULL },
	{ "Path", "s", d_path, NULL },
	{ "Interface", "s", d_iface, NULL },
	{ "IpInterface", "s", d_iface, NULL },
	{ "Driver", "s", d_driver, NULL },
	{ "DriverVersion", "s", d_empty, NULL },
	{ "FirmwareVersion", "s", d_empty, NULL },
	{ "Capabilities", "u", d_caps, NULL },
	{ "Ip4Address", "u", d_ip4addr, NULL },
	{ "State", "u", d_state, NULL },
	{ "StateReason", "(uu)", d_reason, NULL },
	{ "ActiveConnection", "o", d_active, NULL },
	{ "Ip4Config", "o", d_ip4, NULL },
	{ "Dhcp4Config", "o", d_none_o, NULL },
	{ "Ip6Config", "o", d_none_o, NULL },
	{ "Dhcp6Config", "o", d_none_o, NULL },
	{ "Managed", "b", d_true, NULL },
	{ "Autoconnect", "b", d_true, NULL },
	{ "FirmwareMissing", "b", d_false, NULL },
	{ "NmPluginMissing", "b", d_false, NULL },
	{ "DeviceType", "u", d_type, NULL },
	{ "AvailableConnections", "ao", d_available, NULL },
	{ "PhysicalPortId", "s", d_empty, NULL },
	{ "Mtu", "u", d_mtu, NULL },
	{ "Metered", "u", d_zero_u, NULL },
	{ "LldpNeighbors", "aa{sv}", d_lldp, NULL },
	{ "Real", "b", d_true, NULL },
	{ "Ip4Connectivity", "u", d_connectivity, NULL },
	{ "Ip6Connectivity", "u", d_zero_u, NULL },
	{ "InterfaceFlags", "u", d_flags, NULL },
	{ "HwAddress", "s", d_mac, NULL },
	{ "Ports", "ao", nm_empty_ao, NULL },
};

static const Prop WIRED_PROPS[] = {
	{ "HwAddress", "s", d_mac, NULL },
	{ "PermHwAddress", "s", d_mac, NULL },
	{ "Speed", "u", d_speed, NULL },
	{ "S390Subchannels", "as", d_empty_as, NULL },
	{ "Carrier", "b", d_carrier, NULL },
};

GET(w_mode) { (void)obj; dbus_add(m, "u", 2u); }
GET(w_bitrate) { dbus_add(m, "u", D->state == DEV_ACTIVATED ? 300000u : 0u); }
GET(w_aps)
{
	(void)obj;
	dbus_open(m, "ao");
	for (usize i = 0; i < ARRAY_LEN(aps); i++)
		if (aps[i].used)
			add_path(m, AP_PATH(&aps[i]));
	dbus_close(m);
}
GET(w_active_ap)
{
	Active *a = active_on(D);
	if (a && a->ap && a->ap->used)
		add_path(m, AP_PATH(a->ap));
	else
		dbus_add(m, "o", "/");
}
GET(w_caps) { (void)obj; dbus_add(m, "u", 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x100u | 0x200u | 0x400u); }
GET(w_last_scan) { (void)obj; dbus_add(m, "x", (i64)time_mono_ms()); }

static const Prop WIRELESS_PROPS[] = {
	{ "HwAddress", "s", d_mac, NULL },
	{ "PermHwAddress", "s", d_mac, NULL },
	{ "Mode", "u", w_mode, NULL },
	{ "Bitrate", "u", w_bitrate, NULL },
	{ "AccessPoints", "ao", w_aps, NULL },
	{ "ActiveAccessPoint", "o", w_active_ap, NULL },
	{ "WirelessCapabilities", "u", w_caps, NULL },
	{ "LastScan", "x", w_last_scan, NULL },
};

static bool secured(const Ap *a)
{
	return !str_eq(a->sec, "open");
}

GET(a_flags) { dbus_add(m, "u", secured(A) ? 1u : 0u); }
GET(a_wpa) { dbus_add(m, "u", str_eq(A->sec, "wpa") ? 0x100u | 0x4u | 0x40u : 0u); }
GET(a_rsn)
{
	u32 f = 0;
	if (str_eq(A->sec, "wpa2"))
		f = 0x100 | 0x8 | 0x80;            /* PSK, CCMP */
	else if (str_eq(A->sec, "wpa3"))
		f = 0x400 | 0x8 | 0x80;            /* SAE */
	else if (str_eq(A->sec, "enterprise"))
		f = 0x200 | 0x8 | 0x80;            /* 802.1X */
	dbus_add(m, "u", f);
}
GET(a_ssid)
{
	dbus_open(m, "ay");
	for (int i = 0; i < A->len; i++)
		dbus_add(m, "y", A->ssid[i]);
	dbus_close(m);
}
GET(a_freq) { (void)obj; dbus_add(m, "u", 2437u); }
GET(a_hw) { (void)obj; dbus_add(m, "s", ""); }
GET(a_mode) { (void)obj; dbus_add(m, "u", 2u); }
GET(a_bitrate) { (void)obj; dbus_add(m, "u", 300000u); }
GET(a_bandwidth) { (void)obj; dbus_add(m, "u", 20u); }
GET(a_strength) { dbus_add(m, "y", A->strength); }
GET(a_last_seen) { (void)obj; dbus_add(m, "i", (i32)(time_mono_ms() / 1000)); }

static const Prop AP_PROPS[] = {
	{ "Flags", "u", a_flags, NULL },
	{ "WpaFlags", "u", a_wpa, NULL },
	{ "RsnFlags", "u", a_rsn, NULL },
	{ "Ssid", "ay", a_ssid, NULL },
	{ "Frequency", "u", a_freq, NULL },
	{ "HwAddress", "s", a_hw, NULL },
	{ "Mode", "u", a_mode, NULL },
	{ "MaxBitrate", "u", a_bitrate, NULL },
	{ "Bandwidth", "u", a_bandwidth, NULL },
	{ "Strength", "y", a_strength, NULL },
	{ "LastSeen", "i", a_last_seen, NULL },
};

GET(ac_conn) { add_path(m, CONN_PATH(AC->conn)); }
GET(ac_specific) { if (AC->ap && AC->ap->used) add_path(m, AP_PATH(AC->ap)); else dbus_add(m, "o", "/"); }
GET(ac_id) { dbus_add(m, "s", AC->conn->id); }
GET(ac_uuid) { dbus_add(m, "s", AC->conn->uuid); }
GET(ac_type) { dbus_add(m, "s", AC->conn->wifi ? "802-11-wireless" : "802-3-ethernet"); }
GET(ac_devices) { dbus_open(m, "ao"); add_path(m, DEV_PATH(AC->dev)); dbus_close(m); }
GET(ac_state) { dbus_add(m, "u", AC->state); }
GET(ac_state_flags) { dbus_add(m, "u", AC->state == AC_ACTIVATED ? 0x8u | 0x10u : 0u); }
GET(ac_default) { dbus_add(m, "b", AC->state == AC_ACTIVATED); }
GET(ac_ip4) { if (AC->state == AC_ACTIVATED) add_path(m, IP4_PATH(AC)); else dbus_add(m, "o", "/"); }

static const Prop ACTIVE_PROPS[] = {
	{ "Connection", "o", ac_conn, NULL },
	{ "SpecificObject", "o", ac_specific, NULL },
	{ "Id", "s", ac_id, NULL },
	{ "Uuid", "s", ac_uuid, NULL },
	{ "Type", "s", ac_type, NULL },
	{ "Devices", "ao", ac_devices, NULL },
	{ "State", "u", ac_state, NULL },
	{ "StateFlags", "u", ac_state_flags, NULL },
	{ "Default", "b", ac_default, NULL },
	{ "Ip4Config", "o", ac_ip4, NULL },
	{ "Dhcp4Config", "o", d_none_o, NULL },
	{ "Default6", "b", d_false, NULL },
	{ "Ip6Config", "o", d_none_o, NULL },
	{ "Dhcp6Config", "o", d_none_o, NULL },
	{ "Vpn", "b", d_false, NULL },
	{ "Controller", "o", d_none_o, NULL },
	{ "Master", "o", d_none_o, NULL },
};

/* The address and its prefix length, from the kernel. */
static u32 prefix_of(const char *iface)
{
	int fd = (int)sys_socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 24;
	u8 ifr[40] = { 0 };
	str_copy((char *)ifr, iface, 16);
	u32 prefix = 24;
	if (sys_ioctl(fd, 0x891b /* SIOCGIFNETMASK */, ifr) == 0) {
		u32 mask;
		memcpy(&mask, ifr + 20, 4);
		prefix = (u32)__builtin_popcount(mask);
	}
	sys_close(fd);
	return prefix;
}

static u32 ip_u32(const char *s)
{
	u32 parts[4] = { 0 };
	usize n = 0;
	char **p = str_split(s, '.', &n);
	for (usize i = 0; i < n && i < 4; i++)
		parts[i] = (u32)str_to_int(p[i], NULL);
	str_split_free(p, n);
	return parts[0] | parts[1] << 8 | parts[2] << 16 | parts[3] << 24;
}

GET(ip_address_data)
{
	dbus_open(m, "aa{sv}");
	if (st.address[0]) {
		dbus_open(m, "a{sv}");
		dbus_dict_add(m, "address", "s", st.address);
		dbus_dict_add(m, "prefix", "u", prefix_of(AC->dev->name));
		dbus_close(m);
	}
	dbus_close(m);
}
GET(ip_addresses)
{
	dbus_open(m, "aau");
	if (st.address[0]) {
		dbus_open(m, "au");
		dbus_add(m, "uuu", ip_u32(st.address), prefix_of(AC->dev->name), ip_u32(st.gateway));
		dbus_close(m);
	}
	dbus_close(m);
}
GET(ip_gateway) { (void)obj; dbus_add(m, "s", st.gateway); }
GET(ip_routes)
{
	(void)obj;
	dbus_open(m, "aa{sv}");
	if (st.gateway[0]) {
		dbus_open(m, "a{sv}");
		dbus_dict_add(m, "dest", "s", "0.0.0.0");
		dbus_dict_add(m, "prefix", "u", 0u);
		dbus_dict_add(m, "next-hop", "s", st.gateway);
		dbus_close(m);
	}
	dbus_close(m);
}
GET(ip_ns_data)
{
	(void)obj;
	dbus_open(m, "aa{sv}");
	usize n = 0;
	char **p = str_split(st.dns, ' ', &n);
	for (usize i = 0; i < n; i++)
		if (p[i][0]) {
			dbus_open(m, "a{sv}");
			dbus_dict_add(m, "address", "s", p[i]);
			dbus_close(m);
		}
	str_split_free(p, n);
	dbus_close(m);
}
GET(ip_ns)
{
	(void)obj;
	dbus_open(m, "au");
	usize n = 0;
	char **p = str_split(st.dns, ' ', &n);
	for (usize i = 0; i < n; i++)
		if (p[i][0])
			dbus_add(m, "u", ip_u32(p[i]));
	str_split_free(p, n);
	dbus_close(m);
}
GET(ip_prio) { (void)obj; dbus_add(m, "i", 100); }

static const Prop IP4_PROPS[] = {
	{ "AddressData", "aa{sv}", ip_address_data, NULL },
	{ "Addresses", "aau", ip_addresses, NULL },
	{ "Gateway", "s", ip_gateway, NULL },
	{ "RouteData", "aa{sv}", ip_routes, NULL },
	{ "NameserverData", "aa{sv}", ip_ns_data, NULL },
	{ "Nameservers", "au", ip_ns, NULL },
	{ "Domains", "as", d_empty_as, NULL },
	{ "Searches", "as", d_empty_as, NULL },
	{ "DnsOptions", "as", d_empty_as, NULL },
	{ "DnsPriority", "i", ip_prio, NULL },
	{ "WinsServerData", "as", d_empty_as, NULL },
};

#undef D
#undef A
#undef C
#undef AC

/* ---- Objects: exporting, announcing, ObjectManager ----------------------------------------------- */

static const Iface I_NM = IFACE(NM_IF, NM_PROPS), I_SETTINGS = IFACE(SETTINGS_IF, SETTINGS_PROPS), I_CONN = IFACE(CONN_IF, CONN_PROPS),
		   I_DEVICE = IFACE(DEVICE_IF, DEVICE_PROPS), I_WIRED = IFACE(WIRED_IF, WIRED_PROPS),
		   I_WIRELESS = IFACE(WIRELESS_IF, WIRELESS_PROPS), I_AP = IFACE(AP_IF, AP_PROPS), I_ACTIVE = IFACE(ACTIVE_IF, ACTIVE_PROPS),
		   I_IP4 = IFACE(IP4_IF, IP4_PROPS);

static u64 props_hash(const Iface *in, void *obj)
{
	return iface_hash(in, obj);
}

static void sync_props(const char *path, const Iface *in, void *obj, u64 *hash)
{
	iface_sync(Z.bus, path, in, obj, hash);
}

static void add_iface(DBusMsg *m, const Iface *in, void *obj)
{
	iface_add(m, in, obj);
}

static void interfaces_added(const char *path, const Iface *a, const Iface *b, void *obj)
{
	const Iface *list[] = { a, b };
	om_added(Z.bus, OM_PATH, path, list, b ? 2 : 1, obj);
}

static void interfaces_removed(const char *path, const char *a, const char *b)
{
	const char *list[] = { a, b };
	om_removed(Z.bus, OM_PATH, path, list, b ? 2 : 1);
}

static void signal_o(const char *path, const char *iface, const char *member, const char *arg)
{
	DBusMsg *m = dbus_new_signal(path, iface, member);
	dbus_add(m, "o", arg);
	dbus_send(Z.bus, m);
}

/* ---- Methods ----------------------------------------------------------------------------------- */

static bool device_method(DBus *bus, DBusMsg *call, void *data);
static bool ap_method(DBus *bus, DBusMsg *call, void *data);
static bool conn_method(DBus *bus, DBusMsg *call, void *data);
static bool active_method(DBus *bus, DBusMsg *call, void *data);
static bool ip4_method(DBus *bus, DBusMsg *call, void *data);
static void refresh(void);

/* The settings of a connection, as NetworkManager describes them. */
static void add_settings(DBusMsg *m, const Conn *c, bool with_psk)
{
	dbus_open(m, "a{sa{sv}}");
	dbus_open(m, "{sa{sv}}");
	dbus_add(m, "s", "connection");
	dbus_open(m, "a{sv}");
	dbus_dict_add(m, "id", "s", c->id);
	dbus_dict_add(m, "uuid", "s", c->uuid);
	dbus_dict_add(m, "type", "s", c->wifi ? "802-11-wireless" : "802-3-ethernet");
	dbus_dict_add(m, "autoconnect", "b", true);
	if (!c->wifi)
		dbus_dict_add(m, "interface-name", "s", c->iface);
	dbus_close(m);
	dbus_close(m);
	if (c->wifi) {
		Ap *a = find_ap(c->hex);
		dbus_open(m, "{sa{sv}}");
		dbus_add(m, "s", "802-11-wireless");
		dbus_open(m, "a{sv}");
		dbus_open(m, "{sv}");
		dbus_add(m, "s", "ssid");
		dbus_open_variant(m, "ay");
		dbus_open(m, "ay");
		for (int i = 0; i < c->len; i++)
			dbus_add(m, "y", c->ssid[i]);
		dbus_close(m);
		dbus_close(m);
		dbus_close(m);
		dbus_dict_add(m, "mode", "s", "infrastructure");
		if (!a || secured(a))
			dbus_dict_add(m, "security", "s", "802-11-wireless-security");
		dbus_close(m);
		dbus_close(m);
		if (!a || secured(a)) {
			dbus_open(m, "{sa{sv}}");
			dbus_add(m, "s", "802-11-wireless-security");
			dbus_open(m, "a{sv}");
			dbus_dict_add(m, "key-mgmt", "s", a && str_eq(a->sec, "wpa3") ? "sae" : "wpa-psk");
			if (with_psk && c->psk[0])
				dbus_dict_add(m, "psk", "s", c->psk);
			dbus_close(m);
			dbus_close(m);
		}
	} else {
		dbus_open(m, "{sa{sv}}");
		dbus_add(m, "s", "802-3-ethernet");
		dbus_open(m, "a{sv}");
		dbus_close(m);
		dbus_close(m);
	}
	static const char *const ips[] = { "ipv4", "ipv6" };
	for (usize i = 0; i < 2; i++) {
		dbus_open(m, "{sa{sv}}");
		dbus_add(m, "s", ips[i]);
		dbus_open(m, "a{sv}");
		dbus_dict_add(m, "method", "s", "auto");
		dbus_close(m);
		dbus_close(m);
	}
	dbus_close(m);
}

static Conn *conn_by_path(const char *path)
{
	for (usize i = 0; i < ARRAY_LEN(conns); i++) {
		if (!conns[i].used)
			continue;
		char *p = CONN_PATH(&conns[i]);
		bool same = str_eq(p, path);
		mem_free(p);
		if (same)
			return &conns[i];
	}
	return NULL;
}

static Ap *ap_by_path(const char *path)
{
	for (usize i = 0; i < ARRAY_LEN(aps); i++) {
		if (!aps[i].used)
			continue;
		char *p = AP_PATH(&aps[i]);
		bool same = str_eq(p, path);
		mem_free(p);
		if (same)
			return &aps[i];
	}
	return NULL;
}

static Active *active_by_path(const char *path)
{
	for (usize i = 0; i < ARRAY_LEN(actives); i++) {
		if (!actives[i].used)
			continue;
		char *p = ACTIVE_PATH(&actives[i]);
		bool same = str_eq(p, path);
		mem_free(p);
		if (same)
			return &actives[i];
	}
	return NULL;
}

/* An active connection object, made as a connection starts (znet's state then takes over). */
static Active *start_active(Conn *c, Dev *d, Ap *ap)
{
	Active *a = active_on(d);
	if (a && a->conn == c)
		return a;
	if (!a)
		for (usize i = 0; i < ARRAY_LEN(actives) && !a; i++)
			if (!actives[i].used)
				a = &actives[i];
	if (!a)
		return NULL;
	bool fresh = !a->used;
	if (!fresh && a->state == AC_ACTIVATED) {
		char *old = ACTIVE_PATH(a);
		interfaces_removed(old, ACTIVE_IF, IP4_IF);
		mem_free(old);
		fresh = true;
	}
	*a = (Active){ .used = true, .num = next_num++, .conn = c, .dev = d, .ap = ap, .state = AC_ACTIVATING };
	char *p = ACTIVE_PATH(a), *ip = IP4_PATH(a);
	dbus_export(Z.bus, p, ACTIVE_IF, active_method, a, NULL);
	dbus_export(Z.bus, ip, IP4_IF, ip4_method, a, NULL);
	interfaces_added(p, &I_ACTIVE, NULL, a);
	mem_free(p);
	mem_free(ip);
	return a;
}

static void drop_active(Active *a)
{
	char *p = ACTIVE_PATH(a), *ip = IP4_PATH(a);
	dbus_unexport(Z.bus, p, ACTIVE_IF);
	dbus_unexport(Z.bus, ip, IP4_IF);
	interfaces_removed(p, ACTIVE_IF, NULL);
	interfaces_removed(ip, IP4_IF, NULL);
	mem_free(p);
	mem_free(ip);
	*a = (Active){ 0 };
}

/* Secrets: GNOME Shell's agent asks the person for the network's password. */
typedef struct {
	Conn *conn;
} SecretsWait;

static void on_secrets(DBus *bus, DBusMsg *reply, void *data)
{
	(void)bus;
	SecretsWait *w = data;
	Conn *c = w->conn;
	mem_free(w);
	const char *psk = NULL;
	if (reply && dbus_type(reply) != DBUS_ERROR) {
		const DBusVal *sec = dbus_dict_get(dbus_arg(reply, 0), "802-11-wireless-security");
		psk = dbus_str(dbus_dict_get(sec, "psk"), NULL);
	}
	if (!c->used)
		return;
	if (!psk || !*psk) {
		/* dismissed: the attempt ends */
		Dev *d = wifi_dev();
		Active *a = d ? active_on(d) : NULL;
		if (a && a->conn == c)
			drop_active(a);
		if (d)
			d->reason = REASON_NO_SECRETS;
		refresh();
		return;
	}
	str_copy(c->psk, psk, sizeof c->psk);
	znet_connect(c->hex, c->psk);
}

static void ask_secrets(Conn *c)
{
	if (!agent_owner[0]) {
		Dev *d = wifi_dev();
		Active *a = d ? active_on(d) : NULL;
		if (a)
			drop_active(a);
		return;
	}
	DBusMsg *m = dbus_new_call(agent_owner, AGENT_PATH, AGENT_IF, "GetSecrets");
	add_settings(m, c, false);
	char *p = CONN_PATH(c);
	dbus_add(m, "os", p, "802-11-wireless-security");
	mem_free(p);
	dbus_open(m, "as");
	dbus_close(m);
	dbus_add(m, "u", 1u);   /* ALLOW_INTERACTION */
	SecretsWait *w = mem_alloc(sizeof *w);
	w->conn = c;
	dbus_call_async(Z.bus, m, on_secrets, w);
}

/* Starts a Wi-Fi connection: saved, given a password, open, or asking for one. */
static void activate_wifi(Conn *c, Ap *ap)
{
	Dev *d = wifi_dev();
	if (!d)
		return;
	if (!ap)
		ap = find_ap(c->hex);
	start_active(c, d, ap);
	d->reason = REASON_NONE;
	if (c->psk[0])
		znet_connect(c->hex, c->psk);
	else if ((ap && (ap->saved || !secured(ap))) || !c->unsaved)
		znet_connect(c->hex, NULL);
	else
		ask_secrets(c);
}

/* A connection from settings a program made (a{sa{sv}}). */
static Conn *conn_from_settings(const DBusVal *settings, Ap *ap)
{
	const DBusVal *wifi = dbus_dict_get(settings, "802-11-wireless");
	const DBusVal *ssid = dbus_unvariant(dbus_dict_get(wifi, "ssid"));
	char hex[65] = { 0 };
	if (ssid && ssid->type == 'a' && ssid->count && ssid->count <= 32) {
		u8 b[32];
		for (usize i = 0; i < ssid->count; i++)
			b[i] = (u8)dbus_uint(ssid->items[i], 0);
		hex_of(b, (int)ssid->count, hex);
	} else if (ap) {
		str_copy(hex, ap->hex, sizeof hex);
	} else {
		return NULL;
	}
	bool existed = find_conn_key(true, hex) != NULL;
	Conn *c = add_conn(true, hex);
	if (!c)
		return NULL;
	if (!existed) {
		c->unsaved = true;
		c->seen = true;
		char *p = CONN_PATH(c);
		dbus_export(Z.bus, p, CONN_IF, conn_method, c, NULL);
		interfaces_added(p, &I_CONN, NULL, c);
		signal_o(SETTINGS_PATH, SETTINGS_IF, "NewConnection", p);
		mem_free(p);
	}
	const char *psk = dbus_str(dbus_dict_get(dbus_dict_get(settings, "802-11-wireless-security"), "psk"), "");
	if (*psk)
		str_copy(c->psk, psk, sizeof c->psk);
	const char *id = dbus_str(dbus_dict_get(dbus_dict_get(settings, "connection"), "id"), "");
	if (*id)
		str_copy(c->id, id, sizeof c->id);
	return c;
}

static void reply_paths2(DBus *bus, DBusMsg *call, Conn *c, Active *a, bool options)
{
	DBusMsg *m = dbus_new_reply(call);
	char *cp = CONN_PATH(c);
	char *ap = a ? ACTIVE_PATH(a) : str_dup("/");
	dbus_add(m, "oo", cp, ap);
	if (options) {
		dbus_open(m, "a{sv}");
		dbus_close(m);
	}
	dbus_send(bus, m);
	mem_free(cp);
	mem_free(ap);
}

static bool permissions_reply(DBus *bus, DBusMsg *call)
{
	static const char *const yes[] = {
		"org.freedesktop.NetworkManager.enable-disable-network", "org.freedesktop.NetworkManager.enable-disable-wifi",
		"org.freedesktop.NetworkManager.network-control", "org.freedesktop.NetworkManager.settings.modify.own",
		"org.freedesktop.NetworkManager.settings.modify.system", "org.freedesktop.NetworkManager.settings.modify.hostname",
		"org.freedesktop.NetworkManager.wifi.scan", "org.freedesktop.NetworkManager.enable-disable-statistics",
		"org.freedesktop.NetworkManager.enable-disable-connectivity-check",
	};
	static const char *const no[] = {
		"org.freedesktop.NetworkManager.wifi.share.open", "org.freedesktop.NetworkManager.wifi.share.protected",
		"org.freedesktop.NetworkManager.enable-disable-wwan", "org.freedesktop.NetworkManager.sleep-wake",
		"org.freedesktop.NetworkManager.settings.modify.global-dns", "org.freedesktop.NetworkManager.reload",
		"org.freedesktop.NetworkManager.checkpoint-rollback",
	};
	DBusMsg *m = dbus_new_reply(call);
	dbus_open(m, "a{ss}");
	for (usize i = 0; i < ARRAY_LEN(yes); i++) {
		dbus_open(m, "{ss}");
		dbus_add(m, "ss", yes[i], "yes");
		dbus_close(m);
	}
	for (usize i = 0; i < ARRAY_LEN(no); i++) {
		dbus_open(m, "{ss}");
		dbus_add(m, "ss", no[i], "no");
		dbus_close(m);
	}
	dbus_close(m);
	dbus_send(bus, m);
	return true;
}

static bool nm_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, NM_IF, NM_PROPS, ARRAY_LEN(NM_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "GetDevices") || str_eq(mbr, "GetAllDevices")) {
		DBusMsg *m = dbus_new_reply(call);
		add_paths_devices(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "GetPermissions")) {
		return permissions_reply(bus, call);
	} else if (str_eq(mbr, "state")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "u", nm_state);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "CheckConnectivity")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "u", nm_state == NM_STATE_CONNECTED_GLOBAL ? 4u : 1u);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "ActivateConnection")) {
		Conn *c = conn_by_path(dbus_str(dbus_arg(call, 0), ""));
		Ap *ap = ap_by_path(dbus_str(dbus_arg(call, 2), ""));
		if (!c && ap)
			c = add_conn(true, ap->hex);
		if (!c) {
			dbus_reply_error(bus, call, "org.freedesktop.NetworkManager.UnknownConnection", "No such connection");
			return true;
		}
		Active *a = NULL;
		if (c->wifi) {
			activate_wifi(c, ap);
			Dev *d = wifi_dev();
			a = d ? active_on(d) : NULL;
		} else {
			Dev *d = find_dev(c->iface);
			a = d ? active_on(d) : NULL;
		}
		DBusMsg *m = dbus_new_reply(call);
		char *p = a ? ACTIVE_PATH(a) : str_dup("/");
		dbus_add(m, "o", p);
		mem_free(p);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "AddAndActivateConnection") || str_eq(mbr, "AddAndActivateConnection2")) {
		Ap *ap = ap_by_path(dbus_str(dbus_arg(call, 2), ""));
		Conn *c = conn_from_settings(dbus_arg(call, 0), ap);
		if (!c || !wifi_dev()) {
			dbus_reply_error(bus, call, "org.freedesktop.NetworkManager.InvalidConnection", "Only Wi-Fi connections can be added");
			return true;
		}
		activate_wifi(c, ap);
		reply_paths2(bus, call, c, active_on(wifi_dev()), str_eq(mbr, "AddAndActivateConnection2"));
	} else if (str_eq(mbr, "DeactivateConnection")) {
		Active *a = active_by_path(dbus_str(dbus_arg(call, 0), ""));
		if (a && a->conn->wifi) {
			znet("disconnect");
			a->dev->reason = REASON_USER;
		}
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "Enable") || str_eq(mbr, "Sleep") || str_eq(mbr, "SetLogging") || str_eq(mbr, "Reload")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "GetLogging")) {
		DBusMsg *m = dbus_new_reply(call);
		dbus_add(m, "ss", "INFO", "");
		dbus_send(bus, m);
	} else {
		return false;
	}
	return true;
}

static bool settings_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (props_call(bus, call, SETTINGS_IF, SETTINGS_PROPS, ARRAY_LEN(SETTINGS_PROPS), NULL))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "ListConnections")) {
		DBusMsg *m = dbus_new_reply(call);
		s_connections(m, NULL);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "GetConnectionByUuid")) {
		const char *uuid = dbus_str(dbus_arg(call, 0), "");
		for (usize i = 0; i < ARRAY_LEN(conns); i++)
			if (conns[i].used && str_eq(conns[i].uuid, uuid)) {
				DBusMsg *m = dbus_new_reply(call);
				add_path(m, CONN_PATH(&conns[i]));
				dbus_send(bus, m);
				return true;
			}
		dbus_reply_error(bus, call, "org.freedesktop.NetworkManager.Settings.InvalidConnection", "No such connection");
	} else if (str_eq(mbr, "AddConnection") || str_eq(mbr, "AddConnectionUnsaved") || str_eq(mbr, "AddConnection2")) {
		Conn *c = conn_from_settings(dbus_arg(call, 0), NULL);
		if (!c) {
			dbus_reply_error(bus, call, "org.freedesktop.NetworkManager.Settings.InvalidConnection", "Only Wi-Fi connections can be added");
			return true;
		}
		DBusMsg *m = dbus_new_reply(call);
		add_path(m, CONN_PATH(c));
		if (str_eq(mbr, "AddConnection2")) {
			dbus_open(m, "a{sv}");
			dbus_close(m);
		}
		dbus_send(bus, m);
	} else if (str_eq(mbr, "SaveHostname")) {
		dbus_reply_error(bus, call, "org.freedesktop.NetworkManager.Settings.NotSupported", "Use hostname1");
	} else if (str_eq(mbr, "ReloadConnections") || str_eq(mbr, "LoadConnections")) {
		DBusMsg *m = dbus_new_reply(call);
		if (str_eq(mbr, "ReloadConnections")) {
			dbus_add(m, "b", true);
		} else {
			dbus_add(m, "b", true);
			dbus_open(m, "as");
			dbus_close(m);
		}
		dbus_send(bus, m);
	} else {
		return false;
	}
	return true;
}

static bool conn_method(DBus *bus, DBusMsg *call, void *data)
{
	Conn *c = data;
	if (props_call(bus, call, CONN_IF, CONN_PROPS, ARRAY_LEN(CONN_PROPS), c))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "GetSettings")) {
		DBusMsg *m = dbus_new_reply(call);
		add_settings(m, c, false);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "GetSecrets")) {
		/* znet keeps only the key made from the password, never the password */
		DBusMsg *m = dbus_new_reply(call);
		dbus_open(m, "a{sa{sv}}");
		dbus_close(m);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "Update") || str_eq(mbr, "UpdateUnsaved") || str_eq(mbr, "Update2")) {
		const char *psk = dbus_str(dbus_dict_get(dbus_dict_get(dbus_arg(call, 0), "802-11-wireless-security"), "psk"), "");
		if (*psk)
			str_copy(c->psk, psk, sizeof c->psk);
		const char *id = dbus_str(dbus_dict_get(dbus_dict_get(dbus_arg(call, 0), "connection"), "id"), "");
		if (*id)
			str_copy(c->id, id, sizeof c->id);
		DBusMsg *m = dbus_new_reply(call);
		if (str_eq(mbr, "Update2")) {
			dbus_open(m, "a{sv}");
			dbus_close(m);
		}
		dbus_send(bus, m);
		char *p = CONN_PATH(c);
		dbus_send(bus, dbus_new_signal(p, CONN_IF, "Updated"));
		mem_free(p);
	} else if (str_eq(mbr, "Delete")) {
		if (!c->wifi) {
			dbus_reply_error(bus, call, "org.freedesktop.NetworkManager.Settings.Connection.NotSupported",
					 "Wired connections are automatic");
			return true;
		}
		char cmd[96];
		fmt(cmd, sizeof cmd, "forget %s", c->hex);
		znet(cmd);
		c->unsaved = false;
		c->seen = false;
		dbus_reply_empty(bus, call);
		refresh();
	} else if (str_eq(mbr, "Save") || str_eq(mbr, "ClearSecrets")) {
		dbus_reply_empty(bus, call);
	} else {
		return false;
	}
	return true;
}

static bool device_method(DBus *bus, DBusMsg *call, void *data)
{
	Dev *d = data;
	if (props_call(bus, call, DEVICE_IF, DEVICE_PROPS, ARRAY_LEN(DEVICE_PROPS), d) ||
	    props_call(bus, call, d->wifi ? WIRELESS_IF : WIRED_IF, d->wifi ? WIRELESS_PROPS : WIRED_PROPS,
		       d->wifi ? ARRAY_LEN(WIRELESS_PROPS) : ARRAY_LEN(WIRED_PROPS), d))
		return true;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "Disconnect")) {
		if (d->wifi) {
			znet("disconnect");
			d->reason = REASON_USER;
		}
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "RequestScan")) {
		znet("scan");
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "GetAccessPoints") || str_eq(mbr, "GetAllAccessPoints")) {
		DBusMsg *m = dbus_new_reply(call);
		w_aps(m, d);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "GetAppliedConnection")) {
		Active *a = active_on(d);
		if (!a) {
			dbus_reply_error(bus, call, "org.freedesktop.NetworkManager.Device.NotActive", "Not connected");
			return true;
		}
		DBusMsg *m = dbus_new_reply(call);
		add_settings(m, a->conn, false);
		dbus_add(m, "t", (u64)a->num);
		dbus_send(bus, m);
	} else if (str_eq(mbr, "Reapply") || str_eq(mbr, "SetManaged")) {
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "Delete")) {
		dbus_reply_error(bus, call, "org.freedesktop.NetworkManager.Device.NotSoftware", "A real adapter can't be deleted");
	} else {
		return false;
	}
	return true;
}

static bool ap_method(DBus *bus, DBusMsg *call, void *data)
{
	return props_call(bus, call, AP_IF, AP_PROPS, ARRAY_LEN(AP_PROPS), data);
}

static bool active_method(DBus *bus, DBusMsg *call, void *data)
{
	return props_call(bus, call, ACTIVE_IF, ACTIVE_PROPS, ARRAY_LEN(ACTIVE_PROPS), data);
}

static bool ip4_method(DBus *bus, DBusMsg *call, void *data)
{
	return props_call(bus, call, IP4_IF, IP4_PROPS, ARRAY_LEN(IP4_PROPS), data);
}

static bool agents_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	const char *mbr = dbus_member(call);
	if (str_eq(mbr, "Register") || str_eq(mbr, "RegisterWithCapabilities")) {
		str_copy(agent_owner, dbus_sender(call), sizeof agent_owner);
		dbus_reply_empty(bus, call);
	} else if (str_eq(mbr, "Unregister")) {
		if (str_eq(agent_owner, dbus_sender(call)))
			agent_owner[0] = 0;
		dbus_reply_empty(bus, call);
	} else {
		return false;
	}
	return true;
}

static bool om_method(DBus *bus, DBusMsg *call, void *data)
{
	(void)data;
	if (!str_eq(dbus_member(call), "GetManagedObjects"))
		return false;
	DBusMsg *m = dbus_new_reply(call);
	dbus_open(m, "a{oa{sa{sv}}}");
#define OBJECT(path_expr, ...)                                \
	do {                                                  \
		char *p_ = (path_expr);                       \
		dbus_open(m, "{oa{sa{sv}}}");                 \
		dbus_add(m, "o", p_);                         \
		dbus_open(m, "a{sa{sv}}");                    \
		__VA_ARGS__;                                  \
		dbus_close(m);                                \
		dbus_close(m);                                \
		mem_free(p_);                                 \
	} while (0)
	OBJECT(str_dup(NM_PATH), add_iface(m, &I_NM, NULL));
	OBJECT(str_dup(SETTINGS_PATH), add_iface(m, &I_SETTINGS, NULL));
	for (usize i = 0; i < ARRAY_LEN(devs); i++)
		if (devs[i].used)
			OBJECT(DEV_PATH(&devs[i]), add_iface(m, &I_DEVICE, &devs[i]);
			       add_iface(m, devs[i].wifi ? &I_WIRELESS : &I_WIRED, &devs[i]));
	for (usize i = 0; i < ARRAY_LEN(aps); i++)
		if (aps[i].used)
			OBJECT(AP_PATH(&aps[i]), add_iface(m, &I_AP, &aps[i]));
	for (usize i = 0; i < ARRAY_LEN(conns); i++)
		if (conns[i].used)
			OBJECT(CONN_PATH(&conns[i]), add_iface(m, &I_CONN, &conns[i]));
	for (usize i = 0; i < ARRAY_LEN(actives); i++)
		if (actives[i].used) {
			OBJECT(ACTIVE_PATH(&actives[i]), add_iface(m, &I_ACTIVE, &actives[i]));
			if (actives[i].state == AC_ACTIVATED)
				OBJECT(IP4_PATH(&actives[i]), add_iface(m, &I_IP4, &actives[i]));
		}
#undef OBJECT
	dbus_close(m);
	dbus_send(bus, m);
	return true;
}

/* ---- Keeping up with znet ------------------------------------------------------------------------ */

static void device_state(Dev *d, u32 state, u32 reason)
{
	if (d->state == state)
		return;
	u32 old = d->state;
	d->state = state;
	if (reason)
		d->reason = reason;
	char *p = DEV_PATH(d);
	DBusMsg *m = dbus_new_signal(p, DEVICE_IF, "StateChanged");
	dbus_add(m, "uuu", state, old, d->reason);
	dbus_send(Z.bus, m);
	mem_free(p);
}

static void refresh(void)
{
	read_status();
	read_devices();
	read_networks();
	read_connections();

	/* devices that went away, and new ones */
	for (usize i = 0; i < ARRAY_LEN(devs); i++) {
		Dev *d = &devs[i];
		if (!d->used)
			continue;
		char *p = DEV_PATH(d);
		if (!d->seen) {
			Active *a = active_on(d);
			if (a)
				drop_active(a);
			dbus_unexport(Z.bus, p, DEVICE_IF);
			dbus_unexport(Z.bus, p, d->wifi ? WIRELESS_IF : WIRED_IF);
			interfaces_removed(p, DEVICE_IF, d->wifi ? WIRELESS_IF : WIRED_IF);
			signal_o(NM_PATH, NM_IF, "DeviceRemoved", p);
			*d = (Dev){ 0 };
		} else if (!d->hash[0]) {
			d->state = DEV_DISCONNECTED;
			dbus_export(Z.bus, p, DEVICE_IF, device_method, d, NULL);
			dbus_export(Z.bus, p, d->wifi ? WIRELESS_IF : WIRED_IF, device_method, d, NULL);
			d->hash[0] = props_hash(&I_DEVICE, d);
			d->hash[1] = props_hash(d->wifi ? &I_WIRELESS : &I_WIRED, d);
			interfaces_added(p, &I_DEVICE, d->wifi ? &I_WIRELESS : &I_WIRED, d);
			signal_o(NM_PATH, NM_IF, "DeviceAdded", p);
		}
		mem_free(p);
	}
	Dev *wd = wifi_dev();

	/* access points */
	for (usize i = 0; i < ARRAY_LEN(aps); i++) {
		Ap *a = &aps[i];
		if (!a->used)
			continue;
		char *p = AP_PATH(a);
		if (!a->seen) {
			for (usize k = 0; k < ARRAY_LEN(actives); k++)
				if (actives[k].ap == a)
					actives[k].ap = NULL;
			dbus_unexport(Z.bus, p, AP_IF);
			interfaces_removed(p, AP_IF, NULL);
			if (wd) {
				char *dp = DEV_PATH(wd);
				signal_o(dp, WIRELESS_IF, "AccessPointRemoved", p);
				mem_free(dp);
			}
			*a = (Ap){ 0 };
		} else if (!a->hash) {
			dbus_export(Z.bus, p, AP_IF, ap_method, a, NULL);
			a->hash = props_hash(&I_AP, a);
			interfaces_added(p, &I_AP, NULL, a);
			if (wd) {
				char *dp = DEV_PATH(wd);
				signal_o(dp, WIRELESS_IF, "AccessPointAdded", p);
				mem_free(dp);
			}
		} else {
			sync_props(p, &I_AP, a, &a->hash);
		}
		mem_free(p);
	}

	/* connections */
	for (usize i = 0; i < ARRAY_LEN(conns); i++) {
		Conn *c = &conns[i];
		if (!c->used)
			continue;
		char *p = CONN_PATH(c);
		if (!c->seen) {
			for (usize k = 0; k < ARRAY_LEN(actives); k++)
				if (actives[k].used && actives[k].conn == c)
					drop_active(&actives[k]);
			dbus_unexport(Z.bus, p, CONN_IF);
			interfaces_removed(p, CONN_IF, NULL);
			dbus_send(Z.bus, dbus_new_signal(p, CONN_IF, "Removed"));
			signal_o(SETTINGS_PATH, SETTINGS_IF, "ConnectionRemoved", p);
			memset(c->psk, 0, sizeof c->psk);
			*c = (Conn){ 0 };
		} else if (!c->hash) {
			dbus_export(Z.bus, p, CONN_IF, conn_method, c, NULL);
			c->hash = props_hash(&I_CONN, c);
			interfaces_added(p, &I_CONN, NULL, c);
			signal_o(SETTINGS_PATH, SETTINGS_IF, "NewConnection", p);
		} else {
			sync_props(p, &I_CONN, c, &c->hash);
		}
		mem_free(p);
	}

	/* what is active, from znet's state */
	for (usize i = 0; i < ARRAY_LEN(devs); i++) {
		Dev *d = &devs[i];
		if (!d->used)
			continue;
		Active *a = active_on(d);
		bool up = st.connected && str_eq(st.iface, d->name);
		if (d->wifi) {
			bool connecting = str_eq(st.wifi_state, "connecting") || (str_eq(st.wifi_state, "connected") && !up);
			if (!radio_on) {
				if (a)
					drop_active(a);
				device_state(d, DEV_UNAVAILABLE, REASON_NONE);
				continue;
			}
			if (up || connecting) {
				Conn *c = st.wifi_hex[0] ? add_conn(true, st.wifi_hex) : NULL;
				if (c && (!a || a->conn != c)) {
					if (!c->hash) {
						char *p = CONN_PATH(c);
						c->seen = true;
						dbus_export(Z.bus, p, CONN_IF, conn_method, c, NULL);
						c->hash = props_hash(&I_CONN, c);
						interfaces_added(p, &I_CONN, NULL, c);
						mem_free(p);
					}
					a = start_active(c, d, find_ap(st.wifi_hex));
				}
				if (a) {
					a->state = up ? AC_ACTIVATED : AC_ACTIVATING;
					if (up)
						a->conn->unsaved = false;
				}
				device_state(d, up ? DEV_ACTIVATED : str_eq(st.wifi_state, "connected") ? DEV_IP_CONFIG : DEV_CONFIG, REASON_NONE);
			} else {
				/* not connected: a failed attempt says why */
				bool failed = a && a->state == AC_ACTIVATING && st.wifi_error[0];
				if (a && (a->state == AC_ACTIVATED || failed || !str_eq(st.wifi_state, "idle") || d->reason == REASON_USER))
					drop_active(a);
				else if (a && a->state == AC_ACTIVATING && !st.wifi_error[0])
					continue;   /* waiting for the password, or znet hasn't started yet */
				if (failed) {
					device_state(d, DEV_FAILED, str_find(st.wifi_error, "password") ? REASON_NO_SECRETS : REASON_SUPPLICANT_FAILED);
				} else {
					device_state(d, DEV_DISCONNECTED, REASON_NONE);
				}
			}
		} else {
			Conn *c = find_conn_key(false, d->name);
			if (up && c) {
				if (!a)
					a = start_active(c, d, NULL);
				if (a)
					a->state = AC_ACTIVATED;
				device_state(d, DEV_ACTIVATED, REASON_NONE);
			} else {
				if (a)
					drop_active(a);
				device_state(d, d->carrier ? (st.connecting ? DEV_IP_CONFIG : DEV_DISCONNECTED) : DEV_UNAVAILABLE,
					     d->carrier ? REASON_NONE : REASON_CARRIER);
			}
		}
	}

	/* the rest: properties that changed */
	for (usize i = 0; i < ARRAY_LEN(devs); i++)
		if (devs[i].used) {
			char *p = DEV_PATH(&devs[i]);
			sync_props(p, &I_DEVICE, &devs[i], &devs[i].hash[0]);
			sync_props(p, devs[i].wifi ? &I_WIRELESS : &I_WIRED, &devs[i], &devs[i].hash[1]);
			mem_free(p);
		}
	for (usize i = 0; i < ARRAY_LEN(actives); i++)
		if (actives[i].used) {
			char *p = ACTIVE_PATH(&actives[i]), *ip = IP4_PATH(&actives[i]);
			u32 before = actives[i].hash[0] ? 1 : 0;
			sync_props(p, &I_ACTIVE, &actives[i], &actives[i].hash[0]);
			if (actives[i].state == AC_ACTIVATED) {
				if (!actives[i].hash[1]) {
					actives[i].hash[1] = props_hash(&I_IP4, &actives[i]);
					interfaces_added(ip, &I_IP4, NULL, &actives[i]);
				} else {
					sync_props(ip, &I_IP4, &actives[i], &actives[i].hash[1]);
				}
			}
			(void)before;
			mem_free(p);
			mem_free(ip);
		}
	u32 old_state = nm_state;
	nm_state = primary() ? NM_STATE_CONNECTED_GLOBAL : activating() ? NM_STATE_CONNECTING : NM_STATE_DISCONNECTED;
	if (old_state != nm_state) {
		DBusMsg *m = dbus_new_signal(NM_PATH, NM_IF, "StateChanged");
		dbus_add(m, "u", nm_state);
		dbus_send(Z.bus, m);
	}
	sync_props(NM_PATH, &I_NM, NULL, &root_hash);
	sync_props(SETTINGS_PATH, &I_SETTINGS, NULL, &settings_hash);
}

static i64 status_mtime, networks_mtime;

static void tick(Loop *loop, void *data)
{
	(void)loop, (void)data;
	i64 a = file_mtime(STATUS_FILE), b = file_mtime(NETWORKS_FILE);
	static u64 last_full;
	/* a full look every few seconds catches cables and adapters; at once when znet writes */
	if (a != status_mtime || b != networks_mtime || time_mono_ms() - last_full > 3000) {
		status_mtime = a, networks_mtime = b;
		last_full = time_mono_ms();
		refresh();
	}
}

static void on_name_owner(DBus *bus, DBusMsg *sig, void *data)
{
	(void)bus, (void)data;
	const char *name = dbus_str(dbus_arg(sig, 0), ""), *now = dbus_str(dbus_arg(sig, 2), "");
	if (!*now && agent_owner[0] && str_eq(name, agent_owner))
		agent_owner[0] = 0;
}

void network_start(DBus *bus)
{
	dbus_export(bus, OM_PATH, OM_IF, om_method, NULL, NULL);
	dbus_export(bus, NM_PATH, NM_IF, nm_method, NULL, NULL);
	dbus_export(bus, SETTINGS_PATH, SETTINGS_IF, settings_method, NULL, NULL);
	dbus_export(bus, AGENTS_PATH, AGENTS_IF, agents_method, NULL, NULL);
	refresh();
	root_hash = props_hash(&I_NM, NULL);
	settings_hash = props_hash(&I_SETTINGS, NULL);
	loop_timer(Z.loop, 500, 500, tick, NULL);
	dbus_subscribe(bus, "org.freedesktop.DBus", NULL, "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner, NULL);
	dbus_request_name(bus, NM, DBUS_NAME_NO_QUEUE);
}
