# 0287 — The management address is the single truth; the network and its `management` flag are derived

## Status

Accepted

Issue [#436](https://git.home.arpa/itdlabs/cix/issues/436) (a persisted bind address never reapplied at boot — subsumed here, because boot-reapply becomes the one boot path of this model) and [#441](https://git.home.arpa/itdlabs/cix/issues/441) (no dashboard surface for the management network — becomes this resource). Supersedes [ADR-0068](0068-dedicated-daemon-bind-ip.md) outright. Amends the `is_management` contract of [ADR-0058](0058-the-hosts-own-management-address-becomes-a-real-api-managed-network.md) and [ADR-0067](0067-network-address-field-rename.md): where those made `is_management` a flag *set by name* and persisted, it is now a field named plainly `management`, *derived from the address* and never stored. The `is_` prefix is dropped in the same cut-over — a derived predicate needs no verb to disambiguate it from a setter, and the field now reads as one of the network's plain properties (`management: true`).

## Context

Raised directly by the owner, as a foundational correction rather than a feature: "why do we have an explicit management network? We don't want one. cixd sits firmly always on 127.0.0.1 and a network of choice, and that network should have a property `is_management`." The frustration was earned — the implementation had drifted into three overlapping ways to say one thing, which is a One-Source-of-Truth violation hiding as configuration surface.

Measured against the code, the daemon's off-box listen address was derivable from **three** persisted inputs, with a **fourth** persisted flag that could disagree with all of them:

1. `management_network` — a network chosen *by name* (`network_set_management()`, `PUT /system/daemon-config`'s `management_network` field). cixd binds that network's own `address` (`network.h:62`, "at most one network has it").
2. `bind_ip` — an *optional dedicated address* within that network's subnet, layered on top as an override (ADR-0068), set via `daemon-config --bind-ip`.
3. `net.conf` — the install-time IP/prefix/interface/gateway, which `bootstrap_management_network()` turns into a reserved `"management"` network on a fresh box.
4. `is_management` — a persisted boolean on the network, settable independently of any of the above.

Two mechanisms already implemented "move the daemon's off-box address": `network_set_address()` (re-address the management network in place — its own docstring says it "exists for `PUT /system/management-network`") *and* `bind_ip` (a separate dedicated address). One truth, two code paths — the exact "No Parallel Implementations" violation the owner sensed. And because `is_management` is stored separately from the address it is supposed to describe, the two can drift: a box can persist a flag on a network whose address the daemon is not actually bound to.

What was already correct and stays: **127.0.0.1 is always bound**, unconditionally, on its own loopback listeners that no reconfiguration path ever touches (`main.c:1435`, asked for by the owner after an address change once left a box bound to nothing) — so the on-box console cixctl and any local client always have a listener that cannot be reconfigured away. This ADR does not weaken that; it is the floor the whole model stands on.

## Decision

**The management address is the single persisted truth for where cixd listens off-box. The network it belongs to, and that network's `management` status (renamed from `is_management`), are *derivations* of the address — computed, never independently stored.**

Concretely:

- **One input: a `management_address` (an IPv4 address).** Persisted in exactly one place (`daemon_config`). There is no `management_network` name and no separate `bind_ip`; both are removed. The name `management_address` is used identically for the CLI command, the REST resource, the JSON key, and the C field (One Source of Truth for the name itself), and matches ADR-0067's deliberate `address` vocabulary.

- **The network is derived, and derivation is unambiguous by construction.** `network_create()` already refuses a subnet that overlaps any existing network (`ranges_overlap()`, `network.c`), so at most one network can contain any address. The management network is *whichever network's subnet contains `management_address`*, found by that containment test alone.

- **`management` is that derivation, not a stored flag** (renamed from `is_management`). `management(net) := net.in_use && net's subnet contains the current management_address`. It is computed on load and on every read; it is never written to persisted network state and never set by name. `network_set_management(name)` as a public "make this network management" path is removed. The field remains as an in-memory, derived property (so `GET /networks` and the allocation floor below keep working) — its *source* changes from stored to computed, and its name loses the `is_` prefix.

- **The address cixd binds is added to the derived network's bridge; the network keeps its own `address`.** Setting `management_address` does **not** re-address the network. cixd guarantees the address is present on the derived network's bridge (idempotent add — `rtnl_addr_add_ipv4()`'s `NLM_F_EXCL` makes re-adding an address already there a no-op) and binds its off-box listeners to it. In the common case where the operator chooses the network's own address, that is a single address on the bridge; when they choose a different in-subnet address, cixd's bind address is an additional one. This is ADR-0068's mechanism, generalized from an *override* into *the* mechanism — there is now no second path. `network_set_address()`, the old in-place re-address call, is **removed**: its only caller was the deleted `PUT /system/management-network` handler, and a network's address is otherwise fixed at creation (the network PUT adjusts only its auto-allocation window, #137). One fewer way to move an address means one fewer way for the truth to drift.

- **An address in no existing network is refused.** `PUT /system/management-address` with an address outside every network's subnet returns `400` ("no network contains A.B.C.D — create one first"). The daemon never auto-creates a network, because "just an address" carries no prefix and inventing one would smuggle back the inputs this ADR removes. A fresh install is therefore not a special case: the installer creates the first network from `net.conf` (an ordinary `POST /networks`, IP/prefix/interface) and then sets `management_address` to `net.conf`'s IP (an ordinary `PUT /system/management-address`). `bootstrap_management_network()` dissolves into those two ordinary operations.

- **`net.conf` is consumed once, then persisted state wins.** On the first boot with no networks, `net.conf` seeds the first network and the management address. Thereafter `daemon_config`'s persisted `management_address` is authoritative and `net.conf` is not re-read. There is one place the address lives after first boot.

- **Reset drops the box to loopback-only, deliberately.** `DELETE /system/management-address` clears the persisted address, removes it from the derived network's bridge *unless it coincides with that network's own `address`* (never delete a network's own address out from under it), stops both off-box listeners, and `management` then derives to *none*. The box remains fully reachable on 127.0.0.1. This "loopback-only by choice" state is distinct from `g_bind_unavailable` ("an address was configured and could not be bound"), which must still refuse to confirm a good A/B boot slot; the two states stay separate.

- **Deleting the network that carries the live management address is refused.** It returns `409` with the owner's wording: "reset the management address first, or move it to another network." This is the existing delete-guard (`NETWORK_ERR_IS_MANAGEMENT`), now keyed on the derived flag rather than a stored one. Deletion is the only network op that can strand the address — there is no in-place re-address call any more (see above), and creation cannot touch an existing network — so this one guard is the whole surface.

### Invariants this model must not lose

These were found empirically and are restated here so a future change cannot re-derive them wrongly:

- **The old address is removed on a short deferred timer, never synchronously.** Deleting the address out from under the very connection carrying the `PUT` leaves the client waiting forever for bytes the kernel accepted and can no longer deliver (ADR-0068, measured). The existing `CONN_BIND_IP_CLEANUP` one-shot timer is reused; the two addresses being briefly live together is what makes the handover safe.
- **Both off-box listeners (HTTP and HTTPS) rebind atomically.** #454 was the HTTPS listener silently not rebinding because the comparison used the shared `g_bind_addr`; a listener's bound address lives on the listener (`listen_addr`). The new rebind uses that per-listener field.
- **The loopback listeners are never touched by any reconfiguration path.** Unchanged from `main.c:1435`.
- **The management network's allocation floor survives** (`network.c:898`, #70) — it keys on the derived `management` flag, which still exists, only its source and name changed.
- **Gateway is not part of this.** ADR-0067 already separated the upstream default route from any network's address; the default route lives in `/system/routes`. `management-network set --gateway` is removed with the rest, not relocated here.

## Consequences

- **Cut in one clean cut-over, no shim** (project standing instruction): the whole `PUT /system/management-network` endpoint; the `management_network` and `bind_ip` fields of `daemon-config`; `network_set_management(name)` as a public path; `MGMT_NETWORK_NAME` as anything but the installer's default network name. Added: one resource, `GET`/`PUT`/`DELETE /system/management-address`, whose representation reports **configured** and **bound** separately (the configured-intent-vs-kernel-state split this codebase already uses elsewhere). CLI: `cixctl management-address show|set|reset`.
- **#436 closes with this**, not separately: reapplying the persisted `management_address` at boot (add to bridge, bind listeners) is the *only* boot path in the new model, so there is no longer a way for it to be persisted-but-not-reapplied.
- **#441's dashboard surface becomes this resource** — the web Daemon/Networks page reads and writes `/system/management-address` like any other REST client (API-First), gaining no capability the endpoint lacks.
- **The `is_management` contract of ADR-0058/0067 is amended, not reversed**: a network still *can* be management, at most one is, and the allocation floor still applies — but "is it management" is now answered by the address it contains, not by a stored flag, so the flag can no longer disagree with reality.
- **A pre-existing box carrying the old persisted keys** (`management_network`, `bind_ip`, a stored `is_management`) is migrated once on load into the single `management_address`, consistent with this project's clean-cut-over posture for persisted-state key renames (ADR-0067's own precedent) — the derived model then owns the truth and the old keys are never written again.
- **Sequencing** (API-First): this ADR, then `docs/api/openapi.yaml` + `docs/api/README.md` together, then the daemon, then the CLI, then the web dashboard. `docs/architecture/architecture.svg` is updated if the box/arrow shape changes. Adjacent but out of scope: #444 (shutdown closes only one of four listeners) — fixed opportunistically if the listener code is open, not scoped in.
