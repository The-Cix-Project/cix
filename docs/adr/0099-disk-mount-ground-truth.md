# 0099 — GET /disks reports real mount status from /proc/mounts, not job history

## Status

Accepted

## Context

Task #672: there was no REST-visible way to confirm a disk is actually mounted. `diskformat.c`'s own `DISKFORMAT_STATE_READY` (`GET /diskformat/{name}`) looked like it might answer this, but it doesn't: that state is purely in-memory, tracked per-daemon-process, for the single most recent (or currently running) format job this daemon itself started — `DISKFORMAT_STATE_NONE` means "no format job has ever run this daemon lifetime," which is true again after every restart even though a real mount from an earlier lifetime still persists. It also has nothing to say about a disk mounted by hand, or one that was already mounted before this mechanism existed at all. An operator asking "is this disk actually mounted right now" had no reliable answer from either endpoint.

## Decision

`GET /disks` now reports real, current ground truth read fresh from `/proc/mounts` on every call, independent of any job-state: `mounted` (bool) and `mount_path` (string, empty when not mounted). `disk_enumerate()` does one pass over `/proc/mounts` after building the disk list, matching each mounted device's own parent whole-disk name (via the same `disk_name_from_partition()` helper `resolve_os_disk_name()` already uses) against each entry, same "the kernel's own live state is the one source of truth" precedent `resolve_os_disk_name()` and `GET /system/routes` (ADR-0066) already established for this project.

## Consequences

- An operator (or the dashboard, once a Disks view exists) can now tell whether a disk is mounted without caring whether Kanxeo itself did the mounting, or has restarted since.
- No behavior change to `diskformat.c`'s own job-state tracking — it still answers a different, narrower question ("did my most recent format job succeed") and is left alone.
- Full local regression sweep clean, zero compiler warnings. No new dedicated test was added — `GET /disks` has never had one (real, live `/sys/class/block` enumeration isn't practical to exercise deterministically in the existing sandboxed test harness with no attachable disks); a real, pre-existing gap this change doesn't close but also doesn't worsen.
