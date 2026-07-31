#include "device.h"
#include "namecheck.h"
#include "network.h"
#include "persist.h"
#include "registry.h"
#include "rtnetlink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct network_def g_networks[NETWORK_MAX];
static char g_state_path[PATH_MAX];

static int network_name_is_valid(const char *name)
{
	return simple_name_is_valid(name, NETWORK_NAME_MAX);
}

static uint32_t mask_for_prefix(int prefix_len)
{
	if (prefix_len <= 0)
		return 0;
	return (uint32_t)0xFFFFFFFFu << (32 - prefix_len);
}

static int host_max_for_prefix(int prefix_len)
{
	return (int)(1u << (32 - prefix_len)) - 2;
}

static int ranges_overlap(uint32_t a_base_be, int a_prefix, uint32_t b_base_be, int b_prefix)
{
	int common = a_prefix < b_prefix ? a_prefix : b_prefix;
	uint32_t mask = mask_for_prefix(common);

	return (ntohl(a_base_be) & mask) == (ntohl(b_base_be) & mask);
}

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	network_write_json_list(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

/*
 * Parses one persisted entry into slot. Deliberately strict: this
 * file is only ever written by network_create()'s validated path, so
 * a malformed entry means real corruption (disk fault, manual
 * tampering) -- silently dropping it would be exactly the "restart
 * forgets a live bridge" bug this module exists to prevent. Returns 0
 * on success, -1 on any validation failure.
 *
 * Migration (No Regressions): every network persisted before gateways
 * became optional has no "has_gateway" key at all -- current code
 * always writes one (see network_write_json_one()). Its *absence* is
 * therefore the reliable signal this is an old-format entry, created
 * back when every network unconditionally got a host-owned gateway at
 * base|1 -- so it's loaded exactly that way, not silently demoted to
 * gateway-less. Only entries written by the current code (which always
 * includes "has_gateway") can describe a genuinely gateway-less
 * network.
 */
static int network_ifname_is_valid(const char *ifname)
{
	return simple_name_is_valid(ifname, NETWORK_IFNAME_MAX);
}

static int parse_persisted_entry(const struct json_value *item, struct network_def *slot, int idx)
{
	const char *name = json_as_string(json_object_get(item, "name"));
	const char *subnet = json_as_string(json_object_get(item, "subnet"));
	const struct json_value *jprefix = json_object_get(item, "prefix_len");
	const struct json_value *jhas_gw = json_object_get(item, "has_gateway");
	const struct json_value *jinterfaces = json_object_get(item, "interfaces");
	int prefix_len;
	int has_gateway;
	uint32_t gateway_be = 0;
	struct in_addr addr;
	int i;

	if (!network_name_is_valid(name) || subnet == NULL || jprefix == NULL)
		return -1;
	prefix_len = (int)json_as_number(jprefix);
	if (prefix_len < 8 || prefix_len > 30)
		return -1;
	if (inet_pton(AF_INET, subnet, &addr) != 1)
		return -1;
	if ((ntohl(addr.s_addr) & ~mask_for_prefix(prefix_len)) != 0)
		return -1;

	if (jhas_gw == NULL) {
		/* Old-format entry, predating this field: today's exact
		 * legacy behavior, always a host-owned gateway at base|1. */
		has_gateway = 1;
		gateway_be = htonl(ntohl(addr.s_addr) | 1);
	} else {
		has_gateway = (jhas_gw->type == JSON_BOOL && jhas_gw->u.boolean);
		if (has_gateway) {
			const char *gw = json_as_string(json_object_get(item, "gateway"));
			struct in_addr gwaddr;

			if (gw == NULL || inet_pton(AF_INET, gw, &gwaddr) != 1)
				return -1;
			gateway_be = gwaddr.s_addr;
		}
	}

	for (i = 0; i < idx; i++) {
		if (strcmp(g_networks[i].name, name) == 0)
			return -1;
		if (ranges_overlap(addr.s_addr, prefix_len, g_networks[i].base_be,
		                    g_networks[i].prefix_len))
			return -1;
	}

	memset(slot, 0, sizeof(*slot));
	strncpy(slot->name, name, sizeof(slot->name) - 1);
	slot->base_be = addr.s_addr;
	slot->prefix_len = prefix_len;
	slot->has_gateway = has_gateway;
	slot->gateway_be = gateway_be;

	if (jinterfaces != NULL) {
		size_t j;

		if (jinterfaces->type != JSON_ARRAY || jinterfaces->u.array.count > NETWORK_MAX_INTERFACES)
			return -1;
		for (j = 0; j < jinterfaces->u.array.count; j++) {
			const struct json_value *jent = jinterfaces->u.array.items[j];
			const char *ifname = json_as_string(json_object_get(jent, "ifname"));
			const struct json_value *jvlan = json_object_get(jent, "vlan_id");

			if (!network_ifname_is_valid(ifname) || jvlan == NULL)
				return -1;
			strncpy(slot->interfaces[j].ifname, ifname, sizeof(slot->interfaces[j].ifname) - 1);
			slot->interfaces[j].vlan_id = (int)json_as_number(jvlan);
			slot->interface_count++;
		}
	}

	slot->in_use = 1;
	return 0;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int rc = 0;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted state yet -- first-ever startup */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted network state\n", g_state_path);
		return -1;
	}
	if (root->u.array.count > NETWORK_MAX) {
		json_free(root);
		fprintf(stderr, "%s: more networks persisted than NETWORK_MAX\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		if (parse_persisted_entry(root->u.array.items[i], &g_networks[i], (int)i) != 0) {
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			rc = -1;
			break;
		}
	}
	json_free(root);
	return rc;
}

