# 0091 — DNS server bindings were never persisted, silently lost on every restart

## Status

Accepted

## Context

Raised directly by the user: after registering `dns-1`/`dns-2` as DNS servers earlier in the same session (`POST /v1/dns/servers`, confirmed working at the time), a later, unrelated daemon restart (deploying ADR-0089/ADR-0090) silently lost both registrations. `GET /v1/dns/servers` came back empty with no error anywhere, and every DNS record created after that point (`ldapsvc`) was never pushed to either server's own hosts file, even though the record itself existed correctly in `GET /v1/dns/records` -- confirmed live: `dig ldapsvc.uk.home.arpa` against `dns-1` timed out (NXDOMAIN-equivalent for a server with no forwarding match) despite the record being real and correct.

Root cause, in `daemon/src/dns.c`: `g_bindings[DNS_SERVER_MAX]`, the array `dns_server_register()`/`dns_server_unregister()` maintain, was purely in-memory -- zeroed at `dns_init()`, never written to or read from disk, unlike `g_records[]` (the DNS record set itself), which already had a full persist/load cycle. `dns_server_sync_all()` (called on every record create/delete) was already correctly designed to push the current record set to every *currently known* binding -- it just had nothing to iterate after a restart, since the bindings themselves never survived one.

## Decision

Give server bindings the same persistence `g_records[]` already had: a second state file (`<data-dir>/dns_servers.json`), saved on every `dns_server_register()`/`dns_server_unregister()`/`dns_server_forget()`, loaded in `dns_init()` (now taking a second path argument). Loading alone isn't sufficient -- at `dns_init()` time (early in daemon startup) no container has actually started yet, so any binding's own container has no live pid/pidfd to write to. `dns_server_sync_all()` is called a second time, explicitly, right after `containerdef_autostart_all()` completes in `main()`, once every persisted container definition (including the DNS servers themselves) has a real, running process again.

## Consequences

- `dns_server_register()`'s own registration now survives a daemon restart or full host reboot, the same way every other persisted resource in this project already does -- this was a real gap, not a deliberate design choice.
- Live-verified across a real reboot: `GET /v1/dns/servers` still lists both bindings after restart, and a DNS record created before the reboot (`ldapsvc`) resolves correctly through both servers afterward with no manual re-registration step.
- `dns_server_sync_all()` itself needed no changes -- it was already correct, just never given anything to act on.
