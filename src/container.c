#include "container.h"
#include "internal.h"
#include "linux_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
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
static void child_diag(int fd, const char *prefix)
{
	dprintf(fd, "%s: %s\n", prefix, strerror(errno));
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

int container_create(const struct container_spec *spec, struct container_handle *out)
{
	int cgroup_fd;
	int pidfd = -1;
	int bpf_prog_fd = -1;
	long ret;
	int net_pipe[2] = { -1, -1 };
	int diag_pipe[2] = { -1, -1 };
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
		{
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
		if (mountns_pivot(spec->ov.merged, &spec->mnt) != 0) {
			child_diag(diag_pipe[1], "child: mountns_pivot");
			_exit(112);
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

int container_wait(const struct container_handle *h, int *exit_status)
{
	siginfo_t info;

	if (waitid(P_PIDFD, h->pidfd, &info, WEXITED) != 0)
		return -1;

	*exit_status = info.si_status;
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

void container_decode_exit_status(int exit_status, char *buf, size_t bufsize)
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
