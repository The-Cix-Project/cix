#include "image.h"
#include "json.h"
#include "namecheck.h"
#include "persist.h"
#include "pkg.h"
#include "registry.h"
#include "btrfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

/* ADR-0343: see image_set_discard() in image.h. */
static image_discard_fn g_discard;

void image_set_discard(image_discard_fn fn)
{
	g_discard = fn;
}

struct subdir_name {
	char name[NAME_MAX + 1];
};

/*
 * Every directory directly inside `dir`, dot-named ones included, into a
 * malloc'd array the caller frees. Listed before anything is moved, so
 * the caller never renames entries out of a directory it is reading.
 * Returns the count, 0 when `dir` does not exist, -1 on error.
 */
static int list_subdirs(const char *dir, struct subdir_name **out)
{
	DIR *d;
	struct dirent *e;
	struct subdir_name *v = NULL;
	int count = 0, cap = 0;

	*out = NULL;
	d = opendir(dir);
	if (d == NULL)
		return errno == ENOENT ? 0 : -1;
	while ((e = readdir(d)) != NULL) {
		char path[PATH_MAX];
		struct stat st;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path) ||
		    lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		if (count == cap) {
			struct subdir_name *nv;

			cap = cap == 0 ? 16 : cap * 2;
			nv = realloc(v, (size_t)cap * sizeof(*v));
			if (nv == NULL) {
				free(v);
				closedir(d);
				return -1;
			}
			v = nv;
		}
		snprintf(v[count++].name, sizeof(v[0].name), "%s", e->d_name);
	}
	closedir(d);
	*out = v;
	return count;
}

/*
 * ADR-0343: empties one version directory without deleting anything
 * large on this thread, then removes it. Every directory inside -- its
 * rootfs, and any rootfs.replaced-* (#551), each a subvolume on btrfs --
 * goes to g_discard. What is left is files, removed here: the package
 * snapshot pkg.c writes beside each version (#398) is the one there is.
 *
 * Never the version directory as a whole. On btrfs, SNAP_DESTROY fails
 * on a plain directory and cix_btrfs_subvol_delete_or_rmtree() falls
 * back to a file-by-file rmtree. That is unlink, the call #619 measured
 * as slow: 1219 ms for 386 calls into jumpbox on 192.168.15.95,
 * 2026-10-20.
 */
static enum image_error set_aside_version_dir(const char *name, const char *version_dir)
{
	struct subdir_name *subdirs = NULL;
	DIR *d;
	struct dirent *e;
	int n, i, failed = 0;

	if (g_discard == NULL)
		return IMAGE_ERR_DELETE_FAILED;
	n = list_subdirs(version_dir, &subdirs);
	if (n < 0)
		return IMAGE_ERR_DELETE_FAILED;
	for (i = 0; i < n && !failed; i++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s", version_dir, subdirs[i].name);
		failed = g_discard(name, path) != 0;
	}
	free(subdirs);
	if (failed)
		return IMAGE_ERR_DELETE_FAILED;

	d = opendir(version_dir);
	if (d == NULL)
		return IMAGE_ERR_DELETE_FAILED;
	while ((e = readdir(d)) != NULL) {
		char path[PATH_MAX];

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", version_dir, e->d_name);
		if (unlink(path) != 0)
			failed = 1;
	}
	closedir(d);
	if (failed || rmdir(version_dir) != 0)
		return IMAGE_ERR_DELETE_FAILED;
	return IMAGE_OK;
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
	/*
	 * ADR-0333: nothing is seeded into it. A new image is an EMPTY
	 * tree, and its content arrives only by installing packages.
	 *
	 * This used to call pkg_seed_image_baseline(), which wrote device
	 * nodes, nsswitch.conf, a CA bundle, os-release and two directories
	 * -- making the platform a writer of image content beside the
	 * package manager, unversioned and reaching only images created
	 * after a change (#577). Every one of those was per-container state
	 * and now arrives per container.
	 *
	 * An image with nothing in it was already the norm for the thing
	 * that matters most: ADR-0216 closed the glibc floor, so a fresh
	 * image has no C library and POST /v1/containers refuses it by
	 * name. This extends that to everything.
	 */

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
	 * ADR-0343: every tree goes through the discard queue, so nothing
	 * large is deleted here. This used to destroy each version's rootfs
	 * subvolume inline (ADR-0207 phase 1) -- on the event loop, for every
	 * build environment a finished build released. A version directory
	 * is emptied by set_aside_version_dir(); any other directory (a
	 * .staging leftover is a subvolume) is set aside whole. What
	 * persist_remove_tree() then meets is files and empty directories.
	 */
	{
		struct subdir_name *subdirs = NULL;
		int n, i, failed = 0;

		n = list_subdirs(image_dir, &subdirs);
		if (n < 0)
			return IMAGE_ERR_DELETE_FAILED;
		for (i = 0; i < n && !failed; i++) {
			char tree[PATH_MAX], rootfs[PATH_MAX];
			struct stat rst;

			snprintf(tree, sizeof(tree), "%s/%s", image_dir, subdirs[i].name);
			snprintf(rootfs, sizeof(rootfs), "%s/rootfs", tree);
			if (lstat(rootfs, &rst) == 0 && S_ISDIR(rst.st_mode))
				failed = set_aside_version_dir(name, tree) != IMAGE_OK;
			else
				failed = g_discard == NULL || g_discard(name, tree) != 0;
		}
		free(subdirs);
		if (failed)
			return IMAGE_ERR_DELETE_FAILED;
	}

