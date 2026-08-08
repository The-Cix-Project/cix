# 0080 — Container creation/exit diagnostics visibility (real errno text, not just numeric codes)

## Status

Accepted

## Context

Directly requested by the user during the live investigation of the 192.168.15.95 container-creation regression (ADR-0079): after redeploying the cgroup delegation fix, a *new*, more severe symptom appeared on that box — every container after the first one failed to exec, and there was no way to tell why beyond a bare `exit_status` integer. The user's own framing: *"what commands are we missing to help make our diagnostics easier ... let's close the visibility gap, re-roll-out kanxeo with them, and continue digging."*

This project already had a rich, deliberately-designed numeric exit-code encoding for a container's own child process (`src/container.c`: 110-119 for named pre-exec setup stages, 130-136 for `overlay_create()`'s six named steps, 141-255 for a real `mount(2)`/`execve()` errno sharing one range) — but the *only* place any of that was ever explained in human terms was a `perror()` call writing to this process's own stdout/stderr, which on a real installed box (`kanxeod` as PID 1, no attached console, no systemd journal — every deployment target this project actually ships to) reaches nobody. The numeric code alone is also genuinely ambiguous in one real case: `overlay_create()`'s own `mount(2)` errno and the final `execve()`'s errno share the same 141-255 range by design (they're mutually exclusive within one run), so a bare exit code like `142` cannot say which of the two actually failed, or distinguish either from a real container command that happens to exit with that same byte value on its own.

A second, related gap existed one level up: `registry_create()` collapsed *every* `container_create()` failure (a missing image, a cgroup controller the kernel never delegated, anything) into one generic `REGISTRY_ERR_CREATE_FAILED`, which `create_container_from_body()` (`daemon/src/main.c`) turned into an undiagnosed `"failed to create container"` 500 — already flagged as a known follow-on in ADR-0079's own Consequences section, closed here.

A third, independent instance of the same class of gap: `spawn_kanxeo_bootroot_assembly()`'s own child (the `mkbootroot` process a `kanxeo` hostbuild's automatic bootroot assembly spawns) only ever logged its bare exit status (`"kanxeo bootroot assembly: mkbootroot exited 1"`, ADR-0078/Part 23) — the still-unresolved `mkbootroot exited 1` failure discovered during that session's own final validation has stayed unsolved specifically because nothing captured *what* `mkbootroot` itself printed before failing.

## Decision

**A real, always-on diagnostic channel for a container's own pre-exec/exec failures**, not opt-in like the pre-existing `capture_output` feature (a different, existing mechanism: it captures the *exec'd program's own* stdout/stderr for build logging, and only starts right before the final `execve()` — too late for every setup step this covers).

- `container_create()` (`src/container.c`) opens an `O_CLOEXEC` pipe before `clone3()`. Every one of the child's own pre-exec failure paths (`mountns_make_private`, `overlay_create`'s six named steps plus a real mount(2) errno, `mountns_pivot`, `container_dev_mknod`, `sethostname`, network configuration, `prctl`, and the final `execve()` itself — argv[0] included in that last one's message) now writes a real `"step: strerror(errno)"` line to it via a new `child_diag()` helper, replacing the old bare `perror()` calls. `O_CLOEXEC` means the write end vanishes on its own at a successful `execve()` — no extra code needed to distinguish "the exec'd program's own real exit code" from "our own setup diagnostic," since a successful exec always leaves the pipe empty.
- `struct container_handle` gains `diag_fd` (the read end); `container_read_diag()` does one bounded, non-blocking-in-practice read (the writer's fd is always already closed by the time a caller reaches this — either via the CLOEXEC on success or via the child's own `_exit()` on failure) and `container_decode_exit_status()` provides a fixed-category fallback string for the rarer case the pipe itself has nothing (a genuinely clean exit, or a daemon restart that lost the live pipe).
- `registry_entry` gains `last_exit_reason`, populated by `registry_mark_exited()` (preferring the real diagnostic text, falling back to the decoded category) and both logged (`GET /system/logs`) and echoed as a new `exit_reason` field on every container JSON response (`GET /v1/containers`, `GET .../{name}`, `kanxeoctl ps`/`inspect`).
- `create_container_from_body()`'s `REGISTRY_ERR_CREATE_FAILED` branch now captures `errno` immediately after `registry_create()` returns (before `json_free()`, which isn't guaranteed to preserve it) and surfaces `strerror()` in both the `500` response body and the log store, instead of the old bare `"failed to create container"`.
- `spawn_kanxeo_bootroot_assembly()` gets the identical `pipe2(O_CLOEXEC)` + `dup2` treatment for `mkbootroot`'s own stdout/stderr; `handle_bootroot_assemble_event()` reads it once after `waitpid()` and logs it alongside the exit status/signal it already reported.

Deliberately **not** a redesign of the existing numeric exit-code scheme — this closes the *visibility* gap around it (the real text was always computable, just never reaching anyone), not a change to the scheme's own encoding.

## Consequences

- Confirmed working end-to-end via a local reproduction: a container given a genuinely missing binary now reports `exit_reason: "child: execve(/usr/bin/does-not-exist): No such file or directory"` instead of a bare `exit_status: 142` — exactly the class of diagnosis the live 192.168.15.95 investigation needed and didn't have.
- The `overlay mount(2) vs. exec` numeric ambiguity noted above is resolved in practice by this same mechanism: the diag text's own prefix (`"child: overlay_create: ..."` vs. `"child: execve(...): ..."`) says which one actually happened; `container_decode_exit_status()`'s own fallback text for that shared range says so explicitly when the diag text itself isn't available.
- `last_exit_reason` is deliberately not persisted (same reasoning as `exit_status` itself: meaningless across a daemon restart, since the process that produced it is gone either way).
- This is a strictly additive, non-breaking REST change — one new nullable field, no existing field's meaning or shape changed.
- Redeploying this to 192.168.15.95 is what unblocks continuing the live investigation into that box's own "every container after the first fails to exec" regression — a distinct, not-yet-root-caused symptom this ADR provides the tooling for, not the fix for.
