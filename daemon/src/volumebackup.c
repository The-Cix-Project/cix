#include "volumebackup.h"

#include "disk.h"
#include "persist.h"
#include "treecopy.h"
#include "volume.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/*
 * Where snapshots live on the backup disk. One directory per volume,
 * one directory per snapshot inside it, named by a sortable timestamp:
 *
 *   <mount>/volume-backups/<volume>/<YYYYmmddTHHMMSSZ>/...
 *
 * The name being lexicographically sortable is what makes retention a
 * plain sort-and-trim rather than a date parse, and makes "newest" a
 * string comparison rather than a stat() of every directory.
 */
#define VOLUMEBACKUP_SUBDIR "volume-backups"

static char g_state_path[PATH_MAX];
static char g_disk[64];
static int g_enabled;
static int g_interval_hours = 24;

/* Outcome of the most recent attempt per volume, in memory only. A
 * replayed "it worked yesterday" is not evidence about now, the same
 * reasoning ADR-0182 applies to probed server health. */
struct attempt {
	char volume[VOLUME_NAME_MAX];
	int success;
	char error[160];
	time_t at;
	int in_use;
};
static struct attempt g_attempts[VOLUME_MAX];

static struct attempt *attempt_slot(const char *volume)
{
	int i, free_slot = -1;

	for (i = 0; i < VOLUME_MAX; i++) {
		if (g_attempts[i].in_use && strcmp(g_attempts[i].volume, volume) == 0)
			return &g_attempts[i];
		if (!g_attempts[i].in_use && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return NULL;
	memset(&g_attempts[free_slot], 0, sizeof(g_attempts[free_slot]));
	snprintf(g_attempts[free_slot].volume, sizeof(g_attempts[free_slot].volume), "%s", volume);
	g_attempts[free_slot].in_use = 1;
	return &g_attempts[free_slot];
}

static void record_attempt(const char *volume, int success, const char *error)
{
	struct attempt *a = attempt_slot(volume);

	if (a == NULL)
		return;
	a->success = success;
	a->at = time(NULL);
	snprintf(a->error, sizeof(a->error), "%s", error != NULL ? error : "");
}

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	volumebackup_write_config_json(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int volumebackup_init(const char *state_path)
{
	char *buf;
	size_t len;
	struct json_value *root;

	snprintf(g_state_path, sizeof(g_state_path), "%s", state_path);
	memset(g_attempts, 0, sizeof(g_attempts));
	if (persist_read_file(state_path, &buf, &len) != 0 || buf == NULL)
		return 0; /* nothing configured yet is a valid state, not an error */
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		return 0;
	}
	{
		const char *disk = json_as_string(json_object_get(root, "disk"));
		const struct json_value *en = json_object_get(root, "enabled");
		const struct json_value *iv = json_object_get(root, "interval_hours");

		if (disk != NULL)
			snprintf(g_disk, sizeof(g_disk), "%s", disk);
		g_enabled = (en != NULL && en->type == JSON_BOOL && en->u.boolean);
		if (iv != NULL && iv->type == JSON_NUMBER && json_as_number(iv) > 0)
			g_interval_hours = (int)json_as_number(iv);
	}
	json_free(root);
	return 0;
}

void volumebackup_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

int volumebackup_enabled(void)
{
	return g_enabled;
}

int volumebackup_interval_hours(void)
{
	return g_interval_hours;
}

const char *volumebackup_disk(void)
{
	return g_disk;
}

enum volumebackup_error volumebackup_set(const char *disk_name, int enabled, int interval_hours)
{
	if (interval_hours <= 0 || interval_hours > 24 * 365)
		return VOLUMEBACKUP_ERR_INVALID;
	snprintf(g_disk, sizeof(g_disk), "%s", disk_name != NULL ? disk_name : "");
	g_enabled = enabled ? 1 : 0;
	g_interval_hours = interval_hours;
	if (save_state() != 0)
		return VOLUMEBACKUP_ERR_PERSIST_FAILED;
	return VOLUMEBACKUP_OK;
}

void volumebackup_write_config_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "disk");
	if (g_disk[0] != '\0')
		jw_str(w, g_disk);
	else
		jw_null(w);
	jw_key(w, "enabled");
	jw_bool(w, g_enabled);
	jw_key(w, "interval_hours");
	jw_int(w, g_interval_hours);
	jw_obj_close(w);
}

/*
 * The snapshot store's own root on whichever disk currently carries the
 * configured name. Resolved fresh every time rather than cached: a disk
 * can be unmounted between one sweep and the next, and a stale path
 * would write the "backup" into the mountpoint directory on the OS disk
 * instead -- silently, and looking exactly like it worked.
 */
static int store_root(char *out, size_t out_size)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int count, i;

	if (g_disk[0] == '\0')
		return -1;
	count = disk_enumerate(disks, DISK_ENUM_MAX, "");
	for (i = 0; i < count; i++) {
		if (strcmp(disks[i].name, g_disk) != 0)
			continue;
		if (!disks[i].mounted)
			return -2; /* present but not mounted -- a different problem */
		snprintf(out, out_size, "%s/%s", disks[i].mount_path, VOLUMEBACKUP_SUBDIR);
		return 0;
	}
	return -1;
}

