# Public keys

The public halves of the keys Cix signs things with. Everything here is
meant to be copied out, published, and pinned — that is the whole job of
this directory. No private key is ever kept here, or anywhere else in
this repository.

| File | What it verifies | Held by |
|---|---|---|
| [`cix-release.pub`](cix-release.pub) | Installer ISOs published to the artifact cache | The one host that cuts release media ([ADR-0220](../adr/0220-a-separate-release-signing-key.md)) |

## Verifying an installer ISO

```
curl -fsSLO http://<cache>:8080/cix-installer-<version>-<release>-<arch>.iso
curl -fsSLO http://<cache>:8080/cix-installer-<version>-<release>-<arch>.iso.minisig
minisign -Vm cix-installer-<version>-<release>-<arch>.iso -p cix-release.pub
```

A good result names the release it is vouching for, because the version
is inside the signed trusted comment rather than only in the filename:

```
Signature and comment signature verified
Trusted comment: cix installer iso, v2.5.1
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

Take a copy of `cix-release.pub` **once**, from a source you trust, and
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
