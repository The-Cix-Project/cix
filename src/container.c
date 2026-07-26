#include "container.h"
#include "internal.h"

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
	long ret;
	int net_pipe[2] = { -1, -1 };
	int want_net = (spec->net.bridge != NULL);

	if (cgroup_create(&spec->cg, &cgroup_fd) != 0)
		return -1;

	if (want_net && pipe(net_pipe) != 0) {
		int saved_errno = errno;
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
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	if (ret == 0) {
		/* Child: from here on we live inside the new namespaces. */
		if (want_net)
			close(net_pipe[1]);

		if (mountns_make_private() != 0) {
			perror("child: mountns_make_private");
			_exit(126);
		}
		if (overlay_create(&spec->ov) != 0) {
			perror("child: overlay_create");
			_exit(126);
		}
		if (mountns_pivot(spec->ov.merged, &spec->mnt) != 0) {
			perror("child: mountns_pivot");
			_exit(126);
		}

		if (spec->ns.hostname != NULL &&
		    sethostname(spec->ns.hostname, strlen(spec->ns.hostname)) != 0) {
			perror("child: sethostname");
			_exit(126);
		}

		if (want_net) {
			if (container_net_child_configure(&spec->net, net_pipe[0]) != 0) {
				perror("child: container_net_child_configure");
				_exit(126);
			}
			close(net_pipe[0]);
		}

		if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
			perror("child: prctl(PR_SET_PDEATHSIG)");
			_exit(126);
		}

		execve(spec->argv[0], spec->argv, spec->envp);
		perror("child: execve");
		_exit(127);
	}

	/* Parent. */
	if (want_net) {
		close(net_pipe[0]);
		if (container_net_host_setup(&spec->net, (pid_t)ret, net_pipe[1]) != 0) {
			/*
			 * The child is already blocked reading net_pipe[0]
			 * waiting for the veth name; closing our write end
			 * without writing makes its read() see EOF and fail
			 * cleanly (_exit(126)) instead of hanging forever.
			 */
			int saved_errno = errno;
			siginfo_t info;

			close(net_pipe[1]);
			waitid(P_PIDFD, pidfd, &info, WEXITED);
			close(pidfd);
			close(cgroup_fd);
			errno = saved_errno;
			return -1;
		}
		close(net_pipe[1]);
	}

	out->pid = (pid_t)ret;
	out->cgroup_fd = cgroup_fd;
	out->pidfd = pidfd;
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
