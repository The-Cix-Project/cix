#include "image.h"
#include "json.h"
#include "namecheck.h"
#include "persist.h"
#include "pkg.h"
#include "registry.h"
#include "btrfs.h"

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

/*
 * #398: the manifest as it stood when one particular version was
 * produced, kept beside that version's own rootfs.
 *
 * The image-level manifest.json is a LIVE document -- it is the
 * operator's current declared intent, and it changes every time a
 * package is added or a mode flipped. A container, though, records the
 * image VERSION it runs from and keeps running that version until it
 * is recreated, so "what is in this container" cannot be answered from
 * the live manifest: it would describe what the image contains now,
 * which is a different question with the same shape. Presenting one as
 * the other is the confident-wrong-answer failure #395 already cost a
 * debugging round to.
 *
 * What it records is the INSTALLED SET -- the sorted name@version
 * pairs of everything installed into the image -- and not the declared
 * manifest.json this file otherwise deals in. Those are two different
 * documents and only one of them answers the question. The declared
 * manifest is operator intent, and a `rolling` entry's version there is
 * a FLOOR rather than a fact, so it cannot say which version of a
 * package a container actually holds. The installed set can, and it is
 * additionally what image_produce_new_version() hashes to get the
 * version in the first place (build_image_manifest_string(), ADR-0108)
 * -- so the snapshot is a copy of the version's own identity, not a
 * second, independently-drifting record of it.
 *
 * The writer therefore lives in pkg.c, next to the installed set it
 * reads. Only the reader is here, because a reader just parses a JSON
 * file.
 *
 * Versions produced before this existed have no snapshot, and
 * image_manifest_version_write_json() reports that as a plain
 * NOT_FOUND rather than falling back to the live manifest. A fallback
 * there would reintroduce exactly the wrong answer this exists to
 * prevent, with nothing marking it.
 */
