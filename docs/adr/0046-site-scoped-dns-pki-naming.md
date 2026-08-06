# 0046 — Site-scoped DNS/PKI naming: a real, configurable convenience, not enforcement

## Status

Accepted

## Context

Raised directly by the user during a batch of platform-maturity questions: should DNS records and PKI certificate SANs live under a domain, and if so, what should it be? Investigated before answering: this project's DNS records (`daemon/include/dns.h`) and PKI leaf SANs (`daemon/include/pki.h`) are, and always have been, bare operator-supplied strings — `dns_name_is_valid()` accepts any well-formed multi-label hostname, nothing enforces or suggests a suffix, and the live demo topology in this sandbox (`cr-1`, `srv1`, ...) has always used short, unqualified names.

The obvious default — hardcode `.internal` — was explicitly rejected by the user: "this should be configurable, and the site name a configurable... this has an effect on both DNS and PKI, and should have the necessary API." `.internal` itself is still the right *default* (IANA-reserved for exactly this use, RFC 9476 — never collides with real public DNS, unlike inventing a suffix or squatting a real domain the operator doesn't control), but it needed to become a real, settable value with a real API behind it, not a compile-time constant.

The harder question was scope: does declaring a site identity mean DNS records and PKI SANs should be *validated* against it (reject an unqualified name), or does it stay a convenience? Enforcement was considered and explicitly rejected for this phase: this project's own live demo topology has five containers with short, unqualified names (`cr-1`, `srv1`, `blah`, ...) that work correctly today; retroactively requiring a domain suffix would be a real regression against working state (violates the project's own "No Regressions" maxim) for zero functional gain — nothing about routing, DNS resolution, or TLS validation actually requires a qualified name in this platform's architecture. Multi-host/multi-site support (`<host>.<site>.internal` patterns) was named as a real future need but explicitly out of scope for this phase — this phase's job was giving operators a place to *declare* a site identity, not building a mesh naming scheme on top of it.

## Decision

New minimal module, `daemon/src/siteconfig.c`/`daemon/include/siteconfig.h`: two persisted fields, `site_name` (operator-chosen label, e.g. `"lab1"`; `""` is valid, meaning no site tier — a single-site deployment) and `domain_suffix` (defaults to `"internal"` but real and settable, validated with `dns_name_is_valid()` — reused, not reimplemented). Persisted via the same atomic-JSON pattern every other piece of daemon state already uses (`persist_atomic_write()`/`persist_read_file()`, ADR-0012). New `GET`/`PUT /v1/system/site` (the platform's first `PUT` endpoint — every prior mutating endpoint used `POST`; a genuine "replace this resource's current state" semantic, matching REST convention more precisely than a `POST` would have here).

This module has **no effect on DNS record creation or PKI cert issuance** — no daemon-side code ever reads `site_name`/`domain_suffix` to validate, qualify, or reject a name. Its only consumer is the web dashboard's own DNS-record-create and cert-issue forms, which read `GET /v1/system/site` once and pre-fill a suggested FQDN (`<typed-name>.<site_name>.<domain_suffix>`, or `<typed-name>.<domain_suffix>` when `site_name` is empty) — editable, never forced. This composition happens entirely client-side; no server-side "qualify a name" function exists, since nothing server-side needs one (avoiding a speculative, unused C function per this project's own "No Stop-Gaps" discipline).

*(Superseded in substance, found during the Phase 41 documentation audit: ADR-0052 later added `siteconfig_qualify()` — a real, server-side default-qualification function, called from both DNS record creation and PKI cert issuance for a bare, dot-free name. This paragraph's "no server-side function, nothing server-side needs one" claim is no longer accurate; see ADR-0052 for the current mechanism and why it was added. This ADR's own site-config module, `GET`/`PUT /v1/system/site`, and the rest of this Decision remain accurate and unchanged.)*

## Consequences

- Real, working today: an operator can declare `lab1`/`corp.internal` (or any valid pair) via a real API, and it survives a daemon restart, verified directly against this session's own live daemon.
- Zero behavior change to DNS/PKI: every existing unqualified name (`cr-1`, `srv1`, ...) keeps working exactly as before this module existed — confirmed by design, not just by omission.
- If strict enforcement (reject any DNS/PKI name not ending in the configured suffix) is ever actually wanted, that is a separate, harder-to-reverse decision requiring its own ADR — this one deliberately does not preclude it, but does not build it either.
- Multi-host/multi-site naming patterns (`<host>.<site>.internal`) remain a real, named-but-undesigned future need — this ADR only covers one install's own declared identity, not a cross-host naming scheme.
