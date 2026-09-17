# 0300 — A build slot is validated against the registry, not against the entry that holds it

## Status

Accepted. Issue [#339](https://git.home.arpa/itdlabs/cix/issues/339). Completes [ADR-0157](0157-parallel-package-builds-design.md)'s ten-slot budget and the reap added for [#246](https://git.home.arpa/itdlabs/cix/issues/246); neither is reversed.

## Context

A package build holds one of ten chain slots. Two functions decide when a slot is free, and until now both asked the same question of the same source:

```c
if (e != NULL && (e->state == PKG_STATE_FETCHING || e->state == PKG_STATE_BUILDING))
	continue;   /* still busy -- leave the slot alone */
```

`chain_reap_stale()` runs on every allocation and lookup, and `chain_release_if_job_over()` runs at each completion path. Both read `e->state` — the entry's own account of what it is doing.

That works exactly as long as the entry's account is true. On 192.168.15.95 it was not. A `rtw88-firmware@cix-firmware` build reported success in its own log (`installed 150984 bytes to /lib/firmware/rtw88/rtw8822b_fw.bin`) while its entry stayed at `state: "building"` with `version: ""`, and its image stayed at the empty-manifest hash — so nothing was installed despite the log saying it had been. Ten such entries accumulated, each pinning a slot, and every subsequent `pkg install` and `pkg hostbuild` was refused with `409 all 10 package job slots are in use`. The measured recovery was a reboot, on a host with no shell.

**The reap could not help, because it trusted the field that was wrong.** An entry claiming BUILDING is not evidence of a running build; it is a claim, and the one thing that could contradict the claim was never consulted. `GET /v1/containers` showed `__pkgbuild-2` did not exist at all.

Two things about this are already fixed and are not what this ADR decides. `POST /v1/pkg/cancel` gained a branch (v2.57.161) that drives completion directly when the build container is gone from the registry, which does recover the slot — so #339's "recoverable only by a reboot" has been stale since that tag. And `pkg_build_completed()` already handles a container it cannot match to an entry by clearing the slot. What remained is that **an operator had to know to call cancel**: the recovery existed and nothing reached for it on its own.

The cause of the original stuck entry is still not established. This decision deliberately does not wait for it.

## Decision

**Liveness is a question for the registry, and the registry's answer is authoritative over the entry's.** `chain_reap_stale()` now reclaims a slot whose entry says BUILDING when that entry's build container is absent from the registry, and marks the entry failed rather than leaving it claiming to build.

**main.c answers the question; pkg.c is given the means to ask.** `pkg_set_build_container_live_fn()` registers a predicate immediately after `pkg_init()`. pkg.c has never linked `registry.h` and does not start now — main.c owns every `registry_*` call, which is the same division `handle_pkg_cancel()` already depends on. A NULL predicate means "do not judge liveness", i.e. the pre-#339 behaviour, which is what any binary linking pkg.c without main.c gets.

**Absence is sound as the test, and the reason is an ordering fact.** `container_exit_finalize()` calls `pkg_build_completed()` *before* `registry_remove()`. A container that is gone from the registry has therefore already driven its completion, so there is no window in which it is absent while an exit event for it is still in flight. Without that ordering, reclaiming on absence could hand a slot back from under a job that still owned it — the [#98](https://git.home.arpa/itdlabs/cix/issues/98) hazard `chain_release_if_job_over()` exists to avoid. The ordering is what makes this safe, so it is named in the predicate's own comment rather than left to be rediscovered.

**Present-but-not-running counts as LIVE.** Such a container's exit has not been finalized, and finalization is the thing that will release the slot properly; judging it dead here would race the path that does the job correctly. It is also how a deliberately preserved container behaves ([ADR-0175](0175-pkg-build-keep-on-failure.md)'s `keep_on_failure`), whose slot was already released by the completion that ran before it was kept.

**The entry is fixed, not just the slot.** `pkg_fail()` with the installed record kept, because leaving the entry in BUILDING is the lie that produced the measured state — ten entries all claiming to build one package, with no way for an operator to tell a live build from a dead one. Freeing the slot alone would unwedge the box and leave the report wrong for as long as the daemon ran.

## Alternatives rejected

**Judge a build dead when its container is present but not running.** It would cover one more hypothetical shape of the failure, and it races real finalization to do so. The measured failure is the absent case. Guessing at the other would be inventing a mechanism for a failure nobody has seen, which is how the stale `pkg_cancel()` reasoning this issue also corrected got written.

**Extend the same validation to FETCHING entries.** A fetch's liveness is its `fetch_pid`, which pkg.c can read for itself, so the mechanism would be cheap. No leak of that kind has been measured since [#239](https://git.home.arpa/itdlabs/cix/issues/239) gave cancel a way to kill a stuck fetch, and a second unmeasured mechanism is not worth the surface.

**Give pkg.c the registry.** It would remove the indirection, and it would also remove the boundary that keeps every namespace, cgroup and registry operation in one file. The predicate is one function pointer against that.

**Wait for the root cause of the stuck entry before fixing anything.** The cause is unknown, the symptom took the box's whole package system down, and the fix does not depend on the cause. Holding a recovery back until an unreproducible failure is explained is how the wedge stays reachable.

## Consequences

The failure this addresses becomes self-healing and, more usefully, **observable**: the reclaim writes a warning naming the slot, the package and the reason, so a recurrence of the unexplained stuck-entry cause announces itself instead of presenting as a box that refuses every install. That is the part most likely to establish the cause #339 could not.

**The recovery path has no automated gate, and cannot have one yet.** Fabricating the state needs a build container that vanishes without its exit reaching the handler, which is precisely the unestablished cause. The normal path — a slot allocated, a build completed, the slot released — is exercised by every build the platform does, including the one that ships this change. `test_pkg` is not in `SELFTESTS` (#224, #480), so even a fabricated case would not run in the release gate; that gap is #480's.
