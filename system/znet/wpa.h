/* WPA2-Personal: keys and EAPOL-Key frames for the 4-way and group key
 * handshakes (IEEE 802.11-2016, 12.7). Pure functions, shared by the network
 * service and the test access point. */
#pragma once
#include "zr.h"
#include "zn.h"

#define ETH_P_PAE 0x888e

/* Key Information bits */
enum {
	KI_VER_MASK = 7, KI_VER_SHA1_AES = 2,   /* HMAC-SHA1 MIC, AES key wrap */
	KI_PAIRWISE = 1 << 3, KI_INSTALL = 1 << 6, KI_ACK = 1 << 7, KI_MIC = 1 << 8, KI_SECURE = 1 << 9,
	KI_ERROR = 1 << 10, KI_REQUEST = 1 << 11, KI_ENCRYPTED = 1 << 12,
};

/* Cipher and key management suites (00-0F-AC:n, as a number for nl80211) */
#define SUITE(n) (0x000fac00u | (n))
enum { CIPHER_TKIP = 2, CIPHER_CCMP = 4, AKM_8021X = 1, AKM_PSK = 2, AKM_PSK_SHA256 = 6, AKM_SAE = 8 };

#define EAPOL_KEY_HDR 99   /* 4 bytes of EAPOL header + 95 of key descriptor */

typedef struct {
	u8 version;              /* EAPOL protocol version (answers use the peer's) */
	u16 info;
	u16 key_len;
	u8 replay[8];
	u8 nonce[32];
	u8 rsc[8];
	u8 mic[16];
	const u8 *data;          /* key data, inside the frame */
	u16 data_len;
} EapolKey;

typedef struct {
	u8 kck[16], kek[16], tk[16];
} Ptk;

typedef struct {
	u32 group;
	u32 pairwise[4];
	int npairwise;
	u32 akm[4];
	int nakm;
	u16 caps;
} Rsn;

/* PMK = PBKDF2-SHA1(passphrase, SSID, 4096 rounds). */
void wpa_pmk(const char *pass, const u8 *ssid, usize ssid_len, u8 pmk[32]);
/* PTK from the PMK, both addresses and both nonces. */
void wpa_ptk(const u8 pmk[32], const u8 aa[6], const u8 spa[6], const u8 anonce[32], const u8 snonce[32], Ptk *out);
/* IEEE 802.11 PRF with HMAC-SHA1. */
void wpa_prf(const u8 *key, usize klen, const char *label, const u8 *data, usize dlen, u8 *out, usize olen);

bool eapol_key_parse(const u8 *frame, usize n, EapolKey *k);
/* Builds a frame from k (info, key_len, replay, nonce, rsc) and key data; with a
 * KCK, the MIC is filled in. Returns its length. */
usize eapol_key_build(u8 *out, usize cap, const EapolKey *k, const u8 *data, usize dlen, const u8 *kck);
bool eapol_key_mic_ok(const u8 *frame, usize n, const u8 kck[16]);

/* Key data in messages 3 and group 1 is wrapped with the KEK. */
bool wpa_unwrap(const u8 kek[16], const u8 *in, usize n, u8 *out, usize *out_len);
usize wpa_wrap(const u8 kek[16], const u8 *in, usize n, u8 *out);   /* pads as the standard says */

/* Finds the GTK key data element (KDE) in unwrapped key data. */
bool wpa_find_gtk(const u8 *data, usize n, u8 *gtk, usize *gtk_len, int *key_id);
/* Finds an element by id in a list of elements (beacons, key data); returns it
 * with its two header bytes, or NULL. */
const u8 *wpa_find_ie(const u8 *ies, usize n, u8 id);
/* Parses the body of an RSN element (after id and length). */
bool rsn_parse(const u8 *body, usize n, Rsn *r);
/* Our RSN element for WPA2-PSK with CCMP pairwise and the given group cipher. */
usize rsn_element(u8 *out, u32 group_cipher);
