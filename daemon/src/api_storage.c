#include "api_storage.h"

#include "apiresp.h"
#include "apiroute.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "logstore.h"
#include "containerdef.h"
#include "backupconfig.h"
#include "device.h"
#include "devicemap.h"
#include "disk.h"
#include "diskrole.h"
#include "diskformat.h"
#include "diskpart.h"
#include "registry.h"
#include "namecheck.h"
#include "storageplacement.h"
#include "persist.h"

#include <stdio.h>
#include <string.h>

void handle_device_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "devices");
	/*
	 * registry_device_holders injected rather than called from device.c
	 * (#356) -- device.c has no knowledge of containers and does not
	 * gain any here, the same shape registry_write_json_list() already
	 * uses to ask containerdef.c a question.
	 */
	device_write_json_list(&w, CONTAINERS_DIR, registry_device_holders);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Real host block devices (Phase A, multi-disk management -- see
 * ROADMAP.md's own queue entry). Read-only, live-enumerated exactly
 * like GET /devices above (no persisted state yet -- role assignment
 * is a later phase). CONTAINERS_DIR is passed straight through so
 * disk.c can flag which one disk is the fixed OS disk without needing
 * any daemon-layer state of its own.
 */
void handle_disk_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "storage");
	disk_write_json_list(&w, CONTAINERS_DIR);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_devicemap_error(int fd, enum devicemap_error err)
{
	switch (err) {
	case DEVICEMAP_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid name");
		break;
	case DEVICEMAP_ERR_INVALID_KIND:
		respond_error(fd, 400, "Bad Request", "kind must be \"exact\" or \"vendor_model\"");
		break;
	case DEVICEMAP_ERR_INVALID_SELECTOR:
		respond_error(fd, 400, "Bad Request", "invalid selector");
		break;
	case DEVICEMAP_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a device mapping with this name already exists");
		break;
	case DEVICEMAP_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "device mapping table full");
		break;
	case DEVICEMAP_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such device mapping");
		break;
	case DEVICEMAP_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "device mapping operation failed");
		break;
	}
}

void handle_devicemap_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "devicemaps");
	devicemap_write_json_list(&w, CONTAINERS_DIR);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_devicemap_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *kind, *selector;
	enum devicemap_error derr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	name = json_as_string(json_object_get(root, "name"));
	kind = json_as_string(json_object_get(root, "kind"));
	selector = json_as_string(json_object_get(root, "selector"));
	if (name == NULL || kind == NULL || selector == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name, kind, and selector are required");
		return;
	}

	{
		/* name is a pointer into root's own parsed tree -- copied here
		 * since devicemap_write_json_one() below needs it again after
		 * json_free(root) makes the original dangling. */
		char name_buf[DEVICEMAP_NAME_MAX];

		snprintf(name_buf, sizeof(name_buf), "%s", name);
		derr = devicemap_create(name, kind, selector);
		json_free(root);

		if (derr != DEVICEMAP_OK) {
			respond_devicemap_error(fd, derr);
			return;
		}

		{
			struct json_writer w;

			jw_init(&w);
			devicemap_write_json_one(name_buf, CONTAINERS_DIR, &w);
			respond_json(fd, 201, "Created", &w);
			jw_free(&w);
		}
	}
}

