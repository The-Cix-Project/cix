#include "dhcp.h"
#include "containerpath.h"
#include "logstore.h"
#include "dns.h"
#include "persist.h"
#include "registry.h"
#include "linux_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>

static struct dhcp_network g_networks[DHCP_MAX_NETWORKS];
static struct dhcp_static g_statics[DHCP_MAX_STATIC];
static char g_servers[DHCP_MAX_SERVERS][DHCP_SERVER_NAME_MAX];
static char g_state_path[512];
/*
 * The conf text the running servers were last started against. A range
 * only takes effect at dnsmasq startup, so this is what makes "the
 * config changed but the servers are still serving the old one" a
 * detectable state rather than an invisible one.
 */

static int mac_is_valid(const char *mac)
{
	int i;

	if (mac == NULL || strlen(mac) != 17)
		return 0;
	for (i = 0; i < 17; i++) {
		if ((i % 3) == 2) {
			if (mac[i] != ':')
				return 0;
			continue;
		}
		if (!((mac[i] >= '0' && mac[i] <= '9') || (mac[i] >= 'a' && mac[i] <= 'f')))
			return 0;
	}
	return 1;
}

/*
 * A hostname here becomes a name dnsmasq will answer for, so it is held
 * to the same shape a DNS label has: letters, digits and hyphens, not
 * starting or ending with one. Refused rather than sanitised -- this
 * string is written into a config file the server parses.
 */
static int hostname_is_valid(const char *name)
{
	size_t i, n;

	if (name == NULL)
		return 0;
	n = strlen(name);
	if (n == 0 || n >= DHCP_HOSTNAME_MAX)
		return 0;
	if (name[0] == '-' || name[n - 1] == '-')
		return 0;
	for (i = 0; i < n; i++) {
		char c = name[i];

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
			continue;
		return 0;
	}
	return 1;
}

static struct dhcp_network *net_find(const char *network)
{
	int i;

	for (i = 0; i < DHCP_MAX_NETWORKS; i++)
		if (g_networks[i].network[0] != '\0' && strcmp(g_networks[i].network, network) == 0)
			return &g_networks[i];
	return NULL;
}

static struct dhcp_static *static_find(const char *mac)
{
	int i;

	for (i = 0; i < DHCP_MAX_STATIC; i++)
		if (g_statics[i].mac[0] != '\0' && strcmp(g_statics[i].mac, mac) == 0)
			return &g_statics[i];
	return NULL;
}

const struct dhcp_network *dhcp_network_find(const char *network)
{
	return net_find(network);
}

int dhcp_any_enabled(void)
{
	int i;

	for (i = 0; i < DHCP_MAX_NETWORKS; i++)
		if (g_networks[i].network[0] != '\0' && g_networks[i].enabled)
			return 1;
	return 0;
}

static void ip_str(uint32_t be, char *out, size_t out_size)
{
	struct in_addr a;

	a.s_addr = be;
	if (inet_ntop(AF_INET, &a, out, (socklen_t)out_size) == NULL)
		snprintf(out, out_size, "%s", "");
}

