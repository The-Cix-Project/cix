#include "device.h"
#include "namecheck.h"
#include "network.h"
#include "persist.h"
#include "registry.h"
#include "rtnetlink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <net/if.h>
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
 * Migration (No Regressions): every network persisted before this
 * address became optional has no "has_address" key at all (ADR-0037
 * called this "has_gateway" at the time -- ADR-0067 renamed the field,
 * this migration is about a point in time before *either* key name
 * existed) -- current code always writes one (see
 * network_write_json_one()). Its *absence* is therefore the reliable
 * signal this is that original old-format entry, created back when
 * every network unconditionally got a host-owned address at base|1 --
 * so it's loaded exactly that way, not silently demoted to
 * address-less. Only entries written by current code (which always
 * includes "has_address") can describe a genuinely address-less
 * network -- per this project's own clean-cut-over posture, a
 * persisted entry still using ADR-0037's original "has_gateway"/
 * "gateway" key names from between that ADR and ADR-0067 is not
 * separately migrated.
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
	const struct json_value *jhas_addr = json_object_get(item, "has_address");
	const struct json_value *jis_mgmt = json_object_get(item, "is_management");
	const struct json_value *jinterfaces = json_object_get(item, "interfaces");
	int prefix_len;
	int has_address;
	uint32_t address_be = 0;
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

	if (jhas_addr == NULL) {
		/* Old-format entry, predating this field: today's exact
		 * legacy behavior, always a host-owned address at base|1. */
		has_address = 1;
		address_be = htonl(ntohl(addr.s_addr) | 1);
	} else {
		has_address = (jhas_addr->type == JSON_BOOL && jhas_addr->u.boolean);
		if (has_address) {
			const char *a = json_as_string(json_object_get(item, "address"));
			struct in_addr aaddr;

			if (a == NULL || inet_pton(AF_INET, a, &aaddr) != 1)
				return -1;
			address_be = aaddr.s_addr;
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
	slot->has_address = has_address;
	slot->address_be = address_be;
	/* Absent on any entry predating this field (Part 0.5) -- no network
	 * was ever "management" before this existed, so absence simply
	 * means not-management, no legacy-default reasoning needed here
	 * the way has_address's own absence-handling above requires. */
	slot->is_management = (jis_mgmt != NULL && jis_mgmt->type == JSON_BOOL && jis_mgmt->u.boolean);
	/* Issue #70: absent on any entry predating this field -> 0 (unset,
	 * the full-range default), no legacy reasoning needed. */
	{
		const struct json_value *jas = json_object_get(item, "alloc_start_host");
		const struct json_value *jae = json_object_get(item, "alloc_end_host");

		slot->alloc_start_host = (jas != NULL && jas->type == JSON_NUMBER) ? (int)json_as_number(jas) : 0;
		slot->alloc_end_host = (jae != NULL && jae->type == JSON_NUMBER) ? (int)json_as_number(jae) : 0;
	}

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
 * restart, the same idempotent-reapply posture the bridge/address
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

void network_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
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
		if (g_networks[i].has_address &&
		    rtnl_addr_add_ipv4(fd, g_networks[i].name, g_networks[i].address_be,
		                        g_networks[i].prefix_len) != 0 &&
		    errno != EEXIST) {
			perror("rtnl_addr_add_ipv4 (bridge address)");
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
	 * Unlike the bridge/address above (pure software, always
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
 * Validates an operator-supplied address for a not-yet-created
 * network: parses as IPv4, falls within [base_be, base_be|prefix]'s
 * subnet, and isn't the network address (host-part 0) or the
 * broadcast address (host-part host_max+1) -- the only two addresses
 * that are never valid for anything on this subnet regardless of
 * whether it has one at all.
 */
static int address_str_is_valid(const char *address_str, uint32_t base_be, int prefix_len,
                                 uint32_t *out_address_be)
{
	struct in_addr addr;
	uint32_t mask, ip_host, base_host;
	int host_part;

	if (inet_pton(AF_INET, address_str, &addr) != 1)
		return -1;

	mask = mask_for_prefix(prefix_len);
	ip_host = ntohl(addr.s_addr);
	base_host = ntohl(base_be);
	if ((ip_host & mask) != base_host)
		return -1;

	host_part = (int)(ip_host - base_host);
	if (host_part < 1 || host_part > host_max_for_prefix(prefix_len) + 1)
		return -1;

	*out_address_be = addr.s_addr;
	return 0;
}

int network_address_str_is_valid(const struct network_def *net, const char *address_str,
                                  uint32_t *out_address_be)
{
	return address_str_is_valid(address_str, net->base_be, net->prefix_len, out_address_be);
}

/*
 * Issue #70: parse one optional dotted-IP allocation bound into a
 * host-part offset, validating it is inside this subnet. Returns 0 and
 * writes *out_host (>=1); -1 if the string is set but not a valid
 * in-subnet address. A NULL/empty string leaves *out_host at 0 (unset).
 */
static int parse_alloc_bound(const char *s_str, uint32_t base_be, int prefix_len, int *out_host)
{
	struct in_addr a;
	uint32_t host;

	*out_host = 0;
	if (s_str == NULL || s_str[0] == '\0')
		return 0;
	if (inet_pton(AF_INET, s_str, &a) != 1)
		return -1;
	if ((ntohl(a.s_addr) & mask_for_prefix(prefix_len)) != ntohl(base_be))
		return -1;
	host = ntohl(a.s_addr) - ntohl(base_be);
	if (host < 1 || (int)host > host_max_for_prefix(prefix_len))
		return -1;
	*out_host = (int)host;
	return 0;
}

enum network_error network_create(const char *name, const char *subnet_str, int prefix_len,
                                   const char *address_str, const char *alloc_start_str,
                                   const char *alloc_end_str, struct network_def **out)
{
	struct in_addr addr;
	uint32_t address_be = 0;
	int has_address;
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

	has_address = (address_str != NULL && address_str[0] != '\0');
	if (has_address && address_str_is_valid(address_str, addr.s_addr, prefix_len, &address_be) != 0)
		return NETWORK_ERR_INVALID_ADDRESS;

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
	{
		int as = 0, ae = 0;

		if (parse_alloc_bound(alloc_start_str, addr.s_addr, prefix_len, &as) != 0 ||
		    parse_alloc_bound(alloc_end_str, addr.s_addr, prefix_len, &ae) != 0 ||
		    (as != 0 && ae != 0 && ae < as)) {
			return NETWORK_ERR_INVALID_ADDRESS;
		}
		e->alloc_start_host = as;
		e->alloc_end_host = ae;
	}
	e->has_address = has_address;
	e->address_be = address_be;

	fd = rtnl_open();
	if (fd < 0)
		return NETWORK_ERR_CREATE_FAILED;
	if (rtnl_bridge_create(fd, e->name) != 0) {
		rtnl_close(fd);
		return NETWORK_ERR_CREATE_FAILED;
	}
	if (has_address && rtnl_addr_add_ipv4(fd, e->name, e->address_be, e->prefix_len) != 0) {
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

/*
 * Re-address an existing network in place: new subnet, new prefix, new
 * host address, same name and same bridge.
 *
 * Exists for PUT /v1/system/management-network, which moves a booted
 * box onto a different network. Re-addressing rather than creating a
 * replacement network is deliberate: the management network is
 * referenced by name in persisted state and by the is_management flag,
 * and a box that changed address three times would otherwise accumulate
 * three networks, only one of them real.
 *
 * Does NOT remove the old address from the bridge. The caller gets it
 * back through out_old_* and is expected to defer that removal by a
 * couple of seconds, because deleting an address out from under the
 * connection carrying the reply leaves the client waiting forever for
 * bytes the kernel accepted and can no longer deliver -- measured, and
 * the reason ADR-0068's own cleanup is on a timer. Both addresses being
 * briefly live on the bridge is exactly what makes the handover safe.
 *
 * The caller is responsible for refusing this when containers still
 * hold addresses in the old subnet; this function has no view of the
 * registry. It does reset the allocation window when the subnet moves,
 * since a window is a pair of host-parts within one specific subnet and
 * carrying it across would silently re-point it at different addresses.
 */
enum network_error network_set_address(const char *name, const char *subnet_str, int prefix_len,
                                        const char *address_str, uint32_t *out_old_address_be,
                                        int *out_old_prefix_len)
{
	struct in_addr addr;
	uint32_t address_be = 0;
	struct network_def *e;
	uint32_t old_base_be;
	int old_prefix_len;
	uint32_t old_address_be;
	int old_has_address;
	int old_alloc_start, old_alloc_end;
	int subnet_changed;
	int i;
	int fd;

	e = network_find(name);
	if (e == NULL)
		return NETWORK_ERR_NOT_FOUND;

	if (prefix_len < 8 || prefix_len > 30 || inet_pton(AF_INET, subnet_str, &addr) != 1)
		return NETWORK_ERR_INVALID_SUBNET;
	if ((ntohl(addr.s_addr) & ~mask_for_prefix(prefix_len)) != 0)
		return NETWORK_ERR_INVALID_SUBNET;
	if (address_str == NULL || address_str[0] == '\0')
		return NETWORK_ERR_INVALID_ADDRESS;
	if (address_str_is_valid(address_str, addr.s_addr, prefix_len, &address_be) != 0)
		return NETWORK_ERR_INVALID_ADDRESS;

	/* Overlap is checked against every OTHER network -- self-overlap is
	 * the normal case here (an address change inside the same subnet)
	 * and must not be reported as a conflict. */
	for (i = 0; i < NETWORK_MAX; i++) {
		if (!g_networks[i].in_use || &g_networks[i] == e)
			continue;
		if (ranges_overlap(addr.s_addr, prefix_len, g_networks[i].base_be,
		                    g_networks[i].prefix_len))
			return NETWORK_ERR_OVERLAP;
	}

	old_base_be = e->base_be;
	old_prefix_len = e->prefix_len;
	old_address_be = e->address_be;
	old_has_address = e->has_address;
	old_alloc_start = e->alloc_start_host;
	old_alloc_end = e->alloc_end_host;
	subnet_changed = (old_base_be != addr.s_addr || old_prefix_len != prefix_len);

	/* Idempotent: the same subnet and the same address is a no-op
	 * rather than a spurious EEXIST from rtnl_addr_add_ipv4()'s own
	 * NLM_F_EXCL, the same reasoning ADR-0068 applies to bind_ip. */
	if (!subnet_changed && old_has_address && old_address_be == address_be) {
		if (out_old_address_be != NULL)
			*out_old_address_be = 0;
		if (out_old_prefix_len != NULL)
			*out_old_prefix_len = 0;
		return NETWORK_OK;
	}

	fd = rtnl_open();
	if (fd < 0)
		return NETWORK_ERR_CREATE_FAILED;
	if (rtnl_addr_add_ipv4(fd, e->name, address_be, prefix_len) != 0 && errno != EEXIST) {
		rtnl_close(fd);
		return NETWORK_ERR_CREATE_FAILED;
	}
	rtnl_close(fd);

	e->base_be = addr.s_addr;
	e->prefix_len = prefix_len;
	e->address_be = address_be;
	e->has_address = 1;
	if (subnet_changed) {
		e->alloc_start_host = 0;
		e->alloc_end_host = 0;
	}

	if (save_state() != 0) {
		/* Same posture as network_create(): a kernel change we cannot
		 * remember is worse than no change, so put both back. */
		int rfd = rtnl_open();

		if (rfd >= 0) {
			rtnl_addr_del_ipv4(rfd, e->name, address_be, prefix_len);
			rtnl_close(rfd);
		}
		e->base_be = old_base_be;
		e->prefix_len = old_prefix_len;
		e->address_be = old_address_be;
		e->has_address = old_has_address;
		e->alloc_start_host = old_alloc_start;
		e->alloc_end_host = old_alloc_end;
		return NETWORK_ERR_CREATE_FAILED;
	}

	if (out_old_address_be != NULL)
		*out_old_address_be = old_has_address ? old_address_be : 0;
	if (out_old_prefix_len != NULL)
		*out_old_prefix_len = old_prefix_len;
	return NETWORK_OK;
}

enum network_error network_delete(const char *name)
{
	struct network_def *e = network_find(name);
	int fd;
	int i;

	if (e == NULL)
		return NETWORK_ERR_NOT_FOUND;
	if (e->is_management)
		return NETWORK_ERR_IS_MANAGEMENT;
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
	/* NULL: this lookup only ever matches a "net:" id, never a "disk:"
	 * one -- os_containers_dir (ADR-0142's OS-disk exclusion) has no
	 * bearing here, see device.h's own doc comment. */
	snprintf(dev_id, sizeof(dev_id), "net:%s", ifname);
	dev = device_find(dev_id, NULL);
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
	if (net->is_management)
		return NETWORK_ERR_IS_MANAGEMENT;
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

struct network_def *network_find_management(void)
{
	int i;

	for (i = 0; i < NETWORK_MAX; i++) {
		if (g_networks[i].in_use && g_networks[i].is_management)
			return &g_networks[i];
	}
	return NULL;
}

enum network_error network_set_management(const char *name)
{
	struct network_def *e = network_find(name);
	struct network_def *prev;

	if (e == NULL)
		return NETWORK_ERR_NOT_FOUND;
	if (!e->has_address)
		return NETWORK_ERR_INVALID_ADDRESS;
	if (e->is_management)
		return NETWORK_OK; /* already the management network -- idempotent */

	prev = network_find_management();
	if (prev != NULL)
		prev->is_management = 0;
	e->is_management = 1;

	if (save_state() != 0) {
		/* Roll back, same "never report success on a lie" discipline
		 * every other mutator in this file already follows. */
		e->is_management = 0;
		if (prev != NULL)
			prev->is_management = 1;
		return NETWORK_ERR_CREATE_FAILED;
	}
	return NETWORK_OK;
}

int network_alloc_ip(const char *name, uint32_t *out_ip_be)
{
	struct network_def *net = network_find(name);
	uint32_t exclude_be;

	if (net == NULL)
		return -1;
	/* Host-part 1 is no longer implicitly this network's own address --
	 * it's only excluded (via exclude_be) when this network actually
	 * has one, and then at whatever address the operator chose, not
	 * always .1. */
	exclude_be = net->has_address ? net->address_be : 0;
	{
		int lo = net->alloc_start_host > 0 ? net->alloc_start_host : 1;
		int hi = net->alloc_end_host > 0 ? net->alloc_end_host
		                                 : host_max_for_prefix(net->prefix_len);

		/*
		 * Issue #137: REFUSE to auto-allocate on a network that is
		 * bridged onto real infrastructure and has no declared pool.
		 *
		 * An enslaved physical interface is the ground truth for "this
		 * bridge reaches equipment this platform does not own" -- the
		 * subnet is then shared with routers, switches, NAS boxes and
		 * whatever else lives there, none of which this daemon can see
		 * or ARP for reliably at allocation time. Handing out an
		 * address from it is a coin flip against someone's production
		 * LAN.
		 *
		 * This is not theoretical: containers auto-allocated .2 and .3
		 * on exactly such a network and went live on the operator's
		 * real LAN, inside a range they had explicitly reserved for
		 * other equipment. Issue #70's floor of 2 (below) skips only
		 * the .1 gateway convention -- it does nothing about .2
		 * onwards.
		 *
		 * So this fails CLOSED. The two safe ways forward both remain
		 * fully available and are named in the error: declare the pool
		 * this platform may draw from, or pin the address explicitly
		 * per container (an explicit address is deliberately never
		 * constrained by the pool -- operator intent wins).
		 *
		 * An internal, cix-owned bridge has no enslaved interface, so
		 * it is unaffected and keeps allocating from host-part 1.
		 */
		if (net->interface_count > 0 && net->alloc_start_host == 0 &&
		    net->alloc_end_host == 0)
			return -2;

		/* Issue #70 safe default: a management network is bridged onto a
		 * real LAN whose gateway is, by overwhelming convention, .1 --
		 * never auto-hand-out host-part 1 there unless the operator has
		 * explicitly widened the range back down to it. An internal,
		 * cix-owned network has no external gateway, so its floor
		 * stays 1. */
		if (net->is_management && net->alloc_start_host == 0 && lo < 2)
			lo = 2;
		return registry_alloc_ip(net->base_be, lo, hi, exclude_be, out_ip_be);
	}
}

enum network_error network_set_alloc_window(const char *name, const char *alloc_start_str,
                                             const char *alloc_end_str)
{
	struct network_def *net = network_find(name);
	int as, ae;

	if (net == NULL)
		return NETWORK_ERR_NOT_FOUND;

	/*
	 * Both bounds are resolved against the CURRENT values first, so a
	 * partial update validates the pair it will actually end up with
	 * rather than the pair it was handed. Setting only alloc_end below
	 * an existing alloc_start must be rejected, and it would not be if
	 * the unchanged bound were treated as absent.
	 */
	as = net->alloc_start_host;
	ae = net->alloc_end_host;

	if (alloc_start_str != NULL) {
		if (alloc_start_str[0] == '\0')
			as = 0; /* explicit clear */
		else if (parse_alloc_bound(alloc_start_str, net->base_be, net->prefix_len, &as) != 0)
			return NETWORK_ERR_INVALID_ADDRESS;
	}
	if (alloc_end_str != NULL) {
		if (alloc_end_str[0] == '\0')
			ae = 0;
		else if (parse_alloc_bound(alloc_end_str, net->base_be, net->prefix_len, &ae) != 0)
			return NETWORK_ERR_INVALID_ADDRESS;
	}
	if (as != 0 && ae != 0 && ae < as)
		return NETWORK_ERR_INVALID_ADDRESS;

	net->alloc_start_host = as;
	net->alloc_end_host = ae;

	/*
	 * Persisted immediately. A window that survives only until the next
	 * restart is worse than none: it would silently revert a bridged
	 * network from "pool declared" back to "fails closed", and the
	 * operator would have no reason to look.
	 *
	 * CREATE_FAILED rather than a new code: the caller maps it to 500,
	 * which is the honest answer for "the change is in memory but did
	 * not reach disk", and adding a code used by exactly one call site
	 * would not tell an operator anything the message does not.
	 */
	if (save_state() != 0)
		return NETWORK_ERR_CREATE_FAILED;
	return NETWORK_OK;
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
	 * has its own address there (or wherever the operator put it). */
	if ((ip_host & mask) != base_host)
		return NETWORK_ERR_IP_OUT_OF_RANGE;
	host_part = ip_host - base_host;
	if (host_part < 1 || (int)host_part > host_max)
		return NETWORK_ERR_IP_OUT_OF_RANGE;
	if (net->has_address && ip_be == net->address_be)
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
	jw_key(w, "has_address");
	jw_bool(w, net->has_address);
	jw_key(w, "is_management");
	jw_bool(w, net->is_management);
	jw_key(w, "alloc_start_host");
	if (net->alloc_start_host > 0)
		jw_int(w, net->alloc_start_host);
	else
		jw_null(w);
	jw_key(w, "alloc_end_host");
	if (net->alloc_end_host > 0)
		jw_int(w, net->alloc_end_host);
	else
		jw_null(w);
	jw_key(w, "address");
	if (net->has_address) {
		char address_str[INET_ADDRSTRLEN];

		a.s_addr = net->address_be;
		inet_ntop(AF_INET, &a, address_str, sizeof(address_str));
		jw_str(w, address_str);
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

/* ADR-0066: a real, read-only view of the box's own kernel routing
 * table -- the daemon is the only way to ever inspect a running
 * Cix install (ADR-0034, no SSH/general shell), and until now
 * there was no way to see this at all. */
#define ROUTE_DUMP_MAX 64

static void route_write_json_one(const struct kernel_route *r, struct json_writer *w)
{
	struct in_addr a;
	char buf[INET_ADDRSTRLEN];
	char ifname[IF_NAMESIZE];

	jw_obj_open(w);
	jw_key(w, "dest");
	if (r->dst_prefix_len > 0) {
		a.s_addr = r->dst_be;
		inet_ntop(AF_INET, &a, buf, sizeof(buf));
		jw_str(w, buf);
	} else {
		jw_str(w, "default");
	}
	jw_key(w, "prefix");
	jw_int(w, r->dst_prefix_len);
	jw_key(w, "gateway");
	if (r->gateway_be != 0) {
		a.s_addr = r->gateway_be;
		inet_ntop(AF_INET, &a, buf, sizeof(buf));
		jw_str(w, buf);
	} else {
		jw_null(w);
	}
	jw_key(w, "interface");
	if (r->oif_index > 0 && if_indextoname((unsigned int)r->oif_index, ifname) != NULL)
		jw_str(w, ifname);
	else
		jw_null(w);
	jw_key(w, "protocol");
	jw_int(w, r->protocol);
	jw_key(w, "scope");
	jw_int(w, r->scope);
	jw_obj_close(w);
}

/* Returns -1 (nothing written to w) only if the rtnetlink socket
 * itself or the dump request/response fails outright -- caller turns
 * that into a 500, matching every other host-state read in this
 * daemon that has no sane "empty but successful" fallback for a
 * genuine transport failure. */
int network_write_routes_json(struct json_writer *w)
{
	struct kernel_route routes[ROUTE_DUMP_MAX];
	int count = 0;
	int fd;
	int i;
	int n;

	fd = rtnl_open();
	if (fd < 0)
		return -1;
	if (rtnl_route_dump_ipv4(fd, routes, ROUTE_DUMP_MAX, &count) != 0) {
		rtnl_close(fd);
		return -1;
	}
	rtnl_close(fd);

	n = count < ROUTE_DUMP_MAX ? count : ROUTE_DUMP_MAX;
	jw_arr_open(w);
	for (i = 0; i < n; i++)
		route_write_json_one(&routes[i], w);
	jw_arr_close(w);
	return 0;
}

/*
 * Reads /sys/class/net/<ifname>/statistics/<file> -- shared by
 * per-container stats (the host-side veth's own counters, the same
 * numbers a bridge/switch would see) and host-wide stats (real host
 * interfaces). A missing file (ENOENT -- e.g. a container's veth
 * already torn down) is not a request failure, just a 0 for that one
 * counter: GET .../stats stays a best-effort snapshot, not an
 * all-or-nothing report.
 */
long long read_net_stat(const char *ifname, const char *file)
{
	char path[PATH_MAX];
	char buf[32];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", ifname, file);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	return strtoll(buf, NULL, 10);
}

/*
 * The host-side veth for a container's Nth network attachment. The
 * name is a convention (src/container_net.c coins it at creation from
 * the child's pid), not something stored -- so it is derived in one
 * place rather than re-spelled at each call site, which is how the
 * stats reader and the teardown path came to carry the same format
 * string twice.
 *
 * A live-attached network (ADR-0156) is the exception: it was added to
 * an already-running container and carries its real name, since it was
 * never coined from the pid at all.
 */
void container_veth_host_name(const struct registry_entry *e, int idx, char *out,
                                      size_t out_size)
{
	if (e->nets[idx].veth_host[0] != '\0') {
		snprintf(out, out_size, "%s", e->nets[idx].veth_host);
		return;
	}
	snprintf(out, out_size, "vh%d-%d", (int)e->handle.pid, idx);
}
