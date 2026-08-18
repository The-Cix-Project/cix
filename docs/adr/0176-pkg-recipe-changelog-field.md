# 0176 — `pkg_changelog=`: an optional, short, per-version recipe field

## Status

Accepted

## Context

Issue #44 (user-requested, 2026-08-18), alongside a web dashboard change: the Recipes list's inline ▸/▾ "N versions" expand-in-place toggle (issue #19) was replaced with a real "Versions" tab on a package's own detail page, matching the pattern the Image detail page's Versions tab already established (ADR-0107/0108's real, immutable version history). The user explicitly asked for a short changelog to be shown per version there, "for consistency" with a real detail-page section rather than a modal popup.

No changelog text exists anywhere in a recipe today — `pkg_name=`/`pkg_version=`/`pkg_source=`/`pkg_sha256=`/`pkg_depends=`/`pkg_artifact_sha256=`, nothing else. Three real options were on the table: show nothing (just version/date/depends, no new field); compute a diff summary from existing fields between consecutive versions; or add a real, new free-text field recipe authors fill in. Asked directly (`AskUserQuestion`); the user chose the new field, explicitly accepting the larger scope over the cheaper alternatives.

## Decision

`pkg_changelog=` joins `pkg_depends=`/`pkg_artifact_sha256=` as a third fully optional recipe field, parsed the identical way (`extract_line_value()`, a single-line, double-quoted value — reads up to the closing quote or a newline, whichever comes first). This is a deliberate, structural constraint, not an oversight: the user's own ask was for a *short* changelog, and a single-line field makes a real multi-paragraph release note unrepresentable by construction rather than relying on recipe-author discipline to keep it short.

Bounded at `PKG_CHANGELOG_MAX` (512 bytes) — generous for one descriptive line, still a real, enforced cap. An over-long value is silently dropped (empty), the same behavior `pkg_depends=`/`pkg_artifact_sha256=` already have for their own overflow case — not a hard recipe-parse failure over one optional field.

Exposed in both `GET /pkg/recipes` (list) and `GET /pkg/recipes/{name}` (single) JSON responses as `"changelog"`, nullable — `null` for the ~80 existing recipes published before this field existed. **Never backfilled**: recipe authors add it going forward, on their own schedule, exactly like every other optional field this project has ever added to the recipe format.

Web dashboard: the package detail page gains a `Versions` tab (`renderPackageDetailVersions()`) listing every published `(name, version)` for that package — version, changelog, created-at, and a real per-version delete (unlike an image's own immutable version history, a package recipe version genuinely can be deleted). The list view's own former inline expand toggle is gone entirely — one browsing surface per concern: the list shows "the latest of everything," the detail page's Versions tab shows "every version of this one thing."

## Consequences

- `daemon/include/pkg.h`/`daemon/src/pkg.c`: `PKG_CHANGELOG_MAX`, `struct pkg_recipe.changelog`, `parse_recipe()` extraction, both JSON write sites (`pkg_write_json_recipes()`, `pkg_recipe_get()`).
- `web/index.html`/`web/app.js`: new `pkgd-tab-versions`/`pkgd-panel-versions`, `renderPackageDetailVersions()`, simplified `renderRecipesList()` (no more `expandedPkgRecipes` state or inline expand rows), the now-fully-unused `.recipe-version-toggle`/`.recipe-version-row` CSS rules removed.
- Image and container *recipes* (as opposed to an image's own real version history) still have no multi-version concept at all server-side (ADR-0149/0151: one current file per name, no history) — explicitly out of scope here; extending those to keep real version history would be a separate, larger architectural change, not a natural extension of this one.
- No retroactive edit of any existing recipe file across the repo — this field is adopted incrementally, recipe by recipe, as each one is next re-pinned or newly published.
