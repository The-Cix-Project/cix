#ifndef NTP_H
#define NTP_H

#include "json.h"

#include <stdint.h>

/*
 * NTP: two related but genuinely independent mechanisms, confirmed
 * directly with the user before designing either (task #751-755):
 *
 * 1. The host's own clock. A small hand-rolled SNTP client (the
 *    client subset of RFC 5905, historically RFC 4330) -- the same
 *    "platform-level primitive gets hand-rolled" precedent
 *    daemon/src/ping.c already set (ADR-0007): setting the HOST's own
 *    clock via clock_settime() needs CAP_SYS_TIME from the host's own
 *    namespace, which no container can do without either breaking
 *    container isolation (granting it host-level capabilities) or
 *    cixd doing the actual clock_settime() call itself anyway --
 *    so this can never be delegated to a containerized workload the
 *    way DNS/dnsmasq or LDAP/glauth are. GET/PUT /v1/system/ntp: the
 *    upstream server address list, persisted the same way resolv.c's
 *    own nameserver list is (ntp_set_upstream()/ntp_write_json_
 *    config() below).
 *
 * 2. Container-to-container NTP: POST/GET/DELETE /v1/ntp/servers,
 *    registering a running container as an available time source --
 *    mirrors dns_server_register()/ldap_server_register() (ntp_
 *    server_* below). Simpler than either: NTP is itself a live
 *    query/response protocol, the same reasoning that already let
 *    LDAP registration skip pid/pidfd entirely (task #725) -- no
 *    config-file push, no signal needed. cixd resolves the
 *    registered container's own live IP (registry_find()'s
 *    nets[0].ip_be) fresh at every sync attempt and queries it
 *    directly with the exact same SNTP client as (1) -- a registered
 *    container is just one more candidate source ntp_sync_start()
 *    tries, ahead of the external addresses from (1).
 *
 * The sync mechanism itself (part 3) is a real, bounded async job,
 * matching this project's own established "one job of this kind in
 * flight at a time" v1 constraint (ping/disk-format/ISO-build/pkg-
 * fetch all already work this way) -- never a blocking syscall in
 * cixd's own single-threaded epoll loop (ADR-0005/0007).
 */

#define NTP_MAX_UPSTREAM 3     /* matches RESOLV_MAX_NAMESERVERS's own convention */
#define NTP_IP_STRLEN 16       /* "255.255.255.255\0" */
#define NTP_SERVER_MAX 8       /* registered NTP-serving containers */
#define NTP_SERVER_NAME_MAX 64 /* matches REGISTRY_NAME_MAX by value, no header dependency */
#define NTP_PORT 123
#define NTP_PACKET_LEN 48 /* the fixed SNTP/NTPv4 client-mode wire size, RFC 5905 Figure 8 */
/* Per-candidate reply timeout -- main.c arms/re-arms its own timerfd
 * to this value alongside every ntp_sync_start()/advance-to-next-
 * candidate step, so the constant needs to be visible here rather
 * than staying private to ntp.c. */
#define NTP_SYNC_TIMEOUT_MS 1500

enum ntp_error {
	NTP_OK = 0,
	NTP_ERR_INVALID_IP,
	NTP_ERR_TOO_MANY,
	NTP_ERR_PERSIST_FAILED,
	NTP_ERR_DUPLICATE,              /* server registration */
	NTP_ERR_FULL,                   /* server registration */
	NTP_ERR_NOT_FOUND,              /* server unregistration: no such registration */
	NTP_ERR_CONTAINER_NOT_FOUND,    /* server registration: named container doesn't exist at all */
	NTP_ERR_CONTAINER_NOT_RUNNING,  /* server registration: exists but not running */
	NTP_ERR_INVALID_TIME            /* manual time set */
};

/* ---- Part 1: upstream server address list (GET/PUT /v1/system/ntp) ---- */

/*
 * Loads the persisted upstream list (if any) and the persisted server
 * registrations (if any) -- both come back in one call, same
 * dual-load convention dns_init()'s own header comment already
 * documents for the analogous DNS records/servers pair.
 */
int ntp_init(const char *state_path, const char *servers_state_path);

/* ADR-0141 Phase 2: repoints without reloading -- see network_repoint()'s
 * own doc comment for the shared reasoning. */
void ntp_repoint(const char *new_state_path, const char *new_servers_state_path);

