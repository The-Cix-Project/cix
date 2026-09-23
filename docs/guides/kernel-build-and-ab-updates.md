# Building and rolling out a kernel update

A complete runbook: building a kernel on a Cix host, rolling it onto a running install through the A/B slot mechanism, and confirming it stuck. [`docs/api/README.md`](../api/README.md#host--package-updates) documents the `/system/update` endpoint itself, and ADR-0014/ADR-0031/ADR-0032 document the *why* behind A/B slots and per-slot kernels; this page is the operator's "how do I do this".

## Background: how A/B kernel updates work here

Cix boots from one of two symmetric slots (`cix-root-a`/`cix-root-b`), each with its **own** kernel file on the ESP (`cix-bzImage-a`/`cix-bzImage-b`). An update always targets the *inactive* slot — whichever one this daemon is **not** running from. `POST /system/update` writes the new kernel there and stages a fresh systemd-boot loader entry with a fresh **Automatic Boot Assessment** tries-left counter (`ROOT_UPDATE_TRIES`, 3). Nothing takes effect until you reboot into that slot — writing and booting are two separate steps ([ADR-0031](../adr/0031-host-and-package-update-mechanism.md)).

The new slot confirms itself. When the booted daemon has its listeners up, its uplink attached and its management address bound, it renames its loader entry to drop the tries-left counter (`maybe_confirm_boot()` in `daemon/src/main.c`). If the uplink or the management address is missing, an unconfirmed slot retries for 120 seconds and then reboots without confirming, spending a try so the loader falls back; a slot already confirmed on an earlier boot is kept, never rebooted over this. A slot that never reaches that point fails its tries and systemd-boot falls back to the previous slot on its own. The old kernel and root are untouched throughout, so a bad update is recoverable by the nature of A/B.

## Step 0: decide which version you are moving to

Which kernel *line* this box tracks is a setting (issue #65):

```sh
cixctl kernel-policy refresh          # ask kernel.org what each channel is at
cixctl kernel-policy show
cixctl kernel-policy set --channel=longterm
```

It reports the running kernel, the version your channel currently points at, and whether you are behind it — resolved from kernel.org's own `releases.json`. It never rewrites the recipe pin: the version you build is chosen by the `kernel` recipe you publish. See [`docs/api/README.md`'s "Kernel line"](../api/README.md#kernel-line-issue-65) for how `longterm` resolves when kernel.org lists several longterm lines at once.

## Step 1: build the kernel on the host

The kernel is a [hostbuild](writing-recipes.md#the-hostbuild-variant) of the `kernel` recipe, which lives in [cix-recipes](https://git.home.arpa/itdlabs/cix-recipes) as `recipes/package/kernel@<version>.sh`. Its `pkg_source` fetches both the kernel tarball and this repository's `image/kernel/qemu-part1.config`, host-side, before the build container starts — the config by a **pinned Gitea raw URL** (`.../raw/image/kernel/qemu-part1.config?ref=<commit>`), so the exact config a kernel version was built against is fixed in the recipe and reviewable in git.

```
cixctl pkg hostbuild kernel --wait
```

Add `--upgrade` when a `kernel` hostbuild is already installed on the host; without it the call answers `409`. No image is named: the build container is composed from the recipe's own `pkg_build_depends` ([ADR-0304](../adr/0304-a-hostbuild-composes-its-build-environment-like-every-other-build.md)) — gcc and binutils, `kmod` for the final `depmod`, the `bc`/`bison`/`flex`/`elfutils`/`perl` the kernel build reaches for, and `wireless-regdb` because `CONFIG_EXTRA_FIRMWARE` embeds the regulatory database. The kernel is built with gcc (`pkg_toolchain="gcc"`), as a third-party package may be ([ADR-0226](../adr/0226-gcc-is-an-ordinary-choice-for-third-party-packages.md)).

The build runs the ordinary kernel sequence against that config — `allnoconfig`, merge `qemu-part1.config`, `olddefconfig`, `bzImage modules`, `modules_install`, `depmod` — plus any source patches the recipe carries, each asserted by the build. The `=m` drivers the config enables are a curated set of common NICs and USB controllers ([ADR-0061](../adr/0061-kernel-module-loading.md)).

When `--wait` returns with `state: "installed"`, the finished `bzImage` is at that job's `artifact_path` (`GET /pkg/hostbuild/kernel`), alongside a `lib/modules/<kernelrelease>/` tree in the same artifact directory. `mkbootroot`'s `<modules-dir>`/`<kmod-bin-dir>` arguments (see [`installing.md`](installing.md#building-the-iso)) stage both into a control-plane root, so `cixd`'s boot-time `modprobe` has something to load.

`build-inputs/bzImage`, which the installer ISO and the boot tests take as an input, is the same kernel: take it out of the `kernel` package artifact a Cix host already published to the cache (`tar xzf kernel-<ver>.tar.gz ./bzImage`) rather than building one anywhere else.

**Need a driver that isn't in the curated `=m` set?** ([ADR-0159](../adr/0159-api-driven-kernel-module-management.md) Phase B) `cixctl kmod-build --symbol=CONFIG_DUMMY --wait` is the same `pkg hostbuild kernel` call, with a repeatable `--symbol=` that merges extra `CONFIG_*` symbols into the curated config, each forced to `=m`. See [`docs/api/README.md`](../api/README.md#building-an-extra-kernel-module-adr-0159-phase-b) for the design. Applying the result is an ordinary kernel update (Step 2); there is no same-boot way to add a module the curated set did not build.

## Step 2: write it to the inactive slot

```
cixctl update --kernel=<artifact_path>/bzImage
```

(or `--image=<path>` too, to update the control-plane squashfs in the same call — see [`staying-updated.md`](staying-updated.md) for that half). This does **not** reboot. `--kernel=` alone pairs the new kernel with a fresh copy of the root the active slot is running ([ADR-0095](../adr/0095-update-one-sided-footgun.md)), so neither half is left stale.

## Step 3: reboot into it

```
cixctl reboot
```

The machine restarts into the slot just written, with no boot-menu selection needed. Before rebooting, `cixctl esp show` (`GET /system/esp`, [ADR-0202](../adr/0202-the-esp-is-reachable-over-rest.md)) reports `selected_entry`, the entry systemd-boot will actually boot next; if it is not the one this update staged, a stale `default` pattern is outranking it, and `cixctl esp set --default=PATTERN` corrects it.

To boot a specific slot **once** — to try a slot, or to roll back to the previous one — arm it before rebooting:

```
cixctl boot-next b             # or a; `boot-next clear` disarms, no argument reports
cixctl reboot
```

`boot-next` reverts to normal selection after that one boot. Pinning the loader default to a slot instead is sticky and breaks the next update, which stages the other slot.

## Step 4: confirm it stuck

There is no explicit "confirm" call — the booted daemon confirms itself, per [Background](#background-how-ab-kernel-updates-work-here). To verify from the outside:

```
cixctl health
cixctl boot
```

A `200` from `health` over the management address means the daemon reached its listeners with that address bound, which is the point at which it confirms. `cixctl boot` (`GET /system/boot`) reports `build_version` (the `git describe` this `cixd` was built from), `build_time`, `slot` (`"a"`/`"b"`, or `null` for a daemon started without `--slot=`) and `kernel_version` (the running `uname -r`). Check `slot` and `kernel_version`, not just `health`: a `200` alone proves only that *some* daemon answered.

If the box comes back on the *old* kernel with no intervention from you, the new slot failed its boot assessment and the loader fell back. Find out why before writing it again — a serial console, if one is attached, is the most direct way to see a boot failure.

## Doing both kernel and root together

A single `cixctl update --image=<squashfs> --kernel=<bzImage>` writes both to the same inactive slot in one request — useful when a [self-hosted rebuild](building-cix.md#rebuilding-cix-on-a-running-host) has produced a fresh control-plane squashfs at the same time as a fresh kernel, so the two roll out and are confirmed together rather than as two reboot cycles.
