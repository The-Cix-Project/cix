# 0098 — tarball_has_common_top_dir() corrupted its own capture while draining it

## Status

Accepted

## Context

ADR-0093 added `tarball_has_common_top_dir()` to `daemon/src/pkg.c`, capturing up to 64KB of a tarball's `tar -tf` listing to decide whether `extract_tarball()` should pass `--strip-components=1`. Found live installing `coreutils` (3881 files) onto the `thinc-hosttools` image as part of fixing task #735 (the server-side mkbootroot re-assembly's own missing-`sha256sum` gap): `coreutils-9.11.tar.xz`'s own `tar -tf` listing is 137,883 bytes, well over double the 64KB cap, and the whole tarball got wrongly rejected as having "no common top dir" even though every one of its 3881 real entries shares the same `coreutils-9.11/` prefix — silently landing every recipe's `pkg_build()` one directory level too deep (`./configure: No such file or directory`, a failure that reads like a recipe bug but isn't).

Two distinct bugs compounded here, found in sequence, the second only after the first fix alone didn't actually resolve the live symptom:

1. **The truncation boundary itself.** When the capture buffer fills before EOF, the trailing bytes are an arbitrary mid-line cut, not a complete entry — confirmed exactly: `"...coreutils-9.11/lib/stdio-read.c\ncoreuti"`. The dangling 7-byte `"coreuti"` fragment (no `/` in it) got compared against the real top-level component (`"coreutils-9.11"`, length 14) and false-positived a mismatch.
2. **The drain loop reused the capture buffer.** The existing post-capture drain (`while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) ;`, there specifically so a large listing doesn't leave `tar` blocked writing to a full pipe and leaking a zombie) read its discarded bytes back into `buf` starting at index 0 — silently overwriting the very capture the mismatch scan still needed. Invisible for any tarball whose listing fits inside 64KB (nothing left to drain, this loop never touches `buf` at all); real and fully reproducible for one that doesn't. Confirmed directly with a standalone extraction of this exact function: after fixing bug 1 alone, the live symptom persisted identically — tracing `buf`'s own contents before and after the drain showed the head of the capture replaced by arbitrary tail fragments (e.g. `"thanks-gen"`, a file near the very end of the real listing, ending up at `buf[0]`).

## Decision

Both fixed together, in the order they execute: (1) the drain now reads into a small, separate 4KB discard buffer instead of reusing `buf` — the drained bytes are never needed for anything, only their being pulled off the pipe is, so there's no reason to touch the capture at all; (2) when the capture ended because the buffer filled (not a real EOF), the trailing partial line is trimmed off — everything after the last real `\n` in the captured bytes — before the mismatch scan runs, so only complete, newline-terminated entries are ever compared.

## Consequences

- Fixes a real, silent breakage for **any** from-source recipe whose tarball listing exceeds 64KB — not just `coreutils`, though that's the only one confirmed live so far.
- Verified in isolation before redeploying: a standalone extraction of the fixed function against the real `coreutils-9.11.tar.xz`, instrumented to print its own internal state, confirmed the drain no longer touches the capture and the trim now lands on a genuine line boundary (`top='coreutils-9.11' top_len=14`, `result=1`) — catching what a live-only retry loop would have taken far longer to isolate.
- Full local regression sweep clean (`test_pkg`, `test_daemon`, `test_cli`, `test_dns`, `test_system_update`), zero compiler warnings. No new dedicated test was added reproducing a >64KB listing specifically — the existing sandboxed fixtures don't have a large-enough real tarball to exercise this path deterministically without fetching one; a real, acknowledged gap.
