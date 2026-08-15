# 0079 — cgroup v2 controller delegation at daemon startup

## Status

Accepted

## Context

Found during the Phase 44 final-validation pass on 192.168.15.95 (this project's first genuinely bare-metal, real-PID-1 install to actually exercise a resource-limited container): `thincctl run` with no resource flags succeeded, but the identical command with `--memory-max=`, `--pids-max=`, or `--cpuset-cpus=` returned a bare, undiagnosed HTTP 500. A flagless `run` and a `--network=` attach both worked; only the resource-limit path failed.

Local reproduction on this dev sandbox (a privileged LXC under a normal Debian systemd install) with the exact same binary succeeded for every flag combination -- ruling out a code logic bug and pointing at an environment-specific condition unique to 192.168.15.95.

Root cause, confirmed by reading `cgroup_create()` (`src/cgroup.c`) against real cgroup v2 kernel semantics: `cgroup.subtree_control` at any cgroup level starts genuinely **empty** by kernel default. No controller is ever auto-enabled for child cgroups regardless of what `cgroup.controllers` lists as merely *available* at that level -- a parent must explicitly write `+<controller>` to its own `subtree_control` before that controller's files (`memory.max`, `pids.max`, etc.) exist at all in any child. `cgroup_create()`'s writes to `memory.max`/`pids.max`/`cpu.max`/`cpuset.cpus` are conditional on the caller having requested that limit (`if (lim->memory_max > 0) ...`); creating the bare child cgroup directory needs no delegated controller at all, which is exactly why flagless containers were unaffected.

Every dev/test environment this project had exercised so far ran under a real distro's own systemd, which pre-delegates `memory`/`pids`/`cpu`/`cpuset` to its own hierarchy by default -- masking this gap completely until a genuine `thincd`-as-PID-1, no-systemd install (192.168.15.95's actual deployment mode) hit it for real. The project already had precedent for exactly this class of fix: `cgroup_enable_io_accounting()`/`cgroup_enable_cpuset()` (`src/cgroup.c`, called once at daemon startup) delegated `io` and `cpuset` for the same reason, but the delegation was never extended to `memory`/`pids`/`cpu` -- confirmed via exhaustive grep to be the only two `subtree_control` writes anywhere in the codebase before this fix.

## Decision

**Consolidate the two existing per-controller functions into one**, `cgroup_enable_controllers(void)`, which writes `"+io +cpuset +memory +pids +cpu"` to the root's `cgroup.subtree_control` in a single call (cgroup v2 accepts multiple space-separated `+controller` tokens in one write). Called once at daemon startup, before any container's cgroup leaf can exist, replacing the two separate `cgroup_enable_io_accounting()`/`cgroup_enable_cpuset()` calls in `daemon/src/main.c`'s init sequence.

A clean cut-over, not two old functions kept alongside a new third one (this project's own "no backward-compat shims" convention) -- the old functions' scope (io, cpuset) is a strict subset of the new one's, so nothing is lost by merging.

Same "best-effort, never fatal" posture the io/cpuset handling already established: a write failure (already delegated from a prior daemon instance, or a genuinely restricted host missing one of these controllers) is logged via `perror()` but never blocks daemon startup. A container requesting a limit whose controller isn't available simply runs unrestricted for that one resource -- "degrade, never fail container creation over it," unchanged from the existing posture, just extended to cover memory/pids/cpu as well as io/cpuset.

## Consequences

- Fixes the real regression: `--memory-max=`/`--pids-max=`/`--cpuset-cpus=` now succeed on a real-PID-1, no-systemd install, matching behavior already correct under every systemd-based dev/test environment.
- `include/container.h`'s declaration and all doc-comment cross-references (`daemon/src/main.c`, `daemon/include/swap.h`, `src/overlay.c`, `src/cgroup.c` itself) were updated to the new function name for internal consistency; no other file references the two retired names.
- The separate, still-open gap this investigation surfaced but did not fix: `registry_create()` (`daemon/src/registry.c`) collapses any `container_create()` failure into a single generic `REGISTRY_ERR_CREATE_FAILED`, and `create_container_from_body()` (`daemon/src/main.c`) turns that into an undiagnosed "failed to create container" 500 -- the real errno/failure point is discarded regardless of cause. This is exactly the kind of silent failure that made this particular regression hard to diagnose from the API alone (local reproduction was required); a real fix belongs to the same "surface real diagnostics, don't swallow errno" precedent already established for `pkg.c`'s build-container-prep path, and is tracked as follow-on work, not addressed here.
- This finding is specific to real-PID-1, no-systemd installs. Every dev/test environment that runs under a distro's own systemd (this project's own sandbox included) already had these controllers pre-delegated and would never have surfaced this gap on its own -- a reminder that this project's only genuine bare-metal deployment target is also its only reliable place to catch PID-1-specific gaps like this one.
