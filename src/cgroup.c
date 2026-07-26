#include "container.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CGROUP_ROOT "/sys/fs/cgroup"

static int write_cgroup_file(const char *dir, const char *file, const char *value)
{
	char path[PATH_MAX];
	int fd;
	ssize_t written;
	size_t len;

	if (snprintf(path, sizeof(path), "%s/%s", dir, file) >= (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	len = strlen(value);
	written = write(fd, value, len);
	if (written < 0 || (size_t)written != len) {
		int saved_errno = (written < 0) ? errno : EIO;
		close(fd);
		errno = saved_errno;
		return -1;
	}

	if (close(fd) != 0)
		return -1;

	return 0;
}

int cgroup_create(const struct cgroup_limits *lim, int *out_fd)
{
	char dir[PATH_MAX];
	char value[32];
	int fd;

	if (snprintf(dir, sizeof(dir), "%s/%s", CGROUP_ROOT, lim->name) >= (int)sizeof(dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		return -1;

	if (lim->memory_max > 0) {
		snprintf(value, sizeof(value), "%lld", lim->memory_max);
		if (write_cgroup_file(dir, "memory.max", value) != 0)
			return -1;
	}

	if (lim->pids_max > 0) {
		snprintf(value, sizeof(value), "%lld", lim->pids_max);
		if (write_cgroup_file(dir, "pids.max", value) != 0)
			return -1;
	}

	if (lim->cpu_max != NULL) {
		if (write_cgroup_file(dir, "cpu.max", lim->cpu_max) != 0)
			return -1;
	}

	fd = open(dir, O_PATH);
	if (fd < 0)
		return -1;

	*out_fd = fd;
	return 0;
}
