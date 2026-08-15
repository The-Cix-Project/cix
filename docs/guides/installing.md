# Installing thinC

Building and using the installer ISO — from a set of already-built binaries (`build/thincd`, `build/thinc-install`, a kernel `build/bzImage`, see [`building-thinc.md`](building-thinc.md) and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)) to a real, booted, network-reachable install.

## One-time: the thinC Secure Boot signing key

`image/keys/thinc-signing.{crt,cer}` (the public cert, PEM and DER) are committed; `image/keys/thinc-signing.key` (the private key) is gitignored and must exist locally before building the ISO. Generate it once, and keep it — every machine that has enrolled it (see [Secure Boot](#secure-boot) below) needs the *same* key for future installs to keep working:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 7300 \
    -keyout image/keys/thinc-signing.key -out image/keys/thinc-signing.crt \
    -subj "/CN=thinC Secure Boot Signing Key/O=thinC Project"
openssl x509 -in image/keys/thinc-signing.crt -outform DER -out image/keys/thinc-signing.cer
```

## One-time (optional): AMD GPU firmware, for GPU passthrough

Only needed if you plan to grant a `gpu:N` device to a container (ADR-0028/ADR-0029) — skip this and pass `""` below otherwise. Not vendored into this repo (redistribution/size); fetched fresh from upstream `linux-firmware`, sparse-checked-out to just the `amdgpu/` subtree so only that vendor's blobs (a few hundred MB, not the full multi-vendor firmware set) are actually downloaded:

```sh
git clone --filter=blob:none --sparse \
    https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git \
    /tmp/linux-firmware
cd /tmp/linux-firmware && git sparse-checkout set amdgpu
```

This gives you `/tmp/linux-firmware/amdgpu` — pass that path as `mkbootroot`'s 5th argument below. The kernel's own `CONFIG_DRM_AMDGPU`/`CONFIG_HSA_AMD` drivers load these at boot-time driver-probe, before any container exists, so they have to live on the host root itself, not a container image.

## Building the ISO

This section covers the dev-machine path (`mkbootroot`/`mkinstalleriso` run by hand). A running thinC host can also assemble a fresh ISO itself, no separate dev machine involved — see [`building-thinc.md`'s "Build a fresh installer ISO, server-side"](building-thinc.md#4-build-a-fresh-installer-iso-server-side) (`POST /system/iso` / `thincctl iso build`, ADR-0064). Either path produces the same kind of ISO, described below.

Once `build/thincd`, `build/thinc-install`, `build/thinc-recover`, `build/bzImage`, and the signing key above all exist:

```sh
sudo build/mkbootroot  /tmp/root_stage build/thincd build/thincctl web /tmp/thincd-root.squashfs \
     /tmp/linux-firmware/amdgpu \  # or "" to skip GPU firmware entirely
     /path/to/kernel-hostbuild-artifact/lib/modules \  # or "" to skip kernel modules
     /path/to/kmod-usr-bin                             # or "" to skip modprobe/depmod/etc
sudo build/mkinstalleriso build/iso_stage build/thinc-install build/thinc-recover build/bzImage \
     /tmp/thincd-root.squashfs \
     image/keys/thinc-signing.key image/keys/thinc-signing.crt image/keys/thinc-signing.cer \
     build/thinc-install.iso \
     "--disk=/dev/CHANGEME --ip=CHANGEME --prefix=24 --gateway=CHANGEME --interface=CHANGEME --auto-partition"
```

The generated media carries a second GRUB entry, "thinC Recovery" — a break-glass tool for resetting host-auth admin_groups on an already-installed system if you're ever locked out of the API; see [`security.md`'s recovery section](security.md#break-glass-recovery) and [ADR-0146](../adr/0146-ldap-startup-resync-and-break-glass-recovery.md). It never reformats or reinstalls anything, so keeping this same ISO around after a normal install is worthwhile on its own.

This produces `build/thinc-install.iso` — attach it as a CD-ROM/optical drive to a VM (or a real machine) and boot from it. The placeholder args are deliberate: at the GRUB boot menu (it waits 10s before auto-booting, giving you a real chance to interrupt it), press `e` to edit the boot entry, replace `/dev/CHANGEME`/`CHANGEME`/`CHANGEME`/`CHANGEME` with the real target disk, IP address, gateway, and physical interface name (e.g. `eth0` — check `ls /sys/class/net` from a rescue shell if you're not sure which one is which) for this install, then `Ctrl-X` to boot. If you forget, `thinc-install`'s own disk check fails safely — it refuses to touch a disk that doesn't exist rather than silently doing the wrong thing.

**Why an interface name at all**: this is a one-time bootstrap value only, used to attach a physical NIC to the `management` network `thincd` binds to at first boot — it does *not* need to be perfect. If it's wrong, or your NIC layout changes later, the management network is an ordinary, API-managed `network_def` afterward (`GET /v1/networks`) and can be repointed to a different interface without reinstalling.

**Kernel modules** (Part 3 of the bare-metal-readiness plan, ADR-0061): the two new `mkbootroot` arguments above stage a real kernel module tree and `kmod`'s own tools onto the installed system, so `thincd`'s own boot-time `modprobe` (a curated set of common real-hardware NICs/USB controllers — see the ADR for exactly which) has something real to load on hardware whose driver isn't built directly into the kernel. Both come from real thinC build artifacts, not anything fetched by `mkbootroot` itself: `/path/to/kernel-hostbuild-artifact/lib/modules` is `kernel.recipe`'s own hostbuild output (`thincctl pkg hostbuild kernel --build-image=dev --wait`, see [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)); `/path/to/kmod-usr-bin` is wherever `recipes/package/kmod/` was installed (e.g. extracted from a real image's own `usr/bin/{kmod,modprobe,depmod,...}`). Omit both (`""`) for a QEMU/CI boot test, same as GPU firmware above — no real target hardware means nothing needs a driver module at all.

**Target disk**: **VirtIO Block** disks (`/dev/vda`) and real **SATA/PATA** disks (`/dev/sda`, `CONFIG_BLK_DEV_SD`, ADR-0061/Part 3) both work — NVMe (`/dev/nvme0n1`) and software RAID (`/dev/mdN`) too. VirtIO-SCSI-attached disks specifically are not yet covered (no `CONFIG_SCSI_VIRTIO`). On Proxmox, attach the target disk with Bus/Device: `VirtIO Block` (simplest) or `SATA`.

**Partitioning** — three ways, pick one via `thinc-install`'s own flags (baked into the last argument above):

- **`--auto-partition`** (recommended for VMs / scripted provisioning): partitions the disk itself, non-interactively, with the standard fixed layout below — no typing required. This is what the example above uses, and what `test/test_installer.c`'s own install session actually exercises.
- **(no flag, the default)**: drops into a real, interactive `fdisk` session — use this if you need different partition sizes than the standard layout. GPT, five partitions, named exactly (`thinc-esp`, `thinc-root-a`, `thinc-root-b`, `thinc-config`, `thinc-containers` — see `docs/roadmap/ROADMAP.md`'s Phase 11 part 3 write-up for the role each one plays):

  ```
  g                                  # new GPT label
  n  1    +64M                       # thinc-esp   (partition 1, default first sector, then Enter for each)
  n  2    +160M                      # thinc-root-a
  n  3    +160M                      # thinc-root-b
  n  4    +64M                       # thinc-config
  n  5                               # thinc-containers (rest of the disk -- Enter twice, no size)
  t  1  1                            # partition 1 -> EFI System type
  x                                  # expert menu, to set partition names
  n  1  thinc-esp
  n  2  thinc-root-a
  n  3  thinc-root-b
  n  4  thinc-config
  n  5  thinc-containers
  r                                  # back to the main menu
  w                                  # write and exit
  ```

  (Each `n  <number>  <size>` above is really three separate prompts: partition number, first sector — just press Enter for the default — then last sector/size, where you type `+64M` etc., or Enter alone for partition 5's "rest of the disk".)

- **`--skip-partition`**: the disk is already partitioned correctly by other means (e.g. scripted provisioning that ran `sfdisk` itself beforehand) — `thinc-install` just reads the existing table back.

`--skip-partition` and `--auto-partition` are mutually exclusive; omitting both means interactive `fdisk`.

It then formats, writes the system, and reboots into a running `thincd` at the IP you gave it — reachable at that address directly (`thincd` binds to the exact IP given via `--ip=`, not just loopback).

**Console login**: the installed system drops straight into an interactive `thincctl` shell on both the video console and the serial console once boot completes — no username, no password (this platform has no authentication anywhere yet; physical console access is already at least as privileged as the unauthenticated network API). Typing `exit`/`quit`/Ctrl-D ends the session and a fresh one starts automatically a couple of seconds later.

## Changing the management network, port, or enabling HTTPS after install

The `--ip=`/`--gateway=`/`--interface=` values above are a one-time bootstrap only — everything they set up is a real, ordinary, API-managed network named `management` (`GET /v1/networks`), and `thincd`'s own listen port/HTTP/HTTPS exposure is a small, dedicated, live-reconfigurable resource, `GET`/`PUT /v1/system/daemon-config` (see [`docs/api/README.md`](../api/README.md#the-management-network-and-thincds-own-listeners) for the full contract). Also reachable from the web dashboard's System > Host > Daemon page, or directly with `thincctl daemon-config` (see [`docs/guides/cli-reference.md`](cli-reference.md)):

```sh
thincctl --host=<install-ip> daemon-config show
thincctl --host=<install-ip> daemon-config set --port=8080
thincctl --host=<install-ip> daemon-config set --enable-https
thincctl --host=<install-ip> daemon-config set --management-network=lan1
```

Or, equivalently, straight `curl` (every `thincctl` subcommand is exactly one HTTP call, per the API-First Mandate):

```sh
curl http://<install-ip>:80/v1/system/daemon-config
curl -X PUT http://<install-ip>:80/v1/system/daemon-config \
     -d '{"port": 8080}'
curl -X PUT http://<install-ip>:80/v1/system/daemon-config \
     -d '{"https_enabled": true}'
curl -X PUT http://<install-ip>:80/v1/system/daemon-config \
     -d '{"management_network": "lan1"}'
```

Every change here is applied live (no reboot, no restart — `thincd` runs as real PID 1 on an installed system, so there is no restart to fall back on) and persisted, so it survives a real one too. Enabling HTTPS needs a bootstrapped PKI root CA first (`POST /v1/pki/ca`) — it reuses the already-issued `"host"` leaf certificate rather than a separate cert. Repointing `management_network` needs the target network to already have its own address (`has_address: true`, e.g. created via `POST /v1/networks` with an `address` field, or another network attached to a physical NIC via `POST /v1/networks/{name}/interfaces`) — double-check you can actually reach the new address before relying on it, since a mistake here has no remote undo, only physical console access (above).

## Secure Boot

Also requires **UEFI firmware and a Q35 machine type** — thinC is UEFI-only (see `docs/roadmap/ROADMAP.md`'s Phase 11 architecture decisions); a legacy-BIOS/i440fx VM has no AHCI CD-ROM controller for this kernel to find and will panic trying to mount root. On Proxmox: VM → Hardware → BIOS → `OVMF (UEFI)`; VM → Options → Machine → `q35`.

The **installer media itself** always needs Secure Boot **off** for its own one-time, unsigned boot — no different from installing most non-Windows OSes from scratch (see ADR-0015 for why this can't be avoided). Concretely, on Proxmox: when adding the VM's EFI Disk, leave **"Pre-Enroll keys" unchecked** — if Secure Boot's keys are already enrolled before this first boot, it fails with `Access Denied`.

The **installed system** is Secure-Boot-capable, but getting there needs one real, in-order sequence:

1. `thinc-install` signs its own boot chain with the thinC key above and, during install, stages a one-time key-enrollment request — you'll be asked to set a temporary password.
2. Before letting it reboot into the installed system, enter the firmware's own setup screen (on Proxmox/OVMF: interrupt at the Tianocore splash, usually `Esc`) → **Device Manager → Secure Boot Configuration** → enable Secure Boot / "Enroll Default Secure Boot Keys" (exact wording varies by OVMF build) → save and reset. This is the step that actually turns Secure Boot on for this machine, using the *same* EFI vars store that already has the pending key request from step 1 — don't recreate the EFI Disk to do this, that would discard the pending request.
3. On that reset, `shim` (Microsoft-signed, always trusted) detects the pending request and shows its own **MokManager** screen: **press any key** (there's a ~10s countdown — miss it and it falls straight through to a boot attempt that correctly fails with `Verification failed: (0x1A) Security Violation`, since nothing got enrolled) → from the main menu, arrow down to **"Enroll MOK"** (*not* "Continue boot" — see the warning below) → **"Continue"** (a *different* "Continue", inside the Enroll-MOK submenu, confirming you want to proceed past viewing the key) → **"Yes"** → enter the *same* password from step 1 → back at the main menu, now offering **"Reboot"** as the top entry — select it.
4. From then on, Secure Boot stays on with zero further prompts on that machine.

**⚠️ Do not select "Continue boot"** at that main MokManager menu, even by mistake — confirmed directly: picking it doesn't just skip the prompt for this boot, it **permanently discards** the pending enrollment request. Every later boot's menu will be missing the "Enroll MOK" option entirely, and there's no way to get it back short of a full reinstall (a fresh `mokutil --import` needs a fresh install run — `thinc-install` has no standalone "just do enrollment" mode).

If this already happened to you, there's a working recovery that doesn't need a reinstall: **hash-enroll the two boot-chain files directly**, via the same MokManager menu's **"Enroll hash from disk"** option — do this for *both* files, rebooting only after both are done:
1. `\EFI\BOOT\grubx64.efi`
2. `\thinc-bzImage`

This trusts those exact files by hash rather than by the thinC signing key, so it's narrower than the cert-based path (a future kernel rebuild or reinstall changes the hashes and needs re-enrolling) but gets you unblocked immediately.

## What's next

Once booted, the box is reachable at the IP you gave it — see [`quickstart.md`](quickstart.md) for the fastest path to a first running container, [`cli-reference.md`](cli-reference.md)/[`web-dashboard.md`](web-dashboard.md) for day-to-day usage, and [`staying-updated.md`](staying-updated.md) for how to keep it current.
