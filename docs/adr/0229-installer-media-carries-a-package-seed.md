# ADR-0229: Installer media carries a package seed, so a fresh box can name the network

- **Status:** Accepted
- **Date:** 2026-09-02
- **Issue:** [#135](https://git.home.arpa/itdlabs/cix/issues/135), [#189](https://git.home.arpa/itdlabs/cix/issues/189)

## Context

Bringing up a freshly installed host was circular:

1. A fresh install has no recipes.
2. Recipes come from `pkg sync`, which fetches from a git forge **by hostname**.
3. A hostname needs DNS.
4. This platform's DNS is `dns-1`/`dns-2` — containers built from the
   `dns` **recipe**.
5. Back to 1.

What actually happened the last time a box was brought up: 298 package
and 20 image recipes pushed one at a time from a developer machine, the
build toolchain served over plain HTTP from a laptop by literal IP, the
artifact cache addressed by literal IP, and the repo URL temporarily
pointed at an IP — which then failed TLS verification, because the
certificate names the host. None of that is something an operator can be
asked to do.

**Both halves of the fix already existed and neither was used.**
`mkinstalleriso` accepts a seed directory and stages `<seed>/recipes`
and `<seed>/artifacts` onto the media; `cix-install` copies them into
the installed box and refuses a half-staged seed. The daemon passed
`""`, with a comment saying that staging a real seed "changes what the
media DOES ... and that is a product decision, not a repair."

This ADR is that decision.

## Decision

**An installer ISO carries a package seed: every recipe this host has,
and the artifacts a fresh box needs to reach name resolution.**

The artifact set is a short explicit list — glibc, zlib, dnsmasq — and
the reasoning for each half is different:

**Recipes: all of them.** They are small text (~14 MB), and having the
complete set is what lets the installed box build anything at all
afterwards.

**Artifacts: only enough to reach DNS.** Once `dns-1`/`dns-2` are
running, the forge resolves, `pkg sync` works, and the artifact cache is
reachable — so everything else follows from that one capability. That is
also precisely the acceptance test #135 states. It costs about **57 MB**,
of which glibc is 56.8 MB.

Staging everything the local cache holds was rejected: it would put gcc,
the kernel and the whole toolchain on the media for no bootstrap
benefit.

## Why a list and not a derivation

Deriving the artifact set — from the `dns` image's manifest, or from
registered DNS servers — was considered and rejected on two grounds.
It would silently change what the media carries whenever an unrelated
image changed; and on a host that does not itself run DNS it would
produce an **empty** artifact set, shipping media that claims a seed and
cannot bootstrap.

A list is visible in a diff, so it cannot change by accident. Same
posture as the toolchain count in `test_toolchain_policy`: the number is
not the point, being unable to change it silently is.

## Failing rather than half-delivering

A missing artifact fails the whole ISO build, naming what is missing.
Media that claims a seed and cannot bring up DNS is worse than media
carrying none, because nothing reveals it until the installed box is
already standing there unable to resolve anything — the same
silent-success class as an assembly reporting success, a `201 running`
that only proved `execve()`, and a loader file standing in for a C
library. `cix-install` already refuses a seed whose directories are
missing; this is the same refusal one level up, where the reason is
still known and can be reported.

The seed is staged fresh on every build rather than kept, because it is
a snapshot of this host's recipes and artifacts at the moment the ISO is
made; a stale one would ship recipes that no longer match their
artifacts.

## Consequences

- Installer media grows by roughly **71 MB**.
- A freshly installed box has a package source before it has a network,
  and can build its own DNS with no external fetch.
- The seed is a **delivery mechanism, not a trust boundary**: the daemon
  verifies every artifact against its own recipe's checksum before
  installing it, exactly as it does for one fetched from the cache.
- The full acceptance test — install a box from this media, with no
  external DNS configured, and reach a working `dns-1`/`dns-2` — needs a
  real install and is not covered by building the ISO. What is verified
  here is that the media carries the seed and that a build refuses
  rather than shipping an incomplete one.
