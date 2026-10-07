/* hwsim-ap: a small WPA2-Personal access point for testing znet's Wi-Fi in a
 * virtual machine with the kernel's simulated radios (mac80211_hwsim).
 *
 *     hwsim-ap [IFACE [SSID [PASSWORD]]]      defaults: wlan1 ZurbaxTest zurbax-wifi
 *
 * It beacons on channel 1, answers probes, authentication and association,
 * runs the authenticator side of the 4-way handshake, changes the group key
 * once a little later, and hands out 192.168.77.10 by DHCP. An empty password
 * makes an open network. Progress goes to the kernel log. Test builds start it
 * from /system/test; it is never part of a release. */
#include "../../system/znet/nl.c"
#include "../../system/znet/wpa.c"

#define SIOCGIFFLAGS 0x8913
#define SIOCSIFFLAGS 0x8914
#define SIOCSIFADDR 0x8916
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFHWADDR 0x8927
#define IFF_UP 1
#define SO_BINDTODEVICE 25
#define SO_BROADCAST 6
#define FREQ 2412

struct sockaddr_ll {
	u16 family;
	u16 protocol;
	i32 ifindex;
	u16 hatype;
	u8 pkttype;
	u8 halen;
	u8 addr[8];
};

static struct {
	char ifname[16];
	int ifindex;
	u8 mac[6];
	u8 ssid[32];
	int ssid_len;
	char pass[64];
	bool open;
	u8 pmk[32];
	Genl cmd, mgmt;
	int eapol, dhcp;
	u8 beacon[256];
	usize beacon_len;    /* the whole beacon, for probe responses */
	usize head_len;      /* up to where the tail starts */
	u8 rsn[32];
	usize rsn_len;
	u8 gtk[16];
	int gtk_id;
	/* the one client */
	bool have_sta;
	u8 sta[6];
	u8 anonce[32], snonce[32];
	Ptk ptk;
	u64 replay;
	int step;            /* 1 sent msg1, 3 sent msg3, 4 done, 5 rekey sent, 6 rekeyed */
	u64 next_ms;
	u64 done_ms;
	u16 seq;
} A = { .eapol = -1, .dhcp = -1 };

static void say(const char *f, ...) PRINTF_LIKE(1, 2);
static void say(const char *f, ...)
{
	char line[300];
	va_list ap;
	va_start(ap, f);
	int n = fmt(line, sizeof line, "<6>hwsim-ap: ");
	n += fmt_v(line + n, sizeof line - (usize)n, f, ap);
	va_end(ap);
	int fd = (int)sys_open("/dev/kmsg", O_WRONLY | O_CLOEXEC, 0);
	if (fd >= 0) {
		sys_write(fd, line, (usize)n);
		sys_close(fd);
	}
	eprint("%s\n", line + 3);
}

static bool ifflags(bool up)
{
	int fd = (int)sys_socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	u8 ifr[40] = { 0 };
	str_copy((char *)ifr, A.ifname, 16);
	bool ok = sys_ioctl(fd, SIOCGIFFLAGS, ifr) == 0;
	u16 flags = (u16)(ifr[16] | ifr[17] << 8);
	flags = up ? (u16)(flags | IFF_UP) : (u16)(flags & ~IFF_UP);
	ifr[16] = (u8)flags, ifr[17] = (u8)(flags >> 8);
	ok = ok && sys_ioctl(fd, SIOCSIFFLAGS, ifr) == 0;
	if (up) {
		/* 192.168.77.1/24 */
		memset(ifr + 16, 0, 24);
		ifr[16] = AF_INET;
		u8 ip[4] = { 192, 168, 77, 1 }, mask[4] = { 255, 255, 255, 0 };
		memcpy(ifr + 20, ip, 4);
		sys_ioctl(fd, SIOCSIFADDR, ifr);
		memcpy(ifr + 20, mask, 4);
		sys_ioctl(fd, SIOCSIFNETMASK, ifr);
	}
	memset(ifr + 16, 0, 24);
	if (sys_ioctl(fd, SIOCGIFHWADDR, ifr) == 0)
		memcpy(A.mac, ifr + 18, 6);
	sys_close(fd);
	return ok;
}

