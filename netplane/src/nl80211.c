#include "nl80211.h"

#include "nlmsg.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Constants from the kernel's own headers, self-declared here rather
 * than #include'd -- the same convention include/linux_compat.h already
 * follows, and for the same reason: this platform pins what it talks to
 * instead of inheriting whatever the build host happens to ship.
 *
 * Every value below was read off <linux/nl80211.h>, <linux/genetlink.h>
 * and <linux/netlink.h> by compiling a program that printed them, not
 * recalled.
 */
#define CIX_NETLINK_GENERIC 16
#define CIX_GENL_ID_CTRL 16
#define CIX_CTRL_CMD_GETFAMILY 3
#define CIX_CTRL_ATTR_FAMILY_ID 1
#define CIX_CTRL_ATTR_FAMILY_NAME 2
#define CIX_NL80211_CMD_GET_INTERFACE 5
#define CIX_NL80211_CMD_SET_WIPHY_NETNS 49
#define CIX_NL80211_ATTR_IFINDEX 3
#define CIX_NL80211_ATTR_WIPHY 1
#define CIX_NL80211_ATTR_NETNS_FD 219

/* struct genlmsghdr, which is 4 bytes and has no packing subtleties --
 * unlike struct epoll_event, whose alignment TCC gets wrong (ADR-0008),
 * this one is three naturally-aligned fields and needs no #pragma pack.
 * Confirmed by printing sizeof() against the real header: 4. */
struct cix_genlmsghdr {
	uint8_t cmd;
	uint8_t version;
	uint16_t reserved;
};

/*
 * Reads one netlink datagram into a buffer sized from the datagram.
 *
 * Shared by every reply this file parses, for the reason #341 exists: a
 * netlink datagram larger than the buffer handed to recv() is truncated
 * and its remainder discarded, silently. The caller frees *out.
 *
 * Returns the length, or -1 with errno set.
 */
static ssize_t genl_recv_sized(int fd, char **out)
{
	ssize_t dgram, n;
	char *buf;

	*out = NULL;
	dgram = recv(fd, NULL, 0, MSG_PEEK | MSG_TRUNC);
	if (dgram < 0)
		return -1; /* errno from recv() */
	if ((size_t)dgram < sizeof(struct nlmsghdr)) {
		errno = EBADMSG;
		return -1;
	}
	buf = malloc((size_t)dgram);
	if (buf == NULL) {
		errno = ENOMEM;
		return -1;
	}
	n = recv(fd, buf, (size_t)dgram, 0);
	if (n < 0) {
		int saved = errno;

		free(buf);
		errno = saved;
		return -1;
	}
	if (n != dgram) {
		free(buf);
		errno = EBADMSG;
		return -1;
	}
	*out = buf;
	return n;
}

static int genl_open(void)
{
	int fd;
	struct sockaddr_nl addr;

	/* SOCK_CLOEXEC for the same standing reason rtnl_open() gives
	 * (ADR-0009): no fd this process opens for its own bookkeeping may
	 * leak into a clone3()'d child. */
	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, CIX_NETLINK_GENERIC);
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

/*
 * Asks the generic-netlink controller for the nl80211 family id.
 *
 * This round trip has no rtnetlink equivalent and is the reason generic
 * netlink needs its own code at all: RTM_NEWLINK is a constant, while
 * nl80211's message type is assigned at runtime and differs between
 * boots. A reply here is DATA, not an ack, so nl_msg_send_and_ack()
 * cannot be used -- it would see a non-NLMSG_ERROR reply and call it a
 * failure.
 *
 * The reply is read into a buffer sized from the datagram itself rather
 * than a fixed one. This is not caution: nl80211's family description
 * lists every command and multicast group it has, and measures 2556
 * bytes on this platform's own kernel (measured, not estimated), while
 * every other message this file sends fits in a few dozen. A netlink
 * datagram larger than the supplied buffer is TRUNCATED and its
 * remainder discarded, so a fixed buffer here does not degrade -- the
 * family lookup simply never succeeds, and every wireless operation
 * fails before it sends anything. That was #341: the failure surfaced
 * as an errno left behind by an unrelated earlier syscall, attributed
 * to a phy move that had not been attempted.
 *
 * Returns the family id, or -1.
 */
