# 0045 — Container start/pause: cgroup-freeze lifecycle completeness

## Status

Accepted

## Context

Raised directly by the user against a real, reproducible symptom: `POST /v1/containers/{name}/stop` kills the live process but deliberately keeps the persisted definition (ADR-0027) — yet `GET /v1/containers` (`registry_write_json_list()`) only ever iterated the live in-memory registry. The moment a container was stopped, it vanished from every list/get response entirely, with no REST path to bring it back short of a full daemon restart (and even then, only for `restart: "always"`/`"on-failure"` — `"unless-stopped"` stays down forever, and `"no"` was never tracked at all). Re-`POST`ing the same name to recreate it 409s ("already exists"), because the persisted definition itself was never removed.

Separately, the user asked for real cgroup-freezer pause/resume, explicitly choosing it over settling for the start fix alone.

Investigating the mechanism surfaced that `container_handle.cgroup_fd` (`include/container.h`) is already an `O_PATH` fd on each container's own cgroup v2 leaf, opened at `cgroup_create()` time and kept alive for the container's lifetime — `openat(cgroup_fd, "cgroup.freeze", O_WRONLY)` needs no new kernel-facing mechanism at all, just one more file opened underneath an fd that already exists.

Two more, unplanned findings surfaced while wiring the fix all the way through to the web dashboard, both real and both fixed in this same phase rather than deferred:

1. **The freeze/kill interaction**: a cgroup v2 freezer blocks signal delivery to every task inside it. `registry_remove()` (the single shared kill path for both `POST .../stop` and `DELETE`) issuing `SIGKILL` to an already-frozen container would queue the signal but never actually terminate the process — a hung, unkillable entry. Confirmed directly: without the fix, `DELETE` on a paused container never returns; a real container ends up permanently stuck.

2. **A latent, pre-existing bug in `containerdef_autostart_all()`**: it already correctly ignores a stale `stopped=1` flag for `"always"`/`"on-failure"` policies when deciding *whether* to autostart (ADR-0027's own documented behavior — a daemon restart is a fresh chance regardless of a prior manual stop) — but it never *cleared* that flag on a successful autostart. `handle_restart_timer_event()` (the crash-restart path) checks `!def->stopped` **unconditionally, regardless of policy** — so a container that was ever manually stopped once, then survived one daemon restart via autostart, would come back live but permanently ineligible for its own crash-restart policy from that point forward, with no symptom visible anywhere except a missing restart after some future crash. Found by tracing exactly why a freshly-autostarted `srv1` still reported `"stopped": true` in its own `GET` response despite genuinely running. Confirmed via the same live daemon this session was already exercising, both before and after the fix, across an actual daemon restart.

Both findings share one root cause worth naming: `def->stopped` had two different, silently-diverging meanings ("don't autostart at boot" vs. "don't crash-restart"), and only one of the two paths that could clear it (`handle_start()`, ADR-0045's own new endpoint) actually did.

A third, related gap became unavoidable once `start` existed as a concept: for `start` to be reachable from the web dashboard at all, a stopped-but-defined container has to be *findable* — `GET /v1/containers` and `GET /v1/containers/{name}` both needed to stop treating "not in the live registry" as synonymous with "doesn't exist."

## Decision

**`POST /v1/containers/{name}/start`**: idempotent (200, no-op) if already live; 404 if no persisted definition exists at all; otherwise replays the definition's own stored body through `create_container_from_body()` — the exact same function `containerdef_autostart_all()` and `handle_restart_timer_event()` already use, One Source of Truth for "how a definition becomes a live container," not a fourth bespoke path. Clears `stopped` on success, mirroring the fix in `containerdef_autostart_all()` below.

**`POST /v1/containers/{name}/pause` and `.../unpause`**: real cgroup v2 freezer control (`cgroup.freeze` = `"1"`/`"0"`), not `SIGSTOP` — uninterceptable/unignorable by the frozen process, unlike a signal a process can catch or handle. New `registry_set_paused()` (`daemon/src/registry.c`, not `main.c`) is the single mechanism both the REST handlers and `registry_remove()`'s own pre-kill thaw call — One Source of Truth for the actual freeze/thaw syscalls, REST-level 404/409 decisions stay in `main.c` where every other endpoint's error mapping already lives. Unlike `start`/`stop`'s idempotent double-call tolerance, pausing an already-paused container is a `409` — the caller should already know this from its last `GET`, and silently no-opping it could mask a real caller bug (e.g. two racing pause requests).

**`registry_remove()` thaws before killing**: `if (e->paused) registry_set_paused(e, 0);` immediately before `sys_pidfd_send_signal(..., SIGKILL)`, fixing the hang described above. Best-effort — a thaw failure doesn't block the `SIGKILL` attempt that already existed before this fix.

**`containerdef_autostart_all()` clears `stopped` on every successful autostart**, the same call `handle_start()` already makes on its own success path — restoring the true invariant the rest of this phase's own code (below) now depends on: `def->stopped == 1` if and only if the container was manually stopped and is *not* currently live.

**`GET /v1/containers` and `GET /v1/containers/{name}` now also report stopped-but-defined containers**, status `"stopped"`, via a new `containerdef_write_json_stopped_list()`/`containerdef_write_json_stopped_one()` pair in `containerdef.c` (called from `registry_write_json_list()`/`handle_get_one()`, mirroring the existing pattern where `registry_write_json_one()` already reaches into `containerdef_find()` for `restart`/`depends_on`/`readiness`). Safe to rely on `stopped == 1` alone — no live-registry cross-check needed — precisely because the invariant above now holds. Every field a live entry would have but a stopped one genuinely doesn't (`pid`, `networks`, `devices`, ...) is `null`/empty rather than guessed; `image` is recovered by a read-only parse of the persisted request body's own `"image"` field (never sourced/executed, same posture every other consumer of a stored body already has).

`Container.status` gains `"paused"` as a third enum value (alongside `"running"`/`"exited"`, plus the new `"stopped"` case above); a plain `paused: boolean` field is included too for clients that prefer switching on a flag.

## Consequences

- A stopped container is now genuinely recoverable — via CLI (`cixctl start/pause/unpause NAME`), REST, or the web dashboard (buttons gated on `Container.status`, right-click context menu, a distinct amber tree-status dot for paused) — without ever needing a daemon restart.
- The autostart stale-flag fix is a real behavior change for any container that was manually stopped and later revived by a daemon restart: it is now eligible for crash-restart again, matching its own declared policy, instead of silently never restarting again for the rest of that daemon's uptime.
- `GET /v1/containers`' response shape grows a new possible `status` value (`"stopped"`) and a synthesized entry shape for it; every existing consumer (CLI formatters, the web dashboard, `test_*.c` harnesses) was checked against this and found to degrade gracefully (an unrecognized status string is simply displayed as-is, not treated as an error) — additive, not breaking.
- No new authentication/authorization boundary — these endpoints are exactly as protected as every other existing mutating endpoint (network reachability only), the same posture ADR-0043 already named for the console endpoint.
