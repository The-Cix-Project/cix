#include "internal.h"
#include "rtnetlink.h"

#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int container_net_host_setup(const struct network_spec *nets, int net_count, pid_t child_pid,
                              int ready_pipe_write)
{
	int fd;
	int idx;

	fd = rtnl_open();
	if (fd < 0)
		return -1;

	for (idx = 0; idx < net_count; idx++) {
		char veth_host[16];
		char veth_ctr[16];
		size_t len;

		snprintf(veth_host, sizeof(veth_host), "vh%d-%d", (int)child_pid, idx);
		snprintf(veth_ctr, sizeof(veth_ctr), "vc%d-%d", (int)child_pid, idx);

		if (rtnl_veth_create(fd, veth_host, veth_ctr) != 0 ||
		    rtnl_link_set_netns_pid(fd, veth_ctr, child_pid) != 0 ||
		    rtnl_link_set_master(fd, veth_host, nets[idx].bridge) != 0 ||
		    rtnl_link_set_up(fd, veth_host) != 0) {
			rtnl_close(fd);
			return -1;
		}

		/* Include the NUL so the child's single read() knows exactly
		 * where the name ends without needing its own delimiter scan. */
		len = strlen(veth_ctr) + 1;
		if ((size_t)write(ready_pipe_write, veth_ctr, len) != len) {
			rtnl_close(fd);
			return -1;
		}
	}

	rtnl_close(fd);
	return 0;
}

/*
 * Loopback, for ANY container with its own network namespace -- whether
 * or not it has a single network attached (#224).
 *
 * A fresh netns starts with lo present but DOWN, so 127.0.0.1 is not
 * assignable. That is invisible until something binds it, and then it
 * fails as "Cannot assign requested address", which reads like a
 * configuration mistake rather than a missing interface.
 *
 * This used to live inside container_net_child_configure(), which
 * container.c calls only when net_count > 0. So a container with no
 * networks -- every package build sandbox, which is deliberately
 * isolated from the network -- had no working loopback either. Measured
 * on a real build container: uid 0, a writable data dir, fork and
 * waitpid all fine, and a bind to 127.0.0.1 refused. That one missing
 * interface is what stopped 67 daemon-linked tests from being runnable
 * in the build gate, since every one of them forks a cixd and talks to
 * it over loopback.
 *
 * Loopback is not network access: it reaches nothing outside the
 * namespace, so a build sandbox stays as isolated from the network as
 * it was. What changes is that it stops being isolated from ITSELF,
 * which no real Linux system is and no software expects to be.
 */
int container_net_child_loopback_up(void)
{
	int fd = rtnl_open();

	if (fd < 0)
		return -1;
	if (rtnl_link_set_up(fd, "lo") != 0) {
		rtnl_close(fd);
		return -1;
	}
	rtnl_close(fd);
	return 0;
}

int container_net_child_configure(const struct network_spec *nets, int net_count,
                                   int ready_pipe_read)
{
	int fd;
	int idx;

	fd = rtnl_open();
	if (fd < 0)
		return -1;

	for (idx = 0; idx < net_count; idx++) {
		char veth_ctr[16];
		char ifname[16];
		ssize_t n;

		n = read(ready_pipe_read, veth_ctr, sizeof(veth_ctr) - 1);
		if (n <= 0) {
			rtnl_close(fd);
			return -1;
		}
		/* Defensive: guarantee termination regardless of whether the
		 * sender's NUL already landed within these n bytes. */
		veth_ctr[n] = '\0';
		snprintf(ifname, sizeof(ifname), "eth%d", idx);

		if (rtnl_link_rename(fd, veth_ctr, ifname) != 0 ||
		    rtnl_addr_add_ipv4(fd, ifname, nets[idx].container_ip_be, nets[idx].prefix_len) != 0 ||
		    rtnl_link_set_up(fd, ifname) != 0) {
			rtnl_close(fd);
			return -1;
		}
	}

	/* nets[0] is "primary": the only attachment that could get a
	 * default route, and only when its network actually has a
	 * host-owned address to route through -- a container on a pure-L2
	 * network gets no default route at all, same as a real host
	 * plugged into a real switch with no DHCP (its image/operator owns
	 * routing entirely, e.g. via an explicit --route=0.0.0.0/0:VIA).
	 * Any other attachment already has its own subnet's connected
	 * route, installed automatically by the address assignment above,
	 * regardless of whether that network has its own address either. */
	if (net_count > 0 && nets[0].has_address &&
	    rtnl_route_add_default_ipv4(fd, nets[0].address_ip_be) != 0) {
		rtnl_close(fd);
		return -1;
	}

	rtnl_close(fd);
	return 0;
}

