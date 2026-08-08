# 0085 — the real, final root cause: mkbootroot's own source reads assumed a merged-usr host

## Status

Accepted

## Context

ADR-0083 and ADR-0084 both addressed real gaps in `mkbootroot`, but neither was the actual cause of the persistent "kanxeo bootroot assembly: mkbootroot exited 1" / `ld-linux-x86-64.so.2: No such file or directory` failure — confirmed the hard way: after deploying both fixes and re-triggering a self-hosted build, the exact same bare message reappeared, with no prefix from any of this project's own diagnostic code.

Reproduced precisely via `strace` against a genuinely non-merged-usr chroot (built to match `192.168.15.95`'s own real root exactly — no `/usr/lib/x86_64-linux-gnu`, only `/lib/x86_64-linux-gnu`, matching `CLAUDE.md`'s own long-standing note that this project's produced roots are deliberately not merged-usr): the failing syscall was `openat(AT_FDCWD, "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", O_RDONLY) = -1 ENOENT`, immediately followed by `write(2, "/usr/lib/x86_64-linux-gnu/ld-lin"...)` — i.e., `test_image_fixture_build()`'s own `test_image_fixture_copy_file()` call, not the dynamic linker at all, and not `mksquashfs`.

The real bug: `test_image_fixture_build()`'s three staging steps (lib64/ld-linux, lib/x86_64-linux-gnu/libc.so.6, lib/x86_64-linux-gnu/ld-linux — the last one being ADR-0083's own addition) all read their *source* from `/usr/lib/x86_64-linux-gnu/...`. On this dev sandbox that resolves fine (`/lib` is itself a symlink to `usr/lib`), which is exactly why every local test run and every "it works here" check passed. But `mkbootroot` doesn't only ever run on this dev sandbox — `spawn_kanxeo_bootroot_assembly()` (`daemon/src/main.c`) execs it directly on a real installed host's own currently-running root, with no chroot. That root is one of this project's own prior control-plane squashfs builds — deliberately non-merged-usr — where `/usr/lib/x86_64-linux-gnu/` simply does not exist. ADR-0083's own fix got the *destination* right (staging the second copy at `lib/x86_64-linux-gnu/`, correctly matching this project's own convention) but left the *source* reads on the wrong, merged-usr-only path — so the newly-added copy step failed at the exact same read every single self-hosted build has always failed at, just with the ld-linux path this time instead of the (previously never-reached) `mksquashfs` step ADR-0084 fixed separately.

Continuing the same `strace`-driven verification loop past this fix surfaced two further, smaller instances of the identical class of bug in the same file, both only reachable once the first one was fixed:

1. `host_tool_bins[]`'s own `rm` entry (`image/src/mkbootroot.c`) used `dev_host_path = "/usr/bin/rm"` as its fallback source when `host_tools_dir` is empty — but `rm`'s own *destination* (`rootfs_path`) is deliberately `bin/rm`, not `usr/bin/rm` (unlike its three siblings `cp`/`sha256sum`/`gzip`, which use `usr/bin/X` for both). A real installed host only ever has `rm` at `/bin/rm` — `/usr/bin/rm` doesn't exist there either.
2. `libpthread.so.0`'s own lazy `dlopen()` of `libgcc_s.so.1` for `pthread_exit()`/`pthread_cancel()` stack unwinding — never a `DT_NEEDED` entry, so invisible to every prior `ldd`-based dependency-closure pass this file's own comments already documented. `mksquashfs` (now genuinely reachable thanks to ADR-0084) starts and runs correctly, then aborts at its own ordinary `pthread_exit()` with "libgcc_s.so.1 must be installed for pthread_exit to work" — a real, if extremely subtle, missing runtime dependency.

## Decision

- `test/test_image_fixture.c`: all three source reads in `test_image_fixture_build()` changed from `/usr/lib/x86_64-linux-gnu/...` to `/lib/x86_64-linux-gnu/...` — resolves identically on this dev sandbox (`/lib` → `usr/lib` symlink) and correctly on every one of this project's own non-merged-usr produced roots (the actual, real location there).
- `image/src/mkbootroot.c`: `rm`'s `host_tool_bins[]` entry's `dev_host_path` changed from `/usr/bin/rm` to `/bin/rm`, matching its own `bin/rm` destination and the same dev-sandbox-symlink reasoning above.
- `image/src/mkbootroot.c`: added `/lib/x86_64-linux-gnu/libgcc_s.so.1` to `shelled_bin_libs[]`, alongside the other `unsquashfs`/`mksquashfs` runtime dependencies.
- Verification discipline established and used for all three: reproduce the exact failure via `strace -f` against a real, deliberately non-merged-usr chroot built to match the real deployed host's own constraints (no rich dev-sandbox `/usr`, `host_tools_dir` empty) — not just "it built without warnings" or "it worked in this dev sandbox." Confirmed the full pipeline now produces a real, well-formed squashfs (`unsquashfs -l` verified) end-to-end in that same chroot before touching the live host again.

## Consequences

- This is very likely the actual, complete fix for the long-open Part 23 "mkbootroot exited 1" failure — the first time the full sequence (fresh source push → self-hosted build → real assembly) has been verified end-to-end in a faithful reproduction of the real host's own constraints, rather than assumed from a partial local test.
- General lesson, now demonstrated three times across ADR-0083/0084/0085 in the same file: this codebase's own dev sandbox is a merged-usr Debian system where `/lib` is a symlink to `/usr/lib`, silently making `/usr/lib/...` and `/lib/...` source paths behave identically here — while this project's own *produced* roots are deliberately not merged-usr, so only the `/lib/...` form is ever real there. Any future binary staged into the control-plane root by path, not by build system, should default to `/lib/...`/`/bin/...` source forms and treat `/usr/lib/...`/`/usr/bin/...` as suspect unless a specific reason says otherwise (e.g. a real Debian-packaged binary's own hardcoded `PT_INTERP`, which does use the `/usr/`-prefixed form and is a separate, already-known distinction — see ADR-0084's own investigation).
