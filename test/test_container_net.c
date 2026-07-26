/*
 * Phase 6 part 2 end-to-end test: proves real networking works
 * through the actual container lifecycle (container_create(), not
 * raw rtnetlink primitives -- those were already proven in isolation
 * by test_rtnetlink.c). Creates two containers concurrently on the
 * same bridge and proves real, simultaneous connectivity to each,
 * then confirms the kernel tears down both veth ends automatically
 * once each container exits -- no explicit delete call needed for
 * that part, only for the bridge itself.
 */
#include "container.h"
#include "linux_compat.h"
#include "rtnetlink.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define BRIDGE_NAME "kanxeo-ctnet0"
#define IMAGE_ROOT "/tmp/container_net_test/lower"
#define NET_CHILD_PORT 17700

static int mkdir_p1(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

static int iface_exists(const char *name)
{
	char path[128];
	struct stat st;

	snprintf(path, sizeof(path), "/sys/class/net/%s", name);
	return stat(path, &st) == 0;
}

/*
 * Network namespace (and the veth pairs living only in one) teardown
 * runs on a kernel workqueue, not synchronously with the last task in
 * that namespace being reaped -- so the interface can briefly still
 * exist right after waitid() returns. Poll instead of asserting
 * immediately, the same pattern used everywhere else in this project
 * for "wait for something genuinely asynchronous."
 */
static int iface_gone_eventually(const char *name, int max_attempts)
{
	int i;

	for (i = 0; i < max_attempts; i++) {
		if (!iface_exists(name))
			return 1;
		usleep(100000);
	}
	return 0;
}

static int connect_and_echo(uint32_t ip_be, int *ok)
{
	int fd;
	struct sockaddr_in addr;
	char sbyte = 7, rbyte = 0;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		*ok = 0;
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(NET_CHILD_PORT);
	addr.sin_addr.s_addr = ip_be;

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("connect");
		*ok = 0;
		close(fd);
		return -1;
	}
	if (write(fd, &sbyte, 1) != 1 || read(fd, &rbyte, 1) != 1 || rbyte != sbyte) {
		fprintf(stderr, "FAIL: connectivity round-trip mismatch\n");
		*ok = 0;
	}
	close(fd);
	return 0;
}

static int build_container_spec(struct container_spec *spec, const char *cg_name,
                                 const char *scratch_dir, const char *container_ip,
                                 char **argv, char **envp)
{
	static char lowerdir[256];
	static char upperdir[256];
	static char workdir[256];
	static char merged[256];
	struct in_addr a;

	snprintf(lowerdir, sizeof(lowerdir), "%s", IMAGE_ROOT);
	snprintf(upperdir, sizeof(upperdir), "%s/upper", scratch_dir);
	snprintf(workdir, sizeof(workdir), "%s/work", scratch_dir);
	snprintf(merged, sizeof(merged), "%s/merged", scratch_dir);

	if (mkdir_p1(scratch_dir) != 0 || mkdir_p1(upperdir) != 0 || mkdir_p1(workdir) != 0 ||
	    mkdir_p1(merged) != 0)
		return -1;

	memset(spec, 0, sizeof(*spec));
	spec->ns.clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET |
	                        CLONE_NEWCGROUP | CLONE_INTO_CGROUP;
	spec->ns.hostname = cg_name;
	spec->cg.name = cg_name;
	spec->cg.memory_max = 67108864;
	spec->cg.pids_max = 32;
	spec->ov.lowerdir = lowerdir;
	spec->ov.upperdir = upperdir;
	spec->ov.workdir = workdir;
	spec->ov.merged = merged;
	spec->mnt.put_old_rel = ".old_root";

	spec->net.bridge = BRIDGE_NAME;
	inet_pton(AF_INET, container_ip, &a);
	spec->net.container_ip_be = a.s_addr;
	inet_pton(AF_INET, "172.30.1.1", &a);
	spec->net.gateway_ip_be = a.s_addr;
	spec->net.prefix_len = 24;

	spec->argv = argv;
	spec->envp = envp;
	return 0;
}

