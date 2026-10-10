/*
 * Wi-Fi: scanning, joining networks and WPA2-Personal, through nl80211.
 *
 * The kernel's Wi-Fi layer finds networks and associates; znet does the
 * WPA2 4-way handshake itself over an EAPOL socket and hands the resulting
 * keys to the kernel. Saved networks keep only the derived key (PMK), never
 * the password, in /data/system/wifi, readable by root alone.
 *
 * Settings and the quick settings talk to it over /run/zurbax/wifi.sock:
 *     scan | connect SSIDHEX [PASSHEX] | disconnect | forget SSIDHEX | radio on|off
 * and read what it finds from /run/zurbax/wifi.networks, one line per network:
 *     signal(0-100) TAB security TAB saved TAB connected TAB SSIDHEX
 */
#include "znet.h"
#include "nl.h"
#include "wpa.h"

#define WIFI_DIR "/data/system/wifi"
#define NETWORKS_FILE "/run/zurbax/wifi.networks"
#define CTL_SOCKET "/run/zurbax/wifi.sock"

enum { SEC_OPEN, SEC_WPA2, SEC_WPA3, SEC_WPA, SEC_WEP, SEC_ENTERPRISE };
static const char *const SEC_NAMES[] = { "open", "wpa2", "wpa3", "wpa", "wep", "enterprise" };

enum { WS_OFF, WS_IDLE, WS_CONNECTING, WS_HANDSHAKE, WS_CONNECTED };

typedef struct {
	u8 ssid[32];
	int ssid_len;
	u8 bssid[6];
	u32 freq;
	int signal;              /* dBm */
	int sec;
	u32 group;               /* group cipher, WPA2 */
	u8 rsn[64];              /* the AP's RSN element, checked again in message 3 */
	int rsn_len;
	bool associated;
} Net;

static struct {
	bool present;
	Genl cmd, ev;
	Iface *iface;
	bool radio;
	int state;
	Net nets[64];
	int nnets;
	bool scanning;
	u64 scan_at, last_scan, last_dump;
	/* the network being joined */
	Net target;
	u8 pmk[32];
	bool save;               /* remember it once the handshake works */
	int eapol;
	u8 anonce[32], snonce[32];
	Ptk ptk;
	bool sent_msg2, have_msg3;
	int msg1_count;          /* an AP that keeps starting over didn't like our answer */
	u8 replay[8];
	bool replay_set;
	u64 deadline;
	char error[160];
	bool user_off;           /* disconnected on purpose: don't rejoin by ourselves */
	u8 failed_ssid[32];
	int failed_len;
	u64 failed_until;
	int ctl;
	char last_list[4096];
} W = { .eapol = -1, .ctl = -1 };

/* ---- Small helpers ------------------------------------------------------------------------ */

static void ssid_hex(const u8 *ssid, int len, char *out)
{
	hex_encode(ssid, (usize)len, out);
}

static int signal_pct(int dbm)
{
	return CLAMP((dbm + 90) * 100 / 60, 0, 100);
}

static const char *ssid_text(const Net *n)
{
	static char buf[64];
	usize w = 0;
	for (int i = 0; i < n->ssid_len && w < sizeof buf - 1; i++)
		buf[w++] = n->ssid[i] >= 0x20 && n->ssid[i] != 0x7f ? (char)n->ssid[i] : '?';
	buf[w] = 0;
	return buf;
}

static bool same_ssid(const Net *n, const u8 *ssid, int len)
{
	return n->ssid_len == len && memcmp(n->ssid, ssid, (usize)len) == 0;
}

static char *saved_path(const u8 *ssid, int len)
{
	char hex[65];
	ssid_hex(ssid, len, hex);
	return str_fmt(WIFI_DIR "/%s.conf", hex);
}

static bool is_saved(const u8 *ssid, int len)
{
	char *p = saved_path(ssid, len);
	bool ok = file_exists(p);
	mem_free(p);
	return ok;
}

static bool load_pmk(const u8 *ssid, int len, u8 pmk[32])
{
	char *p = saved_path(ssid, len);
	Conf *c = conf_load(p);
	mem_free(p);
	const char *h = conf_get(c, "pmk", "");
	bool ok = strlen(h) == 64 && hex_decode(h, pmk, 32);
	conf_free(c);
	return ok;
}

