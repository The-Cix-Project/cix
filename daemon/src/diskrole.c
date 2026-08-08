#include "diskrole.h"
#include "disk.h"
#include "namecheck.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct diskrole_entry {
	char disk_name[DISKROLE_DISK_NAME_MAX];
	enum diskrole_kind role;
	int in_use;
};

static char g_state_path[512];
static struct diskrole_entry g_roles[DISKROLE_MAX];

static struct diskrole_entry *role_find(const char *disk_name)
{
	int i;

	for (i = 0; i < DISKROLE_MAX; i++) {
		if (g_roles[i].in_use && strcmp(g_roles[i].disk_name, disk_name) == 0)
			return &g_roles[i];
	}
	return NULL;
}

static const char *role_str(enum diskrole_kind role)
{
	return role == DISKROLE_CONTAINER_STORAGE ? "container-storage" : "backup";
}

static int disk_is_os_disk(const char *disk_name, const char *os_containers_dir)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n = disk_enumerate(disks, DISK_ENUM_MAX, os_containers_dir);
	int i;

	for (i = 0; i < n; i++) {
		if (strcmp(disks[i].name, disk_name) == 0)
			return disks[i].is_os_disk;
	}
	return 0; /* not currently a real disk at all -- can't be the OS disk */
}

static int disk_currently_present(const char *disk_name, const char *os_containers_dir)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n = disk_enumerate(disks, DISK_ENUM_MAX, os_containers_dir);
	int i;

	for (i = 0; i < n; i++) {
		if (strcmp(disks[i].name, disk_name) == 0)
			return 1;
	}
	return 0;
}

static int save_state(void)
{
	struct json_writer w;
	int rc, i;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < DISKROLE_MAX; i++) {
		if (!g_roles[i].in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "disk_name");
		jw_str(&w, g_roles[i].disk_name);
		jw_key(&w, "role");
		jw_str(&w, role_str(g_roles[i].role));
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted disk roles\n", g_state_path);
		return -1;
	}
	if (root->u.array.count > DISKROLE_MAX) {
		json_free(root);
		fprintf(stderr, "%s: more disk roles persisted than DISKROLE_MAX\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *disk_name = json_as_string(json_object_get(item, "disk_name"));
		const char *role = json_as_string(json_object_get(item, "role"));

		if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX) || role == NULL ||
		    (strcmp(role, "container-storage") != 0 && strcmp(role, "backup") != 0)) {
			json_free(root);
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			return -1;
		}

		memset(&g_roles[count], 0, sizeof(g_roles[count]));
		snprintf(g_roles[count].disk_name, sizeof(g_roles[count].disk_name), "%s", disk_name);
		g_roles[count].role =
		    strcmp(role, "container-storage") == 0 ? DISKROLE_CONTAINER_STORAGE : DISKROLE_BACKUP;
		g_roles[count].in_use = 1;
		count++;
	}
	json_free(root);
	return 0;
}

int diskrole_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	memset(g_roles, 0, sizeof(g_roles));
	return load_state();
}

enum diskrole_error diskrole_create(const char *disk_name, const char *role_str_in,
                                     const char *os_containers_dir)
{
	enum diskrole_kind role;
	int slot = -1;
	int i;

	if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX))
		return DISKROLE_ERR_INVALID_DISK_NAME;
	if (role_str_in == NULL)
		return DISKROLE_ERR_INVALID_ROLE;
	if (strcmp(role_str_in, "container-storage") == 0)
		role = DISKROLE_CONTAINER_STORAGE;
	else if (strcmp(role_str_in, "backup") == 0)
		role = DISKROLE_BACKUP;
	else
		return DISKROLE_ERR_INVALID_ROLE;

	if (disk_is_os_disk(disk_name, os_containers_dir))
		return DISKROLE_ERR_IS_OS_DISK;

	if (role_find(disk_name) != NULL)
		return DISKROLE_ERR_DUPLICATE;

	for (i = 0; i < DISKROLE_MAX; i++) {
		if (!g_roles[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return DISKROLE_ERR_FULL;

	memset(&g_roles[slot], 0, sizeof(g_roles[slot]));
	snprintf(g_roles[slot].disk_name, sizeof(g_roles[slot].disk_name), "%s", disk_name);
	g_roles[slot].role = role;
	g_roles[slot].in_use = 1;

	if (save_state() != 0) {
		memset(&g_roles[slot], 0, sizeof(g_roles[slot]));
		return DISKROLE_ERR_PERSIST_FAILED;
	}
	return DISKROLE_OK;
}

enum diskrole_error diskrole_delete(const char *disk_name)
{
	struct diskrole_entry *r = role_find(disk_name);

	if (r == NULL)
		return DISKROLE_ERR_NOT_FOUND;
	memset(r, 0, sizeof(*r));
	if (save_state() != 0)
		return DISKROLE_ERR_PERSIST_FAILED;
	return DISKROLE_OK;
}

const char *diskrole_lookup(const char *disk_name)
{
	struct diskrole_entry *r = role_find(disk_name);

	return (r != NULL) ? role_str(r->role) : NULL;
}

static void write_role_json_one(const struct diskrole_entry *r, struct json_writer *w,
                                 const char *os_containers_dir)
{
	jw_obj_open(w);
	jw_key(w, "disk_name");
	jw_str(w, r->disk_name);
	jw_key(w, "role");
	jw_str(w, role_str(r->role));
	jw_key(w, "present");
	jw_bool(w, disk_currently_present(r->disk_name, os_containers_dir));
	jw_obj_close(w);
}

int diskrole_write_json_one(const char *disk_name, struct json_writer *w,
                             const char *os_containers_dir)
{
	struct diskrole_entry *r = role_find(disk_name);

	if (r == NULL)
		return 0;
	write_role_json_one(r, w, os_containers_dir);
	return 1;
}

void diskrole_write_json_list(struct json_writer *w, const char *os_containers_dir)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < DISKROLE_MAX; i++) {
		if (g_roles[i].in_use)
			write_role_json_one(&g_roles[i], w, os_containers_dir);
	}
	jw_arr_close(w);
}