int container_net_host_attach_interfaces(const char *const *interfaces, int interface_count,
                                          pid_t child_pid, int *out_netns_fd)
{
	char ns_path[64];
	int fd;
	int i;
	pid_t helper;
	int status;

	*out_netns_fd = -1;
	if (interface_count == 0)
		return 0;

	snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/net", (int)child_pid);
	*out_netns_fd = open(ns_path, O_RDONLY);
	if (*out_netns_fd < 0)
		return -1;

	fd = rtnl_open();
	if (fd < 0) {
		close(*out_netns_fd);
		*out_netns_fd = -1;
		return -1;
	}

	for (i = 0; i < interface_count; i++) {
		if (rtnl_link_set_netns_pid(fd, interfaces[i], child_pid) != 0) {
			rtnl_close(fd);
			close(*out_netns_fd);
			*out_netns_fd = -1;
			return -1;
		}
	}
	rtnl_close(fd);

	/*
	 * The kernel administratively downs a link as part of moving it to
	 * a new netns (dev_change_net_namespace()) -- confirmed directly,
	 * not assumed -- so "up" has to happen AFTER the move, from inside
	 * the netns it just landed in, not before. A forked, short-lived
	 * helper does the same setns()-then-fresh-socket dance
	 * container_net_teardown_interfaces() uses for the reverse move,
	 * for the same reason: a netlink socket can only address
	 * interfaces visible in its own netns.
	 */
	helper = fork();
	if (helper < 0) {
		close(*out_netns_fd);
		*out_netns_fd = -1;
		return -1;
	}
	if (helper == 0) {
		int hfd;

		if (setns(*out_netns_fd, CLONE_NEWNET) != 0)
			_exit(1);
		hfd = rtnl_open();
		if (hfd < 0)
			_exit(1);
		for (i = 0; i < interface_count; i++) {
			if (rtnl_link_set_up(hfd, interfaces[i]) != 0) {
				rtnl_close(hfd);
				_exit(1);
			}
		}
		rtnl_close(hfd);
		_exit(0);
	}
	if (waitpid(helper, &status, 0) != helper || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		close(*out_netns_fd);
		*out_netns_fd = -1;
		return -1;
	}

	return 0;
}

