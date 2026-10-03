# 0326 — Upstream keys travel in the signed catalogue

## Status

Accepted, 2026-10-03, the owner's answer when asked how the kernel's signing key should reach a host: *"keys in the catalogue"*. It amends ADR-0318, whose upstream key store took keys only by an operator's API call. That store and its rules otherwise stand, as ADR-0323 kept them.

## Context

ADR-0323 rung 2 authenticates a release from a signed checksum list, verified under a key installed for that one package. As built in 0.2.57-458, the only way to install that key was `cixctl pkg upstream-keys add` on each host. The owner asked whether that was friction, and it was. A step that every host's operator must repeat for every upstream key does not scale past one box, and it sat between kernel.org publishing a release and every Cix host rolling to it.

The trust decision itself, that a given fingerprint is the kernel.org checksum autosigner, is made once, by a person. Where that decision is recorded is what ADR-0324 already answers for artifact keys: in the catalogue the owner signs, which every host trusting that source follows.

## Decision

**An upstream key is a file in the catalogue of the source that owns the package, and a host adopts it at sync.**

- **Layout:** `recipes/keys/<package>@<FINGERPRINT>.asc`, flat like recipes (ADR-0308).
- **Coverage:** `recipes/keys` is in the signed index (`recipes/INDEX`, ADR-0324 step C). A signed source yields only the key files its index lists.
- **Which sources:** keys are taken only from a source the host trusts for keys (`trust_keys`, ADR-0324). They are taken only for packages that source owns, so a source cannot vouch for another source's package.
- **Which keys:** a key is adopted only when its own v4 fingerprint is the one in its file name. Anything else is refused and logged.
- **Replacement:** each sync replaces the keys a source supplied with what it offers now.
  - A key removed from git leaves every host at its next sync. This is the revocation path.
  - Artifact keys deliberately work the other way (merge only, ADR-0324), because dropping one would un-approve artifacts already built. An upstream key approves nothing already built; it only authenticates future releases. So removal is safe, and it is wanted.
  - A source that is refused, or whose archive could not be fetched, keeps what it supplied before. A source an operator removes takes its keys with it.
- **The operator call stays,** for a key that is the host's own. A key the catalogue supplies cannot be replaced or removed by it (409), because the next sync would restore it.
- **Nothing fetches a key.** Trust still begins with a person: the owner commits the key file, having checked the fingerprint out of band. That commit is the out-of-band act ADR-0318 asked for, made once instead of once per host.

## Consequences

- The kernel's key reaches every host trusting cix-recipes with no per-host step. The same holds for any later rung-2 or rung-1 package.
- A host's own private source can carry keys for its own packages the same way.
- Compromising the catalogue lets an attacker add an upstream key, just as it already lets them add an artifact key. The signed index (step C) is what closes that, once the owner's catalogue key exists.
