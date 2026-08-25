/*
 * Phase 6 part 2 / Phase 7 parts 2+3 end-to-end test: proves real
 * networking works through the actual container lifecycle
 * (container_create(), not raw rtnetlink primitives -- those were
 * already proven in isolation by test_rtnetlink.c).
 *
 * Five scenarios:
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
 * 4. Real interface passthrough (Phase 12 part 6, ADR-0022) -- a veth
 *    end standing in for a physical NIC, moved into a container's
 *    netns and back out again.
 * 5. Gateway optionality (ADR-0037) -- has_address=0/1 on a network
 *    attachment controls whether container_net_child_configure()
 *    installs a default route at all, checked directly against each
 *    container's own /proc/<pid>/net/route.
 */
#include "container.h"
#include "internal.h"
#include "linux_compat.h"
#include "rtnetlink.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BRIDGE_NAME "cix-ctnet0"
#define BRIDGE_NAME2 "cix-ctnet1"
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

/*
 * Reads /proc/<pid>/net/route directly -- readable cross-netns without
 * setns() (unlike /sys/class/net, this /proc/<pid>/... form is scoped
 * to that pid's own netns per-access, not captured at mount time; the
 * same "the mount is stale, the /proc/<pid>/... path isn't" distinction
 * check_iface_inside_netns()'s own comment above already draws for a
 * different file). Returns 1 if a default route (destination field
 * "00000000") is present, 0 if not, -1 on any read error.
 */
