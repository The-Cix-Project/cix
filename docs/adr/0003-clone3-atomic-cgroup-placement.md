# 0003 — clone3 + CLONE_INTO_CGROUP + CLONE_PIDFD over legacy clone()

## Status

Accepted

## Context

The traditional way to place a new process into a cgroup is: create it, then write its PID to the cgroup's `cgroup.procs` file. That has a race window — between the process existing and the write landing, it's running in the *wrong* (parent's) cgroup, which is a real correctness problem for resource limits meant to apply from the first instruction. Reaping also traditionally means `waitpid()` on a PID, which is subject to PID-reuse races if the PID is recycled between exit and reap.

## Decision

Use `clone3(2)` (see ADR-0002 for how, given no glibc wrapper) with `CLONE_INTO_CGROUP` (atomic placement into a pre-opened cgroup, passed as an `O_PATH` fd — no `cgroup.procs` write, no race window) and `CLONE_PIDFD` (yields a pidfd for race-free `waitid(P_PIDFD, ...)` reaping, immune to PID reuse).

## Consequences

- Requires a kernel new enough for `clone3`/`CLONE_INTO_CGROUP` (5.7+) and cgroup v2 — acceptable, this project targets a modern mainline kernel by design.
- The cgroup must be created and opened (`cgroup_create()`) *before* the clone, which shapes the whole `container_create()` call order (cgroup first, then clone3, then the child does its own namespace/mount setup) — see `docs/ROADMAP.md` Phase 1 for the full sequence and verification that placement is actually atomic (exactly one PID in `cgroup.procs` at the moment of check).
