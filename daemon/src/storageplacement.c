#include "storageplacement.h"
#include "diskrole.h"
#include "disk.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static char g_state_path[512];
/* Empty string: no placement set for this kind (default OS-disk). */
static char g_disk[STORAGE_KIND_COUNT][DISKROLE_DISK_NAME_MAX];
/*
 * The filesystem UUID of the placement disk, which is what actually
 * identifies it (#255). g_disk above is only where the kernel put it
 * last time, and a kernel device name moves when the driver set,
 * controller set or disk set changes -- that really happened here, and
 * a placement still naming the old letter stopped the daemon booting.
 * Empty for a record written before this existed, or a filesystem with
 * no UUID; both fall back to the name.
 */
static char g_uuid[STORAGE_KIND_COUNT][DISK_FS_UUID_MAX];
/*
 * ADR-0364 decision 3: a move recorded now and made at the next boot.
 * Three states, so a move back to the default is not confused with no
 * move at all: g_pending_set 0 is none; 1 with an empty g_pending_disk
 * is "back to the default location"; 1 with a name is that disk, which
 * is identified by g_pending_uuid as the active placement is. Persisted
 * as "<kind>_pending": absent, null or a disk name.
 */
static int g_pending_set[STORAGE_KIND_COUNT];
static char g_pending_disk[STORAGE_KIND_COUNT][DISKROLE_DISK_NAME_MAX];
static char g_pending_uuid[STORAGE_KIND_COUNT][DISK_FS_UUID_MAX];
/* The outcome of the last move the boot made ("moved" or "failed"),
 * its reason and when; empty outcome when no move has been made. */
static char g_move_outcome[STORAGE_KIND_COUNT][16];
static char g_move_reason[STORAGE_KIND_COUNT][512];
static long long g_move_at[STORAGE_KIND_COUNT];

static const char *kind_key(enum storage_kind kind);
static enum storageplacement_error save_state(void);

/* The persisted key holding a kind's UUID, alongside its own name key. */
static void uuid_key(enum storage_kind kind, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s_uuid", kind_key(kind));
}

/* "<kind>_<suffix>", for the pending and last-move keys. */
static void suffixed_key(enum storage_kind kind, const char *suffix, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s_%s", kind_key(kind), suffix);
}

