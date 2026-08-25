# 0070 — A consolidated, API-accessible, size-capped log store

## Status

Accepted

## Context

Raised directly by the user, spelled out precisely after an earlier, vaguer mention had been lost to context compaction: real kernel `dmesg`, `cixd`'s own diagnostics, and an audit trail of every action taken through either client (`cixctl`, the web dashboard) should all land in one place, retrievable over the API like everything else this project builds (no SSH, no general shell, ADR-0034) — with a user-configurable total size cap and oldest-first eviction once that cap is reached.

Three design questions were resolved directly with the user before writing any code, not assumed:

- **What do "web UI logs" and "ctl logs" mean?** Resolved as: neither client generates or ships its own log entries. Both are pure read-only viewers into the one log `cixd` writes — consistent with the API-First Mandate, which already means every `cixctl` command and every web UI action is, underneath, a REST call this daemon already handles. The "real audit trail" the user separately asked for falls out of this for free: one log call in `dispatch()` (the single function every request already passes through) captures method+path for every real action taken via either client, with zero client-side instrumentation needed.
- **One shared pool or per-source quotas?** One shared pool, one FIFO — every source interleaves into a single store bounded by one total byte cap, so a `GET /system/logs` tail stays in real chronological order across sources rather than needing to merge multiple per-source streams.
- **Format?** Structured JSON-lines (`{"ts", "source", "level", "msg"}`) — enables real filtering by source/level/time through the REST API, not just raw tail/grep.

## Decision

**New `daemon/src/logstore.c`/`logstore.h`**, matching `swap.c`'s (ADR-0069) self-contained-module shape: its own persisted state, its own init function taking a directory + state path, no dependency threaded through any other subsystem's own code beyond the one `dispatch()` hook and the one `/dev/kmsg` epoll registration.

**Storage is a small, fixed number of rotating segment files (`LOGSTORE_SEGMENT_COUNT` = 8), not a byte-exact single-file ring buffer.** A literal ring buffer of variable-length JSON lines needs an O(n) rewrite per eviction (shift every remaining byte down to make room). Real bounded-log systems that already solve this problem (journald, `logrotate`) all make the same tradeoff instead: drop the oldest whole segment, O(1), and accept that the cap is enforced at segment granularity (each segment capped at `max_bytes / 8`) rather than byte-exact. A few percent of slop against the configured cap is the accepted, ordinary cost, not a bug.

**Kernel capture reads `/dev/kmsg` directly** (non-blocking, registered into the daemon's existing single-threaded `epoll` loop the same way every other fd it watches already is — a new `CONN_KMSG` conn kind, no new threading model) rather than shelling out to a `dmesg` binary that isn't staged anywhere on a minimal image. `/dev/kmsg`'s own record format (`<prio>,<seq>,<us>,<flags>;<text>`, documented in the kernel's own `Documentation/ABI/testing/dev-kmsg`) is parsed directly; the syslog priority nibble maps to this store's own `level` field.

**The audit trail is one `logstore_write("audit", ...)` call at the top of `dispatch()`** (`daemon/src/main.c`), logging `{method, path}` for every request except `GET /v1/health` — both clients poll that purely for a status dot every few seconds, and logging it would drown every real action in noise for no diagnostic value.

**`cixd`'s own diagnostics keep going to stderr, unchanged, in addition to the new store** — stderr remains the only observable channel during boot, before the API is reachable at all (the same reasoning `cix-install.c`'s own dual-console work already established); this ADR is additive to that, not a replacement.

**`GET /v1/system/logs`** (query params `source`/`level`/`tail`/`since`, mirroring `.../files?path=...`'s own established query-string convention) and **`GET`/`PUT /v1/system/logs/config`** (`{"max_bytes": N}`, bounded to `[1 MB, 100 GB]`) are the REST surface; `cixctl logs`/`logs config` and a "Logs" page under the dashboard's System group (grouped alongside Daemon/Devices/Routes/Update/Backup — the Routes page moved into this same group in the same change, a small unrelated nav reorg the user asked for alongside this) are its only two clients, matching the API-First Mandate exactly.

## Consequences

- Verified live against a real daemon: audit entries appear for real requests (network create, swap toggles, etc.) and are correctly excluded for `/v1/health`; `source`/`level`/`tail`/`since` filtering all round-trip correctly; `PUT .../config` validates bounds (`400` on an out-of-range value) and persists across the exact same request/response cycle `swap.c`'s own config-style endpoints already established.
- `/dev/kmsg` capture is best-effort, matching every other non-critical startup reconciliation step in this daemon (`swap_init()`'s own `swapon()` retry, `cgroup_enable_io_accounting()`) — a dev/test invocation or a kernel where `/dev/kmsg` isn't reachable simply never gets kernel-source entries; every other source is unaffected.
- A box that never calls anything under `/v1/system/logs` still gets the audit trail and cixd's own diagnostics captured automatically (this is *not* opt-in the way `swap` is, since the whole point was visibility by default) — only the size cap is operator-tunable, defaulting to 100 MB.
- Deployment to the real target VM (`192.168.15.95`) rides the same fresh-ISO mechanism already used for the `CONFIG_SWAP` kernel fix (ADR-0069) in this same session, since both land in the same rebuild.
