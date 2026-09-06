# 0254 — An upstream checksum is verified, never computed here

## Status

Accepted

Answers the question [ADR-0193](0193-kernel-release-channel-policy.md) deliberately left open, and supersedes its "the channel reports; it does not act" only in the narrow sense that acting now has a safe form. The reporting behaviour, the null semantics and the longterm resolution rule all stand.

## Context

ADR-0193 gave a box a kernel channel: `stable` resolves, live from kernel.org's own `releases.json`, to a version and a tarball URL. It stopped there, and said why:

> an auto-generated pin means the daemon records a `pkg_sha256` it computed by downloading the tarball itself, which changes that checksum's meaning from "an operator verified this out of band" to "whatever arrived first". That is a downgrade of a security property, and it should not happen as a side effect of a convenience feature.

That reasoning is right and this ADR does not weaken it. `pkg_sha256` is a **trust anchor**, not a hash. Its value is entirely that a human vouched for those bytes; a checksum the daemon computes over its own download is circular — every later verification compares the bytes against themselves.

The owner's objection to stopping there is also right, and is about a different thing: a *persisted operator setting* that changes only what is reported is a promise the system does not keep. A box can be told to track `stable`, be told it is behind, and have no path to acting on it that is not a human editing a recipe by hand. The gap was never that the machinery is missing — `mode: rolling` plus `queue_rolling_rebuilds_for()` already rebuild every image tracking a package the moment a new recipe revision is published. The missing link is **turning an upstream release into a recipe revision without downgrading the anchor**.

## Decision

**A checksum this platform pins is one an upstream publisher signed, verified against a fingerprint a human pinned. It is never one the daemon computed over its own download.**

kernel.org publishes `sha256sums.asc` in every release directory: its own SHA-256 for every tarball, PGP-clearsigned. Quoting that checksum is neither trust-on-first-use nor a false claim of out-of-band review — it is precisely what an operator does by hand, and it stays checkable by anyone afterwards.

`daemon/src/pgpverify.c` verifies it. Four things about its shape are deliberate:

**It implements framing, not cryptography.** OpenPGP packet parsing and RFC 4880 §7 cleartext canonicalisation are here; the SHA-2 and the RSA verification are OpenSSL's, which `cixd` already links. A hand-rolled signature primitive is the one thing this project should never own. Packet framing is bounded, and a malformed packet fails verification rather than leaking anything.

**Key trust is the caller's, and a pin is mandatory.** Nothing in the verifier fetches, stores or blesses a key. The caller supplies the armored key *and* the fingerprint it expects; `expect_fingerprint == NULL` is refused rather than treated as "any key will do". A verifier that acquired its own trust root over the network would rebuild trust-on-first-use one layer up, which is the exact thing this exists to prevent.

**Unverified text is unreachable.** The signed text is returned only on success. There is no API for reading the payload of a document that did not verify, so a caller cannot accidentally use it, and `pgp_checksum_lookup()` takes the *verified* text specifically so it cannot be called on raw input.

**Unsupported constructs are refused by name.** v4 signatures, RSA, SHA-256/384/512. Anything else returns `PGP_VERIFY_UNSUPPORTED` naming what it was, rather than being approximated. Partial body lengths are refused for the same reason: half-supporting a framing feature is how parsers become exploitable.

## Consequences

- The remaining link — channel → verified checksum → generated recipe revision → publish — becomes ordinary code with no security question left in it. Publishing is what the existing rolling machinery already reacts to.
- **The fingerprint is pinned by a human, out of band, and that cannot be automated.** It belongs in `docs/keys/` beside this project's own public keys, where it is auditable. Until one is pinned there is nothing to verify against, and the verifier says so rather than proceeding.
- Two real defects were found by testing against kernel.org's actual document rather than only against fixtures written here, and both are recorded because they argue for that practice:
  - `canonicalise()` allocated `len + 2`. LF becomes CRLF, so a k-line document can grow by k bytes; the ~200-line real file overran the heap. Now `len * 2 + 2` with a hard guard on every write.
  - `pgp_checksum_lookup()` did not strip the CR from CRLF-canonical text, so every lookup missed by one byte.
- `test_pgpverify` embeds its fixtures, so it needs no network and no gpg — a test requiring either is a test that silently stops running in a build container. Four defects were reintroduced to prove it catches them, including both of the above.
- The fixture's target line sits deliberately **mid-document**: as the last line it has no trailing CRLF, and the CR-stripping bug passed against it by luck. A test that passes for the wrong reason is worse than one that fails.

## Alternatives considered

**Package GnuPG and shell out to `gpgv`.** A real, audited implementation, and the obvious first instinct. Rejected on dependency weight: GnuPG 2.x needs libgpg-error, libgcrypt, libassuan, libksba and npth — five packages, and every one of them then sits on the kernel-update path. GnuPG 1.4 is self-contained but unmaintained. Neither is a good thing for this to depend on when the cryptography we actually need is already linked.

**Compute the checksum from our own download and record where it came from.** Cheapest, and honest about its own provenance. Rejected because it defends against nothing: the tarball and the checksum come from the same origin over the same channel, so an origin able to forge one can forge both. The signature is what makes the checksum independent of the transport.

**Verify over HTTPS alone and skip the signature.** The same objection. Certificate verification authenticates the CDN, and the CDN is precisely what a published signature exists to be independent of.
