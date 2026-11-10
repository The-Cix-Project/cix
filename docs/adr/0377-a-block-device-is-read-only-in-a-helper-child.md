# 0377 — A block device is read only in a helper child, and every disk read is answered from one snapshot

## Status

Accepted by the owner on 2026-11-09. Asked to choose between parking `GET /v1/storage`, `/v1/storage/quota` and `/v1/volumes` in a helper child and keeping one disk snapshot that every caller reads, the owner chose the snapshot. Issue [#675](https://git.home.arpa/itdlabs/cix/issues/675), which [ADR-0376](0376-the-self-check-reads-in-a-helper-child.md) left open.

- It applies [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md), the loop's blocking budget, and [ADR-0278](0278-the-reactor-forks-work-it-cannot-afford-to-wait-for.md)'s `helper_run()`.
- It extends to every disk read what [#100](https://git.home.arpa/itdlabs/cix/issues/100) did for one caller (`disk_currently_present()` in `diskrole.c`).
- It changes `GET /v1/storage`'s contract: an identity or a capacity not known yet is `null`, never `""` or `0`.
- It also moves the artifact cache's walk into the same child, so `GET /v1/pkg/cache` and `GET /v1/pkg`'s `artifact_cached` answer from the snapshot.

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

1. **Once the loop serves, a block device is read only in a helper child.** Superblock probes, GPT label reads and `statvfs` of a mounted data filesystem happen in `disksnap_work()`, the snapshot's helper. Nowhere else after start: no handler, no write path, no grant check. Part 1 moved the device walk, and part 2 the direct single-device reads (see the end); what is left is one read on the boot path, before the loop serves.

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
   - **In a request that needs a disk's identity to decide:** unknown is a 409 with `Retry-After`, naming the disk as still being probed. The partition resize and the data-storage move are the two.
   - **In a write that records a disk's UUID** (a storage placement, a pending move's target, a role after a format): the UUID is taken from the snapshot if it is known. If it is not, it is left empty, and the first snapshot install that knows the disk adopts it (`diskrole_adopt_uuids()`, `storageplacement_adopt_uuids()`). This is #255's adoption at boot, run after every install. A 409 was the first plan and is wrong here. These writes are often not requests at all: a format completes on its own, and a move ends when its copy does. A format has also just made its disk unknown, so the write would refuse every time. An empty UUID is never recorded as the disk's identity. It means only "not adopted yet", and resolution falls back to the name, exactly as it did for a role made before #255.

5. **Every device has a change counter, and it alone decides whether a probe result may be installed.** It is bumped by `disksnap_touch()` whenever something changes, or may have changed, what a device holds:
   - **A format** touches its disk when `mkfs` starts and again when it ends, because it runs asynchronously while the loop goes on serving.
   - **A partition write** (a new table, or adding, resizing or deleting a partition) touches its disk, and the partition it named, once, after the write. The write runs synchronously on the loop, so no reader on the loop can see it half done. A probe child forked before or during it is discarded by the counter that moved.
   - **A kernel block uevent** (`add`, `remove` or `change`) touches the device and, for a partition, its disk.

   Bumping the counter also makes the entry unknown at once. A touch that turns a known entry unknown is announced (item 9), because every read built from the snapshot now answers differently. A probe result is installed only if its device's counter is the same as when the child was forked. That is what stops a periodic probe from installing a superblock it read halfway through a format. The rule is one pure function, `disksnap_result_installable()`, with its own unit test.

6. **A refresh asked for while one is running is coalesced, never dropped.** This differs from ADR-0376's rolling check, which skips. A self-check that is skipped loses nothing, because the next one compares the same state. A refresh is asked for because something changed, so skipping it would leave an entry unknown until the next periodic one. So there is one child at a time, plus an `again` flag. The child that is running finishes, and exactly one more starts.

7. **When the snapshot refreshes.**
   - After every counter bump.
   - Every 30 s, the period `listStorage` already declares. This keeps capacity current, and it bounds how stale identity can be after a write nobody announces. The case that exists is a container writing a block device it was granted. Reading that device's superblock while it is held is read-only and harmless.
   - At start, in-process and before the loop serves (`disksnap_build_now()`). This is ADR-0376's argument for the baseline: no build is running then, a handover waits until none is in flight, and boot's role mounts need identity before the loop exists.

8. **A format is finished when its device is known again.** A format job reports `ready` only after the probe that follows it has been installed. A client that waits for the format and then reads `GET /v1/storage` therefore sees the new filesystem, never "probing".

   This has a bound, stated rather than hidden: a format whose disk leaves before the re-probe reports `running` until the disk is seen again, and so does a format whose refresh child cannot be started. In both cases the device's `probed_at` is `null`, which tells them apart from a format still writing.

   A partition write returns when `sfdisk` does. The partitions it touched read as unknown until their probe is installed, which announces (item 9). A client reading in between sees `null`, never the old answer.

9. **A change to the snapshot announces every read made from it**, through one setter, `disksnap_announce()`. The reads are `listStorage`, `getStorageQuota`, `listDevices`, `listDeviceMaps`, `listVolumes` and `getStorageFormatStatus`. There are two such changes:
   - an install that changed anything a client sees;
   - a touch that made a known device unknown.

   The second matters for a kernel uevent, which has no request for `dispatch()` to announce. ADR-0339 says the daemon's own state changes announce. A refresh that changed nothing announces nothing.

   `listStorage` and `getStorageQuota` are samples, so a capacity that moves between refreshes is announced by being observed to change. It is not a volatile field.

10. **The artifact cache's walk joins the snapshot.** The same child that probes the disks also walks the artifact cache: one `readdir` and one `stat` per file, the walk `cache_scan()` does today. `GET /v1/pkg/cache` and each package's `artifact_cached` in `GET /v1/pkg` answer from that.

    The LRU eviction moves into the child too. When the walk finds the cache over its maximum, the child unlinks the oldest files until it fits, and reports what is left. It is no longer a walk on the loop before every store.

    **An in-memory index kept at each write was considered and rejected.** The cache has more than one writer: `pkg_cache_save_from_file()`, the export child that writes a hostbuild's artifact straight into the directory, and the signatures beside the artifacts. An index that missed one writer would be wrong silently, and for as long as cixd ran. The walk cannot miss a writer. At worst a write nobody announced is seen one period late.

    `pkg_announce_cache()`, which every writer already calls, now asks for a refresh. The install that follows announces `getPkgCache` and `listPkgs` if the walk changed anything. So the announcement always comes after the state it describes.

    **Decisions still `stat` the one file they ask about.** Whether this version is cached (`pkg_cache_has()`) decides whether a build runs, and must be true at that moment. One `stat` of one path is not a walk.

11. **Gates.**
    - `test_blocking_waits` counts the lines naming `disk_enumerate_probe(`, the one door to the walk that reads devices: the child and the start-up build.
    - It counts `disk_probe_fs_type(` and `disk_probe_fs_uuid(`, one device's superblock read directly, per file. Part 1 budgeted them at what was there; part 2 lowered them to the one boot-path read, a lowering visible in the diff.
    - It counts `cache_scan(` and `pkg_cache_walk_evict(` the same way: the definitions, the child and the start-up build.
    - `test_disksnap` drives the install rule, touches, replaced and departed devices, and the announcement flag.

## Built in part 1 and part 2

**Part 1** (0.2.57-669):
- the snapshot and its helper child;
- `disk_enumerate()` reading identity and capacity from it;
- touches from formats, partition writes and kernel block uevents;
- the 30 s period and the start-up build;
- `null`, `probed_at` and a format reporting `running` until its disk is known;
- the 409 on growing a partition of unknown identity;
- the dashboard's "probing" and the gates;
- the artifact cache's walk and eviction in the same child (item 10), which `GET /v1/pkg/cache` and `artifact_cached` answer from.

**Part 2** (0.2.57-670):
- **The direct superblock reads on the loop are gone.**
  - The data-storage move reads the disk through `disk_identity_of()`, and is a 409 while the disk is unknown.
  - Storage placement and role writes record a UUID from the snapshot, or let a later install adopt it (item 4).
  - Role resolution by UUID reads the UUID `disk_enumerate()` already holds.
  - What remains is `data_disk_device()`, called only by `mount_data_dir()` at boot, before the loop serves. Part 1's gate budgeted nine direct reads in four files. Part 2 leaves one, on that boot path.
- **The release of a block device grant touches the device.** It happens in `registry_mark_exited()`, because a container may have rewritten what it held.
- **A mount, an unmount and a swap change ask for a refresh (`disksnap_want()`), and do not touch.** None of them changes identity, so a touch would make the device unknown, and a 409 possible, for nothing. What they do change is capacity, which `disksnap_fill()` already reports as unknown for a device mounted since its probe, and which the refresh they ask for reads.

## Out of scope

- **`GET /v1/system/stats` runs `statvfs` on the data directory's own filesystem** (`handle_system_stats()`). So does the build's disk-pressure note in `pkg.c`. That is one filesystem, the OS disk's state partition, and neither has shown in a stall record. They stay as they are, and they are named here so a later stall in them is recognised.
- **Partition writes still wait for `sfdisk` on the loop.** Those waits are already budgeted in `test_blocking_waits`. Moving them is its own decision.

## Consequences

- No API read waits on a block device or a busy filesystem. A sample of `listStorage` costs a copy from memory.
- A caller deciding by identity can be told "not known yet" for the length of one probe after a change. Item 4 says what each one does with that.
- Capacity is up to 30 s old, and `probed_at` says how old.
- A device a container holds and rewrites shows its new identity within one period.
