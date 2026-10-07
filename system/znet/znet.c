/*
 * znet — the network service.
 *
 * Brings up loopback, watches the network adapters, gets addresses by DHCP
 * on wired links and configures them through rtnetlink. Its state goes to
 * /run/zurbax/network.status for the desktop and Settings:
 *
 *   state = connected | connecting | disconnected
 *   kind = ethernet | wifi        name = Wired / the Wi-Fi network
 *   strength = 0..100             address, gateway, dns, iface
 *   ethernet = text for Settings  wifi_available = 1 if there's a Wi-Fi adapter
 */
#include "znet.h"

#define STATUS_FILE "/run/zurbax/network.status"
#define ETH_P_IP 0x0800
#define SIOCGIFFLAGS 0x8913
#define SIOCSIFFLAGS 0x8914
#define IFF_UP 0x1
#define NETLINK_ROUTE 0
#define RTM_NEWADDR 20
#define RTM_DELADDR 21
#define RTM_NEWROUTE 24
#define NLM_F_REQUEST 1
#define NLM_F_ACK 4
#define NLM_F_REPLACE 0x100
#define NLM_F_CREATE 0x400
#define NLMSG_ERROR 2

static Iface ifaces[8];
static int nifaces;
static int rtnl = -1;
static u32 nl_seq;

/* ---- Logging ------------------------------------------------------------------------- */

void log_msg(const char *f, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, f);
	int n = fmt(line, sizeof line, "<6>znet: ");
	n += fmt_v(line + n, sizeof line - (usize)n, f, ap);
	va_end(ap);
	int fd = (int)sys_open("/dev/kmsg", O_WRONLY | O_CLOEXEC, 0);
	if (fd >= 0) {
		sys_write(fd, line, (usize)n);
		sys_close(fd);
	}
}

