/* Wi-Fi through the network service: the networks it sees, and requests. */
#include "zui.h"

#define NETWORKS_FILE "/run/zurbax/wifi.networks"
#define CTL_SOCKET "/run/zurbax/wifi.sock"

/* SSIDs are bytes; show them as UTF-8, with '?' for anything that isn't. */
static void ssid_text(const u8 *b, usize n, char *out, usize cap)
{
	usize w = 0;
	for (usize i = 0; i < n && w + 4 < cap;) {
		u32 cp;
		usize len = utf8_decode((const char *)b + i, n - i, &cp);
		if (!len || cp < 0x20 || cp == 0x7f) {
			out[w++] = '?';
			i++;
			continue;
		}
		memcpy(out + w, b + i, len);
		w += len;
		i += len;
	}
	out[w] = 0;
}

int wifi_networks(WifiNet *out, int max)
{
	char *text = file_read(NETWORKS_FILE, NULL);
	if (!text)
		return 0;
	int n = 0;
	for (char *line = text; *line && n < max;) {
		char *end = (char *)str_chr(line, '\n');
		if (end)
			*end = 0;
		char *f[5] = { 0 };
		int nf = 0;
		for (char *p = line; nf < 5; ) {
			f[nf++] = p;
			char *tab = (char *)str_chr(p, '\t');
			if (!tab)
				break;
			*tab = 0;
			p = tab + 1;
		}
		if (nf == 5 && strlen(f[4]) <= 64 && strlen(f[4]) % 2 == 0) {
			WifiNet *w = &out[n];
			memset(w, 0, sizeof *w);
			w->signal = (int)str_to_int(f[0], NULL);
			str_copy(w->security, f[1], sizeof w->security);
			w->saved = f[2][0] == '1';
			w->connected = f[3][0] == '1';
			str_copy(w->hex, f[4], sizeof w->hex);
			u8 raw[32];
			usize rl = strlen(f[4]) / 2;
			if (hex_decode(f[4], raw, rl)) {
				ssid_text(raw, rl, w->ssid, sizeof w->ssid);
				n++;
			}
		}
		if (!end)
			break;
		line = end + 1;
	}
	mem_free(text);
	return n;
}

bool wifi_supported(const WifiNet *w)
{
	return str_eq(w->security, "open") || str_eq(w->security, "wpa2");
}

bool wifi_command(const char *cmd)
{
	int fd = (int)sys_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	str_copy(a.sun_path, CTL_SOCKET, sizeof a.sun_path);
	bool ok = sys_sendto(fd, cmd, strlen(cmd), 0, &a, sizeof a) >= 0;
	sys_close(fd);
	return ok;
}

void wifi_connect(const char *ssid_hex, const char *password)
{
	StrBuf sb = { 0 };
	sb_fmt(&sb, "connect %s", ssid_hex);
	if (password && *password) {
		usize n = strlen(password);
		char *hex = mem_alloc(n * 2 + 1);
		hex_encode((const u8 *)password, n, hex);
		sb_fmt(&sb, " %s", hex);
		memset(hex, 0, n * 2);
		mem_free(hex);
	}
	wifi_command(sb_str(&sb));
	memset(sb.data, 0, sb.len);
	sb_free(&sb);
}

const char *wifi_signal_icon(int signal)
{
	return signal > 66 ? "network-wireless" : signal > 33 ? "network-wireless-good" : "network-wireless-weak";
}
