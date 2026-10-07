/* Generic netlink, for talking to the kernel's Wi-Fi layer (nl80211). */
#pragma once
#include "zr.h"

#define NETLINK_GENERIC 16
#define SOL_NETLINK 270
#define NETLINK_ADD_MEMBERSHIP 1
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define NLM_F_REQUEST 1
#define NLM_F_MULTI 2
#define NLM_F_ACK 4
#define NLM_F_DUMP 0x300
#define NLA_F_NESTED 0x8000

/* nl80211 commands and attributes (include/uapi/linux/nl80211.h) */
enum {
	NL80211_CMD_GET_INTERFACE = 5, NL80211_CMD_SET_INTERFACE = 6, NL80211_CMD_SET_KEY = 10, NL80211_CMD_NEW_KEY = 11,
	NL80211_CMD_DEL_KEY = 12, NL80211_CMD_START_AP = 15, NL80211_CMD_STOP_AP = 16, NL80211_CMD_GET_STATION = 17,
	NL80211_CMD_SET_STATION = 18, NL80211_CMD_NEW_STATION = 19, NL80211_CMD_DEL_STATION = 20, NL80211_CMD_GET_SCAN = 32,
	NL80211_CMD_TRIGGER_SCAN = 33, NL80211_CMD_NEW_SCAN_RESULTS = 34, NL80211_CMD_SCAN_ABORTED = 35,
	NL80211_CMD_DEAUTHENTICATE = 39, NL80211_CMD_DISASSOCIATE = 40, NL80211_CMD_CONNECT = 46, NL80211_CMD_ROAM = 47,
	NL80211_CMD_DISCONNECT = 48, NL80211_CMD_REGISTER_FRAME = 58, NL80211_CMD_FRAME = 59, NL80211_CMD_FRAME_TX_STATUS = 60,
};

enum {
	NL80211_ATTR_WIPHY = 1, NL80211_ATTR_IFINDEX = 3, NL80211_ATTR_IFNAME = 4, NL80211_ATTR_IFTYPE = 5, NL80211_ATTR_MAC = 6,
	NL80211_ATTR_KEY_DATA = 7, NL80211_ATTR_KEY_IDX = 8, NL80211_ATTR_KEY_CIPHER = 9, NL80211_ATTR_KEY_SEQ = 10,
	NL80211_ATTR_KEY_DEFAULT = 11, NL80211_ATTR_BEACON_INTERVAL = 12, NL80211_ATTR_DTIM_PERIOD = 13,
	NL80211_ATTR_BEACON_HEAD = 14, NL80211_ATTR_BEACON_TAIL = 15, NL80211_ATTR_STA_AID = 16, NL80211_ATTR_STA_FLAGS = 17,
	NL80211_ATTR_STA_LISTEN_INTERVAL = 18, NL80211_ATTR_STA_SUPPORTED_RATES = 19, NL80211_ATTR_STA_INFO = 21,
	NL80211_ATTR_WIPHY_FREQ = 38, NL80211_ATTR_IE = 42, NL80211_ATTR_SCAN_SSIDS = 45, NL80211_ATTR_BSS = 47,
	NL80211_ATTR_FRAME = 51, NL80211_ATTR_SSID = 52, NL80211_ATTR_AUTH_TYPE = 53, NL80211_ATTR_REASON_CODE = 54,
	NL80211_ATTR_KEY_TYPE = 55, NL80211_ATTR_STA_FLAGS2 = 67, NL80211_ATTR_CONTROL_PORT = 68, NL80211_ATTR_PRIVACY = 70,
	NL80211_ATTR_DISCONNECTED_BY_AP = 71, NL80211_ATTR_STATUS_CODE = 72, NL80211_ATTR_CIPHER_SUITES_PAIRWISE = 73,
	NL80211_ATTR_CIPHER_SUITE_GROUP = 74, NL80211_ATTR_WPA_VERSIONS = 75, NL80211_ATTR_AKM_SUITES = 76,
	NL80211_ATTR_REQ_IE = 77, NL80211_ATTR_RESP_IE = 78, NL80211_ATTR_KEY = 80, NL80211_ATTR_FRAME_MATCH = 91,
	NL80211_ATTR_FRAME_TYPE = 101, NL80211_ATTR_KEY_DEFAULT_TYPES = 110, NL80211_ATTR_HIDDEN_SSID = 126,
	NL80211_ATTR_STA_WME = 129,
};

