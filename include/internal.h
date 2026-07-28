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
 * if spec->net_count > 0. For each of the net_count attachments in
 * nets[], creates a veth pair (named from the child's real pid and
 * that attachment's index), moves the container-side end into the
 * child's netns, attaches the host side to that attachment's bridge,
 * brings it up, then writes the container-side veth's name through
 * ready_pipe_write -- not just a bare "ready" signal, since the child
 * can't recover that name itself via getpid() (CLONE_NEWPID means it
 * sees itself as pid 1 in its own namespace). One write per
 * attachment, same order the child will read them in. On failure, the
 * caller must close ready_pipe_write without writing, so the child's
 * read() observes EOF and fails cleanly instead of blocking forever.
 */
int container_net_host_setup(const struct network_spec *nets, int net_count, pid_t child_pid,
                              int ready_pipe_write);

/*
 * Child side, called after mount/root setup and sethostname(), before
 * PR_SET_PDEATHSIG/execve, only if spec->net_count > 0. For each of
 * the net_count attachments in nets[], blocks reading one veth name
 * from ready_pipe_read (failure or EOF -> -1), renames it to "eth" +
 * index, addresses it, and brings it up. After the loop, brings up
 * "lo" and installs the default route via nets[0] only -- the first
 * attachment is "primary"; any others get only their subnet's
 * connected route, which the kernel already installed as a side
 * effect of the address assignment.
 */
int container_net_child_configure(const struct network_spec *nets, int net_count,
                                   int ready_pipe_read);

/*
 * Child side, called after container_net_child_configure() (if any)
 * succeeds, before PR_SET_PDEATHSIG/execve. Installs each of routes[]
 * in order via one rtnetlink session; a no-op that returns 0
 * immediately if route_count == 0. Needs no pipe synchronization --
 * unlike the interface setup above, this only touches the child's own
 * already-established netns, nothing the parent needs to coordinate.
 */
int container_net_install_routes(const struct route_spec *routes, int route_count);

/*
 * Child side, called if spec->ip_forward is set. Enables
 * net.ipv4.ip_forward inside the container's own netns by writing
 * directly to /proc/sys/net/ipv4/ip_forward -- a per-netns sysctl, so
 * this only affects this one container, never the host or siblings.
 */
int container_net_enable_ip_forward(void);

/*
 * Parent side, called right after cgroup_create() and before
 * ns_clone3(). device_count == 0 is a no-op (*out_prog_fd = -1,
 * returns 0 immediately) -- a container that hasn't been granted any
 * devices gets no BPF program at all, identical to today's behavior.
 * Otherwise hand-assembles a BPF_PROG_TYPE_CGROUP_DEVICE program (one
 * unrolled comparison block per granted (type, major, minor),
 * default-deny) and attaches it to cgroup_fd via BPF_CGROUP_DEVICE.
 * Must run before ns_clone3(): CLONE_INTO_CGROUP places the child
 * into the cgroup atomically as part of that syscall, so the policy
 * has to already be attached for there to be no race window, and
 * because the child's own container_dev_mknod() calls are themselves
 * subject to a BPF_DEVCG_ACC_MKNOD check under this same program. On
 * success *out_prog_fd is the loaded program's own fd, kept open for
 * the container's lifetime (closed alongside cgroup_fd/pidfd on
 * removal). On failure, errno is set and nothing is attached.
 */
int container_dev_bpf_attach(int cgroup_fd, const struct device_spec *devices, int device_count,
                              int *out_prog_fd);

/*
 * Child side, called right after mountns_pivot() succeeds -- the
 * first point the container's /dev is genuinely private (its own
 * pivoted mount namespace, no longer the host's or any sibling's).
 * mknod()s each of devices[]'s granted nodes (creating dev_path's
 * parent directories first, e.g. /dev/bus/usb/002/), mode 0666 --
 * this project's containers already run as full root with no user
 * namespace, so the real access gate is the BPF program
 * container_dev_bpf_attach() already attached before clone3(), not
 * these POSIX permission bits. A no-op if device_count == 0.
 */
int container_dev_mknod(const struct device_spec *devices, int device_count);

#endif /* CONTAINER_INTERNAL_H */
