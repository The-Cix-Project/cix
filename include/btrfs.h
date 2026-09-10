#ifndef CIX_BTRFS_H
#define CIX_BTRFS_H

/*
 * btrfs storage primitives (ADR-0207).
 *
 * The platform's writable storage moves to btrfs so that an image
 * version is a subvolume and a container's rootfs is a writable
 * snapshot of it -- copy-on-write, persistent, and (unlike an overlay)
 * a plain mount that can carry MOUNT_ATTR_IDMAP, which is what makes
 * user namespaces the default possible (ADR-0179's overlay dead end).
 *
 * Every call has a non-btrfs fallback (plain directory / recursive
 * copy / recursive remove), because ext4 is retired only in the final
 * phase of the migration, not the first -- a host still on ext4 must
 * keep working identically until btrfs is proven. The fallback is the
 * documented transition mechanism, not a permanent parallel path: it
 * goes away with ext4.
 *
 * Lives in the runtime lib (src/) rather than the daemon, because the
 * phase-2 consumer is container.c -- also in the runtime lib, which
 * must not depend on the daemon -- so this module is self-contained on
 * libc + linux_compat.h alone.
 */

/*
 * 1 if `path` (which must already exist) is on a btrfs filesystem, 0
 * otherwise (including any statfs error -- an unreadable path is
 * treated as "not btrfs", the safe answer that takes the fallback).
 */
int cix_btrfs_is_backing(const char *path);

/*
 * Create `path`. If its parent directory is on btrfs, `path` is a real
 * subvolume (BTRFS_IOC_SUBVOL_CREATE); otherwise it is a plain
 * directory (mkdir). The parent must already exist either way.
 * Idempotent: an already-present `path` of the matching kind is
 * success (EEXIST tolerated), mirroring the mkdir convention the image
 * store already relies on across crash/recreate.
 *
 * Returns 0 on success, -1 with errno set otherwise.
 */
int cix_btrfs_subvol_create_or_dir(const char *path);

/*
 * Is `path` a btrfs SUBVOLUME, as opposed to an ordinary directory that
 * merely lives on btrfs? 0 for both "a plain directory" and "not on
 * btrfs at all", so a caller on ext4 gets a truthful no.
 *
 * The distinction decides what a caller may do with the path: a qgroup
 * attaches to a subvolume and BTRFS_IOC_SNAP_CREATE_V2 refuses a plain
 * directory with EINVAL, so code that needs either has to ask first
 * rather than assume the layout it expected.
 */
int cix_btrfs_is_subvolume(const char *path);

/*
 * Create `dst` as a WRITABLE snapshot of `src` if both live on btrfs
 * (BTRFS_IOC_SNAP_CREATE_V2 -- O(1), copy-on-write, sharing every
 * unchanged extent with `src`); otherwise recursively copy `src` to a
 * new plain directory `dst`. `dst` must not already exist; its parent
 * must.
 *
 * `src` no longer has to be a subvolume. It used to, and a plain
 * directory on btrfs made the ioctl fail with EINVAL -- which is what
 * a storage migration produces, since copying content faithfully turns
 * subvolumes into ordinary directories (#180). Such a `src` is now
 * copied into a freshly created subvolume instead: one full copy, once,
 * after which `dst` is a real subvolume and every later call snapshots
 * from it in O(1).
 *
 * Returns 0 on success, -1 with errno set otherwise.
 */
int cix_btrfs_snapshot_or_copy(const char *src, const char *dst);

/*
 * Remove `path`. If it is a btrfs subvolume, BTRFS_IOC_SNAP_DESTROY;
 * otherwise a recursive unlink. An already-absent `path` is success.
 *
 * Returns 0 on success, -1 with errno set otherwise.
 */
/*
 * Reproduces a subvolume at dst when it cannot be a snapshot of src,
 * i.e. when the two are on different filesystems. Copies in full --
 * sharing does not cross filesystems -- but the result IS a subvolume,
 * which a plain directory copy would not be. Used by cross-disk
 * container storage migration; container creation deliberately does
 * NOT use it (see cix_btrfs_snapshot_or_copy()).
 */
