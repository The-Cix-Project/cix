#include "container.h"
#include "internal.h"
#include "linux_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Distinct negative return codes per failure branch (OVERLAY_ERR_*,
 * container.h), not a bare -1 -- overlay_create()'s own single caller
 * (src/container.c, the clone3'd child) already maps its own ~10
 * pre-exec setup steps to distinct process exit codes so a daemon-layer
 * caller with real log-store access (pkg.c) can report specifically
 * which one failed; a bare -1 here would have collapsed six genuinely
 * different overlay-mount failure modes back into that single
 * "overlay_create" bucket. Every existing caller already only ever
 * checked `!= 0`, so this is a pure refinement, not a behavior change.
 */
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
		return OVERLAY_ERR_STAT_LOWERDIR;
	}

	if (mkdir(ov->upperdir, 0755) != 0 && errno != EEXIST) {
		perror("overlay_create: mkdir(upperdir)");
		return OVERLAY_ERR_MKDIR_UPPERDIR;
	}

	/*
	 * Real ext4 project-quota tagging (Part 4, ADR-0062): FS_IOC_FSSETXATTR
	 * with fsx_projid set and FS_XFLAG_PROJINHERIT on -- the modern,
	 * XFS-originated project-quota API ext4 also implements, distinct
	 * from the older, deprecated single-flags-word FS_IOC_SETFLAGS/
	 * FS_PROJINHERIT_FL pair. PROJINHERIT is not optional: without it,
	 * only upperdir itself would carry the project id, and every file
	 * a container later creates *inside* it (the overlay's whole reason
	 * to exist) would carry no project id at all, silently exempting
	 * all real container disk usage from the very quota this call
	 * exists to enforce. Deliberately fails loud (unlike, say,
	 * cgroup_enable_controllers()'s own best-effort posture): a quota
	 * that was requested but silently not applied is a correctness bug
	 * wearing a false promise, not a missing optional capability.
	 */
	if (ov->project_id != 0) {
		int fd = open(ov->upperdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		struct kx_fsxattr fsx;

		if (fd < 0) {
			perror("overlay_create: open(upperdir) for project-quota tagging");
			return OVERLAY_ERR_QUOTA;
		}
		if (ioctl(fd, KX_FS_IOC_FSGETXATTR, &fsx) != 0) {
			perror("overlay_create: FS_IOC_FSGETXATTR");
			close(fd);
			return OVERLAY_ERR_QUOTA;
		}
		fsx.fsx_projid = ov->project_id;
		fsx.fsx_xflags |= KX_FS_XFLAG_PROJINHERIT;
		if (ioctl(fd, KX_FS_IOC_FSSETXATTR, &fsx) != 0) {
			perror("overlay_create: FS_IOC_FSSETXATTR");
			close(fd);
			return OVERLAY_ERR_QUOTA;
		}
		close(fd);
	}

	if (mkdir(ov->workdir, 0700) != 0 && errno != EEXIST) {
		perror("overlay_create: mkdir(workdir)");
		return OVERLAY_ERR_MKDIR_WORKDIR;
	}

	if (mkdir(ov->merged, 0755) != 0 && errno != EEXIST) {
		perror("overlay_create: mkdir(merged)");
		return OVERLAY_ERR_MKDIR_MERGED;
	}

	if (snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s",
	             ov->lowerdir, ov->upperdir, ov->workdir) >= (int)sizeof(opts)) {
		errno = ENAMETOOLONG;
		return OVERLAY_ERR_OPTS_TOO_LONG;
	}

	if (mount("overlay", ov->merged, "overlay", 0, opts) != 0) {
		/*
		 * The real mount(2) errno (EINVAL/ENOSPC/ENODEV/E2BIG/...) is
		 * the single most useful piece of information overlay_create()
		 * can hand back here -- e.g. EINVAL is the classic symptom of
		 * lowerdir's filesystem not returning real d_type from
		 * readdir(), a well-known overlayfs mount precondition this
		 * project had never actually had a live counter-example for
		 * before. Encoded directly (OVERLAY_ERR_MOUNT_ERRNO_BASE -
		 * errno, container.h) rather than collapsed into the same
		 * generic bucket every other overlay_create() failure gets,
		 * since container.c's caller has no other channel back to a
		 * daemon-layer diagnostic than this function's own return
		 * value -> process exit code.
		 */
		int mount_errno = errno;

		perror("overlay_create: mount(overlay)");
		if (mount_errno > 0 && mount_errno <= OVERLAY_ERR_MOUNT_ERRNO_MAX)
			return OVERLAY_ERR_MOUNT_ERRNO_BASE - mount_errno;
		return OVERLAY_ERR_MOUNT_OVERLAY;
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
