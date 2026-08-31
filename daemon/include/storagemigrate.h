#ifndef STORAGEMIGRATE_H
#define STORAGEMIGRATE_H

#include "json.h"
#include "storageplacement.h"

#include <sys/types.h>

/*
 * ADR-0141 Phase 2: moves the live content of one of the three
 * daemon-wide storage singletons (state/rebuildable/log) from its
 * current active location to a new disk (or back to the default
 * OS-disk location) -- the async job half of the multi-disk storage
 * placement feature, mirroring diskformat.c's own fork+pidfd+epoll
 * shape exactly (RUNNING/READY/FAILED state machine, one job at a
 * time PER kind rather than one daemon-wide slot, since a state-
 * storage migration and a rebuildable-storage migration are
 * genuinely independent operations an operator might reasonably run
 * at the same time).
 *
 * Two-step design (see ADR-0141's own "Migration mechanics" section):
 * storagemigrate_start() forks a child that does the bulk, permission-
 * preserving recursive copy (treecopy_recursive(), shared with
 * pkg.c's own use of the same primitive -- No Parallel
 * Implementations) from source_dir to target_dir, while the daemon
 * keeps reading/writing source_dir completely normally the entire
 * time. Once main.c's own reactor reaps that child and calls
 * storagemigrate_completed() with a successful exit status,
 * storagemigrate_finalize() does one more synchronous pass (a second,
 * fast treecopy_recursive() -- deliberately not a true diff, since
 * "nothing changes much" during the async pass makes a full second
 * copy cheap enough for the small trees this covers) -- ONLY once
 * that returns success does the caller (main.c, which alone knows how
 * to repoint STATE_DIR/REBUILDABLE_DIR/call logstore_repoint()) treat
 * the migration as truly complete: repoint the live in-memory path,
 * persist the new placement via storageplacement_set(), and remove
 * the old location's data (persist_remove_tree()).
 */

enum storagemigrate_state {
	STORAGEMIGRATE_STATE_NONE, /* no migration job has ever run this daemon lifetime, for this kind */
	STORAGEMIGRATE_STATE_RUNNING,
	STORAGEMIGRATE_STATE_READY,
	STORAGEMIGRATE_STATE_FAILED,
};

enum storagemigrate_error {
	STORAGEMIGRATE_OK = 0,
	STORAGEMIGRATE_ERR_BUSY,             /* a migration for this kind is already running */
	STORAGEMIGRATE_ERR_NOT_FOUND,        /* target disk doesn't currently exist */
	STORAGEMIGRATE_ERR_IS_OS_DISK,
	STORAGEMIGRATE_ERR_WRONG_ROLE,       /* target disk exists but doesn't carry the matching role */
	STORAGEMIGRATE_ERR_NOT_MOUNTED,      /* target disk carries the role but isn't currently mounted */
	STORAGEMIGRATE_ERR_ALREADY_ACTIVE,   /* target disk (or NULL/default) is already the active placement */
	STORAGEMIGRATE_ERR_SPAWN_FAILED,
};

/*
 * Validates target_disk_name (NULL means "migrate back to the default
 * OS-disk placement" -- always a valid target) against disk_enumerate()
 * (must exist, must not be is_os_disk), diskrole_lookup() (must carry
 * the role matching kind -- "state-storage" for STORAGE_KIND_STATE,
 * etc.), and disk_enumerate()'s own mounted field (must currently be
 * mounted). os_containers_dir is passed straight through to
 * disk_enumerate() for its own OS-disk resolution.
 *
 * source_dir/target_dir are the real, already-resolved current and
 * proposed live paths -- this module has no knowledge of STATE_DIR/
 * REBUILDABLE_DIR/LOG_DIR globals of its own, the caller (main.c)
 * computes both from whichever of those applies to kind.
 *
 * On STORAGEMIGRATE_OK, *out_pid/*out_pidfd identify the forked child
 * for the caller to register with its own epoll reactor and later
 * waitpid() -- mirrors diskformat_start()'s own convention exactly.
 */
enum storagemigrate_error storagemigrate_start(enum storage_kind kind, const char *source_dir,
                                                const char *target_dir, const char *target_disk_name,
                                                const char *os_containers_dir, pid_t *out_pid,
                                                int *out_pidfd);

/*
 * Called by main.c after waitpid() on the child storagemigrate_start()
 * returned; exit_status is the raw WEXITSTATUS() (or -1 if the child
 * could not be reaped / did not exit normally). Updates this kind's
 * own state to STORAGEMIGRATE_STATE_READY or _FAILED.
 */
void storagemigrate_completed(enum storage_kind kind, int exit_status);

/*
 * Only meaningful right after storagemigrate_completed() has just set
 * kind's state to READY. Runs the second, synchronous copy pass
 * described above. Returns 0 (caller should now do the real repoint)
 * or -1 (the synchronous pass itself failed -- this function has
 * already moved kind's own state to FAILED with a real error message,
 * the caller does not need to do anything further for this job).
 */
int storagemigrate_finalize(enum storage_kind kind);

/* The exact source_dir/target_dir/target_disk_name (NULL-safe: "" if
 * the job migrated back to the default placement) this kind's most
 * recently STARTED job was given -- valid for the caller's own
 * finalize-time repoint logic once storagemigrate_finalize() returns
 * 0. Empty string if no job has ever run for this kind. */
/*
 * This kind's current error text, or "" when there is none.
 *
 * Issue #172: the failure reason used to be readable ONLY by polling
 * GET .../migrate. That is the wrong place for it to live alone -- an
 * operator reconstructing what happened on a host reads the log store,
 * and a migration that failed hours ago has usually stopped being
 * polled by then. Exposed so the event loop can put the real reason
 * where that operator will actually look.
 */
const char *storagemigrate_job_error(enum storage_kind kind);

const char *storagemigrate_job_source_dir(enum storage_kind kind);
const char *storagemigrate_job_target_dir(enum storage_kind kind);
const char *storagemigrate_job_target_disk(enum storage_kind kind);

/*
 * Reports the most recent (or currently running) job for kind:
 * {"state":..., "disk":..., "error":...} -- mirrors diskformat_write_
 * status_json()'s own shape. Writes {"state":"none"} if no job has
 * ever run for this kind.
 */
void storagemigrate_write_status_json(struct json_writer *w, enum storage_kind kind);

#endif /* STORAGEMIGRATE_H */
