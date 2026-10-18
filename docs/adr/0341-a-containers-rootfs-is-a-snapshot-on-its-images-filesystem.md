# 0341 — A container's rootfs is a snapshot of its image, on the image's filesystem

## Status

Proposed, 2026-10-18, for the owner's decision. Issue [#608](https://git.home.arpa/itdlabs/cix/issues/608).

The first draft of this ADR moved the copy off the event loop. The owner then asked what the ideal would be on a box with hundreds of containers. At that scale, a roll of one image followed by 100 containers would be 100 full copies and 100 full trees, every time. The question is whether the copy should exist at all, not how to schedule it, so the draft was replaced before any decision on it. It is the ADR-0332 → ADR-0333 lesson again.

It refines [ADR-0207](0207-btrfs-storage-substrate-userns-by-default.md) (btrfs storage, one writable snapshot per container) and [ADR-0277](0277-a-container-rootfs-is-derived-from-its-image.md) (a rootfs is derived from its image and re-seeded when the pin moves). It changes where a container's rootfs lives, which is hard to reverse, and it changes what a disk's `container-storage` role holds. That makes it the owner's decision.

## Context

**The stall.** cixd is one event loop. Measured on 192.168.15.95 (0.2.57-539), 2026-10-18: both of the day's `event container` stalls sit between two log lines of one container create, as `jump` rolled onto a new `jumpbox` image version.

| `rebuilding it from the new version` | `rolled to image version` | stall record |
|---|---|---|
| 09:00:42Z | 09:00:46Z | `event container (4504 ms)` |
| 16:00:36Z | 16:00:41Z | `event container (5061 ms)`, 3 slow passes |

Inside both windows the kernel logs `BTRFS info (device vdb5): Snapshot src from another FS`. That is `BTRFS_IOC_SNAP_CREATE_V2` refusing with EXDEV. For a userns container, that refusal sends the create to ADR-0179's phase-2b presentation, on the loop: `cix_tree_copy()` of the whole image, then `chown_tree()` to the subordinate base.

**The storage model ADR-0207 designed does not run on the reference box.** ADR-0207 phase 3 gives every container one writable btrfs snapshot of its image version, presented through an id-mapped mount. That is O(1), with no copy and no chown, and the extents are shared with the image. A snapshot cannot cross a filesystem. On .95, though:

- **Image versions** are subvolumes under `REBUILDABLE_DIR`. `image_produce_new_version()` snapshots each version forward from the last. `GET /v1/storage-roles` gives `sda` the `rebuildable-storage` role, so that is where the image store lives.
- **Container bases** are under `CONTAINERS_DIR` on the OS disk (`vdb5`). Every container checked on .95 is `"disk":null`. `vda` holds the `container-storage` role and carries none of them.

So every create falls back to something:

- **The 10 userns containers** copy and chown the whole image, silently.
- **The 2 non-userns ones** fall back to overlay, and say so at every create: `container ntp-1: could not snapshot the image rootfs (Invalid cross-device link) -- falling back to the overlay path`. The log holds that pair at every one of their creates it was asked for, ten since 2026-10-17.

**Overlay cannot replace the copy for userns.** `probe-ovlidmap@1-1` read kernel 7.2.11's source on the box. `ovl_fs_type` sets only `FS_USERNS_MOUNT`, so the overlay mount cannot be id-mapped; its layers can be. ADR-0179 built id-mapped layers on 6.18 and measured why they fail with a persistent upper on a host filesystem. Mounted from outside the userns, `work/` creation is denied. Mounted from inside it, the setup needs privilege over the layer filesystem's init `s_user_ns`. Nothing in 7.2.11 changes either.

**The scale question decides the shape.** On a host with hundreds of containers, any design that copies per container is a disk-churn scheduler, however it is scheduled. A snapshot of 100 containers onto one new image version is 100 ioctls. Discarding the old trees is 100 `SNAP_DESTROY`s, whose extent freeing the kernel's cleaner thread does in the background. That is what a btrfs-backed container store is for, and it is why other btrfs container systems keep image and container subvolumes on one filesystem.

## Decision

