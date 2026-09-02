#include "btrfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "linux_compat.h"

/*
 * Split "/a/b/c" into parent "/a/b" and leaf "c". Returns 0 and fills
 * both on success; -1 (errno EINVAL) if there is no '/' or the leaf is
 * empty. parent_size must hold the parent plus its NUL.
 */
static int split_parent_leaf(const char *path, char *parent, size_t parent_size,
                             const char **leaf)
{
	const char *slash = strrchr(path, '/');

	if (slash == NULL || slash[1] == '\0') {
		errno = EINVAL;
		return -1;
	}
	if ((size_t)(slash - path) >= parent_size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	snprintf(parent, parent_size, "%.*s", (int)(slash - path), path);
	*leaf = slash + 1;
	return 0;
}

int cix_btrfs_is_backing(const char *path)
{
	struct statfs sf;

	if (statfs(path, &sf) != 0)
		return 0;
	return sf.f_type == CIX_BTRFS_SUPER_MAGIC;
}

int cix_btrfs_subvol_create_or_dir(const char *path)
{
	char parent[PATH_MAX];
	const char *leaf;
	int parent_fd;
	struct cix_btrfs_ioctl_vol_args vol_args;

	if (split_parent_leaf(path, parent, sizeof(parent), &leaf) != 0)
		return -1;

	if (!cix_btrfs_is_backing(parent)) {
		/* ext4 (or anything non-btrfs): a plain directory, exactly as
		 * the image store did before ADR-0207. EEXIST is success. */
		if (mkdir(path, 0755) != 0 && errno != EEXIST)
			return -1;
		return 0;
	}

	parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent_fd < 0)
		return -1;

	memset(&vol_args, 0, sizeof(vol_args));
	snprintf(vol_args.name, sizeof(vol_args.name), "%s", leaf);
	if (ioctl(parent_fd, CIX_BTRFS_IOC_SUBVOL_CREATE, &vol_args) != 0) {
		/* EEXIST: a prior run already created it (recreate after a
		 * crash before cleanup) -- the same tolerant convention the
		 * mkdir path above and overlay's own upperdir create use. */
		int saved = errno;

		close(parent_fd);
		if (saved == EEXIST)
			return 0;
		errno = saved;
		return -1;
	}
	close(parent_fd);
	return 0;
}

/* Recursively copy a plain directory tree src -> dst (dst created
 * fresh). Self-contained so the runtime lib stays free of daemon
 * helpers; only the non-btrfs fallback path uses it. */