void ip_str(u32 a, char *out, usize cap)
{
	const u8 *b = (const u8 *)&a;
	fmt(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* ---- Links ------------------------------------------------------------------------------ */

void link_up(const char *name)
{
	int fd = (int)sys_socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	u8 ifr[40] = { 0 };
	str_copy((char *)ifr, name, 16);
	if (sys_ioctl(fd, SIOCGIFFLAGS, ifr) == 0) {
		u16 flags = (u16)(ifr[16] | ifr[17] << 8);
		if (!(flags & IFF_UP)) {
			flags |= IFF_UP;
			ifr[16] = (u8)flags, ifr[17] = (u8)(flags >> 8);
			sys_ioctl(fd, SIOCSIFFLAGS, ifr);
		}
	}
	sys_close(fd);
}

static bool parse_mac(const char *s, u8 mac[6])
{
	for (int i = 0; i < 6; i++) {
		if (!is_xdigit(s[0]) || !is_xdigit(s[1]))
			return false;
		u8 v;
		hex_decode(s, &v, 1);
		mac[i] = v;
		s += 2;
		if (i < 5 && *s++ != ':')
			return false;
	}
	return true;
}

static Iface *find_iface(const char *name)
{
	for (int i = 0; i < nifaces; i++)
		if (str_eq(ifaces[i].name, name))
			return &ifaces[i];
	return NULL;
}

static void scan_links(void)
{
	for (int i = 0; i < nifaces; i++)
		ifaces[i].present = false;
	DirList list = { 0 };
	dir_list("/sys/class/net", &list, false);
	for (usize i = 0; i < list.len; i++) {
		const char *n = list.data[i].name;
		if (str_eq(n, "lo") || strlen(n) >= 16)
			continue;
		char *p = str_fmt("/sys/class/net/%s/type", n);
		i64 type = file_read_int(p, 0);
		mem_free(p);
		if (type != 1) /* Ethernet framing only (Wi-Fi uses it too) */
			continue;
		Iface *f = find_iface(n);
		if (!f) {
			if (nifaces == (int)ARRAY_LEN(ifaces))
				continue;
			f = &ifaces[nifaces++];
			memset(f, 0, sizeof *f);
			str_copy(f->name, n, sizeof f->name);
			f->fd = -1;
			p = str_fmt("/sys/class/net/%s/wireless", n);
			char *q = str_fmt("/sys/class/net/%s/phy80211", n);
			f->wireless = dir_exists(p) || dir_exists(q);
			mem_free(p);
			mem_free(q);
			log_msg("found %s adapter %s", f->wireless ? "Wi-Fi" : "wired", n);
		}
		f->present = true;
		p = str_fmt("/sys/class/net/%s/ifindex", n);
		f->index = (int)file_read_int(p, 0);
		mem_free(p);
		p = str_fmt("/sys/class/net/%s/address", n);
		char *mac = file_read_line(p);
		mem_free(p);
		if (mac)
			parse_mac(mac, f->mac);
		mem_free(mac);
		/* Wi-Fi adapters are brought up by the Wi-Fi code, which uses just one */
		if (!f->wireless)
			link_up(n);
		else
			wifi_adapter(f);
		p = str_fmt("/sys/class/net/%s/carrier", n);
		f->carrier = file_read_int(p, 0) == 1;
		mem_free(p);
		p = str_fmt("/sys/class/net/%s/speed", n);
		f->speed = (int)file_read_int(p, 0);
		mem_free(p);
	}
	dir_list_free(&list);
}

/* ---- rtnetlink ----------------------------------------------------------------------------- */

static void nl_attr(u8 *msg, usize *len, u16 type, const void *data, usize n)
{
	u8 *a = msg + *len;
	u16 alen = (u16)(4 + n);
	memcpy(a, &alen, 2);
	memcpy(a + 2, &type, 2);
	memcpy(a + 4, data, n);
	*len += ALIGN_UP(alen, 4u);
}

static bool nl_send(u8 *msg, usize len, u16 type, u16 flags)
{
	if (rtnl < 0)
		return false;
	u32 l32 = (u32)len, seq = ++nl_seq, pid = 0;
	memcpy(msg, &l32, 4);
	memcpy(msg + 4, &type, 2);
	u16 f = (u16)(NLM_F_REQUEST | NLM_F_ACK | flags);
	memcpy(msg + 6, &f, 2);
	memcpy(msg + 8, &seq, 4);
	memcpy(msg + 12, &pid, 4);
	struct { u16 family, pad; u32 pid, groups; } sa = { AF_NETLINK, 0, 0, 0 };
	if (sys_sendto(rtnl, msg, len, 0, &sa, sizeof sa) < 0)
		return false;
	u8 reply[1024];
	struct pollfd p = { rtnl, POLLIN, 0 };
	while (sys_poll(&p, 1, 1000) > 0) {
		long n = sys_recvfrom(rtnl, reply, sizeof reply, 0, NULL, NULL);
		if (n < 20)
			return false;
		u16 rtype;
		u32 rseq;
		memcpy(&rtype, reply + 4, 2);
		memcpy(&rseq, reply + 8, 4);
		if (rseq != seq)
			continue;
		if (rtype == NLMSG_ERROR) {
			i32 err;
			memcpy(&err, reply + 16, 4);
			return err == 0 || err == -EEXIST;
		}
		return true;
	}
	return false;
}

static int prefix_len(u32 mask)
{
	return __builtin_popcount(mask);
}

static bool set_address(Iface *f, u32 addr, u32 mask, bool add)
{
	u8 msg[128] = { 0 };
	usize len = 16;
	u8 *ifa = msg + len;
	ifa[0] = AF_INET;
	ifa[1] = (u8)prefix_len(mask);
	memcpy(ifa + 4, &f->index, 4);
	len += 8;
	nl_attr(msg, &len, 2 /* IFA_LOCAL */, &addr, 4);
	nl_attr(msg, &len, 1 /* IFA_ADDRESS */, &addr, 4);
	u32 brd = addr | ~mask;
	nl_attr(msg, &len, 4 /* IFA_BROADCAST */, &brd, 4);
	return nl_send(msg, len, add ? RTM_NEWADDR : RTM_DELADDR, add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
}

static bool set_default_route(Iface *f, u32 gw)
{
	u8 msg[128] = { 0 };
	usize len = 16;
	u8 *rt = msg + len;
	rt[0] = AF_INET; /* family */
	rt[4] = 254;     /* main table */
	rt[5] = 16;      /* set by DHCP */
	rt[6] = 0;       /* universe */
	rt[7] = 1;       /* unicast */
	len += 12;
	nl_attr(msg, &len, 5 /* RTA_GATEWAY */, &gw, 4);
	nl_attr(msg, &len, 4 /* RTA_OIF */, &f->index, 4);
	return nl_send(msg, len, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_REPLACE);
}

/* ---- DHCP ------------------------------------------------------------------------------------ */

static u16 ip_checksum(const u8 *p, usize n)
{
	u32 sum = 0;
	for (usize i = 0; i + 1 < n; i += 2)
		sum += (u32)(p[i] << 8 | p[i + 1]);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (u16)~sum;
}

static void dhcp_send(Iface *f, int type)
{
	u8 pkt[576] = { 0 };
	u8 *ip = pkt, *udp = pkt + 20, *d = pkt + 28;
	/* BOOTP */
	d[0] = 1, d[1] = 1, d[2] = 6;
	memcpy(d + 4, &f->xid, 4);
	d[10] = 0x80; /* please broadcast the answer: we have no address yet */
	memcpy(d + 28, f->mac, 6);
	u8 *o = d + 236;
	o[0] = 99, o[1] = 130, o[2] = 83, o[3] = 99;
	o += 4;
	*o++ = 53, *o++ = 1, *o++ = (u8)type;
	*o++ = 61, *o++ = 7, *o++ = 1;
	memcpy(o, f->mac, 6), o += 6;
	*o++ = 12, *o++ = 6;
	memcpy(o, "zurbax", 6), o += 6;
	if (type == 3) {
		*o++ = 50, *o++ = 4;
		memcpy(o, &f->offered, 4), o += 4;
		*o++ = 54, *o++ = 4;
		memcpy(o, &f->server, 4), o += 4;
	}
	static const u8 wanted[] = { 1, 3, 6, 15, 26, 51, 54, 58, 59 };
	*o++ = 55, *o++ = sizeof wanted;
	memcpy(o, wanted, sizeof wanted), o += sizeof wanted;
	*o++ = 255;
	usize dlen = MAX((usize)(o - d), (usize)300);
	usize total = 28 + dlen;
	/* UDP 68 -> 67 (checksum optional over IPv4) */
	udp[0] = 0, udp[1] = 68, udp[2] = 0, udp[3] = 67;
	udp[4] = (u8)((8 + dlen) >> 8), udp[5] = (u8)(8 + dlen);
	/* IPv4 0.0.0.0 -> 255.255.255.255 */
	ip[0] = 0x45;
	ip[2] = (u8)(total >> 8), ip[3] = (u8)total;
	ip[8] = 64, ip[9] = 17;
	memset(ip + 16, 0xff, 4);
	u16 sum = ip_checksum(ip, 20);
	ip[10] = (u8)(sum >> 8), ip[11] = (u8)sum;
	struct sockaddr_ll sa = { .family = AF_PACKET, .protocol = htons(ETH_P_IP), .ifindex = f->index, .halen = 6 };
	memset(sa.addr, 0xff, 6);
	sys_sendto(f->fd, pkt, total, 0, &sa, sizeof sa);
}

static void dhcp_start(Iface *f)
{
	if (f->fd < 0) {
		f->fd = (int)sys_socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_IP));
		if (f->fd < 0)
			return;
		/* Only DHCP answers reach us (UDP to port 68), not a copy of all traffic. */
		struct { u16 code; u8 jt, jf; u32 k; } prog[] = {
			{ 0x30, 0, 0, 9 },       /* A = IP protocol */
			{ 0x15, 0, 3, 17 },      /* UDP? */
			{ 0xb1, 0, 0, 0 },       /* X = IP header length */
			{ 0x48, 0, 0, 2 },       /* A = UDP destination port */
			{ 0x15, 0, 1, 68 },
			{ 0x06, 0, 0, 0xffff },  /* accept */
			{ 0x06, 0, 0, 0 },       /* drop */
		};
		struct { u16 len; void *filter; } fprog = { ARRAY_LEN(prog), prog };
		sys_setsockopt(f->fd, SOL_SOCKET, 26 /* SO_ATTACH_FILTER */, &fprog, sizeof fprog);
		struct sockaddr_ll sa = { .family = AF_PACKET, .protocol = htons(ETH_P_IP), .ifindex = f->index };
		sys_bind(f->fd, &sa, sizeof sa);
	}
	rand_bytes(&f->xid, 4);
	f->state = DHCP_DISCOVER;
	f->tries = 0;
	f->next_ms = 0;
}

static void dhcp_stop(Iface *f)
{
	if (f->fd >= 0)
		sys_close(f->fd);
	f->fd = -1;
	f->state = DHCP_IDLE;
	if (f->configured) {
		set_address(f, f->cfg_addr, f->cfg_mask, false);
		f->configured = false;
	}
}

static void dhcp_apply(Iface *f)
{
	if (f->configured && f->cfg_addr != f->offered)
		set_address(f, f->cfg_addr, f->cfg_mask, false);
	u32 mask = f->mask ? f->mask : htonl(0xffffff00);
	bool ok = set_address(f, f->offered, mask, true);
	if (f->router)
		ok = set_default_route(f, f->router) && ok;
	f->configured = true;
	f->cfg_addr = f->offered, f->cfg_mask = mask;
	char a[20], g[20];
	ip_str(f->offered, a, sizeof a);
	ip_str(f->router, g, sizeof g);
	log_msg("%s: %s via %s%s", f->name, a, g, ok ? "" : " (configuration failed)");
}

static void dhcp_receive(Iface *f)
{
	u8 pkt[1600];
	for (;;) {
		long n = sys_recvfrom(f->fd, pkt, sizeof pkt, 0, NULL, NULL);
		if (n <= 0)
			return;
		if (n < 28 + 240 || (pkt[0] >> 4) != 4 || pkt[9] != 17)
			continue;
		usize ihl = (usize)(pkt[0] & 15) * 4;
		const u8 *udp = pkt + ihl, *d = udp + 8;
		if ((usize)n < ihl + 8 + 240 || udp[2] != 0 || udp[3] != 68 || d[0] != 2 || memcmp(d + 4, &f->xid, 4) != 0)
			continue;
		const u8 *o = d + 240, *end = pkt + n;
		int type = 0;
		u32 server = 0, mask = 0, router = 0, lease = 0, dns[3] = { 0 };
		int ndns = 0;
		while (o < end && *o != 255) {
			if (*o == 0) {
				o++;
				continue;
			}
			if (o + 2 > end || o + 2 + o[1] > end)
				break;
			u8 code = o[0], len = o[1];
			const u8 *v = o + 2;
			if (code == 53 && len >= 1)
				type = v[0];
			else if (code == 54 && len >= 4)
				memcpy(&server, v, 4);
			else if (code == 1 && len >= 4)
				memcpy(&mask, v, 4);
			else if (code == 3 && len >= 4)
				memcpy(&router, v, 4);
			else if (code == 51 && len >= 4)
				lease = (u32)v[0] << 24 | (u32)v[1] << 16 | (u32)v[2] << 8 | v[3];
			else if (code == 6)
				for (int i = 0; i + 4 <= len && ndns < 3; i += 4)
					memcpy(&dns[ndns++], v + i, 4);
			o += 2 + len;
		}
		u32 yiaddr;
		memcpy(&yiaddr, d + 16, 4);
		if (type == 2 && f->state == DHCP_DISCOVER) {
			f->offered = yiaddr, f->server = server;
			f->state = DHCP_REQUEST;
			f->tries = 0;
			f->next_ms = 0;
		} else if (type == 5 && f->state != DHCP_DISCOVER) {
			f->offered = yiaddr;
			if (server)
				f->server = server;
			f->mask = mask, f->router = router, f->lease = lease ? lease : 3600;
			memcpy(f->dns, dns, sizeof dns);
			f->ndns = ndns;
			f->state = DHCP_BOUND;
			f->bound_ms = time_mono_ms();
			/* Renew halfway through the lease. */
			f->next_ms = f->bound_ms + (u64)MIN(f->lease, 86400u) * 500;
			dhcp_apply(f);
		} else if (type == 6) {
			dhcp_start(f); /* NAK: start over */
		}
	}
}

static void dhcp_tick(Iface *f, u64 now)
{
	if (f->state == DHCP_IDLE || now < f->next_ms)
		return;
	if (f->state == DHCP_BOUND) {
		/* Renewal: ask again for the same address. */
		f->state = DHCP_REQUEST;
		f->tries = 0;
	}
	if (f->tries >= 6) {
		/* No answer: start over after a pause. */
		if (f->state == DHCP_REQUEST && f->configured) {
			dhcp_start(f);
		} else {
			f->state = DHCP_DISCOVER;
			f->tries = 0;
			f->next_ms = now + 10000;
			return;
		}
	}
	dhcp_send(f, f->state == DHCP_DISCOVER ? 1 : 3);
	f->tries++;
	f->next_ms = now + (u64)MIN(1000u << f->tries, 16000u);
}

/* ---- Status ------------------------------------------------------------------------------------ */

static char last_status[1024];

static void write_status(void)
{
	Iface *best = NULL;
	bool wifi_available = false, connecting = false;
	for (int i = 0; i < nifaces; i++) {
		Iface *f = &ifaces[i];
		if (!f->present)
			continue;
		wifi_available |= f->wireless;
		if (f->state == DHCP_BOUND && (!best || (!f->wireless && best->wireless)))
			best = f;
		if (f->carrier && f->state != DHCP_BOUND && f->state != DHCP_IDLE)
			connecting = true;
	}
	StrBuf s = { 0 };
	Iface *wired = NULL;
	for (int i = 0; i < nifaces; i++)
		if (ifaces[i].present && !ifaces[i].wireless && (!wired || ifaces[i].carrier))
			wired = &ifaces[i];
	const char *wname = "Wi-Fi";
	int wstrength = 100;
	StrBuf wifi = { 0 };
	wifi_status(&wifi, &wname, &wstrength);
	if (best) {
		char a[20], g[20];
		ip_str(best->offered, a, sizeof a);
		ip_str(best->router, g, sizeof g);
		sb_fmt(&s, "state = connected\nkind = %s\nname = %s\nstrength = %d\niface = %s\naddress = %s\ngateway = %s\ndns =",
		       best->wireless ? "wifi" : "ethernet", best->wireless ? wname : "Wired", best->wireless ? wstrength : 100, best->name, a, g);
		for (int i = 0; i < best->ndns; i++) {
			char d[20];
			ip_str(best->dns[i], d, sizeof d);
			sb_fmt(&s, " %s", d);
		}
		sb_addc(&s, '\n');
	} else {
		sb_fmt(&s, "state = %s\n", connecting ? "connecting" : "disconnected");
	}
	if (wired) {
		if (wired->state == DHCP_BOUND)
			sb_fmt(&s, wired->speed > 0 ? "ethernet = Connected · %d Mb/s\n" : "ethernet = Connected\n", wired->speed);
		else if (wired->carrier)
			sb_adds(&s, "ethernet = Connecting…\n");
	}
	if (wifi_available)
		sb_adds(&s, "wifi_available = 1\n");
	sb_add(&s, wifi.data, wifi.len);
	sb_free(&wifi);
	if (!str_eq(sb_str(&s), last_status)) {
		str_copy(last_status, sb_str(&s), sizeof last_status);
		file_write_atomic(STATUS_FILE, s.data, s.len);
	}
	sb_free(&s);
}

/* ---- Main -------------------------------------------------------------------------------------- */

int main(void)
{
	sig_ignore(SIGPIPE);
	dir_make("/run/zurbax", 0755);
	link_up("lo");
	rtnl = (int)sys_socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	wifi_init();
	write_status();
	u64 last_scan = 0;
	for (;;) {
		u64 now = time_mono_ms();
		if (now - last_scan >= 2000 || last_scan == 0) {
			last_scan = now;
			scan_links();
			for (int i = 0; i < nifaces; i++) {
				Iface *f = &ifaces[i];
				bool want = f->present && (f->wireless ? wifi_link_ok(f) : f->carrier);
				if (want && f->state == DHCP_IDLE) {
					log_msg(f->wireless ? "%s: joined a network" : "%s: cable connected", f->name);
					dhcp_start(f);
				} else if (!want && f->state != DHCP_IDLE) {
					log_msg("%s: link down", f->name);
					dhcp_stop(f);
				}
			}
		}
		wifi_tick(now);
		struct pollfd pfd[12];
		int np = 0, map[12];
		for (int i = 0; i < nifaces; i++) {
			Iface *f = &ifaces[i];
			dhcp_tick(f, now);
			if (f->fd >= 0) {
				pfd[np] = (struct pollfd){ f->fd, POLLIN, 0 };
				map[np++] = i;
			}
		}
		int nw = wifi_fds(pfd + np, 4);
		write_status();
		sys_poll(pfd, (usize)(np + nw), 250);
		for (int i = 0; i < np; i++)
			if (pfd[i].revents & POLLIN)
				dhcp_receive(&ifaces[map[i]]);
		wifi_ready(pfd + np, nw);
	}
}