enum { NL80211_BSS_BSSID = 1, NL80211_BSS_FREQUENCY = 2, NL80211_BSS_CAPABILITY = 5, NL80211_BSS_INFORMATION_ELEMENTS = 6,
       NL80211_BSS_SIGNAL_MBM = 7, NL80211_BSS_STATUS = 9, NL80211_BSS_SEEN_MS_AGO = 10, NL80211_BSS_BEACON_IES = 11 };
enum { NL80211_BSS_STATUS_ASSOCIATED = 1 };
enum { NL80211_IFTYPE_STATION = 2, NL80211_IFTYPE_AP = 3 };
enum { NL80211_KEYTYPE_GROUP = 0, NL80211_KEYTYPE_PAIRWISE = 1 };
enum { NL80211_KEY_DEFAULT_TYPE_UNICAST = 1, NL80211_KEY_DEFAULT_TYPE_MULTICAST = 2 };
enum { NL80211_STA_FLAG_AUTHORIZED = 1, NL80211_STA_FLAG_WME = 3, NL80211_STA_FLAG_AUTHENTICATED = 5,
       NL80211_STA_FLAG_ASSOCIATED = 7 };
enum { NL80211_STA_INFO_SIGNAL = 7 };
enum { NL80211_WPA_VERSION_2 = 1 << 1 };

/* Message building: a netlink header, a generic netlink header, then attributes. */
typedef struct {
	u8 buf[4096];
	usize len;
} NlMsg;

void nlm_begin(NlMsg *m, u16 family, u16 flags, u8 cmd);
void nlm_put(NlMsg *m, u16 type, const void *data, usize n);
static inline void nlm_u8(NlMsg *m, u16 type, u8 v) { nlm_put(m, type, &v, 1); }
static inline void nlm_u16(NlMsg *m, u16 type, u16 v) { nlm_put(m, type, &v, 2); }
static inline void nlm_u32(NlMsg *m, u16 type, u32 v) { nlm_put(m, type, &v, 4); }
static inline void nlm_flag(NlMsg *m, u16 type) { nlm_put(m, type, NULL, 0); }
usize nlm_nest(NlMsg *m, u16 type);
void nlm_nest_end(NlMsg *m, usize at);

/* Attribute parsing: tb[type] for each attribute found (types up to max). */
typedef struct {
	const u8 *data;
	u16 len;
	bool present;
} NlAttr;

void nla_parse(const u8 *p, usize n, NlAttr *tb, int max);
static inline u8 nla_u8(const NlAttr *a) { return a->present && a->len >= 1 ? a->data[0] : 0; }
static inline u16 nla_u16(const NlAttr *a) { u16 v = 0; if (a->present && a->len >= 2) memcpy(&v, a->data, 2); return v; }
static inline u32 nla_u32(const NlAttr *a) { u32 v = 0; if (a->present && a->len >= 4) memcpy(&v, a->data, 4); return v; }

typedef struct {
	int fd;
	u32 seq;
	u16 family;               /* nl80211 */
	u32 grp_scan, grp_mlme;   /* multicast groups */
} Genl;

/* Opens a socket and finds the nl80211 family; false if there's no Wi-Fi in the kernel. */
bool genl_open(Genl *g);
/* Joins a multicast group, for events. */
bool genl_join(Genl *g, u32 group);
/* Sends a request and reads replies until the acknowledgement (or end of a dump),
 * calling cb with each reply's command and attributes. Returns 0 or a negative errno. */
typedef void (*GenlCb)(void *user, u8 cmd, const u8 *attrs, usize n);
int genl_call(Genl *g, NlMsg *m, GenlCb cb, void *user);
/* Reads one waiting event without blocking; false if none. */
bool genl_event(Genl *g, GenlCb cb, void *user);