static int copy_tree(const char *src, const char *dst)
{
	DIR *d;
	struct dirent *de;
	struct stat st;

	if (lstat(src, &st) != 0)
		return -1;

	if (S_ISDIR(st.st_mode)) {
		if (mkdir(dst, st.st_mode & 07777) != 0 && errno != EEXIST)
			return -1;
		d = opendir(src);
		if (d == NULL)
			return -1;
		while ((de = readdir(d)) != NULL) {
			char s[PATH_MAX];
			char t[PATH_MAX];

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;
			if (snprintf(s, sizeof(s), "%s/%s", src, de->d_name) >= (int)sizeof(s) ||
			    snprintf(t, sizeof(t), "%s/%s", dst, de->d_name) >= (int)sizeof(t)) {
				closedir(d);
				errno = ENAMETOOLONG;
				return -1;
			}
			if (copy_tree(s, t) != 0) {
				closedir(d);
				return -1;
			}
		}
		closedir(d);
		return 0;
	}

	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(src, target, sizeof(target) - 1);

		if (n < 0)
			return -1;
		target[n] = '\0';
		if (symlink(target, dst) != 0 && errno != EEXIST)
			return -1;
		return 0;
	}

	if (S_ISREG(st.st_mode)) {
		int in = open(src, O_RDONLY | O_CLOEXEC);
		int out;
		char buf[65536];
		ssize_t r;

		if (in < 0)
			return -1;
		out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
		if (out < 0) {
			close(in);
			return -1;
		}
		while ((r = read(in, buf, sizeof(buf))) > 0) {
			ssize_t off = 0;

			while (off < r) {
				ssize_t w = write(out, buf + off, (size_t)(r - off));

				if (w < 0) {
					close(in);
					close(out);
					return -1;
				}
				off += w;
			}
		}
		close(in);
		close(out);
		return r < 0 ? -1 : 0;
	}

	/* Device/fifo/socket nodes: recreate by type+rdev. A seeded image
	 * rootfs legitimately carries device nodes (/dev/null etc.). */
	if (mknod(dst, st.st_mode, st.st_rdev) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

/*
 * Is `path` a btrfs SUBVOLUME, as opposed to an ordinary directory that
 * merely lives on btrfs?
 *
 * Every subvolume's root directory has inode number 256
 * (BTRFS_FIRST_FREE_OBJECTID) -- the same test btrfs-progs itself uses.
 * An ordinary directory on the same filesystem has an arbitrary inode
 * number and essentially never 256.
 *
 * This distinction is not academic: BTRFS_IOC_SNAP_CREATE_V2 fails with
 * EINVAL when handed a plain directory, and that is exactly how an
 * image store carried across a storage migration breaks (#180). The
 * migration copies content faithfully, which turns subvolumes into
 * plain directories, and nothing notices until the next snapshot.
 */
static int is_subvolume(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return 0;
	return S_ISDIR(st.st_mode) && st.st_ino == 256;
}

int cix_btrfs_snapshot_or_copy(const char *src, const char *dst)
{
	char parent[PATH_MAX];
	const char *leaf;
	int parent_fd, src_fd;
	struct cix_btrfs_ioctl_vol_args_v2 v2;

	if (!cix_btrfs_is_backing(src))
		return copy_tree(src, dst);

	/*
	 * On btrfs, but `src` is a plain directory rather than a subvolume
	 * -- so there is nothing to snapshot. Create `dst` as a real
	 * subvolume and copy into it (copy_tree tolerates an existing
	 * destination directory).
	 *
	 * That costs one full copy, once, and then the problem is gone:
	 * `dst` becomes the image's current version, it IS a subvolume, and
	 * every later version snapshots from it in O(1) as intended. The
	 * alternative -- failing, as this did before #180 -- leaves an
	 * image that can never gain another version, and since uninstall
	 * and delete both go through this same call, one that can never be
	 * repaired or removed either.
	 */
	if (!is_subvolume(src)) {
		if (cix_btrfs_subvol_create_or_dir(dst) != 0)
			return -1;
		return copy_tree(src, dst);
	}

	if (split_parent_leaf(dst, parent, sizeof(parent), &leaf) != 0)
		return -1;

	src_fd = open(src, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (src_fd < 0)
		return -1;
	parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent_fd < 0) {
		int saved = errno;

		close(src_fd);
		errno = saved;
		return -1;
	}

	memset(&v2, 0, sizeof(v2));
	v2.fd = src_fd;    /* the subvolume to snapshot */
	v2.flags = 0;      /* writable snapshot */
	snprintf(v2.name, sizeof(v2.name), "%s", leaf);
	if (ioctl(parent_fd, CIX_BTRFS_IOC_SNAP_CREATE_V2, &v2) != 0) {
		int saved = errno;

		close(src_fd);
		close(parent_fd);
		/*
		 * Deliberately NOT falling back to a copy on EXDEV (dst on a
		 * different filesystem). The container-create path relies on
		 * this failing: it then falls back to the overlay rootfs,
		 * which still shares the image through its lowerdir and costs
		 * nothing. Copying here instead would silently turn a cheap
		 * fallback into a full image copy on every such create.
		 *
		 * A caller that genuinely wants the copy -- cross-disk
		 * migration, where there is no lowerdir to share with -- asks
		 * for it explicitly via cix_btrfs_subvol_copy().
		 */
		errno = saved;
		return -1;
	}
	close(src_fd);
	close(parent_fd);
	return 0;
}

/*
 * Reproduce a subvolume at `dst` when `dst` cannot be a snapshot of
 * `src` -- because it is on a different filesystem.
 *
 * Snapshots do not cross filesystems: BTRFS_IOC_SNAP_CREATE_V2 returns
 * EXDEV and there is no cheaper correct answer. `btrfs send -p` could
 * preserve sharing only against a parent with common lineage, and an
 * image seeded independently on each disk has none, so the data is
 * copied in full and the result occupies its whole size on the target.
 * That cost is inherent to moving between filesystems, not a shortcut.
 *
 * What this preserves is the invariant callers actually depend on: the
 * result IS a subvolume. A plain directory copy would leave something
 * that looks right and cannot be snapshotted or qgroup-limited
 * afterwards -- the container would work until the first thing that
 * needed either.
 */
int cix_btrfs_subvol_copy(const char *src, const char *dst)
{
	if (cix_btrfs_subvol_create_or_dir(dst) != 0)
		return -1;
	return copy_tree(src, dst);
}

/* Recursive unlink of a plain directory tree (non-btrfs fallback, and
 * the contents of a subvolume before SNAP_DESTROY is not needed --
 * SNAP_DESTROY removes a subvolume whole). */
static int rmtree(const char *path)
{
	DIR *d;
	struct dirent *de;
	struct stat st;

	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : -1;

	if (S_ISDIR(st.st_mode)) {
		d = opendir(path);
		if (d == NULL)
			return -1;
		while ((de = readdir(d)) != NULL) {
			char child[PATH_MAX];

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;
			if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= (int)sizeof(child)) {
				closedir(d);
				errno = ENAMETOOLONG;
				return -1;
			}
			if (rmtree(child) != 0) {
				closedir(d);
				return -1;
			}
		}
		closedir(d);
		return rmdir(path);
	}
	return unlink(path);
}

