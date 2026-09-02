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

static const char *kind_key(enum storage_kind kind);
static enum storageplacement_error save_state(void);

/* The persisted key holding a kind's UUID, alongside its own name key. */
static void uuid_key(enum storage_kind kind, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s_uuid", kind_key(kind));
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
	default:
		return "rebuildable";
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

	for (kind = STORAGE_KIND_REBUILDABLE; kind <= STORAGE_KIND_SWAP; kind++) {
		const struct json_value *jdisk = json_object_get(root, kind_key(kind));
		const char *disk;

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

		for (k = STORAGE_KIND_REBUILDABLE; k <= STORAGE_KIND_SWAP; k++) {
			char resolved[DISKROLE_DISK_NAME_MAX];
			char dev_path[PATH_MAX];

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
	for (kind = STORAGE_KIND_REBUILDABLE; kind <= STORAGE_KIND_SWAP; kind++) {
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
	return load_state();
}

const char *storageplacement_get(enum storage_kind kind)
{
	if (kind < STORAGE_KIND_REBUILDABLE || kind > STORAGE_KIND_SWAP)
		return NULL;
	return g_disk[kind][0] != '\0' ? g_disk[kind] : NULL;
}

enum storageplacement_error storageplacement_set(enum storage_kind kind, const char *disk_name)
{
	char prev[DISKROLE_DISK_NAME_MAX];
	char prev_uuid[DISK_FS_UUID_MAX];
	enum storageplacement_error err;

	if (kind < STORAGE_KIND_REBUILDABLE || kind > STORAGE_KIND_SWAP)
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
}
