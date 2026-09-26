# 0313 — the cix release recipes retire with their artifacts

## Status

Accepted. Amends [ADR-0309](0309-shell-recipes-are-history-the-shell-path-retires-with-its-last-dependent.md) **for the `cix` package only**. ADR-0309's decision that the 1,502 published `.sh` recipes stay stands for every other package, unchanged and for its original reasons.

## Context

[ADR-0312](0312-the-version-is-0-2-x-and-the-release-is-the-counter.md) renumbered releases to `0.2.57-<release>` and retired the `v2.57` line from the recipe **corpus**. It deliberately left the box's recipe **store** alone, and said so: 611 cix recipe versions were still there, 568 of them shell, and removing them collided with ADR-0309.

Then the owner chose a clean cutoff for the artifacts — *"I do not want any crap littering where we are"* — and **404 pre-renumber artifacts were deleted** (390 `cix` tarballs, 14 `cix-installer` ISOs, each with its signature; 1,698 MiB). That changed the facts this question rests on.

## Decision

**Every cix recipe version outside the `0.x` line is removed from the recipe store.** 610 of 613; the three kept are `0.2.57-358`, `0.2.57-359` and `0.2.57-360`.

The files themselves are already in `trash/` in the recipes repository (ADR-0312), and `test_versioning` already refuses any cix recipe outside `0.x` in the corpus, so nothing re-adds them.

## Why ADR-0309's reasoning no longer holds here

ADR-0309 gave four reasons to keep published `.sh` recipes. Taken against the cix recipes specifically, after the artifact deletion:

| ADR-0309's reason | Status for `cix` |
|---|---|
| `pkg sync` is merge-only, so deleting from git removes nothing from a host | About git, and true — which is why it never argued against `pkg recipe rm`. It says deleting the *files* is insufficient, not that removing them from the *store* is wrong. |
| each carries the artifact approval that makes its cached bytes trustworthy | **Void.** Those cached bytes no longer exist. An approval for an absent artifact approves nothing. |
| 67 of 179 installed versions are shell-built | Unaffected. `pkg_entry_drift()` returns 0 when an installed package has no recipe, so both installed cix entries keep their files and report no drift. |
| `cix@*.sh` is the platform's own release record | The 782 git tags are that record, and the recipe text is preserved in `trash/`. Nothing is lost that is not also somewhere else. |

The second row is the whole of this ADR. The reason was sound when written and the artifacts were deleted afterwards, so it expired rather than being overruled.

## What this fixes

`pkg_version_compare()` is natural sort, so every `v2.x` recipe out-ranks `0.2.57` on the first character. `find_recipe_path()`'s own comment names the highest-ordered version as the rolling-implicit default for plain `pkg install`, dependency resolution, hostbuild and update checks. Measured on 192.168.15.95 before this change:

```
cix  __hostbuild  0.2.57-360   installed  available=v2.57.358-1
cix  base         v2.57.167    installed  available=v2.57.358-1
```

The daemon named an upgrade to a version whose artifact had just been deleted. That closes cix#532's remaining half.

## Consequences

**No pre-renumber cix release can be installed or rebuilt from the box.** Its artifact is gone and now its recipe is too. The source remains in the git tags, and the recipe text in `trash/`, so a determined reconstruction is possible; a routine one is not.

**This is deliberately narrow.** Only `cix`. Every other package keeps its full recipe history, because no other package was renumbered and none of them lost its artifacts. Generalising this would be exactly the arbitrary act ADR-0309 exists to prevent.

**`test_versioning` already gates the corpus**, so the store and the corpus cannot drift apart again in the direction that caused this.

## Note on execution

The removal was 610 API calls and was refused by Claude Code's own safety classifier, correctly — a bulk delete loop against a live host is what that brake is for. It was carried out by the owner from a reviewed script with a dry-run default and a guard that aborts if a `0.x` version reaches the delete list. Recorded because the same shape will recur, and splitting a denied batch into smaller pieces is not the answer.
