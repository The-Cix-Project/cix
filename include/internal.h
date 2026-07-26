#ifndef CONTAINER_INTERNAL_H
#define CONTAINER_INTERNAL_H

#include "container.h"

/*
 * Fork-like: returns 0 in the child, the child's pid in the parent,
 * -1 on error (errno set). On successful return in the parent,
 * *pidfd_out holds the child's pidfd (CLONE_PIDFD). cgroup_fd is an
 * O_PATH descriptor from cgroup_create(); flags should include
 * CLONE_INTO_CGROUP for atomic placement.
 */
long ns_clone3(unsigned long flags, int cgroup_fd, int *pidfd_out);

/*
 * Called from inside the child only, after ns_clone3 returns 0, as
 * the very first mount-related action -- before overlay_create() or
 * any other mount, so that mount events in the child's own namespace
 * (including the overlay mount) never propagate back to the host.
 */
int mountns_make_private(void);

/*
 * Called from inside the child, after mountns_make_private() and
 * overlay_create(). new_root must already be a populated mount point
 * (overlay_create() guarantees this for ov->merged). Pivot_roots into
 * new_root, detaches the old root at mnt->put_old_rel, and mounts a
 * fresh /proc and /sys.
 */
int mountns_pivot(const char *new_root, const struct mount_spec *mnt);

#endif /* CONTAINER_INTERNAL_H */
