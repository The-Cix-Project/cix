# 0157 — parallel package builds: design (task #841)

## Status

Proposed — a complete, actionable design, deliberately not implemented in the same session it was written. See "Why proposed, not accepted" below.

## Context

Task #841 asks for a configurable limit (default 10) on concurrent package builds — today, `pkg install`/`pkg hostbuild` serialize to exactly one job in flight, system-wide, returning `PKG_ERR_BUSY` (`409`) for any second attempt while one is running.

Investigated directly before designing anything (per this project's own "Outline" discipline), and the real shape of "one job at a time" turns out to be woven far deeper than a single semaphore:

- **At least ten distinct module-level statics in `daemon/src/pkg.c`** carry "the current job"'s state with no job identifier attached at all — `g_current_job_name`/`_image`/`_is_hostbuild`/`_build_image`/`_cache_hit`/`_target_version`, `g_dep_queue[]`/`_count`/`_pos`/`_is_upgrade`, `g_build_lowerdir`/`_upperdir`/`_workdir`/`_merged`/`_argv_cmd`/`_argv`/`_envp`, `g_build_output_rd`/`_captured`/`_captured_len`.
- **The build container itself has a single, hardcoded name**, `PKG_BUILD_CONTAINER_NAME = "__pkgbuild"` — the container registry (`registry_create()`) would reject a second one outright even before any of the state above became a problem. This alone proves today's design assumes singularity at the registry layer, not just at pkg.c's own bookkeeping.
- **The reactor (`daemon/src/main.c`) has a single `CONN_PKG_BUILD_OUTPUT` conn kind** registered directly against pkg.c's own single output-capture pipe (`pkg_build_output_fd()`), and a single `CONN_PKG_BUILD_LOG_WS` live-tail WebSocket (`GET /pkg/build/log`) that tails "the" in-flight build with no way to name which one.
- **A hazard that looked real at first but isn't**: every ordinary (non-hostbuild) install's build container uses `g_pkgbuild_rootfs` — a single, shared, persistent directory — as its own overlay lowerdir, and also merges its own completed build output back into that same directory (`merge_tree(dest_dir, g_pkgbuild_rootfs, "", NULL)`, the "cumulative sandbox" `CLAUDE.md` already documents real bugs from, e.g. ambient-gcc contamination). This looks like a write race under real concurrency — see "no new synchronization needed" below for why it isn't one in this codebase's actual single-threaded architecture.

### Why proposed, not accepted

This is the single most heavily-used, most foundational subsystem in the project — every image, every container's dependencies, every hostbuild (including the self-hosted deploy pipeline this same session spent hours getting working end to end for the first time, ADR-0154/0155) goes through it. A rushed rewrite here, attempted at the tail end of an already-long session, risks a subtle bug that corrupts real, live state on the one real deployment target (192.168.15.95) this project has — exactly the class of "silent, hard-to-reproduce" bug `CLAUDE.md` repeatedly documents (the TCC/VLA parsing gap, the struct-packing gaps, tonight's own byte-swap bug). Per this project's own mandated workflow ("Outline — Execute — Verify," "never generate code for multiple systems/phases at once"), the Outline is real, complete work in its own right — landing it now, cleanly, is more consistent with "no hacks" than continuing straight into an under-verified implementation.

## Decision (the design itself)

Kept deliberately close to the shape the codebase already has — no new parallel bookkeeping structure where an existing table already fits.

### 1. Build-transient fields move onto `struct pkg_entry` itself — no new array for this part

`g_packages[PKG_MAX_PACKAGES]` (`struct pkg_entry`, keyed uniquely by `(name, image)` via the already-existing `pkg_find()`) is already the real, per-package state table — every in-flight build already corresponds to exactly one specific entry whose `state` is `fetching`/`building`. `g_build_lowerdir`/`_upperdir`/`_workdir`/`_merged`/`_argv_cmd`/`_argv`/`_envp`, `g_build_output_rd`/`_captured`/`_captured_len`, and `g_current_job_cache_hit` all move onto `pkg_entry` as fields only meaningful while that specific entry's own `state` is `fetching` or `building` — fields on the table that already exists for exactly this purpose, not a second table. "How many builds are in flight right now" is simply a count of entries in those two states.

### 2. A small, separate table for in-flight *chains* — sized to the concurrency limit, not the package table