static int nl_check(Genl *g, NlMsg *m, const char *what)
{
	int r = genl_call(g, m, NULL, NULL);
	if (r < 0)
		say("%s failed: %d", what, r);
	return r;
}

static void add_ie(u8 *buf, usize *len, u8 id, const void *data, usize n)
{
	buf[(*len)++] = id;
	buf[(*len)++] = (u8)n;
	memcpy(buf + *len, data, n);
	*len += n;
}

static void build_beacon(void)
{
	u8 *b = A.beacon;
	usize n = 0;
	b[n++] = 0x80, b[n++] = 0;   /* beacon */
	b[n++] = 0, b[n++] = 0;      /* duration */
	memset(b + n, 0xff, 6), n += 6;
	memcpy(b + n, A.mac, 6), n += 6;
	memcpy(b + n, A.mac, 6), n += 6;
	b[n++] = 0, b[n++] = 0;      /* sequence */
	memset(b + n, 0, 8), n += 8; /* timestamp */
	b[n++] = 100, b[n++] = 0;    /* beacon interval */
	u16 cap = 0x0001 | 0x0400 | (A.open ? 0 : 0x0010);   /* ESS, short slot, privacy */
	b[n++] = (u8)cap, b[n++] = (u8)(cap >> 8);
	add_ie(b, &n, 0, A.ssid, (usize)A.ssid_len);
	static const u8 rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
	add_ie(b, &n, 1, rates, sizeof rates);
	u8 ch = 1;
	add_ie(b, &n, 3, &ch, 1);
	A.head_len = n;
	static const u8 ext[] = { 0x30, 0x48, 0x60, 0x6c };
	if (!A.open) {
		A.rsn_len = rsn_element(A.rsn, SUITE(CIPHER_CCMP));
		memcpy(b + n, A.rsn, A.rsn_len);
		n += A.rsn_len;
	}
	add_ie(b, &n, 50, ext, sizeof ext);
	A.beacon_len = n;
}

static bool start_ap(void)
{
	NlMsg m;
	nlm_begin(&m, A.cmd.family, 0, NL80211_CMD_START_AP);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
	nlm_put(&m, NL80211_ATTR_BEACON_HEAD, A.beacon, A.head_len);
	nlm_put(&m, NL80211_ATTR_BEACON_TAIL, A.beacon + A.head_len, A.beacon_len - A.head_len);
	nlm_u32(&m, NL80211_ATTR_BEACON_INTERVAL, 100);
	nlm_u32(&m, NL80211_ATTR_DTIM_PERIOD, 2);
	nlm_put(&m, NL80211_ATTR_SSID, A.ssid, (usize)A.ssid_len);
	nlm_u32(&m, NL80211_ATTR_HIDDEN_SSID, 0);
	nlm_u32(&m, NL80211_ATTR_WIPHY_FREQ, FREQ);
	nlm_u32(&m, NL80211_ATTR_AUTH_TYPE, 0);
	if (!A.open) {
		nlm_flag(&m, NL80211_ATTR_PRIVACY);
		nlm_u32(&m, NL80211_ATTR_WPA_VERSIONS, NL80211_WPA_VERSION_2);
		nlm_u32(&m, NL80211_ATTR_CIPHER_SUITES_PAIRWISE, SUITE(CIPHER_CCMP));
		nlm_u32(&m, NL80211_ATTR_CIPHER_SUITE_GROUP, SUITE(CIPHER_CCMP));
		nlm_u32(&m, NL80211_ATTR_AKM_SUITES, SUITE(AKM_PSK));
	}
	return nl_check(&A.cmd, &m, "starting the access point") == 0;
}

