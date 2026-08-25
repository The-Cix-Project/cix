#ifndef STORAGEPLACEMENT_H
#define STORAGEPLACEMENT_H

#include "json.h"

/*
 * ADR-0141 Phase 2: which disk (if any) is the *active* placement for
 * each of Cix's own daemon-wide storage singletons -- state-storage,
 * rebuildable-storage, log-storage. Persisted at a fixed g_base_dir
 * path (STORAGE_PLACEMENT_PATH, main.c), deliberately NEVER inside
 * STATE_DIR itself -- the same bootstrap-circularity reasoning
 * DISKROLE_STATE_PATH's own comment documents: this file has to be
 * readable before STATE_DIR's own real location can even be
 * determined, so it can't live inside the thing it's describing.
 *
 * A NULL placement (the default, and what every fresh install starts
 * with) means "the default OS-disk location" -- STATE_DIR/REBUILDABLE_
 * DIR/LOG_DIR's own g_base_dir-relative default, unchanged from before
 * this feature existed. A non-NULL placement names a real disk (by its
 * disk_enumerate() name, e.g. "sdc") that must carry the matching role
 * (diskrole.h's DISKROLE_STATE_STORAGE for STORAGE_KIND_STATE, etc.)
 * and be currently mounted for the daemon to actually use it -- this
 * module only stores the pointer, storagemigrate.c and main.c's own
 * boot-time resolution are what act on it.
 */

enum storage_kind {
	STORAGE_KIND_STATE = 0,
	STORAGE_KIND_REBUILDABLE,
	STORAGE_KIND_LOG,
	/*
	 * issue #28: unlike the three above, swap has no live directory of
	 * real, worth-preserving data to migrate -- the swap FILE's own
	 * content is transient and discardable by definition, so there is
	 * no storagemigrate.c-style move job for this kind and never will
	 * be. Reusing this same pure-pointer module anyway rather than a
	 * fourth parallel "which disk for X" mechanism (One Source of
	 * Truth) -- daemon/src/swap.c's own POST /system/swap handler
	 * resolves and repoints directly, no migration step involved.
	 */
	STORAGE_KIND_SWAP,
};

enum storageplacement_error {
	STORAGEPLACEMENT_OK = 0,
	STORAGEPLACEMENT_ERR_PERSIST_FAILED,
};

/*
 * Loads state_path (the persisted placement pointers, if any) at
 * startup. Absent file (first-ever boot, or a --data-dir= test
 * invocation) is not an error -- every kind defaults to NULL (the
 * default OS-disk placement). Returns 0, or -1 if the file exists but
 * is malformed.
 */
int storageplacement_init(const char *state_path);

/*
 * The disk currently recorded as kind's active placement, or NULL for
 * the default OS-disk placement. Pointer is valid until the next
 * storageplacement_set() call for the same kind.
 */
const char *storageplacement_get(enum storage_kind kind);

/*
 * Persists disk_name (or clears the placement back to the default
 * OS-disk one when disk_name is NULL) for kind. Pure bookkeeping --
 * does not itself validate the disk's role/mount state, move any data,
 * or repoint any live in-memory path; storagemigrate.c's own migration
 * job does the real work and calls this only once that work has
 * actually completed successfully.
 */
enum storageplacement_error storageplacement_set(enum storage_kind kind, const char *disk_name);

/* Writes {"disk": "sdc"|null} for kind into w -- the shape every
 * GET /v1/system/{state,rebuildable,log}-storage response has. */
void storageplacement_write_json(struct json_writer *w, enum storage_kind kind);

#endif /* STORAGEPLACEMENT_H */
