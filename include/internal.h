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
 * Parent side, called right after ns_clone3() returns the child's
 * pid, alongside (but independent of, no shared pipe/coordination
 * needed) container_net_host_setup() -- a passthrough interface keeps
 * its own real kernel name and gets no address from this daemon at
 * all, so there is nothing the child needs to be told. interface_count
 * == 0 is a no-op (*out_netns_fd = -1, returns 0). Otherwise: opens
 * *out_netns_fd on /proc/child_pid/ns/net FIRST, before moving
 * anything (this is the fd the caller must then keep open for the
 * container's entire lifetime and hand to
 * container_net_teardown_interfaces() at removal -- see that
 * function's own comment for why). Moves each of interfaces[] into
 * the child's netns by pid, then brings each one up from a forked,
 * short-lived helper that briefly enters that same netns (via
 * *out_netns_fd) to do it -- confirmed directly, not assumed, that
 * the kernel administratively downs a link as part of
 * dev_change_net_namespace(), so bringing it up beforehand (while
 * still addressable via the caller's own root-netns socket) doesn't
 * stick; a netlink socket can only address interfaces visible in its
 * own netns, so "up" has to happen from inside the netns the
 * interface actually landed in, the same reason
 * container_net_teardown_interfaces() below needs its own helper for
 * the reverse move. No rename, no address -- the operator's own
 * userspace inside the container owns that once installed via
 * `pkg install`.
 */
int container_net_host_attach_interfaces(const char *const *interfaces, int interface_count,
                                          pid_t child_pid, int *out_netns_fd);

/*
 * Called at container removal (registry_remove()), regardless of
 * whether the owning process is still alive. netns_fd (whatever
 * container_net_host_attach_interfaces() returned) is what makes this
 * safe even after the process has already exited: an open fd on
 * /proc/<pid>/ns/net keeps that namespace (and everything still in
 * it) alive for as long as the fd itself stays open, exactly like a
 * live process would. A netlink socket can only address interfaces
 * visible in ITS OWN netns, so moving one back out means briefly
 * entering netns_fd to open a socket scoped to it -- done in a
 * forked, short-lived helper (setns() is a whole-process operation;
 * isolating it in a throwaway child -- the same posture this codebase
 * already takes for curl fetches and package builds -- means the
 * long-lived daemon process's own netns is never at risk). Blocks
 * until the helper exits (bounded, real work -- a handful of netlink
 * round trips). Always closes netns_fd itself before returning, even
 * on failure -- nothing further this daemon can do differs based on
 * netns_fd staying open past this call. interface_count == 0
 * (netns_fd == -1) is a no-op. Returns 0 on success, -1 if the helper
 * couldn't be spawned or exited non-zero (logged -- on failure the
 * interface may be left in the now-orphaned netns, exactly the
 * kernel's own default fallback behavior this function exists to
 * avoid in the first place, not a new failure mode of its own).
 */
int container_net_teardown_interfaces(const char *const *interfaces, int interface_count,
                                       int netns_fd);

/*
 * ADR-0156/task #861: live network attach/detach on an already-running
 * container -- see container_net.c's own doc comments for exactly how
 * each works. attach's veth_host/veth_ctr must be caller-generated,
 * unique host-wide names (main.c derives them from child_pid + the
 * container's own current net_count, mirroring container_net_host_setup()'s
 * own vh<pid>-<idx>/vc<pid>-<idx> scheme). ifname is the name the
 * interface gets inside the container's own netns (main.c: "eth<idx>",
 * continuing the same numbering create-time attachments already use).
 */
int container_net_attach_running(const char *bridge, uint32_t container_ip_be, int prefix_len,
                                  pid_t child_pid, const char *veth_host, const char *veth_ctr,
                                  const char *ifname);
int container_net_detach_running(const char *veth_host);

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
 * Child side, called once per spec->sysctls[] entry, same call site as
 * container_net_enable_ip_forward() above and for the same reason
 * (already validated by the daemon -- daemon/src/main.c -- to start
 * with "net." and use only a safe key charset before ever reaching
 * here). Writes value to /proc/sys/<key with every '.' replaced by
 * '/'>. Returns -1 on any open()/write() failure (e.g. an unknown
 * sysctl name -- validated shape doesn't guarantee the kernel actually
 * has that entry).
 */
int container_net_apply_sysctl(const char *key, const char *value);

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
 * ADR-0161 Phase D: the explicit counterpart container_dev_bpf_attach()
 * itself has never needed until now -- creation-time device_count == 0
 * deliberately attaches nothing at all (a container with no requested
 * devices simply inherits whatever its ancestor cgroup's own policy
 * is), which is correct there but WRONG for a live detach down to zero
 * devices: the cgroup already has a real program attached (granting
 * exactly the device just being revoked), and simply not replacing it
 * would leave that grant silently in effect forever. This performs a
 * real BPF_PROG_DETACH, reverting the cgroup to "no explicit program"
 * -- the same end state creation-time's own zero-device case already
 * has, just reached from the other direction. prog_fd is the daemon's
 * own currently-held reference to the program being detached (still
 * closed by the caller afterward, same as any other now-superseded
 * prog_fd). Returns 0, or -1 (errno set) on failure -- nothing is
 * closed by this function itself, in or out.
 */
int container_dev_bpf_detach(int cgroup_fd, int prog_fd);

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