void handle_devicemap_delete(int fd, const char *name)
{
	enum devicemap_error derr = devicemap_delete(name);

	if (derr != DEVICEMAP_OK) {
		respond_devicemap_error(fd, derr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * Multi-disk management Phase B (ROADMAP.md): persisted disk role
 * assignment, the direct follow-up to Phase A's read-only GET /disks.
 * CONTAINERS_DIR is threaded through exactly like handle_disk_list()'s
 * own os_containers_dir param, so diskrole.c can reject assigning a
 * role to the real OS disk without needing any daemon-layer state of
 * its own.
 */
static void respond_diskrole_error(int fd, enum diskrole_error err)
{
	switch (err) {
	case DISKROLE_ERR_INVALID_DISK_NAME:
		respond_error(fd, 400, "Bad Request", "invalid disk_name");
		break;
	case DISKROLE_ERR_INVALID_ROLE:
		/*
		 * Names the whole vocabulary. It listed two roles for as long
		 * as there were two, and stayed that way while four more were
		 * added -- so an operator who typed a real, supported role and
		 * got it slightly wrong was told the wrong thing about what
		 * exists.
		 */
		respond_error(fd, 400, "Bad Request",
		              "role must be one of \"container-storage\", \"backup\", "
		              "\"rebuildable-storage\", \"log-storage\" or \"swap\"");
		break;
	case DISKROLE_ERR_IS_OS_DISK:
		respond_error(fd, 400, "Bad Request",
		              "this disk holds the fixed OS layout -- it is never a role-assignment candidate");
		break;
	case DISKROLE_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this disk already has a role assigned");
		break;
	case DISKROLE_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "disk role table full");
		break;
	case DISKROLE_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no role assigned to this disk");
		break;
	case DISKROLE_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "disk role operation failed");
		break;
	}
}

void handle_diskrole_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "storage_roles");
	diskrole_write_json_list(&w, CONTAINERS_DIR);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_diskrole_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *disk_name, *role;
	enum diskrole_error derr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	disk_name = json_as_string(json_object_get(root, "disk_name"));
	role = json_as_string(json_object_get(root, "role"));
	if (disk_name == NULL || role == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "disk_name and role are required");
		return;
	}

	{
		/* disk_name is a pointer into root's own parsed tree --
		 * copied here since diskrole_write_json_one() below needs
		 * it again after json_free(root) makes the original
		 * dangling. */
		char disk_name_buf[DISKROLE_DISK_NAME_MAX];

		snprintf(disk_name_buf, sizeof(disk_name_buf), "%s", disk_name);
		derr = diskrole_create(disk_name, role, CONTAINERS_DIR);
		json_free(root);

		if (derr != DISKROLE_OK) {
			respond_diskrole_error(fd, derr);
			return;
		}

		{
			struct json_writer w;

			jw_init(&w);
			diskrole_write_json_one(disk_name_buf, &w, CONTAINERS_DIR);
			respond_json(fd, 201, "Created", &w);
			jw_free(&w);
		}
	}
}

/*
 * ADR-0141: true if disk_name is the *currently active* placement for
 * any of the three daemon-wide storage singletons (state-storage/
 * log-storage/rebuildable-storage), OR the currently configured
 * backup-config disk -- removing the role out from under an in-use
 * placement, or destroying it via format, would leave the daemon's own
 * live-location tracking (or, for backup, the operator's own configured
 * disaster-recovery target) pointing at a disk that, per its own role
 * table, doesn't do that anymore. Shared by handle_diskrole_delete()
 * and handle_disk_format_post() below. Named for the three storage
 * kinds since they came first, but backup-config's own disk is checked
 * here too -- ADR-0141's own safety-check section lists it explicitly
 * alongside the three, not as an afterthought.
 */
int is_active_storage_singleton_placement(const char *disk_name)
{
	const char *log_disk = storageplacement_get(STORAGE_KIND_LOG);
	const char *rebuildable_disk = storageplacement_get(STORAGE_KIND_REBUILDABLE);
	const char *backup_disk = backupconfig_disk();
	const char *swap_disk = storageplacement_get(STORAGE_KIND_SWAP);

	return (log_disk != NULL && strcmp(log_disk, disk_name) == 0) ||
	       (rebuildable_disk != NULL && strcmp(rebuildable_disk, disk_name) == 0) ||
	       (backup_disk != NULL && strcmp(backup_disk, disk_name) == 0) ||
	       (swap_disk != NULL && strcmp(swap_disk, disk_name) == 0);
}

/*
 * ADR-0142 Section 4's own natural extension of the safety check above:
 * a container-storage-role disk that one or more real containers are
 * actually placed on (via POST /containers' own original "disk" field,
 * ADR-0102 -- predating this ADR entirely, migrate-storage is simply
 * the first thing that ever needed this check to exist) is exactly as
 * unsafe to pull the role out from under, or reformat, as any of the
 * four singleton placements above -- doing so would silently orphan or
 * destroy that container's own live workload data. Checks every live
 * registry entry (e->disk_name) and every persisted definition
 * currently between a crash and its next restart (re-parsed from its
 * own stored body's "disk" field, the same fallback DELETE's own
 * crashed-container cleanup already uses) -- a container can be using
 * a disk in either state.
 */
