# Kanxeo

**The Architecture of Absolute Clarity**

Kanxeo is a rolling-release hardware and workload orchestration platform, compiled entirely from source: a hand-rolled container runtime on raw Linux namespaces and cgroups, OverlayFS-based image layering, a 100% custom C networking data plane, and a REST control layer for the host, containers, hardware, DNS, and PKI — no runc, no Open vSwitch, no eBPF, compiled exclusively with the Tiny C Compiler (TCC). Every capability, including hardware itself, is a first-class API resource; containers are where all real work happens, the host is the thinnest possible layer underneath them. Full charter: [`docs/MISSION.md`](docs/MISSION.md).

## The Name

**Kan·x·eo** fuses three words that govern how the system is engineered:

| Fragment | From | Principle |
|---|---|---|
| **Kan** | *Kanso* | The Zen aesthetic of simplicity and the elimination of clutter. No bloated toolchains, no stop-gap dependencies — just TCC, native namespace isolation, and everything unnecessary stripped away. |
| **x** | *Axiom* | A self-evident, undeniable truth. One Source of Truth: no parallel implementations, no regressions, no conflicting state. The host OS is the singular, unbreakable foundation — the shared lowerdir every container is built on. |
| **eo** | *Luceo* | Latin for clarity and illumination. Zero compile warnings, a fully integrated REST API, and internal state that's always transparent, accessible, and programmable. |

## What It Represents

Kanxeo is more than a distribution — it's a discipline. Complex routing protocols, VPNs, and container infrastructure are built systematically, one pristine layer at a time, with no hacks and no bypasses. It exists to prove that a completely unified, custom C-based stack can be more reliable than a patchwork of legacy components.

## Status

| Phase | Name | Status |
|---|---|---|
| 0 | Toolchain smoke test | Done |
| 1 | Namespace + cgroup v2 container harness | Done |
| 2 | OverlayFS root construction | Done |
| 3 | REST API spec + daemon (host + container lifecycle) | Done |
| 4 | CLI (pure REST API client) | Done |
| 5 | Web dashboard (pure REST API client) | Done |
| 6 | Custom virtual switch / rtnetlink data plane | Done |
| 7 | Routing protocols (containerized VPNs/routers) | Done |
| 8 | DNS service | Done |
| 9 | PKI / certificate management | Done |
| 10 | Package manager (source-based, dependency resolution, upgrades) | Done |
| 11 | Bare-metal boot: kernel, bootloader, A/B root, installer | Done |

Every phase above is fully implemented, tested end to end, and documented — see the per-phase write-ups (design, verification steps, real bugs found and fixed) in `docs/ROADMAP.md`. Full charter: [`docs/MISSION.md`](docs/MISSION.md). API contract: [`docs/api/openapi.yaml`](docs/api/openapi.yaml), with a narrative walkthrough at [`docs/api/README.md`](docs/api/README.md).

## Building

Requires `tcc` and a Linux kernel with cgroup v2 and `clone3`/`CLONE_INTO_CGROUP` support (5.7+). Namespace/mount tests must run as root and require a **privileged** container if run inside one — see `docs/ROADMAP.md` Phase 1 for why.

```sh
make                       # builds everything into build/, -Wall -Werror, zero warnings
sudo build/kanxeod         # start the daemon (REST API + web dashboard on :7620)
build/kanxeoctl health     # talk to it with the CLI
make clean
```

Every test binary in `build/` is self-contained (each forks and execs its own `kanxeod` instance where needed) and runs standalone as root, e.g. `sudo build/test_pkg`. There is one test binary per phase/part; see `docs/ROADMAP.md` for what each one verifies.

`build/bzImage` (the kernel) is a separate, deliberately-not-automated build — not reproduced by `make`/`make clean`. See `image/kernel/qemu-part1.config`'s own header comment for the exact fetch-and-build recipe (mainline source, a hand-curated config fragment, a real GCC toolchain — never TCC, which governs this project's own code, not unmodified upstream software).

## Building and Using the Installer ISO

### One-time: the Kanxeo Secure Boot signing key

