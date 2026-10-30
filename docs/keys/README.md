# Public keys

Two kinds of key live here, and the difference matters.

**Keys Cix signs with** — the public halves, meant to be copied out,
published and pinned by anyone verifying a Cix artifact. No private key
is ever kept here, or anywhere else in this repository.

| File | What it verifies | Status |
|---|---|---|
| [`cix-release-2026-09.pub`](cix-release-2026-09.pub) | Installer ISOs published to the artifact cache, from 2026-09-06 onward. Key id `0b19db46b2b6db6c` | **Current** — held by the one host that cuts release media ([ADR-0220](../adr/0220-a-separate-release-signing-key.md)) |
| [`cix-release.pub`](cix-release.pub) | Installer ISOs published **before** 2026-09-06 | **Retired** — kept, never deleted; see below |
| [`cix-secureboot.crt`](cix-secureboot.crt) | The EFI binaries on installer media (shim/GRUB/kernel), and the MOK certificate an operator enrolls to boot them. `CN = Cix Secure Boot Signing`, SHA-256 fingerprint `D6:3F:94:CA:B7:DD:72:FF:2E:56:D3:FB:67:5D:08:FC:83:EC:68:DC:E2:FF:D7:D5:77:ED:D4:02:B8:CC:93:75` | Current |
| [`cix-secureboot-2026-08.crt`](cix-secureboot-2026-08.crt) | The EFI binaries on installer media built before the 2026-09-06 rotation, and the MOK certificate installs of that time enrolled. SHA-256 fingerprint `87:D4:C9:86:6B:80:ED:AD:08:08:6E:42:BC:80:88:1F:F1:31:F7:11:A7:1E:47:43:7C:DC:FB:BE:A8:5E:9E:E0` | **Retired** — kept so that what it signed stays checkable. It sat in `image/keys/cix-signing.crt` until 2026-10-30, where the guides still pointed at it as current (#628) |

### Why the release key was rotated on 2026-09-06

Not a scheduled rotation and not a compromise: **the private half was lost.** 192.168.15.95 was reinstalled, both private keys lived only in that box's `STATE_DIR/keys`, and nothing held a copy off-box — `GET /v1/system/release-key` and `GET /v1/system/signing-keys` both returned `key_set: false` afterwards. There was nothing to recover, so a new key was generated and the old one retired.

**Everything already published stays verifiable**, which is the whole reason `cix-release.pub` is still here: its public half was committed to this repository, so it survived the loss of its private half. Any ISO signed before this date verifies against it exactly as it always did. What is gone is the ability to sign *new* artifacts with that identity.

The lesson is the one this directory's design already assumed and the operational practice did not follow: a public key in git survives anything that happens to a host, and a private key that exists in exactly one place is one reinstall from gone.

**Keys Cix verifies against** — upstream publishers' keys — do not live
here. They travel in the signed recipe catalogue, as
`recipes/keys/<package>@<FINGERPRINT>.asc` in the source that owns the
package, and a host adopts one only when the key's own fingerprint is
its name ([ADR-0326](../adr/0326-upstream-keys-travel-in-the-signed-catalogue.md)).
kernel.org's autosigner key is `recipes/keys/kernel@B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1.asc`
in cix-recipes. A copy of it was kept here as `kernel.org-autosigner.asc`
until 2026-10-30, read by nothing after ADR-0326, and removed at the
owner's decision (#628) so that one key has one home.

## Verifying an installer ISO

```
curl -fsSLO http://<cache>:8080/cix-installer-<version>-<release>-<arch>.iso
curl -fsSLO http://<cache>:8080/cix-installer-<version>-<release>-<arch>.iso.minisig
minisign -Vm cix-installer-<version>-<release>-<arch>.iso -p cix-release-2026-09.pub
```

A good result names the release it is vouching for, because the version
is inside the signed trusted comment rather than only in the filename:

```
Signature and comment signature verified
Trusted comment: cix installer iso, <version>-<release>
```

Use **stock `minisign`**, not a Cix tool. An installer that checks its
own signature is the code being checked doing the checking, and a
substituted ISO either reports success or never implements the check at
all. The point of this signature is that a stranger with no Cix software
can still tell a genuine installer from a fabricated one — so the last
step deliberately does not belong to this project.

Verify **before** writing the USB stick, on a machine you already trust.
A check performed after booting the thing you were checking has already
lost.

## Why the key lives in git

Deliberately not in the artifact cache, and this is the only property
that makes any of it worth doing: the cache serves the bytes, so a cache
that also served the key would be vouching for its own payload. Git is a
different system, with different credentials, reached over a different
path — an attacker who can replace an ISO in the cache still cannot
replace the key you checked it against.

That is also why **no per-ISO checksum belongs here.** A checksum
committed next to the recipes travels the same path as the recipes and
proves nothing the recipe did not already claim; the signature reaches a
verifier by a path the bytes never took. Only long-lived public keys go
in this directory.

## Pinning

Take a copy of `cix-release-2026-09.pub` **once**, from a source you trust, and
keep it. Re-fetching it before every verification defeats the exercise:
whoever could hand you a bad ISO could hand you the key that matches it.

Signatures here do not expire and there is no revocation lookup, on
purpose. An ISO from two years ago has to verify on a laptop with no
route to anything, and a control that can strand an operator during an
outage is a control that gets worked around. If a release key is ever
retired, that is announced by publishing a new key here — an ordinary,
reviewable commit — not by a network call at verification time.

## If the key changes

A new key is a new file and a new row in the table above, with the old
one kept rather than deleted: artifacts signed by it stay verifiable,
and silently removing the key that verifies a published artifact would
turn a good ISO into an unverifiable one.
