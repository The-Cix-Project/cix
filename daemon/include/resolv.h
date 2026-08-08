#ifndef RESOLV_H
#define RESOLV_H

#include "json.h"

/*
 * GET/PUT /v1/system/resolv (ADR-0076): the host's own outbound DNS
 * resolver configuration -- persisted the same way every other piece
 * of host state is (<g_base_dir>/resolv.conf), in literal resolv.conf
 * format (not JSON-wrapped), because that file IS what a real
 * --init-mode boot bind-mounts onto /etc/resolv.conf -- writing it
 * via resolv_set() is what actually takes effect, immediately, no
 * reboot needed. Covers both "point at one of our own DNS-server
 * containers" (resolve its IP once, set it here) and "point at a real
 * external resolver" with one orthogonal mechanism -- this module has
 * no notion of containers at all.
 */

#define RESOLV_MAX_NAMESERVERS 3 /* matches glibc's own resolv.conf MAXNS */
#define RESOLV_IP_STRLEN 16      /* "255.255.255.255\0" */

enum resolv_error {
	RESOLV_OK = 0,
	RESOLV_ERR_INVALID_IP,
	RESOLV_ERR_TOO_MANY,
	RESOLV_ERR_PERSIST_FAILED
};

/*
 * Loads state_path (the real, literal resolv.conf-format file) if it
 * already exists -- populates the in-memory nameserver list GET
 * reports. Remembers state_path for resolv_set()'s own later writes.
 * A missing file is not an error (nothing configured yet).
 */
int resolv_init(const char *state_path);

/*
 * Replaces the full nameserver list and rewrites the persisted file
 * with literal "nameserver A.B.C.D" lines, one per entry -- in order,
 * matching resolv.conf's own documented try-in-order semantics.
 * count == 0 clears it entirely (host falls back to no resolution,
 * today's default). Each entry must be a valid, non-empty IPv4
 * dotted-quad string; count above RESOLV_MAX_NAMESERVERS is rejected
 * outright (glibc's own resolver never reads past the fourth
 * "nameserver" line anyway -- silently accepting more here would just
 * silently guarantee some of them are ignored).
 */
enum resolv_error resolv_set(const char *const *nameservers, int count);

void resolv_write_json(struct json_writer *w);

#endif /* RESOLV_H */
