# Building and rolling out a kernel update

A complete runbook: producing a kernel image, then rolling it onto a running install through the A/B slot mechanism, and confirming it actually stuck. This is the one guide that ties those three steps together — [`docs/api/README.md`](../api/README.md#host--package-updates) documents the `/system/update` endpoint itself, and ADR-0014/ADR-0031/ADR-0032 document the *why* behind A/B slots and per-slot kernels; this page is the operator-facing "how do I actually do this" walkthrough neither of those is.

## Background: how A/B kernel updates work here

Cix boots from one of two symmetric slots (`cix-root-a`/`cix-root-b`), each with its **own** kernel file pre-staged on the ESP at install time (`cix-bzImage-a`/`cix-bzImage-b`) — a kernel update always targets the *inactive* slot (whichever one this daemon is **not** currently running as), never the live one. `POST /system/update` writes the new kernel there and stages a fresh systemd-boot loader entry with a fresh **Automatic Boot Assessment** tries-left counter (`ROOT_UPDATE_TRIES = 3`). Nothing takes effect until you explicitly reboot into that slot — writing and booting are two separate, deliberate steps (ADR-0031). Once the newly-booted daemon reaches a genuinely healthy, serving state, it automatically renames its own loader entry to drop the tries-left counter — that's the actual "this slot is confirmed good" signal, and it needs no operator action. If a freshly-updated slot instead fails to boot to health three times in a row, systemd-boot's own native counter falls back to the previous good slot by itself — the old kernel and root are untouched the whole time, so a bad update is always recoverable by nature of A/B, not by a script hoping to undo damage after the fact.

## Step 0: decide which version you are moving to

Which kernel *line* this box tracks is a real setting (issue #65), not something to work out by hand each time:

```sh
cixctl kernel-policy refresh          # ask kernel.org what each channel is at
cixctl kernel-policy show
cixctl kernel-policy set --channel=longterm
```

It reports the running kernel, the version your channel currently points at, and whether you are behind it — resolved from kernel.org's own `releases.json`, so nothing here has to be kept up to date by hand. It deliberately stops there: it never rewrites the recipe pin, and the version you build below is still yours to choose. See [`docs/api/README.md`'s "Kernel line"](../api/README.md#kernel-line-issue-65) for why, including how `longterm` resolves when kernel.org lists six longterm lines at once.

## Step 1: get a kernel image

Two ways to produce a `bzImage`; both use the exact same source version and kernel config fragment (`image/kernel/qemu-part1.config`), so they produce equivalent kernels.

### On a dev machine

```sh
curl -sSL -o linux.tar.xz https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz
tar xf linux.tar.xz && cd linux-6.18.40
make allnoconfig ARCH=x86_64
./scripts/kconfig/merge_config.sh -m .config ../image/kernel/qemu-part1.config
make olddefconfig ARCH=x86_64
make -j$(nproc) ARCH=x86_64 bzImage
make -j$(nproc) ARCH=x86_64 modules
cp arch/x86/boot/bzImage <repo>/build/bzImage
make ARCH=x86_64 INSTALL_MOD_PATH=<repo>/build modules_install
depmod -b <repo>/build "$(make -s ARCH=x86_64 kernelrelease)"
```

A real GCC toolchain, not TCC — this is unmodified upstream software, not this project's own code, so the TCC mandate doesn't apply to it (same split as any other real software this platform runs as a workload rather than authors itself). The last two lines (Part 3, bare-metal-readiness plan) harvest a real `.ko` tree for the `=m` drivers `image/kernel/qemu-part1.config` enables (a curated set of common real-hardware NICs/USB controllers, see ADR-0061 for exactly which and why) — needs a real `depmod` (`kmod`, any distro package or `recipes/package/kmod/`) on this dev machine's own `PATH`.

### Self-hosted, from a running Cix box

Using the [hostbuild](writing-recipes.md#the-hostbuild-variant) mechanism against `recipes/package/kernel/`, which reproduces the identical sequence above (including the modules build + a real `depmod`) inside a build container:

`kernel.recipe`'s own `pkg_source` fetches both the kernel tarball and `image/kernel/qemu-part1.config` host-side, before the build container starts — the config by its **pinned Gitea raw URL** (`.../raw/image/kernel/qemu-part1.config?ref=<commit>`), so the exact config a given kernel version was built against is fixed in the recipe and reviewable in git, the same as any other `pkg_source` entry. Nothing has to be served locally:

```
cixctl pkg hostbuild kernel --build-image=<a gcc+kmod image> --wait
```

> Earlier revisions of this recipe fetched that config from a scratch
> `http://127.0.0.1:8901/` server the operator had to start by hand, and this
> guide told you to run `python3 -m http.server 8901` first. That step is gone
> — it predates the recipe being pinned to a real Gitea ref, and running it
> today serves a port nothing reads.

`kernel-builder` is the image whose one job this is ([ADR-0208](../adr/0208-build-image-taxonomy.md)): a real GCC toolchain plus `kmod`, because the recipe compiles with `CC=/usr/bin/gcc` and finishes with a real `depmod`, along with the `bc`/`bison`/`flex`/`elfutils` the kernel's own build genuinely reaches for. It is the one build image that legitimately holds gcc — everything else this project builds is TCC-only by ADR-0001.

> This guide used to say `--build-image=dev`, and `dev`'s manifest held neither `gcc` nor `kmod` — so the documented self-hosted kernel build could not work as written. That is what prompted ADR-0208: `dev` named no job, so it got used for one it could not do.

Once `--wait` returns with `state: "installed"`, the finished `bzImage` is at that job's own `artifact_path` (`GET /pkg/hostbuild/kernel`), alongside a real `lib/modules/<kernelrelease>/` tree in the same artifact directory — `build/mkbootroot`'s own `<modules-dir>`/`<kmod-bin-dir>` arguments (see [`installing.md`](installing.md#building-the-iso)) stage both onto a real control-plane squashfs, so `cixd`'s own boot-time `modprobe` (ADR-0061) has something real to load on an installed system.

**Need a driver that isn't in the curated `=m` set at all?** (ADR-0159 Phase B) — `cixctl kmod-build --build-image=kernel-builder --symbol=CONFIG_DUMMY --wait` is the exact same `pkg hostbuild kernel` call above, gaining a `--symbol=` flag (repeatable) that merges extra `CONFIG_*` symbols into the same curated config, each forced to `=m`. No new mechanism, no persistent kernel-build-tree kept around between builds — deliberately not that, per [`docs/api/README.md`](../api/README.md#building-an-extra-kernel-module-adr-0159-phase-b)'s own note on the simpler design that was chosen instead. Applying the result is identical to any other kernel update: `cixctl update --kernel=<artifact_path>/bzImage` then a reboot onto the inactive slot (Step 2 below) — there is no live, same-boot way to add a module the curated set didn't already build.

## Step 2: write it to the inactive slot

```
cixctl update --kernel=<path-to-bzImage>
```

(or `--image=<path>` too, to update the control-plane squashfs in the same call — see [`staying-updated.md`](staying-updated.md) for that half). This does **not** reboot. `--kernel=` alone leaves the inactive slot's own root squashfs untouched; only the kernel file and the loader entry change.

## Step 3: reboot into it

```
cixctl reboot
```

The machine restarts into whichever slot was just written — systemd-boot picks the freshest loader entry (the one this update just staged) automatically, no manual boot-menu selection needed under normal conditions.

## Step 4: confirm it stuck

There's no explicit "confirm" API call — a healthy daemon confirms itself automatically, per [Background](#background-how-ab-kernel-updates-work-here) above. To verify from the outside:

```
cixctl health
cixctl boot
```

A `200` from `health` means the daemon is up and has already self-confirmed (health-check success is exactly the "genuinely healthy, serving state" condition that triggers the rename). If the box instead comes back up on the *old* kernel with no intervention from you, the new one failed Automatic Boot Assessment three times and the bootloader silently fell back — check the new kernel/config for a real boot failure (serial console output, if you have it attached, is the most direct way to see why) before writing it again.

`GET /system/boot`'s response carries `build_version` (the `git describe` this `cixd` was actually built from), `build_time`, `slot` (`"a"`/`"b"`, or `null` for a dev/test daemon started without `--slot=`), and `kernel_version` (the running `uname -r`) — check these, not just `health`'s `200`, before trusting that a given round trip actually landed: a `200` alone only proves *some* daemon answered, not that it's the one you just wrote, and `slot`/`kernel_version` are the direct answer to "did I boot into the slot and kernel I just wrote." A kernel-only or root-only update auto-fills the other half from the active slot's own currently-running copy (ADR-0095) rather than leaving it stale — see [Doing both kernel and root together](#doing-both-kernel-and-root-together) below for the case where you actually have fresh copies of both to write in one call.

## Doing both kernel and root together

A single `cixctl update --image=<squashfs> --kernel=<bzImage>` call writes both to the same inactive slot in one request — useful when a [self-hosted rebuild](building-cix.md#from-a-running-cix-host-self-hosted-rebuild) has produced a fresh control-plane squashfs at the same time as a fresh kernel, so the two roll out and get confirmed together rather than as two separate reboot cycles.
