# 0084 — mkbootroot's own mksquashfs exec target was never reachable on a real installed host

## Status

Accepted

## Context

ADR-0083 fixed one real cause of the long-open "mkbootroot exited 1" failure (the missing second `ld-linux` copy for `test_image_fixture_build()`'s own staged binaries), but the symptom persisted after that fix was deployed. Re-reading `image/src/mkbootroot.c` in full surfaced a second, independent, and more fundamental gap in the same function family.

`run_mksquashfs()` execs a hardcoded `MKSQUASHFS_BIN = "/usr/bin/mksquashfs"`. This path is real and correct on this dev sandbox (confirmed via `readelf -l`/`ldd`) — but `spawn_thinc_bootroot_assembly()` (`daemon/src/main.c`) runs `mkbootroot` directly on a real installed host, as a plain fork+execve from `thincd` itself, with **no chroot or namespace involved**. On that host, the daemon's own process root *is* the previous generation's assembled control-plane squashfs — and `mksquashfs` was never one of the binaries `shelled_bins[]` stages into that root (confirmed by inspection: `openssl`/`curl`/`tar`/`unsquashfs`/`bzip2`/`xz`/`mkfs.ext4` are staged there because `thincd` itself shells out to them at runtime; `mksquashfs` is a build-time-only tool nothing inside the assembled root ever calls, so it was correctly never added to that list — but nothing else made it reachable either). `squashfs-tools.recipe`'s own header comment states plainly: *"thincd does its own squashfs assembly via ... execve("/usr/bin/mksquashfs", ...) calls, which is a real, separate staged binary already, not this recipe's concern"* — this claim was never actually true; it was an assumption made when writing that recipe (ADR-0078) that was never verified against `mkbootroot.c`'s own `shelled_bins[]` list, which is the only place staging into the assembled root happens.

The net effect: every `spawn_thinc_bootroot_assembly()` run on a real installed host was always going to fail at the `execve(MKSQUASHFS_BIN, ...)` step with a bare ENOENT — regardless of ADR-0083's own fix, since that fix addresses a *different* binary's linking (the staged `thincd`/`thincctl`), not this one's reachability at all.

## Decision

Resolve the `mksquashfs` exec target dynamically in `mkbootroot.c`'s `main()`, following the exact same tolerant-default shape `host_tool_bins[]` (`cp`/`rm`/`sha256sum`/`gzip`) already established: when `host_tools_dir` is non-empty (`thinc-hosttools` was built — `squashfs-tools.recipe` already installs a real `mksquashfs` there, alongside `unsquashfs`, from the exact same upstream build per task #692/693), use `<host_tools_dir>/usr/bin/mksquashfs` directly — a real, ordinary filesystem path under `BASE_DIR`, entirely outside the transient assembled-root/squashfs boundary, so no staging-into-`image_root` is needed for a tool nothing else consumes. Falls back to the dev-sandbox `/usr/bin/mksquashfs` when `host_tools_dir` was never built (matching every other call site's own existing fallback).

Also added a `stat()`-based precheck in `run_mksquashfs()` before the `fork()`/`execve()`, so a future failure of this kind reports "mksquashfs binary not found at `<path>`" unambiguously instead of the bare, hard-to-attribute ENOENT text ADR-0083's own investigation had to work backward from.

`mktoolchainimage.c` carries the identical `MKSQUASHFS_BIN` macro but is never invoked by `thincd` (confirmed via grep — it's a manual, dev-sandbox-only tool, same category as `mkinstalleriso`) — left unchanged, since the dev sandbox always has a real `/usr/bin/mksquashfs` and no equivalent host-tools indirection is needed there.

## Consequences

- The real, complete fix for the "mkbootroot exited 1" failure (Part 23) requires *both* ADR-0083 (the `ld-linux` second copy, needed for the staged `thincd`/`thincctl` binaries to even start) and this ADR (a reachable `mksquashfs`, needed for the assembly's own final step to run at all) — confirmed as two independent bugs in the same code path, not one bug with two symptoms.
- `squashfs-tools.recipe`'s own header comment is now stale in the same way ADR-0083's target comment was — worth a follow-up correction pass so a future reader doesn't re-inherit the same wrong assumption.
- General lesson, same one ADR-0083 already named: a hardcoded dev-sandbox-only path silently works everywhere this dev sandbox's own tests run, and only breaks on the one real target (a genuinely minimal installed host) that most needs it to work — the second occurrence of this exact pattern in two ADRs in a row is itself worth remembering the next time a new binary gets shelled out to from this codebase.
