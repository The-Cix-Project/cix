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

	if (cgroup_create(&spec->cg, &cgroup_fd) != 0)
		return -1;

	ret = ns_clone3(spec->ns.clone_flags, cgroup_fd, &pidfd);
	if (ret < 0) {
		int saved_errno = errno;
		close(cgroup_fd);
		errno = saved_errno;
		return -1;
	}

	if (ret == 0) {
		/* Child: from here on we live inside the new namespaces. */
		if (mountns_pivot(&spec->mnt) != 0) {
			perror("child: mountns_pivot");
			_exit(126);
		}

		if (spec->ns.hostname != NULL &&
		    sethostname(spec->ns.hostname, strlen(spec->ns.hostname)) != 0) {
			perror("child: sethostname");
			_exit(126);
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
