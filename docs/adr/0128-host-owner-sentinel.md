# 0128 — `__host` owner sentinel for the daemon's own PKI cert / DNS record

## Status

Accepted

## Context

Part 5 of the logging/web-UI epic — a small, user-requested clarity fix: "for the entry for the host in pki certs, dns, can we make the owner the host? so that it's clear?"

The daemon auto-issues its own leaf PKI cert (`reissue_host_pki_cert()`, name `"host"`, ADR-0050) and auto-maintains a DNS record for its own FQDN (`reconcile_instance_dns_record()`, ADR-0053) — both keyed to the current site config, both re-issued/re-pointed whenever `PUT /v1/system/site` changes the FQDN. Neither is owned by any container (the `owner_container` field PKI/DNS records already have for auto-provisioned, container-tied service accounts, task #727) — both call sites passed `NULL` for it, which the API/web UI rendered as a blank owner column. Confirmed correct in isolation, but genuinely ambiguous next to a real record whose owner is legitimately absent for an unrelated reason (a plain, manually-created DNS record, for instance) — nothing distinguished "not owned by a container, because this is the daemon's own record" from "not owned by a container, full stop."

Confirmed via `AskUserQuestion` before implementing: use `"__host"` as a reserved sentinel string, not the bare `"host"`. `simple_name_is_valid()` (`include/namecheck.h`) permits `_` in a container name, so a container literally named `"host"` is a real, creatable name — passing bare `"host"` as the owner would make that container's own eventual `DELETE` wrongly call `pki_cert_forget_owner("host")`/`dns_record_forget_owner("host")` and wipe the daemon's own record, an unrelated, real container just happening to share a name with the sentinel. `"__host"` can never collide with a real container's own name in the same way this project's existing `"__pkgbuild"`/`"__hostbuild"` reserved pseudo-names already don't (confirmed: neither is rejected outright at container-creation time either — this is an accepted, narrow, already-established risk class in this codebase, not a new one introduced here).

## Decision

Two call sites in `daemon/src/main.c` changed from `NULL` to the literal `"__host"`:
- `reissue_host_pki_cert()`: `pki_cert_create("host", sans, 1, 365, "__host", &scratch)`.
- `reconcile_instance_dns_record()`: `dns_record_create(fqdn, addr.s_addr, "__host", &rec)`.

No other code changes to `pki.c`/`dns.c` — `owner_container` was already a plain string field with no special-casing, so this is purely a different value passed into an existing parameter, not a new mechanism.

**Display, not raw sentinel, at every surface**: a new `formatOwner()` (`web/app.js`) and `owner_display()` (`cli/src/main.c`) both map `"__host"` → `"host (this daemon)"` for display, leaving every other owner value (including `null`/absent) unchanged. The REST API itself is untouched — `GET /v1/pki/certs/host`/`GET /v1/dns/records/<fqdn>` both return the raw `"__host"` string in their own `"owner"` field, exactly as any other owner value would be returned; only the two client surfaces add a friendlier label, the same "REST returns the real value, CLI/web decide how to present it" split every other formatted field in this codebase already follows.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. `test/test_pki.c`'s own existing ADR-0050 assertion (site-config-triggered host-cert reissue) updated from "owner must be null" to "owner must be exactly `__host`" — this is a real, deliberate behavior change from this ADR, not a stale test left unfixed. Full regression sweep (37 test binaries) confirms zero regressions.

## Consequences

- `GET /v1/pki/certs`/`GET /v1/dns/records` now unambiguously distinguish "the daemon's own auto-maintained record" from "no owner for any other reason" — closes the exact ambiguity the user flagged.
- A future container genuinely named `host` (or, in principle, `__host` itself, since the charset check doesn't forbid it) is a real, narrow, pre-existing risk this ADR doesn't introduce or worsen — already true of `__pkgbuild`/`__hostbuild`. Not addressed here; a genuine fix (rejecting `__`-prefixed container names outright) would be a separate, broader decision affecting every reserved pseudo-name in this codebase at once, not scoped to this one part.
