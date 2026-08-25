# 0040 — package recipes are managed via a real REST API, not baked into the installer ISO

## Status

Accepted

## Context

ADR-0039 fixed the immediate symptom (a fresh install has zero recipes, so `pkg install` can never find anything) by mirroring ADR-0019's own mechanism: stage `pkg/recipes/*.recipe` into the installer's payload, copy them onto the containers partition at real install time. It worked — verified end-to-end via a full real Secure-Boot QEMU install.

Presented with that fix, the user pushed back, correctly: baking a fixed recipe set into the ISO means the *only* way to add a new recipe, or update an existing one, is to rebuild the entire installer image and reinstall the whole OS. That's a fundamentally different kind of thing than what ADR-0019 actually seeds. Runtime libs (`ld.so`, `libc.so.6`, `libtinfo.so.6`) are fixed OS infrastructure — they essentially never change, so baking them in at install time costs nothing. Recipes are the opposite: a package catalog, expected to grow and update continuously over a system's life, the way `apt update`/`dnf makecache` never require reinstalling the distro underneath them. Tying recipe availability to a full reinstall is a real design smell, not a reasonable trade-off — mirroring ADR-0019's *mechanism* uncritically, without checking whether recipes have the same *nature* as runtime libs, was the mistake.

## Decision

Revert ADR-0039's install-time staging entirely (`mkinstalleriso.c`/`cix-install.c`/`test_installer.c`/`README.md` back to their pre-ADR-0039 shape — confirmed byte-for-byte via a clean recompile). Recipes are managed live, on an already-running system, via a real REST API:

- `POST /pkg/recipes` (body: `{"name", "content"}`) — adds a new recipe or replaces an existing one with the same name (upsert). `content` is written to a staging file under `pkg_dir/recipes/` first and validated with the exact same `parse_recipe()` every install-time lookup already uses (`name` must be a valid package name, and must equal `content`'s own `pkg_name=` field — the same invariant `resolve_chain()`'s dependency lookups already rely on). Only on success is the staging file atomically `rename()`d over `{name}.recipe` — an invalid upload can never clobber a recipe that was already working.
- `DELETE /pkg/recipes/{name}` — removes a recipe. Doesn't touch anything already installed through it (a build's output is merged into an image at install time; nothing about an already-installed package depends on its own recipe file continuing to exist).
- CLI: `cixctl pkg recipe add --name=NAME --file=PATH` (reads a local `.recipe` file, same `read_local_file()`-then-`jw_str()` pattern `run --file=` already uses for container config staging) and `cixctl pkg recipe rm NAME`.

`pkg.c`'s existing `parse_recipe()`/`g_recipes_dir` needed no changes — this is purely a new way to get a well-formed `.recipe` file onto disk at that same path, alongside the pre-existing manual-copy convenience this project's own dev sandbox has always used informally.

## Consequences

- Updating the recipe catalog on a running system is now a normal, live operation — no ISO rebuild, no reinstall, matching how every real package manager's own catalog actually works. This is the fix that actually addresses the user's original complaint (`pkg install --name=bash` failing on a fresh install) in a way that stays fixed as the catalog grows, not just once.
- ADR-0039's Status is `Superseded by ADR-0040` (append-only per `docs/adr/0000-adr-process.md` — its own Context/Decision text is left as-is, a genuine record of the reasoning that got revisited, not deleted).
- A fresh install still starts with zero recipes — an operator has to `pkg recipe add` at least one before `pkg install` can do anything. This is a real, deliberate trade-off (not a gap): a fresh install's very first useful action is now "add a recipe" rather than "everything I might ever want is silently pre-baked whether I need it or not." `POST /v1/system/restore`'s own pre-existing `pkg_recipes` field remains a valid bulk-seeding path (restoring a whole catalog at once) for an operator who wants that.
- `test/test_image_fixture.c`'s `test_image_fixture_copy_dir_files()` (extracted from `mkbootroot.c` during ADR-0039's work) stays — it's still genuinely used by `mkbootroot.c` for `web/`/firmware staging, independent of the recipes question, and gives that tool one real implementation instead of a private copy either way.
