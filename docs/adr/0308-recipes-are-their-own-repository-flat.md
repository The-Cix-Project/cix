# 0308 — recipes are their own repository, and a recipe is one flat file

## Status

Accepted. Supersedes [ADR-0149](0149-unified-recipes-git-layout.md) on
layout, and moves what that ADR governs out of this repository.

## Context

Two changes, made together on 2026-09-21 because both rewrite every
recipe path and doing them separately would rewrite them twice.

**The corpus had become most of this repository.** Measured before the
change: 1711 tracked files under `recipes/` against 892 everywhere
else, so two thirds of the repository was recipes. `cix` alone held 559
revision directories, one per release tag, most of them a three-line
version bump whose history git already records.

**ADR-0149's layout carried no information.** It gave every recipe
`<kind>/<name>/<version>/build.<ext>`, and it was right to unify the
kinds. What it could not have known then is how the shape would read at
1577 versions: every one of those directories holds **exactly one
file** — checked across all of them, no patches, no data, nothing — so
two directory levels existed to carry two fields, under a leaf filename
that was the same two strings 1577 times over. Every editor tab, grep
hit, diff header and error message said `build.cbs`, and only the path
said which recipe it was.

## Decision

**Recipes live in `git.home.arpa/itdlabs/cix-recipes`**, with history
preserved, and **a recipe is one file named `<name>@<version>.<ext>`**
in one flat directory per kind:

```
recipes/package/zstd@1.5.7-5.cbs
recipes/image/cix-builder@1.4.0.sh
recipes/deployment/dns-1@1.2.0.json
```

The extension is the format, which is [ADR-0305](0305-a-recipes-filename-is-its-format.md)
finishing its own thought: `cbs` for a CBS recipe in CPDL, `sh` for a
shell one, `json` for a container deployment definition.

`@` is the separator. Not `_`, which was the first proposal: no package
name or version contains an underscore *today*, but that is a fact
about today and upstream names with underscores are ordinary, so the
split rule would break on the first one. `@` is already this platform's
name/version separator wherever it prints one — `glibc@2.44-14`,
`kmod@cix-builder`, `pkg rm ncurses@jump` — and no recipe name
contains one.

## Consequences

**The daemon reads the new shape and only the new shape.** Three sync
walkers in `daemon/src/pkg.c` changed, sharing one `recipe_file_split()`
parser. There is no dual-layout support and no fallback: this project
does clean cut-overs, so the daemon must ship before the repository is
synced, and `pkg sync` is the only thing in the window between.

**Which repository is already runtime configuration**, which is why
this is not a bigger change than it is. `pkg sync` fetches whatever
`pkg repo-config set --url=` names and walks `recipes/` inside it; only
the layout below that was fixed in code.

**Two tests moved and three changed.** `test_kernelrecipe` and
`test_recipe_hygiene` assert things about recipe *content*, so they
went with the corpus. `test_pkg_sync` builds a synthetic repository and
is the gate on the walker, so it stayed and went flat.
`test_image_fixture` and `test_installer` read real recipes, so they
stayed and now resolve the corpus through `test_recipes_root()` —
`CIX_RECIPES_DIR`, defaulting to a sibling checkout.

**A clean checkout of this repository can no longer run the whole test
suite by itself.** That is the real cost of the split and it is not
hidden: those tests need `cix-recipes` beside it. It was accepted
because the alternative — a test that skips when the corpus is absent —
is the silently-disabled gate this project has already been bitten by.

**A change spanning the daemon and a recipe is now two commits in two
repositories.** Recipes overwhelmingly track upstream versions rather
than daemon internals, so this is rare; where it is not rare is exactly
this ADR's own cut-over, which is why the sequencing above is written
down rather than left to judgement.

**The credential incident bounded how history moved.** `cix-recipes` is
public, and six commits under `recipes/` carried a live
`git.home.arpa` credential (see [CHANGELOG](../../CHANGELOG.md), #502).
History was rewritten with `git filter-repo` to replace it with the
`{{REPO_TOKEN}}` placeholder before the first push, and the result was
verified by grepping **every commit** in the rewritten history, not
just the log. That scrub covers the one credential known to be there;
it is not a proof that 1330 commits contain no other secret, and the
same credential remains in this repository's own history regardless,
so it still wants rotating.
