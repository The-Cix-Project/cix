#include "internal.h"
#include "nl80211.h"
#include "rtnetlink.h"

#include <errno.h>
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
		/* ADR-0259: the operator's chosen name when there is one, and
		 * the positional eth<idx> when there is not. */
		if (nets[idx].ifname[0] != '\0')
			snprintf(ifname, sizeof(ifname), "%s", nets[idx].ifname);
		else
			snprintf(ifname, sizeof(ifname), "eth%d", idx);

		if (rtnl_link_rename(fd, veth_ctr, ifname) != 0) {
			rtnl_close(fd);
			return -1;
		}

		if (nets[idx].container_bridge[0] != '\0') {
			/*
			 * This attachment is a PORT of a bridge inside the
			 * container (#30, ADR-0264), not an addressed interface.
			 *
			 * EEXIST is success, not tolerance for its own sake: the
			 * netns is fresh, so the only way the bridge already
			 * exists is that an earlier attachment in this same loop
			 * named it, which is exactly the "several ports, one
			 * bridge" case this supports. Any other errno is a real
			 * failure and still fails the container.
			 *
			 * The address goes on the bridge, never on the port -- a
			 * bridge port with an address of its own does not receive
			 * on it. Zero means no address at all, which is a pure
			 * port and the ordinary case for an access point's own
			 * wireless-side attachment.
			 */
			const char *br = nets[idx].container_bridge;

			if (rtnl_bridge_create(fd, br) != 0 && errno != EEXIST) {
				rtnl_close(fd);
				return -1;
			}
			if (rtnl_link_set_master(fd, ifname, br) != 0 ||
			    rtnl_link_set_up(fd, ifname) != 0) {
				rtnl_close(fd);
				return -1;
			}
			if (nets[idx].container_ip_be != 0 &&
			    rtnl_addr_add_ipv4(fd, br, nets[idx].container_ip_be,
			                        nets[idx].prefix_len) != 0) {
				rtnl_close(fd);
				return -1;
			}
			if (rtnl_link_set_up(fd, br) != 0) {
				rtnl_close(fd);
				return -1;
			}
			continue;
		}

		if (rtnl_addr_add_ipv4(fd, ifname, nets[idx].container_ip_be, nets[idx].prefix_len) != 0 ||
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
		/*
		 * A radio does not move the way a netdev does (#341).
		 *
		 * rtnl_link_set_netns_pid() on a wireless interface returns
		 * EINVAL: the netdev belongs to a wiphy, one wiphy can own
		 * several netdevs, and the kernel refuses to let one of them
		 * leave alone. NL80211_CMD_SET_WIPHY_NETNS moves the whole PHY
		 * instead, which is what an access point needs -- hostapd
		 * drives the radio, so the radio has to be where hostapd is.
		 *
		 * Everything on that wiphy goes with it and the host loses the
		 * radio until this container is deleted. That is the kernel's
		 * model, not a choice made here.
		 */
		int wireless = nl80211_is_wireless(interfaces[i]);
		int rc;

		/*
		 * A wiphy will not move while its interfaces are up. nl80211.h
		 * says so in as many words -- "all devices associated with
		 * this wiphy must be down and will follow" -- and skipping it
		 * is why the move failed with a kernel errno that named no
		 * cause anyone could act on.
		 *
		 * Down first, then move. The interface comes back up inside
		 * the container by the helper below, which this path already
		 * did for the rtnetlink case: the kernel downs a link as part
		 * of moving it either way, so "up" always belonged after the
		 * move rather than before it.
		 *
		 * Only for the wireless path. An ordinary netdev needs no
		 * such preparation, and downing one that the operator handed
		 * over already-configured would be a change this code has no
		 * reason to make.
		 */
		if (wireless && rtnl_link_set_down(fd, interfaces[i]) != 0) {
			char step[128];

			snprintf(step, sizeof(step),
			         "container_create: radio \"%s\": could not be brought down before the "
			         "phy move",
			         interfaces[i]);
			container_set_last_error_step(step);
			rtnl_close(fd);
			close(*out_netns_fd);
			*out_netns_fd = -1;
			return -1;
		}

		rc = wireless ? nl80211_move_phy_to_netns_fd(interfaces[i], *out_netns_fd)
		              : rtnl_link_set_netns_pid(fd, interfaces[i], child_pid);

		if (rc != 0) {
			char step[128];

			/*
			 * Name the mechanism AND the interface. "attach
			 * interfaces failed" plus an errno leaves an operator
			 * guessing which of two netlink families refused and
			 * which of several interfaces it was about -- and the two
			 * families fail for entirely different reasons, so the
			 * distinction is the first thing worth knowing.
			 */
			snprintf(step, sizeof(step), "container_create: %s \"%s\": %s netns move",
			         wireless ? "radio" : "interface", interfaces[i],
			         wireless ? "nl80211 phy" : "rtnetlink");
			container_set_last_error_step(step);
			/* Preserve the errno of the move itself: rtnl_close() and
			 * close() below can both overwrite it, and which of the two
			 * mechanisms refused is the whole diagnosis. */
			int e = errno;

			rtnl_close(fd);
			close(*out_netns_fd);
			*out_netns_fd = -1;
			errno = e;
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

		/*
		 * Exit with the REAL errno of whatever failed, clamped into an
		 * exit status. The parent cannot see this child's errno any
		 * other way, and it used to _exit(1) for all three failures --
		 * so the parent returned -1 with whatever errno happened to be
		 * lying around from earlier in container_create(), typically a
		 * long-ignored EEXIST from a mkdir. That reported "File exists"
		 * for a failure that had nothing to do with a file, and cost a
		 * full deploy cycle to see through.
		 */
		if (setns(*out_netns_fd, CLONE_NEWNET) != 0)
			_exit(errno > 0 && errno < 256 ? errno : EIO);
		hfd = rtnl_open();
		if (hfd < 0)
			_exit(errno > 0 && errno < 256 ? errno : EIO);
		for (i = 0; i < interface_count; i++) {
			if (rtnl_link_set_up(hfd, interfaces[i]) != 0) {
				int e = errno;

				rtnl_close(hfd);
				_exit(e > 0 && e < 256 ? e : EIO);
			}
		}
		rtnl_close(hfd);
		_exit(0);
	}
	if (waitpid(helper, &status, 0) != helper) {
		close(*out_netns_fd);
		*out_netns_fd = -1;
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		/* The child's own errno, not this process's stale one. */
		int e = WIFEXITED(status) ? WEXITSTATUS(status) : EIO;

		close(*out_netns_fd);
		*out_netns_fd = -1;
		errno = e > 0 ? e : EIO;
		/* The move itself already succeeded if we are here, so this
		 * is specifically the inside-the-namespace half: setns, a
		 * netlink socket in that namespace, or bringing a link up. */
		container_set_last_error_step(
		        "container_create: bringing moved interfaces up inside the container netns");
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
			/*
			 * Same split as the inbound move: a wiphy goes back by
			 * nl80211, everything else by rtnetlink. Checked from
			 * inside the container, where the interface now lives --
			 * /sys/class/net is namespaced, so this reads the right
			 * one.
			 */
			int one = nl80211_is_wireless(interfaces[i])
			                  ? nl80211_move_phy_to_netns_fd(interfaces[i], root_fd)
			                  : rtnl_link_set_netns_fd(fd, interfaces[i], root_fd);

			if (one != 0)
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
