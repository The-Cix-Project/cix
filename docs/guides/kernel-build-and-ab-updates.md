# Building and rolling out a kernel update

A complete runbook: producing a kernel image, then rolling it onto a running install through the A/B slot mechanism, and confirming it actually stuck. This is the one guide that ties those three steps together — [`docs/api/README.md`](../api/README.md#host--package-updates) documents the `/system/update` endpoint itself, and ADR-0014/ADR-0031/ADR-0032 document the *why* behind A/B slots and per-slot kernels; this page is the operator-facing "how do I actually do this" walkthrough neither of those is.

## Background: how A/B kernel updates work here

Kanxeo boots from one of two symmetric slots (`kanxeo-root-a`/`kanxeo-root-b`), each with its **own** kernel file pre-staged on the ESP at install time (`kanxeo-bzImage-a`/`kanxeo-bzImage-b`) — a kernel update always targets the *inactive* slot (whichever one this daemon is **not** currently running as), never the live one. `POST /system/update` writes the new kernel there and stages a fresh systemd-boot loader entry with a fresh **Automatic Boot Assessment** tries-left counter (`ROOT_UPDATE_TRIES = 3`). Nothing takes effect until you explicitly reboot into that slot — writing and booting are two separate, deliberate steps (ADR-0031). Once the newly-booted daemon reaches a genuinely healthy, serving state, it automatically renames its own loader entry to drop the tries-left counter — that's the actual "this slot is confirmed good" signal, and it needs no operator action. If a freshly-updated slot instead fails to boot to health three times in a row, systemd-boot's own native counter falls back to the previous good slot by itself — the old kernel and root are untouched the whole time, so a bad update is always recoverable by nature of A/B, not by a script hoping to undo damage after the fact.

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

A real GCC toolchain, not TCC — this is unmodified upstream software, not this project's own code, so the TCC mandate doesn't apply to it (same split as any other real software this platform runs as a workload rather than authors itself). The last two lines (Part 3, bare-metal-readiness plan) harvest a real `.ko` tree for the `=m` drivers `image/kernel/qemu-part1.config` enables (a curated set of common real-hardware NICs/USB controllers, see ADR-0061 for exactly which and why) — needs a real `depmod` (`kmod`, any distro package or `pkg/recipes/kmod.recipe`) on this dev machine's own `PATH`.

### Self-hosted, from a running Kanxeo box

Using the [hostbuild](writing-recipes.md#the-hostbuild-variant) mechanism against `pkg/recipes/kernel.recipe`, which reproduces the identical sequence above (including the modules build + a real `depmod`) inside a build container:

`kernel.recipe`'s own `pkg_source` fetches `image/kernel/qemu-part1.config` from `http://127.0.0.1:8901/qemu-part1.config` (curl'd host-side, before the build container starts, same as the kernel tarball itself) — that address is never automatically served by anything, so start a plain local HTTP server pointed at the config's own directory first, from wherever this daemon's own host filesystem has the repo checked out:

```
cd image/kernel && python3 -m http.server 8901 --bind 127.0.0.1 &
kanxeoctl pkg hostbuild kernel --build-image=dev --wait
```

`--build-image=dev` needs a real image with a working GCC toolchain **and `kmod` (`modprobe`/`depmod`/...)** already installed (this project's own "Phase 33" `dev` image, built up via ordinary `pkg install` calls the same way any build image is — see [`building-kanxeo.md`](building-kanxeo.md#1-build-a-toolchain-image) for the general pattern, substituting `gcc`/`make`/`kmod`/etc. for the TCC-specific set used there). Once `--wait` returns with `state: "installed"`, the finished `bzImage` is at that job's own `artifact_path` (`GET /pkg/hostbuild/kernel`), alongside a real `lib/modules/<kernelrelease>/` tree in the same artifact directory — `build/mkbootroot`'s own `<modules-dir>`/`<kmod-bin-dir>` arguments (see [`installing.md`](installing.md#building-the-iso)) stage both onto a real control-plane squashfs, so `kanxeod`'s own boot-time `modprobe` (ADR-0061) has something real to load on an installed system.

## Step 2: write it to the inactive slot

```
kanxeoctl update --kernel=<path-to-bzImage>
```

(or `--image=<path>` too, to update the control-plane squashfs in the same call — see [`staying-updated.md`](staying-updated.md) for that half). This does **not** reboot. `--kernel=` alone leaves the inactive slot's own root squashfs untouched; only the kernel file and the loader entry change.

## Step 3: reboot into it

```
kanxeoctl reboot
```

The machine restarts into whichever slot was just written — systemd-boot picks the freshest loader entry (the one this update just staged) automatically, no manual boot-menu selection needed under normal conditions.

## Step 4: confirm it stuck

There's no explicit "confirm" API call — a healthy daemon confirms itself automatically, per [Background](#background-how-ab-kernel-updates-work-here) above. To verify from the outside:

```
kanxeoctl health
kanxeoctl boot
```

A `200` from `health` means the daemon is up and has already self-confirmed (health-check success is exactly the "genuinely healthy, serving state" condition that triggers the rename). If the box instead comes back up on the *old* kernel with no intervention from you, the new one failed Automatic Boot Assessment three times and the bootloader silently fell back — check the new kernel/config for a real boot failure (serial console output, if you have it attached, is the most direct way to see why) before writing it again.

`GET /system/boot`'s response carries `build_version` (the `git describe` this `kanxeod` was actually built from), `build_time`, `slot` (`"a"`/`"b"`, or `null` for a dev/test daemon started without `--slot=`), and `kernel_version` (the running `uname -r`) — check these, not just `health`'s `200`, before trusting that a given round trip actually landed: a `200` alone only proves *some* daemon answered, not that it's the one you just wrote, and `slot`/`kernel_version` are the direct answer to "did I boot into the slot and kernel I just wrote." This matters most when a kernel-only or root-only update leaves the *other* file in that slot stale from an earlier cycle — always write `image_path` and `kernel_path` together (see [Doing both kernel and root together](#doing-both-kernel-and-root-together) below) even when only one actually changed, so the pairing in the slot you're about to boot is never a mix of old and new.

## Doing both kernel and root together

A single `kanxeoctl update --image=<squashfs> --kernel=<bzImage>` call writes both to the same inactive slot in one request — useful when a [self-hosted rebuild](building-kanxeo.md#from-a-running-kanxeo-host-self-hosted-rebuild) has produced a fresh control-plane squashfs at the same time as a fresh kernel, so the two roll out and get confirmed together rather than as two separate reboot cycles.
