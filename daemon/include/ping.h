#ifndef PING_H
#define PING_H

#include "json.h"

#include <stdint.h>

/*
 * GET/POST /v1/system/ping -- a real, hand-rolled ICMP echo
 * request/reply, no shelling out to a "ping" binary (this project's
 * images don't have one, and every network primitive this daemon
 * needs already talks to the kernel directly -- rtnetlink for
 * routes/addresses, this for reachability). Raw ICMP sockets need
 * CAP_NET_RAW, which cixd already has as root on a real
 * --init-mode host; no new privilege requirement.
 *
 * v1 single-job constraint, matching every other async job in this
 * daemon (disk format, ISO build, pkg fetch, ...): only one ping may
 * be in flight at a time.
 */

enum ping_state {
	PING_STATE_NONE,    /* no ping has ever been started */
	PING_STATE_PENDING, /* sent, waiting for a reply or the timeout */
	PING_STATE_DONE     /* resolved -- see reachable/timed_out below */
};

enum ping_error {
	PING_OK = 0,
	PING_ERR_BUSY,          /* a ping is already in flight */
	PING_ERR_SOCKET_FAILED  /* socket()/sendto() failed -- typically
	                          * EPERM (no CAP_NET_RAW, e.g. a dev/test
	                          * cixd not running as root) or a real
	                          * routing failure on send */
};

/*
 * Starts a new ICMP echo request to target_ip_be (network byte
 * order). On success *out_sockfd is a non-blocking raw ICMP socket
 * the caller (main.c) registers with epoll for EPOLLIN -- the request
 * has already been sent by the time this returns.
 */
enum ping_error ping_start(uint32_t target_ip_be, int *out_sockfd);

/*
 * Called by main.c when the raw socket becomes readable. Reads one
 * packet and checks whether it's really this job's own echo reply --
 * a raw ICMP socket sees every ICMP packet system-wide, not just this
 * one's own, so a non-matching packet (unrelated ICMP traffic) is
 * silently ignored and the job stays PENDING for the next event.
 * Returns 1 once a real, matching reply has been recorded (job done,
 * caller should tear down both the socket and the timeout timer), 0
 * if still waiting.
 */
int ping_handle_reply(int sockfd);

/*
 * Called by main.c when the timeout timer fires before any matching
 * reply arrived. Records a timed-out result (job done).
 */
void ping_handle_timeout(void);

void ping_write_json_status(struct json_writer *w);

#endif /* PING_H */
