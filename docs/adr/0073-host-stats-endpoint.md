# 0073 — Host-wide stats endpoint (GET /v1/system/stats)

## Status

Accepted

## Context

Raised directly by the user while monitoring a real, live `lldap` build on 192.168.15.95: "we should add a system load and memory and network and so on (same as we did for containers) but for the host, right? I can see that memory is full on the host when I look through the proxmox stats for the vm." Every container already gets a real, host-side stats snapshot via `GET /v1/containers/{name}/stats` (ADR-0054) — CPU/memory/disk/network, sourced from that container's own cgroup v2 accounting files and its veth's `/sys/class/net` counters. Nothing equivalent existed for the host itself: an operator had no way to see the box's own load average, memory pressure, or aggregate network/disk state through Kanxeo's own API — only indirectly, by eyeballing a hypervisor's own guest-level view (Proxmox, in the user's case), which conflates page cache with genuinely unavailable memory and has no visibility into Kanxeo's own state at all.

## Decision

**New `GET /v1/system/stats`**, deliberately mirroring `handle_container_stats()`'s own established shape: nested per-category JSON objects (`load`, `cpu`, `memory`, `disk`, `networks`), raw cumulative/monotonic counters only — never a pre-computed rate or percentage, matching this project's consistent "client computes its own deltas" convention (the web dashboard's CPU chart already does exactly this for container stats, see `web/app.js`'s `cpuPercents` computation).

Every field comes from a real, existing kernel-exposed source, read directly (no shelling out, matching this project's syscall/proc-first convention throughout):

- `load`: `/proc/loadavg`'s first three fields (1/5/15-minute load averages).
- `cpu`: `/proc/stat`'s own first `cpu` line — user/nice/system/idle/iowait/irq/softirq/steal jiffies, in the kernel's own fixed field order.
- `memory`: `/proc/meminfo`'s `MemTotal`/`MemFree`/`MemAvailable`/`Buffers`/`Cached`/`SwapTotal`/`SwapFree` (all kB in the source file, converted to bytes).
- `disk`: `statvfs()` on `g_base_dir` (Kanxeo's own data root) — total/free/avail bytes, the same call `diskformat.c` already uses elsewhere in this codebase.
- `networks`: every real interface under `/sys/class/net`, using the same per-interface `rx_bytes`/`tx_bytes`/`rx_packets`/`tx_packets` reader container stats already uses (`read_net_stat()`, generalized from a veth-only helper to accept any interface name — the two now share one implementation instead of duplicating the exact same `/sys/class/net/<if>/statistics/<file>` read, avoiding a real "No Parallel Implementations" violation).

**New `jw_num()` JSON-writer primitive** (`daemon/src/json.c`/`daemon/include/json.h`) — the first genuinely fractional value (`load1`/`5`/`15`) this daemon has ever needed to serialize; every prior numeric field across the whole API is a whole-number counter or size, so `jw_int()` alone sufficed until now. Fixed `%.2f` formatting (matching `/proc/loadavg`'s own real precision) rather than a general-purpose float writer — the only values ever passed through it, load averages, are always non-negative with two meaningful decimal places, so a general formatter would be unused generality.

Every read here is explicitly best-effort: a missing/malformed source (e.g. no `/proc/loadavg` on some hypothetical minimal kernel build) leaves that section zeroed rather than failing the whole request — the same "snapshot, not all-or-nothing" convention `read_net_stat()` already established for container stats' own network section.

## Consequences

- Purely additive: a new read-only endpoint, no change to any existing route, schema, or persisted state.
- `read_net_stat()`'s parameter renamed `veth` → `ifname` to reflect its now-dual use (host interfaces are not veths) — a doc-comment-only, non-breaking rename of a `static` function's own parameter name.
- Directly answers the user's "memory is full on the host" question with real, disaggregated numbers (`total`/`free`/`available`/`buffers`/`cached`) instead of a single opaque "used" figure a hypervisor's own guest view conflates — `available_bytes` in particular is the kernel's own best estimate of reclaimable-and-usable memory, the number that actually answers "is the box under real memory pressure."
- Does not include per-container or host-wide pressure-stall information (PSI) — a real, related, but separate gap tracked as task #677, deliberately scoped out of this change to keep it to one concern.
- CLI: `kanxeoctl host-stats` (not `stats`, already `stats NAME` for a container — a bare host-scoped verb, consistent with `swap`/`routes`/`logs`'s own flat top-level naming for other `/v1/system/*` resources).
