#ifndef DISKPART_H
#define DISKPART_H

/*
 * Multi-disk management: partition-level operations on a disk already
 * enumerated by disk.c. Once a partition exists, disk_enumerate()
 * reports it as an ordinary struct discovered_disk entry
 * (is_partition=1, parent_disk set) -- diskrole.c and diskformat.c
 * already operate purely on disk_enumerate()'s own name/dev_path/
 * is_os_disk fields, so role assignment and format/mount work on a
 * partition with zero changes to either module; this module only ever
 * needs to create/grow/shrink the partition table itself.
 *
 * sfdisk (util-linux) is the real tool driving this -- the same one
 * image/src/thinc-install.c's own auto_partition() already uses for
 * the fixed OS-disk layout at install time. This module drives it the
 * same way (a scripted stdin body) but incrementally, on an
 * operator-chosen data disk, after install:
 *   - diskpart_create_table(): "label: gpt\n" -- a fresh, empty table.
 *   - diskpart_add(): "size=<N>MiB, type=linux, name=\"<name>\"\n" (or
 *     no size= field at all for "consume all remaining space", the
 *     same convention auto_partition()'s own last line already relies
 *     on) via `sfdisk --append`, which -- unlike a bare `sfdisk` -- adds
 *     to the existing table rather than replacing it.
 *   - diskpart_delete(): `sfdisk --delete <parent> <partno>`.
 *
 * Both create and add are near-instantaneous (metadata-only, unlike
 * mkfs.ext4/btrfs) -- run synchronously, blocking the reactor for
 * their real, brief duration, the same way rtnetlink bridge creation
 * and other quick admin operations already are; no pidfd/epoll
 * job-tracking needed (contrast diskformat.h's own async two-step
 * design, which exists specifically because mkfs is genuinely slow).
 *
 * A disk is used in exactly one of two mutually-exclusive modes: role
 * assigned directly to the whole disk (today's pre-existing model), or
 * partitioned with roles assigned to its individual partitions instead
 * -- diskpart_create_table()/diskpart_add() both refuse to touch a
 * whole disk that already has a role of its own, and diskrole_create()
 * already refuses (via disk_enumerate()'s own is_os_disk propagation,
 * not a diskpart.c concept) to assign a role to a partition of the OS
 * disk. Nothing in this module or diskrole.c stops assigning a role to
 * a whole disk that also happens to have partitions on it from a prior
 * life, though -- validated at the point of the destructive action
 * (create-table/add) instead, matching diskformat.c's own "the
 * destructive action carries its own safety checks" convention.
 */

#define DISKPART_SFDISK_BIN "/usr/sbin/sfdisk"

/* Same charset/length rules as every other simple resource name in
 * this project (simple_name_is_valid(), namecheck.h) -- a GPT
 * partition name allows more (spaces, unicode), but restricting to
 * this project's own established charset both keeps validation
 * consistent and guarantees the name can never contain a `"` or `\`
 * that would need escaping inside the sfdisk script's own
 * name="..." field. */
#define DISKPART_NAME_MAX 32

enum diskpart_error {
	DISKPART_OK = 0,
	DISKPART_ERR_INVALID_DISK_NAME,
	DISKPART_ERR_NOT_FOUND,
	DISKPART_ERR_IS_PARTITION,   /* target of create-table/add must be a whole disk */
	DISKPART_ERR_NOT_A_PARTITION, /* target of delete must itself be a partition */
	DISKPART_ERR_WRONG_PARENT,   /* delete target's real parent isn't the disk named in the URL */
	DISKPART_ERR_IS_OS_DISK,
	DISKPART_ERR_HAS_ROLE,
	DISKPART_ERR_MOUNTED,
	DISKPART_ERR_INVALID_PART_NAME,
	DISKPART_ERR_INVALID_SIZE,
	DISKPART_ERR_SFDISK_FAILED,
	/*
	 * sfdisk is not installed on this host -- execve() never ran it.
	 * Kept separate from SFDISK_FAILED because they need different
	 * actions from an operator and the two used to be indistinguishable
	 * from the API, which is how a missing binary went unnoticed for a
	 * whole release.
	 */
	DISKPART_ERR_SFDISK_MISSING,
};

