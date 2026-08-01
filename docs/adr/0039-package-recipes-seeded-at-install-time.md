# 0039 — package recipes are seeded at install time too, same mechanism as ADR-0019

## Status

Superseded by ADR-0040

## Context

Diagnosing why the user's real install couldn't bootstrap PKI/pkg (Phase 25) led to walking them through creating a first test container. The very first `pkg install --name=bash` on their real, freshly installed box failed: `no such recipe, or it failed to parse`. Direct investigation (`daemon/src/pkg.c`'s `g_recipes_dir`, `PKG_DIR "/recipes"` under `BASE_DIR`) confirmed the real cause: `image/src/mkbootroot.c` never staged `pkg/recipes/*.recipe` anywhere, and neither did anything else. Every recipe that appeared to work on this project's own dev sandbox (`bash.recipe`, `bird.recipe`, etc.) got there by a developer manually copying files from the repo into `/var/lib/kanxeo/pkg/recipes` during earlier sessions — never a real, repeatable platform mechanism. On a genuinely fresh install, `PKG_DIR/recipes` is empty. `pkg install` cannot find *any* recipe, for *any* package — not a bash-specific gap, a total one.

This is the identical shape of problem ADR-0019 already solved once, for a different piece of "content a fresh install needs that the control-plane squashfs itself doesn't carry": `BASE_DIR` (`/var/lib/kanxeo`) is backed by the real, persistent `kanxeo-containers` partition (ADR-0018), not the read-only squashfs `mkbootroot.c` builds — so anything that needs to live there at first boot has to be written by `kanxeo-install` itself, at install time, from content staged onto the installer's own media by `mkinstalleriso.c`. ADR-0019 built exactly that path for runtime libs (`/payload/kanxeo-runtime/` → `images/base/rootfs/...`). Recipes need the same treatment, just never got it — this wasn't a design choice, it was an untouched gap nobody had hit yet.

## Decision

Reuse ADR-0019's mechanism exactly, generalized one step: `mkbootroot.c`'s own `copy_dir_files()` (previously `static`, used only for staging `web/` and an optional firmware directory) moved to `test/test_image_fixture.c` as an exported `test_image_fixture_copy_dir_files()` — one real "copy every regular file from a flat directory" implementation, shared by `mkbootroot.c` and now `mkinstalleriso.c` too, rather than a second copy drifting apart from the first.

`image/src/mkinstalleriso.c` gains a new required argv, `recipes-dir` (a plain directory of `*.recipe` files — `pkg/recipes` in this repo, but the tool itself stays generic, matching how `web-dir` is already caller-supplied rather than hardcoded), staged via that shared helper into a new `/payload/kanxeo-recipes/` subtree — same flat-payload shape as `/payload/kanxeo-runtime/`. `image/src/kanxeo-install.c` gains a matching `KANXEO_RECIPES_DIR_SRC` payload-path constant and, inside the same containers-partition mount/write/unmount block ADR-0019 already established, creates `pkg/recipes` and copies every staged file into it — via a small local `copy_dir_files()` (this file deliberately never links `test/`, since it runs as production code on a real target disk, so it mirrors the shared helper with its own local `ensure_dir()`/`copy_file()` rather than gaining a test-tree dependency for one function).

`test/test_installer.c`'s real, full Secure-Boot QEMU install flow now also verifies recipes land: right after the existing ADR-0019 runtime-lib check (extracting the containers partition after session 1, before any `pkg install` and before the independent session-3 reboot), a `debugfs -R "ls -l pkg/recipes"` confirms `bash.recipe`/`bird.recipe` are present — proving the installer itself wrote them, not a later step.

## Consequences

- A fresh install can now actually install packages — the user confirmed the original failure (`pkg install --name=bash` → "no such recipe") is what this closes; verified end-to-end via a full real 5-session Secure-Boot `test_installer.c` run (not just the two narrower console tests Phase 25 already used), including the new recipe-presence check.
- Every `*.recipe` file in `pkg/recipes/` at ISO-build time ships on every install from that ISO. This is a fixed set baked in at build time, not a live/updatable catalog — the same posture ADR-0019 already accepted for runtime libs, and the same boundary: a real "add/update a recipe on an already-installed system without rebuilding the ISO" mechanism doesn't exist yet (`POST /v1/system/restore`'s `pkg_recipes` field can inject one today, but it's a backup/restore mechanism repurposed for this, not a real single-recipe-upload endpoint) — a known, explicit gap, not attempted here since it wasn't what broke.
- `mkbootroot.c`'s own behavior is unchanged (same output, just calling the relocated shared helper instead of its own private copy) — confirmed via the existing firmware/web-staging test coverage passing unmodified.
- This is a distinct instance of the same underlying platform question Task "real base-image FHS-layout convention" (still open) is about: what does a fresh install/image actually need staged, and by which tool. This ADR closes the concrete, now-blocking recipe gap; it doesn't generalize the pattern into that broader convention, which remains future work.
