#ifndef VOLUME_H
#define VOLUME_H

#include "json.h"

#include <time.h>

/*
 * Persistent per-container storage (issue #88).
 *
 * Everything a container writes at runtime lives in its overlay upper
 * layer, and DELETE removes that layer outright (ADR-0106). Until this
 * existed there was no way to keep ANY container-generated data across
 * a recreate -- only the recipe's own declared files[], which are
 * re-staged identically for every instance and so cannot hold anything
 * a user or workload actually produced.
 *
 * That is a sharp edge for anything an operator lives in. The case that
 * raised it: the jump box's home directories. It has follow_rolling
 * set, so an ordinary image rebuild recreates it automatically; during
 * a single working session it was recreated about a dozen times, and
 * every shell history, dotfile and scratch note went with it, silently.
 *
 * A volume is a named directory whose lifetime is INDEPENDENT of any
 * container using it. Deleting a container never deletes its volumes --
 * that is the entire point -- so a volume has its own explicit delete,
 * which refuses while any container definition still references it
 * (rather than leaving data orphaned or silently removing data still in
 * use).
 *
 * Deliberately NOT a second storage story: a volume is placed by the
 * same disk-role mechanism container overlays already use, so
 * "container-storage" disks serve both, and there is one answer to
 * "where does container data live" rather than two.
 */

#define VOLUME_MAX 64
#define VOLUME_NAME_MAX 64
/* Path inside a container that a volume is mounted at. */
#define VOLUME_MOUNT_PATH_MAX 256
#define VOLUME_DISK_NAME_MAX 64

enum volume_error {
	VOLUME_OK = 0,
	VOLUME_ERR_INVALID_NAME,
	VOLUME_ERR_DUPLICATE,
	VOLUME_ERR_FULL,
	VOLUME_ERR_NOT_FOUND,
	VOLUME_ERR_IN_USE,
	VOLUME_ERR_PERSIST_FAILED,
	VOLUME_ERR_IO,
	/*
	 * Migration's own refusals. Each is a different thing to tell an
	 * operator, so each is its own value rather than one generic
	 * "cannot migrate".
	 */
	VOLUME_ERR_TARGET_NOT_FOUND,
	VOLUME_ERR_TARGET_NOT_READY,
	VOLUME_ERR_SAME_PLACE,
	VOLUME_ERR_IN_USE_RUNNING,
	VOLUME_ERR_COPY_FAILED
};

/*
 * How a backup behaves when something is using the volume. Not a
 * boolean, because "pause it and copy safely" is a genuinely different
 * answer from "copy it live and accept the mess" -- collapsing them
 * would have forced the worse one.
 */
enum volume_running_mode {
	VOLUME_RUNNING_REFUSE = 0, /* the safe default, and useless for an always-on workload */
	VOLUME_RUNNING_PAUSE,      /* freeze every mounting container for the copy, then thaw */
	VOLUME_RUNNING_ALLOW       /* copy live; crash-consistent */
};

struct volume {
	char name[VOLUME_NAME_MAX];
	/*
	 * Which disk this volume's data sits on, by the same disk-role
	 * naming container overlays use. Empty means the default OS-disk
	 * placement -- the identical convention a container's own "disk"
	 * field already has, so there is one rule to learn, not two.
	 */
	char disk[VOLUME_DISK_NAME_MAX];
	time_t created_at;
	/*
	 * Issue #96: per-volume backup policy. Opt-in, because a volume
	 * holding a scratch build tree should not be snapshotted just for
	 * existing, and because copying workload data is something to be
	 * asked for rather than assumed.
	 *
	 * Retention lives here rather than in the global schedule: how much
	 * history is worth keeping is a property of what the volume holds,
	 * and a database and a home directory rarely want the same answer.
	 * The interval is global, though -- N independent timers is N ways
	 * for a schedule to be quietly wrong.
	 */
	int backup_enabled;
	int backup_retain;
	/*
	 * What to do when a container mounting this volume is running at
	 * snapshot time. This single field decides whether the volume is
	 * ever actually backed up: a service container is normally
	 * `restart: always` and never stops, so REFUSE means "never", and
	 * that was the whole feature's blind spot.
	 *
	 * PAUSE is the good answer and the reason this is not a boolean.
	 * Freezing every mounting container for the copy makes the snapshot
	 * genuinely consistent rather than crash-consistent, because the
	 * cgroup freezer stops tasks at the kernel level -- unlike SIGSTOP,
	 * a process can neither ignore nor handle it. The cost is real
	 * downtime for the length of the copy, which is a trade worth
	 * stating rather than hiding.
	 */
	enum volume_running_mode backup_while_running;
	/* When the scheduled sweep last took one, so it can tell what is
	 * due without re-reading the snapshot store for every volume. */
	time_t backup_last_at;
	int in_use;
};

/*
 * Loads the persisted volume list. Volumes are real directories on
 * disk; this registry is the record of which ones exist deliberately,
 * so a stray directory is never mistaken for a volume.
 */
int volume_init(const char *state_path);

/* Sets the default directory volumes live under (BASE_DIR/volumes). */
void volume_set_dir(const char *dir);
void volume_repoint(const char *new_state_path);

/*
 * Same charset rule as a container name -- a volume name becomes a real
 * directory name, so it must never be able to escape its parent or
 * collide with path syntax.
 */
int volume_name_is_valid(const char *name);

/*
 * Creates the volume's own real directory as well as its record.
 * VOLUME_ERR_DUPLICATE if one already exists by that name.
 */
enum volume_error volume_create(const char *name, const char *disk, struct volume **out);

/*
 * Removes the record AND the data. Callers must establish that nothing
 * references it first (main.c checks every container definition) --
 * this is genuinely destructive and has no undo.
 */
enum volume_error volume_delete(const char *name);

struct volume *volume_find(const char *name);
int volume_list(struct volume out[], int max);

/*
 * Resolves a volume's real host directory, honouring its disk
 * placement. Returns 0 on success.
 */
int volume_host_path(const struct volume *v, char *out, size_t out_size);

void volume_write_json_one(const struct volume *v, struct json_writer *w);
void volume_write_json_list(struct json_writer *w);

/*
 * Moves a volume's data to disk_name ("" for the default OS-disk
 * placement), then repoints it.
 *
 * Copy first, repoint second, and only remove the original once the
 * repoint has been persisted -- so a failure at any point leaves the
 * volume still pointing at data that exists. The reverse order would
 * have a window where the volume points somewhere the data is not.
 *
 * Refused while any container that mounts this volume is running: a
 * bind mount resolves to a host path at container start, so moving the
 * data underneath a running container would leave it writing to the old
 * location with no indication anything had changed.
 */
enum volume_error volume_migrate(const char *name, const char *disk_name);

/*
 * Issue #96: set a volume's own backup policy. A retain of 0 means
 * "leave whatever is already set" -- the caller validates any explicit
 * value, so 0 only ever reaches here when the field was omitted, and a
 * volume that has never had one set falls back to the default when it
 * is read.
 */
enum volume_error volume_set_backup_policy(const char *name, int enabled, int retain,
                                          enum volume_running_mode while_running);

/* Parses the API's own spelling; anything unrecognised is REFUSE, the
 * safe reading of a value this daemon does not understand. */
enum volume_running_mode volume_running_mode_parse(const char *s);
const char *volume_running_mode_name(enum volume_running_mode m);

/* Records that a scheduled snapshot was taken, so the sweep knows what
 * is due. Persisted, so a restart does not re-snapshot everything. */
void volume_note_backup_taken(const char *name, time_t when);

#endif /* VOLUME_H */
