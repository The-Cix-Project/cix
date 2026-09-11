# 0277 — A container's rootfs is derived from its image, not durable state

## Status

Accepted

Issue [#401](https://git.home.arpa/itdlabs/cix/issues/401). Amends
[ADR-0207](0207-btrfs-storage-substrate-userns-by-default.md)'s reuse rule
and makes [ADR-0124](0124-pkg-redesign-part5-rolling-containers-and-restart-jitter.md)'s
`follow_rolling` actually apply. Neither is reversed.

## Context

ADR-0207 gave each container its own writable rootfs — a btrfs snapshot
of the image version it was created from, or a copy where snapshots are
not possible — and decided the storage mode from one fact on disk:

> The on-disk presence of `<base>/rootfs` is the mode marker: a revived
> container whose rootfs exists keeps it (that IS its state —
> re-snapshotting would silently discard every write it ever made).

The reasoning is sound and the sentence is still in the code. What it did
not say is which image version that kept tree came from, because at the
time there was only one answer: the version it was created from.

Once `follow_rolling` (ADR-0124) could move a container's pin, there were
two, and nothing distinguished them. `daemon/src/main.c` computed
`lowerdir` from the *new* pin and then, in direct mode, ignored it:

```c
if (stat(direct_rootfs_dir, &rst) == 0 && S_ISDIR(rst.st_mode)) {
        direct_mode = 1;                 /* reuse whatever is there */
} else if (...) {
        cix_btrfs_snapshot_or_copy(lowerdir, direct_rootfs_dir);
}
```

So the tree was seeded once, on first start, and reused forever.
`apply_rolling_container_restarts()` did its half correctly — patched the
pin, armed a restart — and the container came back on the old tree. Since
userns is the default (ADR-0179) and btrfs is the substrate (ADR-0207),
that is every container on a real host: **rolling updates have never
applied to a running container.**

Measured on 192.168.15.95 (2026-09-11, v2.57.61): `fastfetch@2.68.1-4`
installed into image `jumpbox`, whose `current_version` moved
`a976195c…` → `6ba75bd0…`. `GET /v1/containers/jump` then reported the new
version while its init was still pid 1946 from boot. A full stop+start
gave a new pid and still `/usr/bin/fastfetch` dated `Sep 11 01:17` — the
*previous* revision — while a container freshly created from the same
image, reporting the same version, had the `02:24` one. The image was
right; the container was not; the API said it was.

The failure is silent by construction. There is no error, no warning, and
the one field an operator would check agrees with the wrong answer. It
also feeds #394: `e->lowerdir` is recorded from the new version, so the
files API's fallback reads a tree the container is not running.

## Decision

**A container's rootfs is derived from its image version. When the pin
moves, the tree is discarded and seeded again.**

The rootfs records which image version it was seeded from, in
`<base>/rootfs.version` — beside the tree, not inside it. Inside would be
visible to the container and would travel with a snapshot of it, and both
are wrong for a fact about provenance.

On start, an existing rootfs is kept only while that marker matches the
version the container is pinned to. When it does not, the tree is deleted
(`cix_btrfs_subvol_delete_or_rmtree()`, which handles both a subvolume and
a plain directory) and seeded from the new version by the path that would
have run for a fresh container.

**In-container writes do not survive an image version change.** That is
the cost, stated rather than discovered: anything that must outlive an
image version belongs on a volume. This is what the rest of the platform
already assumes — `files[]` are re-staged on every start, real data lives
on volumes, an image is its declared packages plus the baseline.

**A rootfs with no marker is adopted, not rebuilt.** Its real origin
cannot be recovered, and a silent mass re-seed of every container on the
first boot after this lands would be a far worse surprise than one more
cycle of a staleness that already exists. A container that had already
drifted stays drifted until its image version next moves, at which point
it rebuilds like any other.

## Alternatives considered

**Leave the behaviour and fix the reporting.** Report the version actually
running and have `follow_rolling` say plainly that it cannot apply. Smaller
and honest, and rejected because it makes ADR-0124 permanently inert:
every update would then mean recreating a container by hand, which is the
thing that feature exists to avoid.

**Re-seed on every start.** Simpler — no marker, no comparison — and
wrong: a crash-restart would wipe the container's rootfs, which is a far
more common event than a version change and has nothing to do with one.

**Keep the writes and merge the new image under them.** This is what
overlay mode does, and it works there because the image tree and the
container's writes are separate objects. Direct mode collapsed them into
one; re-separating them would undo ADR-0207's whole point (O(1)
snapshots, extent sharing, no overlay).

## Consequences

- `follow_rolling` applies for the first time. A package installed into an
  image reaches every container tracking it, at the next restart.
- `image_version` stops being a claim the container may not be honouring.
- A container that kept state in its own rootfs loses it at the next
  version change. Only three deployments on 192.168.15.95 set
  `follow_rolling`, and none of them does: `jump` keeps home directories
  on a volume; `ldap-1`/`ldap-2` hold users and groups in cixd's own
  persisted state, rendered into the container by
  `ldap_record_sync_all()`.
- Rebuilding is O(1) on btrfs — a fresh snapshot of the new version —
  so the cost of the change is a delete and a snapshot, not a copy.
- Gated by `test_direct_rootfs`, which is in `SELFTESTS`: the existing
  case proves writes survive a same-version restart, and the new one
  proves they do not survive a version change.
