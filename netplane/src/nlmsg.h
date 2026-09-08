#ifndef NLMSG_H
#define NLMSG_H

/*
 * Building and sending one netlink request, shared by every family
 * this platform speaks.
 *
 * These used to be file-static in rtnetlink.c, which was correct while
 * rtnetlink was the only family. It is not any more: moving a wireless
 * PHY between network namespaces is an nl80211 operation, and nl80211
 * is generic netlink -- a different family on a different protocol,
 * built out of exactly the same message plumbing (#341).
 *
 * A header of static functions rather than a fourth .c file, and
 * deliberately `static` rather than `inline`: TCC emits a bare `inline`
 * definition as a strong global in every translation unit that includes
 * it, so two includers collide at link with "defined twice" (CLAUDE.md,
 * and the m4 build that found it). `static` is the spelling that works
 * on both compilers, at the cost of one copy per includer, which for
 * functions this small is not a cost worth engineering around.
 */

#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

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


#endif /* NLMSG_H */
