#include "api_image.h"

#include "apiresp.h"
#include "apiroute.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "logstore.h"
#include "containerdef.h"
#include "image.h"
#include "pkg.h"
#include "persist.h"
#include "registry.h"
#include "namecheck.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void respond_image_error(int fd, enum image_error err)
{
	switch (err) {
	case IMAGE_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid image name");
		break;
	case IMAGE_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "an image with this name already exists");
		break;
	case IMAGE_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such image");
		break;
	case IMAGE_ERR_PROTECTED:
		respond_error(fd, 400, "Bad Request", "the base image cannot be removed");
		break;
	case IMAGE_ERR_IN_USE:
		respond_error(fd, 409, "Conflict", "image is still referenced by a running container");
		break;
	case IMAGE_ERR_HAS_PACKAGES:
		respond_error(fd, 409, "Conflict", "image still has packages installed -- remove them first");
		break;
	case IMAGE_ERR_INVALID_PACKAGE:
		respond_error(fd, 400, "Bad Request", "invalid package name");
		break;
	case IMAGE_ERR_INVALID_VERSION:
		respond_error(fd, 400, "Bad Request", "version missing, empty, or too long");
		break;
	case IMAGE_ERR_MANIFEST_FULL:
		respond_error(fd, 500, "Internal Server Error", "image manifest is full");
		break;
	case IMAGE_ERR_CREATE_FAILED:
	case IMAGE_ERR_DELETE_FAILED:
	case IMAGE_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "image operation failed");
		break;
	}
}

/*
 * Reopens a just-closed top-level JSON object so a caller can splice
 * in one more sibling key after the fact (used to add "manifest" onto
 * image_write_json_one()'s own already-closed {"name":...} without
 * teaching that function itself about manifests -- see its own two
 * call sites' comments for why). w must have had exactly one
 * jw_obj_close() as its very last write; the "}" it wrote is dropped
 * and the object's own comma-tracking frame is restored so a
 * subsequent jw_key()/jw_obj_close() behaves exactly as if the object
 * had never been closed.
 */
static void jw_reopen_object(struct json_writer *w)
{
	w->len--;
	w->depth++;
}

/*
 * ADR-0107/task #721: splices "current_version" (a bare string, "" if
 * somehow unresolvable -- IMAGE_ERR_NO_CURRENT_VERSION is unreachable
 * for anything created via image_create(), which always produces one,
 * but this reports rather than assumes) and "versions" (newest-first,
 * each {version, created_at} -- image_version_history_write_json())
 * onto an already-open image JSON object. Shared by handle_image_create()'s
 * 201 and handle_image_get_one()'s 200 so the two responses report the
 * exact same shape rather than one silently lagging the other.
 */
static void write_image_version_fields(const char *name, struct json_writer *w)
{
	char current_version[IMAGE_VERSION_MAX];

	jw_key(w, "current_version");
	if (image_current_version(name, current_version, sizeof(current_version)) == IMAGE_OK)
		jw_str(w, current_version);
	else
		jw_str(w, "");
	jw_key(w, "versions");
	image_version_history_write_json(name, w);
}

/*
 * ADR-0209: is this image version still referenced by anything?
 *
 * Three sources, and all three are needed. A running container pins the
 * version it was created with (ADR-0108), so the registry is the live
 * answer. A container that is merely stopped has no registry entry but
 * will be revived from its persisted definition, which names the same
 * version -- collecting that version would turn a stopped container
 * into one that can never start again. And current_version is what a
 * fresh container is created from.
 */
static int image_version_is_referenced(const char *image, const char *version)
{
	char names[REGISTRY_MAX_CONTAINERS][REGISTRY_NAME_MAX];
	char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	char current[IMAGE_VERSION_MAX];
	int count, i;

	if (image_current_version(image, current, sizeof(current)) == IMAGE_OK &&
	    strcmp(current, version) == 0)
		return 1;

	count = registry_list_names(names, REGISTRY_MAX_CONTAINERS);
	for (i = 0; i < count; i++) {
		struct registry_entry *e = registry_find(names[i]);

		if (e != NULL && strcmp(e->image, image) == 0 && strcmp(e->image_version, version) == 0)
			return 1;
	}

	count = containerdef_resolve_order(order);
	for (i = 0; i < count; i++) {
		struct container_def *def = containerdef_find(order[i]);
		struct json_value *root;
		const char *dimg, *dver;
		int hit;

		if (def == NULL || def->body == NULL)
			continue;
		root = json_parse(def->body, def->body_len);
		if (root == NULL)
			continue;
		dimg = json_as_string(json_object_get(root, "image"));
		dver = json_as_string(json_object_get(root, "image_version"));
		hit = dimg != NULL && dver != NULL && strcmp(dimg, image) == 0 &&
		      strcmp(dver, version) == 0;
		json_free(root);
		if (hit)
			return 1;
	}
	return 0;
}