	if (persist_remove_tree(image_dir) != 0)
		return IMAGE_ERR_DELETE_FAILED;

	return IMAGE_OK;
}

/*
 * ADR-0343: a version directory's own age, for a version the recorded
 * history does not name. Birth time where the filesystem keeps one,
 * since the copy-forward rebuild touches directory metadata (the reason
 * created_at is explicit, ADR-0108); mtime otherwise. Nanoseconds, so
 * versions made within one second still order.
 */
static long long version_dir_birth_ns(const char *version_dir)
{
	struct statx sx;
	struct stat st;

	if (statx(AT_FDCWD, version_dir, AT_SYMLINK_NOFOLLOW, STATX_BTIME | STATX_MTIME, &sx) == 0) {
		if (sx.stx_mask & STATX_BTIME)
			return (long long)sx.stx_btime.tv_sec * 1000000000LL + sx.stx_btime.tv_nsec;
		if (sx.stx_mask & STATX_MTIME)
			return (long long)sx.stx_mtime.tv_sec * 1000000000LL + sx.stx_mtime.tv_nsec;
	}
	if (stat(version_dir, &st) == 0)
		return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
	return 0;
}

/* One version a plan is deciding about, with the key it orders by. */
struct retain_work {
	struct image_retain_entry e;
	long long key;
};

/* Oldest first. Equal keys order by version, so a plan is the same
 * every time it is made from the same state. */
static int retain_work_cmp(const void *a, const void *b)
{
	const struct retain_work *x = a, *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return strcmp(x->e.v.version, y->e.v.version);
}

