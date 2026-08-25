# 0154 — LD_LIBRARY_PATH for host-tools-sourced mksquashfs

## Status

Accepted

## Context

Task #865's deploy (the ADR-0150 devpts/PTY fix, plus everything else this session shipped, onto the real box, 192.168.15.95) exercised the `cix-hosttools` mechanism (ADR-0078) for the first time end to end against a real installed box: a real `squashfs-tools` build staged onto that image, and `spawn_cix_bootroot_assembly()` (`daemon/src/main.c`) correctly resolved `host_tools_dir` and exec'd its `usr/bin/mksquashfs` instead of the (nonexistent, on a real minimal install) dev-sandbox fallback.

`mkbootroot` reported success (`cix bootroot assembly: succeeded`, `mksquashfs` exited 0) on three consecutive attempts — but `POST /v1/system/update`'s own real squashfs-magic check (`memcmp(magic4, "hsqs", 4)`) rejected the resulting file every time, a genuine on-disk corruption, not a misdiagnosis of a working file.

Root cause, confirmed by direct code review of `run_mksquashfs()`'s own `execve(mksquashfs_bin, argv, environ)` call: `mksquashfs`, built dynamically linked against `liblzma.so.5` (this project's own "never `-static`" rule), is exec'd directly off `host_tools_dir` — an arbitrary image rootfs path, never a chroot or pivot_root'd root the way an ordinary `pkg install` build container reaches its own libs. Nothing in `mkbootroot.c` ever told the dynamic linker to look inside `host_tools_dir` for shared libraries, so `ld.so` fell back to its own ordinary system search path. A genuinely *missing* library fails loudly (the dynamic linker's own startup code exits nonzero before `main()` runs, which `run_mksquashfs()`'s `WIFEXITED`/`WEXITSTATUS` check already catches correctly) — but this real box's own bare host environment apparently already has *some* `liblzma.so.5` resolvable via the ordinary system path, so the dynamic linker silently linked against that one instead, with no error at all. `mksquashfs` ran and exited 0, against a library it was never actually built or tested against, producing corrupt output.

## Decision

`run_mksquashfs()` (`image/src/mkbootroot.c`) now takes `host_tools_dir` as an explicit parameter. When non-empty, the child process gets a real inherited-environment-plus-one-entry `envp`: every existing `environ` entry copied forward, plus `LD_LIBRARY_PATH=<host_tools_dir>/lib/x86_64-linux-gnu:<host_tools_dir>/usr/lib:<host_tools_dir>/lib` appended — covering every lib-staging convention this recipe set's own package recipes actually use (confirmed: `xz.recipe` stages to `lib/x86_64-linux-gnu`, other recipes to `usr/lib`). When `host_tools_dir` is empty (the dev-sandbox fallback path, unchanged from before this ADR), the child gets the plain, untouched `environ` exactly as before — this fix is additive only for the real, host-tools-sourced case.

Deliberately a real environment *copy-then-append*, not a replace: overwriting the whole `envp` with just `LD_LIBRARY_PATH` would silently drop everything else the daemon's own process was started with, for no reason connected to the actual bug. A fixed 64-entry buffer is used for the copy (a real, live daemon environment this large would itself be abnormal) — overflow fails loudly rather than silently truncating.

`mktoolchainimage.c`'s own, separate `run_mksquashfs()` is deliberately untouched: that tool always uses the hardcoded dev-sandbox `/usr/bin/mksquashfs` (per its own header comment, "run manually, occasionally, on a real toolchain-having machine") and never sources a binary from `host_tools_dir` at all, so it was never exposed to this bug in the first place.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. `test/test_mkbootroot_firmware.c` (the existing permanent coverage for `mkbootroot.c`) passes clean — it never sets `host_tools_dir`, exercising exactly the unchanged fallback path, confirming this fix doesn't disturb existing dev-sandbox behavior. `test/test_boot.c` could not be re-run in this sandbox (needs a real `build/bzImage`, a separate, deliberately-not-automated build this sandbox doesn't currently have — a pre-existing environment gap, not something this change caused).

The real, decisive verification is the live deploy this ADR exists because of: re-running `pkg hostbuild cix --deploy` against the real box, once this fix's own hostbuild round produces a fresh `mkbootroot` binary carrying it, either produces a real, `POST /system/update`-accepted squashfs or it doesn't — see `CHANGELOG.md`'s own entry for this task for the confirmed outcome.

## Consequences

- Any *future* host-tools binary that ever needs to be directly exec'd by `mkbootroot` (not just staged as a file, the way `cp`/`rm`/`sha256sum`/`gzip` already are) needs the same `LD_LIBRARY_PATH` treatment `run_mksquashfs()` now gets — a real, now-documented precedent for whoever adds the next one, not an isolated one-off fix.
- This is the first time `cix-hosttools` has ever been built and actually exercised end to end against a real installed box (ADR-0078 shipped the mechanism itself much earlier, but nothing had triggered a real assembly using it until this task) — a real, previously-undiscovered gap in a feature that had shipped untested against its own real intended use case. A useful, general lesson: "the code compiles and the feature exists" is not the same as "the feature has actually been exercised for real."
