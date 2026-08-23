#ifndef PATHUTIL_H
#define PATHUTIL_H

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*
 * mkdir -p equivalent: creates dir_path and every missing parent
 * component (EEXIST tolerated throughout). A pure, stateless helper
 * both the daemon layer (daemon/src/persist.c's persist_mkdir_p(),
 * writing DNS/PKI state under arbitrary caller-supplied paths) and
 * the runtime library (src/container_dev.c, creating a granted
 * device node's parent directories, e.g. /dev/bus/usb/002/, inside a
 * container's freshly-pivoted /dev) both need. Lives here rather than
 * in daemon/include/persist.h so the runtime library (LIB_SRCS) never
 * gains a dependency on the daemon layer -- that dependency direction
 * doesn't exist anywhere else in this codebase and shouldn't start
 * here. One real implementation, reused rather than duplicated (No
 * Parallel Implementations), matching the precedent daemon/include/
 * namecheck.h already set for a small shared static-inline helper.
 */
static inline int thinc_mkdir_p(const char *dir_path)
{
	char tmp[PATH_MAX];
	size_t len;
	char *p;

	if (snprintf(tmp, sizeof(tmp), "%s", dir_path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	len = strlen(tmp);
	if (len == 0)
		return 0;
	if (tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				fprintf(stderr, "thinc_mkdir_p: mkdir %s failed: %s\n", tmp, strerror(errno));
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "thinc_mkdir_p: mkdir %s failed: %s\n", tmp, strerror(errno));
		return -1;
	}
	return 0;
}

#endif /* PATHUTIL_H */