int cix_btrfs_subvol_delete_or_rmtree(const char *path)
{
	char parent[PATH_MAX];
	const char *leaf;
	int parent_fd;
	struct cix_btrfs_ioctl_vol_args vol_args;
	struct stat st;

	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : -1;

	if (!cix_btrfs_is_backing(path))
		return rmtree(path);

	/*
	 * A btrfs path is a subvolume if it was created by
	 * subvol_create/snapshot above. SNAP_DESTROY removes it whole
	 * (its own CoW extents freed, shared extents untouched). If it
	 * turns out to be an ordinary directory on btrfs (never made a
	 * subvolume), SNAP_DESTROY fails and we fall back to rmtree --
	 * so the call is correct whichever it is.
	 */
	if (split_parent_leaf(path, parent, sizeof(parent), &leaf) != 0)
		return -1;
	parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent_fd < 0)
		return -1;
	memset(&vol_args, 0, sizeof(vol_args));
	snprintf(vol_args.name, sizeof(vol_args.name), "%s", leaf);
	if (ioctl(parent_fd, CIX_BTRFS_IOC_SNAP_DESTROY, &vol_args) != 0) {
		close(parent_fd);
		return rmtree(path);
	}
	close(parent_fd);
	return 0;
}

int cix_btrfs_qgroup_limit_excl(const char *path, unsigned long long bytes)
{
	int fd;
	struct cix_btrfs_ioctl_quota_ctl_args qc;
	struct cix_btrfs_ioctl_qgroup_limit_args ql;

	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	/* Enable quotas filesystem-wide; EINVAL means already enabled --
	 * the same idempotent convention ADR-0103's upperdir path uses. */
	memset(&qc, 0, sizeof(qc));
	qc.cmd = CIX_BTRFS_QUOTA_CTL_ENABLE;
	if (ioctl(fd, CIX_BTRFS_IOC_QUOTA_CTL, &qc) != 0 && errno != EINVAL) {
		int saved = errno;

		close(fd);
		errno = saved;
		return -1;
	}

	/* qgroupid 0 addresses the subvolume owning this fd -- btrfs's own
	 * documented convention for self-addressing. */
	memset(&ql, 0, sizeof(ql));
	ql.qgroupid = 0;
	ql.lim.flags = CIX_BTRFS_QGROUP_LIMIT_MAX_EXCL;
	ql.lim.max_excl = bytes;
	if (ioctl(fd, CIX_BTRFS_IOC_QGROUP_LIMIT, &ql) != 0) {
		int saved = errno;

		close(fd);
		errno = saved;
		return -1;
	}
	close(fd);
	return 0;
}