static void set_gtk(int id)
{
	NlMsg m;
	nlm_begin(&m, A.cmd.family, 0, NL80211_CMD_NEW_KEY);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
	nlm_put(&m, NL80211_ATTR_KEY_DATA, A.gtk, 16);
	nlm_u32(&m, NL80211_ATTR_KEY_CIPHER, SUITE(CIPHER_CCMP));
	nlm_u8(&m, NL80211_ATTR_KEY_IDX, (u8)id);
	nlm_u32(&m, NL80211_ATTR_KEY_TYPE, NL80211_KEYTYPE_GROUP);
	nl_check(&A.cmd, &m, "group key");
	nlm_begin(&m, A.cmd.family, 0, NL80211_CMD_SET_KEY);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
	nlm_u8(&m, NL80211_ATTR_KEY_IDX, (u8)id);
	nlm_flag(&m, NL80211_ATTR_KEY_DEFAULT);
	usize at = nlm_nest(&m, NL80211_ATTR_KEY_DEFAULT_TYPES);
	nlm_flag(&m, NL80211_KEY_DEFAULT_TYPE_MULTICAST);
	nlm_nest_end(&m, at);
	nl_check(&A.cmd, &m, "default group key");
}

static void send_frame(const u8 *f, usize n)
{
	NlMsg m;
	nlm_begin(&m, A.mgmt.family, 0, NL80211_CMD_FRAME);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
	nlm_put(&m, NL80211_ATTR_FRAME, f, n);
	nl_check(&A.mgmt, &m, "sending a frame");
}

static void mgmt_header(u8 *f, u8 subtype, const u8 *to)
{
	f[0] = subtype, f[1] = 0, f[2] = 0, f[3] = 0;
	memcpy(f + 4, to, 6);
	memcpy(f + 10, A.mac, 6);
	memcpy(f + 16, A.mac, 6);
	A.seq += 16;
	f[22] = (u8)A.seq, f[23] = (u8)(A.seq >> 8);
}

static void send_eapol(const u8 *frame, usize n)
{
	struct sockaddr_ll to = { .family = AF_PACKET, .protocol = htons(ETH_P_PAE), .ifindex = A.ifindex, .halen = 6 };
	memcpy(to.addr, A.sta, 6);
	if (sys_sendto(A.eapol, frame, n, 0, &to, sizeof to) < 0)
		say("sending EAPOL failed");
}

static void replay_bytes(u8 out[8])
{
	for (int i = 0; i < 8; i++)
		out[i] = (u8)(A.replay >> (56 - 8 * i));
}

static void send_msg1(void)
{
	rand_bytes(A.anonce, 32);
	A.replay++;
	EapolKey k = { .info = KI_VER_SHA1_AES | KI_PAIRWISE | KI_ACK, .key_len = 16 };
	replay_bytes(k.replay);
	memcpy(k.nonce, A.anonce, 32);
	u8 out[256];
	send_eapol(out, eapol_key_build(out, sizeof out, &k, NULL, 0, NULL));
	A.step = 1;
	A.next_ms = time_mono_ms() + 1000;
	say("sent message 1");
}

static usize gtk_kde(u8 *out)
{
	u8 kde[8] = { 0xdd, 22, 0x00, 0x0f, 0xac, 0x01, (u8)A.gtk_id, 0 };
	memcpy(out, kde, 8);
	memcpy(out + 8, A.gtk, 16);
	return 24;
}

static void send_msg3(void)
{
	u8 plain[128], wrapped[160];
	memcpy(plain, A.rsn, A.rsn_len);
	usize pl = A.rsn_len + gtk_kde(plain + A.rsn_len);
	usize wl = wpa_wrap(A.ptk.kek, plain, pl, wrapped);
	A.replay++;
	EapolKey k = { .info = KI_VER_SHA1_AES | KI_PAIRWISE | KI_ACK | KI_MIC | KI_INSTALL | KI_SECURE | KI_ENCRYPTED, .key_len = 16 };
	replay_bytes(k.replay);
	memcpy(k.nonce, A.anonce, 32);
	u8 out[512];
	send_eapol(out, eapol_key_build(out, sizeof out, &k, wrapped, wl, A.ptk.kck));
	A.step = 3;
	A.next_ms = time_mono_ms() + 1000;
	say("sent message 3");
}