int disk_has_container_in_use(const char *disk_name)
{
	char names[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int count = containerdef_resolve_order(names);
	int i;

	for (i = 0; i < count; i++) {
		struct registry_entry *live = registry_find(names[i]);

		if (live != NULL) {
			if (live->disk_name[0] != '\0' && strcmp(live->disk_name, disk_name) == 0)
				return 1;
			continue;
		}
		{
			struct container_def *def = containerdef_find(names[i]);
			struct json_value *defroot;

			if (def == NULL)
				continue;
			defroot = json_parse(def->body, def->body_len);
			if (defroot != NULL) {
				const char *d = json_as_string(json_object_get(defroot, "disk"));
				int match = (d != NULL && strcmp(d, disk_name) == 0);

				json_free(defroot);
				if (match)
					return 1;
			}
		}
	}
	return 0;
}

void handle_diskrole_delete(int fd, const char *disk_name)
{
	enum diskrole_error derr;

	if (is_active_storage_singleton_placement(disk_name)) {
		respond_error(fd, 409, "Conflict",
		              "this disk is the active state-storage, log-storage, or rebuildable-storage "
		              "placement, the configured backup-config disk, or the current swap placement "
		              "(issue #28) -- migrate away first (POST .../migrate to a different disk or "
		              "disk: null, PUT /v1/system/backup-config with a different disk/null, or "
		              "POST /v1/system/swap with a different disk/omitted) before removing its role");
		return;
	}
	if (disk_has_container_in_use(disk_name)) {
		respond_error(fd, 409, "Conflict",
		              "one or more containers currently have their own storage on this disk -- "
		              "migrate them away first (POST /containers/{name}/migrate-storage) before "
		              "removing its role");
		return;
	}
	derr = diskrole_delete(disk_name);
	if (derr != DISKROLE_OK) {
		respond_diskrole_error(fd, derr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * Multi-disk management Phase C (ROADMAP.md): format + mount an
 * already-role-assigned disk. Deliberately a SEPARATE, explicit action
 * from Phase B's role assignment above (confirmed with the user) --
 * assigning a role never has a destructive side effect of its own, and
 * this endpoint requires the operator to name the exact target disk
 * again in the request body (confirm_disk_name, matching the URL's own
 * disk name) as a deliberate double-confirmation before anything
 * irreversible happens. See diskformat.h for the actual job mechanism.
 */
void respond_diskformat_error(int fd, enum diskformat_error err)
{
	switch (err) {
	case DISKFORMAT_ERR_INVALID_DISK_NAME:
		respond_error(fd, 400, "Bad Request", "invalid disk name");
		break;
	case DISKFORMAT_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such disk");
		break;
	case DISKFORMAT_ERR_IS_OS_DISK:
		respond_error(fd, 400, "Bad Request",
		              "this disk holds the fixed OS layout -- it can never be formatted or unmounted");
		break;
	case DISKFORMAT_ERR_NO_ROLE:
		respond_error(fd, 400, "Bad Request",
		              "this disk has no assigned role -- assign one via POST /v1/storage-roles first");
		break;
	case DISKFORMAT_ERR_BUSY:
		respond_error(fd, 409, "Conflict", "a format job is already running");
		break;
	case DISKFORMAT_ERR_MKDIR_FAILED:
		respond_error(fd, 500, "Internal Server Error", "could not create mount point");
		break;
	case DISKFORMAT_ERR_NOT_MOUNTED:
		respond_error(fd, 400, "Bad Request", "this disk is not currently mounted");
		break;
	case DISKFORMAT_ERR_UMOUNT_FAILED:
		respond_error(fd, 500, "Internal Server Error",
		              "umount2(2) failed -- something on this disk may still be busy/in use");
		break;
	case DISKFORMAT_ERR_SPAWN_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "could not start format job");
		break;
	}
}



/*
 * Found live, issue #34: a disk an operator has already `diskrole rm`'d
 * stays mounted forever -- there was no way to actually let go of it,
 * and a real, currently-mounted-but-role-less disk turned out to
 * correlate with a genuine mountns_pivot() EXDEV failure in every
 * subsequent container creation. Synchronous (diskformat_unmount()'s
 * own doc comment), not async like format -- a real umount2(2) is fast.
 * Same double-confirmation shape as handle_disk_format_post() (this is
 * a real, if less destructive, action -- data on the disk survives, but
 * anything still relying on it being mounted stops working the instant
 * this succeeds) and the identical two data-safety checks that handler
 * already has: refuses a disk that's the active placement for any
 * storage singleton/backup-config/swap, or that a live container has
 * its own storage on, exactly the same reasoning -- unmounting out from
 * under either would silently break something already relying on it.
 */
void handle_disk_unmount_post(int fd, const char *disk_name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *confirm;
	enum diskformat_error derr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	confirm = json_as_string(json_object_get(root, "confirm_disk_name"));
	if (confirm == NULL || strcmp(confirm, disk_name) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "confirm_disk_name must be given and must match the disk name in the URL");
		return;
	}
	json_free(root);

	if (is_active_storage_singleton_placement(disk_name)) {
		respond_error(fd, 409, "Conflict",
		              "this disk is the active state-storage, log-storage, or rebuildable-storage "
		              "placement, the configured backup-config disk, or the current swap placement -- "
		              "unmounting it would break whatever currently relies on it; migrate away first");
		return;
	}
	if (disk_has_container_in_use(disk_name)) {
		respond_error(fd, 409, "Conflict",
		              "one or more containers currently have their own storage on this disk -- "
		              "unmounting it would break them; migrate them away first "
		              "(POST /containers/{name}/migrate-storage)");
		return;
	}

	derr = diskformat_unmount(disk_name, CONTAINERS_DIR, g_base_dir);
	if (derr == DISKFORMAT_ERR_IS_DATA_DIR) {
		/*
		 * #249: structural, and worth saying so plainly rather than
		 * returning a bare umount2(2) failure. The data directory is
		 * chosen at control-plane assembly (cixd --data-dir=) and
		 * there is no runtime endpoint that moves it, so no sequence
		 * of migrations frees this filesystem.
		 */
		char msg[512];

		snprintf(msg, sizeof(msg),
		         "cannot unmount: this filesystem carries the daemon's own data directory "
		         "(%s) -- it is fixed at control-plane assembly (cixd --data-dir=) and no "
		         "migration frees it",
		         g_base_dir);
		respond_error(fd, 409, "Conflict", msg);
		return;
	}
	if (derr == DISKFORMAT_ERR_UMOUNT_FAILED) {
		/*
		 * umount2(2) returns EBUSY for a mountpoint that carries other
		 * filesystems, and to an operator that is indistinguishable
		 * from an open file or a running process -- the old message
		 * said only that something "may still be busy/in use", which
		 * names nothing actionable.
		 *
		 * Asked only after the real call failed, so the kernel stays
		 * the judge of whether it was possible.
		 *
		 * This used to add that it was "the ordinary case on this
		 * platform's own containers partition", because role disks
		 * mounted under <data-dir>/disks/. ADR-0231 moved them to
		 * /mnt/cix on a tmpfs, so that is no longer where they are and
		 * the containers partition no longer carries them.
		 */
		struct discovered_disk sub_disks[DISK_ENUM_MAX];
		int sub_n = disk_enumerate(sub_disks, DISK_ENUM_MAX, CONTAINERS_DIR);
		char subs[512];
		char msg[768];
		int si;
		int nsub = 0;

		subs[0] = '\0';
		for (si = 0; si < sub_n; si++) {
			if (strcmp(sub_disks[si].name, disk_name) == 0) {
				nsub = diskformat_submounts(sub_disks[si].mount_path, subs, sizeof(subs));
				break;
			}
		}
		if (nsub > 0) {
			snprintf(msg, sizeof(msg),
			         "cannot unmount: other filesystems are mounted beneath it (%s) -- "
			         "unmount those first",
			         subs);
			respond_error(fd, 409, "Conflict", msg);
			return;
		}
	}
	if (derr != DISKFORMAT_OK) {
		respond_diskformat_error(fd, derr);
		return;
	}

	{
		struct discovered_disk disks[DISK_ENUM_MAX];
		struct discovered_disk d;
		int n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
		int i;
		struct json_writer w;

		memset(&d, 0, sizeof(d));
		snprintf(d.name, sizeof(d.name), "%s", disk_name);
		for (i = 0; i < n; i++) {
			if (strcmp(disks[i].name, disk_name) == 0) {
				d = disks[i];
				break;
			}
		}
		jw_init(&w);
		disk_write_json_one(&d, &w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

void handle_disk_format_get(int fd, const char *disk_name)
{
	struct json_writer w;

	jw_init(&w);
	diskformat_write_status_json(&w, disk_name);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Partition-level disk management (ROADMAP.md task #844) -- diskpart.h's
 * own doc comment covers the design; these three handlers are thin REST
 * wrappers, the same shape every other disk-management handler above
 * already has.
 */
static void respond_diskpart_error(int fd, enum diskpart_error err)
{
	switch (err) {
	case DISKPART_ERR_INVALID_DISK_NAME:
		respond_error(fd, 400, "Bad Request", "invalid disk name");
		break;
	case DISKPART_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such disk or partition");
		break;
	case DISKPART_ERR_IS_PARTITION:
		respond_error(fd, 400, "Bad Request",
		              "this name is itself a partition -- partition-table operations target a whole disk");
		break;
	case DISKPART_ERR_NOT_A_PARTITION:
		respond_error(fd, 400, "Bad Request", "this name is a whole disk, not a partition");
		break;
	case DISKPART_ERR_WRONG_PARENT:
		respond_error(fd, 404, "Not Found", "this partition does not belong to the named disk");
		break;
	case DISKPART_ERR_IS_OS_DISK:
		/*
		 * Issue #140: this used to say the disk "can never be
		 * repartitioned", which is no longer true and was the more
		 * misleading half of the old blanket policy. Only the TABLE
		 * REWRITE is refused here -- partitions can be added, and the
		 * ones past the system set can be deleted and resized.
		 */
		respond_error(fd, 400, "Bad Request",
		              "this is the OS disk: its partition table can never be rewritten, because "
		              "that would destroy the ESP and both root slots. Adding a partition after "
		              "the existing ones is allowed, and so is managing anything past the first "
		              "four");
		break;
	case DISKPART_ERR_PROTECTED_PARTITION:
		respond_error(fd, 400, "Bad Request",
		              "this is one of the four partitions this machine boots from (ESP, both "
		              "root slots, /config) -- destroying or resizing it would leave the host "
		              "unbootable with no remote recovery. Partitions after them on the same "
		              "disk are ordinary and can be managed freely");
		break;
	case DISKPART_ERR_HAS_ROLE:
		respond_error(fd, 409, "Conflict",
		              "this disk or partition already has a role assigned -- remove it first "
		              "(DELETE /v1/storage-roles/{name})");
		break;
	case DISKPART_ERR_MOUNTED:
		respond_error(fd, 409, "Conflict", "this disk or partition is currently mounted");
		break;
	case DISKPART_ERR_INVALID_PART_NAME:
		respond_error(fd, 400, "Bad Request", "invalid partition name");
		break;
	case DISKPART_ERR_INVALID_SIZE:
		respond_error(fd, 400, "Bad Request", "invalid size_mib");
		break;
	case DISKPART_ERR_SHRINK_REFUSED:
		respond_error(fd, 400, "Bad Request",
		              "a partition can only be grown, never shrunk -- shrinking requires the "
		              "filesystem inside it to be shrunk first, and cutting the table entry "
		              "before that destroys the tail of a live filesystem");
		break;
	case DISKPART_ERR_NO_ROOM_AFTER:
		respond_error(fd, 409, "Conflict",
		              "there is not enough free space immediately after this partition -- free "
		              "space elsewhere on the disk cannot extend it (see GET "
		              "/v1/storage/{name}/free-space)");
		break;
	case DISKPART_ERR_FS_UNSUPPORTED:
		respond_error(fd, 400, "Bad Request",
		              "only an ext4, btrfs or unformatted partition can be grown");
		break;
	case DISKPART_ERR_KERNEL_SIZE_STALE:
		respond_error(fd, 409, "Conflict",
		              "the partition table was grown but the kernel is still reporting the old "
		              "size, so the filesystem was NOT grown -- growing it now would silently "
		              "resize it to the old bound. Something else on this disk is in use; "
		              "unmount it and retry, or reboot to re-read the table");
		break;
	case DISKPART_ERR_FS_UNCLEAN:
		respond_error(fd, 409, "Conflict",
		              "the filesystem needs a check that cannot be made automatically; the "
		              "partition table was NOT changed. Check it by hand before retrying");
		break;
	case DISKPART_ERR_RESIZE_FS_FAILED: {
		/* Say WHY, the same way DISKPART_ERR_SFDISK_FAILED below does.
		 * Without this every distinct cause -- a mount that failed, a
		 * missing shared library, a filesystem the tool refused --
		 * reached the operator as one identical sentence. */
		const char *why = diskpart_last_tool_error();
		char msg[700];

		snprintf(msg, sizeof(msg),
		         "the partition grew but the filesystem inside it could not be grown to match "
		         "-- the extra space is real but not yet usable%s%s",
		         (why != NULL && why[0] != '\0') ? ": " : "; run resize2fs (ext4) or `btrfs "
		                                                   "filesystem resize max` by hand",
		         (why != NULL && why[0] != '\0') ? why : "");
		logstore_write("cixd", "error", "diskpart: %s", msg);
		respond_error(fd, 500, "Internal Server Error", msg);
		break;
	}
	case DISKPART_ERR_SFDISK_MISSING:
		respond_error(fd, 500, "Internal Server Error",
		              "sfdisk is not installed on this host -- partitioning needs "
		              "/usr/sbin/sfdisk, which this control-plane image does not carry. "
		              "Update to an image built after this was fixed.");
		break;
	case DISKPART_ERR_SFDISK_FAILED: {
		/*
		 * Report what the tool ACTUALLY said, not a guess at it.
		 *
		 * This used to offer a fixed list of things that might be
		 * wrong ("may already have the layout, or not enough free
		 * space"). Found the hard way while first appending to a live
		 * OS disk: the real reason was on sfdisk's own stderr and went
		 * nowhere, and the guess sent the reader looking at free space
		 * that was demonstrably there. A guess presented as a
		 * diagnosis is worse than none, because it is confidently
		 * wrong about its own cause -- the same lesson #125/#132/#172
		 * already record for other paths.
		 */
		const char *why = diskpart_last_tool_error();
		char msg[640];

		if (why != NULL && why[0] != '\0')
			snprintf(msg, sizeof(msg), "sfdisk rejected the request: %s", why);
		else
			snprintf(msg, sizeof(msg),
			         "sfdisk rejected the request and said nothing about why");
		logstore_write("cixd", "error", "diskpart: %s", msg);
		respond_error(fd, 500, "Internal Server Error", msg);
		break;
	}
	default:
		respond_error(fd, 500, "Internal Server Error",
		              "the partitioning tool could not be run");
		break;
	}
}

/*
 * GET /v1/storage/{name}/free-space (issue #95) -- how much room is
 * actually left in this disk's partition table, from sfdisk rather than
 * from subtracting the partition sizes a client can already see. That
 * subtraction is wrong in three ways it cannot detect: partition
 * alignment, the GPT's own reserved areas at both ends, and any gap an
 * earlier delete left in the middle.
 *
 * Its own endpoint rather than a field on GET /disks, because it forks
 * a subprocess and GET /disks runs on every poll for every disk.
 */
void handle_disk_free_space(int fd, const char *disk_name)
{
	struct diskpart_free_space fs;
	enum diskpart_error derr;
	struct json_writer w;
	int i;

	derr = diskpart_free_space(disk_name, CONTAINERS_DIR, &fs);
	if (derr != DISKPART_OK) {
		respond_diskpart_error(fd, derr);
		return;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "disk_name");
	jw_str(&w, disk_name);
	jw_key(&w, "has_partition_table");
	jw_bool(&w, fs.has_table);
	jw_key(&w, "total_free_bytes");
	jw_int(&w, (long long)fs.total_free_bytes);
	/*
	 * The largest single extent, which is the number that actually
	 * bounds a new partition -- total free space can be spread across
	 * gaps no one request can use.
	 */
	jw_key(&w, "largest_free_bytes");
	jw_int(&w, (long long)(fs.largest_free_sectors * fs.sector_bytes));
	jw_key(&w, "largest_free_mib");
	jw_int(&w, (long long)(fs.largest_free_sectors * fs.sector_bytes / (1024 * 1024)));
	jw_key(&w, "extents");
	jw_arr_open(&w);
	for (i = 0; i < fs.extent_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "start_sector");
		jw_int(&w, (long long)fs.extents[i].start_sector);
		jw_key(&w, "sectors");
		jw_int(&w, (long long)fs.extents[i].sectors);
		jw_key(&w, "bytes");
		jw_int(&w, (long long)(fs.extents[i].sectors * fs.sector_bytes));
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_disk_partition_table_post(int fd, const char *disk_name, const char *body,
                                              size_t body_len)
{
	struct json_value *root;
	const char *confirm;
	enum diskpart_error derr;
	struct discovered_disk d;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	confirm = json_as_string(json_object_get(root, "confirm_disk_name"));
	if (confirm == NULL || strcmp(confirm, disk_name) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "confirm_disk_name must be given and must match the disk name in the URL -- "
		              "this is a destructive operation");
		return;
	}
	json_free(root);

	derr = diskpart_create_table(disk_name, CONTAINERS_DIR);
	if (derr != DISKPART_OK) {
		respond_diskpart_error(fd, derr);
		return;
	}

	{
		struct discovered_disk disks[DISK_ENUM_MAX];
		int n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
		int i;
		struct json_writer w;

		for (i = 0; i < n; i++) {
			if (strcmp(disks[i].name, disk_name) == 0) {
				d = disks[i];
				break;
			}
		}
		jw_init(&w);
		disk_write_json_one(&d, &w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

void handle_disk_partitions_post(int fd, const char *disk_name, const char *body,
                                         size_t body_len)
{
	struct json_value *root;
	const char *part_name;
	const struct json_value *jsize;
	unsigned long long size_mib = 0;
	enum diskpart_error derr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	part_name = json_as_string(json_object_get(root, "name"));
	if (part_name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name is required");
		return;
	}
	jsize = json_object_get(root, "size_mib");
	if (jsize != NULL) {
		if (jsize->type != JSON_NUMBER || json_as_number(jsize) < 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "size_mib must be a non-negative number");
			return;
		}
		size_mib = (unsigned long long)json_as_number(jsize);
	}

	{
		char part_name_buf[DISKPART_NAME_MAX];

		snprintf(part_name_buf, sizeof(part_name_buf), "%s", part_name);
		json_free(root);

		derr = diskpart_add(disk_name, CONTAINERS_DIR, part_name_buf, size_mib);
		if (derr != DISKPART_OK) {
			respond_diskpart_error(fd, derr);
			return;
		}
	}

	{
		struct discovered_disk disks[DISK_ENUM_MAX];
		int n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
		int i;
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "disk_name");
		jw_str(&w, disk_name);
		jw_key(&w, "partitions");
		jw_arr_open(&w);
		for (i = 0; i < n; i++) {
			if (disks[i].is_partition && strcmp(disks[i].parent_disk, disk_name) == 0)
				disk_write_json_one(&disks[i], &w);
		}
		jw_arr_close(&w);
		jw_obj_close(&w);
		respond_json(fd, 201, "Created", &w);
		jw_free(&w);
	}
}

/*
 * POST /v1/storage/{disk}/partitions/{partition}/resize (issue #94).
 *
 * Grow only, and both halves of the job: the table entry, then the
 * filesystem inside it. Growing only the entry would leave the extra
 * space invisible to everything using the filesystem, which reads as
 * the resize having silently done nothing.
 */
void handle_disk_partition_resize(int fd, const char *disk_name, const char *partition_name,
                                          const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *jsize;
	unsigned long long size_mib = 0;
	enum diskpart_error derr;

	root = json_parse(body, body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jsize = json_object_get(root, "size_mib");
	if (jsize != NULL) {
		if (jsize->type != JSON_NUMBER || jsize->u.number < 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "size_mib must be a non-negative number");
			return;
		}
		size_mib = (unsigned long long)jsize->u.number;
	}
	json_free(root);

	derr = diskpart_resize(disk_name, partition_name, CONTAINERS_DIR, size_mib);
	if (derr != DISKPART_OK) {
		respond_diskpart_error(fd, derr);
		return;
	}
	{
		struct discovered_disk disks[DISK_ENUM_MAX];
		int n = disk_enumerate(disks, DISK_ENUM_MAX, CONTAINERS_DIR);
		int i;
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "disk_name");
		jw_str(&w, disk_name);
		jw_key(&w, "partitions");
		jw_arr_open(&w);
		for (i = 0; i < n; i++) {
			if (disks[i].is_partition && strcmp(disks[i].parent_disk, disk_name) == 0)
				disk_write_json_one(&disks[i], &w);
		}
		jw_arr_close(&w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

void handle_disk_partition_delete(int fd, const char *disk_name, const char *partition_name)
{
	enum diskpart_error derr = diskpart_delete(disk_name, partition_name, CONTAINERS_DIR);

	if (derr != DISKPART_OK) {
		respond_diskpart_error(fd, derr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * ADR-0141 Phase 2/3: GET/POST /v1/system/{rebuildable,log}-storage(/migrate).
 * rebuildable-storage reuses the identical storagemigrate.c/
 * storageplacement.c machinery, just not exposed here yet (Phase 4).
 */
