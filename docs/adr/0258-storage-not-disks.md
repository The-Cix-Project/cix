# 0258 — The resource is Storage; a disk is a device in it

## Status

Accepted

## Context

This platform called the same subject two things. The REST resource was `/v1/disks` and `/v1/diskroles`; the CLI had `cixctl disks` *and* a separate `cixctl storage`; the dashboard's tree said "Hardware > Disks" while the pages underneath managed volumes, placement, swap and backups as well.

None of that was wrong locally. Each name was chosen when its own piece was built, and "disks" was accurate for the piece that enumerated block devices. It stopped being accurate when the same area grew volumes, storage placement, swap and backup — at which point an operator looking for "where does the log store live" had no reason to look under Disks, and a developer had two vocabularies for one subject.

The collision was already real and undetected: **`cixctl storage` existed for storage placement while `cixctl disks` existed for block devices**, two sibling top-level commands for the same noun.

## Decision

**The resource is `storage`. A disk is a device in it.**

| Was | Is |
|---|---|
| `GET /v1/disks` | `GET /v1/storage` |
| `/v1/disks/{disk_name}/...` | `/v1/storage/{name}/...` |
| `/v1/diskroles` | `/v1/storage-roles` |
| `{"disks": [...]}` | `{"storage": [...]}` |
| `{"diskroles": [...]}` | `{"storage_roles": [...]}` |
| `listDisks`, `formatDisk`, … | `listStorage`, `formatStorage`, … |
| schema `Disk`, `DiskRole` | `StorageDevice`, `StorageRole` |
| `cixctl disks` + `cixctl storage` | one `cixctl storage` |
| `cixctl diskrole` | `cixctl storage-role` |

Clean cut, no aliases, per this project's standing no-backward-compatibility rule.

**The two CLI commands merge into one rather than gaining a `placement` level.** Their subcommands do not overlap — `ls`/`format`/`unmount`/`free-space`/`partition-table`/`add-partition`/`rm-partition`/`grow-partition`/`format-status` against `logs`/`rebuildable` — so `cixctl storage logs migrate` and `cixctl storage format sdb` sit side by side. An artificial `storage placement logs migrate` would have added a level that exists only to record that these used to be two commands.

### Where "disk" survives, and why that is not the inconsistency being fixed

Two response fields keep the word: `is_os_disk` and `parent_disk`. They name a **physical disk**, which is what they are, and a partition really does have a parent disk.

The alternative spellings are worse. `is_os_device`/`parent_device` collide with **Devices**, which is a different top-level subject in this platform (PCI/USB/GPU passthrough). `is_os_storage`/`parent_storage` describe a category where a device is meant.

So the rule is: **the resource and the section are Storage; the individual thing is still a disk.** A category containing devices of a kind is not a second vocabulary.

## Consequences

**A contract change on 12 endpoints.** Every client breaks at once and visibly, which is the intended behaviour of a clean cut — a silently accepted old path would leave two names for one resource indefinitely, which is the thing being removed.

**Historical documents are not rewritten.** ADRs, `CHANGELOG.md` and `docs/roadmap/ROADMAP.md` keep `/v1/disks` where they recorded what was true at the time. An ADR is append-only and a changelog is a record; editing them to say something that was not true then would trade one inaccuracy for a worse one. Operator-facing documents (`docs/api/README.md`, the guides) are updated, because they describe what to do now.

**One pre-existing defect surfaced and was fixed on the way.** `test_diskpart` could not link at all — `disk.c` calls `partlabel_read()`/`partlabel_parent_name()` and its Makefile rule never listed `daemon/src/partlabel.c`. It went unnoticed because the release build compiles named targets plus `selftest`, never `all`, so a test binary that cannot build is never asked to. That is the same family as the SELFTESTS-membership trap already recorded in `CLAUDE.md`: a gate that does not run is not a gate.

## Alternatives considered

**Keep `/v1/disks` and add `/v1/storage` as an alias.** Rejected: two live names for one resource is exactly the state being left, and an alias makes it permanent rather than transitional.

**Rename only the dashboard's section label.** Rejected — it would have left the API and CLI saying `disks` while the UI said Storage, which is the drift this ADR exists to end, and the owner asked for the rename to reach the API.

**Rename the C modules too (`disk.c` → `storage.c`).** Deferred, deliberately. The operator-facing vocabulary is the contract and is what "one source of truth" means for anyone using this platform; renaming five source files and their symbols in the same change would bundle a mechanical churn with a contract change and make a failure harder to localise. Worth doing as its own step.
