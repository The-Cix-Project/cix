# 0379 — The host roll deploys the root when cix-root has moved

## Status

Proposed on 2026-11-16, for the owner's acceptance. It builds the half of [ADR-0371](0371-the-control-plane-root-is-one-image.md)'s decision B that is still missing, the root following its image. It is also the case that [ADR-0327](0327-the-host-follows-its-rolling-packages-in-a-nightly-window.md)'s host roll does not yet consider.

## Context

Since ADR-0371 the control-plane root is the current version of one image, `cix-root`. Most of that image rolls: kmod, the firmware, wireless-regdb, cbs, squashfs-tools and gcc-libs as entries, and since [ADR-0378](0378-a-rolling-images-dependencies-roll-with-it.md) every library they reach (openssl, curl, glibc, zlib and the rest). So `cix-root` gains a version whenever one of those is published.

Those versions do not appear by themselves. On 192.168.15.95 on 2026-11-16, `cixctl pkg source-catalogue` shows discovery running for three packages only: kernel, hibr and longwire. None of `cix-root`'s 24 installed packages is among them. A new revision of any of them is published by a person, or by an agent acting for one. Once published, the drain moves `cix-root` within minutes.

The host roll decides on two things only, the kernel and cix. `hostroll_decide()` stages a root when either is behind what is published, or not what is running. A `cix-root` that moved while both are current is never deployed. Measured on .95 on 2026-11-16:

- the running root (0.2.57-690, switched in at 18:19:13) was sealed from `cix-root` version `8ec94a64…` at 18:18:52;
- installing `gcc-libs` and upgrading `squashfs-tools` (cix#680) moved `cix-root` to `24df9a73…` by 18:31;
- `cixctl schedule run host-roll` then answered `current: kernel 7.3.4-1 and cix 0.2.57-690 are the newest and running`.

So a change to the root's content reaches the host only with the next cix release, however soon it is published. That includes an openssl security fix. cbs v0.1.130 was in `cix-root` at 16:00 that day, and only the 690 release brought it into the running root.

Nothing records which `cix-root` version a running root was sealed from.
- `mkbootroot` writes `BUILD_ID`, the cix release, into `/etc/os-release`, but not the image version.
- cixd keeps the version each assembly sealed (`root_image_version` in `GET /v1/system/assembly`) only in memory. That is what the last assembly did, not what is booted.
- `mkbootroot` takes no further input argument (CLAUDE.md, ADR-0371), and the tree it seals is image content, so the identity cannot be written into the root.

## Decision

**The host roll compares the running root's `cix-root` version with `cix-root`'s current one. When they differ, and the current version has not failed before, it assembles, stages and switches exactly as for a new cix, in its window or when run by hand, and at no other time.**

1. **cixd records what each slot holds, and only what the roll put there.**
   - Before anything writes a slot, by any path (the roll, `POST /v1/system/update`, a restore), that slot's record is cleared.
   - When the roll stages a root its own assembly just sealed, the record is set to the `cix-root` version that assembly sealed. That happens after the slot write succeeds and before any switch or reboot.
   - A request's `image_path` is never tied to a version: cixd cannot know what that file was sealed from.
   - The running root's version is the record for the slot cixd booted or switched into. A slot with no record is unknown: an installer-written slot, a slot written before this release, or one written by any other path.
2. **"The root is behind" is one comparison, made only on a settled image.**
   - It holds when `cix-root` is settled (`pkg_image_settled()`: no job in flight for it and not queued) and its current version differs from the running root's. Unknown counts as different.
   - It never compares content or dates. The image version is a hash of the image's package set, and so names it ([ADR-0155](0155-baseline-reseed-manifest-hash-dedup-gap.md)).
   - A `cix-root` still converging is not behind yet. The roll does not wait at its assembly step for a drain.
3. **A version that failed is not tried again.**
   - Before switching or rebooting into a root, cixd records `attempting V on slot X`, where V is the `cix-root` version.
   - The next cixd to start (the successor after a switch, or the next boot) resolves the mark:
     - **confirmed** if it runs from slot X and the slot is confirmed (ADR-0375);
     - **failed** if it runs from the other slot, after a refused switch, a reboot fallback or the boot counter's rollback.
   - A failed V is marked bad, and while `cix-root`'s current version is that V the root is not behind. The roll's reason says so, for example `root: cix-root 24df9a73 failed to deploy on 2026-11-17; waiting for cix-root to move`.
   - The mark clears when `cix-root` gains any other version. This is ADR-0323's rule for a container that fails on a rolled image, applied to the root: no person published this root, so it must not be retried every night.
4. **It joins the roll's existing decision as a third reason.**
   - `hostroll_decide()` stages when the kernel or cix is behind or not running, or when the root is behind.
   - A root behind with the kernel unchanged switches in place (ADR-0375), as a cix release does. The reason names it, for example `root: cix-root 8ec94a64 -> 24df9a73`, and so does the schedule line (#692).
   - The existing guard stays: `root_image_behind()` holds the seal until `cix-root` carries the installed hostbuild's `cixd` and `cixctl` and the kernel's modules.
5. **Only the roll deploys.** A new `cix-root` version arriving at any hour starts no assembly and no switch; it waits for the 03:00 window or a hand-run roll. That keeps ADR-0327's rule that the window is the one moment the host changes, and bounds churn to one switch a night however often `cix-root` moved.
6. **The running root's version is visible.** `GET /v1/system/boot` reports `root_image_version` (null when unknown) and `root_image_failed` (the bad version, or null). `cixctl boot` prints both.

## What "confirmed" proves, and what it does not

A switch is confirmed by ADR-0375's checks:
- the successor takes the handover and every container's record matches the kernel;
- boot confirmation attaches the uplink.

A reboot is confirmed by the same boot confirmation. **Neither proves that the new root can do everything the old one did.** The #589 root booted, reported health ok, ran every container, and could not fetch over HTTPS, because a different libcurl answered the same soname. A root that moved only by a library revision is exactly where that class of failure lives.

So this ADR names the limit and does not close it. A probation that makes one TLS fetch through `curlfetch_perform()` before confirming is a separate change, offered to the owner alongside this one.

## What this does not do

- **cix is not followed yet.** The roll still hostbuilds cix and the kernel, and `bootroot_drive()` still installs their outputs into `cix-root` by explicit install. That is the bridge ADR-0371 names, and removing it is the other half of decision B. The 689 roll measured ADR-0371's precondition the other way round: the drain builds `cix`, and the hostbuild takes its cached artifact. So that half needs its own design.
- **The kernel is unchanged.** `kernel-modules` stays pinned in `cix-root`, and a kernel change still reboots.

## Consequences

- **A fix reaches the host sooner.** Once a revision of anything in the root's closure is published, it reaches the host by the next window with no further step, instead of waiting for a cix release.
- **What a root-only roll costs:**
  - one assembly (about 25 s on .95), one slot write and one in-place switch, with no reboot;
  - a brief API pause at the switch, about 20 s when cbs changed and an explain sweep runs, while every container keeps running.
- **The first roll after this ships** finds no record, so it assembles and switches once even when nothing moved. From then on the record exists.
- **The contract changes:** `GET /v1/system/boot` gains two fields. `docs/api/openapi.yaml` and `docs/api/README.md` change together, and the read's announcement is declared (ADR-0339).
- **Gate:** the decision is a pure function, tested in SELFTESTS. It takes the running record, the current version, settled or not, and the bad mark, and is tested with:
  - unknown, equal and different versions;
  - an unsettled image;
  - a bad current version;
  - a bad version that `cix-root` has moved past;
  - a roll whose kernel or cix is also behind.
- **Live check:** the release's first roll reads `root: cix-root unknown -> <current>` and switches in place. A second hand-run roll then answers `current`, now including the root. The next real `cix-root` move is the first root-only roll.
