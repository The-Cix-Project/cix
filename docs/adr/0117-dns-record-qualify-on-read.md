# 0117 — DNS record GET/PUT/DELETE now qualify a bare name the same way POST already does

## Status

Accepted

## Context

Found live during task #760's full feature regression sweep on 192.168.15.95, right after ADR-0116's `json.c` fix: `kanxeoctl dns record rm sweeptest` returned `404 no such DNS record` immediately after `kanxeoctl dns record create --name=sweeptest --ip=...` had just succeeded and `dns record ls` showed it present — as `sweeptest.uk.home.arpa`.

`handle_dns_record_create()` (`daemon/src/main.c`) has always called `siteconfig_qualify()` (ADR-0052) on an incoming `name` before storing it — a bare label with no `.` gets this install's own `site_name`/`domain_suffix` appended, so `POST {"name":"sweeptest",...}` actually stores (and returns) `sweeptest.uk.home.arpa`. `dns_record_find()` (`daemon/src/dns.c`) matches by an exact `strcmp()` against that stored name. `handle_dns_record_get_one()`, `handle_dns_record_update()` (PUT, task #749), and `handle_dns_record_delete()` all passed their own `name` argument straight to `dns_record_find()`/`dns_record_update()`/`dns_record_delete()` with no qualification step — so only the exact, fully-qualified FQDN a caller might not even know to reconstruct (deriving it requires knowing this install's own `site show` output) could ever look the record back up. The bare name a caller just used to create it always 404'd.

## Decision

`handle_dns_record_get_one()`, `handle_dns_record_update()`, and `handle_dns_record_delete()` now call `siteconfig_qualify()` on their `name` argument before doing the lookup, exactly mirroring `handle_dns_record_create()`. This is safe and idempotent for every caller shape: `siteconfig_qualify()` only ever qualifies a label with **no** `.` in it — an already-fully-qualified name (or any name containing a dot for unrelated reasons) passes through completely unchanged, so this fix introduces no behavior change for a caller that already knew and used the full FQDN.

## Verification

`test/test_dns.c`'s existing site-qualification scenario (`site_name=lab9 domain_suffix=qualify.test`, `POST {"name":"bareweb",...}` confirmed to store as `bareweb.lab9.qualify.test`) is extended with three new assertions using the *bare* name throughout: `GET /v1/dns/records/bareweb` (200, finds the qualified record), `PUT /v1/dns/records/bareweb` (200, updates it), `DELETE /v1/dns/records/bareweb` (204, removes it) — all three would 404 before this fix and all pass after. Full clean rebuild (`-Wall -Werror`), zero warnings. Full regression sweep (23 tests) all pass.

Live-verified directly against 192.168.15.95: `kanxeoctl dns record create --name=sweeptest --ip=192.168.15.200` followed immediately by `kanxeoctl dns record rm sweeptest` now succeeds (previously 404).

## Consequences

- Every DNS record CRUD caller (CLI, web dashboard, any future REST client) can now consistently use the same name — bare or qualified — across create/read/update/delete, matching the ordinary expectation that "the name you created something with is the name you can look it back up by."
- No API-visible schema change: the response body for every one of these endpoints already returned the qualified name (confirmed by the existing test's own assertion on `POST`'s response) — this fix only changes which *request* names successfully resolve to an existing record, not what gets returned once one does.
- The same class of gap was checked for and confirmed absent elsewhere: LDAP user/group names and PKI certificate names are not site-qualified by `siteconfig_qualify()` at all (confirmed by inspection — only `dns_record_create()` and the auto-DNS-registration path call it), so no equivalent fix is needed for those CRUD surfaces.
