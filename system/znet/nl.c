/* Generic netlink: building requests, reading replies and events. */
#include "nl.h"

#define GENL_ID_CTRL 0x10
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2
#define CTRL_ATTR_MCAST_GROUPS 7
#define CTRL_ATTR_MCAST_GRP_NAME 1
#define CTRL_ATTR_MCAST_GRP_ID 2

#define NLMSG_HDR 16
#define GENL_HDR 4
#define ALIGN4(n) (((n) + 3) & ~(usize)3)

struct sockaddr_nl {
	u16 family;
	u16 pad;
	u32 pid;
	u32 groups;
};

void nlm_begin(NlMsg *m, u16 family, u16 flags, u8 cmd)
{
	memset(m->buf, 0, NLMSG_HDR + GENL_HDR);
	memcpy(m->buf + 4, &family, 2);
	u16 f = (u16)(flags | NLM_F_REQUEST);
	memcpy(m->buf + 6, &f, 2);
	m->buf[NLMSG_HDR] = cmd;
	m->buf[NLMSG_HDR + 1] = 1;   /* version */
	m->len = NLMSG_HDR + GENL_HDR;
}

void nlm_put(NlMsg *m, u16 type, const void *data, usize n)
{
	usize total = 4 + n;
	if (m->len + ALIGN4(total) > sizeof m->buf)
		return;
	u16 l = (u16)total;
	memcpy(m->buf + m->len, &l, 2);
	memcpy(m->buf + m->len + 2, &type, 2);
	if (n)
		memcpy(m->buf + m->len + 4, data, n);
	memset(m->buf + m->len + total, 0, ALIGN4(total) - total);
	m->len += ALIGN4(total);
}

usize nlm_nest(NlMsg *m, u16 type)
{
	usize at = m->len;
	nlm_put(m, (u16)(type | NLA_F_NESTED), NULL, 0);
	return at;
}

void nlm_nest_end(NlMsg *m, usize at)
{
	u16 l = (u16)(m->len - at);
	memcpy(m->buf + at, &l, 2);
}

void nla_parse(const u8 *p, usize n, NlAttr *tb, int max)
{
	memset(tb, 0, sizeof(NlAttr) * (usize)(max + 1));
	usize off = 0;
	while (off + 4 <= n) {
		u16 len, type;
		memcpy(&len, p + off, 2);
		memcpy(&type, p + off + 2, 2);
		if (len < 4 || off + len > n)
			break;
		type &= 0x3fff;
		if (type <= max) {
			tb[type].data = p + off + 4;
			tb[type].len = (u16)(len - 4);
			tb[type].present = true;
		}
		off += ALIGN4(len);
	}
}

static int nl_send(Genl *g, NlMsg *m)
{
	u32 l = (u32)m->len, seq = ++g->seq;
	memcpy(m->buf, &l, 4);
	memcpy(m->buf + 8, &seq, 4);
	struct sockaddr_nl to = { .family = AF_NETLINK };
	long r = sys_sendto(g->fd, m->buf, m->len, 0, &to, sizeof to);
	return r < 0 ? (int)r : 0;
}

/* Walks the messages in one datagram. Returns 1 when the request is finished,
 * 0 to keep reading, or a negative errno from the kernel. */
static int walk(Genl *g, const u8 *buf, usize n, u32 seq, GenlCb cb, void *user)
{
	usize off = 0;
	int result = 0;
	while (off + NLMSG_HDR <= n) {
		u32 len, mseq;
		u16 type, flags;
		memcpy(&len, buf + off, 4);
		memcpy(&type, buf + off + 4, 2);
		memcpy(&flags, buf + off + 6, 2);
		memcpy(&mseq, buf + off + 8, 4);
		if (len < NLMSG_HDR || off + len > n)
			break;
		const u8 *body = buf + off + NLMSG_HDR;
		usize blen = len - NLMSG_HDR;
		if (seq && mseq != seq) {
			/* a stale reply to something earlier */
		} else if (type == NLMSG_ERROR) {
			i32 err = 0;
			if (blen >= 4)
				memcpy(&err, body, 4);
			return err ? err : 1;
		} else if (type == NLMSG_DONE) {
			return 1;
		} else if (blen >= GENL_HDR && cb) {
			cb(user, body[0], body + GENL_HDR, blen - GENL_HDR);
			if (!(flags & NLM_F_MULTI) && !seq)
				result = 1;
		}
		off += ALIGN4(len);
	}
	return result;
}