static void save_network(const Net *n, const u8 *pmk)
{
	dir_make(WIFI_DIR, 0700);
	char *p = saved_path(n->ssid, n->ssid_len);
	char hex[65] = "";
	if (pmk)
		hex_encode(pmk, 32, hex);
	char *body = str_fmt("# %s\nsecurity = %s\n%s%s%s", ssid_text(n), SEC_NAMES[n->sec], pmk ? "pmk = " : "", hex, pmk ? "\n" : "");
	char *tmp = str_fmt("%s.tmp", p);
	int fd = (int)sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd >= 0) {
		write_all(fd, body, strlen(body));
		sys_close(fd);
		sys_rename(tmp, p);
	}
	mem_free(tmp);
	mem_free(body);
	mem_free(p);
}

static void forget_network(const u8 *ssid, int len)
{
	char *p = saved_path(ssid, len);
	sys_unlink(p);
	mem_free(p);
}

static void radio_save(void)
{
	dir_make(WIFI_DIR, 0700);
	file_write_str(WIFI_DIR "/radio", W.radio ? "on\n" : "off\n");
}

/* Soft-blocks or unblocks every Wi-Fi radio (laptops often start blocked). */
static void rfkill(bool block)
{
	int fd = (int)sys_open("/dev/rfkill", O_WRONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return;
	u8 ev[8] = { 0, 0, 0, 0, 1 /* WLAN */, 3 /* change all */, block, 0 };
	sys_write(fd, ev, sizeof ev);
	sys_close(fd);
}

/* ---- nl80211 requests ----------------------------------------------------------------------- */

static int ifindex(void)
{
	return W.iface ? W.iface->index : 0;
}

static void trigger_scan(void)
{
	if (!W.present || !W.radio || !W.iface || W.scanning)
		return;
	NlMsg m;
	nlm_begin(&m, W.cmd.family, 0, NL80211_CMD_TRIGGER_SCAN);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)ifindex());
	usize at = nlm_nest(&m, NL80211_ATTR_SCAN_SSIDS);
	nlm_put(&m, 1, NULL, 0);   /* the wildcard SSID: an active scan */
	nlm_nest_end(&m, at);
	int r = genl_call(&W.cmd, &m, NULL, NULL);
	if (r == 0 || r == -EBUSY) {
		W.scanning = true;
		W.scan_at = time_mono_ms();
	} else {
		log_msg("scan failed: %d", r);
	}
	W.last_scan = time_mono_ms();
}

static void parse_ies(Net *n, const u8 *ies, usize len, u16 capability)
{
	const u8 *e = wpa_find_ie(ies, len, 0);
	if (e) {
		n->ssid_len = MIN(e[1], 32);
		memcpy(n->ssid, e + 2, (usize)n->ssid_len);
	}
	const u8 *rsn = wpa_find_ie(ies, len, 0x30);
	n->sec = SEC_OPEN;
	if (rsn) {
		Rsn r;
		n->sec = SEC_ENTERPRISE;
		if (rsn_parse(rsn + 2, rsn[1], &r)) {
			bool psk = false, sae = false, ccmp = false;
			for (int i = 0; i < r.nakm; i++) {
				psk |= r.akm[i] == SUITE(AKM_PSK);
				sae |= r.akm[i] == SUITE(AKM_SAE);
			}
			for (int i = 0; i < r.npairwise; i++)
				ccmp |= r.pairwise[i] == SUITE(CIPHER_CCMP);
			bool group_ok = r.group == SUITE(CIPHER_CCMP) || r.group == SUITE(CIPHER_TKIP);
			bool mfp_required = r.caps & 0x0040;
			if (psk && ccmp && group_ok && !mfp_required)
				n->sec = SEC_WPA2;
			else if (sae || psk)
				n->sec = SEC_WPA3;
			n->group = r.group;
		}
		n->rsn_len = MIN(rsn[1] + 2, (int)sizeof n->rsn);
		memcpy(n->rsn, rsn, (usize)n->rsn_len);
	} else {
		/* WPA (version 1) is a vendor element; WEP only sets the privacy bit */
		for (usize off = 0; off + 2 <= len; off += 2u + ies[off + 1]) {
			if (ies[off] == 0xdd && ies[off + 1] >= 4 && off + 6 <= len && ies[off + 2] == 0x00 && ies[off + 3] == 0x50 &&
			    ies[off + 4] == 0xf2 && ies[off + 5] == 1)
				n->sec = SEC_WPA;
		}
		if (n->sec == SEC_OPEN && (capability & 0x10))
			n->sec = SEC_WEP;
	}
}

