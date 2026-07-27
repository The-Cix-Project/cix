#include "test_image_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	size_t len;
	char *p;

	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				perror(tmp);
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
		perror(tmp);
		return -1;
	}
	return 0;
}

int test_image_fixture_copy_file(const char *src_path, const char *dst_path)
{
	int src, dst;
	char buf[4096];
	ssize_t n;

	src = open(src_path, O_RDONLY);
	if (src < 0) {
		perror(src_path);
		return -1;
	}
	dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (dst < 0) {
		perror(dst_path);
		close(src);
		return -1;
	}
	while ((n = read(src, buf, sizeof(buf))) > 0) {
		if (write(dst, buf, (size_t)n) != n) {
			perror("write");
			close(src);
			close(dst);
			return -1;
		}
	}
	if (n < 0)
		perror(src_path);
	close(src);
	close(dst);
	return n < 0 ? -1 : 0;
}

int test_image_fixture_build(const char *image_root, const char *child_binary_path,
                              const char *child_basename)
{
	char path[PATH_MAX];

	if (mkdir_p(image_root) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/bin", image_root);
	if (mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/bin/%s", image_root, child_basename);
	if (test_image_fixture_copy_file(child_binary_path, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib64", image_root);
	if (mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib64/ld-linux-x86-64.so.2", image_root);
	if (test_image_fixture_copy_file("/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu", image_root);
	if (mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib/x86_64-linux-gnu/libc.so.6", image_root);
	if (test_image_fixture_copy_file("/usr/lib/x86_64-linux-gnu/libc.so.6", path) != 0)
		return -1;

	return 0;
}

int test_image_fixture_add_lib(const char *image_root, const char *host_lib_abs_path)
{
	char dst_path[PATH_MAX];
	char dst_dir[PATH_MAX];
	char *slash;

	if (snprintf(dst_path, sizeof(dst_path), "%s%s", image_root, host_lib_abs_path) >=
	    (int)sizeof(dst_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	snprintf(dst_dir, sizeof(dst_dir), "%s", dst_path);
	slash = strrchr(dst_dir, '/');
	if (slash != NULL)
		*slash = '\0';

	if (mkdir_p(dst_dir) != 0)
		return -1;
	return test_image_fixture_copy_file(host_lib_abs_path, dst_path);
}
