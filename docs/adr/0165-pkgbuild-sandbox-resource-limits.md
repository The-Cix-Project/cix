# 0165 — Real cgroup resource limits on the pkgbuild sandbox

## Status

Accepted

## Context

A burst of ~19 `pkg install` calls fired in quick succession against 192.168.15.95 (rebuilding the images for its 7 restored containers, part of the Phase 5 rebrand migration) left `cixd` completely unresponsive: TCP still accepted connections, but no HTTP request ever completed again. This matches a previously-documented incident on this same box (ROADMAP Part 144), triggered the same way — a burst of concurrent `pkg install`/image-recipe-apply activity.

Investigating the actual code (not assumption) found the real gap: every `__pkgbuild-N` build sandbox already goes through `container_create()` — the identical namespaced-container mechanism a regular container uses, including a real `struct cgroup_limits` — but `pkg.c`'s own build-spec construction only ever set `cg.name`. `memory_max`/`pids_max`/`cpu_max`/`cpuset_cpus` were all left at their zero-value default, which `cgroup_create()` correctly (if silently) treats as "no limit" for that field. `pkg-build-config`'s existing `max_concurrent_jobs` only caps how many builds run *at once* — it never capped what any single one could consume. Confirmed by grep: zero references to `memory.max`/`cpu.max`/`pids.max` anywhere in `daemon/src/pkg.c` before this fix.

Checking Proxmox directly during the incident showed the VM had gone CPU-idle almost immediately and stayed at a flat ~6GB memory the whole time it was unresponsive — not the rising-memory, sustained-high-CPU signature a classic OOM/thrashing death spiral would show. That doesn't cleanly confirm the original Part 144 "memory pressure" hypothesis (which was itself explicitly hedged as unproven); it reads more like a genuine deadlock or a blocking syscall than a slow resource-exhaustion grind. The exact mechanism remains unconfirmed both times. What's certain either way: unbounded resource use by concurrent, un-sandboxed-by-limit build processes is a real gap independent of the precise failure mechanism, and a deadlock is if anything *more* likely to reproduce under concurrent load than under a single build, not less.

## Decision

Extend the existing `pkg-build-config` resource (`GET`/`PUT /v1/system/pkg-build-config`, ADR-0157) with two more fields, rather than inventing a separate config resource for a concern that's really "how pkg builds behave" — the same bucket `max_concurrent_jobs` already lives in:

- `memory_max` (bytes, default **2GiB**)
- `cpu_max` (raw cgroup v2 `cpu.max` syntax — `"QUOTA PERIOD"` in microseconds, default **`"100000 100000"`**, one full CPU's worth)

Both reuse the identical `struct cgroup_limits` mechanism (and identical field names: `memory_max`/`cpu_max`) that every regular container's own `POST /containers` already has — `pkg.c`'s build-spec construction now populates `spec_out->cg.memory_max`/`cg.cpu_max` from the configured values immediately after setting `cg.name`, instead of leaving them at the zero-value "no limit" default. `0`/`null` means unlimited for either field, a deliberate, explicit opt-out (a box with real spare capacity can say so), not a validation error.

The PUT endpoint became a genuine partial update (only the fields given are changed) rather than requiring `max_concurrent_jobs` on every call, matching the newer convention `daemon-config`/`hostauth-config` already established elsewhere in this API.

Defaults are deliberately **not** unlimited — a real ceiling from the very first boot of a freshly installed box, not an opt-in an operator has to remember to configure only after getting burned once (which is exactly what happened here).

## Consequences

- Every future pkgbuild sandbox is capped, immediately, on any box running this build — including ones that never explicitly configure `pkg-build-config` at all.
- An unusually large single package (a full kernel build, say) could plausibly need more than 2GiB/1 CPU and would need `PUT /v1/system/pkg-build-config` raised first, or set to `0`/`null` (unlimited) for that box. This is a real, known trade-off of picking a conservative default over no ceiling at all.
- `memory_max`/`cpu_max` only take effect for the *next* build's own cgroup — an already-running build is not retroactively adjusted (matches `max_concurrent_jobs`'s own existing "doesn't disrupt jobs already in flight" posture, for the same reason: nothing about a build already in progress can safely be pulled out from under it).
- This closes the *resource-ceiling* gap specifically. It does not, on its own, explain or fix whatever the actual blocking mechanism inside `cixd`'s event loop was during either incident — that remains an open, unconfirmed question, same as ROADMAP Part 144 left it. A resource limit makes a hang triggered by unbounded consumption impossible; it does not rule out every other way a single-threaded event loop could still get stuck.
