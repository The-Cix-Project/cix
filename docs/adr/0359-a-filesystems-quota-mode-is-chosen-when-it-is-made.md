# 0359 — A filesystem's quota mode is chosen when it is made

## Status

Proposed, 2026-10-27, for #613. The owner's direction of the same day: *"we should have simple and non-simple quotas and that should be a setting with simple as the default"*. This document states what that setting governs and what it cannot.

## Context

Cix limits a container's rootfs and a volume with a btrfs qgroup: an exclusive-bytes limit on the subvolume (`cix_btrfs_qgroup_limit_excl()`, `src/btrfs.c`). Quotas are turned on lazily, in FULL mode (`BTRFS_QUOTA_CTL_ENABLE`), the first time anything on a filesystem gets a limit, and an `EINVAL` from an enable that was already done is taken as success. Under ADR-0341 every container rootfs is a snapshot of its image version, so each roll is a snapshot create and a snapshot delete per container, and full accounting walks a dropped snapshot's tree during the commit (`btrfs_qgroup_trace_subtree()`) or, above a threshold, marks the whole filesystem's accounting inconsistent until a rescan (#613, #619).

Kernel 7.2 also has simple quotas. Measured on 192.168.15.95 on 2026-10-27, kernel 7.2.13, on a btrfs on `vda` that cixd formatted (#613 has the scripts and tables):

- **A container's quota bounds what the container writes, in both modes.** A snapshot of a 279 MB image subvolume was charged 16 KiB. 64 MiB written into it was charged to it (67190784 bytes). A 100 MiB exclusive limit on it refused the next write with `Disk quota exceeded` at 104841216 bytes. The numbers are identical under simple and full.
- **They differ in what an image is charged.** Under simple, shared extents stay charged to the subvolume that wrote them, so the image keeps its 278986752 bytes. Under full, both the image and the snapshot drop to their exclusive bytes (16384).
- **Simple was faster at this size.** 20 cycles of snapshot, 4 MiB write and delete with commit took 508 ms under simple and 750 ms under full, over 20000 small files. The tall-tree case, where full accounting marks the filesystem inconsistent, was not reached.
- **Simple never counts what was written before it was enabled.** 64 MiB was written into a subvolume with quotas off, then simple quotas were enabled. The subvolume was charged 0, then 8404992 bytes after 8 MiB more. A 40 MiB limit then let 16 MiB more through on a subvolume holding 88 MiB. The kernel decides this in `btrfs_record_squota_delta()`: `if (delta->generation < fs_info->qgroup_enable_gen) return 0;` (`fs/btrfs/qgroup.c:4955`, kernel 7.2.13 source, `probe-squota@2-1`).
- **The mode is readable from the filesystem.** The qgroup status item's flags carry `BTRFS_QGROUP_STATUS_FLAG_SIMPLE_MODE (1ULL << 3)` (`include/uapi/linux/btrfs_tree.h:1279`), beside `_ON`, `_RESCAN` and `_INCONSISTENT`. The status item is what `cix_btrfs_qgroup_query()` already reads.
- **mkfs can turn either mode on before the first byte.** The installed `mkfs.btrfs` (btrfs-progs 7.1-12, in `cix-hosttools`) lists the features `quota` and `squota` (`mkfs.btrfs -O list-all`).

So simple accounting is correct only for a filesystem that had it from the start. Turned on later, it misses everything already there, permanently, and a limit then means less than it says.

## Decision

1. **The quota mode is a host setting, `simple` or `full`, and `simple` is the default.** It is read and written through the REST contract, beside the other storage settings, and a host that never saved it is on `simple`.
2. **The setting is applied when a filesystem is made.** cixd's format (`diskformat.c`) passes `-O squota` or `-O quota` to `mkfs.btrfs`, so quotas are on before anything is written. The installer formats the containers partition with `-O squota`: an install has no saved setting, so it takes the default.
3. **A filesystem's mode is fixed for its life.** Changing it means making the filesystem again: reformatting a disk cixd manages, or rebuilding rebuildable storage. There is no live switch.
4. **A filesystem whose quotas are off when its first limit is set gets FULL accounting**, whatever the setting says. It may already hold data, and only full accounting counts it (a rescan). That is the case for every btrfs that predates this decision. A limit never enables quotas in a mode other than full, and never re-enables them.
5. **cixd reads each filesystem's mode from its status item** and reports it beside the setting: for every mounted btrfs it manages, `simple`, `full` or `off`, and whether full accounting is inconsistent. When a filesystem's mode differs from the setting, the read says so, and says that only remaking the filesystem changes it.
6. **The rescan and settle machinery (#619) runs only for full accounting.** A simple filesystem is never inconsistent and the kernel skips its rescan, so nothing waits for one.

## Alternatives

- **Switch a live filesystem.** It is a disable and a re-enable, which drops every qgroup and limit, and simple accounting would then miss everything already written: a container at 400 MiB of a 512 MiB limit would be charged nothing and could write 512 MiB more, permanently (measured above). An operation whose honest description is "your quotas stop meaning what they said" is not one to offer.
- **Enable lazily in the setting's mode,** as today but simple. That is the same defect as a live switch, on every filesystem that held data before its first limit.
- **Simple only.** The owner asked for a setting. Full accounting is also the one mode that charges an image version only its exclusive bytes, which an operator reading per-version usage may want.

## Consequences

- On 192.168.15.95 the image store (`sda`) stays on full accounting until it is rebuilt. Its quotas were enabled by the first container limit (`jump`'s 512 MiB), and the only enable in the code is full mode. That is read from the code; the status item will report it once decision 5 is in. `vda` has quotas off and holds nothing, so it gets simple only if cixd reformats it. The read shows both, and says why they differ from the setting.
- A fresh install is on simple accounting from its first byte.
- Under simple accounting, an image version's reported usage is everything it wrote, including the blocks its containers share. A container's own usage figure means the same in both modes.
- A gate in a build container cannot make a btrfs, since there are no loop devices. It proves the mkfs arguments, the status-flag parsing and the lazy-enable rule. The modes themselves are proven on 192.168.15.95.
