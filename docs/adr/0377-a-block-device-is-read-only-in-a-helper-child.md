# 0377 — A block device is read only in a helper child, and every disk read is answered from one snapshot

## Status

Accepted by the owner on 2026-11-09. Asked to choose between parking `GET /v1/storage`, `/v1/storage/quota` and `/v1/volumes` in a helper child and keeping one disk snapshot that every caller reads, the owner chose the snapshot. Issue [#675](https://git.home.arpa/itdlabs/cix/issues/675), which [ADR-0376](0376-the-self-check-reads-in-a-helper-child.md) left open.

- It applies [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md), the loop's blocking budget, and [ADR-0278](0278-the-reactor-forks-work-it-cannot-afford-to-wait-for.md)'s `helper_run()`.
- It extends to every disk read what [#100](https://git.home.arpa/itdlabs/cix/issues/100) did for one caller (`disk_currently_present()` in `diskrole.c`).
- It changes `GET /v1/storage`'s contract: an identity or a capacity not known yet is `null`, never `""` or `0`.
- It also makes `GET /v1/pkg/cache` and `GET /v1/pkg`'s `artifact_cached` answer from memory.

## Context

`disk_enumerate()` (`daemon/src/disk.c`) is how cixd learns about block devices, and it is called on the event loop. For every whole disk and partition under `/sys/class/block` it reads:

- **sysfs**: name, size, partition-ness, start sector, model, removable, and I/O counters. These come from kernel memory, with no block I/O.
- **`/proc/mounts`**: which device is mounted where, and which disk holds the OS. Also kernel memory.
- **Identity**: `probe_fs()` opens `/dev/<name>` and `pread()`s its superblock for the filesystem type and UUID, and `partlabel_read()` reads the parent disk's GPT for the partition's label. This is block I/O, once per device per call.
- **Capacity**: `fill_capacity()` runs `statvfs()` on every mounted filesystem. On a busy btrfs this can wait for the filesystem.

About 25 call sites run it on the loop. They include `GET /v1/storage`, `/v1/storage/quota`, `/v1/devices` (each disk is a device record, ADR-0344), `/v1/volumes` (once per volume placed on a disk, in `volume_host_path()`), `/v1/storage-roles`, container create with a volume, every device grant decision, swap and backup placement. Several modules also call `disk_probe_fs_uuid()` directly (`diskrole.c`, `storageplacement.c`, `main.c`).

`listStorage` and `getStorageQuota` are declared `x-cix-read: sample` with a 30 s period. ADR-0376 deliberately left samples on the loop. So every 30 s the self-check, and every dashboard subscribed to the storage page, ran the whole enumeration in-process.

Measured on 192.168.15.95 (`GET /v1/system/stalls`), every slow pass came while a build or an assembly was running:

| when | what held the loop | time |
|---|---|---|
| ts 1793915024 | `GET /v1/storage` | 5240 ms |
| ts 1793896968, 1793760163 | `GET /v1/volumes` | 1173, 778 ms |
| ts 1793415971 | `GET /v1/storage/quota` | 795 ms |
| #100 | `GET /v1/storage-roles`, in `blk_execute_rq` | ~6 s |
| ts 1793934027 | `GET /v1/pkg/cache`, in D state on `wait_current_trans` | 9410 ms |

The last row is the artifact cache: one `readdir` of the cache directory and one `stat` per artifact. Its stall record shows cixd waiting for a btrfs transaction to commit, not for a disk read.

Which step inside `disk_enumerate()` costs the time is **not measured**: the superblock read, the GPT read and `statvfs` are each plausible under a busy disk. This decision does not depend on the answer, because all three move.

## Decision

1. **Once the loop serves, a block device is read only in a helper child.** Superblock probes, GPT label reads and `statvfs` of a mounted data filesystem happen in `disksnap_work()`, the snapshot's helper. Nowhere else after start: no handler, no write path, no grant check.

2. **One snapshot holds what that child read.** For every device, keyed by name, it holds:
   - identity: filesystem type, filesystem UUID and partition label;
   - for a mounted filesystem, its capacity (used and free bytes);
   - when it was probed.

   It is the one source of those facts.

3. **`disk_enumerate()` keeps its signature, and the ~25 callers do not change.** Topology and mount status are still read live from sysfs and `/proc/mounts`, because they cost no I/O and must be current for safety decisions: is this mounted, is this the OS disk. Identity and capacity are copied from the snapshot. Those fields become a known/unknown pair (`identity_known`, `capacity_known`), and an entry with no current probe is unknown.

4. **Unknown is explicit, in the API and in every decision.**
   - **In the contract:** `GET /v1/storage` reports an unknown `fs_type`, `fs_uuid`, `part_label`, `used_bytes` or `free_bytes` as `null`, and adds `probed_at`, the time of the probe the identity came from. `""` keeps meaning "probed, and there is none". Contract and README change together.
   - **In the dashboard:** the storage view renders null as "probing", never as "no filesystem".
   - **In a caller that decides by identity:** unknown never matches. That covers role resolution by UUID (`diskrole_resolve_recorded()`), the ADR-0345 `fs_uuid` selector, storage placement by UUID, and the quota report's btrfs filter.
   - **In a write that records a disk's identity** (storage placement, a role's filesystem type): unknown is a 409 naming the disk as still being probed, so the request can be retried. It never records an empty UUID.

5. **Every device has a change counter, and it alone decides whether a probe result may be installed.** The counter is bumped by:
   - every cixd operation that changes a device: a format, a partition-table write, a partition add, delete or resize, a mount or unmount, a swap on or off. Each operation bumps it both before it starts and after it ends;
   - a kernel block uevent (`add`, `remove` or `change`), for the device, its parent and its partitions;
   - the release of a block device grant.

   Bumping the counter also makes the entry unknown at once. A probe result is installed only if its device's counter is the same as when the child was forked. That is what stops a periodic probe from installing a superblock it read halfway through a format. The rule is one pure function, `disksnap_result_installable()`, with its own unit test.

6. **A refresh asked for while one is running is coalesced, never dropped.** This differs from ADR-0376's rolling check, which skips. A self-check that is skipped loses nothing, because the next one compares the same state. A refresh is asked for because something changed, so skipping it would leave an entry unknown until the next periodic one. So there is one child at a time, plus an `again` flag. The child that is running finishes, and exactly one more starts.

7. **When the snapshot refreshes.**
   - After every counter bump.
   - Every 30 s, the period `listStorage` already declares. This keeps capacity current, and it bounds how stale identity can be after a write nobody announces. The case that exists is a container writing a block device it was granted. Reading that device's superblock while it is held is read-only and harmless.
   - At start, in-process and before the loop serves (`disksnap_build_now()`). This is ADR-0376's argument for the baseline: no build is running then, a handover waits until none is in flight, and boot's role mounts need identity before the loop exists.

8. **A format is finished when its device is known again.** A format job reports `ready` only after the probe that follows it has been installed. A client that waits for the format and then reads `GET /v1/storage` therefore sees the new filesystem, never "probing".

   A partition write returns when `sfdisk` does. The partitions it touched read as unknown until their probe is installed, which announces (item 9). A client reading in between sees `null`, never the old answer.

9. **Installing a snapshot that changed anything announces every read made from it**, through one setter: `listStorage`, `getStorageQuota`, `listDevices`, `listDeviceMaps` and `listVolumes`. ADR-0339 says the daemon's own state changes announce. A refresh that changed nothing announces nothing. Capacity moves on every refresh under write load, so used and free bytes are `x-cix-volatile-fields` of `listStorage` and `getStorageQuota`, as the session expiry is of the session list.

10. **The artifact cache's total is kept in memory**, and the same applies to whether a given artifact is cached.
    - cixd reads the cache directory once, in-process before serving.
    - From then on it updates one in-memory index at every write: storing an artifact (`pkg_cache_save_from_file()`), the completion of an export child that wrote one, an eviction, a clear, and the start-up retirement of `.tar.gz` files.
    - `GET /v1/pkg/cache`, the LRU eviction order, and each package's `artifact_cached` in `GET /v1/pkg` read the index.
    - Nothing else writes the directory, and the gate counts the paths that open it.

11. **Gates.**
    - `test_blocking_waits` counts the call sites of `probe_fs(`, `partlabel_read(`, `disk_probe_fs_type(`, `disk_probe_fs_uuid(` and `fill_capacity(` in `daemon/src`. They are allowed only in the child's work function and in start-up code that runs before the loop.
    - It counts `opendir(g_cache_dir)` the same way, allowing only the start-up read and a clear.

## Out of scope

- **`GET /v1/system/stats` runs `statvfs` on the data directory's own filesystem** (`handle_system_stats()`). So does the build's disk-pressure note in `pkg.c`. That is one filesystem, the OS disk's state partition, and neither has shown in a stall record. They stay as they are, and they are named here so a later stall in them is recognised.
- **Partition writes still wait for `sfdisk` on the loop.** Those waits are already budgeted in `test_blocking_waits`. Moving them is its own decision.

## Consequences

- No API read waits on a block device or a busy filesystem. A sample of `listStorage` costs a copy from memory.
- A caller deciding by identity can be told "not known yet" for the length of one probe after a change. Item 4 says what each one does with that.
- Capacity is up to 30 s old, and `probed_at` says how old.
- A device a container holds and rewrites shows its new identity within one period.
