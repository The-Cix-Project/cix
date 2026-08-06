#ifndef NETWORK_H
#define NETWORK_H

#include "json.h"

#include <stdint.h>

#define NETWORK_MAX 64
#define NETWORK_NAME_MAX 16 /* IFNAMSIZ -- a network's name IS its bridge's ifname */

/*
 * A generous fixed bound, same "host hardware, no natural daemon-side
 * ceiling" reasoning CONTAINER_MAX_INTERFACES (include/container.h)
 * already established -- enough physical uplinks/VLAN trunks for one
 * network's bridge to plausibly need.
 */
#define NETWORK_MAX_INTERFACES 8
#define NETWORK_IFNAME_MAX 16 /* IFNAMSIZ, same bare-literal precedent as NETWORK_NAME_MAX */

enum network_error {
	NETWORK_OK = 0,
	NETWORK_ERR_INVALID_NAME,
	NETWORK_ERR_INVALID_SUBNET,
	NETWORK_ERR_INVALID_GATEWAY, /* gateway_str out of subnet, or the network/broadcast address */
	NETWORK_ERR_DUPLICATE,
	NETWORK_ERR_OVERLAP,
	NETWORK_ERR_FULL,
	NETWORK_ERR_CREATE_FAILED,
	NETWORK_ERR_NOT_FOUND,
	NETWORK_ERR_IN_USE,
	NETWORK_ERR_DELETE_FAILED,
	NETWORK_ERR_IP_OUT_OF_RANGE, /* not this network's subnet, or its reserved gateway/network address */
	NETWORK_ERR_IP_TAKEN,       /* a valid, in-range address, but already assigned to a running container */
	NETWORK_ERR_INTERFACE_NOT_FOUND,   /* not a known, currently-assignable "net:" device */
	NETWORK_ERR_INTERFACE_ATTACHED,    /* already attached to this network */
	NETWORK_ERR_INTERFACE_NOT_ATTACHED, /* detach requested for one that isn't attached here */
	NETWORK_ERR_INTERFACE_FULL,        /* this network's own interfaces[] table is full */
	NETWORK_ERR_INTERFACE_NAME_TOO_LONG, /* "<ifname>.<vlan_id>" wouldn't fit IFNAMSIZ */
	NETWORK_ERR_IS_MANAGEMENT /* refused: this network currently carries kanxeod's own
	                            * bind address -- see network_set_management() */
};

struct network_attached_interface {
	char ifname[NETWORK_IFNAME_MAX]; /* the real interface the operator named -- for
	                                   * a VLAN attach, this is the PARENT link, not
	                                   * the derived "<ifname>.<vlan_id>" sub-interface
	                                   * that's actually enslaved to the bridge */
	int vlan_id;                     /* 0: ifname itself is enslaved directly (untagged) */
};

struct network_def {
	char name[NETWORK_NAME_MAX];
	uint32_t base_be; /* network address, network byte order */
	int prefix_len;
	int has_gateway;     /* 0: bridge is pure L2, no host-owned address at all */
	uint32_t gateway_be; /* only meaningful when has_gateway -- operator-chosen
	                       * (network create --gateway=), no longer implicitly
	                       * base|1 */
	struct network_attached_interface interfaces[NETWORK_MAX_INTERFACES];
	int interface_count;
	int in_use;         /* 0 for free slots */
	int is_management; /* this network's gateway is kanxeod's own bind address --
	                     * see network_set_management(); at most one network has
	                     * this set at a time */
};

/*
 * Loads state_path (the persisted network list, if it exists) and
 * recreates each network's bridge idempotently (EEXIST tolerated on
 * both the bridge and its gateway address, same spirit as the
 * daemon's ensure_dir()) -- a bridge created via network_create() is
 * real, persistent kernel state that outlives this process, so a
 * restart must not forget about it. state_path is remembered for
 * subsequent network_create()/network_delete() calls to rewrite.
 * Call once at daemon startup, before serving any request.
 */
int network_init(const char *state_path);

/*
 * Validates name (1-15 chars, [A-Za-z0-9_-], unique), that subnet_str
 * parses as an IPv4 network address whose host bits are actually zero
 * for prefix_len (e.g. "172.31.0.5" with prefix_len 24 is rejected --
 * only "172.31.0.0" is valid), that prefix_len is in [8,30], and that
 * the resulting range doesn't overlap any existing network.
 *
 * gateway_str is optional (NULL/empty -- the default): the bridge is
 * created with no host-owned IP address at all, purely L2. A network
 * whose own routing is owned by container(s) attached to it (a router
 * pair running a routing protocol, a shared VRRP address, etc.) wants
 * this -- the host has no business holding an address on a segment it
 * isn't routing for. When non-NULL, gateway_str must be a valid IPv4
 * address within this subnet (not the network or broadcast address,
 * not already reserved) and is assigned to the bridge device itself,
 * the same "gateway lives on the bridge, never a port" convention
 * ADR-0011 already established -- just operator-chosen now instead of
 * hardcoded to base|1.
 *
 * On success creates the bridge (rtnetlink), assigns the gateway
 * address if requested, appends to the in-memory table, and
 * atomically rewrites the persisted file (write to a temp file,
 * fsync, rename over the real one -- a crash mid-write must never
 * corrupt this state). *out points at the stored entry on
 * NETWORK_OK.
 */
