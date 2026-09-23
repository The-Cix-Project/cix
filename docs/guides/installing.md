# Installing Cix

Building and using the installer ISO — from a set of already-built binaries (`build/cixd`, `build/cix-install`, a kernel `build-inputs/bzImage`, see [`building-cix.md`](building-cix.md) and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)) to a real, booted, network-reachable install.

## One-time: the Cix Secure Boot signing key

`image/keys/cix-signing.{crt,cer}` (the public cert, PEM and DER) are committed; `image/keys/cix-signing.key` (the private key) is gitignored and must exist locally before building the ISO by hand. Generate it once, and keep it — every machine that has enrolled it (see [Secure Boot](#secure-boot) below) needs the *same* key for future installs to keep working. A host that builds ISOs itself (`cixctl iso build`) uses the pair installed with `cixctl signing-keys set` ([ADR-0212](../adr/0212-signing-keys-over-rest.md)):

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

This gives you `/tmp/linux-firmware/amdgpu` — pass that path as `mkbootroot`'s 6th argument below. The kernel's own `CONFIG_DRM_AMDGPU`/`CONFIG_HSA_AMD` drivers load these at boot-time driver-probe, before any container exists, so they have to live on the host root itself, not a container image.

## Building the ISO

This section covers the dev-machine path (`mkbootroot`/`mkinstalleriso` run by hand). A running Cix host can also assemble a fresh ISO itself, no separate dev machine involved — see [`building-cix.md`'s "Build a fresh installer ISO, server-side"](building-cix.md#4-build-a-fresh-installer-iso-server-side) (`POST /system/iso` / `cixctl iso build`, ADR-0064). Either path produces the same kind of ISO, described below.

Once `build/cixd`, `build/cixctl`, `build/cix-install`, `build/cix-recover`, `build/cix-boot.efi`, `build-inputs/bzImage`, and the signing key above all exist:

```sh
sudo build/mkbootroot /tmp/root_stage build/cixd build/cixctl web /tmp/cixd-root.squashfs \
     /tmp/linux-firmware/amdgpu \
     /path/to/kernel-hostbuild-artifact/lib/modules \
     /path/to/kmod-usr-bin \
     /path/to/host-tools-rootfs
sudo build/mkinstalleriso build/iso_stage build/cix-install build/cix-recover \
     build/cix-boot.efi build-inputs/bzImage \
     /tmp/cixd-root.squashfs \
     image/keys/cix-signing.key image/keys/cix-signing.crt image/keys/cix-signing.cer \
     build/cix-install.iso \
     "" \
     /path/to/isotools-artifact \
     /path/to/seed \
     /path/to/kernel-hostbuild-artifact/lib/modules \
     /path/to/kmod-usr-bin
```

`mkbootroot` takes nine arguments. The last four may each be `""`:

- **firmware root**: the GPU firmware above; `""` skips it.
- **modules dir** and **kmod bin dir**: see **Kernel modules** below; `""` skips them.
- **host-tools rootfs**: the rootfs of an image carrying the `coreutils` and `gzip` packages (such as `cix-hosttools`, ADR-0078). When given, the tools `mkbootroot` copies into the root come from it rather than from the build machine; `""` uses the build machine's.

`mkinstalleriso` takes fifteen. The last four are worth a word. **isotools-root** is the harvested `isotools` hostbuild artifact (ADR-0064) holding `grub-mkrescue`, `xorriso`, `sbsign`, `mokutil`, shim and their libraries. It is an argument so this tool can run on a Cix host, which has none of those at Debian's paths.

**kernel-modules-dir** and **kmod-bin-dir** are the same two inputs `mkbootroot` takes, and here they supply the installer's NIC drivers (#429). The five NIC drivers this platform supports are kernel *modules*, so without them `cix-install` can list only interfaces whose driver is built into the kernel, which in practice is `virtio_net` alone. Only the NIC modules and their dependency closure are staged, read from the tree's own `modules.dep` (`ixgbe`, `r8169` and `tg3` all pull in `libphy`/`mdio_bus`), plus `modprobe` to load them. With both `""` the media still installs on virtio, and `cix-install` says on screen which of the two it lacks.

**seed** is optional (`""` for none) and is what makes a freshly installed box able to run a container without a network: a directory of `recipes/` and `artifacts/` copied onto the installed system's own package directories, from which the daemon installs a C library into the default image at first boot (#189). Without it, a fresh install comes up healthy but its default image has no C library, and `POST /v1/containers` refuses with a message naming what to install.

The generated media carries a "Cix Recovery" entry per media kind — a break-glass tool for resetting host-auth admin_groups on an already-installed system if you're ever locked out of the API; see [`security.md`'s recovery section](security.md#break-glass-recovery-adr-0146) and [ADR-0146](../adr/0146-ldap-startup-resync-and-break-glass-recovery.md). It never reformats or reinstalls anything, so keeping this same ISO around after a normal install is worthwhile on its own.

This produces `build/cix-install.iso`, which boots from a USB stick or an optical drive. Write it to a stick with a plain byte copy (`dd if=cix-install.iso of=/dev/sdX bs=4M` against the *whole device*, not a partition), or attach it as a CD-ROM to a VM.

**Pick the entry matching your media at the GRUB menu** — "Cix Install (USB media)" or "Cix Install (CD/DVD media)". With no initramfs there is nothing to go and find the installer's own filesystem, so the device holding it is named on the kernel command line, and the two differ: a USB stick is a SCSI disk (`/dev/sda`), an optical drive is `/dev/sr0`. Choosing the wrong one fails immediately and legibly with `Cannot open root device`, and installs nothing — pick the other and boot again. Naming `sda` also assumes the stick is the machine's first SCSI disk, which is true of a machine whose only other disk is NVMe; [#430](https://git.home.arpa/itdlabs/cix/issues/430) carries the measured reason this is not yet a `PARTUUID=`, which would need no choice at all.

Nothing else needs editing: the empty kernel-args argument above passes the installer nothing, and it asks. It lists the machine's own disks with their sizes, has you pick one by number, and makes you type `ERASE` against that specific disk before it touches it. Then it lists the real network interfaces and asks for the management interface, address, prefix and gateway, defaulting where there is a sensible default.

Supply real values in that argument instead to build media that installs one specific machine unattended — a fleet of identical boxes, say. Mixing works too: a fixed `--disk=` with the address left to be asked.

**Secure Boot**: if the firmware is not enforcing it, the installer says so and skips key enrolment entirely — no password. When it *is* enforcing, you are asked for a one-time password and asked again at the next boot in MokManager, which is shim's proof that a human is physically present and cannot be skipped while enrolling. If enrolment fails the install still completes and says what is left to do.

**Why an interface name at all**: this is a one-time bootstrap value, used to attach a physical NIC to the network `cixd`'s management address sits on at first boot. It does *not* need to be perfect. That network is an ordinary, API-managed network afterward (`GET /v1/networks`), and its interfaces can be changed without reinstalling.

**Kernel modules** (ADR-0061): the modules-dir and kmod-bin-dir arguments stage a kernel module tree and `kmod`'s tools onto the installed system, so `cixd`'s boot-time `modprobe` (a curated set of common NICs and USB controllers; see the ADR) has something to load on hardware whose driver is not built into the kernel. Both come from Cix build artifacts: `/path/to/kernel-hostbuild-artifact/lib/modules` is the `kernel` package's hostbuild output (`cixctl pkg hostbuild kernel --wait`, see [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)); `/path/to/kmod-usr-bin` is the `usr/bin` of an image carrying the `kmod` package (`kmod`, `modprobe`, `depmod`, …), such as `cix-kmod`. Pass `""` for both for a QEMU/CI boot test, where nothing needs a driver module.

**Target disk**: **VirtIO Block** (`/dev/vda`), **VirtIO SCSI** and **SATA/PATA** (`/dev/sda`) disks work, as do NVMe (`/dev/nvme0n1`) and software RAID (`/dev/mdN`). The kernel config (`image/kernel/qemu-part1.config`) builds in `CONFIG_SCSI_VIRTIO` and the common hardware RAID/HBA drivers, since a kernel with no initramfs cannot load a module off a disk it cannot yet see.

**Partitioning** — two ways; the first needs no flag:

- **(no flag, the default)**: `cix-install` partitions the disk itself, non-interactively, with the standard layout below — no typing required. This is what the example above uses, and what `test/test_installer.c`'s own install session actually exercises.

  There is no interactive partitioning ([ADR-0214](../adr/0214-no-interactive-partitioning.md)). `cix-install` reads partition **roles back from GPT names**, which fixes the structure, and the layout sizes itself to the disk.

  **It does not take the whole disk** ([ADR-0190](../adr/0190-install-leaves-the-disk-mostly-unallocated.md), issue #104). The system partitions take 896 MiB (ESP 64, two root slots of 160, `cix-config` 512). `cix-containers` is the smaller of 16 GiB and half of what remains, and everything past it is left unallocated for you. The installer prints what it did:

  ```
  partitioning /dev/sda: 1907729 MiB total, 896 MiB system, 16384 MiB data, 1890441 MiB left unallocated for you to use
  ```

  Grow that data partition into the free space whenever you want (`cixctl storage grow-partition DISK PARTITION` with kernel names, e.g. `sda sda5`, since `cix-containers` is the fifth partition; issues #94 and #163. It is btrfs, so it grows online), or partition the remainder yourself and give it a role (see [`storage.md`](storage.md)). Growing into free space is safe; shrinking a filesystem that holds the system's state is not, so the default leaves the reversible direction open.
- **`--wipe-config`**: format the `cix-config` partition even if it already holds a CA. **By default an existing CA on that partition is kept, not destroyed** — `cix-install` mounts it read-only, looks for `/state/pki/ca.key`, and if it finds one says so plainly and skips the format, preserving the PKI along with the DNS records, networks and container definitions that live beside it (#415). A mount failure means "format it", which is the right answer for both a fresh disk and a partition whose geometry moved. Carrying a CA across deliberately — to new hardware, or after a disk failure, where there is nothing on the disk to preserve — is [`cixctl pki export`](security.md#carrying-the-ca-across-a-reinstall).
- **`--skip-partition`**: the disk is already partitioned correctly by other means (e.g. scripted provisioning that ran `sfdisk` itself beforehand) — `cix-install` just reads the existing table back. It must carry all five partitions, named exactly `cix-esp`, `cix-root-a`, `cix-root-b`, `cix-config` and `cix-containers`, since that is how roles are identified.

Omitting `--skip-partition` is the default: `cix-install` writes the layout itself.

It then formats, writes the system, and reboots into a running `cixd` at the IP you gave it — reachable at that address directly (`cixd` binds to the exact IP given via `--ip=`, not just loopback).

**You can also install with no network at all.** Pick `lo` and the box comes up running, answering on `127.0.0.1`, with no off-box address. Give it one once it has booted, from the console:

```
cixctl network create --name=lan --subnet=192.168.15.0 --prefix=24
cixctl network attach-interface lan --interface=eth0
cixctl management-address set 192.168.15.95
cixctl routes add --default --gateway=192.168.15.1
```

`management-address set` requires the address to fall inside an existing network's subnet, and the network follows from the address ([ADR-0287](../adr/0287-the-management-address-is-the-single-truth.md)). It applies immediately and persists across reboots, and it is also how an addressed box moves to another address later. The default route is separate, under `routes`. This is the route to take whenever the port you want is not in the installer's list: install on loopback, boot, and set it then.

The installer stages the five NIC drivers and `modprobe` and loads them before it lists interfaces (#429). If the list still shows no real NIC, the installer says which of three things happened: the media carries no module tools (a media defect — it was built on a host with no `cix-kmod` image), a driver load failed and here is what `modprobe` said (a media defect — the module tree is missing, is for another kernel release, or was built from another config), or all five loaded cleanly and this machine's Ethernet simply is not one of them (not a media defect at all). The recorded driver gaps are Broadcom NetXtreme II (`bnx2`, needs a firmware blob) and Intel I225/I226 2.5G (`igc`, absent from the kernel config).

### Installing over a serial console

Fully supported, and often the only console a rack or hypervisor VM offers. Both
the GRUB menu and the installer's own output reach the serial port, as does the
MokManager Secure Boot screen at first reboot (verified from real serial
captures). Configure the machine's serial port as you normally would; nothing
extra is needed on the Cix side.

The video console works too: the ISO's `grub.cfg` loads `all_video` so the
kernel receives a framebuffer. If GRUB prints
`error: no suitable video mode found / Booting in blind mode` and the screen stays
black, the media lacks that line and should be rebuilt; the install is still
proceeding, and the serial console shows it.

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

### Outbound DNS on a fresh install

Set the host's resolvers with `cixctl resolv set --nameserver=A.B.C.D` (up to three). It takes effect immediately, with no reboot, and anything the box fetches for itself (an artifact cache, a package source) resolves through them.

This platform's own `.internal` DNS (the `dns-1`/`dns-2` containers) is a
separate mechanism; see the DNS section of
[`docs/api/README.md`](../api/README.md).

**Console login**: the installed system drops straight into an interactive `cixctl` shell on both the video console and the serial console once boot completes, with no username or password to start it ([ADR-0034](../adr/0034-console-login-via-supervised-cixctl.md)). The shell is an ordinary API client on `127.0.0.1`: once host-auth write-gating is active ([ADR-0144](../adr/0144-host-authentication-and-real-ldap.md)), mutating commands there need `login` like anywhere else. Typing `exit`/`quit`/Ctrl-D ends the session and a fresh one starts automatically a couple of seconds later.

## Changing the address, port, or enabling HTTPS after install

The `--ip=`/`--prefix=`/`--gateway=`/`--interface=` values above are a one-time bootstrap. After install, where `cixd` answers off-box is the management address (`cixctl management-address show|set|reset`, `GET`/`PUT`/`DELETE /v1/system/management-address`), the default route is under `cixctl routes`, and `cixd`'s listen ports and HTTP/HTTPS exposure are `daemon-config` (`GET`/`PUT /v1/system/daemon-config`). See [The management address and cixd's own listeners](../api/README.md#the-management-address-and-cixds-own-listeners) for the contract, and [`cli-reference.md`](cli-reference.md) for the commands:

```sh
cixctl --host=<install-ip> daemon-config show
cixctl --host=<install-ip> daemon-config set --port=8080
cixctl --host=<install-ip> daemon-config set --enable-https
cixctl --host=<install-ip> management-address set 192.168.20.5
```

The same calls with `curl`:

```sh
curl http://<install-ip>:80/v1/system/daemon-config
curl -X PUT http://<install-ip>:80/v1/system/daemon-config \
     -d '{"port": 8080}'
curl -X PUT http://<install-ip>:80/v1/system/daemon-config \
     -d '{"https_enabled": true}'
```

Once write-gating is active, a `curl` write needs `-H "Authorization: Bearer <token>"`.

Every change here is applied live (no reboot, no restart — `cixd` runs as real PID 1 on an installed system, so there is no restart to fall back on) and persisted, so it survives a real one too. `https_enabled` (and `http_enabled`) both already default to `true` on a fresh install (ADR-0171), on ports 80/443 — but HTTPS has nothing to actually serve TLS with until a PKI root CA is bootstrapped (`POST /v1/pki/ca`, which also auto-issues the `"host"` leaf certificate HTTPS reuses), so a genuinely fresh install's own first boot just has the HTTPS listener silently not come up (logged, non-fatal). Once PKI is bootstrapped, re-running `daemon-config set --enable-https` (even though it's already logically enabled) brings the listener up immediately, live, with no reboot needed — the handler checks whether the listener is actually running, not just the persisted flag. `management-address set` needs an existing network whose subnet contains the new address, and refuses (400) otherwise. Check that you can reach the new address before relying on it: a mistake here has no remote undo, only the console (above), which always answers on `127.0.0.1`.

## Secure Boot

Also requires **UEFI firmware and a Q35 machine type** — Cix is UEFI-only (see `docs/roadmap/ROADMAP.md`'s Phase 11 architecture decisions); a legacy-BIOS/i440fx VM has no AHCI CD-ROM controller for this kernel to find and will panic trying to mount root. On Proxmox: VM → Hardware → BIOS → `OVMF (UEFI)`; VM → Options → Machine → `q35`.

The **installer media itself** always needs Secure Boot **off** for its own one-time, unsigned boot — no different from installing most non-Windows OSes from scratch (see ADR-0015 for why this can't be avoided). Concretely, on Proxmox: when adding the VM's EFI Disk, leave **"Pre-Enroll keys" unchecked** — if Secure Boot's keys are already enrolled before this first boot, it fails with `Access Denied`.

The **installed system** is Secure-Boot-capable, but getting there needs one real, in-order sequence:

1. `cix-install` signs its own boot chain with the Cix key above and, during install, stages a one-time key-enrollment request — you'll be asked to set a temporary password.
2. Before letting it reboot into the installed system, enter the firmware's own setup screen (on Proxmox/OVMF: interrupt at the Tianocore splash, usually `Esc`) → **Device Manager → Secure Boot Configuration** → enable Secure Boot / "Enroll Default Secure Boot Keys" (exact wording varies by OVMF build) → save and reset. This is the step that actually turns Secure Boot on for this machine, using the *same* EFI vars store that already has the pending key request from step 1 — don't recreate the EFI Disk to do this, that would discard the pending request.
3. On that reset, `shim` (Microsoft-signed, always trusted) detects the pending request and shows its own **MokManager** screen: **press any key** (there's a ~10s countdown — miss it and it falls straight through to a boot attempt that correctly fails with `Verification failed: (0x1A) Security Violation`, since nothing got enrolled) → from the main menu, arrow down to **"Enroll MOK"** (*not* "Continue boot" — see the warning below) → **"Continue"** (a *different* "Continue", inside the Enroll-MOK submenu, confirming you want to proceed past viewing the key) → **"Yes"** → enter the *same* password from step 1 → back at the main menu, now offering **"Reboot"** as the top entry — select it.
4. From then on, Secure Boot stays on with zero further prompts on that machine.

**⚠️ Do not select "Continue boot"** at that main MokManager menu, even by mistake — confirmed directly: picking it doesn't just skip the prompt for this boot, it **permanently discards** the pending enrollment request. Every later boot's menu will be missing the "Enroll MOK" option entirely, and there's no way to get it back short of a full reinstall (a fresh `mokutil --import` needs a fresh install run — `cix-install` has no standalone "just do enrollment" mode).

If this already happened to you, there's a working recovery that doesn't need a reinstall: **hash-enroll the two boot-chain files directly**, via the same MokManager menu's **"Enroll hash from disk"** option — do this for each of these files, rebooting only after all are done:
1. `\EFI\BOOT\grubx64.efi` (the Cix boot manager, loaded by shim under that name)
2. `\cix-bzImage-a` and `\cix-bzImage-b`, each slot's kernel

This trusts those exact files by hash rather than by the Cix signing key, so it's narrower than the cert-based path (a future kernel rebuild or reinstall changes the hashes and needs re-enrolling) but gets you unblocked immediately.

## What's next

Once booted, the box is reachable at the IP you gave it — see [`quickstart.md`](quickstart.md) for the fastest path to a first running container, [`cli-reference.md`](cli-reference.md)/[`web-dashboard.md`](web-dashboard.md) for day-to-day usage, and [`staying-updated.md`](staying-updated.md) for how to keep it current.
