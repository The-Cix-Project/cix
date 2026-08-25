#include "container.h"
#include "internal.h"
#include "linux_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

int overlay_backing_is_btrfs(const char *path)
{
	struct statfs sf;

	if (statfs(path, &sf) != 0)
		return 0;
	return sf.f_type == CIX_BTRFS_SUPER_MAGIC;
}

/*
 * Creates upperdir as a real btrfs subvolume (BTRFS_IOC_SUBVOL_CREATE,
 * called on parent_dir's own fd with the subvolume's leaf name --
 * btrfs subvolumes are always created relative to an existing parent
 * directory on the same filesystem, never by an absolute path of
 * their own) instead of overlay_create()'s usual plain mkdir(). If
 * quota_bytes is nonzero, also enables btrfs quotas on this
 * filesystem (BTRFS_IOC_QUOTA_CTL, idempotent -- EINVAL if already
 * enabled is not treated as failure, same "already in the state we
 * wanted" tolerance devicemap.c/diskrole.c's own idempotent-create
 * paths already use elsewhere in this codebase) and sets a real qgroup
 * hard limit on the new subvolume (BTRFS_IOC_QGROUP_LIMIT with
 * qgroupid=0, meaning "the subvolume owning the fd this ioctl is
 * called on" -- btrfs's own documented convention for addressing a
 * subvolume by fd rather than by its kernel-assigned numeric id,
 * avoiding any need for this project to look that id up at all).
 * Returns 0, or a negative OVERLAY_ERR_* code (errno set) -- exactly
 * the same contract overlay_create()'s existing ext4 quota-tagging
 * block already has.
 */
