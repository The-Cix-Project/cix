#ifndef BACKUPCONFIG_H
#define BACKUPCONFIG_H

#include "json.h"

/*
 * ADR-0141 Phase 5: turns the `backup` disk role (present since Phase B
 * of multi-disk management but, until now, a pure inert label nothing
 * ever acted on) into a real, working mechanism -- writes exactly the
 * same JSON bundle GET /system/backup already produces, confirmed
 * directly against do_system_backup()'s own real content rather than
 * assumed from ADR-0141's own summary text: container definitions,
 * networks, DNS records, installed-package state, every on-disk
 * package recipe version, and site config. Deliberately, and pre-
 * existingly, NEVER PKI keys/certs (do_system_backup() itself has
 * never included them -- cixctl's own `backup` command help already
 * says so) and NEVER container workload data or rebuildable-storage
 * content (no point backing up something regenerable from recipes/
 * sources by definition). An operator who also wants PKI material
 * preserved off-box has `state-storage`'s own migration (which does
 * carry the real PKI directory, ADR-0141 Phase 2) as the mechanism for
 * that -- a genuinely different concern (a live working copy vs. a
 * portable snapshot) than what this module exists for.
 *
 * This module owns only the persisted config (which disk, whether
 * enabled, the auto-snapshot interval) and the in-memory status of the
 * most recent attempt -- it has no disk/mount/timer knowledge of its
 * own. main.c validates the configured disk (must carry the `backup`
 * role, must be currently mounted -- the same check storagemigrate.c
 * already does for the other three kinds) and does the actual
 * synchronous write of do_system_backup()'s own bundle to
 * "<mount_path>/backup.json" -- a single, always-current snapshot, not
 * a timestamped history: this mechanism exists to guarantee a real,
 * fresh disaster-recovery copy always exists somewhere off the OS
 * disk, not to be a backup-retention system in its own right (an
 * operator wanting history already has this file to copy elsewhere on
 * whatever cadence they choose).
 */

enum backupconfig_error {
	BACKUPCONFIG_OK = 0,
	BACKUPCONFIG_ERR_INVALID_INTERVAL,
	BACKUPCONFIG_ERR_PERSIST_FAILED,
};

enum backup_snapshot_state {
	BACKUP_SNAPSHOT_NEVER, /* no snapshot attempt has ever run this daemon lifetime */
	BACKUP_SNAPSHOT_OK,
	BACKUP_SNAPSHOT_FAILED,
};

/*
 * Loads state_path (the persisted config, if any) at startup. Absent
 * file (first-ever boot, or a --data-dir= test invocation) is not an
 * error -- defaults to disabled, no disk, 0 interval (no automatic
 * schedule). Returns 0, or -1 if the file exists but is malformed.
 */
int backupconfig_init(const char *state_path);

/* ADR-0141 Phase 5: repoints without reloading -- see network_repoint()'s
 * own doc comment (daemon/src/network.c) for the shared reasoning. */
void backupconfig_repoint(const char *new_state_path);

/* NULL if no disk configured (the default -- no automatic snapshots
 * possible until one is set, regardless of `enabled`). */
const char *backupconfig_disk(void);
int backupconfig_enabled(void);
/*
 * ADR-0257: the interval that used to live here is gone. WHEN a backup
 * runs is a schedule (`GET /v1/schedules`), and this config answers
 * only WHERE it goes and whether it is on at all. Keeping both would be
 * two places to look when backups do not run, which is the exact thing
 * that ADR replaced.
 *
 * Read once at init from an older config file, for the one-time
 * migration that turns it into a schedule. 0 when there was none.
 */
int backupconfig_legacy_interval_hours(void);
void backupconfig_clear_legacy_interval(void);

/*
 * Persists all three fields together. disk_name NULL clears the
 * configured disk. interval_hours must be 0 (no schedule) or a
 * positive value -- BACKUPCONFIG_ERR_INVALID_INTERVAL otherwise. Pure
 * bookkeeping: does not itself validate the disk's role/mount state or
 * touch any timer.
 *
 * PUT /v1/system/backup-config mirrors daemon-config's own "only the
 * fields given are changed" convention (per ADR-0141's own design) --
 * that partial-update merge is main.c's own handler's job (read the
 * three current values back via the getters below, override whichever
 * fields the request body actually supplied, then call this with the
 * merged result); this function itself always takes and persists a
 * complete triple, the same shape save_state() itself needs.
 */
enum backupconfig_error backupconfig_set(const char *disk_name, int enabled);

/* Writes {"disk": "sdc"|null, "enabled": bool}. */
void backupconfig_write_json(struct json_writer *w);

/*
 * Records the outcome of a snapshot attempt (manual or automatic) --
 * in-memory only, never persisted, the same "ephemeral job status"
 * convention diskformat.c/storagemigrate.c's own state already
 * follows. error_msg is ignored on success.
 */
void backupconfig_record_attempt(int success, const char *error_msg);

/* Writes {"state": "never"|"ok"|"failed", "last_attempt_unixtime": N,
 * "error": "..."} -- last_attempt_unixtime/error omitted when state is
 * "never"; error omitted unless state is "failed". */
void backupconfig_write_status_json(struct json_writer *w);

#endif /* BACKUPCONFIG_H */
