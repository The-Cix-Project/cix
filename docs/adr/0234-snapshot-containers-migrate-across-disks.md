# 0234 — Snapshot containers migrate across disks, at the cost of extent sharing

## Status

Accepted. Lifts the phase-2 limitation recorded in
[ADR-0207](0207-btrfs-storage-substrate-userns-by-default.md), which
deferred cross-disk migration of snapshot containers to "the phase-4
migration tooling". ADR-0207's storage model is unchanged.

Asked for directly, after the refusal was hit while moving the
platform's own DNS containers onto a dedicated disk:

> *"let's fix that issue to be able to move the btrfs containers, right?"*

## Context

`POST /v1/containers/{name}/migrate-storage` refused a snapshot
container outright:

```
409  this container uses a snapshot rootfs (ADR-0207) --
     storage migration for snapshot containers is not available yet
```

The refusal was correct at the time. The migration child copied the
whole container directory with `treecopy_recursive()`, which suits the
overlay layout — `upper/` and `work/` are ordinary trees. A snapshot
container has neither: its entire state is one btrfs subvolume at
`<dir>/rootfs`. A recursive copy reproduces that as an **ordinary
directory**. Every file is present, the container starts, and it can
never be snapshotted or qgroup-limited again — the damage shows up
later, at the next operation that needs subvolume semantics. Refusing
loudly beat corrupting quietly.

What made it worse than a missing feature is that the refusal arrived
*after* the operator had already committed to the idea. The containers
in question had been migrated successfully on an earlier build, back
when they were overlay-backed; recreating them made them snapshot-backed
and the same operation started failing.

## Decision

**Reproduce the subvolume as a subvolume.** The migration child
recognises the snapshot layout and uses a new, explicitly-named
`cix_btrfs_subvol_copy()`: create the target as a real subvolume, then
copy into it. The overlay path is untouched.

**Do not fold this into `cix_btrfs_snapshot_or_copy()`.** That was the
first attempt and it was wrong. Container *creation* calls that helper
and depends on it **failing** with `EXDEV` when the image store and the
container storage are on different filesystems — it then falls back to
an overlay rootfs, which still shares the image through its lowerdir and
costs nothing. Adding a copy fallback there would have silently turned a
cheap, deliberate fallback into a full image copy on every such create.
The two callers want opposite things from the same failure, so they ask
different functions.

**State the cost rather than hide it.** Extent sharing does not cross
filesystems. On its original disk a snapshot container shares almost
every extent with its image and costs only its own changes; migrated, it
occupies its full size on the target. `btrfs send -p` could preserve
sharing only against a parent with common lineage, and an image seeded
independently on each disk has none — so there is no cheaper correct
answer available, and the API documentation says so plainly.

## Consequences

- **A snapshot container can be moved to a `container-storage` disk**,
  which is what the role is for. The refusal is gone.
- **A migrated container costs its full size on the target.** An
  operator moving several containers off a small disk should size the
  target for their real totals, not for what `used_bytes` showed while
  they shared an image. This is in the endpoint's own description, not
  only here.
- **The result is still a subvolume**, so quotas (`disk_quota_bytes`,
  qgroups) and any future snapshot of it keep working after the move.
  That is the property the old refusal was protecting, and it is now
  protected by construction instead.
- **The second, synchronous copy pass needed no change.** `treecopy`
  only ever `mkdir`s (tolerating `EEXIST`) and unlinks individual files;
  it never removes a directory, so copying into the freshly created
  target subvolume refreshes its contents without destroying it.
- **Migrating back to the default OS-disk placement has the same cost**,
  in the same direction, for the same reason. There is no asymmetry to
  exploit.
