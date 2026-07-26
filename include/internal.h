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
 * Called from inside the child only, after ns_clone3 returns 0.
 * Makes mount propagation private, bind-mounts mnt->root_source onto
 * itself, pivot_roots into it, detaches the old root at
 * mnt->put_old_rel, and mounts a fresh /proc.
 */
int mountns_pivot(const struct mount_spec *mnt);

#endif /* CONTAINER_INTERNAL_H */