static const char *kind_key(enum storage_kind kind)
{
	switch (kind) {
	case STORAGE_KIND_REBUILDABLE:
		return "rebuildable";
	case STORAGE_KIND_LOG:
		return "log";
	case STORAGE_KIND_SWAP:
		return "swap";
	case STORAGE_KIND_DATA:
		return "data";
	default:
		/* Unreachable: every caller loops below STORAGE_KIND_COUNT. A
		 * key no kind reads, rather than another kind's, so a mistake
		 * could never overwrite a real placement. */
		return "unknown";
	}
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	enum storage_kind kind;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed persisted storage placement\n", g_state_path);
		return -1;
	}

	/*
	 * An upgraded box's file may still carry a "state" key from the
	 * retired state-storage placement (#251). Nothing reads it any
	 * more, so it is ignored rather than rejected -- the loop simply
	 * starts at the first live kind -- and the next save drops it.
	 */

	for (kind = STORAGE_KIND_REBUILDABLE; kind < STORAGE_KIND_COUNT; kind++) {
		const struct json_value *jdisk = json_object_get(root, kind_key(kind));
		const char *disk;
		char key[64];
		const struct json_value *jv;

		/* ADR-0364: a pending move and the last move's outcome. Read
		 * before the active placement, which `continue`s when unset. */
		suffixed_key(kind, "pending", key, sizeof(key));
		jv = json_object_get(root, key);
		if (jv != NULL && (jv->type == JSON_NULL || jv->type == JSON_STRING)) {
			g_pending_set[kind] = 1;
			if (jv->type == JSON_STRING)
				snprintf(g_pending_disk[kind], sizeof(g_pending_disk[kind]), "%s",
				         json_as_string(jv));
			suffixed_key(kind, "pending_uuid", key, sizeof(key));
			jv = json_object_get(root, key);
			if (jv != NULL && jv->type == JSON_STRING)
				snprintf(g_pending_uuid[kind], sizeof(g_pending_uuid[kind]), "%s",
				         json_as_string(jv));
		}
		suffixed_key(kind, "move_outcome", key, sizeof(key));
		jv = json_object_get(root, key);
		if (jv != NULL && jv->type == JSON_STRING) {
			snprintf(g_move_outcome[kind], sizeof(g_move_outcome[kind]), "%s",
			         json_as_string(jv));
			suffixed_key(kind, "move_reason", key, sizeof(key));
			jv = json_object_get(root, key);
			if (jv != NULL && jv->type == JSON_STRING)
				snprintf(g_move_reason[kind], sizeof(g_move_reason[kind]), "%s",
				         json_as_string(jv));
			suffixed_key(kind, "move_at", key, sizeof(key));
			g_move_at[kind] = (long long)json_as_number(json_object_get(root, key));
		}

		if (jdisk == NULL || jdisk->type != JSON_STRING)
			continue;
		disk = json_as_string(jdisk);
		if (disk == NULL)
			continue;
		if (snprintf(g_disk[kind], sizeof(g_disk[kind]), "%s", disk) >=
		    (int)sizeof(g_disk[kind])) {
			json_free(root);
			fprintf(stderr, "%s: persisted disk name too long for %s\n", g_state_path,
			        kind_key(kind));
			return -1;
		}
		{
			char ukey[64];
			const struct json_value *juuid;
			const char *uuid;

			uuid_key(kind, ukey, sizeof(ukey));
			juuid = json_object_get(root, ukey);
			uuid = (juuid != NULL && juuid->type == JSON_STRING) ? json_as_string(juuid) : NULL;
			if (uuid != NULL)
				snprintf(g_uuid[kind], sizeof(g_uuid[kind]), "%s", uuid);
		}
	}

	json_free(root);

	/*
	 * #255: follow the filesystem rather than the name it had last
	 * boot, and adopt a UUID for any record written before this
	 * existed.
	 *
	 * Both halves matter. Resolving is what survives a rename; adopting
	 * is what makes an already-configured host survive the FIRST one,
	 * which is the case that took a box down -- the placement said
	 * "sda", a kernel change made it sdb, and the daemon refused to
	 * start.
	 */
	{
		enum storage_kind k;
		int adopted = 0;

		for (k = STORAGE_KIND_REBUILDABLE; k < STORAGE_KIND_COUNT; k++) {
			char resolved[DISKROLE_DISK_NAME_MAX];
			char dev_path[PATH_MAX];

			/* A pending target follows its filesystem the same way. */
			if (g_pending_disk[k][0] != '\0' &&
			    diskrole_resolve_recorded(g_pending_disk[k], g_pending_uuid[k], resolved,
			                              sizeof(resolved)) == 1)
				snprintf(g_pending_disk[k], sizeof(g_pending_disk[k]), "%s", resolved);
			if (g_disk[k][0] == '\0')
				continue;
			if (diskrole_resolve_recorded(g_disk[k], g_uuid[k], resolved,
			                               sizeof(resolved)) == 1) {
				fprintf(stderr,
				        "%s: %s placement %s now appears as %s (same filesystem %s) -- "
				        "following it (#255)\n",
				        g_state_path, kind_key(k), g_disk[k], resolved, g_uuid[k]);
				snprintf(g_disk[k], sizeof(g_disk[k]), "%s", resolved);
			}
			if (g_uuid[k][0] != '\0')
				continue;
			snprintf(dev_path, sizeof(dev_path), "/dev/%s", g_disk[k]);
			disk_probe_fs_uuid(dev_path, g_uuid[k], sizeof(g_uuid[k]));
			if (g_uuid[k][0] != '\0')
				adopted++;
		}
		if (adopted > 0) {
			fprintf(stderr, "%s: adopted filesystem UUIDs for %d storage placement(s) (#255)\n",
			        g_state_path, adopted);
			if (save_state() != STORAGEPLACEMENT_OK)
				fprintf(stderr, "%s: could not persist adopted UUIDs -- they will be read "
				                "again next boot\n", g_state_path);
		}
	}
	return 0;
}

