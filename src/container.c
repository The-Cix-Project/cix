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
 * included) would run with the wrong credentials. Ordering is the
 * documented kernel requirement: "deny" to setgroups (a task may only
 * gain, never re-drop, group membership across a map) BEFORE gid_map,
 * then uid_map. Each map is the single line "0 <base> <len>": the
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

static int write_userns_maps(pid_t pid, long long base, long long len)
{
	char map[64];

	if (write_proc_line(pid, "setgroups", "deny") != 0)
		return -1;
	snprintf(map, sizeof(map), "0 %lld %lld", base, len);
	if (write_proc_line(pid, "gid_map", map) != 0)
		return -1;
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

		overlay_lower_fd = (int)thinc_open_tree(-1, spec->ov.userns_rootfs, OPEN_TREE_CLONE);
		if (overlay_lower_fd < 0) {
			saved_errno = errno;
			fail_step = "container_create: open_tree(userns_rootfs)";
			prep_ret = -1;
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
		if (!spec->userns_enabled) {
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
			if (thinc_move_mount(overlay_lower_fd, "", -1, spec->ov.merged,
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
				if (mount(vol->host_path, target, NULL, MS_BIND | MS_REC, NULL) != 0) {
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
			int mountns_pivot_ret = mountns_pivot(spec->ov.merged, &spec->mnt);

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
		 * (host uid 0 -- the child inherits thincd's uid, which is unmapped
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
		                               spec->userns_len) == 0) &&
		            (write(userns_pipe[1], "x", 1) == 1);
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
		{ 115, "container_net_child_configure failed" },
		{ 116, "container_net_install_routes failed" },
		{ 117, "container_net_enable_ip_forward failed" },
		{ 118, "container_net_apply_sysctl failed" },
		{ 119, "prctl(PR_SET_PDEATHSIG) failed" },
		{ 120, "container_caps_drop failed" },
		{ 121, "userns map sync failed (parent never released the child)" },
		{ 122, "userns per-container rootfs move_mount failed (pre-pivot_root)" },
		{ 124, "userns setgid/setuid(0) to the mapped root failed (pre-exec)" },
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
