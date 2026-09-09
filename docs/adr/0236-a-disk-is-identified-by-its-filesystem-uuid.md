# 0236 — A disk is identified by its filesystem UUID, not by its kernel name

## Status

Accepted

Issue [#255](https://git.home.arpa/itdlabs/cix/issues/255). Decided 2026-09-02.

## Context

Disk roles (`diskrole.c`) and storage placements (`storageplacement.c`) each
persist a reference to a disk, and both persisted it as a kernel device name:
`"disk_name": "sda"`, `{"rebuildable": "sda"}`.

A kernel device name is assigned in probe order. It is a *location*, not an
identity, and it moves whenever the ordering changes — a different driver set,
a different controller, a disk added or removed.

That is not a theoretical concern. Adding SCSI low-level drivers to the kernel
config (#242) registered more SCSI hosts, and the platform's scratch disk moved
from `sda` to `sdb`. Nothing was wrong with the disk, the filesystem, or the
new drivers. Every record naming it went stale in the same instant, the
rebuildable-storage placement could not be resolved, and the daemon refused to
start. The host was unreachable until an operator selected the other A/B slot
at the boot loader — with a console, on a machine otherwise managed entirely
over the network.

This is the same mistake ADR-0140's protection rule deliberately avoided, for
the same reason: that rule is keyed on partition **number** rather than label,
on the stated grounds that a protection a rename can lift is not a protection.
A placement a rename can break is not a placement.

## Decision

**A persisted disk reference is a (name, filesystem UUID) pair, and the UUID is
the identity.** At every boot the UUID is resolved to whatever kernel name the
disk currently answers to, and the name is updated to match.

`disk_probe_fs_uuid()` reads the UUID from the superblock, at the same offsets
`disk_probe_fs_type()` already uses, checking btrfs before ext4 for the reason
recorded there — `mkfs.btrfs` does not zero the ext4 superblock, so a converted
disk still carries a convincing `0xEF53`.

`diskrole_resolve_recorded()` is the single resolution point, shared by both
modules. Both were broken by the same rename; they resolve it through one
function rather than two that can disagree.

Three cases, distinguished deliberately:

- **No UUID recorded** (a pre-existing record, or a filesystem that has none):
  fall back to the name. This is exactly the old behaviour.
- **UUID resolves to a different name**: adopt it, and say so.
- **UUID recorded but no disk carries it**: genuinely absent, which is not the
  same as renamed, and must not be reported as one.

Only ext4 and btrfs are read. They are the two filesystems this platform puts
on a role disk. vfat has a 32-bit volume id rather than a UUID and squashfs has
none; both correctly yield nothing and fall back to the name. **A UUID that
cannot be trusted to identify the thing it names is worse than admitting there
isn't one.**

## Backfill

A record written before this existed adopts its UUID on the next boot: read
once from the disk the record currently names, then persisted. Without this the
fix would only protect disks whose roles were assigned *after* the upgrade —
every already-configured disk would stay name-identified, which is precisely
the state that broke.

New filesystems capture their UUID at `diskrole_set_fs_type()`, which is the
moment a format has just created one. Doing it at role *creation* would be
wrong: there may be no filesystem yet, and a UUID read then would belong to
whatever was there before.

## Alternatives considered

**Partition label.** Already known to be unreliable here: a `part_label` can
read empty for a window after every boot, observed directly on this platform.
A label is also editable by anything that writes the partition table.

**`/dev/disk/by-uuid` symlinks.** Requires udev, which this platform does not
run. Reading the superblock is what `disk_probe_fs_type()` already does.

**Serial number or WWN.** Identifies the *device* rather than the filesystem.
The wrong granularity — a placement is a filesystem, and the device may be
repartitioned or replaced while the filesystem is restored onto something else.

## Consequences

A disk can be renamed by any kernel, driver or hardware change and its roles
and placements follow it.

The `disk` field in the API still reports a name, because a name is what is
used everywhere else. It is simply no longer what the record *is*.

A filesystem restored from a byte-level image carries the same UUID, and is
therefore correctly recognised as the same placement. Two disks carrying the
same UUID (a cloned image) are ambiguous — the first match wins, which is the
same rule `disk_enumerate()` already applies.