static void send_rekey(void)
{
	rand_bytes(A.gtk, 16);
	A.gtk_id = A.gtk_id == 1 ? 2 : 1;
	u8 plain[64], wrapped[96];
	usize pl = gtk_kde(plain);
	usize wl = wpa_wrap(A.ptk.kek, plain, pl, wrapped);
	A.replay++;
	EapolKey k = { .info = KI_VER_SHA1_AES | KI_ACK | KI_MIC | KI_SECURE | KI_ENCRYPTED, .key_len = 16 };
	replay_bytes(k.replay);
	u8 out[256];
	send_eapol(out, eapol_key_build(out, sizeof out, &k, wrapped, wl, A.ptk.kck));
	A.step = 5;
	say("sent a new group key (id %d)", A.gtk_id);
}

static void station_flags(u32 mask, u32 set)
{
	NlMsg m;
	nlm_begin(&m, A.cmd.family, 0, NL80211_CMD_SET_STATION);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
	nlm_put(&m, NL80211_ATTR_MAC, A.sta, 6);
	u32 upd[2] = { mask, set };
	nlm_put(&m, NL80211_ATTR_STA_FLAGS2, upd, sizeof upd);
	nl_check(&A.cmd, &m, "station flags");
}

static void on_assoc(const u8 *f, usize n, bool reassoc)
{
	const u8 *sta = f + 10;
	usize body = 24 + (reassoc ? 10 : 4);
	if (n < body)
		return;
	u16 listen = (u16)(f[26] | f[27] << 8);
	const u8 *ies = f + body;
	usize il = n - body;
	if (!A.open) {
		const u8 *rsn = wpa_find_ie(ies, il, 0x30);
		Rsn r;
		if (!rsn || !rsn_parse(rsn + 2, rsn[1], &r) || r.akm[0] != SUITE(AKM_PSK) || r.pairwise[0] != SUITE(CIPHER_CCMP)) {
			say("association without a usable RSN element");
			return;
		}
	}
	u8 rates[16];
	usize nr = 0;
	for (u8 id = 1; id != 0; id = id == 1 ? 50 : 0) {
		const u8 *e = wpa_find_ie(ies, il, id);
		for (int i = 0; e && i < e[1] && nr < sizeof rates; i++)
			rates[nr++] = e[2 + i] & 0x7f;
	}
	/* a client coming back starts over */
	NlMsg m;
	nlm_begin(&m, A.cmd.family, 0, NL80211_CMD_DEL_STATION);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
	nlm_put(&m, NL80211_ATTR_MAC, sta, 6);
	genl_call(&A.cmd, &m, NULL, NULL);
	nlm_begin(&m, A.cmd.family, 0, NL80211_CMD_NEW_STATION);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
	nlm_put(&m, NL80211_ATTR_MAC, sta, 6);
	nlm_put(&m, NL80211_ATTR_STA_SUPPORTED_RATES, rates, nr);
	nlm_u16(&m, NL80211_ATTR_STA_LISTEN_INTERVAL, listen);
	nlm_u16(&m, NL80211_ATTR_STA_AID, 1);
	u32 upd[2] = { 0, 0 };   /* the kernel marks it authenticated and associated */
	nlm_put(&m, NL80211_ATTR_STA_FLAGS2, upd, sizeof upd);
	if (nl_check(&A.cmd, &m, "adding the station") < 0)
		return;
	memcpy(A.sta, sta, 6);
	A.have_sta = true;
	u8 resp[128];
	mgmt_header(resp, reassoc ? 0x30 : 0x10, sta);
	usize rn = 24;
	u16 cap = 0x0001 | 0x0400 | (A.open ? 0 : 0x0010);
	resp[rn++] = (u8)cap, resp[rn++] = (u8)(cap >> 8);
	resp[rn++] = 0, resp[rn++] = 0;         /* success */
	resp[rn++] = 1, resp[rn++] = 0xc0;      /* AID 1 */
	static const u8 r1[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 }, r2[] = { 0x30, 0x48, 0x60, 0x6c };
	add_ie(resp, &rn, 1, r1, sizeof r1);
	add_ie(resp, &rn, 50, r2, sizeof r2);
	send_frame(resp, rn);
	say("associated %02x:%02x:%02x:%02x:%02x:%02x", sta[0], sta[1], sta[2], sta[3], sta[4], sta[5]);
	if (A.open) {
		station_flags(1u << NL80211_STA_FLAG_AUTHORIZED, 1u << NL80211_STA_FLAG_AUTHORIZED);
		A.step = 4;
	} else {
		A.step = 0;
		A.next_ms = time_mono_ms() + 100;   /* message 1 after the response has gone */
	}
}

