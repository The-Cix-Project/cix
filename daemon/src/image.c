#include "image.h"
#include "json.h"
#include "namecheck.h"
#include "persist.h"
#include "pkg.h"
#include "registry.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char g_images_dir[PATH_MAX];

/* All three of an image's own persisted concerns (ADR-0107/0108) --
 * operator-declared package intent, which built version is current,
 * and the full version history -- live in one in-memory struct and one
 * on-disk document (manifest.json), read/written together so they can
 * never drift out of sync with each other. */
struct image_state {
	struct image_manifest_entry packages[IMAGE_MANIFEST_MAX_PACKAGES];
	int package_count;
	char current_version[IMAGE_VERSION_MAX];
	struct image_version_entry versions[IMAGE_MAX_VERSION_HISTORY];
	int version_count;
};

void image_init(const char *images_dir)
{
	snprintf(g_images_dir, sizeof(g_images_dir), "%s", images_dir);
}

static int image_name_is_valid(const char *name)
{
	return simple_name_is_valid(name, PKG_IMAGE_NAME_MAX);
}

static void manifest_path(const char *name, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s/manifest.json", g_images_dir, name);
}

static const char *mode_str(enum image_pkg_mode mode)
{
	return mode == IMAGE_PKG_PINNED ? "pinned" : "rolling";
}

static int mode_from_str(const char *s, enum image_pkg_mode *out)
{
	if (strcmp(s, "pinned") == 0) {
		*out = IMAGE_PKG_PINNED;
		return 0;
	}
	if (strcmp(s, "rolling") == 0) {
		*out = IMAGE_PKG_ROLLING;
		return 0;
	}
	return -1;
}

void image_version_rootfs_path(const char *name, const char *version, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s/%s/rootfs", g_images_dir, name, version);
}

int image_hash_manifest_string(const char *s, char *out_hash, size_t out_hash_size)
{
	char tmp_path[PATH_MAX];
	int rc;

	snprintf(tmp_path, sizeof(tmp_path), "%s/.hash_tmp.%d", g_images_dir, (int)getpid());
	if (persist_atomic_write(tmp_path, s, strlen(s)) != 0)
		return -1;
	rc = pkg_run_capture_sha256(tmp_path, out_hash, out_hash_size);
	unlink(tmp_path);
	return rc;
}

/* manifest.json's own existence is the authoritative "does this image
 * exist" signal -- image_create() always writes it as the very last
 * step, atomically, so a partially-created image (rootfs staged but
 * the process died before this file landed) correctly reads back as
 * "doesn't exist" rather than a corrupt half-image. */
static int load_state(const char *name, struct image_state *st)
{
	char path[PATH_MAX];
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jpackages, *jcurrent, *jversions;
	size_t i;

	memset(st, 0, sizeof(*st));
	manifest_path(name, path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return -1;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		fprintf(stderr, "%s: malformed image manifest.json\n", path);
		return -1;
	}

	jcurrent = json_object_get(root, "current_version");
	if (jcurrent != NULL && json_as_string(jcurrent) != NULL)
		snprintf(st->current_version, sizeof(st->current_version), "%s",
		         json_as_string(jcurrent));

	jpackages = json_object_get(root, "packages");
	if (jpackages != NULL && jpackages->type == JSON_ARRAY) {
		for (i = 0; i < jpackages->u.array.count && st->package_count < IMAGE_MANIFEST_MAX_PACKAGES;
		     i++) {
			const struct json_value *item = jpackages->u.array.items[i];
			const char *package = json_as_string(json_object_get(item, "package"));
			const char *mode_field = json_as_string(json_object_get(item, "mode"));
			const char *version = json_as_string(json_object_get(item, "version"));
			enum image_pkg_mode mode;

			if (!simple_name_is_valid(package, PKG_NAME_MAX) || mode_field == NULL ||
			    mode_from_str(mode_field, &mode) != 0 || version == NULL || version[0] == '\0' ||
			    strlen(version) >= PKG_VERSION_MAX) {
				fprintf(stderr, "%s: invalid packages[%zu], skipped\n", path, i);
				continue;
			}
			snprintf(st->packages[st->package_count].package,
			         sizeof(st->packages[0].package), "%s", package);
			st->packages[st->package_count].mode = mode;
			snprintf(st->packages[st->package_count].version,
			         sizeof(st->packages[0].version), "%s", version);
			st->package_count++;
		}
	}

	jversions = json_object_get(root, "versions");
	if (jversions != NULL && jversions->type == JSON_ARRAY) {
		for (i = 0; i < jversions->u.array.count && st->version_count < IMAGE_MAX_VERSION_HISTORY;
		     i++) {
			const struct json_value *item = jversions->u.array.items[i];
			const char *version = json_as_string(json_object_get(item, "version"));

			if (version == NULL || version[0] == '\0' || strlen(version) >= IMAGE_VERSION_MAX) {
				fprintf(stderr, "%s: invalid versions[%zu], skipped\n", path, i);
				continue;
			}
			snprintf(st->versions[st->version_count].version,
			         sizeof(st->versions[0].version), "%s", version);
			st->versions[st->version_count].created_at =
			    (long)json_as_number(json_object_get(item, "created_at"));
			st->version_count++;
		}
	}

	json_free(root);
	return 0;
}

