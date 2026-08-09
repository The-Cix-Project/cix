#include "image.h"
#include "namecheck.h"
#include "persist.h"
#include "pkg.h"
#include "registry.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
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
