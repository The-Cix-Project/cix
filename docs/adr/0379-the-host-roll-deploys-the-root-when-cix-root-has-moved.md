# 0379 — The host roll deploys the root when cix-root has moved

## Status

Accepted by the owner on 2026-11-16. The owner answered three questions:
- whether to build it: *"Accept"*;
- when a moved `cix-root` should reach the host: *"As soon as it settles"*, over the nightly window, which decision 5 records;
- what a failed version gets: *"Mark it bad"*.

It builds the half of [ADR-0371](0371-the-control-plane-root-is-one-image.md)'s decision B that is still missing, the root following its image. It is also the case that [ADR-0327](0327-the-host-follows-its-rolling-packages-in-a-nightly-window.md)'s host roll does not yet consider.

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

**cixd compares the running root's `cix-root` version with `cix-root`'s current one. When they differ, `cix-root` has settled, and the current version has not failed before, it assembles, stages and switches the root in place: at once by a root-only roll that never reboots, and as part of any scheduled or hand-run roll.**

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
5. **A settled move deploys at once, by a root-only roll that never reboots.** The owner chose this over waiting for the window.
   - When a package job ends and `cix-root` is behind by decision 2, cixd starts a root-only roll, provided no roll is in progress, the kernel is current and running, and the version is not bad.
   - A root-only roll assembles, stages the root alone, and switches in place. It never hostbuilds: a kernel or cix that is also behind waits for the window or a hand-run roll.
   - **It never reboots.** The window stays the only time the host goes down (ADR-0327, CLAUDE.md), and a root-only roll switches in place or does nothing.
     - A switch the handover refuses marks the version bad, by decision 3.
     - If work stays in flight past the switch's 30-minute wait, the roll stops without switching. The staged slot waits for the next roll.
   - A roll started by the schedule or by hand also deploys a behind root, as part of whatever it does.
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

- **A fix reaches the host sooner.** Once a revision of anything in the root's closure is published, it reaches the host as soon as `cix-root` settles, by an in-place switch, instead of waiting for a cix release.
- **What a root-only roll costs, each time `cix-root` moves:**
  - one assembly (about 25 s on .95), one slot write and one in-place switch, with no reboot;
  - a brief API pause at the switch, about 20 s when cbs changed and an explain sweep runs, while every container keeps running.
- **Once after this ships, a root-only roll switches when nothing moved.** The release that ships it is deployed by the previous daemon's roll, which writes no record, so the running slot starts unknown. The first package job to end afterwards then starts a root-only roll, which assembles and switches once. From then on the record exists.
- **The contract changes:** `GET /v1/system/boot` gains two fields. `docs/api/openapi.yaml` and `docs/api/README.md` change together, and the read's announcement is declared (ADR-0339).
- **Gate:** the decision is a pure function, tested in SELFTESTS. It takes the running record, the current version, settled or not, and the bad mark, and is tested with:
  - unknown, equal and different versions;
  - an unsettled image;
  - a bad current version;
  - a bad version that `cix-root` has moved past;
  - a roll whose kernel or cix is also behind.
- **Live check:**
  - after the release, the first package job's end logs `host roll: root: cix-root unknown -> <current>`, and that roll switches in place without rebooting;
  - `cixctl boot` then reports `root_image_version` equal to `cixctl image show cix-root`'s current version, and a hand-run roll answers `current`, now including the root;
  - the next real `cix-root` move is the first root-only roll with something in it.