/*
 * Replaces the full upstream address list and rewrites the persisted
 * file. count == 0 clears it entirely (host then relies solely on any
 * registered server containers, if any). Each entry must be a valid,
 * non-empty IPv4 dotted-quad string; count above NTP_MAX_UPSTREAM is
 * rejected outright, the same rule resolv_set() already enforces for
 * its own nameserver list.
 */
enum ntp_error ntp_set_upstream(const char *const *addrs, int count);

void ntp_write_json_config(struct json_writer *w);

/* ---- Part 2: registered NTP-serving containers (mirrors dns_server_*) ---- */

/*
 * Registers container_name as an available time source. No pid/pidfd
 * needed (unlike dns_server_register()) -- see the header comment
 * above for why: this is pure bookkeeping, the container's own live
 * IP is resolved fresh from the registry at every sync attempt, never
 * cached here. container_name must already exist and be running
 * (NTP_ERR_CONTAINER_NOT_RUNNING otherwise, the same rule POST
 * /v1/dns/servers and /v1/ldap/servers already enforce).
 */
enum ntp_error ntp_server_register(const char *container_name);
enum ntp_error ntp_server_unregister(const char *container_name);

/* Called from the same container-delete cleanup path as
 * dns_server_forget()/ldap_server_forget() -- safe no-op if
 * container_name was never registered. */
void ntp_server_forget(const char *container_name);

void ntp_server_write_json_list(struct json_writer *w);

/* ---- Part 3: the SNTP sync mechanism ---- */

enum ntp_start_error {
	NTP_START_OK = 0,
	NTP_START_BUSY,          /* a sync attempt is already in flight */
	NTP_START_NO_CANDIDATES, /* nothing configured: no registered servers, no upstream addresses */
	NTP_START_SOCKET_FAILED
};

/*
 * Starts one sync attempt: builds the ordered candidate list
 * (registered, currently-running server containers first -- resolved
 * to their live IP right now, not cached -- then the configured
 * upstream addresses, in the order each was added), opens one
 * non-blocking UDP socket, and sends the first candidate's own SNTP
 * request. *out_sockfd is that socket -- main.c registers it with
 * epoll (EPOLLIN) plus a paired one-shot timeout timer (the same
 * CONN_PING/CONN_PING_TIMER shape ping.c already established), and
 * must call ntp_sync_handle_reply()/ntp_sync_handle_timeout() as
 * those fire.
 */
enum ntp_start_error ntp_sync_start(int *out_sockfd);

/*
 * Called by main.c when sockfd becomes readable. Reads one packet and
 * validates it's really a matching reply to the candidate currently
 * being queried (source address plus the origin timestamp we sent,
 * echoed back per RFC 5905's own client validation rule -- a stray
 * unrelated UDP packet arriving on the same socket, e.g. a delayed
 * reply from an earlier candidate this job already moved past, is
 * silently ignored rather than accepted).
 *
 * Returns 1 if the whole sync attempt is now resolved (a valid reply
 * was applied via clock_settime(), or every candidate has been tried
 * and none answered) -- caller tears down both the socket and the
 * timeout timer. Returns 0 if it moved on to the next candidate on
 * the SAME socket (still PENDING) -- caller must re-arm the timeout
 * timer for the new candidate but the socket itself is unchanged, the
 * one real difference from ping.c's simpler always-one-shot model.
 */
int ntp_sync_handle_reply(int sockfd);

/* Same 0/1 contract as ntp_sync_handle_reply(), called when the
 * per-candidate timeout fires before a valid reply arrived. */
int ntp_sync_handle_timeout(int sockfd);

enum ntp_sync_result {
	NTP_SYNC_NEVER,  /* no sync attempted yet since daemon start */
	NTP_SYNC_OK,     /* last attempt succeeded */
	NTP_SYNC_FAILED  /* last attempt exhausted every candidate with no valid reply */
};

void ntp_write_json_status(struct json_writer *w);

/* ---- Part 4: GET/PUT /v1/system/time (manual override/inspection) ---- */

/* Real clock_settime(CLOCK_REALTIME, ...) -- immediate, host-wide
 * effect, same "no reboot needed" relationship to the running system
 * every other live-apply resource in this daemon already has. */
enum ntp_error ntp_time_set(int64_t unix_seconds);

void ntp_write_json_time(struct json_writer *w);

/* Issue #81: uniform enumerator for the shared server-health prober. */
int ntp_server_list_containers(char out[][NTP_SERVER_NAME_MAX], int max);

#endif /* NTP_H */
