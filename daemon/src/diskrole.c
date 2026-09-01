#include "diskrole.h"
#include "disk.h"
#include "diskpart.h"
#include "namecheck.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

/* fs_type is empty until diskrole_set_fs_type() records a real
 * successful format (ADR-0142) -- never set by diskrole_create()
 * itself, since assigning a role has no destructive side effect. */
struct diskrole_entry {
	char disk_name[DISKROLE_DISK_NAME_MAX];
	enum diskrole_kind role;
	char fs_type[16];
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
	switch (role) {
	case DISKROLE_CONTAINER_STORAGE:
		return "container-storage";
	case DISKROLE_STATE_STORAGE:
		return "state-storage";
	case DISKROLE_REBUILDABLE_STORAGE:
		return "rebuildable-storage";
	case DISKROLE_LOG_STORAGE:
		return "log-storage";
	case DISKROLE_SWAP:
		return "swap";
	case DISKROLE_BACKUP:
	default:
		return "backup";
	}
}

static int role_from_str(const char *s, enum diskrole_kind *out)
{
	if (strcmp(s, "container-storage") == 0)
		*out = DISKROLE_CONTAINER_STORAGE;
	else if (strcmp(s, "backup") == 0)
		*out = DISKROLE_BACKUP;
	else if (strcmp(s, "state-storage") == 0)
		*out = DISKROLE_STATE_STORAGE;
	else if (strcmp(s, "rebuildable-storage") == 0)
		*out = DISKROLE_REBUILDABLE_STORAGE;
	else if (strcmp(s, "log-storage") == 0)
		*out = DISKROLE_LOG_STORAGE;
	else if (strcmp(s, "swap") == 0)
		*out = DISKROLE_SWAP;
	else
		return -1;
	return 0;
}

/*
 * Is this device part of the fixed OS layout, and therefore never a
 * role-assignment candidate?
 *
 * Asks diskpart's one rule rather than testing is_os_disk directly
 * (issue #9). A partition inherits is_os_disk from its parent, so the
 * bare flag also refuses an operator-created partition in the OS disk's
 * reserved free space -- which #140 went to some trouble to allow
 * creating, and which is useless without a role.
 */
static int disk_is_os_layout(const char *disk_name, const char *os_containers_dir)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n = disk_enumerate(disks, DISK_ENUM_MAX, os_containers_dir);
	int i;

	for (i = 0; i < n; i++) {
		if (strcmp(disks[i].name, disk_name) == 0)
			return diskpart_os_layout_untouchable(disks[i].name, disks[i].is_os_disk,
			                                       disks[i].is_partition);
	}
	return 0; /* not currently a real disk at all */
}

/*
 * Is a block device with this name present right now?
 *
 * Answered from sysfs, with no block I/O at all -- and that is the
 * point.
 *
 * This used to call disk_enumerate(), which opens every block device it
 * finds and READS ITS SUPERBLOCK to identify the filesystem
 * (disk_probe_fs_type()). stallwatch (#100) caught the consequence:
 * GET /v1/diskroles blocked in blk_execute_rq -- the kernel waiting on
 * a block-device command -- for six seconds, on the single-threaded
 * event loop, while the disk was busy with a build.
 *
 * The superblock read exists to fill in fs_type. This field is
 * "present", which never looks at fs_type. It was paying for an answer
 * it did not use.
 *
 * The check is faithful rather than approximate: disk_enumerate()
 * decides something is a real block device by reading its "size"
 * attribute and skipping anything without one (that is how loop-control
 * and friends are excluded), and it lists whole disks and partitions
 * alike. Testing for that same attribute asks exactly the question
 * enumeration would have answered, and costs one stat.
 */
static int disk_currently_present(const char *disk_name)
{
	char path[PATH_MAX];

	if (disk_name == NULL || disk_name[0] == '\0' || strchr(disk_name, '/') != NULL)
		return 0;
	snprintf(path, sizeof(path), "/sys/class/block/%s/size", disk_name);
	return access(path, F_OK) == 0;
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
		if (g_roles[i].fs_type[0] != '\0') {
			jw_key(&w, "fs_type");
			jw_str(&w, g_roles[i].fs_type);
		}
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
		const char *fs_type = json_as_string(json_object_get(item, "fs_type"));
		enum diskrole_kind role_kind;

		if (!simple_name_is_valid(disk_name, DISKROLE_DISK_NAME_MAX) || role == NULL ||
		    role_from_str(role, &role_kind) != 0) {
			json_free(root);
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			return -1;
		}

		memset(&g_roles[count], 0, sizeof(g_roles[count]));
		snprintf(g_roles[count].disk_name, sizeof(g_roles[count].disk_name), "%s", disk_name);
		g_roles[count].role = role_kind;
		if (fs_type != NULL)
			snprintf(g_roles[count].fs_type, sizeof(g_roles[count].fs_type), "%s", fs_type);
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
	if (role_str_in == NULL || role_from_str(role_str_in, &role) != 0)
		return DISKROLE_ERR_INVALID_ROLE;

	if (disk_is_os_layout(disk_name, os_containers_dir))
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

enum diskrole_error diskrole_set_fs_type(const char *disk_name, const char *fs_type)
{
	struct diskrole_entry *r = role_find(disk_name);

	if (r == NULL)
		return DISKROLE_ERR_NOT_FOUND;
	snprintf(r->fs_type, sizeof(r->fs_type), "%s", fs_type);
	if (save_state() != 0)
		return DISKROLE_ERR_PERSIST_FAILED;
	return DISKROLE_OK;
}

const char *diskrole_lookup_fs_type(const char *disk_name)
{
	struct diskrole_entry *r = role_find(disk_name);

	return (r != NULL && r->fs_type[0] != '\0') ? r->fs_type : NULL;
}

static void write_role_json_one(const struct diskrole_entry *r, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "disk_name");
	jw_str(w, r->disk_name);
	jw_key(w, "role");
	jw_str(w, role_str(r->role));
	jw_key(w, "present");
	jw_bool(w, disk_currently_present(r->disk_name));
	if (r->fs_type[0] != '\0') {
		jw_key(w, "fs_type");
		jw_str(w, r->fs_type);
	}
	jw_obj_close(w);
}

int diskrole_write_json_one(const char *disk_name, struct json_writer *w,
                             const char *os_containers_dir)
{
	struct diskrole_entry *r = role_find(disk_name);

	(void)os_containers_dir; /* presence no longer needs an enumeration */
	if (r == NULL)
		return 0;
	write_role_json_one(r, w);
	return 1;
}

void diskrole_write_json_list(struct json_writer *w, const char *os_containers_dir)
{
	int i;

	(void)os_containers_dir; /* presence no longer needs an enumeration */
	jw_arr_open(w);
	for (i = 0; i < DISKROLE_MAX; i++) {
		if (g_roles[i].in_use)
			write_role_json_one(&g_roles[i], w);
	}
	jw_arr_close(w);
}
