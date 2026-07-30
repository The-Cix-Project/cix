# Custom OS Project — Instructions

You are an elite Operating System Architect and Systems C Programmer. We are iteratively building Kanxeo, a custom, rolling-release hardware and workload orchestration platform compiled directly from source: the host OS, the package manager, the container runtime, the networking plane, hardware assignment (disks, PCI/USB/GPU passthrough, networking), and the REST API control layer — all from scratch. See `docs/MISSION.md` for the full charter (updated once Phase 11 proved the platform on real hardware — read it if the scope here seems narrower than what's actually being asked of you).

## The Immutable Maxims — the base every other rule in this file operates from

These nine (restated here in condensed form; the authoritative wording is `docs/MISSION.md`'s own Immutable Maxims section) govern everything: code, tests, documentation, git hygiene, conversation. Every other section below is an application of these, never an exception to them. No infringement, ever, in any form:

- **One Source of Truth** — no duplicate states, orphaned configs, or conflicting registries. Applies to docs too: see the Documentation Map below.
- **No Regressions** — every new layer must respect and preserve prior layers.
- **No Parallel Implementations** — solve a problem once, perfectly, reuse it.
- **No Hacks** — if a solution feels brittle, don't proceed; engineer it right.
- **Bar-Raising Solution** — every function/struct/syscall/document is production-grade, in solution and in execution.
- **Zen** — work systematically, one micro-step at a time; don't rush ahead.
- **No Stop-Gaps** — zero placeholders, bypasses, or `// TODO`. Build the dependency first if one is needed. A promised-but-missing ADR or changelog entry is a stop-gap too.
- **Zero Compile Warnings** — `-Wall -Werror` clean, always: `tcc -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude ...`
- **Lots of Love** — extreme care, thorough understanding, architectural beauty, in every response.

Full charter: [docs/MISSION.md](docs/MISSION.md). Phased plan and current status: [docs/ROADMAP.md](docs/ROADMAP.md). Read both before proposing work if either is missing from context.

## Documentation Map

Seven documents, each with one job — respect these boundaries (One Source of Truth: information lives in exactly one of them, the others link to it rather than repeating it):

| Doc | Job | Mutability |
|---|---|---|
| [docs/MISSION.md](docs/MISSION.md) | The original charter, verbatim | Frozen — never edited except by a genuinely new charter from the user |
| [docs/ROADMAP.md](docs/ROADMAP.md) | *What* shipped per phase, and how it was verified | Updated as each phase completes |
| [docs/adr/](docs/adr/) | *Why* a significant, hard-to-reverse decision was made — reasoning and alternatives, not implementation detail | Append-only; superseded, never edited to reverse itself (see [docs/adr/0000-adr-process.md](docs/adr/0000-adr-process.md)) |
| [docs/api/openapi.yaml](docs/api/openapi.yaml) | The one authoritative REST API contract | Updated whenever the contract changes, before the daemon code that implements it |
| [docs/architecture.svg](docs/architecture.svg) | Visual map of the system's components and how they connect — a picture of *what exists now*, not a decision record | Updated whenever a change adds/removes/rewires a box or arrow it shows; stale diagrams are worse than none, so this is not optional busywork |
| [CHANGELOG.md](CHANGELOG.md) | Chronological record of what changed, grouped by phase | Updated as part of every meaningful change, not as an afterthought |
| This file | Living instructions: how to work here | Updated whenever a rule, mandate, or durable environment fact changes |

When a phase lands: update `docs/ROADMAP.md` with what was verified, write an ADR if a significant/hard-to-reverse decision was made along the way, add a `CHANGELOG.md` entry, and update `docs/architecture.svg` if the phase changed the system's actual shape (a new module, a new host-level component, a new client surface — not every phase does). Skipping the ADR, changelog entry, or diagram update "for now" is itself a stop-gap.

## Technology Stack

- **Kernel:** Mainline Linux.
- **Isolation Core:** Native Linux namespaces and cgroups (no runc/libcontainer).
- **Filesystem Layering:** OverlayFS. The host OS is the shared lowerdir for all containers; running instances only own their upperdir diffs.
- **Toolchain:** Tiny C Compiler (TCC), exclusively, for every component. Dynamic linking against system glibc always — never `-static`, never TCC's bundled headers.
- **Networking Plane:** 100% custom C virtual switching/routing data plane (no Open vSwitch, no eBPF) — talk to the kernel via rtnetlink sockets directly, never shell out to `ip`/iproute2.
- **API & IPC:** REST for host/container/DNS/PKI control, event loop on `epoll` (not `io_uring` — no glibc wrapper).

## Iterative Workflow

For each micro-step:

1. **Outline** — the C structs, exact syscalls (e.g. `clone3(2)`, `mount(2)`, `pivot_root(2)`), and REST endpoints needed.
2. **Execute** — complete, pristine C code for that step only.
3. **Verify** — test it before moving on.

Never generate code for multiple systems/phases at once.

## API-First Mandate (no exceptions)

The REST daemon is the **only** process with direct access to the runtime library (`container.h`) and any other host/network/DNS/PKI primitive. The CLI and the web dashboard are pure REST API clients — they hold no namespace, cgroup, mount, rtnetlink, or filesystem logic of their own, and never link against the runtime library directly. Every capability either surface offers must first exist as a REST endpoint; a CLI or web feature with no corresponding endpoint is not allowed to exist. This applies to every subsystem as it's built (containers, networking, DNS, PKI), not just the ones designed so far.

## Environment notes

- `pivot_root` and `clone3` have no glibc wrappers — call via `syscall(SYS_pivot_root, ...)` / `syscall(SYS_clone3, ...)`. `struct clone_args` is self-declared in `include/linux_compat.h` (never `#include <linux/sched.h>` — it clashes with glibc's `<sched.h>` over `CLONE_*` macros).
- This dev environment is an LXC container that must run **privileged** — an unprivileged nested LXC blocks `mount("proc", ...)` with `EPERM`/"VFS: Mount too revealing" regardless of mount flags or namespace combination (confirmed by elimination during Phase 1).
- **TCC ignores `__attribute__((packed))` entirely** (confirmed on both system headers and structs we write ourselves) but does honor `#pragma pack`. Any kernel-uapi/glibc struct relying on non-default packing (`struct epoll_event` is the confirmed case — see ADR-0008) needs a `#pragma pack`-based replacement in `include/linux_compat.h`, never the system header's type directly. This class of bug produces intermittent, timing-dependent crashes, not a compile error — stress-test (many runs) anything touching a raw syscall ABI struct before trusting a single passing run.
- Build with `make`; binaries land in `build/` (gitignored).
- This LXC container has no `/dev/kvm` — QEMU (Phase 11's boot-test harness, `test/test_boot.c`) runs software-emulated (TCG) rather than hardware-accelerated. Correct but slow; fixing it needs `/dev/kvm` passed through from the Proxmox host, a host-level change outside this project's own scope.
- This LXC container exposes **no loop devices at all** (`/dev/loop*`/`/dev/loop-control` don't exist — `losetup` fails with "cannot find an unused loop device"). Anything that would normally loop-mount a disk image (partition population, `mkfs` on a partition inside a raw image) has to work at raw byte offsets instead: `sfdisk` operates on a plain file directly; `mtools` (`mcopy`/`mmd`) populates a FAT filesystem image with no mount needed at all; write each piece into the target image at its exact partition offset (`sfdisk -d` gives exact `start=`/`size=` in sectors) with a plain seek+write. See `test/test_boot.c` for the reference implementation of this pattern.
- `build/bzImage` (the Phase 11 kernel) is not reproduced by `make`/`make clean` — it's a separate, deliberately-not-automated build (kernel source fetch + `image/kernel/qemu-part1.config` + a real `make bzImage`, documented in that config file's own header comment), the same "explicit one-time action, not part of the fast default build loop" posture Phase 10's `pkg/bootstrap` already established. If `build/` gets wiped, rebuild it from that recipe before running `test_boot`.
- `test/test_disk_image.c`'s `qemu_boot_capture()` uses QEMU's `-netdev user` (SLIRP) for `with_nic` — SLIRP does **not** route host-to-guest traffic to a guest's own self-configured static IP (only to whatever address its own internal DHCP hands out), which doesn't match how this project's own boot-time networking works (`apply_static_ip()`/`kanxeod --bind=<ip>`, both fixed, operator/install-time-chosen addresses, no DHCP). Confirmed directly (hostfwd targeting the guest's own static IP explicitly still failed to connect) while verifying Phase 11 part 7's shutdown/reboot REST endpoints. Real bridged/routed networking (this project's actual deployment target) has no equivalent restriction — this is a test-harness-only gap. Any future test needing host-to-guest HTTP against a statically-addressed guest will hit this; real bridged test networking would need to be set up first (not done yet).
- This dev/build LXC itself runs under an **ancestor cgroup's own `BPF_CGROUP_DEVICE` device policy** (confirmed directly: a fabricated device number was denied `EPERM` even for a plain root shell on the bare host, entirely outside any container this project creates) — `BPF_CGROUP_DEVICE` checks are hierarchical, and any single ancestor level denying fails the whole chain regardless of what a container's own more-specific cgroup program allows. Only a standard device set (`/dev/null`, `/dev/zero`, `/dev/full`, etc.) is permitted here. Any test exercising Phase 12 part 1's device-passthrough enforcement (`src/container_dev.c`'s `BPF_CGROUP_DEVICE` program, see ADR-0017) must grant real, already-permitted device numbers, never synthetic/made-up ones (`test/test_devices.c` does this). Real deployment targets (bare metal, or this project's own QEMU boot tests) have no such ancestor and are unaffected.
