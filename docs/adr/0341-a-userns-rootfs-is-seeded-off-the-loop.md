# 0341 — A userns container's rootfs is seeded off the event loop

## Status

Proposed, 2026-10-18, for the owner's decision. Issue [#608](https://git.home.arpa/itdlabs/cix/issues/608). It changes when a container create finishes, and every caller of `create_container_from_body()` has to handle a create that has not finished yet. That is hard to reverse. It applies [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md) (the loop's blocking budget) and [ADR-0278](0278-the-reactor-forks-work-it-cannot-afford-to-wait-for.md) (`helper_run()`). It uses [ADR-0340](0340-a-credential-is-checked-off-the-loop-and-the-request-waits-for-it.md)'s park and resume for the REST case. It leaves [ADR-0179](0179-user-namespaces-by-default-subordinate-id-allocation.md)'s storage model and [ADR-0277](0277-a-container-rootfs-is-derived-from-its-image.md)'s re-seed rule unchanged.

## Context

cixd is one event loop. While a handler runs, nothing else is answered: no request, no event-stream frame, no heartbeat.

**Measured on 192.168.15.95 (0.2.57-539), 2026-10-18.** Both of the day's `event container` stalls sit between two log lines written by the same container create, as `jump` rolled onto a new `jumpbox` image version:

| `rebuilding it from the new version` | `rolled to image version` | stall record |
|---|---|---|
| 09:00:42Z | 09:00:46Z | `event container (4504 ms)` |
| 16:00:36Z | 16:00:41Z | `event container (5061 ms)`, 3 slow passes |

Inside both windows the kernel logs `BTRFS info (device vdb5): Snapshot src from another FS`. That is `BTRFS_IOC_SNAP_CREATE_V2` refusing with EXDEV, because the image version is on a different filesystem from the container's base. The same stall followed every hourly hibr build on 2026-10-17 (#608's table).

**The path.** When a userns container's image pin moves, the create in `daemon/src/main.c` (the userns branch, from `rootfs_needs_reseed()` onwards) does three things on the loop:

1. It discards the old tree with `cix_btrfs_subvol_delete_or_rmtree()`.
2. It tries `cix_btrfs_snapshot_or_copy()`, which fails with EXDEV.
3. It falls to ADR-0179's phase-2b presentation: `cix_tree_copy()` of the whole image, then `chown_tree()` to the subordinate base.

A non-userns container answers the same EXDEV with the overlay path, which costs nothing.

**This is the intended layout.** `GET /v1/storage-roles` gives `sda` the `rebuildable-storage` role and `vda` the `container-storage` role. Ten containers on .95 are userns, and every one of them is `"disk":null`, so its base is `CONTAINERS_DIR`. Each pays a full copy whenever its image moves, and on any fresh create. `jump` shows it hourly only because hibr rolls hourly.

**The copy cannot be removed by overlay on this kernel.** This was the first question, because removing the copy would be better than moving it. `probe-ovlidmap@1-1` read kernel 7.2.11's own source on the box:

- `ovl_fs_type` sets `.fs_flags = FS_USERNS_MOUNT` and no `FS_ALLOW_IDMAP`, so the overlay mount itself cannot be id-mapped.
- Its layers can be id-mapped mounts (`ovl_upper_mnt_idmap()`, and `is_idmapped_mnt()` in `inode.c`).

ADR-0179 built exactly that on 6.18 and recorded why it fails with a persistent upper on a host filesystem. Mounted from outside the userns, overlay's own `work/` creation is denied (`failed to create directory /work/work (errno: 13 EACCES)`). Mounted from inside it, `ovl_make_workdir()`'s setup needs privilege over the layer filesystem's `s_user_ns`, which is init (`upper fs does not support tmpfile`). Nothing in the 7.2.11 reading changes either fact, so that path stays closed.

## Decision

1. **Seeding a userns rootfs by copy runs in a `helper_run()` child, never on the loop.** The child does the copy, the chown and the seed-version marker write, into `<base>/rootfs.next`. It is the same work `create_container_from_body()` does today, moved and not changed. A btrfs snapshot is O(1) and stays where it is.

2. **The create swaps the tree in, by rename.** When `rootfs_needs_reseed()` says the pin has moved and `rootfs.next` carries the wanted version, the create renames `rootfs` to `rootfs.old` and `rootfs.next` to `rootfs`, both O(1). `rootfs.old` is deleted by a helper child, because an rmtree is as slow as a copy.

3. **A create that needs a seed which is not ready asks for it and is resumed.** It starts the helper and returns a new status that means "not finished", having changed nothing. When the helper finishes, the create runs again from the start and finds the tree ready. This is ADR-0340's shape. For a REST `POST /v1/containers` it is ADR-0340's park itself, so the contract does not change: the 201 arrives later. The other callers of `create_container_from_body()` resume their own work: the rolling replay, the restart-always respawn, boot revival and the rest.

4. **A roll seeds before it stops the container.** `handle_rolling_restart_timer_event()` asks for the new version's seed first, and stops the running container only when it is ready. The swap in item 2 then happens at create. `jump` stays up while its next tree is copied, so its downtime is the stop and start, not the copy too.

5. **One seed per container at a time, and seeds count against one bound.** A second ask for the same container and version joins the first. Concurrent seeds are bounded like ADR-0340's checks, because each is a full image copy on the same disks. Past the bound a REST create gets 503 with `Retry-After`, and a daemon-driven one waits its turn.

6. **The false comment in `src/btrfs.c` is corrected.** Its EXDEV branch says the create path "falls back to the overlay rootfs, which ... costs nothing". That is true of the non-userns branch only.

7. **`test_blocking_waits` counts the copy.** `cix_tree_copy(` and `chown_tree(` are budgeted per file, the same way ADR-0340 budgets the credential calls, so a copy cannot drift back onto the loop unseen.

## Consequences

- No container create holds the loop for a copy. Measured today, that is 4–5 s on every `jump` roll, and a full image copy for any fresh userns create on .95.
- A roll costs `jump` less downtime: the copy happens while it is still running.
- A create can now be unfinished. Every caller of `create_container_from_body()` handles that state, and that is most of the work. A caller that does not handle it would stop resuming a container, so the change is gated by a test per caller.
- During a seed the disk briefly holds two full trees for that container. That is the same peak as today's discard followed by a copy, but it lasts for the whole seed rather than only during the copy.
- The copy still costs disk bandwidth and time; only the loop stops paying for it.

## Alternatives considered

- **Overlay with id-mapped layers for userns.** It would remove the copy entirely. Rejected because ADR-0179 measured why it cannot work with a persistent host upper, and kernel 7.2.11's source changes neither reason (Context).
- **Mounting the overlay with an unprivileged creator credential**, so that overlay's own operations go through the id-mapped upper as a mapped uid. Not measured anywhere, and it would make a security-sensitive credential arrangement part of every container mount. Not pursued.
- **One copy of each image version per filesystem**, so that every container on that filesystem snapshots in O(1) with an id-mapped mount and no chown. This saves the chown and shares one copy between containers of the same image (`dns-1`/`dns-2`, `ldap-1`/`ldap-2`). It still copies once per image version, so it still needs item 1, and it adds a second image store per filesystem to keep consistent. It could follow this ADR, but it is not a substitute for it.
- **Putting images and containers on the same filesystem.** The storage roles are separate by design. This is the operator's layout, and making it a requirement would bring the stall back on any host laid out differently.
- **Leaving it.** About 5 s an hour with the control plane silent. It falls on the dashboard and on `cixctl`, and it grows with the image.
