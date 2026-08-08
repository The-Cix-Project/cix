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
 * interpret. "swap" is deliberately NOT one of the roles: this
 * project already has a real, working, dedicated on-demand host swap
 * FILE mechanism (daemon/src/swap.c, ADR-0069) with its own REST
 * surface -- a disk-level "swap" role would either duplicate that
 * mechanism or need to be reconciled with it, a real design question
 * with no answer yet, so it's left out rather than added as a role
 * nothing can act on (No Stop-Gaps).
 */

#define DISKROLE_DISK_NAME_MAX 32
#define DISKROLE_MAX 32

enum diskrole_kind {
	DISKROLE_CONTAINER_STORAGE,
	DISKROLE_BACKUP,
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
 * Assigns role_str ("container-storage" or "backup" -- anything else
 * is DISKROLE_ERR_INVALID_ROLE) to disk_name. disk_name is validated
 * as a plain simple name (same charset every other simple resource
 * name uses) but is NOT required to currently resolve to a real disk
 * (GET /v1/diskroles reports whether it currently does, the same
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