static void scan_cb(void *user, u8 cmd, const u8 *attrs, usize n)
{
	NlAttr tb[NL80211_ATTR_BSS + 1], bss[NL80211_BSS_BEACON_IES + 1];
	nla_parse(attrs, n, tb, NL80211_ATTR_BSS);
	if (!tb[NL80211_ATTR_BSS].present || W.nnets >= (int)ARRAY_LEN(W.nets))
		return;
	nla_parse(tb[NL80211_ATTR_BSS].data, tb[NL80211_ATTR_BSS].len, bss, NL80211_BSS_BEACON_IES);
	if (!bss[NL80211_BSS_BSSID].present || bss[NL80211_BSS_BSSID].len != 6)
		return;
	Net net = { 0 };
	memcpy(net.bssid, bss[NL80211_BSS_BSSID].data, 6);
	net.freq = nla_u32(&bss[NL80211_BSS_FREQUENCY]);
	net.signal = bss[NL80211_BSS_SIGNAL_MBM].present ? (int)(i32)nla_u32(&bss[NL80211_BSS_SIGNAL_MBM]) / 100 : -80;
	net.associated = bss[NL80211_BSS_STATUS].present && nla_u32(&bss[NL80211_BSS_STATUS]) == NL80211_BSS_STATUS_ASSOCIATED;
	const NlAttr *ies = bss[NL80211_BSS_INFORMATION_ELEMENTS].present ? &bss[NL80211_BSS_INFORMATION_ELEMENTS] : &bss[NL80211_BSS_BEACON_IES];
	if (ies->present)
		parse_ies(&net, ies->data, ies->len, nla_u16(&bss[NL80211_BSS_CAPABILITY]));
	/* hidden networks (no name, or zeros) aren't listed */
	bool named = false;
	for (int i = 0; i < net.ssid_len; i++)
		named |= net.ssid[i] != 0;
	if (!named)
		return;
	/* one entry per name: the strongest access point, or the one we're on */
	for (int i = 0; i < W.nnets; i++) {
		Net *o = &W.nets[i];
		if (same_ssid(o, net.ssid, net.ssid_len)) {
			if (net.associated || (!o->associated && net.signal > o->signal))
				*o = net;
			return;
		}
	}
	W.nets[W.nnets++] = net;
}

static void sort_nets(void)
{
	for (int i = 1; i < W.nnets; i++) {
		Net t = W.nets[i];
		int j = i - 1;
		while (j >= 0 && W.nets[j].signal < t.signal) {
			W.nets[j + 1] = W.nets[j];
			j--;
		}
		W.nets[j + 1] = t;
	}
}

static void write_networks(void)
{
	StrBuf sb = { 0 };
	for (int i = 0; i < W.nnets; i++) {
		Net *n = &W.nets[i];
		char hex[65];
		ssid_hex(n->ssid, n->ssid_len, hex);
		bool on = (W.state == WS_CONNECTED || W.state == WS_HANDSHAKE || W.state == WS_CONNECTING) && same_ssid(&W.target, n->ssid, n->ssid_len);
		sb_fmt(&sb, "%d\t%s\t%d\t%d\t%s\n", signal_pct(n->signal), SEC_NAMES[n->sec], is_saved(n->ssid, n->ssid_len), on, hex);
	}
	if (!str_eq(sb_str(&sb), W.last_list)) {
		str_copy(W.last_list, sb_str(&sb), sizeof W.last_list);
		file_write_atomic(NETWORKS_FILE, sb.data, sb.len);
	}
	sb_free(&sb);
}

static void connect_to(Net *n, const char *pass);

static void dump_scan(void)
{
	if (!W.iface)
		return;
	W.nnets = 0;
	NlMsg m;
	nlm_begin(&m, W.cmd.family, NLM_F_DUMP, NL80211_CMD_GET_SCAN);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)ifindex());
	genl_call(&W.cmd, &m, scan_cb, NULL);
	sort_nets();
	static int last_count = -1;
	if (W.nnets != last_count) {
		log_msg("%d Wi-Fi network%s in range", W.nnets, W.nnets == 1 ? "" : "s");
		last_count = W.nnets;
	}
	W.last_dump = time_mono_ms();
	/* keep the signal of the network we're on fresh */
	for (int i = 0; i < W.nnets; i++)
		if (W.nets[i].associated && same_ssid(&W.target, W.nets[i].ssid, W.nets[i].ssid_len))
			W.target.signal = W.nets[i].signal;
	write_networks();
	/* join a saved network we can see */
	if (W.state != WS_IDLE || !W.radio || W.user_off)
		return;
	u64 now = time_mono_ms();
	for (int i = 0; i < W.nnets; i++) {
		Net *n = &W.nets[i];
		if (!is_saved(n->ssid, n->ssid_len) || (n->sec != SEC_OPEN && n->sec != SEC_WPA2))
			continue;
		if (now < W.failed_until && n->ssid_len == W.failed_len && memcmp(n->ssid, W.failed_ssid, (usize)n->ssid_len) == 0)
			continue;
		log_msg("joining saved network %s", ssid_text(n));
		connect_to(n, NULL);
		return;
	}
}

