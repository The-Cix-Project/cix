# 0174 — `overlay_create()` tags `workdir` with the container's project id too (issue #34's real root cause)

## Status

Accepted

## Context

Issue #34 (see ADR-0173's own context for how this investigation started): new container creation on 192.168.15.95 began failing with a real, reproducible `mountns_pivot()` `EXDEV` ("Invalid cross-device link"), box-wide.

Two real hypotheses were tested and ruled out before the actual cause was found, each with real evidence, not assumption:

1. **`sda` mount correlation.** The failure window narrowed to right after a scratch disk (`sda`, from ADR-0169's own swap-placement testing) was formatted and mounted. A reboot naturally leaves a role-less disk unmounted (`diskformat_remount_present_role_disks()` skips any disk with no assigned role) — confirmed `sda` came back unmounted, and the failure was completely unchanged. Ruled out.
2. **`redirect_dir=on`.** The daemon only ever reported the whole `mountns_pivot()` call as having failed, with no way to tell which of its ~15 real syscalls was the actual source — a genuine diagnostic gap on a box with no shell. Fixed by adding per-step diagnostics (mirroring `overlay_create()`'s own existing `OVERLAY_ERR_*`/step-name precedent) — this pinpointed the failure precisely: `mkdir(/dev/pts): Invalid cross-device link`. A real, documented overlayfs kernel behavior (EXDEV on a rename when `redirect_dir` support isn't engaged) looked like a strong match, so it was added as an explicit mount option. The `EXDEV` did go away — but a *different* failure appeared (`execve()` `ENOENT` for every binary in the merged view, not just one), traced to `redirect_dir=on` actively breaking lowerdir content visibility on this project's own from-scratch kernel (never built with `CONFIG_OVERLAY_FS_REDIRECT_DIR=y`). Reverted.

## Decision

The real, complete root cause: `overlay_create()`'s own ext4 project-quota tagging (`FS_IOC_FSSETXATTR` + `FS_XFLAG_PROJINHERIT`, ADR-0062) only ever applied to `upperdir`, never `workdir`. Overlayfs's own kernel copy-up mechanism, when a new entry is created under a directory that exists only in `lowerdir` (never mutated — a container image's own `/dev` is exactly this case, since `mountns_pivot()` creates `/dev/pts` fresh on every single container start), works by creating the new object in `workdir` first, then renaming it into place in `upperdir`. An untagged-`workdir`-into-`PROJINHERIT`-tagged-`upperdir` rename is precisely the kind of operation ext4's own project-quota implementation refuses outright with `EXDEV` — it would silently change the moved object's project id without any real accounting for that move, which project-quota's own isolation guarantees can't allow.

Confirmed precisely, not guessed: a live A/B test on 192.168.15.95 (an otherwise-identical diagnostic container, `disk_quota_bytes` present vs. absent in the request body) reproduced and cleared the exact same failure on demand, before the fix was written. `overlay_create()` now tags `workdir` with the same project id as `upperdir`, via a small shared helper (`tag_dir_project_id()`) used for both.

## Consequences

- `src/overlay.c` — `tag_dir_project_id()` extracted from the existing upperdir-tagging code (no behavior change there), called a second time for `workdir` right after it's created. The `redirect_dir=on` mount-option addition and its later revert both live in this same file's own git history, not carried forward.
- **Why this matters beyond closing one issue**: a real disk quota is now required by default for any container created through the intended CLI/web flow (Part 183) — this bug would have hit essentially every new production container going forward, not just this investigation's own diagnostic containers. It was invisible until now because none of this project's own captured container recipes (`ldap-1/2`, `dns-1/2`, etc.) happen to set a disk quota, and boot-time recreation of already-defined containers was never affected — only a genuinely *new* container creation with a quota hits the copy-up path this bug lived in.
- Verified live, for real, with a quota: a fresh container on 192.168.15.95 with `disk_quota_bytes` set creates and runs cleanly — confirmed with a real, successfully-executed binary and real captured output, not a state-transition proxy. All 8 production containers survived five real reboots across this investigation without disruption; boot-time container recreation was never affected by any of this, only live, post-boot creation.
- Local regression coverage (`test_overlay`/`test_container_pty`/`test_container_net`/`test_devices`/`test_disk_quota`) all pass, though this dev sandbox's own backing filesystem has no real project-quota support to exercise the actual fixed code path locally (an already-documented environment limitation) — the real, load-bearing verification is the live A/B test on 192.168.15.95 described above, not the local suite.
