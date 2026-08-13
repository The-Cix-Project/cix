#include "backupconfig.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BACKUPCONFIG_ERROR_MAX 256

static char g_state_path[512];
static char g_disk[64]; /* empty: no disk configured */
static int g_enabled;
static int g_interval_hours;

static enum backup_snapshot_state g_last_state = BACKUP_SNAPSHOT_NEVER;
static time_t g_last_attempt_unixtime;
static char g_last_error[BACKUPCONFIG_ERROR_MAX];

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jdisk, *jenabled, *jinterval;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed persisted backup config\n", g_state_path);
		return -1;
	}

	jdisk = json_object_get(root, "disk");
	if (jdisk != NULL && jdisk->type == JSON_STRING) {
		const char *disk = json_as_string(jdisk);

		if (disk != NULL)
			snprintf(g_disk, sizeof(g_disk), "%s", disk);
	}
	jenabled = json_object_get(root, "enabled");
	if (jenabled != NULL && jenabled->type == JSON_BOOL)
		g_enabled = jenabled->u.boolean;
	jinterval = json_object_get(root, "interval_hours");
	if (jinterval != NULL)
		g_interval_hours = (int)json_as_number(jinterval);

	json_free(root);
	return 0;
}

static enum backupconfig_error save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "disk");
	if (g_disk[0] != '\0')
		jw_str(&w, g_disk);
	else
		jw_null(&w);
	jw_key(&w, "enabled");
	jw_bool(&w, g_enabled);
	jw_key(&w, "interval_hours");
	jw_int(&w, g_interval_hours);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc == 0 ? BACKUPCONFIG_OK : BACKUPCONFIG_ERR_PERSIST_FAILED;
}

int backupconfig_init(const char *state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", state_path);
	g_disk[0] = '\0';
	g_enabled = 0;
	g_interval_hours = 0;
	return load_state();
}

void backupconfig_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

const char *backupconfig_disk(void)
{
	return g_disk[0] != '\0' ? g_disk : NULL;
}

int backupconfig_enabled(void)
{
	return g_enabled;
}

int backupconfig_interval_hours(void)
{
	return g_interval_hours;
}

enum backupconfig_error backupconfig_set(const char *disk_name, int enabled, int interval_hours)
{
	char prev_disk[sizeof(g_disk)];
	int prev_enabled = g_enabled;
	int prev_interval = g_interval_hours;
	enum backupconfig_error err;

	if (interval_hours < 0)
		return BACKUPCONFIG_ERR_INVALID_INTERVAL;

	snprintf(prev_disk, sizeof(prev_disk), "%s", g_disk);
	if (disk_name != NULL)
		snprintf(g_disk, sizeof(g_disk), "%s", disk_name);
	else
		g_disk[0] = '\0';
	g_enabled = enabled;
	g_interval_hours = interval_hours;

	err = save_state();
	if (err != BACKUPCONFIG_OK) {
		snprintf(g_disk, sizeof(g_disk), "%s", prev_disk);
		g_enabled = prev_enabled;
		g_interval_hours = prev_interval;
	}
	return err;
}

void backupconfig_write_json(struct json_writer *w)
{
	jw_key(w, "disk");
	if (g_disk[0] != '\0')
		jw_str(w, g_disk);
	else
		jw_null(w);
	jw_key(w, "enabled");
	jw_bool(w, g_enabled);
	jw_key(w, "interval_hours");
	jw_int(w, g_interval_hours);
}

void backupconfig_record_attempt(int success, const char *error_msg)
{
	g_last_attempt_unixtime = time(NULL);
	if (success) {
		g_last_state = BACKUP_SNAPSHOT_OK;
		g_last_error[0] = '\0';
	} else {
		g_last_state = BACKUP_SNAPSHOT_FAILED;
		snprintf(g_last_error, sizeof(g_last_error), "%s", error_msg != NULL ? error_msg : "unknown error");
	}
}

static const char *state_str(enum backup_snapshot_state s)
{
	switch (s) {
	case BACKUP_SNAPSHOT_OK:
		return "ok";
	case BACKUP_SNAPSHOT_FAILED:
		return "failed";
	case BACKUP_SNAPSHOT_NEVER:
	default:
		return "never";
	}
}

void backupconfig_write_status_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "state");
	jw_str(w, state_str(g_last_state));
	if (g_last_state != BACKUP_SNAPSHOT_NEVER) {
		jw_key(w, "last_attempt_unixtime");
		jw_int(w, (long long)g_last_attempt_unixtime);
	}
	if (g_last_state == BACKUP_SNAPSHOT_FAILED) {
		jw_key(w, "error");
		jw_str(w, g_last_error);
	}
	jw_obj_close(w);
}