static void stamp_now(char *out, size_t out_size, time_t when)
{
	struct tm tmv;

	gmtime_r(&when, &tmv);
	strftime(out, out_size, "%Y%m%dT%H%M%SZ", &tmv);
}

/* Newest first, which is the order both retention and the API want. */
static int compare_stamp_desc(const void *a, const void *b)
{
	const struct volumebackup_snapshot *sa = a;
	const struct volumebackup_snapshot *sb = b;

	return strcmp(sb->stamp, sa->stamp);
}

int volumebackup_list(const char *volume_name, struct volumebackup_snapshot *out, int cap)
{
	char root[PATH_MAX];
	char dir[PATH_MAX];
	DIR *d;
	struct dirent *ent;
	int count = 0;

	if (store_root(root, sizeof(root)) != 0)
		return 0;
	snprintf(dir, sizeof(dir), "%s/%s", root, volume_name);
	d = opendir(dir);
	if (d == NULL)
		return 0;
	while ((ent = readdir(d)) != NULL && count < cap) {
		if (ent->d_name[0] == '.')
			continue;
		snprintf(out[count].stamp, sizeof(out[count].stamp), "%s", ent->d_name);
		out[count].bytes = 0;
		count++;
	}
	closedir(d);
	qsort(out, (size_t)count, sizeof(out[0]), compare_stamp_desc);
	return count;
}

