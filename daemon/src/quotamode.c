#include "quotamode.h"
#include "btrfs.h"
#include "disk.h"
#include "json.h"
#include "logstore.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum quotamode g_mode = QUOTAMODE_SIMPLE;
static char g_state_path[512];

const char *quotamode_name(enum quotamode mode)
{
	return mode == QUOTAMODE_FULL ? "full" : "simple";
}

int quotamode_parse(const char *s, enum quotamode *out)
{
	if (s == NULL)
		return -1;
	if (strcmp(s, "simple") == 0)
		*out = QUOTAMODE_SIMPLE;
	else if (strcmp(s, "full") == 0)
		*out = QUOTAMODE_FULL;
	else
		return -1;
	return 0;
}

/*
 * The two features `mkfs.btrfs -O list-all` lists in btrfs-progs 7.1-12
 * (cix-hosttools, measured 2026-10-27): "quota" for qgroups, "squota"
 * for simple accounting.
 */
const char *quotamode_mkfs_feature(enum quotamode mode)
{
	return mode == QUOTAMODE_FULL ? "quota" : "squota";
}

enum quotamode quotamode_get(void)
{
	return g_mode;
}

static int save_state(void)
{
	char buf[64];
	int n;

	if (g_state_path[0] == '\0')
		return -1;
	n = snprintf(buf, sizeof(buf), "{\"mode\":\"%s\"}\n", quotamode_name(g_mode));
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -1;
	return persist_atomic_write(g_state_path, buf, (size_t)n);
}

void quotamode_init(const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	g_mode = QUOTAMODE_SIMPLE;
	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		root = json_parse(buf, len);
		if (root != NULL) {
			const char *m = json_as_string(json_object_get(root, "mode"));

			/* A file gone bad leaves the default rather than refusing
			 * to boot, and says so. */
			if (quotamode_parse(m, &g_mode) != 0) {
				g_mode = QUOTAMODE_SIMPLE;
				logstore_write("cixd", "warn",
				               "quota mode: ignoring an invalid mode in %s -- simple "
				               "(ADR-0359)",
				               path);
			}
			json_free(root);
		}
	}
	free(buf);
}

/*
 * The setting, and the mode each mounted btrfs really has. The second is
 * read from each filesystem's qgroup status item rather than from the
 * setting, because a filesystem keeps its mode for life and the setting
 * governs only what is made next (ADR-0359 decision 5).
 */
void quotamode_write_json(struct json_writer *w, const char *containers_dir)
{
	static struct discovered_disk disks[DISK_ENUM_MAX];
	int count, i;

	jw_obj_open(w);
	jw_key(w, "mode");
	jw_str(w, quotamode_name(g_mode));
	jw_key(w, "filesystems");
	jw_arr_open(w);
	count = disk_enumerate(disks, DISK_ENUM_MAX, containers_dir);
	for (i = 0; i < count; i++) {
		enum cix_btrfs_quota_mode mode;
		int inconsistent = 0;

		if (!disks[i].mounted || strcmp(disks[i].fs_type, "btrfs") != 0)
			continue;
		if (cix_btrfs_quota_mode_of(disks[i].mount_path, &mode, &inconsistent) != 0)
			continue;
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, disks[i].name);
		jw_key(w, "mount_path");
		jw_str(w, disks[i].mount_path);
		jw_key(w, "mode");
		jw_str(w, mode == CIX_BTRFS_QUOTA_SIMPLE ? "simple"
		          : mode == CIX_BTRFS_QUOTA_FULL ? "full"
		                                         : "off");
		jw_key(w, "inconsistent");
		jw_bool(w, inconsistent);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

enum quotamode_error quotamode_set(enum quotamode mode)
{
	enum quotamode previous = g_mode;

	g_mode = mode;
	if (save_state() != 0) {
		g_mode = previous;
		return QUOTAMODE_ERR_PERSIST_FAILED;
	}
	return QUOTAMODE_OK;
}
