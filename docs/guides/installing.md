# Installing Cix

Building and using the installer ISO — from a set of already-built binaries (`build/cixd`, `build/cix-install`, a kernel `build-inputs/bzImage`, see [`building-cix.md`](building-cix.md) and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)) to a real, booted, network-reachable install.

## One-time: the Cix Secure Boot signing key

`image/keys/cix-signing.{crt,cer}` (the public cert, PEM and DER) are committed; `image/keys/cix-signing.key` (the private key) is gitignored and must exist locally before building the ISO. Generate it once, and keep it — every machine that has enrolled it (see [Secure Boot](#secure-boot) below) needs the *same* key for future installs to keep working:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 7300 \
    -keyout image/keys/cix-signing.key -out image/keys/cix-signing.crt \
    -subj "/CN=Cix Secure Boot Signing Key/O=Cix Project"
openssl x509 -in image/keys/cix-signing.crt -outform DER -out image/keys/cix-signing.cer
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

This section covers the dev-machine path (`mkbootroot`/`mkinstalleriso` run by hand). A running Cix host can also assemble a fresh ISO itself, no separate dev machine involved — see [`building-cix.md`'s "Build a fresh installer ISO, server-side"](building-cix.md#4-build-a-fresh-installer-iso-server-side) (`POST /system/iso` / `cixctl iso build`, ADR-0064). Either path produces the same kind of ISO, described below.

Once `build/cixd`, `build/cix-install`, `build/cix-recover`, `build-inputs/bzImage`, and the signing key above all exist:

```sh
sudo build/mkbootroot  /tmp/root_stage build/cixd build/cixctl web /tmp/cixd-root.squashfs \
     /tmp/linux-firmware/amdgpu \  # or "" to skip GPU firmware entirely
     /path/to/kernel-hostbuild-artifact/lib/modules \  # or "" to skip kernel modules
     /path/to/kmod-usr-bin                             # or "" to skip modprobe/depmod/etc
sudo build/mkinstalleriso build/iso_stage build/cix-install build/cix-recover \
     build/cix-boot.efi build-inputs/bzImage \
     /tmp/cixd-root.squashfs \
     image/keys/cix-signing.key image/keys/cix-signing.crt image/keys/cix-signing.cer \
     build/cix-install.iso \
     "" \
     /path/to/isotools-artifact \
     /path/to/seed                                     # or "" for no package seed
```

The last two arguments are worth a word. **isotools-root** is the harvested `isotools` hostbuild artifact (ADR-0064) holding `grub-mkrescue`, `xorriso`, `sbsign`, `mokutil`, shim and their libraries — it is an argument rather than something read off the build machine precisely so this tool can run on a real Cix host, which has none of those at Debian's absolute paths. This example previously omitted it altogether and would not have run.

**seed** is optional (`""` for none) and is what makes a freshly installed box able to run a container without a network: a directory of `recipes/` and `artifacts/` copied onto the installed system's own package directories, from which the daemon installs a C library into the default image at first boot (#189). Without it, a fresh install comes up healthy but its default image has no C library, and `POST /v1/containers` refuses with a message naming what to install.

The generated media carries a second GRUB entry, "Cix Recovery" — a break-glass tool for resetting host-auth admin_groups on an already-installed system if you're ever locked out of the API; see [`security.md`'s recovery section](security.md#break-glass-recovery) and [ADR-0146](../adr/0146-ldap-startup-resync-and-break-glass-recovery.md). It never reformats or reinstalls anything, so keeping this same ISO around after a normal install is worthwhile on its own.

This produces `build/cix-install.iso` — attach it as a CD-ROM/optical drive to a VM (or a real machine) and boot from it. **Nothing needs editing at the GRUB menu**: the empty kernel-args argument above passes the installer nothing, and it asks. It lists the machine's own disks with their sizes, has you pick one by number, and makes you type `ERASE` against that specific disk before it touches it. Then it lists the real network interfaces and asks for the management interface, address, prefix and gateway, defaulting where there is a sensible default.

Supply real values in that argument instead to build media that installs one specific machine unattended — a fleet of identical boxes, say. Mixing works too: a fixed `--disk=` with the address left to be asked.

This replaced a workflow where the args were the literal placeholders `/dev/CHANGEME`/`CHANGEME`/…, and installing meant pressing `e` at the GRUB menu and editing a kernel command line by hand — typed blind, with no list of the machine's disks or NICs, and no feedback until the installer refused to start.

**Secure Boot**: if the firmware is not enforcing it, the installer says so and skips key enrolment entirely — no password. When it *is* enforcing, you are asked for a one-time password and asked again at the next boot in MokManager, which is shim's proof that a human is physically present and cannot be skipped while enrolling. If enrolment fails the install still completes and says what is left to do.

**Why an interface name at all**: this is a one-time bootstrap value only, used to attach a physical NIC to the `management` network `cixd` binds to at first boot — it does *not* need to be perfect. If it's wrong, or your NIC layout changes later, the management network is an ordinary, API-managed `network_def` afterward (`GET /v1/networks`) and can be repointed to a different interface without reinstalling.

**Kernel modules** (Part 3 of the bare-metal-readiness plan, ADR-0061): the two new `mkbootroot` arguments above stage a real kernel module tree and `kmod`'s own tools onto the installed system, so `cixd`'s own boot-time `modprobe` (a curated set of common real-hardware NICs/USB controllers — see the ADR for exactly which) has something real to load on hardware whose driver isn't built directly into the kernel. Both come from real Cix build artifacts, not anything fetched by `mkbootroot` itself: `/path/to/kernel-hostbuild-artifact/lib/modules` is `kernel.recipe`'s own hostbuild output (`cixctl pkg hostbuild kernel --build-image=kernel-builder --wait`, see [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)); `/path/to/kmod-usr-bin` is wherever `recipes/package/kmod/` was installed (e.g. extracted from a real image's own `usr/bin/{kmod,modprobe,depmod,...}`). Omit both (`""`) for a QEMU/CI boot test, same as GPU firmware above — no real target hardware means nothing needs a driver module at all.

**Target disk**: **VirtIO Block** disks (`/dev/vda`) and real **SATA/PATA** disks (`/dev/sda`, `CONFIG_BLK_DEV_SD`, ADR-0061/Part 3) both work — NVMe (`/dev/nvme0n1`) and software RAID (`/dev/mdN`) too. VirtIO-SCSI-attached disks specifically are not yet covered (no `CONFIG_SCSI_VIRTIO`). On Proxmox, attach the target disk with Bus/Device: `VirtIO Block` (simplest) or `SATA`.

**Partitioning** — two ways; the first needs no flag:

- **(no flag, the default)**: `cix-install` partitions the disk itself, non-interactively, with the standard layout below — no typing required. This is what the example above uses, and what `test/test_installer.c`'s own install session actually exercises.

  This replaced an interactive `fdisk` session, which used to be what you got by passing no flag ([ADR-0214](../adr/0214-no-interactive-partitioning.md)). `cix-install` reads partition **roles back from GPT names**, so driving `fdisk` by hand meant reproducing five exact names in the right order with the right types — a contract `fdisk`'s own UI says nothing about, whose failure shows up much later as a role that cannot be found. And because that read-back fixes the *structure*, hand-partitioning could only ever vary the partition **sizes** — which is exactly what the layout below already handles, sizing itself to the disk and leaving the remainder for you to claim through the REST partition API afterwards.

  **It does not take the whole disk** ([ADR-0190](../adr/0190-install-leaves-the-disk-mostly-unallocated.md), issue #104). `cix-containers` is the smaller of 16 GiB and half of what remains after the system partitions, and everything past it is left unallocated for you. The installer prints what it did:

  ```
  partitioning /dev/sda: 1907729 MiB total, 448 MiB system, 16384 MiB data, 1890897 MiB left unallocated for you to use
  ```

  Grow that data partition into the free space whenever you want (`cixctl storage partition resize`, issue #94), or partition the remainder yourself and give it a role. The reason for the conservative default is that the reversible direction should be the one left open: growing into free space is safe, shrinking a filesystem that already holds the system's state is not.
- **`--skip-partition`**: the disk is already partitioned correctly by other means (e.g. scripted provisioning that ran `sfdisk` itself beforehand) — `cix-install` just reads the existing table back. It must carry all five partitions, named exactly `cix-esp`, `cix-root-a`, `cix-root-b`, `cix-config` and `cix-containers`, since that is how roles are identified.

Omitting `--skip-partition` is the default: `cix-install` writes the layout itself.

It then formats, writes the system, and reboots into a running `cixd` at the IP you gave it — reachable at that address directly (`cixd` binds to the exact IP given via `--ip=`, not just loopback).

### Installing over a serial console

Fully supported, and often the only console a rack or hypervisor VM offers. Both
the GRUB menu and the installer's own output reach the serial port, as does the
MokManager Secure Boot screen at first reboot (verified from real serial
captures). Configure the machine's serial port as you normally would; nothing
extra is needed on the Cix side.

The video console works too — but note that **installer media built before
Part 207 rendered the GRUB menu and then nothing else**, showing
`error: no suitable video mode found / Booting in blind mode` and a black screen
for the rest of the install. That was a missing `insmod all_video` in the ISO's
own `grub.cfg`; media built from Part 207 onward do not have it. If you see that
message, your media predates the fix — the install is still proceeding, and the
serial console will show it.

### If the first boot comes up without its network

If the interface named by `--interface=` is not present when the daemon starts,
the box **does not fail** — it logs exactly what it looked for and what it found,
then continues without an uplink:

```
bootstrap_management_network: cannot attach uplink "eth0" (error 13) --
interfaces present: [lo sit0 management]. Continuing WITHOUT an uplink ...
```

`cixd` is still running and the console shell still works; attach the real
interface live and no reinstall is needed:

```
network attach-interface management --interface=<the right name>
```

(Before Part 207 this condition killed the machine with a kernel panic, which
looked like a crash rather than a configuration problem.)

### Outbound DNS on a fresh install

**Known limitation (issue #138):** a freshly installed host cannot resolve
hostnames, and setting resolvers with `cixctl resolv set` does not change that —
the file those resolvers are written to is never mounted where the system reads
it. Until that ships, anything the box fetches for itself (an artifact cache, a
package source) must be addressed by **literal IP**, over plain HTTP — an IP
against an HTTPS endpoint whose certificate names the host fails verification.

This platform's own `.internal` DNS (the `dns-1`/`dns-2` containers) is a
separate mechanism and works normally; see the DNS section of
[`docs/api/README.md`](../api/README.md).

**Console login**: the installed system drops straight into an interactive `cixctl` shell on both the video console and the serial console once boot completes — no username, no password (this platform has no authentication anywhere yet; physical console access is already at least as privileged as the unauthenticated network API). Typing `exit`/`quit`/Ctrl-D ends the session and a fresh one starts automatically a couple of seconds later.

## Changing the management network, port, or enabling HTTPS after install

The `--ip=`/`--gateway=`/`--interface=` values above are a one-time bootstrap only — everything they set up is a real, ordinary, API-managed network named `management` (`GET /v1/networks`), and `cixd`'s own listen port/HTTP/HTTPS exposure is a small, dedicated, live-reconfigurable resource, `GET`/`PUT /v1/system/daemon-config` (see [`docs/api/README.md`](../api/README.md#the-management-network-and-cixds-own-listeners) for the full contract). Also reachable from the web dashboard's System > Host > Daemon page, or directly with `cixctl daemon-config` (see [`docs/guides/cli-reference.md`](cli-reference.md)):

```sh
cixctl --host=<install-ip> daemon-config show
cixctl --host=<install-ip> daemon-config set --port=8080
cixctl --host=<install-ip> daemon-config set --enable-https
cixctl --host=<install-ip> daemon-config set --management-network=lan1
```

Or, equivalently, straight `curl` (every `cixctl` subcommand is exactly one HTTP call, per the API-First Mandate):

```sh
curl http://<install-ip>:80/v1/system/daemon-config
curl -X PUT http://<install-ip>:80/v1/system/daemon-config \
     -d '{"port": 8080}'
curl -X PUT http://<install-ip>:80/v1/system/daemon-config \
     -d '{"https_enabled": true}'
curl -X PUT http://<install-ip>:80/v1/system/daemon-config \
     -d '{"management_network": "lan1"}'
```

Every change here is applied live (no reboot, no restart — `cixd` runs as real PID 1 on an installed system, so there is no restart to fall back on) and persisted, so it survives a real one too. `https_enabled` (and `http_enabled`) both already default to `true` on a fresh install (ADR-0171), on ports 80/443 — but HTTPS has nothing to actually serve TLS with until a PKI root CA is bootstrapped (`POST /v1/pki/ca`, which also auto-issues the `"host"` leaf certificate HTTPS reuses), so a genuinely fresh install's own first boot just has the HTTPS listener silently not come up (logged, non-fatal). Once PKI is bootstrapped, re-running `daemon-config set --enable-https` (even though it's already logically enabled) brings the listener up immediately, live, with no reboot needed — the handler checks whether the listener is actually running, not just the persisted flag. Repointing `management_network` needs the target network to already have its own address (`has_address: true`, e.g. created via `POST /v1/networks` with an `address` field, or another network attached to a physical NIC via `POST /v1/networks/{name}/interfaces`) — double-check you can actually reach the new address before relying on it, since a mistake here has no remote undo, only physical console access (above).

## Secure Boot

Also requires **UEFI firmware and a Q35 machine type** — Cix is UEFI-only (see `docs/roadmap/ROADMAP.md`'s Phase 11 architecture decisions); a legacy-BIOS/i440fx VM has no AHCI CD-ROM controller for this kernel to find and will panic trying to mount root. On Proxmox: VM → Hardware → BIOS → `OVMF (UEFI)`; VM → Options → Machine → `q35`.

The **installer media itself** always needs Secure Boot **off** for its own one-time, unsigned boot — no different from installing most non-Windows OSes from scratch (see ADR-0015 for why this can't be avoided). Concretely, on Proxmox: when adding the VM's EFI Disk, leave **"Pre-Enroll keys" unchecked** — if Secure Boot's keys are already enrolled before this first boot, it fails with `Access Denied`.

The **installed system** is Secure-Boot-capable, but getting there needs one real, in-order sequence:

1. `cix-install` signs its own boot chain with the Cix key above and, during install, stages a one-time key-enrollment request — you'll be asked to set a temporary password.
2. Before letting it reboot into the installed system, enter the firmware's own setup screen (on Proxmox/OVMF: interrupt at the Tianocore splash, usually `Esc`) → **Device Manager → Secure Boot Configuration** → enable Secure Boot / "Enroll Default Secure Boot Keys" (exact wording varies by OVMF build) → save and reset. This is the step that actually turns Secure Boot on for this machine, using the *same* EFI vars store that already has the pending key request from step 1 — don't recreate the EFI Disk to do this, that would discard the pending request.
3. On that reset, `shim` (Microsoft-signed, always trusted) detects the pending request and shows its own **MokManager** screen: **press any key** (there's a ~10s countdown — miss it and it falls straight through to a boot attempt that correctly fails with `Verification failed: (0x1A) Security Violation`, since nothing got enrolled) → from the main menu, arrow down to **"Enroll MOK"** (*not* "Continue boot" — see the warning below) → **"Continue"** (a *different* "Continue", inside the Enroll-MOK submenu, confirming you want to proceed past viewing the key) → **"Yes"** → enter the *same* password from step 1 → back at the main menu, now offering **"Reboot"** as the top entry — select it.
4. From then on, Secure Boot stays on with zero further prompts on that machine.

**⚠️ Do not select "Continue boot"** at that main MokManager menu, even by mistake — confirmed directly: picking it doesn't just skip the prompt for this boot, it **permanently discards** the pending enrollment request. Every later boot's menu will be missing the "Enroll MOK" option entirely, and there's no way to get it back short of a full reinstall (a fresh `mokutil --import` needs a fresh install run — `cix-install` has no standalone "just do enrollment" mode).

If this already happened to you, there's a working recovery that doesn't need a reinstall: **hash-enroll the two boot-chain files directly**, via the same MokManager menu's **"Enroll hash from disk"** option — do this for *both* files, rebooting only after both are done:
1. `\EFI\BOOT\grubx64.efi`
2. `\cix-bzImage`

This trusts those exact files by hash rather than by the Cix signing key, so it's narrower than the cert-based path (a future kernel rebuild or reinstall changes the hashes and needs re-enrolling) but gets you unblocked immediately.

## What's next

Once booted, the box is reachable at the IP you gave it — see [`quickstart.md`](quickstart.md) for the fastest path to a first running container, [`cli-reference.md`](cli-reference.md)/[`web-dashboard.md`](web-dashboard.md) for day-to-day usage, and [`staying-updated.md`](staying-updated.md) for how to keep it current.
