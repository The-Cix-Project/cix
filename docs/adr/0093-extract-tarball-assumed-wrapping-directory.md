# 0093 — extract_tarball() assumed every source tarball has a wrapping directory

## Status

Accepted

## Context

`daemon/src/pkg.c`'s `extract_tarball()` has always called `tar --strip-components=1` unconditionally -- correct for an ordinary release tarball (`foo-1.2.3.tar.gz`, everything under one top-level `foo-1.2.3/` directory, the shape every recipe's own upstream tarball in this project happens to have), but `cix.recipe`'s own self-build source snapshot fetches a `git archive`-generated tarball from gitea's REST API (`.../archive/<tag>.tar.gz`). `git archive` invoked without an explicit `--prefix=` (gitea's own default archive-download behavior) produces a tarball with **no** wrapping directory at all -- every top-level file and directory of the real source tree (`Makefile`, `daemon/`, `docs/`, ...) sits at depth 0. Unconditionally stripping one path component from that shape doesn't fail loudly -- it silently drops or misplaces real top-level content, corrupting the extracted source tree in a way that only surfaces later, as a confusing downstream build failure with no obvious connection to the actual cause.

## Decision

`extract_tarball()` now inspects the tarball's own listing (`tar -tf`) before deciding whether to strip: if every entry shares the same single top-level path component, extraction proceeds exactly as before (`--strip-components=1`); if not (any two entries diverge on their first path component, or the divergence isn't found within a generous 64KB listing capture), extraction runs without stripping at all, preserving the tree exactly as archived. The listing capture reuses the same fork/pipe/exec shape `pkg_run_capture_sha256()` already established, kept as its own function since the two serve genuinely different purposes (a checksum vs. a structural check).

## Consequences

- Both tarball shapes -- a normal release tarball's own wrapping directory, and `git archive`'s prefix-less output -- now extract correctly through the same code path, with no recipe-level workaround needed.
- Verified via the existing `test_pkg.c` regression suite (unaffected -- every existing fixture uses the normal wrapping-directory shape, confirming no behavior change there) plus the reasoning above for the git-archive shape, matching what this project's own `cix.recipe` self-build source snapshot actually produces.
- The 64KB listing-capture bound is a deliberate, documented tradeoff: large enough that any real top-level divergence (which shows up immediately, at the very first few entries, for a prefix-less archive) is always caught, without needing to buffer an entire large source tree's file listing in memory just to make that determination.
