#include "api_swap.h"

#include "apiresp.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "swap.h"

#include "storageplacement.h"
#include "disk.h"
#include "diskrole.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void respond_swap_error(int fd, enum swap_error serr)
{
	switch (serr) {
	case SWAP_ERR_ALREADY_ENABLED:
		respond_error(fd, 409, "Conflict", "swap is already enabled -- disable it first to resize");
		return;
	case SWAP_ERR_NOT_ENABLED:
		respond_error(fd, 409, "Conflict", "swap is not enabled");
		return;
	case SWAP_ERR_INVALID_SIZE:
		respond_error(fd, 400, "Bad Request", "size_mb out of range");
		return;
	case SWAP_ERR_IO:
		respond_error(fd, 500, "Internal Server Error", "swap file creation or activation failed");
		return;
	case SWAP_ERR_PERSIST_FAILED:
		respond_error(fd, 500, "Internal Server Error", "swap state could not be persisted");
		return;
	case SWAP_OK:
		return;
	}
}

void handle_swap_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	swap_write_json(&w, storageplacement_get(STORAGE_KIND_SWAP));
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * issue #28: resolves disk_name to a real, currently-mounted disk
 * carrying the swap role, writing its swapfile path into out_path.
 * Same three checks (present, mounted, correct role) do_backup_
 * snapshot_now() already established for backup's own identically-shaped
 * "operator-named disk, validated at the point of real use" field --
 * deliberately not shared as a common helper, matching that this
 * project has never factored these three checks out for the other
 * three (state/rebuildable/log) singleton placements either. Returns
 * NULL (a real, specific reason) on failure, or out_path on success.
 */
static const char *resolve_swap_disk_now(const char *disk_name, char *out_path,
                                          size_t out_path_size)
{
	struct discovered_disk disks[DISK_ENUM_MAX];
	int n, i;

	n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
	for (i = 0; i < n; i++) {
		const char *role;

		if (strcmp(disks[i].name, disk_name) != 0)
			continue;
		if (!disks[i].mounted)
			return NULL;
		role = diskrole_lookup(disk_name);
		if (role == NULL || strcmp(role, "swap") != 0)
			return NULL;
		snprintf(out_path, out_path_size, "%s/%s/swapfile", DISKS_MOUNT_DIR, disk_name);
		return out_path;
	}
	return NULL;
}

void handle_swap_enable(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jsize, *jdisk;
	int64_t size_mb;
	const char *disk_name;
	enum swap_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jsize = json_object_get(root, "size_mb");
	if (jsize == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "size_mb missing");
		return;
	}
	size_mb = (int64_t)json_as_number(jsize);
	jdisk = json_object_get(root, "disk");
	disk_name = jdisk != NULL ? json_as_string(jdisk) : NULL;
	if (jdisk != NULL && disk_name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "disk must be a string");
		return;
	}

	/*
	 * Checked before touching repoint/placement at all: repointing
	 * while already enabled would desync g_file_path from the file
	 * swapon(2) is actually holding open, corrupting swap_write_json()'s
	 * own "path" field, for a call about to fail with
	 * SWAP_ERR_ALREADY_ENABLED regardless (swap_enable() below re-checks
	 * this itself -- this is not a substitute for that, just avoiding a
	 * side effect ahead of a call already known to fail).
	 */
	if (swap_is_enabled()) {
		json_free(root);
		respond_swap_error(fd, SWAP_ERR_ALREADY_ENABLED);
		return;
	}

	/*
	 * Every call repoints, whether disk_name is given or not -- an
	 * omitted disk explicitly means "the default location," not
	 * "whatever the last call happened to leave it at." disk_name is
	 * copied out of root before json_free() below, since resolve_swap_
	 * disk_now()/storageplacement_set() both need it to outlive that.
	 */
	if (disk_name != NULL) {
		char resolved_path[PATH_MAX];
		char disk_name_copy[DISKROLE_DISK_NAME_MAX];

		snprintf(disk_name_copy, sizeof(disk_name_copy), "%s", disk_name);
		json_free(root);
		if (resolve_swap_disk_now(disk_name_copy, resolved_path, sizeof(resolved_path)) == NULL) {
			respond_error(fd, 400, "Bad Request",
			              "disk is not currently present, not mounted, or does not carry the "
			              "swap role (POST /diskroles first)");
			return;
		}
		swap_repoint(resolved_path);
		storageplacement_set(STORAGE_KIND_SWAP, disk_name_copy);
	} else {
		json_free(root);
		swap_repoint(SWAP_FILE_PATH);
		storageplacement_set(STORAGE_KIND_SWAP, NULL);
	}

	serr = swap_enable(size_mb);
	if (serr != SWAP_OK) {
		respond_swap_error(fd, serr);
		return;
	}

	jw_init(&w);
	swap_write_json(&w, storageplacement_get(STORAGE_KIND_SWAP));
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_swap_disable(int fd)
{
	enum swap_error serr = swap_disable();

	if (serr != SWAP_OK) {
		respond_swap_error(fd, serr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}
