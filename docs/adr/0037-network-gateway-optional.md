# 0037 — a network's host-owned gateway becomes optional, default flips to gateway-less

## Status

Accepted

## Context

Raised directly by the user, from a real topology they wanted to build: two core router containers (`cr-1`/`cr-2`) on a shared L2 segment, each with their own stable IP, sharing a VRRP address as the segment's actual gateway, running a routing protocol (bird) between them. A third, plain workload container attaches to that same segment with a manually-configured default route pointing at the VRRP address, not at anything the host owns.

That topology was impossible to build. `network create` (`daemon/src/network.c`) always created a bridge **and** always assigned host-part `.1` as a real IP address on the bridge device itself, permanently reserved from allocation. Every container attached to a network unconditionally got a default route installed pointing at that address (`container_net_child_configure()`, `src/container_net.c`) — no opt-out existed. The host was therefore forced to be the L3 gateway for every network it hosted, with no way to make a bridge purely L2 and let containers attached to it (a VRRP pair, or anything else) own routing for that segment instead.

Tracing the history: ADR-0011 modeled the bridge's own gateway address on the simplest possible case — a `docker0`-style network where the host itself is the gateway. Reasonable as a *default*, but it was never revisited once the router use case (ADR-0020 explicit IPs, ADR-0021 per-attachment IP override, ADR-0022 real NIC passthrough) got built out around it — this is the one piece of that use case that stayed hardcoded the whole time.

The user, asked directly, confirmed the deeper judgment: the host's job is compute, storage, hardware/device grants, and switching fabric (a bridge is genuinely just that — zero policy) — not routing policy for a network that has its own router containers. Conflating "L2 broadcast domain" with "L3 gateway owner" in one `network create` call was the actual bug, not merely an oversight. Confirmed by the user's own decision: flip the default outright rather than keep it an opt-in flag — gateway-less is now what an operator gets unless they ask for a host-owned gateway.

## Decision

**`network create` gains an optional `gateway` field/`--gateway=A.B.C.D` flag.** Omitted (the new default): the bridge is created with no host-owned IP address at all — pure L2, containers attach, plain switching, nothing more. When given, the address must be a real, in-subnet address (not the network or broadcast address) and is assigned to the bridge device itself, the same "gateway lives on the bridge, never a port" convention ADR-0011 already established — just operator-chosen now instead of hardcoded to `base|1`. This also directly answers the user's own original question ("why can't I choose my own gateway IP") for the networks that still want a host-owned one.

**`struct network_def` gains `has_gateway`** (`daemon/include/network.h`); `gateway_be` is only meaningful when set. **`struct network_spec` gains the same flag** (`include/container.h`), propagated at container-creation time. `container_net_child_configure()`'s previously-unconditional default-route install (`src/container_net.c`) is now conditional on it — a container on a gateway-less network gets no default route at all, exactly like a real host plugged into a real switch with no DHCP; its image/operator owns routing entirely, e.g. via the pre-existing `--route=0.0.0.0/0:VIA` mechanism (unrelated, unchanged) for the VRRP-address case. `.1` (or wherever an explicit gateway sits) is only reserved from IP allocation (`network_ip_available()`) when the network actually has one — `registry_alloc_ip()` grew an `exclude_be` parameter for this, since the gateway is no longer guaranteed to sit at a fixed host-part.

**Migration (No Regressions), the one place a mistake here would silently break real deployed state**: every network persisted before this shipped has no `has_gateway` key in its JSON at all — `parse_persisted_entry()` treats that specific absence as the reliable signal this is an old-format entry, loading it exactly as today's legacy behavior always worked (`has_gateway=1`, `gateway=base|1`), never silently demoting an already-relied-upon network to gateway-less. Only entries written by the current code (which always includes `has_gateway`, `true` or `false`) can describe a genuinely gateway-less network.

## Consequences

- Verified against a live daemon (`test/test_networks.c`, extended): default `network create` with no `--gateway=` produces a gateway-less network (`has_gateway: false`, `gateway: null`); an explicit `--gateway=` is honored and reserved from allocation; a hand-written old-format persisted entry (no `has_gateway` key) loads as `has_gateway: true` with `.1`, proving the migration path. A container attached to a gateway-less network gets no default route (`test/test_container_net.c`), but an explicit `--route=0.0.0.0/0:VIA` still installs one.
- `NetworkCreateRequest.gateway` and `Network.gateway` (`docs/api/openapi.yaml`) both become optional/nullable — a real, deliberate API contract change, not just an internal one, since a client that assumed `gateway` was always present now needs to handle `null`.
- No NAT/masquerade exists on this platform (unchanged, deliberate, per `daemon/include/pkg.h`'s own existing doc comment) — a gateway-less network's containers reaching anything outside their own L2 segment was already, and remains, entirely the job of whatever router container(s) are attached to it, never the host.
- `network_delete()` now also releases/removes any interfaces attached via `network_attach_interface()` (ADR-0038) before deleting the bridge itself — best-effort, not blocking, since a genuinely broken physical interface shouldn't make an otherwise-unused network permanently un-deletable.