static int genl_family_id_on(int fd, const char *family)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct cix_genlmsghdr *gh;
	char *rbuf;
	ssize_t n;
	struct nlmsghdr *rnh;
	struct rtattr *rta;
	size_t remaining;
	int id = -1;
	int saved;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	gh = nl_msg_put(&m, sizeof(*gh));
	if (nh == NULL || gh == NULL) {
		errno = ENOBUFS;
		return -1;
	}
	gh->cmd = CIX_CTRL_CMD_GETFAMILY;
	gh->version = 1;
	if (nl_msg_put_attr_str(&m, CIX_CTRL_ATTR_FAMILY_NAME, family) == NULL) {
		errno = ENOBUFS;
		return -1;
	}

	nh->nlmsg_type = CIX_GENL_ID_CTRL;
	nh->nlmsg_len = (uint32_t)m.len;
	nh->nlmsg_flags = NLM_F_REQUEST;
	nh->nlmsg_seq = 1;
	nh->nlmsg_pid = 0;

	/* Same errno discipline as nl_msg_send_and_ack(): a caller that
	 * reports errno must never be handed a stale one. */
	n = send(fd, m.buf, m.len, 0);
	if (n < 0)
		return -1; /* errno from send() */
	if ((size_t)n != m.len) {
		errno = EIO;
		return -1;
	}
	n = genl_recv_sized(fd, &rbuf);
	if (n < 0)
		return -1;


	rnh = (struct nlmsghdr *)rbuf;
	if (rnh->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *err;

		if ((size_t)n < NLMSG_LENGTH(sizeof(*err))) {
			free(rbuf);
			errno = EBADMSG;
			return -1;
		}
		err = (struct nlmsgerr *)NLMSG_DATA(rnh);
		saved = err->error != 0 ? -err->error : EPROTO;
		free(rbuf);
		errno = saved;
		return -1;
	}
	if (rnh->nlmsg_len > (size_t)n || rnh->nlmsg_len < NLMSG_LENGTH(sizeof(*gh))) {
		free(rbuf);
		errno = EBADMSG;
		return -1;
	}

	/* Attributes start after the netlink header and the generic one. */
	rta = (struct rtattr *)((char *)NLMSG_DATA(rnh) + NLMSG_ALIGN(sizeof(*gh)));
	remaining = rnh->nlmsg_len - NLMSG_LENGTH(NLMSG_ALIGN(sizeof(*gh)));
	while (RTA_OK(rta, remaining)) {
		if (rta->rta_type == CIX_CTRL_ATTR_FAMILY_ID &&
		    RTA_PAYLOAD(rta) >= sizeof(uint16_t)) {
			uint16_t v;

			memcpy(&v, RTA_DATA(rta), sizeof(v));
			id = (int)v;
			break;
		}
		rta = RTA_NEXT(rta, remaining);
	}
	free(rbuf);
	if (id < 0)
		/* The controller answered, but without the one attribute the
		 * request exists to obtain. */
		errno = ENOENT;
	return id;
}

