# 0220 — A separate release-signing key

## Status

Accepted. Unblocks [#197](https://git.home.arpa/itdlabs/cix/issues/197),
whose stated approach cannot be implemented. Extends
[ADR-0212](0212-signing-keys-over-rest.md)'s REST key-transport pattern to
a second key rather than overloading the first.

## Context

The artifact cache accepts installer ISOs and refuses unsigned ones. It
requires a `minisign` signature (`<iso>.minisig`), published *before* the
ISO, and rejects an unsigned bootable at header time. That side is
finished and deployed (cix-cache#8, `v2.11.0`); nothing there is
reachable until Cix signs an ISO.

#197 specified doing that with **the existing platform signing key** —
explicitly "no new key, no new key management" — while also specifying
**Ed25519**. Those cannot both hold:

- the existing key is the **Secure Boot** key. `image/src/mkinstalleriso.c`
  hands it to `sbsign`, and UEFI Secure Boot mandates **RSA**.
- `minisign` is **Ed25519-only**.

An Ed25519 signature cannot be produced from an RSA key. That is a
different algorithm, not a different encoding, so an implementation
built to #197 as written fails at its first signing call.

## Decision

**A second, separate Ed25519 key, for signing releases.**

This is not a reluctant workaround for the algorithm mismatch. The two
keys answer different questions and deserve to be different keys:

| key | answers | blast radius if stolen |
|---|---|---|
| Secure Boot (RSA) | may this firmware boot this image? | enrolled in machine firmware; rotating means re-enrolling MOK on every host |
| Release (Ed25519) | did Cix publish these bytes? | rotate by publishing a new public key |

Sharing one key would mean anyone able to sign a download is also able
to sign a bootloader. The separation is the safer arrangement even where
one key would technically work, and #197's "no new key" instinct was
optimising for the wrong cost.

Specifics, each verified against stock `minisign 0.11` before being
written down:

1. **Operator-supplied, never generated here.** The key arrives as an
   Ed25519 private key in PEM through REST, exactly as ADR-0212's pair
   does, and nothing — no GET, no error, no log line — brings it back
   out. `cixd` never generates or fetches a signing key on its own
   initiative; that property of ADR-0064 and ADR-0212 is preserved
   rather than re-argued.

2. **Legacy `Ed` mode, signing the raw ISO.** minisign supports both a
   prehashed `ED` mode (BLAKE2b-512 of the message) and legacy `Ed`
   (Ed25519 over the message itself). We use `Ed`, because OpenSSL's
   Ed25519 is PureEdDSA and signs the message directly — so the daemon
   needs no BLAKE2b implementation, and `openssl` is already a runtime
   dependency it shells out to. Stock minisign verifies both.

   Note this corrects #197's wording, which says the daemon signs "the
   digest". It signs the ISO.

3. **The key id is derived, not random.** minisign generates a random
   8-byte id at keygen and stores it in both the secret and public key
   files. We hold only a PEM private key, so the id is the first 8 bytes
   of `SHA-256(public key)` — deterministic, so the public key file and
   every signature agree by construction, with no id to store, lose, or
   let drift.

4. **The global signature is not optional.** A `.minisig` carries two
   signatures: one over the file, and one over `signature ‖ trusted
   comment`. Omitting the second is the standard mistake, and stock
   minisign rejects the result. Verified in both directions — altering
   only the trusted comment produces `Comment signature verification
   failed`.

## Consequences

**An operator must provision a second key before an ISO can be
published.** That is a real new step, and the honest cost of this
decision. `POST /v1/system/iso` still builds an ISO without one; only
publishing requires it, so a host that never publishes needs no release
key at all.

**The public key must reach verifiers by some path other than the
artifact cache.** A signature checked against a key fetched from the
same place as the bytes proves nothing. Distribution of the public key
is out of scope here and is the operator's to arrange — the daemon
exposes it, and says nothing about how it travels.

**Verification is deliberately not ours.** The check runs with upstream
`minisign`, not a verifier written here. This is the artifact that gets
*booted*; the last thing between a substituted ISO and a machine should
not be code from the same project that produced the ISO. Signing with
OpenSSL and verifying with minisign is therefore correct here rather
than a smell — only the verify side is a trust boundary.

## Alternatives considered

**Reuse the Secure Boot key.** Impossible, as above — and undesirable
even if RSA were acceptable to the cache, for the blast-radius reason.

**Have the cache accept an OpenSSL/RSA signature format.** Rejected: it
is deployed and its format is already the widely-implemented one, and it
would replace a standard verifier with a bespoke one on the single most
security-sensitive artifact this project produces.

**Generate the Ed25519 key on the release host.** Tempting — no operator
step, and the private key never travels. Rejected because it inverts
ADR-0064's property that `cixd` never creates signing material on its own
initiative, and because a key that exists only on one box is lost with
that box, taking the ability to sign anything verifiable by already-
published public keys with it.
