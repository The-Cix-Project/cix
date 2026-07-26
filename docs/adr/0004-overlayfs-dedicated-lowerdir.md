# 0004 — OverlayFS lowerdir is a dedicated, purpose-built tree

## Status

Accepted

## Context

The mission requires the host OS to act as the shared, read-only lowerdir for every container, with running instances owning only their upperdir diff. Phase 1's harness predated real image layering and stood in a crude substitute: it bind-mounted the *live* development container's own `/` as the container's root. That meant a running container's writes landed directly on the real host filesystem — never the intended end state, and a real safety gap (a container should never be able to mutate the live host it's developing on).

Two paths forward at Phase 2: (a) build a dedicated, purpose-built rootfs tree that this project owns and grows itself, fully decoupled from the dev environment, or (b) keep bind-mounting the live dev container's root, just now through a real overlay mount instead of a raw bind.

## Decision

(a). The lowerdir is always a dedicated tree under a path the container runtime owns (`/var/lib/kanxeo/images/<image>/rootfs` as of Phase 3 — see `docs/ROADMAP.md` Phase 3), populated deliberately and never auto-created empty by our own code (an auto-created-but-empty lowerdir would mean a silently-broken container, exactly the kind of stop-gap this project forbids). It starts minimal — just enough content to prove the mechanism (a binary plus the dynamic linker/libc it needs) — not a full debootstrapped userland; building out real OS content from source is separate, later scope, not something to smuggle into "prove OverlayFS works."

Phase 1's `test_harness.c` still exercises `lowerdir="/"` as a special case, to preserve its original full-host-visible verification without redesigning an already-passing test — but this is that one test's own scope, not the general contract.

## Consequences

- Every container creation path (the Phase 3 daemon, and everything built on it) must go through `overlay_create()` with a real, pre-populated lowerdir; there is no "just bind-mount the host" fallback anywhere outside that one legacy test.
- Building a real base OS image (libc, coreutils, init, shell — all eventually from source, per the mission) is real, substantial future work, explicitly not done yet and not implied by anything built so far.
