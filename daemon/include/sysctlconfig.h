#ifndef SYSCTLCONFIG_H
#define SYSCTLCONFIG_H

#include "json.h"

/*
 * ADR-0160: the persisted half of the host-level sysctl REST surface --
 * which keys this daemon has been asked to set (and their values),
 * reapplied at every boot by apply_configured_sysctls() (daemon/src/
 * main.c). Same atomic-JSON-persisted table shape devicemap.c already
 * established. The live half (actually reading/writing /proc/sys) has
 * no state of its own and lives directly in main.c's request handlers.
 *
 * A key's presence in this table *is* "persist this at boot" -- there
 * is no separate boolean. PUT /v1/system/sysctl/{key} upserts here by
 * default (persist-by-default, opt out per-call with "persist":false,
 * which never calls sysctlconfig_set() at all); DELETE removes it here
 * without touching the live value (see this ADR's own reasoning: there
 * is no general, reliable "value before this project touched it" to
 * revert to).
 */

#define SYSCTL_KEY_MAX 128
/* Comfortably covers any real tuple-shaped sysctl this project has
 * seen (e.g. net.ipv4.tcp_wmem's 3 integers) joined with single
 * spaces, plus headroom -- not a hard protocol limit, just a generous
 * bound matching every other fixed-size value field in this codebase. */
#define SYSCTL_VALUE_MAX 256
#define SYSCTL_CONFIG_MAX 128

enum sysctlconfig_error {
	SYSCTLCONFIG_OK = 0,
	SYSCTLCONFIG_ERR_INVALID_KEY,
	SYSCTLCONFIG_ERR_FULL,
	SYSCTLCONFIG_ERR_NOT_FOUND,
	SYSCTLCONFIG_ERR_PERSIST_FAILED,
};

/*
 * Non-empty, fits SYSCTL_KEY_MAX, contains no '/' -- a dotted sysctl
 * key never legitimately has one; main.c's own live GET/PUT handlers
 * use this too, sharing the one check rather than three near-copies.
 * Deliberately does NOT restrict which keys are otherwise valid (no
 * net.*-only limit, no allowlist) -- "fully open passthrough" is a
 * deliberate design choice for this host-level surface, confirmed
 * with the user, unlike the existing net.*-only per-container path.
 */
int sysctl_key_is_valid(const char *key);

/* Loads state_path (the persisted key/value list, if any) at startup. */
int sysctlconfig_init(const char *state_path);

/* ADR-0141 Phase 2 style repoint without reloading -- see
 * devicemap_repoint()'s own doc comment for the shared reasoning. */
void sysctlconfig_repoint(const char *new_state_path);

/*
 * Upserts key -> value (value already in its canonical form -- tokens
 * joined by a single space, see main.c's own JSON-array-to-string
 * normalization for a PUT body). key must be non-empty, fit
 * SYSCTL_KEY_MAX, and contain no '/' (a dotted sysctl key never
 * legitimately has one; the live handler's own dots-to-slashes
 * translation would otherwise produce a nonsensical path).
 */
enum sysctlconfig_error sysctlconfig_set(const char *key, const char *value);

enum sysctlconfig_error sysctlconfig_delete(const char *key);

/*
 * Writes value as a plain JSON string if it's a single whitespace-
 * delimited token, or a JSON array of strings if it has more than one
 * (e.g. a real tuple-shaped sysctl like net.ipv4.tcp_wmem) -- the one
 * shared formatting rule both this module's own persisted-list output
 * and main.c's live GET /v1/system/sysctl/{key} handler (reading a
 * freshly-read /proc/sys value, not a persisted one) both use, so a
 * given multi-value key reports identically either way.
 */
void sysctl_value_write_json(const char *value, struct json_writer *w);

/*
 * Writes {"key":..., "value": "..."|[...]} for key into w (via
 * sysctl_value_write_json() above). Returns 1 if key has a persisted
 * entry, 0 (writes nothing) otherwise.
 */
int sysctlconfig_write_json_one(const char *key, struct json_writer *w);

/* Every persisted key, same per-entry shape, wrapped in a JSON array. */
void sysctlconfig_write_json_list(struct json_writer *w);

/*
 * Iterates every persisted entry, calling fn(key, value, ctx) for
 * each -- used by apply_configured_sysctls() (main.c) so this module
 * doesn't need to know about /proc/sys or container_net_apply_sysctl()
 * at all, matching the same "reuse the plain write, don't couple this
 * config table to the live-apply mechanism" separation the rest of
 * this codebase already keeps between a *_config module and its own
 * live-apply call site.
 */
void sysctlconfig_foreach(void (*fn)(const char *key, const char *value, void *ctx), void *ctx);

#endif /* SYSCTLCONFIG_H */
