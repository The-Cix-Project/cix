#include "container.h"
#include "internal.h"
#include "linux_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Writes "prefix: strerror(errno)\n" to fd (the diag pipe's write end,
 * always valid in the child by the time this is ever called -- see
 * container_create()'s own comment on why this pipe is unconditional,
 * not opt-in). dprintf() reads errno itself via strerror(), so callers
 * just need to call this immediately after the failing call, exactly
 * like the perror() it replaces.
 */
/*
 * Issue #88: mkdir -p for a volume's mount point inside the container's
 * future root. Needed because a volume may legitimately be mounted at a
 * path the image itself has no concept of (e.g. /home on a minimal
 * rootfs), so requiring the image to pre-create it would make volumes
 * usable only where the image already anticipated them. Child-side and
 * pre-pivot, so it must not depend on anything outside this file.
 */
static int volume_mkdir_p(char *path)
{
	char *p;

	for (p = path + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0755) != 0 && errno != EEXIST) {
			*p = '/';
			return -1;
		}
		*p = '/';
	}
	if (mkdir(path, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static void child_diag(int fd, const char *prefix)
{
	dprintf(fd, "%s: %s\n", prefix, strerror(errno));
}

/*
 * mountns_pivot()'s own distinct step, for the exact same reason
 * overlay_step_names[] (container_decode_exit_status(), below) exists
 * -- ~15 real syscalls behind one function, a flat "child: mountns_pivot"
 * diag message left genuinely no way to tell which one actually failed
 * on a shell-less production box (issue #34's own investigation).
 * Reads errno in the same dprintf() call that resolves the step name,
 * matching child_diag()'s own contract -- no snprintf/other call
 * in between that could touch it first.
 */
static void child_diag_mountns_pivot(int fd, int mountns_pivot_ret)
{
	static const struct {
		int code;
		const char *step;
	} steps[] = {
		{ MOUNTNS_PIVOT_ERR_MKDIR_PUT_OLD, "mkdir(put_old)" },
		{ MOUNTNS_PIVOT_ERR_CHDIR_NEW_ROOT, "chdir(new_root)" },
		{ MOUNTNS_PIVOT_ERR_PIVOT_ROOT, "pivot_root" },
		{ MOUNTNS_PIVOT_ERR_CHDIR_ROOT, "chdir(/)" },
		{ MOUNTNS_PIVOT_ERR_UMOUNT_PUT_OLD, "umount2(put_old)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_PROC, "mkdir(/proc)" },
		{ MOUNTNS_PIVOT_ERR_MOUNT_PROC, "mount(proc)" },
		{ MOUNTNS_PIVOT_ERR_UMOUNT_SYS, "umount2(/sys)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_SYS, "mkdir(/sys)" },
		{ MOUNTNS_PIVOT_ERR_MOUNT_SYS, "mount(sysfs)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_RUN, "mkdir(/run)" },
		{ MOUNTNS_PIVOT_ERR_MOUNT_RUN, "mount(tmpfs /run)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_DEV, "mkdir(/dev)" },
		{ MOUNTNS_PIVOT_ERR_MKDIR_DEV_PTS, "mkdir(/dev/pts)" },
		{ MOUNTNS_PIVOT_ERR_MOUNT_DEV_PTS, "mount(devpts)" },
		{ MOUNTNS_PIVOT_ERR_PATH_TOO_LONG, "path too long" },
	};
	const char *step = "unknown step";
	size_t i;

	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		if (steps[i].code == mountns_pivot_ret) {
			step = steps[i].step;
			break;
		}
	}
	dprintf(fd, "child: mountns_pivot: %s: %s\n", step, strerror(errno));
}

/*
 * Parent-side equivalent of child_diag() above, for every failure
 * point in container_create()/cgroup_create() that runs before
 * clone3() (or its own post-fork parent-side steps) -- these have no
 * child process to write a diag_pipe message through, and this
 * project's single-threaded event loop makes a plain static buffer
 * safe: only ever written immediately before a synchronous -1 return,
 * only ever read by the same caller immediately after, never
 * concurrently. See container_create_last_error_step()'s own doc
 * comment in container.h for the production regression this closes.
 */
static char g_last_error_step[256];

void container_set_last_error_step(const char *prefix)
{
	snprintf(g_last_error_step, sizeof(g_last_error_step), "%s: %s", prefix, strerror(errno));
}

const char *container_create_last_error_step(void)
{
	return g_last_error_step;
}

/*
 * ADR-0179 (issue #29) phase 2: the PARENT writes a freshly-cloned
 * CLONE_NEWUSER child's uid/gid maps, then releases it. The child blocks
 * on userns_pipe before touching anything -- until these maps land it has
 * no valid mapped identity, so any privileged op (the overlay mount
 * included) would run with the wrong credentials. gid_map is written
 * first, then uid_map -- see write_userns_maps() below for why
 * setgroups is no longer denied unconditionally. Each map is the
 * single line "0 <base> <len>": the
 * container's own 0..len-1 onto host [base, base+len). Raw /proc writes,
 * no glibc wrapper needed -- same posture as this file's other raw
 * syscalls.
 */
static int write_proc_line(pid_t pid, const char *which, const char *val)
{
	char path[64];
	int fd;
	ssize_t n;
	size_t len = strlen(val);

	snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, which);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	n = write(fd, val, len);
	close(fd);
	return (n == (ssize_t)len) ? 0 : -1;
}

/*
 * setgroups is left PERMITTED when the kernel allows it, and that is
 * the difference between a user namespace a real workload can run in
 * and one it cannot.
 *
 * user_namespaces(7): "deny" must be written to setgroups before
 * gid_map *only* when the writing process lacks CAP_SETGID in the
 * parent user namespace. cixd is real root and has it, so it can write
 * gid_map directly and leave setgroups usable inside the container.
 * Denying it unconditionally was simply the recipe from the
 * unprivileged case, applied where it was never required.
 *
 * The cost was not theoretical. dnsmasq -- this platform's own DNS --
 * calls setgroups() before setgid() when told to run as a user, so it
 * died at startup with "failed to change group-id to root: Operation
 * not permitted" and exit 5, having written nothing to stderr because
 * it logs to syslog by default. Once userns became the default for new
 * containers (ADR-0207), that turned into "any container that drops or
 * sets groups cannot start", found when dns-1 and dns-2 would not come
 * back after a storage migration recreated them.
 *
 * The fallback is kept and is not dead code: gid_map is attempted
 * first, and only if the kernel refuses it do we deny setgroups and
 * retry, which is the correct sequence for any caller that does not
 * hold CAP_SETGID. So an unprivileged path still works, and a
 * privileged one is no longer needlessly crippled.
 *
 * What denying protects against does not apply here: it exists so an
 * unprivileged user cannot drop supplementary groups to escape a
 * negative-permission ACL. This namespace is created by root over a
 * dedicated, non-overlapping id range that owns nothing on the host.
 */
static int write_userns_maps(pid_t pid, long long base, long long len)
{
	char map[64];

	snprintf(map, sizeof(map), "0 %lld %lld", base, len);
	if (write_proc_line(pid, "gid_map", map) != 0) {
		if (write_proc_line(pid, "setgroups", "deny") != 0)
			return -1;
		if (write_proc_line(pid, "gid_map", map) != 0)
			return -1;
	}
	if (write_proc_line(pid, "uid_map", map) != 0)
		return -1;
	return 0;
}

/*
 * ADR-0179 (issue #29) phase 2c: the new mount API (open_tree/move_mount) has
 * no glibc wrappers -- raw syscalls, x86_64 numbers, same posture as this
 * file's other raw syscalls. Used to hand a userns container's own
 * per-container rootfs from the parent (open_tree -> a detached, unlocked
 * mount) to the child (move_mount into its own namespace, then pivot_root).
 */
/*
 * open_tree()/move_mount() moved to include/linux_compat.h (issue #92
 * part 2) -- a second caller needed them, and this file's own private
 * copies would have become one of two definitions of the same
 * syscalls, which is exactly what the shared header exists to prevent.
 */

/* True if this container was explicitly granted a named capability
 * (#257). cap_add is the operator's own opt-in list, so this asks a
 * question about intent rather than about the current capability set --
 * which at the point it is called is still the full inherited one. */
static int spec_has_cap(const struct container_spec *spec, const char *name)
{
	int i;

	for (i = 0; i < spec->cap_add_count && i < CONTAINER_MAX_CAP_ADD; i++) {
		if (strcmp(spec->cap_add[i], name) == 0)
			return 1;
	}
	return 0;
}

int container_create(const struct container_spec *spec, struct container_handle *out)
{
	int cgroup_fd;
	int pidfd = -1;
	int bpf_prog_fd = -1;
	long ret;
	int net_pipe[2] = { -1, -1 };
	int diag_pipe[2] = { -1, -1 };
	int userns_pipe[2] = { -1, -1 };
	int overlay_lower_fd = -1; /* phase 2c: id-mapped per-container userns rootfs mount */
	int want_net = (spec->net_count > 0);
	int i;

	if (cgroup_create(&spec->cg, &cgroup_fd) != 0)
		return -1;

	/*
	 * A container trusted with CAP_SYS_ADMIN gets its own cgroup
	 * subtree too (#224).
	 *
	 * Implicit rather than a separate knob, because the alternative is
	 * incoherent: CAP_SYS_ADMIN already lets this container unshare a
	 * cgroup namespace and mount cgroup2 -- both measured working --
	 * and withholding the chown leaves it able to mount the tree and
	 * unable to write one directory in it. Delegation adds strictly
	 * less than the capability it follows.
	 *
	 * Only for a userns container, and that is the safety argument: the
	 * subtree is handed to a MAPPED uid that owns nothing on the host,
	 * so "root of its own cgroups" is not root of anything else. A
	 * non-userns container's root is real root, where a chown would
	 * mean nothing and delegation would be a fiction.
	 *
	 * Non-fatal: a host whose cgroup layout refuses this still runs the
	 * container, exactly as it did before. It simply cannot nest.
	 */
	if (spec->userns_enabled) {
		int wants_admin = 0;

		for (i = 0; i < spec->cap_add_count; i++) {
			if (strcmp(spec->cap_add[i], "CAP_SYS_ADMIN") == 0) {
				wants_admin = 1;
				break;
			}
		}
		if (wants_admin &&
		    cgroup_delegate(&spec->cg, spec->userns_uid_base, spec->userns_gid_base) != 0)
			fprintf(stderr,
			        "container_create: cgroup delegation failed for %s -- it will run but "
			        "cannot create nested containers\n",
			        spec->cg.name);
	}

	/*
	 * Before ns_clone3(): CLONE_INTO_CGROUP places the child into
	 * cgroup_fd atomically as part of that syscall, so the device
	 * policy must already be attached for there to be no race window,
	 * and because the child's own container_dev_mknod() calls are
	 * themselves subject to a BPF_DEVCG_ACC_MKNOD check under this
	 * same program.
	 */
	if (container_dev_bpf_attach(cgroup_fd, spec->devices, spec->device_count,
	                              &bpf_prog_fd) != 0) {
		int saved_errno = errno;
		perror("container_create: container_dev_bpf_attach");
		container_set_last_error_step("container_create: container_dev_bpf_attach");
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	if (want_net && pipe(net_pipe) != 0) {
		int saved_errno = errno;
		perror("container_create: pipe(net_pipe)");
		container_set_last_error_step("container_create: pipe(net_pipe)");
		if (bpf_prog_fd >= 0)
			close(bpf_prog_fd);
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	/*
	 * O_CLOEXEC on both ends: the write end vanishes on its own at a
	 * successful execve() (see container_create()'s own header
	 * comment) without any extra code in the child; the read end
	 * (kept open here in the parent past this container's own
	 * clone3()) shouldn't leak into any *later* container's own
	 * clone3()/execve() either.
	 */
	if (pipe2(diag_pipe, O_CLOEXEC) != 0) {
		int saved_errno = errno;
		perror("container_create: pipe2(diag_pipe)");
		container_set_last_error_step("container_create: pipe2(diag_pipe)");
		if (want_net) {
			close(net_pipe[0]);
			close(net_pipe[1]);
		}
		if (bpf_prog_fd >= 0)
			close(bpf_prog_fd);
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	/*
	 * ADR-0179 phase 2: the userns sync pipe. Plain blocking pipe (same
	 * posture as net_pipe) -- the child blocks reading it right after
	 * clone3(), the parent writes the maps then releases it. Only created
	 * when userns is actually requested, so every non-userns create is
	 * byte-for-byte unchanged.
	 */
	if (spec->userns_enabled && pipe(userns_pipe) != 0) {
		int saved_errno = errno;
		perror("container_create: pipe(userns_pipe)");
		container_set_last_error_step("container_create: pipe(userns_pipe)");
		close(diag_pipe[0]);
		close(diag_pipe[1]);
		if (want_net) {
			close(net_pipe[0]);
			close(net_pipe[1]);
		}
		if (bpf_prog_fd >= 0)
			close(bpf_prog_fd);
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	/*
	 * ADR-0179 phase 2c option (a): a userns container uses its OWN
	 * per-container rootfs (a copy of the image, prepared host-side by the
	 * daemon and chown'd to the container's subordinate <base> id --
	 * spec->ov.userns_rootfs), NOT an overlay. Kernel-native id-mapped
	 * overlay proved unachievable from this architecture: overlay's own
	 * whiteout/tmpfile ops need privilege over the layer fs's init-owned
	 * s_user_ns, which the mapped-root child lacks (confirmed live via
	 * /v1/system/kmsg). And id-mapped mounts made the child a non-writable
	 * owner in practice; plain <base> ownership is unambiguous -- the
	 * container's own userns maps <base> straight back to uid 0. So the
	 * parent just open_tree()s the rootfs into a detached (unlocked) mount;
	 * the child move_mounts + pivots straight into it, owning it directly.
	 */
	if (spec->userns_enabled) {
		int saved_errno = 0;
		const char *fail_step = NULL;
		int prep_ret = 0;

		overlay_lower_fd = (int)cix_open_tree(-1, spec->ov.userns_rootfs, OPEN_TREE_CLONE);
		if (overlay_lower_fd < 0) {
			saved_errno = errno;
			fail_step = "container_create: open_tree(userns_rootfs)";
			prep_ret = -1;
		}
		/*
		 * Clear nodev on the container's own rootfs (issue #173).
		 *
		 * boot_init() mounts the containers partition MS_NOSUID|
		 * MS_NODEV -- correct hardening for the host -- and this
		 * detached rootfs is cloned from it, so it inherits nodev.
		 * The effect was that a userns container could open NO device
		 * node at all: not /dev/null, not /dev/zero, read or write,
		 * with every node present at mode 0666 and correctly mapped to
		 * container-root. Non-userns containers never hit it because
		 * OverlayFS is a fresh mount inheriting nothing.
		 *
		 * Done HERE, right after open_tree, rather than beside the
		 * MOUNT_ATTR_IDMAP call below, because that call only runs for
		 * idmap-presented containers. A first attempt put it there and
		 * changed nothing on a real ext4 host, which takes the phase-2b
		 * copy+chown path instead -- the fix has to cover both.
		 *
		 * Rootfs device nodes are deliberate content:
		 * pkg_seed_image_baseline() stages /dev/null, /dev/zero,
		 * /dev/full and /dev/ptmx into every image. What a container
		 * may DO with a device is enforced by the BPF_CGROUP_DEVICE
		 * program attached unconditionally in this function (ADR-0017),
		 * a default-deny allow-list -- the real control, unaffected by
		 * this. Volumes are untouched and keep nodev.
		 */
		if (prep_ret == 0) {
			struct cix_mount_attr devattr;

			memset(&devattr, 0, sizeof(devattr));
			devattr.attr_clr = MOUNT_ATTR_NODEV;
			if (cix_mount_setattr(overlay_lower_fd, "", CIX_AT_EMPTY_PATH, &devattr) != 0) {
				saved_errno = errno;
				fail_step = "container_create: mount_setattr(clear nodev)";
				prep_ret = -1;
			}
		}
		/*
		 * ADR-0207 phase 3: an idmap-presented container's volumes are
		 * detached here too, so the parent can id-map them alongside
		 * the rootfs once the child's maps are written -- a host-0-
		 * owned volume is otherwise unmapped inside the userns and
		 * every write returns EOVERFLOW (ADR-0179's confirmed gap).
		 * The fds are inherited across clone3, and a fork-inherited fd
		 * references the SAME mount object, so the parent's
		 * mount_setattr is visible to the child with no fd passing.
		 */
		if (prep_ret == 0 && spec->userns_idmap) {
			int vi;

			for (vi = 0; vi < spec->volume_count; vi++) {
				((struct container_spec *)spec)->volume_idmap_fds[vi] =
				    (int)cix_open_tree(-1, spec->volumes[vi].host_path,
				                       OPEN_TREE_CLONE | AT_RECURSIVE);
				if (spec->volume_idmap_fds[vi] < 0) {
					saved_errno = errno;
					fail_step = "container_create: open_tree(volume)";
					prep_ret = -1;
					break;
				}
			}
		}
		if (prep_ret != 0) {
			if (overlay_lower_fd >= 0) {
				close(overlay_lower_fd);
				overlay_lower_fd = -1;
			}
			errno = saved_errno;
			if (fail_step != NULL)
				container_set_last_error_step(fail_step);
			close(diag_pipe[0]);
			close(diag_pipe[1]);
			if (want_net) {
				close(net_pipe[0]);
				close(net_pipe[1]);
			}
			close(userns_pipe[0]);
			close(userns_pipe[1]);
			if (bpf_prog_fd >= 0)
				close(bpf_prog_fd);
			close(cgroup_fd);
			errno = saved_errno;
			return -1;
		}
	}

	ret = ns_clone3(spec->ns.clone_flags, cgroup_fd, &pidfd);
	if (ret < 0) {
		int saved_errno = errno;
		perror("container_create: ns_clone3");
		container_set_last_error_step("container_create: ns_clone3");
		close(diag_pipe[0]);
		close(diag_pipe[1]);
		if (want_net) {
			close(net_pipe[0]);
			close(net_pipe[1]);
		}
		if (spec->userns_enabled) {
			close(userns_pipe[0]);
			close(userns_pipe[1]);
			if (overlay_lower_fd >= 0)
				close(overlay_lower_fd); /* the detached rootfs mount is freed with its last fd */
		}
		if (bpf_prog_fd >= 0)
			close(bpf_prog_fd);
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	if (ret == 0) {
		/* Child: from here on we live inside the new namespaces. */
		close(diag_pipe[0]);
		if (want_net)
			close(net_pipe[1]);

		/*
		 * ADR-0179 phase 2: with CLONE_NEWUSER in the clone flags,
		 * block until the parent has written our uid/gid maps. Until
		 * then this process holds no valid mapped identity, and every
		 * privileged step below (the overlay mount included) would run
		 * with the wrong credentials. Closing our own write end first
		 * makes a parent that dies before writing surface as a clean
		 * EOF here (read returns 0) rather than an indefinite hang.
		 */
		if (spec->userns_enabled) {
			char c;

			close(userns_pipe[1]);
			if (read(userns_pipe[0], &c, 1) != 1) {
				child_diag(diag_pipe[1], "child: userns map sync");
				_exit(121);
			}
			close(userns_pipe[0]);
		}

		/*
		 * Each pre-exec setup step gets its own exit code (110-119)
		 * rather than a single uniform 126 -- the parent (and anything
		 * further up the chain that only ever sees a plain wait()
		 * exit status, e.g. pkg_build_completed()'s "build failed
		 * (exit status N)") had no way to tell these ten completely
		 * different failure modes apart otherwise. child_diag() writes
		 * the same "step: strerror(errno)" text perror() used to send
		 * to this process's own stderr into diag_pipe[1] instead (see
		 * container_create()'s own header comment for why this is a
		 * real, always-on diagnostic channel rather than the numeric
		 * code being the only signal a caller ever sees), but a
		 * distinct code STILL matters too: container_decode_exit_status()
		 * needs it as a fallback category when the diag pipe itself
		 * is unavailable (a daemon restart losing an already-exited
		 * container's own pipe fd). 127 is deliberately left alone:
		 * it's execve()'s own existing code below, already a
		 * recognizable, conventional "exec failed" signal independent
		 * of this scheme.
		 */
		if (mountns_make_private() != 0) {
			child_diag(diag_pipe[1], "child: mountns_make_private");
			_exit(110);
		}
		/*
		 * A userns container has no overlay -- it uses its own
		 * per-container rootfs, which the parent open_tree'd and the child
		 * move_mounts + pivots into just below. So the classic child-side
		 * overlay_create runs for the non-userns case only.
		 */
		if (!spec->userns_enabled && spec->ov.direct_rootfs) {
			/*
			 * ADR-0207 phase 2: no overlay. merged IS this
			 * container's own rootfs (a btrfs snapshot of its
			 * image, or the test-mode copy); pivot_root demands a
			 * mount point and a plain directory is not one, so
			 * self-bind it first -- the identical lesson the userns
			 * path below learned in ADR-0179 phase 2b, applied to
			 * the non-userns direct path.
			 */
			if (mount(spec->ov.merged, spec->ov.merged, NULL, MS_BIND, NULL) != 0) {
				child_diag(diag_pipe[1], "child: bind direct rootfs");
				_exit(123);
			}
		}
		if (!spec->userns_enabled && !spec->ov.direct_rootfs) {
			int overlay_ret = overlay_create(&spec->ov);

			/*
			 * overlay_create() itself already distinguishes six
			 * named steps plus (for the actual mount(2) call) the
			 * real errno -- translated here into its own small,
			 * disjoint exit-code range (130-136 for the six named
			 * steps, 141-255 for a mount(2) errno in [1,115], the
			 * same shared range the final execve() below also uses)
			 * so a daemon-layer caller with log-store access (pkg.c)
			 * doesn't just learn "overlay_create failed" but exactly
			 * which of its own six steps, and for the mount itself,
			 * the kernel's own real reason. The "overlay mount" vs
			 * "exec" ambiguity that same shared 141-255 range would
			 * otherwise leave in the bare exit code (both reachable
			 * only in mutual exclusion, but genuinely indistinguishable
			 * from the number alone) is exactly what the diag pipe's
			 * own distinct message prefixes below resolve.
			 */
			if (overlay_ret != 0) {
				child_diag(diag_pipe[1], "child: overlay_create");
				if (overlay_ret <= OVERLAY_ERR_MOUNT_ERRNO_BASE)
					_exit(140 + (OVERLAY_ERR_MOUNT_ERRNO_BASE - overlay_ret));
				_exit(129 - overlay_ret);
			}
		}
		/*
		 * ADR-0179 phase 2c option (a): attach the parent's detached,
		 * id-mapped per-container rootfs into THIS child's own mount
		 * namespace with move_mount() and pivot into it. A mount the child
		 * attaches itself is not MNT_LOCKED (so pivot_root accepts it), and
		 * it's a plain ext4 mount carrying the parent's id-mapping -- the
		 * container's mapped root owns and can write its whole rootfs, with
		 * no overlay (and so none of overlay's userns-blocked setup ops).
		 * Non-userns mounted merged in overlay_create above and needs none
		 * of this.
		 */
		if (spec->userns_enabled) {
			if (cix_move_mount(overlay_lower_fd, "", -1, spec->ov.merged,
			                  MOVE_MOUNT_F_EMPTY_PATH) != 0) {
				child_diag(diag_pipe[1], "child: move_mount userns rootfs");
				_exit(122);
			}
			close(overlay_lower_fd);
		}
		/*
		 * Issue #88: bind persistent volumes into the container's
		 * future root BEFORE pivot_root, while `merged` is still
		 * addressable by its host path. Done here, in the child, so the
		 * mounts land in this container's OWN mount namespace and are
		 * torn down with it -- the host never accumulates them.
		 *
		 * A failure is deliberately fatal rather than skipped: a
		 * container that silently started WITHOUT its persistent
		 * storage would write to the overlay upper layer instead, look
		 * completely healthy, and lose that data on its next recreate --
		 * exactly the failure volumes exist to prevent, made harder to
		 * notice.
		 */
		{
			int vi;

			for (vi = 0; vi < spec->volume_count; vi++) {
				const struct container_volume *vol = &spec->volumes[vi];
				char target[PATH_MAX];

				if (snprintf(target, sizeof(target), "%s%s", spec->ov.merged,
				             vol->mount_path) >= (int)sizeof(target)) {
					errno = ENAMETOOLONG;
					child_diag(diag_pipe[1], "child: volume target path");
					_exit(125);
				}
				/* The mount point must exist inside the container. Creating
				 * it here (rather than requiring the image to ship it) is
				 * what lets a volume be mounted at a path the image has no
				 * concept of, e.g. /home on a minimal rootfs. */
				if (volume_mkdir_p(target) != 0) {
					child_diag(diag_pipe[1], "child: volume mkdir");
					_exit(125);
				}
				if (spec->userns_idmap && spec->volume_idmap_fds[i] >= 0) {
					/* The parent's id-mapped detached tree -- attaching
					 * it is what makes a host-0-owned volume writable
					 * by this container's mapped root (ADR-0207 ph3). */
					if (cix_move_mount(spec->volume_idmap_fds[i], "", -1, target,
					                  MOVE_MOUNT_F_EMPTY_PATH) != 0) {
						child_diag(diag_pipe[1], "child: volume move_mount idmap");
						_exit(125);
					}
					close(spec->volume_idmap_fds[i]);
				} else if (mount(vol->host_path, target, NULL, MS_BIND | MS_REC, NULL) != 0) {
					child_diag(diag_pipe[1], "child: volume bind mount");
					_exit(125);
				}
				if (vol->read_only &&
				    mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) != 0) {
					/* A read-only volume that silently came up writable
					 * would be a false guarantee, so this fails too. */
					child_diag(diag_pipe[1], "child: volume remount ro");
					_exit(125);
				}
			}
		}
		{
			/*
			 * #257: does this container run containers of its own?
			 *
			 * Both halves are required, and each answers a different
			 * question. CAP_SYS_ADMIN says the operator deliberately
			 * gave this container elevated privilege -- the pivot
			 * runs before capabilities are dropped, so without an
			 * explicit signal every container would silently receive
			 * a writable cgroup tree. CLONE_NEWCGROUP is what makes
			 * granting it safe: the mount then shows the container
			 * its own cgroup as the root, so it cannot reach anything
			 * above itself and the parent's limits keep applying.
			 *
			 * Derived here rather than set by each caller, so the
			 * daemon's container path and the package-build path
			 * cannot end up with different rules.
			 */
			struct mount_spec mnt = spec->mnt;
			int mountns_pivot_ret;

			mnt.mount_cgroup2 = ((spec->ns.clone_flags & CLONE_NEWCGROUP) != 0 &&
			                      spec_has_cap(spec, "CAP_SYS_ADMIN"));
			mountns_pivot_ret = mountns_pivot(spec->ov.merged, &mnt);

			if (mountns_pivot_ret != 0) {
				child_diag_mountns_pivot(diag_pipe[1], mountns_pivot_ret);
				_exit(112);
			}
		}
		if (container_dev_mknod(spec->devices, spec->device_count) != 0) {
			child_diag(diag_pipe[1], "child: container_dev_mknod");
			_exit(113);
		}

		if (spec->ns.hostname != NULL &&
		    sethostname(spec->ns.hostname, strlen(spec->ns.hostname)) != 0) {
			child_diag(diag_pipe[1], "child: sethostname");
			_exit(114);
		}

		/* Loopback first, and independent of want_net: a container
		 * with its own netns but no networks (every build sandbox)
		 * still needs 127.0.0.1 to work. See
		 * container_net_child_loopback_up() (#224). */
		if ((spec->ns.clone_flags & CLONE_NEWNET) != 0 &&
		    container_net_child_loopback_up() != 0) {
			child_diag(diag_pipe[1], "child: container_net_child_loopback_up");
			_exit(111);
		}

		if (want_net) {
			if (container_net_child_configure(spec->nets, spec->net_count, net_pipe[0]) != 0) {
				child_diag(diag_pipe[1], "child: container_net_child_configure");
				_exit(115);
			}
			close(net_pipe[0]);
		}

		if (container_net_install_routes(spec->routes, spec->route_count) != 0) {
			child_diag(diag_pipe[1], "child: container_net_install_routes");
			_exit(116);
		}
		if (spec->ip_forward && container_net_enable_ip_forward() != 0) {
			child_diag(diag_pipe[1], "child: container_net_enable_ip_forward");
			_exit(117);
		}
		for (i = 0; i < spec->sysctl_count; i++) {
			if (container_net_apply_sysctl(spec->sysctls[i].key, spec->sysctls[i].value) != 0) {
				child_diag(diag_pipe[1], "child: container_net_apply_sysctl");
				_exit(118);
			}
		}

		if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
			child_diag(diag_pipe[1], "child: prctl(PR_SET_PDEATHSIG)");
			_exit(119);
		}

		if (spec->capture_output) {
			dup2(spec->stdout_fd, STDOUT_FILENO);
			dup2(spec->stderr_fd, STDERR_FILENO);
			/* Close the original fds once dup2()'d onto 1/2 --
			 * both often alias the same pipe write end (see
			 * pkg.c), so a bare close() without the guard would
			 * double-close a already-closed fd if stderr_fd ==
			 * stdout_fd and both already collapsed onto the same
			 * number as one of the standard streams. */
			if (spec->stdout_fd != STDOUT_FILENO && spec->stdout_fd != STDERR_FILENO)
				close(spec->stdout_fd);
			if (spec->stderr_fd != STDOUT_FILENO && spec->stderr_fd != STDERR_FILENO &&
			    spec->stderr_fd != spec->stdout_fd)
				close(spec->stderr_fd);
		}

		/*
		 * ADR-0179 phase 2c: drop into the namespace's mapped root before
		 * exec. Every privileged setup step above ran as real host root
		 * (host uid 0 -- the child inherits cixd's uid, which is unmapped
		 * in this new userns), which is why it could mount/mknod/pivot. Now
		 * that setup is done, become userns uid/gid 0 -- i.e. host <base> --
		 * so the workload runs genuinely unprivileged on the host, the whole
		 * point of the user namespace. The first process in a new userns
		 * holds full caps there regardless of its unmapped host uid, and
		 * setuid TO uid 0 keeps them, so container_caps_drop below still
		 * governs the final set. Done before caps_drop so CAP_SETUID/SETGID
		 * are still present to perform it. (setgroups() is denied in this
		 * userns -- see write_userns_maps -- so supplementary groups are left
		 * as-is; harmless.)
		 */
		if (spec->userns_enabled && (setgid(0) != 0 || setuid(0) != 0)) {
			child_diag(diag_pipe[1], "child: userns setgid/setuid(0)");
			_exit(124);
		}

		/*
		 * #260: volunteer this container's anonymous memory for KSM.
		 *
		 * Deliberately last, immediately before capabilities are
		 * dropped and the payload is exec'd: the flag lives on the mm
		 * and is inherited by everything the payload forks, so setting
		 * it here covers memory that does not exist yet, which is the
		 * whole point -- the payload is an arbitrary program with no
		 * call sites this daemon can add a madvise() to.
		 *
		 * A failure is NOT fatal. Merging is an optimisation: a
		 * container that cannot opt in still runs correctly, just
		 * without the saving, and refusing to start it would trade a
		 * working container for a memory optimisation. The diagnostic
		 * says so rather than the container silently differing from
		 * what was asked for.
		 */
		if (spec->ksm && prctl(PR_SET_MEMORY_MERGE, 1, 0, 0, 0) != 0)
			child_diag(diag_pipe[1], "child: prctl(PR_SET_MEMORY_MERGE) -- running without KSM");

		if (container_caps_drop(spec->cap_add, spec->cap_add_count) != 0) {
			child_diag(diag_pipe[1], "child: container_caps_drop");
			_exit(120);
		}

		execve(spec->argv[0], spec->argv, spec->envp);
		{
			/*
			 * The exact same encoding overlay_create()'s own mount(2)
			 * errno already uses (140 + errno, 141-255) -- deliberately
			 * the SAME shared range, not a second disjoint one: the
			 * two are mutually exclusive within a single run (this
			 * execve() is only ever reached once overlay_create() has
			 * already succeeded), so reusing it is unambiguous, and it
			 * gives this, the last and most likely to actually matter
			 * spot where a bare "_exit(127)" hid real information
			 * (ENOENT the interpreter/binary genuinely missing, EACCES
			 * not executable -- lost +x bit or a noexec mount, ENOEXEC
			 * bad ELF format, ELIBBAD 80 "corrupted shared library" --
			 * a real, confirmed-live case that needed exactly this
			 * wide a range), the full width of the byte rather than a
			 * cramped half of it. 127 itself stays the fallback for an
			 * errno too large even for this, preserving its own
			 * existing meaning as "some exec-class failure."
			 */
			int exec_errno = errno;
			char prefix[PATH_MAX + 32];

			snprintf(prefix, sizeof(prefix), "child: execve(%s)", spec->argv[0]);
			errno = exec_errno;
			child_diag(diag_pipe[1], prefix);
			if (exec_errno > 0 && exec_errno <= OVERLAY_ERR_MOUNT_ERRNO_MAX)
				_exit(140 + exec_errno);
			_exit(127);
		}
	}

	/* Parent. */
	close(diag_pipe[1]);
	/*
	 * Phase 2c: the child inherited its own copy of the detached rootfs fd
	 * at clone3; drop the parent's copy. The detached mount stays alive on
	 * the child's copy until it move_mounts it into its own namespace.
	 */
	if (spec->userns_enabled && spec->userns_idmap) {
		int vi;

		for (vi = 0; vi < spec->volume_count; vi++)
			if (spec->volume_idmap_fds[vi] >= 0)
				close(spec->volume_idmap_fds[vi]);
	}
	if (spec->userns_enabled && overlay_lower_fd >= 0)
		close(overlay_lower_fd);

	/*
	 * ADR-0179 phase 2: write the child's uid/gid maps, then release it.
	 * This must happen before the child can reach any privileged step, so
	 * it comes first in the parent -- ahead of the veth handshake below,
	 * which the child only reaches long after the userns barrier. On any
	 * failure, closing the write end EOFs the child's blocked read (clean
	 * _exit(121)); we then also SIGKILL+reap to abort creation, matching
	 * the interface-attach failure path's own posture.
	 */
	if (spec->userns_enabled) {
		int userns_ok, saved_errno;

		close(userns_pipe[0]);
		userns_ok = (write_userns_maps((pid_t)ret, spec->userns_uid_base,
		                               spec->userns_len) == 0);
		/*
		 * ADR-0207 phase 3: with the maps written, the child's user
		 * namespace exists and can be named -- id-map the detached
		 * rootfs (and volumes) NOW, before the release byte, so by the
		 * time the child move_mounts them every inode already presents
		 * as its mapped ids. This replaces phase 2b's per-inode chown
		 * for snapshot-provisioned containers: the disk stays host-0,
		 * extent sharing intact, and the kernel does the presenting.
		 */
		if (userns_ok && spec->userns_idmap) {
			char uns_path[64];
			int uns_fd;

			snprintf(uns_path, sizeof(uns_path), "/proc/%d/ns/user", (int)ret);
			uns_fd = open(uns_path, O_RDONLY | O_CLOEXEC);
			if (uns_fd < 0) {
				userns_ok = 0;
			} else {
				struct cix_mount_attr mattr;
				int vi;

				memset(&mattr, 0, sizeof(mattr));
				mattr.attr_set = MOUNT_ATTR_IDMAP;
				/*
				 * Clear nodev on the container's own rootfs.
				 *
				 * boot_init() mounts the containers partition
				 * MS_NOSUID|MS_NODEV -- correct hardening for the host
				 * -- and this detached rootfs is a bind from it, so it
				 * inherits nodev. The result was that a userns
				 * container could not open ANY device node: not
				 * /dev/null, not /dev/zero, read or write, even though
				 * every node was present with mode 0666 and mapped to
				 * container-root (issue #173). Non-userns containers
				 * never hit it because OverlayFS is a fresh mount that
				 * inherits nothing.
				 *
				 * A container's rootfs device nodes are deliberate
				 * content, not something a workload smuggled in:
				 * pkg_seed_image_baseline() stages /dev/null,
				 * /dev/zero, /dev/full and /dev/ptmx into every image.
				 * What a container may actually DO with a device is
				 * enforced by the BPF_CGROUP_DEVICE program attached
				 * unconditionally in container_create() (ADR-0017) --
				 * a default-deny allow-list, which is the real control
				 * and is unaffected by this. nodev here was redundant
				 * against that and broke /dev/null, which shell
				 * redirection and almost every configure script needs.
				 */
				mattr.userns_fd = (uint64_t)uns_fd;
				if (cix_mount_setattr(overlay_lower_fd, "",
				                      CIX_AT_EMPTY_PATH, &mattr) != 0)
					userns_ok = 0;
				for (vi = 0; userns_ok && vi < spec->volume_count; vi++) {
					if (spec->volume_idmap_fds[vi] >= 0 &&
					    cix_mount_setattr(spec->volume_idmap_fds[vi], "",
					                      CIX_AT_EMPTY_PATH | AT_RECURSIVE,
					                      &mattr) != 0)
						userns_ok = 0;
				}
				close(uns_fd);
			}
		}
		userns_ok = userns_ok && (write(userns_pipe[1], "x", 1) == 1);
		saved_errno = errno;
		close(userns_pipe[1]);
		if (!userns_ok) {
			siginfo_t info;

			errno = saved_errno;
			perror("container_create: userns map/release");
			container_set_last_error_step("container_create: userns map/release");
			if (want_net) {
				close(net_pipe[0]);
				close(net_pipe[1]);
			}
			sys_pidfd_send_signal(pidfd, SIGKILL);
			waitid(P_PIDFD, pidfd, &info, WEXITED);
			close(diag_pipe[0]);
			close(pidfd);
			if (bpf_prog_fd >= 0)
				close(bpf_prog_fd);
			close(cgroup_fd);
			errno = saved_errno;
			return -1;
		}
	}

	if (want_net) {
		close(net_pipe[0]);
		if (container_net_host_setup(spec->nets, spec->net_count, (pid_t)ret, net_pipe[1]) != 0) {
			/*
			 * The child is already blocked reading net_pipe[0]
			 * waiting for the veth name; closing our write end
			 * without writing makes its read() see EOF and fail
			 * cleanly (_exit(115)) instead of hanging forever.
			 */
			int saved_errno = errno;
			siginfo_t info;

			perror("container_create: container_net_host_setup");
			container_set_last_error_step("container_create: container_net_host_setup");
			close(net_pipe[1]);
			waitid(P_PIDFD, pidfd, &info, WEXITED);
			close(diag_pipe[0]);
			close(pidfd);
			if (bpf_prog_fd >= 0)
				close(bpf_prog_fd);
			close(cgroup_fd);
			errno = saved_errno;
			return -1;
		}
		close(net_pipe[1]);
	}

	out->interfaces_netns_fd = -1;
	if (spec->interface_count > 0) {
		const char *iface_ptrs[CONTAINER_MAX_INTERFACES];
		int i;

		for (i = 0; i < spec->interface_count; i++)
			iface_ptrs[i] = spec->interfaces[i];

		if (container_net_host_attach_interfaces(iface_ptrs, spec->interface_count, (pid_t)ret,
		                                          &out->interfaces_netns_fd) != 0) {
			/*
			 * Unlike the veth path above, the child was never blocked
			 * waiting on anything interface-related -- it may already
			 * be running. There is no clean "let it fail on its own"
			 * signal to send, so an explicit kill is the only correct
			 * way to abort creation here.
			 */
			int saved_errno = errno;
			siginfo_t info;

			perror("container_create: container_net_host_attach_interfaces");
			container_set_last_error_step("container_create: container_net_host_attach_interfaces");
			sys_pidfd_send_signal(pidfd, SIGKILL);
			waitid(P_PIDFD, pidfd, &info, WEXITED);
			close(diag_pipe[0]);
			close(pidfd);
			if (bpf_prog_fd >= 0)
				close(bpf_prog_fd);
			close(cgroup_fd);
			errno = saved_errno;
			return -1;
		}
	}

	out->pid = (pid_t)ret;
	out->cgroup_fd = cgroup_fd;
	out->pidfd = pidfd;
	out->bpf_prog_fd = bpf_prog_fd;
	out->diag_fd = diag_pipe[0];
	return 0;
}

int container_wait(const struct container_handle *h, int *exit_status, int *term_signal)
{
	siginfo_t info;

	if (waitid(P_PIDFD, h->pidfd, &info, WEXITED) != 0)
		return -1;

	*exit_status = info.si_status;
	if (term_signal != NULL) {
		/*
		 * si_status is dual-purpose: an exit code when the child
		 * exited normally (CLD_EXITED), the signal number when it was
		 * killed (CLD_KILLED / CLD_DUMPED). Surface which one it is so
		 * a caller never has to guess whether "9" means exit code 9 or
		 * SIGKILL (issue #78). exit_status still carries the raw
		 * si_status either way, so the "== 0 means clean" test every
		 * restart/pkg path relies on is unchanged.
		 */
		*term_signal = (info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED)
		                   ? info.si_status
		                   : 0;
	}
	return 0;
}

ssize_t container_read_diag(struct container_handle *h, char *buf, size_t bufsize)
{
	ssize_t total = 0, n;

	buf[0] = '\0';
	if (h->diag_fd < 0)
		return 0;

	while (total < (ssize_t)bufsize - 1) {
		n = read(h->diag_fd, buf + total, bufsize - 1 - (size_t)total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(h->diag_fd);
			h->diag_fd = -1;
			return -1;
		}
		if (n == 0)
			break;
		total += n;
	}
	buf[total] = '\0';
	/* Trim the single trailing '\n' child_diag() always writes -- the
	 * caller's own format string (logstore_write()/a JSON field) adds
	 * its own line ending or delimiter, doesn't need ours too. */
	if (total > 0 && buf[total - 1] == '\n')
		buf[total - 1] = '\0';
	close(h->diag_fd);
	h->diag_fd = -1;
	return total;
}

/*
 * Short uppercase name for the handful of signals a container actually
 * dies from in practice -- SIGKILL (stop/delete), SIGTERM, and the
 * common crash signals. Not exhaustive: an unlisted signal just prints
 * as its bare number, which is still unambiguous next to term_signal.
 */
const char *container_signal_name(int sig)
{
	switch (sig) {
	case 2:  return "SIGINT";
	case 6:  return "SIGABRT";
	case 9:  return "SIGKILL";
	case 11: return "SIGSEGV";
	case 15: return "SIGTERM";
	default: return NULL;
	}
}

void container_decode_exit_status(int exit_status, int term_signal, char *buf, size_t bufsize)
{
	static const struct {
		int code;
		const char *desc;
	} named[] = {
		{ 110, "mountns_make_private failed" },
		{ 112, "mountns_pivot failed" },
		{ 113, "container_dev_mknod failed" },
		{ 114, "sethostname failed" },
		{ 111, "container_net_child_loopback_up failed" },
		{ 115, "container_net_child_configure failed" },
		/* Was already in use at the direct-rootfs bind and simply had
		 * no name here, so it decoded as a bare number. Noticed while
		 * nearly reusing it for the loopback failure above, which
		 * would have made this table state the wrong cause. */
		{ 123, "bind direct rootfs failed" },
		{ 116, "container_net_install_routes failed" },
		{ 117, "container_net_enable_ip_forward failed" },
		{ 118, "container_net_apply_sysctl failed" },
		{ 119, "prctl(PR_SET_PDEATHSIG) failed" },
		{ 120, "container_caps_drop failed" },
		{ 121, "userns map sync failed (parent never released the child)" },
		{ 122, "userns per-container rootfs move_mount failed (pre-pivot_root)" },
		{ 124, "userns setgid/setuid(0) to the mapped root failed (pre-exec)" },
		/* Three volume-mount failures share this code; the child_diag
		 * line says which. Named because an unnamed code decodes as a
		 * bare number, which is the diagnostic saying nothing. */
		{ 125, "volume mount failed (target path, mkdir, or move_mount)" },
		{ 127, "exec failed (errno out of encodable range)" },
		{ 130, "overlay: lowerdir stat failed" },
		{ 131, "overlay: upperdir mkdir failed" },
		{ 132, "overlay: disk quota setup failed" },
		{ 133, "overlay: workdir mkdir failed" },
		{ 134, "overlay: merged mountpoint mkdir failed" },
		{ 135, "overlay: mount options too long" },
		{ 136, "overlay: mount(2) itself failed" },
	};
	size_t i;

	if (term_signal != 0) {
		const char *name = container_signal_name(term_signal);

		if (name != NULL)
			snprintf(buf, bufsize, "killed by signal %d (%s)", term_signal, name);
		else
			snprintf(buf, bufsize, "killed by signal %d", term_signal);
		return;
	}
	if (exit_status == 0) {
		snprintf(buf, bufsize, "clean exit");
		return;
	}
	for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
		if (named[i].code == exit_status) {
			snprintf(buf, bufsize, "%s", named[i].desc);
			return;
		}
	}
	if (exit_status >= 141 && exit_status <= 140 + OVERLAY_ERR_MOUNT_ERRNO_MAX) {
		/*
		 * Genuinely ambiguous from the number alone (overlay mount(2)
		 * vs. the final execve(), see container_create()'s own
		 * comment on why they share this range) -- the real
		 * disambiguation lives in container_read_diag()'s own text
		 * ("child: overlay_create: ..." vs "child: execve(...): ..."),
		 * not here.
		 */
		snprintf(buf, bufsize, "overlay mount or exec failed: %s",
		         strerror(exit_status - 140));
		return;
	}
	snprintf(buf, bufsize, "exited with status %d", exit_status);
}
