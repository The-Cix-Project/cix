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
/*
 * The one place this value is defined (One Source of Truth) -- used
 * both as create_container_from_body()'s own default when a live
 * POST omits restart_delay_seconds, and by parse_persisted_entry()
 * when loading a container_defs.json written before this field
 * existed (Phase 13 parts 1-2).
 */
#define CONTAINERDEF_DEFAULT_RESTART_DELAY_SECONDS 2

/*
 * Part 5 (pkg/ redesign, ADR-0124): default/max width of the random
 * delay window added on top of a rolling-restart, spreading out
 * simultaneous restarts of every follow_rolling container sharing an
 * image that just rebuilt rather than killing them all in the same
 * instant. Operator-configurable via containerdef_jitter_window_get()/
 * _set() -- these are only the persisted-config bootstrap default and
 * upper bound, not a hardcoded behavior.
 */
#define CONTAINERDEF_JITTER_DEFAULT_SECONDS 60
#define CONTAINERDEF_JITTER_MAX_SECONDS 3600

struct container_def {
	char name[REGISTRY_NAME_MAX];
	char *body; /* owned, malloc'd -- the original request body, verbatim */
	size_t body_len;
	char depends_on[CONTAINERDEF_MAX_DEPENDS][REGISTRY_NAME_MAX];
	int depends_on_count;
	/*
	 * Cached from the same request's own "readiness" field (see
	 * ADR-0026), the same "parsed once, not re-parsed from body on
	 * every read" shape depends_on already has above. has_readiness
	 * == 0 means no readiness check was configured; the other two
	 * fields are meaningless in that case.
	 */
	int has_readiness;
	int readiness_tcp_port;
	int readiness_timeout_seconds;
	/*
	 * "no" | "always" | "on-failure" | "unless-stopped". ADR-0181 (#73,
	 * superseding ADR-0027): every container is now persisted, so a def CAN
	 * be "no" -- it just means "never auto-restart" (not on exit, boot, or
	 * rolling), kept and startable by hand. Previously "no" meant "no def at
	 * all" (ephemeral), which made `stop` silently destroy the default
	 * container.
	 */
	char restart_policy[16];
	/* Base crash-restart delay, seconds, 1-300 -- see CONTAINER_RESTART_
	 * BACKOFF_CAP_SECONDS/CONTAINER_RESTART_STABILITY_SECONDS in main.c
	 * for how this seeds the actual (backed-off) delay applied. */
	int restart_delay_seconds;
	/*
	 * Persisted. Set by POST .../stop, cleared by every containerdef_add()
	 * call (a fresh create/redefine is definitionally not stopped).
	 * Consulted by containerdef_autostart_all() ONLY when restart_policy
	 * == "unless-stopped"; consulted by handle_restart_timer_event()
	 * unconditionally (any policy) -- see ADR-0027 for why these differ.
	 */
	int stopped;
	/*
	 * NOT persisted -- deliberately absent from save_state()/
	 * parse_persisted_entry(), so it naturally resets to 0 via
	 * containerdef_init()'s own memset(g_defs, 0, ...) on every daemon
	 * boot. A crash-loop history from before a daemon restart isn't
	 * meaningful afterward.
	 */
	int consecutive_failures;
	/*
	 * Part 5 (ADR-0124): opt-in, set only by an explicit "follow_rolling":
	 * true on the original POST body (mirrors restart_policy/depends_on's
	 * own "parsed once by the caller, cached here" shape). Since ADR-0181
	 * every container is persisted, so this is stored even for a
	 * restart_policy:"no" def -- but it is inert there, because
	 * apply_rolling_container_restarts() skips restart:"no" defs entirely
	 * ("never auto-restart" includes "never auto-recreated by a rolling
	 * update"). Stored-but-inert, not rejected -- the same "ignored, not
	 * an error" precedent restart_delay_seconds established.
	 * When set (and the policy auto-restarts), apply_rolling_container_
	 * restarts() (daemon/src/main.c) keeps this container's own pinned
	 * image_version chasing its image's current_version whenever that
	 * changes.
	 */
	int follow_rolling;
	/*
	 * Part 5 follow-up: optional per-container override of the
	 * daemon-wide jitter window (containerdef_jitter_window_get()),
	 * set only by an explicit "follow_rolling_jitter_seconds" on the
	 * original POST body -- the same has_readiness shape this struct
	 * already uses for an optional int (has_follow_rolling_jitter == 0
	 * means "use the daemon default," the other field is then
	 * meaningless). Like follow_rolling itself, inert without follow_rolling
	 * also being set and on a restart:"no" def (ADR-0181: persisted, but
	 * rolling-follow is skipped for it); not independently rejected for
	 * either combination here, the same "ignored, not an error" precedent
	 * restart_delay_seconds/follow_rolling already established.
	 */
	int has_follow_rolling_jitter;
	int follow_rolling_jitter_seconds;
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

/* ADR-0141 Phase 2: repoints without reloading -- see network_repoint()'s
 * own doc comment for the shared reasoning. */
void containerdef_repoint(const char *new_state_path);

/*
 * Persists name's definition (body verbatim, depends_on/readiness/
 * restart_policy/restart_delay_seconds already parsed/validated by the
 * caller from that same body -- kept alongside it as a cached index
 * for containerdef_resolve_order()/containerdef_autostart_all(), not a
 * second source of truth: only this function ever writes a
 * definition, and it always receives these from the same request
 * parse). has_readiness == 0 means no readiness check (the other two
 * readiness parameters are then ignored). restart_policy may be any of
 * "no", "always", "on-failure", or "unless-stopped" -- ADR-0181 persists
 * every container, "no" included (it then means "kept but never
 * auto-restarted"); a "no" definition simply isn't autostarted at boot,
 * crash-restarted, or rolling-followed (see handle_create()/
 * containerdef_autostart_all()). Overwrites any existing definition for the same name, explicitly
 * clearing its stopped/consecutive_failures state (a fresh create/
 * redefine is definitionally not stopped and not backed off).
 * has_follow_rolling_jitter == 0 means "use the daemon-wide jitter
 * window" (containerdef_jitter_window_get()); the other field is then
 * ignored. Returns 0, or -1 on a persist (disk) failure.
 */
int containerdef_add(const char *name, const char *body, size_t body_len,
                      const char depends_on[][REGISTRY_NAME_MAX], int depends_on_count,
                      int has_readiness, int readiness_tcp_port, int readiness_timeout_seconds,
                      const char *restart_policy, int restart_delay_seconds, int follow_rolling,
                      int has_follow_rolling_jitter, int follow_rolling_jitter_seconds);

/* Removes name's definition, if any. A no-op (returns 0) if none exists. */
int containerdef_remove(const char *name);

/*
 * Sets (or clears) name's persisted stopped flag -- POST .../stop's
 * own primitive (daemon/src/main.c's handle_stop()). A no-op (returns
 * 0) if name has no definition, mirroring containerdef_remove()'s own
 * shape. Does not touch anything else about the definition.
 */
int containerdef_set_stopped(const char *name, int stopped);

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

/*
 * Writes one synthesized, Container-shaped JSON object (ADR-0045) for
 * every persisted definition that has NO live registry entry -- i.e.
 * every kept-but-not-running container -- letting GET /v1/containers
 * show it (status "stopped") instead of it simply vanishing from the
 * list. ADR-0181 (persist-all): this is no longer only the explicitly-
 * stopped (stopped == 1) defs. A restart:"no" container that exited on
 * its own, an on-failure container that exited cleanly, or ANY def
 * loaded at boot and not autostarted is inactive-but-kept too, with
 * stopped == 0 -- so the "stopped flag as liveness proxy" this used to
 * rely on no longer holds. Liveness is now decided by the caller-
 * supplied is_live() predicate (registry_find() != NULL), keeping this
 * module free of a back-dependency on the registry. Not wrapped in its
 * own array -- the caller (registry_write_json_list(), which already
 * depends on this module for restart/depends_on/readiness on live
 * entries too) writes these as extra items inside its own single
 * array, one list, no second endpoint. image is recovered by parsing
 * the persisted body's own "image" field (read-only JSON parse, same
 * as every other consumer of a stored body already does -- never
 * sourced/executed). Every field a live entry would have but an
 * inactive one genuinely doesn't (pid, networks, devices, ...) is
 * written as null/empty rather than guessed.
 */
void containerdef_write_json_inactive_list(struct json_writer *w,
                                           int (*is_live)(const char *name));

/*
 * Single-name counterpart to containerdef_write_json_inactive_list(),
 * for GET /v1/containers/{name} (handle_get_one() in main.c) to fall
 * back to when registry_find() misses -- same synthesized shape. The
 * caller has already confirmed there is no live entry for name, so this
 * writes the definition regardless of its stopped flag: returns 1 and
 * writes into w if a definition named name exists, 0 (writes nothing)
 * otherwise -- the caller is then free to fall through to its own 404.
 */
int containerdef_write_json_inactive_one(const char *name, struct json_writer *w);

/*
 * Part 5 (ADR-0124): rewrites just the "image_version" value already
 * spliced into name's own persisted body (see handle_create()'s own
 * comment on why every restart_policy != "no" body carries one) to
 * new_version, in place -- raw string find/replace on the existing
 * value, not a full JSON re-serialize (mirrors handle_create()'s own
 * "the body is already known-valid JSON, only surgery is needed"
 * reasoning). Used by apply_rolling_container_restarts() so a future
 * replay (an explicit POST .../start, a crash respawn, or this same
 * mechanism's own jittered restart) picks up the new pin instead of
 * silently reverting to the version this container was originally
 * created against. Returns 0 on success, -1 if name has no definition
 * or its body unexpectedly has no "image_version" key to replace (real
 * corruption -- every restart-capable body is guaranteed to have one,
 * spliced in at creation time).
 */
int containerdef_patch_image_version(const char *name, const char *new_version);

/*
 * ADR-0142 Section 4 (container-storage migration): same raw-surgery
 * posture as containerdef_patch_image_version() above, applied to the
 * "disk" field instead -- always writes a value (a quoted disk name,
 * or the JSON literal null for the default OS-disk placement), never
 * removes the key, so a subsequent replay's own json_as_string() read
 * sees exactly what a fresh POST with that same "disk" value would
 * have produced (a JSON null parses the same as the key being absent
 * entirely, per create_container_from_body()'s own resolution). If
 * name's body has no "disk" key yet (created without one, i.e. always
 * defaulted to the OS disk until now), splices one in using the exact
 * same "find the trailing '}', insert one more key in front of it"
 * technique handle_create() already established for image_version.
 * Returns 0, or -1 if name has no definition or its body is
 * unexpectedly malformed (no trailing '}' -- real corruption, every
 * persisted body is guaranteed well-formed JSON by
 * create_container_from_body()'s own json_parse() at creation time).
 */
int containerdef_patch_disk(const char *name, const char *new_disk);

/*
 * Loads (or initializes, if config_path doesn't exist yet)
 * config_path as the persisted rolling-restart jitter window --
 * mirrors pkg_cache_init()'s own "small standalone JSON config file,
 * one key" shape exactly (daemon/src/pkg.c, ADR-0122). Call once at
 * daemon startup, after containerdef_init(). Returns 0, or -1 on a
 * malformed persisted file (real corruption, same posture as every
 * other persisted-state loader here).
 */
int containerdef_rolling_config_init(const char *config_path);

void containerdef_rolling_config_repoint(const char *new_config_path);

/* Current jitter window, seconds -- CONTAINERDEF_JITTER_DEFAULT_SECONDS
 * until containerdef_jitter_window_set() is ever called. */
int containerdef_jitter_window_get(void);

/* Validates 0 <= seconds <= CONTAINERDEF_JITTER_MAX_SECONDS (0 means
 * "no jitter, restart immediately" -- a legitimate choice for a small
 * fleet where a thundering herd isn't a real concern), persists, and
 * takes effect for every rolling restart scheduled from then on.
 * Returns 0, or -1 (out of range, or a persist failure). */
int containerdef_jitter_window_set(int seconds);

#endif /* CONTAINERDEF_H */
