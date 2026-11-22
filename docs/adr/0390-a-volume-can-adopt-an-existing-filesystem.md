# 0390 — A volume can adopt an existing filesystem

## Status

Accepted, 2026-11-22, by the owner, for #725: *"yes, not read only though"*, on the plan to share a 5 TB disk holding their own data from `nas` as `share`, read-write, without formatting it.

## Context

A disk with the `volume-storage` role that already carries ext4 or btrfs is mounted at once, unformatted (#660). But every volume was a new, empty directory Cix created on it, `<disk>/volumes/<name>`. An operator's existing data, at the filesystem's root, could not be given to a container at all.

Cix also does things to its volumes that are right for data it created and wrong for an operator's disk: delete removes the tree, migrate copies and removes it, owner chowns it, quota attaches a qgroup, backups snapshot it, restore empties it before copying a snapshot in, and usage may walk the whole tree on the event loop.

Ownership was already solved for the owner's case: a user-namespace container's volumes are id-mapped (`volume_idmap_fds`), so a file owned by uid 1000 on the disk is uid 1000 inside the container, and nothing needs to be chowned.

## Decision

1. **`POST /v1/volumes` with `"adopt": true` and `"disk"` takes that disk's existing filesystem, unchanged, as the volume** (`volume_adopt()`). The disk must carry the `volume-storage` role and be mounted, as for any volume. Nothing is created or written on the disk; the record is all that changes.

2. **An adopted volume's tree is the filesystem's root,** found by the filesystem's UUID through `diskrole_resolve_recorded()`, so the volume follows the drive whatever the kernel names it this boot. **Absent or unmounted means no path**, never the default location: a container naming the volume fails to create rather than being handed an empty directory to write into.

3. **Cix never changes an adopted volume's data.** Migrate, convert, owner, quota, enabling backups, taking a snapshot and restoring each refuse with 409 (`VOLUME_ERR_ADOPTED`, `VOLUMEBACKUP_ERR_ADOPTED`). Delete forgets the record and leaves the filesystem as it is. Formatting the disk, or removing its role, is refused while an adopted volume names its filesystem, matched by UUID as well as by name.

4. **Usage is the filesystem's own used space** (`statvfs`), source `filesystem`, never a tree walk of an operator's disk on the event loop.

5. **One adoption per filesystem.** A second volume adopting the same UUID is refused.

## Alternatives considered

- **Copy the data into a Cix volume.** Rejected: it doubles 5 TB of data, takes hours, and still leaves the original disk to manage.
- **A container `mounts` field naming a host path.** Rejected: a second way to put host storage into a container beside volumes, without the volume's protections or its place in the API.
- **Adopt read-only first.** Offered, and the owner declined: the share is read-write from the start.

## Consequences

- **What an operator does with a disk that holds data:** give it the `volume-storage` role, which mounts it unformatted, then `cixctl volume create --name=NAME --disk=DISK --adopt`. An empty disk is formatted and gets ordinary Cix volumes as before.
- **The operator keeps ownership of the files.** New files written through a container are owned by whatever uid wrote them inside that container, mapped by the same id-mapping.
- **An adopted volume is never backed up by Cix.** Backing up an operator's own disk is the operator's arrangement, made outside Cix.
- `test_volume` seeds an adopted volume whose filesystem is absent and checks every refusal, the null host path and the forget-only delete. The real disk, `share` on gibsson-1's 5 TB drive, is the live proof.
