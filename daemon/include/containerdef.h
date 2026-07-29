#ifndef CONTAINERDEF_H
#define CONTAINERDEF_H

#include "registry.h"

#include <stddef.h>

/*
 * Persisted, auto-restarting container definitions ("restart: always"
 * on POST /v1/containers) -- the one form of container state that
 * survives a daemon restart, unlike the registry itself (in-memory
 * only by deliberate design -- every container dies automatically via
 * PR_SET_PDEATHSIG if this daemon exits). A definition stores the
 * exact, already-validated original POST /v1/containers request body
 * and replays it verbatim through the same creation path
 * (daemon/src/main.c's create_container_from_body()) at boot and
 * after an unprompted exit -- not a second, independently-maintained
 * schema for "what a container looks like." See ADR-0025.
 */

#define CONTAINERDEF_MAX 256 /* matches REGISTRY_MAX_CONTAINERS -- never more defs than containers could exist */
#define CONTAINERDEF_MAX_DEPENDS 16

struct container_def {
	char name[REGISTRY_NAME_MAX];
	char *body; /* owned, malloc'd -- the original request body, verbatim */
	size_t body_len;
	char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
	int depends_on_count;
	int in_use;
};

/*
 * Loads state_path (the persisted definition list, if it exists).
 * state_path is remembered for subsequent containerdef_add()/
 * containerdef_remove() calls to rewrite. Call once at daemon
 * startup, before any container is created (autostart or otherwise).
 * Returns 0, or -1 if the file exists but is malformed -- this file
 * is only ever written by containerdef_add()'s own validated path,
 * so corruption is real and should stop startup, the same "a
 * malformed entry means real corruption" posture network.c/pkg.c's
 * own persisted state already has, not silently proceed with
 * whatever loaded.
 */
int containerdef_init(const char *state_path);

/*
 * Persists name's definition (body verbatim, depends_on already
 * parsed/validated by the caller from that same body -- kept
 * alongside it as a cached index for containerdef_resolve_order(),
 * not a second source of truth: only this function ever writes a
 * definition, and it always receives both from the same request
 * parse). Overwrites any existing definition for the same name.
 * Returns 0, or -1 on a persist (disk) failure.
 */
int containerdef_add(const char *name, const char *body, size_t body_len,
                      const char depends_on[][REGISTRY_NAME_MAX], int depends_on_count);

/* Removes name's definition, if any. A no-op (returns 0) if none exists. */
int containerdef_remove(const char *name);

struct container_def *containerdef_find(const char *name);

/*
 * Resolves every currently-defined container into dependency order (a
 * depends_on-respecting topological order) -- DFS with a visiting[]
 * cycle-detection stack, the same shape pkg.c's own resolve_chain()
 * already established for pkg_depends, applied here to depends_on
 * instead. Writes up to CONTAINERDEF_MAX names into out_order,
 * returns the count. A definition whose own depends_on names an
 * unknown definition, or that participates in a cycle, is skipped
 * (logged to stderr here -- this is boot/reconciliation-time work
 * with no single HTTP response to attach an error to, the same
 * posture main.c's own dns_register/pki_issue best-effort paths
 * already have) and simply omitted from out_order, never aborting
 * resolution of the rest.
 */
int containerdef_resolve_order(char out_order[][REGISTRY_NAME_MAX]);

#endif /* CONTAINERDEF_H */
