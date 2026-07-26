/*
 * Phase 1 demonstrable test: proves clone3-based PID/MNT/UTS/NET/CGROUP
 * isolation, atomic cgroup placement, and pivot_root all work together
 * end to end. See /home/osakka/.claude/plans/compressed-riding-harbor.md
 * for the 7-point verification this implements.
 */
#include "container.h"
#include "linux_compat.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Phase 2 mounts an overlay onto spec.ov.merged before pivoting into
 * it, so writes the child makes copy up into upperdir rather than
 * mutating the live host directly -- the result file is read back
 * from there, not from the bare host path.
 */
#define UPPERDIR "/tmp/harness_overlay/upper"
#define RESULT_PATH UPPERDIR "/tmp/harness_result.txt"

static int read_cgroup_procs_count(const char *cgroup_name, pid_t expect_pid)
{
	char path[256];
	FILE *f;
	pid_t pid;
	int count = 0;
	int matched = 0;

	snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cgroup.procs", cgroup_name);
	f = fopen(path, "r");
	if (f == NULL) {
		perror("fopen cgroup.procs");
		return -1;
	}
	while (fscanf(f, "%d", &pid) == 1) {
		count++;
		if (pid == expect_pid)
			matched = 1;
	}
	fclose(f);

	if (count != 1 || !matched) {
		fprintf(stderr, "FAIL: cgroup.procs has %d entries (expected 1), matched=%d\n",
		        count, matched);
		return -1;
	}
	return 0;
}

static int check_result_file(void)
{
	FILE *f;
	char line[256];
	int saw_pid1 = 0;
	int saw_hostname = 0;
	int netif_count = 0;
	int saw_lo_only = 1;

	f = fopen(RESULT_PATH, "r");
	if (f == NULL) {
		perror("fopen " RESULT_PATH);
		return -1;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\n")] = '\0';

		if (strcmp(line, "PID=1") == 0)
			saw_pid1 = 1;
		else if (strcmp(line, "HOSTNAME=container-test") == 0)
			saw_hostname = 1;
		else if (strncmp(line, "NETIF=", 6) == 0) {
			/*
			 * bonding_masters is a per-netns control pseudo-file the
			 * kernel's bonding module auto-creates in every network
			 * namespace (present whenever the host has the module
			 * loaded, regardless of isolation) -- not a real
			 * interface, so it doesn't count as a netif leak.
			 */
			if (strcmp(line + 6, "bonding_masters") == 0)
				continue;
			netif_count++;
			if (strcmp(line + 6, "lo") != 0)
				saw_lo_only = 0;
		}
	}
	fclose(f);

	if (!saw_pid1)
		fprintf(stderr, "FAIL: child was not PID 1 in its namespace\n");
	if (!saw_hostname)
		fprintf(stderr, "FAIL: child did not observe hostname 'container-test'\n");
	if (netif_count != 1 || !saw_lo_only)
		fprintf(stderr, "FAIL: expected exactly one netif ('lo'), got %d (lo-only=%d)\n",
		        netif_count, saw_lo_only);

	return (saw_pid1 && saw_hostname && netif_count == 1 && saw_lo_only) ? 0 : -1;
}

int main(void)
{
	char *child_argv[] = { "/home/osakka/new_project/build/harness_child", NULL };
	char *child_envp[] = { NULL };
	struct container_spec spec;
	struct container_handle handle;
	char host_hostname_before[256];
	char host_hostname_after[256];
	int exit_status;
	int ok = 1;

	if (gethostname(host_hostname_before, sizeof(host_hostname_before)) != 0) {
		perror("gethostname (host, before)");
		return 1;
	}

	if (mkdir("/tmp/harness_overlay", 0755) != 0 && errno != EEXIST) {
		perror("mkdir /tmp/harness_overlay");
		return 1;
	}

	memset(&spec, 0, sizeof(spec));
	spec.ns.clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
	                       CLONE_NEWNET | CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec.ns.hostname = "container-test";
	spec.cg.name = "tcc-harness-test";
	spec.cg.memory_max = 67108864;
	spec.cg.pids_max = 32;
	spec.cg.cpu_max = NULL;
	spec.ov.lowerdir = "/";
	spec.ov.upperdir = UPPERDIR;
	spec.ov.workdir = "/tmp/harness_overlay/work";
	spec.ov.merged = "/tmp/harness_root";
	spec.mnt.put_old_rel = ".old_root";
	spec.argv = child_argv;
	spec.envp = child_envp;

	if (container_create(&spec, &handle) != 0) {
		perror("container_create");
		return 1;
	}

	printf("PARENT: child pid=%d pidfd=%d cgroup_fd=%d\n",
	       (int)handle.pid, handle.pidfd, handle.cgroup_fd);

	/* Read while the child is (almost certainly) still alive: proves
	 * atomic CLONE_INTO_CGROUP placement, no post-hoc cgroup.procs write. */
	if (read_cgroup_procs_count(spec.cg.name, handle.pid) != 0)
		ok = 0;

	if (container_wait(&handle, &exit_status) != 0) {
		perror("container_wait");
		return 1;
	}
	close(handle.pidfd);
	close(handle.cgroup_fd);

	printf("PARENT: child exit status=%d\n", exit_status);
	if (exit_status != 7) {
		fprintf(stderr, "FAIL: expected exit status 7, got %d\n", exit_status);
		ok = 0;
	}

	if (check_result_file() != 0)
		ok = 0;

	if (gethostname(host_hostname_after, sizeof(host_hostname_after)) != 0) {
		perror("gethostname (host, after)");
		ok = 0;
	} else if (strcmp(host_hostname_before, host_hostname_after) != 0 ||
	           strcmp(host_hostname_before, "container-test") == 0) {
		fprintf(stderr, "FAIL: host hostname changed ('%s' -> '%s')\n",
		        host_hostname_before, host_hostname_after);
		ok = 0;
	}

	printf(ok ? "HARNESS RESULT: PASS\n" : "HARNESS RESULT: FAIL\n");
	return ok ? 0 : 1;
}
