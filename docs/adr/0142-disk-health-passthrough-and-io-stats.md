# 0142 — Disk mount health/auto-remount, raw disk passthrough, per-disk I/O stats, and post-creation container-storage migration

## Status

Accepted

## Context

Raised mid-implementation of ADR-0141, folding directly into the same body of work rather than a separate later round:

1. **Mount-state recovery.** ADR-0140's own Consequences already found this live: 192.168.15.95's `sda` shows `mounted: false` despite having been formatted and mounted successfully during Phase C verification. Root cause, confirmed directly: `diskformat.c`'s own job state (`DISKFORMAT_STATE_READY`, which fs_type it used) is **purely in-memory**, forgotten on every daemon restart -- there is nothing persisted that would let a restart know "this disk was already formatted as ext4, remount it." A reboot genuinely loses the mount with no path back except a manual re-format (destructive) or a manual `mount(8)` by hand.
2. **Raw disk passthrough.** Disks without a role -- no `state-storage`/`rebuildable-storage`/`log-storage`/`container-storage`/`backup` assignment, no thinc-managed filesystem at all -- should still be grantable directly to a container as a raw block device, the same way `--device=` already grants a USB/PCI/GPU/net device. A workload that wants to manage its own filesystem (a NAS app, a database wanting raw block access) shouldn't be forced through thinc's own format/mount pipeline first.
3. **Per-disk stats.** `GET /system/stats`'s `disk` field is a single aggregate (`statvfs()` on wherever `g_base_dir` lives) -- no per-disk capacity, and no I/O throughput/latency at all, for any disk. Checked directly: `/sys/block/<dev>/stat` gives real, standard per-device counters (11+ space-separated integers -- reads/writes completed, sectors, and critically field 10, "milliseconds spent doing I/Os," the real per-device analog of the "delay" the user asked for -- cumulative time the device had at least one I/O outstanding).
4. **Container-storage should be migratable after creation too**, not just chosen once at `POST /containers` time -- ADR-0141 explicitly called this out of scope; the user asked for it directly once ADR-0141's own state/rebuildable/log-storage migration mechanism was already designed. Same underlying mechanism, narrower scope (one container's own overlay directory instead of a daemon-wide directory).

## Decision

### 1. Disk mount-state persistence + auto-remount on startup

`diskformat`'s per-disk outcome (fs_type, and that it succeeded) becomes a small persisted fact, not just in-memory job state -- extending `diskrole`'s own already-persisted entry (`disk_name` -> `role`) with the fs_type it was last formatted with once a format job reaches `DISKFORMAT_STATE_READY`, reusing the existing `diskroles.json` (now living under `STATE_DIR` per ADR-0141) rather than a second parallel file. At startup, after role/state initialization, a new pass walks every role-assigned disk that's `present` (`disk_enumerate()`) but currently **not** mounted, and re-mounts it directly (a plain `mount(2)`, no `mkfs` -- the filesystem already exists) using its own remembered fs_type. A disk that's present but fails to remount (corrupted filesystem, physically failing) logs the failure and is left unmounted, exactly the same "don't guess, surface it" posture `diskformat`'s own `FAILED` state already has -- never a second automatic format attempt, which would be destructive.

### 2. Raw disk passthrough via the existing device-grant mechanism

`struct discovered_device`'s `bus` field grows from `char bus[4]` to `char bus[8]` (`"disk"` is 4 characters + NUL, doesn't fit the current 4-byte buffer -- found directly while checking this, not assumed) and `device_enumerate()` gains a new pass alongside its existing USB/PCI/net/GPU walks: every disk from `disk_enumerate()` that is **not** the OS disk and has **no** assigned role becomes a `"disk:<name>"` grantable device, exactly the same `--device=` container-creation field every other bus already uses. A disk claimed this way is handed to the container as a raw block device node (mirroring how a claimed `net:` interface simply stops appearing in `GET /devices` once it's moved into a container's own netns) -- thinc never touches its content, formats it, or expects a filesystem thinc itself understands. A disk that already carries *any* role is never offered for passthrough (the two mechanisms are mutually exclusive by construction, not by a runtime check that could drift) -- `device_enumerate()`'s own disk pass simply skips anything `diskrole_lookup()` returns non-NULL for.

### 3. Per-disk capacity and I/O stats

`GET /disks` (already the live, per-poll disk inventory) gains two new fields per entry, both real and freshly read on every call, no persistence: `io` (cumulative `read_bytes`/`write_bytes`/`read_ops`/`write_ops`/`io_time_ms`, parsed from `/sys/block/<name>/stat`, the client computing rates the exact same way it already does for `SystemStats.networks[]`'s own cumulative counters) and, when `mounted` is true, `usage` (`total_bytes`/`free_bytes`/`avail_bytes`, a real `statvfs()` on `mount_path` -- the exact same call `SystemStats.disk` already makes, just per-disk instead of only for `g_base_dir`). No new endpoint -- this is the natural home, since `GET /disks` already is the one place every disk's live state is reported. Web dashboard: the existing Disks page (ADR-0140) gains a small live usage bar + I/O rate per row (reusing `drawChart()` for a compact per-disk sparkline would be excessive on a list page; a live-updating number and a thin capacity bar match the density every other list page here already uses), and **Host Stats gains a per-disk chart section** for anyone wanting the fuller graph view -- reusing the exact same canvas renderer every other stats graph on that page already uses, one small chart set per currently-mounted disk.

### 4. Container-storage migration, after creation

Extends the exact same migration-job mechanism ADR-0141 already designed for `state-storage`/`rebuildable-storage`/`log-storage` -- generalized further, not a fourth bespoke implementation:

```
POST /v1/containers/{name}/migrate-storage   body {"disk": "sdc"|null}   -> 202 + job status
GET  /v1/containers/{name}/migrate-storage   -> job status
```

`disk: null` migrates back to the default OS-disk placement, symmetric with the other three. The container's own overlay directory (upperdir/workdir/merged) is copied to the new disk's own `containers/<name>/` path using the same permission-preserving tree-copy primitive (ADR-0141's own correctness fix, not `merge_tree()`'s original hardcoded-mode version), then the container definition's own `disk` field is atomically updated to point at the new location -- the container itself is briefly stopped for the final incremental copy + cutover (unlike the daemon-wide singletons, a running container actively writing to its own overlay can't be safely repointed underneath itself the way a single-threaded reactor can safely repoint its own static path variables; this is a real, necessary difference from the other three migration types, not an oversight) and restarted automatically once cutover completes, matching this project's own restart-policy-aware container lifecycle handling elsewhere.

## Consequences

- Closes the exact live gap ADR-0140 found and explicitly deferred (`sda` losing its mount across a reboot) -- 192.168.15.95 will self-heal this on its next restart once this phase deploys, no manual intervention needed.
- A disk with no role at all is no longer a dead end -- it's either thinc-managed storage (one of the five roles) or directly usable by a container's own workload, never neither.
- Real, per-disk visibility into both capacity and I/O behavior, closing the "proper disk stats somewhere" gap directly.
- Storage placement becomes uniformly movable across all four kinds this project now has an opinion about (container/state/rebuildable/log), not three plus one deliberately-excluded exception.
- Folds into the existing six-phase ADR-0141 plan rather than replacing it: mount persistence/auto-remount and the `bus[4]`->`bus[8]` passthrough fix land in Phase 1 (shared primitives, since both touch the same disk-role/disk-enumeration groundwork); per-disk stats land alongside Phase 6 (the natural point once every disk-facing piece is otherwise done); container-storage migration becomes its own phase after `rebuildable-storage`, reusing that phase's own now-proven migration-job pattern.
