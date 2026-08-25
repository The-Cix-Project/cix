# 0153 — live, ephemeral single-file container updates

## Status

Accepted

## Context

Investigating "where in the image recipe do deps/restart-policy/dns/sysctl/files/cmd live" (the same question that led to ADR-0151's container recipes) surfaced a related, narrower gap: task #861 asks for the ability to update a container's `cmd`/files/network/options *without recreating it*. Recreating a container is already possible (`DELETE` + `POST` with an edited body, or applying an edited container recipe per ADR-0151) but is destructive to the running process and any non-`files[]`-persisted state.

The one piece of this scoped and built here is **file content**: writing or overwriting a single file inside an already-existing container, running or not, in place. `GET /v1/containers/{name}/files?path=...` (ADR-0055) already reads a file this way; there was no write counterpart.

The rest of task #861's original scope (`cmd`, network, and other option updates without recreate) is deliberately **not** part of this decision — those touch the container's core process/namespace identity in ways a plain file write does not, and need their own design pass. This ADR covers files only.

## Decision

**`PUT /v1/containers/{name}/files?path=/abs/path`**, body `{"content":"...", "mode":"0644", "owner":N, "group":N}` (`content` required, the rest optional — same shape and defaults as a `files[]` entry in `POST /v1/containers`). Responds `204 No Content` on success.

Resolves the target path exactly like the existing read side does, by run state:
- **running**: `/proc/<pid>/root<path>` — kernel-resolved through the container's own mount namespace. Writing here triggers the overlay's own copy-up, so the result lands in the container's real, host-visible `upper/` directory, same as any process inside the container writing to that path would.
- **not running** (stopped, exited, or never started): `<containers-dir>/<name>/upper<path>` directly, matching `handle_container_file_read()`'s own already-established fallback.

Reuses the exact open/write/`fchmod`/`fchown` sequence `handle_create()`'s own inline `files[]`-staging loop already uses, and the same `file_path_is_safe()` traversal check the read side already enforces (`..` components and a missing leading `/` both rejected with 400, matching `GET`'s existing behavior byte-for-byte).

**Deliberately LIVE and EPHEMERAL, never persisted into the container's own definition.** A `PUT` here does not touch the container's stored `files[]` body at all — a future restart or recreate replays the *original* persisted definition unchanged, with no memory of this write. This is a real, permanent design boundary, not an oversight: merging one ad-hoc file write into an arbitrary already-persisted `files[]` JSON array (dedup by path, handle removal, keep it consistent across repeated writes) is real complexity this feature does not need to take on. The durable path — "this change should survive a recreate" — is a container recipe (ADR-0151): edit the recipe, re-apply it. `PUT .../files` is for the other case: a quick live patch (updating a config file to unstick a running service, dropping in a diagnostic script) that intentionally does not want to become part of the container's permanent definition.

**CLI**: `cixctl files put NAME --path=/some/path --file=LOCAL_PATH [--mode=0644]` — reads a local file and PUTs its content, mirroring `files get`'s own existing shape.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings.

**A real use-after-free bug was caught and fixed during this feature's own verification, before it shipped.** The first implementation read `content` as a pointer straight out of the parsed JSON tree (`json_as_string(json_object_get(root, "content"))`) and then called `json_free(root)` *before* using that same pointer in the subsequent `write()` — so by the time `write()` ran, `content` pointed into already-freed memory. This was not a theoretical concern: a real end-to-end test wrote a 17-byte string and read back 17 bytes of garbage (`de21 592c 0000 0000 a2c5 69b0 6b3e 86fa 0a`) instead of the actual content. Fixed by taking an owned `strdup()` copy of the content before freeing `root`, and freeing that copy explicitly on every return path.

New permanent coverage in `test/test_container_files.c` (scenarios 8-9): a full write/read round-trip against a *running* container (proving the `/proc/<pid>/root` + overlay-copy-up path, with a direct `stat()`/`fopen()` check against the real host-visible `upper/` file for content, mode, and owner/group, plus a follow-up `GET` proving the daemon's own read path sees the identical bytes), an overwrite-replaces-not-appends check, and the same traversal/no-leading-slash/missing-content/unknown-container rejection cases the read side already has — plus a write/read round-trip against a container that has already exited on its own (the `!running` / direct-`upper/` branch). Full regression sweep (`test_daemon`, `test_cli`, `test_web`, `test_container_lifecycle`, `test_images`, `test_container_restart`, `test_container_recipe`, `test_hostauth`) clean afterward.

## Consequences

- Operators/agents get a genuine "patch one file live" capability with no recreate, no downtime for anything not touching that specific file, and no risk of accidentally making an ephemeral debugging change look like it's part of the container's permanent definition.
- The live/persisted split is now a real, two-tool answer to "how do I change a file in a container": `PUT .../files` for right-now, a container recipe for forever — an operator has to consciously pick, which is the intended trade-off, not an inconsistency to smooth over later.
- `cmd`, network attachment, and other non-file option updates without recreate remain unimplemented — task #861 stays open for that remaining scope.
