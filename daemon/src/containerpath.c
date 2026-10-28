/*
 * container_file_host_path() -- see daemon/include/containerpath.h for
 * why every daemon-side write into a container goes through here rather
 * than through /proc/<pid>/root (#269).
 */
#include "containerpath.h"
#include "daemonpaths.h"

#include <stdio.h>

void container_file_host_path(const char *container_name, const char *rel_path, char *out,
                              size_t out_size)
{
	snprintf(out, out_size, "%s/%s%s", CONTAINER_ROOTFS_DIR, container_name, rel_path);
}
