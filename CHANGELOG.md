# Changelog

All notable changes to this project are recorded here. Format is loosely [Keep a Changelog](https://keepachangelog.com/)-style, adapted for a rolling-release OS built phase by phase rather than a semantically-versioned library: entries are grouped by roadmap phase (see `docs/ROADMAP.md`), newest first. This file is updated as part of every meaningful change, not as an afterthought — see `CLAUDE.md`'s Documentation Map.

## [Unreleased]

### Phase 7 follow-up: raise the per-container network cap

#### Changed
- `include/container.h`: `CONTAINER_MAX_NETWORKS` raised from `4` to `64`, matching `daemon/include/network.h`'s `NETWORK_MAX` — a container can never attach to more networks than could possibly exist, so that's the real ceiling, not an arbitrary round number picked without justification. `cli/src/main.c`'s mirrored `CLI_MAX_NETWORKS` raised to match. `docs/api/openapi.yaml`'s `ContainerCreateRequest.networks.maxItems` and the daemon's validation error message updated accordingly.
- Deliberately still a fixed, bounded array (not switched to dynamic/heap allocation) — consistent with every other bounded table in this codebase (`REGISTRY_MAX_CONTAINERS`, `NETWORK_MAX`, `argv_buf[64]`). A cap that can't realistically be hit isn't the same problem as no cap at all.

## Phase 7 (parts 1–3): dynamic networks, multi-homed containers, IP forwarding + static routes — the container router

### Phase 7 (part 3): IP forwarding + static routes

#### Added
- `include/container.h`: `struct route_spec` (`dest_be`, `dest_prefix_len`, `gateway_be`) + `CONTAINER_MAX_ROUTES` (`8`); `container_spec` gains `ip_forward` and `routes[]`/`route_count`.
- `netplane/`: `rtnl_route_add_ipv4(fd, dest_be, dest_prefix_len, gateway_be)` generalizes the existing default-route-only helper — `rtnl_route_add_default_ipv4()` is now a one-line wrapper around it, so there's still exactly one implementation that builds an `RTM_NEWROUTE` message.
- `src/container_net.c`: `container_net_install_routes()` and `container_net_enable_ip_forward()` — both run in the child after interface setup, needing no pipe synchronization (only the child's own already-established netns is touched), both following the exact same `perror()`+`_exit(126)` failure contract every other post-barrier child-side step already uses.
- `daemon/src/main.c`: `handle_create()` parses `ip_forward` (bool) and `routes` (0-8 `{dest, prefix_len, via}` entries, format-validated: well-formed IPv4, prefix `0-32`). `registry_entry` gains `ip_forward` (routes themselves are deliberately not stored/echoed back — a stated scope boundary, not a gap).
- `docs/api/openapi.yaml`: `ContainerCreateRequest.ip_forward`/`routes` (new `RouteSpec` schema); `Container.ip_forward`.
- `cli/src/main.c`: `kanxeoctl run --ip-forward` and repeatable `--route=DEST/PREFIX:VIA`; `fmt_container_line()` shows `fwd=yes/no`.
- `web/`: an IP forwarding checkbox and comma-separated static-routes field on the create form; a Fwd column on the containers table.
- `test/net_connect.c`: a new exec target that connects *out* to a given IP (unlike `net_child`, which only listens) — needed because proving packets are forwarded through a third container requires the connection to originate from *inside* the calling container's own netns/routing table, not the host's (every other connectivity check in this project connects from the host, which isn't a router in these tests and would prove nothing about forwarding).
- `test/test_container_net.c` gained a real 3-container router topology (R on two networks with `ip_forward` on; H and T each with a static route via R for the other's subnet, including T's *reply* route — the real asymmetric-routing case that makes this a meaningful test) proving genuine L3 forwarding through R's kernel routing table. `test/test_daemon_net.c` gained the same topology driven entirely over real HTTP; `test/test_cli.c` gained a scenario confirming `--ip-forward`/`--route=` are plumbed through by the real binary.

#### Changed
- `docs/ROADMAP.md`: Phase 7 marked **done** (all three parts). No new ADR — the route-primitive generalization and the set-once-at-creation scope boundary are consistent extensions of ADR-0011, not a new durable architectural stance.

#### Fixed
- **`test_daemon_net.c`'s router scenario had T and H's gateway IPs swapped in the first draft**: each container's static route must go via the router's IP on *that container's own* subnet (a gateway has to be directly reachable on one of the container's own connected subnets), not the router's IP on the far side. Caught immediately by the test itself (`ENETUNREACH` from `container_net_install_routes()`), fixed, and re-verified before moving on — not left to surface later.

### Phase 7 (part 2): multi-homed containers

#### Added
- `include/container.h`: `struct container_spec.net` (singular) replaced by `nets[CONTAINER_MAX_NETWORKS]` + `net_count` — a container can now attach to up to 4 networks at creation; `net_count == 0` is exactly the pre-existing isolated-netns behavior, unchanged.
- `src/container_net.c`: `container_net_host_setup()`/`container_net_child_configure()` loop over the attachment array (one `rtnl_open()` for the whole loop). Veth naming gains an index suffix (`vh<pid>-<idx>`/`vc<pid>-<idx>`, one pair per attachment); the child renames each to `eth<idx>` instead of a hardcoded `eth0`. Only the first attachment ("primary") gets the default route — the rest get their subnet's connected route automatically from the address assignment, no extra syscall needed.
- `daemon/include/registry.h`: new `struct registry_network_attachment` (name + ip); `registry_entry`'s single `ip_be`/`network` fields replaced by `nets[CONTAINER_MAX_NETWORKS]` + `net_count`. `registry_network_in_use()`/`registry_alloc_ip()` generalized to scan the array; `registry_alloc_ip()`'s own contract is otherwise unchanged.
- `daemon/src/main.c`: `handle_create()` parses a `"networks"` array (1–4 entries) instead of a singular `"network"` string; allocates one IP per entry (no rollback needed on a partial failure, since allocation was already a pure scan with nothing reserved out-of-band).
- `docs/api/openapi.yaml`: `ContainerCreateRequest.networks` (array, 1–4 items) replaces `network`; `Container.networks` (array of `{name, ip}`) replaces the singular `network`/`ip` fields.
- `cli/src/main.c`: `--network=NAME` is now **repeatable** (same flag, appends to the request array); `fmt_container_line()` renders every attachment (`networks=name:ip,name:ip` or `-`).
- `web/`: the create form's Network field accepts a comma-separated list, split client-side into the request array; the containers table's IP column renders every attachment.
- `test/net_child.c` gained an optional argv connection-count (default `1`, every existing caller unchanged) so proving connectivity to a multi-homed container from more than one of its networks didn't need a second, near-duplicate exec target.
- `test/test_container_net.c` gained a scenario: one container on two bridges at once, real TCP connectivity to both independently-addressed interfaces. `test/test_daemon_net.c` and `test/test_cli.c` gained matching multi-network scenarios over real HTTP and the real `kanxeoctl` binary respectively.

#### Changed
- `docs/ROADMAP.md`: Phase 7 marked in progress (parts 1–2 done); part 3 (IP forwarding + static routes) still explicitly not designed, per Zen.
- API shape: this is the **second** time a networking field has changed shape across two phases (singular → array, following Phase 6 part 3 → Phase 7 part 1's `enum:[default]` → free-form string) — each one lifting a stated v1 limitation from the phase before it, not an unplanned break.

### Phase 7 (part 1): dynamic, REST-managed networks

#### Added
- `daemon/include/network.h` + `daemon/src/network.c`: a real `Network` resource (`POST/GET/DELETE /v1/networks`) — `struct network_def` (name, subnet, prefix length, derived gateway) in a fixed-size table, mirroring `registry.c`'s existing shape. `network_alloc_ip()` delegates straight to the existing, already topology-agnostic `registry_alloc_ip()` (Phase 6 part 3) — no changes needed there.
- The project's **first durable host-state persistence file** (ADR-0012), `/var/lib/kanxeo/networks.json`: unlike containers (safe to be in-memory-only, since they die with the daemon), a bridge created via this API outlives the process, so it's written atomically (temp file, `fsync`, `rename()`) on every create/delete and reloaded (bridges recreated idempotently, `EEXIST` tolerated) at startup.
- `daemon/include/registry.h`/`.c`: `struct registry_entry` gains `network` (the attached network's name, recorded directly) so `network_delete()` can refuse removal while a container is still attached.
- Validation in `network_create()`: unique 1–15 char name (`IFNAMSIZ` — the name *is* the bridge's ifname), subnet's host bits must be zero for the given prefix, prefix length in `[8,30]`, no overlap with any existing network's range. Gateway is always the subnet's first host address, never independently settable.
- `cli/src/main.c`: `kanxeoctl network create/ls/rm`, mirroring the container subcommands exactly.
- `web/`: a Networks section (table + create form + remove button), same pattern as the containers section.
- `test/test_networks.c`: create/list/get, full validation coverage (duplicate/misaligned/overlapping/out-of-range), delete-while-in-use refusal, and a restart-survival proof (create a network, restart the daemon, confirm the API and the underlying bridge both still show it without a second create call — the actual reason the persistence file exists).

#### Changed
- `docs/ROADMAP.md`: Phase 7 marked in progress (part 1 done); parts 2 (multi-homed containers) and 3 (IP forwarding + static routes) explicitly not designed yet, per Zen.
- `daemon/src/main.c`: the old hardcoded `DEFAULT_BRIDGE`/`ensure_default_network()`/single-fixed-subnet machinery (Phase 6 part 3) is gone, replaced entirely by the dynamic network table. `ContainerCreateRequest.network` is no longer `enum: [default]` — any name created via `POST /v1/networks` is valid.
- `test/test_daemon_net.c` and `test/test_cli.c`: their networking scenarios now create their own network via the new API first, since there is no more free-standing "default" to assume.
- `test/test_net_cleanup.c`/`.h` **removed**: the retrying `kanxeo0` cleanup it existed for (Phase 6 part 3) is now dead code — nothing creates a bridge unconditionally at daemon startup anymore, so `test_daemon.c`/`test_web.c`/`test_cli.c` no longer need it either. The class of bug it was written to paper over goes away by construction, not by another patch.

#### Fixed
- **`network_create()` always failed with a `500`, rolling back (deleting) the bridge it had just successfully created.** Root cause: `save_state()` compared the number of bytes written against `w.len` *after* calling `jw_free(&w)`, which resets `len` to `0` — so the check always saw a mismatch on an actually-successful write. Fixed by capturing the length before freeing the writer. Caught by `test_networks.c`'s very first assertion, before this part was ever declared done.

## Phase 6 (parts 2–3): wired into containers, exposed through the daemon/API/CLI/dashboard

### Phase 6 (part 3): exposed through the daemon/API/CLI/dashboard

#### Added
- `daemon/include/registry.h`/`.c`: `struct registry_entry` gains `ip_be`; `registry_create()` takes it as a construction parameter (set atomically, not poked in afterward — closes off a stale-IP-from-a-reused-slot footgun); new `registry_alloc_ip(network_base_be, host_min, host_max, *out_ip_be)`, kept topology-agnostic (subnet passed in, no hardcoded network knowledge in the registry).
- `daemon/src/main.c`: `ensure_default_network()` — creates bridge `kanxeo0` (`172.30.0.0/24`, gateway `.1`) at startup, tolerating `EEXIST` on both the bridge and the address assignment, same idempotent-restart pattern as the existing `ensure_dir()` calls. `handle_create()` gains an optional `"network"` field (`"default"` only in v1; anything else → `400`), allocates an IP, and populates `struct network_spec` from Part 2.
- `docs/api/openapi.yaml`: `ContainerCreateRequest.network`, `Container.ip` (nullable).
- `cli/src/main.c`: `kanxeoctl run --network=default`; `ip=` shown in `ps`/`inspect` output.
- `web/index.html`/`app.js`: a Network field on the create form, an IP column in the container table.
- `test/net_child.c` + `test/test_daemon_net.c`: end-to-end verification over real HTTP — two containers on `"default"` get distinct real IPs with real TCP connectivity to each, `GET /v1/containers` reflects both correctly, omitting `network` still yields `ip:null` (explicit regression check), an unsupported network name is `400`.
- `test/test_cli.c`: a new scenario driving the real `kanxeoctl` binary with `--network=default`, checking its output shows a real `ip=`.
- `test/test_net_cleanup.c`/`.h`: shared `test_cleanup_bridge()` — retries deleting a bridge (up to 20×, 100ms apart) rather than a single attempt, since netns/veth teardown runs on a kernel workqueue and can lag briefly. Used by every test that transitively starts a real `kanxeod`, since `ensure_default_network()` now creates `kanxeo0` as a side effect of every daemon startup.

#### Changed
- `docs/ROADMAP.md`: Phase 6 marked done (all three parts).

#### Fixed — test-hygiene bug, not a bug in the shipped daemon/library
- `test/test_rtnetlink.c` (Part 1) started hanging intermittently. Root cause: it creates its own standalone bridge on the same `172.30.0.0/24` subnet that `ensure_default_network()` now uses for `kanxeo0`, and `kanxeo0` left behind by a prior `test_daemon.c`/`test_cli.c`/`test_web.c`/`test_daemon_net.c` run (each of which starts a real `kanxeod`, and so silently creates `kanxeo0`, whether or not that specific test exercises networking) collided with it. Fixed by giving all four of those tests the same retrying cleanup via the new shared `test_cleanup_bridge()`, rather than four separate copies of the same retry loop.
- `test/test_overlay.c` (Phase 2), found while stress-running the full suite repeatedly to confirm the above fix: it reuses fixed `/tmp/overlay_test/...` paths across runs (needed for its host-side "did a previous container's write leak here" assertions) but never cleared them first, so a second invocation always failed against the first's leftover `status.txt`. Fixed with a small `nftw()`-based `rm_tree()` at the start of `build_lowerdir()`. Unrelated to networking, but blocked exactly the repeated-run stress-testing discipline ADR-0008/ADR-0009 established, so fixed alongside this phase's other test-hygiene work rather than left for a future phase to rediscover.

### Phase 6 (part 2): wired into the real container lifecycle

#### Added
- `include/container.h`: `struct network_spec` on `container_spec` — opt-in (`bridge == NULL` means exactly today's isolated-netns behavior, unchanged; `test_harness.c`/`test_overlay.c` needed zero source changes, confirmed by re-running them).
- `src/container_net.c` (+ declarations in `include/internal.h`): `container_net_host_setup()`/`container_net_child_configure()`, following the existing one-file-per-concern pattern (`mountns.c`, `overlay.c`).
- `netplane/`'s `rtnl_link_rename()` — needed to rename a container's veth end to `eth0`; the one operation that must identify its target by ifindex rather than `IFLA_IFNAME`, since that attribute means "set this as the new name" here.
- A synchronization barrier in `container_create()` (a `pipe()` inherited across `clone3()`) so the parent can move a veth into the child's netns before the child touches it. The container-side veth's name is sent *through* that same pipe, not recovered via `getpid()` in the child — `CLONE_NEWPID` means the child sees itself as pid 1 in its own namespace, so it can't know the name the parent used.
- `test/net_child.c` + `test/test_container_net.c`: end-to-end verification through the real `container_create()` (not the raw rtnetlink primitives) — two containers, concurrently, on the same bridge, with real simultaneous TCP connectivity to each, and confirmation that the kernel auto-removes both veth ends once each container exits.
- `test/test_image_fixture.c`/`.h` gained a third parameter (destination basename) so `test_container_net.c` could reuse it for `net_child` instead of writing a second, near-duplicate staging function.

#### Changed
- `docs/ROADMAP.md`: Phase 6 marked in progress (parts 1–2 done); part 3 (daemon bridge lifecycle + IP allocation + REST/CLI/dashboard exposure) still explicitly deferred, per Zen.

#### Fixed / real-world kernel behavior learned
- `container_create()`'s failure path when the parent's host-side network setup fails: closes the pipe's write end *without writing*, so the already-blocked child's `read()` sees EOF and fails cleanly (`_exit(126)`) instead of hanging forever; the parent then reaps it and reports the whole call as failed, same contract every other early-return in that function already honors.
- Network namespace (and veth) teardown runs on a kernel workqueue, not synchronously with the exiting process being reaped — an interface can briefly still exist right after `waitid()` returns. `test_container_net.c` polls for its disappearance (same pattern as every other "wait for something async" check in this project) after an initial run caught exactly that race.

## Phase 5 + Phase 6 (part 1)

### Phase 6 (part 1): rtnetlink primitives

#### Added
- `netplane/`: `rtnetlink.h`/`rtnetlink.c` — hand-built netlink messages (no `ip`/iproute2, no OVS, no eBPF) for bridge creation, veth pair creation, moving a link into another process's netns, bridge attachment, IPv4 addressing, default routes, and link deletion. See ADR-0011 for the "custom control plane over the kernel's own bridge, not a userspace switch" decision, and the bridge-vs-port addressing pitfall found while building this.
- `test/test_rtnetlink.c`: end-to-end verification including real TCP connectivity through a constructed bridge+veth+netns topology, not just successful syscalls.
- `docs/adr/0011-rtnetlink-control-plane-over-kernel-bridge.md`.

#### Changed
- `docs/ROADMAP.md`: Phase 6 marked in progress (part 1 done); part 2 (wiring this into real containers, the REST API, the CLI, and the dashboard) explicitly scoped as separate, not-yet-started follow-up work, per Zen.

#### Verification note
- Before writing `rtnetlink.c`, per ADR-0008's lesson, confirmed via a throwaway `sizeof`/`offsetof` check that TCC lays out every kernel-ABI struct this module touches (`nlmsghdr`, `ifinfomsg`, `ifaddrmsg`, `rtmsg`, `rtattr`, `nlmsgerr`) identically to GCC, and that the relevant `<linux/*.h>` headers don't conflict with glibc's own networking headers — verified, not assumed.

### Phase 5: Web dashboard, pure REST API client

#### Added
- `web/`: the dashboard itself (`index.html`, `app.js`, `style.css`) — vanilla HTML/CSS/JS, no framework, no build step (ADR-0010). Health indicator, auto-refreshing container table, create-container form, remove button per row — mirrors `kanxeoctl`'s exact command surface.
- `daemon/src/staticfile.c` + `include/staticfile.h`: `static_serve()` — path-traversal-safe static file serving, content-type by extension. `kanxeod` gains a `--web-root` flag (default `web/`) and serves the dashboard same-origin as the API.
- `daemon/include/http.h`'s `http_set_blocking()`, promoted from `main.c`'s private `make_blocking()` so `staticfile.c` and `main.c`'s `respond_json()` share one implementation.
- `client/include/httpclient.h`'s `struct kx_response` gained `content_type`/`body`/`body_len` — needed to verify non-JSON (static file) responses.
- `test/test_web.c`: end-to-end verification of static serving (status/content-type per asset, 404, path-traversal rejection, no regression in `/v1/...` routing).
- `docs/adr/0010-vanilla-web-dashboard-no-build-step.md`.

#### Changed
- `docs/ROADMAP.md`: Phase 5 marked done.

#### Scope boundary (stated, not silently skipped)
- Whether the dashboard renders and behaves correctly in a real browser wasn't automated-tested — no headless-browser/Node toolchain exists in this project, and adding one for one small dashboard would repeat the exact dependency-cost trade-off ADR-0010 decided against. Checked instead: `node --check` (pre-installed system tool, not a new project dependency) on `app.js`, and every DOM ID it references confirmed present in `index.html`. A real browser check at `http://127.0.0.1:7620/` is the remaining step.

## Phase 4: CLI, pure REST API client

### Added
- `client/`: `kx_client_request()` — a reusable HTTP client library for talking to the Kanxeo API, extracted from `test_daemon.c`'s ad hoc socket code so `kanxeoctl` and the test suite share one implementation instead of two. Its response buffer is dynamically-growing (no fixed size cap), unlike the test code it replaced.
- `cli/`: `kanxeoctl` — the CLI, one subcommand per endpoint (`health`, `ps`, `run`, `inspect`, `rm`), zero direct runtime access. `--json` for raw output; formatted text by default. Exit codes: `0`/`1`/`2` (success/API-or-transport-failure/usage-error).
- `test/test_image_fixture.c`: shared test-image staging, factored out of `test_daemon.c` so `test_cli.c` doesn't duplicate it.
- `test/test_cli.c`: end-to-end verification driving the real `kanxeoctl` binary as a subprocess.
- `docs/adr/0009-cloexec-daemon-fds-before-clone3.md`.

### Changed
- `test/test_daemon.c`: refactored to use `client/src/httpclient.c` and `test/test_image_fixture.c` instead of its own duplicated logic — same 9 assertions, no behavior change.
- `docs/ROADMAP.md`: Phase 4 marked done; locked-in decisions gained the CLOEXEC rule.

### Fixed
- **Every `POST /v1/containers` request silently stalled for as long as the created container ran** (discovered as `test_daemon.c` taking ~30s instead of milliseconds, under the same repeated-run stress-testing discipline that caught the Phase 3 `epoll_event` bug). Root cause: none of the daemon's own fds (listening socket, accepted client sockets, `epoll` fd, cgroup `O_PATH` fd) were `CLOEXEC`, so every `clone3()`'d container inherited a duplicate of the triggering client connection, and the kernel withholds EOF on a TCP connection until every reference to it — across every process — closes. Fixed by creating every one of those fds `CLOEXEC` from the start (`SOCK_CLOEXEC`/`EPOLL_CLOEXEC`/`O_CLOEXEC`). See ADR-0009. Verified by timing: ~30s → ~0.3s per `test_daemon.c` run.

## Phase 3: REST API spec + daemon

### Added
- `docs/api/openapi.yaml`: the versioned REST API contract (health check, container CRUD), written before any handler code — see ADR-0006.
- `daemon/`: the REST daemon (`kanxeod`) — a hand-rolled, single-threaded, `epoll`-driven HTTP/1.1 + JSON reactor with an in-memory container registry, implementing that contract. See ADR-0005 and ADR-0007.
- `struct kx_epoll_event` / `kx_epoll_ctl()` / `kx_epoll_wait()` in `include/linux_compat.h`.
- `test/test_daemon.c` + `test/daemon_child.c`: end-to-end verification driving the daemon over real HTTP.
- `docs/adr/`: Architecture Decision Records (0000–0008), covering every significant decision made so far, not just this phase's.
- `docs/api/README.md`: human-oriented quick reference alongside the authoritative OpenAPI spec.
- This file.

### Changed
- `CLAUDE.md`: added the API-First Mandate (the daemon is the only process with direct runtime access; CLI/web are pure API clients) and a Documentation Map.
- `docs/ROADMAP.md`: Phase 3 re-scoped from "minimal container CLI" to "REST API spec + daemon," and Phases 4/5 split into CLI and web dashboard, both now depending on Phase 3 instead of the runtime library directly.
- `include/linux_compat.h`: all epoll usage across the daemon now goes through `kx_epoll_event`, never the system `struct epoll_event`.

### Fixed
- **`epoll_ctl()`/`epoll_wait()` silently corrupting their `data` field under TCC**, causing an intermittent (~50–60% of runs) segfault on the very first request. Root cause: TCC ignores `__attribute__((packed))` entirely, so the kernel's 12-byte `struct epoll_event` ABI was being compiled as 16 bytes. See ADR-0008.

## Phase 2 — OverlayFS root construction

### Added
- `docs/MISSION.md` (verbatim charter), `docs/ROADMAP.md` (phase-by-phase status), `CLAUDE.md` (auto-loaded project instructions).
- `struct overlay_spec` + `overlay_create()` (`include/container.h` / `src/overlay.c`): real OverlayFS layering (lowerdir/upperdir/workdir), replacing Phase 1's bind-mount-of-live-host-root. See ADR-0004.
- `mountns_make_private()`, split out of `mountns_pivot()` so mount-propagation is privatized before the overlay mount, not after.
- `test/test_overlay.c` + `test/overlay_child.c`.
- `README.md`.

### Changed
- `struct mount_spec`: `root_source` removed (folded into `overlay_spec.merged`) to eliminate a duplicate-state footgun.
- `mountns_pivot()` no longer populates the root itself; it only requires the root already be a mount point.
- `test/test_harness.c` updated to route through `overlay_create()` (`lowerdir="/"`, preserving its original full-host-visible behavior) and to read its result file from upperdir's copy-up path instead of the bare host path.

## Phase 0 + Phase 1 — Toolchain smoke test, namespace + cgroup v2 container harness

### Added
- `test/test_toolchain.c`: confirms TCC compiles and dynamically links against host glibc, and every header family later phases need parses clean. See ADR-0001.
- `include/container.h`, `include/linux_compat.h`, `include/internal.h`, `src/cgroup.c`, `src/ns_create.c`, `src/mountns.c`, `src/container.c`: the container runtime library — `clone3`-based namespace creation with atomic cgroup v2 placement, mount-namespace pivot, pidfd-based reaping. See ADR-0002, ADR-0003.
- `test/test_harness.c` + `test/harness_child.c`.
- `Makefile`, `.gitignore`, initial repository scaffold.

### Notes
- Requires a **privileged** LXC container/host to run — an unprivileged nested LXC blocks the final `mount("proc", ...)` with `EPERM`/"VFS: Mount too revealing" regardless of mount flags or namespace combination tried (confirmed by elimination during this phase).
