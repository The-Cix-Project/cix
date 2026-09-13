# 0286 — a container sees its own disk

## Status

Accepted

Extends [ADR-0262](0262-a-container-sees-its-own-limits.md), which made a container
see its own memory and CPU, and [ADR-0207](0207-btrfs-storage-substrate-userns-by-default.md), whose
btrfs-subvolume rootfs is what makes the chosen mechanism available.

## Context

A container on this platform is told the truth about its memory and its CPU and
is lied to about its disk. Measured inside `jump` on 192.168.15.95, 2026-09-13:

```
claude@jump:~$ df -h
Filesystem      Size  Used Avail Use% Mounted on
/dev/vdb5        16G   12G  3.7G  76% /
/dev/vdb5        16G   12G  3.7G  76% /home

claude@jump:~$ cat /proc/partitions
 254  0  15728640 vda        254 16  33554432 vdb      254 17     65536 vdb1
 254 18    163840 vdb2       254 19    163840 vdb3     254 20    524288 vdb4
 254 21  16314368 vdb5        11  0   1048575 sr0        8  0 104857600 sda

claude@jump:~$ ls /sys/block
sda  sr0  vda  vdb
```

`jump` has `disk_quota_bytes: 536870912` (512 MiB) and its `/home` volume has
`quota_bytes: 2147483648` (2 GiB). Neither number appears anywhere. The container
is shown the whole 16 GiB pool twice, plus every block device on the host —
including a 100 GB scratch disk and a CD-ROM it has no business knowing exist.

It reads as a container rather than as a machine of its own, which is the
opposite of what this platform is for.

### The obvious fix is already done and does not work

The kernel qgroup limit is set, correct, and readable — the daemon reads it
successfully:

```
GET /v1/volumes/jump-home/usage
{"bytes":98304,"source":"qgroup","quota_bytes":2147483648,
 "limit_bytes":2147483648,"accounting":"ok"}
```

`source: "qgroup"` means that figure came from the kernel's own accounting, and
`api_volume.c:325` sets the limit with `cix_btrfs_qgroup_limit_excl()` at volume
creation. So **btrfs `statfs()` does not report qgroup limits.** `df` asks a
different question of the same filesystem and gets the pool. "Set the quota
properly" was never the missing piece.

### Three problems, not one

**`df` is wrong.** `statfs()` is answered by whatever filesystem is mounted at the
path, so nothing serving a `/proc` file can affect it. This is the hard one.

**`/proc/partitions` and `/sys/block` expose the host's block layer.** `lsblk`,
`fdisk -l` and anything else reading those sees the same.

**Volumes must be right too.** A container with several volumes must show each at
its own size, not all of them reporting one number.

### What is already correct, and is what makes the fix possible

`/proc/self/mounts` is accurate — it is per-mount-namespace by construction — and
it shows the real shape:

```
/dev/vdb5 / btrfs rw,…,idmapped,…,subvolid=777,subvol=/containers/jump/rootfs
/dev/vdb5 /home btrfs rw,…,idmapped,…,subvolid=591,subvol=/volumes/jump-home
```

The container root is a **real btrfs subvolume** (ADR-0207), not an overlay, and
so is the volume. Per-subvolume qgroups are therefore the natural accounting unit
and they already exist and already carry the right numbers.

### The two userspace answers, and why both are worse

**A loop-backed filesystem image per container, sized to the quota.** `df` becomes
exactly right with no interception at all. It was the leading candidate until one
measurement killed it: containers are seeded by **btrfs snapshot**, O(1) and
copy-on-write —

```c
daemon/src/main.c:13748   cix_btrfs_snapshot_or_copy(lowerdir, userns_rootfs) == 0
daemon/src/main.c:13751   * is the SAME O(1) writable snapshot a non-userns one
```

— and an image file cannot be snapshotted from the image subvolume. Every
container create would become a full copy of its image instead. That is a worse
regression than the bug, and it also needs `CONFIG_BLK_DEV_LOOP` (deliberately
excluded in kernel 6.18.40-20), loop-device lifecycle management, `mkfs` per
container, and a resize dance on every quota change.

