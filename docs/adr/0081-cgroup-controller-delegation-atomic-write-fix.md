# 0081 — cgroup controller delegation: fix the atomic-write regression ADR-0079 introduced

## Status

Accepted

## Context

ADR-0079's own fix (`cgroup_enable_controllers()`, `src/cgroup.c`) consolidated two working, separate `subtree_control` writes (`"+io"`, `"+cpuset"`) into one combined write (`"+io +cpuset +memory +pids +cpu"`), reasoning that cgroup v2 accepts multiple space-separated `+controller` tokens in a single write. That reasoning was correct as far as it went, but missed a real consequence: **a `subtree_control` write is atomic across every token it contains** -- if even one requested controller is unavailable on the running kernel, the kernel rejects the *whole* write with `EINVAL`, not just the unavailable token.

Confirmed live on 192.168.15.95 during the ADR-0080 diagnostics investigation, once real error text was available to see it: after deploying ADR-0079's fix, `--cpuset=` (which had worked fine under the old two-separate-writes code, moments earlier in the very same investigation) now *also* failed with the identical `cgroup_create: No such file or directory` as `--memory-max=`/`--pids-max=`. `cgroup_enable_controllers()`'s single combined write was failing as a unit -- silently regressing `io`/`cpuset` delegation back to broken, while never actually fixing `memory`/`pids`/`cpu` either, on whatever this specific kernel doesn't offer for at least one of the five requested tokens.

This is the exact kind of bug ADR-0080's own diagnostics work exists to catch quickly: before that pipe existed, this would have looked identical to ADR-0079's own regression from the API side (a bare `500`), and the "fix" would have appeared to make nothing better without a lot more manual digging.

## Decision

`cgroup_enable_controllers()` now reads `cgroup.controllers` (the root's own file listing which controllers this kernel actually supports at this cgroup) **first**, and builds the `subtree_control` write string from only the tokens genuinely present there. One write, same as ADR-0079's own design -- but now guaranteed to only ever request controllers the kernel can actually grant, so a kernel missing one of the five never takes the other four down with it.

Not a reversal of ADR-0079's underlying decision (delegate `io`/`cpuset`/`memory`/`pids`/`cpu` once at daemon startup, in the root's own `subtree_control`, best-effort) -- that stands unchanged. Only the *mechanism* for building the write string changes, from a fixed literal to one computed against real, live kernel capability.

## Consequences

- Fixes the real regression this ADR's own Context section describes: `--cpuset=` works again, and `--memory-max=`/`--pids-max=` now actually take effect too, confirmed live against `kanxeo-builder` (a real image with genuine executables, unlike the empty `base` image ADR-0080's own investigation used first and had to correct for).
- No functional change on a kernel where all five controllers are genuinely available (the overwhelming common case) -- the computed request string is identical to ADR-0079's own fixed one in that case.
- General lesson, worth carrying forward: a cgroup v2 `subtree_control` write's atomicity across tokens means any future addition to this same request list should go through this same "check `cgroup.controllers` first" pattern, not a second hand-written literal.
