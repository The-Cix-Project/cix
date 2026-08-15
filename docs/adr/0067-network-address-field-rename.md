# 0067 — Rename a network's `gateway`/`has_gateway` field to `address`/`has_address`

## Status

Accepted

## Context

Raised directly by the user after reinstalling `192.168.15.95` with ADR-0065's ISO: `GET /v1/networks` showed the freshly-bootstrapped `management` network as `subnet=192.168.15.0/24 gateway=192.168.15.95` — the box's *own* address, reported under a field named `gateway`. The user called this "terrible and untrue": nothing routes *through* `192.168.15.95` the way "gateway" implies, and a genuinely different, real gateway (the upstream router, `192.168.15.254`) already exists in this system with no field of its own at all, visible only via ADR-0066's `GET /v1/system/routes`.

Worked through as a real discussion, not patched cosmetically: is a network's `gateway` field actually always a host-owned address, or was Part 0.5 (ADR-0058) overloading a field meant for something else? Verified directly against the code, not assumed: `network_create()` and `network_attach_interface()` (`daemon/src/network.c`) call `rtnl_open()`/`rtnl_addr_add_ipv4()` from the daemon's own process context, with no `CLONE_NEWNET` anywhere in that path. Every network's optional address, for every network regardless of role, is added to the bridge as a real address the *host* owns — there is no code path where it means anything else. Part 0.5's reuse of this field as thincd's own bind address was therefore never an overload; it exploited a property already true of every network. The user's own hypothesis was correct.

The actual problem is narrower than "the field is wrong": `gateway` is an accurate name for an *ordinary* network (containers really do route their default traffic through it, via `container_net_child_configure()`'s conditional default-route install). It is not accurate for `management` — nothing routes through it, it is simply where the daemon happens to live. Since `is_management` can be set on any network (see the routing-isolation discussion below), a name that's only sometimes true is a genuine defect, not a display nit.

A second, related question the user raised in the same discussion: does grouping routes/networking more tightly under a "Networks" UI concept risk needing real Linux policy routing to keep multicast/routing-protocol traffic (OSPF, VRRP) from conflicting across networks? Confirmed directly, not assumed: every container already gets its own `CLONE_NEWNET` (`daemon/src/main.c`, `src/container_net.c`'s veth-move helpers) — a fully separate kernel routing table, ARP table, and multicast state per container. This was already proven live in the Phase 24 BIRD/keepalived dual-router test (`cr-1`/`cr-2` running OSPF and VRRP side by side) with zero table conflicts. The host's own single root-netns table (what `GET /v1/system/routes` shows) only ever holds bridge on-link routes plus the upstream default route — it is never shared with container-side routing decisions. Building real policy routing on top would duplicate isolation already provided, better, by namespaces — a "No Parallel Implementations" violation avoided by not pursuing it.

## Decision

**Rename the field outright: `gateway`/`has_gateway` → `address`/`has_address`, everywhere it means "a network's own host-owned address"** — `struct network_def`, `struct network_spec`, the `POST /v1/networks` JSON body, the `Network`/`NetworkCreateRequest` OpenAPI schemas, the CLI's `--gateway=` flag, and every web dashboard field/label reading or writing it. `address` is technically correct for every network regardless of role — an ordinary network's containers still route through it exactly as before; only the name changes, not the behavior.

**No backward-compatibility shim.** Per this project's standing "clean cut-over" instruction, the JSON persisted-state key names change outright (`"has_gateway"`/`"gateway"` → `"has_address"`/`"address"`) with no dual-read migration between the two key names. The one migration this project already had — an entry with *no* address-related key at all is a genuinely pre-ADR-0037 legacy entry, defaulted to `has_address=1`/`address=<subnet base>|1` — is preserved unchanged in meaning (current code always writes one of the new keys, so its absence is still a reliable signal), but an entry still using ADR-0037's original `"has_gateway"`/`"gateway"` key names (written between ADR-0037 and this ADR) is *not* specially migrated, the same posture the earlier `mgmt`→`management` rename already established.

**What does *not* rename**, confirmed as three genuinely distinct concepts sharing a name coincidentally, not a naming defect:

- `net.conf`'s/GRUB's `--gateway=` and `IsoBuildRequest.gateway` — the box's own *upstream* default route (the home/ISP router), never a network's own field.
- `struct route_spec.gateway_be` (a container's own static route's next-hop, `--route=DEST/PREFIX:VIA` on `POST /containers`) and the `RouteSpec.via` OpenAPI field it maps to.
- `KernelRoute.gateway`/`RTA_GATEWAY` (ADR-0066's kernel route dump) and the CLI's `fmt_route_line()` — the kernel's own next-hop for a real routing-table entry.

**The routing-isolation question is resolved by evidence, not new infrastructure**: no policy-routing work is done. Networks (and the routes UI reorg that follows this rename) rely entirely on the container-netns isolation already proven in Phase 24.

## Consequences

- A network's persisted JSON, REST responses, CLI output, and the web dashboard all describe the same field the same way for the first time — a network created purely as an L2 attachment point for a router pair no longer implies a "gateway" that was never true of it.
- `is_management`'s own contract is unaffected by this rename — it remains a plain boolean settable only via `bootstrap_management_network()` or `PUT /system/daemon-config`'s `management_network` field, never at network-creation time; this ADR only renames the address field it happens to read.
- The rename touches every layer (daemon, CLI, web, docs, tests) in one pass rather than staging it — consistent with "One Source of Truth": a period where some layers said `gateway` and others said `address` would itself be a drift the next reader has to reconcile.
- Sets up the two follow-on parts of the same work item: route CRUD (`rtnl_route_del_ipv4()` + `POST`/`DELETE /v1/system/routes`) and moving the routes view under Networks in the web dashboard, both scoped separately since they add new capability rather than rename an existing one.
