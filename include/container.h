#ifndef CONTAINER_H
#define CONTAINER_H

#include <stdint.h>
#include <sys/types.h>

struct ns_config {
	unsigned long clone_flags;
	const char *hostname;
};

struct cgroup_limits {
	const char *name;
	long long memory_max;
	long long pids_max;
	const char *cpu_max;
};

struct mount_spec {
	const char *put_old_rel;
};

/*
 * Matches daemon/include/network.h's NETWORK_MAX: a container can
 * never attach to more networks than could possibly exist, so that's
 * the real ceiling here, not an arbitrary round number.
 */
#define CONTAINER_MAX_NETWORKS 64

/*
 * One network attachment. Addresses are network byte order (e.g.
 * straight from inet_pton()).
 */
struct network_spec {
	const char *bridge;
	uint32_t container_ip_be;
	uint32_t gateway_ip_be;
	int prefix_len;
};

#define CONTAINER_MAX_ROUTES 8

/*
 * One static route, installed inside the container's own netns at
 * creation time (not modifiable on an already-running container --
 * that would need a separate "enter another netns from outside"
 * primitive, not built yet). gateway_be is required: v1 has no
 * on-link/direct route support, only via-a-gateway.
 */
struct route_spec {
	uint32_t dest_be;
	int dest_prefix_len;
	uint32_t gateway_be;
};

struct overlay_spec {
	const char *lowerdir;
	const char *upperdir;
	const char *workdir;
	const char *merged;
};

/*
 * A considered, generous fixed bound with no natural daemon-side
 * ceiling to mirror (devices are host hardware, not a daemon-created
 * resource the way CONTAINER_MAX_NETWORKS mirrors NETWORK_MAX) --
 * same reasoning CONTAINER_MAX_ROUTES already used: enough for a
 * whole USB controller's several endpoints, a multi-port serial card,
 * or a handful of NVMe namespaces in one container.
 */
#define CONTAINER_MAX_DEVICES 16

enum device_node_type {
	DEVICE_NODE_CHAR,
	DEVICE_NODE_BLOCK,
};

/*
 * One device grant. dev_path is mknod()'d verbatim into the
 * container's own private /dev after pivot (container_dev_mknod());
 * type/major/minor drive both that mknod() and the BPF_CGROUP_DEVICE
 * program built by container_dev_bpf_attach() -- the two must always
 * describe the same node, which is why they travel together in one
 * struct rather than being independently specified.
 */
struct device_spec {
	enum device_node_type type;
	unsigned int major;
	unsigned int minor;
	char dev_path[64];
};

struct container_spec {
	struct ns_config ns;
	struct cgroup_limits cg;
	struct overlay_spec ov;
	struct mount_spec mnt;
	/*
	 * Opt-in network attachment: net_count == 0 means no networking --
	 * the container gets exactly what every container has gotten since
	 * Phase 1, an isolated netns with only "lo". nets[0] is "primary"
	 * (gets the default route); any further attachments get only their
	 * subnet's connected route, from the kernel automatically assigning
	 * one alongside the address.
	 */
	struct network_spec nets[CONTAINER_MAX_NETWORKS];
	int net_count;
	/*
	 * ip_forward enables net.ipv4.ip_forward inside the container's own
	 * netns -- harmless without net_count > 1, but not gated on it
	 * either; the operator asked for it, so it's applied regardless.
	 * routes are installed after the primary default route, in order.
	 */
	int ip_forward;
	struct route_spec routes[CONTAINER_MAX_ROUTES];
	int route_count;
	/*
	 * Opt-in device passthrough: device_count == 0 means no /dev
	 * nodes at all beyond whatever the shared OverlayFS lowerdir
	 * image already contains, and no BPF_CGROUP_DEVICE program is
	 * loaded or attached -- a container that doesn't opt in is
	 * byte-for-byte unaffected by this feature (No Regressions).
	 */
	struct device_spec devices[CONTAINER_MAX_DEVICES];
	int device_count;
	char *const *argv;
	char *const *envp;
};

struct container_handle {
	pid_t pid;
	int cgroup_fd;
	int pidfd;
	/*
	 * The loaded/attached BPF_CGROUP_DEVICE program's own fd, or -1
	 * when device_count was 0 at creation time. Kept open for the
	 * container's lifetime so the kernel-side attachment (pinned to
	 * the still-existing cgroup leaf -- see cgroup_create()'s own
	 * comment on why leaves are never rmdir()'d) isn't torn down
	 * early; closed alongside cgroup_fd/pidfd on removal.
	 */
	int bpf_prog_fd;
};

/*
 * Creates the cgroup v2 leaf described by lim, applying memory.max,
 * pids.max and cpu.max. On success *out_fd is an O_PATH descriptor
 * on the leaf directory, suitable for clone_args.cgroup.
 */
int cgroup_create(const struct cgroup_limits *lim, int *out_fd);

/*
 * Mounts an overlayfs at ov->merged: lowerdir must already exist and
 * be populated (never auto-created -- a silently-empty lowerdir would
 * mean a silently-broken container); upperdir/workdir/merged are
 * created if missing. On success ov->merged is a mount point ready
 * for mountns_pivot().
 */
int overlay_create(const struct overlay_spec *ov);

/*
 * Creates and starts a container per spec: clone3 into new
 * PID/MNT/UTS/NET/CGROUP namespaces, places the child atomically
 * into the cgroup opened by cgroup_create, mounts spec->ov and
 * pivot_roots into it, and execve's argv[0] with argv/envp. On
 * success fills out with the child's pid, cgroup fd and pidfd.
 */
int container_create(const struct container_spec *spec, struct container_handle *out);

/*
 * Race-free wait via the handle's pidfd. On return *exit_status
 * holds the child's exit status as reported by waitid().
 */
int container_wait(const struct container_handle *h, int *exit_status);

#endif /* CONTAINER_H */
