# 0086 — stopping __pkgbuild via POST .../stop left pkg.c's job lock stuck forever

## Status

Accepted

## Context

Found live, while diagnosing a stalled `perl` build on 192.168.15.95 (host-tools bootstrap, ADR-0084/0085's own follow-on work): `cpu.usage_usec` on the `__pkgbuild` container froze completely (identical value across 30+ seconds, confirmed via repeated `cixctl stats`), with the host's own `load1`/`load5`/`load15` correspondingly near zero — a genuine stall, not a normal lull between compile steps. `POST /v1/containers/__pkgbuild/stop` was used to kill it (the only available recovery primitive), and it worked correctly at the container level: `ps` and `inspect __pkgbuild` both confirmed the container was fully gone.

But every subsequent `pkg install` then failed with a persistent `409 another package install is already in progress` — even seconds later, even after the container had been confirmed gone for a while. Root cause, found by reading `handle_stop()` (`daemon/src/main.c`) against `pkg_build_completed()` (`daemon/src/pkg.c`): `handle_stop()` kills and reaps a container *directly* — it explicitly removes the pidfd from epoll before calling `registry_remove()` (SIGKILL + a synchronous `waitpid`), specifically so this synchronous path doesn't race a live epoll event for the same fd. That's correct for its own stated purpose (an ordinary user container), but it means `handle_stop()` **never goes through `handle_container_event()`** — the epoll-driven callback that's the *only* place `pkg_build_completed()` normally gets invoked, and therefore the only place `pkg.c`'s own `g_current_job_name`/`g_dep_queue_*`/`g_build_output_rd` state ever gets cleared.

`__pkgbuild` (`PKG_BUILD_CONTAINER_NAME`) is a reserved, daemon-internal name — nothing in this project had ever manually `stop`ped it before this session, so this gap had never been exercised. `handle_stop()` was written purely with ordinary user containers in mind (its own comment talks about restart policy and DNS/PKI ownership, nothing about `pkg.c`'s job state) and had no reason to know that this one specific name needs extra cleanup.

## Decision

`handle_stop()` now checks whether the stopped name is `PKG_BUILD_CONTAINER_NAME`, and if so calls `pkg_build_completed()` itself — the exact same function the normal exit path would have called, with the exact same `exit_status` `registry_remove()`'s own `registry_mark_exited()` call already wrote onto the (still-valid, only-just-marked-not-in-use) registry entry. No new mechanism: this reuses `pkg_build_completed()`'s own existing cleanup and chaining logic verbatim, the same way `handle_container_event()` already does for the organic-exit case. A manually-killed build's `exit_status` is never `0`, so `pkg_build_completed()`'s own success-only dependency-chaining branch correctly never fires from this path.

## Consequences

- Manually stopping a stuck `__pkgbuild` build now correctly frees `pkg.c`'s own job lock — a subsequent `pkg install`/`pkg hostbuild` can proceed immediately instead of being wedged behind a `409` with no REST-visible recovery path at all (confirmed live: before this fix, the only way out would have been restarting `cixd` itself).
- This is a distinct bug from the already-tracked #668 ("pkg hostbuild has no upgrade/force path, bare 409") — that one is about *retrying an already-completed* job; this one is about a job whose own container was killed out from under it through a code path that never told `pkg.c` it happened.
- General lesson: any future "kill this specific container right now" mechanism needs to check whether the target is one of this project's own reserved/internal container names (`__pkgbuild` today, potentially others later) and route through that subsystem's own completion handling — a generic `registry_remove()` is correct for tearing down the *container*, but not sufficient on its own when a separate subsystem (`pkg.c`) is tracking state keyed to that container's lifecycle.
