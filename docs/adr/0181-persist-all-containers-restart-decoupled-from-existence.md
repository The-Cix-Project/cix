# 0181 — Every container is persisted; `restart` governs only auto-restart, never existence

## Status

Accepted

Supersedes the *one specific* decision in ADR-0027 that a `restart:"no"` container has **no persisted definition** (that "absence of a def IS `no`"). Everything else in ADR-0027 — the `on-failure`/`unless-stopped` semantics, the `stopped` flag and its two-site consultation split, per-container base delay, and automatic backoff — stands unchanged.

## Context

ADR-0025/0027 tied a container's *existence across a stop or reboot* to its `restart` policy: a definition was persisted only when `restart != "no"`, and `restart:"no"` was represented by the **absence** of any `container_defs.json` entry. `registry_write_json_one()` even read "this is a restarting container" straight off `containerdef_find(name) != NULL`.

That coupling produced a genuine, user-visible surprise, caught directly this session: `POST /v1/containers/{name}/stop` on a default (`restart:"no"`) container returned `"stopping"` and then the container **vanished entirely** — no definition to fall back to, gone from `GET /v1/containers`, unrecoverable without recreating it from scratch. From the operator's side, `stop` had silently behaved like `delete`. The user's framing was unambiguous: *"stop is stop, and delete is delete"* — a `stop` must never destroy a container, whatever its restart policy.

The root cause is the conflation of two independent axes:

1. **Does this container still exist after it stops / after a reboot?** (lifecycle / persistence)
2. **Should it come back up on its own after an exit, boot, or rolling update?** (restart policy)

ADR-0027 let axis 2 (`restart`) decide axis 1. It shouldn't.

## Decision

**Persist every container's definition at create time, regardless of restart policy.** `restart` now governs *only* auto-restart. The two axes are fully decoupled:

- **`stop` always keeps the container.** `POST .../stop` kills the running process and leaves the persisted definition behind; the container reappears in listings as `status:"stopped"`, startable by hand. Only `DELETE` removes a container. This holds for every policy, `"no"` included.
- **`restart:"no"` now means "persisted, but never auto-restarted"** — not "ephemeral". A `"no"` container is *not* brought back on its own exit, *not* autostarted at daemon boot, and *not* auto-recreated by a rolling update. It is a kept, startable, down container until an explicit `container start` (or a `DELETE`). The default policy stays `"no"`.
- **A container that exits on its own is retained, not deleted** — its live registry entry is kept as `status:"exited"` with the real `exit_status` preserved (Docker's `ps -a` semantics). This makes a self-exited container both *observable* (real exit code, not a bare "stopped") and *name-reserving* (a create of the same name still `409`s). It only leaves the live registry on the *restart* path, where the entry is removed so the delayed restart can recreate the same name. The persisted definition survives a daemon restart independently; there it reappears as `status:"stopped"` (`exit_status` is still never persisted — ADR-0027's stated v1 boundary, unchanged).

The nine existing infrastructure containers and every `always`/`on-failure`/`unless-stopped` workload are unaffected: they were already persisted, already auto-restart, and their behavior is byte-for-byte the same. The change is purely additive for them and corrective for `"no"`.

### Mechanics

- **Create** (`handle_create`): the persist step is now unconditional (was gated on `restart_policy != "no"`), splicing the resolved `image_version` pin onto the body exactly as before.
- **Boot** (`containerdef_autostart_all`): skips `restart:"no"` defs — they stay down until started by hand. `unless-stopped` + `stopped` skip is unchanged.
- **Exit** (`handle_container_event`): `registry_remove()` runs *only* on the restart path; on the no-restart path the "exited" entry is deliberately kept.
- **Rolling** (`apply_rolling_container_restarts`): skips `restart:"no"` defs — "never auto-restart" includes "never auto-recreated by a rolling update", so `follow_rolling` is stored-but-inert on a `"no"` def.
- **Listing** (`registry_write_json_list` → `containerdef_write_json_inactive_list`): the "show a persisted-but-not-running container" fallback no longer keys on the `stopped` flag (which a self-exited or freshly-booted `"no"` container doesn't have). Liveness is decided by an injected `registry_name_is_live()` predicate — a def is shown as `"stopped"` exactly when it has no live registry entry — keeping `containerdef.c` free of a back-dependency on the registry.

## Consequences

- **`stop` can never again silently destroy a container.** The exact reported surprise is closed at its root, for every restart policy uniformly.
- **`restart:"no"` is now a useful, first-class policy**, not a synonym for "ephemeral": define a container, run it, stop it, start it again — all without it ever auto-restarting or auto-vanishing.
- **Self-exited containers accumulate** as `status:"exited"` entries until `DELETE`d, matching the "persist until delete" model the user asked for (and Docker's own `ps -a`). An operator who wants them gone runs `DELETE`, the same as for any stopped container.
- **`stopped` flag on a `"no"` def is now set by `stop` but changes no behavior** — `"no"` is already never autostarted or crash-restarted, so the flag is belt-and-suspenders there, consulted meaningfully only for `unless-stopped` (autostart) and any policy (the mid-delay restart-timer race), exactly as ADR-0027 defined.
- Verified end-to-end on a live daemon: `test/test_daemon.c` (a default-`"no"` `c1` exits and is observable as `status:"exited"` with `exit_status=5`, and its name stays reserved — a duplicate create `409`s) and `test/test_container_restart.c` (an `on-failure` container that exits cleanly stays down as `"exited"` and never comes back; every crash/`always`/`unless-stopped`/backoff scenario still passes). `test/test_container_restart.c`'s own `container_exists()` liveness helper was updated to read a retained `"exited"` container as not-live, matching its documented "genuinely live right now" intent. Full daemon/web/files suites re-run clean.
- ADR-0027's `restart:"no"` → no-def representation is the only thing reversed; its Status is annotated accordingly rather than flipped to fully `Superseded`, since the rest of that ADR remains in force.
