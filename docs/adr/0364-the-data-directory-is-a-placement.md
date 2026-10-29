# 0364 — The data directory is a placement

## Status

Accepted by the owner on 2026-10-29 (proposed the same day), for #249. The owner decided on 2026-09-14 that the data directory should be relocatable and API-driven, reusing ADR-0141's placement machinery. This ADR is that decision's design, written after measuring what the directory actually carries.

## Context

`/var/lib/cix` is `g_base_dir`. On an installed host it is the `cix-containers` partition, which the installer sizes at up to 16 GiB (`AUTO_DATA_MAX_MIB`), mounted by label at boot (`boot_init()`). #249 was raised because that partition cannot be reclaimed: no migration frees its mount.

What it holds was unknown until `GET /v1/system/data-dir` (0.2.57-620). Measured on 192.168.15.95, 2026-10-29:

- **Before #664:** 3.99 GiB used, 3.78 GiB of it build slots' overlay trees from before ADR-0360, which nothing removed.
- **After #664 (0.2.57-622): 0.13 GiB used.** What is left, and who owns it:

| Entry | Bytes | Owner | Already movable? |
|---|---|---|---|
| `logs/` | ~0.09 GiB | log store | yes: `log-storage` placement (ADR-0141) |
| `volumes/` | ~1 MB | volumes created with no `disk` | per volume, by migrate to a `volume-storage` disk |
| `swap/` | 0 (off) | swap file | yes: the `swap` role (#28) |
| `containers/` | 0 | userns pivot mountpoints, empty directories | no |
| `diskroles.json`, `storage_placement.json` | bytes | disk roles, placements | no |

Two facts follow, and neither needs a decision to act on:

- **The two state files are here for a reason that no longer exists.** They were kept out of `STATE_DIR` because state-storage was a relocatable placement, and "which disk holds state" cannot be read from a file on that disk (ADR-0141 phase 2 correction, the comment in `main.c`). State-storage was retired in #251, and `STATE_DIR` is always `/config/state` now, which is mounted before either file is read.
- **A missing data partition is silent.** If `cix-containers` cannot be mounted as btrfs or ext4, `boot_init()` mounts a tmpfs at `/var/lib/cix` and carries on. Everything placed there then lives in memory until the next boot, with nothing saying so.

## Decision

1. **`diskroles.json` and `storage_placement.json` move to `STATE_DIR`.** At start, a file found at the old path and absent at the new one is moved once, then read from the new path. The data directory then holds no state the boot needs.
2. **A data directory that cannot be mounted is a reported fault, not a tmpfs.** Boot still mounts a tmpfs so the control plane comes up and can be reached, but the daemon records the fault, and `GET /v1/system/data-dir` and the dashboard's health show that the data directory is not persistent.
3. **The data directory is a placement, `data-storage`, moved like the others.** `POST /v1/system/data-dir/migrate {disk}` targets a disk carrying the `data-storage` role. Because a running container holds its pivot directory, the move happens at the next start, before anything runs: the request records the target in `/config`, the next boot copies `logs`, `volumes`, `swap` and `containers` onto the disk, mounts it at `/var/lib/cix`, and only then releases `cix-containers`. Until that boot completes, the source is untouched, and a failed copy leaves the host on the source with the fault reported. `cix-containers` is then an ordinary partition the operator can delete or reuse.

**Decision 3 as built (2026-10-29, before any code, after a design review).** The move carries less than the text above says, and is the same mechanism the other placements use:

- **It is a fourth placement kind, `data-storage`**, in `storageplacement.c` beside rebuildable, log and swap. It uses the same record (which decision 1 just moved to `/config`), the same degraded reporting (#256), and the same refusal to remove or format an active placement's disk. Its endpoints sit under `/system/data-storage`, and the CLI is `storage data`.
- **Volumes are not copied at boot.** A volume is a btrfs subvolume with a qgroup limit, and `POST /volumes/{name}/migrate` already moves one properly; a second subvolume copy in the boot sequence would be a parallel implementation. So a move request is refused (409, naming them) while any volume has no `disk`, or while swap is enabled on the default placement. The operator moves those first with the mechanisms that exist.
- **What the boot copies is the log store, as plain files.** `containers/` holds only pivot directories, which every create makes again, and `.discard` is skipped. The copy therefore takes as long as the log store's size, which `max_bytes` bounds.
- **The target is named by filesystem UUID**, resolved through `disk_enumerate()`, because kernel names move (#256). It must be btrfs.
- **Boot reads the record from `/config`, so `/config` is mounted before the data directory.** That reorders `boot_init()`. If an active target is missing at boot, the daemon mounts `cix-containers` and reports the placement degraded, or uses decision 2's tmpfs if that is missing too. A pending move whose copy fails stays on the source, with the reason recorded.
- **Rebuildable storage is a precondition too** (added when stage b was built). On a host where it is on its default placement, `rebuildable/` (images, packages, artifacts) is inside the data directory, and a move that carried only the log store would boot with an empty image store. So the request is refused while rebuildable storage, any volume with no disk, or an enabled swap file on the default placement is still there, and the 409 names which.
- **The request records a pending move with three states**: none, a target disk (with its UUID), or back to `cix-containers`, which is not the same as none. The boot mounts both ends at `/mnt/cix-data-move/{source,target}`, never at the data directory, so whichever wins is mounted there afterwards. It then writes the new placement, the cleared pending move and the outcome in one record, so a failed move is not retried by itself. `GET /system/data-storage` reports `pending` and `last_move` (`moved` or `failed`, with the reason); `degraded` keeps its meaning, an active placement not honoured. A target that already has a `logs/` (a move back) has it renamed `logs.replaced-<time>`, never merged or deleted.
- **Staged:** first the mount order, UUID resolution, the kind and its read, with no placement set, which changes nothing live; then the request and the at-boot move; then the move back. Each is verified by rebooting 192.168.15.95, which needs a disk with no role.

## Alternatives considered

- **Retire the data directory instead of moving it.** Re-home each resident permanently: pivots under rebuildable storage, volumes required to name a disk, the log store and swap always on placed disks. It frees the partition with no move at all, but it removes the "works on one disk" default an install has today, which every placement currently falls back to.
- **Only shrink the partition.** btrfs grows and shrinks online, so the installer could size it small and `storage grow-partition` exists. It recovers space without a mechanism, and leaves #249's actual complaint, that the partition cannot be freed, standing.
- **Leave it.** After #664 the partition is 99 % free. That costs an operator 16 GiB they cannot assign to anything else.

## Consequences

- Decisions 1 and 2 are small, safe and needed under every alternative. They can ship first.
- Decision 3 is the only part that moves the ground the daemon stands on, and it does so only at boot, before anything runs, with the source kept until the target is mounted. It needs the `init_mode` boot path, which no test exercises (it is verified live on 192.168.15.95), and a copy of volumes, which are irreplaceable.
- With decision 3, #462 (container-storage pooling) has nothing left to pool: container rootfs are snapshots on rebuildable storage since ADR-0360.