void image_version_manifest_path(const char *name, const char *version, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s/%s/manifest.json", g_images_dir, name, version);
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

int image_empty_manifest_version(char *out, size_t out_size)
{
	return image_hash_manifest_string("", out, out_size);
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
	/*
	 * ADR-0207 phase 1: the version rootfs is a btrfs subvolume where
	 * the image store is on btrfs, a plain directory on ext4. The
	 * version directory (rootfs's parent) is created first, since a
	 * subvolume is created relative to an existing parent; on ext4
	 * this is exactly the persist_mkdir_p the store did before, split
	 * into parent-then-leaf.
	 */
	{
		char verdir[PATH_MAX];
		const char *slash = strrchr(rootfs, '/');

		snprintf(verdir, sizeof(verdir), "%.*s", (int)(slash - rootfs), rootfs);
		if (persist_mkdir_p(verdir) != 0)
			return IMAGE_ERR_CREATE_FAILED;
		if (cix_btrfs_subvol_create_or_dir(rootfs) != 0)
			return IMAGE_ERR_CREATE_FAILED;
	}
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

/*
 * Issue #124: rename an image and every package row that names it.
 *
 * The two halves must move together -- a directory rename that left the
 * rows behind would produce exactly the orphaned "installed in an image
 * that does not exist" rows issue #111 was about. The rows are moved
 * only after the directory rename succeeds, so a failed rename changes
 * nothing at all.
 */
enum image_error image_rename(const char *old_name, const char *new_name)
{
	char old_dir[PATH_MAX];
	char new_dir[PATH_MAX];
	char probe[PATH_MAX];
	struct stat st;

	if (!image_name_is_valid(old_name) || !image_name_is_valid(new_name))
		return IMAGE_ERR_INVALID_NAME;
	if (strcmp(old_name, new_name) == 0)
		return IMAGE_ERR_DUPLICATE;
	if (strcmp(old_name, PKG_DEFAULT_IMAGE) == 0 || strcmp(new_name, PKG_DEFAULT_IMAGE) == 0)
		return IMAGE_ERR_PROTECTED;

	manifest_path(old_name, probe, sizeof(probe));
	if (stat(probe, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;
	manifest_path(new_name, probe, sizeof(probe));
	if (stat(probe, &st) == 0)
		return IMAGE_ERR_DUPLICATE;

	/* A running container's overlay lowerdir points into this directory
	 * by path; renaming it out from under one is not survivable. */
	if (registry_image_in_use(old_name))
		return IMAGE_ERR_IN_USE;

	snprintf(old_dir, sizeof(old_dir), "%s/%s", g_images_dir, old_name);
	snprintf(new_dir, sizeof(new_dir), "%s/%s", g_images_dir, new_name);
	if (rename(old_dir, new_dir) != 0)
		return IMAGE_ERR_DELETE_FAILED;

	if (pkg_rename_image(old_name, new_name) < 0) {
		/* State could not be persisted -- put the directory back so the
		 * two halves stay consistent rather than half-applied. */
		rename(new_dir, old_dir);
		pkg_rename_image(new_name, old_name);
		return IMAGE_ERR_DELETE_FAILED;
	}
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

	/*
	 * ADR-0207 phase 1: destroy each version's rootfs subvolume first,
	 * so the plain recursive remove below only ever faces ordinary
	 * directories -- a btrfs subvolume root cannot be rmdir()'d, which
	 * is why persist_remove_tree alone would fail on btrfs. On ext4
	 * this removes the rootfs tree that persist_remove_tree would have
	 * removed anyway (a harmless head start), keeping one code path for
	 * both filesystems. Errors here are not fatal: persist_remove_tree
	 * is the backstop and reports the real failure.
	 */
	{
		DIR *d = opendir(image_dir);

		if (d != NULL) {
			struct dirent *e;

			while ((e = readdir(d)) != NULL) {
				char rootfs[PATH_MAX];
				struct stat rst;

				if (e->d_name[0] == '.')
					continue;
				snprintf(rootfs, sizeof(rootfs), "%s/%s/rootfs", image_dir, e->d_name);
				if (lstat(rootfs, &rst) == 0 && S_ISDIR(rst.st_mode))
					cix_btrfs_subvol_delete_or_rmtree(rootfs);
			}
			closedir(d);
		}
	}

	if (persist_remove_tree(image_dir) != 0)
		return IMAGE_ERR_DELETE_FAILED;

	return IMAGE_OK;
}

/*
 * ADR-0209: every version directory that exists ON DISK for this image.
 *
 * Read from the filesystem rather than from the state file's own
 * versions[] list, because it is the disk that holds the bytes and the
 * two can drift: IMAGE_MAX_VERSION_HISTORY caps the recorded list at
 * 256 while image_record_version() keeps repointing current_version
 * past that bound, so a long-lived image can own directories its own
 * history no longer names. A collector that trusted the list would
 * walk straight past exactly the versions most worth reclaiming.
 *
 * ".staging.<pid>" leftovers are skipped by the leading-dot rule, the
 * same convention image_delete() already relies on.
 */
int image_ondisk_versions(const char *name, char out[][IMAGE_VERSION_MAX], int max)
{
	char image_dir[PATH_MAX];
	DIR *d;
	struct dirent *ent;
	int count = 0;

	if (!image_name_is_valid(name))
		return 0;
	snprintf(image_dir, sizeof(image_dir), "%s/%s", g_images_dir, name);
	d = opendir(image_dir);
	if (d == NULL)
		return 0;
	while (count < max && (ent = readdir(d)) != NULL) {
		char rootfs[PATH_MAX];
		struct stat st;

		if (ent->d_name[0] == '.')
			continue;
		/* A version directory is one that actually holds a rootfs.
		 * manifest.json and any other bookkeeping file sitting beside
		 * the version directories is skipped by this test rather than
		 * by name, so a future sibling file needs no change here. */
		snprintf(rootfs, sizeof(rootfs), "%s/%s/rootfs", image_dir, ent->d_name);
		if (stat(rootfs, &st) == 0 && S_ISDIR(st.st_mode))
			snprintf(out[count++], IMAGE_VERSION_MAX, "%s", ent->d_name);
	}
	closedir(d);
	return count;
}

/*
 * Deletes one version: its rootfs, its version directory, and its entry
 * in the recorded history.
 *
 * Refuses the current version outright. That is not a convenience
 * check -- current_version is what a fresh container is created from,
 * so removing it turns every subsequent create into "image rootfs does
 * not exist" with nothing to point at the cause.
 *
 * Subvolume-aware for the same reason image_delete() is (ADR-0207): on
 * btrfs a version rootfs is a subvolume, which rmdir() cannot remove,
 * so persist_remove_tree() alone would fail.
 */
enum image_error image_delete_version(const char *name, const char *version)
{
	struct image_state st;
	char rootfs[PATH_MAX], version_dir[PATH_MAX];
	struct stat vst;
	int i, w;

	if (!image_name_is_valid(name) || version == NULL || version[0] == '\0' ||
	    strchr(version, '/') != NULL || strcmp(version, ".") == 0 || strcmp(version, "..") == 0)
		return IMAGE_ERR_NOT_FOUND;
	if (load_state(name, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;
	if (strcmp(st.current_version, version) == 0)
		return IMAGE_ERR_PROTECTED;

	snprintf(version_dir, sizeof(version_dir), "%s/%s/%s", g_images_dir, name, version);
	if (stat(version_dir, &vst) != 0 || !S_ISDIR(vst.st_mode))
		return IMAGE_ERR_NOT_FOUND;

	image_version_rootfs_path(name, version, rootfs, sizeof(rootfs));
	cix_btrfs_subvol_delete_or_rmtree(rootfs);
	if (persist_remove_tree(version_dir) != 0)
		return IMAGE_ERR_DELETE_FAILED;

	/* Drop it from the recorded history too, so GET .../versions stops
	 * offering a version whose bytes are gone. Absent from the list is
	 * not an error: the list is capped and the disk is authoritative. */
	for (i = 0, w = 0; i < st.version_count; i++) {
		if (strcmp(st.versions[i].version, version) == 0)
			continue;
		if (w != i)
			st.versions[w] = st.versions[i];
		w++;
	}
	if (w != st.version_count) {
		st.version_count = w;
		if (save_state(name, &st) != 0)
			return IMAGE_ERR_PERSIST_FAILED;
	}
	return IMAGE_OK;
}

int image_list_names(char names[][PKG_IMAGE_NAME_MAX], int max)
{
	DIR *d;
	struct dirent *ent;
	int count = 0;

	d = opendir(g_images_dir);
	if (d == NULL)
		return 0;
	while (count < max && (ent = readdir(d)) != NULL) {
		char path[PATH_MAX];
		struct stat st;

		if (ent->d_name[0] == '.')
			continue;
		manifest_path(ent->d_name, path, sizeof(path));
		if (stat(path, &st) == 0)
			snprintf(names[count++], PKG_IMAGE_NAME_MAX, "%s", ent->d_name);
	}
	closedir(d);
	return count;
}

void image_write_json_list(struct json_writer *w)
{
	char names[IMAGE_LIST_MAX][PKG_IMAGE_NAME_MAX];
	int count = image_list_names(names, IMAGE_LIST_MAX);
	int i;

	jw_arr_open(w);
	for (i = 0; i < count; i++) {
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, names[i]);
		jw_obj_close(w);
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

enum image_error image_manifest_version_write_json(const char *name, const char *version,
                                                    struct json_writer *w)
{
	char path[PATH_MAX];
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jpackages;
	size_t i;

	if (!image_name_is_valid(name) || version == NULL || version[0] == '\0')
		return IMAGE_ERR_NOT_FOUND;
	image_version_manifest_path(name, version, path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return IMAGE_ERR_NOT_FOUND;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		return IMAGE_ERR_NOT_FOUND;
	}
	jpackages = json_object_get(root, "packages");
	jw_arr_open(w);
	if (jpackages != NULL && jpackages->type == JSON_ARRAY) {
		for (i = 0; i < jpackages->u.array.count; i++) {
			const struct json_value *item = jpackages->u.array.items[i];
			const char *package = json_as_string(json_object_get(item, "package"));
			const char *pkg_version = json_as_string(json_object_get(item, "version"));

			if (package == NULL || pkg_version == NULL)
				continue;
			jw_obj_open(w);
			jw_key(w, "package");
			jw_str(w, package);
			jw_key(w, "version");
			jw_str(w, pkg_version);
			jw_obj_close(w);
		}
	}
	jw_arr_close(w);
	json_free(root);
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