void volumebackup_write_list_json(struct json_writer *w, const char *volume_name)
{
	struct volumebackup_snapshot snaps[VOLUMEBACKUP_MAX_SNAPSHOTS];
	int n = volumebackup_list(volume_name, snaps, VOLUMEBACKUP_MAX_SNAPSHOTS);
	int i;

	jw_arr_open(w);
	for (i = 0; i < n; i++) {
		jw_obj_open(w);
		jw_key(w, "stamp");
		jw_str(w, snaps[i].stamp);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

/* Trims to the newest `retain` snapshots. Runs after a successful
 * snapshot, never before -- deleting the oldest to make room for one
 * that then fails would lose history for nothing. */
static void trim_to_retention(const char *volume_name, int retain)
{
	struct volumebackup_snapshot snaps[VOLUMEBACKUP_MAX_SNAPSHOTS];
	char root[PATH_MAX];
	int n, i;

	if (retain <= 0)
		retain = VOLUMEBACKUP_DEFAULT_RETAIN;
	if (store_root(root, sizeof(root)) != 0)
		return;
	n = volumebackup_list(volume_name, snaps, VOLUMEBACKUP_MAX_SNAPSHOTS);
	for (i = retain; i < n; i++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s/%s", root, volume_name, snaps[i].stamp);
		persist_remove_tree(path);
	}
}

enum volumebackup_error volumebackup_take(const char *volume_name,
                                          int (*is_running)(const char *volume_name))
{
	struct volume *v = volume_find(volume_name);
	char root[PATH_MAX];
	char src[PATH_MAX];
	char dst[PATH_MAX];
	char stamp[VOLUMEBACKUP_STAMP_MAX];
	time_t now = time(NULL);
	int rc;

	if (v == NULL)
		return VOLUMEBACKUP_ERR_NOT_FOUND;
	/*
	 * A live writer means the copy captures whatever was on disk at
	 * that moment, so a file being written mid-copy is caught
	 * half-written. Refused by default for that reason -- but only by
	 * default: a service container is normally `restart: always` and
	 * never stops, so an unconditional refusal would mean "never back
	 * this up", quietly, for exactly the volumes that most need it.
	 * allow_while_running is the operator saying their data tolerates
	 * a crash-consistent copy, which for a home or config tree it
	 * generally does.
	 */
	if (!v->backup_while_running && is_running != NULL && is_running(volume_name)) {
		record_attempt(volume_name, 0, "a container mounting this volume is running");
		return VOLUMEBACKUP_ERR_IN_USE_RUNNING;
	}
	rc = store_root(root, sizeof(root));
	if (rc == -2) {
		record_attempt(volume_name, 0, "the configured backup disk is not mounted");
		return VOLUMEBACKUP_ERR_DISK_NOT_READY;
	}
	if (rc != 0) {
		record_attempt(volume_name, 0, "no backup disk configured");
		return VOLUMEBACKUP_ERR_NO_DISK;
	}
	if (volume_host_path(v, src, sizeof(src)) != 0) {
		record_attempt(volume_name, 0, "could not resolve the volume's own path");
		return VOLUMEBACKUP_ERR_COPY_FAILED;
	}

	stamp_now(stamp, sizeof(stamp), now);
	snprintf(dst, sizeof(dst), "%s/%s/%s", root, volume_name, stamp);
	if (persist_mkdir_p(dst) != 0 || treecopy_recursive(src, dst) != 0) {
		/* A partial copy is not a snapshot. Remove it rather than
		 * leaving something that will later be restored as if whole. */
		persist_remove_tree(dst);
		record_attempt(volume_name, 0, "copying the volume's data failed");
		return VOLUMEBACKUP_ERR_COPY_FAILED;
	}

	trim_to_retention(volume_name, v->backup_retain);
	volume_note_backup_taken(volume_name, now);
	record_attempt(volume_name, 1, "");
	return VOLUMEBACKUP_OK;
}

enum volumebackup_error volumebackup_restore(const char *volume_name, const char *stamp,
                                             int (*is_running)(const char *volume_name))
{
	struct volume *v = volume_find(volume_name);
	char root[PATH_MAX];
	char src[PATH_MAX];
	char dst[PATH_MAX];
	struct stat st;
	int rc;

	if (v == NULL)
		return VOLUMEBACKUP_ERR_NOT_FOUND;
	if (stamp == NULL || stamp[0] == '\0' || strchr(stamp, '/') != NULL)
		return VOLUMEBACKUP_ERR_INVALID;
	if (is_running != NULL && is_running(volume_name))
		return VOLUMEBACKUP_ERR_IN_USE_RUNNING;
	rc = store_root(root, sizeof(root));
	if (rc == -2)
		return VOLUMEBACKUP_ERR_DISK_NOT_READY;
	if (rc != 0)
		return VOLUMEBACKUP_ERR_NO_DISK;

	snprintf(src, sizeof(src), "%s/%s/%s", root, volume_name, stamp);
	if (stat(src, &st) != 0 || !S_ISDIR(st.st_mode))
		return VOLUMEBACKUP_ERR_NO_SUCH_SNAPSHOT;
	if (volume_host_path(v, dst, sizeof(dst)) != 0)
		return VOLUMEBACKUP_ERR_COPY_FAILED;

	/*
	 * Replace, not merge. Copying over the existing tree would leave
	 * files that the snapshot does not contain, so the result would be
	 * neither the old state nor the backed-up one -- and a restore that
	 * produces a third thing nobody has ever seen is worse than a
	 * refusal.
	 */
	persist_remove_tree(dst);
	if (persist_mkdir_p(dst) != 0 || treecopy_recursive(src, dst) != 0)
		return VOLUMEBACKUP_ERR_COPY_FAILED;
	return VOLUMEBACKUP_OK;
}

enum volumebackup_error volumebackup_delete_snapshot(const char *volume_name, const char *stamp)
{
	char root[PATH_MAX];
	char path[PATH_MAX];
	struct stat st;

	if (stamp == NULL || stamp[0] == '\0' || strchr(stamp, '/') != NULL)
		return VOLUMEBACKUP_ERR_INVALID;
	if (store_root(root, sizeof(root)) != 0)
		return VOLUMEBACKUP_ERR_NO_DISK;
	snprintf(path, sizeof(path), "%s/%s/%s", root, volume_name, stamp);
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		return VOLUMEBACKUP_ERR_NO_SUCH_SNAPSHOT;
	persist_remove_tree(path);
	return VOLUMEBACKUP_OK;
}

int volumebackup_sweep(time_t now, int (*is_running)(const char *volume_name))
{
	struct volume list[VOLUME_MAX];
	int n, i, taken = 0;

	if (!g_enabled || g_disk[0] == '\0')
		return 0;
	n = volume_list(list, VOLUME_MAX);
	for (i = 0; i < n; i++) {
		double due_after;

		if (!list[i].backup_enabled)
			continue;
		due_after = (double)g_interval_hours * 3600.0;
		if (list[i].backup_last_at != 0 && difftime(now, list[i].backup_last_at) < due_after)
			continue;
		/*
		 * A volume whose container is running and has not opted into
		 * crash-consistent copies is skipped BEFORE take() is called,
		 * so no failed attempt is recorded. It is a normal, expected
		 * state for a workload that stays up; recording a failure
		 * every interval would fill the status with something the
		 * operator already knows and drown the real failures in it.
		 * The state is visible either way -- the volume's own policy
		 * says whether it can be backed up while running.
		 */
		if (!list[i].backup_while_running && is_running != NULL && is_running(list[i].name))
			continue;
		if (volumebackup_take(list[i].name, is_running) == VOLUMEBACKUP_OK)
			taken++;
	}
	return taken;
}

void volumebackup_write_status_json(struct json_writer *w, const char *volume_name)
{
	int i;

	jw_obj_open(w);
	for (i = 0; i < VOLUME_MAX; i++) {
		if (!g_attempts[i].in_use || strcmp(g_attempts[i].volume, volume_name) != 0)
			continue;
		jw_key(w, "last_attempt_at");
		jw_int(w, (long long)g_attempts[i].at);
		jw_key(w, "last_attempt_ok");
		jw_bool(w, g_attempts[i].success);
		jw_key(w, "last_error");
		if (g_attempts[i].error[0] != '\0')
			jw_str(w, g_attempts[i].error);
		else
			jw_null(w);
		jw_obj_close(w);
		return;
	}
	/* Never attempted in this daemon's lifetime -- said plainly rather
	 * than implied by absent fields. */
	jw_key(w, "last_attempt_at");
	jw_null(w);
	jw_key(w, "last_attempt_ok");
	jw_null(w);
	jw_key(w, "last_error");
	jw_null(w);
	jw_obj_close(w);
}
