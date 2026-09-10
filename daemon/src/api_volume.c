#include "api_volume.h"

#include "apiresp.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "logstore.h"
#include "volume.h"
#include "volumebackup.h"
#include "btrfs.h"
#include "container.h"
#include "quotamap.h"
#include "registry.h"
#include "containerdef.h"
#include "storageplacement.h"
#include "diskrole.h"
#include "persist.h"

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ---- Issue #88: persistent volumes ---- */

static void respond_volume_error(int fd, enum volume_error e)
{
	switch (e) {
	case VOLUME_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request",
		              "invalid volume name -- letters, digits, '_' and '-' only, not starting "
		              "with '.' or '-'");
		return;
	case VOLUME_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a volume with this name already exists");
		return;
	case VOLUME_ERR_FULL:
		respond_error(fd, 507, "Insufficient Storage", "volume table is full");
		return;
	case VOLUME_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	case VOLUME_ERR_IN_USE:
		respond_error(fd, 409, "Conflict",
		              "this volume is still referenced by a container definition -- delete or "
		              "edit that container first");
		return;
	case VOLUME_ERR_IO:
		respond_error(fd, 500, "Internal Server Error", "could not create the volume directory");
		return;
	case VOLUME_ERR_TARGET_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such disk or partition to migrate onto");
		return;
	case VOLUME_ERR_TARGET_NOT_READY:
		respond_error(fd, 409, "Conflict",
		              "that disk is not mounted -- give it a role and format it first");
		return;
	case VOLUME_ERR_SAME_PLACE:
		respond_error(fd, 409, "Conflict", "this volume is already there");
		return;
	case VOLUME_ERR_IN_USE_RUNNING:
		respond_error(fd, 409, "Conflict",
		              "a container mounting this volume is running -- a bind mount resolves to a "
		              "host path when the container starts, so moving the data underneath it "
		              "would leave it writing to the old location. Stop the container first");
		return;
	case VOLUME_ERR_COPY_FAILED:
		respond_error(fd, 500, "Internal Server Error",
		              "could not copy the volume's data to the new location -- nothing was moved "
		              "and the volume still points at its original data");
		return;
	default:
		respond_error(fd, 500, "Internal Server Error", "could not persist the volume");
		return;
	}
}

