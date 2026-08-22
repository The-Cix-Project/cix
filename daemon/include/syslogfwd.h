#ifndef SYSLOGFWD_H
#define SYSLOGFWD_H

#include "json.h"

/*
 * Optional, redundant external syslog forwarding (logging epic Part 2,
 * ADR-0127): the consolidated log store (logstore.c, ADR-0070/ADR-0126)
 * stays the one source of truth the REST API and web UI ever read from
 * -- this module is a pure, best-effort, fire-and-forget SIDE CHANNEL
 * on top of it, never the other way around. Registering a running
 * container (typically syslog-1/syslog-2, a redundant pair running a
 * real syslogd, e.g. sysklogd.recipe) as a target means every
 * subsequent container-sourced log line (source="container" only --
 * kernel/thincd/audit entries are NOT forwarded, matching the user's
 * own "all the logs for the containers" scoping) is also sent to it as
 * a real RFC 3164 BSD-syslog UDP datagram, for operators who want
 * standard external tooling on top of this platform's own logging. A
 * forward failure (target unreachable, container stopped, EAGAIN under
 * momentary load) is silently dropped -- UDP syslog has never had
 * delivery guarantees, and logstore.c already holds the durable,
 * replayable copy of everything this module ever sends.
 *
 * Mirrors ntp_server_register()/_unregister()/_forget() (ntp.h) almost
 * exactly -- pure bookkeeping, no pid/pidfd, a registered container's
 * live IP is resolved fresh from the registry at every send, never
 * cached. The one real difference: NTP's registration governs an
 * inbound query thincd itself initiates and waits on a reply for;
 * this one governs an outbound, unacknowledged fire-and-forget send
 * triggered by unrelated container-output activity, so there is no
 * async job/epoll machinery here at all -- just a single persistent,
 * non-blocking UDP socket, opened once at init and reused for every
 * send.
 */

#define SYSLOG_TARGET_MAX 8       /* registered syslog-forward-target containers */
#define SYSLOG_TARGET_NAME_MAX 64 /* matches REGISTRY_NAME_MAX by value, no header dependency */
#define SYSLOG_UDP_PORT 514       /* standard syslog port, RFC 3164 */

enum syslogfwd_error {
	SYSLOGFWD_OK = 0,
	SYSLOGFWD_ERR_PERSIST_FAILED,
	SYSLOGFWD_ERR_DUPLICATE,             /* target registration */
	SYSLOGFWD_ERR_FULL,                  /* target registration */
	SYSLOGFWD_ERR_NOT_FOUND,             /* target unregistration: no such registration */
	SYSLOGFWD_ERR_CONTAINER_NOT_FOUND,   /* target registration: named container doesn't exist at all */
	SYSLOGFWD_ERR_CONTAINER_NOT_RUNNING  /* target registration: exists but not running */
};

/*
 * Loads the persisted target registrations (if any) and opens the one
 * outbound UDP socket this module reuses for every send. Returns -1
 * only on socket creation failure (never expected on a real Linux
 * host -- treated the same as any other init()-time fatal condition
 * this daemon already has).
 */
int syslogfwd_init(const char *state_path);

/* ADR-0141 Phase 2: repoints without reloading -- see network_repoint()'s
 * own doc comment for the shared reasoning. */
void syslogfwd_repoint(const char *new_state_path);

/*
 * Registers container_name as a syslog forward target. container_name
 * must already exist and be running (SYSLOGFWD_ERR_CONTAINER_NOT_RUNNING
 * otherwise, the same rule POST /v1/ntp/servers already enforces) --
 * a target's IP is resolved fresh from the registry at every send,
 * never cached here, so nothing about the target beyond its name is
 * stored.
 */
enum syslogfwd_error syslogfwd_target_register(const char *container_name);
enum syslogfwd_error syslogfwd_target_unregister(const char *container_name);

/* Called from the same container-delete cleanup path as
 * ntp_server_forget()/dns_server_forget() -- safe no-op if
 * container_name was never registered. */
void syslogfwd_target_forget(const char *container_name);

void syslogfwd_target_write_json_list(struct json_writer *w);

/*
 * Best-effort fan-out to every registered, currently-running target,
 * as one RFC 3164 UDP datagram per target. level must be one of the
 * real syslog severity names logstore.c already accepts --
 * logstore_level_severity() maps it to the RFC 5424 numeric severity
 * used in the PRI field. container becomes the datagram's HOSTNAME
 * field (the container is the real originator of the message, not
 * this daemon) -- TAG is always "thincd" (identifying this daemon as
 * the relay). A no-op if zero targets are currently registered, so
 * this is safe to call unconditionally from every container-output
 * line without a separate "is forwarding enabled" check at the call
 * site.
 */
void syslogfwd_send(const char *container, const char *level, const char *msg);

/* Issue #81: uniform enumerator for the shared server-health prober. */
int syslogfwd_target_list_containers(char out[][SYSLOG_TARGET_NAME_MAX], int max);

#endif /* SYSLOGFWD_H */