static void mgmt_cb(void *user, u8 cmd, const u8 *attrs, usize n)
{
	if (cmd != NL80211_CMD_FRAME)
		return;
	NlAttr tb[NL80211_ATTR_FRAME + 1];
	nla_parse(attrs, n, tb, NL80211_ATTR_FRAME);
	if (!tb[NL80211_ATTR_FRAME].present || tb[NL80211_ATTR_FRAME].len < 24)
		return;
	const u8 *f = tb[NL80211_ATTR_FRAME].data;
	usize fl = tb[NL80211_ATTR_FRAME].len;
	u8 sub = f[0] & 0xf0;
	if (sub == 0x40) {
		/* probe request: answer with the beacon's contents */
		u8 resp[256];
		memcpy(resp, A.beacon, A.beacon_len);
		mgmt_header(resp, 0x50, f + 10);
		send_frame(resp, A.beacon_len);
	} else if (sub == 0xb0 && fl >= 30) {
		u16 alg = (u16)(f[24] | f[25] << 8), seq = (u16)(f[26] | f[27] << 8);
		if (alg != 0 || seq != 1)
			return;
		u8 resp[30];
		mgmt_header(resp, 0xb0, f + 10);
		resp[24] = 0, resp[25] = 0, resp[26] = 2, resp[27] = 0, resp[28] = 0, resp[29] = 0;
		send_frame(resp, sizeof resp);
		say("authenticated a client");
	} else if (sub == 0x00 || sub == 0x20) {
		on_assoc(f, fl, sub == 0x20);
	} else if (sub == 0xa0 || sub == 0xc0) {
		say("the client left");
		A.have_sta = false;
		A.step = 0;
	}
}

