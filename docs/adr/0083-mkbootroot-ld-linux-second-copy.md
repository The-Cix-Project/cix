# 0083 — mkbootroot's own control-plane root was missing the second ld-linux copy

## Status

Accepted

## Context

The real, root cause of the "cix bootroot assembly: mkbootroot exited 1" failure first found during Part 23's own final validation, and left unresolved through ADR-0078/ADR-0080's own diagnostics work -- now actually readable thanks to ADR-0080's mkbootroot output-capture fix. Triggering a fresh `cix` hostbuild round on 192.168.15.95 and reading `GET /system/logs` for the first time showed the real text: `/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2: No such file or directory`.

`test_image_fixture_build()` (`test/test_image_fixture.c`) -- the function `mkbootroot.c` uses to stage the *control-plane root's own* runtime libs (as opposed to `pkg_seed_image_baseline()`, `daemon/src/pkg.c`, which does the equivalent for a `pkg install`-managed container image) -- only ever staged `ld-linux-x86-64.so.2` at `lib64/ld-linux-x86-64.so.2` (the path the kernel's own `PT_INTERP` lookup needs at `execve()` time). It never staged a second copy at `lib/x86_64-linux-gnu/ld-linux-x86-64.so.2` -- the path glibc >= 2.34's own `libc.so.6` needs reachable via the *ordinary* runtime library search path, because `libc.so.6` itself carries a `DT_NEEDED` entry on `ld-linux-x86-64.so.2`.

This is the exact same gap `pkg_seed_image_baseline()` already carries a fix for (ADR-0057's own "Second copy of the same file" comment, quoted directly in ADR-0079's own investigation of an unrelated cgroup bug) -- it was just never applied to `test_image_fixture_build()`, the sibling function serving the control-plane root instead of a container image. Confirmed as the real, sole cause: `mkbootroot` itself runs fine (its own `PT_INTERP` lookup at `lib64/...` succeeds), but anything it or its own libc needs to resolve the second path for -- in practice, this affects any dynamically-linked binary sharing that same libc.so.6, including `mkbootroot`'s own `mksquashfs` child process -- fails with exactly this text.

## Decision

Add the identical second-copy staging `pkg_seed_image_baseline()` already does, to `test_image_fixture_build()`: `lib/x86_64-linux-gnu/ld-linux-x86-64.so.2`, same source file, same one extra `test_image_fixture_copy_file()` call. No new mechanism -- literally the same fix, applied to the second of the two functions in this codebase that needed it.

## Consequences

- Fixes the real, long-open "cix bootroot assembly: mkbootroot exited 1" failure (Part 23) at its actual root, not a workaround -- confirmed locally: a fresh `mkbootroot` run now stages both `lib64/ld-linux-x86-64.so.2` and `lib/x86_64-linux-gnu/ld-linux-x86-64.so.2` in the produced control-plane root.
- Affects every produced control-plane root (`cixd-root.squashfs`) going forward -- both the manual dev-machine path (`docs/guides/installing.md`) and the server-side automatic assembly (`spawn_cix_bootroot_assembly()`, ADR-0057) share this same `test_image_fixture_build()` call.
- General lesson worth naming directly: this exact class of bug (a glibc >= 2.34 `DT_NEEDED`-on-`ld-linux` gap) has now been found and fixed twice in this codebase, in two structurally near-identical but separately-written functions. `include/container.h`/`test_image_fixture.c`'s own doc comments should be the first place a future third occurrence of this pattern gets checked against, rather than re-discovering it a third time from a bare exit code.
