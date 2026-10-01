# 0321 — cixd's cryptography is done in the libcrypto it links, never by forking `openssl`

## Status

Accepted by the owner on 2026-10-01 ("do it all"), for [#351](https://git.home.arpa/itdlabs/cix/issues/351).

## Context

The PKI (ADR-0059's note: "`pki.c` shells out to the `openssl` binary as a subprocess for CA/cert operations"), the release-signing key (ADR-0220/ADR-0279) and the Secure Boot signing keys all did their cryptography by forking `/usr/bin/openssl` and parsing what it printed. That was 23 call sites in four files, behind one runner (`opensslrun.c`). The WebSocket handshake did too, until it moved in-process under the same issue.

At the same time cixd links `-lssl -lcrypto` for its HTTPS listener (ADR-0059), so the library that did the work was already in the process that forked a second program to reach it. That is a parallel implementation, which the maxims refuse. The networking plane talks rtnetlink rather than running `ip` for the same reason.

It also cost something concrete:
- **One more program the control-plane root must carry.** A missing program on that root is a known failure mode: 0.2.57-414's assembly forked a `cp` the running root no longer had (#554).
- **Secrets on disk.** The export passphrase and the decrypted bundle, every private key, went through temp files so the child could read them.
- **Answers taken from a child's text output**, which also hid a bug: `signingkeys` reported `fingerprint_sha256`, and the value was SHA-1. A comment said OpenSSL 3 defaults to SHA-256; the CLI's default is SHA-1, measured below.

## Decision

All of it is done in-process, through one module, `daemon/src/pkicrypto.c`, which pki.c, signingkeys.c and releasekey.c call. `opensslrun.c` and every `/usr/bin/openssl` path in cixd are gone, and `openssl` leaves `CONTROLPLANE_PROGRAMS` and mkbootroot's staging list.

The module reproduces what the CLI produced. That was measured on a Cix host, not recalled: `probe-pki-cli@1` on 192.168.15.95, 2026-10-01, OpenSSL 3.0.20.

| what | as the CLI did it, and so as this does |
|---|---|
| keys | RSA-2048, unencrypted PKCS#8 PEM, mode 0600 (now set at creation, not by a later chmod) |
| root CA (`req -x509 -new`) | v3, sha256WithRSA, extensions Subject Key Identifier, Authority Key Identifier (key id only), basicConstraints critical CA:TRUE, in that order |
| issued certificates (`x509 -req -copy_extensions copy`) | the requested extensions in order (the intermediate's basicConstraints and keyUsage, a leaf's subjectAltName, parsed by the same libcrypto routine `-addext` used), then SKID and AKID |
| validity, serial | now to now + days; a random 159-bit serial (`-CAcreateserial` started from one) |
| subject | CN as a UTF8String, which is what the live root CA's DER carries |
| field text | subject `CN = name`, serial uppercase hex, dates `Sep  7 16:47:25 2026 GMT`. The API returns these strings, so they are checked against the live root CA's values |
| export bundle | `openssl enc -aes-256-cbc -pbkdf2 -iter 600000 -md sha256 -salt -a -A`, byte-compatible, so bundles exported by earlier releases import |
| Ed25519 | raw public key and PureEdDSA signatures over whole files. Same key bytes, so release key ids are unchanged |

`test_pkicrypto` (SELFTESTS) holds the CLI's own outputs as known answers: the live root's field strings, the CLI's ciphertext and PBKDF2 key/IV, and its Ed25519 public key and signature. It also checks a fresh root → intermediate → leaf chain against the measured profile and verifies it.

## What changes that anyone can see

- **Serial numbers are random for every certificate.** The CLI's serial file counted up from a random start, which is why the live leaves read `…3099`, `…30A9`, `…30AF`. Nothing writes `ca.srl`/`intermediate.srl` now. A PKI reset still deletes ones an older release left. A serial number carries no meaning beyond uniqueness, and 159 random bits give that.
- **`fingerprint_sha256` is a SHA-256 fingerprint**, as the field's name and the API contract always said. Before, it carried a SHA-1 fingerprint.
- The secrets stay in memory, and the decrypted bundle is zeroed after parsing.

## What does not change

- The OpenSSL *libraries* stay on the control-plane root: cixd links them, as it did. Only the `openssl` program leaves. #351 framed the deliverable as "the openssl package leaves the root", which is not possible while cixd links the library.
- Certificates already issued, the CA, the API and every on-disk path are untouched, so a rollback to the other A/B slot reads everything this writes.

## Alternatives

- **Keep the CLI and fix only the fingerprint.** It leaves the parallel implementation and the secrets-on-disk path, which are the substance of #351.
- **Move to a modern export format (AEAD, a versioned container).** Better cryptography, but every existing bundle would stop importing, and a backup format is not changed in passing. If it is wanted, it is its own decision, with a version field and a reader for the old format.
