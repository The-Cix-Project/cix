# 0207 — btrfs as the sole storage substrate; per-container snapshot rootfs; user namespaces by default

## Status

Accepted; phased. Supersedes the OverlayFS shared-lower storage model
([ADR-0004](0004-overlayfs-dedicated-lowerdir.md)). Completes issue #29 /
[ADR-0179](0179-user-namespaces-by-default-subordinate-id-allocation.md)'s
user-namespace goal (issue #156) by a different mechanism than id-mapped
overlay. Retires ext4 from the platform's own storage once btrfs is proven in
production.

## Context

Today an image version is an immutable, content-addressed rootfs *directory*
(`IMAGES_DIR/<name>/<version>/rootfs`), and it is the shared **overlay
lowerdir** for every container built from it; a running container owns only its
upperdir diff (ADR-0004). The backing filesystem is ext4. An upperdir is a
btrfs subvolume only where an operator happened to format btrfs, and only for
quota (ADR-0103).

The security goal is issue #29: a container's root is **host uid 0**.
[ADR-0168](0168-container-capability-bounding-set-drop.md) dropped the dangerous
capabilities — done and verified live on 192.168.15.95 (`CAP_SYS_MODULE` and 19
others gone from a real container's `CapBnd`). The deeper defence — user
namespaces, so container-root is an *unprivileged mapped uid* and a
boundary-escape lands unprivileged rather than as host root — is ADR-0179.

ADR-0179 reached a **fundamental wall on the 6.18 kernel**, confirmed against
kernel source and live `/dev/kmsg`, not assumed: kernel-native id-mapped
**overlay** requires overlay's own workdir setup ops (the `O_TMPFILE` probe, the
whiteout `mknod`/`RENAME_WHITEOUT` probe, the overlay xattr) to run with
privilege over the **layer filesystem's** `s_user_ns` — which is **init**,
because Cix's persistent upper lives on host ext4 mounted by cixd in the initial
namespace. The mapped-root child is privileged only in *its own* userns, so
those ops are denied. A persistent host upper + shared host-0 lower + id-mapped
overlay is **not achievable** from this architecture on this kernel. The
remaining options were an architecture tradeoff: an ephemeral tmpfs upper
(RAM-bound — a large build writes gigabytes into RAM and OOMs the host — and
loses write persistence), or a per-container rootfs (loses the shared lower).

The decisive fact: **overlay is the only reason this is hard.** ADR-0179 already
established that a *plain, non-overlay* mount can carry `MOUNT_ATTR_IDMAP`
correctly — it was overlay's workdir setup, not id-mapping itself, that was the
dead end. Remove overlay from the picture and the id-mapped-mount primitive that
works becomes usable directly. btrfs gives us the plain, per-container,
copy-on-write-cheap, persistent rootfs that overlay was providing — **without
overlay**. That reframes the tradeoff: there is no longer a choice between
"persistent" and "shared and cheap." A btrfs snapshot is both.

## Decision

Three coupled decisions, made together because each depends on the others.

**1. btrfs is the sole storage substrate for the platform's own writable
storage** — the image store, container rootfs, and STATE/REBUILDABLE data. The
OS root stays squashfs A/B (read-only, unchanged — it was never ext4). ext4 is
retired from platform storage; `mkfs.ext4`/`e2fsprogs` leave the host-tool set
once btrfs is proven (staged, below).

**2. A container's rootfs is a writable btrfs snapshot of its image subvolume,
not an overlay.**

- An image version (`IMAGES_DIR/<name>/<version>/rootfs`) is a **btrfs
  subvolume**, seeded exactly as today (`pkg_seed_image_baseline()`).
- Creating a container is `BTRFS_IOC_SNAP_CREATE_V2` of that subvolume: an O(1),
  copy-on-write, writable clone that shares every unchanged extent with the
  image and is a real, persistent on-disk subvolume.
- OverlayFS (`src/overlay.c` and its `lowerdir`/`upperdir`/`workdir` use across
  `registry.c`, `pkg.c`, `container.c`, `image.c`, `mountns.c`, `main.c`) is
  **retired**. There is no overlay fallback.
- **Disk usage does not regress.** The base is stored once and only a
  container's changed extents cost space — the exact property the shared lower
  gave us, now via CoW instead of a union mount.

**3. User namespaces are on by default; the snapshot is presented through an
id-mapped mount.**

- Every container gets `CLONE_NEWUSER` + a subordinate-ID range (ADR-0179 phase
  1's allocator, `daemon/src/subid.c`, already built and tested) **unless the
  spec sets `userns:false`** (opt-out).
- The container's snapshot subvolume is owned by host uid 0 and mounted into the
  container with `mount_setattr(MOUNT_ATTR_IDMAP)` keyed to the container's
  userns, so its mapped root sees the whole tree as its own. **No per-container
  `chown`** — chowning the tree would rewrite every inode and is unnecessary;
  the id-mapped mount re-presents ownership in the kernel and preserves full
  extent sharing. This is the plain-mount idmap ADR-0179 confirmed works; only
  overlay could not use it.
- **Volumes** are bind-mounted with the same `MOUNT_ATTR_IDMAP`, so a
  host-0-owned volume is writable by the mapped root — the piece ADR-0179
  flagged as tractable-but-unbuilt (bind-mount idmap works where overlay-idmap
  did not).
- **Per-container quota** stays btrfs qgroups (ADR-0103's `BTRFS_IOC_QUOTA_CTL` /
  `BTRFS_IOC_QGROUP_LIMIT` code, already present), now applied to the snapshot
  rather than a standalone upper subvolume.
- The platform sets `userns:false` on its **own build/hostbuild containers** —
  trusted internal infrastructure running this project's own checksummed
  recipes, with no security case for isolating them from the host they build on,
  and heavy writers for which the mapped-uid tier buys nothing. This is a
  documented policy on the *existing* opt-out field, not a second code path.

Secure by default, opt-out — the posture the owner set.

## Consequences

**What this unlocks — the whole point.** Every container runs as an unprivileged
mapped uid by default, so a namespace-boundary escape lands as an unprivileged
host user, not host root: the defence-in-depth #29 asked for, now achievable
because overlay is gone. Writes are **persistent** (a real on-disk subvolume),
**disk-backed** (no tmpfs RAM/OOM — build containers included), **CoW-thin** (no
disk regression), under **one storage model** for every container. Three
separate problems — the overlay-userns dead end, the tmpfs RAM risk, the
ephemeral-writes regression — are retired by one substrate change rather than
mitigated one at a time.

**The image model changes and must stay one source of truth.** An image version
becomes a subvolume, not a plain directory. `image.c`'s create/delete/
version/rename logic (ADR-0107/0108/0155) operates on subvolumes
(`SUBVOL_CREATE`/`SNAP_CREATE_V2`/`SNAP_DESTROY`) instead of directory copies.
The content-addressed versioning and the same-manifest dedup (ADR-0155) are
**unchanged in semantics** — a version is still keyed by its manifest hash; only
the on-disk representation changes. Every code path that assumes an image or
container rootfs is a plain directory (`rename`, recursive `rm`, path
arithmetic) must move to the subvolume API. A half-migrated model where some
paths are subvolumes and some are directories is exactly the
parallel-implementation state this decision forbids.

**overlay retires completely, on purpose.** Keeping it "just in case" would be
the two-source-of-truth state we are removing. `src/overlay.c` goes; so does
every union-mount assumption downstream of it.

**Migration is explicit, never silent.** A host on the old ext4+overlay model
does not auto-convert. 192.168.15.95 is reformatted btrfs by the operator — its
role as the test box makes it the right first migration, and it is the box whose
guest kernel faithfully exercises `mount_setattr(MOUNT_ATTR_IDMAP)`. A
general installed-host migration (back up state via `system/backup`, reformat,
restore) is its own tracked work before any non-test host moves; a host is never
reformatted out from under its live state.

**ext4 retirement is staged, not big-bang.** btrfs becomes the platform standard
and the install default immediately; ext4 stays buildable but non-default
through the transition; `mkfs.ext4`/`e2fsprogs` leave the host-tool set only
after btrfs has run the platform's own storage in production on .95 long enough
to trust. Burning the fallback before the replacement is proven is precisely the
"confirm directly, don't assume" discipline this project runs on. (The install's
`fs_type` field and #103's btrfs formatting already exist, so the default flip
is small; the removal is the deliberate later step.)

**What needs live verification before the default flips — not assumed.** That
`MOUNT_ATTR_IDMAP` over a btrfs snapshot subvolume presents mapped ownership
correctly to a userns container on the .95 guest kernel; and that a container
writing to that idmapped snapshot persists across a restart with extents still
shared. ADR-0179's discipline carries over: the dev sandbox's own LSM blocks the
`uid_map` write, so this is verified on the .95 VM, one container at a time,
watching each come back — never flipped fleet-wide on faith.

**Risk, stated plainly.** btrfs single-disk with subvolumes/snapshots/qgroups is
mature and in-tree since 2009 (the default in Fedora and SUSE); we use none of
its historically fragile parts (RAID5/6). The real risk is not btrfs but the
*size* of the change — it touches the storage substrate, the image model, the
container mount path, and the security default at once. It is therefore phased,
each phase verified on .95 before the next, with ext4 recoverable until the
whole is proven.

## Implementation phases

Each phase is independently shippable and verified on .95 before the next
begins; the security default flips only in phase 3.

1. **Image store on btrfs.** Image versions become subvolumes;
   `pkg_seed_image_baseline()` and `image.c`'s create/delete/version/rename move
   to the subvolume API. Overlay is still in place — its lowerdir is now a
   subvolume tree, a no-op for the union mount — so existing containers keep
   working unchanged. Verifies the subvolume image model in isolation.
2. **Container rootfs = snapshot on btrfs.** `BTRFS_IOC_SNAP_CREATE_V2` per
   container, self-bound and pivoted into directly; qgroup quota in EXCLUSIVE
   bytes (a snapshot references the whole image from the first instant, and the
   quota owed is the container's own divergence — the semantics the upperdir
   quota always had). Verifies non-userns containers run, write, persist across
   restart, and hold quota.

   > **Correction (2026-08-28, during phase-2 implementation).** This phase
   > originally also said "`src/overlay.c` and its call sites removed — no
   > overlay fallback." That was written ahead of two measured facts. First,
   > the dev sandbox — where the entire container test suite runs — is ext4
   > with no loop devices and a read-only `/sys`: **btrfs cannot exist there at
   > all**, so with overlay deleted, every sandbox container would take a
   > full-copy fallback. Second, the staged test toolchain image is ~1GB
   > (wholesale `/usr/{include,lib,bin,...}`), and `test_pkg` alone creates a
   > build container per install — **15–25GB of copying per suite run**, which
   > is not a test suite anyone runs. Hardlinks are categorically unsafe for a
   > writable rootfs, so there is no cheap copy. Overlay therefore remains the
   > non-btrfs path — which is also what every not-yet-migrated ext4 host
   > actually runs, so the sandbox keeps exercising a real production path,
   > not a throwaway one — and **overlay's code retires in phase 4 together
   > with ext4**, exactly as ext4's own staged retirement was already
   > specified. The decision is unchanged: overlay still dies on production
   > and btrfs snapshots are the single forward model; only the deletion
   > commit moves to the phase where its last real user disappears. The
   > direct-rootfs machinery is fully exercised in the sandbox regardless,
   > through a copy-based `--test-direct-rootfs` mode covering everything but
   > the snapshot ioctl itself.
3. **userns by default.** `CLONE_NEWUSER` + idmapped snapshot mount + idmapped
   volume binds; build/hostbuild containers set `userns:false`. Verified live on
   .95: `/proc/1/uid_map` shows the mapped range, writes persist, extents stay
   shared — one container at a time.
4. **Migration + rollout.** Operator reformats .95 btrfs; the fleet is recreated
   onto the new model and watched back up. Once proven, `mkfs.ext4`/`e2fsprogs`
   are removed from the host-tool set and ext4 retired from the install default
   path.

---

## Addendum (2026-09-07): an id-mapped mount presents ownership for lookups; a *create* additionally constrains the caller

Phase 3's model above — "the disk stays host-0, extent sharing intact, and the
kernel does the presenting" — is correct, and incomplete in a way that took
every workload on the first btrfs host down (#321).

The mapping is not only consulted to answer *who owns this inode*. On a
**create**, the kernel also has to decide what on-disk owner to write, and it
does that by mapping the **caller's own fsuid** back down through the mount's
id-map. `may_create()` calls `fsuidgid_has_mapping()` for exactly this. With a
map of `"0 <base> <len>"`, a caller whose uid is not in `[base, base+len)` maps
to `INVALID_UID`, and the create fails **`EOVERFLOW`** — not `EPERM`, which is
why it does not read as a permissions problem.

The container child performed its whole setup as real host uid 0, on the
reasoning that host root was what let it mount, mknod and pivot. Host uid 0 is
deliberately *outside* the map. So every directory the child created inside its
own rootfs — each volume mount point, and all eight in `mountns_pivot()`,
`put_old` first — was refused, and the container exited 112 with
`mkdir(put_old): Value too large for defined data type` before it ever ran.

The reasoning was also wrong on its own terms: `pivot_root`, `move_mount`,
`mount(proc)` and the netlink work are gated on **capabilities in the new user
namespace**, which the child holds regardless of its uid, and `mknod` of a
device node is gated on `capable(CAP_MKNOD)` against the **initial** user
namespace, which no user-namespaced container has ever held. Host uid 0 bought
nothing and cost this.

**The rule this adds: under the id-mapped presentation the child must already
be the namespace's mapped root before its first write into its own rootfs.**
The `setgid(0)/setuid(0)` therefore happens the moment the rootfs is attached,
not just before `execve`. Capabilities survive it — the kernel compares against
`make_kuid(cred->user_ns, 0)`, i.e. `<base>`, not host 0 — so
`container_caps_drop()` still governs the final set, exactly as before.

This is a correction to how phase 3 is implemented, not a reversal of it: the
storage model, the host-0-owned snapshot and the id-mapped presentation are all
unchanged.

**Why no test caught it.** The branch is selected by `cix_btrfs_is_backing()`,
and every daemon-linked test runs against a `mkdtemp` under `/tmp`, which is not
btrfs — so the id-mapped presentation was unreachable in this suite and had
never once executed here. `test_userns_run` now runs its whole body against
*both* presentations, using a `--test-userns-idmap` daemon flag that makes the
copy present itself the way a snapshot does (host-0-owned, id-mapped); id-mapped
mounts need only the kernel, not btrfs. It reaches the release gate through
`DAEMON_SELFTESTS_2`, and it caught a second defect there on its first run: the
privilege drop was placed early enough that the child could no longer *traverse*
the host path to its own rootfs (a test daemon's `mkdtemp` directory is 0700).
The child now enters the new root by path once, while still host root, and
addresses everything after the drop relative to that — the volume mount points
and `put_old` included. **A privilege drop must not leave the process depending
on `o+x` for a directory it does not own.** `mountns_pivot()` therefore no
longer takes a root to walk at all: its contract is that the caller has already
entered it, so a caller cannot pass the wrong thing because there is nothing to
pass.

The test also asserts the storage model directly now, in both directions: the
mount point the container's own root creates inside its rootfs lands as on-disk
uid **0** under the id-map, and as the subordinate base under copy+chown. That
is phase 3's actual claim, and it is the assertion that refuses the plausible
wrong fix — chowning the snapshot to the subordinate base leaves every container
running perfectly and breaks the extent-sharing property this ADR exists for.