/*
 * The wiphy that owns this interface, or -1 with errno set.
 *
 * ASKED OVER NETLINK, NOT READ FROM SYSFS, AND THAT IS THE WHOLE POINT.
 *
 * This used to read /sys/class/net/<if>/phy80211, which is correct in
 * the namespace the interface is actually in and silently wrong
 * everywhere else -- because **a sysfs instance is bound to the network
 * namespace it was MOUNTED in, not to the reader's current one**.
 * Measured directly: a process that unshares into a brand-new network
 * namespace holding only `lo` still sees the host's `eth0` in
 * /sys/class/net.
 *
 * That broke teardown (#345). The teardown helper does setns() into the
 * container's network namespace and no mount-namespace change, so its
 * sysfs reads still went to the daemon's host-side view -- where the
 * interface is absent, because it is inside the container. The
 * classifier therefore answered "not wireless" for a radio, every time,
 * and the radio was sent down the rtnetlink path that returns EINVAL:
 * exactly the failure #341 opened with. An operator who deleted a
 * wireless container lost the radio until the host rebooted.
 *
 * A netlink socket has the opposite property: it is scoped to the
 * namespace it was created in, which is the one the caller means. So
 * the question is asked where the interface actually is, and one query
 * answers both halves of it -- whether there is a wiphy at all, and
 * which one -- because NL80211_CMD_GET_INTERFACE returns the wiphy
 * index in its reply. if_nametoindex() is namespace-correct for the
 * same reason.
 */
static int wiphy_for(const char *ifname)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct cix_genlmsghdr *gh;
	char *rbuf = NULL;
	ssize_t n;
	int fd, family, saved, wiphy = -1;
	unsigned int ifindex;
	struct nlmsghdr *rnh;
	struct rtattr *rta;
	size_t remaining;

	if (ifname == NULL || ifname[0] == '\0') {
		errno = EINVAL;
		return -1;
	}
	ifindex = if_nametoindex(ifname);
	if (ifindex == 0)
		return -1; /* errno from if_nametoindex(): ENODEV */

	family = nl80211_family_id();
	if (family < 0)
		return -1;
	fd = genl_open();
	if (fd < 0)
		return -1;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	gh = nl_msg_put(&m, sizeof(*gh));
	if (nh == NULL || gh == NULL) {
		close(fd);
		errno = ENOBUFS;
		return -1;
	}
	gh->cmd = CIX_NL80211_CMD_GET_INTERFACE;
	gh->version = 0;
	if (nl_msg_put_attr_u32(&m, CIX_NL80211_ATTR_IFINDEX, (uint32_t)ifindex) == NULL) {
		close(fd);
		errno = ENOBUFS;
		return -1;
	}
	nh->nlmsg_type = (uint16_t)family;
	nh->nlmsg_len = (uint32_t)m.len;
	nh->nlmsg_flags = NLM_F_REQUEST;
	nh->nlmsg_seq = 1;
	nh->nlmsg_pid = 0;

	n = send(fd, m.buf, m.len, 0);
	if (n < 0 || (size_t)n != m.len) {
		saved = n < 0 ? errno : EIO;
		close(fd);
		errno = saved;
		return -1;
	}

	n = genl_recv_sized(fd, &rbuf);
	saved = errno;
	close(fd);
	if (n < 0) {
		errno = saved;
		return -1;
	}

	rnh = (struct nlmsghdr *)rbuf;
	if (rnh->nlmsg_type == NLMSG_ERROR) {
		/* An ordinary netdev is not an nl80211 interface: the kernel
		 * says ENODEV, which is a real answer rather than a fault. */
		struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(rnh);

		saved = ((size_t)n >= NLMSG_LENGTH(sizeof(*err)) && err->error != 0) ? -err->error
		                                                                    : EPROTO;
		free(rbuf);
		errno = saved;
		return -1;
	}
	if (rnh->nlmsg_len > (size_t)n || rnh->nlmsg_len < NLMSG_LENGTH(sizeof(*gh))) {
		free(rbuf);
		errno = EBADMSG;
		return -1;
	}

	rta = (struct rtattr *)((char *)NLMSG_DATA(rnh) + NLMSG_ALIGN(sizeof(*gh)));
	remaining = rnh->nlmsg_len - NLMSG_LENGTH(NLMSG_ALIGN(sizeof(*gh)));
	while (RTA_OK(rta, remaining)) {
		if (rta->rta_type == CIX_NL80211_ATTR_WIPHY &&
		    RTA_PAYLOAD(rta) >= sizeof(uint32_t)) {
			uint32_t v;

			memcpy(&v, RTA_DATA(rta), sizeof(v));
			if (v <= 0x7fffffff)
				wiphy = (int)v;
			break;
		}
		rta = RTA_NEXT(rta, remaining);
	}
	free(rbuf);
	if (wiphy < 0)
		errno = ENOENT;
	return wiphy;
}

