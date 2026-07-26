# 0012 — Atomically-rewritten flat file for network persistence, not a database

## Status

Accepted

## Context

Phase 7 part 1 made networks a dynamic, REST-managed resource rather than one fixed subnet baked into the daemon at compile time — a choice the user made directly when asked whether to keep a small fixed table or expose a real `POST/GET/DELETE /v1/networks` API. That choice has a consequence that doesn't show up just by reading `network.c`: unlike containers, which have been safely in-memory-only since Phase 3 (every one dies with the daemon via `PR_SET_PDEATHSIG`, so a restart has no orphan state to reconcile), a bridge created via this new API is real kernel state that outlives the daemon process entirely. If the daemon forgot about a network on restart, its bridge would keep working at the packet-forwarding level while becoming invisible and unmanageable through the API — a silent, confusing split between "what the kernel is actually doing" and "what the daemon thinks exists." This is the project's first time needing state that must survive a restart, so it's the first time this project has needed real persistence at all.

## Decision

A single flat file, `/var/lib/kanxeo/networks.json`, holding a JSON array of network definitions (name, subnet, prefix length — gateway is derived, never stored independently, so it can never drift from the subnet it belongs to). Every `network_create()`/`network_delete()` call rewrites the *entire* file atomically: write to `networks.json.tmp`, `fsync()`, then `rename()` over the real path. `network_init()` reads it once at startup and idempotently recreates every listed network's bridge (`EEXIST` tolerated on both the bridge and its address, same pattern `ensure_dir()` already established for the storage directories).

Deliberately not a database (SQLite or otherwise): at the scale this project's networks operate at (a handful, created by an operator, not a high-frequency workload), a whole-file rewrite is simple, uses the JSON reader/writer this project already has (`daemon/src/json.c`), and needs no new dependency or schema-migration story — consistent with this project's established preference for hand-rolled minimalism over pulling in a library (ADR-0007, ADR-0010). Deliberately a *full* rewrite on every change rather than an append-only log or incremental patch: at this scale the simplicity of "the file is always a complete, valid snapshot" outweighs the cost of rewriting a few dozen bytes more than strictly necessary, and it avoids an entire class of log-replay/compaction bugs a journal would introduce.

## Consequences

- This is now the reference pattern for any future durable host-state this project needs (a real candidate: Phase 8's DNS records, Phase 9's PKI material) — one flat, atomically-rewritten JSON file per resource type under `/var/lib/kanxeo/`, loaded once at startup, rather than reaching for a database the first time persistence comes up again.
- A malformed or corrupted persisted file is treated as a hard startup failure (`network_init()` returns nonzero, the daemon refuses to start) rather than silently dropping the entries it can't parse — consistent with why this file exists in the first place: silently forgetting a live bridge is exactly the bug this design prevents, so silently forgetting persisted *records* on a read error would be the same bug wearing a different hat.
- `network_create()`/`network_delete()` roll back (deleting a just-created bridge, or refusing to remove a table entry) if the persistence write itself fails, rather than reporting success on a change that couldn't actually be durably recorded — the in-memory table and the file are never allowed to disagree about what "successfully created" means.
