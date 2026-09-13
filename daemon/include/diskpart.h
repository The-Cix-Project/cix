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
 * image/src/cix-install.c's own auto_partition() already uses for
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
/*
 * Issue #94: growing a partition is two operations. The table entry
 * grows (sfdisk), and then the filesystem inside it has to be grown to
 * match, or the extra space is simply invisible to anything using it.
 * resize2fs requires a clean filesystem, which is what e2fsck is for.
 */
#define DISKPART_RESIZE2FS_BIN "/usr/sbin/resize2fs"
#define DISKPART_E2FSCK_BIN "/usr/sbin/e2fsck"
/*
 * Issue #163: the btrfs multi-tool, for the other half of the same
 * two-step growth.
 *
 * btrfs is this platform's own storage substrate (ADR-0207), and until
 * this existed a btrfs partition could not be grown at all: the table
 * entry would grow and the filesystem would not, which is precisely the
 * "the extra space is real but not usable" outcome the resize path
 * exists to prevent. Its constraint is the opposite of resize2fs's --
 * `btrfs filesystem resize` operates on a MOUNTED filesystem -- which
 * is why this is a different flow rather than another binary name.
 */
#define DISKPART_BTRFS_BIN "/usr/sbin/btrfs"

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
	/*
	 * Issue #140: distinct from IS_OS_DISK, because they are different
	 * situations and an operator needs to be told which one they hit.
	 * IS_OS_DISK means "this disk's table can never be rewritten";
	 * PROTECTED_PARTITION means "this particular partition is one of
	 * the four the machine boots from" -- and on the same disk, its
	 * neighbours further along are perfectly ordinary. One message
	 * covering both would be wrong about one of them.
	 */
	DISKPART_ERR_PROTECTED_PARTITION,
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
	/* Issue #94's own refusals, each a different thing to tell an
	 * operator and so each its own value rather than one generic
	 * "cannot resize". */
	DISKPART_ERR_SHRINK_REFUSED,
	DISKPART_ERR_NO_ROOM_AFTER,
	DISKPART_ERR_FS_UNSUPPORTED,
	DISKPART_ERR_FS_UNCLEAN,
	DISKPART_ERR_RESIZE_FS_FAILED,
	/*
	 * The table entry grew but the kernel is still reporting the old
	 * size, so growing the filesystem would grow it to the OLD bound
	 * and report success -- the silent no-op this whole path exists to
	 * avoid. Reported instead of pressed through (#163).
	 */
	DISKPART_ERR_KERNEL_SIZE_STALE,
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
/*
 * Issue #140: the OS disk's first four partitions are structurally
 * untouchable -- cix-esp, cix-root-a, cix-root-b, cix-config, in the
 * fixed order image/src/cix-install.c lays down. Destroy any of them
 * and the machine does not boot, with no remote recovery. Everything
 * after them is ordinary allocatable space.
 *
 * Exposed rather than kept private so the policy can be tested
 * directly: it is a rule about which operations are OFFERED, and a
 * rule that can only be observed by attempting the dangerous thing on
 * real hardware is a rule nothing will ever check.
 */
#define DISKPART_OS_PROTECTED_PARTITIONS 4

/* Trailing partition number: "vda5" -> 5, "nvme0n1p5" -> 5, 0 when
 * there is no numeric suffix. */
int diskpart_partition_number(const char *name);

/*
 * Whether this partition must never be offered for delete or resize.
 * Fails CLOSED: a partition on the OS disk whose number cannot be
 * determined counts as protected -- being wrong that way costs an
 * operator some manual work, the other way costs a bootable machine.
 */
int diskpart_partition_protected(const char *name, int is_os_disk);

/*
 * True when this device must never be given a role, formatted or
 * unmounted: the OS disk itself, and the structural partitions of the
 * OS layout. An operator-created partition in the OS disk's reserved
 * free space is NOT untouchable -- see the definition for why that
 * distinction is the point (issue #9).
 */
int diskpart_os_layout_untouchable(const char *name, int is_os_disk, int is_partition);

/*
 * What the partitioning tool itself last said, or "" if nothing.
 * Only meaningful immediately after a DISKPART_ERR_SFDISK_FAILED.
 */
const char *diskpart_last_tool_error(void);

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

/*
 * Grows partition_name to size_mib (0 meaning "all contiguous free
 * space immediately after it"). Grow only -- see diskpart.c for why
 * shrinking is refused rather than supported.
 *
 * A MOUNTED btrfs partition is grown online, in place, on its live
 * mountpoint -- the one path that lets the daemon's own data directory
 * (cixd --data-dir=, which can never be unmounted) be extended. Every
 * other case is grown unmounted (DISKPART_ERR_MOUNTED otherwise). The
 * table entry is grown with --no-tell-kernel + BLKPG_RESIZE_PARTITION
 * so it works on a disk carrying mounted partitions; see diskpart.c.
 */
enum diskpart_error diskpart_resize(const char *disk_name, const char *partition_name,
                                     const char *os_containers_dir, unsigned long long size_mib);

/*
 * The pure size arithmetic of a grow, split out so it can be tested
 * without a block device: given the partition's current size and the
 * contiguous free room immediately after it (both in bytes) and the
 * requested size_mib (0 meaning "all that room"), sets *out_want_bytes
 * to the target total size. Refuses a genuine shrink (want < current)
 * and a request larger than current+room. want == current is allowed
 * and means "the table is already this size, finish the filesystem
 * grow" -- the idempotent retry after a prior fs-grow failure.
 */
enum diskpart_error diskpart_resize_target(unsigned long long current_bytes,
                                            unsigned long long room_bytes,
                                            unsigned long long size_mib,
                                            unsigned long long *out_want_bytes);

#endif /* DISKPART_H */