static int new_key(const u8 *mac, const u8 *key, usize klen, u32 cipher, int idx, const u8 *seq)
{
	NlMsg m;
	nlm_begin(&m, W.cmd.family, 0, NL80211_CMD_NEW_KEY);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)ifindex());
	if (mac)
		nlm_put(&m, NL80211_ATTR_MAC, mac, 6);
	nlm_put(&m, NL80211_ATTR_KEY_DATA, key, klen);
	nlm_u32(&m, NL80211_ATTR_KEY_CIPHER, cipher);
	nlm_u8(&m, NL80211_ATTR_KEY_IDX, (u8)idx);
	nlm_u32(&m, NL80211_ATTR_KEY_TYPE, mac ? NL80211_KEYTYPE_PAIRWISE : NL80211_KEYTYPE_GROUP);
	if (seq)
		nlm_put(&m, NL80211_ATTR_KEY_SEQ, seq, 6);
	int r = genl_call(&W.cmd, &m, NULL, NULL);
	if (r < 0)
		log_msg("installing a key failed: %d", r);
	return r;
}

static void disconnect(void)
{
	if (!W.iface)
		return;
	NlMsg m;
	nlm_begin(&m, W.cmd.family, 0, NL80211_CMD_DISCONNECT);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)ifindex());
	nlm_u16(&m, NL80211_ATTR_REASON_CODE, 3);   /* leaving */
	genl_call(&W.cmd, &m, NULL, NULL);
}

static void set_state(int s)
{
	W.state = s;
	write_networks();
}

static void fail(const char *why, bool wrong_password)
{
	log_msg("couldn't join %s: %s", ssid_text(&W.target), why);
	str_copy(W.error, why, sizeof W.error);
	/* a saved network that refuses us isn't retried for a while */
	memcpy(W.failed_ssid, W.target.ssid, 32);
	W.failed_len = W.target.ssid_len;
	W.failed_until = time_mono_ms() + (wrong_password ? 10 * 60 * 1000 : 60 * 1000);
	disconnect();
	set_state(W.radio ? WS_IDLE : WS_OFF);
}

static bool open_eapol(void)
{
	if (W.eapol >= 0)
		sys_close(W.eapol);
	W.eapol = (int)sys_socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_PAE));
	if (W.eapol < 0)
		return false;
	struct sockaddr_ll sa = { .family = AF_PACKET, .protocol = htons(ETH_P_PAE), .ifindex = ifindex() };
	return sys_bind(W.eapol, &sa, sizeof sa) == 0;
}

