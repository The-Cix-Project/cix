# 0149 — unified `recipes/<kind>/` git layout, image recipes gain real version history

## Status

Accepted

## Context

Direct follow-up to task #867 (create `pkg/image-recipes/` in git, per ADR-0123's own flagged gap, and wire `pkg sync` to walk it). While implementing that, the user pointed out that `pkg/recipes/` (package build scripts) and the newly-created `pkg/image-recipes/` (declarative image package-list manifests) didn't read as one coherent system — two directories under `pkg/`, one versioned (`<name>/<version>/build.sh`) and one not (`<name>.recipe`), with no shared naming convention signaling they're two kinds of the same thing (a recipe git-syncs into a running daemon's catalog).

The user's own proposed fix — `recipes/<image|package>/<name>/<version>/build.sh`, one top-level directory split by kind, identical shape either side — directly touches a real, previously-made decision: ADR-0123 deliberately made image recipes **name-keyed, no version-keying of its own**, reasoning that an image's actual build result is already content-addressed independently (ADR-0108), so a second, competing version scheme on top would be a second source of truth for the same fact. Presented that tension back to the user rather than silently picking a side (`AskUserQuestion`): keep package recipes versioned and leave image recipes flat (`recipes/image/<name>.recipe`), or version both the same way. The user chose to version both.

That choice is reconcilable, not a reversal of ADR-0123, once "version" is read as applying to two genuinely different things:

- **A package recipe's version** names a real released version of the upstream package being built (`curl/8.21.0`) — multiple versions are simultaneously real and simultaneously useful (a running container can be pinned to any of them).
- **An image recipe's version**, under this ADR, names a revision of the *recipe's own declared content* — its package list changed, so it's a new version of the manifest, the same way any git-tracked text file has a meaningful history. This has never conflicted with ADR-0108: ADR-0108's content hash is a property of a *built rootfs*, computed from what actually got installed; this new version number is a property of the *recipe source* that declares intent before any build happens. Two different axes, not a duplicate one.

## Decision

**New git layout**, replacing `pkg/recipes/` and the (never-committed) `pkg/image-recipes/`:

```
recipes/package/<name>/<version>/build.sh   -- unchanged content/format (ADR-0107/0120)
recipes/image/<name>/<version>/build.sh     -- unchanged content/format (ADR-0123): image_packages="...", optional image_artifact_sha256=
```

The image side's `build.sh` filename matches the package side purely for a uniform, predictable layout an author or `pkg sync` can walk identically regardless of kind — it is still a plain `key="value"` declaration file, never a shell script with `pkg_build()`/`pkg_install()` functions, and `parse_image_recipe_buf()` is unchanged.

**The daemon-side API contract is unchanged, on purpose.** `image_recipe_add()`/`GET`/`DELETE /v1/images/recipes/{name}` still take no version — one current manifest per image name, exactly as ADR-0123 decided, because there is still no real use case for "run this image pinned to an old recipe revision" the way there is for a package. Only the *git source of truth* gained history; the *runtime* did not gain a new axis of state to manage.

**`pkg sync` (ADR-0121) extended to walk both trees**, `daemon/src/pkg.c`'s `pkg_sync_completed()`:

- `recipes/package/<name>/<version>/build.sh` → `pkg_recipe_add(name, content)`, unchanged walking logic, just the root path moved from `pkg/recipes` to `recipes/package`.
- `recipes/image/<name>/<version>/build.sh` → new `sync_walk_image_recipes()`: for each image name directory, picks the **highest** version by `pkg_version_compare()` (the identical selection rule `find_recipe_path()` already uses for an unpinned `pkg install NAME`) and publishes only that one via `image_recipe_add(name, content)`. Lower versions in git are real history, browsable and revertible by hand, but never separately reachable through the daemon's own unversioned API — consistent with the decision above.
- Because `image_recipe_add()` always overwrites (name-keyed, no duplicate-rejection the way immutable package recipe versions have), every successful image-recipe sync counts as "added" in `GET /v1/pkg/sync`'s status, even re-syncing identical content — a real, documented asymmetry from the package side's "added vs. skipped" merge semantics, not a bug.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. `test/test_pkg_sync.c` extended: the fixture archive now stages a package recipe *and* two versions of one image recipe (`0.9.0`, `1.0.0`) side by side; asserts the first sync adds both the package and image recipe, that `GET /v1/images/recipes/{name}` returns the higher (`1.0.0`) version's content and not the stale one, and that a re-sync correctly shows the image recipe landing in "added" again (always-overwrite) while the package recipe correctly lands in "skipped" (immutable, already present) — proving the asymmetry above is real and intentional, not accidental. Full regression sweep re-run clean.

## Consequences

- `pkg/` no longer exists as a git directory (it contained only `recipes/` and `image-recipes/`, both now relocated) — anywhere that referred to `pkg/recipes/...` as an informal path in a comment or doc has been updated to `recipes/package/...`.
- Task #867's original, narrower scope (commit the 4 already-built image recipes, wire sync) is fully subsumed by this ADR — no separate follow-up needed.
- `recipes/README.md` (replacing `pkg/recipes/README.md`) now documents both kinds under one roof, explicit about the two-different-meanings-of-"version" distinction above so a future recipe author doesn't assume image recipe versions mean the same thing package ones do.
- ADR-0123's own Consequences line ("Part 2's sync currently only walks `pkg/recipes/`, not `pkg/image-recipes/` — a deliberate, separate future decision, not assumed here") is now implemented; that ADR gets a one-line forward-pointer to this one rather than an edit to its own historical reasoning.
- The four image recipes captured this session (`dns`, `ldap`, `syslog`, `jumpbox`) each start at version `1.0.0` under `recipes/image/<name>/1.0.0/build.sh` — a real starting point for their own future revision history, not a placeholder.