`image/keys/kanxeo-signing.{crt,cer}` (the public cert, PEM and DER) are committed; `image/keys/kanxeo-signing.key` (the private key) is gitignored and must exist locally before building the ISO. Generate it once, and keep it — every machine that has enrolled it (see below) needs the *same* key for future installs to keep working:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 7300 \
    -keyout image/keys/kanxeo-signing.key -out image/keys/kanxeo-signing.crt \
    -subj "/CN=Kanxeo Secure Boot Signing Key/O=Kanxeo Project"
openssl x509 -in image/keys/kanxeo-signing.crt -outform DER -out image/keys/kanxeo-signing.cer
```

### Building the ISO

Once `build/kanxeod`, `build/kanxeo-install`, `build/bzImage`, and the signing key above all exist:

```sh
sudo build/mkbootroot  /tmp/root_stage build/kanxeod web /tmp/kanxeod-root.squashfs
sudo build/mkinstalleriso build/iso_stage build/kanxeo-install build/bzImage \
     /tmp/kanxeod-root.squashfs \
     image/keys/kanxeo-signing.key image/keys/kanxeo-signing.crt image/keys/kanxeo-signing.cer \
     build/kanxeo-install.iso \
     "--disk=/dev/CHANGEME --ip=CHANGEME --prefix=24 --gateway=CHANGEME --auto-partition"
```

This produces `build/kanxeo-install.iso` — attach it as a CD-ROM/optical drive to a VM (or a real machine, once you have one on hand) and boot from it. The placeholder args are deliberate: at the GRUB boot menu (it waits 10s before auto-booting, giving you a real chance to interrupt it), press `e` to edit the boot entry, replace `/dev/CHANGEME`/`CHANGEME`/`CHANGEME` with the real target disk, IP address, and gateway for this install, then `Ctrl-X` to boot. If you forget, `kanxeo-install`'s own disk check fails safely — it refuses to touch a disk that doesn't exist rather than silently doing the wrong thing.

**Target disk**: currently only **VirtIO Block** disks work (`/dev/vda`) — the kernel doesn't have the SCSI-disk driver needed for SATA/IDE/VirtIO-SCSI-attached disks (only the CD-ROM driver, for the installer media itself). On Proxmox, attach the target disk with Bus/Device: `VirtIO Block`.

**Partitioning** — three ways, pick one via `kanxeo-install`'s own flags (baked into the last argument above):

- **`--auto-partition`** (recommended for VMs / scripted provisioning): partitions the disk itself, non-interactively, with the standard fixed layout below — no typing required. This is what the example above uses, and what `test/test_installer.c`'s own install session actually exercises.
- **(no flag, the default)**: drops into a real, interactive `fdisk` session (switched from `cfdisk`: `fdisk`'s line-based UI needs no terminal database and can be scripted for testing, unlike `cfdisk`'s full-screen UI) — use this if you need different partition sizes than the standard layout. GPT, five partitions, named exactly (`kanxeo-esp`, `kanxeo-root-a`, `kanxeo-root-b`, `kanxeo-config`, `kanxeo-containers` — see `docs/ROADMAP.md`'s Phase 11 part 3 write-up for the role each one plays):

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

### Using the installed system

- **Dashboard:** `http://<ip>:7620/` — same daemon and port as the API, no separate process.
- **CLI:** `kanxeoctl --host=<ip> health`, `... ps`, `... run --name=... --image=... --network=... -- CMD`, `... network create --name=... --subnet=... --prefix=...` (see `docs/api/README.md` for the full walkthrough).
- **Shutdown/reboot:** `kanxeoctl --host=<ip> shutdown` / `... reboot` — the only clean way to power off or restart (as PID 1, `kanxeod` has no shell to run `shutdown`/`reboot` from; these call the real `reboot(2)` syscall internally, see ADR-0016).

### Secure Boot

