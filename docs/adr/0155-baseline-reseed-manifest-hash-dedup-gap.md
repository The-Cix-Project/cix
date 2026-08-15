# 0155 — baseline self-heal does not survive a same-manifest reinstall

## Status

Accepted

## Context

Closing out task #865 (the real jumpbox1 PTY deploy) required getting ADR-0150's `/dev/ptmx` baseline symlink onto jumpbox1's own actual running image — but jumpbox's own image had gone 22 versions without it, despite `pkg_seed_image_baseline()`'s own doc comment claiming it "lets an image that missed baseline seeding self-heal on its very next install."

Tried the obvious thing first: uninstall then reinstall an already-present package (`psmisc`, then `screen`) to force a fresh pass through `install_mutate()` (which calls `pkg_seed_image_baseline()` before merging). Confirmed live, twice, that this does **not** work — `current_version` after the reinstall was byte-identical to the version *before* the uninstall, and a direct exec probe (`exec("/dev/ptmx")` inside a fresh container on that version) still failed `ENOENT`, not `EACCES`.

Root cause, confirmed by directly comparing against a brand-new, never-seeded test image (where the exact same self-heal path *did* work correctly on its very first real install): ADR-0108's own image-version hash is a **manifest hash** — sorted `name@version` pairs of installed packages — not a content hash of the actual filesystem tree. Reinstalling a package at the exact version it already had reproduces the byte-identical manifest, and `image_produce_new_version()`'s own dedup logic (ADR-0108: "a version whose content hash already exists in the image's history is deduplicated, no new directory, `current_version` just repoints") short-circuits straight to the *pre-existing* on-disk directory for that hash — the freshly-staged tree that `install_mutate()` actually built (complete with a correctly re-seeded `/dev/ptmx`) is discarded entirely, never becoming the new `current_version`.

## Decision

No code changed here — this is a genuine, permanent characteristic of the manifest-hash-based versioning scheme (ADR-0108's own design, not a bug in it), not something to "fix" by switching to a content hash (that would break the real, load-bearing deduplication ADR-0108 exists for). Documenting it as a real, discovered limitation instead: **the self-heal `pkg_seed_image_baseline()`'s own doc comment describes only works when the resulting manifest is genuinely new** — a package version that has never been part of this exact combination before. A same-manifest reinstall cycle can never trigger it, no matter how many times repeated.

The real, practical unblock for jumpbox specifically: install a package the image didn't already have (`xz`, itself a genuinely useful admin tool and already freshly TCC-fixed and verified working the same night — see `recipes/image/jumpbox/1.3.0/build.sh`) — guaranteed to produce a manifest that has never existed in this image's history, forcing a real fresh build. Confirmed live: the resulting version's `/dev/ptmx` exists (`EACCES` not `ENOENT`), and jumpbox1 (`follow_rolling:true`) picked it up automatically via ADR-0124's own reconciliation, with no manual recreate needed.

## Verification

Reproduced the gap live, twice (psmisc, then screen) before finding the real unblock. Confirmed the underlying self-heal mechanism itself is not broken — a brand-new test image (`ptmxtest`) correctly got `/dev/ptmx` on its very first real install, and correctly kept it through an uninstall+reinstall cycle of its own single package (`zlib`) that also deduped back to an unchanged manifest hash and correctly stayed `EACCES` (proving the *existing* symlink survives being repointed-not-rewritten; only a *missing* one can never be added this way). Confirmed jumpbox1's own live `pid` and `image_version` both changed automatically after the `xz` install, with no manual container recreate.

## Consequences

- Any future image that's discovered to have missed a baseline-seeding change (this won't be the last one) needs the same real, useful-package-addition approach to force a genuinely new manifest — not a same-manifest reinstall cycle, which this ADR now documents as provably ineffective.
- `pkg_seed_image_baseline()`'s own doc comment ("self-heal on its very next install") is not wrong, but is now understood to carry an implicit precondition (a genuinely new manifest) that wasn't previously stated — worth keeping in mind for anyone reading that comment without this ADR's context.
- This is a real, permanent property of manifest-hash versioning, not scheduled for a future fix — a content-hash scheme would remove this gap but reopen the real, load-bearing dedup problem ADR-0108 solved in the first place; not a trade worth making for this comparatively narrow benefit.
