#ifndef RTNETLINK_H
#define RTNETLINK_H

#include <stdint.h>
#include <sys/types.h>

/*
 * The networking plane's control channel to the kernel: everything
 * here talks to the kernel via NETLINK_ROUTE directly (raw netlink
 * messages we build and parse by hand) -- never `ip`/iproute2, never
 * a virtual-switch daemon, never eBPF. Actual packet forwarding is
 * still the kernel's own (bridge, veth, routing table); "custom"
 * refers to this control plane, not a userspace reimplementation of
 * L2 switching.
 *
 * IPv4 addresses throughout are network byte order (e.g. straight
 * from inet_pton()/inet_addr()) -- never host-order values that still
 * need htonl().
 *
 * A netlink socket is scoped to whatever network namespace was
 * current when rtnl_open() created it; to configure a link that now
 * lives in a different netns (e.g. after rtnl_link_set_netns_pid()),
 * open a new socket from within that namespace.
 */

int rtnl_open(void);
void rtnl_close(int fd);

/* Creates a bridge device. Fails (EEXIST-style) if it already exists. */
int rtnl_bridge_create(int fd, const char *name);

/* Creates a veth pair, name <-> peer_name, both ending up in the
 * caller's current netns. */
int rtnl_veth_create(int fd, const char *name, const char *peer_name);

/* Moves the named link into the network namespace of process pid. */
int rtnl_link_set_netns_pid(int fd, const char *name, pid_t pid);

/* Attaches (enslaves) the named link to a bridge. */
int rtnl_link_set_master(int fd, const char *name, const char *bridge_name);

/* Brings a link up (IFF_UP). */
int rtnl_link_set_up(int fd, const char *name);

/* Assigns an IPv4 address/prefix to a link. */
int rtnl_addr_add_ipv4(int fd, const char *link_name, uint32_t addr_be, int prefix_len);

/* Adds a default (0.0.0.0/0) IPv4 route via gateway_be, or a direct
 * (gateway-less) default route if gateway_be is 0. */
int rtnl_route_add_default_ipv4(int fd, uint32_t gateway_be);

/* Deletes the named link. Deleting either end of a veth pair removes
 * both, kernel-side. */
int rtnl_link_delete(int fd, const char *name);

/* Renames a link. Identifies the target by resolving old_name to an
 * ifindex first (via if_nametoindex()), unlike every other operation
 * here -- IFLA_IFNAME as an attribute means "set this as the new
 * name," so it can't double as the identifier when renaming. */
int rtnl_link_rename(int fd, const char *old_name, const char *new_name);

#endif /* RTNETLINK_H */
