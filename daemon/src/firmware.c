/*
 * ADR-0381: the firmware image's stable link, and the kernel's firmware
 * path. See firmware.h.
 */
#include "firmware.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void firmware_link_target(const char *link, char *out, size_t out_size)
{
	ssize_t n;

	if (out_size == 0)
		return;
	out[0] = '\0';
	n = readlink(link, out, out_size - 1);
	if (n <= 0 || (size_t)n >= out_size - 1) {
		out[0] = '\0';
		return;
	}
	out[n] = '\0';
}

int firmware_link_set(const char *link, const char *target, const char *empty_dir, char *err,
                      size_t err_size)
{
	char current[4096], tmp[4096];
	struct stat st;

	if (target == NULL || target[0] == '\0') {
		if (mkdir(empty_dir, 0755) != 0 && errno != EEXIST) {
			snprintf(err, err_size, "could not create %s: %s", empty_dir, strerror(errno));
			return -1;
		}
		if (stat(empty_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
			snprintf(err, err_size, "%s exists and is not a directory", empty_dir);
			return -1;
		}
		target = empty_dir;
	}
	firmware_link_target(link, current, sizeof(current));
	if (strcmp(current, target) == 0)
		return 0;
	/* A path that exists and is not a symlink is not ours to replace. */
	if (current[0] == '\0' && lstat(link, &st) == 0) {
		snprintf(err, err_size, "%s exists and is not a symlink", link);
		return -1;
	}
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.new", link) >= sizeof(tmp)) {
		snprintf(err, err_size, "%s is too long", link);
		return -1;
	}
	if (unlink(tmp) != 0 && errno != ENOENT) {
		snprintf(err, err_size, "could not remove %s: %s", tmp, strerror(errno));
		return -1;
	}
	if (symlink(target, tmp) != 0) {
		snprintf(err, err_size, "could not link %s to %s: %s", tmp, target, strerror(errno));
		return -1;
	}
	if (rename(tmp, link) != 0) {
		snprintf(err, err_size, "could not rename %s to %s: %s", tmp, link, strerror(errno));
		unlink(tmp);
		return -1;
	}
	return 0;
}

int firmware_path_apply(const char *param, const char *path, char *err, size_t err_size)
{
	char current[FIRMWARE_PATH_MAX + 2];
	size_t len = strlen(path);
	ssize_t n;
	int fd;

	if (len == 0 || len > FIRMWARE_PATH_MAX) {
		snprintf(err, err_size,
		         "%s is %zu bytes, and the kernel's firmware path holds 1 to %d -- refused "
		         "rather than truncated",
		         path, len, FIRMWARE_PATH_MAX);
		return -1;
	}
	fd = open(param, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		n = read(fd, current, sizeof(current) - 1);
		close(fd);
		if (n > 0) {
			current[n] = '\0';
			current[strcspn(current, "\n")] = '\0';
			if (strcmp(current, path) == 0)
				return 0;
		}
	}
	/* The exact bytes, with no newline, so nothing depends on whether
	 * the kernel strips one from a string parameter (not read in the
	 * source; probe-fwpath@1-1 read only the loader). */
	fd = open(param, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, err_size, "could not open %s: %s", param, strerror(errno));
		return -1;
	}
	n = write(fd, path, len);
	if (n != (ssize_t)len) {
		snprintf(err, err_size, "could not write %s: %s", param,
		         n < 0 ? strerror(errno) : "short write");
		close(fd);
		return -1;
	}
	if (close(fd) != 0) {
		snprintf(err, err_size, "could not write %s: %s", param, strerror(errno));
		return -1;
	}
	return 0;
}