static int overlay_create_btrfs_upperdir(const char *upperdir, long long quota_bytes)
{
	char parent_dir[PATH_MAX];
	const char *leaf;
	const char *slash = strrchr(upperdir, '/');
	int parent_fd, upper_fd;
	struct cix_btrfs_ioctl_vol_args vol_args;

	if (slash == NULL || slash == upperdir) {
		errno = EINVAL;
		return OVERLAY_ERR_MKDIR_UPPERDIR;
	}
	snprintf(parent_dir, sizeof(parent_dir), "%.*s", (int)(slash - upperdir), upperdir);
	leaf = slash + 1;

	parent_fd = open(parent_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent_fd < 0) {
		perror("overlay_create: open(parent) for btrfs subvolume create");
		return OVERLAY_ERR_MKDIR_UPPERDIR;
	}

	memset(&vol_args, 0, sizeof(vol_args));
	snprintf(vol_args.name, sizeof(vol_args.name), "%s", leaf);
	if (ioctl(parent_fd, CIX_BTRFS_IOC_SUBVOL_CREATE, &vol_args) != 0) {
		/* EEXIST: a previous run already created this subvolume
		 * (the container is being recreated after a crash/restart
		 * before cleanup) -- same tolerant convention the plain
		 * mkdir(upperdir) path below already has. */
		if (errno != EEXIST) {
			perror("overlay_create: BTRFS_IOC_SUBVOL_CREATE");
			close(parent_fd);
			return OVERLAY_ERR_MKDIR_UPPERDIR;
		}
	}
	close(parent_fd);

	if (quota_bytes <= 0)
		return 0;

	upper_fd = open(upperdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (upper_fd < 0) {
		perror("overlay_create: open(upperdir) for btrfs qgroup limit");
		return OVERLAY_ERR_QUOTA;
	}

	{
		struct cix_btrfs_ioctl_quota_ctl_args qc;

		memset(&qc, 0, sizeof(qc));
		qc.cmd = CIX_BTRFS_QUOTA_CTL_ENABLE;
		if (ioctl(upper_fd, CIX_BTRFS_IOC_QUOTA_CTL, &qc) != 0 && errno != EINVAL) {
			perror("overlay_create: BTRFS_IOC_QUOTA_CTL (enable)");
			close(upper_fd);
			return OVERLAY_ERR_QUOTA;
		}
	}

	{
		struct cix_btrfs_ioctl_qgroup_limit_args ql;

		memset(&ql, 0, sizeof(ql));
		ql.qgroupid = 0; /* the subvolume owning upper_fd itself */
		ql.lim.flags = CIX_BTRFS_QGROUP_LIMIT_MAX_RFER | CIX_BTRFS_QGROUP_LIMIT_MAX_EXCL;
		ql.lim.max_rfer = (uint64_t)quota_bytes;
		ql.lim.max_excl = (uint64_t)quota_bytes;
		if (ioctl(upper_fd, CIX_BTRFS_IOC_QGROUP_LIMIT, &ql) != 0) {
			perror("overlay_create: BTRFS_IOC_QGROUP_LIMIT");
			close(upper_fd);
			return OVERLAY_ERR_QUOTA;
		}
	}

	close(upper_fd);
	return 0;
}

/*
 * Real ext4 project-quota tagging (Part 4, ADR-0062; extended to cover
 * workdir too, issue #34): FS_IOC_FSSETXATTR with fsx_projid set and
 * FS_XFLAG_PROJINHERIT on -- the modern, XFS-originated project-quota
 * API ext4 also implements, distinct from the older, deprecated
 * single-flags-word FS_IOC_SETFLAGS/FS_PROJINHERIT_FL pair. PROJINHERIT
 * is not optional on upperdir: without it, only upperdir itself would
 * carry the project id, and every file a container later creates
 * *inside* it (the overlay's whole reason to exist) would carry no
 * project id at all, silently exempting all real container disk usage
 * from the very quota this call exists to enforce. Deliberately fails
 * loud (unlike, say, cgroup_enable_controllers()'s own best-effort
 * posture): a quota that was requested but silently not applied is a
 * correctness bug wearing a false promise, not a missing optional
 * capability. `what` names which one ("upperdir"/"workdir") for the
 * diag message -- the two call sites below are otherwise identical.
 */
int overlay_tag_project_id(const char *dir, unsigned int project_id, const char *what)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	struct cix_fsxattr fsx;

	if (fd < 0) {
		fprintf(stderr, "overlay_create: open(%s) for project-quota tagging: %s\n", what,
		        strerror(errno));
		return OVERLAY_ERR_QUOTA;
	}
	if (ioctl(fd, CIX_FS_IOC_FSGETXATTR, &fsx) != 0) {
		fprintf(stderr, "overlay_create: FS_IOC_FSGETXATTR(%s): %s\n", what, strerror(errno));
		close(fd);
		return OVERLAY_ERR_QUOTA;
	}
	fsx.fsx_projid = project_id;
	fsx.fsx_xflags |= CIX_FS_XFLAG_PROJINHERIT;
	if (ioctl(fd, CIX_FS_IOC_FSSETXATTR, &fsx) != 0) {
		fprintf(stderr, "overlay_create: FS_IOC_FSSETXATTR(%s): %s\n", what, strerror(errno));
		close(fd);
		return OVERLAY_ERR_QUOTA;
	}
	close(fd);
	return 0;
}

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
/*
 * Everything overlay_create() does EXCEPT the final mount(2): stat lowerdir,
 * create+quota-tag upperdir (btrfs subvolume or ext4 dir), create+quota-tag
 * workdir, create the merged mountpoint. Split out (ADR-0179 phase 2c) so the
 * userns path can prepare the same dirs and then build an *id-mapped* overlay
 * via the fd-based mount API instead of the classic mount() overlay_create
 * still does. Returns 0 or a negative OVERLAY_ERR_* (errno set).
 */
