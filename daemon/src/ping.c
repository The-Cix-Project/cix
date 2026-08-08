#include "ping.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PING_PAYLOAD_LEN 32
#define PING_RECV_BUF_LEN 512 /* IP header (<=60 bytes, incl. options) + ICMP header (8) + our own PING_PAYLOAD_LEN -- comfortably above the real max */

static struct {
	enum ping_state state;
	uint32_t target_ip_be;
	uint16_t ident;
	uint16_t seq;
	struct timespec sent_at;
	int reachable;
	int timed_out;
	double rtt_ms;
} g_ping;

/* RFC 1071 Internet checksum -- the same algorithm every IP/ICMP/TCP/UDP
 * header checksum in existence uses. len is in bytes; an odd trailing
 * byte (never the case for our own fixed-size packet, but kept general)
 * is padded with a zero high byte, matching the RFC's own definition. */
static uint16_t icmp_checksum(const void *data, size_t len)
{
	const uint16_t *p = data;
	uint32_t sum = 0;

	for (; len > 1; len -= 2)
		sum += *p++;
	if (len == 1)
		sum += *(const uint8_t *)p;
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return (uint16_t)~sum;
}

enum ping_error ping_start(uint32_t target_ip_be, int *out_sockfd)
{
	int fd;
	struct sockaddr_in dst;
	struct {
		struct icmphdr hdr;
		char payload[PING_PAYLOAD_LEN];
	} pkt;
	static uint16_t next_seq = 1;

	if (g_ping.state == PING_STATE_PENDING)
		return PING_ERR_BUSY;

	fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
	if (fd < 0)
		return PING_ERR_SOCKET_FAILED;

	memset(&pkt, 0, sizeof(pkt));
	pkt.hdr.type = ICMP_ECHO;
	pkt.hdr.code = 0;
	pkt.hdr.un.echo.id = (uint16_t)(getpid() & 0xffff);
	pkt.hdr.un.echo.sequence = next_seq++;
	memset(pkt.payload, 0x42, sizeof(pkt.payload));
	pkt.hdr.checksum = 0;
	pkt.hdr.checksum = icmp_checksum(&pkt, sizeof(pkt));

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_addr.s_addr = target_ip_be;

	if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
		close(fd);
		return PING_ERR_SOCKET_FAILED;
	}

	memset(&g_ping, 0, sizeof(g_ping));
	g_ping.state = PING_STATE_PENDING;
	g_ping.target_ip_be = target_ip_be;
	g_ping.ident = pkt.hdr.un.echo.id;
	g_ping.seq = pkt.hdr.un.echo.sequence;
	clock_gettime(CLOCK_MONOTONIC, &g_ping.sent_at);

	*out_sockfd = fd;
	return PING_OK;
}

int ping_handle_reply(int sockfd)
{
	char buf[PING_RECV_BUF_LEN];
	ssize_t n;
	const struct iphdr *ip;
	const struct icmphdr *icmp;
	size_t ip_hlen;
	struct timespec now;

	n = recvfrom(sockfd, buf, sizeof(buf), 0, NULL, NULL);
	if (n < (ssize_t)sizeof(struct iphdr))
		return 0; /* too short to even have an IP header -- not our reply */

	ip = (const struct iphdr *)buf;
	ip_hlen = (size_t)ip->ihl * 4;
	if ((size_t)n < ip_hlen + sizeof(struct icmphdr))
		return 0;

	icmp = (const struct icmphdr *)(buf + ip_hlen);
	if (icmp->type != ICMP_ECHOREPLY)
		return 0; /* some other ICMP traffic entirely (unreachable, redirect, someone
	                   * else's own echo request/reply) -- this raw socket sees all of it */
	if (icmp->un.echo.id != g_ping.ident || icmp->un.echo.sequence != g_ping.seq)
		return 0; /* not this job's own request -- e.g. a real `ping` command
	                   * running elsewhere on the same box at the same time */

	clock_gettime(CLOCK_MONOTONIC, &now);
	g_ping.rtt_ms = (double)(now.tv_sec - g_ping.sent_at.tv_sec) * 1000.0 +
	                (double)(now.tv_nsec - g_ping.sent_at.tv_nsec) / 1000000.0;
	g_ping.reachable = 1;
	g_ping.timed_out = 0;
	g_ping.state = PING_STATE_DONE;
	return 1;
}

void ping_handle_timeout(void)
{
	g_ping.reachable = 0;
	g_ping.timed_out = 1;
	g_ping.rtt_ms = 0;
	g_ping.state = PING_STATE_DONE;
}

void ping_write_json_status(struct json_writer *w)
{
	struct in_addr a;
	char ipstr[INET_ADDRSTRLEN];

	jw_obj_open(w);
	jw_key(w, "state");
	switch (g_ping.state) {
	case PING_STATE_NONE:
		jw_str(w, "none");
		break;
	case PING_STATE_PENDING:
		jw_str(w, "pending");
		break;
	case PING_STATE_DONE:
		jw_str(w, "done");
		break;
	}

	if (g_ping.state == PING_STATE_NONE) {
		jw_key(w, "host");
		jw_null(w);
	} else {
		a.s_addr = g_ping.target_ip_be;
		inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
		jw_key(w, "host");
		jw_str(w, ipstr);
	}

	jw_key(w, "reachable");
	if (g_ping.state == PING_STATE_DONE)
		jw_bool(w, g_ping.reachable);
	else
		jw_null(w);

	jw_key(w, "rtt_ms");
	if (g_ping.state == PING_STATE_DONE && g_ping.reachable)
		jw_num(w, g_ping.rtt_ms);
	else
		jw_null(w);

	jw_key(w, "timed_out");
	if (g_ping.state == PING_STATE_DONE)
		jw_bool(w, g_ping.timed_out);
	else
		jw_null(w);

	jw_obj_close(w);
}
