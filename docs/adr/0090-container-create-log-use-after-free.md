# 0090 — container-create failure logging used a dangling JSON-tree pointer

## Status

Accepted

## Context

Raised directly by the user, reading the web dashboard's log view during this session's DNS/networking investigation: a `"container %s: failed to create: Operation not supported"` log line showed garbled bytes instead of the real container name (observed as e.g. `⌐╛╝8` and `K⌐8` for what should have been real, readable container names).

Traced to `daemon/src/main.c`'s container-create handler: `name` is `json_as_string(jname)`, a pointer straight into the request body's own parsed JSON tree (`root`) rather than a copy -- `json_as_string()` never allocates, it hands back a pointer into the tree's own string storage (`daemon/src/json.c`). The success path already avoids this exact hazard, using `entry->name` (the registry's own persisted copy) instead of `name`, with its own comment explaining why. The `REGISTRY_ERR_CREATE_FAILED` branch's `logstore_write()` call was the one remaining spot still using `name` directly, after `json_free(root)` had already run -- a genuine use-after-free, not a hypothetical one: the freed memory had already been reused by the time the log line was formatted, producing exactly the kind of garbled text the user reported.

## Decision

Copy `name` into a fixed `REGISTRY_NAME_MAX`-sized stack buffer (`name_copy`) immediately after `create_errno = errno` (before `json_free(root)` runs), and use that copy in the `REGISTRY_ERR_CREATE_FAILED` branch's `logstore_write()` call instead of the dangling `name` pointer.

## Consequences

- Container-create failure log lines now show the real, intended container name, not freed memory.
- No behavior change on the success path or any other error branch -- this was the one specific spot using the dangling pointer.
- Deployed in the same build as ADR-0089's root-netns `ip_forward` fix (bundled deploy, both in `daemon/src/main.c`) -- kept as a distinct fix and ADR since it's a separate bug with a separate root cause, found and reported independently.