void handle_volume_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "volumes");
	volume_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_volume_get(int fd, const char *name)
{
	struct volume *v = volume_find(name);
	struct json_writer w;

	if (v == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	jw_init(&w);
	volume_write_json_one(v, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_volume_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const char *name, *disk;
	struct volume *v = NULL;
	enum volume_error verr;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	name = json_as_string(json_object_get(root, "name"));
	disk = json_as_string(json_object_get(root, "disk"));
	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name is required");
		return;
	}
	{
		/*
		 * Issue #102: owner at creation, because the moment a volume
		 * exists is the only moment its contents are certainly empty
		 * -- setting it later means deciding what to do about files
		 * that are already there, which is a question worth not having
		 * to ask.
		 */
		const struct json_value *ju = json_object_get(root, "owner_uid");
		const struct json_value *jg = json_object_get(root, "owner_gid");
		int uid = ju != NULL ? (int)json_as_number(ju) : -1;
		int gid = jg != NULL ? (int)json_as_number(jg) : -1;

		if ((ju != NULL) != (jg != NULL)) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "owner_uid and owner_gid go together -- a volume owned by one user and "
			              "an unrelated group is almost always a typo");
			return;
		}
		if (ju != NULL && (uid < 0 || gid < 0)) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "owner_uid/owner_gid must not be negative");
			return;
		}

		verr = volume_create(name, disk, &v);
		if (verr != VOLUME_OK) {
			json_free(root);
			respond_volume_error(fd, verr);
			return;
		}
		if (ju != NULL) {
			verr = volume_set_owner(name, uid, gid);
			if (verr == VOLUME_OK)
				verr = volume_apply_owner(v, 0);
			if (verr != VOLUME_OK) {
				json_free(root);
				/* The directory exists and is root-owned: report the
				 * real outcome rather than a 201 that implies an owner
				 * that was never applied. */
				respond_volume_error(fd, verr);
				return;
			}
		}
		/*
		 * Freed only now. `name` points INTO this tree, and every call
		 * above takes it -- freeing before them left volume_set_owner()
		 * looking up a name in freed memory and answering "no such
		 * volume" for one it had just created.
		 */
		json_free(root);
	}
	jw_init(&w);
	volume_write_json_one(v, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

/*
 * Issue #88: a volume outlives containers by design, so deleting one is
 * refused while any container DEFINITION still names it -- not merely
 * while one is running. A stopped container whose definition references
 * the volume will come back and expect its data, and silently deleting
 * it underneath would be a data-loss bug that only surfaces later.
 */
/*
 * Does this container's definition mount that volume. Split out of
 * volume_referenced_by_container() below, which answers "is anything
 * using it" and stops at the first match -- pausing needs every one.
 */
static int container_mounts_volume(const char *container_name, const char *volume_name)
{
	struct container_def *def = containerdef_find(container_name);
	struct json_value *root;
	const struct json_value *jvols;
	size_t k;
	int found = 0;

	if (def == NULL || def->body == NULL)
		return 0;
	root = json_parse(def->body, def->body_len);
	if (root == NULL)
		return 0;
	jvols = json_object_get(root, "volumes");
	if (jvols != NULL && jvols->type == JSON_ARRAY) {
		for (k = 0; k < jvols->u.array.count && !found; k++) {
			const char *n = json_as_string(json_object_get(jvols->u.array.items[k], "name"));

			if (n != NULL && strcmp(n, volume_name) == 0)
				found = 1;
		}
	}
	json_free(root);
	return found;
}

static int volume_referenced_by_container(const char *volume_name, char *out_container,
                                           size_t out_size)
{
	char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int count = containerdef_resolve_order(order);
	int i;

	for (i = 0; i < count; i++) {
		struct container_def *def = containerdef_find(order[i]);
		struct json_value *root;
		const struct json_value *jvols;
		size_t k;

		if (def == NULL)
			continue;
		root = json_parse(def->body, def->body_len);
		if (root == NULL)
			continue;
		jvols = json_object_get(root, "volumes");
		if (jvols != NULL && jvols->type == JSON_ARRAY) {
			for (k = 0; k < jvols->u.array.count; k++) {
				const char *n = json_as_string(json_object_get(jvols->u.array.items[k], "name"));

				if (n != NULL && strcmp(n, volume_name) == 0) {
					snprintf(out_container, out_size, "%s", order[i]);
					json_free(root);
					return 1;
				}
			}
		}
		json_free(root);
	}
	return 0;
}

/*
 * Issue #96: does any container mounting this volume have a live
 * process right now.
 *
 * Injected into volumebackup.c rather than looked up there, so that
 * module needs no knowledge of the registry or of container
 * definitions -- the same shape containerdef.c already gets its own
 * liveness predicate through.
 *
 * registry_find() alone is NOT the test: since ADR-0181 the registry
 * also holds exited containers, so a container that ran once and
 * stopped would block every snapshot forever.
 */
static int volume_has_running_container(const char *volume_name)
{
	char user[REGISTRY_NAME_MAX];
	struct registry_entry *e;

	if (!volume_referenced_by_container(volume_name, user, sizeof(user)))
		return 0;
	e = registry_find(user);
	return e != NULL && e->running;
}

/*
 * Issue #93: apply a volume's size limit to its real directory.
 *
 * Two operations, the same pair a container overlay already uses: tag
 * the directory with a project id so everything inside it counts, and
 * set that project's byte limit via quotactl(2). Reusing the overlay's
 * own tagging call rather than repeating the ioctls here -- a second
 * implementation of something that must stay identical, on a path where
 * getting it subtly wrong yields a quota that reports as applied and is
 * not.
 *
 * On btrfs the mechanism is different and so is the call: the limit
 * lives on a QGROUP attached to a subvolume, set with the same
 * cix_btrfs_qgroup_limit_excl() a container's own rootfs quota already
 * uses. Volumes are created as subvolumes (#364), and one predating
 * that is converted by the caller before reaching here.
 *
 * This used to refuse btrfs outright, which was right at the time --
 * accepting a limit and not enforcing it is the "false promise"
 * overlay.c's own comment warns about -- but it meant no volume on this
 * platform's DEFAULT substrate could be bounded at all. A plain
 * directory is still refused rather than silently unenforced, because
 * that failure has not stopped being a false promise.
 *
 * quota_bytes of 0 clears the limit.
 */
static int volume_apply_quota(const char *volume_name, const char *dir, long long quota_bytes,
                              char *err, size_t err_size)
{
	uint32_t projid;

	if (overlay_backing_is_btrfs(dir)) {
		if (!cix_btrfs_is_subvolume(dir)) {
			snprintf(err, err_size,
			         "this volume is a plain directory on btrfs, where a size limit needs a "
			         "subvolume -- it could not be converted, so the limit is refused rather "
			         "than accepted and not enforced");
			return -1;
		}
		/* Bytes, and 0 legitimately means "no limit" -- the same
		 * clearing convention the project-quota path below uses. */
		if (cix_btrfs_qgroup_limit_excl(dir, (unsigned long long)quota_bytes) != 0) {
			snprintf(err, err_size,
			         "could not set the qgroup limit (%s) -- the filesystem may not have quotas "
			         "enabled (btrfs quota enable)",
			         strerror(errno));
			return -1;
		}
		return 0;
	}
	/* A distinct project id per volume, from the same allocator
	 * containers use -- ids are never reused, so a deleted volume's id
	 * can never silently start limiting a new one. */
	if (quotamap_get_or_assign(volume_name, &projid) != 0) {
		snprintf(err, err_size, "could not assign a quota project id");
		return -1;
	}
	if (quotamap_apply(dir, projid, quota_bytes) != 0) {
		snprintf(err, err_size,
		         "could not set the limit (%s) -- the filesystem holding this volume may not have "
		         "project quotas enabled",
		         strerror(errno));
		return -1;
	}
	/* Tagging after the limit is set, matching the container path's own
	 * ordering: the limit is already in force by the moment any file
	 * can carry this project id. */
	if (quota_bytes > 0 && overlay_tag_project_id(dir, projid, "volume") != 0) {
		snprintf(err, err_size, "could not tag the volume directory with its quota project id");
		return -1;
	}
	return 0;
}

/*
 * POST /v1/volumes/{name}/migrate -- move a volume's data to another
 * disk or partition, then repoint it.
 *
 * Refused while any container mounting this volume is RUNNING. A bind
 * mount resolves to a host path once, when the container starts
 * (ADR-0183), so moving the data underneath a live container would
 * leave it writing to the old location with nothing to indicate
 * anything had changed -- a silent split-brain rather than an error. A
 * stopped container is fine: it picks up the new location on its next
 * start, like any other definition change.
 */
void handle_volume_migrate(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *target;
	char user[REGISTRY_NAME_MAX];
	enum volume_error verr;

	if (volume_find(name) == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	target = json_as_string(json_object_get(root, "disk"));
	if (target == NULL)
		target = ""; /* omitted means the default OS-disk placement */

	/*
	 * Only a genuinely RUNNING container blocks this. registry_find()
	 * alone is not the test -- since ADR-0181 the registry also holds
	 * exited containers, so using it would refuse a migrate because
	 * something that ran once and stopped still has an entry. An exited
	 * or stopped container picks up the new location on its next start,
	 * exactly like any other definition change; only a live process has
	 * a bind mount already resolved to the old path.
	 */
	{
		struct registry_entry *e = NULL;

		if (volume_referenced_by_container(name, user, sizeof(user)))
			e = registry_find(user);
		if (e != NULL && e->running) {
			json_free(root);
			respond_volume_error(fd, VOLUME_ERR_IN_USE_RUNNING);
			return;
		}
	}

	verr = volume_migrate(name, target);
	json_free(root);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	{
		/*
		 * Issue #93: a project-quota tag lives on the directory, so a
		 * volume that moved to another filesystem arrives untagged and
		 * unlimited. Re-applied here, or the migrate would silently
		 * drop a limit the operator still believes is in force -- which
		 * is worse than never having set one.
		 */
		struct volume *moved = volume_find(name);
		char path[PATH_MAX];
		char qerr[256];

		if (moved != NULL && moved->quota_bytes > 0 &&
		    volume_host_path(moved, path, sizeof(path)) == 0 &&
		    volume_apply_quota(name, path, moved->quota_bytes, qerr, sizeof(qerr)) != 0) {
			logstore_write("volume", "error",
			               "volume %s moved, but its size limit could not be re-applied at the new "
			               "location: %s",
			               name, qerr);
		}
	}
	handle_volume_get(fd, name);
}

/*
 * PUT /v1/volumes/{name}/owner (issue #102) -- who may write to this
 * volume.
 *
 * A volume is a directory the daemon creates as root, and nothing could
 * change that: a workload not running as root could not write to its
 * own volume. Found the plain way, by an operator logging into the jump
 * box and finding their home directory -- a volume -- owned by root.
 *
 * `recursive` is the caller's explicit choice and defaults to false. A
 * volume that has been in use holds files whose ownership someone may
 * have set deliberately, and rewriting all of it because the top-level
 * owner changed is a quiet kind of data loss.
 */
void handle_volume_owner_put(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const struct json_value *ju, *jg, *jr;
	struct volume *v;
	enum volume_error verr;
	int uid, gid, recursive;
	struct json_writer w;

	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	v = volume_find(name);
	if (v == NULL) {
		json_free(root);
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	ju = json_object_get(root, "uid");
	jg = json_object_get(root, "gid");
	jr = json_object_get(root, "recursive");
	recursive = (jr != NULL && jr->type == JSON_BOOL && jr->u.boolean);

	if (ju == NULL || jg == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "uid and gid are both required -- pass the value it already has to leave "
		              "one alone, or null for both to hand the volume back to root");
		return;
	}
	if (ju->type == JSON_NULL && jg->type == JSON_NULL) {
		uid = -1;
		gid = -1;
	} else {
		uid = (int)json_as_number(ju);
		gid = (int)json_as_number(jg);
		if (uid < 0 || gid < 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "uid/gid must not be negative");
			return;
		}
	}
	json_free(root);

	verr = volume_set_owner(name, uid, gid);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	v = volume_find(name);
	/* Clearing back to root is a real request too: chown it to 0:0
	 * rather than leaving whoever owned it last still owning it. */
	if (uid < 0) {
		char path[PATH_MAX];

		if (volume_host_path(v, path, sizeof(path)) == 0 && lchown(path, 0, 0) != 0) {
			respond_error(fd, 500, "Internal Server Error",
			              "could not hand the volume directory back to root");
			return;
		}
	} else {
		verr = volume_apply_owner(v, recursive);
		if (verr != VOLUME_OK) {
			respond_volume_error(fd, verr);
			return;
		}
	}

	jw_init(&w);
	volume_write_json_one(v, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * PUT /v1/volumes/{name}/quota -- set or clear a volume's size limit.
 */
/*
 * GET /v1/volumes/{name}/usage (#365) -- how much the volume holds.
 *
 * The one question a volume's record could not answer: it describes how
 * the volume is CONFIGURED and says nothing about its contents, and
 * `df` inside a container cannot fill the gap because a volume is a
 * bind of a directory and statfs(2) reports the superblock -- so `df`
 * shows the whole backing filesystem, as it does for a container rootfs
 * that carries a real qgroup limit. This is the only place the number
 * exists.
 *
 * TWO SOURCES, AND THE ANSWER SAYS WHICH ONE IT CAME FROM. A btrfs
 * subvolume with quotas enabled already has its size counted by the
 * kernel, so cix_btrfs_qgroup_query() reads it for the price of two
 * tree lookups and `source` reports "qgroup". Anything else -- a plain
 * directory, a non-btrfs volume, or a subvolume on a filesystem where
 * quotas were never enabled -- falls back to walking the tree, and
 * `source` reports "walk". This is the same dispatch-on-filesystem this
 * file already does for quotas, not a second implementation: each
 * branch is the one way to get the number on that filesystem.
 *
 * The two numbers do NOT mean the same thing, which is why the caller
 * is told. The walk sums apparent file content; the qgroup counts
 * EXCLUSIVE ALLOCATED extents, so sparse, compressed and reflinked data
 * read smaller, and it settles at transaction commit (~30 s) where the
 * walk is instant. The qgroup figure is nonetheless the better one for
 * a volume carrying a limit, because it is exactly what that limit is
 * enforced against -- a walk can report a volume comfortably under a
 * quota the kernel is about to refuse a write on.
 *
 * overlay_upperdir_size() rather than a second walker on the fallback
 * path: a container's own disk.upper_bytes is measured with it, and two
 * implementations of "how big is this tree" would be two definitions of
 * what counts.
 *
 * Its own comment notes the walk is synchronous and non-reentrant,
 * which is why this is a SEPARATE endpoint and not a field on GET
 * /volumes: the list is what the dashboard renders, and a field there
 * would walk every volume on every render. The qgroup path has no such
 * cost, but the endpoint stays separate rather than becoming fast for
 * some volumes and slow for others in a way a caller cannot predict.
 */
void handle_volume_usage(int fd, const char *name)
{
	struct volume *v = volume_find(name);
	char path[PATH_MAX];
	long long bytes = 0;
	unsigned long long used = 0, kernel_limit = 0;
	enum cix_btrfs_qgroup_state qstate = CIX_BTRFS_QGROUP_OK;
	const char *source = "walk";
	struct json_writer w;

	if (v == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	if (volume_host_path(v, path, sizeof(path)) != 0) {
		respond_error(fd, 500, "Internal Server Error", "could not resolve the volume's own path");
		return;
	}
	/* ENODATA (quotas on, this subvolume not yet accounted) and ENOENT
	 * (quotas never enabled) are ordinary states, not failures: the
	 * volume still has a real size and the walk still knows it. */
	if (cix_btrfs_qgroup_query(path, &used, &kernel_limit, &qstate) == 0) {
		source = "qgroup";
		bytes = (long long)used;
	} else if (overlay_upperdir_size(path, &bytes) != 0) {
		char err[256];

		snprintf(err, sizeof(err), "could not measure %s: %s", path, strerror(errno));
		respond_error(fd, 500, "Internal Server Error", err);
		return;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, v->name);
	jw_key(&w, "bytes");
	jw_int(&w, bytes);
	jw_key(&w, "source");
	jw_str(&w, source);
	/* Echoed so a caller has both halves of "how full is this" without
	 * a second request for the volume's own record. */
	jw_key(&w, "quota_bytes");
	jw_int(&w, v->quota_bytes);
	if (strcmp(source, "qgroup") == 0) {
		/* The limit the KERNEL is actually enforcing, which is not
		 * necessarily quota_bytes above: that one is this daemon's own
		 * record of what was asked for. They should agree, and a
		 * caller that can see both can notice when they do not. */
		jw_key(&w, "limit_bytes");
		jw_int(&w, (long long)kernel_limit);
		/* "stale" means accounting is mid-rescan or flagged
		 * inconsistent, so `bytes` may read low; "simple" means squota,
		 * where exclusive bytes are attributed on a different rule. */
		jw_key(&w, "accounting");
		jw_str(&w, qstate == CIX_BTRFS_QGROUP_SIMPLE ? "simple"
		            : qstate == CIX_BTRFS_QGROUP_STALE ? "stale" : "ok");
	}
	jw_key(&w, "measured_at");
	jw_int(&w, (long long)time(NULL));
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_volume_quota_put(int fd, const char *name, const char *body, size_t body_len)
{
	struct volume *v = volume_find(name);
	struct json_value *root;
	const struct json_value *jq;
	long long bytes;
	char path[PATH_MAX];
	char err[256];
	enum volume_error verr;

	if (v == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	root = json_parse(body, body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jq = json_object_get(root, "quota_bytes");
	if (jq == NULL || jq->type != JSON_NUMBER || json_as_number(jq) < 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "quota_bytes must be a non-negative number (0 clears it)");
		return;
	}
	bytes = (long long)json_as_number(jq);
	json_free(root);

	if (volume_host_path(v, path, sizeof(path)) != 0) {
		respond_error(fd, 500, "Internal Server Error", "could not resolve the volume's own path");
		return;
	}

	/*
	 * #364: a btrfs qgroup attaches to a SUBVOLUME, and a volume created
	 * before that was an ordinary directory. Converting it is what makes
	 * the operator's request satisfiable at all, so it happens here
	 * rather than being reported back as homework -- volumes created
	 * since are already subvolumes and this is a no-op for them.
	 *
	 * Refused while a container mounting the volume is RUNNING, and the
	 * test is the one handle_volume_migrate() already uses, for the same
	 * reason: a bind mount resolves once at container start (ADR-0183)
	 * and pins that directory's inode, so a live container would go on
	 * writing into the tree the conversion replaces -- a silent
	 * split-brain rather than an error. registry_find() alone is not the
	 * test, because since ADR-0181 the registry also holds exited
	 * containers; only a live process has a bind mount already resolved.
	 *
	 * Clearing a limit (0) needs no subvolume, so it is never blocked by
	 * this -- an operator must always be able to remove a limit.
	 */
	if (bytes > 0 && cix_btrfs_is_backing(path) && !cix_btrfs_is_subvolume(path)) {
		struct registry_entry *e = NULL;
		char user[REGISTRY_NAME_MAX];
		enum volume_error cerr;

		if (volume_referenced_by_container(name, user, sizeof(user)))
			e = registry_find(user);
		if (e != NULL && e->running) {
			respond_volume_error(fd, VOLUME_ERR_IN_USE_RUNNING);
			return;
		}
		cerr = volume_convert_to_subvolume(name);
		if (cerr != VOLUME_OK) {
			respond_volume_error(fd, cerr);
			return;
		}
		logstore_write("volume", "info",
		               "volume %s converted from a directory to a btrfs subvolume so a size limit "
		               "can attach to it (#364)",
		               name);
	}

	if (volume_apply_quota(name, path, bytes, err, sizeof(err)) != 0) {
		respond_error(fd, 409, "Conflict", err);
		return;
	}
	verr = volume_set_quota(name, bytes);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	handle_volume_get(fd, name);
}

/* ---- Issue #96: volume content snapshots ---- */
/*
 * Issue #96: freeze (or thaw) every running container mounting this
 * volume. Returns how many were acted on, or -1 if any failed.
 *
 * ALL of them, not just one. Any container with the volume mounted
 * could be writing to it, so freezing a subset would leave the copy
 * exposed to the rest -- and a snapshot that is only mostly quiesced is
 * a crash-consistent one wearing a consistent one's label.
 *
 * The freeze is the cgroup freezer (ADR-0045), not SIGSTOP: a process
 * can neither ignore nor handle it, which is what makes the resulting
 * snapshot genuinely consistent rather than merely likely to be.
 *
 * On a partial failure the ones already frozen are thawed again before
 * returning, so a failure to quiesce never leaves containers stopped.
 */
static int volume_set_containers_paused(const char *volume_name, int freeze)
{
	char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int count = containerdef_resolve_order(order);
	int i, acted = 0, failed = 0;

	for (i = 0; i < count; i++) {
		struct registry_entry *e;

		if (!container_mounts_volume(order[i], volume_name))
			continue;
		e = registry_find(order[i]);
		if (e == NULL || !e->running)
			continue;
		if (freeze && e->paused)
			continue; /* already frozen by someone else -- leave it alone */
		if (registry_set_paused(e, freeze) != 0) {
			failed = 1;
			break;
		}
		acted++;
	}
	if (failed && freeze) {
		/* Undo what this call managed before giving up. */
		for (i = 0; i < count; i++) {
			struct registry_entry *e = registry_find(order[i]);

			if (e != NULL && e->running && e->paused && container_mounts_volume(order[i], volume_name))
				registry_set_paused(e, 0);
		}
		return -1;
	}
	return acted;
}

static const struct volumebackup_hooks g_volumebackup_hooks = {
	volume_has_running_container,
	volume_set_containers_paused,
};

/*
 * The scheduled sweep runs from a timerfd handler, which stays in
 * main.c with the event loop (ADR-0249), while the hooks it needs are
 * these two volume-local functions. An accessor rather than an exported
 * global: the hooks are this module's business and the sweep only needs
 * to be handed them.
 */
const struct volumebackup_hooks *api_volume_backup_hooks(void)
{
	return &g_volumebackup_hooks;
}



static void respond_volumebackup_error(int fd, enum volumebackup_error e)
{
	switch (e) {
	case VOLUMEBACKUP_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such volume");
		break;
	case VOLUMEBACKUP_ERR_NO_SUCH_SNAPSHOT:
		respond_error(fd, 404, "Not Found", "no such snapshot");
		break;
	case VOLUMEBACKUP_ERR_NO_DISK:
		respond_error(fd, 409, "Conflict",
		              "no backup disk configured -- set one via PUT /v1/system/volume-backup-config "
		              "(it must be a disk or partition carrying the 'backup' role)");
		break;
	case VOLUMEBACKUP_ERR_DISK_NOT_READY:
		respond_error(fd, 409, "Conflict",
		              "the configured backup disk is not mounted -- format and mount it first");
		break;
	case VOLUMEBACKUP_ERR_IN_USE_RUNNING:
		respond_error(fd, 409, "Conflict",
		              "a container mounting this volume is running -- copying its data now would "
		              "capture a half-written state, which looks exactly like a good snapshot "
		              "until someone restores it. Stop the container first");
		break;
	case VOLUMEBACKUP_ERR_COPY_FAILED:
		respond_error(fd, 500, "Internal Server Error", "copying the volume's data failed");
		break;
	case VOLUMEBACKUP_ERR_INVALID:
		respond_error(fd, 400, "Bad Request", "invalid request");
		break;
	default:
		respond_error(fd, 500, "Internal Server Error", "could not persist the backup config");
		break;
	}
}

void handle_volume_backup_config_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	volumebackup_write_config_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_volume_backup_config_put(int fd, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const struct json_value *jen;
	const char *disk;
	enum volumebackup_error verr;

	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	/*
	 * ADR-0257: interval_hours is gone. WHEN the sweep runs is a
	 * schedule; this says only where snapshots go and whether the
	 * sweep is on. Refused rather than ignored, so a client still
	 * sending it learns that its schedule is not being set.
	 */
	if (json_object_get(root, "interval_hours") != NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		               "interval_hours moved to /v1/schedules (ADR-0257) -- set a schedule "
		               "with action \"volume.backup\" instead");
		return;
	}
	disk = json_as_string(json_object_get(root, "disk"));
	jen = json_object_get(root, "enabled");
	verr = volumebackup_set(disk != NULL ? disk : "",
	                        jen != NULL && jen->type == JSON_BOOL && jen->u.boolean);
	json_free(root);
	if (verr != VOLUMEBACKUP_OK) {
		respond_volumebackup_error(fd, verr);
		return;
	}
	handle_volume_backup_config_get(fd);
}

/* The volume's own policy plus its snapshots and last attempt, in one
 * response -- they are always wanted together and splitting them across
 * three calls would be three round trips to render one panel. */
void handle_volume_backups_get(int fd, const char *name)
{
	struct volume *v = volume_find(name);
	struct json_writer w;

	if (v == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "volume");
	jw_str(&w, name);
	jw_key(&w, "enabled");
	jw_bool(&w, v->backup_enabled);
	jw_key(&w, "retain");
	jw_int(&w, v->backup_retain > 0 ? v->backup_retain : VOLUMEBACKUP_DEFAULT_RETAIN);
	/* Whether this volume can be snapshotted while a container using it
	 * is running -- without it, an always-on service's volume is never
	 * backed up at all, which is the state most worth surfacing. */
	jw_key(&w, "while_running");
	jw_str(&w, volume_running_mode_name(v->backup_while_running));
	jw_key(&w, "last_backup_at");
	jw_int(&w, (long long)v->backup_last_at);
	jw_key(&w, "snapshots");
	volumebackup_write_list_json(&w, name);
	jw_key(&w, "status");
	volumebackup_write_status_json(&w, name);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_volume_backup_policy_put(int fd, const char *name, const char *body,
                                             size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const struct json_value *jen, *jre;
	int retain = 0;
	enum volume_error verr;

	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	jen = json_object_get(root, "enabled");
	jre = json_object_get(root, "retain");
	if (jre != NULL) {
		if (jre->type != JSON_NUMBER || json_as_number(jre) < 1 ||
		    json_as_number(jre) > VOLUMEBACKUP_MAX_RETAIN) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "retain must be between 1 and 365");
			return;
		}
		retain = (int)json_as_number(jre);
	}
	{
		const char *jwr = json_as_string(json_object_get(root, "while_running"));
		struct volume *cur = volume_find(name);

		/* Omitted leaves the current mode alone -- a partial update
		 * that silently rewrites a field it was not given is how
		 * settings get lost. */
		verr = volume_set_backup_policy(
		    name, jen != NULL && jen->type == JSON_BOOL && jen->u.boolean, retain,
		    jwr != NULL ? volume_running_mode_parse(jwr)
		                : (cur != NULL ? cur->backup_while_running : VOLUME_RUNNING_REFUSE));
	}
	json_free(root);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	handle_volume_backups_get(fd, name);
}

void handle_volume_backup_now(int fd, const char *name)
{
	enum volumebackup_error verr = volumebackup_take(name, &g_volumebackup_hooks);

	if (verr != VOLUMEBACKUP_OK) {
		respond_volumebackup_error(fd, verr);
		return;
	}
	handle_volume_backups_get(fd, name);
}

/*
 * Restoring replaces the volume's contents outright. Guarded like every
 * other destructive operation in this API: the caller has to name the
 * volume back, so it can never be something a stray click did.
 */
void handle_volume_restore(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root = json_parse(body, body_len);
	const char *stamp, *confirm;
	enum volumebackup_error verr;

	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	stamp = json_as_string(json_object_get(root, "snapshot"));
	confirm = json_as_string(json_object_get(root, "confirm_volume_name"));
	if (confirm == NULL || strcmp(confirm, name) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "confirm_volume_name must be given and must match the volume in the URL -- "
		              "restoring replaces everything currently in this volume");
		return;
	}
	if (stamp == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "snapshot is required");
		return;
	}
	verr = volumebackup_restore(name, stamp, &g_volumebackup_hooks);
	json_free(root);
	if (verr != VOLUMEBACKUP_OK) {
		respond_volumebackup_error(fd, verr);
		return;
	}
	handle_volume_backups_get(fd, name);
}

void handle_volume_backup_delete(int fd, const char *name, const char *stamp)
{
	enum volumebackup_error verr = volumebackup_delete_snapshot(name, stamp);

	if (verr != VOLUMEBACKUP_OK) {
		respond_volumebackup_error(fd, verr);
		return;
	}
	handle_volume_backups_get(fd, name);
}

void handle_volume_delete(int fd, const char *name)
{
	char user[REGISTRY_NAME_MAX];
	enum volume_error verr;

	if (volume_find(name) == NULL) {
		respond_error(fd, 404, "Not Found", "no such volume");
		return;
	}
	if (volume_referenced_by_container(name, user, sizeof(user))) {
		char msg[256];

		snprintf(msg, sizeof(msg),
		         "still referenced by container '%s' -- delete or edit that container first", user);
		respond_error(fd, 409, "Conflict", msg);
		return;
	}
	verr = volume_delete(name);
	if (verr != VOLUME_OK) {
		respond_volume_error(fd, verr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}