static void handle_eapol(void)
{
	u8 frame[1024];
	struct sockaddr_ll from;
	u32 fl = sizeof from;
	long n = sys_recvfrom(A.eapol, frame, sizeof frame, MSG_DONTWAIT, &from, &fl);
	EapolKey k;
	if (n <= 0 || !eapol_key_parse(frame, (usize)n, &k) || !A.have_sta || memcmp(from.addr, A.sta, 6) != 0)
		return;
	u8 want[8];
	replay_bytes(want);
	if (memcmp(k.replay, want, 8) != 0) {
		say("a reply with the wrong replay counter");
		return;
	}
	if (A.step == 1 && (k.info & KI_PAIRWISE) && (k.info & KI_MIC) && !(k.info & KI_SECURE)) {
		memcpy(A.snonce, k.nonce, 32);
		wpa_ptk(A.pmk, A.mac, A.sta, A.anonce, A.snonce, &A.ptk);
		if (!eapol_key_mic_ok(frame, (usize)n, A.ptk.kck)) {
			say("message 2 failed its check: wrong password");
			return;
		}
		say("message 2 is good");
		send_msg3();
	} else if (A.step == 3 && (k.info & KI_PAIRWISE) && (k.info & KI_SECURE)) {
		if (!eapol_key_mic_ok(frame, (usize)n, A.ptk.kck)) {
			say("message 4 failed its check");
			return;
		}
		NlMsg m;
		nlm_begin(&m, A.cmd.family, 0, NL80211_CMD_NEW_KEY);
		nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
		nlm_put(&m, NL80211_ATTR_MAC, A.sta, 6);
		nlm_put(&m, NL80211_ATTR_KEY_DATA, A.ptk.tk, 16);
		nlm_u32(&m, NL80211_ATTR_KEY_CIPHER, SUITE(CIPHER_CCMP));
		nlm_u8(&m, NL80211_ATTR_KEY_IDX, 0);
		nlm_u32(&m, NL80211_ATTR_KEY_TYPE, NL80211_KEYTYPE_PAIRWISE);
		nl_check(&A.cmd, &m, "pairwise key");
		station_flags(1u << NL80211_STA_FLAG_AUTHORIZED, 1u << NL80211_STA_FLAG_AUTHORIZED);
		A.step = 4;
		A.done_ms = time_mono_ms();
		say("handshake complete: the client is in");
	} else if (A.step == 5 && !(k.info & KI_PAIRWISE) && (k.info & KI_SECURE)) {
		if (!eapol_key_mic_ok(frame, (usize)n, A.ptk.kck)) {
			say("group message 2 failed its check");
			return;
		}
		set_gtk(A.gtk_id);
		A.step = 6;
		say("group key changed");
	}
}

/* ---- DHCP: one address for the one client ---------------------------------------------------------- */

static void dhcp_reply(const u8 *req, usize n)
{
	if (n < 240 || req[0] != 1)
		return;
	int type = 0;
	for (usize i = 240; i + 2 <= n && req[i] != 255;) {
		if (req[i] == 0) {
			i++;
			continue;
		}
		if (req[i] == 53 && req[i + 1] >= 1)
			type = req[i + 2];
		i += 2u + req[i + 1];
	}
	if (type != 1 && type != 3)
		return;
	u8 out[300] = { 0 };
	out[0] = 2, out[1] = 1, out[2] = 6;
	memcpy(out + 4, req + 4, 4);       /* transaction */
	memcpy(out + 10, req + 10, 2);     /* flags */
	u8 yi[4] = { 192, 168, 77, 10 }, si[4] = { 192, 168, 77, 1 };
	memcpy(out + 16, yi, 4);
	memcpy(out + 20, si, 4);
	memcpy(out + 28, req + 28, 16);    /* client hardware address */
	out[236] = 99, out[237] = 130, out[238] = 83, out[239] = 99;
	usize p = 240;
	u8 opts[] = { 53, 1, (u8)(type == 1 ? 2 : 5), 54, 4, 192, 168, 77, 1, 51, 4, 0, 0, 0x0e, 0x10, 1, 4, 255, 255, 255, 0,
		      3, 4, 192, 168, 77, 1, 6, 4, 192, 168, 77, 1, 255 };
	memcpy(out + p, opts, sizeof opts);
	p += sizeof opts;
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(68) };
	memset(&to.sin_addr, 0xff, 4);
	sys_sendto(A.dhcp, out, p, 0, &to, sizeof to);
	say(type == 1 ? "offered 192.168.77.10" : "leased 192.168.77.10");
}

