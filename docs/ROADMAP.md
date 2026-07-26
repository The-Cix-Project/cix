# Roadmap

Phased, dependency-ordered breakdown of the mission in [MISSION.md](MISSION.md). Each phase is designed in detail only when we reach it — see the Zen maxim.

| Phase | Name | Depends on | Status |
|---|---|---|---|
| 0 | Toolchain smoke test | — | Done |
| 1 | Namespace + cgroup v2 container harness | 0 | Done |
| 2 | OverlayFS root construction | 1 | Done |
| 3 | Minimal container CLI | 1, 2 | Next |
| 4 | REST daemon skeleton | — (parallel-safe) | Not started |
| 5 | REST container lifecycle endpoints | 3, 4 | Not started |
| 6 | Custom virtual switch / rtnetlink data plane | 4 | Not started |
| 7 | Routing protocols (containerized VPNs/routers) | 6 | Not started |
| 8 | DNS service | 4, 6 | Not started |
| 9 | PKI / certificate management | 4 | Not started |
| 10 | Package manager | 1–3 | Not started |

## Locked-in technical decisions (apply to every phase)

- Dynamic linking against **system glibc** always — never `-static`, never TCC's bundled headers.
- `pivot_root` and `clone3` have no glibc wrappers — call via `syscall(SYS_pivot_root, ...)` / `syscall(SYS_clone3, ...)`; `struct clone_args` is declared ourselves in `include/linux_compat.h` (never `#include <linux/sched.h>`, it clashes with glibc's `<sched.h>`).
- The REST daemon's event loop (Phase 4) uses `epoll`, not `io_uring` (no glibc wrapper, heavier hand-rolled-uapi lift — an explicit future optimization, not a baseline requirement).
- Networking phases talk to the kernel via rtnetlink sockets directly in C — never shell out to `ip`/iproute2.
- Build: `tcc -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude ...` — zero warnings is a hard gate, not a suggestion.

## Phase 0 — Toolchain smoke test (done)

Confirmed TCC compiles and dynamically links against host glibc with `-Wall -Werror`, and every header family later phases need (`sched.h`, `sys/mount.h`, `sys/prctl.h`, `sys/wait.h`, `linux/netlink.h`, `sys/epoll.h`) parses clean. `test/test_toolchain.c`.

## Phase 1 — Namespace + cgroup v2 container harness (done)

Clone a child into new PID+MNT+UTS+NET(+CGROUP) namespaces, place it into a cgroup v2 leaf with memory/pids limits atomically at creation (`clone3` + `CLONE_INTO_CGROUP`, no TOCTOU write to `cgroup.procs`), pivot_root into a bind-mounted copy of host `/`, exec a target, reap race-free via pidfd. User-namespace/rootless support deferred to a later "Phase 1b."

Implementation: `include/container.h`, `include/linux_compat.h`, `include/internal.h`, `src/cgroup.c`, `src/ns_create.c`, `src/mountns.c`, `src/container.c`. Verified end to end by `test/test_harness.c` (`test/harness_child.c` is its exec target) — all of:

1. No nesting/seccomp-blocked syscalls.
2. Child exit status `7` — `execve` ran inside the new namespaces.
3. Child PID `1` — PID namespace isolation.
4. Child hostname `container-test`, host hostname unchanged — UTS isolation without global mutation.
5. `/sys/class/net` (and `/proc/net/dev`) inside the child shows only `lo` — NET namespace isolation.
6. `cgroup.procs` has exactly one PID, matching clone3's return value — atomic cgroup placement, no race window.
7. Zero warnings under `-Wall -Werror`.

Runs only on a **privileged** LXC container/host — an unprivileged nested LXC container blocks the final `mount("proc", ...)` with `EPERM`/"VFS: Mount too revealing" regardless of mount flags or namespace combination tried (confirmed by elimination, not guesswork; resolved by converting this dev container to privileged).

## Phase 2 — OverlayFS root construction (done)

Real OverlayFS layering (lowerdir/upperdir/workdir) replaces Phase 1's bind-mount-of-live-host-root: `overlay_create()` mounts `mount("overlay", merged, "overlay", 0, "lowerdir=...,upperdir=...,workdir=...")` inside the child's own mount namespace, before `mountns_pivot()` pivots into `merged`. `lowerdir` must already exist and be populated — never auto-created, since a silently-empty lowerdir would mean a silently-broken container. The propagation-privatizing remount (`MS_REC|MS_PRIVATE`) was split out of `mountns_pivot()` into its own `mountns_make_private()` and now runs *before* the overlay mount, not after — otherwise the overlay mount could propagate back onto the host.

Implementation: `struct overlay_spec` + `overlay_create()` in `include/container.h`/`src/overlay.c`; `mountns_pivot()` in `src/mountns.c` no longer populates the root itself, it only requires `new_root` already be a mount point. `struct mount_spec.root_source` was removed (folded into `overlay_spec.merged`) to eliminate a duplicate-state footgun. Verified end to end by `test/test_overlay.c` (`test/overlay_child.c` is its exec target), against a minimal, purpose-built lowerdir (just the exec binary + the dynamic linker/libc it needs + a marker file — no debootstrap, no full userland):

1. Container `c1` runs against a fresh upperdir over the shared lowerdir, exits `42`.
2. `c1`'s upperdir has `status.txt` showing it saw the lowerdir's marker content correctly and no pre-existing file — proves shared-lowerdir visibility.
3. The lowerdir itself has no `status.txt` afterward — proves the shared lowerdir is never mutated by a container's writes (copy-up lands only in upperdir).
4. Container `c2` runs against a second fresh upperdir over the *same* lowerdir, exits `42`, and its `status.txt` also shows no pre-existing file — proves `c2` never saw `c1`'s diff (per-container upperdir isolation over a shared base).
5. Zero warnings under `-Wall -Werror`.
6. Phase 1's `test_harness` re-verified to still fully pass after this shared-code refactor (its `ov.lowerdir` is `/`, preserving its original full-host-visible content, now routed through the same real overlay mechanism instead of a raw bind mount — its result file now reads from upperdir's copy-up path rather than the bare host path, since containers can no longer write straight to the live host).

## Phase 3 — Minimal container CLI (next, not yet designed)

Will wrap `container_create()`/`container_wait()` in a small command-line tool. Detailed design (structs, syscalls, CLI surface) to be written when we start this phase, not before.
