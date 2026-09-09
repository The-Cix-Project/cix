#ifndef CONTAINER_H
#define CONTAINER_H

#include <stdint.h>
#include <sys/types.h>

#include "cixinit.h"

struct ns_config {
	unsigned long clone_flags;
	const char *hostname;
};

struct cgroup_limits {
	const char *name;
	long long memory_max;
	/*
	 * Issue #52: cgroup v2 memory.swap.max, in bytes. **-1 means
	 * unset**, unlike every other limit here, where 0 does -- because 0
	 * is a real and useful value for this one: it forbids the container
	 * from swapping at all, so it is reclaimed or OOM-killed rather
	 * than pushed to disk. A limit whose "off" value and whose
	 * strictest setting were the same number could not express that.
	 *
	 * Every initialiser memsets this struct to zero, so each one has to
	 * set this back to -1 explicitly; that is the price of 0 meaning
	 * something.
	 */
	long long memory_swap_max;
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
	/*
	 * Mount a cgroup2 filesystem at /sys/fs/cgroup inside the
	 * container (#257).
	 *
	 * Without it a cixd running INSIDE a container has no cgroup tree
	 * to create under -- src/cgroup.c writes to a hardcoded
	 * /sys/fs/cgroup, the container gets a fresh sysfs with nothing
	 * mounted there, and every container it tries to create dies at
	 * mkdir with ENOENT. That is not a corner case: it is why nesting
	 * did not work, and running this platform's own test suite needs
	 * nesting.
	 *
	 * Deliberately opt-in rather than always-on. The pivot happens
	 * while the child still holds full capabilities (they are dropped
	 * afterwards), so an unconditional mount would hand every
	 * container a writable cgroup tree whether it had any business
	 * with one or not. container_create() sets this only for a
	 * container that has its own cgroup namespace AND was explicitly
	 * granted CAP_SYS_ADMIN -- see its own comment for why both.
	 */
	int mount_cgroup2;
	/*
	 * ADR-0262/#336: files to bind over the container's own /proc, so a
	 * process inside reads its OWN cgroup's limits.
	 *
	 * The runtime is told the directory and the names and nothing else
	 * -- deliberately. What those files mean, how they are synthesised
	 * and which server answers them is the daemon's business; this
	 * layer only knows "bind these names from here over /proc/<name>",
	 * which keeps src/ free of any dependency on daemon/.
	 *
	 * procfuse_dir is a HOST absolute path. mountns_pivot() runs after
	 * pivot_root(), so it reaches it through put_old, in the one window
	 * where both that path and the container's fresh /proc exist.
	 *
	 * NULL or empty means bind nothing, which is the correct state for
	 * a container that opted out AND for a host whose server is not
	 * running -- the container then reads the host's real /proc, which
	 * is what every container read before this existed.
	 */
	const char *procfuse_dir;
	const char *const *procfuse_files;
	int procfuse_file_count;
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
	int has_address; /* 0: this network is pure L2 -- address_ip_be is
	                   * meaningless, no default route gets installed */
	uint32_t address_ip_be;
	int prefix_len;
	/*
	 * What this attachment is called INSIDE the container (ADR-0259).
	 * Empty means "the platform picks", which is eth<idx> -- the name
	 * this field's absence produced for the platform's whole history,
	 * so an unset value changes nothing.
	 *
	 * A fixed array rather than the `const char *` bridge uses above,
	 * deliberately: this is read by container_net_child_configure(),
	 * which runs in the cloned child, and a value living in the struct
	 * cannot outlive its own pointee. 16 is IFNAMSIZ.
	 */
	char ifname[16];
	/*
	 * A bridge INSIDE the container that this attachment becomes a
	 * port of (#30). Empty -- the default, and what every attachment
	 * before this had -- means the veth is configured directly, which
	 * is the ordinary case.
	 *
	 * This exists because an access point is a bridge between a radio
	 * and a wired segment, and the two halves arrive from opposite
	 * directions. hostapd owns the radio: it puts wlan0 into whatever
	 * bridge its own `bridge=` names, and it will create that bridge
	 * if it has to -- but it knows nothing about this container's
	 * other interfaces and will never enslave one. Cix owns the wired
	 * half: it makes the veth pair and moves one end into the netns.
	 * Nothing joined them, so the AP could carry traffic between its
	 * clients and nowhere else.
	 *
	 * Cix creates the bridge and enslaves the veth at container
	 * creation, before any service starts, so hostapd finds an
	 * existing bridge and adds the radio to it. That ordering is the
	 * whole design: the alternative is a service inside the container
	 * running `ip link`, which would need iproute2 in the image, a
	 * shell to drive it, and a network the platform did not build --
	 * three things this platform deliberately does not have.
	 *
	 * A port carries no address of its own, so when this is set the
	 * container_ip_be above is assigned to the BRIDGE instead of to
	 * the veth. An attachment that wants no address at all leaves
	 * container_ip_be zero and gets a pure port, which is what an AP's
	 * own access-side attachment usually wants -- its clients are
	 * addressed by DHCP from elsewhere on the segment, and the AP
	 * needs no presence on it.
	 *
	 * Several attachments may name the same bridge; the first creates
	 * it and the rest join it. A fixed array for the same reason
	 * ifname above is one: this is read in the cloned child.
	 */
	char container_bridge[16];
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
	/*
	 * ADR-0179 phase 2c option (a): for a userns container this is its own
	 * per-container rootfs -- a CoW copy of the image (`cp --reflink=auto`),
	 * host-uid-0 on disk. container_create() id-maps it (idmap_bind) and the
	 * child move_mounts + pivots straight into it: a plain ext4 mount, not an
	 * overlay, so none of overlay's own whiteout/tmpfile ops (which need
	 * privilege over the layer fs's init-owned s_user_ns -- unachievable from
	 * inside the userns) ever run. NULL for non-userns (classic overlay path).
	 */
	const char *userns_rootfs;
	/*
	 * ADR-0207 phase 2: when set, `merged` IS this container's own
	 * per-container rootfs (a btrfs snapshot of its image's subvolume,
	 * or -- under the test-only copy mode -- a plain copied tree), and
	 * there is no overlay at all: lowerdir/upperdir/workdir are unused,
	 * the child self-binds `merged` (pivot_root needs a mount point, a
	 * plain directory is not one -- the same lesson the userns path
	 * learned in ADR-0179 phase 2b) and pivots straight in. 0 keeps
	 * the classic overlay path, which non-btrfs hosts (including the
	 * btrfs-incapable dev sandbox) run until ADR-0207 phase 4.
	 */
	int direct_rootfs;
	const char *merged;
	/*
	 * Real ext4 project-quota id (Part 4, bare-metal-readiness plan,
	 * ADR-0062) to tag upperdir with via FS_IOC_FSSETXATTR, so every
	 * file this container's overlay ever writes counts against it.
	 * 0 means "no quota requested" -- skip tagging entirely, the same
	 * zero-means-off convention cgroup_limits' own optional fields
	 * already use. See src/overlay.c's own comment for exactly what
	 * the ioctl does and why FS_XFLAG_PROJINHERIT is required, not
	 * optional. Only meaningful when upperdir's own backing filesystem
	 * is NOT btrfs -- see quota_bytes below for that case.
	 */
	uint32_t project_id;
	/*
	 * Real btrfs qgroup hard limit in bytes for upperdir (task #678,
	 * ADR-0103) -- 0 means "no quota requested," same convention
	 * project_id above already uses. Only meaningful when upperdir's
	 * own backing filesystem IS btrfs: overlay_create() detects this
	 * itself (overlay_backing_is_btrfs()) and creates upperdir as a
	 * real btrfs subvolume instead of a plain directory in that case,
	 * since qgroups are subvolume-scoped, not arbitrary-directory-
	 * scoped like ext4 project quotas. The caller populates this
	 * unconditionally alongside project_id whenever a quota was
	 * requested at all -- overlay_create() itself decides which of
	 * the two mechanisms actually applies, based on what it finds.
	 */
	long long quota_bytes;
};

