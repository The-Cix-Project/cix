# 0005 — API-First Mandate: the REST daemon is the only path to the runtime

## Status

Accepted

## Context

The original roadmap had a "minimal container CLI" as Phase 3, wrapping `container_create()`/`container_wait()` directly. Partway through, the requirement was sharpened: this project is meant to be 100% API-spec-anchored — one REST API, with the CLI and any future web dashboard as clients of it, no exceptions. A CLI (or web UI) that links the runtime library directly would mean two independent paths to the same capability (the direct library call, and whatever the API eventually exposes), which is precisely a parallel implementation, and an easy way for the two to drift out of sync over time.

## Decision

The REST daemon (`daemon/`, see `docs/ROADMAP.md` Phase 3) is the **only** process with direct access to `include/container.h` or any other host/network/DNS/PKI primitive. Every capability the CLI or web dashboard exposes must exist as a REST endpoint *first* — a client-side feature with no corresponding endpoint is not allowed to exist. This is recorded as a durable rule in `CLAUDE.md`, not just a one-time phase choice, because it constrains every phase from here on (networking, DNS, PKI), not only containers.

This reordered the roadmap: the REST daemon had to move ahead of the CLI, since the CLI now depends on the daemon's API existing rather than the runtime library directly.

## Consequences

- Every new host capability requires, in order: an API contract addition (`docs/api/openapi.yaml`), a daemon handler, and only then a CLI/web feature. Slower for a single quick feature, but there is exactly one control plane and one source of truth for what the host can do.
- The CLI and web dashboard (Phases 4–5) will be thin HTTP clients with no namespace/cgroup/mount/rtnetlink logic of their own — see ADR-0007 for what the daemon side of that contract looks like.
