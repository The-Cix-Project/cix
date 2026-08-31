# 0219 — Versioning and release tags

## Status

Accepted. Records a convention that was never written down, drifted, and
was caught only because the owner asked whether the current behaviour was
deliberate.

## Context

Cix tags releases `vMAJOR.MINOR.PATCH`. It used that plainly for a long
time — `v2.0.0`, `v2.0.1`, then eight `v2.1.x` releases — with **no
release-candidate tags at all**.

`v2.2.0` broke the pattern. Thirty-four `v2.2.0-rcN` tags were cut, and
they did not stop when `v2.2.0` shipped: `rc31` through `rc35` all
postdate the release they claim to be candidates for.

```
$ git merge-base --is-ancestor v2.2.0 v2.2.0-rc31 && echo "rc31 comes AFTER the release"
rc31 comes AFTER the release
```

A release candidate that postdates its release is a contradiction. It
also reads backwards to anything following SemVer, where a pre-release
sorts **before** its release — so an installed host reporting
`v2.2.0-rc35` was announcing a version that a SemVer-aware reader ranks
as *older* than a release it actually contained.

### Why nobody noticed

`pkg_version_cmp()` compares character by character with numeric runs
compared numerically, and at the point where `"v2.2.0"` ends and
`"v2.2.0-rc35"` continues, the shorter string is ranked lower.
Extracting the function and running it confirms it directly:

```
v2.2.0       vs v2.2.0-rc35   ->  FIRST is older
```

That is the opposite of SemVer, and it is why the contradiction was
invisible from inside the system: our own comparator disagreed with the
standard in exactly the direction that made the wrong tags look right.
Every internal decision that used it — which artifact is newer, which
package upgrades which — happened to come out correct, so nothing ever
failed.

This is worth stating plainly because it is the interesting part: a
convention can be wrong for a long time without anything breaking, when
the tool that would have caught it is wrong in the compensating
direction.

## Decision

**Ordinary versioning. Release candidates only precede a release, and
only when one is genuinely wanted.**

1. **`vMAJOR.MINOR.PATCH`** is the tag form. After a release ships, the
   next tag is a **new version** — a patch for fixes, a minor for
   features — never another candidate for the version already out.

2. **`-rcN` is optional and always pre-release.** If a release wants
   candidates, they are cut *before* it and stop when it ships. Nothing
   requires them: `v2.0` and `v2.1` used none, and that was fine.

3. **Deployment does not need a candidate.** The habit that produced 34
   of them was wanting a tag to deploy from. A patch bump serves that
   exactly as well and does not lie about being provisional.

4. **`pkg_version_cmp()` keeps its current behaviour, and that is now a
   stated position rather than an accident.** It ranks
   `2.2.0-rc35 > 2.2.0`, which SemVer would reverse. Changing it would
   silently re-rank every existing artifact and installed package on
   every host — a real migration for a case this decision now prevents
   from recurring. It is documented here so the disagreement is known
   rather than discovered again.

## Consequences

**The rc31–rc35 tags stay.** They are wrong, they are history, and
deleting published tags to tidy a record is worse than a record with a
visible correction in it. `v2.3.0` is the first tag under this decision.

**Anything reading versions from outside must be assumed SemVer.** The
artifact cache, a boot manager, a person — none of them share
`pkg_version_cmp()`'s reading. Keeping pre-release suffixes strictly
pre-release is what keeps internal and external readings agreeing.

**This ADR exists because the convention was implicit.** Nothing in the
repository said how to version, so a habit formed and ran for 34 tags
without anyone deciding it. That is the same shape as the index drift in
#201: the substance was fine and the rule was never written down.

## Alternatives considered

**Fix `pkg_version_cmp()` to be SemVer-correct instead.** Rejected for
now, and not on principle: it would re-rank existing artifacts and
installed packages on every host at once, which is a migration, not a
correction. The mismatch is now recorded, and correct tagging keeps it
from mattering. Worth revisiting if a real SemVer-aware consumer is ever
given authority over ordering.

**Keep cutting `-rcN` and treat the release tag as the anomaly.**
Rejected: it inverts the standard meaning of a pre-release suffix, and
it was never chosen — it is exactly the drift this ADR exists to stop.
