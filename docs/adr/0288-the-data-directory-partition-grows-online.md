# 0288 — The data-directory partition grows online, in place

## Status

Accepted

Issue [#94](https://git.home.arpa/itdlabs/cix/issues/94) (grow a partition and the filesystem inside it) reaches its last unreachable case here; [#249](https://git.home.arpa/itdlabs/cix/issues/249) (the data-directory partition's size was permanent) is the specific gap closed. Builds directly on [#140](https://git.home.arpa/itdlabs/cix/issues/140)'s mechanism (append to a disk in use, telling the kernel about just the one change) and [#163](https://git.home.arpa/itdlabs/cix/issues/163) (btrfs and ext4 both grown).

## Context

`POST /storage/{disk}/partitions/{part}/resize` (issue #94) grows a partition and the filesystem inside it. It required the partition to be **unmounted**, for a reason that was correct as far as it went: growing the table entry with `sfdisk` and then `--force`-ing the kernel to re-read the whole table (`BLKRRPART`) fails with `EBUSY` while anything on the disk is mounted, and `resize2fs` on ext4 needs the filesystem offline.

But the one partition an operator most often needs to grow is the daemon's own data directory — `cix-containers`, mounted at `/var/lib/cix`, which fills up as images and containers accumulate. And that partition **can never be unmounted**: it carries `cixd --data-dir=`, fixed at control-plane assembly, and `POST /storage/cix-containers/unmount` is refused by rule (issue #249). So the resize endpoint could not touch the exact partition it was most wanted for. Measured on a real host (192.168.10.240, 2026-09-13): `cix-containers` was 16 GiB with **227 GiB of contiguous free space immediately after it**, and there was no API path to use it — `unmount` returned a deliberate `409`, and `resize` would have returned `409` "mounted".

This was a real One-Source-of-Truth-shaped gap: the capability existed, the space existed, and the two could not be connected because of a precondition that was true for ext4 and false for btrfs.

The unmount precondition was also *btrfs-inappropriate* on its face. `btrfs filesystem resize` operates on a **mounted** filesystem — the opposite of `resize2fs` — so the existing code already mounted an unmounted btrfs partition at a private scratch point just to grow it. For a partition that is *already* mounted, that scratch mount is not just unnecessary, it is impossible (the device is busy). The natural operation for a mounted btrfs is to grow it exactly where it is.

The append path (#140) had already solved the "change the partition table on a disk that carries mounted filesystems" problem: `sfdisk --no-reread --no-tell-kernel` rewrites the table on disk without touching the kernel's view, and then `ioctl(BLKPG_ADD_PARTITION)` tells the kernel about the single new partition — the `partx(8)` mechanism, which never triggers the whole-disk re-read that `EBUSY`s. The resize path was still on the older `--force` / `BLKRRPART` mechanism and had never been moved over.

## Decision

**A mounted btrfs partition is grown online, in place, on its live mountpoint. The table-entry grow uses the append path's own kernel-safe mechanism for every resize, mounted or not.**

Concretely, in `diskpart_resize()`:

- **Mounted is allowed for exactly one case: a mounted btrfs.** It is grown with `btrfs filesystem resize max <live-mountpoint>` — no scratch mount, and it is left mounted, exactly as found. This is the only path that extends the data directory. A mounted **non-btrfs** partition is still refused (`409`): `resize2fs`'s `e2fsck`+grow needs ext4 offline, and there is no need to relax that. An unmounted partition keeps its existing flow (scratch-mount a btrfs, `e2fsck -f -p` + `resize2fs` an ext4).

- **The table entry is grown with `sfdisk -N --no-reread --no-tell-kernel` for every resize**, then the kernel is told about *just this one partition's new size* via `ioctl(BLKPG_RESIZE_PARTITION)`. This replaces the `--force` / `BLKRRPART` mechanism entirely — one mechanism, shared with the append path, that behaves identically whether or not the disk carries mounted partitions. The geometry handed to the kernel is read **back from the table** sfdisk actually wrote (it aligns to the disk's granularity), so the kernel's view and the on-disk table cannot drift — the same discipline the append path uses. `--force` is deliberately dropped: measured on a crafted GPT (2026-09-13), `sfdisk -N --no-reread --no-tell-kernel` grows the entry *and still refuses a grow that would overlap the next partition, leaving the table untouched* — so the safety `--force` would have overridden is exactly the safety worth keeping.

- **The `BLKPG_RESIZE_PARTITION` return code is the authority on whether the kernel took the new size.** It either resizes the one device or fails, with no whole-disk re-read to `EBUSY`. This replaces the previous "re-enumerate the partition and compare sizes" check, which could not distinguish "the kernel refused" from "the size is already right" — a distinction that matters for the idempotent retry below.

- **The operation is idempotent.** If the table and kernel view grow but the filesystem grow then fails (the documented `500` case: the space is real but not yet usable), re-issuing the *same* request finishes the job. The requested total now equals the current size, which is treated as "the table is already right, grow the filesystem" rather than refused as a shrink. The pure size arithmetic is split into `diskpart_resize_target()` so this rule is unit-tested without a block device (the dev sandbox has none).

## Consequences

- The data directory can be grown for the first time. On the measured host, `cix-containers` can go from 16 GiB to 243 GiB into the free space behind it, live, without a reboot and without unmounting anything.

- One table-grow mechanism instead of two. The resize path and the append path now both go through `--no-reread --no-tell-kernel` + a `BLKPG` ioctl; the `--force`/`BLKRRPART` path is gone. This is a strict simplification and removes the `EBUSY`-on-a-busy-disk failure mode from resize entirely.

- The frontend "Grow…" button now appears for any non-protected growable partition, including a mounted btrfs, and no longer for a mounted ext4 (which the daemon refuses). It had also been stale since #163 (it excluded btrfs outright); both are corrected together.

- Shrinking, moving, reclaiming and deleting the data-directory partition remain refused. This ADR closes only the grow direction of #249 — deliberately, because growing into trailing free space is the reversible, safe direction, and the irreversible ones are not worth the risk on a live box.

- Verified on a scratch disk, not on the data directory of a production box: the mechanism is proven by growing a role-assigned btrfs partition online on scratch storage, since a full data-directory grow is validated by hand on the target host. The `diskpart_resize_target()` arithmetic (including the idempotent-retry case) is gated in `test_diskpart.c`.

## Alternatives considered

- **Special-case the data directory.** Detect that the partition is `/var/lib/cix` and grow only that online. Rejected: growing a mounted btrfs is a general, correct capability, and a special case would be a second code path for the same operation (No Parallel Implementations). The rule "a mounted btrfs grows online" is simpler and strictly more useful than "the data directory grows online."

- **Keep `BLKRRPART` and require a reboot for the data directory.** Grow the table while mounted, then tell the operator to reboot so the kernel re-reads it. Rejected: it makes an online operation an offline one for no reason — `BLKPG_RESIZE_PARTITION` updates exactly the one partition the kernel needs to know about, which is what `partx --update` itself does.

- **Unmount, grow, remount the data directory.** Impossible: the daemon *is* using `/var/lib/cix` (it is `--data-dir`), so it cannot be unmounted while cixd runs, which on a shell-less box is always.
