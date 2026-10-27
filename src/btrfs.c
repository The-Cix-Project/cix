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
		 * mkdir path above uses. */
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

/*
 * Carries src's owner, exact mode and timestamps onto dst (#464), so the
 * copy is what `cp -a` made it and not an approximation of it.
 *
 * Owner first: a chown clears setuid and setgid, so the mode goes on
 * after it. The mode is set with chmod() because mkdir(), open() and
 * mknod() all apply the process umask -- under 022 a /tmp that is 1777
 * in the image came out 1755. A symlink gets its owner and times but no
 * mode: Linux has none to set on one.
 */
static int copy_meta(const char *dst, const struct stat *st)
{
	struct timespec times[2];

	if (lchown(dst, st->st_uid, st->st_gid) != 0)
		return -1;
	if (!S_ISLNK(st->st_mode) && chmod(dst, st->st_mode & 07777) != 0)
		return -1;
	times[0] = st->st_atim;
	times[1] = st->st_mtim;
	return utimensat(AT_FDCWD, dst, times, AT_SYMLINK_NOFOLLOW);
}

/* Recursively copy a plain directory tree src -> dst (dst created
 * fresh, or reused if it already exists), preserving each entry's
 * owner, mode and timestamps (copy_meta()), symlinks as symlinks, and
 * device, fifo and socket nodes by type and rdev. Self-contained so
 * the runtime lib stays free of daemon helpers. A directory's own
 * metadata is set after its children are copied into it, since adding
 * them changes its mtime. Hard links become separate copies. */
static int copy_tree(const char *src, const char *dst)
{
	DIR *d;
	struct dirent *de;
	struct stat st;

	if (lstat(src, &st) != 0)
		return -1;

	if (S_ISDIR(st.st_mode)) {
		/* 0700 while it is filled; copy_meta() sets the real mode after,
		 * so a read-only directory can still receive its children. */
		if (mkdir(dst, 0700) != 0 && errno != EEXIST)
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
		return copy_meta(dst, &st);
	}

	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(src, target, sizeof(target) - 1);

		if (n < 0)
			return -1;
		target[n] = '\0';
		if (symlink(target, dst) != 0 && errno != EEXIST)
			return -1;
		return copy_meta(dst, &st);
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
		if (r < 0)
			return -1;
		return copy_meta(dst, &st);
	}

	/* Device/fifo/socket nodes: recreate by type+rdev. A seeded image
	 * rootfs legitimately carries device nodes (/dev/null etc.). */
	if (mknod(dst, st.st_mode, st.st_rdev) != 0 && errno != EEXIST)
		return -1;
	return copy_meta(dst, &st);
}