/*
 * The mechanical half of attaching one interface to a bridge, shared
 * between network_init() (re-applying persisted state on every daemon
 * restart, the same idempotent-reapply posture the bridge/gateway
 * setup above already has) and network_attach_interface() (a fresh
 * attach). vlan_id 0 enslaves ifc->ifname directly; nonzero creates
 * "<ifname>.<vlan_id>" and enslaves that instead, leaving ifname
 * itself untouched. tolerate_exist governs whether an EEXIST from
 * rtnl_vlan_create() (the sub-interface already existing, e.g. a
 * genuine daemon restart rather than a real host reboot) is
 * swallowed -- true for network_init()'s own idempotent replay, false
 * for a fresh attach, where EEXIST would mean real, unexpected state.
 */
static int network_apply_interface(int fd, const struct network_def *net,
                                    const struct network_attached_interface *ifc,
                                    int tolerate_exist)
{
	if (ifc->vlan_id != 0) {
		char derived[NETWORK_IFNAME_MAX];

		if (snprintf(derived, sizeof(derived), "%s.%d", ifc->ifname, ifc->vlan_id) >=
		    (int)sizeof(derived))
			return -1;
		if (rtnl_vlan_create(fd, derived, ifc->ifname, ifc->vlan_id) != 0 &&
		    !(tolerate_exist && errno == EEXIST))
			return -1;
		if (rtnl_link_set_master(fd, derived, net->name) != 0)
			return -1;
		if (rtnl_link_set_up(fd, derived) != 0)
			return -1;
	} else {
		if (rtnl_link_set_master(fd, ifc->ifname, net->name) != 0)
			return -1;
		if (rtnl_link_set_up(fd, ifc->ifname) != 0)
			return -1;
	}
	return 0;
}

