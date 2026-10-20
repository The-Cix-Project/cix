# 0343 — An image keeps a bounded history, and a version leaves it off the event loop

## Status

Accepted by the owner, 2026-10-20 ("Accepted, go ahead and implement it"), as proposed the same day. Issue [#619](https://git.home.arpa/itdlabs/cix/issues/619). The owner chose this over a one-off `POST /v1/images/gc` as the way to test #619's hypothesis, because that call cannot be limited and does all of its deletes on the event loop. It changes how much image history a host keeps, which is hard to reverse once versions are gone, so accepting it is the owner's decision.

It amends [ADR-0107](0107-package-image-versioning.md), whose v1 scope left out automatic pruning. It amends [ADR-0209](0209-derived-images-and-one-container-mechanism.md)'s collector. It adds a field to [ADR-0320](0320-an-image-policy-decides-how-its-three-copies-agree.md)'s image policy. It reuses [ADR-0341](0341-a-containers-rootfs-is-a-snapshot-on-its-images-filesystem.md)'s set-aside-and-delete, which runs in an [ADR-0278](0278-the-reactor-forks-work-it-cannot-afford-to-wait-for.md) helper child.

## Context

**The stall.** Every hour that `hibr` releases upstream, the `jumpbox` image rolls, and the event loop waits about 1.7 s. On 0.2.57-551, at 00:59:47Z on 2026-10-20, stallwatch's per-call tallies named the cost:

```
event container (1774 ms), slowest step image version: the caller's mutate step (1351 ms), most in unlink (1219 ms, 386 calls), then copy (123 ms)
```

**The control.** On 0.2.57-552, at about 02:03Z, I ran the same `hibr` 0.99.264-1 → 0.99.265-1 upgrade into a scratch image, `probe-unlink`. It held 4 versions; `jumpbox` holds 256 or more. Both images sit on the same filesystem (`sda`, `rebuildable-storage`), under the same full quota accounting (`disk.usage.accounting: "ok"`). The scratch install took:

```
event pkg-unpack (173 ms), slowest step pkg install: close the run and save state (78 ms), most in copy (6 ms, 193 calls), then unlink old files (3 ms)
```

That is the same 193 files and the same code, roughly 400× apart. So full quota accounting alone is not the cause. **The hypothesis is that an unlink costs in proportion to how many snapshots share the file's extents.** Nothing has confirmed it yet.

**Nothing removes an image version unless someone asks.** ADR-0107's v1 scope said so. Every install snapshots a new version forward from the last (`pkg.c`'s `cix_btrfs_snapshot_or_copy()`), so a rolling image accumulates them without limit. A dry run of `POST /v1/images/gc` on .95 found 813 that nothing references: 256 in `jumpbox`, 109 in `base`, 96 in `cix-hosttools`, 88 in `cix-builder`, 69 in `cpdl-stage`, and the rest in smaller images. It kept 22.

**The history is already past its own cap.**
- `image_record_version()` stops appending once `IMAGE_MAX_VERSION_HISTORY` (256) is reached, but still moves `current_version` forward.
- So `jumpbox`'s recorded history holds its *oldest* 256 versions. Its newest ones may be on disk but missing from that history.
- `image_ondisk_versions()` is also capped at 256, so the collector sees at most 256 versions of any one image. The true count for `jumpbox` is unknown.

**The collector cannot be run safely as it is.** `handle_images_gc()` (`api_image.c`):
- runs entirely on the event loop;
- has no image filter and no count limit, and the API has no single-version delete;
- calls `image_delete_version()` inline, which deletes each subvolume, or removes the tree where btrfs is absent.

The **dry run alone, with no deletes, held the loop for 1909 ms** (stallwatch, 02:04:24Z). The code shows a likely reason: `image_version_is_referenced()` loads the image's state and re-parses every container definition, once per version, which is 835 versions times about a dozen definitions. That reason was read from the code, not measured. Decision 3's tallies will show where the time goes. A real run would add 813 subvolume deletes, under full quota accounting, on the one thread that serves the API. This box is shell-less, and cixd has wedged before (CLAUDE.md, the concurrent-heavy-builds rule).

## Decision

