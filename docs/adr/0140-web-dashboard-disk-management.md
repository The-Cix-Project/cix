# 0140 — Web dashboard: disk management surface (closing a real API/UI parity gap)

## Status

Accepted

## Context

Asked directly: "we need to add the disk management stuff onto the web UI right?" Checked before assuming -- grepped `web/app.js`/`web/index.html` for every "disk" reference and found all 22 hits were about the disk-usage *stat* (the Host Stats / container Stats graphs), never about disk *management*. Cross-checked `docs/api/openapi.yaml` and `cli/src/main.c` and confirmed the real gap: `GET /disks` (real host block-device enumeration), `GET`/`POST`/`DELETE /diskroles` (persisted role assignment -- `container-storage` or `backup`), and `GET`/`POST /disks/{name}/format` (destructive mkfs+mount, ADR-0071/ADR-0102/ADR-0104) have all existed at the API layer, and in `thincctl disks`/`diskrole`, since multi-disk management Phases A-C shipped -- with zero web dashboard surface at all. A real, confirmed API-First-Mandate gap: every capability exists as a REST endpoint, but "nothing is dashboard-exclusive" doesn't mean the reverse holds, and this was a case where it clearly should have.

## Decision

**One new leaf, System > Host > Disks**, alongside Devices (the same kind of thing -- host hardware inventory, ADR-0138's own grouping logic) rather than a new top-level subgroup: disk management doesn't have PKI/DNS/LDAP/NTP's multi-page depth, one page covers it.

The page is a single table (`GET /disks`, polled like every other list here) with a Role column cross-referenced client-side against `GET /diskroles` (mirroring how Network detail's own "Containers on this network" table, ADR-0138, cross-references two already-fetched lists rather than needing a combined endpoint) and a Format column showing the live state of that disk's own format job, fetched only for role-assigned, non-OS disks while the page is actually open -- the same "only while this page is showing" guard `refreshImageRecipeApplyStatus()` already established, keeping the extra per-disk requests bounded regardless of disk count.

**Role assignment is non-destructive and reachable two ways**: the header's shared `+ Create > Disk Role` modal (a disk-name select populated from currently unassigned, non-OS disks, plus a role select), or a per-row "Assign role…" shortcut that pre-fills the same modal -- the exact pattern the Devices page's own "Name…" button (device mapping) already established. Role removal (`DELETE /diskroles/{name}`) is a plain button, no confirmation dialog, matching this dashboard's own existing convention for reversible, non-data-destroying removals (Network/Image/Container Remove are the same way).

**Format is a per-row action, not a modal**, since the disk it targets is already unambiguous from the row it appears on -- a filesystem select (ext4/btrfs) plus a Format button, gated by a real `confirm()` dialog stating plainly that it destroys existing content. `confirm_disk_name` (the API's own deliberate double-confirmation field) is filled in automatically from that same already-known disk name rather than asked for a second time as a typed field -- mirroring `thincctl disks format NAME`'s own established reasoning (`cli/src/main.c`'s `cmd_disks_format()` comment: "matching what the operator already typed once"), not a new, stricter convention invented for the web UI alone. A disk with a format job currently `running` shows that state instead of the format controls, since the API itself refuses a second concurrent job (409) -- avoiding an action button that would just fail.

## Consequences

- The API/CLI/web-dashboard surface for disk management is now at parity -- every capability `thincctl disks`/`diskrole` already exposed is reachable from the dashboard too.
- No new endpoint, no daemon change of any kind -- this is entirely `web/`.
- Verified carefully given the real risk involved: this sandbox's own `/sys/class/block` enumerates real host block devices (LVM `dm-*` volumes, real disk models), not virtualized/isolated ones, so live verification was deliberately read-only-only -- table rendering (64 real rows), badge states, and the "Assign role…" modal's pre-fill/population were all confirmed via a real headless-browser session, but no role-assignment or format action was ever submitted against this sandbox's own disks. The "has a role assigned" rendering path (Remove role / Format controls) was verified by code review against the already-established, working `deviceRow()`/TLS-Throttle patterns it mirrors, plus a planned read-only check against 192.168.15.95's own already-role-assigned `sda` (assigned during the original Phase B/C work, left untouched here) once deployed.
- Found along the way, noted but out of scope for this ADR: 192.168.15.95's `sda` currently shows `mounted: false` despite having been formatted and mounted successfully during the original Phase C verification -- consistent with the API's own documented caveat that a real mount can outlive the in-memory format-job state but doesn't itself say whether the mount survives a reboot; this box has rebooted several times since. Worth a real investigation on its own, not folded into this change.