A dependency chain (today's `g_dep_queue[]`) is inherently a property of one *top-level request* ("install X, which needs A and B first"), not of any single package — a package being built doesn't know what triggered its own install. This is the one piece that genuinely needs a new, small table: `struct pkg_chain g_chains[PKG_MAX_CONCURRENT_JOBS]`, sized to the *configured concurrency limit* (small — 10 by default, a modest compile-time ceiling above that, not `PKG_MAX_PACKAGES`), holding what `g_dep_queue`/`_count`/`_pos`/`_is_upgrade`/`_target_version`/`_is_hostbuild`/`_build_image` already hold today, plus which `pkg_entry` is the chain's own currently-active one. A completion handler for a given `pkg_entry` looks up its owning chain (a back-reference stored on the entry itself, valid only while fetching/building) to find what's next.

### 3. Per-build container identity

`PKG_BUILD_CONTAINER_NAME` becomes `PKG_BUILD_CONTAINER_NAME "-" <g_packages[] index>` (stable for as long as the entry's own state stays fetching/building) — a real, distinct registry entry per concurrent build, exactly like any other container. Every completion handler that currently does `pkg_find(g_current_job_name, g_current_job_image)` instead receives the specific `pkg_entry*` directly — the reactor's own conn already knows which pidfd/output-pipe fired and can point straight at the entry, the same "conn holds a pointer back to the specific resource it's for" pattern `registry_entry.reactor_conn` already establishes in reverse.

### 4. The `g_pkgbuild_rootfs` merge-back — no new synchronization needed

The actual compile/build step reads `g_pkgbuild_rootfs` as a lowerdir — safe to share across every concurrent build unchanged. The merge-back (`merge_tree(dest_dir, g_pkgbuild_rootfs, ...)`) looked like a critical section at first, but isn't one in practice: this daemon has no real threads anywhere, and every event handler (a completion callback included) already runs to completion, uninterrupted, before `epoll_wait()` is ever called again — `merge_tree()` is a plain, synchronous, blocking function with no `epoll`-yielding I/O in it. Two builds' own child processes can genuinely finish their real, off-process compile work at the same wall-clock instant, but the *daemon's own* handling of each completion can never interleave with another's, simply because there is only ever one thread of daemon-side control flow. The only real requirement is that each completion handler operates on the *right* entry's own `dest_dir` — already guaranteed once state moves onto `pkg_entry` in section 1, with no new lock, flag, or queue needed on top.

### 5. Config surface

`GET`/`PUT /v1/system/pkg-build-config` — `{"max_concurrent_jobs": N}`, default 10 per the task's own ask, validated range `[1, PKG_MAX_CONCURRENT_JOBS]` — mirrors `rolling-config`/`pkg-cache-config`/every other daemon-wide tunable's own established `GET`/`PUT` shape (ADR-0122/0124 precedent). `pkg_install_start()`/`pkg_hostbuild_start()`'s current `if (g_current_job_name[0] != '\0') return PKG_ERR_BUSY;` becomes "count of in-flight chains >= `max_concurrent_jobs`? then `PKG_ERR_BUSY`."

### 6. Reactor/API surface changes

- `CONN_PKG_BUILD_OUTPUT` needs a `pkg_entry*` (or its stable `g_packages[]` index) in its own conn struct instead of implicitly meaning "the" build, so output drains route to the right entry's own capture buffer.
- `GET /pkg/build/log` (the live-tail WebSocket) currently tails "the" in-flight build with no way to say which — needs a new required query parameter (`?name=`) once more than one can be in flight. A real, user-visible API shape change, not purely internal.
- `GET /pkg/{name}` and `GET /pkg` (the list) already report per-package state keyed by name, not by a singular "the current job" — no shape change needed, just correct routing internally.
- Each concurrent top-level install still walks its own dependency chain serially (package B's build can't start before package A it depends on finishes) — the parallelism is across *unrelated* top-level installs' chains, not within one chain.

### 7. Hostbuilds

Hostbuilds (`pkg_hostbuild_start()`) reuse the exact same `pkg_entry`/chain mechanism — a hostbuild's own build container uses a *named image's* rootfs as its lowerdir (`g_current_job_build_image`), not the shared `g_pkgbuild_rootfs`, so it has no merge-back concern at all; it's the simpler case once the mechanism above exists. No separate design needed here.

## Phased implementation plan (for whoever picks this up)

1. Move the build-transient fields onto `pkg_entry` (section 1) and introduce the small `g_chains[]` table (section 2), migrating every `g_current_job_*`/`g_dep_queue_*`/`g_build_*` call site to the new locations one at a time, **with the effective concurrency limit still fixed at 1** — a pure refactor, zero behavior change, fully verifiable against the existing test suite with no new tests needed yet. This is the largest, most mechanical, least risky phase — get it merged and verified completely on its own before phase 2.
2. Raise the effective limit to 2 and write the first real concurrency test: two genuinely different packages installed back to back with no `--wait` between the two `POST`s, both expected to reach `state: "installed"` with the second `POST` never getting `409`. This is the real risk-proving phase for the phase-1 migration — any field that should have moved but didn't surfaces here as one job corrupting the other's result.
3. Add the config endpoint, raise the real default to 10, extend `GET /pkg/build/log` with `?name=`, update CLI/web/docs.
4. A real stress test: install N (config limit + a few more) genuinely different packages concurrently, confirm the `(limit+1)`th `POST` gets a real `409` until a slot frees, and confirm `g_pkgbuild_rootfs`'s own final content (once every job completes) is byte-identical to the same set of installs run serially on a fresh sandbox — the decisive proof that concurrent merge-backs genuinely don't interleave, not just that a bug didn't happen to reproduce once.

## Consequences

- Real, substantial follow-on work — comparable in size to any other single feature shipped this session, not a quick add. Whoever implements it should expect to spend real, dedicated time on phase 1 alone before any user-visible behavior changes.
- Every existing daemon-linked test that touches `pkg install`/`pkg hostbuild` needs to keep passing unmodified through phase 1 (a pure refactor) — a good, cheap correctness signal for that phase specifically.
- `g_pkgbuild_rootfs`'s own "cumulative sandbox" design (already flagged in `CLAUDE.md` as a source of real, subtle bugs like ambient-gcc contamination) stays exactly as load-bearing as it already is today — this design doesn't add a new synchronization mechanism around it (section 4), and doesn't change the underlying fact that its contents depend on *some* history of prior builds having happened, in *some* order. A future redesign replacing the cumulative-sandbox model entirely is out of scope here.