/*
 * True if path's own backing filesystem is btrfs (a plain statfs(2)
 * check against CIX_BTRFS_SUPER_MAGIC), false for anything else
 * (including a statfs() failure -- a path that can't even be statfs'd
 * is conservatively never treated as btrfs). Exposed publicly (not
 * static to src/overlay.c) because daemon/src/main.c's own container-
 * create handler needs the same answer before overlay_create() ever
 * runs, to decide whether to take the existing ext4 quotactl(2) path
 * at all (btrfs has no quotactl(2) project-quota support -- calling
 * it against a btrfs-backed path would just fail) -- one real check,
 * not two independently-written statfs() calls that could drift.
 */
int overlay_backing_is_btrfs(const char *path);

/*
 * Tags a directory with an ext4 project id, with PROJINHERIT set so
 * everything created inside it inherits the tag -- without which only
 * the directory itself would count against the quota and every file in
 * it would be silently exempt from the limit the tag exists to enforce.
 *
 * Exported (issue #93) because a volume needs exactly this and nothing
 * else overlay_create() does. Duplicating the two ioctls daemon-side
 * would have been a second implementation of a thing that has to stay
 * identical, on a code path where getting it subtly wrong means a quota
 * that reports as applied and is not.
 *
 * `what` names the directory in the diagnostic. Returns 0, or
 * OVERLAY_ERR_QUOTA -- and deliberately fails loud: a quota requested
 * but silently not applied is a correctness bug wearing a false
 * promise, not a missing optional capability.
 */
