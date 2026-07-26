# Roadmap

Phased, dependency-ordered breakdown of the mission in [MISSION.md](MISSION.md). Each phase is designed in detail only when we reach it — see the Zen maxim. This page covers *what* shipped and how it was verified; *why* the significant decisions behind it were made lives in [docs/adr/](adr/README.md). Chronological summary: [CHANGELOG.md](../CHANGELOG.md).

| Phase | Name | Depends on | Status |
|---|---|---|---|
| 0 | Toolchain smoke test | — | Done |
| 1 | Namespace + cgroup v2 container harness | 0 | Done |
| 2 | OverlayFS root construction | 1 | Done |
| 3 | REST API spec + daemon (host + container lifecycle) | 1, 2 | Done |
| 4 | CLI (pure REST API client) | 3 | Done |
| 5 | Web dashboard (pure REST API client) | 3 | Not started |
| 6 | Custom virtual switch / rtnetlink data plane | 3 | Not started |
| 7 | Routing protocols (containerized VPNs/routers) | 6 | Not started |
| 8 | DNS service | 3, 6 | Not started |
| 9 | PKI / certificate management | 3 | Not started |
| 10 | Package manager | 1, 2, 4 | Not started |

**API-first, no exceptions (added after Phase 2):** the REST daemon is the only process with direct access to the runtime library or any host/network/DNS/PKI primitive. The CLI (4) and web dashboard (5) are pure REST clients — every capability they expose must exist as a REST endpoint first. This reordered the roadmap: the CLI can no longer come before the REST daemon, since it now depends on the daemon's API existing rather than linking against `container.h` directly. See `CLAUDE.md`'s API-First Mandate.

## Locked-in technical decisions (apply to every phase)

