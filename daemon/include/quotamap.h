#ifndef QUOTAMAP_H
#define QUOTAMAP_H

#include "registry.h"

#include <stdint.h>

/*
 * Real ext4 project-quota enforcement (Part 4, bare-metal-readiness
 * plan, ADR-0062). A project id is a plain uint32_t the kernel's
 * project-quota subsystem uses to group inodes for accounting/
 * limiting (quotactl(2)'s PRJQUOTA type, src/overlay.c's own
 * FS_IOC_FSSETXATTR call) -- entirely distinct from a Linux uid/gid.
 * This module owns the one persisted mapping from container name to
 * its project id.
 *
 * A generous fixed bound, matching this codebase's own established
 * "considered generous fixed bound with no natural ceiling to mirror"
 * convention (include/container.h's CONTAINER_MAX_DEVICES comment) --
 * unlike REGISTRY_MAX_CONTAINERS (a ceiling on containers *live at
 * once*), this table accumulates one entry per distinct name that has
 * *ever* requested a quota over this system's whole lifetime (see
 * quotamap_get_or_assign()'s own comment for why entries are never
 * removed), so it needs real headroom beyond that number.
 */
#define QUOTAMAP_MAX 1024

int quotamap_init(const char *state_path);

/* ADR-0141 Phase 2: repoints without reloading -- see network_repoint()'s
 * own doc comment for the shared reasoning. */
void quotamap_repoint(const char *new_state_path);

/*
 * Returns name's already-assigned project id in *out_projid if one
 * exists, or allocates a fresh one (the persisted counter's next
 * value) and persists it before returning if not. 0 is never a valid
 * assigned id (see quotamap.c's own QUOTAMAP_FIRST_ID) so *out_projid
 * is always nonzero on success. Returns 0 on success, -1 (the table is
 * full, or a real persistence I/O failure -- errno set by the failing
 * persist_atomic_write()) otherwise.
 *
 * Deliberately never reclaimed/reused/recycled, even after the
 * container itself is deleted: DELETE /v1/containers/{name} does
 * remove the container's own upperdir now (ADR-0106, task #738 --
 * this comment used to cite "ADR-0054's pre-existing backup/restore
 * design" as the reason it didn't, but that citation was stale and
 * incorrect, see ADR-0106's own Context section), but the id is kept
 * anyway: recreating a container under the same name later
 * intentionally gets the same id back (this function's own lookup-
 * before-allocate order already gives that for free) -- correct,
 * since that's the same logical container reoccupying the same
 * on-disk upperdir path, and a real, in-flight deletion (kill+unmount+
 * remove, ADR-0106) racing a fresh create of the same name is exactly
 * the kind of window a stable, never-recycled id sidesteps entirely
 * rather than having to reason about.
 */
int quotamap_get_or_assign(const char *name, uint32_t *out_projid);

/*
 * Applies a project quota to the filesystem backing base_path.
 *
 * The counterpart to quotamap_get_or_assign(): that decides which
 * project id a name owns, this makes the id mean something on disk.
 * Both callers -- volume quota changes and container creation -- want
 * the pair, and splitting them across two files meant neither owned
 * "quota". Returns 0, or -1 with errno set.
 */
int quotamap_apply(const char *base_path, uint32_t projid, long long quota_bytes);

#endif /* QUOTAMAP_H */
