# 0092 — auto-registered container DNS records were never site-qualified, unlike manual ones

## Status

Accepted

## Context

Raised directly by the user, noticing `dns-1`/`dns-2` (auto-registered via `--dns-register` at container-create time) had no domain suffix in `GET /v1/dns/records` (`"dns-1"`, `"dns-2"`) while every manually-created record (`kanxeo`, `ldapsvc`) showed the full site-qualified form (`kanxeo.uk.home.arpa`, `ldapsvc.uk.home.arpa`) -- asking directly whether the bare form was intentional.

It was not. `daemon/src/main.c`'s manual `dns record create` handler (`handle_dns_record_create()`) has always called `siteconfig_qualify()` (ADR-0052: a bare, dot-less label gets the site's own default suffix appended server-side) before `dns_record_create()`. The container-create handler's own `dns_register` block, one function away in the same file, called `dns_record_create(entry->name, ...)` directly -- skipping that exact call, for no documented reason; a real, previously-undiscovered inconsistency, not a deliberate design choice, confirmed by grepping both call sites side by side.

## Decision

Apply `siteconfig_qualify()` in the `dns_register` auto-registration path too, matching the manual path exactly -- `entry->name` (the container's own name) is qualified into a local buffer before being passed to `dns_record_create()`; `owner_container` (used for auto-delete when the container is removed) stays the raw, unqualified container name, since it identifies the container, not a DNS name.

This exposed a second, smaller bug while fixing it: `dns_record_forget_owner()` (called on container delete) looked up the record to remove via `dns_record_find(container_name)` -- a lookup by *name*, which only worked because `name == container_name` held for every auto-registered record before this fix. Once qualification makes that no longer true, the lookup would silently miss and leak the record on every future container delete. Fixed by having `dns_record_forget_owner()` scan `g_records[]` for `owner_container == container_name` directly -- the semantically correct approach regardless of what qualification does to the name, and the more honest read of what "forget owner" actually means.

## Consequences

- `--dns-register` and manual `POST /v1/dns/records` now produce identically-shaped names for a bare label -- no more inconsistency between the two entry points into the same record set.
- Deleting a `--dns-register`'d container correctly removes its now-qualified record (verified via the `dns_record_forget_owner()` fix above) -- would have silently leaked otherwise.
- `dns-1`/`dns-2`'s own pre-existing bare records (created before this fix, in an earlier session) were manually removed and left to be recreated fresh by the qualified path -- no migration needed for a single-digit number of existing records on one box; a fleet-scale deployment would want a one-time migration pass instead, not attempted here since this project has none yet.
