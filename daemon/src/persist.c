#include "persist.h"
#include "pathutil.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int persist_atomic_write(const char *path, const char *data, size_t len)
{
	char tmp_path[PATH_MAX + 8];
	int fd;
	ssize_t written;

	if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path))
		return -1;

	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		perror(tmp_path);
		return -1;
	}
	written = write(fd, data, len);
	if (written < 0 || (size_t)written != len) {
		if (written < 0)
			perror(tmp_path);
		else
			fprintf(stderr, "%s: short write (%zd of %zu bytes)\n", tmp_path, written, len);
		close(fd);
		return -1;
	}
	if (fsync(fd) != 0) {
		perror(tmp_path);
		close(fd);
		return -1;
	}
	close(fd);

	if (rename(tmp_path, path) != 0) {
		perror(path);
		return -1;
	}
	return 0;
}

int persist_read_file(const char *path, char **out_buf, size_t *out_len)
{
	int fd;
	struct stat st;
	char *buf;
	ssize_t n;

	*out_buf = NULL;
	*out_len = 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT)
			return 0;
		perror(path);
		return -1;
	}
	if (fstat(fd, &st) != 0) {
		perror(path);
		close(fd);
		return -1;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL) {
		fprintf(stderr, "%s: malloc(%lld) failed\n", path, (long long)st.st_size + 1);
		close(fd);
		return -1;
	}
	n = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if (n < 0 || (size_t)n != (size_t)st.st_size) {
		if (n < 0)
			perror(path);
		else
			fprintf(stderr, "%s: short read (%zd of %lld bytes)\n", path, n, (long long)st.st_size);
		free(buf);
		return -1;
	}
	buf[n] = '\0';

	*out_buf = buf;
	*out_len = (size_t)n;
	return 0;
}

int persist_mkdir_p(const char *dir_path)
{
	return cix_mkdir_p(dir_path);
}

/*
 * nftw() callback for a physical (FTW_PHYS -- never follows a symlink,
 * only ever removes the link itself), post-order (FTW_DEPTH -- a
 * directory's own children are always visited, and removed, before the
 * directory itself) walk. Handles every entry type nftw() can report
 * for a physical walk (regular files, symlinks, device nodes, ... all
 * unlink()able the same way; FTW_DP directories need rmdir() instead,
 * only reachable once already empty). Extracted from image.c's own
 * previously-private, identical callback -- see persist.h's own doc
 * comment for why.
 */
static int remove_tree_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	(void)sb;
	(void)ftwbuf;
	if (typeflag == FTW_DP)
		return rmdir(path);
	return unlink(path);
}

int persist_remove_tree(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0 && errno == ENOENT)
		return 0;
	return nftw(path, remove_tree_cb, 16, FTW_DEPTH | FTW_PHYS);
}
