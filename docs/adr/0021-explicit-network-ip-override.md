# 0021 — a container's network attachment IP can be explicitly chosen, not only auto-allocated

## Status

Accepted

## Context

Confirmed alongside the per-image `pkg install` gap (ADR-0020) in the same conversation, from the user's own router use case: a router's own interfaces typically need stable, operator-chosen addresses (a `lan1` bridge's gateway-facing side at a specific, predictable IP), not whatever `network_alloc_ip()`'s first-free-address scan happens to pick. Every container network attachment had exactly one path to an IP since Phase 6 part 3 — `network_alloc_ip()` — with no way to request a specific one.

## Decision

`daemon/src/main.c`'s `handle_create()` already parsed `"networks"` as an array of bare strings only; each array entry may now *also* be an object, `{"name": "...", "ip": "..."}`. A new `parse_network_entry()` helper recognizes both shapes for one array item, used identically by the existing two-pass structure (a validation pass, then the pass that actually builds `net_attachments[]` — unchanged from before this decision, just now IP-aware).

The actual availability check is split by ownership, matching `registry_alloc_ip()`'s own existing topology-agnostic design: `registry_ip_available(candidate_be)` (`daemon/src/registry.c`) is extracted from `registry_alloc_ip()`'s own collision-scan loop as a pure "is this exact address already assigned to a running container" check, with zero notion of subnets, ranges, or gateways — registry.c stays deliberately ignorant of network topology, as its own existing doc comment already states. `network_ip_available()` (`daemon/src/network.c`), which *does* own that topology knowledge (`mask_for_prefix()`, `host_max_for_prefix()`, `net->gateway_be`), does the subnet-membership, range, and gateway-exclusion checks first, then delegates the final collision check to `registry_ip_available()`.

REST: no new field name collision to resolve — `networks[]` already existed as the array field, `oneOf: [string, NetworkAttachmentRequest]` per entry (`docs/api/openapi.yaml`) is a natural, additive extension. CLI: `--network=NAME:IP`, parsed the same way `--route=DEST/PREFIX:VIA` already is (`parse_route_flag()`, direct template for the new `parse_network_flag()`) — split on the first `:`, since neither a network name nor an IPv4 address can itself contain one.

## Consequences

- Verified over real HTTP against a live daemon (`test/test_networks.c`, extended): an explicit valid IP is honored (not auto-allocated); the reserved gateway address, an out-of-subnet address, and an already-assigned address are each correctly rejected (400/400/409 respectively) — 3 consecutive clean runs. Full regression sweep re-run clean.
- Within a single `POST /v1/containers` request, two network attachments requesting the same explicit IP (or an explicit request colliding with what a later bare-name entry would have auto-allocated) are not cross-checked against each other — only against already-registered containers. This mirrors a pre-existing boundary `network_alloc_ip()` itself already had (two auto-allocated attachments in the same request aren't cross-checked either); not a new gap introduced here, and attaching to the same network twice in one request is not a realistic use case.
- `respond_network_error()` moved earlier in `daemon/src/main.c` (right after `respond_error()`) so `handle_create()`'s new validation pass can call it — a pure reordering, no behavior change for its two existing callers (`handle_network_create`/`handle_network_delete`).
