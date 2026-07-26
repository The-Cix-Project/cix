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

/*
 * Parent side, called after ns_clone3() returns the child's pid, only
 * if spec->net.bridge != NULL. Creates a veth pair (named from the
 * child's real pid), moves the container-side end into the child's
 * netns, attaches the host side to the bridge, brings it up, then
 * writes the container-side veth's name through ready_pipe_write --
 * not just a bare "ready" signal, since the child can't recover that
 * name itself via getpid() (CLONE_NEWPID means it sees itself as pid
 * 1 in its own namespace). On failure, the caller must close
 * ready_pipe_write without writing, so the child's read() observes
 * EOF and fails cleanly instead of blocking forever.
 */
int container_net_host_setup(const struct network_spec *net, pid_t child_pid, int ready_pipe_write);

/*
 * Child side, called after mount/root setup and sethostname(), before
 * PR_SET_PDEATHSIG/execve, only if spec->net.bridge != NULL. Blocks
 * reading the veth name from ready_pipe_read (failure or EOF -> -1),
 * renames it to "eth0", addresses it, brings it and "lo" up, and
 * installs the default route.
 */
int container_net_child_configure(const struct network_spec *net, int ready_pipe_read);

#endif /* CONTAINER_INTERNAL_H */
