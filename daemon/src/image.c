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
#include <unistd.h>

static char g_images_dir[PATH_MAX];

void image_init(const char *images_dir)
{
	snprintf(g_images_dir, sizeof(g_images_dir), "%s", images_dir);
}

static int image_name_is_valid(const char *name)
{
	return simple_name_is_valid(name, PKG_IMAGE_NAME_MAX);
}

static void image_rootfs_path(const char *name, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s/rootfs", g_images_dir, name);
}

static int has_rootfs_subdir(const char *image_dir)
{
	char rootfs[PATH_MAX];
	struct stat st;

	snprintf(rootfs, sizeof(rootfs), "%s/rootfs", image_dir);
	return stat(rootfs, &st) == 0 && S_ISDIR(st.st_mode);
}

enum image_error image_create(const char *name)
{
	char rootfs[PATH_MAX];
	struct stat st;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_INVALID_NAME;

	image_rootfs_path(name, rootfs, sizeof(rootfs));
	if (stat(rootfs, &st) == 0)
		return IMAGE_ERR_DUPLICATE;

	if (persist_mkdir_p(rootfs) != 0)
		return IMAGE_ERR_CREATE_FAILED;
	if (pkg_seed_image_baseline(name) != PKG_OK)
		return IMAGE_ERR_CREATE_FAILED;

	return IMAGE_OK;
}

enum image_error image_delete(const char *name)
{
	char rootfs[PATH_MAX];
	char image_dir[PATH_MAX];
	struct stat st;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;

	image_rootfs_path(name, rootfs, sizeof(rootfs));
	if (stat(rootfs, &st) != 0)
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
			char image_dir[PATH_MAX];

			if (ent->d_name[0] == '.')
				continue;
			snprintf(image_dir, sizeof(image_dir), "%s/%s", g_images_dir, ent->d_name);
			if (has_rootfs_subdir(image_dir)) {
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
	char rootfs[PATH_MAX];
	struct stat st;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;

	image_rootfs_path(name, rootfs, sizeof(rootfs));
	if (stat(rootfs, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, name);
	jw_obj_close(w);
	return IMAGE_OK;
}

/* -------------------------------------------------------------------
 * Image manifests (ADR-0107): a real, persisted {package, mode,
 * version} list expressing operator intent, distinct from the pure
 * filesystem-state functions above. See image.h's own doc comment for
 * why this reads/writes its file fresh on every call rather than
 * loading into a startup-resident table.
 * ------------------------------------------------------------------- */

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

enum image_error image_manifest_read(const char *name, struct image_manifest_entry *out,
                                      int *out_count, int max_entries)
{
	char path[PATH_MAX];
	char rootfs[PATH_MAX];
	struct stat st;
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	*out_count = 0;

	if (!image_name_is_valid(name))
		return IMAGE_ERR_NOT_FOUND;
	image_rootfs_path(name, rootfs, sizeof(rootfs));
	if (stat(rootfs, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	manifest_path(name, path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0)
		return IMAGE_OK; /* no manifest.json yet -- a real, valid empty manifest */
	if (buf == NULL)
		return IMAGE_OK;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted image manifest\n", path);
		return IMAGE_OK; /* display-only data corruption -- treat as empty, don't fail the caller */
	}

	for (i = 0; i < root->u.array.count && count < max_entries; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *package = json_as_string(json_object_get(item, "package"));
		const char *mode_field = json_as_string(json_object_get(item, "mode"));
		const char *version = json_as_string(json_object_get(item, "version"));
		enum image_pkg_mode mode;

		if (!simple_name_is_valid(package, PKG_NAME_MAX) || mode_field == NULL ||
		    mode_from_str(mode_field, &mode) != 0 || version == NULL || version[0] == '\0' ||
		    strlen(version) >= PKG_VERSION_MAX) {
			fprintf(stderr, "%s: invalid entry at index %zu, skipped\n", path, i);
			continue;
		}
		snprintf(out[count].package, sizeof(out[count].package), "%s", package);
		out[count].mode = mode;
		snprintf(out[count].version, sizeof(out[count].version), "%s", version);
		count++;
	}
	json_free(root);
	*out_count = count;
	return IMAGE_OK;
}

enum image_error image_manifest_write_json(const char *name, struct json_writer *w)
{
	struct image_manifest_entry entries[IMAGE_MANIFEST_MAX_PACKAGES];
	int count, i;
	enum image_error err;

	err = image_manifest_read(name, entries, &count, IMAGE_MANIFEST_MAX_PACKAGES);
	if (err != IMAGE_OK)
		return err;

	jw_arr_open(w);
	for (i = 0; i < count; i++) {
		jw_obj_open(w);
		jw_key(w, "package");
		jw_str(w, entries[i].package);
		jw_key(w, "mode");
		jw_str(w, mode_str(entries[i].mode));
		jw_key(w, "version");
		jw_str(w, entries[i].version);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	return IMAGE_OK;
}

static enum image_error save_manifest(const char *name, const struct image_manifest_entry *entries,
                                       int count)
{
	char path[PATH_MAX];
	struct json_writer w;
	int rc, i;

	manifest_path(name, path, sizeof(path));

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "package");
		jw_str(&w, entries[i].package);
		jw_key(&w, "mode");
		jw_str(&w, mode_str(entries[i].mode));
		jw_key(&w, "version");
		jw_str(&w, entries[i].version);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(path, w.buf, w.len);
	jw_free(&w);
	return rc == 0 ? IMAGE_OK : IMAGE_ERR_PERSIST_FAILED;
}

enum image_error image_manifest_set(const char *name, const char *package,
                                     enum image_pkg_mode mode, const char *version)
{
	struct image_manifest_entry entries[IMAGE_MANIFEST_MAX_PACKAGES];
	int count, i;
	enum image_error err;

	if (!simple_name_is_valid(package, PKG_NAME_MAX))
		return IMAGE_ERR_INVALID_PACKAGE;
	if (version == NULL || version[0] == '\0' || strlen(version) >= PKG_VERSION_MAX)
		return IMAGE_ERR_INVALID_VERSION;

	err = image_manifest_read(name, entries, &count, IMAGE_MANIFEST_MAX_PACKAGES);
	if (err != IMAGE_OK)
		return err;

	for (i = 0; i < count; i++) {
		if (strcmp(entries[i].package, package) == 0) {
			entries[i].mode = mode;
			snprintf(entries[i].version, sizeof(entries[i].version), "%s", version);
			return save_manifest(name, entries, count);
		}
	}

	if (count >= IMAGE_MANIFEST_MAX_PACKAGES)
		return IMAGE_ERR_MANIFEST_FULL;
	snprintf(entries[count].package, sizeof(entries[count].package), "%s", package);
	entries[count].mode = mode;
	snprintf(entries[count].version, sizeof(entries[count].version), "%s", version);
	count++;
	return save_manifest(name, entries, count);
}

enum image_error image_manifest_unset(const char *name, const char *package)
{
	struct image_manifest_entry entries[IMAGE_MANIFEST_MAX_PACKAGES];
	int count, i;
	enum image_error err;

	err = image_manifest_read(name, entries, &count, IMAGE_MANIFEST_MAX_PACKAGES);
	if (err != IMAGE_OK)
		return err;

	for (i = 0; i < count; i++) {
		if (strcmp(entries[i].package, package) == 0) {
			memmove(&entries[i], &entries[i + 1],
			        (size_t)(count - i - 1) * sizeof(entries[0]));
			count--;
			return save_manifest(name, entries, count);
		}
	}
	return IMAGE_OK; /* already absent -- not an error */
}