static int has_default_route(pid_t pid)
{
	char path[64];
	FILE *f;
	char line[256];
	int first = 1;
	int found = 0;

	snprintf(path, sizeof(path), "/proc/%d/net/route", (int)pid);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;

	while (fgets(line, sizeof(line), f) != NULL) {
		char iface[64], dest[16];

		if (first) {
			first = 0; /* header line, not a route */
			continue;
		}
		if (sscanf(line, "%63s %15s", iface, dest) == 2 && strcmp(dest, "00000000") == 0) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/*
 * container_create() returns once the parent's own host-side setup is
 * done, not once the child has finished its whole setup sequence
 * (network config, then routes, then ip_forward/sysctls, then
 * execve()) -- route installation in particular happens strictly
 * after network config (src/container.c), so checking has_default_
 * route() a single time immediately after container_create() races
 * the child's own in-progress setup. Poll instead, the same bounded-
 * retry pattern iface_gone_eventually() above already established for
 * a different genuinely-asynchronous condition.
 */
static int has_default_route_eventually(pid_t pid, int max_attempts)
{
	int i;

	for (i = 0; i < max_attempts; i++) {
		int r = has_default_route(pid);

		if (r != 0)
			return r; /* found (1), or a real read error (-1) -- stop either way */
		usleep(100000);
	}
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

/*
 * Forks a throwaway helper that setns()'s into netns_fd and checks,
 * from that exact vantage point, whether ifname is visible there and
 * administratively up. Deliberately NOT via /sys/class/net (unlike
 * iface_exists() above, which only ever runs in this process's own,
 * never-changed netns): sysfs's netns view is captured at the time
 * /sys was mounted, not re-evaluated per access, so a stale, already-
 * mounted /sys keeps showing the OLD netns's interfaces even after a
 * successful setns() -- confirmed the hard way while first writing
 * this check (see docs/adr/0022's own Context). SIOCGIFFLAGS via a
 * socket created fresh, after setns(), doesn't have that problem: a
 * socket (like the netlink sockets container_net.c's own real
 * teardown helper uses) is scoped to whatever netns is current at ITS
 * OWN creation, genuinely dynamic. Returns 1 if visible and up, 0 if
 * visible but down, -1 if not visible at all (or any other error).
 */
static int check_iface_inside_netns(int netns_fd, const char *ifname)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int sock;
		struct ifreq ifr;

		if (setns(netns_fd, CLONE_NEWNET) != 0)
			_exit(2);

		sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock < 0)
			_exit(2);

		memset(&ifr, 0, sizeof(ifr));
		snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
		if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
			close(sock);
			_exit(2);
		}
		close(sock);
		_exit((ifr.ifr_flags & IFF_UP) ? 1 : 0);
	}

	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return -1;
	if (WEXITSTATUS(status) == 2)
		return -1;
	return WEXITSTATUS(status);
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
		net1.has_address = 1;
		net1.address_ip_be = ipv4("172.30.1.1");
		net1.prefix_len = 24;

		net2.bridge = BRIDGE_NAME;
		net2.container_ip_be = ipv4("172.30.1.11");
		net2.has_address = 1;
		net2.address_ip_be = ipv4("172.30.1.1");
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

		if (container_wait(&h1, &exit1, NULL) != 0) {
			perror("container_wait c1");
			ok = 0;
		} else if (exit1 != 0) {
			fprintf(stderr, "FAIL: c1 exit status %d, expected 0\n", exit1);
			ok = 0;
		}
		if (container_wait(&h2, &exit2, NULL) != 0) {
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
		nets3[0].has_address = 1;
		nets3[0].address_ip_be = ipv4("172.30.1.1");
		nets3[0].prefix_len = 24;

		nets3[1].bridge = BRIDGE_NAME2;
		nets3[1].container_ip_be = ipv4("172.30.2.20");
		nets3[1].has_address = 1;
		nets3[1].address_ip_be = ipv4("172.30.2.1");
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

		if (container_wait(&h3, &exit3, NULL) != 0) {
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
		netsR[0].has_address = 1;
		netsR[0].address_ip_be = ipv4("172.30.1.1");
		netsR[0].prefix_len = 24;
		netsR[1].bridge = BRIDGE_NAME2;
		netsR[1].container_ip_be = ipv4("172.30.2.30");
		netsR[1].has_address = 1;
		netsR[1].address_ip_be = ipv4("172.30.2.1");
		netsR[1].prefix_len = 24;

		netH.bridge = BRIDGE_NAME;
		netH.container_ip_be = ipv4("172.30.1.40");
		netH.has_address = 1;
		netH.address_ip_be = ipv4("172.30.1.1");
		netH.prefix_len = 24;
		routeH.dest_be = ipv4("172.30.2.0");
		routeH.dest_prefix_len = 24;
		routeH.gateway_be = ipv4("172.30.1.30"); /* R's bridge-1 IP */

		netT.bridge = BRIDGE_NAME2;
		netT.container_ip_be = ipv4("172.30.2.50");
		netT.has_address = 1;
		netT.address_ip_be = ipv4("172.30.2.1");
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
		if (container_wait(&hH, &exitH, NULL) != 0) {
			perror("container_wait h (router scenario)");
			ok = 0;
		} else if (exitH != 0) {
			fprintf(stderr,
			        "FAIL: h (router scenario) exit status %d, expected 0 -- packet "
			        "forwarding through the router container did not work\n",
			        exitH);
			ok = 0;
		}

		if (container_wait(&hT, &exitT, NULL) != 0) {
			perror("container_wait t (router scenario)");
			ok = 0;
		} else if (exitT != 0) {
			fprintf(stderr, "FAIL: t (router scenario) exit status %d, expected 0\n", exitT);
			ok = 0;
		}

		if (container_wait(&hR, &exitR, NULL) != 0) {
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

	/*
	 * 4. Real interface passthrough (Phase 12 part 6) --
	 * container_net_host_attach_interfaces()/container_net_teardown_interfaces()
	 * (src/container_net.c), proven directly against the real kernel
	 * primitives rather than through GET /v1/devices (this dev
	 * sandbox's own root netns has no physically-backed NIC visible to
	 * it at all -- confirmed directly, see docs/adr/0022 -- so nothing
	 * would ever appear there to test with end to end). A veth pair's
	 * own two ends stand in for "some named interface not currently in
	 * a container's netns": rtnl_link_set_netns_pid()/_fd() are fully
	 * generic (confirmed via kernel source, not veth-specific), so
	 * moving one really does exercise the identical code path a real
	 * physical NIC would take.
	 */
	{
		struct container_spec spec4;
		struct container_handle h4;
		const char *ifname_move = "cix-ifpt-b";
		const char *ifname_keep = "cix-ifpt-a";
		char *argv4[] = { "/bin/daemon_child", "2", "0", NULL };
		int exit4 = -1;
		int inside;

		rtnl_link_delete(bfd, ifname_keep);
		if (rtnl_veth_create(bfd, ifname_keep, ifname_move) != 0) {
			perror("veth create (interface passthrough scenario)");
			ok = 0;
			goto scenario4_done;
		}
		if (!iface_exists(ifname_move) || !iface_exists(ifname_keep)) {
			fprintf(stderr, "FAIL: veth pair not visible in root netns right after creation\n");
			ok = 0;
			goto scenario4_cleanup;
		}

		if (build_container_spec(&spec4, "tcc-net-ifpt", "/tmp/container_net_test/ifpt", NULL, 0,
		                          0, NULL, 0, argv4, envp) != 0) {
			ok = 0;
			goto scenario4_cleanup;
		}
		spec4.interface_count = 1;
		snprintf(spec4.interfaces[0], sizeof(spec4.interfaces[0]), "%s", ifname_move);

		if (container_create(&spec4, &h4) != 0) {
			perror("container_create ifpt");
			ok = 0;
			goto scenario4_cleanup;
		}

		/* Moved out: no longer visible in the root netns at all -- the
		 * exact mechanism that makes exclusivity automatic in
		 * GET /v1/devices too. Its untouched peer is proof this wasn't
		 * just "the whole veth pair vanished." */
		if (iface_exists(ifname_move)) {
			fprintf(stderr, "FAIL: %s still visible in root netns after being granted\n",
			        ifname_move);
			ok = 0;
		}
		if (!iface_exists(ifname_keep)) {
			fprintf(stderr, "FAIL: %s (untouched peer) vanished too\n", ifname_keep);
			ok = 0;
		}

		/* Visible inside the container's own netns, still under its
		 * real, unrenamed name, and administratively up -- both
		 * confirmed from that exact vantage point, not inferred. */
		inside = check_iface_inside_netns(h4.interfaces_netns_fd, ifname_move);
		if (inside != 1) {
			fprintf(stderr,
			        "FAIL: %s not visible+up inside the container's netns (result=%d)\n",
			        ifname_move, inside);
			ok = 0;
		}

		if (container_wait(&h4, &exit4, NULL) != 0) {
			perror("container_wait ifpt");
			ok = 0;
		} else if (exit4 != 0) {
			fprintf(stderr, "FAIL: ifpt exit status %d, expected 0\n", exit4);
			ok = 0;
		}

		/* Teardown: the same call registry_remove() makes. The whole
		 * point of holding interfaces_netns_fd open since creation --
		 * the container's own process (and, left alone, its netns) is
		 * already gone by this point. */
		if (container_net_teardown_interfaces((const char *const[]){ ifname_move }, 1,
		                                       h4.interfaces_netns_fd) != 0) {
			fprintf(stderr, "FAIL: container_net_teardown_interfaces() returned nonzero\n");
			ok = 0;
		}

		close(h4.pidfd);
		close(h4.cgroup_fd);

		/* Reappears in the root netns under its real, original name --
		 * not the kernel's own unpredictable "devN" fallback, since
		 * this daemon acted before the netns was ever allowed to
		 * disappear on its own. */
		{
			int reappeared = 0;
			int attempt;

			for (attempt = 0; attempt < 20; attempt++) {
				if (iface_exists(ifname_move)) {
					reappeared = 1;
					break;
				}
				usleep(100000);
			}
			if (!reappeared) {
				fprintf(stderr,
				        "FAIL: %s did not reappear in the root netns after teardown\n",
				        ifname_move);
				ok = 0;
			}
		}

	scenario4_cleanup:
		rtnl_link_delete(bfd, ifname_keep);
	scenario4_done:;
	}

	/*
	 * 5. Gateway-optionality (ADR-0037) at the container_net_child_
	 * configure() level: has_address=0 on the primary attachment must
	 * install no default route at all, has_address=1 must install one
	 * pointing at address_ip_be exactly as before, and an explicit
	 * 0.0.0.0/0 route_spec (the documented escape hatch for a gateway-
	 * less network, e.g. pointing at a VRRP address neither Cix nor
	 * the host owns) still installs one regardless of has_address.
	 * Checked directly against each container's own /proc/<pid>/net/
	 * route -- readable cross-netns without setns() (a real, standard
	 * Linux property, not this project's own mechanism) -- rather than
	 * an indirect connectivity proxy, since what's actually under test
	 * is a one-line conditional, not end-to-end packet forwarding
	 * (scenario 3 already proves that part).
	 */
	{
		struct container_spec spec5a, spec5b, spec5c;
		struct container_handle h5a, h5b, h5c;
		struct network_spec net5a, net5b, net5c;
		struct route_spec defroute5c;
		char *argv5[] = { "/bin/daemon_child", "2", "0", NULL };
		int exit5;

		net5a.bridge = BRIDGE_NAME;
		net5a.container_ip_be = ipv4("172.30.1.50");
		net5a.has_address = 0;
		net5a.address_ip_be = ipv4("172.30.1.1"); /* deliberately set but must be ignored */
		net5a.prefix_len = 24;

		net5b = net5a;
		net5b.container_ip_be = ipv4("172.30.1.51");
		net5b.has_address = 1;

		net5c = net5a;
		net5c.container_ip_be = ipv4("172.30.1.52");
		defroute5c.dest_be = 0;
		defroute5c.dest_prefix_len = 0;
		defroute5c.gateway_be = ipv4("172.30.1.53"); /* not actually a live router -- only the
		                                               * route table entry itself is checked */

		if (build_container_spec(&spec5a, "tcc-net-g1", "/tmp/container_net_test/g1", &net5a, 1, 0,
		                          NULL, 0, argv5, envp) != 0 ||
		    build_container_spec(&spec5b, "tcc-net-g2", "/tmp/container_net_test/g2", &net5b, 1, 0,
		                          NULL, 0, argv5, envp) != 0 ||
		    build_container_spec(&spec5c, "tcc-net-g3", "/tmp/container_net_test/g3", &net5c, 1, 0,
		                          &defroute5c, 1, argv5, envp) != 0)
			return 1;

		if (container_create(&spec5a, &h5a) != 0 || container_create(&spec5b, &h5b) != 0 ||
		    container_create(&spec5c, &h5c) != 0) {
			perror("container_create (scenario 5)");
			return 1;
		}

		/* h5b/h5c: poll -- their own child hasn't necessarily finished
		 * installing routes yet the instant container_create() returns
		 * (see has_default_route_eventually()'s own comment). h5a: a
		 * fixed settle delay first, long enough that its own sibling
		 * containers' route-install steps (strictly simpler/faster than
		 * this poll's own worst case) have certainly finished too, then
		 * one check -- an absence can't be racily "not there yet" in a
		 * way a longer wait would ever flip. */
		if (!has_default_route_eventually(h5b.pid, 20)) {
			fprintf(stderr, "FAIL: has_address=1 did not install a default route\n");
			ok = 0;
		}
		if (!has_default_route_eventually(h5c.pid, 20)) {
			fprintf(stderr,
			        "FAIL: explicit 0.0.0.0/0 route_spec on a gateway-less network did not "
			        "install a default route\n");
			ok = 0;
		}
		usleep(500000);
		if (has_default_route(h5a.pid)) {
			fprintf(stderr, "FAIL: has_address=0 installed a default route anyway\n");
			ok = 0;
		}

		if (container_wait(&h5a, &exit5, NULL) != 0)
			perror("container_wait g1");
		if (container_wait(&h5b, &exit5, NULL) != 0)
			perror("container_wait g2");
		if (container_wait(&h5c, &exit5, NULL) != 0)
			perror("container_wait g3");
		close(h5a.pidfd);
		close(h5a.cgroup_fd);
		close(h5b.pidfd);
		close(h5b.cgroup_fd);
		close(h5c.pidfd);
		close(h5c.cgroup_fd);
	}

	rtnl_link_delete(bfd, BRIDGE_NAME);
	rtnl_link_delete(bfd, BRIDGE_NAME2);
	rtnl_close(bfd);

	printf(ok ? "CONTAINER NET RESULT: PASS\n" : "CONTAINER NET RESULT: FAIL\n");
	return ok ? 0 : 1;
}
