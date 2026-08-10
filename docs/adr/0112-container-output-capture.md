# 0112 — Opt-in stdout/stderr capture for ordinary containers (closes task #676's own follow-on gap, unblocks task #747)

## Status

Accepted

## Context

`struct container_spec`'s `capture_output`/`stdout_fd`/`stderr_fd` fields (task #652) and `src/container.c`'s `dup2()`-before-`execve()` wiring have existed since Phase 46, but only `pkg.c`'s own build containers (task #653, ADR-0087's incremental epoll-drain discipline) ever set them. An ordinary, operator-created container via `POST /v1/containers` had no way to capture its own output at all.

This gap became a real, named blocker twice, independently:

- **Task #731** (jump box SSH access): while diagnosing why a freshly-created `jumpbox1` container's `sshd -D -e` exited 1 with no visible cause, the only two ways to see *why* — `kanxeoctl console`/the WebSocket exec endpoint, and this capture mechanism — were both unavailable (console/exec turned out to have its own separate, unrelated 500 bug, deliberately set aside rather than fixed opportunistically mid-investigation). Task #731 was ultimately resolved a different way (rendering real Unix accounts, ADR-0111) without needing output capture at all — but the diagnostic gap itself remained real and undirected.
- **Task #747** (glauth's LDAP listener never binds on real hardware, Part 69 of `ROADMAP.md`): every other layer this project owns was independently proven working (config staged byte-for-byte correct, binary well-formed, no capability/seccomp restriction anywhere in `src/container.c`), narrowing the cause to "something glauth itself is doing (or failing to do) that only its own stdout/stderr would explain" — with no way to read it. That investigation's own closing note named the exact fix: "extend `POST /v1/containers` with an opt-in `capture_output` field... then recreate this exact `glauth-diag` container with it enabled and read glauth's own log output directly."

## Decision

Reuse the existing `container_spec` mechanism verbatim — no new dup2/exec-time logic needed, `src/container.c` is untouched — and wire it into the two places it was previously missing:

1. **`POST /v1/containers`**: `create_container_from_body()` (`daemon/src/main.c`) parses a new optional `capture_output` boolean, and — when set — `pipe2(..., O_CLOEXEC)` + `fcntl(..., O_NONBLOCK)` on the read end, following `pkg_build_start()`'s exact pattern (`daemon/src/pkg.c`). The parent's own copy of the write end is closed immediately after `registry_create()` returns (clone3() already gave the child an independent fd-table copy by then), the same discipline `handle_pkg_fetch_event()` already established for its own stdio-relay pipe.

2. **`struct registry_entry`** (`daemon/include/registry.h`) gains `output_fd`/`capture_requested`/`captured_output[4096]`/`captured_output_len` — a per-container destination, distinct from `pkg.c`'s single global build-output buffer, since multiple ordinary containers can have capture enabled concurrently (unlike the "one build in flight" build path). `register_container_output()`/`handle_container_output_event()` (`daemon/src/main.c`) mirror `register_pkg_build_output()`/`handle_pkg_build_output_event()` exactly — incremental epoll-driven drain (ADR-0087's discipline again: never a single blocking read at exit, which could deadlock a still-running child against a full 64KB kernel pipe buffer), appending into the bounded buffer (oldest bytes kept, newest dropped past the 4096-byte cap — generous enough for a real startup failure's own diagnostic text, bounded so one runaway-logging container can't grow a registry entry unbounded) rather than a shared global.

3. **`GET /v1/containers/{name}`** gains a `captured_output` field: the string if capture was requested (empty string if nothing written yet), `null` (not `""`) if it wasn't — the two are deliberately distinguishable, since "opted out" and "opted in but silent so far" are different facts a caller needs to tell apart.

A real bug caught during this work, worth recording since it's the kind of thing easy to reintroduce: the epoll dispatch loop's own `if (cc->kind == CONN_PKG_BUILD_OUTPUT) ... else if (...)` chain in `main.c`'s event loop had no corresponding branch for the new `CONN_CONTAINER_OUTPUT` kind — the conn was registered with epoll correctly, but its events were silently never dispatched to a handler, so the pipe filled its kernel buffer and captured_output simply never updated. Caught by a live test (`test_container_lifecycle.c`'s new step 10), not by inspection.

## Consequences

- A capture-enabled container costs one extra pipe fd and up to 4KB of daemon memory for its lifetime — opt-in, default `false`, so the common case is unaffected.
- Not a live-tail mechanism (see `GET /v1/pkg/build/log`'s WebSocket relay, ADR-0101, for that shape) — a diagnostic snapshot read back after the fact, which is what both motivating use cases (#731, #747) actually needed: "what did this process say right before it gave up," not "watch it run in real time."
- `container_decode_exit_status()`'s own [141,255] exit-code-range ambiguity (a separate, real observability gap found but deliberately not fixed during the #731 investigation — a legitimately-exec'd program's own exit code in that range gets mislabeled as an overlay/exec failure) is now easier to work around operationally (read the real captured output instead of trusting the label), though still not fixed at the source. Left as its own, separate follow-on.
