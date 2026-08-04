# 0053 — Auto-maintained instance DNS record

## Status

Accepted

## Context

Direct follow-up in the same identity-focused conversation as ADR-0049 through ADR-0052: "wouldn't it make sense to show a dns entry for the hostname... make it editable and it reflects back to the system dns entry?" Confirmed with the user via `AskUserQuestion`: the record should be auto-maintained (option b, "editable, and it reflects back"), not a second, independently-editable record for the same concept -- exactly the same "reflects back" relationship `reissue_host_pki_cert()` (ADR-0050) already has with the PKI leaf, applied here to a DNS record instead.

Two real design questions, settled before writing code:

1. **What IP does the record point at?** The daemon's own `--bind=` address is the only value it has any authority to publish. But `--bind=0.0.0.0` (listen on every interface, the box's current live configuration on this project's own production daemon) and the default `127.0.0.1` (loopback-only) both have no single correct address to hand out -- publishing either would be actively wrong, not just imprecise. Both are treated as a deliberate no-op rather than a guess.
2. **How does a rename get cleaned up?** Since `dns_record_create()`'s own record is keyed by name, and this record's name *is* the composed FQDN, a rename (any of `instance_name`/`site_name`/`domain_suffix` changing) needs the *old* name deleted, not just a new one created alongside it.

## Decision

**New `reconcile_instance_dns_record()`** (`daemon/src/main.c`): composes the current FQDN via `siteconfig_host_fqdn()` (already built for ADR-0050's host cert), skips entirely if `g_bind_addr` is `"0.0.0.0"` or `"127.0.0.1"` (no address to publish) or isn't a valid IPv4 literal, otherwise deletes whatever this function itself last registered (tracked in a new in-process-only `g_last_instance_dns_name`, not persisted) if it differs from the current FQDN, then creates the record fresh under the current name.

Called from two points: once at daemon startup, right after `siteconfig_init()` (so the record exists immediately for an already-configured site + a real `--bind=` address, without needing an operator to re-PUT anything after every restart), and from `handle_site_put()`'s success path, alongside `reissue_host_pki_cert()` (so a live rename is reflected immediately, matching the "editable, reflects back" behavior the user specifically asked for over the alternative of a second, independently-editable record).

**The rename-tracking variable is deliberately in-process only, not persisted.** A rename that happens to straddle a daemon restart with no `PUT /system/site` ever registering the "old" name during that process's own lifetime is a real, narrow gap this doesn't solve -- accepted rather than adding persistence machinery for a self-heals-on-next-real-PUT edge case (the *next* actual rename, whenever it happens, correctly cleans up whatever's currently registered from that point forward).

**Works today even without a DNS server actually serving it (Phase 38 Part 6, deferred).** The record sits in the same table `dns_record_create()`/`GET /dns/records` already expose -- inert until something consumes it, exactly like any other unconsumed record already behaves in this codebase. Whether your desktop can actually resolve it depends on a management-plane DNS server existing and being reachable from outside Kanxeo's own managed networks, a separate, larger, deliberately-not-yet-built piece (see the DNS-server-container item in this same identity work, still pending).

## Consequences

- On this project's own live production daemon (currently `--bind=0.0.0.0`), no instance DNS record is created at all -- correct per the design above, but a real, current limitation an operator would need to bind to a specific, real IP (not `0.0.0.0`) to get any record at all. Worth stating plainly rather than leaving implicit.
- A record created this way is otherwise a completely ordinary DNS record -- deletable, listable, indistinguishable from any operator-created one except by name. No special "this one is managed" flag or protection was added; an operator could delete or edit it manually, and the next `PUT /system/site` (or daemon restart) simply recreates/corrects it, the same tolerance this project's other "reflects back" mechanisms already have.
- Two independent in-process caches now exist for "what did I last register for this install's identity" -- `reissue_host_pki_cert()`'s implicit one (the `"host"` PKI record itself, persisted) and this ADR's explicit `g_last_instance_dns_name` (not persisted). Different persistence needs for a good reason: PKI's own state was already persisted infrastructure this reused; DNS's rename-cleanup need is new and narrow enough not to justify new persisted state of its own.
