# 0055 — Container file-read REST endpoint

## Status

Accepted

## Context

thinC's own `files[]` mechanism (POST /containers, ADR-0030) is write-only: a client can stage a file into a container's rootfs before `clone3()`, but there was never a way to read one back out over the API. This surfaced while planning a self-hosted kernel/control-plane build pipeline that needs to pull a freshly built artifact out of a build container — but it's independently useful on its own (inspecting a config file, a log, a generated cert, without opening a full interactive `console` session and `cat`-ing it through a terminal renderer).

The write path already has a real precedent for how to reach a container's files safely and cheaply: ADR-0013 established `/proc/<pid>/root/<path>` (the kernel-resolved "magic symlink" through a live process's own mount namespace and root) for writing into a *running* container without `setns()`. That precedent only covers the running case, though — `dns.c`/`pki.c`'s own write paths never had to consider an exited container, since neither ever needed to touch one. This endpoint does: `GET .../stats` (ADR-0054) already established that an exited-but-not-removed container (`registry_mark_exited()`, `running == 0`, entry still present) is a legitimate target for other GETs, and a file-read endpoint should honor the same convention rather than only supporting the running case.

## Decision

**`GET /v1/containers/{name}/files?path=<path>` — raw bytes, not JSON.** This is only the second non-JSON response body this daemon has ever produced (the first being `staticfile.c`'s own static asset serving for the web dashboard) — reuses the same `http_write_response()`/`http_set_blocking()` primitive, `application/octet-stream` content type, whole-file-into-a-`malloc()`-buffer read (no streaming/chunking — every file this daemon will ever serve this way is small, matching `staticfile.c`'s own posture).

**Path resolution branches on liveness, not on a single universal lookup:**
- **Running** (`registry_entry.running == 1`): `/proc/<pid>/root<path>` — the exact ADR-0013 pattern, no `setns()`.
- **Exited-but-registered** (`running == 0`, entry still present — only `DELETE` calls `registry_remove()` and truly tears it down): `<CONTAINERS_DIR>/<name>/upper<path>` first (the real, host-visible overlay upperdir — still on disk even though the *mounted* merged view was namespace-local and died with the last process in it), falling back to `<IMAGES_DIR>/<entry->image>/rootfs<path>` (the shared, read-only lowerdir, since `entry->image` survives exit).

A stale `registry_entry.handle.pid` is never trusted once `running == 0` — PIDs get reused by the kernel, so `/proc/<pid>/root` would silently read some unrelated process's files. This is the same reasoning ADR-0013 implicitly relies on for the write path (it only ever touches a *known-live* target), made explicit here because this endpoint is the first place a stale-pid read would actually be reachable.

**Path validation reuses `file_path_is_safe()` verbatim** — the same function `POST /containers`' own `files[].path` staging already validates against (`path[0]=='/'`, no `.`/`..`/empty component). One validator, two call sites, not a second set of rules that could quietly drift from the first.

**A path resolving to a directory is a 400, not a listing.** This endpoint reads one file; browsing a container's rootfs as a directory tree is a different, unbuilt feature (see Consequences).

## Consequences

- The CLI (`thincctl files get NAME --path=... [--output=PATH]`) needed no new HTTP client primitive: `kx_client_request()` already captures a response's raw bytes into `struct kx_response.body`/`body_len` regardless of Content-Type (`json` is simply `NULL` when the body isn't valid JSON) — reused as-is rather than adding a redundant `..._raw()` sibling function.
- No streaming/range support (`Range:` header, partial content) — every file this daemon has any reason to serve this way is small; there's no precedent for chunked responses anywhere in this codebase, and none was added here either.
- No directory listing. An operator wanting to browse a container's rootfs still needs `console` + `ls`; this endpoint only ever answers "give me exactly this one file's bytes."
- No new authentication/authorization boundary — this endpoint is exactly as protected as every other one here (network reachability only), same as ADR-0043 already noted for `console`.
- Query-string parsing (`url_query_param()`) is new to this daemon — deliberately narrow (one key, `%XX`-only decoding, no repeated-key/array semantics) rather than a general-purpose parser nothing else needs yet. If a future endpoint needs a second query parameter, this is the function to extend, not duplicate.
