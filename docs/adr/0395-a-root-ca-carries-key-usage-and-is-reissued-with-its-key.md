# 0395 — A root CA carries key usage, and an existing one is re-issued with its own key

## Status

Accepted, 2026-11-23, by the owner ("Accept"). Found while deploying `dav` (Radicale).

## Context

Radicale's LDAP login on gibsson-1 failed on its connection to ldap-1:

```
[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: CA cert does not include key usage extension
```

Python 3.13 verifies with `VERIFY_X509_STRICT` by default, which enforces RFC 5280: a CA certificate must carry `keyUsage` with `keyCertSign`. Cix's intermediate has it (`pki_intermediate_create()`: `keyUsage=critical,keyCertSign,cRLSign`). The root does not: `pkicrypto.c` gives it `subjectKeyIdentifier`, `authorityKeyIdentifier` and `basicConstraints=critical,CA:TRUE` only. So every Python TLS client that verifies a Cix certificate fails, and others that verify strictly will too.

The root on gibsson-1 is the site CA, "Cix Platform CA - uk.home.arpa". The owner's desktops trust it directly (2026-11-22).

## Decision

1. **A root CA is created with `keyUsage=critical,keyCertSign,cRLSign`**, as the intermediate already is.
2. **An existing root without it is re-issued in place: the same key and the same subject, a new serial, the missing extension added**, self-signed again. Nothing it signed changes: the intermediate's signature verifies against the same key, so every certificate under it stays valid, and so does trust in either copy of the root.
3. **It is an operator's action**, `POST /v1/pki/ca/reissue` (`cixctl pki ca reissue`), refused when the root already carries the extension. The new root reaches the trust bundle cixd stages into containers, so a container gets it at its next start.

## Alternatives considered

- **`POST /v1/pki/ca/reset`.** Rejected: new keys for the root and the intermediate. Every desktop would have to trust a new root, and every certificate would be re-issued.
- **Turn off strict verification in each Python client.** Rejected: it is a workaround in every consumer for a defect in one place.

## Consequences

- After the re-issue, Radicale's LDAP login works with full verification, and so does any other Python client.
- Desktops that trust the old root certificate keep validating every chain, since the key is the same. Replacing it there with the new copy is tidy but not required.
- Only the root is re-signed. No private key is generated or leaves the box.
