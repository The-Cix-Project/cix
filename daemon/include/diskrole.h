#ifndef DISKROLE_H
#define DISKROLE_H

#include "json.h"

/*
 * Persisted, operator-assigned disk roles -- Phase B of multi-disk
 * management (ROADMAP.md), the direct follow-up to Phase A's read-only
 * disk.c enumeration. Mirrors devicemap.c's own persistence shape
 * (ADR-0012 atomic-JSON, a name -> binding table loaded at startup,
 * rewritten atomically on every create/delete), but the direction is
 * inverted from devicemap's: devicemap binds an operator-chosen NAME to
 * a hardware SELECTOR; a disk already has a real, stable-enough kernel
 * name of its own (disk.h's struct discovered_disk.name), so this binds
 * that real disk name directly to a ROLE instead of inventing a second
 * alias layer with nothing to alias.
 *
 * The role vocabulary is a small, fixed, closed set -- not an arbitrary
 * operator-chosen string the way a devicemap name is -- because a role
 * is meaningful only insofar as a later phase (C: mount/format, D:
 * container-storage migration) actually understands and acts on it;
 * an open vocabulary here would just be state nothing could ever
 * interpret.
 *
 * "swap" (issue #28, added after being deliberately left out at first
 * -- see git history for the original reasoning) reconciles cleanly
 * with the existing on-demand host swap FILE mechanism
 * (daemon/src/swap.c, ADR-0069): the role names which disk is *allowed*
 * to back an operator-chosen swap placement, exactly the same
 * "eligible candidate, not automatically active" meaning
 * DISKROLE_BACKUP already has -- POST /system/swap's own optional
 * `disk` field is what actually resolves and activates it, reusing
 * storageplacement.h's STORAGE_KIND_SWAP as the pure "which disk is
 * currently active" pointer, the same module state/rebuildable/log
 * storage already share. No new mechanism, no duplication.
 */

#define DISKROLE_DISK_NAME_MAX 32
#define DISKROLE_MAX 32

enum diskrole_kind {
	DISKROLE_CONTAINER_STORAGE,
	DISKROLE_BACKUP,
	/* ADR-0141: each of these three is a daemon-wide singleton (unlike
	 * container-storage, which is inherently multi-instance) -- multiple
	 * disks can carry the same one of these roles (eligible candidates),
	 * but only one is ever the *active* placement at a time, tracked
	 * separately (see storageplacement.h). Assigning the role itself
	 * never moves anything; that's what POST .../migrate is for. */
	/*
	 * DISKROLE_STATE_STORAGE was here and is retired (#251). It moved
	 * state onto another disk, and the only reason worth doing that --
	 * surviving loss of the OS disk -- never worked: the migration
	 * copied current state onto the disk rather than adopting state
	 * already there, the record of which disk held state lived on the
	 * OS disk itself, and boot read only that record. State now lives
	 * on the config partition, which survives an upgrade and is wiped
	 * only by a full reinstall; durability is GET /system/backup.
	 */
	DISKROLE_REBUILDABLE_STORAGE,
	DISKROLE_LOG_STORAGE,
	/*
	 * issue #28: same "daemon-wide singleton, multiple disks can carry
	 * it as eligible candidates but only one is ever the active
	 * placement" shape as the four roles above -- see this file's own
	 * top comment for how it reconciles with ADR-0069's existing swap
	 * FILE mechanism.
	 */
	DISKROLE_SWAP,
};

enum diskrole_error {
	DISKROLE_OK = 0,
	DISKROLE_ERR_INVALID_DISK_NAME,
	DISKROLE_ERR_INVALID_ROLE,
	DISKROLE_ERR_IS_OS_DISK,
	DISKROLE_ERR_DUPLICATE,
	DISKROLE_ERR_FULL,
	DISKROLE_ERR_NOT_FOUND,
	DISKROLE_ERR_PERSIST_FAILED,
};

/* Loads state_path (the persisted role list, if any) at startup. */
int diskrole_init(const char *state_path);

/*
 * Where a recorded disk actually is now (#255).
 *
 * A kernel device name is a location, not an identity -- it is assigned
 * in probe order and moves when the driver set, the controller set or
 * the disk set changes. Callers persist a (name, filesystem UUID) pair
 * and ask this for the name to use today.
 *
 * Returns 0 and echoes recorded_name when there is no UUID to go on or
 * the UUID still resolves to the same name; 1 when the disk was found
 * under a different name, which is written to out_name; -1 when a UUID
 * was recorded and no disk on this machine carries it, which means
 * genuinely absent rather than renamed.
 */
int diskrole_resolve_recorded(const char *recorded_name, const char *recorded_uuid,
                               char *out_name, size_t out_size);

/*
 * Assigns role_str ("container-storage", "backup", "state-storage",
 * "rebuildable-storage", or "log-storage" -- anything else is
 * DISKROLE_ERR_INVALID_ROLE) to disk_name. disk_name is validated
 * as a plain simple name (same charset every other simple resource
 * name uses) but is NOT required to currently resolve to a real disk
 * (GET /v1/storage-roles reports whether it currently does, the same
 * tolerant "present" convention devicemap.c's own mappings already
 * use for hardware that might be temporarily absent) -- EXCEPT that a
 * disk_name which DOES currently resolve to the real OS disk
 * (disk.c's own is_os_disk, re-checked here via disk_enumerate() with
 * os_containers_dir) is always rejected: the fixed OS-disk layout is
 * never a role-assignment candidate (ROADMAP.md's own stated
 * constraint on this whole feature). DISKROLE_ERR_DUPLICATE if
 * disk_name already has a role assigned (DELETE it first to
 * reassign, the same no-silent-overwrite convention devicemap_create()
 * already established).
 */
enum diskrole_error diskrole_create(const char *disk_name, const char *role_str,
                                     const char *os_containers_dir);

enum diskrole_error diskrole_delete(const char *disk_name);

/* NULL if disk_name has no assigned role. */
const char *diskrole_lookup(const char *disk_name);

/*
 * ADR-0142: records which real filesystem a disk was last successfully
 * formatted with (diskformat.c calls this once a format job reaches
 * DISKFORMAT_STATE_READY) -- diskformat.c's own job state is purely
 * in-memory and forgotten across a restart, but this persists
 * alongside the role itself in the same diskroles.json, so a later
 * boot's own auto-remount pass knows which real fstype to mount with
 * instead of guessing. No-op (returns DISKROLE_ERR_NOT_FOUND) if
 * disk_name has no role assigned -- fs_type is only ever meaningful
 * attached to a real role.
 */
enum diskrole_error diskrole_set_fs_type(const char *disk_name, const char *fs_type);

/* NULL if disk_name has no role, or has a role but was never
 * successfully formatted (diskrole_set_fs_type() never called for it). */
const char *diskrole_lookup_fs_type(const char *disk_name);

/*
 * Writes one entry (disk_name, role, and -- resolved fresh via
 * disk_enumerate() -- present: bool) into w. Returns 1 if disk_name
 * has a real assigned role, 0 (writes nothing) otherwise.
 */
int diskrole_write_json_one(const char *disk_name, struct json_writer *w,
                             const char *os_containers_dir);

/* Same per-entry shape as diskrole_write_json_one(), for every
 * assigned role, wrapped in a JSON array. */
void diskrole_write_json_list(struct json_writer *w, const char *os_containers_dir);

#endif /* DISKROLE_H */