int main(void)
{
	int bfd;
	int ok = 1;
	struct in_addr gw;
	struct container_spec spec1, spec2;
	struct container_handle h1, h2;
	char *argv[] = { "/bin/net_child", NULL };
	char *envp[] = { NULL };
	int exit1 = -1, exit2 = -1;
	char vh1[16], vh2[16];
	struct in_addr ip1, ip2;

	bfd = rtnl_open();
	if (bfd < 0) {
		perror("rtnl_open");
		return 1;
	}

	rtnl_link_delete(bfd, BRIDGE_NAME);

	if (rtnl_bridge_create(bfd, BRIDGE_NAME) != 0) {
		perror("rtnl_bridge_create");
		return 1;
	}
	inet_pton(AF_INET, "172.30.1.1", &gw);
	if (rtnl_addr_add_ipv4(bfd, BRIDGE_NAME, gw.s_addr, 24) != 0) {
		perror("rtnl_addr_add_ipv4 (bridge)");
		return 1;
	}
	if (rtnl_link_set_up(bfd, BRIDGE_NAME) != 0) {
		perror("rtnl_link_set_up (bridge)");
		return 1;
	}

	if (test_image_fixture_build(IMAGE_ROOT, "build/net_child", "net_child") != 0)
		return 1;

	if (build_container_spec(&spec1, "tcc-net-c1", "/tmp/container_net_test/c1", "172.30.1.10",
	                          argv, envp) != 0)
		return 1;
	if (build_container_spec(&spec2, "tcc-net-c2", "/tmp/container_net_test/c2", "172.30.1.11",
	                          argv, envp) != 0)
		return 1;

	/* both created before either is waited on -- genuinely concurrent */
	if (container_create(&spec1, &h1) != 0) {
		perror("container_create c1");
		return 1;
	}
	if (container_create(&spec2, &h2) != 0) {
		perror("container_create c2");
		return 1;
	}

	snprintf(vh1, sizeof(vh1), "vh%d", (int)h1.pid);
	snprintf(vh2, sizeof(vh2), "vh%d", (int)h2.pid);

	/* give each container a brief moment to bind() before we connect */
	usleep(200000);

	inet_pton(AF_INET, "172.30.1.10", &ip1);
	inet_pton(AF_INET, "172.30.1.11", &ip2);
	connect_and_echo(ip1.s_addr, &ok);
	connect_and_echo(ip2.s_addr, &ok);

	if (container_wait(&h1, &exit1) != 0) {
		perror("container_wait c1");
		ok = 0;
	} else if (exit1 != 0) {
		fprintf(stderr, "FAIL: c1 exit status %d, expected 0\n", exit1);
		ok = 0;
	}
	if (container_wait(&h2, &exit2) != 0) {
		perror("container_wait c2");
		ok = 0;
	} else if (exit2 != 0) {
		fprintf(stderr, "FAIL: c2 exit status %d, expected 0\n", exit2);
		ok = 0;
	}
	close(h1.pidfd);
	close(h1.cgroup_fd);
	close(h2.pidfd);
	close(h2.cgroup_fd);

	/* The kernel destroys both ends of a veth pair once the netns
	 * holding one end is torn down -- no explicit delete needed, and
	 * worth confirming directly rather than assuming. */
	if (!iface_gone_eventually(vh1, 20)) {
		fprintf(stderr, "FAIL: %s still exists after c1 exited\n", vh1);
		ok = 0;
	}
	if (!iface_gone_eventually(vh2, 20)) {
		fprintf(stderr, "FAIL: %s still exists after c2 exited\n", vh2);
		ok = 0;
	}

	rtnl_link_delete(bfd, BRIDGE_NAME);
	rtnl_close(bfd);

	printf(ok ? "CONTAINER NET RESULT: PASS\n" : "CONTAINER NET RESULT: FAIL\n");
	return ok ? 0 : 1;
}
