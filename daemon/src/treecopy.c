#include "treecopy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/*
 * Why the last treecopy_recursive() failed, in a form worth reporting.
 * errno alone is not enough -- "No such file or directory" out of a
 * 15 GB image-store migration tells an operator nothing about WHICH
 * entry stopped it, which is exactly the gap that made a real
 * rebuildable-storage migration failure undiagnosable (issue #172).
 */
static char g_treecopy_error[PATH_MAX + 128];

const char *treecopy_last_error(void)
{
	return g_treecopy_error[0] != '\0' ? g_treecopy_error : "no error recorded";
}

static void treecopy_fail(const char *what, const char *path)
{
	snprintf(g_treecopy_error, sizeof(g_treecopy_error), "%s: %s: %s", what, path,
	         strerror(errno));
}

/* Real permission-preserving file copy -- fstat()s src to learn its
 * real mode, then open()s dst with that same mode from the start
 * (not O_CREAT with a fixed mode followed by a separate fchmod(),
 * which would leave a brief window where dst has the wrong, default
 * mode -- irrelevant for most files here but a real, avoidable gap
 * for the 0600 private keys this primitive exists to carry safely). */
static int copy_file_preserve_mode(const char *src, const char *dst)
{
	int in, out;
	struct stat st;
	char buf[65536];
	ssize_t n;
	int rc = 0;

	in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	if (fstat(in, &st) != 0) {
		close(in);
		return -1;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			rc = -1;
			break;
		}
	}
	if (n < 0)
		rc = -1;
	close(in);
	close(out);
	return rc;
}

/*
 * Gives dst the source's owner and mode. Every entry type goes through
 * this once it exists: before it, a copy kept only a regular file's
 * mode, so every entry came out owned by the daemon and every directory
 * 0755. Found on 192.168.15.95, 2026-10-29, moving a home volume whose
 * user's files belong to a userns-mapped uid and whose ~/.ssh is 0700:
 * the copy would have handed the user's home to root and opened it.
 *
 * Owner first, then mode, because chown() clears setuid and setgid
 * bits. A symlink's own mode means nothing on Linux and lchmod() does
 * not exist, so a link gets its owner only. The chown is skipped when
 * the source already belongs to this process, so an unprivileged
 * caller copying its own files needs no privilege it lacks.
 */
static int apply_owner_and_mode(const char *dst, const struct stat *st)
{
	if ((st->st_uid != geteuid() || st->st_gid != getegid()) &&
	    lchown(dst, st->st_uid, st->st_gid) != 0) {
		treecopy_fail("set owner", dst);
		return -1;
	}
	if (!S_ISLNK(st->st_mode) && chmod(dst, st->st_mode & 07777) != 0) {
		treecopy_fail("set mode", dst);
		return -1;
	}
	return 0;
}

static int is_skipped(const char *name, const char *const *skip)
{
	for (; skip != NULL && *skip != NULL; skip++)
		if (strcmp(name, *skip) == 0)
			return 1;
	return 0;
}

