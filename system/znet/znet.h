/* znet: the network service's shared pieces. */
#pragma once
#include "zr.h"

struct sockaddr_ll {
	u16 family;
	u16 protocol;
	i32 ifindex;
	u16 hatype;
	u8 pkttype;
	u8 halen;
	u8 addr[8];
};

enum { DHCP_IDLE, DHCP_DISCOVER, DHCP_REQUEST, DHCP_BOUND };

typedef struct {
	char name[16];
	int index;
	u8 mac[6];
	bool wireless, present, carrier;
	int speed;              /* Mb/s, wired */
	/* DHCP */
	int state;
	int fd;                 /* packet socket while DHCP runs */
	u32 xid;
	u32 offered, server, mask, router, dns[3];
	int ndns;
	u32 lease;
	u64 next_ms, bound_ms;
	int tries;
	bool configured;
	u32 cfg_addr, cfg_mask;
} Iface;

void log_msg(const char *f, ...) PRINTF_LIKE(1, 2);
void ip_str(u32 a, char *out, usize cap);
void link_up(const char *name);

/* ---- Wi-Fi (wifi.c) ---- */

/* Finds the kernel's Wi-Fi layer and opens the control socket. */
void wifi_init(void);
/* A wireless adapter was found; the first one is used. */
void wifi_adapter(Iface *f);
/* Timers: scans, timeouts, reconnecting. */
void wifi_tick(u64 now);
/* Descriptors to wait on, and handling them when ready. */
int wifi_fds(struct pollfd *pfd, int max);
void wifi_ready(struct pollfd *pfd, int n);
/* True once the adapter has joined a network (and finished the handshake). */
bool wifi_link_ok(const Iface *f);
/* Adds the Wi-Fi lines to network.status; fills the network's name and signal. */
void wifi_status(StrBuf *s, const char **name, int *strength);
