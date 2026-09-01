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
 * ADR-0141 Phase 2: repoints without reloading -- see network_repoint()'s
 * own doc comment for the shared reasoning, and this module's own .c
 * file comment for why the real /etc/resolv.conf bind mount is
 * deliberately NOT this function's concern.
 */
void resolv_repoint(const char *new_state_path);

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

/*
 * Issue #135: break the fresh-install cycle -- a new box has no
 * recipes, recipes are fetched by hostname, hostnames need DNS, and
 * this platform's DNS is containers built FROM recipes.
 *
 * The way out was already sitting in the configuration and never
 * connected to anything: the DNS forwarders (issue #134) are real
 * upstream resolvers, and on a site network they resolve the site's own
 * names -- verified against this deployment's own forwarders, which
 * answer for the git host the recipes come from. A host that resolves
 * through them can fetch recipes, build its DNS containers, and only
 * then point at itself.
 *
 * Applies the forwarders as the host's own resolvers, but ONLY when the
 * host has none configured. An operator who has set them deliberately
 * is never overridden: a resolver list is exactly the kind of setting
 * someone changes for a reason the daemon cannot see, and a config that
 * silently reverts is worse than one that never helped.
 *
 * Returns 1 if it wrote something, 0 if it left an existing
 * configuration alone or had no forwarders to apply, -1 on a write
 * failure.
 */
int resolv_seed_from_forwarders(const char *const *forwarders, int count);

/* How many nameservers are currently configured. */
int resolv_configured_count(void);

void resolv_write_json(struct json_writer *w);

#endif /* RESOLV_H */
