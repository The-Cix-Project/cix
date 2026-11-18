# 0380 — cix is followed in cix-root, and a release is a publish

## Status

Accepted by the owner on 2026-11-18 (*"Accept"*). It builds the other half of [ADR-0371](0371-the-control-plane-root-is-one-image.md)'s decision B, which the owner chose on 2026-11-03: *cix is followed like any entry*. [ADR-0379](0379-the-host-roll-deploys-the-root-when-cix-root-has-moved.md) built the first half, the root following its image.

## Context

A cix release today takes these steps:
1. Publish `cix@N`, git first.
2. Run the host roll by hand, or wait for 03:00.
3. The roll hostbuilds cix into `ARTIFACTS_DIR/cix`. Since #688 that build is a cache hit on the agent image's own build of the same recipe.
4. `bootroot_drive()` installs the hostbuild's `cixd` and `cixctl` into `cix-root`. That is an explicit install moving their pins, which ADR-0371 calls "a bridge until cix is followed (decision B), not the design".
5. The assembly runs `ARTIFACTS_DIR/cix/mkbootroot`, and the roll stages and switches in place.

Measured on 192.168.15.95, 2026-11-18, at the 692 roll: the roll waited behind the agent build from 17:08, built cix as a cache hit at 17:35:47, and switched at 17:36.

**What the cix hostbuild holds,** measured the same day with `GET /v1/pkg/cix@__hostbuild`: exactly the main package's six host tools, at the top of its tree. They are `mkbootroot`, `mkinstalleriso`, `cix-install`, `cix-recover`, `cix-boot.efi` and `cix-xorriso`. `cixd` is the `cixd` output's `bin/cixd`, and `cixctl` the `cixctl` output's `usr/bin/cixctl`.

**Four things read the cix hostbuild:**
- **The assembly:** `mkbootroot` (`spawn_cix_bootroot_assembly()`).
- **The installer ISO:** `mkinstalleriso`, `cix-install`, `cix-recover` and `cix-boot.efi` (`POST /v1/system/iso`).
- **The bridge:** `cixd` and `cixctl` (`g_root_outputs[]`, `root_image_behind()`).
- **The roll's decision:** "cix behind" and "cix not running" (`hostroll_parts_read()`).

Since ADR-0379 the root follows `cix-root`. So if `cixd` and `cixctl` were rolling entries of `cix-root`, publishing a release would be enough. The drain would build the cix recipe once, `cix-root` would move to the new `cixd`, and a root-only roll would switch into it in place. The hostbuild, the bridge and the hand-run roll would have nothing left to do for cix.

The kernel is different. A new kernel needs a reboot, which only the window may do, and `bzImage` is not root content. So it stays a hostbuild at 03:00, and its `kernel-modules` stay bridged into `cix-root` as now.

## Decision

**`cixd` and `cixctl` are rolling entries of `cix-root`, so publishing a cix release deploys it: the drain builds it, `cix-root` moves, and a root-only roll (ADR-0379) switches into it in place. Nothing hostbuilds cix.**

1. **`mkbootroot` ships in the `cixd` output,** at `usr/libexec/cix/mkbootroot`. The assembly runs the copy in the `cix-root` version it is sealing, so the release being sealed seals itself (ADR-0057). It runs as the hostbuild copy runs today: exec'd from its path, against the running root's own loader and libraries. It is not given the tree's libraries, because the running loader with the tree's `libc.so.6` would be two glibc builds mixed, and a glibc roll is exactly when the two differ.
2. **The installer's tools ship in a new output, `cix-installer-tools`:** `mkinstalleriso`, `cix-install`, `cix-recover`, `cix-boot.efi` and `cix-xorriso`, under `usr/libexec/cix/`.
   - The `iso-builder` image's recipe declares the output, as ADR-0358 says of every tool the ISO needs, and `POST /v1/system/iso` reads them from that image's current tree.
   - **The installer and the root must be one release.** An ISO build refuses with a 409 when `iso-builder`'s `cix-installer-tools` and `cix-root`'s `cixd` are different versions, naming both. Today one hostbuild guarantees it.
   - **The ISO's release is that version,** for its signature and its disk GUID alike. They used to derive from two different releases: the GUID from the running daemon, the signature from the hostbuild.
   - **Release A compares with `cix-root`'s installed `cixd`,** which is not quite the right comparator: the media carries the last *assembled* root, and after B that can lag `cix-root` (with `root: window`, or a deferred roll). Release B compares with the release the assembled root itself names, so an ISO can never stamp N+1 on an N root (#434's failure).
3. **`cix-root`'s recipe makes `cixd` and `cixctl` rolling.**
   - The bridge's cix rows go: `g_root_outputs[]` keeps only `kernel-modules`, and `root_image_behind()` asks only about the kernel.
   - The two are separate installs. The assembly refuses a `cix-root` whose `cixd` and `cixctl` are different versions, and names them. The image is not settled while either is queued (ADR-0374), so this is a guard, not a wait.
