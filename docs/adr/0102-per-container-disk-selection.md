# 0102 — Per-container disk selection at creation time (Phase D)

## Status

Accepted

## Context

Task #638, the previously-open "Phase D: container-storage migration" referenced in `daemon/include/diskrole.h`'s own comment. `POST /v1/containers` always placed a new container's writable overlay storage (upperdir/workdir/merged) under a single fixed `CONTAINERS_DIR` (the OS disk's own containers directory) — there was no way to tell a container "put your data on disk X" at creation time. Meanwhile the disk subsystem already had everything else in place but disconnected: `disk.c` (real enumeration + live `mounted`/`mount_path` per ADR-0099), and `diskrole.c` (Phase B, an operator-assignable `"container-storage"` role) — but nothing ever *read* that role. Assigning it did nothing; it was pure inert metadata.

## Decision

`POST /v1/containers` gains an optional `disk` field (bare kernel disk name, e.g. `"sdb"`). When given, `resolve_container_disk_root()` (`daemon/src/main.c`) validates it three ways before using it: the disk must exist (`disk_enumerate()`), be currently mounted (ADR-0099's live `/proc/mounts` ground truth), and carry the `"container-storage"` role (`diskrole_lookup()`) — any failure is a specific 400, not a generic one. On success, the container's own `container_base` is rooted at `<disk's mount_path>/containers/<name>` instead of `CONTAINERS_DIR/<name>`; everything downstream (upperdir/workdir/merged, `spec.ov.*`) is unchanged, since it already only ever concatenates onto `container_base`.

`struct registry_entry` gains `disk_name` (empty = default placement), set by `registry_create()`'s new final parameter and echoed as `"disk"` in `GET /containers`. This is purely a memo — the caller has already resolved the real filesystem paths before `registry_create()` is ever called — used only by `container_root_for()` (main.c) to reconstruct the correct root for the two other places that need it after the fact: `handle_container_file_read()`'s stopped-container branch and `handle_container_stats()`'s upperdir-size read. Both re-resolve the disk's *current* mount_path fresh via `disk_enumerate()` rather than trusting a cached path, matching ADR-0099's own "live state is the one source of truth" precedent — if the disk was since unmounted (a real but rare operational anomaly), the lookup falls back to `CONTAINERS_DIR` and fails cleanly (ENOENT) rather than crashing.

A `restart:"always"`/etc. container's own persisted create-request body (`containerdef.c`) already includes whatever `"disk"` was originally given, so autostart replay (`containerdef_autostart_all()`, itself just a re-POST of that body through this same code path) reconstructs the correct placement automatically — no separate persistence mechanism needed for that case.

## Consequences

- The `"container-storage"` disk role, assignable since Phase B but never consumed by anything, is now real: it's the one thing that makes a mounted disk a valid container-placement target, closing the gap this project's own docs flagged as "Phase D."
- `kanxeoctl run --disk=NAME ... -- CMD` is the CLI surface.
- **Not addressed here, a genuinely pre-existing, separate gap found while investigating this**: `DELETE /v1/containers/{name}` has never removed a container's own upper/work/merged directories on disk, for *any* placement (OS disk included) — `handle_delete()` only ever calls `registry_remove()`/`containerdef_remove()`, no directory cleanup exists anywhere in this codebase. This predates disk selection entirely and isn't made worse by it; fixing it is a real, separate piece of work, not folded into this change (see task backlog).
- The web dashboard's container-creation form does not yet offer a disk picker — a real, acknowledged gap, left for a follow-on rather than expanding this change's own scope.
- No new automated test exercises the successful disk-placement path itself: this sandbox's own visible block devices (`/sys/class/block`) are the underlying Proxmox host's real hardware, not anything safe to format/mount for a test — the same "not practical to exercise deterministically in the sandboxed test harness" gap ADR-0099 already accepted for `GET /disks`. The validation/error paths (`resolve_container_disk_root()`'s three specific 400 cases) were verified by direct reasoning against `disk_enumerate()`/`diskrole_lookup()`'s existing, already-tested behavior, not a new end-to-end test.
- Full local regression sweep clean, zero compiler warnings.