int overlay_prepare_dirs(const struct overlay_spec *ov)
{
	struct stat st;

	/*
	 * lowerdir is the shared image tree and must already exist and be
	 * populated -- silently creating a missing one would produce a
	 * silently-empty, broken container instead of a loud failure.
	 */
	if (stat(ov->lowerdir, &st) != 0) {
		perror("overlay_create: stat(lowerdir)");
		return OVERLAY_ERR_STAT_LOWERDIR;
	}

	/*
	 * Backing-filesystem branch (ADR-0103): btrfs has no quotactl(2)
	 * project-quota support at all, so upperdir itself has to be a real
	 * subvolume there (overlay_create_btrfs_upperdir()) with any quota
	 * enforced via qgroups instead. Detected against upperdir's own
	 * *parent* directory, not upperdir itself -- upperdir doesn't exist
	 * yet at this point in either branch, so statfs(upperdir) would
	 * always fail and silently misroute a btrfs container onto the
	 * ext4 mkdir path below.
	 */
	{
		char parent_dir[PATH_MAX];
		const char *slash = strrchr(ov->upperdir, '/');
		int on_btrfs;

		if (slash != NULL && slash != ov->upperdir) {
			snprintf(parent_dir, sizeof(parent_dir), "%.*s",
			         (int)(slash - ov->upperdir), ov->upperdir);
			on_btrfs = overlay_backing_is_btrfs(parent_dir);
		} else {
			on_btrfs = overlay_backing_is_btrfs(ov->upperdir);
		}

		if (on_btrfs) {
			int rc = overlay_create_btrfs_upperdir(ov->upperdir, ov->quota_bytes);

			if (rc != 0)
				return rc;
		} else {
			if (mkdir(ov->upperdir, 0755) != 0 && errno != EEXIST) {
				perror("overlay_create: mkdir(upperdir)");
				return OVERLAY_ERR_MKDIR_UPPERDIR;
			}

			if (ov->project_id != 0) {
				int rc = overlay_tag_project_id(ov->upperdir, ov->project_id, "upperdir");

				if (rc != 0)
					return rc;
			}
		}
	}

	if (mkdir(ov->workdir, 0700) != 0 && errno != EEXIST) {
		perror("overlay_create: mkdir(workdir)");
		return OVERLAY_ERR_MKDIR_WORKDIR;
	}

	/*
	 * Issue #34's own real root cause: workdir must carry the identical
	 * project id upperdir just got tagged with, not just upperdir alone.
	 * Overlayfs's own kernel implementation copies a new object up from
	 * lowerdir by first creating it IN workdir, then renaming it into
	 * its final place in upperdir -- untagged workdir renaming into a
	 * PROJINHERIT-tagged upperdir is exactly the kind of rename ext4's
	 * own project-quota implementation refuses outright, with EXDEV,
	 * since it would silently change the moved object's project id
	 * without real quota-accounting bookkeeping for that move. Every
	 * fresh-every-start mount point mountns_pivot() creates under a
	 * lowerdir-only parent directory (confirmed live: mkdir(/dev/pts),
	 * since a real image's own /dev never gets copied up during
	 * ordinary use) needs this exact copy-up-then-rename dance, so this
	 * bug reliably hit any container created with a real disk quota --
	 * which, since ADR/Part 183, every container the intended CLI/web
	 * flow creates now has by default. Confirmed precisely, not
	 * guessed: mountns_pivot()'s own new per-step diagnostics (a
	 * separate, earlier fix) pinpointed mkdir(/dev/pts) as the exact
	 * failing call, and a live A/B test (identical container, only
	 * disk_quota_bytes present or absent) reproduced/cleared the exact
	 * same failure on demand.
	 */
	if (ov->project_id != 0) {
		int rc = overlay_tag_project_id(ov->workdir, ov->project_id, "workdir");

		if (rc != 0)
			return rc;
	}

	if (mkdir(ov->merged, 0755) != 0 && errno != EEXIST) {
		perror("overlay_create: mkdir(merged)");
		return OVERLAY_ERR_MKDIR_MERGED;
	}

	return 0;
}

int overlay_create(const struct overlay_spec *ov)
{
	char opts[3 * PATH_MAX + 64];
	int prep = overlay_prepare_dirs(ov);

	if (prep != 0)
		return prep;

	/*
	 * redirect_dir=on was tried here first for issue #34 (a real,
	 * documented overlayfs behavior that looked like a plausible match
	 * for the mkdir(/dev/pts) EXDEV mountns_pivot()'s own per-step
	 * diagnostics had just pinpointed) but proved both unnecessary
	 * (overlay_tag_project_id() on workdir, added right after this comment
	 * was written, is what actually fixed the EXDEV) and actively
	 * harmful on this project's own from-scratch 6.18.40 kernel build,
	 * which was never compiled with CONFIG_OVERLAY_FS_REDIRECT_DIR=y:
	 * confirmed live, precisely, that forcing the mount option on
	 * anyway broke lowerdir content visibility outright afterward
	 * (execve() ENOENT for a binary confirmed present in the image,
	 * for every binary tried, not just one) -- reverted here rather
	 * than left in as a "shouldn't hurt" leftover.
	 */
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
