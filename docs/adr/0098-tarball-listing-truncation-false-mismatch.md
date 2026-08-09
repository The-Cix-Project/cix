# 0098 — tarball_has_common_top_dir() false-positived on its own 64KB capture truncation

## Status

Accepted

## Context

ADR-0093 added `tarball_has_common_top_dir()` to `daemon/src/pkg.c`, capturing up to 64KB of a tarball's `tar -tf` listing to decide whether `extract_tarball()` should pass `--strip-components=1`. The function's own comment reasoned that "a capture that's still fully consistent when it ends (whether by EOF or by filling the buffer) is treated as 'has a common top dir'" — but the implementation didn't actually distinguish those two cases: when the buffer filled before EOF, the trailing bytes captured are an arbitrary mid-line cut, not a real, complete path entry, and the parsing loop scanned it anyway as if it were.

Found live installing `coreutils` (a real, sizeable release tarball — 3881 files) onto the `kanxeo-hosttools` image as part of fixing task #735 (the server-side mkbootroot re-assembly's own missing-`sha256sum` gap): `coreutils-9.11.tar.xz`'s own `tar -tf` listing is 137,883 bytes, well over double the 64KB cap. The capture cut off exactly mid-prefix — `"...coreutils-9.11/lib/stdio-read.c\ncoreuti"` — leaving a dangling 7-byte fragment (`"coreuti"`) with no `/` in it. The slash-scan compared that fragment's own length (7) against the already-established real top-level component (`"coreutils-9.11"`, length 14), found a mismatch, and returned "no common top dir" for a tarball that unambiguously has one (every one of its 3881 real entries starts with the same `coreutils-9.11/` prefix). `extract_tarball()` then skipped the strip, landing every recipe's `pkg_build()` one directory level too deep — `./configure: No such file or directory`, a real, previously-invisible build failure that reads like a recipe bug but isn't.

## Decision

When the capture ends because the buffer filled (not because of a real EOF), the trailing partial line — everything after the last real `\n` in the captured bytes — is trimmed off before the mismatch scan runs, so only genuinely complete, newline-terminated entries are ever compared. A truncation that happens to land exactly on a line boundary needs no trimming (nothing to trim); one that lands mid-line, as coreutils' listing did, now contributes no false signal instead of one.

## Consequences

- Fixes a real, silent breakage for **any** from-source recipe whose tarball listing exceeds 64KB — not just `coreutils`, though that's the only one confirmed live so far; anything with a few thousand entries in its own archive is equally exposed.
- Live-verified directly: the exact `coreutils` install that surfaced this (onto `kanxeo-hosttools`, unblocking task #735's own `sha256sum` gap) completed successfully once deployed.
- Full local regression sweep clean (`test_pkg`, `test_daemon`, `test_cli`, `test_dns`, `test_system_update`), zero compiler warnings. No new dedicated test was added reproducing a >64KB listing specifically — the existing sandboxed fixtures don't have a large-enough real tarball to exercise this path deterministically without fetching one; a real, acknowledged gap, closed here by the live coreutils install itself standing in as the actual proof.