int network_init(const char *state_path)
{
	int fd;
	int i;

	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;

	memset(g_networks, 0, sizeof(g_networks));
	if (load_state() != 0)
		return -1;

	fd = rtnl_open();
	if (fd < 0) {
		perror("rtnl_open (network init)");
		return -1;
	}
	for (i = 0; i < NETWORK_MAX; i++) {
		if (!g_networks[i].in_use)
			continue;
		if (rtnl_bridge_create(fd, g_networks[i].name) != 0 && errno != EEXIST) {
			perror("rtnl_bridge_create");
			rtnl_close(fd);
			return -1;
		}
		if (g_networks[i].has_gateway &&
		    rtnl_addr_add_ipv4(fd, g_networks[i].name, g_networks[i].gateway_be,
		                        g_networks[i].prefix_len) != 0 &&
		    errno != EEXIST) {
			perror("rtnl_addr_add_ipv4 (bridge gateway)");
			rtnl_close(fd);
			return -1;
		}
		if (rtnl_link_set_up(fd, g_networks[i].name) != 0) {
			perror("rtnl_link_set_up (bridge)");
			rtnl_close(fd);
			return -1;
		}
	}

	/*
	 * Unlike the bridge/gateway above (pure software, always
	 * reconstructible), a persisted physical-interface attachment
	 * depends on real host hardware actually being present on THIS
	 * boot -- deliberately best-effort, not a hard startup failure,
	 * the same "not present on this host -- skip, not fatal" posture
	 * test_image_fixture_stage_toolchain()'s own extras loop already
	 * established for exactly this class of "may or may not exist on
	 * this particular host" dependency.
	 */
	for (i = 0; i < NETWORK_MAX; i++) {
		int j;

		if (!g_networks[i].in_use)
			continue;
		for (j = 0; j < g_networks[i].interface_count; j++) {
			int rfd = rtnl_open();

			if (rfd < 0)
				continue;
			if (network_apply_interface(rfd, &g_networks[i], &g_networks[i].interfaces[j], 1) !=
			    0)
				fprintf(stderr,
				        "%s: could not re-attach interface %s to network %s on startup "
				        "(hardware not present on this boot?)\n",
				        g_state_path, g_networks[i].interfaces[j].ifname, g_networks[i].name);
			rtnl_close(rfd);
		}
	}

	return 0;
}

struct network_def *network_find(const char *name)
{
	int i;

	for (i = 0; i < NETWORK_MAX; i++) {
		if (g_networks[i].in_use && strcmp(g_networks[i].name, name) == 0)
			return &g_networks[i];
	}
	return NULL;
}

/*
 * Validates an operator-supplied gateway address for a not-yet-created
 * network: parses as IPv4, falls within [base_be, base_be|prefix]'s
 * subnet, and isn't the network address (host-part 0) or the
 * broadcast address (host-part host_max+1) -- the only two addresses
 * that are never valid for anything on this subnet regardless of
 * whether it has a gateway at all.
 */
static int gateway_str_is_valid(const char *gateway_str, uint32_t base_be, int prefix_len,
                                 uint32_t *out_gateway_be)
{
	struct in_addr addr;
	uint32_t mask, ip_host, base_host;
	int host_part;

	if (inet_pton(AF_INET, gateway_str, &addr) != 1)
		return -1;

	mask = mask_for_prefix(prefix_len);
	ip_host = ntohl(addr.s_addr);
	base_host = ntohl(base_be);
	if ((ip_host & mask) != base_host)
		return -1;

	host_part = (int)(ip_host - base_host);
	if (host_part < 1 || host_part > host_max_for_prefix(prefix_len) + 1)
		return -1;

	*out_gateway_be = addr.s_addr;
	return 0;
}

