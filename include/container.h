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
	/* Raw cgroup v2 cpuset.cpus range-list syntax (e.g. "0-1,3"),
	 * passed straight through to the real cgroup file -- same
	 * pass-through convention as cpu_max, no reinterpretation. NULL
	 * means no CPU affinity restriction (every online CPU, the
	 * default cpuset.cpus already inherits from the root). */
	const char *cpuset_cpus;
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
	int has_gateway; /* 0: this network is pure L2 -- gateway_ip_be is
	                   * meaningless, no default route gets installed */
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
	/*
	 * Real ext4 project-quota id (Part 4, bare-metal-readiness plan,
	 * ADR-0062) to tag upperdir with via FS_IOC_FSSETXATTR, so every
	 * file this container's overlay ever writes counts against it.
	 * 0 means "no quota requested" -- skip tagging entirely, the same
	 * zero-means-off convention cgroup_limits' own optional fields
	 * already use. See src/overlay.c's own comment for exactly what
	 * the ioctl does and why FS_XFLAG_PROJINHERIT is required, not
	 * optional.
	 */
	uint32_t project_id;
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

/*
 * A generous fixed bound, mirroring CONTAINER_MAX_DEVICES's own
 * reasoning -- host hardware, not a daemon-created resource, so no
 * natural daemon-side ceiling to mirror; enough for a router with a
 * handful of physical uplinks.
 */
#define CONTAINER_MAX_INTERFACES 16
/*
 * IFNAMSIZ (linux/if.h), kept as a bare literal rather than pulling a
 * whole extra header into this foundational one -- same precedent
 * daemon/include/network.h's own NETWORK_NAME_MAX already set.
 */
#define CONTAINER_IFNAME_MAX 16

/*
 * A generous fixed bound, same reasoning CONTAINER_MAX_DEVICES already
 * gives -- enough for a router's config file plus a startup script
 * plus a few more, without being unbounded.
 */
#define CONTAINER_MAX_SYSCTLS 32
/*
 * "net." plus every dot-separated component of a real sysctl name
 * (e.g. "net.ipv4.conf.all.rp_filter") -- generous, no real sysctl
 * name approaches this.
 */
#define CONTAINER_SYSCTL_KEY_MAX 128
#define CONTAINER_SYSCTL_VALUE_MAX 64

/*
 * One net.* sysctl to apply inside the container's own netns (see
 * container_net_apply_sysctl()) -- deliberately restricted to the
 * net.* tree at validation time (daemon/src/main.c), since that's the
 * one sysctl subtree the kernel actually namespaces end to end; most
 * others (vm.*, fs.*, ...) are host-wide regardless of netns and
 * would be a real container-escape-adjacent primitive if allowed here.
 */
struct container_sysctl {
	char key[CONTAINER_SYSCTL_KEY_MAX];
	char value[CONTAINER_SYSCTL_VALUE_MAX];
};

/*
 * Not part of struct container_spec below -- config-file staging
 * (POST /v1/containers' own "files" field) is a purely daemon-side,
 * pre-clone3() host filesystem write straight into the container's
 * own upperdir, before the runtime library below is ever invoked (see
 * daemon/src/main.c) -- struct container_spec/container_create() never
 * see it. Declared here only to keep every container-related size
 * bound in one place (daemon/include/registry.h already reuses this
 * file's own CONTAINER_MAX_DEVICES the same way, for the same reason).
 */
#define CONTAINER_MAX_FILES 16
#define CONTAINER_FILE_PATH_MAX 256
/* Generous for a config file or a shell script; HTTP_MAX_REQUEST_SIZE
 * (daemon/include/http.h, 1MiB) already caps the whole request body
 * regardless, so this is a sane per-file ceiling on top of an existing
 * hard one, not the only bound. */
