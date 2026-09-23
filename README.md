# Cix

**Systems, directly.**

The Cix source is available under the [Apache License, Version 2.0](LICENSE).
Contributions follow the [DCO-based contribution guide](CONTRIBUTING.md); the
Cix name and marks are governed separately by [TRADEMARK.md](TRADEMARK.md).

Cix OS is a rolling-release hardware and workload orchestration platform, compiled entirely from source: a hand-rolled container runtime on raw Linux namespaces and cgroups, OverlayFS-based image layering, a 100% custom C networking data plane, and a REST control layer for the host, containers, hardware, disks, networks, DNS, PKI, and more — no runc, no Open vSwitch, no eBPF-based networking dataplane. Cix's own code is compiled exclusively with the Tiny C Compiler (TCC); third-party packages build with TCC by default and with Cix's own self-hosted gcc where TCC cannot ([ADR-0224](docs/adr/0224-the-toolchain-tenet.md), [ADR-0226](docs/adr/0226-gcc-is-an-ordinary-choice-for-third-party-packages.md)). Every capability, including hardware itself, is a first-class API resource; containers are where all real work happens, the host is the thinnest possible layer underneath them. Full charter: [`docs/mission/MISSION.md`](docs/mission/MISSION.md).

## The Name

**Cix** — pronounced *six*. Two halves, each naming a real part of what this is:

| Part | Principle |
|---|---|
| **C** | Both meanings are literal here. The core is hand-rolled C all the way down — the container runtime, the networking data plane, the REST control layer — compiled exclusively with the Tiny C Compiler; C is not an implementation detail, it is the product. And **c**ontainers are where all real work happens, with the host as the thinnest possible layer underneath them. |
| **ix** | For POSIX and UNIX: direct Linux primitives, exposed rather than wrapped. Raw namespaces and cgroups, OverlayFS, rtnetlink — no runc, no Open vSwitch, no bundled platform services. It exposes the kernel, not an opinion. |

The name is the architecture, not a label bolted on afterward. Its full brand system — strategy, voice, colour, typography, the mark and the UI icon set — lives in [`docs/brand/`](docs/brand/), with [`brand-guidelines.md`](docs/brand/brand-guidelines.md) as the authority.

## What It Represents

Cix OS is more than a distribution — it's a discipline. Complex routing protocols, VPNs, and container infrastructure are built systematically, one pristine layer at a time, with no hacks and no bypasses. It exists to prove that a completely unified, custom C-based stack can be more reliable than a patchwork of legacy components.

## Documentation

[`docs/README.md`](docs/README.md) is the index to the entire documentation set — start there if you're not sure which document has what you're looking for. [`docs/guides/quickstart.md`](docs/guides/quickstart.md) is the fastest real path from nothing to a running container.

See [`docs/roadmap/ROADMAP.md`](docs/roadmap/ROADMAP.md) for the full phase-by-phase history — what shipped, how each was verified. Architecture diagram: [`docs/architecture/architecture.svg`](docs/architecture/architecture.svg). API contract: [`docs/api/openapi.yaml`](docs/api/openapi.yaml), with a narrative walkthrough at [`docs/api/README.md`](docs/api/README.md). Why a given significant, hard-to-reverse decision was made: [`docs/adr/`](docs/adr/).

## Getting started

- **Build it**: [`docs/guides/building-cix.md`](docs/guides/building-cix.md) — on a dev machine, or self-hosted from a running Cix box with no separate dev machine at all.
- **Install it**: [`docs/guides/installing.md`](docs/guides/installing.md) — the installer ISO, disk partitioning, Secure Boot. [`docs/guides/quickstart.md`](docs/guides/quickstart.md) takes a fresh install to a first running container.
- **Use it**: [`docs/guides/cli-reference.md`](docs/guides/cli-reference.md) (the `cixctl` command surface) and [`docs/guides/web-dashboard.md`](docs/guides/web-dashboard.md) (the browser UI) — both pure REST clients over the same API documented in [`docs/api/README.md`](docs/api/README.md).
- **Run workloads**: [`docs/guides/containers-and-services.md`](docs/guides/containers-and-services.md) (containers and deployments), [`docs/guides/images.md`](docs/guides/images.md) (what a container runs from), and [`docs/guides/network-services.md`](docs/guides/network-services.md) (DNS, DHCP, NTP, syslog).
- **Administer it**: [`docs/guides/administration.md`](docs/guides/administration.md) (monitoring, backup/restore, disks), [`docs/guides/storage.md`](docs/guides/storage.md) (disks, roles, filesystems), [`docs/guides/networking.md`](docs/guides/networking.md) (networks, routing, VLANs), [`docs/guides/security.md`](docs/guides/security.md) (PKI, HTTPS, LDAP accounts), and [`docs/guides/reinstall-and-restore.md`](docs/guides/reinstall-and-restore.md) (wiping and reinstalling a host).
- **Keep it updated**: [`docs/guides/kernel-build-and-ab-updates.md`](docs/guides/kernel-build-and-ab-updates.md) and [`docs/guides/staying-updated.md`](docs/guides/staying-updated.md).
- **Extend it**: [`docs/guides/writing-recipes.md`](docs/guides/writing-recipes.md) — building real software from source into a `pkg install`-able package — and [`docs/guides/remote-development.md`](docs/guides/remote-development.md) for pushing local changes onto a real box.

## Repository Layout

```
include/       runtime library public API (container.h) and internal glue
src/           runtime implementation (cgroup, namespaces, mounts, overlay, container networking)
netplane/      custom rtnetlink control plane (bridges, veth, routes — no ip/iproute2, no OVS)
daemon/        cixd: the REST daemon — the only process with direct runtime/network/DNS/PKI access
client/        shared HTTP client library used by the CLI and the daemon's own test suite
cli/           cixctl: pure REST API client, no direct runtime access
web/           browser dashboard: vanilla HTML/CSS/JS, no framework, no bundler, served by cixd; web/api.js is generated from openapi.yaml by apigen
image/         bare-metal boot tooling: kernel config, mkbootroot, cix-install, mkinstalleriso
init/          cix-init: PID 1 in every container, freestanding (no libc) — the one exception to the TCC-and-glibc rule
tools/         developer tooling: apigen (generates the API surface from openapi.yaml), verify-symbols.sh (link-level symbol checks),
               carry-artifact-approvals.sh (copies artifact approvals from a daemon back into recipes),
               cpdl-coverage-audit.py (recipe-corpus CPDL audit), dashboard-screenshot.py (authenticated dashboard screenshots)
test/          the test suite: per-feature tests, contract gates, and the subset the release selftest runs
docs/          all documentation — see docs/README.md, which indexes every subdirectory
build/         compiled output (gitignored)
build-inputs/  hand-fetched build inputs that `make` does not reproduce (gitignored): the Phase 11 kernel bzImage, the test floor's package artifacts
```
