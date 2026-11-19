# 0384 — A service declares how it reloads, and cix-init delivers it

## Status

Accepted by the owner on 2026-11-19 (*"go for it"*), for #697: fix the reload that never arrives, with cix-init delivering it. The shape below, a declared `reload_signal`, is this ADR's.

## Context

cixd rewrites files inside running containers: a DNS server's records and forwarders, a DHCP server's reservations. It then sends SIGHUP to the container's pidfd so the server re-reads them (`dns_server_sync_all()`, `dns_forwarders_sync_all()`, `dhcp_sync_all()`).

Since [ADR-0260](0260-a-container-declares-services-not-a-command.md), that pid is cix-init, pid 1 of the container. It installs handlers for SIGCHLD and SIGTERM only (`init/src/cix_init.c`, `install_signal()`). A signal to a namespace's init with no handler is discarded, so the SIGHUP has reached nothing since then.

Measured on 192.168.15.95, 2026-11-19:
- a new DNS record was in dns-1's `/etc/dnsmasq-hosts`, and both servers answered NXDOMAIN for it, while a record written before their last start resolved;
- a DHCP range change restarted dns-1 and dns-2. dnsmasq started on the deployment's empty forwarders placeholder; cixd wrote the real file and sent the SIGHUP; both servers answered REFUSED to every query until their service was restarted by hand.

#697's first part fixed the second case without this ADR: the DNS files are now staged at create, as ADR-0197 already did for the DHCP files, and every sync writes and signals only on a difference. What remains is a change made while a server runs: a record, a forwarder, a reservation.

## Decision

**A service declares the signal that makes it reload. When cixd has changed a container's files, it asks that container's cix-init, which sends each running, ready service its declared signal. A service that declares none is never signalled.**

1. **`reload_signal` in the service declaration**, beside `stop_signal`, from the same set, with no default. dnsmasq's services declare `SIGHUP`. The knowledge of how a program reloads belongs to the declaration of that program, as how it stops already does; no registration names a service.
2. **A new command, `CIXINIT_OP_RELOAD`**, over the control socket cixd already has (the channel ADR-0260 uses for start, stop and restart, chosen for the same reason: a message is read whenever cix-init reaches its loop, a signal to pid 1 is not). It names no service: cix-init signals every service that declared a `reload_signal` and is ready.
3. **Only a ready service is signalled.** A program that has not installed its handler yet dies of SIGHUP (dnsmasq is pid 2, not a namespace init, so the default action applies). A service that is starting gets no signal; it reads the files when it starts, and they are already written.
4. **cixd knows which cix-init each container runs.** `CIXINIT_VERSION` becomes 4, and cix-init's `EV_UP` already reports its version. cixd records it on the registry entry, and the handover record carries it as an optional per-container field (`init_version`), so an in-place switch keeps it.
5. **An older cix-init gets a restart instead.** For a container whose cix-init is older than 4, or unknown, cixd restarts each service that declares a `reload_signal` (`CIXINIT_OP_RESTART`, which every cix-init has). That is heavier, and correct; it ends as each container is next recreated.
6. **One entry point.** The three sync paths call one daemon function, `container_reload()`, never a signal to a pidfd.

## Alternatives

- **cix-init forwards SIGHUP to its services.** It would send every service a signal some of them die of, and a signal sent before cix-init installs the handler is discarded too (the startup window the CLAUDE.md entry on namespace inits records). Rejected.
- **Restart the service on every change.** It works with every cix-init, but drops dnsmasq's cache and both servers briefly at every record change. Kept only as the fallback.
- **The registration names the service.** It puts a second copy of a fact about the program in another subsystem's state. Rejected for the declaration.

## Consequences

- dns-1's and dns-2's deployments add `"reload_signal": "SIGHUP"` to their dnsmasq service, and are re-applied once (one at a time) so their cix-init is the new one.
- Until a container declares `reload_signal`, a change to its files takes effect at its next start, which staging now makes correct.

## Live checks

- A DNS record added with both servers running resolves at once, from both.
- A reservation added is served without a restart.
- A container restarted by a range change answers NOERROR as soon as it is ready.