enum network_error network_create(const char *name, const char *subnet_str, int prefix_len,
                                   const char *gateway_str, struct network_def **out);

/*
 * Removes name. Returns NETWORK_ERR_IN_USE (not deleted) if any
 * registry entry currently reports this network as its own -- see
 * registry_network_in_use(). Otherwise deletes the bridge, removes
 * the entry, rewrites the persisted file.
 */
enum network_error network_delete(const char *name);

struct network_def *network_find(const char *name);

/*
 * network_find(name) plus IP allocation within it: delegates to the
 * existing, topology-agnostic registry_alloc_ip(), same as every
 * container's IP has been allocated since Phase 6 part 3. Returns 0
 * and fills *out_ip_be, or -1 (no such network, or the range is
 * exhausted).
 */
int network_alloc_ip(const char *name, uint32_t *out_ip_be);

/*
 * Validates an explicit, operator-chosen IP for network name: must
 * fall within this network's own subnet (its masked base, not just
 * any address), must not be the reserved gateway address (only
 * reserved at all when the network has_gateway -- a gateway-less
 * network reserves nothing beyond the network/broadcast addresses
 * themselves) or the network address itself, must fall within the
 * same usable host range network_alloc_ip() itself allocates from,
 * and must not already be assigned to a running container's network
 * attachment (registry_ip_available()). NETWORK_ERR_NOT_FOUND if no
 * such network.
 */
enum network_error network_ip_available(const char *name, uint32_t ip_be);

void network_write_json_one(const struct network_def *net, struct json_writer *w);
void network_write_json_list(struct json_writer *w);

/*
 * Attaches a real host network interface to name's bridge -- the
 * "physical ethernet on a host-managed switch" half of what a
 * gateway-less network needs to be useful for more than veth-attached
 * containers alone. ifname must currently be a known, assignable
 * "net:<ifname>" device (device_find(), daemon/src/device.c) -- not
 * already moved into a container's netns (ADR-0022) and not already
 * enslaved to anything (including a previous call here), the same
 * sysfs-visibility-is-exclusivity rule that path already established,
 * now extended to bridge enslavement instead of a netns move.
 *
 * vlan_id 0 enslaves ifname directly (untagged/access mode -- the
 * whole link belongs to this one network). A nonzero vlan_id instead
 * creates a new "<ifname>.<vlan_id>" 802.1q sub-interface and enslaves
 * *that* -- ifname itself is untouched and stays free to be attached
 * (with a different vlan_id) to other networks. Chosen over bridge
 * VLAN filtering because it needs no new per-port-tag concept this
 * project's one-bridge-per-network model doesn't already have.
 *
 * Persisted as part of name's own entry, re-applied idempotently by
 * network_init() on every daemon restart, same as the bridge/gateway
 * themselves already are.
 */
enum network_error network_attach_interface(const char *name, const char *ifname, int vlan_id);

/*
 * Reverses network_attach_interface(): releases ifname from name's
 * bridge (rtnl_link_clear_master()) or, for a VLAN attach, deletes the
 * "<ifname>.<vlan_id>" sub-interface entirely (rtnl_link_delete()) --
 * ifname itself was never touched by the VLAN case, so there's nothing
 * to release on it directly. Simpler than ADR-0022's container-
 * passthrough teardown: the interface never left the root netns, so
 * none of that ADR's netns-fd/kernel-fallback-name hazards apply here.
 */
enum network_error network_detach_interface(const char *name, const char *ifname);

/*
 * Designates name as the one network whose gateway address kanxeod
 * itself binds to -- the API-managed counterpart to what used to be a
 * GRUB-only, invisible apply_static_ip() call (Part 0.5). Requires
 * name to already have a gateway (has_gateway, network_create()'s
 * gateway_str) -- that address becomes the daemon's own bind address.
 * Clears is_management from whichever other network previously held
 * it (at most one at a time) and sets it on name, in that order, then
 * persists -- never a window with zero or two management networks.
 * Does *not* itself perform the listen-socket rebind; that's the
 * caller's job once this returns NETWORK_OK (see daemon_config.c).
 * NETWORK_ERR_NOT_FOUND if no such network; NETWORK_ERR_INVALID_
 * GATEWAY if it has no gateway address to bind to.
 */
enum network_error network_set_management(const char *name);

/*
 * The network currently designated by network_set_management(), or
 * NULL if none has been set yet (e.g. a --data-dir= test invocation
 * with no boot-time bootstrap). At most one entry ever has
 * is_management set -- this is a linear scan, not a cached pointer,
 * since g_networks[] can be reloaded wholesale by network_init().
 */
struct network_def *network_find_management(void);

#endif /* NETWORK_H */
