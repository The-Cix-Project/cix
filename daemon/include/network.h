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
	NETWORK_ERR_INVALID_ADDRESS, /* address_str out of subnet, or the network/broadcast address */
	NETWORK_ERR_DUPLICATE,
	NETWORK_ERR_OVERLAP,
	NETWORK_ERR_FULL,
	NETWORK_ERR_CREATE_FAILED,
	NETWORK_ERR_NOT_FOUND,
	NETWORK_ERR_IN_USE,
	NETWORK_ERR_DELETE_FAILED,
	NETWORK_ERR_IP_OUT_OF_RANGE, /* not this network's subnet, or its reserved address/network address */
	NETWORK_ERR_IP_TAKEN,       /* a valid, in-range address, but already assigned to a running container */
	NETWORK_ERR_INTERFACE_NOT_FOUND,   /* not a known, currently-assignable "net:" device */
	NETWORK_ERR_INTERFACE_ATTACHED,    /* already attached to this network */
	NETWORK_ERR_INTERFACE_NOT_ATTACHED, /* detach requested for one that isn't attached here */
	NETWORK_ERR_INTERFACE_FULL,        /* this network's own interfaces[] table is full */
	NETWORK_ERR_INTERFACE_NAME_TOO_LONG, /* "<ifname>.<vlan_id>" wouldn't fit IFNAMSIZ */
	NETWORK_ERR_IS_MANAGEMENT /* refused: this network currently carries cixd's own
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
	int has_address;     /* 0: bridge is pure L2, no host-owned address at all */
	uint32_t address_be; /* only meaningful when has_address -- operator-chosen
	                       * (network create --address=), no longer implicitly
	                       * base|1 */
	struct network_attached_interface interfaces[NETWORK_MAX_INTERFACES];
	int interface_count;
	int in_use;         /* 0 for free slots */
	int is_management; /* this network's address is cixd's own bind address --
	                     * see network_set_management(); at most one network has
	                     * this set at a time */
	/*
	 * Issue #70: the host-part window auto-allocation draws from
	 * (network_alloc_ip()). 0 = unset (the full [1, host_max] default,
	 * unchanged). An explicitly-requested IP is NOT constrained by
	 * this -- only auto-allocation is, so an operator can still pin a
	 * container anywhere in-subnet. Exists because a LAN-bridged
	 * network shares real external hosts (a router at .1, a switch, a
	 * NAS...) that the allocator must not blindly hand out; a
	 * management network additionally gets a safe default floor of
	 * host-part 2 even when this is unset, skipping the near-universal
	 * .1 gateway convention (see network_alloc_ip()).
	 */
	int alloc_start_host; /* first usable host-part; 0 = unset */
	int alloc_end_host;   /* last usable host-part;  0 = unset */
};

/*
 * Loads state_path (the persisted network list, if it exists) and
 * recreates each network's bridge idempotently (EEXIST tolerated on
 * both the bridge and its own address, same spirit as the
 * daemon's ensure_dir()) -- a bridge created via network_create() is
 * real, persistent kernel state that outlives this process, so a
 * restart must not forget about it. state_path is remembered for
 * subsequent network_create()/network_delete() calls to rewrite.
 * Call once at daemon startup, before serving any request.
 */
int network_init(const char *state_path);

/*
 * ADR-0141 Phase 2: repoints where future save_state() calls write to,
 * for a live state-storage migration -- deliberately NOT a second
 * network_init() call (which would re-open a fresh rtnl socket and
 * reload g_networks[] from whatever's at the new path, discarding this
 * process's already-live in-memory bridge state). The migration job's
 * own bulk copy has already placed a correct, current copy of the old
 * state_path's content at new_state_path before this is ever called --
 * this only changes where the *next* save goes.
 */
void network_repoint(const char *new_state_path);

/*
 * Validates name (1-15 chars, [A-Za-z0-9_-], unique), that subnet_str
 * parses as an IPv4 network address whose host bits are actually zero
 * for prefix_len (e.g. "172.31.0.5" with prefix_len 24 is rejected --
 * only "172.31.0.0" is valid), that prefix_len is in [8,30], and that
 * the resulting range doesn't overlap any existing network.
 *
 * address_str is optional (NULL/empty -- the default): the bridge is
 * created with no host-owned IP address at all, purely L2. A network
 * whose own routing is owned by container(s) attached to it (a router
 * pair running a routing protocol, a shared VRRP address, etc.) wants
 * this -- the host has no business holding an address on a segment it
 * isn't routing for. When non-NULL, address_str must be a valid IPv4
 * address within this subnet (not the network or broadcast address,
 * not already reserved) and is assigned to the bridge device itself,
 * the same "this address lives on the bridge, never a port" convention
 * ADR-0011 already established -- just operator-chosen now instead of
 * hardcoded to base|1.
 *
 * On success creates the bridge (rtnetlink), assigns the address if
 * requested, appends to the in-memory table, and atomically rewrites
 * the persisted file (write to a temp file, fsync, rename over the
 * real one -- a crash mid-write must never corrupt this state). *out
 * points at the stored entry on NETWORK_OK.
 */
/*
 * Issue #137: change an existing network's auto-allocation window.
 *
 * Only the window, deliberately. Subnet, prefix and host address define
 * the network's identity and its real bridge; changing them under
 * running containers would invalidate addresses already handed out. The
 * window constrains only FUTURE auto-allocation, so it is safe to
 * narrow or widen with containers attached.
 *
 * Exists because the window was previously settable only at creation,
 * which put it out of reach on the network that needs it most: the
 * management network cannot be deleted while it is the management
 * network, so "delete and recreate with a pool" was never a route.
 *
 * NULL for either string leaves that bound unchanged; the empty string
 * clears it. Clearing both on a bridged network returns it to failing
 * closed, which is the safe end state, not a regression.
 */