int genl_call(Genl *g, NlMsg *m, GenlCb cb, void *user)
{
	/* every request asks for an acknowledgement, so the end is always clear */
	u16 flags;
	memcpy(&flags, m->buf + 6, 2);
	flags |= NLM_F_ACK;
	memcpy(m->buf + 6, &flags, 2);
	int r = nl_send(g, m);
	if (r < 0)
		return r;
	u32 seq = g->seq;
	static u8 buf[65536];
	for (;;) {
		struct pollfd p = { g->fd, POLLIN, 0 };
		if (sys_poll(&p, 1, 3000) <= 0)
			return -ETIMEDOUT;
		long n = sys_recvfrom(g->fd, buf, sizeof buf, 0, NULL, NULL);
		if (n < 0)
			return (int)n;
		r = walk(g, buf, (usize)n, seq, cb, user);
		if (r)
			return r < 0 ? r : 0;
	}
}

bool genl_event(Genl *g, GenlCb cb, void *user)
{
	static u8 buf[65536];
	long n = sys_recvfrom(g->fd, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL);
	if (n <= 0)
		return false;
	walk(g, buf, (usize)n, 0, cb, user);
	return true;
}

typedef struct {
	Genl *g;
	bool found;
} FamilyCtx;

static void family_cb(void *user, u8 cmd, const u8 *attrs, usize n)
{
	FamilyCtx *c = user;
	NlAttr tb[8];
	nla_parse(attrs, n, tb, 7);
	if (!tb[CTRL_ATTR_FAMILY_ID].present)
		return;
	c->g->family = nla_u16(&tb[CTRL_ATTR_FAMILY_ID]);
	c->found = true;
	if (!tb[CTRL_ATTR_MCAST_GROUPS].present)
		return;
	/* a list of nested groups, each with a name and an id */
	const u8 *p = tb[CTRL_ATTR_MCAST_GROUPS].data;
	usize len = tb[CTRL_ATTR_MCAST_GROUPS].len, off = 0;
	while (off + 4 <= len) {
		u16 l;
		memcpy(&l, p + off, 2);
		if (l < 4 || off + l > len)
			break;
		NlAttr gt[3];
		nla_parse(p + off + 4, l - 4u, gt, 2);
		if (gt[CTRL_ATTR_MCAST_GRP_NAME].present) {
			const char *name = (const char *)gt[CTRL_ATTR_MCAST_GRP_NAME].data;
			u32 id = nla_u32(&gt[CTRL_ATTR_MCAST_GRP_ID]);
			if (str_eq(name, "scan"))
				c->g->grp_scan = id;
			else if (str_eq(name, "mlme"))
				c->g->grp_mlme = id;
		}
		off += ALIGN4(l);
	}
}

bool genl_open(Genl *g)
{
	memset(g, 0, sizeof *g);
	g->fd = (int)sys_socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (g->fd < 0)
		return false;
	struct sockaddr_nl me = { .family = AF_NETLINK };
	sys_bind(g->fd, &me, sizeof me);
	NlMsg m;
	nlm_begin(&m, GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY);
	nlm_put(&m, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
	FamilyCtx c = { g, false };
	if (genl_call(g, &m, family_cb, &c) < 0 || !c.found) {
		sys_close(g->fd);
		g->fd = -1;
		return false;
	}
	return true;
}

bool genl_join(Genl *g, u32 group)
{
	return group && sys_setsockopt(g->fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, 4) == 0;
}
