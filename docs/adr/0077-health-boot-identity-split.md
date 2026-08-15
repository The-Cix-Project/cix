# 0077 — Split GET /health into liveness + GET /system/boot identity

## Status

Accepted

## Context

`GET /health` originally answered two different questions at once: "is the daemon up" (`status`) and "which build/slot is this" (`build_version`/`build_time`/`slot`, added for A/B update verification -- see `docs/guides/kernel-build-and-ab-updates.md`). Both `thincctl` and the web dashboard poll `/health` every few seconds purely for a status dot (`dispatch()` in `daemon/src/main.c` already excludes it from the audit trail for exactly this reason -- low-value polling noise). The build/slot fields never change between polls of a running daemon and cost nothing at the wire level, but conflating "cheap liveness ping" with "identity/diagnostic snapshot" is the wrong shape going forward: identity is a real, separate concern (this session also added `kernel_version`, which belongs with build/slot, not with a liveness dot), and a future identity field shouldn't have to justify itself against every-few-seconds polling overhead.

## Decision

`GET /health` reverts to `{"status": "ok"}` only.

New `GET /system/boot` carries everything `/health` used to plus one addition:
- `build_version` / `build_time` -- unchanged, `git describe`/build timestamp from `build/version.h`.
- `slot` -- unchanged, this boot's `--slot=` (`"a"`/`"b"`/`null`).
- `kernel_version` -- new, the running kernel's `uname(2)` release string. Read fresh on every call (not cached at startup) since a boot identity check should reflect the kernel actually running right now, not what `main()` observed once at process start (the two are identical in practice for a real host, but the daemon makes no such assumption).

`thincctl boot` is the CLI surface; `thincctl health` drops its `build:`/`slot:` output lines to match the now-minimal response. Every place that referenced `/health`'s build/slot fields for deploy verification (`docs/guides/kernel-build-and-ab-updates.md`, `docs/guides/remote-development.md`, `docs/api/README.md`) now points at `/system/boot` instead.

## Consequences

- `GET /health` stays exactly as cheap as it always should have been for its actual call pattern (every-few-seconds polling); no wire-format change for callers that only ever read `status`.
- Deploy/reboot verification (the actual reason build/slot existed) gets a dedicated, correctly-scoped endpoint, and gains `kernel_version` for free -- confirming a kernel-only or root-only `system/update` actually landed no longer requires inferring it from the reboot succeeding.
- Purely additive at the protocol level (`/system/boot` is a new path); the one breaking change is `/health`'s own response shrinking, an intentional revert to its original minimal shape (ADR predates `build_version`/`slot` being added to it in the first place) rather than a new incompatibility.