enum image_error image_retain_plan(const char *name, int keep, image_referenced_fn referenced,
                                   void *ctx, struct image_retain_plan *out)
{
	struct image_state st;
	struct subdir_name *subdirs = NULL;
	struct retain_work *work;
	char image_dir[PATH_MAX];
	int n, i, j, unreferenced_kept = 0;

	memset(out, 0, sizeof(*out));
	if (!image_name_is_valid(name) || load_state(name, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;
	snprintf(out->image, sizeof(out->image), "%s", name);
	snprintf(image_dir, sizeof(image_dir), "%s/%s", g_images_dir, name);
	n = list_subdirs(image_dir, &subdirs);
	if (n < 0)
		return IMAGE_ERR_NOT_FOUND;
	work = calloc(n > 0 ? (size_t)n : 1, sizeof(*work));
	if (work == NULL) {
		free(subdirs);
		return IMAGE_ERR_PERSIST_FAILED;
	}

	/*
	 * Every version on disk. A version directory is one that holds a
	 * rootfs; a ".staging" leftover is skipped by the leading dot,
	 * and manifest.json is not a directory at all.
	 *
	 * A recorded version keeps its created_at, and its history index
	 * breaks a tie within one second, which is the order it was recorded
	 * in. An unrecorded one -- on .95, the newest of an image whose
	 * history stopped growing at IMAGE_MAX_VERSION_HISTORY -- is dated
	 * from its directory, and the plan says it repaired something.
	 */
	for (i = 0; i < n; i++) {
		char version_dir[PATH_MAX], rootfs[PATH_MAX];
		struct stat rst;
		struct retain_work *w = &work[out->count];

		if (subdirs[i].name[0] == '.' || strlen(subdirs[i].name) >= IMAGE_VERSION_MAX)
			continue;
		snprintf(version_dir, sizeof(version_dir), "%s/%s", image_dir, subdirs[i].name);
		snprintf(rootfs, sizeof(rootfs), "%s/rootfs", version_dir);
		if (lstat(rootfs, &rst) != 0 || !S_ISDIR(rst.st_mode))
			continue;
		snprintf(w->e.v.version, sizeof(w->e.v.version), "%s", subdirs[i].name);
		for (j = 0; j < st.version_count; j++)
			if (strcmp(st.versions[j].version, subdirs[i].name) == 0)
				break;
		if (j < st.version_count) {
			w->e.v.created_at = st.versions[j].created_at;
			w->key = (long long)st.versions[j].created_at * 1000000000LL + j;
		} else {
			w->key = version_dir_birth_ns(version_dir);
			w->e.v.created_at = (long)(w->key / 1000000000LL);
			out->repaired = 1;
		}
		out->count++;
	}
	free(subdirs);
	if (out->count != st.version_count)
		out->repaired = 1; /* a recorded version has no directory */
	qsort(work, (size_t)out->count, sizeof(*work), retain_work_cmp);

	/* Newest first: what something needs, then the policy's `keep`. */
	for (i = out->count - 1; i >= 0; i--) {
		const char *v = work[i].e.v.version;

		if (strcmp(v, st.current_version) == 0 ||
		    (referenced != NULL && referenced(name, v, ctx)))
			continue;
		if (unreferenced_kept < keep) {
			unreferenced_kept++;
			continue;
		}
		work[i].e.drop = 1;
	}

	out->entries = calloc(out->count > 0 ? (size_t)out->count : 1, sizeof(*out->entries));
	if (out->entries == NULL) {
		free(work);
		out->count = 0;
		return IMAGE_ERR_PERSIST_FAILED;
	}
	for (i = 0; i < out->count; i++) {
		out->entries[i] = work[i].e;
		if (work[i].e.drop)
			out->dropped++;
		else
			out->kept++;
	}
	free(work);
	return IMAGE_OK;
}

void image_retain_plan_free(struct image_retain_plan *plan)
{
	free(plan->entries);
	plan->entries = NULL;
	plan->count = 0;
}

/*
 * Refuses the current version outright. That is not a convenience
 * check -- current_version is what a fresh container is created from,
 * so removing it turns every subsequent create into "image rootfs does
 * not exist" with nothing to point at the cause.
 */
enum image_error image_version_set_aside(const char *name, const char *version)
{
	struct image_state st;
	char version_dir[PATH_MAX];
	struct stat vst;

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
	return set_aside_version_dir(name, version_dir);
}

enum image_error image_retain_commit(const struct image_retain_plan *plan)
{
	struct image_state st;
	int i, start = 0, count = 0, same;

	if (load_state(plan->image, &st) != 0)
		return IMAGE_ERR_NOT_FOUND;

	/*
	 * What survives, in the plan's order. A dropped entry stays recorded
	 * if its directory is still there -- its set-aside failed -- so the
	 * history never forgets a version that exists. Past the array's
	 * bound the oldest are left unrecorded, and the next plan repairs
	 * them from the disk again rather than losing them.
	 */
	{
		struct image_version_entry kept[IMAGE_MAX_VERSION_HISTORY];
		int survivors = 0;

		for (i = 0; i < plan->count; i++) {
			char version_dir[PATH_MAX];
			struct stat vst;

			if (plan->entries[i].drop) {
				snprintf(version_dir, sizeof(version_dir), "%s/%s/%s", g_images_dir,
				         plan->image, plan->entries[i].v.version);
				if (stat(version_dir, &vst) != 0)
					continue;
			}
			survivors++;
		}
		if (survivors > IMAGE_MAX_VERSION_HISTORY)
			start = survivors - IMAGE_MAX_VERSION_HISTORY;
		survivors = 0;
		for (i = 0; i < plan->count; i++) {
			char version_dir[PATH_MAX];
			struct stat vst;

			if (plan->entries[i].drop) {
				snprintf(version_dir, sizeof(version_dir), "%s/%s/%s", g_images_dir,
				         plan->image, plan->entries[i].v.version);
				if (stat(version_dir, &vst) != 0)
					continue;
			}
			if (survivors++ < start)
				continue;
			kept[count++] = plan->entries[i].v;
		}

		same = count == st.version_count;
		for (i = 0; same && i < count; i++)
			same = strcmp(kept[i].version, st.versions[i].version) == 0 &&
			       kept[i].created_at == st.versions[i].created_at;
		if (same)
			return IMAGE_OK;
		memcpy(st.versions, kept, (size_t)count * sizeof(kept[0]));
		st.version_count = count;
	}
	return save_state(plan->image, &st) == 0 ? IMAGE_OK : IMAGE_ERR_PERSIST_FAILED;
}

/*
 * ADR-0343: images that got a version since the automatic pass last ran.
 * In memory only; the pass at daemon start covers whatever a restart
 * drops.
 */
static char g_retain_marked[IMAGE_LIST_MAX][PKG_IMAGE_NAME_MAX];
static int g_retain_marked_count;

static void retain_mark(const char *name)
{
	int i;

	for (i = 0; i < g_retain_marked_count; i++)
		if (strcmp(g_retain_marked[i], name) == 0)
			return;
	if (g_retain_marked_count < IMAGE_LIST_MAX)
		snprintf(g_retain_marked[g_retain_marked_count++], PKG_IMAGE_NAME_MAX, "%s", name);
}

int image_retain_take_marked(char names[][PKG_IMAGE_NAME_MAX], int max)
{
	int i, n = g_retain_marked_count < max ? g_retain_marked_count : max;

	for (i = 0; i < n; i++)
		snprintf(names[i], PKG_IMAGE_NAME_MAX, "%s", g_retain_marked[i]);
	g_retain_marked_count = 0;
	return n;
}

/*
 * Does this name refer to a real image? The one definition: a valid
 * name whose manifest.json is there -- image_create() writes that file
 * last precisely so its presence is the "this exists" signal.
 *
 * It had two other copies, spelled identically, in the listing below
 * and in image_write_json_one(); they are now this one call, so the
 * listing and any other caller cannot come to disagree about what an
 * image is.
 */
int image_exists(const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	if (!image_name_is_valid(name))
		return 0;
	manifest_path(name, path, sizeof(path));
	return stat(path, &st) == 0;
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
		if (ent->d_name[0] == '.')
			continue;
		if (image_exists(ent->d_name))
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
	if (!image_exists(name))
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

	/* ADR-0343: either way the old current version may now be past what
	 * the image keeps; the automatic pass looks once no job is running. */
	retain_mark(name);

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
	 * the real, currently-active version, and the next retention pass
	 * repairs the history from the disk (ADR-0343). Not a failure of
	 * whatever install/rebuild triggered this call. */
	snprintf(st.current_version, sizeof(st.current_version), "%s", version);
	return save_state(name, &st) == 0 ? IMAGE_OK : IMAGE_ERR_PERSIST_FAILED;
}

/*
 * #551: deletes the stored trees image_produce_new_version() set aside
 * when it found one wrong -- IMAGES_DIR/<image>/<version>/rootfs.replaced-*.
 * They were kept only because a running container might still have one
 * mounted as its lower layer. Called at startup, before any container
 * starts, which is the first moment nothing can. And the staging trees
 * of versions a producer never finished (ADR-0356). Returns how many it
 * removed.
 */
int image_sweep_replaced_trees(void)
{
	DIR *images;
	struct dirent *ide;
	int removed = 0;

	images = opendir(g_images_dir);
	if (images == NULL)
		return 0;
	while ((ide = readdir(images)) != NULL) {
		char image_dir[PATH_MAX];
		DIR *versions;
		struct dirent *vde;

		if (ide->d_name[0] == '.')
			continue;
		if (snprintf(image_dir, sizeof(image_dir), "%s/%s", g_images_dir, ide->d_name) >=
		    (int)sizeof(image_dir))
			continue;
		versions = opendir(image_dir);
		if (versions == NULL)
			continue;
		while ((vde = readdir(versions)) != NULL) {
			char version_dir[PATH_MAX];
			DIR *entries;
			struct dirent *ede;

			/*
			 * ADR-0356: a version a producer was building when the
			 * daemon stopped. Nothing is producing one while this runs
			 * (main() calls it before the loop, after the sandbox
			 * migration has finished), so any here is a leftover --
			 * including one spelled
			 * .staging.<pid>, as it was before, which nothing else would
			 * ever look for again.
			 */
			if (strncmp(vde->d_name, ".staging", 8) == 0) {
				char path[PATH_MAX];

				if (snprintf(path, sizeof(path), "%s/%s", image_dir, vde->d_name) <
				        (int)sizeof(path) &&
				    cix_btrfs_subvol_delete_or_rmtree(path) == 0)
					removed++;
				continue;
			}
			if (vde->d_name[0] == '.')
				continue;
			if (snprintf(version_dir, sizeof(version_dir), "%s/%s", image_dir, vde->d_name) >=
			    (int)sizeof(version_dir))
				continue;
			entries = opendir(version_dir);
			if (entries == NULL)
				continue;
			while ((ede = readdir(entries)) != NULL) {
				char path[PATH_MAX];

				if (strncmp(ede->d_name, "rootfs.replaced-", 16) != 0)
					continue;
				if (snprintf(path, sizeof(path), "%s/%s", version_dir, ede->d_name) >=
				    (int)sizeof(path))
					continue;
				if (cix_btrfs_subvol_delete_or_rmtree(path) == 0)
					removed++;
				else
					fprintf(stderr, "image: could not remove replaced tree %s\n", path);
			}
			closedir(entries);
		}
		closedir(versions);
	}
	closedir(images);
	return removed;
}
