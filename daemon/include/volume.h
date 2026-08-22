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
	VOLUME_ERR_IO
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

#endif /* VOLUME_H */
