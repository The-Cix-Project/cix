# 0050 — Auto-issued host PKI cert: a fixed record name, not the FQDN itself

## Status

Accepted

## Context

Direct follow-up in the same identity-focused conversation as ADR-0049 (PKI CA reset): "By default we have a Cert for host.site.domain_suffix?" This is the leaf that Phase E7 (real TLS termination in `kanxeod`'s own listener, planned separately, not part of this phase) will eventually serve — building it now, correct and already synced with site config, rather than inventing it under time pressure inside that larger, later phase.

The real design question: `pki_cert_create()`'s `name` parameter is both the leaf's record-index key *and* its Common Name (`/CN=<name>`) — there's no separate stable identifier from the display name. This install's own FQDN (`<instance_name>.<site_name>.<domain_suffix>`) changes whenever any of those three fields change (`PUT /v1/system/site`, already real and expected to be used) — if the leaf were literally named after the FQDN, every rename would orphan the previous leaf under its old name with no relationship to the new one, accumulating stale, never-cleaned-up "host" certs over the install's lifetime.

## Decision

The host leaf's record name is a fixed literal, `"host"`, never the FQDN itself. The FQDN goes in the SAN list instead (`sans: ["<instance_name>.<site_name>.<domain_suffix>"]`) — modern TLS hostname verification uses SAN, not CN, so a cert whose CN is the generic label `"host"` but whose SAN carries the real, current hostname is both valid and correct; nothing consuming this cert for real TLS verification should ever be checking CN as the hostname anyway.

New `siteconfig_host_fqdn()` (`daemon/src/siteconfig.c`/`.h`) composes the FQDN from the three site-config fields (identical rule to the client-side `suggestedFqdn()`/the server-side `siteconfig_qualify()` this same identity work is adding — see ADR-0051 — but always using `instance_name` as the label, never an arbitrary caller-supplied one).

New `reissue_host_pki_cert()` (`daemon/src/main.c`) is a small, best-effort, delete-then-recreate helper: `pki_cert_delete("host")` (tolerating `PKI_ERR_NOT_FOUND` on the first-ever call) followed by `pki_cert_create("host", {fqdn}, 1, 365, NULL, ...)`. Called from every point in the daemon where the host's identity could have just changed or newly become expressible:

- After a successful `POST /v1/pki/ca` (root just bootstrapped).
- After a successful `POST /v1/pki/intermediate` (leaf signing just switched to the intermediate — reissuing here gets "host" onto the stronger chain immediately rather than waiting for some unrelated future reissue).
- After a successful `PUT /v1/system/site` (the FQDN may have just changed).
- After a successful `POST /v1/pki/reset` (ADR-0049) — technically redundant when "host" was already tracked (the reset's own generic per-leaf reissue loop already reissues it under the identical, still-current FQDN), but called again anyway for the case where it wasn't yet tracked (an existing chain that predates this feature). The redundant case costs one harmless extra `openssl` invocation; the alternative is a real, silent gap.

Every call is best-effort and never fails the caller it's attached to: no root CA bootstrapped yet is the common, expected state on a fresh install (site config is very likely to be set before PKI ever is) -- logged to stderr, never surfaced as an error on the actually-requested operation (a site PUT succeeding shouldn't depend on PKI being ready).

## Consequences

- Exactly one leaf named `"host"` exists at any time once PKI has ever been bootstrapped alongside a real site config -- never more than one, never orphaned duplicates from past renames, satisfying `GET /v1/pki/certs` staying a clean, meaningful list rather than accumulating cruft.
- An operator manually issuing a leaf literally named `"host"` via `POST /v1/pki/certs` would collide with this reserved name -- not prevented (no new namespace-reservation validation added), but a narrow, self-inflicted collision an operator would notice immediately (their own request either 409s against the existing one, or gets silently overwritten by the next site-config-triggered reissue). Worth knowing, not worth building defensive code against a name nobody has an actual reason to choose today.
- No consumer of this cert exists yet -- `kanxeod`'s own HTTP listener still serves plain HTTP only (Phase E7, not part of this work). This leaf sits correct and ready, genuinely unused until that lands.
- `key_pem` for "host" is returned in whichever response triggered its (re)issuance only if that response path chooses to surface it -- today's four trigger points (`handle_pki_ca_create`/`handle_pki_intermediate_create`/`handle_site_put`/`handle_pki_reset`) all discard `reissue_host_pki_cert()`'s own internal scratch response and return their own unrelated success body, so the private key is retrievable afterward only via `GET /v1/pki/certs/host`, which -- like every other leaf -- never includes `key_pem` (only `cert_pem`). Once Phase E7 needs the key on-disk to actually serve TLS, it reads the same `<pki_dir>/certs/host.key` file `pki_cert_deliver()` already knows how to reach; no API gap to close for that consumer, only for a human operator who wants a copy of the host key elsewhere, which isn't a use case anyone has asked for yet.
