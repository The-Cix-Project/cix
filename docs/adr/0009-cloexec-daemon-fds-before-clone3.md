# 0009 — CLOEXEC every daemon-owned fd before any clone3() call

## Status

Accepted

## Context

Discovered during Phase 4's verification, via the same discipline ADR-0008 established (stress-test with many runs, don't trust one passing run): `test_daemon.c` was silently taking ~30 seconds per run instead of a fraction of a second, and running it back-to-back with `test_cli.c` eventually blew past a test timeout.

Root cause: `container_create()` calls `clone3()` (ADR-0002, ADR-0003) directly from inside the request-handling path of a client connection. `clone3()` behaves like `fork()` for file descriptors — the new child inherits a duplicate of *every* fd the daemon holds open at that instant, unless that fd is marked close-on-exec. None of the daemon's own fds (the listening socket, each accepted client socket, the `epoll` fd, and the cgroup `O_PATH` fd from `cgroup_create()`) had `CLOEXEC` set. So every container created via `POST /v1/containers` walked away holding its own duplicate of the very client connection socket that triggered its creation.

The consequence: a TCP connection only reaches EOF once *every* reference to it, across every process, is closed. The daemon's own `close(cc->fd)` right after handling the request wasn't enough — the freshly-created container (which might run for seconds, minutes, or indefinitely) held a second, independent reference to that exact connection. The client's `read()`-until-EOF loop (`client/src/httpclient.c`) had no choice but to block until the container itself exited, even though the actual HTTP response had been written and the daemon had moved on to other work. This defeated the entire point of the non-blocking `epoll` reactor for any request that creates a container, silently, without any error or timeout — the request would eventually complete correctly, just arbitrarily late.

## Decision

Every fd the daemon opens itself is created with close-on-exec from the start, not patched afterward with a separate `fcntl(F_SETFD, FD_CLOEXEC)` call:

- Listening socket: `socket(..., SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)`.
- Accepted client sockets: `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`.
- The `epoll` fd: `epoll_create1(EPOLL_CLOEXEC)`.
- The cgroup `O_PATH` fd (`cgroup_create()`): `open(dir, O_PATH | O_CLOEXEC)` — the kernel only needs this fd's value at the `clone3()` syscall itself (`CLONE_INTO_CGROUP`); nothing needs it to remain open in the child afterward.

This closes them automatically at the moment any child (container) `execve()`s, which happens almost immediately after `clone3()` — well before the container does any real work — with no window where the daemon has to remember to clean them up itself.

## Consequences

- **Standing rule for everything built from here on**: any fd the daemon (or any future long-running process in this project — the future networking/DNS/PKI daemons will have the same shape) opens for its *own* bookkeeping must be `CLOEXEC` from creation, specifically because this project routinely `clone3()`s new processes from inside request-handling code. Default to `CLOEXEC`; only omit it with a specific, stated reason.
- This class of bug produces no error, no crash, and no wrong answer — only a silent, severe latency regression proportional to how long the child process runs. Ordinary single-request testing won't catch it; only noticing that something takes 30 seconds that should take milliseconds does. Timing sanity (not just pass/fail) is now part of what "verified" means for anything touching `clone3()` from a long-running daemon.
