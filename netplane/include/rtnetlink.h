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

/*
 * Moves the named link into the network namespace referenced by an
 * open file descriptor (e.g. an fd opened on /proc/<pid>/ns/net) --
 * unlike rtnl_link_set_netns_pid(), this still works once the
 * original process that fd was opened against has already exited, as
 * long as the fd itself (or anything else, e.g. the process still
 * being alive) keeps that namespace from being torn down. The link
 * must already be visible in fd's own current netns -- exactly the
 * same requirement rtnl_open() itself documents.
 */
int rtnl_link_set_netns_fd(int fd, const char *name, int target_netns_fd);

/* Attaches (enslaves) the named link to a bridge. Generic -- works for
 * any already-existing link by name, not just veth ports; this is
 * also how a real physical NIC gets enslaved to a Kanxeo-managed
 * bridge (network_attach_interface(), daemon/src/network.c). */
int rtnl_link_set_master(int fd, const char *name, const char *bridge_name);

/* Releases whatever bridge (or other master) currently owns the named
 * link -- the reverse of rtnl_link_set_master(). */
int rtnl_link_clear_master(int fd, const char *name);

/* Creates an 802.1q VLAN sub-interface named `name`, tagged vlan_id,
 * carried over parent_ifname (a real link, physical or otherwise,
 * that must already exist and be visible in the caller's current
 * netns). The sub-interface itself still needs rtnl_link_set_up() and
 * (usually) rtnl_link_set_master() to actually carry traffic anywhere
 * -- this call only creates it. */
int rtnl_vlan_create(int fd, const char *name, const char *parent_ifname, int vlan_id);

/* Brings a link up (IFF_UP). */
int rtnl_link_set_up(int fd, const char *name);

/* Assigns an IPv4 address/prefix to a link. */
int rtnl_addr_add_ipv4(int fd, const char *link_name, uint32_t addr_be, int prefix_len);

/* Adds an IPv4 route to dest_be/dest_prefix_len via gateway_be (0 for
 * a direct/gateway-less route). dest_prefix_len == 0 means the
 * default route, and dest_be is ignored (no RTA_DST attribute). */
int rtnl_route_add_ipv4(int fd, uint32_t dest_be, int dest_prefix_len, uint32_t gateway_be);

/* rtnl_route_add_ipv4(fd, 0, 0, gateway_be) -- kept as its own name
 * since "the default route" is the common case every container gets. */
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