enum network_error network_set_alloc_window(const char *name, const char *alloc_start_str,
                                             const char *alloc_end_str);

enum network_error network_create(const char *name, const char *subnet_str, int prefix_len,
                                   const char *address_str, const char *alloc_start_str,
                                   const char *alloc_end_str, struct network_def **out);

/*
 * Removes name. Returns NETWORK_ERR_IN_USE (not deleted) if any
 * registry entry currently reports this network as its own -- see
 * registry_network_in_use(). Otherwise deletes the bridge, removes
 * the entry, rewrites the persisted file.
 */
enum network_error network_delete(const char *name);

/*
 * Re-address an existing network in place -- new subnet/prefix/address,
 * same name, same bridge. Backs PUT /v1/system/management-network.
 *
 * Adds the new address to the bridge and persists, but deliberately
 * leaves the OLD address live and hands it back through out_old_* --
 * the caller must defer removing it, because dropping it under the
 * connection carrying the reply strands that client (ADR-0068). Both
 * addresses being briefly present is what makes the handover safe.
 * out_old_address_be is 0 when there is nothing to clean up (the
 * network had no address, or this was a no-op).
 *
 * Resets the allocation window when the subnet changes; a window is a
 * pair of host-parts within one subnet, meaningless in another.
 * Knows nothing of containers -- refusing a subnet change that would
 * strand attached containers is the caller's job.
 */
enum network_error network_set_address(const char *name, const char *subnet_str, int prefix_len,
                                        const char *address_str, uint32_t *out_old_address_be,
                                        int *out_old_prefix_len);

struct network_def *network_find(const char *name);

/*
 * network_find(name) plus IP allocation within it: delegates to the
 * existing, topology-agnostic registry_alloc_ip(), same as every
 * container's IP has been allocated since Phase 6 part 3. Returns 0
 * and fills *out_ip_be, or -1 (no such network, or the range is
 * exhausted).
 */
/*
 * Auto-allocates a free address. Returns 0 on success, -1 if there is
 * no such network or the range is exhausted, and -2 (issue #137) when
 * this network is bridged onto real infrastructure (a physical
 * interface is enslaved) and has no declared allocation pool -- a
 * deliberate refusal, not a failure: auto-allocating into a subnet
 * shared with equipment this platform does not own is how a container
 * ends up answering on someone's production LAN. Declare a pool, or
 * pin the address per container (an explicit address is never
 * constrained by the pool).
 */
int network_alloc_ip(const char *name, uint32_t *out_ip_be);

/*
 * Validates an explicit, operator-chosen IP for network name: must
 * fall within this network's own subnet (its masked base, not just
 * any address), must not be the reserved address (only reserved at
 * all when the network has_address -- an address-less network
 * reserves nothing beyond the network/broadcast addresses
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
 * Validates address_str as a real, usable IPv4 address within net's
 * own subnet (not the network or broadcast address) -- the same check
 * network_create()'s own address_str argument and network_ip_
 * available() already apply, exposed here so a second, unrelated
 * address on the same subnet (ADR-0068's dedicated daemon bind_ip)
 * can be validated against the identical rule instead of a
 * hand-rolled duplicate. Does not check for collision with any
 * already-assigned address (registry_ip_available()) -- callers that
 * need that guarantee too must still check it themselves. On success
 * writes the parsed address (network byte order) to *out_address_be.
 */
int network_address_str_is_valid(const struct network_def *net, const char *address_str,
                                  uint32_t *out_address_be);

/* ADR-0066: a real, read-only view of the box's own kernel IPv4
 * routing table, via rtnl_route_dump_ipv4() -- the only way to ever
 * inspect a running Cix install's actual routing state (ADR-0034,
 * no SSH/general shell). Returns -1 (nothing written to w) only on a
 * genuine rtnetlink transport/parse failure. */
int network_write_routes_json(struct json_writer *w);

/*
 * Attaches a real host network interface to name's bridge -- the
 * "physical ethernet on a host-managed switch" half of what an
 * address-less network needs to be useful for more than veth-attached
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
 * network_init() on every daemon restart, same as the bridge/address
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
 * Designates name as the one network whose own address cixd itself
 * binds to -- the API-managed counterpart to what used to be a
 * GRUB-only, invisible apply_static_ip() call (Part 0.5). Requires
 * name to already have an address (has_address, network_create()'s
 * address_str) -- that address becomes the daemon's own bind address.
 * Clears is_management from whichever other network previously held
 * it (at most one at a time) and sets it on name, in that order, then
 * persists -- never a window with zero or two management networks.
 * Does *not* itself perform the listen-socket rebind; that's the
 * caller's job once this returns NETWORK_OK (see daemon_config.c).
 * NETWORK_ERR_NOT_FOUND if no such network; NETWORK_ERR_INVALID_
 * ADDRESS if it has no address to bind to.
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

/*
 * Two network utilities that were static in main.c and needed by both
 * the network handlers and the container ones (ADR-0249).
 *
 * read_net_stat() reads /sys/class/net/<ifname>/statistics/<file>;
 * container_veth_host_name() composes the host-side veth name for a
 * container's Nth attachment. Both are network facts about a name, and
 * this module already owns those.
 */
/* Forward declaration rather than including registry.h: that header
 * already includes this one, and only the pointer type is needed. */
struct registry_entry;

long long read_net_stat(const char *ifname, const char *file);
void container_veth_host_name(const struct registry_entry *e, int idx, char *out, size_t out_size);

#endif /* NETWORK_H */