int container_net_teardown_interfaces(const char *const *interfaces, int interface_count,
                                       int netns_fd)
{
	pid_t pid;
	int status;

	if (interface_count == 0)
		return 0;

	pid = fork();
	if (pid < 0) {
		close(netns_fd);
		return -1;
	}
	if (pid == 0) {
		/* Throwaway helper: setns() is whole-process, so this whole
		 * dance is isolated here rather than risking the long-lived
		 * daemon's own netns. root_fd must be opened BEFORE setns()
		 * moves this process itself into the container's netns. */
		int root_fd = open("/proc/self/ns/net", O_RDONLY);
		int fd;
		int i;
		int rc = 0;

		if (root_fd < 0 || setns(netns_fd, CLONE_NEWNET) != 0)
			_exit(1);

		/* Created only now, while genuinely inside the container's
		 * netns -- a netlink socket is scoped to whatever netns was
		 * current at its own creation, same rule rtnetlink.h documents
		 * for every other caller. */
		fd = rtnl_open();
		if (fd < 0)
			_exit(1);

		for (i = 0; i < interface_count; i++) {
			if (rtnl_link_set_netns_fd(fd, interfaces[i], root_fd) != 0)
				rc = 1;
		}

		_exit(rc);
	}

	/* Our own copy of netns_fd is no longer needed -- the helper
	 * duplicated it across fork() and holds its own reference for as
	 * long as it needs one. */
	close(netns_fd);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/*
 * ADR-0156/task #861: attaches a network to an ALREADY-RUNNING
 * container (not at clone3()-time -- container_net_host_setup()/
 * container_net_child_configure() above are that path's own pair,
 * cooperating via a ready_pipe the child reads before its own
 * execve()). No such cooperating child exists here, so the daemon
 * does every step itself: create the veth pair, move its container
 * side into child_pid's own netns, attach the host side to the
 * bridge and bring it up (exactly container_net_host_setup()'s own
 * sequence) -- then, since renaming/addressing/bringing up the
 * container-side end can only be done from inside the netns it now
 * lives in, a short-lived forked helper setns()s in and does that
 * part, mirroring container_net_host_attach_interfaces()'s own
 * identical dance for physical interfaces.
 */
int container_net_attach_running(const char *bridge, uint32_t container_ip_be, int prefix_len,
                                  pid_t child_pid, const char *veth_host, const char *veth_ctr,
                                  const char *ifname)
{
	int fd;
	int netns_fd;
	char ns_path[64];
	pid_t helper;
	int status;

	fd = rtnl_open();
	if (fd < 0)
		return -1;

	if (rtnl_veth_create(fd, veth_host, veth_ctr) != 0 ||
	    rtnl_link_set_netns_pid(fd, veth_ctr, child_pid) != 0 ||
	    rtnl_link_set_master(fd, veth_host, bridge) != 0 || rtnl_link_set_up(fd, veth_host) != 0) {
		rtnl_close(fd);
		return -1;
	}
	rtnl_close(fd);

	snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/net", (int)child_pid);
	netns_fd = open(ns_path, O_RDONLY);
	if (netns_fd < 0)
		return -1;

	helper = fork();
	if (helper < 0) {
		close(netns_fd);
		return -1;
	}
	if (helper == 0) {
		int hfd;

		if (setns(netns_fd, CLONE_NEWNET) != 0)
			_exit(1);
		hfd = rtnl_open();
		if (hfd < 0)
			_exit(1);
		if (rtnl_link_rename(hfd, veth_ctr, ifname) != 0 ||
		    rtnl_addr_add_ipv4(hfd, ifname, container_ip_be, prefix_len) != 0 ||
		    rtnl_link_set_up(hfd, ifname) != 0) {
			rtnl_close(hfd);
			_exit(1);
		}
		rtnl_close(hfd);
		_exit(0);
	}
	close(netns_fd);
	if (waitpid(helper, &status, 0) != helper || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

/*
 * The reverse of the above -- deliberately simple: veth_host is
 * always the HOST-side end of the pair, still living in the daemon's
 * own default netns, so no setns() dance is needed at all here.
 * Deleting either end of a veth pair deletes both (confirmed kernel
 * behavior, already relied on implicitly everywhere a container's own
 * netns teardown already frees its create-time veths) -- so this one
 * rtnl_link_delete() also removes the container-side end from inside
 * the still-running container's own netns, with no need to enter it.
 */
int container_net_detach_running(const char *veth_host)
{
	int fd;
	int rc;

	fd = rtnl_open();
	if (fd < 0)
		return -1;
	rc = rtnl_link_delete(fd, veth_host);
	rtnl_close(fd);
	return rc;
}

int container_net_install_routes(const struct route_spec *routes, int route_count)
{
	int fd;
	int i;

	if (route_count == 0)
		return 0;

	fd = rtnl_open();
	if (fd < 0)
		return -1;

	for (i = 0; i < route_count; i++) {
		if (rtnl_route_add_ipv4(fd, routes[i].dest_be, routes[i].dest_prefix_len,
		                         routes[i].gateway_be) != 0) {
			rtnl_close(fd);
			return -1;
		}
	}

	rtnl_close(fd);
	return 0;
}

int container_net_enable_ip_forward(void)
{
	int fd;
	static const char one[] = "1\n";

	fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
	if (fd < 0)
		return -1;
	if (write(fd, one, sizeof(one) - 1) != (ssize_t)(sizeof(one) - 1)) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

int container_net_apply_sysctl(const char *key, const char *value)
{
	char path[16 + CONTAINER_SYSCTL_KEY_MAX];
	char *p;
	int fd;
	size_t len;

	if (snprintf(path, sizeof(path), "/proc/sys/%s", key) >= (int)sizeof(path))
		return -1;
	for (p = path; *p != '\0'; p++) {
		if (*p == '.')
			*p = '/';
	}

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	len = strlen(value);
	if (write(fd, value, len) != (ssize_t)len) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}