int overlay_tag_project_id(const char *dir, unsigned int project_id, const char *what);

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
 * Opt-in extra environment variables merged into the container's own
 * envp at execve() time (POST /v1/containers' own "env" field) -- a
 * generous fixed bound, same reasoning CONTAINER_MAX_SYSCTLS above
 * already gives. Unlike sysctls there's no dedicated struct/field on
 * container_spec below: envp (already declared further down, already
 * used internally by pkg.c's own build-sandbox spec construction) is
 * this project's existing flat, NULL-terminated "KEY=VALUE" execve()
 * shape, so daemon/src/main.c's POST /v1/containers parse renders the
 * "env" JSON object straight into that shape rather than inventing a
 * second, redundant key/value representation on the spec itself; only
 * daemon/include/registry.h's echo-back struct needs the key/value
 * split, and it recovers that directly from spec->envp's own strings
 * (see registry_create()).
 */
#define CONTAINER_MAX_ENV 32
#define CONTAINER_ENV_KEY_MAX 128
#define CONTAINER_ENV_VALUE_MAX 384
/* "KEY=VALUE\0" -- generous enough for both bounds above plus the '='
 * separator and the terminating NUL, with room to spare. */
#define CONTAINER_ENV_ENTRY_MAX 512

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

/*
 * argv itself (struct container_spec.argv below) is a bare NULL-
 * terminated char*const* -- these two bounds are the caller-side
 * (daemon/src/main.c's POST /v1/containers parse) size a request's
 * "cmd" array is validated against, and the per-token echo-storage
 * bound daemon/include/registry.h's struct registry_entry.cmd[] reuses
 * for GET-visibility -- named here rather than left as bare literals,
 * same "one source of truth for every container-related size bound"
 * reasoning CONTAINER_MAX_FILES's own comment above already gives.
 */
#define CONTAINER_MAX_ARGV 64
#define CONTAINER_ARGV_MAX 256
/* Generous for a config file or a shell script; HTTP_MAX_REQUEST_SIZE
 * (daemon/include/http.h, 1MiB) already caps the whole request body
 * regardless, so this is a sane per-file ceiling on top of an existing
 * hard one, not the only bound. */
#define CONTAINER_FILE_CONTENT_MAX 65536

/*
 * A generous fixed bound -- container_caps_drop()'s own default
 * deny-list has under two dozen entries total (see src/container_caps.c),
 * and a real workload needing more than a small handful put back is
 * exactly the "this belongs in the ADR-0166-class full user-namespace
 * follow-up, not one more opt-in exception" signal (issue #29).
 */
#define CONTAINER_MAX_CAP_ADD 8
/* Longest real capability name (CAP_CHECKPOINT_RESTORE) is 22 bytes
 * plus NUL; generous headroom above that, same posture
 * CONTAINER_SYSCTL_KEY_MAX already takes for its own longest real case. */
#define CONTAINER_CAP_NAME_MAX 32

/*
 * Issue #88: one persistent volume mounted into a container. host_path
 * is a real directory that OUTLIVES this container -- deleting the
 * container never touches it -- bind-mounted at mount_path inside the
 * container's own rootfs before pivot_root.
 */
#define CONTAINER_MAX_VOLUMES 8

/* ADR-0260: two sockets plus one output pipe per service (include/cixinit.h). */
#define CONTAINER_MAX_KEEP_FDS (2 + CIXINIT_MAX_SERVICES)

struct container_volume {
	char host_path[4096];
	char mount_path[256];
	int read_only;
};

struct container_spec {
	struct ns_config ns;
	struct cgroup_limits cg;
	struct overlay_spec ov;
	struct mount_spec mnt;
	/*
	 * Issue #88: persistent storage bind-mounted in before pivot_root.
	 * Deliberately part of the spec rather than something layered on
	 * afterwards -- the mounts must exist inside the container's own
	 * mount namespace, which only exists between clone3() and pivot.
	 */
	struct container_volume volumes[CONTAINER_MAX_VOLUMES];
	int volume_count;
	/*
	 * ADR-0207 phase 3: when set, this userns container's rootfs is a
	 * host-uid-0-owned btrfs snapshot presented through an id-mapped
	 * mount, NOT a chowned copy -- container_create() applies
	 * MOUNT_ATTR_IDMAP to the already-open detached rootfs (and
	 * volume) fds after the child's uid/gid maps are written; a
	 * fork-inherited fd references the same mount object, so the
	 * child's move_mount sees the mapping with no fd-passing at all.
	 * 0 keeps the ADR-0179 phase-2b copy+chown presentation, which is
	 * the proven path on non-btrfs backing.
	 */
	int userns_idmap;
	/*
	 * Parent-opened detached trees (open_tree(OPEN_TREE_CLONE)) for
	 * each volume of a userns_idmap container, id-mapped alongside the
	 * rootfs so a host-0-owned volume is writable by the mapped root
	 * (the EOVERFLOW gap ADR-0179 flagged). -1 = no fd, the child does
	 * its ordinary bind mount. Parallel to volumes[] by index.
	 */
	int volume_idmap_fds[CONTAINER_MAX_VOLUMES];
	/*
	 * ADR-0179 (issue #29): user-namespace mapping. userns_enabled
	 * requests CLONE_NEWUSER with the container's own 0..userns_len-1
	 * mapped onto host [userns_uid_base, +len) / [userns_gid_base,
	 * +len). The maps are written by the PARENT into
	 * /proc/<child>/{setgroups,gid_map,uid_map} (deny-then-gid-then-
	 * uid, the documented kernel ordering) while the child blocks on
	 * the userns sync pipe before touching anything -- and the merged
	 * overlay is re-presented through a real id-mapped mount
	 * (mount_setattr(MOUNT_ATTR_IDMAP)) created BEFORE clone3(), so
	 * the child's own mount-namespace copy already carries the mapped
	 * ownership view (shared, read-only lowerdirs are never chown'd --
	 * see the ADR's own retraction of that idea).
	 */
	int userns_enabled;
	long long userns_uid_base;
	long long userns_gid_base;
	long long userns_len;
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
	 * Opt this container's memory into kernel samepage merging (#260).
	 *
	 * KSM merges nothing that has not volunteered, so without this the
	 * host-level scanner (GET/PUT /v1/system/ksm) runs and saves
	 * nothing. Off unless asked for: merging is not free -- the scanner
	 * has to compare pages, and a container whose memory is genuinely
	 * unique pays that cost for no return.
	 *
	 * Applied with prctl(PR_SET_MEMORY_MERGE) in the child rather than
	 * madvise() per mapping, because the payload is an arbitrary
	 * program this daemon does not control and has no call sites in.
	 */
	int ksm;
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
	/*
	 * Opt-in exceptions to container_caps_drop()'s own fixed default
	 * deny-list (see include/internal.h/src/container_caps.c) -- e.g.
	 * "CAP_SYS_TIME" for an NTP daemon that genuinely needs to call
	 * clock_settime(). cap_add_count == 0 (every memset(&spec, 0, ...)
	 * caller, unchanged behavior) means the fixed default list applies
	 * with no exceptions. Only a capability that's actually on the
	 * default deny-list is meaningful here -- one that was never
	 * dropped in the first place is silently a no-op, not an error
	 * (see container_caps_drop()'s own comment for why).
	 */
	char cap_add[CONTAINER_MAX_CAP_ADD][CONTAINER_CAP_NAME_MAX];
	int cap_add_count;
	char *const *argv;
	char *const *envp;
	/*
	 * Opt-in stdout/stderr redirection: capture_output == 0 (the
	 * default for every memset(&spec, 0, ...) caller, unchanged
	 * behavior) means the child inherits the daemon's own stdout/
	 * stderr exactly as before. When set, stdout_fd/stderr_fd
	 * (caller-owned, typically both ends of the same pipe -- see
	 * pkg.c's build-container diagnostic capture) are dup2()'d onto
	 * the child's fd 1/2 immediately before the final execve(), so a
	 * caller with no other way to see a launched program's real
	 * output (no console attached, no shared filesystem to tee into)
	 * can still capture it.
	 */
	int capture_output;
	int stdout_fd;
	int stderr_fd;
	/*
	 * ADR-0260: fds the child keeps across execve() -- cix-init's
	 * control and report sockets and the per-service output pipes. The
	 * daemon creates every one of them close-on-exec so nothing else it
	 * forks inherits them, and the child clears FD_CLOEXEC on exactly
	 * these immediately before execve(). Their numbers survive clone3()
	 * unchanged, which is what lets the daemon name them on cix-init's
	 * argv (include/cixinit.h).
	 */
	int keep_fds[CONTAINER_MAX_KEEP_FDS];
	int keep_fd_count;
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
	/*
	 * Read end of an internal, always-on diagnostic pipe (see
	 * container_create()'s own comment): the child's pre-exec setup
	 * failures and a failed final execve() both write a human-readable
	 * "step: strerror(errno)" line here instead of (or in addition to)
	 * the bare numeric exit code every caller already sees. -1 once
	 * container_read_diag() has consumed it (one-shot; see that
	 * function's own comment for why a single read is always enough).
	 */
	int diag_fd;
};

/*
 * Creates the cgroup v2 leaf described by lim, applying memory.max,
 * pids.max, cpu.max and cpuset.cpus. On success *out_fd is an O_PATH
 * descriptor on the leaf directory, suitable for clone_args.cgroup.
 */
int cgroup_create(const struct cgroup_limits *lim, int *out_fd);

/*
 * Hands a container's own cgroup subtree to it, so it can create
 * cgroups inside it (issue #224).
 *
 * A container running this platform's own test suite has to create
 * containers, and creating a container means creating a cgroup. With
 * CAP_SYS_ADMIN it can already unshare a cgroup namespace and mount
 * cgroup2 -- measured directly -- and then cannot write a single
 * directory inside it, because the cgroup belongs to real root while
 * the container's root is a mapped uid. Kernel-documented delegation is
 * the answer, and it is a chown rather than a capability: the directory
 * plus cgroup.procs, cgroup.subtree_control and cgroup.threads, the
 * exact set the kernel's delegation guidance names. systemd does the
 * same thing for the same reason.
 *
 * Deliberately NOT delegating the resource-limit files (memory.max,
 * pids.max, cpu.max): those are the budget this platform imposed, and a
 * delegatee that can raise its own limits has not been limited. The
 * kernel's delegation model is built around exactly that split, which
 * is why it names only the four.
 *
 * uid/gid are the container's own mapped base -- its root.
 */
int cgroup_delegate(const struct cgroup_limits *lim, long long uid, long long gid);

/*
 * Records "prefix: strerror(errno)" as container_create_last_error_
 * step()'s own return value -- called from cgroup.c and container.c
 * alike (both are parent-side, single-threaded, pre-clone3 failure
 * points) right when errno is still fresh from the failing call.
 * Defined in container.c alongside the getter; see that function's
 * own doc comment for why a plain static buffer is safe here.
 */
void container_set_last_error_step(const char *prefix);

/*
 * Best-effort: enables every cgroup v2 controller this project's own
 * container/host-stats code needs (io, cpuset, memory, pids, cpu) at
 * the root's own subtree_control, once, at daemon startup -- see
 * src/cgroup.c's own comment for the full "why now, why not fatal,
 * and why memory/pids/cpu specifically" rationale (a real gap found
 * live: a genuinely fresh, systemd-less cgroup v2 hierarchy delegates
 * nothing by default). Needed for cgroup_read_io_totals() to ever
 * report nonzero I/O, and for cgroup_limits.memory_max/pids_max/
 * cpu_max/cpuset_cpus to ever actually take effect rather than fail
 * container creation outright.
 */
/*
 * Issue #85: creates/updates a parent cgroup carrying the AGGREGATE limit
 * for a group of containers, and delegates controllers to its subtree.
 * Children are then created by passing "<parent>/<child>" as a
 * cgroup_limits name. A per-container limit is not a budget: applying one
 * to each of N concurrent containers permits N times the intended
 * ceiling, which on a real box starved cixd itself off the run queue.
 * Returns 0 on success, -1 (errno set) if the parent can't be created.
 */
int cgroup_create_parent(const struct cgroup_limits *lim);

void cgroup_enable_controllers(void);

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

/* cpu.max's raw "QUOTA PERIOD" text, trailing newline stripped -- see
 * this function's own doc comment in src/cgroup.c for why it's
 * returned verbatim rather than reinterpreted. */
int cgroup_read_cpu_max(int cgroup_fd, char *out, size_t out_size);
int cgroup_read_cpuset(int cgroup_fd, char *out, size_t out_size);

/*
 * One cgroup v2 pressure-stall (PSI) file's own "some"/"full" lines
 * (cpu.pressure/io.pressure/memory.pressure) -- avg10/avg60/avg300 are
 * percentages (0-100, e.g. "3.03" means 3.03% of the last 10s some/all
 * tasks were stalled), total is cumulative stalled microseconds since
 * boot. "full" is meaningless for cpu.pressure specifically (a single
 * task can't be the only one running AND have every task stalled at
 * once) and the kernel simply omits that line there -- left zeroed,
 * not an error, same convention as every other missing-key case in
 * this file.
 */
struct cgroup_pressure {
	double some_avg10, some_avg60, some_avg300;
	long long some_total;
	double full_avg10, full_avg60, full_avg300;
	long long full_total;
};

/*
 * Reads cgroup_fd's own filename (one of cpu.pressure/io.pressure/
 * memory.pressure). Best-effort like the three readers above: *out is
 * left fully zeroed (not an error) if the file doesn't exist at all --
 * a kernel built without CONFIG_PSI, or a cgroup v1 host, has no such
 * file, and that's a real, non-fatal case this platform must run on.
 */
void cgroup_read_pressure(int cgroup_fd, const char *filename, struct cgroup_pressure *out);

/*
 * overlay_create()'s own distinct failure codes -- every current caller
 * only ever checks `!= 0`, so these are additive, not a behavior
 * change. OVERLAY_ERR_MOUNT_ERRNO_BASE - errno (for errno in
 * [1, OVERLAY_ERR_MOUNT_ERRNO_MAX]) encodes the real mount(2) errno
 * directly for the one failure mode (the actual overlay mount syscall
 * itself) most likely to need it -- e.g. EINVAL is the classic symptom
 * of lowerdir's filesystem not returning real d_type from readdir(), a
 * well-known overlayfs mount precondition. OVERLAY_ERR_MOUNT_OVERLAY is
 * the fallback for an errno too large to encode this way.
 *
 * The cap here is deliberately generous (115, not just the handful of
 * errnos mount(2) itself realistically returns) because src/container.c
 * reuses this exact same numeric encoding for its own final execve()
 * failure too, in the same shared exit-status byte range (140-254) --
 * the two are mutually exclusive within a single run (execve() is only
 * ever reached once overlay_create() has already succeeded), so this
 * is unambiguous, not a collision. A narrower cap already missed a
 * real, confirmed-live case: ELIBBAD (80, "corrupted shared library"),
 * found investigating a real hostbuild failure on 192.168.15.95.
 */
enum overlay_error {
	OVERLAY_ERR_STAT_LOWERDIR = -1,
	OVERLAY_ERR_MKDIR_UPPERDIR = -2,
	OVERLAY_ERR_QUOTA = -3,
	OVERLAY_ERR_MKDIR_WORKDIR = -4,
	OVERLAY_ERR_MKDIR_MERGED = -5,
	OVERLAY_ERR_OPTS_TOO_LONG = -6,
	OVERLAY_ERR_MOUNT_OVERLAY = -7,
	OVERLAY_ERR_MOUNT_ERRNO_MAX = 115,
	OVERLAY_ERR_MOUNT_ERRNO_BASE = -100,
};

/*
 * Mounts an overlayfs at ov->merged: lowerdir must already exist and
 * be populated (never auto-created -- a silently-empty lowerdir would
 * mean a silently-broken container); upperdir/workdir/merged are
 * created if missing. On success ov->merged is a mount point ready
 * for mountns_pivot(). On failure, returns a negative enum
 * overlay_error value identifying which step failed (see above) --
 * every current caller treats any nonzero return as failure, so this
 * refines rather than changes existing behavior.
 */
int overlay_create(const struct overlay_spec *ov);

/*
 * overlay_create() minus the final mount(2) (ADR-0179 phase 2c): creates and
 * quota-tags upperdir/workdir and the merged mountpoint. The userns path uses
 * this then builds an id-mapped overlay via the fd-based mount API instead.
 */
int overlay_prepare_dirs(const struct overlay_spec *ov);

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
 * success fills out with the child's pid, cgroup fd, pidfd and
 * diag_fd.
 *
 * A real, previously-silent gap this diag_fd closes: every one of the
 * child's own pre-exec setup steps (mountns_make_private,
 * overlay_create's six named steps plus a real mount(2) errno,
 * mountns_pivot, container_dev_mknod, sethostname, network
 * configuration, prctl) and the final execve() itself already had a
 * rich, disjoint numeric exit-code encoding (see the child branch's
 * own comments) -- but the ONLY place that encoding was ever
 * explained in human terms was a perror() to this process's own
 * stdin/stderr, which on a real installed box (cixd as PID 1, no
 * attached console, no systemd journal) reaches nobody. On failure of
 * ANY of those steps, before exit()ing with its own numeric code, the
 * child now ALSO writes one "step: strerror(errno)" line to this
 * pipe -- container_read_diag() below is how a caller turns "why did
 * my container die" from "decode this exit code by hand against a
 * table in a comment" into a real, readable answer, with zero extra
 * ceremony on the caller's part (this pipe is unconditional, not an
 * opt-in like capture_output above, which is a different feature: it
 * captures the RUNNING container's own stdout/stderr for build
 * logging, not this process's own setup diagnostics, and only starts
 * capturing right before the final execve -- too late for every
 * earlier step this pipe covers). On success, its write end is
 * O_CLOEXEC and simply vanishes at the exec() that made it
 * unnecessary -- no attempt to also capture the actual container
 * workload's own output through it.
 */
