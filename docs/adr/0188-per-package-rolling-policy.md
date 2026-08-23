# 0188 — Per-package rolling policy: highest, newest, or pinned

## Status

Accepted

Extends [ADR-0107](0107-image-versioning.md), which established version-keyed recipes and the highest-version resolution rule. That rule stays the default; this makes it one of three.

## Context

Every omitted-version resolution in this daemon — a plain `pkg install NAME`, dependency resolution, `pkg hostbuild`, update-candidate checks, a `follow_rolling` image's automatic rebuild, the "available" column in the dashboard — resolves through one function, and that function has always applied one rule: dpkg-style highest version wins.

For a rolling-release platform that is the right default, and it stayed right for a long time. The Part 201 toolchain work broke it. Building a compiler bootstrap chain meant publishing gcc `4.7.4` and `6.4.0` *after* `16.2.0` already existed, so for `gcc` the highest version (`16.2.0-5`) and the newest published (`6.4.0-5`) are different recipes — and neither is what an operator walking that chain actually wants installed. They want one specific link of the chain, and they want it to stay there.

"Pinned" only half-existed: an explicit-version install held until the next `update-all` or rolling rebuild resolved implicitly again and bumped it. There was no hold.

## Decision

**Three policies, per package name, resolved in the one function every implicit resolution already goes through.**

- **`highest`** — unchanged, and the default. No policy set and the default policy are the same state, not two states to keep in step.
- **`newest`** — the most recently *published* recipe wins. A recipe version's file is written exactly once and never rewritten (ADR-0107's immutability rule), so its mtime is a real first-published timestamp rather than an approximation; the codebase already relied on this for display. Ties fall back to version order, so the answer is stable rather than dependent on `readdir()` order.
- **`pinned`** — an explicit version nothing may bump. This needs no special-casing in `update-all` or `follow_rolling`: they resolve through the same function, get the pinned version back, and therefore find no update to apply. A hold that works by *resolution* cannot be forgotten by a caller the way a hold implemented as a check in each caller can.

**Policy is operator state, never recipe content.** A recipe cannot know which link of a bootstrap chain a particular box is meant to sit on; two boxes syncing the same catalogue can legitimately want different links.

**Two refusals, both about not lying:**

- A `pinned` policy with no version is a 400. It would claim to hold something while resolving as `highest` — exactly the drift a pin exists to prevent.
- A pinned version that is not published makes the package unresolvable, rather than falling back to another version. Silently resolving somewhere else is the failure a pin is bought to avoid, and it would be invisible.

## Alternatives considered

**Express the policy in the recipe.** Rejected: it is not a property of the software, it is a property of this box's intent. It would also make every consumer of a shared catalogue inherit one box's bootstrap-chain position.

**A global policy switch rather than per-package.** Rejected: the case that motivated this is one package (`gcc`) behaving unusually inside an otherwise ordinary rolling catalogue. A global switch would drag every other package off the default to fix one.

**Implement `pinned` as a check inside `update-all` and the rolling rebuild.** Rejected — that is the shape the half-existing pin already had, and it failed for the obvious reason: any resolution path that forgets the check silently bumps a pinned package. Resolving through one function makes the hold structural.

## Consequences

`update-all` and `follow_rolling` need no knowledge of policies at all; they keep asking the same question and get a policy-aware answer.

A pinned package with an unpublished pin fails loudly at resolution. That is intended, and the error names the package — an operator who pinned to a version they then deleted has a real problem, and being told about it is the point.

`newest` depends on recipe-file mtime being a true publication time. That holds because recipe files are written once and never touched (ADR-0107); anything that ever rewrites a recipe version in place would break this policy and the immutability rule together, which is why issue #59's re-fetch escape hatch *deletes and re-adds* rather than rewriting.
