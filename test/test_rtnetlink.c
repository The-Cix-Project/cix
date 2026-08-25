/*
 * Phase 6 part 1 end-to-end test: proves the rtnetlink primitives
 * (netplane/src/rtnetlink.c) actually work -- bridge/veth creation,
 * moving a link into another process's network namespace, addressing
 * (including assigning the "gateway" IP to the bridge device itself,
 * not to a bridge port -- a port carrying its own IP while enslaved
 * doesn't behave like a normal host interface), and real TCP
 * connectivity through the resulting topology.
 *
 * Not wired into container_create()/the REST API/any client yet --
 * see docs/roadmap/ROADMAP.md Phase 6 for why that's deliberately a separate,
 * later step.
 */
#include "internal.h"
#include "rtnetlink.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BRIDGE_NAME "cix-test0"
#define VETH_HOST "vt-a"
#define VETH_CTR "vt-b"
#define TEST_PORT 17623

static int iface_exists(const char *name)
{
	char path[128];
	struct stat st;

	snprintf(path, sizeof(path), "/sys/class/net/%s", name);
	return stat(path, &st) == 0;
}

static int bridge_has_port(const char *bridge, const char *port)
{
	char path[128];
	struct stat st;

	snprintf(path, sizeof(path), "/sys/class/net/%s/brif/%s", bridge, port);
	return stat(path, &st) == 0;
}

static int is_bridge(const char *name)
{
	char path[128];
	struct stat st;

	snprintf(path, sizeof(path), "/sys/class/net/%s/bridge", name);
	return stat(path, &st) == 0;
}

static void cleanup_leftovers(int fd)
{
	rtnl_link_delete(fd, VETH_HOST);
	rtnl_link_delete(fd, BRIDGE_NAME);
}

