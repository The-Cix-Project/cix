#ifndef DATADIR_H
#define DATADIR_H

#include "json.h"

/*
 * What lives in the daemon's data directory, and who owns each piece of
 * it (#249, GET /v1/system/data-dir).
 *
 * The data directory is where every placement that has not been moved
 * to a disk of its own keeps its tree. Nothing could say what was in
 * it: on 192.168.15.95, 2026-10-29, the cix-containers partition held
 * 4.28 GB while the log store (capped at 100 MB), the one volume placed
 * there (0.75 MB) and swap (off) accounted for about 0.1 GB of it.
 * Relocating the directory (#249) cannot be designed without knowing
 * what it would carry.
 *
 * One entry per name directly beneath base_dir. Its bytes are what the
 * filesystem allocated to it (st_blocks), not the apparent size
 * treesize_bytes() reports, because the question is what holds the
 * disk. Two trees sharing extents (reflinks, btrfs snapshots) are each
 * charged in full, so the entries can add up to more than the
 * filesystem's used bytes; the response carries both, and says so.
 *
 * A subtree that is a separate mount is not walked: its bytes are on
 * another filesystem. It is counted in the entry's mounts_skipped, and
 * an entry that is itself a mount point has type "mount". Btrfs
 * subvolumes are not mounts and are walked.
 *
 * An unbounded walk, so it runs only in a helper child
 * (datadir_work() in main.c); test_blocking_waits counts its call
 * sites.
 */

/*
 * Where each owner's tree lives NOW. A directory beneath base_dir named
 * for an owner whose live path is elsewhere is "in_use": false -- a tree
 * a placement move left behind. NULL means the owner has no tree.
 */
struct datadir_live {
	const char *log_dir;
	const char *rebuildable_dir;
	const char *volumes_dir;
	const char *containers_dir;
	const char *swap_dir;
	/* ADR-0364: the two bootstrap state files' live paths, in STATE_DIR
	 * -- or the old data-directory path for a boot whose move failed. */
	const char *disk_roles_path;
	const char *storage_placement_path;
	/* Whether a container of this name is defined, for the children of
	 * containers/; NULL reports none as defined. */
	int (*container_defined)(const char *name);
	/* ADR-0364 decision 2: why the data directory is not persistent this
	 * boot (its partition did not mount and it is a tmpfs), or NULL. */
	const char *fault;
};

/*
 * Writes the report into w. measured_at is the caller's clock, so a
 * test can fix it.
 *
 * entry NULL reports the names directly beneath base_dir. entry set
 * reports the names directly beneath base_dir/entry instead, one level
 * down, which is how a large entry is broken into what it holds:
 *   - a child of containers/ has owner "container", and in_use says
 *     whether a container of that name is defined -- a directory for a
 *     container that no longer exists is "in_use": false;
 *   - .discard, wherever it is, has owner "discard-queue": trees set
 *     aside for ADR-0341's delete queue, not yet deleted;
 *   - any other child takes its parent's owner and in_use.
 * entry must be a single name (no '/', not "." or ".."); the caller
 * checks that.
 *
 * Returns 0, or -1 with errno set when the directory itself cannot be
 * read; an entry that cannot be walked is reported with "error" rather
 * than failing the report.
 */
int datadir_write_json(struct json_writer *w, const char *base_dir, const char *entry,
                       const struct datadir_live *live, long long measured_at);

#endif /* DATADIR_H */
