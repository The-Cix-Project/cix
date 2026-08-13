#ifndef CONTAINERSTORAGEMIGRATE_H
#define CONTAINERSTORAGEMIGRATE_H

#include "json.h"
#include "registry.h"
#include "storagemigrate.h" /* reuses enum storagemigrate_state -- same three-state shape */

#include <sys/types.h>

/*
 * ADR-0142 Section 4: the exact same two-step migration-job mechanism
 * ADR-0141's storagemigrate.c already established (fork+pidfd bulk
 * async copy via treecopy_recursive(), then a second synchronous copy
 * pass once reaped) applied to one container's own overlay directory
 * instead of a daemon-wide singleton -- with one real, necessary
 * difference storagemigrate.c never has to deal with: state/rebuildable/
 * log-storage are each a single global slot (one migration of that kind
 * at a time, daemon-wide), but a container-storage migration is keyed by
 * *container name* -- many containers could each have their own
 * migration in flight independently. Job state here is therefore a
 * small keyed table (CONTAINERDEF_MAX slots, found/allocated by name),
 * not a fixed per-kind array.
 *
 * The other real difference (why this isn't just storagemigrate.c with
 * a fourth enum value): a daemon-wide singleton is only ever read/
 * written by this single-threaded reactor itself, so repointing its
 * cached path is safe while the bulk copy runs. A container's own
 * overlay is actively read/written by that container's own live
 * process the whole time -- the bulk copy here is knowingly a first,
 * possibly-stale pass; the caller (main.c) stops the container before
 * calling containerstoragemigrate_finalize()'s second pass, so that
 * pass alone captures a fully consistent, quiescent copy. See main.c's
 * finalize_container_storage_migration() for the full cutover sequence
 * (stop, final copy, patch the persisted definition's own "disk"
 * field, replay-restart).
 */

enum containerstoragemigrate_error {
	CONTAINERSTORAGEMIGRATE_OK = 0,
	CONTAINERSTORAGEMIGRATE_ERR_BUSY,           /* a migration for this container is already running */
	CONTAINERSTORAGEMIGRATE_ERR_ALREADY_ACTIVE, /* target disk (or NULL/default) is already where this container lives */
	CONTAINERSTORAGEMIGRATE_ERR_SPAWN_FAILED,
	CONTAINERSTORAGEMIGRATE_ERR_TABLE_FULL,     /* CONTAINERDEF_MAX distinct containers have ever migrated storage -- see doc comment */
};

/*
 * Starts (or rejects, per the enum above) a container's own bulk async
 * copy from source_dir to target_dir. Unlike storagemigrate_start(),
 * this module has no disk-role/mount-state validation of its own --
 * the caller already re-uses resolve_container_disk_root() (the exact
 * same validation POST /v1/containers itself already applies) to
 * resolve and validate target_dir before ever calling here.
 * target_disk_name is NULL for a migration back to the default
 * OS-disk placement, matching every other storage-placement kind's own
 * convention. On success, *out_pid/*out_pidfd identify the forked
 * child for the caller to register with its own epoll reactor.
 */
enum containerstoragemigrate_error containerstoragemigrate_start(const char *container_name,
                                                                   const char *source_dir,
                                                                   const char *target_dir,
                                                                   const char *target_disk_name,
                                                                   pid_t *out_pid, int *out_pidfd);

/* Same contract as storagemigrate_completed(), keyed by container_name. */
void containerstoragemigrate_completed(const char *container_name, int exit_status);

/* Same contract as storagemigrate_finalize(), keyed by container_name. */
int containerstoragemigrate_finalize(const char *container_name);

/* Same contract as storagemigrate's own trio, keyed by container_name.
 * Empty string if no job has ever run for this container. */
const char *containerstoragemigrate_job_source_dir(const char *container_name);
const char *containerstoragemigrate_job_target_dir(const char *container_name);
const char *containerstoragemigrate_job_target_disk(const char *container_name);

/*
 * Marks container_name's own most recent job FAILED with a caller-given
 * message, without going through the normal completed()->finalize()
 * flow -- used by main.c when the final synchronous copy pass fails
 * (containerstoragemigrate_finalize() already set FAILED for that
 * exact case) or when the cutover itself fails after a successful
 * copy (the container's own replay-restart failing, a real but rare
 * case distinct from a copy failure) -- so GET .../migrate-storage
 * reports the real reason either way, not just "ready" once the copy
 * alone succeeded.
 */
void containerstoragemigrate_mark_failed(const char *container_name, const char *message);

/*
 * {"state":..., "disk":..., "error":...} -- identical shape to
 * storagemigrate_write_status_json(), for one container_name.
 * {"state":"none"} if no job has ever run for this container.
 */
void containerstoragemigrate_write_status_json(struct json_writer *w, const char *container_name);

#endif /* CONTAINERSTORAGEMIGRATE_H */
