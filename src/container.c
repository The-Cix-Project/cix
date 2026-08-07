#include "container.h"
#include "internal.h"
#include "linux_compat.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int container_create(const struct container_spec *spec, struct container_handle *out)
{
	int cgroup_fd;
	int pidfd = -1;
	int bpf_prog_fd = -1;
	long ret;
	int net_pipe[2] = { -1, -1 };
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
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	if (want_net && pipe(net_pipe) != 0) {
		int saved_errno = errno;
		if (bpf_prog_fd >= 0)
			close(bpf_prog_fd);
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	ret = ns_clone3(spec->ns.clone_flags, cgroup_fd, &pidfd);
	if (ret < 0) {
		int saved_errno = errno;
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
		if (want_net)
			close(net_pipe[1]);

		/*
		 * Each pre-exec setup step gets its own exit code (110-119)
		 * rather than a single uniform 126 -- the parent (and anything
		 * further up the chain that only ever sees a plain wait()
		 * exit status, e.g. pkg_build_completed()'s "build failed
		 * (exit status N)") had no way to tell these ten completely
		 * different failure modes apart otherwise. perror() here still
		 * goes to the daemon's own stderr as before (this is
		 * runtime-library code, shared with kanxeoctl console and
		 * others -- it must not take a daemon-layer logstore.h
		 * dependency the way daemon/src/pkg.c's own subprocess
		 * diagnostics do), but a distinct code lets a caller that DOES
		 * have log-store access (pkg.c) report specifically which
		 * step failed using information it already receives for free.
		 * 127 is deliberately left alone: it's execve()'s own existing
		 * code below, already a recognizable, conventional "exec
		 * failed" signal independent of this scheme.
		 */
		if (mountns_make_private() != 0) {
			perror("child: mountns_make_private");
			_exit(110);
		}
		{
			int overlay_ret = overlay_create(&spec->ov);

			/*
			 * overlay_create() itself already distinguishes six
			 * named steps plus (for the actual mount(2) call) the
			 * real errno -- translated here into its own small,
			 * disjoint exit-code range (130-136 for the six named
			 * steps, 141-200 for a mount(2) errno in [1,60]) so a
			 * daemon-layer caller with log-store access (pkg.c)
			 * doesn't just learn "overlay_create failed" but
			 * exactly which of its own six steps, and for the
			 * mount itself, the kernel's own real reason.
			 */
			if (overlay_ret != 0) {
				perror("child: overlay_create");
				if (overlay_ret <= OVERLAY_ERR_MOUNT_ERRNO_BASE)
					_exit(140 + (OVERLAY_ERR_MOUNT_ERRNO_BASE - overlay_ret));
				_exit(129 - overlay_ret);
			}
		}
		if (mountns_pivot(spec->ov.merged, &spec->mnt) != 0) {
			perror("child: mountns_pivot");
			_exit(112);
		}
		if (container_dev_mknod(spec->devices, spec->device_count) != 0) {
			perror("child: container_dev_mknod");
			_exit(113);
		}

		if (spec->ns.hostname != NULL &&
		    sethostname(spec->ns.hostname, strlen(spec->ns.hostname)) != 0) {
			perror("child: sethostname");
			_exit(114);
		}

		if (want_net) {
			if (container_net_child_configure(spec->nets, spec->net_count, net_pipe[0]) != 0) {
				perror("child: container_net_child_configure");
				_exit(115);
			}
			close(net_pipe[0]);
		}

		if (container_net_install_routes(spec->routes, spec->route_count) != 0) {
			perror("child: container_net_install_routes");
			_exit(116);
		}
		if (spec->ip_forward && container_net_enable_ip_forward() != 0) {
			perror("child: container_net_enable_ip_forward");
			_exit(117);
		}
		for (i = 0; i < spec->sysctl_count; i++) {
			if (container_net_apply_sysctl(spec->sysctls[i].key, spec->sysctls[i].value) != 0) {
				perror("child: container_net_apply_sysctl");
				_exit(118);
			}
		}

		if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
			perror("child: prctl(PR_SET_PDEATHSIG)");
			_exit(119);
		}

		execve(spec->argv[0], spec->argv, spec->envp);
		{
			/*
			 * Same technique as overlay_create()'s own mount(2)
			 * errno encoding (src/overlay.c) -- this final execve()
			 * is the last, and most likely to actually matter, spot
			 * where a bare "_exit(127)" hid real information: ENOENT
			 * (the interpreter or binary genuinely missing),
			 * EACCES (not executable -- lost +x bit, or a noexec
			 * mount), ENOEXEC (bad ELF format), and ELIBBAD (80,
			 * "corrupted shared library" -- a real, confirmed-live
			 * case a narrower range would have missed) all look
			 * identical from a caller only checking WEXITSTATUS().
			 * 170 + errno (errno in [1,84]) leaves 127 itself free
			 * as the fallback for an errno too large to encode this
			 * way, preserving its own existing meaning as "some
			 * exec-class failure" for that rare case. Deliberately
			 * starts right after overlay_create()'s own (now
			 * narrower, 141-170) mount-errno range -- the two never
			 * both apply to the same run (execve() is never reached
			 * unless overlay_create() already succeeded), so this
			 * exit-status byte's remaining space is reused, not
			 * shared ambiguously.
			 */
			int exec_errno = errno;

			perror("child: execve");
			if (exec_errno > 0 && exec_errno <= 84)
				_exit(170 + exec_errno);
			_exit(127);
		}
	}

	/* Parent. */
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

			close(net_pipe[1]);
			waitid(P_PIDFD, pidfd, &info, WEXITED);
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

			sys_pidfd_send_signal(pidfd, SIGKILL);
			waitid(P_PIDFD, pidfd, &info, WEXITED);
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
