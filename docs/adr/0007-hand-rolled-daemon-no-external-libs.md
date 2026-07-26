# 0007 — Hand-rolled HTTP/JSON/reactor in the daemon, no external libraries

## Status

Accepted

## Context

Implementing the REST daemon meant choosing how to handle HTTP parsing, JSON encoding, and the event loop. The mission already locks in `epoll` over `io_uring` for the event loop (no glibc wrapper for `io_uring`, a heavier hand-rolled-uapi lift than warranted yet). The remaining question was whether to pull in an HTTP/JSON library or write both by hand, and how much concurrency model complexity to take on for a first version.

## Decision

Everything is hand-rolled in plain C, consistent with the project's "no parallel implementations / build the dependency yourself" ethos already applied to the container runtime and networking plane:

- **HTTP** (`daemon/src/http.c`): request-line + headers + fixed-length body only. No chunked transfer-encoding, no keep-alive/pipelining — every response closes the connection. A deliberate v1 scope boundary, not an oversight: a control-plane API for container lifecycle doesn't need HTTP/1.1's full feature set to be correct.
- **JSON** (`daemon/src/json.c`): a small generic recursive-descent parser and buffer-based writer, not per-endpoint ad hoc string building. `\uXXXX` escapes are unsupported on the parse side (no field in this API needs one) — an explicit, documented boundary.
- **Concurrency**: single-threaded, non-blocking, `epoll`-driven reactor. No thread pool, no per-connection thread. Response writes assume the client fd is put back into blocking mode first (since responses are small and the connection closes right after) rather than building full non-blocking-write buffering — a slow/malicious reader stalling the reactor is an accepted v1 limitation.
- **Container registry** (`daemon/src/registry.c`): a fixed-size in-memory table, not a database or on-disk journal. Safe because `container.c`'s existing `PR_SET_PDEATHSIG` means every running container dies automatically if the daemon exits — there's no orphaned-state-to-reconcile problem that persistence would be solving.

## Consequences

- No third-party dependency footprint anywhere in the daemon, matching "100% custom" for the parts of the stack the mission calls out explicitly (toolchain, networking) extended here to the control-plane daemon too.
- Each of the scope boundaries above (no keep-alive, no `\uXXXX`, in-memory-only registry, blocking-write-on-respond) is a real v1 limitation to revisit deliberately if a later phase needs more — not a silent gap. None of them were hit by Phase 3's own verification (`docs/ROADMAP.md` Phase 3), but a future phase (e.g. one needing large log streaming) may need to revisit the blocking-write assumption specifically.
