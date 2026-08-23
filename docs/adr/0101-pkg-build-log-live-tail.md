# 0101 — GET /v1/pkg/build/log live-tails an in-flight build's own output

## Status

Accepted

## Context

Task #676: a package build's real-time stdout/stderr had no REST-visible path while the build was still running. ADR-0087 already fixed the deadlock risk by having `daemon/src/pkg.c` epoll-drain the build container's output pipe incrementally into a fixed, sliding-window tail buffer (`g_build_output_captured`, `PKG_BUILD_OUTPUT_CAPTURE_MAX` = 3800 bytes) — but that buffer was only ever read back out by `pkg_build_completed()`, and only on a *failed* build (`exit_status != 0`), to fold into the logged error. A successful build's captured output was simply discarded (`g_build_output_captured_len = 0` reset at the next build). An operator watching a slow build (e.g. `perl`/`gcc` from source, both real, multi-minute builds this project has hit) had no way to see it happening — only a final pass/fail, or (on failure) a static post-mortem snippet.

The existing container console (`GET /v1/containers/{name}/console`, ADR-0043) looked like it might already cover this "for free" by pointing it at the build container's own name (`PKG_BUILD_CONTAINER_NAME`, `"__pkgbuild"`), but doesn't: the build container's stdout/stderr are a **plain pipe** (`spec_out->stdout_fd = output_pipe[1]`, `pkg.c`), not a PTY, and `exec_into_container()` (`daemon/src/exec.c`) doesn't attach to an existing process's I/O at all — it `setns()`s into the target's namespaces and spawns a **brand-new** process on a freshly allocated PTY. Pointing console at `__pkgbuild` mid-build would just open an unrelated interactive shell inside the same namespaces, with zero visibility into the actual running `pkg_build()`/`pkg_install()` script's output stream.

## Decision

A new, narrower WebSocket endpoint, `GET /v1/pkg/build/log`, purpose-built as a one-way relay rather than reusing the console's exec/PTY machinery (which solves a structurally different problem — attaching an interactive shell to a running container's namespaces, not relaying an existing pipe's bytes):

- `pkg_build_output_readable()` (`daemon/src/pkg.c`) gains an optional `new_data`/`new_data_cap`/`new_data_len` out-parameter — the exact raw bytes read *this call*, separate from the existing trimmed tail buffer it still maintains for the failure-log case. A new `pkg_build_output_snapshot()` copies the current tail out for a client attaching mid-build, so it isn't starting blind.
- `daemon/src/main.c` gains `CONN_PKG_BUILD_LOG_WS` and a small fixed attach table (`g_build_log_ws_conns[]`, capacity 4 — "a few operators watching the same build," same soft-reservation sizing precedent this codebase already uses for single-job-slot state). `try_pkg_build_log_upgrade()` mirrors `try_console_upgrade()`'s handshake validation (`Upgrade`/`Connection`/`Sec-WebSocket-Key`/`-Version` headers, the same `ws_compute_accept()`/101 response), but is a fraction of the size: no `exec_into_container()`, no `console_exec_session`, no PTY-half conn — `cc` itself is repurposed in place as the entire session, since there's no second fd to pair it with. On attach: 404 if no build is currently in flight (`registry_find(PKG_BUILD_CONTAINER_NAME)`), 503 if the attach table is already full, otherwise the current snapshot is sent immediately, then `handle_pkg_build_output_event()` broadcasts each newly-drained chunk to every attached conn as it arrives, and sends a real WS close frame (tearing every attached conn down) once the pipe reaches EOF.
- `client/src/console.c` gains `thinc_pkg_build_log_run()`, sharing `do_ws_handshake()` (generalized from `thinc_console_run()`'s own handshake, which previously assumed a `/v1/containers/{name}/console` path) — no raw terminal mode, no stdin relay, just print each frame to stdout. `thincctl pkg build-log` is the new CLI entry point.

## Consequences

- An operator can now watch a real build's output as it happens (`thincctl pkg build-log`, or the equivalent raw WS connection), not just after the fact.
- No behavior change to the existing failure-log path (`pkg_build_completed()`'s own read of `g_build_output_captured` on a failed build) — the new out-parameter is purely additive, `NULL`/`0`/`NULL` there preserves the exact prior behavior.
- `PKG_BUILD_LOG_WS_MAX` (4 concurrent viewers) is a soft cap matching this project's own "one build in flight at a time" invariant — a 5th attempt gets a clean 503, not silently dropped or queued.
- New permanent test: `test/test_pkg_build_log.c`, a real end-to-end run (real daemon, real build container, real WS handshake spoken by hand over a raw socket, same precedent `test/test_console_exec.c` already set) — proves a 404 with no build in flight, a 400 on a missing `Upgrade` header, live incremental marker delivery strictly before a real build's completion, a genuine WS close frame once it finishes, and a fresh 404 again afterward (the attach table and output pipe were actually torn down, not left dangling).
- Full local regression sweep clean, zero compiler warnings.
- Deliberately scoped to `pkg install`/`pkg hostbuild`'s own build-output pipe only. `daemon/src/main.c`'s structurally identical `CONN_BOOTROOT_OUTPUT` (mkbootroot's own captured output, also epoll-drained per ADR-0087) is a natural, later follow-on for the same live-tail treatment, not done here — kept out of scope to land this one micro-step cleanly rather than widen it mid-flight.