1. **An image keeps every version something references, plus its newest `keep` unreferenced versions. Nothing else stays.**
   - "Referenced" stays exactly what `image_version_is_referenced()` says it is, from its four sources:
     - the image's current version;
     - a registered container's version;
     - a container definition's `image_version`;
     - a container definition's `roll_from`, which covers ADR-0323's rollback window.
   - `keep` is a new field of the image policy: `cixctl image policy NAME --keep=N`, beside `recipe`, `apply` and `downgrade`. It counts unreferenced versions only, newest first.
   - The default is **3**. That keeps three earlier versions an operator can still create a container from with an explicit `image_version` on `POST /v1/containers`, on top of the rollback target the reference rule already keeps. The image policy's `downgrade` setting governs package pins in the manifest, not which image version a container is created from, so it does not stand in the way.
   - `keep` may be 0.

2. **The recorded history is the one source of when a version was made, and a pass repairs it from the disk first.** Retention orders versions newest first by the history's `created_at`. On `jumpbox` the history is missing exactly the newest versions, so a pass starts by reconciling the two:
   - a version on disk but not in the history is added, dated by its directory's `statx` birth time, or by its modification time where the filesystem has no birth time;
   - a history entry whose directory is gone is dropped.

   That repair does real work only once, on the first pass over an image that has drifted. From then on the history is correct, it is the only ordering source, and `GET /v1/images/{name}/versions` lists what is actually on disk. Reading the disk remains the repair and the drift check, never a second clock beside `created_at`. The repair works in memory with no 256 cap, so it can hold the whole drift. What it saves fits the cap, because by then retention has trimmed it (decision 6).

3. **A pass decides on the loop, and the cost of deciding is bounded.**
   - The rule stays in one function. The work is restructured so a pass does each piece once:
     - parse every container definition once;
     - walk the registry once;
     - load each image's state once;
     - read each image's version directories once, with no array cap. A pass sees every version on disk, not the first 256.
   - Each pass records stallwatch steps and tallies, so its loop cost is measured, not assumed.

