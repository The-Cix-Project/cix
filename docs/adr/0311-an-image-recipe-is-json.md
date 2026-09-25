# 0311 — An image recipe is JSON, because it was never a script

## Status

Proposed. Adjacent to [ADR-0309](0309-shell-recipes-are-history-the-shell-path-retires-with-its-last-dependent.md), which put image recipes explicitly out of scope of the shell **build path** retirement — correctly, since they are never built. This is the separate question that left behind: what format they should be in at all. Relates to [ADR-0252](0252-an-image-recipe-is-authoritative.md) and [ADR-0308](0308-recipes-are-their-own-repository-flat.md).

## Context

The owner asked whether image recipes should be JSON. They are `.sh` today. The answer is yes, and the evidence is not a preference:

**All 59 image recipes have exactly one line of content, and it is the same line in every one of them.** Measured in `cix-recipes` on 2026-09-25:

```
$ for f in recipes/image/*.sh; do grep -vE '^\s*#|^\s*$' "$f" | wc -l; done
1     # for all 59, without exception

$ grep -hoE '^[a-z_]+=' recipes/image/*.sh | sort | uniq -c
59 image_packages=
```

That line is a space-separated list of `name:policy:version` triples — 692 `pinned` and 147 `rolling` across the corpus.

**The daemon never executes one.** `parse_image_recipe_buf()` (`daemon/src/pkg.c`) calls `extract_line_value(buf, "image_packages=", …)` and then `strtok_r` on `:`. It is a line scanner reading a shell-shaped file, and `api_image.c` refuses anything without that key with *"recipe content failed to parse -- image_packages= is required"*. No shell ever runs. The `.sh` extension is vestigial.

**The repository already has a format for a declarative recipe.** Under [ADR-0308](0308-recipes-are-their-own-repository-flat.md) a recipe is one flat file named `<name>@<version>.<ext>`, and the tree reads:

| kind | count | format |
|---|---|---|
| package | 371 `.cbs`, 9 `.sh` | CPDL |
| deployment | 81 `.json` | JSON |
| image | 59 `.sh` | one shell assignment |

A deployment recipe — `{"name": "ar-1", "image": "wifi_router", "services": [...]}` — is a declarative document in JSON, parsed with the daemon's own `json.c`. An image recipe is a *more* declarative document in a format that claims to be executable and is not.

**The extension makes a claim the file does not honour.** This project already forbids a comment that asserts a mechanism which does not exist, on the grounds that it is what the next reader believes instead of reading the code. A filename is a stronger claim than a comment and is read first.

**What must not be lost.** [ADR-0252](0252-an-image-recipe-is-authoritative.md) made this file the authority on what an image holds, and the reasoning moved into it with the authority. `cix-builder@6.1.0.sh` is one `image_packages=` line under twenty-five lines of comment recording why two pins moved (glibc 2.44-12 → 2.44-14 as the first package rebuilt under ADR-0251's finalize policy: 2114 files to 1341), why `gcc` and `binutils` remain in a TCC platform's build image (`build/cix-boot.efi`, ADR-0215), and why `gitea`, `go` and `go-bootstrap` were deliberately dropped. JSON has no comments. A conversion that drops that prose destroys the record ADR-0252 deliberately created, and would be a worse outcome than the format it fixes.

## Decision

**An image recipe is a JSON document, `recipes/image/<name>@<version>.json`.**

```json
{
  "image": "cix-builder",
  "version": "6.1.0",
  "notes": "6.1.0 exists because ADR-0252 made this file authoritative, and two pins in 6.0.0 no longer described the image …",
  "packages": [
    { "name": "bash", "policy": "pinned", "version": "5.2.37-5" },
    { "name": "gcc",  "policy": "pinned", "version": "16.2.0-13",
      "notes": "for build/cix-boot.efi only (ADR-0215); everything else cix builds is TCC, per ADR-0001" }
  ]
}
```

1. **`notes` is a first-class field, at the document and at each package.** This is the clause that makes the change safe rather than lossy. The reasoning in these files is per-package as often as it is per-image — "why is gcc in a TCC platform's build image" belongs on the gcc entry, not in a preamble — and a format that could only hold a preamble would quietly flatten it.
2. **`policy` is spelled out** (`pinned`, `rolling`) rather than positional. The colon triple has no room for a fourth field and no way to omit the third; JSON does.
3. **The daemon parses it with `json.c`**, and `parse_image_recipe_buf()`'s line scanner goes. One parser for declarative recipes, which is what the deployment path already uses.
4. **Published `.sh` image recipes stay**, for ADR-0309's reasons unchanged: `pkg sync` is merge-only, so deleting a published recipe removes nothing from a host and only splits the source of truth. New revisions are JSON. The daemon reads both until no image manifest resolves to a `.sh` revision, which is a runnable query rather than a date — the same retirement shape ADR-0307 uses.
5. **The extension decides the format**, as [ADR-0305](0305-a-recipes-format-is-its-filename.md) already decided for packages. One rule for all three recipe kinds.

## Consequences

- The format stops claiming to be a script. Nothing sources these files and nothing ever did.
- `notes` becomes the place the reasoning lives, and it is machine-readable — `GET /v1/images/recipes/<name>` can return why a pin is where it is, which a shell comment could never do.
- One parser instead of two. `extract_line_value`/`tokenize_into` remain for package recipes only.
- A conversion pass over 59 files, mechanical except for the prose, which is the part that needs a person's judgement about where each paragraph belongs.
- `test_image_recipe` and `test_docindex` move with it.
- Independent of the shell **build path** retirement (ADR-0309, #516): an image recipe never reaches `PKG_BUILD_CMD`, so neither work blocks the other.

## Alternatives considered

**CPDL (`.cbs`), for one format across all recipes.** Rejected. An image is a manifest, not a build: there is no source, no build phase and no artifact, so CPDL would have nothing to execute and the recipe would be a `package` block with only metadata in it. It would also make listing an image's contents depend on the CPDL engine being present, which is a real dependency (ADR-0307 clause 6) taken on for a list of names.

**Leave them `.sh`.** Rejected. It is a second declarative format maintained for no benefit, and the extension misdescribes the file. The cost of the status quo is small and constant, which is exactly how a wrong default survives.

**TOML or YAML.** Rejected without much deliberation: neither has a parser in this tree, JSON has one that is already used for the sibling recipe kind, and adding a third document format to avoid quoting is not a trade this project would take.

**Keep the prose in a sibling `.md`.** Rejected. It splits one recipe across two files, which is what ADR-0308 flattened the corpus to avoid, and it puts the reasoning somewhere the API cannot return it.
