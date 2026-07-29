#include "rtnetlink.h"

#include <errno.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NL_MSG_MAX 1024

/*
 * A single netlink request being built up in a fixed buffer. Nothing
 * here is ever used concurrently across sends (one socket, one
 * request/response at a time), so a hardcoded sequence number is
 * fine -- there is never another in-flight message to confuse it
 * with.
 */
struct nl_msg {
	char buf[NL_MSG_MAX];
	size_t len;
};

static void nl_msg_init(struct nl_msg *m)
{
	memset(m->buf, 0, sizeof(m->buf));
	m->len = 0;
}

/* Reserves `size` bytes (NLMSG_ALIGN'd, zero-filled) at the end of
 * the message. NULL if it wouldn't fit. */
static void *nl_msg_put(struct nl_msg *m, size_t size)
{
	size_t aligned = NLMSG_ALIGN(size);
	void *p;

	if (m->len + aligned > sizeof(m->buf))
		return NULL;
	p = m->buf + m->len;
	m->len += aligned;
	return p;
}

static struct rtattr *nl_msg_put_attr(struct nl_msg *m, unsigned short type, const void *data,
                                      size_t data_len)
{
	size_t attr_len = RTA_LENGTH(data_len);
	size_t aligned = RTA_ALIGN(attr_len);
	struct rtattr *rta;

	if (m->len + aligned > sizeof(m->buf))
		return NULL;

	rta = (struct rtattr *)(m->buf + m->len);
	rta->rta_type = type;
	rta->rta_len = (unsigned short)attr_len;
	if (data_len > 0 && data != NULL)
		memcpy(RTA_DATA(rta), data, data_len);
	m->len += aligned;
	return rta;
}

static struct rtattr *nl_msg_put_attr_str(struct nl_msg *m, unsigned short type, const char *s)
{
	return nl_msg_put_attr(m, type, s, strlen(s) + 1);
}

static struct rtattr *nl_msg_put_attr_u32(struct nl_msg *m, unsigned short type, uint32_t v)
{
	return nl_msg_put_attr(m, type, &v, sizeof(v));
}

/* Reserves a nested attribute's header; its rta_len is backfilled by
 * nl_msg_nest_end() once everything inside it has been appended. */
static struct rtattr *nl_msg_nest_start(struct nl_msg *m, unsigned short type)
{
	return nl_msg_put_attr(m, type, NULL, 0);
}

static void nl_msg_nest_end(struct nl_msg *m, struct rtattr *nest)
{
	size_t nest_len = (size_t)((m->buf + m->len) - (char *)nest);

	nest->rta_len = (unsigned short)nest_len;
}

static int nl_msg_send_and_ack(int fd, struct nl_msg *m)
{
	struct nlmsghdr *nh = (struct nlmsghdr *)m->buf;
	char rbuf[NL_MSG_MAX];
	ssize_t n;
	struct nlmsghdr *rnh;
	struct nlmsgerr *err;

	nh->nlmsg_len = (uint32_t)m->len;
	nh->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
	nh->nlmsg_seq = 1;
	nh->nlmsg_pid = 0;

	n = send(fd, m->buf, m->len, 0);
	if (n < 0 || (size_t)n != m->len)
		return -1;

	n = recv(fd, rbuf, sizeof(rbuf), 0);
	if (n < 0)
		return -1;
	if ((size_t)n < sizeof(*rnh))
		return -1;

	rnh = (struct nlmsghdr *)rbuf;
	if (rnh->nlmsg_type != NLMSG_ERROR)
		return -1;
	if ((size_t)n < NLMSG_LENGTH(sizeof(*err)))
		return -1;

	err = (struct nlmsgerr *)NLMSG_DATA(rnh);
	if (err->error != 0) {
		errno = -err->error;
		return -1;
	}
	return 0;
}

int rtnl_open(void)
{
	int fd;
	struct sockaddr_nl addr;

	/*
	 * SOCK_CLOEXEC per the standing rule from ADR-0009: any fd a
	 * long-running process opens for its own bookkeeping must not
	 * leak into a clone3()'d child.
	 */
	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

void rtnl_close(int fd)
{
	close(fd);
}

int rtnl_bridge_create(int fd, const char *name)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct ifinfomsg *ifi;
	struct rtattr *linkinfo;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	ifi = nl_msg_put(&m, sizeof(*ifi));
	if (nh == NULL || ifi == NULL)
		return -1;
	ifi->ifi_family = AF_UNSPEC;

	if (nl_msg_put_attr_str(&m, IFLA_IFNAME, name) == NULL)
		return -1;

	linkinfo = nl_msg_nest_start(&m, IFLA_LINKINFO);
	if (linkinfo == NULL)
		return -1;
	if (nl_msg_put_attr_str(&m, IFLA_INFO_KIND, "bridge") == NULL)
		return -1;
	nl_msg_nest_end(&m, linkinfo);

	nh->nlmsg_type = RTM_NEWLINK;
	nh->nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL;
	return nl_msg_send_and_ack(fd, &m);
}