- Dynamic linking against **system glibc** always — never `-static`, never TCC's bundled headers.
- `pivot_root` and `clone3` have no glibc wrappers — call via `syscall(SYS_pivot_root, ...)` / `syscall(SYS_clone3, ...)`; `struct clone_args` is declared ourselves in `include/linux_compat.h` (never `#include <linux/sched.h>`, it clashes with glibc's `<sched.h>`).
- The REST daemon's event loop uses `epoll`, not `io_uring` (no glibc wrapper, heavier hand-rolled-uapi lift — an explicit future optimization, not a baseline requirement).
- Networking phases talk to the kernel via rtnetlink sockets directly in C — never shell out to `ip`/iproute2.
- Build: `tcc -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude ...` — zero warnings is a hard gate, not a suggestion.
- **TCC ignores `__attribute__((packed))` entirely** (confirmed on both the system `<sys/epoll.h>` struct and a minimal struct we wrote ourselves with the attribute) but *does* honor `#pragma pack`. Any kernel-ABI struct that relies on packing (`struct epoll_event` is the one hit so far: real ABI is 12 bytes/data at offset 4, TCC's default-aligned version is 16 bytes/data at offset 8) needs its own `#pragma pack`-based replacement in `include/linux_compat.h` rather than the system header's type — see `struct kx_epoll_event` / `kx_epoll_ctl()` / `kx_epoll_wait()`. Watch for this on every future struct pulled from a Linux uapi/glibc header before assuming its layout is safe under TCC.
- **Every fd the daemon opens for its own bookkeeping is `CLOEXEC` from creation** (`SOCK_CLOEXEC`/`EPOLL_CLOEXEC`/`O_CLOEXEC`, never patched on afterward). Any process built on `container_create()` routinely `clone3()`s new children from inside request-handling code, and an inherited non-CLOEXEC fd (a client connection socket, in the bug this caught) silently stalls that request until the spawned container itself exits — no crash, no error, just a severe, easy-to-miss latency regression. See ADR-0009.

## Phase 0 — Toolchain smoke test (done)

Confirmed TCC compiles and dynamically links against host glibc with `-Wall -Werror`, and every header family later phases need (`sched.h`, `sys/mount.h`, `sys/prctl.h`, `sys/wait.h`, `linux/netlink.h`, `sys/epoll.h`) parses clean. `test/test_toolchain.c`. See ADR-0001.

## Phase 1 — Namespace + cgroup v2 container harness (done)

Clone a child into new PID+MNT+UTS+NET(+CGROUP) namespaces, place it into a cgroup v2 leaf with memory/pids limits atomically at creation (`clone3` + `CLONE_INTO_CGROUP`, no TOCTOU write to `cgroup.procs`), pivot_root into a bind-mounted copy of host `/`, exec a target, reap race-free via pidfd. User-namespace/rootless support deferred to a later "Phase 1b." See ADR-0002 and ADR-0003 for why.

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

Real OverlayFS layering (lowerdir/upperdir/workdir) replaces Phase 1's bind-mount-of-live-host-root: `overlay_create()` mounts `mount("overlay", merged, "overlay", 0, "lowerdir=...,upperdir=...,workdir=...")` inside the child's own mount namespace, before `mountns_pivot()` pivots into `merged`. `lowerdir` must already exist and be populated — never auto-created, since a silently-empty lowerdir would mean a silently-broken container. The propagation-privatizing remount (`MS_REC|MS_PRIVATE`) was split out of `mountns_pivot()` into its own `mountns_make_private()` and now runs *before* the overlay mount, not after — otherwise the overlay mount could propagate back onto the host. See ADR-0004 for why the lowerdir is a dedicated tree rather than the live host root.

Implementation: `struct overlay_spec` + `overlay_create()` in `include/container.h`/`src/overlay.c`; `mountns_pivot()` in `src/mountns.c` no longer populates the root itself, it only requires `new_root` already be a mount point. `struct mount_spec.root_source` was removed (folded into `overlay_spec.merged`) to eliminate a duplicate-state footgun. Verified end to end by `test/test_overlay.c` (`test/overlay_child.c` is its exec target), against a minimal, purpose-built lowerdir (just the exec binary + the dynamic linker/libc it needs + a marker file — no debootstrap, no full userland):

1. Container `c1` runs against a fresh upperdir over the shared lowerdir, exits `42`.
2. `c1`'s upperdir has `status.txt` showing it saw the lowerdir's marker content correctly and no pre-existing file — proves shared-lowerdir visibility.
3. The lowerdir itself has no `status.txt` afterward — proves the shared lowerdir is never mutated by a container's writes (copy-up lands only in upperdir).
4. Container `c2` runs against a second fresh upperdir over the *same* lowerdir, exits `42`, and its `status.txt` also shows no pre-existing file — proves `c2` never saw `c1`'s diff (per-container upperdir isolation over a shared base).
5. Zero warnings under `-Wall -Werror`.
6. Phase 1's `test_harness` re-verified to still fully pass after this shared-code refactor (its `ov.lowerdir` is `/`, preserving its original full-host-visible content, now routed through the same real overlay mechanism instead of a raw bind mount — its result file now reads from upperdir's copy-up path rather than the bare host path, since containers can no longer write straight to the live host).

## Phase 3 — REST API spec + daemon: host + container lifecycle (done)

The API contract lives at `docs/api/openapi.yaml` (OpenAPI 3.0), written before any handler code: `GET /v1/health`, `GET/POST /v1/containers`, `GET/DELETE /v1/containers/{name}`. The daemon (`daemon/src/main.c`, binary `kanxeod`) is a single-threaded, non-blocking, `epoll`-driven reactor with no external libraries — its own minimal HTTP/1.1 parser (`daemon/src/http.c`, request-line + headers + fixed-length body, `Connection: close` only, no keep-alive/chunked — a deliberate v1 scope boundary) and its own minimal generic JSON parser/writer (`daemon/src/json.c`; `\uXXXX` escapes unsupported, also a deliberate boundary — no field in this API needs one). Containers live in a fixed-size in-memory table (`daemon/src/registry.c`) keyed by client-supplied name; safe to be in-memory-only (no restart persistence) because `container.c`'s existing `PR_SET_PDEATHSIG` means every running container dies automatically if the daemon exits, so there's no orphan/restart-reconciliation problem to solve.

New durable storage convention (first time this project needed one, versus `/tmp` test scratch): `/var/lib/kanxeo/images/<image>/rootfs` (a lowerdir — never auto-created by the daemon, same Phase-2 rule) and `/var/lib/kanxeo/containers/<name>/{upper,work,merged}`.

**A real, non-obvious bug found and fixed here, not just a test gap:** `epoll_ctl()`/`epoll_wait()` were silently corrupting their `data` field under TCC — see the `kx_epoll_event` entry in "Locked-in technical decisions" above, and ADR-0008 for the full root-cause writeup. It surfaced as an intermittent (~50-60% of runs) segfault on the very first request, requiring a core-dump-free `gdb`+`strace` bisection since the failure wasn't reproducible on demand.

Design rationale for the API-first reordering, the OpenAPI spec choice, and the daemon's hand-rolled/no-external-libs approach: ADR-0005, ADR-0006, ADR-0007.

Verified end to end by `test/test_daemon.c` (`test/daemon_child.c` is its exec target; takes a sleep-seconds and an exit-code argument) — drives the built daemon purely over real HTTP (originally hand-written request/response handling, generalized into `client/src/httpclient.c` in Phase 4 and reused from there since):

1. `GET /v1/health` → `200`.
2. Create a container that exits quickly with a known code; poll until `status:"exited"` and confirm the exact `exit_status` came back through the API.
3. `GET /v1/containers` lists it.
4. Create a second, long-running container and `DELETE` it while still running → `204`, and confirm its PID is actually gone from `/proc` (not just marked removed).
5. Deleted container → subsequent `GET` is `404`.
6. Duplicate name → `409`; missing required field → `400`.
7. `SIGTERM` → clean daemon exit (not killed by a timeout).
8. Zero warnings under `-Wall -Werror` across every new file.
9. Phase 1 (`test_harness`) and Phase 2 (`test_overlay`) re-run and still fully pass — no regression from this phase's `linux_compat.h`/shared-code changes.

## Phase 4 — CLI, pure REST API client (done)

`kanxeoctl` (`cli/src/main.c`) — one subcommand per endpoint (`health`, `ps`, `run`, `inspect`, `rm`), zero direct runtime access, per the API-First Mandate. `run`'s request body is built with the JSON writer (`jw_*`), not `snprintf` string concatenation, since its `name`/`image`/`cmd` values come from arbitrary user-supplied argv and must be properly escaped. `--json` prints the raw response (re-serialized through the same JSON writer the daemon itself uses to build responses, so there's no second JSON-rendering implementation); otherwise output is formatted text. Exit codes: `0` for a 2xx response, `1` for a 4xx/5xx or transport failure, `2` for a usage error — scriptable (`if kanxeoctl inspect foo; then ...`).

