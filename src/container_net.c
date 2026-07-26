#include "internal.h"
#include "rtnetlink.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
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

	if (rtnl_link_set_up(fd, "lo") != 0) {
		rtnl_close(fd);
		return -1;
	}
	/* nets[0] is "primary": the only attachment that gets the default
	 * route. Any others already have their subnet's connected route,
	 * installed automatically by the address assignment above. */
	if (net_count > 0 && rtnl_route_add_default_ipv4(fd, nets[0].gateway_ip_be) != 0) {
		rtnl_close(fd);
		return -1;
	}

	rtnl_close(fd);
	return 0;
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
