# 0312 — the version is `0.2.x`, the release is the counter, and the `v2.57` line is retired

## Status

Accepted. Supersedes nothing: the scheme it replaces was never written down anywhere, which is a large part of why it drifted.

## Context

Cix released as `v2.57.358`: a major of 2, a minor of 57, and a third
component that was really a build counter incrementing once per
release. Nothing chose that shape. It accreted — the major and minor
had climbed for years without ever marking a compatibility boundary,
because there has never been a stable interface to break. A platform
that has not shipped a 1.0 was numbering itself as though it had
shipped two.

The owner's instruction, 2026-09-26: *"I think it's 0.2.57 release 358,
right? But if this is the case, I want this plastered on CLAUDE.md and
our readme files and our build files so that we don't mix things up,
and we want consistency moving forwards, I want to stop exaggerating
with versioning."*

## Decision

**A Cix release is `<version>-<release>`, where the version is `0.2.57`
and the release is the counter.** The next release is `0.2.57-359`,
then `0.2.57-360`. The version moves only when something about the
platform's maturity genuinely moves; the release moves every time.

**One string, everywhere.** The git tag, `CIX_BUILD_VERSION`, the
recipe identity and the artifact name are all `0.2.57-358` — no `v`
prefix, no second rendering:

| Where | Value |
|---|---|
| git tag | `0.2.57-358` |
| recipe | `version "0.2.57"` + `release 358` |
| `CIX_VERSION` passed to `make` | `${version}-${release}` |
| `CIX_BUILD_VERSION` / `cixctl boot` / `BUILD_ID` | `0.2.57-358` |
| artifact | `cix-0.2.57-358-x86_64.cixpkg` |

The `${version}`/`${release}` split is not new machinery: every one of
the 477 CPDL recipes already has it, and the old scheme was the odd one
out — it put the whole thing in `version` and left `release` pinned at
1 forever. This makes cix an ordinary package.

**The `v2.57` recipe line is retired from the CORPUS**, so nothing re-adds it and no future cix recipe may sit outside the `0.x` line. Removing the 611 copies already in the box's recipe store is a separate, larger act -- see Consequences.

## Why the old line had to go

`pkg_version_compare()` is dpkg-style natural sort (ADR-0107). It walks
both strings comparing digit runs numerically and other runs bytewise.
`0.2.57` against `v2.57.358` is decided on the very first character:
`'0'` is a digit and `'v'` is not, so the byte comparison runs, and
`0x30 < 0x76`. **Every new release ranks older than every old one.**

That is not a cosmetic ordering question. `find_recipe_path()`'s own comment
names the blast radius exactly: the highest `pkg_version_compare()`
version is *"the 'rolling implicit' default every pre-existing,
non-manifest-aware caller (plain `pkg install NAME`, dependency
resolution, hostbuild, update-candidate checks) relies on."* Renumber
without addressing it and a version-less `pkg install cix` resolves to
`v2.57.358` forever — silently, and correctly by its own logic.

Retiring the line removes the comparison instead of special-casing it.
Once no `v2.57.*` recipe is in the store, the highest cix recipe is the
newest cix recipe, with no code change at all.

## Alternatives considered

**A dpkg epoch (`1:0.2.57`).** This is the mechanism that exists for
precisely this situation, and it was the initial recommendation. It was
withdrawn on a measurement: the artifact cache answers **HTTP 400** to a
colon in an artifact name, where the same name without one answers 404.
So the epoch cannot reach the artifact, and the benefit most often cited
for it here — that the cache would rank releases correctly — was never
available either, since `version_rank` is the cache's own code and not
reachable from this repository.

**An epoch with a filename-safe separator** (`1~0.2.57`). Works
in-repo, and invents a dialect CBS does not parse for a boundary
crossed exactly once.

**A real `epoch` field in CPDL.** Correct and permanent, and the right
answer if this recurs. It needs an upstream CBS language change, a
cixd change and its own ADR. Disproportionate to a one-time renumber,
and it would not have shipped in this release.

**Keeping the old recipes and always passing `--version=`.** Rejected
against the stated requirement of no disruption: it leaves every
version-less path silently resolving to the abandoned line, and arms
the same trap for #508's automated rollout.

## Consequences

**Retirement has two halves and they are not the same act.** This is
the CLAUDE.md distinction — `pkg recipe rm` edits the store, git edits
what refills it — and it decides what is actually given up:

- **The git half, done here.** The 37 `cix@v2.57.*.cbs` files move to
  `trash/`, so nothing re-adds them and `test_versioning` keeps them
  out. The box's store is untouched by this, so **every old release
  stays installable**: its recipe is still there, carrying the artifact
  approval that makes its cached bytes trustworthy.
- **The store half, NOT done here.** Removing them from the store is
  what actually makes `0.2.57` the highest cix recipe, and it is
  larger and costlier than the git half by an order of magnitude.

**Measured on 192.168.15.95, 2026-09-26, and it is why the store half
is deferred rather than done:** the store holds **611** cix recipe
versions, not 37 — **568 shell and 43 CPDL** — because it has
accumulated every cix release since `v1.4.0`. Two consequences:

1. Removing a recipe from the store removes that version's artifact
   approval, so that release becomes **uninstallable**, not merely
   unrebuildable. An earlier draft of this ADR claimed only
   rebuild-from-recipe was lost. That was wrong, and it was wrong
   because it reasoned about the git half and described the store half.
2. 568 of the 611 are shell recipes, which [ADR-0309](0309-shell-recipes-are-history-the-shell-path-retires-with-its-last-dependent.md)
   explicitly decided must stay — *"the 1,502 published `.sh` files
   STAY ... each carries the artifact approval that makes its cached
   bytes trustworthy ... `cix@*.sh` is the platform's own release
   record."* Removing them is a reversal of an accepted decision and
   belongs to the owner, not to this change.

**So the trap is live on the box and is recorded rather than worked
around.** `pkg ls` reports `cix base v2.57.167 installed` with
`available v2.57.358-1`: the daemon's `recipe_latest_version("cix")`
is still the retired line. What that costs is a wrong "available"
column on the cix package and a version-less `pkg install cix`
resolving to the retired line. What it does not cost is this release:
a hostbuild names its version explicitly, the selftest reads the
pinned corpus (which is clean), and nothing auto-installs cix.

**The 390 artifacts are untouched either way**, so every previous
release stays downloadable and bootable, and the A/B slots and the
installer both consume artifacts rather than recipes.

**The running host is unaffected.** `pkg_entry_drift()` returns 0 when
an installed package has no recipe, so the installed `v2.57.358` keeps
working and reports the new release as available once it is published.

**Boot is unaffected, which is worth stating because it is the
frightening case.** A/B boot entry ordering does not read the release
string at all: the entry's `version` directive is `(long)time(NULL)`, a
timestamp, under a fixed `sort-key cix`. A renumber cannot make a box
boot the wrong slot.

**The cache's listing will rank new releases below old ones** until
cix-cache learns the new scheme, because its `version_rank` applies the
same natural sort. This is display ordering in that service's own API
and affects nothing Cix decides. Tracked in its tracker, not worked
around here.

**`test_versioning` gates the scheme**, in SELFTESTS. Prose does not
hold a numbering convention any better than it held a naming one — the
same argument ADR-0224 makes for counting gcc recipes and ADR-0309's
naming gate makes for CBS.

## The general rule this is an instance of

A version number is a claim about a system, and this project already
requires a claim to be true (CLAUDE.md, "Never state anything about
this environment as fact without the command that produced it"). `2.57`
was a claim of two major generations of stable interface that had never
existed. `0.2` says pre-1.0, which is what is true.
