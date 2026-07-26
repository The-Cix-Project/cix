#include "container.h"
#include "internal.h"

#include <errno.h>
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
