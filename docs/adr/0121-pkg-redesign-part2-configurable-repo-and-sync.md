# 0121 — pkg/ redesign Part 2: configurable recipe repo + `pkg sync`

## Status

Accepted

## Context

Part 2 of the five-part `pkg/` redesign (task #739; Part 1, ADR-0120, renamed `recipe.sh`→`build.sh` and fixed the backup/restore recipe bug). Today every recipe reaches a Cix host one at a time, hand-pushed via `POST /v1/pkg/recipes`. That has worked for this project's own development loop (recipes live in this git repo, pushed manually as needed) but doesn't scale to a real fleet: there is no way for an operator to point a Cix host at "the recipe set", only at individual recipes they already know they want.

Design was worked through with the user across several rounds before implementation, converging on:
- **Recipes only, not artifacts.** Part 3 (package artifact fetch/cache) is a separate, later piece — conflating the two here would mean `pkg sync` either always builds from source (defeating the point of a precompiled-artifact cache) or silently starts fetching binaries nobody asked for yet.
- **Merge/additive, not mirror.** A sync never deletes or overwrites a locally-known recipe — matches ADR-0107's own recipe immutability (`pkg_recipe_add()`'s duplicate-(name,version) rejection), and means a host can layer a shared upstream repo's recipes on top of ones it built/added itself without a sync silently discarding local work.
- **Manual first, periodic optional.** `POST /v1/pkg/sync` is always available on demand; a configurable `sync_interval_seconds` (0 = disabled, the default) additionally re-arms an automatic sync on a timer, mirroring the exact periodic-timerfd pattern NTP already established (task #751's `arm_ntp_periodic_timer()`/`start_ntp_periodic_timer()`).
- **Three explicit per-forge branches, not one generic abstraction.** gitea, github, and gitlab each have a genuinely different REST archive-download URL shape and auth header convention (confirmed directly, not assumed) — see `build_sync_fetch_request()` below. Force-fitting these into one "generic git forge" interface would either lose real per-forge behavior or grow speculative configuration knobs for cases that don't exist yet. Three small, separately-readable branches are the more honest fit, consistent with this project's own "No Hacks" maxim.
- **Only gitea is empirically verified.** This project's own dev/test environment can reach `git.home.arpa` (a real gitea instance, already used by ADR-0057's own cix-hostbuild self-fetch) and nothing else. The github/gitlab branches are written to each forge's own documented, stable REST API shape, but have never been exercised against a real github.com/gitlab.com account from this sandbox — stated plainly here rather than silently assumed working, per this project's "confirmed the hard way" documentation ethos.

## Decision

**Configured repo** (`daemon/src/pkg.c`'s new `pkg_repo_*` functions, backed by `<data-dir>/pkg/repo_config.json`, loaded by `pkg_repo_init()` at daemon startup): `repo_url`, `repo_kind` (`gitea`|`github`|`gitlab`), `ref` (default `master`), `auth_token` (optional, for a private repo), `sync_interval_seconds` (default `0`, disabled). `GET`/`PUT /v1/pkg/repo-config` expose this — `PUT` is a **partial update**: any field omitted from the request body leaves the existing value untouched (`pkg_repo_set_config()`'s own NULL-means-unchanged contract), and an explicit `"auth_token": ""` is the one way to clear an already-set token. The token itself is never echoed back by `GET`/`PUT` — only a derived `auth_token_set` boolean — the same "never round-trip a secret" posture already established for every other credential this codebase handles.