static int treecopy_walk(const char *src_root, const char *dst_root, const char *relpath,
                         const char *const *skip_top)
{
	char src_dir[PATH_MAX];
	DIR *d;
	struct dirent *de;

	snprintf(src_dir, sizeof(src_dir), "%s%s%s", src_root, relpath[0] ? "/" : "", relpath);
	d = opendir(src_dir);
	if (d == NULL) {
		/*
		 * A source that cannot be opened is a failure at EVERY level,
		 * including the root. This used to read
		 *
		 *   return relpath[0] == '\0' ? 0 : -1;
		 *
		 * so a missing source ROOT was success -- "nothing to copy,
		 * fine". Every caller of this primitive is a migration, a
		 * backup or a restore (issue #194 lists all eight sites), and
		 * for those "moved nothing" and "moved everything" then look
		 * identical: the caller repoints live state at an empty
		 * destination and reports success. That is how the installer's
		 * package seed was copied from a path that did not exist and
		 * still produced a clean install.
		 *
		 * No caller needs the tolerance. A volume's directory is
		 * created when the volume is (volume.c:218, and creation fails
		 * if it cannot be), and every storage-migration source is a
		 * live tree the daemon is currently serving from.
		 *
		 * The subdirectory case failed already but recorded no reason,
		 * so treecopy_last_error() reported the previous call's error
		 * or "no error recorded". Both paths now name the directory.
		 */
		treecopy_fail("open source directory", src_dir);
		return -1;
	}

	while ((de = readdir(d)) != NULL) {
		char child_rel[PATH_MAX];
		char src_path[PATH_MAX], dst_path[PATH_MAX];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (relpath[0] == '\0' && is_skipped(de->d_name, skip_top))
			continue;
		snprintf(child_rel, sizeof(child_rel), "%s%s%s", relpath, relpath[0] ? "/" : "", de->d_name);
		snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, de->d_name);
		snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_root, child_rel);

		if (lstat(src_path, &st) != 0) {
			treecopy_fail("stat", src_path);
			closedir(d);
			return -1;
		}

		if (S_ISDIR(st.st_mode)) {
			if (mkdir(dst_path, 0755) != 0 && errno != EEXIST) {
				treecopy_fail("mkdir", dst_path);
				closedir(d);
				return -1;
			}
			if (treecopy_walk(src_root, dst_root, child_rel, NULL) != 0) {
				closedir(d);
				return -1;
			}
			/* After the children: a directory without write permission
			 * for its owner is still filled first. */
			if (apply_owner_and_mode(dst_path, &st) != 0) {
				closedir(d);
				return -1;
			}
		} else if (S_ISLNK(st.st_mode)) {
			char target[PATH_MAX];
			ssize_t len = readlink(src_path, target, sizeof(target) - 1);

			if (len < 0) {
				treecopy_fail("read link", src_path);
				closedir(d);
				return -1;
			}
			target[len] = '\0';
			unlink(dst_path); /* tolerate a stale entry from a prior partial attempt */
			if (symlink(target, dst_path) != 0) {
				treecopy_fail("create link", dst_path);
				closedir(d);
				return -1;
			}
			if (apply_owner_and_mode(dst_path, &st) != 0) {
				closedir(d);
				return -1;
			}
		} else if (S_ISREG(st.st_mode)) {
			if (copy_file_preserve_mode(src_path, dst_path) != 0) {
				treecopy_fail("copy file", src_path);
				closedir(d);
				return -1;
			}
			if (apply_owner_and_mode(dst_path, &st) != 0) {
				closedir(d);
				return -1;
			}
		} else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISFIFO(st.st_mode) ||
		           S_ISSOCK(st.st_mode)) {
			/*
			 * A socket is recreated as a socket node, as a FIFO is:
			 * the copy is then what the source was, a name with
			 * nothing listening on it until its server binds again.
			 * Measured on 192.168.15.95, 2026-10-29: moving jump-home
			 * failed on osakka/.screen/152.byobu, a screen session's
			 * socket, while the container was stopped.
			 */
			/*
			 * Device nodes are REAL content here, not stray junk.
			 * the image baseline staged (removed, ADR-0333) /dev/null, /dev/zero,
			 * /dev/full and /dev/ptmx into every image rootfs
			 * (ADR-0150 added ptmx specifically so posix_openpt()
			 * works), so an image-store migration that dropped them
			 * would produce images whose containers fail on /dev/null
			 * and cannot allocate a PTY -- symptoms appearing much
			 * later and nowhere near the migration that caused them.
			 *
			 * This code used to skip them SILENTLY, on a comment
			 * asserting they are "never expected under any of this
			 * project's own storage-placement trees". True for
			 * state-storage and log-storage; false for the image
			 * store (issue #172).
			 */
			if (mknod(dst_path, st.st_mode, st.st_rdev) != 0 && errno != EEXIST) {
				treecopy_fail("create device node", dst_path);
				closedir(d);
				return -1;
			}
			/*
			 * mknod() applies the umask, so the node lands at 0644
			 * where the source was 0666 -- and a /dev/null a
			 * non-root process cannot write to breaks any container
			 * that is not running as host root, which is now the
			 * default (ADR-0207 phase 3). The real mode is set
			 * explicitly, with the owner, rather than inheriting
			 * whatever umask the daemon happens to have.
			 */
			if (apply_owner_and_mode(dst_path, &st) != 0) {
				closedir(d);
				return -1;
			}
		} else {
			/*
			 * Anything else: fail, do not skip. A
			 * storage migration silently dropping content is the one
			 * behaviour that must never be the default -- the caller
			 * repoints live state at this copy believing it complete.
			 */
			errno = EINVAL;
			treecopy_fail("unsupported entry type", src_path);
			closedir(d);
			return -1;
		}
	}
	closedir(d);
	return 0;
}

/* mkdir -p for the destination root, in place rather than pulling in
 * persist.c -- this module is deliberately dependency-free so the
 * forked copy child stays trivial. */
static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	size_t len;
	char *p;

	len = (size_t)snprintf(tmp, sizeof(tmp), "%s", path);
	if (len >= sizeof(tmp))
		return -1;
	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

int treecopy_recursive(const char *src_root, const char *dst_root)
{
	return treecopy_recursive_except(src_root, dst_root, NULL);
}

int treecopy_recursive_except(const char *src_root, const char *dst_root,
                              const char *const *skip_top)
{
	/*
	 * Create the destination ROOT, not just the subdirectories inside
	 * it. treecopy_walk() mkdirs each directory it finds under the
	 * source, but nothing created the root itself -- so a migration to
	 * a freshly formatted disk failed on its very first entry:
	 *
	 *   bulk copy failed -- mkdir:
	 *     /var/lib/cix/disks/sda/rebuildable/pkg: No such file or directory
	 *
	 * because .../rebuildable did not exist to hold pkg. That is issue
	 * #172's third defect and it means rebuildable-storage migration
	 * has never worked; it was invisible for as long as the only
	 * report was the words "bulk copy failed".
	 */
	g_treecopy_error[0] = '\0'; /* this call's reason, never an earlier one's */
	if (mkdir_p(dst_root) != 0) {
		treecopy_fail("create destination root", dst_root);
		return -1;
	}
	return treecopy_walk(src_root, dst_root, "", skip_top);
}
