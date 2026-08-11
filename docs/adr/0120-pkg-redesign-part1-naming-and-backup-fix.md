# 0120 — pkg/ redesign Part 1: recipe.sh → build.sh, and a real backup/restore regression fixed along the way

## Status

Accepted

## Context

Starting a larger redesign of `pkg/` (task #739, this is Part 1 of a five-part sequence — see the ROADMAP entry for this phase for the full shape) with the smallest, highest-confidence piece first, per this project's own established sequencing convention.

The user's own observation, verbatim in intent: `pkg/recipes/<name>/<version>/recipe.sh` says "recipe" twice for a tree that, on the git-tracked side, holds nothing but recipes (`pkg/` has no sibling of `recipes/` today). Confirmed directly by inspection — `pkg/` contains exactly one thing, `recipes/`.

While grounding the redesign in the current code (reading `daemon/src/pkg.c`/`main.c` in full before proposing anything), a real, independent bug surfaced: `do_system_backup()`/`do_system_restore()` (`daemon/src/main.c`) still assumed the pre-ADR-0107 flat `<name>.recipe` layout — a single `opendir(PKG_RECIPES_DIR)` filtering a `.recipe` suffix directly. Once ADR-0107's version-keyed migration landed (`<name>/<version>/recipe.sh`), every entry in `PKG_RECIPES_DIR` became a per-name *subdirectory*, none of which end in `.recipe` — so that filter has matched precisely nothing since that migration shipped. **System backup has included zero recipes for the entire time ADR-0107's layout has been live**, silently, because the existing test (`test_system_backup.c`) only ever asserted `pkg_recipes` was *present and an object* (true even when empty), never that it actually contained anything.

## Decision

**Naming**: the leaf recipe script is renamed `recipe.sh` → `build.sh` everywhere — every existing recipe file (67, via `git mv`, preserving history), `daemon/src/pkg.c`/`pkg.h`'s own path construction and doc comments, and every living doc (`docs/guides/writing-recipes.md`, `pkg/recipes/README.md`, `docs/api/README.md`, `docs/api/openapi.yaml`). `pkg/recipes/` itself is kept, not collapsed to `pkg/` — Part 3/4 of this same redesign add a package-artifact cache and image recipes as real siblings under `pkg/`, at which point `pkg/` genuinely is an umbrella over more than one kind of content, so collapsing it now would just need undoing later. The **in-container staging path stays `/build/recipe.sh` unchanged** (`daemon/include/pkg.h`'s own documented build-container contract, `". /build/recipe.sh"`) — a completely different, private, build-container-internal convention with no "recipes/…recipe.sh" duplication in it at all; renaming it would be pure churn addressing nothing.

**Backup/restore fix**: `do_system_backup()` now walks the real two-level `<name>/<version>/build.sh` structure, emitting each version under a `"<name>/<version>"` compound key (a bare package name can never itself contain `/`, so this is unambiguous and needs no escaping). `do_system_restore()` splits on that same separator to reconstruct the exact nested on-disk path, with `persist_mkdir_p()` creating the intermediate `<name>/<version>/` directory before the atomic write — plus new key-format validation (`400` on a malformed key) in the same "validate everything, then write" pass the function already does for every other field.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. `test_system_backup.c` extended with a real, direct regression test for exactly this bug class: a real recipe is `POST`ed before the backup call, and the backup response is asserted to contain the `"backuptestpkg/1.0"` key with real matching content (would have failed before this fix, on the old code, since that key would never have existed at all). A second check restores a *different* `(name,version)` pair and confirms the file lands at the real nested `<data-dir>/pkg/recipes/restoredpkg/2.0/build.sh` path, proving the restore side specifically (not just backup) writes to the layout `find_recipe_path()` actually looks up. Full regression sweep (24 tests) all pass.

Live on 192.168.15.95: deployed and confirmed `GET /v1/system/backup`'s `pkg_recipes` field now contains real entries.

## Consequences

- Every recipe file in this repository is now named `build.sh`, not `recipe.sh` — any external tooling/scripts referencing the old filename directly (not through the REST API, which was never filename-shaped to begin with) needs updating. None exist in this repository outside what this change already touched.
- System backups taken from this point forward genuinely include recipe content again; backups taken *before* this fix (while ADR-0107's layout was live but this bug was still present) never had recipes in them and cannot retroactively gain them — this is a going-forward fix, not a data-recovery one.
- Sets up the rest of this redesign (Parts 2-5): a configurable repo + `pkg sync`, package-artifact caching, image recipes, and rolling containers, each landing as its own part with its own build/test/deploy/verify cycle.
