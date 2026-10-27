# 0360 — The container store is btrfs, and build containers are snapshots too

## Status

Accepted by the owner on 2026-10-27 (proposed the same day), for #615, #614, #161 and #157. It carries out the owner's decisions of 2026-10-27: btrfs is required for the image and container store, and the OverlayFS and copy fallbacks go (*"btrfs required for the store"*); a volume keeps its disk, through a role renamed `volume-storage` (*"Rename to volume-storage"*); and the work is done as one effort (*"All at once"*), delivered in the two releases that Delivery explains. It completes [ADR-0207](0207-btrfs-storage-substrate-userns-by-default.md) phase 4. It supersedes the OverlayFS lowerdir of [ADR-0004](0004-overlayfs-dedicated-lowerdir.md), the per-container disk of [ADR-0102](0102-per-container-disk-selection.md), the container half of [ADR-0234](0234-snapshot-containers-migrate-across-disks.md), the overlay quota backends of [ADR-0062](0062-ext4-project-disk-quotas.md) and [ADR-0103](0103-btrfs-quota-backend.md), and the copied userns rootfs of [ADR-0179](0179-user-namespaces-by-default-subordinate-id-allocation.md) phase 2b.

## Context

[ADR-0341](0341-a-containers-rootfs-is-a-snapshot-on-its-images-filesystem.md) made an ordinary container's rootfs a btrfs snapshot of its image version, beside the image store. What is left of the older storage model was measured on 192.168.15.95 on 2026-10-27, at 0.2.57-611:

- **Every build container is OverlayFS.** `probe-buildfs@1-1` read `/proc/mounts` inside one: `overlay / overlay rw,relatime,lowerdir=/mnt/cix/sda/rebuildable/images/__buildenv-…/rootfs,upperdir=/var/lib/cix/containers/__pkgbuild-0/upper,workdir=/var/lib/cix/containers/__pkgbuild-0/work`. `stat -f` gives `/`, `/build` and `/build/cbsws/build` as overlayfs and `/run`, `/tmp` as tmpfs. The lower layer is on sda and the upper layer on vdb5, so no snapshot is possible across them. `start_build_container_spec()` (`daemon/src/pkg.c`) always fills `spec.ov.lowerdir/upperdir/workdir/merged`.
- **Build containers have no user namespace.** `start_build_container_spec()` clones `CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWCGROUP`, with no `CLONE_NEWUSER`, and the build reports `uid=0`. A recipe may ask for capabilities, and the cix recipe asks for `CAP_SYS_ADMIN`.
- **Every daemon test in the release gate runs on the fallback.** A test data directory is `mkdtemp` under `/run` (`/run/cix_test_data_…` in the gate's logs), a tmpfs inside a build container, so the containers those tests create take the OverlayFS or copy path. That path is exercised there and in production by build containers alone.
- **A container's `disk` places almost nothing.** On a btrfs host a container's base holds an overlay container's `upper` and `work` and an empty pivot mountpoint (#615). On .95 every container has `"disk": null`.
- **Volumes are the one real consumer of `container-storage`.** [ADR-0183](0183-persistent-volumes.md) places a volume through that role (`volume_host_path()` mirrors `container_root_for()`), and `volume migrate` moves one.
- **Every platform filesystem on .95 is already btrfs.** `GET /v1/storage` reports sda, vda, vdb4 and vdb5 as btrfs (#157, #161), and the installer formats btrfs.

## Decision

1. **The image store is btrfs, required.** Rebuildable storage, which holds the images, cannot be placed, migrated or formatted onto anything else, and a refusal says why. A host whose image store is not btrfs runs no container, and its read says so rather than falling back.
2. **Every container's rootfs is a snapshot of its image version, build containers included.** A build container's rootfs is a snapshot of its build environment's image version, at `container_rootfs_dir()` under its own name, and it pivots onto it directly (`direct_rootfs`). What used to be staged into the upper layer (the recipe, the sources, the cbs workspace, the finalize policy) is staged into the snapshot. A kept build container (#46) keeps its snapshot. A slot's previous tree leaves through `rootfs_discard()`, as #626 made it.
3. **OverlayFS and the copy path are removed.** That covers `src/overlay.c` and the overlay half of `struct overlay_spec`, the copied userns rootfs (a userns container is the id-mapped snapshot of ADR-0207 phase 3), and the ext4 project-quota and upper-layer qgroup backends. It also covers the test hooks that forced the copy path, `--test-direct-rootfs` and `--test-userns-idmap`. A container quota is ADR-0359's qgroup limit on the snapshot.
4. **The release gate runs on btrfs.** A test makes its data directory under `$TMPDIR`. The cix release recipe and the compile probe point `TMPDIR` at a directory on the build container's own root, which by decision 2 is a btrfs snapshot. The test daemons then create, snapshot and delete subvolumes there, as root with `CAP_SYS_ADMIN`. That is not yet measured inside a build container, and it cannot be measured until decision 2 is running on the box, so the work is two releases (see Delivery). If the measurement fails, the finding comes back to the owner before anything is removed.
5. **A container has no `disk`, and there is no `migrate-storage`.** `POST /v1/containers` refuses `disk` with a 400 naming this ADR. `POST /v1/containers/{name}/migrate-storage` is removed with its CLI and dashboard surfaces.
6. **`container-storage` becomes `volume-storage`.** It places volumes and nothing else. A stored assignment of the old name is renamed at start, logged once. `volume create --disk` and `volume migrate` keep working.
7. **ext4 stays a choice for an attached disk that does not hold the image store**: a volume, backup, log or swap disk. That is where the owner left it.

## Delivery

**Two releases, because the gate cannot prove its own move.** The release selftest runs inside a build container made by the daemon already running on the box, not by the code being built. At 0.2.57-611 that container's root is OverlayFS and its `/run` and `/tmp` are tmpfs (`probe-buildfs@1-1`), and the container is sealed, so no btrfs is reachable from inside it. A single release carrying all seven decisions would gate itself in a 611-shaped container, its `$TMPDIR` would not be btrfs, and with the fallback removed every daemon test would fail. The owner chose one effort over staged releases; this is the measured constraint that splits it, not a preference.

- **Release A: decision 2 only.** Build containers become snapshots, the fallback stays, and the gate is unchanged. Half of decision 2 is gated before deployment, by `test_installer`. Its QEMU guest's containers partition is btrfs, so the guest's first-boot glibc install takes the snapshot path for a build slot's prep and install. Compile probe 0.2.57-639 failed exactly there (#657). That install is a cache hit, though, and a cache hit never spawns its container (main.c's `pkg_chain_is_cache_hit()` branch drives completion directly). So the spawn onto a snapshot is gated by nothing before deployment. It failed on .95 at 0.2.57-612 (`cix-init setup failed: failed to stage /sbin/cix-init: Read-only file system`, #657), and the deployed probe below is what found it. Decision 4 is not reachable by the gate either. After release A is deployed, a probe proves that subvolume create, snapshot and destroy work inside a build container, as root with `CAP_SYS_ADMIN`. It did, on 0.2.57-613 (`probe-buildfs@3-1`, ROADMAP Part 342). Build start time was to be measured against 611, but the log store's one-second resolution gave no comparable figure.
- **Release B: everything else.** It carries decisions 1 and 3 to 7. The gate moves to `$TMPDIR`, and the overlay path, the copy path, the test hooks, `disk` and `migrate-storage` are removed. The role is renamed.

**The copy path is dead in production, so removing it breaks nothing running.** Measured on .95 on 2026-10-27: 10 of the 12 containers are userns and 2 (`ntp-1`, `ntp-2`) are not. On a btrfs `CONTAINER_ROOTFS_DIR`, `container_create()` snapshots a userns rootfs and id-maps it (`spec.userns_idmap = 1`, `daemon/src/main.c`, ADR-0207 phase 3, `MOUNT_ATTR_IDMAP` in `src/container.c`). It copies only when that snapshot fails, and then logs `could not snapshot the image rootfs ... copying it instead`. `GET /v1/system/logs?regex=copying it instead` returns `[]` across the whole retained store.

## Alternatives

- **Keep the fallback.** The owner decided against it. It is also the path the release gate runs, so it is the path that ends up proved, while production containers take another.
- **A test-only copy path.** That keeps the gate green by proving a path no host runs, which is the parallel implementation this project does not keep.
- **Staged releases.** Offered first and declined by the owner, who chose one effort.

## Consequences

- A build container costs one snapshot create and one snapshot delete instead of an overlay mount. Build start time could not be compared with 611, which logged no start line; on 0.2.57-613 the first output came within 1 s of the container request, the log store's resolution.
- The dev sandbox, which has no btrfs, can run no container test, which THE SANDBOX RULE already forbids.
- `test_overlay`, `test_container_storage_migrate` and the overlay half of `test_direct_rootfs` go with what they test. Each removal is named in the ROADMAP part.
- The contract loses `disk` on a container and `migrate-storage`, and the role enum changes. The CLI, the dashboard, `docs/api/README.md` and the guides move in the same change.
- On .95, vda's role is renamed and the build containers' trees move from vdb5 to sda, beside the image store.
