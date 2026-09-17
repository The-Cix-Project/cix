# 0291 — The control-plane root is an image

## Status

Proposed. Answers [issue #350](https://git.home.arpa/itdlabs/cix/issues/350). Depends on and composes with [#184](https://git.home.arpa/itdlabs/cix/issues/184) (the library-directory sprawl inside the root) and [#182](https://git.home.arpa/itdlabs/cix/issues/182) (lifecycle domains); references [issue #465](https://git.home.arpa/itdlabs/cix/issues/465) (`pkg_hostbuild_start()` refused non-empty `pkg_depends`, which this design ran directly into — resolved by [ADR-0303](0303-a-hostbuild-carries-pkg-depends-it-does-not-resolve-it.md)). This document is a design proposal for the owner's review, not yet a decision to build from: unlike every other ADR in this corpus, it is being written *before* the work it describes, at #350's own explicit request ("Needs an ADR before code"), and its own sizing is "days of work, not a session."

## Context

`image/src/mkbootroot.c` (1540 lines) assembles the squashfs every installed Cix host boots from. What goes into it is a bespoke, hand-maintained C program, not a declaration:

- `cixd`, `cixctl`, and `web/` come from `ARTIFACTS_DIR/cix/` — the harvest of the most recent successful `cix` hostbuild, three individually-named files/directories copied by absolute path (`daemon/src/main.c`'s `spawn_cix_bootroot_assembly()`, `cixd_bin`/`cixctl_bin`/`web_dir`).
- The kernel modules tree comes from `ARTIFACTS_DIR/kernel/lib/modules`, a fourth independently-resolved harvest.
- Module tools (`modprobe` and friends) come from the `cix-kmod` image's own current version, resolved separately (`resolve_kmod_bin_dir()`).
- Firmware blobs come from the `cix-firmware` image's current version, resolved separately again.
- Every shared library (`.so`) comes from a wholesale sweep of the `cix-hosttools` image's own current version across the five directories `include/libdirs.h` names (#184's own subject).
- The sixteen remaining tools this project has not yet given a from-source recipe — `openssl`, `tar`, `bzip2`, `xz`, `unsquashfs`, the disk tools (`sfdisk`, `mkfs.ext4`, `mkfs.btrfs`, `resize2fs`, `e2fsck`, `btrfs`), `cp`, `gzip`, `mksquashfs` — are copied by absolute path straight from whatever this build host happens to have installed (`shelled_bins[]`, a hardcoded table inside `mkbootroot.c` itself).

Five different sourcing mechanisms, stitched together by ten positional `argv[]` slots main.c computes and mkbootroot.c consumes. This is not a tidiness complaint; three real incidents trace to exactly this shape:

- **A root carrying glibc objects from two different builds panicked the box with `Attempted to kill init!`, twice, while assembly reported success both times** (this file's own environment notes; `verify_platform_libs_intact()` exists only because a hand-maintained table has no way to know it contradicted itself — it is a symptom check bolted onto a structural gap, not a fix for it).
- **ADR-0029's firmware staging had a full implementation and no caller** — `argv[6]` was a hardcoded `""` for some period of this project's history, so a wireless adapter had nowhere for its blob to come from.
- **#347 was the same failure shape for kernel modules** — `argv[7]`/`argv[8]` hardcoded `""`, so no module could ever load. **This is now fixed** (confirmed by reading the current `spawn_cix_bootroot_assembly()`: `firmware_dir`/`modules_dir`/`kmod_bin_dir`/`host_tools_dir` are real, resolved variables today, each independently falling back to `""` only when its own source genuinely has no current version — not hardcoded). #350 cites it as an open casualty; it is closed, and this ADR is not fixing it again. What #347's fix does *not* change is the shape that made the bug possible in the first place: a capability can still be implemented in `mkbootroot.c` and left disconnected, because there is still an `argv[]` position for a caller to forget. The next one won't be caught by re-reading this file; it will be caught by a caller having no slot to leave empty at all, which is what this ADR actually proposes.
- **The owner asked what was in their control plane, and the only available answer was a grep** of a C file, not an API call — every *other* named grouping of packages on this platform (`cix-hosttools`, `cix-firmware`, `cix-kmod`, `base`, every operator-created image) already answers that through `GET /v1/images/<name>`. The control-plane root is the one that does not.

### What already exists to build this on

An "image" (ADR-0107/ADR-0108, `daemon/include/image.h`) is already exactly the mechanism this proposal needs: a name with a persisted `manifest.json` (`{package, mode, version}` entries, `current_version`, and a real version history), and a content-addressed rootfs (`IMAGES_DIR/<name>/<version>/rootfs`, versioned by the sha256 of the resolved manifest string) produced by installing each declared package into it. `cix-hosttools`, `cix-firmware`, and `cix-kmod` already work this way. The control-plane root is the one thing on this platform assembled by absolute-path C code instead.

The other missing piece — "can an ordinary image consume a **hostbuild** package's product, not just an ordinarily-fetched one" — was answered by [ADR-0289](0289-a-hostbuild-package-is-consumed-from-the-cache-like-any-other.md), landed the day before this proposal: once a key-holding host publishes a signed `cix`/`kernel`/`isotools` artifact, any host (including itself) can `pkg install` it into an ordinary image like any other package, no rebuild needed. Without ADR-0289 this proposal would need its own answer to that question; with it, the answer is already there.

## Proposal

**The control-plane root becomes a real image** — call it `cix-boot` — whose manifest declares exactly what today's `shelled_bins[]`/harvest/sweep logic assembles by hand: `cix` (pinned to the version that just hostbuilt), `kernel` (pinned the same way, which brings `lib/modules` as an ordinary part of its own package content rather than a separately-resolved harvest path), `kmod`, the firmware packages (`rtw88-firmware`, `wireless-regdb`), and each of the sixteen remaining helper tools **once each has a real from-source recipe** (tasks #688-693 already track this; #350 itself notes "every package the two issues below remove is one fewer entry in this manifest").

`mkbootroot`'s own job shrinks to: squash `cix-boot`'s current rootfs, plus the structural directories no package should own (`/proc`, `/sys`, `/dev`, `/boot`, `/mnt/cix`, `/config`, `/var/lib/cix`). No `argv[6..9]`, no `shelled_bins[]`, no `verify_platform_libs_intact()` — a manifest resolved by the same `image_produce_new_version()` every other image already uses cannot mix two glibcs by construction, because it is one dependency closure computed once, not five independent copies that can each drift.

Concretely, what this buys:

- **"What is in the control plane" becomes `GET /v1/images/cix-boot`** — an API answer, not a source read.
- **One glibc by construction**, not by a verification pass added after the fact.
- **No absolute paths into the build host** for anything that has a recipe — the Build Provenance Mandate is enforced by the mechanism, not by convention and a wholesale-copy-was-a-real-incident memory (#168, the `cix-builder`-image-shipped-as-`cix-builder` postmortem this file already documents).
- **A capability cannot be implemented and left uncalled**, because there is no positional `argv[]] slot to leave as `""`. Both ADR-0029 and #347 were exactly that failure shape, twice; a manifest has no equivalent gap — a package either is or is not in it, visibly, in one document `GET /v1/images/cix-boot` returns.

## The hostbuild / elfcheck tension (#465) — RESOLVED by ADR-0303, after this was written

**Shape 1 below was taken.** [ADR-0303](0303-a-hostbuild-carries-pkg-depends-it-does-not-resolve-it.md) deleted the refusal: a hostbuild now accepts `pkg_depends` and carries it onto the entry without resolving it, so `cix`'s recipe can declare what `cixd` links (`openssl libarchive curl`) and remain buildable by `pkg hostbuild`. Phase 2 of the migration order below is therefore done, and the rest of this section is kept as the reasoning that led there rather than as an open question.

Making `cix-boot`'s manifest declare `cix` as an ordinary manifest entry means `cix` gets installed into it the ordinary way — `pkg_install_start()`, which runs `elfcheck`'s install-time linkage gate. That gate needs `cix`'s own `pkg_depends` to truthfully list what `cixd` actually links (`openssl`, and since tonight, `libarchive` and `curl` too) or it refuses the install.

`cix`'s `pkg_depends` is empty today, deliberately (#465, discovered live this session): `pkg_hostbuild_start()` refuses **any** recipe with a non-empty `pkg_depends` outright, on the reasoning that dependency resolution targets "merge into an image," which is meaningless for the one-shot `__hostbuild` artifact harvest. So `cix`'s recipe is caught between two paths that want opposite things from the same field: hostbuild wants it empty, an ordinary image install wants it accurate.

This proposal does not have that resolved yet, and it has to be, because `cix-boot`'s own manifest install is exactly the ordinary-image-install path. Two shapes worth weighing when this gets picked up (not a decision made here):

1. Teach `pkg_hostbuild_start()` to *ignore* `pkg_depends` for its own one-shot harvest rather than refusing the whole recipe outright — the field becomes truthful everywhere, and hostbuild simply doesn't resolve/install what it doesn't need to.
2. Keep hostbuild's own gate as-is, and give `cix-boot`'s manifest install a distinct path that installs the already-*verified* hostbuild artifact directly (bypassing `pkg_install_start()`'s normal dependency resolution, similar in spirit to how ADR-0289's cache-consumption path already installs a fetched artifact without re-resolving its build-time dependencies).

Either shape is real work with its own correctness questions (an elfcheck gate that never actually runs against `cix` is not a gate); this ADR names the tension so implementation does not discover it mid-way the way this session discovered #465 itself.

## Consequences

- **This is a phased migration, not a cutover.** A reasonable order: (1) create `cix-boot` as a real image whose manifest is hand-populated to match today's assembled root exactly, verified byte-for-byte against a `mkbootroot`-assembled root before anything depends on it; (2) resolve the hostbuild/elfcheck tension above, since every phase after this one needs it; (3) migrate the firmware and kmod sourcing (already the cleanest, since they already resolve from real images today, just via bespoke C rather than the manifest); (4) migrate `cix`/`kernel`; (5) retire `shelled_bins[]` entries one at a time as #688-693 land real recipes for each tool, not all at once; (6) delete `verify_platform_libs_intact()` and the `argv[6..9]` positional-parameter plumbing only once nothing depends on them.
- **The structural directories (`/proc`, `/sys`, `/dev`, `/boot`, `/mnt/cix`, `/config`, `/var/lib/cix`) stay outside any package's manifest**, deliberately — they are mount points and runtime state, not installed content, and no package should claim to "provide" them.
- **A host with no from-source recipe yet for a given helper tool still needs *some* answer** — until #688-693 land, `cix-boot`'s manifest cannot fully replace `shelled_bins[]` for those entries. The migration should not block on every recipe existing first; a manifest that declares "these N packages, plus these still-dev-host-sourced tools" is still a strict improvement over today's five-mechanism stitching, as long as what remains dev-host-sourced is visibly and honestly marked as such in the manifest or its own tracked gap, not silently reintroduced as a sixth mechanism.
- **This changes how the platform's own root is produced** — the single most foundational build step this project has. Every phase above should be verified live on 192.168.15.95 the same way any boot-path change in this project already must be (this file's own standing rule), not trusted from a green selftest alone; a mistake here is not a bad package, it is a host that does not boot.
