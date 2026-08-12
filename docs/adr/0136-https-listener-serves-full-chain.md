# 0136 — HTTPS listener sends the intermediate CA in its handshake chain

## Status

Accepted

## Context

Direct follow-up to ADR-0135, same investigation: after downloading and trusting the root CA per the new workflow, the user reported HTTPS to 192.168.15.95 was "still failing." `openssl s_client -showcerts -connect 192.168.15.95:8443` showed the daemon sending exactly **one** certificate during the handshake — the `"host"` leaf — even though that box has an intermediate CA bootstrapped. Cross-checked via Authority Key Identifier/Subject Key Identifier matching: the leaf's AKI does match the Intermediate CA's SKI, confirming the leaf really is intermediate-signed, as `pki.c`'s own issuance logic (ADR-0009's tiering: once an intermediate exists, every future leaf is signed by it, transparent to the caller) already guarantees. The leaf just was never being *served* alongside it.

Root cause: `create_tls_ctx()` in `daemon/src/main.c` only ever called `SSL_CTX_use_certificate_file()` against the leaf's own `.crt` file. That OpenSSL call loads exactly one certificate from a PEM file and silently ignores any additional certs present — it was never going to pick up the intermediate even if it had been appended to the same file, which it isn't (`host.crt` on disk holds only the leaf; `pki_cert_deliver()`'s separate chain-building path is what produces a leaf+intermediate file, for containers receiving an issued cert via `--pki-issue`, not for the daemon's own listener). The practical effect: on any box with an intermediate CA bootstrapped, a client that trusts only the root (exactly what ADR-0135's new download-button UX instructs) could never complete a handshake — the client has no way to link the presented leaf back to a root it never saw during the handshake and doesn't have cached. This is a genuine, previously-silent regression: the intermediate CA and HTTPS have coexisted on 192.168.15.95 for multiple prior phases with no test ever exercising a live handshake to notice.

Two other findings surfaced during the same investigation, both informational rather than requiring a code change here:

- Connecting by bare IP address (`https://192.168.15.95:8443/`) rather than the install's DNS FQDN triggers a *separate* browser check — the auto-issued `"host"` leaf's SAN carries only the FQDN, not the raw IP, so a strict client flags a hostname mismatch independent of chain trust. No IP-SAN issuance exists in this project; documented as a known caveat (see Consequences) rather than fixed here.
- `docs/api/README.md` currently overstates what `GET /pki/certs/{name}` returns for the general case (implying every fetched cert always carries the full chain) — that's only true for `pki_cert_deliver()`'s container-delivered `tls.crt`. Left for a documentation-only follow-up; not a functional gap.

## Decision

`create_tls_ctx()` now adds the intermediate CA, if one is bootstrapped, as an extra chain certificate on the same `SSL_CTX` the leaf was already loaded into — via `SSL_CTX_add_extra_chain_cert()`, not by rewriting `host.crt` on disk. A new accessor, `pki_intermediate_cert_pem()` (`daemon/src/pki.c`/`.h`), reads the intermediate's cert PEM straight off disk if bootstrapped; `create_tls_ctx()` parses it with `BIO_new_mem_buf()` + `PEM_read_bio_X509()` and hands the resulting `X509*` to `SSL_CTX_add_extra_chain_cert()`, whose ownership-transfer contract means the pointer is never freed on success and always freed on any failure path (read failure, parse failure, or the add call itself failing) before returning an error.

Deliberately not folded into `host.crt` on disk: that file is also the input to `pki_cert_deliver()`'s own chain-building for containers, which already appends the intermediate correctly for that separate path — writing it into `host.crt` too would double it there. Two different consumers (this daemon's own listener vs. a container receiving an issued cert), two different existing mechanisms, each already correct for its own case; this decision fixes the one that was missing, not both.

## Consequences

- Any box with both HTTPS enabled and an intermediate CA bootstrapped now sends the full leaf+intermediate chain on every handshake — a client trusting only the root, exactly ADR-0135's documented workflow, can now actually validate it. `docs/guides/security.md` updated to state this directly (trusting the root alone is sufficient) instead of leaving it as an untested assumption.
- New regression coverage: `test/test_https_chain.c` bootstraps a root, then an intermediate, then enables HTTPS (the exact ordering that exposed the bug — intermediate present *before* the listener starts), captures the live handshake via `openssl s_client -showcerts`, asserts exactly 2 certificates are sent, and verifies the captured chain with `openssl verify -CAfile <root> -untrusted <chain> <chain>` — real cryptographic verification, not string matching, following `test_pki.c`'s established `run_openssl_argv()` pattern.
- The hostname/SAN-mismatch caveat (bare-IP access) is now documented in `docs/guides/security.md` as a known, accepted limitation rather than silently discovered per-user — no IP-SAN issuance capability is planned as part of this decision.
- Unrelated to this fix but decided in the same round: the CA download button (ADR-0135) now saves the file with a `.crt` extension and `application/x-x509-ca-cert` MIME type instead of `.pem`/`application/x-pem-file` — content is unchanged (still the same PEM-encoded text `cert_pem` already returns), but `.crt` is the extension Windows' own "Install Certificate" flow recognizes directly, removing a manual-rename step the original `.pem` instructions required.
