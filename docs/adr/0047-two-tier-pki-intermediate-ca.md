# 0047 — Two-tier PKI: an intermediate CA for day-to-day signing

## Status

Accepted

## Context

Raised directly by the user in the same batch of platform-maturity questions that produced ADR-0046: "we have a root CA, we also need an intermediate CA, and certs, right?" Investigated first: this project's PKI (`daemon/src/pki.c`, Phase 9) has always been a single self-signed root CA (`pki_ca_create()`, one `openssl req -x509` call) issuing every leaf certificate directly (`pki_cert_create()`, signed with `-CA ca.crt -CAkey ca.key`) — no intermediate tier existed anywhere.

The real motivation for an intermediate tier, confirmed with the user rather than assumed: it lets the root stay offline/rarely-touched (the actual root of trust, exposed to signing operations exactly once — creating the intermediate) while an intermediate does the day-to-day leaf-signing work, and can be rotated or revoked independently without every existing client needing to re-trust a new root. This is standard real-world PKI practice (every public CA does this), not a thinC-specific invention.

## Decision

`pki_intermediate_create()` (`daemon/src/pki.c`) generates a second, real keypair and cert — **signed by the root, not self-signed** — with `basicConstraints=critical,CA:TRUE,pathlen:0` and `keyUsage=critical,keyCertSign,cRLSign` baked in via `-addext` at CSR-creation time (the same `-addext`-then-`-copy_extensions`-at-signing pattern `pki_cert_create()`'s own leaf SAN handling already established — reused, not reinvented). `pathlen:0` deliberately caps the chain at exactly this depth: this intermediate can sign leaves, never a further intermediate — matching the actual, single-extra-tier need, not a speculative arbitrary-depth hierarchy. One-shot only, mirroring `pki_ca_create()`'s own root precedent exactly: `PKI_ERR_NOT_BOOTSTRAPPED` if the root doesn't exist yet, `PKI_ERR_ALREADY_BOOTSTRAPPED` on a second call — no rotate/replace operation in v1.

`pki_cert_create()` (leaf issuance) is **unchanged at the API level** — same function signature, same callers, same response shape. Internally, its signing step now checks `pki_intermediate_bootstrapped()` and signs with the intermediate if one exists, the root directly otherwise. This means bootstrapping an intermediate is fully backward-compatible and non-disruptive: every existing `--pki-issue` container-creation flow, and every already-issued leaf cert, keeps working unchanged; the only observable difference is that a *newly issued* leaf's issuer becomes the intermediate instead of the root, confirmed directly (`openssl x509 -noout -issuer` on a freshly issued leaf shows `CN = thinC Intermediate CA`, not the root).

`pki_cert_deliver()` (writes `tls.crt`/`tls.key` into a running container via `/proc/<pid>/root/`) now writes the **real, complete chain** — leaf followed by intermediate, the standard `fullchain.pem` convention real TLS servers expect — when an intermediate has been bootstrapped, the leaf alone otherwise. The root itself is deliberately never included in the delivered chain (a verifier already trusts the root out-of-band, via `GET /v1/pki/ca`; including it is neither required nor standard practice). Verified directly: a real container's own delivered `tls.crt` genuinely contains 2 certificates in the correct order, and `openssl verify -CAfile root.crt -untrusted intermediate.crt leaf.crt` returns `OK` — a genuine, working trust chain, not just concatenated PEM text.

New endpoints mirror the root's own shape exactly: `GET`/`POST /v1/pki/intermediate` (same 404-for-missing/400-for-precondition/409-for-already-exists mapping `/pki/ca` already established, with endpoint-specific wording since the generic error mapper's root-CA phrasing would otherwise be misleading here).

## Consequences

- The root private key's own real-world exposure is now minimal: it signs exactly one thing (the intermediate), once, and never touches leaf issuance again once an intermediate exists — the actual security property this ADR exists to deliver.
- Fully opt-in and non-breaking: an operator who never bootstraps an intermediate sees zero behavior change anywhere in this platform's PKI.
- No rotation/renewal mechanism for the intermediate in v1 — same accepted boundary the root CA has always had; a real future need if this platform's PKI usage grows, not attempted here.
- `GET /v1/pki/certs/{name}` and the list endpoint continue to report leaf-only `cert_pem` (unchanged shape) — only the *delivered*, on-disk `tls.crt` (what a real TLS server actually reads) carries the full chain. A REST-level "fetch the chain for cert X" endpoint was considered and deferred as unnecessary scope beyond what was asked; the operationally meaningful delivery path already works.
