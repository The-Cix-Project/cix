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
| 3 | Minimal container CLI | Next |
| 4–10 | REST daemon, vswitch, routing, DNS, PKI, package manager | Not started |

Full charter: [`docs/MISSION.md`](docs/MISSION.md). Phase-by-phase design and verification detail: [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Building

Requires `tcc` and a Linux kernel with cgroup v2 and `clone3`/`CLONE_INTO_CGROUP` support (5.7+). Namespace/mount tests must run as root and require a **privileged** container if run inside one — see `docs/ROADMAP.md` Phase 1 for why.

```sh
make              # builds everything into build/, -Wall -Werror, zero warnings
sudo build/test_harness   # Phase 1: namespace + cgroup harness
sudo build/test_overlay   # Phase 2: OverlayFS layering
make clean
```

## Repository Layout

```
include/   public API headers (container.h) and internal glue
src/       runtime implementation (cgroup, namespaces, mounts, overlay, orchestration)
test/      one demonstrable test + exec target per phase
docs/      mission charter and phased roadmap
build/     compiled output (gitignored)
```