/*
 * POST /v1/images/gc -- reclaim image versions nothing references.
 *
 * This exists because there was no reclamation anywhere in the API at
 * all, and the consequence was not theoretical: 66 versions of an 8 GB
 * rootfs filled a 16 GiB partition, and the disk filling up is how it
 * was noticed. Every install produces a new immutable version
 * (ADR-0107/0108) and nothing ever removed one.
 *
 * Refuses while any package job is in flight. A build holds its image's
 * rootfs as the lowerdir its container is running on, and that
 * reference lives in pkg.c rather than in the registry -- so rather
 * than reach into another module's in-flight state and risk being
 * subtly wrong, this simply does not run concurrently with a build.
 * Collection is never urgent enough to justify racing one.
 *
 * Sizes are OPT-IN ("measure":true), and that is not a preference.
 * Measuring means an nftw() walk of every collectable version, and
 * this daemon is single-threaded: on a real box with 80 collectable
 * versions the walk took 117 seconds, during which nothing else in the
 * REST API could be served. Deletion needs no size at all, so the
 * common call now costs nothing and only a caller who actually wants
 * the magnitude pays for it.
 *
 * When measured, "apparent_bytes" is APPARENT size. On btrfs a version
 * is a snapshot sharing extents with its neighbours, so the space
 * actually returned is typically far less -- on that same box the walk
 * reported 390 GB while the filesystem returned 4.5 GB. Reported as
 * apparent rather than omitted, and named so everywhere, because an
 * operator deciding whether to collect wants the magnitude; presenting
 * it as exact would be worse than either. (btrfs qgroup EXCL bytes
 * would give the real figure in O(1) -- worth doing once the platform
 * is btrfs-only.)
 */