static void connect_to(Net *n, const char *pass)
{
	if (!W.present || !W.iface || !W.radio)
		return;
	if (W.state != WS_IDLE)
		disconnect();
	W.error[0] = 0;
	W.target = *n;
	W.save = false;
	if (n->sec == SEC_WPA2) {
		if (pass && *pass) {
			usize pl = strlen(pass);
			bool hex = pl == 64;
			for (usize i = 0; hex && i < 64; i++)
				hex = is_xdigit(pass[i]);
			if (hex)
				hex_decode(pass, W.pmk, 32);   /* the key itself */
			else if (pl >= 8 && pl <= 63)
				wpa_pmk(pass, n->ssid, (usize)n->ssid_len, W.pmk);
			else {
				str_copy(W.error, "Wi-Fi passwords are 8 to 63 characters long.", sizeof W.error);
				write_networks();
				return;
			}
			W.save = true;
		} else if (!load_pmk(n->ssid, n->ssid_len, W.pmk)) {
			str_copy(W.error, "This network needs a password.", sizeof W.error);
			write_networks();
			return;
		}
		rand_bytes(W.snonce, 32);
		W.sent_msg2 = W.have_msg3 = W.replay_set = false;
		W.msg1_count = 0;
		if (!open_eapol()) {
			fail("couldn't open the security channel", false);
			return;
		}
	} else if (n->sec == SEC_OPEN) {
		W.save = !is_saved(n->ssid, n->ssid_len);
	} else {
		static const char *const why[] = { "", "", "WPA3-only networks aren't supported yet.", "Old WPA networks aren't supported.",
						   "WEP networks aren't supported: they aren't safe.",
						   "Enterprise (802.1X) networks aren't supported yet." };
		str_copy(W.error, why[n->sec], sizeof W.error);
		write_networks();
		return;
	}
	NlMsg m;
	nlm_begin(&m, W.cmd.family, 0, NL80211_CMD_CONNECT);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)ifindex());
	nlm_put(&m, NL80211_ATTR_SSID, n->ssid, (usize)n->ssid_len);
	nlm_put(&m, NL80211_ATTR_MAC, n->bssid, 6);
	if (n->freq)
		nlm_u32(&m, NL80211_ATTR_WIPHY_FREQ, n->freq);
	nlm_u32(&m, NL80211_ATTR_AUTH_TYPE, 0);   /* open system */
	if (n->sec == SEC_WPA2) {
		u8 ie[32];
		usize il = rsn_element(ie, n->group);
		nlm_flag(&m, NL80211_ATTR_PRIVACY);
		nlm_u32(&m, NL80211_ATTR_WPA_VERSIONS, NL80211_WPA_VERSION_2);
		nlm_u32(&m, NL80211_ATTR_CIPHER_SUITES_PAIRWISE, SUITE(CIPHER_CCMP));
		nlm_u32(&m, NL80211_ATTR_CIPHER_SUITE_GROUP, n->group);
		nlm_u32(&m, NL80211_ATTR_AKM_SUITES, SUITE(AKM_PSK));
		nlm_put(&m, NL80211_ATTR_IE, ie, il);
	}
	int r = genl_call(&W.cmd, &m, NULL, NULL);
	if (r < 0 && r != -EALREADY) {
		char why[80];
		fmt(why, sizeof why, "the adapter refused to connect (%d)", r);
		fail(why, false);
		return;
	}
	log_msg("connecting to %s", ssid_text(n));
	W.user_off = false;
	W.deadline = time_mono_ms() + 12000;
	set_state(WS_CONNECTING);
}

/* ---- The 4-way handshake --------------------------------------------------------------------- */

static void send_eapol(const u8 *frame, usize n)
{
	struct sockaddr_ll to = { .family = AF_PACKET, .protocol = htons(ETH_P_PAE), .ifindex = ifindex(), .halen = 6 };
	memcpy(to.addr, W.target.bssid, 6);
	if (sys_sendto(W.eapol, frame, n, 0, &to, sizeof to) < 0)
		log_msg("sending a handshake frame failed");
}

static bool replay_newer(const u8 r[8])
{
	return !W.replay_set || memcmp(r, W.replay, 8) > 0;
}

static void install_gtk(const u8 *gtk, usize len, int id, const u8 *rsc)
{
	u8 key[32];
	memcpy(key, gtk, len);
	if (W.target.group == SUITE(CIPHER_TKIP) && len == 32) {
		/* the station's Michael keys are the other way round */
		memcpy(key + 16, gtk + 24, 8);
		memcpy(key + 24, gtk + 16, 8);
	}
	new_key(NULL, key, len, W.target.group, id, rsc);
}

