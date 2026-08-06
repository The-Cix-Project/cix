# Installing Kanxeo

Building and using the installer ISO — from a set of already-built binaries (`build/kanxeod`, `build/kanxeo-install`, a kernel `build/bzImage`, see [`building-kanxeo.md`](building-kanxeo.md) and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)) to a real, booted, network-reachable install.

## One-time: the Kanxeo Secure Boot signing key

`image/keys/kanxeo-signing.{crt,cer}` (the public cert, PEM and DER) are committed; `image/keys/kanxeo-signing.key` (the private key) is gitignored and must exist locally before building the ISO. Generate it once, and keep it — every machine that has enrolled it (see [Secure Boot](#secure-boot) below) needs the *same* key for future installs to keep working:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 7300 \
    -keyout image/keys/kanxeo-signing.key -out image/keys/kanxeo-signing.crt \
    -subj "/CN=Kanxeo Secure Boot Signing Key/O=Kanxeo Project"
openssl x509 -in image/keys/kanxeo-signing.crt -outform DER -out image/keys/kanxeo-signing.cer
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

Once `build/kanxeod`, `build/kanxeo-install`, `build/bzImage`, and the signing key above all exist:

```sh
sudo build/mkbootroot  /tmp/root_stage build/kanxeod build/kanxeoctl web /tmp/kanxeod-root.squashfs \
     /tmp/linux-firmware/amdgpu   # or "" to skip GPU firmware entirely
sudo build/mkinstalleriso build/iso_stage build/kanxeo-install build/bzImage \
     /tmp/kanxeod-root.squashfs \
     image/keys/kanxeo-signing.key image/keys/kanxeo-signing.crt image/keys/kanxeo-signing.cer \
     build/kanxeo-install.iso \
     "--disk=/dev/CHANGEME --ip=CHANGEME --prefix=24 --gateway=CHANGEME --interface=CHANGEME --auto-partition"
```

This produces `build/kanxeo-install.iso` — attach it as a CD-ROM/optical drive to a VM (or a real machine) and boot from it. The placeholder args are deliberate: at the GRUB boot menu (it waits 10s before auto-booting, giving you a real chance to interrupt it), press `e` to edit the boot entry, replace `/dev/CHANGEME`/`CHANGEME`/`CHANGEME`/`CHANGEME` with the real target disk, IP address, gateway, and physical interface name (e.g. `eth0` — check `ls /sys/class/net` from a rescue shell if you're not sure which one is which) for this install, then `Ctrl-X` to boot. If you forget, `kanxeo-install`'s own disk check fails safely — it refuses to touch a disk that doesn't exist rather than silently doing the wrong thing.

**Why an interface name at all**: this is a one-time bootstrap value only, used to attach a physical NIC to the `mgmt` network `kanxeod` binds to at first boot — it does *not* need to be perfect. If it's wrong, or your NIC layout changes later, the management network is an ordinary, API-managed `network_def` afterward (`GET /v1/networks`) and can be repointed to a different interface without reinstalling.

**Target disk**: currently only **VirtIO Block** disks work (`/dev/vda`) — the kernel doesn't have the SCSI-disk driver needed for SATA/IDE/VirtIO-SCSI-attached disks (only the CD-ROM driver, for the installer media itself). On Proxmox, attach the target disk with Bus/Device: `VirtIO Block`.

**Partitioning** — three ways, pick one via `kanxeo-install`'s own flags (baked into the last argument above):

- **`--auto-partition`** (recommended for VMs / scripted provisioning): partitions the disk itself, non-interactively, with the standard fixed layout below — no typing required. This is what the example above uses, and what `test/test_installer.c`'s own install session actually exercises.
- **(no flag, the default)**: drops into a real, interactive `fdisk` session — use this if you need different partition sizes than the standard layout. GPT, five partitions, named exactly (`kanxeo-esp`, `kanxeo-root-a`, `kanxeo-root-b`, `kanxeo-config`, `kanxeo-containers` — see `docs/roadmap/ROADMAP.md`'s Phase 11 part 3 write-up for the role each one plays):

  ```
  g                                  # new GPT label
  n  1    +64M                       # kanxeo-esp   (partition 1, default first sector, then Enter for each)
  n  2    +160M                      # kanxeo-root-a
  n  3    +160M                      # kanxeo-root-b
  n  4    +64M                       # kanxeo-config
  n  5                               # kanxeo-containers (rest of the disk -- Enter twice, no size)
  t  1  1                            # partition 1 -> EFI System type
  x                                  # expert menu, to set partition names
  n  1  kanxeo-esp
  n  2  kanxeo-root-a
  n  3  kanxeo-root-b
  n  4  kanxeo-config
  n  5  kanxeo-containers
  r                                  # back to the main menu
  w                                  # write and exit
  ```

  (Each `n  <number>  <size>` above is really three separate prompts: partition number, first sector — just press Enter for the default — then last sector/size, where you type `+64M` etc., or Enter alone for partition 5's "rest of the disk".)

- **`--skip-partition`**: the disk is already partitioned correctly by other means (e.g. scripted provisioning that ran `sfdisk` itself beforehand) — `kanxeo-install` just reads the existing table back.

`--skip-partition` and `--auto-partition` are mutually exclusive; omitting both means interactive `fdisk`.

It then formats, writes the system, and reboots into a running `kanxeod` at the IP you gave it — reachable at that address directly (`kanxeod` binds to the exact IP given via `--ip=`, not just loopback).

**Console login**: the installed system drops straight into an interactive `kanxeoctl` shell on both the video console and the serial console once boot completes — no username, no password (this platform has no authentication anywhere yet; physical console access is already at least as privileged as the unauthenticated network API). Typing `exit`/`quit`/Ctrl-D ends the session and a fresh one starts automatically a couple of seconds later.

## Changing the management network, port, or enabling HTTPS after install

The `--ip=`/`--gateway=`/`--interface=` values above are a one-time bootstrap only — everything they set up is a real, ordinary, API-managed network named `mgmt` (`GET /v1/networks`), and `kanxeod`'s own listen port/HTTP/HTTPS exposure is a small, dedicated, live-reconfigurable resource, `GET`/`PUT /v1/system/daemon-config` (see [`docs/api/README.md`](../api/README.md#the-management-network-and-kanxeods-own-listeners) for the full contract). No CLI or web dashboard support exists for this resource yet — reach it with `curl` directly:

```sh
curl http://<install-ip>:7620/v1/system/daemon-config
curl -X PUT http://<install-ip>:7620/v1/system/daemon-config \
     -d '{"port": 8080}'
curl -X PUT http://<install-ip>:7620/v1/system/daemon-config \
     -d '{"https_enabled": true}'
curl -X PUT http://<install-ip>:7620/v1/system/daemon-config \
     -d '{"management_network": "lan1"}'
```

Every change here is applied live (no reboot, no restart — `kanxeod` runs as real PID 1 on an installed system, so there is no restart to fall back on) and persisted, so it survives a real one too. Enabling HTTPS needs a bootstrapped PKI root CA first (`POST /v1/pki/ca`) — it reuses the already-issued `"host"` leaf certificate rather than a separate cert. Repointing `management_network` needs the target network to already have a gateway address (`has_gateway: true`, e.g. created via `POST /v1/networks` with a `gateway` field, or another network attached to a physical NIC via `POST /v1/networks/{name}/interfaces`) — double-check you can actually reach the new address before relying on it, since a mistake here has no remote undo, only physical console access (above).

## Secure Boot

Also requires **UEFI firmware and a Q35 machine type** — Kanxeo is UEFI-only (see `docs/roadmap/ROADMAP.md`'s Phase 11 architecture decisions); a legacy-BIOS/i440fx VM has no AHCI CD-ROM controller for this kernel to find and will panic trying to mount root. On Proxmox: VM → Hardware → BIOS → `OVMF (UEFI)`; VM → Options → Machine → `q35`.

The **installer media itself** always needs Secure Boot **off** for its own one-time, unsigned boot — no different from installing most non-Windows OSes from scratch (see ADR-0015 for why this can't be avoided). Concretely, on Proxmox: when adding the VM's EFI Disk, leave **"Pre-Enroll keys" unchecked** — if Secure Boot's keys are already enrolled before this first boot, it fails with `Access Denied`.

The **installed system** is Secure-Boot-capable, but getting there needs one real, in-order sequence:

1. `kanxeo-install` signs its own boot chain with the Kanxeo key above and, during install, stages a one-time key-enrollment request — you'll be asked to set a temporary password.
2. Before letting it reboot into the installed system, enter the firmware's own setup screen (on Proxmox/OVMF: interrupt at the Tianocore splash, usually `Esc`) → **Device Manager → Secure Boot Configuration** → enable Secure Boot / "Enroll Default Secure Boot Keys" (exact wording varies by OVMF build) → save and reset. This is the step that actually turns Secure Boot on for this machine, using the *same* EFI vars store that already has the pending key request from step 1 — don't recreate the EFI Disk to do this, that would discard the pending request.
3. On that reset, `shim` (Microsoft-signed, always trusted) detects the pending request and shows its own **MokManager** screen: **press any key** (there's a ~10s countdown — miss it and it falls straight through to a boot attempt that correctly fails with `Verification failed: (0x1A) Security Violation`, since nothing got enrolled) → from the main menu, arrow down to **"Enroll MOK"** (*not* "Continue boot" — see the warning below) → **"Continue"** (a *different* "Continue", inside the Enroll-MOK submenu, confirming you want to proceed past viewing the key) → **"Yes"** → enter the *same* password from step 1 → back at the main menu, now offering **"Reboot"** as the top entry — select it.
4. From then on, Secure Boot stays on with zero further prompts on that machine.

**⚠️ Do not select "Continue boot"** at that main MokManager menu, even by mistake — confirmed directly: picking it doesn't just skip the prompt for this boot, it **permanently discards** the pending enrollment request. Every later boot's menu will be missing the "Enroll MOK" option entirely, and there's no way to get it back short of a full reinstall (a fresh `mokutil --import` needs a fresh install run — `kanxeo-install` has no standalone "just do enrollment" mode).

If this already happened to you, there's a working recovery that doesn't need a reinstall: **hash-enroll the two boot-chain files directly**, via the same MokManager menu's **"Enroll hash from disk"** option — do this for *both* files, rebooting only after both are done:
1. `\EFI\BOOT\grubx64.efi`
2. `\kanxeo-bzImage`

This trusts those exact files by hash rather than by the Kanxeo signing key, so it's narrower than the cert-based path (a future kernel rebuild or reinstall changes the hashes and needs re-enrolling) but gets you unblocked immediately.

## What's next

Once booted, the box is reachable at the IP you gave it — see [`quickstart.md`](quickstart.md) for the fastest path to a first running container, [`cli-reference.md`](cli-reference.md)/[`web-dashboard.md`](web-dashboard.md) for day-to-day usage, and [`staying-updated.md`](staying-updated.md) for how to keep it current.