4. **The roll decides on the kernel and the root.**
   - "cix behind" and "cix not running" leave `hostroll_parts_read()`. The running cix is behind exactly when the root is.
   - **A published but unbuilt kernel holds nothing.** A root-only roll needs only `cix-root`'s modules to be the running kernel's, which fails only while a built kernel waits for its reboot. Before this, `rootroll_tick()` also returned while a newer kernel was merely published, and discovery publishes kernels hourly: 7.3.5 arrived in the middle of a roll on 2026-11-17. A release would have waited for 03:00 whenever a kernel was out.
5. **A rejected release stays off.**
   - ADR-0379 marks a `cix-root` version bad, and any other move clears the mark. With `cixd` rolling, that means rejecting release N and then publishing anything else, cbs say, would deploy N again.
   - So a bad or rejected root also records the `cixd` version it held. A `cix-root` whose `cixd` is that version is not deployed, whatever else moved, until `cixd` moves or an operator runs the roll by hand. That holds for the window's roll too, which assembles whatever `cix-root` holds: it stops before staging such a root.
   - **A bad mark never names the `cixd` the recording daemon runs:** that `cixd` is evidently not what failed. Both slots hold one `cixd` after a root-only roll that moved a library, so without this a failed switch of such a roll, or a boot that fell back from it, would blame the running release and hold every later root until the next one. For the same reason, when the running slot confirms with the marked `cixd`, only the version stays bad.
   - **The operator's rollback is two steps:** `boot-next` to the previous slot, and nothing more. The rejection rule holds the release off, and the next release, or a hand-run roll, brings a root back. CLAUDE.md's #589 entry, where a published bad release redeployed itself unattended, is the failure this prevents.
6. **No second route.** Never hostbuild cix to deploy it, and never install a release into `cix-root` by hand. The main `cix` package keeps nothing a deploy reads.

## How it gets there: two releases, no fallback

Each is deployed by the path its predecessor already has.

- **Release A** (deployed by 692's roll: hostbuild, bridge, assembly with the hostbuild's `mkbootroot`):
  - **Code:**
    - the assembly takes `mkbootroot` from `usr/libexec/cix/mkbootroot` in the `cix-root` tree it seals;
    - the ISO reads its tools from `iso-builder`'s tree and refuses a skewed pair;
    - the bad mark records `cixd`;
    - a published kernel holds nothing.
  - **Recipe:**
    - adds `usr/libexec/cix/mkbootroot` to the `cixd` output and creates `cix-installer-tools`;
    - the main package keeps its six top-level copies for 692's hostbuild. #553 is per path, and these are different paths.
  - **`iso-builder`'s recipe** gains `cix-installer-tools`.
- **Release B** (deployed by A's roll, which still hostbuilds and bridges):
  - **Code:** drops the cix bridge rows and the cix reasons.
  - **Recipe:** drops the main package's six copies.
  - **Then** `cix-root`'s recipe makes `cixd` and `cixctl` rolling, a cix-recipes commit, made after B is live.
  - The release after B is the first deployed by publishing alone.

## What this does not change

- **The kernel** is hostbuilt at 03:00, its modules are bridged into `cix-root`, and a new kernel reboots (ADR-0327).
- **The agent image's rolling `cixctl`.** It becomes one of two consumers of the drain's single cix build, `cix-root` being the other.
- **ADR-0379's rules** all hold: bad versions, the hold on `host-roll`, the `root` setting and the owed roll.

## Where an operator watches a release

- **While it builds:** `cixctl pkg source-catalogue` shows `cix` installing, and `cixctl pkg ls` shows `cixd`'s and `cixctl`'s rows in `cix-root`.
- **If it fails to build:** those rows carry the error, and `cix-root` does not move. The schedule line keeps reading `current`, which is true: nothing new is ready.
- **Once it is built:** the schedule line follows the root-only roll, from `root: cix-root X -> Y` to `switched in place to <release>` (#692).
- **If it is refused:** `cixctl boot` shows `root_image_failed`.

## Consequences

- **A release is publishing `cix@N`.** With `root: settled` it is live minutes after its build, by an in-place switch. With `root: window` it waits for 03:00.
- **One build per release, always:** the drain's. #691's measured cache hit becomes the only build rather than a coincidence of ordering.
- **A broken release** costs at most one reboot (ADR-0379), and stays off until `cixd` moves or an operator asks.
- **The contract:** `GET /v1/system/assembly`'s `waiting_for` stops naming cix builds, and the ISO's refusal is a new 409. `docs/api/openapi.yaml` and `README.md` change with A and B.
- **CLAUDE.md's release instructions change with B:** publish, then watch the root-only roll. The "run the roll by hand right after publishing" rule goes.

## Live checks

- **Release A:**
  - the next root-only roll seals with `cix-root`'s `usr/libexec/cix/mkbootroot`, and its assembly log names that path;
  - an ISO build succeeds from `iso-builder`'s tree, and refuses when `cix-installer-tools` is held back on purpose;
  - with a kernel published but not built, a root-only roll still runs.
- **Release B:** the release after it is published and nothing else is done. Its row builds once, `cix-root` moves, a root-only roll switches into it, and `cixctl boot` reports the new release. No hostbuild of cix runs.
- **The rejection rule:**
  1. `boot-next` back from a release;
  2. publish an unrelated package into `cix-root`'s closure;
  3. expect no root-only roll, and `root_image_failed` naming the rejected root.
