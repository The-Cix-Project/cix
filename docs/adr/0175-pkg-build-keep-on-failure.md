# 0175 — `keep_on_failure`: preserve a failed build container for real debugging (issue #35)

## Status

Accepted

## Context

Diagnosing issue #32 (the GCC 16 stage2 bootstrap's own deterministic `genpreds` segfault) hit a hard wall: `pkg_build_completed()` tears down a failed build container's registry entry immediately, unconditionally, the instant it exits — so a real, reproducible crash inside a build had no post-mortem artifact left to inspect. The daemon's own captured build output (`e->build_output_captured`, ADR-0087) only ever holds the build script's own stdout/stderr tail, never the actual crashing binary, its shared libraries, or whatever partial object tree existed at the moment of failure.

The user asked directly: "why don't we build a debug and trace mechanism for Cix?" — a permanent, reusable capability, not another one-off manual reproduction.

Before designing anything, the actual scope was checked rather than assumed: a debugger reachable via `console exec` was the first idea, but real GDB 16.2 source was fetched and inspected directly (485 `.cc` files, 0 `.c` files under `gdb/`) — modern GDB is 100% C++, which TCC categorically cannot build, and building it with a real g++ would need exactly the working C++ compiler issue #32 itself is trying to produce (a genuine circular dependency for this specific use case). That ruled out a `gdb.recipe`.

What's actually needed turned out to already exist, twice over: `GET /v1/containers/{name}/files?path=...` (ADR-0055) already reads a file's raw bytes out of a container's rootfs for both a running container (`/proc/<pid>/root/`) and an exited-but-still-registered one (its overlay upperdir, falling back to the image rootfs) — and `DELETE /v1/containers/{name}` already tears one down cleanly. The only real gap was that a failed build container never survives long enough to be addressed by either.

## Decision

Add an opt-in `keep_on_failure` boolean to `POST /pkg/install`, `POST /pkg/hostbuild`, and `POST /system/kmod-build` (default `false`, unchanged behavior for every existing caller). When set, and the chain's own final job (the package actually requested, never an incidental dependency failing mid-chain) fails with a nonzero exit, `pkg_build_completed()` (`daemon/src/pkg.c`) reports this back via a new `out_kept` parameter, and `main.c`'s `handle_container_event()` skips its own `registry_remove()` call for that one case — leaving the exited build container exactly as an ordinary, addressable, registered container instead of tearing it down.

No new registry state, no new teardown mechanism: the preserved container is indistinguishable from any other exited container to every other code path. Cleanup is the completely ordinary `DELETE /v1/containers/{name}` — `pkg_build_completed()` is never re-entered for it (confirmed by reading `handle_delete()` directly: it calls `registry_remove()` only, no pkg.c callback at all). The preserved container's own name is reported back as the failed `PkgEntry`'s new `kept_build_container` field (cleared at the start of every fresh attempt on that entry, so a stale name never survives past the next try).

`keep_on_failure` never applies to a dependency that fails mid-chain (`is_final` gates it) — an incidental prerequisite failure is the ordinary, uninteresting case; only the package the caller actually asked to debug gets preserved.

## Consequences

- `daemon/include/pkg.h`/`daemon/src/pkg.c`: `pkg_install_start()`/`pkg_hostbuild_start()` gain a `keep_on_failure` parameter; `pkg_build_completed()` gains an `out_kept` out-parameter; `struct pkg_chain` gains `keep_on_failure`, `struct pkg_entry` gains `kept_build_container`. Every pre-existing call site (the automatic `pkg update-all` rebuild, the manifest-driven auto-install path) passes `0` explicitly — this is opt-in, operator-driven debugging only, never something an automatic system-triggered rebuild should do.
- `daemon/src/main.c`: `handle_container_event()`'s unconditional `registry_remove()` for a `pkgbuild-N` container becomes conditional on `!kept_build_container`. `handle_stop()` (an explicit operator `POST .../stop`) is deliberately unaffected — a manually-killed build is always torn down regardless of `keep_on_failure`, which only ever concerns an unprompted build failure.
- REST: `keep_on_failure` on the three POST bodies above; `kept_build_container` (nullable) added to `PkgEntry`.
- CLI: `--keep-on-failure` on `pkg install`, `pkg hostbuild`, and `kmod-build`.
- `test/test_pkg.c` step 18: a real build-container failure (not a fetch/checksum failure — a genuine container spawns, extracts real source, then `pkg_build()` deliberately `exit 1`s), proving both directions — without the flag, immediate teardown exactly as before; with it, the container survives, `GET .../files?path=/build/src/hello.c` reads real content back out of it, and an explicit `DELETE` cleans it up afterward, with a follow-up `GET` confirming it's actually gone.
- Local regression suite (`test_pkg`, `test_kmod_build`, `test_pkg_build_log`, `test_pkg_concurrent_stress`, `test_pkg_sync`, `test_pkg_cache`, `test_container_files`, `test_cli`) all pass. Not yet exercised live against 192.168.15.95 — the actual motivating use case (a real, deliberately-forced gcc/kernel bootstrap failure, inspected via this mechanism) is follow-on work, not part of this change.
