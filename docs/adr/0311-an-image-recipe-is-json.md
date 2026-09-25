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

That line is a space-separated list of `name:mode:version` triples (the daemon's own word is `mode` — `IMAGE_PKG_PINNED`/`IMAGE_PKG_ROLLING`) — 692 `pinned` and 147 `rolling` across the corpus.

**The daemon never executes one.** `parse_image_recipe_buf()` (`daemon/src/pkg.c`) calls `extract_line_value(buf, "image_packages=", …)` and then `strtok_r` on `:`. It is a line scanner reading a shell-shaped file, and `api_image.c` refuses anything without that key with *"recipe content failed to parse -- image_packages= is required"*. No shell ever runs. The `.sh` extension is vestigial.

**The repository already has a format for a declarative recipe.** Under [ADR-0308](0308-recipes-are-their-own-repository-flat.md) a recipe is one flat file named `<name>@<version>.<ext>`, and the tree reads:

| kind | count | format |
|---|---|---|
| package | 371 `.cbs`, 9 `.sh` | CPDL |
| deployment | 81 `.json` | JSON |
| image | 59 `.sh` | one shell assignment |

A deployment recipe — `{"name": "ar-1", "image": "wifi_router", "services": [...]}` — is a declarative document in JSON, parsed with the daemon's own `json.c`. An image recipe is a *more* declarative document in a format that claims to be executable and is not.

**The extension makes a claim the file does not honour.** These recipes were declarative from introduction -- the commit that added them calls them exactly that ("declarative image recipes", `9eb76890`) -- so no shell has ever run one. This project already forbids a comment that asserts a mechanism which does not exist, on the grounds that it is what the next reader believes instead of reading the code. A filename is a stronger claim than a comment and is read first.

**What must not be lost.** [ADR-0252](0252-an-image-recipe-is-authoritative.md) made this file the authority on what an image holds, and the reasoning moved into it with the authority. `cix-builder@6.1.0.sh` is one `image_packages=` line under twenty-five lines of comment recording why two pins moved (glibc 2.44-12 → 2.44-14 as the first package rebuilt under ADR-0251's finalize policy: 2114 files to 1341), why `gcc` and `binutils` remain in a TCC platform's build image (`build/cix-boot.efi`, ADR-0215), and why `gitea`, `go` and `go-bootstrap` were deliberately dropped. JSON has no comments. A conversion that drops that prose destroys the record ADR-0252 deliberately created, and would be a worse outcome than the format it fixes.

## Decision

**An image recipe is a JSON document, `recipes/image/<name>@<version>.json`.**

```json
{
  "image": "cix-builder",
  "version": "6.1.0",
  "notes": "6.1.0 exists because ADR-0252 made this file authoritative, and two pins in 6.0.0 no longer described the image …",
  "packages": [
    { "package": "bash", "mode": "pinned", "version": "5.2.37-5" },
    { "package": "gcc",  "mode": "pinned", "version": "16.2.0-13",
      "notes": "for build/cix-boot.efi only (ADR-0215); everything else cix builds is TCC, per ADR-0001" }
  ]
}
```

1. **An entry is spelled exactly as the API already spells one.** `POST /v1/images/{name}/manifest` reads `package`, `mode` and `version` (`api_image.c:467`), `openapi.yaml` describes the triple in those words, and `mode` is `pinned` or `rolling`. A draft of this ADR called the second field `policy`, which is a second name for a concept the system already names — the One Source of Truth failure this project forbids of a registry, applied to a vocabulary. **A recipe should look like the request that consumes it.**
2. **`notes` is a first-class field, at the document and at each package, and it has no length limit.** This is the clause that makes the change safe rather than lossy, and the limit matters as much as the field: the largest existing comment block is **11,688 bytes** (`kernel-builder@1.5.0.sh`, 222 comment lines), and `cix-builder@6.1.0.sh` is 1,626. A cap anywhere near CPDL's 511-byte package changelog would truncate exactly the prose this ADR exists to preserve. That cap belongs to CPDL package metadata and does not reach here; saying so explicitly is the point, because the next person to add a limit will be looking at the wrong precedent.
3. **The reasoning is per-package as often as per-image.** "Why is gcc in a TCC platform's build image" belongs on the gcc entry. A format that could only hold a preamble would flatten that into a paragraph nobody can attach to a line, which is how the reasoning stops being maintained.
4. **The conversion pass IS the new revision, and there is no coexistence window worth the name.** Each image gets `<name>@<next-version>.json` carrying its prose in `notes`, and the daemon drops `.sh` image parsing in the same change as the last one lands. ADR-0307's "retire on a runnable query" shape does not transfer: packages publish new revisions constantly, so a query converges on its own, while these 59 files change only when someone changes them. A query that reads "all 59, until 59 new revisions exist" is not a retirement condition, it is the work restated. Published `.sh` image recipes stay on disk for ADR-0309's reasons — `pkg sync` is merge-only, so deleting one removes nothing from a host and only splits the source of truth — but nothing reads them after the pass.
5. **The daemon parses it with `json.c`**, and `parse_image_recipe_buf()`'s line scanner goes. One parser for declarative recipes, which is what the deployment path already uses.
6. **The extension decides the format**, as [ADR-0305](0305-a-recipes-format-is-its-filename.md) already decided for packages. One rule for all three recipe kinds.
## Consequences

- The format stops claiming to be a script. Nothing sources these files, and the commit that introduced them (`9eb76890`) calls them declarative, so nothing ever did.
- `notes` becomes the place the reasoning lives, and it is machine-readable — `GET /v1/images/recipes/<name>` can return why a pin is where it is, which a shell comment could never do.
- One parser instead of two. `extract_line_value`/`tokenize_into` remain for package recipes only.
- A conversion pass over 59 files, mechanical except for the prose -- which is the part needing judgement about where each paragraph belongs, per-image or per-package. It is also the retirement: see clause 4, there is no organic query to wait on.
- `test_image_recipe` and `test_docindex` move with it.
- Independent of the shell **build path** retirement (ADR-0309, #516): an image recipe never reaches `PKG_BUILD_CMD`, so neither work blocks the other.

## Alternatives considered

**CPDL (`.cbs`), for one format across all recipes.** Rejected. An image is a manifest, not a build: there is no source, no build phase and no artifact, so CPDL would have nothing to execute and the recipe would be a `package` block with only metadata in it. It would also make listing an image's contents depend on the CPDL engine being present, which is a real dependency (ADR-0307 clause 6) taken on for a list of names.

**Leave them `.sh`.** Rejected. It is a second declarative format maintained for no benefit, and the extension misdescribes the file. The cost of the status quo is small and constant, which is exactly how a wrong default survives.

**TOML or YAML.** Rejected without much deliberation: neither has a parser in this tree, JSON has one that is already used for the sibling recipe kind, and adding a third document format to avoid quoting is not a trade this project would take.

**Keep the prose in a sibling `.md`.** Rejected. It splits one recipe across two files, which is what ADR-0308 flattened the corpus to avoid, and it puts the reasoning somewhere the API cannot return it.
