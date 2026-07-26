#include "internal.h"
#include "rtnetlink.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int container_net_host_setup(const struct network_spec *net, pid_t child_pid, int ready_pipe_write)
{
	int fd;
	char veth_host[16];
	char veth_ctr[16];
	size_t len;

	snprintf(veth_host, sizeof(veth_host), "vh%d", (int)child_pid);
	snprintf(veth_ctr, sizeof(veth_ctr), "vc%d", (int)child_pid);

	fd = rtnl_open();
	if (fd < 0)
		return -1;

	if (rtnl_veth_create(fd, veth_host, veth_ctr) != 0) {
		rtnl_close(fd);
		return -1;
	}
	if (rtnl_link_set_netns_pid(fd, veth_ctr, child_pid) != 0) {
		rtnl_close(fd);
		return -1;
	}
	if (rtnl_link_set_master(fd, veth_host, net->bridge) != 0) {
		rtnl_close(fd);
		return -1;
	}
	if (rtnl_link_set_up(fd, veth_host) != 0) {
		rtnl_close(fd);
		return -1;
	}
	rtnl_close(fd);

	/* Include the NUL so the child's single read() knows exactly
	 * where the name ends without needing its own delimiter scan. */
	len = strlen(veth_ctr) + 1;
	if ((size_t)write(ready_pipe_write, veth_ctr, len) != len)
		return -1;

	return 0;
}

int container_net_child_configure(const struct network_spec *net, int ready_pipe_read)
{
	char veth_ctr[16];
	ssize_t n;
	int fd;

	n = read(ready_pipe_read, veth_ctr, sizeof(veth_ctr) - 1);
	if (n <= 0)
		return -1;
	/* Defensive: guarantee termination regardless of whether the
	 * sender's NUL already landed within these n bytes. */
	veth_ctr[n] = '\0';

	fd = rtnl_open();
	if (fd < 0)
		return -1;

	if (rtnl_link_rename(fd, veth_ctr, "eth0") != 0) {
		rtnl_close(fd);
		return -1;
	}
	if (rtnl_addr_add_ipv4(fd, "eth0", net->container_ip_be, net->prefix_len) != 0) {
		rtnl_close(fd);
		return -1;
	}
	if (rtnl_link_set_up(fd, "eth0") != 0) {
		rtnl_close(fd);
		return -1;
	}
	if (rtnl_link_set_up(fd, "lo") != 0) {
		rtnl_close(fd);
		return -1;
	}
	if (rtnl_route_add_default_ipv4(fd, net->gateway_ip_be) != 0) {
		rtnl_close(fd);
		return -1;
	}

	rtnl_close(fd);
	return 0;
}
