# 0173 — A real disk-unmount endpoint (`POST /disks/{name}/unmount`), closing a genuine gap found chasing issue #34

## Status

Accepted

## Context

Issue #34: any new container creation on 192.168.15.95 started failing with a real, reproducible `mountns_pivot()` `EXDEV` ("Invalid cross-device link") error — confirmed box-wide (multiple unrelated images, both hostbuild and plain container creation), confirmed not caused by image content, disk space, or storage placement, and confirmed to reproduce with the identical daemon code locally once a false-positive "it works" reading (a transient `building` status mid-poll, not a real success) was caught and corrected.

Correlating the daemon's own log timeline narrowed the failure's onset to a specific window, inside which the one notable event was `sda` (a real, block-device-backed scratch disk used for ADR-0169's own swap-placement testing) being formatted and mounted at `/var/lib/cix/disks/sda`. That disk's role was later removed (`DELETE /diskroles/sda`) as part of that testing's own cleanup, but the disk itself — confirmed via `GET /disks` — stayed mounted: `diskrole rm` only clears the role assignment, it was never wired to also unmount, and `POST /disks/{name}/format` is the only mount-affecting endpoint that existed, format-only, with no reverse. This left `sda` in a state nothing in the platform's existing API could resolve: mounted, empty, role-less, with no way to let go of it short of a full reformat (destructive, and pointless against an already-empty disk) or a reboot (risky to test blind, since if this bug also affects boot-time container recreation, a reboot could leave the box's real live services — `ldap-1/2`, `dns-1/2`, `ntp-1/2`, `syslog-1/2`, `jumpbox1` — not coming back up at all).

## Decision

**`POST /disks/{disk_name}/unmount`** — a real, synchronous `umount2(2)` against whatever `GET /disks` currently reports as that disk's own `mount_path`. Synchronous (unlike `format`'s async job): a plain `umount2(2)` is fast, matching `diskpart.c`'s own established "quick admin op runs inline" precedent (ADR-0158), no fork/pidfd machinery needed. Never touches the filesystem's own on-disk content — only the attachment to the running system goes away; a later `format` (destructive) or a real `mount(2)` are the only ways back.

Deliberately does **not** require an assigned role, the opposite precondition direction from `format` (`format` refuses a role-less disk; `unmount` is specifically meant to work on one, since a role-less-but-still-mounted disk with no way to let go of it is precisely the gap this closes). Still refuses the OS disk (`DISKFORMAT_ERR_IS_OS_DISK`) and a disk `GET /disks` doesn't currently report as mounted at all (`DISKFORMAT_ERR_NOT_MOUNTED`). Shares `format`'s own two real data-safety checks verbatim (`is_active_storage_singleton_placement()`/`disk_has_container_in_use()`, both already in `main.c`): a disk actively backing live state/rebuildable/log/backup/swap placement, or a container's own storage, is refused (`409`) — unmounting either out from under whatever relies on it would break it the instant the call succeeds, the identical reasoning `format`'s own safety checks already established. Same double-confirmation shape as `format` (`confirm_disk_name` in the body must match the URL).

## Consequences

- `daemon/include/diskformat.h`/`daemon/src/diskformat.c` — two new `enum diskformat_error` values (`DISKFORMAT_ERR_NOT_MOUNTED`, `DISKFORMAT_ERR_UMOUNT_FAILED`) and `diskformat_unmount()`.
- `daemon/src/main.c` — `handle_disk_unmount_post()` (reusing the two existing safety-check functions unchanged) + routing (`POST .../unmount`, mirroring the existing `.../format` suffix-match) + `respond_diskformat_error()` extended for the two new codes; the shared `DISKFORMAT_ERR_IS_OS_DISK` message generalized to cover both "formatted" and "unmounted" rather than only the former.
- `cli/src/main.c` — `cixctl disks unmount NAME`.
- `docs/api/openapi.yaml`/`docs/api/README.md`/`docs/guides/cli-reference.md` updated together.
- Verified: full clean rebuild (`-Wall -Werror`, zero warnings); `test_diskpart`/`test_disk_quota`/`test_daemon` all pass unmodified. The invalid-name/not-found/missing-confirm error paths were verified live against a local scratch daemon; the OS-disk-refusal and successful-unmount paths could not be verified locally (this dev sandbox has no real Cix-booted OS-disk marker, and its own visible block devices are real, shared host storage, permanently off-limits for any real mount/unmount testing — a standing rule from earlier in this project's history). The real, load-bearing verification — does unmounting an actually-mounted, role-less disk fix issue #34's own `mountns_pivot()` `EXDEV` failure — happens live against 192.168.15.95's own real `sda`, tracked in issue #34 itself, not repeated here.