void handle_images_gc(int fd, const char *body, size_t body_len)
{
	char names[IMAGE_LIST_MAX][PKG_IMAGE_NAME_MAX];
	struct json_writer w;
	int active[PKG_MAX_CONCURRENT_JOBS];
	int dry_run = 0;
	int measure = 0;
	int image_count, i, j;
	int collected = 0, kept = 0, failed = 0;
	long long total_bytes = 0;

	if (pkg_active_chain_indices(active) > 0) {
		respond_error(fd, 409, "Conflict",
		              "a package job is in flight -- image collection does not run "
		              "concurrently with a build");
		return;
	}
	if (body != NULL && body_len > 0) {
		struct json_value *root = json_parse(body, body_len);

		if (root != NULL) {
			const struct json_value *dr = json_object_get(root, "dry_run");

			const struct json_value *me = json_object_get(root, "measure");

			dry_run = dr != NULL && dr->type == JSON_BOOL && dr->u.boolean;
			measure = me != NULL && me->type == JSON_BOOL && me->u.boolean;
			json_free(root);
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "dry_run");
	jw_bool(&w, dry_run);
	jw_key(&w, "measured");
	jw_bool(&w, measure);
	jw_key(&w, "reclaimed");
	jw_arr_open(&w);

	image_count = image_list_names(names, IMAGE_LIST_MAX);
	for (i = 0; i < image_count; i++) {
		char versions[IMAGE_MAX_VERSION_HISTORY][IMAGE_VERSION_MAX];
		int vcount = image_ondisk_versions(names[i], versions, IMAGE_MAX_VERSION_HISTORY);

		for (j = 0; j < vcount; j++) {
			char rootfs[PATH_MAX];
			long long bytes = 0;
			enum image_error ierr;

			if (image_version_is_referenced(names[i], versions[j])) {
				kept++;
				continue;
			}
			if (measure) {
				image_version_rootfs_path(names[i], versions[j], rootfs, sizeof(rootfs));
				if (overlay_upperdir_size(rootfs, &bytes) != 0)
					bytes = 0;
			}

			if (!dry_run) {
				ierr = image_delete_version(names[i], versions[j]);
				if (ierr != IMAGE_OK) {
					/* Reported, not fatal: one stubborn version must not
					 * stop the rest being reclaimed, and the operator
					 * needs to know which one resisted. */
					logstore_write("cixd", "error",
					                "image gc: could not remove %s@%s (%d)", names[i],
					                versions[j], (int)ierr);
					failed++;
					continue;
				}
			}
			jw_obj_open(&w);
			jw_key(&w, "image");
			jw_str(&w, names[i]);
			jw_key(&w, "version");
			jw_str(&w, versions[j]);
			jw_key(&w, "apparent_bytes");
			if (measure)
				jw_int(&w, bytes);
			else
				jw_null(&w);
			jw_obj_close(&w);
			collected++;
			total_bytes += bytes;
		}
	}
	jw_arr_close(&w);
	jw_key(&w, "collected");
	jw_int(&w, collected);
	jw_key(&w, "kept");
	jw_int(&w, kept);
	jw_key(&w, "failed");
	jw_int(&w, failed);
	jw_key(&w, "apparent_bytes_total");
	if (measure)
		jw_int(&w, total_bytes);
	else
		jw_null(&w);
	jw_obj_close(&w);

	if (!dry_run && collected > 0)
		logstore_write("cixd", "info",
		                "image gc: reclaimed %d version(s), %lld apparent bytes, kept %d",
		                collected, total_bytes, kept);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_image_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	enum image_error ierr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	if (name == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name missing");
		return;
	}

	ierr = image_create(name);
	if (ierr != IMAGE_OK) {
		json_free(root);
		respond_image_error(fd, ierr);
		return;
	}

	jw_init(&w);
	image_write_json_one(name, &w);
	jw_reopen_object(&w);
	jw_key(&w, "manifest");
	jw_arr_open(&w); /* a just-created image never has a manifest.json yet -- always empty */
	jw_arr_close(&w);
	/*
	 * name still points into root's own parsed tree -- json_free(root)
	 * must not run until every use of name is done (a real
	 * use-after-free was caught live here: write_image_version_fields()
	 * used to run AFTER json_free(root), the same class of bug already
	 * fixed once before for a different handler, task #713).
	 */
	write_image_version_fields(name, &w);
	json_free(root);
	jw_obj_close(&w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_image_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "images");
	image_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * image_write_json_one() writes a bare {"name":...} object; the
 * manifest (ADR-0107) is spliced in as a sibling "manifest" key by
 * re-opening the object rather than by teaching image_write_json_one()
 * itself about manifests -- keeps that function's own contract (pure
 * filesystem-state report) unchanged for its other caller
 * (handle_image_create()'s 201 response, which has no manifest to
 * report for a just-created, empty-manifest image either way).
 */
void handle_image_get_one(int fd, const char *name)
{
	struct json_writer w;
	enum image_error ierr;

	jw_init(&w);
	ierr = image_write_json_one(name, &w);
	if (ierr != IMAGE_OK) {
		jw_free(&w);
		respond_image_error(fd, ierr);
		return;
	}
	jw_reopen_object(&w);
	jw_key(&w, "manifest");
	ierr = image_manifest_write_json(name, &w);
	if (ierr != IMAGE_OK) {
		jw_free(&w);
		respond_image_error(fd, ierr);
		return;
	}
	write_image_version_fields(name, &w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_image_delete(int fd, const char *name)
{
	enum image_error ierr = image_delete(name);

	if (ierr != IMAGE_OK) {
		respond_image_error(fd, ierr);
		return;
	}
	pkg_image_forgotten(name); /* #382: the queue and any grant go with it */
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * GET /v1/images/{name}/versions/{version}/manifest (#398): what one
 * specific version DECLARES, read from the snapshot written beside
 * that version's rootfs when it was produced.
 *
 * A container records the image version it runs from and keeps running
 * it until recreated, so this -- not the image's live manifest -- is
 * the only honest source for "what is in this container". The two
 * agree while the container is current and diverge silently afterwards,
 * which is the failure mode worth engineering against: a wrong answer
 * that looks exactly like a right one.
 *
 * A version produced before snapshots existed 404s rather than falling
 * back. See image_manifest_version_write_json() for why the fallback
 * would be worse than the gap.
 */
void handle_image_version_manifest(int fd, const char *name, const char *version)
{
	struct json_writer w;
	enum image_error ierr;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "version");
	jw_str(&w, version);
	jw_key(&w, "manifest");
	ierr = image_manifest_version_write_json(name, version, &w);
	if (ierr != IMAGE_OK) {
		jw_free(&w);
		respond_error(fd, 404, "Not Found",
		              "no manifest recorded for this image version -- either the image or the "
		              "version does not exist, or the version predates per-version manifest "
		              "snapshots (#398)");
		return;
	}
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * POST /v1/images/{name}/manifest (ADR-0107): upserts one {package,
 * mode, version} entry into name's own manifest -- operator-declared
 * package intent, independent of whatever the image's rootfs currently
 * contains. Does not itself trigger a rebuild (task #720's own job).
 */
void handle_image_manifest_set(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *package;
	const char *mode_str;
	const char *version;
	enum image_pkg_mode mode;
	enum image_error ierr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	package = json_as_string(json_object_get(root, "package"));
	mode_str = json_as_string(json_object_get(root, "mode"));
	version = json_as_string(json_object_get(root, "version"));
	if (package == NULL || mode_str == NULL || version == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "package, mode, and version are all required");
		return;
	}
	if (strcmp(mode_str, "pinned") == 0) {
		mode = IMAGE_PKG_PINNED;
	} else if (strcmp(mode_str, "rolling") == 0) {
		mode = IMAGE_PKG_ROLLING;
	} else {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "mode must be \"pinned\" or \"rolling\"");
		return;
	}

	ierr = image_manifest_set(name, package, mode, version);
	json_free(root);
	if (ierr != IMAGE_OK) {
		respond_image_error(fd, ierr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/* DELETE /v1/images/{name}/manifest/{package} (ADR-0107). */
void handle_image_manifest_unset(int fd, const char *name, const char *package)
{
	enum image_error ierr = image_manifest_unset(name, package);

	if (ierr != IMAGE_OK) {
		respond_image_error(fd, ierr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * ADR-0123: image recipes (Part 4 of the pkg/ redesign) share pkg_error
 * with package recipes, but respond_pkg_recipe_error()'s own
 * PKG_ERR_INVALID_RECIPE wording names pkg_name= (package-recipe-
 * specific) -- a real, misleading-message gap for an image recipe,
 * same reasoning respond_pkg_recipe_error()'s own doc comment already
 * gives for why it exists distinct from respond_pkg_error().
 */
static void respond_image_recipe_error(int fd, enum pkg_error err)
{
	switch (err) {
	case PKG_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid image recipe name");
		break;
	case PKG_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such image recipe");
		break;
	case PKG_ERR_INVALID_RECIPE:
		respond_error(fd, 400, "Bad Request",
		              "recipe content failed to parse -- image_packages= is required, each "
		              "entry \"name:pinned|rolling:version\"");
		break;
	case PKG_ERR_TARGET_IMAGE_NOT_FOUND:
		respond_error(fd, 404, "Not Found",
		              "the recipe parsed fine, but the image it names doesn't exist yet -- "
		              "create it first (image create --name=...)");
		break;
	case PKG_ERR_BUSY:
		respond_error(fd, 409, "Conflict",
		              "another package install/hostbuild/image-recipe-apply is already in progress");
		break;
	case PKG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "image recipe operation failed");
		break;
	}
}

/*
 * GET /v1/software (issue #97) -- what is DECLARED against what is
 * actually INSTALLED, reconciled here rather than in every client.
 *
 * The gap this closes is real and was found on a live box: 11 images
 * existed, 8 had recipes. Two of the three without one were debris
 * nobody had noticed, because nothing anywhere put the declared set
 * next to the installed set. An image with no recipe cannot be rebuilt
 * from source control, which on a platform whose premise is
 * "reproducible from source" is exactly the state worth surfacing.
 *
 * Three states, each meaning something different and each actionable:
 *
 *   declared + installed    normal
 *   installed, not declared drift -- capture a recipe, or it is debris
 *   declared, not installed a recipe nobody has applied
 *
 * Computed server-side deliberately. Two clients would otherwise each
 * implement this join across four endpoints and drift from each other,
 * the way disk role and partition label did before #90 folded that join
 * in. The API-First Mandate makes the endpoint the thing that exists;
 * the dashboard and CLI both render it.
 */
static void software_write_kind(struct json_writer *w, const char *kind,
                                 char declared[][PKG_IMAGE_NAME_MAX], int declared_count,
                                 char installed[][PKG_IMAGE_NAME_MAX], int installed_count)
{
	int i, k;

	for (i = 0; i < installed_count; i++) {
		int is_declared = 0;

		for (k = 0; k < declared_count; k++) {
			if (strcmp(installed[i], declared[k]) == 0) {
				is_declared = 1;
				break;
			}
		}
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, installed[i]);
		jw_key(w, "kind");
		jw_str(w, kind);
		jw_key(w, "declared");
		jw_bool(w, is_declared);
		jw_key(w, "installed");
		jw_bool(w, 1);
		jw_obj_close(w);
	}
	/* Declared but not installed -- the other half of the picture, and
	 * the reason this is a reconciliation rather than a flag on the
	 * image list. */
	for (k = 0; k < declared_count; k++) {
		int is_installed = 0;

		for (i = 0; i < installed_count; i++) {
			if (strcmp(installed[i], declared[k]) == 0) {
				is_installed = 1;
				break;
			}
		}
		if (is_installed)
			continue;
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, declared[k]);
		jw_key(w, "kind");
		jw_str(w, kind);
		jw_key(w, "declared");
		jw_bool(w, 1);
		jw_key(w, "installed");
		jw_bool(w, 0);
		jw_obj_close(w);
	}
}

void handle_software_list(int fd)
{
	static char declared[PKG_MAX_PACKAGES][PKG_IMAGE_NAME_MAX];
	static char installed[PKG_MAX_PACKAGES][PKG_IMAGE_NAME_MAX];
	struct json_writer w;
	int dn, in_;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "software");
	jw_arr_open(&w);

	dn = image_recipe_list_names(declared, PKG_MAX_PACKAGES);
	in_ = image_list_names(installed, PKG_MAX_PACKAGES);
	software_write_kind(&w, "image", declared, dn, installed, in_);

	dn = pkg_recipe_list_names(declared, PKG_MAX_PACKAGES);
	in_ = pkg_installed_list_names(installed, PKG_MAX_PACKAGES);
	software_write_kind(&w, "package", declared, dn, installed, in_);

	{
		/*
		 * A container's "installed" is a persisted definition. Every
		 * container is persisted since ADR-0181, so this is simply
		 * every container that exists, running or not.
		 */
		char order[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
		int n = containerdef_resolve_order(order);
		int i;

		for (i = 0; i < n && i < PKG_MAX_PACKAGES; i++)
			snprintf(installed[i], PKG_IMAGE_NAME_MAX, "%s", order[i]);
		if (n > PKG_MAX_PACKAGES)
			n = PKG_MAX_PACKAGES;
		dn = container_recipe_list_names(declared, PKG_MAX_PACKAGES);
		software_write_kind(&w, "container", declared, dn, installed, n);
	}

	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_image_recipe_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "recipes");
	image_recipe_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_image_recipe_add(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name;
	const char *content;
	enum pkg_error perr;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	name = json_as_string(json_object_get(root, "name"));
	content = json_as_string(json_object_get(root, "content"));
	if (name == NULL || content == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name and content both required");
		return;
	}

	perr = image_recipe_add(name, content);
	json_free(root);
	if (perr != PKG_OK) {
		respond_image_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

void handle_image_recipe_get(int fd, const char *name)
{
	char *content;
	size_t content_len;
	enum pkg_error perr;
	struct json_writer w;

	perr = image_recipe_get(name, &content, &content_len);
	if (perr != PKG_OK) {
		respond_image_recipe_error(fd, perr);
		return;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_obj_close(&w);
	free(content);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_image_recipe_delete(int fd, const char *name)
{
	enum pkg_error perr = image_recipe_rm(name);

	if (perr != PKG_OK) {
		respond_image_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

/*
 * POST /v1/images/{name}/apply-recipe -- always 204. ADR-0209 retired
 * the async artifact-fetch fast path this used to have a 202 branch
 * for, so applying a recipe is bulk-declare and nothing else: it has
 * finished by the time this returns.
 */
void handle_image_recipe_apply(int fd, const char *name)
{
	enum pkg_error perr = pkg_image_recipe_apply_start(name);

	if (perr != PKG_OK) {
		respond_image_recipe_error(fd, perr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}


/*
 * Container recipes (ADR-0151) -- same shape as the image-recipe
 * handlers immediately above, minus the async artifact-fetch path
 * (containers create synchronously, always).
 */
