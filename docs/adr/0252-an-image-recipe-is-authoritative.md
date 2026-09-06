# 0252 — An image recipe is authoritative; the image is derived from it

## Status

Accepted

## Context

`cix-builder` 5.0.0 named eleven packages. The live image held twenty.
The difference was not cosmetic: the recipe pinned `tcc` at `0.9.27-9`,
nine revisions and one compiler upgrade behind the `0.9.28rc-29` the box
was actually building with. That image is gone, and the recipe was only
recoverable because a `GET /v1/system/backup` bundle had been taken
before the box was reinstalled.

6.0.0's header says the recipe "is now the only place it exists". 5.0.0's
header said the same thing about 3.0.0. The drift came back both times,
and the reason is structural rather than careless: **an image can be
changed by two different mechanisms.** `pkg install --image=X` mutates
the live image directly; the recipe describes what the image should be.
Neither is subordinate to the other, so they diverge, and nothing
notices.

It was still diverging while this ADR was being written. Measured on
192.168.15.95 immediately after materializing the image from its own
6.0.0 recipe: `zlib` is installed at `1.3.2-10` where the recipe pins
`1.3.2-9`. One package out of twenty-four, on an image built from its
recipe minutes earlier.

The obvious response — compare the recipe to the live manifest and report
the difference — was considered and rejected. A comparator between two
sources concedes that both are authoritative; it detects divergence
instead of preventing it, and it is a second mechanism enforcing a rule
that a single mechanism could make unbreakable. That is the shape
[ADR-0251](0251-a-package-artifact-carries-what-the-platform-runs.md) had
just finished removing from package contents.

## Decision

**An image recipe is authoritative. An image is derived from its recipe,
and there is no other way to change one.**

The recipe is the single source of truth for image membership. The live
image is a materialization of it, exactly as a package artifact is a
materialization of a package recipe.

What follows, and what has to be true for this to hold:

- **Installing into an image that has a recipe goes through the recipe.**
  A direct `pkg install --image=X` for a recipe-backed image is not a
  second path to the same outcome; it is the mechanism that produced
  every drift above.
- **An image without a recipe is unchanged by this.** Not every image is
  recipe-backed today, and this decision does not retroactively require
  one.
- **Drift becomes unrepresentable rather than detected.** There is no
  comparator, because there is nothing to compare: the manifest is
  whatever the recipe said.

## Consequences

- The recipe's pins become load-bearing rather than documentary. `zlib`
  at `1.3.2-10` against a `1.3.2-9` pin is a bug in the recipe or in the
  materialization, and one of the two must move — it can no longer be
  both.
- Operating a box changes: adding a package to an image is a recipe
  change, published like any other. That is the cost, and it is the point
  — it is what makes the image reproducible from text, which is what
  failed when `cix-builder`'s image was lost.
- **This does not by itself fix the pinning gap this decision exposes.**
  Dependency resolution deliberately ignores pins transitively (see
  `PkgInstallRequest.version`: "pinning applies only to the single package
  actually being installed, never transitively to its build-time
  prerequisites"). An image recipe that pins twenty-four packages
  therefore cannot guarantee those twenty-four versions today, because a
  dependency pulled in by any of them resolves to its own highest
  revision. That is a real gap between what a recipe now claims and what
  the installer can deliver, and it is recorded here rather than assumed
  away.
- Recorded, not yet implemented: this ADR is the decision. The daemon
  still permits a direct install into a recipe-backed image at the time
  of writing.

## Alternatives considered

**Image authoritative, recipe generated from the live manifest.** Also a
single source, and cheaper — nothing to enforce, the recipe simply
reports. Rejected by the owner: it makes the image the thing you must not
lose, which is exactly the position that made `cix-builder` recoverable
only from a backup bundle.

**Compare the two and report drift.** Rejected above: a comparator is a
second mechanism for one rule, and it detects what a single mechanism can
prevent.
