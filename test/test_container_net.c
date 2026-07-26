/*
 * Phase 6 part 2 / Phase 7 parts 2+3 end-to-end test: proves real
 * networking works through the actual container lifecycle
 * (container_create(), not raw rtnetlink primitives -- those were
 * already proven in isolation by test_rtnetlink.c).
 *
 * Three scenarios:
 * 1. Two containers concurrently on the same bridge -- real,
 *    simultaneous connectivity to each, then confirms the kernel
 *    tears down both veth ends automatically once each container
 *    exits (no explicit delete call needed for that part, only for
 *    the bridge itself).
 * 2. One container attached to TWO bridges at once (Phase 7 part 2's
 *    multi-homing) -- both interfaces independently addressed and
 *    reachable from the host.
 * 3. A real 3-container router topology (Phase 7 part 3): R sits on
 *    both bridges with ip_forward on; H (bridge 1 only) and T
 *    (bridge 2 only) each get a static route pointing at R for the
 *    other's subnet. H connects to T *from inside its own netns*,
 *    proving packets actually cross through R's kernel routing --
 *    not just that the syscalls to set it up didn't error.
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
#define BRIDGE_NAME2 "kanxeo-ctnet1"
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
                                 const char *scratch_dir, const struct network_spec *nets,
                                 int net_count, int ip_forward, const struct route_spec *routes,
                                 int route_count, char **argv, char **envp)
{
	static char lowerdir[256];
	static char upperdir[256];
	static char workdir[256];
	static char merged[256];
	int i;

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

	spec->net_count = net_count;
	for (i = 0; i < net_count; i++)
		spec->nets[i] = nets[i];
	spec->ip_forward = ip_forward;
	spec->route_count = route_count;
	for (i = 0; i < route_count; i++)
		spec->routes[i] = routes[i];

	spec->argv = argv;
	spec->envp = envp;
	return 0;
}

static uint32_t ipv4(const char *s)
{
	struct in_addr a;

	inet_pton(AF_INET, s, &a);
	return a.s_addr;
}

int main(void)
{
	int bfd;
	int ok = 1;
	char *argv[] = { "/bin/net_child", NULL };
	char *argv_dual[] = { "/bin/net_child", "2", NULL };
	char *envp[] = { NULL };

	bfd = rtnl_open();
	if (bfd < 0) {
		perror("rtnl_open");
		return 1;
	}

	rtnl_link_delete(bfd, BRIDGE_NAME);
	rtnl_link_delete(bfd, BRIDGE_NAME2);

	if (rtnl_bridge_create(bfd, BRIDGE_NAME) != 0 ||
	    rtnl_addr_add_ipv4(bfd, BRIDGE_NAME, ipv4("172.30.1.1"), 24) != 0 ||
	    rtnl_link_set_up(bfd, BRIDGE_NAME) != 0) {
		perror("bridge 1 setup");
		return 1;
	}
	if (rtnl_bridge_create(bfd, BRIDGE_NAME2) != 0 ||
	    rtnl_addr_add_ipv4(bfd, BRIDGE_NAME2, ipv4("172.30.2.1"), 24) != 0 ||
	    rtnl_link_set_up(bfd, BRIDGE_NAME2) != 0) {
		perror("bridge 2 setup");
		return 1;
	}

	if (test_image_fixture_build(IMAGE_ROOT, "build/net_child", "net_child") != 0)
		return 1;
	if (test_image_fixture_build(IMAGE_ROOT, "build/daemon_child", "daemon_child") != 0)
		return 1;
	if (test_image_fixture_build(IMAGE_ROOT, "build/net_connect", "net_connect") != 0)
		return 1;

	/* 1. two containers concurrently on the same bridge */
	{
		struct container_spec spec1, spec2;
		struct container_handle h1, h2;
		struct network_spec net1, net2;
		int exit1 = -1, exit2 = -1;
		char vh1[16], vh2[16];

		net1.bridge = BRIDGE_NAME;
		net1.container_ip_be = ipv4("172.30.1.10");
		net1.gateway_ip_be = ipv4("172.30.1.1");
		net1.prefix_len = 24;

		net2.bridge = BRIDGE_NAME;
		net2.container_ip_be = ipv4("172.30.1.11");
		net2.gateway_ip_be = ipv4("172.30.1.1");
		net2.prefix_len = 24;

		if (build_container_spec(&spec1, "tcc-net-c1", "/tmp/container_net_test/c1", &net1, 1, 0,
		                          NULL, 0, argv, envp) != 0)
			return 1;
		if (build_container_spec(&spec2, "tcc-net-c2", "/tmp/container_net_test/c2", &net2, 1, 0,
		                          NULL, 0, argv, envp) != 0)
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

		snprintf(vh1, sizeof(vh1), "vh%d-0", (int)h1.pid);
		snprintf(vh2, sizeof(vh2), "vh%d-0", (int)h2.pid);

		/* give each container a brief moment to bind() before we connect */
		usleep(200000);

		connect_and_echo(net1.container_ip_be, &ok);
		connect_and_echo(net2.container_ip_be, &ok);

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
		 * holding one end is torn down -- no explicit delete needed,
		 * and worth confirming directly rather than assuming. */
		if (!iface_gone_eventually(vh1, 20)) {
			fprintf(stderr, "FAIL: %s still exists after c1 exited\n", vh1);
			ok = 0;
		}
		if (!iface_gone_eventually(vh2, 20)) {
			fprintf(stderr, "FAIL: %s still exists after c2 exited\n", vh2);
			ok = 0;
		}
	}

	/* 2. one container, two bridges at once -- Phase 7 part 2's
	 * multi-homing, proven at the real container_create() level */
	{
		struct container_spec spec3;
		struct container_handle h3;
		struct network_spec nets3[2];
		int exit3 = -1;
		char vh3a[16], vh3b[16];

		nets3[0].bridge = BRIDGE_NAME;
		nets3[0].container_ip_be = ipv4("172.30.1.20");
		nets3[0].gateway_ip_be = ipv4("172.30.1.1");
		nets3[0].prefix_len = 24;

		nets3[1].bridge = BRIDGE_NAME2;
		nets3[1].container_ip_be = ipv4("172.30.2.20");
		nets3[1].gateway_ip_be = ipv4("172.30.2.1");
		nets3[1].prefix_len = 24;

		if (build_container_spec(&spec3, "tcc-net-c3", "/tmp/container_net_test/c3", nets3, 2, 0,
		                          NULL, 0, argv_dual, envp) != 0)
			return 1;

		if (container_create(&spec3, &h3) != 0) {
			perror("container_create c3");
			return 1;
		}

		snprintf(vh3a, sizeof(vh3a), "vh%d-0", (int)h3.pid);
		snprintf(vh3b, sizeof(vh3b), "vh%d-1", (int)h3.pid);

		usleep(200000);

		/* real connectivity to BOTH interfaces, from their respective
		 * subnets -- proves eth0/eth1 are both independently addressed
		 * and reachable, not just that container_create() didn't
		 * error out */
		connect_and_echo(nets3[0].container_ip_be, &ok);
		connect_and_echo(nets3[1].container_ip_be, &ok);

		if (container_wait(&h3, &exit3) != 0) {
			perror("container_wait c3");
			ok = 0;
		} else if (exit3 != 0) {
			fprintf(stderr, "FAIL: c3 exit status %d, expected 0\n", exit3);
			ok = 0;
		}
		close(h3.pidfd);
		close(h3.cgroup_fd);

		if (!iface_gone_eventually(vh3a, 20)) {
			fprintf(stderr, "FAIL: %s still exists after c3 exited\n", vh3a);
			ok = 0;
		}
		if (!iface_gone_eventually(vh3b, 20)) {
			fprintf(stderr, "FAIL: %s still exists after c3 exited\n", vh3b);
			ok = 0;
		}
	}

	/* 3. real 3-container router topology -- Phase 7 part 3. R sits on
	 * both bridges with ip_forward on; H and T each get a static route
	 * pointing at R for the other's subnet; H connects to T from
	 * inside its own netns, proving the round trip actually crosses
	 * through R's kernel routing table. */
	{
		struct container_spec specR, specH, specT;
		struct container_handle hR, hH, hT;
		struct network_spec netsR[2], netH, netT;
		struct route_spec routeH, routeT;
		int exitR = -1, exitH = -1, exitT = -1;
		char *argv_router[] = { "/bin/daemon_child", "6", "0", NULL };
		char *argv_host[] = { "/bin/net_connect", "172.30.2.50", NULL };

		netsR[0].bridge = BRIDGE_NAME;
		netsR[0].container_ip_be = ipv4("172.30.1.30");
		netsR[0].gateway_ip_be = ipv4("172.30.1.1");
		netsR[0].prefix_len = 24;
		netsR[1].bridge = BRIDGE_NAME2;
		netsR[1].container_ip_be = ipv4("172.30.2.30");
		netsR[1].gateway_ip_be = ipv4("172.30.2.1");
		netsR[1].prefix_len = 24;

		netH.bridge = BRIDGE_NAME;
		netH.container_ip_be = ipv4("172.30.1.40");
		netH.gateway_ip_be = ipv4("172.30.1.1");
		netH.prefix_len = 24;
		routeH.dest_be = ipv4("172.30.2.0");
		routeH.dest_prefix_len = 24;
		routeH.gateway_be = ipv4("172.30.1.30"); /* R's bridge-1 IP */

		netT.bridge = BRIDGE_NAME2;
		netT.container_ip_be = ipv4("172.30.2.50");
		netT.gateway_ip_be = ipv4("172.30.2.1");
		netT.prefix_len = 24;
		routeT.dest_be = ipv4("172.30.1.0");
		routeT.dest_prefix_len = 24;
		routeT.gateway_be = ipv4("172.30.2.30"); /* R's bridge-2 IP */

		if (build_container_spec(&specR, "tcc-net-r", "/tmp/container_net_test/r", netsR, 2, 1,
		                          NULL, 0, argv_router, envp) != 0)
			return 1;
		if (build_container_spec(&specT, "tcc-net-t", "/tmp/container_net_test/t", &netT, 1, 0,
		                          &routeT, 1, argv, envp) != 0)
			return 1;
		if (build_container_spec(&specH, "tcc-net-h", "/tmp/container_net_test/h", &netH, 1, 0,
		                          &routeH, 1, argv_host, envp) != 0)
			return 1;

		/* T listens, R routes, then H connects out to T through R --
		 * created in this order so T and R are both already up before
		 * H starts (net_connect still retries regardless, since
		 * container startup timing isn't guaranteed). */
		if (container_create(&specT, &hT) != 0) {
			perror("container_create t (router scenario)");
			return 1;
		}
		if (container_create(&specR, &hR) != 0) {
			perror("container_create r (router scenario)");
			return 1;
		}
		if (container_create(&specH, &hH) != 0) {
			perror("container_create h (router scenario)");
			return 1;
		}

		/* H's own exit status is the real proof: net_connect only
		 * exits 0 if its round trip through R's kernel routing table
		 * actually succeeded. */
		if (container_wait(&hH, &exitH) != 0) {
			perror("container_wait h (router scenario)");
			ok = 0;
		} else if (exitH != 0) {
			fprintf(stderr,
			        "FAIL: h (router scenario) exit status %d, expected 0 -- packet "
			        "forwarding through the router container did not work\n",
			        exitH);
			ok = 0;
		}

		if (container_wait(&hT, &exitT) != 0) {
			perror("container_wait t (router scenario)");
			ok = 0;
		} else if (exitT != 0) {
			fprintf(stderr, "FAIL: t (router scenario) exit status %d, expected 0\n", exitT);
			ok = 0;
		}

		if (container_wait(&hR, &exitR) != 0) {
			perror("container_wait r (router scenario)");
			ok = 0;
		} else if (exitR != 0) {
			fprintf(stderr, "FAIL: r (router scenario) exit status %d, expected 0\n", exitR);
			ok = 0;
		}

		close(hH.pidfd);
		close(hH.cgroup_fd);
		close(hT.pidfd);
		close(hT.cgroup_fd);
		close(hR.pidfd);
		close(hR.cgroup_fd);
	}

	rtnl_link_delete(bfd, BRIDGE_NAME);
	rtnl_link_delete(bfd, BRIDGE_NAME2);
	rtnl_close(bfd);

	printf(ok ? "CONTAINER NET RESULT: PASS\n" : "CONTAINER NET RESULT: FAIL\n");
	return ok ? 0 : 1;
}