`test_daemon.c`'s ad hoc socket code (`do_request()`/`connect_to_daemon()`/`write_all()`) was extracted into `client/src/httpclient.c` (`kx_client_request()`) — a real, reusable "talk to the Kanxeo API" library, now shared by `kanxeoctl` and the daemon's own test suite, eliminating what would otherwise have been two independent implementations of the same concern. While generalizing it, the response buffer became dynamically-growing (same doubling pattern as `daemon/src/http.c`) instead of the test's fixed 16KB cap, since a real container listing can legitimately exceed that. `test/test_image_fixture.c` similarly extracts the shared test-image staging logic so `test_cli.c` doesn't duplicate it either.

**A second real, non-obvious bug found here, via the same stress-test discipline ADR-0008 established:** `test_daemon.c` was silently taking ~30 seconds per run (matching its long-running test container's sleep duration) instead of milliseconds — no failure, just a severe latency regression that repeated/back-to-back runs eventually made impossible to ignore. Root cause: none of the daemon's own fds (listening socket, accepted client sockets, the `epoll` fd, the cgroup `O_PATH` fd) were `CLOEXEC`, so every container `clone3()`'d from inside request-handling inherited a duplicate of the triggering client connection — and the kernel won't deliver EOF on a TCP connection until *every* reference to it, across every process, closes. Fixed by making every one of those fds `CLOEXEC` from creation. See ADR-0009. Verified fixed by timing, not just pass/fail: `test_daemon.c` went from ~30s to ~0.3s per run.

Verified end to end by `test/test_cli.c`, driving the real, built `kanxeoctl` binary as a subprocess (not calling `httpclient.c` directly, so it proves the actual shipped binary) — all of:

1. `health` → exit `0`.
2. `run --name=c1 --image=test -- /bin/daemon_child 0 5` → exit `0`, output shows `running`.
3. `ps` → output lists `c1`.
4. Poll `inspect c1` until output shows `exited` and `exit_status=5`.
5. `run --name=c2 ... /bin/daemon_child 30 0`, then `rm c2` while running → exit `0`; the PID (parsed from `run`'s own output) is confirmed gone from `/proc`; `inspect c2` afterward → nonzero exit.
6. Duplicate name → nonzero exit, error message present in output.
7. Daemon unreachable (wrong port) → exit `1`, no crash.
8. Zero warnings under `-Wall -Werror` across every new/changed file; `test_harness`, `test_overlay`, and the refactored `test_daemon` re-run and still fully pass.

## Phase 5 — Web dashboard, pure REST API client (next, not yet designed)

Same constraint as the CLI: a REST client only. Not designed yet.
