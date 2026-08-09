# ADR-0103: btrfs qgroup-based disk quotas as a second backend alongside ext4 project quotas

## Status

Accepted

## Context

`--disk-quota=BYTES` (Part 4, ADR-0062) has always meant exactly one thing under
the hood: an ext4 project quota, set via `quotactl(QCMD(Q_SETQUOTA, PRJQUOTA), ...)`
against the block device backing `CONTAINERS_DIR` (or, since ADR-0102, whichever
disk a container was actually placed on via `--disk=`), with the container's
upperdir tagged `FS_XFLAG_PROJINHERIT` so every file it creates inherits the
project id.

That mechanism is ext4/XFS-specific. `quotactl(2)`'s `PRJQUOTA` interface has no
btrfs implementation at all — btrfs was designed with a completely different
quota model (subvolume-scoped qgroups, not arbitrary-directory-scoped project
ids) and never grew a `quotactl(2)` shim for it. Before this ADR, a container's
`--disk-quota=` on a btrfs-backed disk would either silently no-op or fail
loudly depending on kernel version, with no real enforcement either way — an
acknowledged gap.

The user gave an explicit, dated directive (2026-08-08, filed against task
#678): don't just document this gap — close it. Implement real btrfs
qgroup-based quotas as a second backend, so `--disk-quota=` works correctly
regardless of which filesystem a container's storage disk uses. The directive
also specified the mechanism: raw ioctl calls, not shelling out to
btrfs-progs — consistent with this project's standing convention of talking to
the kernel directly wherever a raw syscall/ioctl route exists (rtnetlink
instead of `ip`, raw ICMP instead of `ping`, `quotactl(2)` itself instead of
`repquota`/`setquota`).

## Decision

### Backing-filesystem detection is the single branch point

`overlay_backing_is_btrfs(const char *path)` (`src/overlay.c`, declared in
`include/container.h`) does a `statfs(2)` on `path` and compares
`f_type` against `KX_BTRFS_SUPER_MAGIC` (`0x9123683e`). It's the one place
that decides which quota mechanism applies, called from two places that both
need the same answer:

- `daemon/src/main.c`'s `create_container_from_body()`, on `container_base`
  (the container's real storage root, disk-selection-aware per ADR-0102) — to
  decide whether to run the existing `quotamap_get_or_assign()` +
  `set_disk_quota()` (quotactl) dance at all, or skip it entirely and pass the
  raw byte limit straight through instead.
- `overlay_create()` (`src/overlay.c`), on upperdir's *parent* directory (not
  upperdir itself — upperdir doesn't exist yet at the point this check needs
  to run, so `statfs(upperdir)` would always fail and silently misroute a
  btrfs container onto the ext4 mkdir path) — to decide whether upperdir gets
  created as a plain directory (ext4 path, unchanged) or as a real btrfs
  subvolume with a qgroup limit attached (new btrfs path).

Two independently-written `statfs()` checks, one in each file, was rejected as
a One-Source-of-Truth violation waiting to happen — the two call sites are
guaranteed to make the same decision because they call the same function.

### Subvolume, not directory: `overlay_create_btrfs_upperdir()`

btrfs qgroups are scoped to subvolumes, not arbitrary directories — a qgroup
limit set on a plain directory inside a subvolume has no effect. So on btrfs,
upperdir itself has to *be* a subvolume, created via `BTRFS_IOC_SUBVOL_CREATE`
(ioctl'd on the parent directory's own fd, with the subvolume's leaf name —
btrfs subvolumes are always created relative to an existing parent on the same
filesystem, never addressed by their own absolute path) rather than `mkdir(2)`.
`EEXIST` is tolerated the same way the existing plain-`mkdir` path already
tolerates it (a container being recreated after a crash/restart before
cleanup).

If `quota_bytes > 0`: btrfs quotas are enabled on the filesystem
(`BTRFS_IOC_QUOTA_CTL`/`ENABLE` — idempotent, `EINVAL` from "already enabled"
is not treated as failure, matching the same "already in the state we wanted"
tolerance `devicemap.c`/`diskrole.c`'s own idempotent-create paths already
use), then a real qgroup hard limit is set via `BTRFS_IOC_QGROUP_LIMIT` with
both `max_rfer` and `max_excl` set to `quota_bytes`.

### The `qgroupid=0` discovery — no id-tracking table needed

The task description that filed #678 anticipated needing a persisted
qgroup-id-tracking table, "parallel to project id tracking" (`quotamap.c`).
This turned out to be unnecessary: `BTRFS_IOC_QGROUP_LIMIT` accepts
`qgroupid=0` as a documented self-addressing convention meaning "the
subvolume that owns the fd this ioctl is called on." Since
`overlay_create_btrfs_upperdir()` already has an open fd on the upperdir
subvolume it just created (needed anyway, to enable quotas on that
filesystem), it can set the limit directly with `qgroupid=0` — no numeric
qgroup id ever needs to be looked up, allocated, or persisted anywhere. This
is a genuine simplification found through research into btrfs's own ioctl
semantics, not an assumption; it eliminates an entire piece of
infrastructure the original task framing expected to need.

### Raw ioctls, hand-transcribed structs — no kernel uapi header, no btrfs-progs

Per the user's explicit directive and this project's established convention
(`clone3`'s `struct clone_args`, `epoll_event`'s packing, `fsxattr`), the
btrfs uapi structs and ioctl numbers are hand-transcribed into
`include/linux_compat.h` with a `KX_` prefix rather than including
`<linux/btrfs.h>` directly (avoiding the same class of glibc/kernel-uapi
header clash this project has hit before) and rather than shelling out to
`btrfs` CLI tooling (which this environment doesn't even have installed, and
which the project's own conventions reject on principle for anything with a
direct ioctl path). The four new ioctl numbers
(`KX_BTRFS_IOC_SUBVOL_CREATE` = `0x5000940e`, `KX_BTRFS_IOC_QUOTA_CTL` =
`0xc0109428`, `KX_BTRFS_IOC_QGROUP_LIMIT` = `0x8030942b`) were hand-derived
via the standard `_IOC(dir,type,nr,size)` encoding and cross-checked by
independently re-deriving this project's own two already-shipped, already-
verified `KX_FS_IOC_FS{GET,SET}XATTR` constants with the identical method,
confirming an exact bit-for-bit match before trusting the same process for
the new, previously-unverified values.

### `struct overlay_spec` gains a parallel field, not a repurposed one

`ov->project_id` (ext4/quotactl) and the new `ov->quota_bytes` (btrfs, the raw
byte limit itself rather than an indirection through a project id) are
mutually exclusive in practice — a given upperdir is on exactly one
filesystem — but kept as two distinct fields rather than one field with
filesystem-dependent meaning, because `struct overlay_spec` is a plain data
contract handed to `overlay_create()` and overloading a field's meaning based
on which branch will consume it is exactly the kind of implicit coupling this
project's own "No Hacks" maxim exists to prevent.

### Scope: `mkfs.btrfs`/disk-format-time btrfs staging is explicitly deferred

`daemon/src/diskformat.c` (Phase C, ADR unnumbered-at-the-time /
task #656-666) is hardcoded end to end for ext4: it shells out to a staged
`mkfs.ext4` binary and mounts the result with `mount(dev, path, "ext4", ...)`.
This ADR closes the *quota enforcement* gap (a disk already formatted btrfs,
by whatever means, now gets real quotas) but does not extend disk
*formatting* to offer btrfs as a choice — that's a materially separate body of
work (staging `mkfs.btrfs` into `mkbootroot`'s host-tools set the same way
`e2fsprogs.recipe`/`mkfs.ext4` were staged, a new `fs_type` parameter on the
format REST endpoint and CLI, and its own mount-type branch in
`diskformat.c`) with no dependency on the ioctl work this ADR covers. Task
#732 (filed earlier as a near-duplicate of this ADR's own originating task,
#678) is retained and re-scoped to track exactly this remaining piece,
rather than folded into this pass or left as a duplicate — so this ADR's own
scope stays reviewable as one coherent unit of work. Until #732 lands, a
btrfs-backed container disk has to have arrived at btrfs by some other means
(a pre-formatted disk, or a disk role reassigned after out-of-band
formatting) for this quota mechanism to ever engage.

### Cannot be live-tested in this sandbox

This dev environment (`/proc/filesystems` confirms kernel btrfs support) has
no mounted btrfs filesystem, no `mkfs.btrfs`, no loop devices, and no disk
safe to reformat for a live test — the same acknowledged gap ADR-0099/
ADR-0102 already documented for disk-placement features generally. The
regression sweep for this change (full daemon-linked suite plus
`test_harness`/`test_overlay`/`test_container_net`/`test_devices`, all of
which exercise `overlay_create()` directly) confirms the *ext4* path is
byte-for-byte unaffected by the new branching logic, and the new code
compiles clean (`-Wall -Werror`, zero warnings) with each raw ioctl number
independently verified per the derivation method above — but the btrfs
success path itself (subvolume creation, quota enable, qgroup limit
actually enforcing) has not been exercised against a real btrfs filesystem.
Live verification against 192.168.15.95 or another real box with a spare
btrfs-formatted disk remains an open follow-up, to be done opportunistically
rather than blocking this ADR.

## Consequences

- `--disk-quota=BYTES` now has a real, correct enforcement path on both
  filesystems this project's own disk-format mechanism can produce (ext4
  today) or that an operator might otherwise hand it (btrfs, formatted
  out-of-band) — the gap the user's directive named is closed, not just
  documented.
- No new persisted state: unlike the ext4 path (`quotamap.c`'s
  name→project-id table), the btrfs path needs nothing persisted at all,
  thanks to the `qgroupid=0` self-addressing convention.
- `overlay_create()` now has two upperdir-creation branches instead of one;
  both are covered by the same existing test suite (which runs exclusively
  against ext4 in this sandbox), so the btrfs branch's compile-correctness is
  verified but its runtime correctness is not, pending real hardware access.
- Task #732 (mkfs.btrfs staging / disk-format-time btrfs support) is a
  tracked, explicit follow-on, not a silently-dropped scope cut.