int cix_btrfs_subvol_copy(const char *src, const char *dst);

int cix_btrfs_subvol_delete_or_rmtree(const char *path);

/*
 * Apply a btrfs qgroup hard limit of `bytes` EXCLUSIVE bytes to the
 * subvolume at `path` (enabling quotas on the filesystem first,
 * idempotently). Exclusive -- not referenced -- because a container's
 * rootfs snapshot shares every unchanged extent with its image: a
 * referenced-bytes limit would count the whole image against the
 * container from the first instant (a 100MB quota on a 1GB image could
 * never even start), while exclusive bytes are precisely the
 * container's own divergence -- the same "quota = your diff" semantics
 * the overlay upperdir quota (ADR-0103) always had.
 *
 * Returns 0 on success, -1 with errno set otherwise. Only meaningful
 * on btrfs; callers gate on the storage mode, not this function.
 */
int cix_btrfs_qgroup_limit_excl(const char *path, unsigned long long bytes);

/*
 * Quota accounting state reported alongside a qgroup read, because a
 * number that is merely stale reads exactly like a number that is
 * correct.
 */
enum cix_btrfs_qgroup_state {
	CIX_BTRFS_QGROUP_OK = 0,
	/* Accounting is mid-rescan or flagged inconsistent: `used` is a
	 * real figure but may be low for data that predates the rescan. */
	CIX_BTRFS_QGROUP_STALE = 1,
	/* Simple quotas (squota). Extents are attributed on a different
	 * rule, so `used` does not mean what it means here. */
	CIX_BTRFS_QGROUP_SIMPLE = 2
};

/*
 * Read back the qgroup of the SUBVOLUME at `path`: its exclusive bytes
 * in use and its MAX_EXCL hard limit, the two numbers
 * cix_btrfs_qgroup_limit_excl() writes the second of.
 *
 * btrfs has no "describe this qgroup" ioctl, so this searches the
 * filesystem's quota tree directly -- the same tree `btrfs qgroup show`
 * walks. Two point lookups, each with min == max, rather than one range
 * search: a tree key is a 136-bit (objectid, type, offset) value
 * compared as a whole, so a range spanning the INFO and LIMIT types
 * also spans every OTHER subvolume's qgroup id between them, and on a
 * host whose containers are all subvolumes the item wanted need not be
 * in the first page at all. Two exact lookups return 0 or 1 item each
 * and cannot paginate.
 *
 * WHY IT REFUSES A PLAIN DIRECTORY. Resolving the subvolume id uses
 * BTRFS_IOC_INO_LOOKUP, which on an ordinary directory succeeds and
 * returns the id of the subvolume CONTAINING it -- so a volume that
 * predates ADR-0207's subvolume conversion would silently be answered
 * with the whole parent's accounting. A wrong answer, not an error,
 * which is the failure mode this project has been bitten by before, so
 * the subvolume test comes first and a plain directory is EINVAL.
 *
 * `used` is EXCLUSIVE bytes -- extents this subvolume alone references,
 * which is what a MAX_EXCL limit is enforced against. It is allocated
 * space, not apparent size, so sparse, compressed and reflinked data
 * all read smaller than a directory walk would report. It is also only
 * as fresh as the last transaction commit (~30 s), because that is when
 * btrfs settles qgroup accounting.
 *
 * `limit` is 0 when no MAX_EXCL limit is set -- checked via the item's
 * own flag bit, since max_excl is undefined when the bit is clear and
 * max_rfer is a different limit entirely.
 *
 * Any of the three out-params may be NULL. Returns 0 on success; -1
 * with errno set on failure -- ENOTTY/ENOTSUP if `path` is not on
 * btrfs, EINVAL if it is not a subvolume, ENOENT if quotas were never
 * enabled on the filesystem (there is no quota tree to search), and
 * ENODATA if quotas are on but this subvolume has no accounting record
 * yet.
 */
int cix_btrfs_qgroup_query(const char *path, unsigned long long *used,
                           unsigned long long *limit,
                           enum cix_btrfs_qgroup_state *state);

#endif /* CIX_BTRFS_H */
