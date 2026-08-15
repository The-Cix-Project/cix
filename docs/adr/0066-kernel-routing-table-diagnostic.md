# 0066 — A real kernel routing-table diagnostic: closing the last blind spot in "the daemon is the only way to inspect a running box"

## Status

Accepted

## Context

Raised directly while diagnosing a real, still-open connectivity bug on a genuinely separate installed box (`192.168.15.95`): `pkg install --name=git` failed fetching from `kernel.org`, both by hostname and by a raw IP address (ruling out DNS specifically), while LAN-local plain-HTTP transfers to/from the box worked fine. The user confirmed a correct, real upstream gateway (`192.168.15.254`) was given at the GRUB `--gateway=` prompt, independently confirmed reachable from this dev sandbox on the same LAN. Tracing `bootstrap_management_network()` showed the box could not have panicked from a failed `rtnl_route_add_default_ipv4()` call (a failure there returns 1 from `main()`, fatal for a real-PID-1 boot) — so the route add itself must have succeeded, and the actual failure mode was, and remains, unconfirmed.

This project has no SSH and no general host shell (ADR-0034) — `thincd` is the *only* way to ever inspect a running box. Every prior diagnostic gap this project has hit (ADR-0055's file-read endpoint, ADR-0054's stats endpoint, ADR-0065's pkg-bootstrap URL-fetch) got closed the same way: build the real, API-driven capability rather than guess. There was no way to see the box's own kernel routing table at all — not even to confirm the one route this whole investigation depends on.

## Decision

**A read-only `GET /v1/system/routes`, backed by a real `RTM_GETROUTE` dump — the first multi-message rtnetlink consumer in this codebase.** Every existing `rtnetlink.c` function (`rtnl_route_add_default_ipv4()` and everything else) sends one request and gets back a single `NLMSG_ERROR` ack, via the shared `nl_msg_send_and_ack()` helper. A dump is a genuinely different shape: the kernel replies with a *sequence* of `RTM_NEWROUTE` messages, potentially spanning more than one `recv()`, terminated by `NLMSG_DONE` — `nl_msg_send_and_ack()` can't be reused for the response side. `rtnl_route_dump_ipv4()` reuses the existing request-builder helpers (`nl_msg_init()`/`nl_msg_put()`) for the request, then walks `NLMSG_OK()`/`NLMSG_NEXT()` over the response itself in a new loop, decoding each route's `RTA_DST`/`RTA_GATEWAY`/`RTA_OIF`/`RTA_PROTOCOL` attributes into a caller-provided fixed array (`struct kernel_route out[]`, capped at a caller-given `max` — the same fixed-buffer, no-dynamic-allocation convention `device.c`'s `DEVICE_ENUM_MAX` already established, not a new pattern).

**Raw values, not reinterpreted** — `protocol`/`scope` are the kernel's own `rtm_protocol`/`rtm_scope` integers, `gateway`/`interface` are `null` when the kernel didn't report them (an on-link route has no gateway), matching this project's own established "pass through raw cgroup/kernel values, don't invent a translation layer" convention (`cpu_max`, `cpuset_cpus`, `disk_quota_bytes` all already do this).

**Home for the JSON serialization is `network.c`, not a new file** — it already includes `rtnetlink.h` and already owns `network_write_json_one()`/`network_write_json_list()`, the same shape a routes list needs (`route_write_json_one()`/`network_write_routes_json()`). `main.c`'s `handle_route_list()` is a thin wrapper, modeled directly on `handle_device_list()`.

**CLI**: a bare top-level `thincctl routes` (not `system routes`) — matching this project's own existing convention that every `/system/*` capability (`health`, `update`, `backup`, `site`, `daemon-config`, `iso`) is a bare top-level verb, not nested under a `system` subcommand.

## Consequences

- Verified real, end to end: run against a manually-started `thincd` on this dev sandbox, cross-checked against this sandbox's own real, independently-known-correct `ip route show default` output (`default via 192.168.15.254 dev eth0`) — the daemon's own dump reported the identical route, confirming the parser is correct before ever trusting it against a remote box. The full route table (18 entries — the default route, two host routes, and every network's own on-link connected route across `eth0` plus three thinC-managed bridges) was cross-checked entry-by-entry between the raw JSON and the CLI's formatted output.
- **Does not fix anything by itself** — this is a read-only diagnostic. The actual `192.168.15.95` connectivity bug this ADR was raised to investigate remains open at the time of writing; the next step is deploying this capability to that box (via a fresh ISO, since it can't receive an in-place daemon update — the same limitation ADR-0065 already documented) and reading its real routing table for the first time, rather than continuing to theorize about ARP/STP/link-state without any way to check.
- No write path (`RTM_NEWROUTE` for arbitrary routes, route deletion, etc.) is added — this ADR is scoped to visibility only. A real host-level route-management API, if ever needed, is a separate, later decision.
