#ifndef SERVERHEALTH_H
#define SERVERHEALTH_H

#include "json.h"
#include "registry.h"

#include <time.h>

/*
 * Health / heartbeat tracking for REGISTERED SERVERS (issue #81).
 *
 * Cix lets an operator register redundant backend servers for four
 * subsystems -- LDAP, DNS, NTP and syslog -- but until this module it
 * only ever tracked *that* a server was registered, never whether it is
 * actually serving. That gap has already cost real debugging time: #80
 * was a registered LDAP pair that answered on the wire but could not
 * resolve anything under the configured base DN, and every login failed
 * with nothing anywhere reporting a server as bad.
 *
 * ONE primitive serves all four kinds rather than four parallel
 * implementations (a Maxim, not a preference): "a registered server,
 * probed on an interval, healthy or not, optionally drained by an
 * operator" is the same concept everywhere. Each kind supplies only its
 * own enumerator and probe target; everything else -- the record, the
 * state machine, the REST/CLI/web surface -- is shared.
 *
 * What this module does NOT do, deliberately: it does not perform the
 * probe itself. Probing is I/O, and this daemon is a single epoll loop
 * where a blocking connect() would stall the entire control plane (the
 * exact failure ADR-0180 exists to prevent). main.c owns the
 * non-blocking probe and simply reports each result back here via
 * serverhealth_record_result(). This module is pure state.
 */

#define SERVERHEALTH_MAX 64
#define SERVERHEALTH_KIND_MAX 12
#define SERVERHEALTH_ERROR_MAX 128
#define SERVERHEALTH_PROBE_MAX 24

/*
 * How many consecutive failed probes before a server that was healthy is
 * declared unhealthy. >1 deliberately: a single dropped probe (a busy
 * box, a container mid-restart) is not a reason to pull a working server
 * out of service, and flapping a directory server in and out is worse
 * than a slow reaction. A success always restores health immediately --
 * recovery is never delayed.
 */
#define SERVERHEALTH_FAILURE_THRESHOLD 3

enum serverhealth_state {
	SERVERHEALTH_UNKNOWN = 0, /* registered, never yet probed */
	SERVERHEALTH_HEALTHY,
	SERVERHEALTH_UNHEALTHY
};

struct serverhealth_record {
	char kind[SERVERHEALTH_KIND_MAX];      /* "ldap" | "dns" | "ntp" | "syslog" */
	char container[REGISTRY_NAME_MAX];
	enum serverhealth_state state;
	/*
	 * Operator override -- this server is deliberately out of service
	 * (maintenance), independent of what the probe says. Persisted: a
	 * drain is an intent that must survive a daemon restart, exactly
	 * like a container's own `stopped` flag. Health state itself is NOT
	 * persisted -- it is a live observation, meaningless across a
	 * restart, and is re-established by the first probe after boot.
	 */
	int drained;
	/*
	 * What the probe actually did, verbatim, e.g. "tcp:3893" or
	 * "process". Reported over the API so an operator is never left
	 * guessing how much a "healthy" verdict really proves -- a TCP
	 * accept is a genuine service check; a process-liveness check is
	 * only "the container is running" and must not be read as more.
	 */
	char probe[SERVERHEALTH_PROBE_MAX];
	time_t last_check_at;
	time_t last_ok_at;
	int consecutive_failures;
	char last_error[SERVERHEALTH_ERROR_MAX];
	int in_use;
};

/* Loads persisted drain flags. Health state always starts UNKNOWN. */
int serverhealth_init(const char *state_path);
void serverhealth_repoint(const char *new_state_path);

/*
 * Records one probe outcome, creating the record if this (kind,
 * container) pair is newly registered. ok != 0 marks it healthy
 * immediately and clears the failure count; ok == 0 increments it and
 * flips to UNHEALTHY only once SERVERHEALTH_FAILURE_THRESHOLD is
 * reached (see that constant). err may be NULL.
 */
void serverhealth_record_result(const char *kind, const char *container, const char *probe, int ok,
                                 const char *err);

struct serverhealth_record *serverhealth_find(const char *kind, const char *container);

/*
 * The question every consumer of a server list actually wants: should
 * this server be handed to clients right now? True unless it is drained
 * or confirmed UNHEALTHY -- a never-yet-probed (UNKNOWN) server counts
 * as in service, so enabling health tracking can never black-hole a
 * working deployment during the first probe interval.
 */
int serverhealth_in_service(const char *kind, const char *container);

/* Operator drain/undrain. Persisted. Returns 0 on success. */
int serverhealth_set_drained(const char *kind, const char *container, int drained);

/* Drops every record for a container -- called when it is deleted, the
 * same shape dns_server_forget()/ldap_server_forget() already have. */
void serverhealth_forget(const char *container);

void serverhealth_write_json_list(struct json_writer *w);

#endif /* SERVERHEALTH_H */
