# 0100 — GET /containers echoes cmd, sourced from registry_entry (live) or the persisted body (stopped)

## Status

Accepted

## Context

Task #675: a container's own entrypoint `cmd`/argv was write-only. `POST /v1/containers`' `"cmd"` field was parsed into `argv_buf[]`, assigned onto `spec.argv`, and handed to `container_create()` to `execve()` — but `spec.argv` only ever pointed into the create request's own parsed JSON tree (`root`), which is `json_free()`'d immediately after `registry_create()` returns (`daemon/src/main.c`, the comment at the `json_free(root)` call site already says as much). Nothing copied it anywhere durable: `struct registry_entry` (`daemon/include/registry.h`) had no `cmd` field at all, `registry_write_json_one()` never emitted one, and `struct container_def`'s own persisted `body` (the full original create request, kept for `restart != "no"` replay) was never read back out for display either — `write_stopped_def_json_one()` (`daemon/src/containerdef.c`) parsed `image` out of it but nothing else. An operator asking "what is this container actually running" had no REST-visible answer at all, live or stopped.

## Decision

Two sources, matching where each kind of container's data already lives — no new parallel storage:

- **Live/recently-exited containers**: `struct registry_entry` gains `cmd[CONTAINER_MAX_ARGV][CONTAINER_ARGV_MAX]` + `cmd_count`, copied from `spec->argv` inside `registry_create()` the exact same way `interfaces[]`/`sysctls[]` already are (read directly off `spec`, no new parameter). `CONTAINER_MAX_ARGV`/`CONTAINER_ARGV_MAX` are new named bounds in `include/container.h`, replacing the bare `argv_buf[64]` literal `daemon/src/main.c`'s own request parser already had (one source of truth for the bound, not just the storage). `registry_write_json_one()` emits it as a `"cmd"` array.
- **Genuinely stopped containers** (registry entry removed, only a `container_def` remains): `write_stopped_def_json_one()` now parses `"cmd"` back out of `d->body` (the full original request, already kept and already re-parsed in this same function for `image`) and echoes it the same way — no second copy of the argv is stored; the existing persisted body is already the one source of truth for a stopped container's original request.

## Consequences

- `GET /containers`/`GET /containers/{name}` now report `cmd` for every container in every state, sourced from whichever of the two already-existing stores actually applies — no new persistence mechanism, no duplication between them.
- `kanxeoctl`'s container listing gained a trailing `cmd=...` column (space-joined argv), and the web dashboard's container JSON now carries the field for any future UI use.
- `CONTAINER_MAX_ARGV` (64) matches the pre-existing (previously unnamed) parse-time cap exactly — no behavior change to what a create request can contain, only to what's now retrievable afterward.
- Full local regression sweep clean, zero compiler warnings.