int main(void)
{
	int fd;
	int ok = 1;
	uint32_t host_ip, ctr_ip;
	int net_ready[2];
	int listen_ready[2];
	long child_pid;
	int child_pidfd = -1;
	struct in_addr a;

	inet_pton(AF_INET, "172.30.0.1", &a);
	host_ip = a.s_addr;
	inet_pton(AF_INET, "172.30.0.2", &a);
	ctr_ip = a.s_addr;

	fd = rtnl_open();
	if (fd < 0) {
		perror("rtnl_open");
		return 1;
	}

	cleanup_leftovers(fd);

	if (rtnl_bridge_create(fd, BRIDGE_NAME) != 0) {
		perror("rtnl_bridge_create");
		ok = 0;
	} else if (!is_bridge(BRIDGE_NAME)) {
		fprintf(stderr, "FAIL: %s does not look like a bridge\n", BRIDGE_NAME);
		ok = 0;
	}

	if (rtnl_veth_create(fd, VETH_HOST, VETH_CTR) != 0) {
		perror("rtnl_veth_create");
		ok = 0;
	} else if (!iface_exists(VETH_HOST) || !iface_exists(VETH_CTR)) {
		fprintf(stderr, "FAIL: veth pair did not appear on the host\n");
		ok = 0;
	}

	if (pipe(net_ready) != 0 || pipe(listen_ready) != 0) {
		perror("pipe");
		return 1;
	}

	/* "container-like" child: just a fresh net namespace, nothing else. */
	child_pid = ns_clone3(CLONE_NEWNET, -1, &child_pidfd);
	if (child_pid < 0) {
		perror("ns_clone3");
		return 1;
	}

	if (child_pid == 0) {
		char c;
		int cfd, lfd, cconn;
		struct sockaddr_in addr;
		char rbyte;

		close(net_ready[1]);
		close(listen_ready[0]);

		/* wait until the parent has moved vt-b in and stood up the
		 * host/bridge side */
		if (read(net_ready[0], &c, 1) != 1) {
			perror("child: read net_ready");
			_exit(1);
		}

		cfd = rtnl_open();
		if (cfd < 0) {
			perror("child: rtnl_open");
			_exit(1);
		}
		if (rtnl_addr_add_ipv4(cfd, VETH_CTR, ctr_ip, 24) != 0) {
			perror("child: rtnl_addr_add_ipv4");
			_exit(1);
		}
		if (rtnl_link_set_up(cfd, VETH_CTR) != 0) {
			perror("child: rtnl_link_set_up veth");
			_exit(1);
		}
		if (rtnl_link_set_up(cfd, "lo") != 0) {
			perror("child: rtnl_link_set_up lo");
			_exit(1);
		}
		rtnl_close(cfd);

		lfd = socket(AF_INET, SOCK_STREAM, 0);
		if (lfd < 0) {
			perror("child: socket");
			_exit(1);
		}
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(TEST_PORT);
		addr.sin_addr.s_addr = ctr_ip;
		if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
			perror("child: bind");
			_exit(1);
		}
		if (listen(lfd, 1) != 0) {
			perror("child: listen");
			_exit(1);
		}

		if (write(listen_ready[1], "x", 1) != 1) {
			perror("child: write listen_ready");
			_exit(1);
		}

		cconn = accept(lfd, NULL, NULL);
		if (cconn < 0) {
			perror("child: accept");
			_exit(1);
		}
		if (read(cconn, &rbyte, 1) != 1 || write(cconn, &rbyte, 1) != 1) {
			perror("child: echo byte");
			_exit(1);
		}
		close(cconn);
		_exit(0);
	}

	close(net_ready[0]);
	close(listen_ready[1]);

	if (rtnl_link_set_netns_pid(fd, VETH_CTR, (pid_t)child_pid) != 0) {
		perror("rtnl_link_set_netns_pid");
		ok = 0;
	} else if (iface_exists(VETH_CTR)) {
		fprintf(stderr, "FAIL: %s still visible on host after netns move\n", VETH_CTR);
		ok = 0;
	}

	if (rtnl_link_set_master(fd, VETH_HOST, BRIDGE_NAME) != 0) {
		perror("rtnl_link_set_master");
		ok = 0;
	} else if (!bridge_has_port(BRIDGE_NAME, VETH_HOST)) {
		fprintf(stderr, "FAIL: %s not attached to %s\n", VETH_HOST, BRIDGE_NAME);
		ok = 0;
	}

	/*
	 * The "gateway" IP goes on the bridge device itself, not on a
	 * port that's now enslaved to it -- a bridge port doesn't behave
	 * like a normal addressable host interface once attached.
	 */
	if (rtnl_addr_add_ipv4(fd, BRIDGE_NAME, host_ip, 24) != 0) {
		perror("rtnl_addr_add_ipv4 (bridge)");
		ok = 0;
	}
	if (rtnl_link_set_up(fd, VETH_HOST) != 0) {
		perror("rtnl_link_set_up (veth host side)");
		ok = 0;
	}
	if (rtnl_link_set_up(fd, BRIDGE_NAME) != 0) {
		perror("rtnl_link_set_up (bridge)");
		ok = 0;
	}

	if (write(net_ready[1], "x", 1) != 1) {
		perror("write net_ready");
		ok = 0;
	}

	{
		char c;

		if (read(listen_ready[0], &c, 1) != 1) {
			perror("read listen_ready");
			ok = 0;
		}
	}

	/* Real connectivity, not just "the syscalls didn't error": connect
	 * from the host netns, through the bridge, to the container's IP. */
	{
		int cfd2;
		struct sockaddr_in addr;
		char sbyte = 42, rbyte = 0;

		cfd2 = socket(AF_INET, SOCK_STREAM, 0);
		if (cfd2 < 0) {
			perror("socket");
			ok = 0;
		} else {
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons(TEST_PORT);
			addr.sin_addr.s_addr = ctr_ip;

			if (connect(cfd2, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
				perror("connect");
				ok = 0;
			} else if (write(cfd2, &sbyte, 1) != 1 || read(cfd2, &rbyte, 1) != 1 ||
			           rbyte != sbyte) {
				fprintf(stderr, "FAIL: connectivity round-trip mismatch\n");
				ok = 0;
			}
			close(cfd2);
		}
	}

	{
		siginfo_t info;

		if (waitid(P_PIDFD, child_pidfd, &info, WEXITED) != 0) {
			perror("waitid");
			ok = 0;
		} else if (info.si_status != 0) {
			fprintf(stderr, "FAIL: child exited with status %d\n", info.si_status);
			ok = 0;
		}
		close(child_pidfd);
	}

	/*
	 * rtnl_vlan_create()/rtnl_link_clear_master() (ADR-0038): a fresh,
	 * independent veth pair stands in for "some real parent link" --
	 * legitimate the same way VETH_HOST/VETH_CTR above already stand
	 * in for a real NIC+its remote end, since rtnl_vlan_create() only
	 * needs an existing, resolvable ifindex to attach IFLA_LINK to,
	 * nothing veth-specific. Proves the sub-interface is a genuinely
	 * distinct netdev enslaved on its own -- the parent itself never
	 * becomes a bridge port -- and that clearing the master releases
	 * it again.
	 */
	{
		static const char *const vlan_parent = "vt-vlanp";
		static const char *const vlan_peer = "vt-vlanc";
		static const char *const vlan_sub = "vt-vlanp.100";

		rtnl_link_delete(fd, vlan_sub);
		rtnl_link_delete(fd, vlan_parent);

		if (rtnl_veth_create(fd, vlan_parent, vlan_peer) != 0) {
			perror("rtnl_veth_create (vlan parent pair)");
			ok = 0;
		} else if (rtnl_link_set_up(fd, vlan_parent) != 0) {
			perror("rtnl_link_set_up (vlan parent)");
			ok = 0;
		} else if (rtnl_vlan_create(fd, vlan_sub, vlan_parent, 100) != 0) {
			perror("rtnl_vlan_create");
			ok = 0;
		} else if (!iface_exists(vlan_sub)) {
			fprintf(stderr, "FAIL: %s does not exist after rtnl_vlan_create\n", vlan_sub);
			ok = 0;
		} else if (rtnl_link_set_up(fd, vlan_sub) != 0) {
			perror("rtnl_link_set_up (vlan sub-interface)");
			ok = 0;
		} else if (rtnl_link_set_master(fd, vlan_sub, BRIDGE_NAME) != 0) {
			perror("rtnl_link_set_master (vlan sub-interface)");
			ok = 0;
		} else if (!bridge_has_port(BRIDGE_NAME, vlan_sub)) {
			fprintf(stderr, "FAIL: %s not attached to %s\n", vlan_sub, BRIDGE_NAME);
			ok = 0;
		} else if (bridge_has_port(BRIDGE_NAME, vlan_parent)) {
			fprintf(stderr, "FAIL: parent %s became a bridge port too -- only the VLAN "
			                "sub-interface should have\n",
			        vlan_parent);
			ok = 0;
		} else if (rtnl_link_clear_master(fd, vlan_sub) != 0) {
			perror("rtnl_link_clear_master");
			ok = 0;
		} else if (bridge_has_port(BRIDGE_NAME, vlan_sub)) {
			fprintf(stderr, "FAIL: %s still attached to %s after rtnl_link_clear_master\n",
			        vlan_sub, BRIDGE_NAME);
			ok = 0;
		}

		rtnl_link_delete(fd, vlan_sub);
		rtnl_link_delete(fd, vlan_parent);
	}

	cleanup_leftovers(fd);
	rtnl_close(fd);

	printf(ok ? "RTNETLINK RESULT: PASS\n" : "RTNETLINK RESULT: FAIL\n");
	return ok ? 0 : 1;
}
