# 0074 — Pressure-stall information (PSI) in host and per-container stats

## Status

Accepted

## Context

Raised by the user in the same session as ADR-0073's host stats work: "let's do the fixes claude, also add to our log, we want diskio cpu pressure stall, io pressure stall, and memory pressure stall graphs per container? Also for the host as we discussed? does that make sense?" — explicitly requested alongside host stats, with the same "asap" priority, to help diagnose real resource contention while other work (the `lldap` build) was ongoing.

Raw usage counters (`cpu.usage_usec`, `memory.current`, `io.stat`'s `rbytes`/`wbytes`) answer "how much of a resource is being consumed" but not "is anything actually being held up waiting for it." A container using 100% of one core is not automatically a problem; a container whose tasks are *stalled* waiting for CPU, memory reclaim, or I/O — even at low nominal usage — genuinely is. Linux's own Pressure Stall Information (PSI, `cpu.pressure`/`io.pressure`/`memory.pressure` in cgroup v2) answers exactly this question and is already present, unused, on every cgroup v2 leaf this daemon creates — this ADR wires it up, it does not invent a new mechanism.

## Decision

**New `cgroup_read_pressure()` primitive** (`src/cgroup.c`/`include/container.h`), modeled directly on the existing `cgroup_read_stat_key()`/`cgroup_read_io_totals()` shape: takes a cgroup directory fd and a filename (`cpu.pressure`/`io.pressure`/`memory.pressure`), parses both the `some` and `full` lines (`avg10`/`avg60`/`avg300` as percentages, `total` as cumulative stalled microseconds), fills a `struct cgroup_pressure`. Same best-effort convention as every other reader in this file: a missing file (kernel built without `CONFIG_PSI`, or a cgroup v1 host) leaves the struct fully zeroed, not an error — this platform must keep running on kernels without PSI support.

**Wired into both stats endpoints identically**, via a shared `write_pressure_json()` JSON-writer helper (`daemon/src/main.c`) so the `{"some":{...},"full":{...}}` shape is defined exactly once:
- `GET /v1/containers/{name}/stats` (ADR-0054): `cpu.pressure`/`memory.pressure` nested inside the existing `cpu`/`memory` objects; `io.pressure` nested inside the existing `disk` object (which already aggregates `io.stat`-derived counters) — deliberately not a new top-level `io` object, to keep one JSON object per real resource rather than splitting disk-related data across two keys.
- `GET /v1/system/stats` (ADR-0073): the same three, read from the cgroup v2 **root** (`/sys/fs/cgroup`, opened fresh per request via `O_PATH`) rather than any single container's leaf — the host-wide view answers "is the box itself under pressure," distinct from any one container's own.

**`cpu.pressure`'s `full` line is real but structurally always near-zero for a single task**: the kernel only ever populates it when *every* runnable task in the cgroup is stalled simultaneously, which a lone process can't produce (it can't be running and fully stalled at once). Left as reported by the kernel rather than special-cased or hidden — real data, real edge case, not a bug in this integration.

## Consequences

- Purely additive to both existing endpoints' response shapes — no field removed, no schema-breaking change.
- `cixctl stats NAME` and `cixctl host-stats` both gain one summary pressure line per resource (`cpu.pressure some.avg10=... full.avg10=...`, etc.) via a shared `print_pressure_line()` CLI helper.
- Verified live against a real running daemon: host-level PSI confirmed nonzero and plausible (this build sandbox's own CPU pressure genuinely elevated during the concurrent `lldap` build); per-container PSI wiring verified via the existing `test_container_stats` regression test passing end-to-end after the change (the reader itself was independently verified live against the real root cgroup, and container leaf cgroups expose the identical file set).
- Does not add server-side history or graphing — same "raw point-in-time snapshot, client computes anything derived" posture ADR-0054/ADR-0073 already established; a web dashboard graph over time is a client-side concern layered on repeated polling, not a new capability this ADR needs to provide.
