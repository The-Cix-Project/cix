# 0130 — Host stats graphs page

## Status

Accepted

## Context

Part 4 of the logging/web-UI epic. User request: "in the system tree, i want to be able to have a page which shows the host stats in graphs and so on?"

`GET /v1/system/stats` (ADR-0073, task #673) — a host-wide load/CPU/memory/disk/network snapshot, the exact host-level counterpart to the already-visualized per-container `GET /containers/{name}/stats` — has existed in the API since ADR-0073 but had zero web dashboard presence until now (confirmed via a repo-wide search: zero references to `"system/stats"` anywhere in `web/`). This part closes that gap by reusing the container Stats tab's own already-built charting machinery rather than inventing a second one.

## Decision

**New tree leaf, System > Server > Host Stats** (`hash: "host-stats"`), between Routes and Logs. A new `stats` tree icon (a small bar-chart glyph, matching every other tree icon's own minimalist stroke-SVG style) — no existing icon fit.

**`drawChart()` reused verbatim** (`web/app.js`) — the same hand-rolled canvas line-chart function the container Stats tab already uses, unmodified. A parallel, new polling/history module (`hostStatsHistory`/`startHostStatsPolling()`/`stopHostStatsPolling()`/`pollHostStatsOnce()`/`renderHostStatsCharts()`) mirrors the container-stats module's own shape (`statsHistory`/`startStatsPolling()`/etc.) closely enough to read as the same pattern, but is a genuinely separate instance rather than a parameterized shared one — the two data shapes differ enough (see below) that forcing one generic function over both would have cost more in conditional branching than the ~40 lines of real duplication it would have saved, the same "duplication is sometimes cheaper than the wrong abstraction" judgment call this codebase already makes elsewhere (e.g. `logstore_write()`/`logstore_write_container()` sharing `write_entry()` only where the shapes are actually identical).

**Two real differences from the container-stats case, not just a copy-paste**:
- **CPU**: `GET /containers/{name}/stats` gives a cgroup's own cumulative `usage_usec` (a straight usec-per-ms ratio computes %). `GET /system/stats` gives raw `/proc/stat` jiffies per category (`user`/`nice`/`system`/`idle`/`iowait`/`irq`/`softirq`/`steal`) — the classic `top(1)`-style formula applies instead: CPU% = `1 - (Δidle / Δtotal)` between two samples, `idle` only (not `iowait`, matching `top`'s own default "busy" definition).
- **Memory/Disk**: a container's `disk.upper_bytes` is itself already a cumulative on-disk footprint (a gauge, charted directly, no diffing). The host's `memory`/`disk` fields are also gauges (`total_bytes`/`available_bytes`/`avail_bytes`), but expressed as free/available rather than used — charted as `total - available`/`total - avail` so the resulting chart reads the same way the container's own memory chart does ("used, against a total ceiling").
- **Network**: `stats.networks` here is *every* real interface under `/sys/class/net`, not scoped to one container's own veth — summed across all of them for one aggregate rate chart, the same reduce-and-sum the container chart already does over its own (much smaller) `networks[]` array. Explicitly labeled "all interfaces combined" in the UI, since this necessarily includes loopback and every container's own veth/bridge traffic, not just external I/O — an honest label rather than a curated-but-misleading one; a filtered view would need new backend support, out of scope here.
- **Load average**: `load1`/`load5`/`load15` are themselves already smoothed exponential averages, far too slow-moving to be meaningfully graphed over this page's own ~2-minute (60-sample × 2s) rolling window — shown as plain text instead of a fifth chart, avoiding a chart that would just look like three flat lines.

## Verification

`node --check web/app.js`. Field names double-checked against `daemon/src/main.c`'s own `jw_key()` calls directly (not just `openapi.yaml`, in case of doc drift) — confirmed exact match. A real scratch daemon's `GET /v1/system/stats` response inspected directly to confirm the live shape matches what the new JS expects. Full clean rebuild (`-Wall -Werror`, zero warnings, no daemon-side code changed) + full regression sweep (37 test binaries, including `test_web.c`) confirm zero regressions.

## Consequences

- Closes the one real remaining gap between `GET /system/stats`'s own existence (ADR-0073) and it having zero operator-facing surface until now.
- The "all interfaces combined" network chart is honest but coarse — an operator wanting per-interface host-level throughput still needs `thincctl host-stats` or a direct API call and their own filtering; not solved here, and not asked for.
