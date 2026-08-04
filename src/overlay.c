#include "container.h"
#include "internal.h"

#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

int overlay_create(const struct overlay_spec *ov)
{
	struct stat st;
	char opts[3 * PATH_MAX + 64];

	/*
	 * lowerdir is the shared image tree and must already exist and be
	 * populated -- silently creating a missing one would produce a
	 * silently-empty, broken container instead of a loud failure.
	 */
	if (stat(ov->lowerdir, &st) != 0) {
		perror("overlay_create: stat(lowerdir)");
		return -1;
	}

	if (mkdir(ov->upperdir, 0755) != 0 && errno != EEXIST) {
		perror("overlay_create: mkdir(upperdir)");
		return -1;
	}

	if (mkdir(ov->workdir, 0700) != 0 && errno != EEXIST) {
		perror("overlay_create: mkdir(workdir)");
		return -1;
	}

	if (mkdir(ov->merged, 0755) != 0 && errno != EEXIST) {
		perror("overlay_create: mkdir(merged)");
		return -1;
	}

	if (snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s",
	             ov->lowerdir, ov->upperdir, ov->workdir) >= (int)sizeof(opts)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	if (mount("overlay", ov->merged, "overlay", 0, opts) != 0) {
		perror("overlay_create: mount(overlay)");
		return -1;
	}

	return 0;
}

/*
 * nftw()'s own C signature has no user-data parameter, so the running
 * total lives here at file scope instead -- safe because this daemon
 * is single-threaded and overlay_upperdir_size() is a synchronous,
 * non-reentrant call (no epoll callback or signal handler ever calls
 * back into it mid-walk).
 */
static long long g_upperdir_size_total;

/*
 * FTW_PHYS (never follows a symlink -- an upperdir can contain
 * overlayfs whiteout entries, char devices with rdev 0:0, whose
 * st_size is always 0 and simply add nothing) walk, summing every
 * regular entry's own real on-disk size. Directory sizes themselves
 * (typically 4096, the directory's own metadata block) are excluded --
 * "disk usage" here means content a container actually wrote, not the
 * directory-tree overhead any filesystem charges to hold it.
 */
static int upperdir_size_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	(void)path;
	(void)ftwbuf;
	if (typeflag == FTW_F || typeflag == FTW_SL)
		g_upperdir_size_total += (long long)sb->st_size;
	return 0;
}

/*
 * Real space consumed by upperdir_path (a container's own overlay
 * upperdir), in bytes -- content only, per upperdir_size_cb's own
 * comment. Returns -1 (errno set by nftw()/stat()) if upperdir_path
 * can't be walked at all (e.g. a container mid-teardown); 0 with
 * *out_bytes set on success, including a genuinely empty upperdir.
 */
int overlay_upperdir_size(const char *upperdir_path, long long *out_bytes)
{
	g_upperdir_size_total = 0;
	if (nftw(upperdir_path, upperdir_size_cb, 16, FTW_PHYS) != 0)
		return -1;
	*out_bytes = g_upperdir_size_total;
	return 0;
}
