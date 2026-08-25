# 0178 — `url_basename()` must strip a query string (issue #46-adjacent, found live 2026-08-19)

## Status

Accepted

## Context

Continuing the issue #32/#46 gcc-bootstrap work, this session applied a real kernel fix (`CONFIG_BLK_CGROUP`/`CONFIG_CGROUP_WRITEBACK`, found while diagnosing a stalled overnight build — see the `kernel: enable CONFIG_BLK_CGROUP...` commit) and cut a new `kernel/6.18.40-3` recipe to carry it. Deploying that kernel via `pkg hostbuild` was the first time this project's own git-raw-file `pkg_source` mechanism (`.../raw/image/kernel/qemu-part1.config?ref=<commit>`, established by `kernel/6.18.40-2` for ADR-0166's own fix) was ever actually exercised end-to-end — `6.18.40-2` itself was committed but its real build was explicitly deferred (blocked on a separate TCC/binutils issue at the time), so this bug had never been triggered before.

The build failed immediately with `cp: cannot stat '/build/extra/qemu-part1.config': No such file or directory`. Root cause, confirmed directly in `daemon/src/pkg.c`: `url_basename()` computed a source's staged filename (`/build/extra/<basename>`, ADR-0036) via a bare `strrchr(url, '/')` — no query-string handling at all. For `.../qemu-part1.config?ref=473858954a927611fed6ccccc6ec346d4aec821d`, this staged the file as `qemu-part1.config?ref=473858954a927611fed6ccccc6ec346d4aec821d` instead of plain `qemu-part1.config`, silently breaking every recipe's own `cp /build/extra/qemu-part1.config ...` reference to the plain name it expected. The actual fetch itself was never affected (curl correctly ignores/strips a `file://` or `https://` URL's query string when resolving content) — only the basename computed for the *staged copy* was wrong.

Two unrelated environment gaps were also hit and worked around while diagnosing this (not code bugs, just this box's own current state, documented here for the record): the `dev` image on 192.168.15.95 is still mid-build-up from the incomplete gcc bootstrap (only `binutils`/`m4`/`zlib` installed, no `bash`) rather than the fully-populated toolchain image its own recipe comments describe, and a leftover kept `__pkgbuild-0` container from a prior failed attempt twice collided with a fresh hostbuild's own container name — both real, but neither is this ADR's own subject.

## Decision

`url_basename()` now strips a trailing `?...` query string when computing a source's staged basename — it finds the last `/`, then truncates at the first `?` within that trailing component if one exists. Changed from returning `const char *` (a pointer into the URL string itself, which can't be truncated in place without corrupting the URL curl still needs for the real fetch) to writing into a caller-supplied buffer, matching this codebase's established `char *out, size_t out_size` idiom.

## Consequences

- `daemon/src/pkg.c`: `url_basename()` signature changed (now `void url_basename(const char *url, char *out, size_t out_size)`); its one call site (`pkg_fetch_completed()`'s extra-source staging loop) updated accordingly.
- `test/test_pkg.c` step 15b: a new regression test reusing test 15's own already-staged `extra1.txt` fixture, addressed via a `file://...?ref=deadbeef` URL — curl's own `file://` handling ignores a trailing query string exactly like a real HTTP(S) fetch would (confirmed directly), so this exercises the real bug without needing a live HTTP server. Verified to genuinely catch the bug, not just pass trivially: temporarily reverted the fix and confirmed this exact test (and only this test) fails with `multisrcquery ended in state 'failed'`, then restored the fix and confirmed the full suite passes again.
- Any future recipe using a query-string URL for a non-index-0 `pkg_source` entry (the git-raw-file `?ref=<commit>` pattern `kernel.recipe`/`cix.recipe` both rely on for their config-file/archive fetches) is now correctly staged under its plain basename.
- Local regression suite (`test_pkg`) passes. Live verification on 192.168.15.95 (redeploying the fixed daemon and retrying `kernel/6.18.40-3`'s own hostbuild) is the immediate next step, not yet done as of this writing.