static enum storageplacement_error save_state(void)
{
	struct json_writer w;
	enum storage_kind kind;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	for (kind = STORAGE_KIND_REBUILDABLE; kind < STORAGE_KIND_COUNT; kind++) {
		jw_key(&w, kind_key(kind));
		if (g_disk[kind][0] != '\0')
			jw_str(&w, g_disk[kind]);
		else
			jw_null(&w);
		if (g_uuid[kind][0] != '\0') {
			char ukey[64];

			uuid_key(kind, ukey, sizeof(ukey));
			jw_key(&w, ukey);
			jw_str(&w, g_uuid[kind]);
		}
		if (g_pending_set[kind]) {
			char key[64];

			suffixed_key(kind, "pending", key, sizeof(key));
			jw_key(&w, key);
			if (g_pending_disk[kind][0] != '\0')
				jw_str(&w, g_pending_disk[kind]);
			else
				jw_null(&w);
			if (g_pending_uuid[kind][0] != '\0') {
				suffixed_key(kind, "pending_uuid", key, sizeof(key));
				jw_key(&w, key);
				jw_str(&w, g_pending_uuid[kind]);
			}
		}
		if (g_move_outcome[kind][0] != '\0') {
			char key[64];

			suffixed_key(kind, "move_outcome", key, sizeof(key));
			jw_key(&w, key);
			jw_str(&w, g_move_outcome[kind]);
			suffixed_key(kind, "move_reason", key, sizeof(key));
			jw_key(&w, key);
			jw_str(&w, g_move_reason[kind]);
			suffixed_key(kind, "move_at", key, sizeof(key));
			jw_key(&w, key);
			jw_int(&w, g_move_at[kind]);
		}
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc == 0 ? STORAGEPLACEMENT_OK : STORAGEPLACEMENT_ERR_PERSIST_FAILED;
}

int storageplacement_init(const char *state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", state_path);
	memset(g_disk, 0, sizeof(g_disk));
	memset(g_uuid, 0, sizeof(g_uuid));
	memset(g_pending_set, 0, sizeof(g_pending_set));
	memset(g_pending_disk, 0, sizeof(g_pending_disk));
	memset(g_pending_uuid, 0, sizeof(g_pending_uuid));
	memset(g_move_outcome, 0, sizeof(g_move_outcome));
	memset(g_move_reason, 0, sizeof(g_move_reason));
	memset(g_move_at, 0, sizeof(g_move_at));
	return load_state();
}

const char *storageplacement_get(enum storage_kind kind)
{
	if (kind < STORAGE_KIND_REBUILDABLE || kind >= STORAGE_KIND_COUNT)
		return NULL;
	return g_disk[kind][0] != '\0' ? g_disk[kind] : NULL;
}

const char *storageplacement_get_uuid(enum storage_kind kind)
{
	if (kind < STORAGE_KIND_REBUILDABLE || kind >= STORAGE_KIND_COUNT)
		return NULL;
	return g_disk[kind][0] != '\0' && g_uuid[kind][0] != '\0' ? g_uuid[kind] : NULL;
}

enum storageplacement_error storageplacement_set_pending(enum storage_kind kind,
                                                         const char *disk_name)
{
	int prev_set;
	char prev_disk[DISKROLE_DISK_NAME_MAX];
	char prev_uuid[DISK_FS_UUID_MAX];
	enum storageplacement_error err;

	if (kind < STORAGE_KIND_REBUILDABLE || kind >= STORAGE_KIND_COUNT)
		return STORAGEPLACEMENT_ERR_PERSIST_FAILED;
	prev_set = g_pending_set[kind];
	snprintf(prev_disk, sizeof(prev_disk), "%s", g_pending_disk[kind]);
	snprintf(prev_uuid, sizeof(prev_uuid), "%s", g_pending_uuid[kind]);

	g_pending_set[kind] = 1;
	g_pending_disk[kind][0] = '\0';
	g_pending_uuid[kind][0] = '\0';
	if (disk_name != NULL) {
		char dev_path[PATH_MAX];

		snprintf(g_pending_disk[kind], sizeof(g_pending_disk[kind]), "%s", disk_name);
		snprintf(dev_path, sizeof(dev_path), "/dev/%s", disk_name);
		disk_probe_fs_uuid(dev_path, g_pending_uuid[kind], sizeof(g_pending_uuid[kind]));
	}
	err = save_state();
	if (err != STORAGEPLACEMENT_OK) {
		g_pending_set[kind] = prev_set;
		snprintf(g_pending_disk[kind], sizeof(g_pending_disk[kind]), "%s", prev_disk);
		snprintf(g_pending_uuid[kind], sizeof(g_pending_uuid[kind]), "%s", prev_uuid);
	}
	return err;
}

enum storageplacement_error storageplacement_clear_pending(enum storage_kind kind)
{
	int prev_set;
	enum storageplacement_error err;

