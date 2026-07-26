#include "internal.h"
#include "linux_compat.h"

#include <signal.h>
#include <string.h>

long ns_clone3(unsigned long flags, int cgroup_fd, int *pidfd_out)
{
	struct clone_args args;
	int pidfd = -1;
	long ret;

	memset(&args, 0, sizeof(args));
	args.flags = (uint64_t)flags | CLONE_PIDFD;
	args.pidfd = (uint64_t)(uintptr_t)&pidfd;
	args.exit_signal = SIGCHLD;
	args.cgroup = (uint64_t)cgroup_fd;

	ret = sys_clone3(&args, sizeof(args));
	if (ret > 0 && pidfd_out != NULL)
		*pidfd_out = pidfd;

	return ret;
}
