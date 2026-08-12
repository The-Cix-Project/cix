#include "treecopy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static int treecopy_walk(const char *src_root, const char *dst_root, const char *relpath)
{
	char src_dir[PATH_MAX];
	DIR *d;
	struct dirent *de;

	snprintf(src_dir, sizeof(src_dir), "%s%s%s", src_root, relpath[0] ? "/" : "", relpath);
	d = opendir(src_dir);
	if (d == NULL)
		return relpath[0] == '\0' ? 0 : -1;

	while ((de = readdir(d)) != NULL) {
		char child_rel[PATH_MAX];
		char src_path[PATH_MAX], dst_path[PATH_MAX];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(child_rel, sizeof(child_rel), "%s%s%s", relpath, relpath[0] ? "/" : "", de->d_name);
		snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, de->d_name);
		snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_root, child_rel);

		if (lstat(src_path, &st) != 0) {
			closedir(d);
			return -1;
		}

		if (S_ISDIR(st.st_mode)) {
			if (mkdir(dst_path, 0755) != 0 && errno != EEXIST) {
				closedir(d);
				return -1;
			}
			if (treecopy_walk(src_root, dst_root, child_rel) != 0) {
				closedir(d);
				return -1;
			}
		} else if (S_ISLNK(st.st_mode)) {
			char target[PATH_MAX];
			ssize_t len = readlink(src_path, target, sizeof(target) - 1);

			if (len < 0) {
				closedir(d);
				return -1;
			}
			target[len] = '\0';
			unlink(dst_path); /* tolerate a stale entry from a prior partial attempt */
			if (symlink(target, dst_path) != 0) {
				closedir(d);
				return -1;
			}
		} else if (S_ISREG(st.st_mode)) {
			if (copy_file_preserve_mode(src_path, dst_path) != 0) {
				closedir(d);
				return -1;
			}
		}
		/* Anything else (a device node, fifo, socket) is never
		 * expected under any of this project's own storage-placement
		 * trees -- silently skipped rather than failing the whole
		 * copy over content that was never supposed to be there. */
	}
	closedir(d);
	return 0;
}

int treecopy_recursive(const char *src_root, const char *dst_root)
{
	return treecopy_walk(src_root, dst_root, "");
}