int container_create(const struct container_spec *spec, struct container_handle *out);


/*
 * True if name is a recognized cap_add exception name (e.g. "CAP_SYS_TIME")
 * -- the same table container_caps_drop() itself checks against, exposed
 * here so a caller building a container_spec (daemon/src/main.c's POST
 * /v1/containers parse) can reject an unrecognized name at REST-validation
 * time with a real 400, instead of only discovering the typo once the
 * container fails to start (container_caps_drop() itself still re-checks
 * this independently -- see its own comment).
 */
int container_cap_name_valid(const char *name);

/*
 * The exact parent-side step container_create()'s own most recent
 * failed call left behind (e.g. "cgroup_create: write cpu.max") --
 * always valid immediately after container_create() returns -1,
 * meaningless otherwise. This project's event loop is single-
 * threaded, so a plain static buffer is safe here the same way
 * several other single-threaded daemon globals already are; this is
 * the parent-side equivalent of the existing diag_pipe mechanism,
 * which only ever covers the CHILD's own post-fork pre-exec steps.
 * Added after a real, otherwise-undiagnosable production regression
 * (ADR-0165, ROADMAP Part 165): the caller only ever had a bare
 * errno to go on, not which of container_create()'s several distinct
 * syscalls actually produced it.
 */
const char *container_create_last_error_step(void);