Also requires **UEFI firmware and a Q35 machine type** — Kanxeo is UEFI-only (see `docs/ROADMAP.md`'s Phase 11 architecture decisions); a legacy-BIOS/i440fx VM has no AHCI CD-ROM controller for this kernel to find and will panic trying to mount root. On Proxmox: VM → Hardware → BIOS → `OVMF (UEFI)`; VM → Options → Machine → `q35`.

The **installer media itself** always needs Secure Boot **off** for its own one-time, unsigned boot — no different from installing most non-Windows OSes from scratch (see ADR-0015 for why this can't be avoided). Concretely, on Proxmox: when adding the VM's EFI Disk, leave **"Pre-Enroll keys" unchecked** — if Secure Boot's keys are already enrolled before this first boot, it fails with `Access Denied`.

The **installed system** is Secure-Boot-capable, but getting there needs one real, in-order sequence (this is the one step our own automated tests skip via a QEMU-only shortcut, so it's easy to miss first time):

1. `kanxeo-install` signs its own boot chain with the Kanxeo key above and, during install, stages a one-time key-enrollment request — you'll be asked to set a temporary password.
2. Before letting it reboot into the installed system, enter the firmware's own setup screen (on Proxmox/OVMF: interrupt at the Tianocore splash, usually `Esc`) → **Device Manager → Secure Boot Configuration** → enable Secure Boot / "Enroll Default Secure Boot Keys" (exact wording varies by OVMF build) → save and reset. This is the step that actually turns Secure Boot on for this machine, using the *same* EFI vars store that already has the pending key request from step 1 — don't recreate the EFI Disk to do this, that would discard the pending request.
3. On that reset, `shim` (Microsoft-signed, always trusted) detects the pending request and shows its own **MokManager** screen: **press any key** (there's a ~10s countdown — miss it and it falls straight through to a boot attempt that correctly fails with `Verification failed: (0x1A) Security Violation`, since nothing got enrolled) → from the main menu, arrow down to **"Enroll MOK"** (*not* "Continue boot" — see the warning below) → **"Continue"** (a *different* "Continue", inside the Enroll-MOK submenu, confirming you want to proceed past viewing the key) → **"Yes"** → enter the *same* password from step 1 → back at the main menu, now offering **"Reboot"** as the top entry — select it.
4. From then on, Secure Boot stays on with zero further prompts on that machine.

**⚠️ Do not select "Continue boot"** at that main MokManager menu, even by mistake — confirmed directly (booted it twice): picking it doesn't just skip the prompt for this boot, it **permanently discards** the pending enrollment request. Every later boot's menu will be missing the "Enroll MOK" option entirely, and there's no way to get it back short of a full reinstall (a fresh `mokutil --import` needs a fresh install run — `kanxeo-install` has no standalone "just do enrollment" mode).

If this already happened to you, there's a working recovery that doesn't need a reinstall: **hash-enroll the two boot-chain files directly**, via the same MokManager menu's **"Enroll hash from disk"** option (confirmed working) — do this for *both* files, rebooting only after both are done:
1. `\EFI\BOOT\grubx64.efi`
2. `\kanxeo-bzImage`

This trusts those exact files by hash rather than by the Kanxeo signing key, so it's narrower than the cert-based path (a future kernel rebuild or reinstall changes the hashes and needs re-enrolling) but gets you unblocked immediately.

## Repository Layout

```
include/       runtime library public API (container.h) and internal glue
src/           runtime implementation (cgroup, namespaces, mounts, overlay, container networking)
netplane/      custom rtnetlink control plane (bridges, veth, routes — no ip/iproute2, no OVS)
daemon/        kanxeod: the REST daemon — the only process with direct runtime/network/DNS/PKI access
client/        shared HTTP client library used by the CLI and the daemon's own test suite
cli/           kanxeoctl: pure REST API client, no direct runtime access
web/           browser dashboard: vanilla HTML/CSS/JS, no framework, no build step, served by kanxeod
image/         bare-metal boot tooling (Phase 11): kernel config, mkbootroot, kanxeo-install, mkinstalleriso
test/          one demonstrable test (+ exec target, where needed) per phase/part
docs/          mission charter, phased roadmap, ADRs, and the OpenAPI contract
build/         compiled output (gitignored)
```