/*
 * Writes a fresh, empty GPT partition table to disk_name -- destructive,
 * wipes any existing partition table and everything on it. Rejects a
 * disk_name that doesn't currently resolve to a real whole disk
 * (DISKPART_ERR_NOT_FOUND), is itself a partition (DISKPART_ERR_IS_PARTITION),
 * is the OS disk (DISKPART_ERR_IS_OS_DISK), already has a role assigned
 * directly to it (DISKPART_ERR_HAS_ROLE -- delete the role first if this
 * disk is being repurposed from whole-disk to partitioned use), or is
 * currently mounted (DISKPART_ERR_MOUNTED).
 */
enum diskpart_error diskpart_create_table(const char *disk_name, const char *os_containers_dir);

/*
 * Appends one new partition to disk_name's existing partition table
 * (never touches any partition already on it). part_name becomes the
 * new partition's GPT name attribute (purely cosmetic -- disk_enumerate()
 * reports the new partition by its real kernel device name, e.g. "sdb2",
 * same as every other disk entry, not by this GPT name). size_mib is the
 * new partition's size in MiB, or 0 to consume all remaining space on
 * the disk (must be the last partition added if so -- sfdisk itself
 * enforces this, surfaced as DISKPART_ERR_SFDISK_FAILED if violated).
 *
 * Same disk_name preconditions as diskpart_create_table() (must be a
 * present, non-partition, non-OS whole disk with no role of its own and
 * not mounted) -- adding a partition to a disk currently in whole-disk
 * role/format use would corrupt that use exactly as much as replacing
 * its table would.
 */
enum diskpart_error diskpart_add(const char *disk_name, const char *os_containers_dir,
                                  const char *part_name, unsigned long long size_mib);

/*
 * Deletes one partition (partition_name, e.g. "sdb2") from disk_name's
 * table -- every other partition on the disk is untouched. Rejects a
 * partition_name that doesn't currently exist (DISKPART_ERR_NOT_FOUND),
 * isn't actually a partition (DISKPART_ERR_NOT_A_PARTITION), doesn't
 * belong to disk_name (DISKPART_ERR_WRONG_PARENT -- the URL's disk_name
 * must be this partition's real parent), is part of the OS disk
 * (DISKPART_ERR_IS_OS_DISK), still has a role assigned
 * (DISKPART_ERR_HAS_ROLE -- delete the role first, same no-silent-data-
 * loss convention diskrole_delete()/devicemap_delete() already use), or
 * is currently mounted (DISKPART_ERR_MOUNTED).
 */
enum diskpart_error diskpart_delete(const char *disk_name, const char *partition_name,
                                     const char *os_containers_dir);

/* Issue #95: how much room is left in a disk's partition table. */
#define DISKPART_FREE_EXTENT_MAX 16

struct diskpart_free_extent {
	unsigned long long start_sector;
	unsigned long long sectors;
};

struct diskpart_free_space {
	/*
	 * Whether the disk has a partition table at all. Reported
	 * separately rather than inferred from a zero total, because
	 * "partitioned and full" and "not partitioned yet" are different
	 * problems with different fixes and sfdisk reports the second as
	 * simply no output.
	 */
	int has_table;
	unsigned long long total_free_bytes;
	unsigned long long largest_free_sectors;
	unsigned long long sector_bytes;
	struct diskpart_free_extent extents[DISKPART_FREE_EXTENT_MAX];
	int extent_count;
};

/*
 * Fills out with disk_name's free space, straight from sfdisk rather
 * than computed by subtracting partition sizes -- alignment, the GPT's
 * own reserved areas, and gaps left by an earlier delete all make that
 * subtraction wrong. On demand only: it forks a subprocess, so it is
 * deliberately not part of disk_enumerate()'s per-poll work.
 */
/*
 * The parsing half, against a device path directly -- exposed so a test
 * can drive it against a real disk image, since sfdisk treats a plain
 * file identically and this sandbox has no block device nodes.
 */
enum diskpart_error diskpart_free_space_from_path(const char *dev_path,
                                                   struct diskpart_free_space *out);

enum diskpart_error diskpart_free_space(const char *disk_name, const char *os_containers_dir,
                                         struct diskpart_free_space *out);

#endif /* DISKPART_H */