int rtnl_veth_create(int fd, const char *name, const char *peer_name)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct ifinfomsg *ifi;
	struct ifinfomsg *peer_ifi;
	struct rtattr *linkinfo, *infodata, *vethpeer;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	ifi = nl_msg_put(&m, sizeof(*ifi));
	if (nh == NULL || ifi == NULL)
		return -1;
	ifi->ifi_family = AF_UNSPEC;

	if (nl_msg_put_attr_str(&m, IFLA_IFNAME, name) == NULL)
		return -1;

	linkinfo = nl_msg_nest_start(&m, IFLA_LINKINFO);
	if (linkinfo == NULL)
		return -1;
	if (nl_msg_put_attr_str(&m, IFLA_INFO_KIND, "veth") == NULL)
		return -1;

	infodata = nl_msg_nest_start(&m, IFLA_INFO_DATA);
	if (infodata == NULL)
		return -1;

	/*
	 * The kernel's veth driver (drivers/net/veth.c, veth_newlink())
	 * expects VETH_INFO_PEER's payload to open with a raw embedded
	 * struct ifinfomsg (not itself an rtattr), followed by ordinary
	 * IFLA_* attributes -- here just the peer's name.
	 */
	vethpeer = nl_msg_nest_start(&m, VETH_INFO_PEER);
	if (vethpeer == NULL)
		return -1;
	peer_ifi = nl_msg_put(&m, sizeof(*peer_ifi));
	if (peer_ifi == NULL)
		return -1;
	peer_ifi->ifi_family = AF_UNSPEC;
	if (nl_msg_put_attr_str(&m, IFLA_IFNAME, peer_name) == NULL)
		return -1;
	nl_msg_nest_end(&m, vethpeer);

	nl_msg_nest_end(&m, infodata);
	nl_msg_nest_end(&m, linkinfo);

	nh->nlmsg_type = RTM_NEWLINK;
	nh->nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL;
	return nl_msg_send_and_ack(fd, &m);
}

int rtnl_link_set_netns_pid(int fd, const char *name, pid_t pid)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct ifinfomsg *ifi;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	ifi = nl_msg_put(&m, sizeof(*ifi));
	if (nh == NULL || ifi == NULL)
		return -1;
	ifi->ifi_family = AF_UNSPEC;

	if (nl_msg_put_attr_str(&m, IFLA_IFNAME, name) == NULL)
		return -1;
	if (nl_msg_put_attr_u32(&m, IFLA_NET_NS_PID, (uint32_t)pid) == NULL)
		return -1;

	nh->nlmsg_type = RTM_NEWLINK;
	nh->nlmsg_flags = 0;
	return nl_msg_send_and_ack(fd, &m);
}

int rtnl_link_set_netns_fd(int fd, const char *name, int target_netns_fd)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct ifinfomsg *ifi;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	ifi = nl_msg_put(&m, sizeof(*ifi));
	if (nh == NULL || ifi == NULL)
		return -1;
	ifi->ifi_family = AF_UNSPEC;

	if (nl_msg_put_attr_str(&m, IFLA_IFNAME, name) == NULL)
		return -1;
	if (nl_msg_put_attr_u32(&m, IFLA_NET_NS_FD, (uint32_t)target_netns_fd) == NULL)
		return -1;

	nh->nlmsg_type = RTM_NEWLINK;
	nh->nlmsg_flags = 0;
	return nl_msg_send_and_ack(fd, &m);
}

int rtnl_link_set_master(int fd, const char *name, const char *bridge_name)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct ifinfomsg *ifi;
	unsigned int master_ifindex;

	master_ifindex = if_nametoindex(bridge_name);
	if (master_ifindex == 0)
		return -1;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	ifi = nl_msg_put(&m, sizeof(*ifi));
	if (nh == NULL || ifi == NULL)
		return -1;
	ifi->ifi_family = AF_UNSPEC;

	if (nl_msg_put_attr_str(&m, IFLA_IFNAME, name) == NULL)
		return -1;
	if (nl_msg_put_attr_u32(&m, IFLA_MASTER, (uint32_t)master_ifindex) == NULL)
		return -1;

	nh->nlmsg_type = RTM_NEWLINK;
	nh->nlmsg_flags = 0;
	return nl_msg_send_and_ack(fd, &m);
}