static void handle_eapol(void)
{
	u8 frame[2048];
	struct sockaddr_ll from;
	u32 flen = sizeof from;
	for (;;) {
		long n = sys_recvfrom(W.eapol, frame, sizeof frame, 0, &from, &flen);
		if (n <= 0)
			return;
		EapolKey k;
		if (!eapol_key_parse(frame, (usize)n, &k) || memcmp(from.addr, W.target.bssid, 6) != 0)
			continue;
		if ((k.info & KI_VER_MASK) != KI_VER_SHA1_AES || !(k.info & KI_ACK))
			continue;
		u8 out[512];
		if ((k.info & KI_PAIRWISE) && !(k.info & KI_MIC)) {
			/* message 1: the AP's nonce; answer with ours and our RSN element */
			if (W.state != WS_HANDSHAKE && W.state != WS_CONNECTING)
				continue;
			if (++W.msg1_count > 3) {
				fail("Wrong password?", true);
				return;
			}
			memcpy(W.anonce, k.nonce, 32);
			wpa_ptk(W.pmk, W.target.bssid, W.iface->mac, W.anonce, W.snonce, &W.ptk);
			EapolKey r = { .version = k.version, .info = KI_VER_SHA1_AES | KI_PAIRWISE | KI_MIC };
			memcpy(r.replay, k.replay, 8);
			memcpy(r.nonce, W.snonce, 32);
			u8 ie[32];
			usize il = rsn_element(ie, W.target.group);
			usize len = eapol_key_build(out, sizeof out, &r, ie, il, W.ptk.kck);
			send_eapol(out, len);
			if (!W.sent_msg2)
				W.deadline = time_mono_ms() + 5000;   /* retries don't buy more time */
			W.sent_msg2 = true;
			W.state = WS_HANDSHAKE;
		} else if ((k.info & KI_PAIRWISE) && (k.info & KI_MIC) && (k.info & KI_INSTALL)) {
			/* message 3: check it, take the group key, confirm, install */
			if (!W.sent_msg2 || memcmp(k.nonce, W.anonce, 32) != 0 || !eapol_key_mic_ok(frame, (usize)n, W.ptk.kck) ||
			    !replay_newer(k.replay))
				continue;
			memcpy(W.replay, k.replay, 8);
			W.replay_set = true;
			u8 data[512];
			usize dlen = 0;
			if (k.info & KI_ENCRYPTED) {
				if (!wpa_unwrap(W.ptk.kek, k.data, k.data_len, data, &dlen)) {
					fail("the network's keys didn't decrypt", false);
					return;
				}
			} else {
				continue;   /* WPA2 always encrypts message 3's key data */
			}
			/* the RSN element must be the one the AP advertised (no downgrade) */
			const u8 *rsn = wpa_find_ie(data, dlen, 0x30);
			if (!rsn || W.target.rsn_len != rsn[1] + 2 || memcmp(rsn, W.target.rsn, (usize)W.target.rsn_len) != 0) {
				fail("the network's security settings didn't match its announcement", false);
				return;
			}
			u8 gtk[32];
			usize gl;
			int gid;
			bool have_gtk = wpa_find_gtk(data, dlen, gtk, &gl, &gid);
			EapolKey r = { .version = k.version, .info = KI_VER_SHA1_AES | KI_PAIRWISE | KI_MIC | KI_SECURE };
			memcpy(r.replay, k.replay, 8);
			usize len = eapol_key_build(out, sizeof out, &r, NULL, 0, W.ptk.kck);
			send_eapol(out, len);
			/* message 4 must leave unencrypted, before the key is in place */
			sleep_ms(20);
			new_key(W.target.bssid, W.ptk.tk, 16, SUITE(CIPHER_CCMP), 0, NULL);
			if (have_gtk)
				install_gtk(gtk, gl, gid, k.rsc);
			W.have_msg3 = true;
			log_msg("joined %s", ssid_text(&W.target));
			if (W.save)
				save_network(&W.target, W.pmk);
			W.failed_len = 0;
			set_state(WS_CONNECTED);
		} else if (!(k.info & KI_PAIRWISE) && (k.info & KI_MIC) && W.state == WS_CONNECTED) {
			/* the AP changing its group key */
			if (!eapol_key_mic_ok(frame, (usize)n, W.ptk.kck) || !replay_newer(k.replay))
				continue;
			memcpy(W.replay, k.replay, 8);
			u8 data[512];
			usize dlen;
			u8 gtk[32];
			usize gl;
			int gid;
			if ((k.info & KI_ENCRYPTED) && wpa_unwrap(W.ptk.kek, k.data, k.data_len, data, &dlen) &&
			    wpa_find_gtk(data, dlen, gtk, &gl, &gid))
				install_gtk(gtk, gl, gid, k.rsc);
			EapolKey r = { .version = k.version, .info = KI_VER_SHA1_AES | KI_MIC | KI_SECURE };
			memcpy(r.replay, k.replay, 8);
			usize len = eapol_key_build(out, sizeof out, &r, NULL, 0, W.ptk.kck);
			send_eapol(out, len);
		}
	}
}

/* ---- Events from the kernel ---------------------------------------------------------------------- */