	if (kind < STORAGE_KIND_REBUILDABLE || kind >= STORAGE_KIND_COUNT)
		return STORAGEPLACEMENT_ERR_PERSIST_FAILED;
	if (!g_pending_set[kind])
		return STORAGEPLACEMENT_OK;
	prev_set = g_pending_set[kind];
	g_pending_set[kind] = 0;
	err = save_state();
	if (err != STORAGEPLACEMENT_OK)
		g_pending_set[kind] = prev_set;
	else {
		g_pending_disk[kind][0] = '\0';
		g_pending_uuid[kind][0] = '\0';
	}
	return err;
}

int storageplacement_get_pending(enum storage_kind kind, const char **disk_name)
{
	if (kind < STORAGE_KIND_REBUILDABLE || kind >= STORAGE_KIND_COUNT || !g_pending_set[kind])
		return 0;
	if (disk_name != NULL)
		*disk_name = g_pending_disk[kind][0] != '\0' ? g_pending_disk[kind] : NULL;
	return 1;
}

const char *storageplacement_get_pending_uuid(enum storage_kind kind)
{
	if (kind < STORAGE_KIND_REBUILDABLE || kind >= STORAGE_KIND_COUNT || !g_pending_set[kind])
		return NULL;
	return g_pending_uuid[kind][0] != '\0' ? g_pending_uuid[kind] : NULL;
}

enum storageplacement_error storageplacement_finish_move(enum storage_kind kind, int moved,
                                                         const char *reason, long long at)
{
	if (kind < STORAGE_KIND_REBUILDABLE || kind >= STORAGE_KIND_COUNT || !g_pending_set[kind])
		return STORAGEPLACEMENT_ERR_PERSIST_FAILED;
	/*
	 * One record, one write: the active placement, the cleared pending
	 * move and the outcome change together, so no boot can read a
	 * record in which the move happened and is still pending, or
	 * happened with no outcome. Nothing is rolled back on a failed
	 * write -- the caller has already mounted the outcome, and the
	 * memory is what this boot runs on; the caller reports it.
	 */
	if (moved) {
		snprintf(g_disk[kind], sizeof(g_disk[kind]), "%s", g_pending_disk[kind]);
		snprintf(g_uuid[kind], sizeof(g_uuid[kind]), "%s", g_pending_uuid[kind]);
	}
	g_pending_set[kind] = 0;
	g_pending_disk[kind][0] = '\0';
	g_pending_uuid[kind][0] = '\0';
	snprintf(g_move_outcome[kind], sizeof(g_move_outcome[kind]), "%s", moved ? "moved" : "failed");
	snprintf(g_move_reason[kind], sizeof(g_move_reason[kind]), "%s", reason != NULL ? reason : "");
	g_move_at[kind] = at;
	return save_state();
}

enum storageplacement_error storageplacement_set(enum storage_kind kind, const char *disk_name)
{
	char prev[DISKROLE_DISK_NAME_MAX];
	char prev_uuid[DISK_FS_UUID_MAX];
	enum storageplacement_error err;

	if (kind < STORAGE_KIND_REBUILDABLE || kind >= STORAGE_KIND_COUNT)
		return STORAGEPLACEMENT_ERR_PERSIST_FAILED;

	snprintf(prev, sizeof(prev), "%s", g_disk[kind]);
	snprintf(prev_uuid, sizeof(prev_uuid), "%s", g_uuid[kind]);
	if (disk_name != NULL) {
		char dev_path[PATH_MAX];

		snprintf(g_disk[kind], sizeof(g_disk[kind]), "%s", disk_name);
		/* #255: record what this disk IS, not only where it is now. */
		snprintf(dev_path, sizeof(dev_path), "/dev/%s", disk_name);
		disk_probe_fs_uuid(dev_path, g_uuid[kind], sizeof(g_uuid[kind]));
	} else {
		g_disk[kind][0] = '\0';
		g_uuid[kind][0] = '\0';
	}

	err = save_state();
	if (err != STORAGEPLACEMENT_OK) {
		/* roll back on persist failure -- both halves, or the pointer
		 * and its identity would disagree in memory (#255). */
		snprintf(g_disk[kind], sizeof(g_disk[kind]), "%s", prev);
		snprintf(g_uuid[kind], sizeof(g_uuid[kind]), "%s", prev_uuid);
	}
	return err;
}

void storageplacement_write_json(struct json_writer *w, enum storage_kind kind)
{
	const char *disk = storageplacement_get(kind);

	jw_key(w, "disk");
	if (disk != NULL)
		jw_str(w, disk);
	else
		jw_null(w);
	/* ADR-0364: only a kind that moves at boot ever has these. */
	if (g_pending_set[kind]) {
		jw_key(w, "pending");
		jw_obj_open(w);
		jw_key(w, "disk");
		if (g_pending_disk[kind][0] != '\0')
			jw_str(w, g_pending_disk[kind]);
		else
			jw_null(w);
		jw_obj_close(w);
	}
	if (g_move_outcome[kind][0] != '\0') {
		jw_key(w, "last_move");
		jw_obj_open(w);
		jw_key(w, "outcome");
		jw_str(w, g_move_outcome[kind]);
		jw_key(w, "reason");
		jw_str(w, g_move_reason[kind]);
		jw_key(w, "at");
		jw_int(w, g_move_at[kind]);
		jw_obj_close(w);
	}
}
