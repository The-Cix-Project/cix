# 0156 — live network attach/detach on a running container

## Status

Accepted

## Context

Task #861 (network updates without a recreate) is the "network" piece of a three-part ask (`cmd`/files/network) — the files piece already shipped as ADR-0153. This ADR covers network; `cmd` is addressed by exclusion below.

**Why `cmd` isn't attempted here, on purpose, not as a deferred gap**: a container's `cmd` is the argv its own init process (PID 1 of a real, `clone3(CLONE_NEWPID | ...)`-created PID namespace) was `execve()`'d with. Changing it means killing that process and starting a new one — but per `pid_namespaces(7)`, the moment a PID namespace's own init dies, the kernel `SIGKILL`s every other process still in it and the namespace itself becomes permanently unusable for new processes. There is no way to "keep the same container, just swap what PID 1 runs" without destroying and recreating the PID namespace — which is architecturally indistinguishable from a real recreate. This isn't a missing feature; it's a real Linux kernel invariant. `cmd` stays a recreate-only field.

**Network is different and genuinely tractable**: a network namespace, unlike a PID namespace, has no such "dies with its own init" rule — it persists for as long as any process is in it or any reference (an open `/proc/<pid>/ns/net` fd) is held. `container_net_host_attach_interfaces()` (ADR-0022, physical interface passthrough) already proves the exact mechanism needed — `setns()` into a running container's own netns from the daemon, do rtnetlink work, leave again — just never previously exposed as its own standalone, post-creation endpoint.

## Decision

**`POST /v1/containers/{name}/networks`** (body: one entry in the identical shape `POST /containers`' own `"networks"` array already uses — a bare name string for an auto-allocated IP, or `{"name":..., "ip":...}` for an operator-chosen one, `parse_network_entry()` reused verbatim) attaches a network to an already-running container:

1. A real veth pair is created on the host (`vh<pid>-a<idx>`/`vc<pid>-a<idx>`, `<idx>` = the container's current `net_count`, collision-free against both create-time and earlier live attachments by construction).
2. The container-side end moves into the target pid's netns; the host-side end is enslaved to the network's bridge and brought up — identical to `container_net_host_setup()`'s own create-time sequence.
3. A short-lived forked helper `setns()`s into the container's netns (mirroring `container_net_host_attach_interfaces()`'s own dance) to rename the interface (`eth<idx>`), assign the address, and bring it up — this part can only be done from inside the netns it now lives in.

**`DELETE /v1/containers/{name}/networks/{network}`** is the reverse, and simpler: deleting the *host-side* end of a veth pair removes both ends (a well-established kernel behavior this codebase already relies on implicitly for create-time netns teardown) — no `setns()` needed at all.

**Deliberately live and ephemeral**, the identical posture ADR-0153 already established for `PUT .../files`: neither endpoint ever touches the container's own persisted create-request body. A restart or recreate replays the original definition unchanged, this attachment gone. The durable "survive a recreate" path is editing the container's own definition (a container recipe, ADR-0151) and re-applying it — not this endpoint.

**Detach only ever removes a live attachment, never a create-time one** (`409` otherwise) — a create-time network attachment has no standalone detach path, matching the exact reasoning ADR-0153 already gives for why a create-time `files[]` entry isn't independently removable either: silently diverging a running container from its own persisted definition mid-life is not something any other endpoint in this API does, and this one shouldn't be the first.

`registry_network_attachment` gained `veth_host`/`ifname` fields to support this: `veth_host` is empty for every create-time attachment (they tear down together with the whole container, no standalone removal ever needed) and only ever populated for a live one (needed at detach time, and to distinguish the two cases for the 409 above). `ifname` is now populated for *every* attachment, create-time included (previously computable but never stored) — `GET /containers/{name}` echoes it, and a live attachment's own array position can genuinely differ from its real kernel-side interface name after an earlier detach shifts the array, so deriving it from array index instead of storing it explicitly would have been actively wrong.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. New coverage in `test/test_daemon_net.c`: a container started with **zero** networks (its own `net_child` already listening on a wildcard `0.0.0.0` bind from before any interface exists — proving the live attach itself, not something baked in at creation, is what makes it reachable) gets a network live-attached, a real TCP connection through the new interface succeeds (`connect_and_echo()`, not just an HTTP 200), `GET` reflects it with `"live":true`, it's live-detached, `GET` shows it gone, and it's re-attached again afterward with another real successful connection (proving no veth-naming collision across a full attach/detach/attach cycle on the same container). Error paths: attach to a nonexistent network (404), attach to a nonexistent/not-running container (404), re-attaching an already-attached network (409), detaching a never-attached network (404), and — on a separate, dedicated container — detaching a genuine create-time attachment (409, not silently torn down). Full regression sweep (`test_daemon`, `test_cli`, `test_web`, `test_container_net`, `test_networks`, `test_network_interfaces`, `test_container_lifecycle`, `test_routes`) clean afterward.

A real test-authoring mistake was caught and fixed before it shipped: the first draft of the create-time-detach-rejected check reused an existing container (`n5`) from an earlier scenario in the same test file — but that container's own `net_child` had already consumed its fixed connection quota and self-exited by the time this new scenario ran, so the check was actually exercising the *"not running"* 404 path, not the intended 409. Fixed with a fresh, dedicated container built specifically for that one assertion.

## Consequences

- `cmd` remains a recreate-only field, permanently — not a gap this project intends to close later, a real kernel-level boundary documented here so it isn't rediscovered.
- Task #861 is now closed in full: files (ADR-0153) and network (this ADR) both shipped live/ephemeral; `cmd` is closed by explanation rather than by code.
- Any operator/tool relying on `PUT .../files`'s or this endpoint's changes surviving a restart needs to know to use a container recipe instead — the same discipline ADR-0153 already established, now consistent across both live-update surfaces this project has.