static void event_cb(void *user, u8 cmd, const u8 *attrs, usize n)
{
	NlAttr tb[NL80211_ATTR_DISCONNECTED_BY_AP + 8];
	nla_parse(attrs, n, tb, NL80211_ATTR_STATUS_CODE);
	if (!W.iface || (tb[NL80211_ATTR_IFINDEX].present && (int)nla_u32(&tb[NL80211_ATTR_IFINDEX]) != ifindex()))
		return;
	switch (cmd) {
	case NL80211_CMD_NEW_SCAN_RESULTS:
		W.scanning = false;
		dump_scan();
		break;
	case NL80211_CMD_SCAN_ABORTED:
		W.scanning = false;
		break;
	case NL80211_CMD_CONNECT: {
		if (W.state != WS_CONNECTING)
			break;
		u16 status = tb[NL80211_ATTR_STATUS_CODE].present ? nla_u16(&tb[NL80211_ATTR_STATUS_CODE]) : 1;
		if (status != 0) {
			char why[80];
			fmt(why, sizeof why, status == 1 && !tb[NL80211_ATTR_STATUS_CODE].present ? "the network didn't answer"
								       : "the network turned the connection down (%u)", status);
			fail(why, false);
			break;
		}
		if (W.target.sec == SEC_OPEN) {
			log_msg("joined %s", ssid_text(&W.target));
			if (W.save)
				save_network(&W.target, NULL);
			set_state(WS_CONNECTED);
		} else {
			/* the handshake may already be under way */
			if (W.state == WS_CONNECTING)
				W.state = WS_HANDSHAKE;
			W.deadline = time_mono_ms() + 6000;
		}
		break;
	}
	case NL80211_CMD_DISCONNECT:
		if (W.state == WS_HANDSHAKE && W.sent_msg2 && !W.have_msg3) {
			fail("Wrong password?", true);
		} else if (W.state == WS_CONNECTED || W.state == WS_HANDSHAKE || W.state == WS_CONNECTING) {
			log_msg("left %s", ssid_text(&W.target));
			set_state(W.radio ? WS_IDLE : WS_OFF);
			W.scan_at = time_mono_ms();
			W.last_scan = 0;   /* look for it again soon */
		}
		break;
	}
}

/* ---- Commands from Settings and quick settings ----------------------------------------------------- */

static int parse_ssid(const char *hex, u8 *out)
{
	usize n = strlen(hex);
	if (n == 0 || n > 64 || n % 2 || !hex_decode(hex, out, n / 2))
		return -1;
	return (int)(n / 2);
}

static void command(char *line)
{
	char *args[3] = { 0 };
	int na = 0;
	for (char *p = line; *p && na < 3;) {
		while (*p == ' ' || *p == '\n')
			*p++ = 0;
		if (!*p)
			break;
		args[na++] = p;
		while (*p && *p != ' ' && *p != '\n')
			p++;
	}
	if (!na)
		return;
	if (str_eq(args[0], "scan")) {
		trigger_scan();
	} else if (str_eq(args[0], "connect") && na >= 2) {
		u8 ssid[32];
		int len = parse_ssid(args[1], ssid);
		if (len < 0)
			return;
		char pass[80] = "";
		if (na >= 3) {
			usize pl = strlen(args[2]);
			if (pl % 2 == 0 && pl / 2 < sizeof pass)
				hex_decode(args[2], (u8 *)pass, pl / 2), pass[pl / 2] = 0;
		}
		W.failed_len = 0;
		for (int i = 0; i < W.nnets; i++)
			if (same_ssid(&W.nets[i], ssid, len)) {
				connect_to(&W.nets[i], pass);
				break;
			}
		memset(pass, 0, sizeof pass);
	} else if (str_eq(args[0], "disconnect")) {
		W.user_off = true;
		disconnect();
		set_state(W.radio ? WS_IDLE : WS_OFF);
	} else if (str_eq(args[0], "forget") && na >= 2) {
		u8 ssid[32];
		int len = parse_ssid(args[1], ssid);
		if (len < 0)
			return;
		forget_network(ssid, len);
		if (W.state != WS_IDLE && W.state != WS_OFF && same_ssid(&W.target, ssid, len)) {
			W.user_off = true;
			disconnect();
			set_state(WS_IDLE);
		}
		write_networks();
	} else if (str_eq(args[0], "radio") && na >= 2) {
		bool on = str_eq(args[1], "on");
		if (on == W.radio)
			return;
		W.radio = on;
		radio_save();
		if (!on) {
			disconnect();
			rfkill(true);
			W.nnets = 0;
			set_state(WS_OFF);
		} else {
			rfkill(false);
			W.user_off = false;
			set_state(WS_IDLE);
			W.last_scan = 0;
		}
	}
}

static void handle_ctl(void)
{
	char buf[512];
	for (;;) {
		long n = sys_recvfrom(W.ctl, buf, sizeof buf - 1, 0, NULL, NULL);
		if (n <= 0)
			return;
		buf[n] = 0;
		command(buf);
		memset(buf, 0, sizeof buf);   /* it may have held a password */
	}
}