enum network_error network_create(const char *name, const char *subnet_str, int prefix_len,
                                   const char *gateway_str, struct network_def **out)
{
	struct in_addr addr;
	uint32_t gateway_be = 0;
	int has_gateway;
	int i, slot = -1;
	int fd;
	struct network_def *e;

	if (!network_name_is_valid(name))
		return NETWORK_ERR_INVALID_NAME;
	if (network_find(name) != NULL)
		return NETWORK_ERR_DUPLICATE;

	if (prefix_len < 8 || prefix_len > 30 || inet_pton(AF_INET, subnet_str, &addr) != 1)
		return NETWORK_ERR_INVALID_SUBNET;
	if ((ntohl(addr.s_addr) & ~mask_for_prefix(prefix_len)) != 0)
		return NETWORK_ERR_INVALID_SUBNET;

	has_gateway = (gateway_str != NULL && gateway_str[0] != '\0');
	if (has_gateway && gateway_str_is_valid(gateway_str, addr.s_addr, prefix_len, &gateway_be) != 0)
		return NETWORK_ERR_INVALID_GATEWAY;

	for (i = 0; i < NETWORK_MAX; i++) {
		if (g_networks[i].in_use &&
		    ranges_overlap(addr.s_addr, prefix_len, g_networks[i].base_be,
		                    g_networks[i].prefix_len))
			return NETWORK_ERR_OVERLAP;
	}

	for (i = 0; i < NETWORK_MAX; i++) {
		if (!g_networks[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return NETWORK_ERR_FULL;

	e = &g_networks[slot];
	memset(e, 0, sizeof(*e));
	strncpy(e->name, name, sizeof(e->name) - 1);
	e->base_be = addr.s_addr;
	e->prefix_len = prefix_len;
	e->has_gateway = has_gateway;
	e->gateway_be = gateway_be;

	fd = rtnl_open();
	if (fd < 0)
		return NETWORK_ERR_CREATE_FAILED;
	if (rtnl_bridge_create(fd, e->name) != 0) {
		rtnl_close(fd);
		return NETWORK_ERR_CREATE_FAILED;
	}
	if (has_gateway && rtnl_addr_add_ipv4(fd, e->name, e->gateway_be, e->prefix_len) != 0) {
		rtnl_link_delete(fd, e->name);
		rtnl_close(fd);
		return NETWORK_ERR_CREATE_FAILED;
	}
	if (rtnl_link_set_up(fd, e->name) != 0) {
		rtnl_link_delete(fd, e->name);
		rtnl_close(fd);
		return NETWORK_ERR_CREATE_FAILED;
	}
	rtnl_close(fd);

	e->in_use = 1;
	if (save_state() != 0) {
		/*
		 * Can't persist -- a restart would forget this bridge while
		 * it keeps running in the kernel, exactly the bug this
		 * module exists to prevent. Roll back rather than report
		 * success on a lie.
		 */
		e->in_use = 0;
		fd = rtnl_open();
		if (fd >= 0) {
			rtnl_link_delete(fd, e->name);
			rtnl_close(fd);
		}
		return NETWORK_ERR_CREATE_FAILED;
	}

	*out = e;
	return NETWORK_OK;
}

enum network_error network_delete(const char *name)
{
	struct network_def *e = network_find(name);
	int fd;
	int i;

	if (e == NULL)
		return NETWORK_ERR_NOT_FOUND;
	if (registry_network_in_use(name))
		return NETWORK_ERR_IN_USE;

	fd = rtnl_open();
	if (fd < 0)
		return NETWORK_ERR_DELETE_FAILED;

	/*
	 * Deleting the bridge itself releases (un-enslaves) any still-
	 * attached ports automatically -- but a VLAN sub-interface this
	 * network created (network_apply_interface(), vlan_id != 0) has no
	 * other owner and would otherwise leak as an orphaned, no-longer-
	 * tracked netdev. Best-effort, not blocking: a genuinely gone/
	 * already-broken physical interface shouldn't make an otherwise
	 * unused network un-deletable.
	 */
	for (i = 0; i < e->interface_count; i++) {
		if (e->interfaces[i].vlan_id != 0) {
			char derived[NETWORK_IFNAME_MAX];

			if (snprintf(derived, sizeof(derived), "%s.%d", e->interfaces[i].ifname,
			             e->interfaces[i].vlan_id) < (int)sizeof(derived) &&
			    rtnl_link_delete(fd, derived) != 0)
				fprintf(stderr, "%s: could not remove VLAN sub-interface %s while deleting network\n",
				        name, derived);
		} else if (rtnl_link_clear_master(fd, e->interfaces[i].ifname) != 0) {
			fprintf(stderr, "%s: could not release interface %s while deleting network\n", name,
			        e->interfaces[i].ifname);
		}
	}

	if (rtnl_link_delete(fd, name) != 0) {
		rtnl_close(fd);
		return NETWORK_ERR_DELETE_FAILED;
	}
	rtnl_close(fd);

	e->in_use = 0;
	e->interface_count = 0;
	if (save_state() != 0)
		return NETWORK_ERR_DELETE_FAILED;
	return NETWORK_OK;
}

enum network_error network_attach_interface(const char *name, const char *ifname, int vlan_id)
{
	struct network_def *net = network_find(name);
	const struct discovered_device *dev;
	char dev_id[8 + NETWORK_IFNAME_MAX];
	char derived[NETWORK_IFNAME_MAX];
	int fd;
	int i;

	if (net == NULL)
		return NETWORK_ERR_NOT_FOUND;
	if (net->interface_count >= NETWORK_MAX_INTERFACES)
		return NETWORK_ERR_INTERFACE_FULL;
	for (i = 0; i < net->interface_count; i++) {
		if (strcmp(net->interfaces[i].ifname, ifname) == 0)
			return NETWORK_ERR_INTERFACE_ATTACHED;
	}
	if (vlan_id != 0 &&
	    snprintf(derived, sizeof(derived), "%s.%d", ifname, vlan_id) >= (int)sizeof(derived))
		return NETWORK_ERR_INTERFACE_NAME_TOO_LONG;

	/* device_find() is the same single source of truth --interface=
	 * passthrough (ADR-0022) already validates against; an interface
	 * already moved into a container's netns, or already enslaved to
	 * any master (including a previous call here), simply won't be
	 * assignable -- see enumerate_net_one()'s master-symlink check,
	 * daemon/src/device.c. */
	snprintf(dev_id, sizeof(dev_id), "net:%s", ifname);
	dev = device_find(dev_id);
	if (dev == NULL || !dev->assignable)
		return NETWORK_ERR_INTERFACE_NOT_FOUND;

	fd = rtnl_open();
	if (fd < 0)
		return NETWORK_ERR_CREATE_FAILED;
	{
		struct network_attached_interface ifc;

		memset(&ifc, 0, sizeof(ifc));
		strncpy(ifc.ifname, ifname, sizeof(ifc.ifname) - 1);
		ifc.vlan_id = vlan_id;

		if (network_apply_interface(fd, net, &ifc, 0) != 0) {
			rtnl_close(fd);
			return NETWORK_ERR_CREATE_FAILED;
		}
		rtnl_close(fd);

		net->interfaces[net->interface_count] = ifc;
		net->interface_count++;
	}

	if (save_state() != 0) {
		/* Roll back, same "never report success on a lie" discipline
		 * network_create() itself already established. */
		net->interface_count--;
		fd = rtnl_open();
		if (fd >= 0) {
			if (vlan_id != 0)
				rtnl_link_delete(fd, derived);
			else
				rtnl_link_clear_master(fd, ifname);
			rtnl_close(fd);
		}
		return NETWORK_ERR_CREATE_FAILED;
	}
	return NETWORK_OK;
}

enum network_error network_detach_interface(const char *name, const char *ifname)
{
	struct network_def *net = network_find(name);
	int idx = -1;
	int i;
	int fd;
	int vlan_id;

	if (net == NULL)
		return NETWORK_ERR_NOT_FOUND;
	for (i = 0; i < net->interface_count; i++) {
		if (strcmp(net->interfaces[i].ifname, ifname) == 0) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		return NETWORK_ERR_INTERFACE_NOT_ATTACHED;
	vlan_id = net->interfaces[idx].vlan_id;

	fd = rtnl_open();
	if (fd < 0)
		return NETWORK_ERR_DELETE_FAILED;
	if (vlan_id != 0) {
		char derived[NETWORK_IFNAME_MAX];

		snprintf(derived, sizeof(derived), "%s.%d", ifname, vlan_id);
		if (rtnl_link_delete(fd, derived) != 0) {
			rtnl_close(fd);
			return NETWORK_ERR_DELETE_FAILED;
		}
	} else if (rtnl_link_clear_master(fd, ifname) != 0) {
		rtnl_close(fd);
		return NETWORK_ERR_DELETE_FAILED;
	}
	rtnl_close(fd);

	/* Compact the array -- interface_count stays a dense prefix, same
	 * shape every other fixed-array table in this codebase keeps. */
	for (i = idx; i < net->interface_count - 1; i++)
		net->interfaces[i] = net->interfaces[i + 1];
	net->interface_count--;

	if (save_state() != 0)
		return NETWORK_ERR_DELETE_FAILED;
	return NETWORK_OK;
}

int network_alloc_ip(const char *name, uint32_t *out_ip_be)
{
	struct network_def *net = network_find(name);
	uint32_t exclude_be;

	if (net == NULL)
		return -1;
	/* Host-part 1 is no longer implicitly the gateway -- it's only
	 * excluded (via exclude_be) when this network actually has one,
	 * and then at whatever address the operator chose, not always .1. */
	exclude_be = net->has_gateway ? net->gateway_be : 0;
	return registry_alloc_ip(net->base_be, 1, host_max_for_prefix(net->prefix_len), exclude_be,
	                          out_ip_be);
}

enum network_error network_ip_available(const char *name, uint32_t ip_be)
{
	struct network_def *net = network_find(name);
	uint32_t mask, ip_host, base_host, host_part;
	int host_max;

	if (net == NULL)
		return NETWORK_ERR_NOT_FOUND;

	mask = mask_for_prefix(net->prefix_len);
	ip_host = ntohl(ip_be);
	base_host = ntohl(net->base_be);
	host_max = host_max_for_prefix(net->prefix_len);

	/* Same masked network, and a host part in [1, host_max] -- .0 is
	 * the network address, the top address is the broadcast address.
	 * .1 is only excluded below, and only when this network actually
	 * has a gateway there (or wherever the operator put it). */
	if ((ip_host & mask) != base_host)
		return NETWORK_ERR_IP_OUT_OF_RANGE;
	host_part = ip_host - base_host;
	if (host_part < 1 || (int)host_part > host_max)
		return NETWORK_ERR_IP_OUT_OF_RANGE;
	if (net->has_gateway && ip_be == net->gateway_be)
		return NETWORK_ERR_IP_OUT_OF_RANGE;

	if (!registry_ip_available(ip_be))
		return NETWORK_ERR_IP_TAKEN;
	return NETWORK_OK;
}

void network_write_json_one(const struct network_def *net, struct json_writer *w)
{
	struct in_addr a;
	char subnet_str[INET_ADDRSTRLEN];

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, net->name);
	a.s_addr = net->base_be;
	inet_ntop(AF_INET, &a, subnet_str, sizeof(subnet_str));
	jw_key(w, "subnet");
	jw_str(w, subnet_str);
	jw_key(w, "prefix_len");
	jw_int(w, net->prefix_len);
	jw_key(w, "has_gateway");
	jw_bool(w, net->has_gateway);
	jw_key(w, "gateway");
	if (net->has_gateway) {
		char gateway_str[INET_ADDRSTRLEN];

		a.s_addr = net->gateway_be;
		inet_ntop(AF_INET, &a, gateway_str, sizeof(gateway_str));
		jw_str(w, gateway_str);
	} else {
		jw_null(w);
	}
	jw_key(w, "interfaces");
	jw_arr_open(w);
	{
		int i;

		for (i = 0; i < net->interface_count; i++) {
			jw_obj_open(w);
			jw_key(w, "ifname");
			jw_str(w, net->interfaces[i].ifname);
			jw_key(w, "vlan_id");
			jw_int(w, net->interfaces[i].vlan_id);
			jw_obj_close(w);
		}
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

void network_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < NETWORK_MAX; i++) {
		if (g_networks[i].in_use)
			network_write_json_one(&g_networks[i], w);
	}
	jw_arr_close(w);
}
