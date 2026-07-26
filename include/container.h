#ifndef CONTAINER_H
#define CONTAINER_H

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
	const char *root_source;
	const char *put_old_rel;
};

struct container_spec {
	struct ns_config ns;
	struct cgroup_limits cg;
	struct mount_spec mnt;
	char *const *argv;
	char *const *envp;
};

struct container_handle {
	pid_t pid;
	int cgroup_fd;
	int pidfd;
};

/*
 * Creates the cgroup v2 leaf described by lim, applying memory.max,
 * pids.max and cpu.max. On success *out_fd is an O_PATH descriptor
 * on the leaf directory, suitable for clone_args.cgroup.
 */
int cgroup_create(const struct cgroup_limits *lim, int *out_fd);

/*
 * Creates and starts a container per spec: clone3 into new
 * PID/MNT/UTS/NET/CGROUP namespaces, places the child atomically
 * into the cgroup opened by cgroup_create, pivot_roots into
 * mnt.root_source, and execve's argv[0] with argv/envp. On success
 * fills out with the child's pid, cgroup fd and pidfd.
 */
int container_create(const struct container_spec *spec, struct container_handle *out);

/*
 * Race-free wait via the handle's pidfd. On return *exit_status
 * holds the child's exit status as reported by waitid().
 */
int container_wait(const struct container_handle *h, int *exit_status);

#endif /* CONTAINER_H */