4. **A version leaves through ADR-0341's mechanism, not a new one.** For each version a pass collects:
   1. It is dropped from the image's recorded history. That is one `save_state()` per image, not one per version, and it means nothing new is offered or created from that version.
   2. Each **subvolume** in its directory is set aside into `IMAGES_DIR/.discard`: its `rootfs`, and any `rootfs.replaced-*` (#551). Each move is one rename, so it is O(1).
   3. The version directory, by then holding only bookkeeping files, is removed on the loop.
   4. Each subvolume's delete is queued with `rootfs_discard_start()`, which runs at most two helper children at once.

   **The queue's limit of two bounds forks, not filesystem work.** `SNAP_DESTROY` returns once the subvolume is unlinked. The kernel's cleaner thread frees the extents afterwards, asynchronously, so 813 quick ioctls can leave far more freeing queued in the kernel than two-at-a-time suggests. Whether that queued work slows the loop is the open measurement below.

   **What is set aside is the subvolume, never the version directory.** `cix_btrfs_subvol_delete_or_rmtree()` destroys a subvolume whole with `SNAP_DESTROY`. Given a plain directory, that ioctl fails and the call falls back to `rmtree()`. That would unlink every file of the `rootfs` one at a time, which is exactly the call #619 measured as slow. ADR-0341 sets a container's `rootfs` aside for the same reason.

   `rootfs_discard()` already requires its trash directory to be on the source's filesystem. `IMAGES_DIR` and `CONTAINER_ROOTFS_DIR` both sit under `REBUILDABLE_DIR`, and image and version directories are plain directories. So each rename moves a subvolume within one parent subvolume, as ADR-0341's does. Startup gains `rootfs_discard_sweep(IMAGES_DIR)` beside the two sweeps it already runs (`main.c`), so a delete interrupted by a restart is finished by the next start. `image_list_names()` already skips names that start with a dot, so `.discard` is never taken for an image.

5. **A pass never runs while a package job is in flight.** This is the collector's existing rule, kept for the same reason. A build composes its environment from build-image versions, and that reference lives in `pkg.c`, where the reference check cannot see it. A pass runs at two times:
   - **Automatically**, once for each image that got a new version, as soon as no package job is in flight. `pkg_completion_followup()`, which already runs after every package job, marks the image. The pass runs when the marking job is the last one in flight, not after each install. On a host whose jobs rarely all stop, the pass waits. It never runs alongside a build, and a later idle moment serves.
   - **Once after the daemon starts**, at the first moment no job is in flight, over every image. A mark held only in memory is lost at a restart, so this pass is what keeps the bound true across one.
   - **On request**, through `POST /v1/images/gc`, which becomes "run a pass over every image now". Each image is held to its own policy's `keep`, and the request has no `keep` of its own: one rule decides how much history an image keeps. Setting an image's policy to `keep` 0 is how to collect everything unreferenced in it. The request keeps `dry_run`, and it still answers 409 while a job is in flight. Its `reclaimed` list now names versions *set aside*. Their space comes back when the helpers finish.

6. **The history cap stops being reachable in practice.** When every image holds at most its referenced versions plus `keep`, nothing approaches 256. The cap stays as the bound on a fixed array, and `image_record_version()`'s full-array branch stays. The cap no longer decides which versions are remembered.

## What is measured, and what is not

- **Measured:** the 1219 ms of `unlink` into `jumpbox` against 3 ms into a 4-version image; the 813 collectable versions; and the 1909 ms dry-run decision pass.
- **Not measured:**
  - Whether two helper children deleting subvolumes under full quota accounting slow the loop *through the filesystem*, for example through transaction commits the loop's own state writes wait on. Decision 4 takes the delete off the loop; it cannot promise the filesystem stays quiet. The first pass on .95 is the measurement. Stallwatch records any pass of 750 ms or more, and its tallies name the call.
  - **Quota accounting after the first pass.** Under full qgroup accounting, the kernel may mark accounting inconsistent rather than account a large shared subtree as it is dropped. That is the kernel's documented behaviour for subtrees deeper than its drop threshold; it has not been measured here. 0.2.57-552's `disk.usage.accounting` would then read `stale`. Container disk limits (`cix_btrfs_qgroup_limit_excl()`) rest on that accounting, so stale cannot be left in place. The daemon has no rescan today; `src/btrfs.c` only reads the status flag. So this ADR adds one:
    - once the discard queue is empty, a quota status reading inconsistent starts `BTRFS_IOC_QUOTA_RESCAN`;
    - it runs in a helper child;
    - it starts once per drained queue, never in a loop;
    - its start and finish are logged, and `accounting` reads `stale` until it finishes.

    A rescan reads every extent. Its cost on .95 is unmeasured, and stallwatch is what will show whether it reaches the loop.
  - **#619's cause.**
    - The first box-wide pass is decision 5's pass at daemon start, at the first boot into the release that implements this ADR. **That is the same deletion the owner declined as a one-off.** Accepting this ADR includes it, now off the loop and with each image's `keep` honoured. It sets aside about 813 versions on .95.
    - The next `hibr` exit into `jumpbox` then shows the `unlink` tally against a 4-version history. If it falls to milliseconds, the hypothesis holds.
    - If it does not fall, the cause is still open. `install_mutate` would still run on the loop, and that becomes its own question, not something folded into this ADR. This ADR stands either way: a history that grows without limit, and a collector that blocks the loop, are defects whatever #619 turns out to be.

## Consequences

- **Contract change.** The image policy gains `keep`, and `POST /v1/images/gc` changes meaning as decision 5 says. `docs/api/openapi.yaml` and `docs/api/README.md` change together, with `x-cix-read` and `x-cix-invalidates` declared (ADR-0339). The dashboard's image policy view gains the field.
- **Removing old versions is now automatic.** A version an operator meant to keep for later must be pinned by a container definition, or covered by `keep`. Once an operator upgrades to this release, `.95` loses about 813 versions it has never used.
- **`measure: true` is untouched.** It is still an `nftw()` walk on the loop, which the comment above `handle_images_gc()` already warns about. Measuring a version from its quota group in O(1) is a separate change.
- `test_images` or `test_pkg` gains a case for each of these:
  - a pass first adds an on-disk version missing from the history, dated by its directory, and drops a history entry whose directory is gone;
  - a pass keeps the referenced versions plus `keep`, newest by `created_at`;
  - an inconsistent quota status after the discard queue drains starts exactly one rescan;
  - a pass skips while a job is in flight;
  - a set-aside version is gone after its helper exits;
  - a `.discard` entry left by an earlier start is swept.

## Alternatives considered

- **Run `POST /v1/images/gc` once by hand.** That is the 813 deletes on the loop that the Context describes, and nothing would stop the history growing back. The owner declined it.
- **A count cap in `image_record_version()`, deleting the oldest version inline.** That puts a subvolume delete on the loop at every install. It also picks victims by recorded history, which on `jumpbox` is missing the newest versions.
- **A new helper child just for image versions.** That would be a parallel implementation of ADR-0341's set-aside, its queue and its startup sweep.
- **Keep everything, and make unlink cheaper with simple quotas (squota).** That may be worth knowing for its own sake; the test on `vda` is a separate question, still waiting on a permission. It would leave the history unbounded and the collector on the loop.