static void open_dhcp(void)
{
	A.dhcp = (int)sys_socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int one = 1;
	sys_setsockopt(A.dhcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	sys_setsockopt(A.dhcp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	sys_setsockopt(A.dhcp, SOL_SOCKET, SO_BINDTODEVICE, A.ifname, (u32)strlen(A.ifname) + 1);
	struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(67) };
	if (sys_bind(A.dhcp, &me, sizeof me) < 0)
		say("DHCP server couldn't start");
}

/* ---- Setup ------------------------------------------------------------------------------------------- */

static bool setup(void)
{
	char *p = str_fmt("/sys/class/net/%s/ifindex", A.ifname);
	A.ifindex = (int)file_read_int(p, 0);
	mem_free(p);
	if (!A.ifindex)
		return false;
	if (!genl_open(&A.cmd) || !genl_open(&A.mgmt)) {
		say("no nl80211");
		return false;
	}
	ifflags(false);
	NlMsg m;
	nlm_begin(&m, A.cmd.family, 0, NL80211_CMD_SET_INTERFACE);
	nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
	nlm_u32(&m, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_AP);
	if (nl_check(&A.cmd, &m, "switching to access point mode") < 0)
		return false;
	/* management frames we answer ourselves */
	static const u16 types[] = { 0x0040, 0x00b0, 0x0000, 0x0020, 0x00a0, 0x00c0 };
	for (usize i = 0; i < ARRAY_LEN(types); i++) {
		nlm_begin(&m, A.mgmt.family, 0, NL80211_CMD_REGISTER_FRAME);
		nlm_u32(&m, NL80211_ATTR_IFINDEX, (u32)A.ifindex);
		nlm_u16(&m, NL80211_ATTR_FRAME_TYPE, types[i]);
		nlm_put(&m, NL80211_ATTR_FRAME_MATCH, NULL, 0);
		nl_check(&A.mgmt, &m, "registering for frames");
	}
	if (!ifflags(true))
		say("couldn't bring %s up", A.ifname);
	build_beacon();
	if (!start_ap())
		return false;
	if (!A.open) {
		wpa_pmk(A.pass, A.ssid, (usize)A.ssid_len, A.pmk);
		rand_bytes(A.gtk, 16);
		A.gtk_id = 1;
		set_gtk(1);
	}
	A.eapol = (int)sys_socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_PAE));
	struct sockaddr_ll sa = { .family = AF_PACKET, .protocol = htons(ETH_P_PAE), .ifindex = A.ifindex };
	sys_bind(A.eapol, &sa, sizeof sa);
	open_dhcp();
	say("network \"%.*s\" is up on %s (%s)", A.ssid_len, (char *)A.ssid, A.ifname, A.open ? "open" : "WPA2");
	return true;
}

int main(int argc, char **argv)
{
	str_copy(A.ifname, argc > 1 ? argv[1] : "wlan1", sizeof A.ifname);
	const char *ssid = argc > 2 ? argv[2] : "ZurbaxTest";
	A.ssid_len = (int)MIN(strlen(ssid), 32);
	memcpy(A.ssid, ssid, (usize)A.ssid_len);
	str_copy(A.pass, argc > 3 ? argv[3] : "zurbax-wifi", sizeof A.pass);
	A.open = !A.pass[0];
	/* the radios appear a moment after boot */
	bool ok = false;
	for (int i = 0; i < 60 && !ok; i++) {
		ok = setup();
		if (!ok)
			sleep_ms(500);
	}
	if (!ok) {
		say("giving up");
		return 1;
	}
	for (;;) {
		struct pollfd p[3] = { { A.mgmt.fd, POLLIN, 0 }, { A.eapol, POLLIN, 0 }, { A.dhcp, POLLIN, 0 } };
		sys_poll(p, 3, 200);
		if (p[0].revents & POLLIN)
			while (genl_event(&A.mgmt, mgmt_cb, NULL))
				;
		if (p[1].revents & POLLIN)
			handle_eapol();
		if (p[2].revents & POLLIN) {
			u8 buf[600];
			long n = sys_recvfrom(A.dhcp, buf, sizeof buf, 0, NULL, NULL);
			if (n > 0)
				dhcp_reply(buf, (usize)n);
		}
		u64 now = time_mono_ms();
		if (A.have_sta && A.step == 0 && now >= A.next_ms)
			send_msg1();
		else if (A.have_sta && (A.step == 1 || A.step == 3) && now >= A.next_ms) {
			say("no answer to message %d; starting over", A.step);
			send_msg1();
		} else if (A.have_sta && A.step == 4 && A.done_ms && now - A.done_ms > 8000)
			send_rekey();
	}
}
