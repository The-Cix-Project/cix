# 0203 — DNS forwarders are daemon-owned config, not container argv

## Status

Accepted. Supersedes the forwarder convention established alongside [ADR-0076](0076-host-dns-resolver-config.md) (that ADR's own subject, `GET/PUT /v1/system/resolv`, is unaffected and remains current).

## Context

Issue #134. Until now, the upstream resolvers a Cix DNS server forwarded through were `--server=` flags baked into the container's own command line at creation time:

```json
"cmd": ["/usr/sbin/dnsmasq", "-k", "-R", "-h", "--server=1.1.1.1", "--server=8.8.8.8"]
```

This was never a considered decision. It was the shape the first working `dnsmasq.recipe` happened to have, and it became a convention by repetition. Bringing up `dns-1`/`dns-2` on a real host is what exposed what it actually costs, three ways:

1. **Changing a forwarder means recreating core infrastructure.** A container's `cmd` is fixed at creation. Pointing DNS at a different upstream is therefore a delete-and-recreate of the very service every other container resolves through — a disproportionate and risky operation for what should be a config edit.
2. **Replicas can silently disagree.** `dns-1` and `dns-2` exist to be redundant, but nothing tied their two argv lists together. Two servers answering the same names through different upstreams is a real and very hard-to-see failure: both are up, both are healthy, and answers differ depending on which one a client happened to reach.
3. **Nothing could answer the question at all.** "What does this platform resolve through?" had no endpoint. The value existed only inside a running process's command line — not in any config file, not in any API response, not in the daemon's own state. On a platform whose charter is that an installed host has no shell, that made it effectively unreadable.

The third point is the one that matters most, and it is the same lesson as [ADR-0202](0202-the-esp-is-reachable-over-rest.md): the expensive part was not that the setting was awkward to change, it was that it was **invisible**. Configuration that lives only in an argv is configuration the control plane does not actually control.

## Decision

Forwarders become ordinary daemon-owned configuration, exposed as `GET/PUT /v1/dns/forwarders` and persisted in the daemon's own state directory like every other piece of host config.

The daemon writes the list to a file each dnsmasq reads via `--servers-file=`, then `SIGHUP`s every registered server. Three consequences follow directly from that mechanism, and they are the point of it:

- **One `PUT` updates every replica**, so `dns-1` and `dns-2` cannot drift apart — the disagreement failure mode is structurally removed, not merely discouraged.
- **A newly registered server picks up the current list at registration time**, without the caller having to know or restate it. (This one was claimed by this ADR before it was true: the first implementation synced only on `PUT` and at startup, so a server registered in between came up authoritative-only while `GET /v1/dns/forwarders` reported the configured list — found on a real host, fixed in `dns_server_register()`, and now covered by a regression test that was itself verified to fail without the fix.)
- **No container is recreated to change a forwarder.** dnsmasq re-reads the file on `SIGHUP`; the process keeps running.

The `PUT` replaces the whole list rather than appending — a forwarder list is a set of peers, and additive semantics would leave no way to remove one. An empty array is legal and means authoritative-only, with no upstream recursion.

This deliberately mirrors `/system/resolv` (ADR-0076) in shape while staying a separate concern: `/system/resolv` is what *this host* resolves through, `/dns/forwarders` is what *the DNS servers* resolve through. The common deployment points the first at the DNS containers and the second at real upstreams.

## Consequences

`dnsmasq.recipe` and the `dns-1`/`dns-2` container recipes drop their `--server=` flags and gain `--servers-file=`. Existing DNS containers built the old way keep working but ignore `/dns/forwarders` until recreated — their baked-in flags are still in their argv, which is precisely the coupling this ADR removes and the reason the recipes changed in the same commit.

The forwarder list is now a real dependency of DNS resolution being correct, and it is empty by default. A DNS server registered with no forwarders configured is authoritative-only — correct, and quiet about it. That is the honest behaviour (the alternative, a hardcoded public default, would be this project silently sending a user's queries to a third party it never chose), but it does mean "DNS is up and resolves internal names but not external ones" is now a state an operator can reach by omission. `docs/api/README.md`'s DNS walkthrough covers it directly for that reason.
