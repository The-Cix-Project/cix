#include "storageplacement.h"
#include "diskrole.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static char g_state_path[512];
/* Empty string: no placement set for this kind (default OS-disk). */
static char g_disk[STORAGE_KIND_COUNT][DISKROLE_DISK_NAME_MAX];

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
	}

	json_free(root);
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
	enum storageplacement_error err;

	if (kind < STORAGE_KIND_REBUILDABLE || kind > STORAGE_KIND_SWAP)
		return STORAGEPLACEMENT_ERR_PERSIST_FAILED;

	snprintf(prev, sizeof(prev), "%s", g_disk[kind]);
	if (disk_name != NULL)
		snprintf(g_disk[kind], sizeof(g_disk[kind]), "%s", disk_name);
	else
		g_disk[kind][0] = '\0';

	err = save_state();
	if (err != STORAGEPLACEMENT_OK)
		snprintf(g_disk[kind], sizeof(g_disk[kind]), "%s", prev); /* roll back on persist failure */
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