**A FUSE passthrough root.** The container's rootfs becomes a filesystem we write,
whose `statfs` answers correctly. It needs `CONFIG_FUSE_PASSTHROUGH` to avoid
routing every byte through userspace, and it means **owning a filesystem** — some
25 VFS operations, security-sensitive under a user namespace, in the path of every
file operation in every container, where a hang puts container processes in
uninterruptible D-state. This project spent 2026-09-13 on a bug (#448) whose entire
severity came from D-state being unkillable.

Both are permanent, large commitments. Both exist to fix the answer to **one
syscall**, on a system where everything else about a container's filesystem is
already native and correct.

## Decision

**Two tiers, and neither is a userspace filesystem.**

### 1. `/proc/partitions` through `cix-procfuse`

A container sees only the devices backing its own mounts. `/proc/partitions` is a
*file*, which is exactly the shape `cix-procfuse` already serves for nine others
(`/proc/meminfo`, `/proc/cpuinfo`, `/proc/stat`, `/proc/uptime`, `/proc/loadavg`,
`/proc/swaps`, three under `/sys/devices/system/cpu/`). It is rendered from the
container's own mount table, which already knows which device backs each
mountpoint, so nothing has to be invented or tracked.

No kernel change, no new mechanism, and it removes `sda`, `sr0` and the host's
partition table from a container's view.

**The one entry it keeps is the real backing device at its real size** — for
`jump`, `vdb5` at 16 GiB, the actual btrfs volume its rootfs and `/home` live on —
and there is deliberately no synthesis. A container's `df` (after tier 2) reports
the 512 MiB qgroup limit while its `/proc/partitions` reports the 16 GiB device,
and that pair is not a contradiction: it is exactly how a real machine reads when a
filesystem does not fill its medium — the partition is the physical extent, the
quota is the logical bound. Showing the true device is more native than fabricating
a partition sized to the quota, which is the "synthetic entry" the owner was right
to be wary of. The cost is that the backing device's size is still visible; the
benefit is that everything shown is true.

### 2. btrfs `statfs()` reports the subvolume's qgroup limit

A 34-line kernel patch, carried in `recipes/package/kernel`:

- **`qgroup.h`** — one declaration.
- **`qgroup.c`** — `btrfs_qgroup_statfs_limit()`: takes `fs_info->qgroup_lock`,
  looks up the level-0 qgroup with the existing static `find_qgroup_rb()`, and
  returns its limit and usage if one is set. Prefers `MAX_EXCL` over `MAX_RFER`,
  matching what `cix_btrfs_qgroup_limit_excl()` already sets — exclusive bytes are
  what a snapshot-seeded subvolume actually owns, and extents inherited from the
  image are not its consumption.
- **`super.c`** — one block in `btrfs_statfs()` overriding `f_blocks`, `f_bfree`
  and `f_bavail`.

No existing kernel line is modified, no new include is needed (`super.c` already
includes `qgroup.h`), no new locking convention, no new data structure. It is a
**complete no-op wherever no qgroup limit is set**, which is every filesystem that
is not one of ours.

Every assumption was verified against the **real v7.2.3 source** this platform
builds, not against mainline: `btrfs_statfs(dentry, buf)` present;
`bits = fs_info->sectorsize_bits` so the units line up with the existing
`buf->f_blocks >>= bits`; `btrfs_root_id(BTRFS_I(d_inode(dentry))->root)` already
computed in the function for the fsid, so the subvolume is in hand;
`btrfs_qgroup_enabled()` exported; `find_qgroup_rb()` static in `qgroup.c`, which
is why the helper lives there rather than in `super.c`; and the `lim_flags` /
`max_excl` / `excl` fields present.

**Why a kernel patch is the smaller commitment.** It puts nothing in the data or
metadata path, adds no process, changes no storage model, and leaves subvolumes,
O(1) snapshot seeding and qgroups exactly as they are. The result is genuinely
native — a real block device, real `btrfs` in `df -T`, no synthesis anywhere, and
nothing that reads differently to someone who looks closely. Neither userspace
option can say that: a FUSE root reports `fuse` in `df -T` however we name the
device.

## Consequences

**This project now carries a kernel patch, and that is a standing obligation.**
The stack says "mainline Linux". `tcc.recipe` has carried local patches before —
the #122 do-while miscompile — and dropped them when upstream fixed the bug, so
there is precedent for the posture and for retiring it. But a kernel is a larger
commitment than a compiler, and the patch must be rebased on every version bump.
Thirty-four lines against owning a filesystem forever is the trade, stated plainly
so that a future reader can judge it rather than inherit it.

**The blast radius was measured, not hoped.** The patch changes `statfs` for any
subvolume carrying a qgroup limit. We set one in exactly three places —
`main.c:13761` (userns container rootfs), `main.c:14570` (direct container
rootfs), `api_volume.c:325` (volume). Our own `statfs` readers are
`main.c:7128` on `g_base_dir` and `disk.c:168` on a disk's `mount_path`, **neither
of which is ever given a limit**. So host-facing disk reporting is unchanged and
only container rootfs and volumes are affected — which is the intent.

**`/sys/block` is not addressed.** It is a directory tree rather than a file, so it
needs a third mechanism, and `lsblk` reads it. A container will still see the
host's block devices there. Recorded as a known remaining gap rather than quietly
left out.

**The opt-out is the one that already exists, and tier 1 inherits it for free.**
`procfuse: false` on a container create declines the whole `cix-procfuse` bind set
today (`main.c:12530`, `main.c:14590`); adding `/proc/partitions` to that set means
a container that opts out of its own `/proc/meminfo` also opts out of its own
`/proc/partitions`, with no second flag and no second policy. Tier 2 is a property
of the filesystem itself and has no per-container switch — a container carrying a
qgroup limit gets the corrected `df` unconditionally, which is correct, because the
limit is real whether or not the container asked to be shown it.

**`CONFIG_FUSE_PASSTHROUGH` is not needed and is not enabled.** It was approved for
the FUSE-root design that this ADR declines. Turning it on is a separate decision
with its own justification, should one ever arise.

**A container with no quota still sees the pool.** No limit means no override,
which is correct: an unbounded container genuinely can use what the pool has.
Whether every container should therefore carry a default quota is a real question
and deliberately not answered here.

**Both tiers shipped and were verified live** (2026-09-13, 192.168.15.95). Tier 1
in cix v2.57.145; tier 2 in kernel 7.2.3-12. `df -T / /home` inside `jump` reports
512 MiB for `/` and 2 GiB for `/home` — its actual quotas — where both read 16 GiB
before. Two things the build taught, recorded so the next kernel patch does not
relearn them: `kernel-builder` carries no `patch(1)` and a hostbuild's
`pkg_build_depends` composes nothing into the build image (ADR-0199), so the patch
is applied with `awk`/`cat`, each insertion asserting its anchor and the build
grepping for the injected symbol so a moved anchor fails the build; and a
recipe-revision bump leaves the kernel *version* string unchanged (`7.2.3`), so
deploying it requires `kernel_path` passed to `/system/update` explicitly and
`uname`'s build date — not the version — to confirm the new kernel actually booted.