**Fetch mechanism** (`build_sync_fetch_request()`): parses `repo_url` into `scheme://host/owner/repo`, then builds a forge-specific archive URL:
- **gitea**: `<scheme>://[token@]<host>/api/v1/repos/<owner>/<repo>/archive/<ref>.tar.gz` — token-in-URL basic auth, the identical convention ADR-0057's own `cix.recipe pkg_source` already relies on.
- **github**: `https://api.github.com/repos/<owner>/<repo>/tarball/<ref>` for `github.com` itself (a separate API domain per GitHub's own docs), or `<scheme>://<host>/api/v3/repos/.../tarball/<ref>` for GitHub Enterprise Server; `Authorization: Bearer <token>` header when a token is set.
- **gitlab**: `<scheme>://<host>/api/v4/projects/<owner>%2F<repo>/repository/archive.tar.gz?sha=<ref>` — identical for gitlab.com and self-hosted; `PRIVATE-TOKEN: <token>` header when set. (A nested subgroup path, e.g. `group/subgroup/repo`, is a known, explicitly out-of-v1-scope gap — it folds into `owner` as one string and fails cleanly at fetch time rather than silently mismatching.)

**Async fetch + merge** (`pkg_sync_start()`/`pkg_sync_completed()`): a direct structural clone of `CONN_BOOTSTRAP_FETCH`'s own fork+execve(curl)+`pidfd`+epoll-tracked pattern (ADR-0065) — `CONN_PKG_SYNC_FETCH` in `main.c`. On success, the fetched tarball is extracted (reusing `extract_tarball()`/`tarball_has_common_top_dir()` unchanged — a gitea/github/gitlab archive download has exactly the same single-top-level-directory shape as a git-archive tag download), then every `pkg/recipes/<name>/<version>/build.sh` found inside is passed to the existing `pkg_recipe_add()` one at a time. `pkg_recipe_add()`'s own duplicate-(name,version) rejection *is* the merge semantics — no separate merge logic was written; a recipe already present is counted as `skipped`, a genuinely new one as `added`, anything that fails to parse as `failed` (surfaced in the status's `error` field, not silently dropped). `POST`/`GET /v1/pkg/sync` expose `state` (`never`|`running`|`success`|`failed`), `last_attempt`, `added`, `skipped`, `error` — `POST` returns `409` if a sync is already running, `400` if no repo is configured yet.

**Periodic timer**: `arm_pkg_sync_periodic_timer()`/`start_pkg_sync_periodic_timer()` mirror NTP's own re-arming timerfd exactly (task #751) — `sync_interval_seconds == 0` means the timer is simply never armed; changing the interval via `PUT /v1/pkg/repo-config` re-arms it immediately, no daemon restart needed.

**CLI**: `cixctl pkg repo-config show|set [--url=] [--kind=] [--ref=] [--token=|--clear-token] [--sync-interval=]`, `cixctl pkg sync [--wait]`, `cixctl pkg sync-status`.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. New `test/test_pkg_sync.c`: a real, unmocked round trip against a `python3 -m http.server` standing in for a gitea REST endpoint (serving a hand-built tarball at the exact path `build_sync_fetch_request()`'s gitea branch computes — the real `curl` subprocess and real `tar`-extraction code paths run genuinely, only the forge itself is a stand-in, same hermetic-but-real discipline `test_pkg.c`'s own `file://` fetch tests already established). Covers: fresh-daemon unconfigured state, `POST /v1/pkg/sync` with no repo configured (`400`), `PUT /v1/pkg/repo-config` partial-update semantics (a later PUT touching only `sync_interval_seconds` leaves `repo_url`/token untouched), explicit token clear, a first sync (`added=1, skipped=0`), a second sync of the identical repo proving merge/additive semantics (`added=0, skipped=1` — not rejected, not duplicated), and a fetch failure against a nonexistent path (`state=failed` with a real, non-empty `error`, not a silent hang). Full regression sweep (25 tests, including this new one) all pass.

## Consequences

- A Cix host can now be pointed at a shared recipe repository once and stay current with it, instead of every recipe needing an individual manual push — the actual operational gap this part closes.
- github/gitlab support is real code, following each forge's own documented API, but carries an honest caveat until verified against a live account of either kind — a future session with real credentials for one of those forges should close that gap, not assume it already is.
- Sets up Part 3 (package artifact fetch/cache) to reuse this same configured-repo/async-fetch machinery for binaries, and Part 4 (image recipes) to reuse the same sync mechanism for a second recipe kind.