#define CONTAINER_FILE_CONTENT_MAX 65536

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
	/*
	 * Opt-in generalized net.* sysctls, applied inside the container's
	 * own netns right after ip_forward above (same call site, same
	 * per-netns reasoning) -- ip_forward stays as its own field since
	 * it's the single most common case; this is the general escape
	 * hatch alongside it, not a replacement.
	 */
	struct container_sysctl sysctls[CONTAINER_MAX_SYSCTLS];
	int sysctl_count;
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
	/*
	 * Opt-in real network-interface passthrough: bare kernel interface
	 * names (e.g. "wlan0"), moved into the container's own netns as-is
	 * -- no veth, no bridge, no rename, no address configured (the
	 * operator's own userspace inside the container, e.g. bird or
	 * dhclient, owns addressing once installed via `pkg install`).
	 * Independent of nets[]/net_count above -- a container can have
	 * passthrough interfaces with zero virtual networks, or both. See
	 * container_net_host_attach_interfaces().
	 */
	char interfaces[CONTAINER_MAX_INTERFACES][CONTAINER_IFNAME_MAX];
	int interface_count;
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
	/*
	 * An fd on /proc/<pid>/ns/net, opened right after the child's
	 * network namespace exists (see container_net_host_attach_interfaces()),
	 * kept open for the container's whole lifetime -- not just at
	 * teardown -- so a container that exits on its own (not via an
	 * explicit DELETE) doesn't lose a passthrough interface to the
	 * kernel's own automatic, unpredictably-named fallback the instant
	 * the last process in that netns exits and nothing else references
	 * it. -1 when spec->interface_count was 0 at creation time.
	 */
	int interfaces_netns_fd;
};

/*
 * Creates the cgroup v2 leaf described by lim, applying memory.max,
 * pids.max, cpu.max and cpuset.cpus. On success *out_fd is an O_PATH
 * descriptor on the leaf directory, suitable for clone_args.cgroup.
 */
int cgroup_create(const struct cgroup_limits *lim, int *out_fd);

/*
 * Best-effort: enables the cgroup v2 io controller at the root's own
 * subtree_control, once, at daemon startup (see src/cgroup.c's own
 * comment for the full "why now, why not fatal" rationale). Needed
 * for cgroup_read_io_totals() below to ever report nonzero I/O.
 */
void cgroup_enable_io_accounting(void);

/*
 * Best-effort, same shape and same call site as cgroup_enable_io_
 * accounting() (enables "+cpuset" at the root's own subtree_control,
 * once, at daemon startup) -- needed for cgroup_limits.cpuset_cpus to
 * ever take effect. Not fatal on failure: a container with a requested
 * cpuset_cpus simply keeps running on every online CPU instead, same
 * "degrade to unrestricted, never fail container creation over it"
 * posture the io controller already has.
 */
void cgroup_enable_cpuset(void);

/*
 * Host-side per-container stats readers (GET /v1/containers/{name}/stats,
 * ADR-0054): each takes the container's own already-open cgroup_fd
 * (struct container_handle.cgroup_fd, an O_PATH fd -- openat() against
 * an O_PATH fd works for regular-file children, no separate open()
 * needed). All three return 0 with best-effort/zeroed output on a
 * missing key or empty file (a stat that hasn't accumulated yet is not
 * an error), -1 only on a real I/O error opening the underlying file.
 */
int cgroup_read_stat_key(int cgroup_fd, const char *filename, const char *key, long long *out);
int cgroup_read_single_value(int cgroup_fd, const char *filename, long long *out, int *out_is_unlimited);
int cgroup_read_io_totals(int cgroup_fd, long long *out_rbytes, long long *out_wbytes,
                           long long *out_rios, long long *out_wios);

/*
 * Mounts an overlayfs at ov->merged: lowerdir must already exist and
 * be populated (never auto-created -- a silently-empty lowerdir would
 * mean a silently-broken container); upperdir/workdir/merged are
 * created if missing. On success ov->merged is a mount point ready
 * for mountns_pivot().
 */
int overlay_create(const struct overlay_spec *ov);

/*
 * Real space consumed by a container's own overlay upperdir (its
 * content diff from the shared lowerdir image), for GET
 * .../stats' "disk.upper_bytes" (ADR-0054). See src/overlay.c's own
 * comment for exactly what counts (content only, not directory-tree
 * overhead).
 */
int overlay_upperdir_size(const char *upperdir_path, long long *out_bytes);

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