/*
 * Race-free wait via the handle's pidfd. On return *exit_status holds
 * the child's raw status as reported by waitid()'s si_status, and
 * *term_signal disambiguates what that number means (issue #78): 0 if
 * the child exited normally (so *exit_status is a real exit code), or
 * the signal number if it was killed by a signal (so *exit_status is
 * that same signal number, NOT an exit code). Callers that only care
 * whether the exit was clean can keep testing *exit_status == 0 (a
 * signal-kill always yields a nonzero *exit_status); callers that
 * report or decode the status must consult *term_signal so a SIGKILL
 * (signal 9, how stop/delete terminate a container) is never shown as
 * "exit code 9". Pass NULL for term_signal to ignore it.
 */
int container_wait(const struct container_handle *h, int *exit_status, int *term_signal);

/*
 * Reads whatever diagnostic text (if any) h->diag_fd's write end
 * received before it closed -- always true by the time a caller
 * reaches this, since container_wait() already reaped the process
 * (closing it directly on a setup/exec failure) or the process
 * exec'd successfully (closing it via O_CLOEXEC) -- so this never
 * blocks. NUL-terminates into buf (truncating, never overflowing, if
 * the real message somehow exceeds bufsize). Closes and invalidates
 * h->diag_fd itself (set to -1) -- a one-shot read, matching every
 * other place in this codebase that reads a diagnostic pipe exactly
 * once after reaping its writer (see pkg.c's own build-output
 * capture). Returns the number of bytes read (0 if the container
 * exited cleanly via a successful exec, with nothing ever written),
 * or -1 on a genuine read(2) error.
 */
