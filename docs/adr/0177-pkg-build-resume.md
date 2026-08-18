# 0177 — `pkg resume`: continue a kept-on-failure build in place (issue #46)

## Status

Accepted

## Context

ADR-0175's `keep_on_failure` (issue #35) solved *inspecting* a failed build, but the follow-on cost showed up immediately in real use: debugging issue #32's GCC bootstrap meant iterating on `gcc.recipe` itself (a fixup function, a `make` parallelism level) across many multi-hour `--enable-bootstrap` attempts, and every single iteration paid for a full `reset_build_container_dir()` (`rm -rf` the whole build container) + `extract_tarball()` (re-untar gcc/gmp/mpfr/mpc from scratch) + a build restarting from configure, even when the only thing that actually changed between attempts was one line in the recipe's own `pkg_build()`.

The user asked directly, mid-investigation: "why don't we continue compilation from where it broke, rather than start from the beginning again?" — a real, reasonable question with a real architectural answer, not just a "yes, that would be nice."

Before designing anything, the actual mechanics were confirmed by reading the code directly, not assumed:
- `overlay_create()` (`src/overlay.c`) already tolerates a pre-existing upperdir/workdir (`mkdir(...) != 0 && errno != EEXIST` for both) — remounting over an already-populated overlay is safe.
- `registry_remove()` (`daemon/src/registry.c`) never touches disk at all — only kills a still-running process, tears down network interfaces, and frees the in-memory registry slot. Disk cleanup is a wholly separate, never-automatically-invoked step (`handle_delete()`'s own `persist_remove_tree()`).
- `struct pkg_entry` (`daemon/src/pkg.c`) already retains `build_lowerdir`/`build_upperdir`/`build_workdir`/`build_merged`/`build_container_name` from the original attempt, untouched by anything a failure does.
- `pkg_build_container_chain_index()` derives a build container's owning chain slot directly by parsing its own name (`"__pkgbuild-N"` → `N`) — the mechanism already exists to reattach to the *same* chain slot a fresh `chain_alloc()` would never reuse.

Every real precondition for "reuse the exact same overlay, skip fetch/extract entirely" was already true; nothing about the container/overlay/registry model needed to change, only a new entry point into it.

## Decision

Add `pkg_resume_build()` (`daemon/src/pkg.c`) and `POST /v1/pkg/resume` (`daemon/src/main.c`): given `(name, image)` addressing a `PKG_STATE_FAILED` entry with a non-empty `kept_build_container` (ADR-0175), it re-resolves `name`'s recipe (optionally at a different, newly-published `version` — the primary use case: publish a fixed recipe, then resume under it), stages the new `build.sh` over the old one in the *existing* `build_upperdir`, resets only `build/pkg-dest` (the previous attempt's partial install output), and leaves `build/src` — the already-extracted, already-partially-built source tree — completely untouched. No fetch subprocess runs at all; the call is synchronous and returns a ready-to-spawn `container_spec` directly, the same shape `pkg_fetch_completed()` produces.

The chain slot reused is always the one `pkg_build_container_chain_index(kept_build_container)` parses back out — never a freshly `chain_alloc()`'d one, since `pkg_build_completed()` looks up `g_chains[chain_idx]` by the exit event's own index and reusing any other slot would corrupt an unrelated chain's bookkeeping. `PKG_ERR_BUSY` if that specific slot has since been reclaimed by a different job (a real, if narrow, possibility — the slot goes idle the instant the original build fails).

`pkg_resume_build()` deliberately does not call `registry_remove()`/`registry_create()` itself — `pkg.c` has never linked against `registry.h`, and this preserves that layering. `handle_pkg_resume()` (main.c) does both, in that order, mirroring `handle_pkg_fetch_event()`'s existing post-`pkg_fetch_completed()` sequence exactly, with one extra `registry_remove()` first since the container name here is a reuse, not a fresh one.

The shared tail every real build-container spawn needs (populate `container_spec` from `e->build_*`, stand up the output-capture pipe, mark the entry `BUILDING`) was extracted out of `pkg_fetch_completed()` into a new static `start_build_container_spec()` helper, so `pkg_resume_build()` doesn't duplicate that security/correctness-sensitive sequence a second time.

## Consequences

- `daemon/include/pkg.h`/`daemon/src/pkg.c`: new `pkg_resume_build()`; `pkg_fetch_completed()`'s own tail refactored into `start_build_container_spec()` with no behavior change.
- `daemon/src/main.c`: new `handle_pkg_resume()` + `POST /v1/pkg/resume` route, built from the exact same `registry_create()`/`register_container_pidfd()`/`register_pkg_build_output()` sequence `handle_pkg_fetch_event()` already establishes.
- REST: `POST /v1/pkg/resume` — body `{name, image?, version?, extra_config_symbols?, keep_on_failure?}`, response shape identical to `GET /v1/pkg/{name}` (a `PkgEntry`), `202 Accepted` on success. `404` if the entry isn't a resumable `failed`-with-`kept_build_container` state, `409` if its chain slot is currently busy.
- CLI: `thincctl pkg resume --name=NAME [--image=IMAGE] [--version=VERSION] [--keep-on-failure]`.
- `test/test_pkg.c` step 19: proves the reuse is genuine, not a disguised restart — the first (deliberately failing) build leaves a marker file in `/build/src`; the resumed recipe's own `pkg_build()` fails loudly if that marker is missing (which a real `extract_tarball()` re-run would never reproduce, since `reset_build_container_dir()` wipes the whole container first). Also confirms: the recipe swap under a new version takes effect, `kept_build_container` clears on a resumed success, and the exact same container name is reused (not a new one) — then automatically torn down on success exactly like any other clean pkgbuild exit.
- Found and fixed during verification, not left in: `handle_pkg_resume()`'s first draft freed the request's parsed JSON tree before its final `pkg_get_one(name, image, ...)` read-back, a real use-after-free (`name`/`image` are pointers into that tree's own string storage) — masked as a plain "resumed but could not be read back" 500 rather than a crash, caught by the new test itself rather than by inspection. Fixed by copying `name`/`image` into local, function-owned buffers before the JSON tree is freed, matching every other pkg handler's own field lifetime discipline.
- Local regression suite (`test_pkg`) passes, including the new step 19. Not yet exercised live against 192.168.15.95 — using it to actually resume the interrupted `gcc/12.5.0-4` bootstrap attempt is the real motivating follow-on, not part of this change.
