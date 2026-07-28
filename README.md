# Kanxeo

**The Architecture of Absolute Clarity**

Kanxeo is a rolling-release, container-native operating system built entirely from source: a hand-rolled container runtime on raw Linux namespaces and cgroups, OverlayFS-based image layering, a 100% custom C networking data plane, and a REST control layer for the host, containers, DNS, and PKI — no runc, no Open vSwitch, no eBPF, compiled exclusively with the Tiny C Compiler (TCC).

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
sudo build/mkbootroot  /tmp/root_stage build/kanxeod /tmp/kanxeod-root.squashfs
sudo build/mkinstalleriso build/iso_stage build/kanxeo-install build/bzImage \
     /tmp/kanxeod-root.squashfs \
     image/keys/kanxeo-signing.key image/keys/kanxeo-signing.crt image/keys/kanxeo-signing.cer \
     build/kanxeo-install.iso \
     "--disk=/dev/CHANGEME --ip=CHANGEME --prefix=24 --gateway=CHANGEME"
```

This produces `build/kanxeo-install.iso` — attach it as a CD-ROM/optical drive to a VM (or a real machine, once you have one on hand) and boot from it. The placeholder args are deliberate: at the GRUB boot menu, press `e` to edit the boot entry, replace `/dev/CHANGEME`/`CHANGEME`/`CHANGEME` with the real target disk (e.g. `/dev/sda`), IP address, and gateway for this install, then `Ctrl-X` to boot. If you forget, `kanxeo-install`'s own disk check fails safely — it refuses to touch a disk that doesn't exist rather than silently doing the wrong thing. Once booted, it partitions the disk interactively via `cfdisk` (see `docs/ROADMAP.md`'s Phase 11 part 3/4 write-ups for the exact GPT partition names it expects: `kanxeo-esp`, `kanxeo-root-a`, `kanxeo-root-b`, `kanxeo-config`, `kanxeo-containers`), formats, writes the system, and reboots into a running `kanxeod` at the IP you gave it.

### Secure Boot

The **installer media itself** always needs Secure Boot **off** in the VM/host firmware settings — it's an unsigned, one-time boot, no different from installing most non-Windows OSes from scratch (see ADR-0015 for why this can't be avoided). Turn it back on any time after.

The **installed system** is Secure-Boot-capable: `kanxeo-install` signs its own boot chain with the Kanxeo key above and, during install, stages a one-time key-enrollment request (you'll be asked to set a temporary password). At the newly-installed system's **very next reboot**, firmware's own `MokManager` screen (blue, text-mode, not anything this project built) will ask you to confirm the enrollment — enter that same password. After that single confirmation, Secure Boot can stay on indefinitely, with zero further exceptions, on that machine.

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