ssize_t container_read_diag(struct container_handle *h, char *buf, size_t bufsize);

/*
 * Decodes a container's raw exit_status (container_wait()'s own
 * output, the same 0-119/127/130-136/141-255 numeric scheme
 * container_create()'s own comment documents) into a short, fixed
 * category string ("clean exit", "mountns_make_private failed",
 * "overlay: lowerdir stat failed", "exec: file not found", etc.).
 * term_signal is container_wait()'s companion output (issue #78): when
 * nonzero the child was killed by that signal, so exit_status is a
 * signal number, not an exit code, and the string reads "killed by
 * signal N (NAME)" instead of "exited with status N" -- this is what a
 * deliberate stop/delete (SIGKILL) produces. Pass 0 for a normal exit.
 * Deliberately just the CATEGORY, not the real errno text (that part
 * comes from container_read_diag() above when available, and this
 * function's own output is the fallback for the rarer case -- a
 * daemon restart losing an already-exited container's diag pipe, or
 * a genuinely exhausted diag pipe -- where it isn't). Always fills
 * buf with something non-empty; never fails.
 */
/*
 * "SIGTERM" for 15, and so on; NULL for a signal this does not name.
 * Public because the package builder needs it too: a build that exits
 * 128+N was killed by signal N, and saying which one is the difference
 * between a useful report and a number.
 */
const char *container_signal_name(int sig);

void container_decode_exit_status(int exit_status, int term_signal, char *buf, size_t bufsize);

#endif /* CONTAINER_H */