static int save_state(const char *name, const struct image_state *st)
{
	char path[PATH_MAX];
	struct json_writer w;
	int rc, i;

	manifest_path(name, path, sizeof(path));

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "packages");
	jw_arr_open(&w);
	for (i = 0; i < st->package_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "package");
		jw_str(&w, st->packages[i].package);
		jw_key(&w, "mode");
		jw_str(&w, mode_str(st->packages[i].mode));
		jw_key(&w, "version");
		jw_str(&w, st->packages[i].version);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_key(&w, "current_version");
	jw_str(&w, st->current_version);
	jw_key(&w, "versions");
	jw_arr_open(&w);
	for (i = 0; i < st->version_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "version");
		jw_str(&w, st->versions[i].version);
		jw_key(&w, "created_at");
		jw_int(&w, st->versions[i].created_at);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);

	rc = persist_atomic_write(path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

enum image_error image_create(const char *name)
{
	char path[PATH_MAX];
	struct stat st;
	struct image_state new_state;
	char hash[IMAGE_VERSION_MAX];
	char rootfs[PATH_MAX];

	if (!image_name_is_valid(name))
		return IMAGE_ERR_INVALID_NAME;

	manifest_path(name, path, sizeof(path));
	if (stat(path, &st) == 0)
		return IMAGE_ERR_DUPLICATE;

	if (image_hash_manifest_string("", hash, sizeof(hash)) != 0)
		return IMAGE_ERR_CREATE_FAILED;

	image_version_rootfs_path(name, hash, rootfs, sizeof(rootfs));
	if (persist_mkdir_p(rootfs) != 0)
		return IMAGE_ERR_CREATE_FAILED;
	if (pkg_seed_image_baseline(rootfs) != PKG_OK)
		return IMAGE_ERR_CREATE_FAILED;

	memset(&new_state, 0, sizeof(new_state));
	snprintf(new_state.current_version, sizeof(new_state.current_version), "%s", hash);
	snprintf(new_state.versions[0].version, sizeof(new_state.versions[0].version), "%s", hash);
	new_state.versions[0].created_at = (long)time(NULL);
	new_state.version_count = 1;

	if (save_state(name, &new_state) != 0)
		return IMAGE_ERR_CREATE_FAILED;
	return IMAGE_OK;
}

enum image_error image_delete(const char *name)
{
	char path[PATH_MAX];
	char image_dir[PATH_MAX];
	struct stat st;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;

	manifest_path(name, path, sizeof(path));
	if (stat(path, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	if (strcmp(name, PKG_DEFAULT_IMAGE) == 0)
		return IMAGE_ERR_PROTECTED;
	if (registry_image_in_use(name))
		return IMAGE_ERR_IN_USE;
	if (pkg_image_has_packages(name))
		return IMAGE_ERR_HAS_PACKAGES;

	snprintf(image_dir, sizeof(image_dir), "%s/%s", g_images_dir, name);
	if (persist_remove_tree(image_dir) != 0)
		return IMAGE_ERR_DELETE_FAILED;

	return IMAGE_OK;
}

void image_write_json_list(struct json_writer *w)
{
	DIR *d;
	struct dirent *ent;

	jw_arr_open(w);
	d = opendir(g_images_dir);
	if (d != NULL) {
		while ((ent = readdir(d)) != NULL) {
			char path[PATH_MAX];
			struct stat st;

			if (ent->d_name[0] == '.')
				continue;
			manifest_path(ent->d_name, path, sizeof(path));
			if (stat(path, &st) == 0) {
				jw_obj_open(w);
				jw_key(w, "name");
				jw_str(w, ent->d_name);
				jw_obj_close(w);
			}
		}
		closedir(d);
	}
	jw_arr_close(w);
}

enum image_error image_write_json_one(const char *name, struct json_writer *w)
{
	char path[PATH_MAX];
	struct stat st;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;

	manifest_path(name, path, sizeof(path));
	if (stat(path, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, name);
	jw_obj_close(w);
	return IMAGE_OK;
}

enum image_error image_manifest_read(const char *name, struct image_manifest_entry *out,
                                      int *out_count, int max_entries)
{
	struct image_state st;
	int n;

	*out_count = 0;
	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;
	if (load_state(name, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	n = st.package_count < max_entries ? st.package_count : max_entries;
	memcpy(out, st.packages, (size_t)n * sizeof(out[0]));
	*out_count = n;
	return IMAGE_OK;
}

enum image_error image_manifest_write_json(const char *name, struct json_writer *w)
{
	struct image_state st;
	int i;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;
	if (load_state(name, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	jw_arr_open(w);
	for (i = 0; i < st.package_count; i++) {
		jw_obj_open(w);
		jw_key(w, "package");
		jw_str(w, st.packages[i].package);
		jw_key(w, "mode");
		jw_str(w, mode_str(st.packages[i].mode));
		jw_key(w, "version");
		jw_str(w, st.packages[i].version);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	return IMAGE_OK;
}

enum image_error image_manifest_set(const char *name, const char *package,
                                     enum image_pkg_mode mode, const char *version)
{
	struct image_state st;
	int i;

	if (!simple_name_is_valid(package, PKG_NAME_MAX))
		return IMAGE_ERR_INVALID_PACKAGE;
	if (version == NULL || version[0] == '\0' || strlen(version) >= PKG_VERSION_MAX)
		return IMAGE_ERR_INVALID_VERSION;
	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;
	if (load_state(name, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	for (i = 0; i < st.package_count; i++) {
		if (strcmp(st.packages[i].package, package) == 0) {
			st.packages[i].mode = mode;
			snprintf(st.packages[i].version, sizeof(st.packages[i].version), "%s", version);
			return save_state(name, &st) == 0 ? IMAGE_OK : IMAGE_ERR_PERSIST_FAILED;
		}
	}
	if (st.package_count >= IMAGE_MANIFEST_MAX_PACKAGES)
		return IMAGE_ERR_MANIFEST_FULL;
	snprintf(st.packages[st.package_count].package, sizeof(st.packages[0].package), "%s", package);
	st.packages[st.package_count].mode = mode;
	snprintf(st.packages[st.package_count].version, sizeof(st.packages[0].version), "%s", version);
	st.package_count++;
	return save_state(name, &st) == 0 ? IMAGE_OK : IMAGE_ERR_PERSIST_FAILED;
}

enum image_error image_manifest_unset(const char *name, const char *package)
{
	struct image_state st;
	int i;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;
	if (load_state(name, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	for (i = 0; i < st.package_count; i++) {
		if (strcmp(st.packages[i].package, package) == 0) {
			memmove(&st.packages[i], &st.packages[i + 1],
			        (size_t)(st.package_count - i - 1) * sizeof(st.packages[0]));
			st.package_count--;
			return save_state(name, &st) == 0 ? IMAGE_OK : IMAGE_ERR_PERSIST_FAILED;
		}
	}
	return IMAGE_OK; /* already absent -- not an error */
}

enum image_error image_current_version(const char *name, char *out, size_t out_size)
{
	struct image_state st;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;
	if (load_state(name, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;
	if (st.current_version[0] == '\0')
		return IMAGE_ERR_NO_CURRENT_VERSION;
	snprintf(out, out_size, "%s", st.current_version);
	return IMAGE_OK;
}

enum image_error image_version_history_write_json(const char *name, struct json_writer *w)
{
	struct image_state st;
	int i;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;
	if (load_state(name, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	jw_arr_open(w);
	for (i = st.version_count - 1; i >= 0; i--) { /* newest first */
		jw_obj_open(w);
		jw_key(w, "version");
		jw_str(w, st.versions[i].version);
		jw_key(w, "created_at");
		jw_int(w, st.versions[i].created_at);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	return IMAGE_OK;
}

enum image_error image_record_version(const char *name, const char *version)
{
	struct image_state st;
	int i;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;
	if (load_state(name, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	for (i = 0; i < st.version_count; i++) {
		if (strcmp(st.versions[i].version, version) == 0) {
			snprintf(st.current_version, sizeof(st.current_version), "%s", version);
			return save_state(name, &st) == 0 ? IMAGE_OK : IMAGE_ERR_PERSIST_FAILED;
		}
	}
	if (st.version_count < IMAGE_MAX_VERSION_HISTORY) {
		snprintf(st.versions[st.version_count].version, sizeof(st.versions[0].version), "%s",
		         version);
		st.versions[st.version_count].created_at = (long)time(NULL);
		st.version_count++;
	}
	/* else: history array full -- current_version still gets repointed to
	 * the real, currently-active version; a real deployment hitting this
	 * bound is a documented, non-silent future problem (image.h's own
	 * comment on IMAGE_MAX_VERSION_HISTORY), not a hard failure of
	 * whatever install/rebuild triggered this call. */
	snprintf(st.current_version, sizeof(st.current_version), "%s", version);
	return save_state(name, &st) == 0 ? IMAGE_OK : IMAGE_ERR_PERSIST_FAILED;
}