1. **A container's rootfs is a btrfs snapshot of its image version, on the image's filesystem.** It lives beside the image store, under `REBUILDABLE_DIR`, wherever that is mounted. A roll is one `SNAP_CREATE_V2` plus the id-mapped mount that already exists, for userns and non-userns alike. A discard is one `SNAP_DESTROY`. Nothing in a create copies or chowns a tree.

   This is what ADR-0277 already says a rootfs is: derived from its image, disposable at every pin move, with anything that must outlive an image version kept on a volume. A derived tree is rebuildable state, and it belongs with the rest of the rebuildable state.

2. **What stays where it is.** The container's base under `CONTAINERS_DIR` keeps everything that is not derived from the image: its seed marker, its staged identity files, its subordinate-id record and its volumes. `disk: NAME` and the `container-storage` role go on deciding where those live. **This is the contract change the owner decides:** today `disk` also places the rootfs, and under this ADR it does not.

3. **The rootfs quota is the snapshot's exclusive qgroup limit**, as `disk_quota_bytes` already is on the snapshot path. That limit now measures what it should: the container's divergence from its image.

4. **The overlay and copy fallbacks remain only where btrfs is absent.** On an ext4 host, `cix_btrfs_is_backing()` is false and ADR-0179's copy is still the only presentation a userns container has. That copy goes into a `helper_run()` child, so it no longer holds the loop. Whether ext4 hosts keep being supported at all is ADR-0207's question, not this one's.

5. **Moving the existing trees is one discard each, off the loop.** A container's current copied tree is a plain directory on the OS disk. At its next pin move it is renamed aside, which is O(1), and deleted by a helper child, and the new rootfs is a snapshot under (1). Nothing is migrated in bulk, and the box never holds the loop for the move.

6. **The silence is fixed too.** The userns branch's EXDEV fallback logs nothing, while the non-userns one logs at every create. Any fallback that remains logs, and so does the false comment in `src/btrfs.c`, which says the create path "falls back to the overlay rootfs, which ... costs nothing" — true of one branch only. And `test_blocking_waits` budgets `cix_tree_copy(` and `chown_tree(` per file, so a copy cannot drift back onto the loop.

## Open before implementation

- **qgroup accounting at scale.** Cix enables full qgroups (`CIX_BTRFS_QUOTA_CTL_ENABLE`, `src/btrfs.c`). From memory, not measured here: full qgroup accounting gets more expensive as snapshots multiply and are deleted often, and btrfs's simple quotas exist for that case. `include/btrfs.h` already recognises `CIX_BTRFS_QGROUP_SIMPLE`. Before hundreds of snapshots roll hourly, a probe should establish what 7.2.11 offers and what a `SNAP_DESTROY` costs under each mode.
- **The rebuildable disk becomes load-bearing for running containers.** Today losing `sda` costs a rebuild of images and packages. Under (1), it also stops every container until each is re-seeded. Today a non-userns container already needs it, because its overlay lowerdir is there, while a userns copy does not. That is a real change in failure domain, so it is stated here rather than discovered.

## Consequences

- No create copies or chowns. A roll of `jump` drops from 4–5 s of loop time to an ioctl, and 100 containers on one image roll for the price of 100 ioctls.
- Disk use on a busy host becomes one copy of each image version plus each container's divergence, rather than a full tree per container.
- The storage model ADR-0207 designed runs on the reference box for the first time, for every container, userns or not.
- `disk` and the `container-storage` role stop placing the rootfs (Decision 2). The API, the CLI and the dashboard say so, in the same change.

## Alternatives considered

- **Seed off the loop and keep the copy** (this ADR's first draft). It fixes the loop and keeps the copies: N full trees and N copies per roll. Replaced for that reason.
- **One replica of each image version per container filesystem.** It keeps `disk` placing the rootfs, at the cost of a second image store per filesystem: one copy per version per filesystem (`cix_btrfs_subvol_copy()`, incremental with `btrfs send -p` once versions share lineage), plus garbage collection and consistency for each replica. It is a per-filesystem copy of the image store, against One Source of Truth. It is the right choice only if the owner names something Decision 1 cannot give.
- **Overlay with id-mapped layers for userns.** Measured unworkable with a persistent host upper (ADR-0179), and kernel 7.2.11 changes neither reason (Context).
- **Mounting the overlay with a mapped creator credential.** Not measured anywhere, and it would make a security-sensitive credential arrangement part of every container mount.
- **Requiring images and containers on one disk, as a layout rule.** It moves the decision to every operator and brings the stall back on any host laid out otherwise. Decision 1 makes the placement follow from the image, so no layout can break it.