static int save_state(void)
{
	char buf[16384];
	struct json_writer w;
	int rc;
	int i;

	if (g_state_path[0] == '\0')
		return -1;
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	jw_arr_open(&w);
	for (i = 0; i < DHCP_MAX_SERVERS; i++)
		if (g_servers[i][0] != '\0')
			jw_str(&w, g_servers[i]);
	jw_arr_close(&w);
	jw_key(&w, "networks");
	jw_arr_open(&w);
	for (i = 0; i < DHCP_MAX_NETWORKS; i++)
		if (g_networks[i].network[0] != '\0')
			dhcp_network_write_json(&g_networks[i], 0, &w);
	jw_arr_close(&w);
	jw_key(&w, "static");
	jw_arr_open(&w);
	for (i = 0; i < DHCP_MAX_STATIC; i++) {
		char ip[16];

		if (g_statics[i].mac[0] == '\0')
			continue;
		ip_str(g_statics[i].ip_be, ip, sizeof(ip));
		jw_obj_open(&w);
		jw_key(&w, "mac");
		jw_str(&w, g_statics[i].mac);
		jw_key(&w, "ip");
		jw_str(&w, ip);
		jw_key(&w, "hostname");
		jw_str(&w, g_statics[i].hostname);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	if (w.len >= sizeof(buf)) {
		jw_free(&w);
		return -1;
	}
	memcpy(buf, w.buf, w.len);
	rc = persist_atomic_write(g_state_path, buf, w.len);
	jw_free(&w);
	return rc;
}


/* ---- Servers ---- */

int dhcp_server_is_registered(const char *container)
{
	int i;

	if (container == NULL || container[0] == '\0')
		return 0;
	for (i = 0; i < DHCP_MAX_SERVERS; i++)
		if (g_servers[i][0] != '\0' && strcmp(g_servers[i], container) == 0)
			return 1;
	return 0;
}

int dhcp_server_list(char out[][DHCP_SERVER_NAME_MAX], int max)
{
	int i, n = 0;

	for (i = 0; i < DHCP_MAX_SERVERS && n < max; i++)
		if (g_servers[i][0] != '\0')
			snprintf(out[n++], DHCP_SERVER_NAME_MAX, "%s", g_servers[i]);
	return n;
}

enum dhcp_error dhcp_server_register(const char *container)
{
	int i, slot = -1;

	if (container == NULL || container[0] == '\0')
		return DHCP_ERR_INVALID;
	if (dhcp_server_is_registered(container))
		return DHCP_ERR_DUPLICATE;
	for (i = 0; i < DHCP_MAX_SERVERS; i++)
		if (g_servers[i][0] == '\0') {
			slot = i;
			break;
		}
	if (slot < 0)
		return DHCP_ERR_FULL;
	snprintf(g_servers[slot], DHCP_SERVER_NAME_MAX, "%s", container);
	return save_state() == 0 ? DHCP_OK : DHCP_ERR_PERSIST_FAILED;
}

/*
 * Unregistering drops the server from every range that named it. A
 * range still naming a server nobody serves from would be a slice of
 * addresses handed to nothing -- worse than one fewer server, because
 * it looks like coverage.
 */
enum dhcp_error dhcp_server_unregister(const char *container)
{
	int i, j, k;
	int found = 0;

	for (i = 0; i < DHCP_MAX_SERVERS; i++) {
		if (g_servers[i][0] == '\0' || strcmp(g_servers[i], container) != 0)
			continue;
		g_servers[i][0] = '\0';
		found = 1;
	}
	if (!found)
		return DHCP_ERR_NOT_FOUND;
	for (i = 0; i < DHCP_MAX_NETWORKS; i++) {
		if (g_networks[i].network[0] == '\0')
			continue;
		for (j = 0; j < g_networks[i].server_count; j++) {
			if (strcmp(g_networks[i].servers[j], container) != 0)
				continue;
			for (k = j; k + 1 < g_networks[i].server_count; k++)
				snprintf(g_networks[i].servers[k], DHCP_SERVER_NAME_MAX, "%s",
				         g_networks[i].servers[k + 1]);
			g_networks[i].server_count--;
			g_networks[i].servers[g_networks[i].server_count][0] = '\0';
			/* No server left to serve it: the range is off, said
			 * plainly rather than left enabled and unserved. */
			if (g_networks[i].server_count == 0)
				g_networks[i].enabled = 0;
			break;
		}
	}
	return save_state() == 0 ? DHCP_OK : DHCP_ERR_PERSIST_FAILED;
}

void dhcp_server_forget(const char *container)
{
	if (dhcp_server_is_registered(container))
		(void)dhcp_server_unregister(container);
}

void dhcp_servers_write_json(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < DHCP_MAX_SERVERS; i++) {
		struct registry_entry *e;

		if (g_servers[i][0] == '\0')
			continue;
		e = registry_find(g_servers[i]);
		jw_obj_open(w);
		jw_key(w, "container");
		jw_str(w, g_servers[i]);
		jw_key(w, "running");
		jw_bool(w, e != NULL && e->running);
		/*
		 * Whether this same container also serves DNS. Reported, not
		 * required: dnsmasq answers for the names of clients it leased
		 * addresses to, so a server that is both makes leases resolve
		 * the instant they are issued -- and one that is not, does
		 * not. Saying which is true beats quietly enforcing it.
		 */
		jw_key(w, "resolves_leases");
		jw_bool(w, dns_server_is_registered(g_servers[i]));
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

static uint32_t parse_ip(const char *s)
{
	struct in_addr a;

	if (s == NULL || inet_pton(AF_INET, s, &a) != 1)
		return 0;
	return a.s_addr;
}

int dhcp_init(const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;
	const struct json_value *arr;
	size_t i;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	memset(g_networks, 0, sizeof(g_networks));
	memset(g_statics, 0, sizeof(g_statics));
	memset(g_servers, 0, sizeof(g_servers));
	if (persist_read_file(path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return 0;

	arr = json_object_get(root, "servers");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		int slot = 0;

		for (i = 0; i < arr->u.array.count && slot < DHCP_MAX_SERVERS; i++) {
			const char *nm = json_as_string(arr->u.array.items[i]);

			if (nm == NULL || nm[0] == '\0')
				continue;
			snprintf(g_servers[slot], DHCP_SERVER_NAME_MAX, "%s", nm);
			slot++;
		}
	}
	arr = json_object_get(root, "networks");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		int slot = 0;

		for (i = 0; i < arr->u.array.count && slot < DHCP_MAX_NETWORKS; i++) {
			const struct json_value *e = arr->u.array.items[i];
			const char *name = json_as_string(json_object_get(e, "network"));
			const struct json_value *en = json_object_get(e, "enabled");

			if (name == NULL || name[0] == '\0')
				continue;
			memset(&g_networks[slot], 0, sizeof(g_networks[slot]));
			snprintf(g_networks[slot].network, sizeof(g_networks[slot].network), "%s", name);
			g_networks[slot].enabled = en != NULL && en->type == JSON_BOOL && en->u.boolean;
			g_networks[slot].range_start_be =
			    parse_ip(json_as_string(json_object_get(e, "range_start")));
			g_networks[slot].range_end_be =
			    parse_ip(json_as_string(json_object_get(e, "range_end")));
			g_networks[slot].lease_seconds =
			    (int)json_as_number(json_object_get(e, "lease_seconds"));
			g_networks[slot].router_be = parse_ip(json_as_string(json_object_get(e, "router")));
			{
				const struct json_value *srv = json_object_get(e, "servers");
				size_t k;

				if (srv != NULL && srv->type == JSON_ARRAY) {
					for (k = 0; k < srv->u.array.count &&
					            g_networks[slot].server_count < DHCP_MAX_RANGE_SERVERS; k++) {
						const char *nm = json_as_string(srv->u.array.items[k]);

						if (nm == NULL || nm[0] == '\0')
							continue;
						snprintf(g_networks[slot].servers[g_networks[slot].server_count],
						         DHCP_SERVER_NAME_MAX, "%s", nm);
						g_networks[slot].server_count++;
					}
				}
			}
			slot++;
		}
	}
	arr = json_object_get(root, "static");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		int slot = 0;

		for (i = 0; i < arr->u.array.count && slot < DHCP_MAX_STATIC; i++) {
			const struct json_value *e = arr->u.array.items[i];
			const char *mac = json_as_string(json_object_get(e, "mac"));
			const char *host = json_as_string(json_object_get(e, "hostname"));

			if (!mac_is_valid(mac))
				continue;
			memset(&g_statics[slot], 0, sizeof(g_statics[slot]));
			snprintf(g_statics[slot].mac, sizeof(g_statics[slot].mac), "%s", mac);
			g_statics[slot].ip_be = parse_ip(json_as_string(json_object_get(e, "ip")));
			if (host != NULL)
				snprintf(g_statics[slot].hostname, sizeof(g_statics[slot].hostname), "%s", host);
			slot++;
		}
	}
	json_free(root);
	return 0;
}

void dhcp_repoint(const char *path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
}

enum dhcp_error dhcp_network_set(const struct dhcp_network *cfg)
{
	struct dhcp_network *slot;
	int i;

	if (cfg == NULL || cfg->network[0] == '\0')
		return DHCP_ERR_INVALID;
	if (cfg->enabled) {
		/*
		 * Everything a range needs to be a range is required together.
		 * Accepting "enabled" with no addresses would store a setting
		 * that renders to a server which then answers nothing -- a
		 * configuration that looks on and is not.
		 */
		if (cfg->range_start_be == 0 || cfg->range_end_be == 0)
			return DHCP_ERR_INVALID;
		if (ntohl(cfg->range_start_be) > ntohl(cfg->range_end_be))
			return DHCP_ERR_INVALID;
		if (cfg->lease_seconds < 60 || cfg->lease_seconds > 30 * 24 * 3600)
			return DHCP_ERR_INVALID;
		if (cfg->server_count < 1)
			return DHCP_ERR_INVALID;
	}

	slot = net_find(cfg->network);
	if (slot == NULL) {
		for (i = 0; i < DHCP_MAX_NETWORKS; i++) {
			if (g_networks[i].network[0] == '\0') {
				slot = &g_networks[i];
				break;
			}
		}
		if (slot == NULL)
			return DHCP_ERR_FULL;
	}
	*slot = *cfg;
	return save_state() == 0 ? DHCP_OK : DHCP_ERR_PERSIST_FAILED;
}

enum dhcp_error dhcp_network_delete(const char *network)
{
	struct dhcp_network *slot = net_find(network);

	if (slot == NULL)
		return DHCP_ERR_NOT_FOUND;
	memset(slot, 0, sizeof(*slot));
	return save_state() == 0 ? DHCP_OK : DHCP_ERR_PERSIST_FAILED;
}

void dhcp_forget_network(const char *network)
{
	struct dhcp_network *slot = net_find(network);

	if (slot != NULL) {
		memset(slot, 0, sizeof(*slot));
		save_state();
	}
}

enum dhcp_error dhcp_static_add(const struct dhcp_static *entry)
{
	struct dhcp_static *slot;
	int i;

	if (entry == NULL || !mac_is_valid(entry->mac) || entry->ip_be == 0)
		return DHCP_ERR_INVALID;
	if (entry->hostname[0] != '\0' && !hostname_is_valid(entry->hostname))
		return DHCP_ERR_INVALID;
	if (static_find(entry->mac) != NULL)
		return DHCP_ERR_DUPLICATE;
	/*
	 * Two reservations for one address is not a configuration, it is a
	 * conflict waiting for both machines to be on at once. Refused
	 * here, where it is one message, rather than discovered there.
	 */
	for (i = 0; i < DHCP_MAX_STATIC; i++)
		if (g_statics[i].mac[0] != '\0' && g_statics[i].ip_be == entry->ip_be)
			return DHCP_ERR_DUPLICATE;

	slot = NULL;
	for (i = 0; i < DHCP_MAX_STATIC; i++) {
		if (g_statics[i].mac[0] == '\0') {
			slot = &g_statics[i];
			break;
		}
	}
	if (slot == NULL)
		return DHCP_ERR_FULL;
	*slot = *entry;
	return save_state() == 0 ? DHCP_OK : DHCP_ERR_PERSIST_FAILED;
}

enum dhcp_error dhcp_static_delete(const char *mac)
{
	struct dhcp_static *slot;

	if (mac == NULL)
		return DHCP_ERR_INVALID;
	slot = static_find(mac);
	if (slot == NULL)
		return DHCP_ERR_NOT_FOUND;
	memset(slot, 0, sizeof(*slot));
	return save_state() == 0 ? DHCP_OK : DHCP_ERR_PERSIST_FAILED;
}

/*
 * Server i of n gets a contiguous, disjoint slice of the range. The
 * remainder goes to the earlier servers one address each, rather than
 * all to the last -- with three servers over ten addresses that is
 * 4/3/3, not 3/3/4, and no address belongs to two servers either way.
 *
 * Disjointness is the whole safety property: two dnsmasq instances have
 * no failover protocol and no shared lease database, so the only thing
 * preventing them handing one address to two machines is that neither
 * holds it.
 */

/*
 * The servers this range names, in the order it names them. Order is
 * the slice order, so it has to be stable -- which is why the range
 * carries a list rather than the daemon deriving one from whatever
 * happens to be attached: a derived order changes when something
 * unrelated does, and every client's address changes with it.
 */
static int servers_for_network(const struct dhcp_network *cfg, char out[][DHCP_SERVER_NAME_MAX],
                                int max)
{
	int i, n = 0;

	for (i = 0; i < cfg->server_count && n < max; i++)
		if (dhcp_server_is_registered(cfg->servers[i]))
			snprintf(out[n++], DHCP_SERVER_NAME_MAX, "%s", cfg->servers[i]);
	return n;
}

int dhcp_slice_for(const struct dhcp_network *cfg, int server_index, int server_count,
                    uint32_t *out_start_be, uint32_t *out_end_be)
{
	uint32_t first = ntohl(cfg->range_start_be);
	uint32_t last = ntohl(cfg->range_end_be);
	uint32_t total;
	uint32_t base;
	uint32_t extra;
	uint32_t start;
	uint32_t size;

	if (server_count <= 1) {
		*out_start_be = cfg->range_start_be;
		*out_end_be = cfg->range_end_be;
		return 0;
	}
	if (last < first)
		return -1;
	total = last - first + 1;
	if (total < (uint32_t)server_count)
		return -1;
	base = total / (uint32_t)server_count;
	extra = total % (uint32_t)server_count;
	start = first + base * (uint32_t)server_index +
	        ((uint32_t)server_index < extra ? (uint32_t)server_index : extra);
	size = base + ((uint32_t)server_index < extra ? 1 : 0);
	*out_start_be = htonl(start);
	*out_end_be = htonl(start + size - 1);
	return 0;
}

int dhcp_render_conf(const char *server_name, char *out, size_t out_size)
{
	size_t off = 0;
	int i;
	int n;

	n = snprintf(out, out_size,
	             "# Rendered by cixd -- every edit here is overwritten.\n"
	             "# Ranges take effect only when dnsmasq starts, which is why\n"
	             "# changing one restarts this container.\n"
	             "# Only this server's own slice of each range appears here:\n"
	             "# dnsmasq has no failover protocol, so two servers on one\n"
	             "# wire are safe only because their pools do not overlap.\n"
	             "# Each range is tagged with its network name, and every\n"
	             "# option is tagged to match. dnsmasq picks a RANGE by the\n"
	             "# subnet of the interface a request arrived on, but an\n"
	             "# untagged OPTION goes to every client on every range -- so\n"
	             "# one server on two networks would hand both segments the\n"
	             "# same default gateway.\n");
	if (n < 0 || (size_t)n >= out_size)
		return -1;
	off = (size_t)n;

	for (i = 0; i < DHCP_MAX_NETWORKS; i++) {
		char start[16], end[16], router[16];
		char on_net[DHCP_MAX_RANGE_SERVERS][DHCP_SERVER_NAME_MAX];
		uint32_t slice_start, slice_end;
		int count;
		int index = -1;
		int k;

		if (g_networks[i].network[0] == '\0' || !g_networks[i].enabled)
			continue;
		count = servers_for_network(&g_networks[i], on_net, DHCP_MAX_RANGE_SERVERS);
		for (k = 0; k < count; k++)
			if (strcmp(on_net[k], server_name) == 0) {
				index = k;
				break;
			}
		/* This range does not name this server: not its range. */
		if (index < 0)
			continue;
		if (dhcp_slice_for(&g_networks[i], index, count, &slice_start, &slice_end) != 0)
			continue;
		ip_str(slice_start, start, sizeof(start));
		ip_str(slice_end, end, sizeof(end));
		/*
		 * set:<network> labels this range so its options can be
		 * addressed to it alone (dnsmasq's own "so that DHCP options
		 * may be specified on a per-network basis"). The network name
		 * is the tag: it is already unique, already what an operator
		 * reads in every other endpoint, and needs no second namespace.
		 *
		 * Without this, a server on two networks emitted two untagged
		 * dhcp-option lines, and dnsmasq sends an untagged option to
		 * every client on every range -- so whichever gateway was
		 * rendered last became the gateway for BOTH segments. The
		 * ranges themselves were always fine, because dnsmasq matches a
		 * request to a range by the subnet of the interface it arrived
		 * on; it is only the options that needed saying which network
		 * they belong to.
		 */
		n = snprintf(out + off, out_size - off, "dhcp-range=set:%s,%s,%s,%ds\n",
		             g_networks[i].network, start, end, g_networks[i].lease_seconds);
		if (n < 0 || (size_t)n >= out_size - off)
			return -1;
		off += (size_t)n;
		if (g_networks[i].router_be != 0) {
			ip_str(g_networks[i].router_be, router, sizeof(router));
			n = snprintf(out + off, out_size - off, "dhcp-option=tag:%s,3,%s\n",
			             g_networks[i].network, router);
		} else {
			/*
			 * router_be == 0 means "advertise no default route", and
			 * saying nothing does NOT mean that: dnsmasq documents its
			 * own default as sending its own address as the default
			 * route. Omitting the line therefore pointed clients at
			 * whichever container happened to be serving DHCP -- a
			 * gateway that does not route, which is a black hole rather
			 * than an absent route.
			 *
			 * Suppressing it is the documented no-data form, spelled by
			 * option NAME because that is the spelling dnsmasq's manual
			 * documents for it ("Options for which dnsmasq normally
			 * provides default values can be omitted by defining the
			 * option with no data ... --dhcp-option = option:router will
			 * result in no router option being sent"). The numeric
			 * spelling stays on the line above, where it carries a value.
			 */
			n = snprintf(out + off, out_size - off, "dhcp-option=tag:%s,option:router\n",
			             g_networks[i].network);
		}
		if (n < 0 || (size_t)n >= out_size - off)
			return -1;
		off += (size_t)n;
	}
	return (int)off;
}

int dhcp_render_hosts(char *out, size_t out_size)
{
	size_t off = 0;
	int i;
	int n;

	n = snprintf(out, out_size, "# Rendered by cixd -- every edit here is overwritten.\n");
	if (n < 0 || (size_t)n >= out_size)
		return -1;
	off = (size_t)n;

	for (i = 0; i < DHCP_MAX_STATIC; i++) {
		char ip[16];

		if (g_statics[i].mac[0] == '\0')
			continue;
		ip_str(g_statics[i].ip_be, ip, sizeof(ip));
		if (g_statics[i].hostname[0] != '\0')
			n = snprintf(out + off, out_size - off, "%s,%s,%s\n", g_statics[i].mac, ip,
			             g_statics[i].hostname);
		else
			n = snprintf(out + off, out_size - off, "%s,%s\n", g_statics[i].mac, ip);
		if (n < 0 || (size_t)n >= out_size - off)
			return -1;
		off += (size_t)n;
	}
	return (int)off;
}

/*
 * Per server, because each one's conf is now its own: what it should
 * serve depends on which networks it is attached to and how many
 * others share them. The previous conf is remembered per server so a
 * change to one network does not restart servers on another -- a
 * restart nobody asked for is exactly what this pairing with DNS makes
 * expensive.
 */
static char g_conf_in_force_name[DHCP_MAX_SERVERS][DHCP_SERVER_NAME_MAX];
static char g_conf_in_force_text[DHCP_MAX_SERVERS][4096];

static char *conf_in_force_slot(const char *name)
{
	int i;
	int free_slot = -1;

	for (i = 0; i < DHCP_MAX_SERVERS; i++) {
		if (g_conf_in_force_name[i][0] == '\0') {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (strcmp(g_conf_in_force_name[i], name) == 0)
			return g_conf_in_force_text[i];
	}
	if (free_slot < 0)
		return NULL;
	snprintf(g_conf_in_force_name[free_slot], DHCP_SERVER_NAME_MAX, "%s", name);
	g_conf_in_force_text[free_slot][0] = '\0';
	return g_conf_in_force_text[free_slot];
}

void dhcp_sync_all(char changed[][DHCP_SERVER_NAME_MAX], int max_changed, int *out_changed_count)
{
	char conf[4096];
	char hosts[8192];
	char names[DHCP_MAX_SERVERS][DHCP_SERVER_NAME_MAX];
	int count;
	int i;

	if (out_changed_count != NULL)
		*out_changed_count = 0;
	if (dhcp_render_hosts(hosts, sizeof(hosts)) < 0)
		return;

	count = dhcp_server_list(names, DHCP_MAX_SERVERS);
	for (i = 0; i < count; i++) {
		struct registry_entry *entry = registry_find(names[i]);
		char *in_force;
		char path[128];

		if (dhcp_render_conf(names[i], conf, sizeof(conf)) < 0)
			continue;
		in_force = conf_in_force_slot(names[i]);
		if (in_force != NULL && strcmp(in_force, conf) != 0) {
			snprintf(in_force, 4096, "%s", conf);
			if (changed != NULL && out_changed_count != NULL && *out_changed_count < max_changed) {
				snprintf(changed[*out_changed_count], DHCP_SERVER_NAME_MAX, "%s", names[i]);
				(*out_changed_count)++;
			}
		}
		if (entry == NULL || !entry->running)
			continue;
		/* Host-side paths, not the container's own id-mapped view, and
		 * the results are checked -- both writes used to be issued and
		 * discarded, so a DHCP server could serve a stale config
		 * indefinitely with nothing recorded (#269). */
		container_file_host_path(names[i], entry->disk_name, DHCP_CONF_PATH,
		                          path, sizeof(path));
		if (persist_atomic_write(path, conf, strlen(conf)) != 0)
			logstore_write("cixd", "error", "dhcp: could not write %s config to %s",
			                names[i], path);
		container_file_host_path(names[i], entry->disk_name, DHCP_HOSTS_PATH,
		                          path, sizeof(path));
		if (persist_atomic_write(path, hosts, strlen(hosts)) != 0)
			logstore_write("cixd", "error", "dhcp: could not write %s hosts to %s",
			                names[i], path);
		/* Puts the hosts file into force immediately. The conf file is
		 * untouched by this signal -- see the header. */
		sys_pidfd_send_signal(entry->handle.pidfd, SIGHUP);
	}
}

void dhcp_network_write_json(const struct dhcp_network *cfg, int include_slices,
                              struct json_writer *w)
{
	char buf[16];

	jw_obj_open(w);
	jw_key(w, "network");
	jw_str(w, cfg->network);
	jw_key(w, "enabled");
	jw_bool(w, cfg->enabled);
	jw_key(w, "range_start");
	if (cfg->range_start_be != 0) {
		ip_str(cfg->range_start_be, buf, sizeof(buf));
		jw_str(w, buf);
	} else {
		jw_null(w);
	}
	jw_key(w, "range_end");
	if (cfg->range_end_be != 0) {
		ip_str(cfg->range_end_be, buf, sizeof(buf));
		jw_str(w, buf);
	} else {
		jw_null(w);
	}
	jw_key(w, "lease_seconds");
	jw_int(w, cfg->lease_seconds);
	jw_key(w, "servers");
	jw_arr_open(w);
	{
		int si;

		for (si = 0; si < cfg->server_count; si++)
			jw_str(w, cfg->servers[si]);
	}
	jw_arr_close(w);
	jw_key(w, "router");
	if (cfg->router_be != 0) {
		ip_str(cfg->router_be, buf, sizeof(buf));
		jw_str(w, buf);
	} else {
		jw_null(w);
	}
	if (include_slices) {
		char names[DHCP_MAX_RANGE_SERVERS][DHCP_SERVER_NAME_MAX];
		int count = servers_for_network(cfg, names, DHCP_MAX_RANGE_SERVERS);
		int i;

		jw_key(w, "slices");
		jw_arr_open(w);
		for (i = 0; cfg->enabled && i < count; i++) {
			uint32_t start_be, end_be;
			char a[16], b[16];

			if (dhcp_slice_for(cfg, i, count, &start_be, &end_be) != 0)
				continue;
			ip_str(start_be, a, sizeof(a));
			ip_str(end_be, b, sizeof(b));
			jw_obj_open(w);
			jw_key(w, "server");
			jw_str(w, names[i]);
			jw_key(w, "range_start");
			jw_str(w, a);
			jw_key(w, "range_end");
			jw_str(w, b);
			jw_obj_close(w);
		}
		jw_arr_close(w);
	}
	jw_obj_close(w);
}

void dhcp_write_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "networks");
	jw_arr_open(w);
	for (i = 0; i < DHCP_MAX_NETWORKS; i++)
		if (g_networks[i].network[0] != '\0')
			dhcp_network_write_json(&g_networks[i], 1, w);
	jw_arr_close(w);
	jw_key(w, "static");
	jw_arr_open(w);
	for (i = 0; i < DHCP_MAX_STATIC; i++) {
		char ip[16];

		if (g_statics[i].mac[0] == '\0')
			continue;
		ip_str(g_statics[i].ip_be, ip, sizeof(ip));
		jw_obj_open(w);
		jw_key(w, "mac");
		jw_str(w, g_statics[i].mac);
		jw_key(w, "ip");
		jw_str(w, ip);
		jw_key(w, "hostname");
		if (g_statics[i].hostname[0] != '\0')
			jw_str(w, g_statics[i].hostname);
		else
			jw_null(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

/*
 * dnsmasq's lease file: "<expiry> <mac> <ip> <hostname> <client-id>",
 * one per line, hostname "*" when the client gave none. Parsed rather
 * than stored: these are the server's own records of what it handed
 * out, and a copy of them here would be a second answer to a question
 * only the server can answer.
 */
void dhcp_leases_write_json(struct json_writer *w)
{
	char names[DHCP_MAX_SERVERS][DHCP_SERVER_NAME_MAX];
	int count = dhcp_server_list(names, DHCP_MAX_SERVERS);
	int i;

	jw_arr_open(w);
	for (i = 0; i < count; i++) {
		struct registry_entry *entry = registry_find(names[i]);
		char path[128];
		char line[512];
		FILE *f;

		if (entry == NULL || !entry->running)
			continue;
		/* A READ, but it must look where the writes go (#269). */
		container_file_host_path(names[i], entry->disk_name, DHCP_LEASE_PATH,
		                          path, sizeof(path));
		f = fopen(path, "r");
		if (f == NULL)
			continue;
		while (fgets(line, sizeof(line), f) != NULL) {
			long long expiry;
			char mac[64], ip[64], host[128];

			if (sscanf(line, "%lld %63s %63s %127s", &expiry, mac, ip, host) < 4)
				continue;
			jw_obj_open(w);
			jw_key(w, "server");
			jw_str(w, names[i]);
			jw_key(w, "mac");
			jw_str(w, mac);
			jw_key(w, "ip");
			jw_str(w, ip);
			jw_key(w, "hostname");
			if (strcmp(host, "*") != 0)
				jw_str(w, host);
			else
				jw_null(w);
			jw_key(w, "expires_at");
			jw_int(w, expiry);
			jw_obj_close(w);
		}
		fclose(f);
	}
	jw_arr_close(w);
}