int cix_tree_copy(const char *src, const char *dst)
{
	return copy_tree(src, dst);
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
int cix_btrfs_is_subvolume(const char *path)
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
	if (!cix_btrfs_is_subvolume(src)) {
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
		 * different filesystem): a caller learns that its snapshot was
		 * impossible, rather than getting a full copy it did not ask
		 * for.
		 *
		 * This comment used to say the container-create path falls
		 * back to the overlay rootfs here, "which ... costs nothing".
		 * That was true of a non-userns container only. A userns one
		 * fell back to copying and chowning the whole image on the
		 * event loop, and on 192.168.15.95 that was every userns
		 * create, because images and container bases were on
		 * different filesystems (#608). ADR-0341 removed the cause:
		 * a container's rootfs now lives beside its image, so this
		 * EXDEV no longer happens there, and a create that hits it
		 * anyway logs it.
		 */
		errno = saved;
		return -1;
	}
	close(src_fd);
	close(parent_fd);
	return 0;
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

static int quota_tree_lookup(int fd, uint32_t type, uint64_t offset, void *out, size_t cap,
                             uint32_t *item_len);

int cix_btrfs_quota_mode_of(const char *path, enum cix_btrfs_quota_mode *mode,
                            int *inconsistent)
{
	struct cix_btrfs_qgroup_status_item status;
	uint32_t len = 0;
	int fd, found, saved;

	*mode = CIX_BTRFS_QUOTA_OFF;
	if (inconsistent != NULL)
		*inconsistent = 0;
	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	found = quota_tree_lookup(fd, CIX_BTRFS_QGROUP_STATUS_KEY, 0, &status, sizeof(status), &len);
	saved = errno;
	close(fd);
	if (found < 0) {
		/* ENOENT: the filesystem has no quota tree, so quotas are off. */
		if (saved == ENOENT)
			return 0;
		errno = saved;
		return -1;
	}
	/* No status item, or one too short to carry flags: off. */
	if (found == 0 || len < 24 || !(status.flags & CIX_BTRFS_QGROUP_STATUS_FLAG_ON))
		return 0;
	*mode = (status.flags & CIX_BTRFS_QGROUP_STATUS_FLAG_SIMPLE_MODE) ? CIX_BTRFS_QUOTA_SIMPLE
	                                                                  : CIX_BTRFS_QUOTA_FULL;
	if (inconsistent != NULL && *mode == CIX_BTRFS_QUOTA_FULL)
		*inconsistent = (status.flags & CIX_BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT) != 0;
	return 0;
}

int cix_btrfs_qgroup_limit_excl(const char *path, unsigned long long bytes)
{
	int fd;
	struct cix_btrfs_ioctl_quota_ctl_args qc;
	struct cix_btrfs_ioctl_qgroup_limit_args ql;
	enum cix_btrfs_quota_mode mode;

	/*
	 * ADR-0359 decision 4: quotas are enabled here only when they are
	 * off, and only in FULL mode. A filesystem cixd made already has its
	 * mode from mkfs; one with quotas off may hold data, and simple
	 * accounting would never count it (measured on 192.168.15.95,
	 * #613). This used to enable full mode every time and take EINVAL
	 * as "already on", which is the wrong test once a filesystem can be
	 * on in simple mode.
	 */
	if (cix_btrfs_quota_mode_of(path, &mode, NULL) != 0)
		return -1;

	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (mode == CIX_BTRFS_QUOTA_OFF) {
		memset(&qc, 0, sizeof(qc));
		qc.cmd = CIX_BTRFS_QUOTA_CTL_ENABLE;
		if (ioctl(fd, CIX_BTRFS_IOC_QUOTA_CTL, &qc) != 0) {
			int saved = errno;

			close(fd);
			errno = saved;
			return -1;
		}
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

/*
 * One point lookup in the quota tree of the filesystem `fd` belongs to.
 * Returns 1 and copies min(hdr.len, cap) bytes of the item into `out`
 * when the key exists, 0 when it does not, -1 with errno set on error.
 *
 * min == max on every field, so the 136-bit key interval the kernel
 * compares against collapses to exactly one key; see the header for why
 * a range would be wrong rather than merely wasteful.
 */
static int quota_tree_lookup(int fd, uint32_t type, uint64_t offset, void *out, size_t cap,
                             uint32_t *item_len)
{
	struct cix_btrfs_ioctl_search_args sa;
	struct cix_btrfs_ioctl_search_header hdr;
	size_t n;

	memset(&sa, 0, sizeof(sa));
	sa.key.tree_id = CIX_BTRFS_QUOTA_TREE_OBJECTID;
	sa.key.min_objectid = 0;
	sa.key.max_objectid = 0;
	sa.key.min_type = type;
	sa.key.max_type = type;
	sa.key.min_offset = offset;
	sa.key.max_offset = offset;
	sa.key.min_transid = 0;
	sa.key.max_transid = (uint64_t)-1;
	sa.key.nr_items = 1;

	if (ioctl(fd, CIX_BTRFS_IOC_TREE_SEARCH, &sa) != 0)
		return -1;
	if (sa.key.nr_items == 0)
		return 0;

	/* The buffer is a search_header followed by the item's bytes. Copy
	 * through a local header rather than casting into the buffer: the
	 * kernel makes no alignment promise about buf, and the item's own
	 * length is what bounds the read, not the size of the struct this
	 * code happens to expect (these items have grown across kernel
	 * versions, and a shorter one must not read past its end). */
	memcpy(&hdr, sa.buf, sizeof(hdr));
	n = hdr.len < cap ? hdr.len : cap;
	memset(out, 0, cap);
	memcpy(out, sa.buf + sizeof(hdr), n);
	if (item_len != NULL)
		*item_len = hdr.len;
	return 1;
}

int cix_btrfs_qgroup_query(const char *path, unsigned long long *used,
                           unsigned long long *limit,
                           enum cix_btrfs_qgroup_state *state)
{
	int fd, found, saved;
	uint64_t subvolid;
	uint32_t len;
	struct cix_btrfs_ioctl_ino_lookup_args il;
	struct cix_btrfs_qgroup_info_item info;
	struct cix_btrfs_qgroup_limit_item lim;
	struct cix_btrfs_qgroup_status_item status;

	if (used != NULL)
		*used = 0;
	if (limit != NULL)
		*limit = 0;
	if (state != NULL)
		*state = CIX_BTRFS_QGROUP_OK;

	/* A plain directory would be answered with its PARENT subvolume's
	 * accounting -- see the header. Refuse before opening anything. */
	if (!cix_btrfs_is_subvolume(path)) {
		errno = EINVAL;
		return -1;
	}

	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	/* objectid 256 (the first free inode) with treeid 0 asks "which
	 * subvolume tree is this fd's own?" -- the kernel fills treeid in,
	 * and for a level-0 qgroup that tree id IS the qgroup id. */
	memset(&il, 0, sizeof(il));
	il.treeid = 0;
	il.objectid = CIX_BTRFS_FIRST_FREE_OBJECTID;
	if (ioctl(fd, CIX_BTRFS_IOC_INO_LOOKUP, &il) != 0) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	subvolid = il.treeid;

	/* Accounting state first: it decides whether the numbers below can
	 * be believed, and a caller told "stale" can say so rather than
	 * publishing a figure that is quietly low. A missing status item
	 * is not an error -- it just means nothing to report. */
	found = quota_tree_lookup(fd, CIX_BTRFS_QGROUP_STATUS_KEY, 0, &status, sizeof(status), &len);
	if (found < 0) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	if (found == 1 && state != NULL && len >= 24) {
		if (status.flags & CIX_BTRFS_QGROUP_STATUS_FLAG_SIMPLE_MODE)
			*state = CIX_BTRFS_QGROUP_SIMPLE;
		else if (status.flags & (CIX_BTRFS_QGROUP_STATUS_FLAG_RESCAN |
		                          CIX_BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT))
			*state = CIX_BTRFS_QGROUP_STALE;
	}

	found = quota_tree_lookup(fd, CIX_BTRFS_QGROUP_INFO_KEY, subvolid, &info, sizeof(info), NULL);
	if (found < 0) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	if (found == 0) {
		/* Quotas are on (the tree exists, or the search would have
		 * failed) but this subvolume has no record yet. */
		close(fd);
		errno = ENODATA;
		return -1;
	}
	if (used != NULL)
		*used = info.excl;

	/* No limit set is the ordinary case, not a failure: a volume with
	 * no quota reports 0 and succeeds. max_excl is only meaningful when
	 * its own flag bit is set -- max_rfer is a different limit. */
	found = quota_tree_lookup(fd, CIX_BTRFS_QGROUP_LIMIT_KEY, subvolid, &lim, sizeof(lim), NULL);
	if (found < 0) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	if (found == 1 && limit != NULL && (lim.flags & CIX_BTRFS_QGROUP_LIMIT_MAX_EXCL))
		*limit = lim.max_excl;

	close(fd);
	return 0;
}

/*
 * ADR-0343: 1 while the root tree still carries an orphan item for a
 * deleted subvolume -- one the cleaner has not finished freeing -- 0
 * when none is left, -1 with errno set. One search with nr_items 1:
 * the question is whether any exists, not how many.
 */
static int root_tree_has_orphans(int fd)
{
	struct cix_btrfs_ioctl_search_args sa;

	memset(&sa, 0, sizeof(sa));
	sa.key.tree_id = CIX_BTRFS_ROOT_TREE_OBJECTID;
	sa.key.min_objectid = CIX_BTRFS_ORPHAN_OBJECTID;
	sa.key.max_objectid = CIX_BTRFS_ORPHAN_OBJECTID;
	sa.key.min_type = CIX_BTRFS_ORPHAN_ITEM_KEY;
	sa.key.max_type = CIX_BTRFS_ORPHAN_ITEM_KEY;
	sa.key.min_offset = 0;
	sa.key.max_offset = (uint64_t)-1;
	sa.key.min_transid = 0;
	sa.key.max_transid = (uint64_t)-1;
	sa.key.nr_items = 1;
	if (ioctl(fd, CIX_BTRFS_IOC_TREE_SEARCH, &sa) != 0)
		return -1;
	return sa.key.nr_items > 0;
}

/* Seconds between two looks at the orphan items while drops are pending. */
#define CIX_BTRFS_SETTLE_POLL_SECONDS 5

enum cix_btrfs_settle cix_btrfs_quota_settle(const char *path, int wait_seconds)
{
	struct cix_btrfs_qgroup_status_item status;
	uint32_t len;
	int fd, waited, pending, found, saved;

	if (!cix_btrfs_is_backing(path))
		return CIX_BTRFS_SETTLE_NOT_BTRFS;
	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return CIX_BTRFS_SETTLE_ERROR;

	for (waited = 0;; waited += CIX_BTRFS_SETTLE_POLL_SECONDS) {
		pending = root_tree_has_orphans(fd);
		if (pending < 0) {
			close(fd);
			return CIX_BTRFS_SETTLE_ERROR;
		}
		if (pending == 0)
			break;
		if (waited >= wait_seconds) {
			close(fd);
			return CIX_BTRFS_SETTLE_TIMEOUT;
		}
		sleep(CIX_BTRFS_SETTLE_POLL_SECONDS);
	}

	/* The status item is read from the last COMMITTED tree, which can lag
	 * the in-memory flags by a commit interval. syncfs() commits it. */
	if (syncfs(fd) != 0) {
		close(fd);
		return CIX_BTRFS_SETTLE_ERROR;
	}
	found = quota_tree_lookup(fd, CIX_BTRFS_QGROUP_STATUS_KEY, 0, &status, sizeof(status), &len);
	saved = errno;
	close(fd);
	if (found < 0)
		/* ENOENT: quotas were never enabled, so there is nothing to keep
		 * consistent. */
		return saved == ENOENT ? CIX_BTRFS_SETTLE_OK : CIX_BTRFS_SETTLE_ERROR;
	if (found == 0 || len < 24)
		return CIX_BTRFS_SETTLE_OK;
	if (status.flags & CIX_BTRFS_QGROUP_STATUS_FLAG_SIMPLE_MODE)
		return CIX_BTRFS_SETTLE_OK; /* squota is never rescanned */
	if ((status.flags & CIX_BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT) &&
	    !(status.flags & CIX_BTRFS_QGROUP_STATUS_FLAG_RESCAN))
		return CIX_BTRFS_SETTLE_NEEDS_RESCAN;
	return CIX_BTRFS_SETTLE_OK;
}

int cix_btrfs_quota_rescan(const char *path)
{
	struct cix_btrfs_ioctl_quota_rescan_args ra;
	int fd, saved;

	fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	memset(&ra, 0, sizeof(ra));
	/* EINPROGRESS: one is already running, which serves equally well --
	 * the wait below waits for that one. */
	if (ioctl(fd, CIX_BTRFS_IOC_QUOTA_RESCAN, &ra) != 0 && errno != EINPROGRESS) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	if (ioctl(fd, CIX_BTRFS_IOC_QUOTA_RESCAN_WAIT) != 0) {
		saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	close(fd);
	return 0;
}