int nl80211_is_wireless(const char *ifname)
{
	/* One question, one implementation. "Is it wireless" is "does it
	 * have a wiphy", and asking twice in two ways is how the two
	 * answers drift apart. */
	return wiphy_for(ifname) >= 0;
}

/*
 * The move itself, with the destination namespace named by whichever
 * attribute the caller can actually supply.
 *
 * Two spellings exist because the two directions genuinely differ. Going
 * IN, the daemon has the new container's pid and no fd for its netns
 * yet. Coming back OUT, the helper is already inside the container's
 * namespace and holds an fd for the host's -- and there is no pid it
 * could name instead without assuming something about its own parent.
 * Passing the attribute in keeps one implementation for both.
 */
int nl80211_family_id(void)
{
	int fd;
	int id;
	int saved;

	fd = genl_open();
	if (fd < 0)
		return -1;
	id = genl_family_id_on(fd, "nl80211");
	saved = errno;
	close(fd);
	errno = saved;
	return id;
}

static int move_phy(const char *ifname, unsigned short attr, uint32_t value)
{
	struct nl_msg m;
	struct nlmsghdr *nh;
	struct cix_genlmsghdr *gh;
	int fd;
	int family;
	int wiphy;
	int rc;

	wiphy = wiphy_for(ifname);
	if (wiphy < 0)
		return -1; /* errno already describes why */

	/* Resolve the family before opening the socket the move goes out
	 * on. The lookup is a self-contained round trip and needs no
	 * particular socket -- a family id is a property of the kernel,
	 * not of a connection -- so it has one implementation, used here
	 * and by anything else that needs the number. */
	family = nl80211_family_id();
	if (family < 0)
		return -1;

	fd = genl_open();
	if (fd < 0)
		return -1;

	nl_msg_init(&m);
	nh = nl_msg_put(&m, sizeof(*nh));
	gh = nl_msg_put(&m, sizeof(*gh));
	if (nh == NULL || gh == NULL) {
		close(fd);
		errno = ENOBUFS;
		return -1;
	}
	gh->cmd = CIX_NL80211_CMD_SET_WIPHY_NETNS;
	gh->version = 0;

	/*
	 * The wiphy by index, and the destination namespace by an open fd.
	 * NL80211_ATTR_PID is the other spelling the kernel accepts (it is
	 * what `iw phy <phy> set netns <pid>` sends) and is deliberately
	 * not used: a pid is read in the caller's own pid namespace and
	 * re-resolved to a namespace kernel-side, while every caller here
	 * already holds the fd for the namespace it means.
	 */
	if (nl_msg_put_attr_u32(&m, CIX_NL80211_ATTR_WIPHY, (uint32_t)wiphy) == NULL ||
	    nl_msg_put_attr_u32(&m, attr, value) == NULL) {
		close(fd);
		errno = ENOBUFS;
		return -1;
	}

	nh->nlmsg_type = (uint16_t)family;
	rc = nl_msg_send_and_ack(fd, &m);
	{
		int saved = errno;

		close(fd);
		errno = saved;
	}
	return rc;
}


int nl80211_move_phy_to_netns_fd(const char *ifname, int netns_fd)
{
	if (netns_fd < 0) {
		errno = EBADF;
		return -1;
	}
	return move_phy(ifname, CIX_NL80211_ATTR_NETNS_FD, (uint32_t)netns_fd);
}
