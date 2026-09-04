/*
 * container_file_host_path() -- see daemon/include/containerpath.h for
 * why every daemon-side write into a container goes through here rather
 * than through /proc/<pid>/root (#269).
 */
#include "containerpath.h"

#include <stdio.h>

/* Both live in main.c, next to the disk-placement code they belong with. */
void container_root_for(const char *disk_name, char *out, size_t out_size);
void container_writable_path(const char *container_root, const char *name, const char *rel_path,
                             char *out, size_t out_size);

void container_file_host_path(const char *container_name, const char *disk_name,
                              const char *rel_path, char *out, size_t out_size)
{
	char container_root[4096];

	container_root_for(disk_name, container_root, sizeof(container_root));
	container_writable_path(container_root, container_name, rel_path, out, out_size);
}
