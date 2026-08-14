# Kanxeo

**The Architecture of Absolute Clarity**

Kanxeo is a rolling-release hardware and workload orchestration platform, compiled entirely from source: a hand-rolled container runtime on raw Linux namespaces and cgroups, OverlayFS-based image layering, a 100% custom C networking data plane, and a REST control layer for the host, containers, hardware, DNS, and PKI — no runc, no Open vSwitch, no eBPF, compiled exclusively with the Tiny C Compiler (TCC). Every capability, including hardware itself, is a first-class API resource; containers are where all real work happens, the host is the thinnest possible layer underneath them. Full charter: [`docs/mission/MISSION.md`](docs/mission/MISSION.md).

## The Name

**Kan·x·eo** fuses three words that govern how the system is engineered:

| Fragment | From | Principle |
|---|---|---|
| **Kan** | *Kanso* | The Zen aesthetic of simplicity and the elimination of clutter. No bloated toolchains, no stop-gap dependencies — just TCC, native namespace isolation, and everything unnecessary stripped away. |
| **x** | *Axiom* | A self-evident, undeniable truth. One Source of Truth: no parallel implementations, no regressions, no conflicting state. The host OS is the singular, unbreakable foundation — the shared lowerdir every container is built on. |
| **eo** | *Luceo* | Latin for clarity and illumination. Zero compile warnings, a fully integrated REST API, and internal state that's always transparent, accessible, and programmable. |

## What It Represents

Kanxeo is more than a distribution — it's a discipline. Complex routing protocols, VPNs, and container infrastructure are built systematically, one pristine layer at a time, with no hacks and no bypasses. It exists to prove that a completely unified, custom C-based stack can be more reliable than a patchwork of legacy components.

## Documentation

[`docs/README.md`](docs/README.md) is the index to the entire documentation set — start there if you're not sure which document has what you're looking for. [`docs/guides/quickstart.md`](docs/guides/quickstart.md) is the fastest real path from nothing to a running container.

See [`docs/roadmap/ROADMAP.md`](docs/roadmap/ROADMAP.md) for the full phase-by-phase history — what shipped, how each was verified. Architecture diagram: [`docs/architecture/architecture.svg`](docs/architecture/architecture.svg). API contract: [`docs/api/openapi.yaml`](docs/api/openapi.yaml), with a narrative walkthrough at [`docs/api/README.md`](docs/api/README.md). Why a given significant, hard-to-reverse decision was made: [`docs/adr/`](docs/adr/).

## Getting started

- **Build it**: [`docs/guides/building-kanxeo.md`](docs/guides/building-kanxeo.md) — on a dev machine, or self-hosted from a running Kanxeo box with no separate dev machine at all.
- **Install it**: [`docs/guides/installing.md`](docs/guides/installing.md) — the installer ISO, disk partitioning, Secure Boot.
- **Use it**: [`docs/guides/cli-reference.md`](docs/guides/cli-reference.md) (the `kanxeoctl` command surface) and [`docs/guides/web-dashboard.md`](docs/guides/web-dashboard.md) (the browser UI) — both pure REST clients over the same API documented in [`docs/api/README.md`](docs/api/README.md).
- **Administer it**: [`docs/guides/administration.md`](docs/guides/administration.md) (monitoring, backup/restore, disks), [`docs/guides/networking.md`](docs/guides/networking.md) (networks, routing, VLANs), and [`docs/guides/security.md`](docs/guides/security.md) (PKI, HTTPS, LDAP accounts).
- **Keep it updated**: [`docs/guides/kernel-build-and-ab-updates.md`](docs/guides/kernel-build-and-ab-updates.md) and [`docs/guides/staying-updated.md`](docs/guides/staying-updated.md).
- **Extend it**: [`docs/guides/writing-recipes.md`](docs/guides/writing-recipes.md) — building real software from source into a `pkg install`-able package.

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
recipes/       package/ + image/ build & manifest recipes for `pkg install`/`image apply-recipe` (see recipes/README.md and docs/guides/writing-recipes.md)
test/          one demonstrable test (+ exec target, where needed) per phase/part
docs/          mission/roadmap/adr/api/architecture/guides — see docs/README.md for what lives where
build/         compiled output (gitignored)
```