int rtnl_link_set_up(int fd, const char *name)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct ifinfomsg *ifi;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	ifi = nl_msg_put(&m, sizeof(*ifi));
	if (nh == NULL || ifi == NULL)
		return -1;
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_flags = IFF_UP;
	ifi->ifi_change = IFF_UP;

	if (nl_msg_put_attr_str(&m, IFLA_IFNAME, name) == NULL)
		return -1;

	nh->nlmsg_type = RTM_NEWLINK;
	nh->nlmsg_flags = 0;
	return nl_msg_send_and_ack(fd, &m);
}

int rtnl_addr_add_ipv4(int fd, const char *link_name, uint32_t addr_be, int prefix_len)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct ifaddrmsg *ifa;
	unsigned int ifindex;

	ifindex = if_nametoindex(link_name);
	if (ifindex == 0)
		return -1;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	ifa = nl_msg_put(&m, sizeof(*ifa));
	if (nh == NULL || ifa == NULL)
		return -1;
	ifa->ifa_family = AF_INET;
	ifa->ifa_prefixlen = (unsigned char)prefix_len;
	ifa->ifa_index = ifindex;

	if (nl_msg_put_attr(&m, IFA_LOCAL, &addr_be, sizeof(addr_be)) == NULL)
		return -1;
	if (nl_msg_put_attr(&m, IFA_ADDRESS, &addr_be, sizeof(addr_be)) == NULL)
		return -1;

	nh->nlmsg_type = RTM_NEWADDR;
	nh->nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL;
	return nl_msg_send_and_ack(fd, &m);
}

int rtnl_route_add_ipv4(int fd, uint32_t dest_be, int dest_prefix_len, uint32_t gateway_be)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct rtmsg *rtm;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	rtm = nl_msg_put(&m, sizeof(*rtm));
	if (nh == NULL || rtm == NULL)
		return -1;
	rtm->rtm_family = AF_INET;
	rtm->rtm_dst_len = (unsigned char)dest_prefix_len;
	rtm->rtm_src_len = 0;
	rtm->rtm_tos = 0;
	rtm->rtm_table = RT_TABLE_MAIN;
	rtm->rtm_protocol = RTPROT_STATIC;
	rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	rtm->rtm_type = RTN_UNICAST;

	if (dest_prefix_len > 0 && nl_msg_put_attr(&m, RTA_DST, &dest_be, sizeof(dest_be)) == NULL)
		return -1;
	if (gateway_be != 0 && nl_msg_put_attr(&m, RTA_GATEWAY, &gateway_be, sizeof(gateway_be)) == NULL)
		return -1;

	nh->nlmsg_type = RTM_NEWROUTE;
	nh->nlmsg_flags = NLM_F_CREATE;
	return nl_msg_send_and_ack(fd, &m);
}

int rtnl_route_add_default_ipv4(int fd, uint32_t gateway_be)
{
	return rtnl_route_add_ipv4(fd, 0, 0, gateway_be);
}

int rtnl_link_delete(int fd, const char *name)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct ifinfomsg *ifi;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	ifi = nl_msg_put(&m, sizeof(*ifi));
	if (nh == NULL || ifi == NULL)
		return -1;
	ifi->ifi_family = AF_UNSPEC;

	if (nl_msg_put_attr_str(&m, IFLA_IFNAME, name) == NULL)
		return -1;

	nh->nlmsg_type = RTM_DELLINK;
	nh->nlmsg_flags = 0;
	return nl_msg_send_and_ack(fd, &m);
}

int rtnl_link_rename(int fd, const char *old_name, const char *new_name)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct ifinfomsg *ifi;
	unsigned int ifindex;

	ifindex = if_nametoindex(old_name);
	if (ifindex == 0)
		return -1;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	ifi = nl_msg_put(&m, sizeof(*ifi));
	if (nh == NULL || ifi == NULL)
		return -1;
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = (int)ifindex;

	if (nl_msg_put_attr_str(&m, IFLA_IFNAME, new_name) == NULL)
		return -1;

	nh->nlmsg_type = RTM_NEWLINK;
	nh->nlmsg_flags = 0;
	return nl_msg_send_and_ack(fd, &m);
}
