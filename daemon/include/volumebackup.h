#ifndef VOLUMEBACKUP_H
#define VOLUMEBACKUP_H

#include "json.h"

#include <time.h>

/*
 * Scheduled snapshots of a volume's CONTENTS (issue #96).
 *
 * The platform backup bundle (ADR-0033) deliberately carries
 * configuration only -- container definitions, networks, DNS, the
 * volume registry -- and never workload data, on the same reasoning
 * that keeps image content out of it. That boundary still stands. But
 * it left volumes with no recovery story at all: a volume exists
 * precisely because its contents should outlive the container that
 * wrote them, and "whatever you arranged yourself outside thinC" was
 * the only answer available, with no way to arrange anything inside it
 * either, since a volume is a host directory no container can reach.
 *
 * This is the answer, deliberately narrow:
 *
 * - **Opt-in per volume.** Nothing is copied unless someone asked. A
 *   volume holding a scratch build tree should not be snapshotted
 *   hourly just because it exists.
 * - **Owned by the volume, not the container.** A volume is the thing
 *   with data; a container merely mounts one, and two containers can
 *   mount the same volume. Hanging the policy on the container would
 *   mean two policies over one set of bytes, and would still exclude a
 *   container's own overlay, which is ephemeral by design. A container
 *   page shows the backups of the volumes it mounts, derived -- the
 *   same way it already shows which volumes those are.
 * - **One global schedule**, reusing the existing periodic-timer shape,
 *   with per-volume retention. N independent timers is N ways for a
 *   schedule to be quietly wrong.
 * - **Onto a disk carrying the `backup` role**, the same role the
 *   configuration snapshots already use -- one answer to "where do
 *   backups go", not two.
 */

/* A volume that never says otherwise keeps this many snapshots. Seven
 * is a week of dailies at the default interval, which is the shape most
 * people mean by "keep some history" without being asked. */
#define VOLUMEBACKUP_DEFAULT_RETAIN 7
#define VOLUMEBACKUP_MAX_RETAIN 365
#define VOLUMEBACKUP_MAX_SNAPSHOTS 512
/* "YYYYmmddTHHMMSSZ" -- deliberately lexicographically sortable, which
 * is what makes retention a plain sort-and-trim rather than a date
 * parse. */
#define VOLUMEBACKUP_STAMP_MAX 24

enum volumebackup_error {
	VOLUMEBACKUP_OK = 0,
	VOLUMEBACKUP_ERR_NOT_FOUND,
	VOLUMEBACKUP_ERR_DISABLED,
	VOLUMEBACKUP_ERR_NO_DISK,
	VOLUMEBACKUP_ERR_DISK_NOT_READY,
	VOLUMEBACKUP_ERR_IN_USE_RUNNING,
	VOLUMEBACKUP_ERR_NO_SUCH_SNAPSHOT,
	VOLUMEBACKUP_ERR_COPY_FAILED,
	VOLUMEBACKUP_ERR_PERSIST_FAILED,
	VOLUMEBACKUP_ERR_INVALID
};

struct volumebackup_snapshot {
	char stamp[VOLUMEBACKUP_STAMP_MAX];
	unsigned long long bytes;
};

/* Loads the global schedule from state_path. */
int volumebackup_init(const char *state_path);
void volumebackup_repoint(const char *new_state_path);

/* The global schedule: whether the sweep runs at all, how often, and
 * which backup-role disk it writes to. Per-volume opt-in and retention
 * live on the volume itself (volume.h). */
int volumebackup_enabled(void);
int volumebackup_interval_hours(void);
const char *volumebackup_disk(void);
enum volumebackup_error volumebackup_set(const char *disk_name, int enabled, int interval_hours);
void volumebackup_write_config_json(struct json_writer *w);

/*
 * Takes one snapshot of volume_name now, then trims to its retention.
 * Refuses while a container mounting it is running -- copying a tree
 * out from under a live writer produces a snapshot of a half-written
 * state, which is worse than no snapshot because it looks like one.
 * is_running is injected rather than looked up so this module needs no
 * knowledge of the registry.
 */
/*
 * Callbacks the caller supplies so this module needs no knowledge of
 * the registry or of container definitions.
 *
 * set_paused freezes (or thaws) EVERY running container mounting the
 * volume and returns how many it acted on, or -1 on failure. All of
 * them, not just one: any container with the volume mounted could be
 * writing, so freezing a subset would leave the copy exposed to the
 * rest.
 */
struct volumebackup_hooks {
	int (*is_running)(const char *volume_name);
	int (*set_paused)(const char *volume_name, int freeze);
};

enum volumebackup_error volumebackup_take(const char *volume_name,
                                          const struct volumebackup_hooks *hooks);

/* Every snapshot of volume_name, newest first. Returns the count. */
int volumebackup_list(const char *volume_name, struct volumebackup_snapshot *out, int cap);
void volumebackup_write_list_json(struct json_writer *w, const char *volume_name);

/*
 * Replaces volume_name's contents with the named snapshot. Destructive
 * and guarded the same way a migrate is: refused while a container
 * mounting it is running, since a bind mount resolves to a host path
 * at container start and swapping the data underneath a live process
 * is a silent split-brain rather than an error.
 */
enum volumebackup_error volumebackup_restore(const char *volume_name, const char *stamp,
                                             const struct volumebackup_hooks *hooks);

enum volumebackup_error volumebackup_delete_snapshot(const char *volume_name, const char *stamp);

/*
 * The periodic sweep: snapshots every opted-in volume that is due.
 * Returns how many it took. now is injected so the caller owns the
 * clock, matching how the rest of this daemon's timers are testable.
 */
int volumebackup_sweep(time_t now, const struct volumebackup_hooks *hooks);

/* Outcome of the most recent attempt for one volume, for the API. */
void volumebackup_write_status_json(struct json_writer *w, const char *volume_name);

#endif /* VOLUMEBACKUP_H */
