# 0267 — A volume's size comes from the kernel when the kernel is already counting

## Status

Accepted. Extended to container disk stats by [#475](https://git.home.arpa/itdlabs/cix/issues/475): `GET /containers/{name}/stats` now prefers the writable subvolume's qgroup over a walk and reports `disk.upper_source`, for the reason decided here. That endpoint had been documenting its walk as the container's diff from its image, which the seeded-subvolume substrate makes false -- so this was not a new decision, only one that had not been applied where it was equally true.

Issue [#369](https://git.home.arpa/itdlabs/cix/issues/369). Extends
[ADR-0103](0103-btrfs-quota-backend.md)'s qgroup enforcement with the read side it
never had, and gives [#365](https://git.home.arpa/itdlabs/cix/issues/365)'s usage
endpoint a second source.

## Context

`GET /volumes/{name}/usage` (#365) answered "how much does this volume
hold" by walking the tree with `overlay_upperdir_size()`. That was the
only mechanism available at the time, and for a volume on ext4 it still
is.

It stopped being the only mechanism when volumes became btrfs
subvolumes carrying real qgroup limits. A qgroup is a counter the kernel
maintains continuously, and it is the counter a `MAX_EXCL` limit is
enforced against. Two consequences followed, and the second is the one
that matters:

1. Walking a tree to produce a number the kernel already has is work
   nobody needs to do. `handle_volume_usage()`'s own comment explains
   the endpoint is separate from `GET /volumes` precisely because the
   walk is O(files).
2. **The walked number and the enforced number are different numbers.**
   A walk sums apparent file content. A qgroup counts exclusive
   allocated extents. On sparse, compressed or reflinked data these
   diverge, and they diverge in the dangerous direction: a walk can
   report a volume comfortably under its quota while the kernel is about
   to refuse the next write. That is the shape of failure this project
   already has scars from -- a number claiming headroom that does not
   exist (#278/#279, and `procfuse.h`'s own account of it).

There was also no way to read a qgroup back at all.
`cix_btrfs_qgroup_limit_excl()` could set a limit; nothing could ask what
limit was set or how full it was. The daemon's `quota_bytes` was a record
of what had been *asked for*, with no way to confirm the filesystem
agreed.

## Decision

**`cix_btrfs_qgroup_query()` reads a subvolume's qgroup, and
`GET /volumes/{name}/usage` prefers it over the walk -- while telling the
caller which one answered.**

The response gains `source` (`"qgroup"` or `"walk"`), and on the qgroup
path `limit_bytes` and `accounting`.

Three parts of this are deliberate:

**Telling the caller the source, rather than presenting one number.**
The two figures do not mean the same thing, and silently swapping which
one a client receives -- depending on a filesystem property that client
cannot see -- would make `bytes` a value whose meaning is unknowable.
Naming the source keeps one field honest instead of making two fields
that mostly agree.

**`limit_bytes` separate from `quota_bytes`.** They should be equal.
Reporting only one would mean an operator could never see the case where
they are not -- a limit this daemon believes it set and the filesystem
never applied. Two numbers side by side make drift visible at no cost.

**`accounting`.** After quotas are enabled btrfs marks accounting
inconsistent and rescans; until that finishes `excl` reads low for
pre-existing data. Under simple quotas (squota) `excl` is attributed on a
different rule entirely. A stale number reads exactly like a correct one,
so the state travels with the number rather than being left for the
reader to not know about.

## Alternatives considered

**Keep the walk everywhere, for consistency.** One number, one meaning,
no `source` field, and clients need not care what filesystem a volume is
on. Rejected because consistency here means being consistently wrong
about the number that is actually enforced. The walk cannot see what
btrfs charges a subvolume, and on a volume with a limit that is the only
figure worth having.

**Report the qgroup only, and refuse volumes without one.** Simpler
response shape. Rejected because volumes on ext4 and plain directories on
btrfs both exist and both have a real size; refusing to answer for them
would remove a working capability to gain a tidier schema.

**Add the qgroup as extra fields, keeping `bytes` always the walk.**
Preserves the existing meaning of `bytes` exactly. Rejected because it
keeps paying for the walk on every request forever, including on the
volumes where it is least useful, and leaves the enforced number in a
field callers have to know to look for.

## Consequences

- `bytes` changes meaning on btrfs volumes with quotas enabled. That is a
  contract change, documented in `openapi.yaml` and `docs/api/README.md`
  in the same change, and it is why `source` exists.
- The qgroup figure lags to the next transaction commit (~30 s) where the
  walk was instant. Accepted: a figure that is 30 s old and enforced beats
  one that is current and unenforced.
- `cix_btrfs_qgroup_query()` **refuses a plain directory with `EINVAL`**
  rather than answering it. `BTRFS_IOC_INO_LOOKUP` on an ordinary
  directory succeeds and returns the id of the subvolume containing it,
  so a query without that guard would answer a pre-conversion volume with
  its whole parent's accounting -- a confident wrong number, not an
  error. `test_btrfs` gates the refusal, and removing the guard was
  confirmed to fail that test.
- Reading a qgroup means searching the quota tree directly, because btrfs
  exposes no call for it. Two point lookups with `min == max`, never a
  range: a tree key is a 136-bit `(objectid, type, offset)` value compared
  as a whole, so a range spanning the INFO and LIMIT types also spans every
  other subvolume's qgroup id between them, and the wanted item need not
  be in the first page.
- The success path cannot be tested in the dev sandbox -- no loop devices
  means no mountable btrfs (see `CLAUDE.md`). `test_btrfs` gates the
  refusal contract, which is where the dangerous failure lives; the
  success path is verified on a real host.
