# 0212 — A Secure Boot signing key pair reaches a host over REST, because there was never another way in

## Status

Accepted. Supersedes the *transport* half of
[ADR-0064](0064-rest-driven-iso-assembly.md); its security reasoning is
kept intact and restated below.

## Context

`POST /v1/system/iso` (ADR-0064) assembles an installer ISO on the host,
from the `cix`, `kernel` and `isotools` hostbuild artifacts plus a Secure
Boot signing key pair at `SIGNING_KEYS_DIR` (`<data-dir>/keys/`). The first
three are found automatically. The fourth was declared an
operator-populated-out-of-band precondition, and ADR-0064 defended that
choice at length and correctly:

> Making `cixd` itself generate, fetch, or auto-copy a release-signing
> private key onto every box that happens to run it would be a genuine
> security regression — any single deployed box's compromise would leak
> the fleet's own Secure Boot key.

That reasoning is right and this ADR does not touch it. What it got wrong
was one sentence about mechanism — that this is

> exactly like `pkg hostbuild cix` already requires a real git token
> nobody but the operator can supply, or `pkg bootstrap --toolchain=`
> requires a real pre-built artifact already `scp`'d onto the box.

The git-token half of that comparison holds: a token really is supplied by
the operator, over REST, via `PUT /v1/pkg/repo-config`. The `scp` half does
not. **A real Cix host has no transport that can put a file in that
directory at all.**

Established directly, not assumed:

- `ssh root@192.168.15.95` is refused — Cix hosts run no sshd.
- Every `/v1/system/*` route was enumerated; none writes to `STATE_DIR/keys`,
  and none executes anything host-side. `boot-console` is GET/PUT
  configuration, not a shell.
- The box has no console.

So the precondition could never be satisfied, and **`POST /v1/system/iso`
has never been servable on any real installed Cix host** — not because of a
bug, but because the door it documented does not exist. The endpoint, its
CLI command, its ADR and its guide have all been shipped and none of them
could run. That is the actual defect: a required input with no supported
way to supply it.

## Decision

`SIGNING_KEYS_DIR` gains a REST surface, `GET`/`PUT`/`DELETE
/v1/system/signing-keys`, and the operator installs the pair by pasting two
PEM blocks.

**The security property ADR-0064 protects is preserved exactly.** cixd still
never generates, fetches, or copies this key on its own initiative. Nothing
propagates it: installing it stays a deliberate operator act against the one
host chosen to cut media, and only the public DER `.cer` is ever staged onto
an installed target, for MOK enrolment. What changes is that the deliberate
act now has a way to happen.

**The private key goes in and never comes back.** `GET` reports `key_set`
and `cert_set` booleans plus the certificate's own public identity —
subject, expiry, SHA-256 fingerprint — and no endpoint, error message or log
line ever emits key material. This is not a new pattern here:
`pkg_repo_write_json_config()` and `pkg_artifact_write_json_config()` both
already report `auth_token_set` rather than the token they hold. Nor is
holding a private key new — `pki.c` has always generated and stored
`ca.key`, `intermediate.key` and per-certificate keys under `STATE_DIR/pki`.

**Two PEM blocks in, three files out.** The DER `.cer` that `mokutil`
consumes is derived here from the certificate rather than accepted as a
third input. Accepting it separately would only create a way for the two to
disagree, and the failure that produces is an enrolled MOK identity that
does not match what actually signed the image.

**The pair is verified before anything is written.** `openssl pkey -pubout`
and `openssl x509 -pubkey -noout` emit byte-identical PEM for the same key,
so an exact comparison is the whole check. This is not defensive padding:
pasting two halves that do not belong together is the realistic mistake a
copy-paste interface enables, both blobs are individually valid, and the
consequence would otherwise surface much later as an image that signs
cleanly and then refuses to boot. The write itself is atomic — temporaries
renamed into place — so a rejected or interrupted `PUT` leaves the previous
pair intact rather than half-replaced on a host that may be actively using
it.

**Removal is supported.** A release host being decommissioned or repurposed
should not keep signing material it no longer needs, and without `DELETE`
the only way to remove it would be to reinstall the box.

## Alternatives considered

**Generate the pair on the box** (`POST .../signing-keys/generate`). The
strongest option on transport grounds — the private key never crosses the
network at all — and it was the owner's first preference. Rejected for this
change because it forces a *new* signing identity: every machine that has
enrolled the existing certificate as a MOK would need re-enrolment, and an
import path is needed anyway for continuity, migration between release
hosts, and disaster recovery. Generation remains a reasonable later
addition on top of this; it is not an alternative to it.

**Leave ADR-0064 alone and cut ISOs on the development machine.** This is
the status quo and it does work — `image/keys/` exists in this repo for
exactly that reason, and every ISO to date was built by invoking
`mkinstalleriso` there directly. It was rejected because it permanently
strands a shipped endpoint, its CLI command and its guide in an unusable
state, and because it puts ISO assembly outside the API-First mandate for
no reason other than a missing file-transfer mechanism.

**Reuse the platform PKI to issue the signing certificate.** Technically
possible and rejected on hygiene: it conflates the internal TLS trust root
with the Secure Boot signing identity, two things with different lifetimes,
different blast radii and different holders. A MOK also wants a self-signed
certificate, which is not what that CA issues.

## Consequences

The transport is only as private as the API it travels over. On a host with
`https_enabled: false` the pasted key crosses the management LAN in clear
text, and with no authentication configured any party on that network can
replace it. That was accepted deliberately for the current deployment
(a trusted management LAN), and it is a property of the API's exposure
rather than of this endpoint — but it is the reason to enable HTTPS and
host authentication before treating any host that holds signing material as
production. This is recorded here rather than left implicit precisely
because ADR-0064's whole concern was where this key can end up.
