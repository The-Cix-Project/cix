#include "volume.h"
#include "disk.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static struct volume g_volumes[VOLUME_MAX];
static char g_state_path[PATH_MAX];
static char g_volumes_dir[PATH_MAX];

int volume_name_is_valid(const char *name)
{
	size_t i, len;

	if (name == NULL || name[0] == '\0')
		return 0;
	len = strlen(name);
	if (len >= VOLUME_NAME_MAX)
		return 0;
	/*
	 * A volume name becomes a real directory name, so anything that
	 * could escape its parent or collide with path syntax is refused
	 * outright rather than sanitised: leading '.' (hidden/".."), and
	 * anything outside a conservative alnum/dash/underscore set.
	 */
	if (name[0] == '.' || name[0] == '-')
		return 0;
	for (i = 0; i < len; i++) {
		char c = name[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

static struct volume *find_slot(const char *name)
{
	int i;

	for (i = 0; i < VOLUME_MAX; i++) {
		if (g_volumes[i].in_use && strcmp(g_volumes[i].name, name) == 0)
			return &g_volumes[i];
	}
	return NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	volume_write_json_list(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int volume_init(const char *state_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	memset(g_volumes, 0, sizeof(g_volumes));
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* never written yet */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted volume state\n", g_state_path);
		return -1;
	}
	for (i = 0; i < root->u.array.count && count < VOLUME_MAX; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *name = json_as_string(json_object_get(item, "name"));
		const char *disk = json_as_string(json_object_get(item, "disk"));
		const struct json_value *created = json_object_get(item, "created_at");

		if (name == NULL || !volume_name_is_valid(name))
			continue; /* skip a corrupt entry rather than fail the whole load */
		memset(&g_volumes[count], 0, sizeof(g_volumes[count]));
		snprintf(g_volumes[count].name, sizeof(g_volumes[count].name), "%s", name);
		if (disk != NULL)
			snprintf(g_volumes[count].disk, sizeof(g_volumes[count].disk), "%s", disk);
		if (created != NULL && created->type == JSON_NUMBER)
			g_volumes[count].created_at = (time_t)json_as_number(created);
		g_volumes[count].in_use = 1;
		count++;
	}
	json_free(root);
	return 0;
}

void volume_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

/*
 * Where a volume's data actually lives. Mirrors container_root_for()'s
 * own resolution exactly: a named, currently-mounted disk puts it under
 * that disk, anything else falls back to the default location. Resolved
 * FRESH every time rather than cached, the same "look up the disk now,
 * never trust a remembered path" discipline the rest of this project's
 * storage placement already follows.
 */
int volume_host_path(const struct volume *v, char *out, size_t out_size)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int count, i;

	if (v == NULL)
		return -1;
	if (v->disk[0] != '\0') {
		count = disk_enumerate(disks, DISK_ENUM_MAX, g_volumes_dir);
		for (i = 0; i < count; i++) {
			if (strcmp(disks[i].name, v->disk) == 0 && disks[i].mounted) {
				snprintf(out, out_size, "%s/volumes/%s", disks[i].mount_path, v->name);
				return 0;
			}
		}
	}
	snprintf(out, out_size, "%s/%s", g_volumes_dir, v->name);
	return 0;
}

void volume_set_dir(const char *dir)
{
	snprintf(g_volumes_dir, sizeof(g_volumes_dir), "%s", dir);
}

enum volume_error volume_create(const char *name, const char *disk, struct volume **out)
{
	struct volume *v;
	char path[PATH_MAX];
	int i, slot = -1;

	if (!volume_name_is_valid(name))
		return VOLUME_ERR_INVALID_NAME;
	if (find_slot(name) != NULL)
		return VOLUME_ERR_DUPLICATE;
	for (i = 0; i < VOLUME_MAX; i++) {
		if (!g_volumes[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return VOLUME_ERR_FULL;

	v = &g_volumes[slot];
	memset(v, 0, sizeof(*v));
	snprintf(v->name, sizeof(v->name), "%s", name);
	if (disk != NULL)
		snprintf(v->disk, sizeof(v->disk), "%s", disk);
	v->created_at = time(NULL);
	v->in_use = 1;

	if (volume_host_path(v, path, sizeof(path)) != 0 || persist_mkdir_p(path) != 0) {
		memset(v, 0, sizeof(*v));
		return VOLUME_ERR_IO;
	}
	if (save_state() != 0) {
		memset(v, 0, sizeof(*v));
		return VOLUME_ERR_PERSIST_FAILED;
	}
	if (out != NULL)
		*out = v;
	return VOLUME_OK;
}

enum volume_error volume_delete(const char *name)
{
	struct volume *v = find_slot(name);
	char path[PATH_MAX];

	if (v == NULL)
		return VOLUME_ERR_NOT_FOUND;
	/*
	 * The data goes with the record. This is the one genuinely
	 * destructive volume operation and there is no undo -- the caller
	 * is responsible for having established that nothing references it
	 * (main.c refuses while any container definition still names it).
	 */
	if (volume_host_path(v, path, sizeof(path)) == 0)
		persist_remove_tree(path);
	memset(v, 0, sizeof(*v));
	return save_state() == 0 ? VOLUME_OK : VOLUME_ERR_PERSIST_FAILED;
}

struct volume *volume_find(const char *name)
{
	return find_slot(name);
}

int volume_list(struct volume out[], int max)
{
	int i, n = 0;

	for (i = 0; i < VOLUME_MAX && n < max; i++) {
		if (g_volumes[i].in_use)
			out[n++] = g_volumes[i];
	}
	return n;
}

void volume_write_json_one(const struct volume *v, struct json_writer *w)
{
	char path[PATH_MAX];

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, v->name);
	jw_key(w, "disk");
	if (v->disk[0] != '\0')
		jw_str(w, v->disk);
	else
		jw_null(w);
	jw_key(w, "created_at");
	jw_int(w, (long long)v->created_at);
	/* Reported so an operator can see where the data really landed --
	 * a disk that is currently unmounted silently falls back to the
	 * default location, and that should never be invisible. */
	jw_key(w, "host_path");
	if (volume_host_path(v, path, sizeof(path)) == 0)
		jw_str(w, path);
	else
		jw_null(w);
	jw_obj_close(w);
}

void volume_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < VOLUME_MAX; i++) {
		if (g_volumes[i].in_use)
			volume_write_json_one(&g_volumes[i], w);
	}
	jw_arr_close(w);
}
