# 0283 — The management address is changeable on a running box, and net.conf is how it sticks

## Status

Accepted

## Context

Asked directly by the owner, twice, in plain terms: *"I want it so that a machine that's booted (not from the ISO), we can switch the binding of cixd around"* and *"move it to any network and give any ip (within reason of course, and that being the realms of logic so we dont shoot ourselves in the foot)."*

It could not be done. Measured, not assumed: `NET_CONF_PATH` appeared exactly twice in `daemon/src/main.c` — the `#define` and one `parse_net_conf()` call at boot. Nothing in the daemon wrote that file. `cix-install` wrote it once, at install time, and `bootstrap_management_network()` read it once per boot, taking the `ip=` line straight into `g_bind_addr` and the `gateway=` line straight into a default route.

So an installed box's management address was fixed at install time. That mattered more than it sounds, because the installer deliberately offers *not* choosing one: a blank interface, or `lo`, installs a box that comes up answering on loopback with no address committed to. The installer even printed "cixd will answer on 127.0.0.1 until one is set" — naming a setting path that did not exist. A box installed that way had exactly one route to a real address: reinstall.

Two pieces of the mechanism already existed and were load-bearing. `rebind_listener()` (Part 0.5) does a live listen-socket move — new socket created and added to `epoll` before the old is torn down, rolling back on failure — and `PUT /v1/system/daemon-config` already drove it for *port* changes and for repointing which named network is management. And ADR-0068 had already measured the one non-obvious hazard: removing an address from a bridge immediately after replying strands the client, because the connection carrying the reply usually has that very address as its own local endpoint, and the kernel accepts the `write()` without having put the bytes on the wire. ADR-0068's answer was a two-second deferred cleanup on a `timerfd`.

What was missing was not machinery. It was that nothing could change the *address*, and nothing could write the file that decides it at the next boot.

## Decision

**A dedicated resource, `GET`/`PUT /v1/system/management-network`,** carrying the four values a booted box comes up on: interface, address, prefix, gateway. Deliberately not more fields on `daemon-config`, which owns the listener's ports and which *named network* is flagged management. Those are real questions about the daemon; this is a question about the machine's place on the network, and conflating them would have meant a single `PUT` whose failure modes ranged from "that port is in use" to "you have just moved the box to a subnet your containers are not on".

**Apply live, then persist, in that order.** A box that answers after the call is a box that answers after a reboot. The ordering is what makes the failure direction safe: if anything fails after the listeners have moved, `net.conf` is not written, so the box is reachable *now* on the new address and returns to the last known-working configuration on reboot. The inverse ordering — persist first — would make a failed move into a box that boots to an address that never worked.

**No confirm-or-revert handshake, and no pending-move record.** Both were designed and dropped. The owner asked twice for this to be simple, and the safety they buy is largely already present: validation refuses what cannot work before anything moves, a failed move is not persisted, and physical console access is the documented fallback for an installed box (it is the fallback for every other way of making cixd unreachable, including the ones `daemon-config` has always permitted). A two-phase commit would also have been the *less* honest design here, because the primitives underneath persist eagerly — `network_create()` and `network_set_management()` write the registry immediately — so "nothing is persisted until you confirm" would have been true of `net.conf` and false of everything else.

**`net.conf` stays the boot source of truth and gains a second writer, so the format moves into one module.** `daemon/src/netconf.c` holds `netconf_parse()` and `netconf_write()`; `cix-install` and `cixd` both link it. Before this, the format was defined twice — a `snprintf()` in the installer and a static parser in the daemon — which was survivable only while there was exactly one writer. Adding a second writer without unifying the format would have put two independent definitions of one file in two programs, which is the One Source of Truth failure the maxims forbid. The write is atomic (temp file, `fsync`, `rename`) because the entire purpose of the file is to be read on the next boot, and a half-written one is a box that does not come back.

**Re-address the existing network rather than create a replacement.** `network_set_address()` is the one genuinely new primitive: new subnet, new prefix, new address, same name and same bridge. Creating a new network per move was the alternative and was rejected — the management network is referenced by name in persisted state and by the `is_management` flag, and a box that changed address three times would accumulate three networks, only one of them real. It deliberately leaves the *old* address on the bridge and hands it back to the caller, who defers its removal onto ADR-0068's existing `CONN_BIND_IP_CLEANUP` timer. Both addresses being briefly live is what makes the handover safe, not a leak to be tidied.

**The guardrails are validation, and they run before anything moves.** A prefix outside 8–30; a malformed address; the subnet or broadcast address itself; an interface `if_nametoindex()` does not know; a gateway outside the new subnet or equal to the new address. Two are worth their own reasoning:

- **`0.0.0.0/8` and `127.0.0.0/8` are refused.** A loopback address is a perfectly legitimate thing for cixd to be *bound* to — it is what a blank-interface install comes up on — but it is not a management *network*: loopback cannot be enslaved to a bridge, and there is nothing to route. Accepting it here would have meant an endpoint that returns 200 and leaves a network that cannot work.
- **A subnet change is refused while containers are attached to the management network**, and the refusal names them. Their addresses would stop belonging to the network they are attached to. A move *within* the same subnet strands nothing and is never blocked on that ground.

**On a box with no management network, this creates one.** That is the primary path, not an edge case: it is the other half of the installer's offer to defer the decision, and it is what finally makes "cixd will answer on 127.0.0.1 until one is set" a true sentence.

**Any `bind_ip` is cleared by a move**, the same rule ADR-0068 already applies when `management_network` is repointed — a dedicated bind address is validated against, and lives on, one specific subnet, which this call may have just changed.

## Consequences

An operator can install a box with no network at all, boot it, and put it wherever it belongs — which is what the installer's blank-interface option was always promising and could not deliver. Moving a box between subnets no longer means reinstalling it.

The lockout risk is real and is not newly introduced: `daemon-config` could already point cixd at an unreachable address. This endpoint refuses more of the ways to do it than that one does, and the console fallback is unchanged. What it does add is a way to *fix* such a box from the console rather than only from an installer.

`net.conf` now has two writers. That is a genuine cost, paid down by both going through `netconf.c`; the file's shape can no longer drift between the program that creates it and the program that maintains it, which it could have before and which nothing would have caught.

`network_set_address()` is general — it takes a network name — so a future `PUT /v1/networks/{name}` re-addressing any network has its primitive already, rather than growing a second one beside it.