/* ---- The service's hooks ------------------------------------------------------------------------------- */

void wifi_init(void)
{
	char *r = file_read_line(WIFI_DIR "/radio");
	W.radio = !r || !str_eq(r, "off");
	mem_free(r);
	W.state = W.radio ? WS_IDLE : WS_OFF;
	if (genl_open(&W.cmd) && genl_open(&W.ev)) {
		W.present = true;
		genl_join(&W.ev, W.ev.grp_scan);
		genl_join(&W.ev, W.ev.grp_mlme);
		sys_fcntl(W.ev.fd, F_SETFL, O_NONBLOCK);
	}
	W.ctl = (int)sys_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (W.ctl >= 0) {
		struct sockaddr_un a = { .sun_family = AF_UNIX };
		str_copy(a.sun_path, CTL_SOCKET, sizeof a.sun_path);
		sys_unlink(CTL_SOCKET);
		if (sys_bind(W.ctl, &a, sizeof a) == 0)
			sys_chmod(CTL_SOCKET, 0666);   /* the desktop user picks networks */
	}
	sys_unlink(NETWORKS_FILE);
	file_write_atomic(NETWORKS_FILE, "", 0);
}

void wifi_adapter(Iface *f)
{
	if (W.iface || !W.present)
		return;
	W.iface = f;
	log_msg("using %s for Wi-Fi", f->name);
	rfkill(!W.radio);
	if (W.radio)
		link_up(f->name);
	W.last_scan = 0;
}

void wifi_tick(u64 now)
{
	if (!W.iface || !W.iface->present)
		return;
	if (!W.radio)
		return;
	if ((W.state == WS_CONNECTING || W.state == WS_HANDSHAKE) && now > W.deadline)
		fail(W.sent_msg2 ? "Wrong password?" : "the network didn't answer", W.sent_msg2);
	if (W.scanning && now - W.scan_at > 15000)
		W.scanning = false;   /* the result event went missing */
	/* look around: often while not connected, rarely once connected */
	u64 every = W.state == WS_CONNECTED ? 5 * 60 * 1000 : W.state == WS_IDLE ? 30 * 1000 : 0;
	if (every && (!W.last_scan || now - W.last_scan >= every))
		trigger_scan();
	/* the signal of the network we're on */
	if (W.state == WS_CONNECTED && now - W.last_dump >= 10000)
		dump_scan();
}

int wifi_fds(struct pollfd *pfd, int max)
{
	int n = 0;
	if (W.present && n < max)
		pfd[n++] = (struct pollfd){ W.ev.fd, POLLIN, 0 };
	if (W.eapol >= 0 && n < max)
		pfd[n++] = (struct pollfd){ W.eapol, POLLIN, 0 };
	if (W.ctl >= 0 && n < max)
		pfd[n++] = (struct pollfd){ W.ctl, POLLIN, 0 };
	return n;
}

void wifi_ready(struct pollfd *pfd, int n)
{
	for (int i = 0; i < n; i++) {
		if (!(pfd[i].revents & POLLIN))
			continue;
		if (W.present && pfd[i].fd == W.ev.fd)
			while (genl_event(&W.ev, event_cb, NULL))
				;
		else if (pfd[i].fd == W.eapol)
			handle_eapol();
		else if (pfd[i].fd == W.ctl)
			handle_ctl();
	}
}

bool wifi_link_ok(const Iface *f)
{
	return f == W.iface && W.state == WS_CONNECTED;
}

void wifi_status(StrBuf *s, const char **name, int *strength)
{
	static const char *const states[] = { "off", "idle", "connecting", "connecting", "connected" };
	if (!W.iface)
		return;
	sb_fmt(s, "wifi = %s\nwifi_state = %s\n", W.radio ? "on" : "off", states[W.state]);
	if (W.state != WS_IDLE && W.state != WS_OFF) {
		char hex[65];
		ssid_hex(W.target.ssid, W.target.ssid_len, hex);
		sb_fmt(s, "wifi_ssid = %s\nwifi_ssid_hex = %s\nwifi_signal = %d\n", ssid_text(&W.target), hex, signal_pct(W.target.signal));
	}
	if (W.error[0])
		sb_fmt(s, "wifi_error = %s\n", W.error);
	if (W.scanning)
		sb_adds(s, "wifi_scanning = 1\n");
	if (name)
		*name = ssid_text(&W.target);
	if (strength)
		*strength = signal_pct(W.target.signal);
}
