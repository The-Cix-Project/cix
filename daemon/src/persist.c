#include "persist.h"
#include "pathutil.h"

#include <errno.h>
#include <fcntl.h>
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
	if (fd < 0)
		return -1;
	written = write(fd, data, len);
	if (written < 0 || (size_t)written != len) {
		close(fd);
		return -1;
	}
	if (fsync(fd) != 0) {
		close(fd);
		return -1;
	}
	close(fd);

	if (rename(tmp_path, path) != 0)
		return -1;
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
		close(fd);
		return -1;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (buf == NULL) {
		close(fd);
		return -1;
	}
	n = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if (n < 0 || (size_t)n != (size_t)st.st_size) {
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
	return kx_mkdir_p(dir_path);
}
