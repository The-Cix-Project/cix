#include "dhcp.h"
#include "containerpath.h"
#include "logstore.h"
#include "dns.h"
#include "persist.h"
#include "registry.h"
#include "network.h"
#include "events.h"
#include "dhcprelay.h"
#include "dhcpdconf.h"
#include "iohelpers.h"
#include "linux_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>

static struct dhcp_network g_networks[DHCP_MAX_NETWORKS];
static struct dhcp_static g_statics[DHCP_MAX_STATIC];
static char g_servers[DHCP_MAX_SERVERS][DHCP_SERVER_NAME_MAX];
/* ADR-0383: registered relays, and the service each runs the relay as. */
static char g_relays[DHCP_MAX_RELAYS][DHCP_SERVER_NAME_MAX];
static char g_relay_services[DHCP_MAX_RELAYS][DHCP_SERVICE_NAME_MAX];
static char g_state_path[512];

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
 * A hostname here is sent to the client as its host-name option, and
 * is held to the shape a DNS label has: letters, digits and hyphens, not
 * starting or ending with one. Refused rather than sanitised -- this
 * string is written into a config file the server parses.
 *
 * LOWER-CASE ONLY, which this comment used to omit and the 400 used to
 * omit too -- both said "letters" while the loop below accepts 'a'-'z'
 * and nothing above it. A DNS label is case-insensitive, so refusing
 * "Printer" is this implementation's choice rather than the standard's;
 * it is kept because the stored string is written verbatim into
 * dhcpd's config and a single spelling is one less thing to compare.
 * Stated here because the contract now declares the same pattern and a
 * generated form refuses what this refuses (ADR-0338, cix#596).
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

static int addr_in_range(uint32_t ip_be, uint32_t start_be, uint32_t end_be)
{
	uint32_t ip = ntohl(ip_be);

	return ip >= ntohl(start_be) && ip <= ntohl(end_be);
}

static int addr_in_net(uint32_t ip_be, uint32_t base_be, int prefix_len)
{
	uint32_t mask = prefix_len <= 0    ? 0
	                : prefix_len >= 32 ? 0xffffffffu
	                                   : 0xffffffffu << (32 - prefix_len);

	return (ntohl(ip_be) & mask) == (ntohl(base_be) & mask);
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
	/* ADR-0383. Optional on read, so an older cixd after boot-next
	 * ignores it (and drops it at its next save). */
	jw_key(&w, "relays");
	jw_arr_open(&w);
	for (i = 0; i < DHCP_MAX_RELAYS; i++) {
		if (g_relays[i][0] == '\0')
			continue;
		jw_obj_open(&w);
		jw_key(&w, "container");
		jw_str(&w, g_relays[i]);
		jw_key(&w, "service");
		jw_str(&w, g_relay_services[i]);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_key(&w, "networks");
	jw_arr_open(&w);
	for (i = 0; i < DHCP_MAX_NETWORKS; i++)
		if (g_networks[i].network[0] != '\0')
			dhcp_network_write_json(&g_networks[i], &w);
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
	if (dhcp_relay_is_registered(container))
		return DHCP_ERR_CONFLICT;
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
 * range still naming a server nobody serves from would be a pool half
 * served by nothing -- worse than one fewer server, because it looks
 * like coverage.
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

/* 1 when the file at path is absent or holds something other than
 * want. Every rendered file is compared before it is written, so a
 * resync after every container start rewrites, and reloads or restarts,
 * nothing that has not changed (#697, ADR-0383). */
static int file_differs(const char *path, const char *want)
{
	char *cur = NULL;
	size_t cur_len = 0, len = strlen(want);
	int same = persist_read_file(path, &cur, &cur_len) == 0 && cur != NULL && cur_len == len &&
	           memcmp(cur, want, len) == 0;

	free(cur);
	return !same;
}

/* ---- Relays (ADR-0383) ---- */

int dhcp_relay_is_registered(const char *container)
{
	return dhcp_relay_service(container) != NULL;
}

const char *dhcp_relay_service(const char *container)
{
	int i;

	if (container == NULL || container[0] == '\0')
		return NULL;
	for (i = 0; i < DHCP_MAX_RELAYS; i++)
		if (g_relays[i][0] != '\0' && strcmp(g_relays[i], container) == 0)
			return g_relay_services[i];
	return NULL;
}

int dhcp_relay_list(char out[][DHCP_SERVER_NAME_MAX], int max)
{
	int i, n = 0;

	for (i = 0; i < DHCP_MAX_RELAYS && n < max; i++)
		if (g_relays[i][0] != '\0')
			snprintf(out[n++], DHCP_SERVER_NAME_MAX, "%s", g_relays[i]);
	return n;
}

static int service_name_is_valid(const char *s)
{
	size_t i, n = strlen(s);

	if (n == 0 || n >= DHCP_SERVICE_NAME_MAX)
		return 0;
	for (i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
		      (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_' || s[i] == '.'))
			return 0;
	return 1;
}

enum dhcp_error dhcp_relay_register(const char *container, const char *service)
{
	int i, slot = -1;

	if (container == NULL || container[0] == '\0')
		return DHCP_ERR_INVALID;
	if (service == NULL || service[0] == '\0')
		service = DHCP_RELAY_SERVICE_DEFAULT;
	if (!service_name_is_valid(service))
		return DHCP_ERR_INVALID;
	if (dhcp_relay_is_registered(container))
		return DHCP_ERR_DUPLICATE;
	if (dhcp_server_is_registered(container))
		return DHCP_ERR_CONFLICT;
	for (i = 0; i < DHCP_MAX_RELAYS; i++)
		if (g_relays[i][0] == '\0') {
			slot = i;
			break;
		}
	if (slot < 0)
		return DHCP_ERR_FULL;
	snprintf(g_relays[slot], DHCP_SERVER_NAME_MAX, "%s", container);
	snprintf(g_relay_services[slot], DHCP_SERVICE_NAME_MAX, "%s", service);
	return save_state() == 0 ? DHCP_OK : DHCP_ERR_PERSIST_FAILED;
}

/* A container's attachments: self when the caller has them (the create
 * path, before the registry holds the container), else the registry's.
 * 0 attachments when there is neither. */
static int attachments_of(const char *container, const struct registry_network_attachment *self,
                          int self_count, const struct registry_network_attachment **out)
{
	struct registry_entry *e;

	if (self != NULL) {
		*out = self;
		return self_count;
	}
	e = registry_find(container);
	if (e == NULL || !e->in_use) {
		*out = NULL;
		return 0;
	}
	*out = e->nets;
	return e->net_count;
}

static int attached_to(const char *container, const struct registry_network_attachment *self,
                       int self_count, const char *network)
{
	const struct registry_network_attachment *atts;
	int n = attachments_of(container, self, self_count, &atts);
	int j;

	for (j = 0; j < n; j++)
		if (strcmp(atts[j].name, network) == 0)
			return 1;
	return 0;
}

static int container_on_network(const char *container, const char *network)
{
	return attached_to(container, NULL, 0, network);
}

/*
 * Unregistering drops the relay from every range that named it. A range
 * left with no relay while it names a server not on its network has a
 * server that can hear nothing there: it is turned off, said plainly, as
 * an unregistered server's last range is (ADR-0197).
 */
enum dhcp_error dhcp_relay_unregister(const char *container)
{
	int i, j, k, s;
	int found = 0;

	for (i = 0; i < DHCP_MAX_RELAYS; i++) {
		if (g_relays[i][0] == '\0' || strcmp(g_relays[i], container) != 0)
			continue;
		g_relays[i][0] = '\0';
		g_relay_services[i][0] = '\0';
		found = 1;
	}
	if (!found)
		return DHCP_ERR_NOT_FOUND;
	for (i = 0; i < DHCP_MAX_NETWORKS; i++) {
		struct dhcp_network *n = &g_networks[i];
		int dropped = 0;

		if (n->network[0] == '\0')
			continue;
		for (j = 0; j < n->relay_count; j++) {
			if (strcmp(n->relays[j], container) != 0)
				continue;
			for (k = j; k + 1 < n->relay_count; k++)
				snprintf(n->relays[k], DHCP_SERVER_NAME_MAX, "%s", n->relays[k + 1]);
			n->relay_count--;
			n->relays[n->relay_count][0] = '\0';
			dropped = 1;
			break;
		}
		if (dropped && n->relay_count == 0)
			for (s = 0; s < n->server_count; s++)
				if (!container_on_network(n->servers[s], n->network))
					n->enabled = 0;
	}
	return save_state() == 0 ? DHCP_OK : DHCP_ERR_PERSIST_FAILED;
}

void dhcp_relay_forget(const char *container)
{
	if (dhcp_relay_is_registered(container))
		(void)dhcp_relay_unregister(container);
}

void dhcp_relays_write_json(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < DHCP_MAX_RELAYS; i++) {
		struct registry_entry *e;

		if (g_relays[i][0] == '\0')
			continue;
		e = registry_find(g_relays[i]);
		jw_obj_open(w);
		jw_key(w, "container");
		jw_str(w, g_relays[i]);
		jw_key(w, "service");
		jw_str(w, g_relay_services[i]);
		jw_key(w, "running");
		jw_bool(w, e != NULL && e->running);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

/* A container's attachments, as dhcprelay.h's facts. -1 when it has
 * none to give. Routes are not filled: only the check reads them, and
 * main.c gives it those from the definition. */
static int host_facts(const char *name, const struct registry_network_attachment *self,
                      int self_count, struct dhcprelay_host *h)
{
	const struct registry_network_attachment *atts;
	int n = attachments_of(name, self, self_count, &atts);
	int j;

	memset(h, 0, sizeof(*h));
	snprintf(h->name, sizeof(h->name), "%s", name);
	if (n == 0)
		return -1;
	for (j = 0; j < n && h->att_count < DHCPRELAY_MAX_ATT; j++) {
		const struct network_def *nd = network_find(atts[j].name);
		struct dhcprelay_att *a = &h->att[h->att_count++];

		snprintf(a->network, sizeof(a->network), "%s", atts[j].name);
		a->ip_be = atts[j].ip_be;
		a->prefix_len = nd != NULL ? nd->prefix_len : 32;
		snprintf(a->ifname, sizeof(a->ifname), "%s", atts[j].ifname);
	}
	return 0;
}

struct relay_hosts {
	struct dhcprelay_host hosts[DHCP_MAX_SERVERS];
	int known[DHCP_MAX_SERVERS];
	int count;
};

static const struct dhcprelay_host *relay_find_host(const char *name, void *ctx)
{
	struct relay_hosts *rh = ctx;
	int i;

	for (i = 0; i < rh->count; i++)
		if (strcmp(rh->hosts[i].name, name) == 0)
			return rh->known[i] ? &rh->hosts[i] : NULL;
	if (rh->count >= DHCP_MAX_SERVERS)
		return NULL;
	i = rh->count++;
	rh->known[i] = host_facts(name, NULL, 0, &rh->hosts[i]) == 0;
	return rh->known[i] ? &rh->hosts[i] : NULL;
}

int dhcp_render_relay_conf(const char *relay_name, const struct registry_network_attachment *self,
                           int self_count, char *out, size_t out_size)
{
	static struct dhcprelay_range ranges[DHCP_MAX_NETWORKS];
	static struct relay_hosts rh;
	struct dhcprelay_host relay;
	int i, k, count = 0;

	memset(&rh, 0, sizeof(rh));
	(void)host_facts(relay_name, self, self_count, &relay);
	for (i = 0; i < DHCP_MAX_NETWORKS; i++) {
		const struct dhcp_network *n = &g_networks[i];
		struct dhcprelay_range *r;

		if (n->network[0] == '\0')
			continue;
		r = &ranges[count++];
		memset(r, 0, sizeof(*r));
		snprintf(r->network, sizeof(r->network), "%s", n->network);
		r->enabled = n->enabled;
		for (k = 0; k < n->server_count && k < DHCPRELAY_MAX_LIST; k++)
			snprintf(r->servers[r->server_count++], DHCPRELAY_NAME_MAX, "%s", n->servers[k]);
		for (k = 0; k < n->relay_count && k < DHCPRELAY_MAX_LIST; k++)
			snprintf(r->relays[r->relay_count++], DHCPRELAY_NAME_MAX, "%s", n->relays[k]);
	}
	return dhcprelay_render(&relay, ranges, count, relay_find_host, &rh, out, out_size);
}

/*
 * The relay's file inside the container is the record of what its
 * service started with: it is staged at create and rewritten only here,
 * and every rewrite is followed by a restart of that service. So a sync
 * compares against the file, and a daemon restart or handover restarts
 * nothing that has not changed.
 */
void dhcp_relays_sync(char changed[][DHCP_SERVER_NAME_MAX], int max_changed,
                      int *out_changed_count)
{
	char names[DHCP_MAX_RELAYS][DHCP_SERVER_NAME_MAX];
	char conf[4096];
	int count, i;

	if (out_changed_count != NULL)
		*out_changed_count = 0;
	count = dhcp_relay_list(names, DHCP_MAX_RELAYS);
	for (i = 0; i < count; i++) {
		struct registry_entry *entry = registry_find(names[i]);
		char path[PATH_MAX];

		if (entry == NULL || !entry->running)
			continue;
		if (dhcp_render_relay_conf(names[i], NULL, 0, conf, sizeof(conf)) < 0) {
			logstore_write("cixd", "error",
			               "dhcp: relay %s's configuration does not fit; left as it was",
			               names[i]);
			continue;
		}
		container_file_host_path(names[i], DHCP_RELAY_CONF_PATH, path, sizeof(path));
		if (!file_differs(path, conf))
			continue;
		if (persist_atomic_write(path, conf, strlen(conf)) != 0) {
			logstore_write("cixd", "error", "dhcp: could not write relay %s's config to %s",
			               names[i], path);
			continue;
		}
		if (changed != NULL && out_changed_count != NULL && *out_changed_count < max_changed) {
			snprintf(changed[*out_changed_count], DHCP_SERVER_NAME_MAX, "%s", names[i]);
			(*out_changed_count)++;
		}
	}
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
	memset(g_relays, 0, sizeof(g_relays));
	memset(g_relay_services, 0, sizeof(g_relay_services));
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
	arr = json_object_get(root, "relays");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		int slot = 0;

		for (i = 0; i < arr->u.array.count && slot < DHCP_MAX_RELAYS; i++) {
			const struct json_value *e = arr->u.array.items[i];
			const char *nm = json_as_string(json_object_get(e, "container"));
			const char *svc = json_as_string(json_object_get(e, "service"));

			if (nm == NULL || nm[0] == '\0')
				continue;
			snprintf(g_relays[slot], DHCP_SERVER_NAME_MAX, "%s", nm);
			snprintf(g_relay_services[slot], DHCP_SERVICE_NAME_MAX, "%s",
			         svc != NULL && svc[0] != '\0' ? svc : DHCP_RELAY_SERVICE_DEFAULT);
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
				/* ADR-0386. Absent in a state file an older cixd
				 * wrote, which reads as none. */
				const struct json_value *jd = json_object_get(e, "dns_servers");
				size_t k;

				for (k = 0; jd != NULL && jd->type == JSON_ARRAY && k < jd->u.array.count &&
				            g_networks[slot].dns_count < DHCP_MAX_DNS;
				     k++) {
					uint32_t a = parse_ip(json_as_string(jd->u.array.items[k]));

					if (a != 0)
						g_networks[slot].dns_be[g_networks[slot].dns_count++] = a;
				}
			}
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
			{
				const struct json_value *rl = json_object_get(e, "relays");
				size_t k;

				if (rl != NULL && rl->type == JSON_ARRAY) {
					for (k = 0; k < rl->u.array.count &&
					            g_networks[slot].relay_count < DHCP_MAX_RANGE_RELAYS; k++) {
						const char *nm = json_as_string(rl->u.array.items[k]);

						if (nm == NULL || nm[0] == '\0')
							continue;
						snprintf(g_networks[slot].relays[g_networks[slot].relay_count],
						         DHCP_SERVER_NAME_MAX, "%s", nm);
						g_networks[slot].relay_count++;
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
		if (cfg->server_count < 1 || cfg->server_count > DHCP_MAX_RANGE_SERVERS)
			return DHCP_ERR_INVALID;
		/* ADR-0386: a reservation is a fixed-address host, which dhcpd
		 * gives its address without a lease, so none may sit in a pool. */
		for (i = 0; i < DHCP_MAX_STATIC; i++)
			if (g_statics[i].mac[0] != '\0' &&
			    addr_in_range(g_statics[i].ip_be, cfg->range_start_be, cfg->range_end_be))
				return DHCP_ERR_IN_POOL;
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
	/* ADR-0386: never inside a range's pool, enabled or not -- enabling
	 * it later would make the same collision. */
	for (i = 0; i < DHCP_MAX_NETWORKS; i++)
		if (g_networks[i].network[0] != '\0' && g_networks[i].range_start_be != 0 &&
		    addr_in_range(entry->ip_be, g_networks[i].range_start_be,
		                  g_networks[i].range_end_be))
			return DHCP_ERR_IN_POOL;

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

/* ---- ADR-0386: servers, pairs and dhcpd's file ---- */

/* The registered servers this range names, in the order it names them. */
static int servers_for_network(const struct dhcp_network *cfg, char out[][DHCP_SERVER_NAME_MAX],
                                int max)
{
	int i, n = 0;

	for (i = 0; i < cfg->server_count && n < max; i++)
		if (dhcp_server_is_registered(cfg->servers[i]))
			snprintf(out[n++], DHCP_SERVER_NAME_MAX, "%s", cfg->servers[i]);
	return n;
}

/* The other server of an enabled two-server range naming this one, or
 * NULL. A server is in at most one pair (dhcp_pair_refused()), so the
 * first found is the only one. */
static const char *pair_partner(const char *server)
{
	int i;

	for (i = 0; i < DHCP_MAX_NETWORKS; i++) {
		const struct dhcp_network *n = &g_networks[i];

		if (n->network[0] == '\0' || !n->enabled || n->server_count != 2)
			continue;
		if (strcmp(n->servers[0], server) == 0)
			return n->servers[1];
		if (strcmp(n->servers[1], server) == 0)
			return n->servers[0];
	}
	return NULL;
}

/*
 * The pair's peer channel: the first network, in the PRIMARY's
 * attachment order, that both are attached to, and each one's address
 * there. The primary is the name that sorts first, so both servers
 * compute the same answer. me's attachments may be given (the create
 * path); the other's are the registry's. 0, or -1 when they share none.
 */
static int pair_addresses(const char *me, const char *peer,
                          const struct registry_network_attachment *self, int self_count,
                          uint32_t *my_be, uint32_t *peer_be)
{
	const struct registry_network_attachment *mine, *theirs, *pri;
	int nm = attachments_of(me, self, self_count, &mine);
	int np = attachments_of(peer, NULL, 0, &theirs);
	int me_primary = strcmp(me, peer) < 0;
	int npri = me_primary ? nm : np;
	int i, j;

	pri = me_primary ? mine : theirs;
	for (i = 0; i < npri; i++) {
		const struct registry_network_attachment *other = me_primary ? theirs : mine;
		int nother = me_primary ? np : nm;

		for (j = 0; j < nother; j++) {
			if (strcmp(other[j].name, pri[i].name) != 0)
				continue;
			if (pri[i].ip_be == 0 || other[j].ip_be == 0)
				continue;
			*my_be = me_primary ? pri[i].ip_be : other[j].ip_be;
			*peer_be = me_primary ? other[j].ip_be : pri[i].ip_be;
			return 0;
		}
	}
	return -1;
}

int dhcp_pair_refused(const struct dhcp_network *cfg, char *err, size_t err_size)
{
	uint32_t a, b;
	int i, k;

	if (cfg->server_count > DHCP_MAX_RANGE_SERVERS) {
		snprintf(err, err_size,
		         "a range is served by one server, or by two forming a failover pair -- not %d "
		         "(ADR-0386)",
		         cfg->server_count);
		return 1;
	}
	if (cfg->server_count != 2)
		return 0;
	if (strcmp(cfg->servers[0], cfg->servers[1]) == 0) {
		snprintf(err, err_size, "a failover pair is two different servers");
		return 1;
	}
	for (i = 0; i < DHCP_MAX_NETWORKS; i++) {
		const struct dhcp_network *n = &g_networks[i];

		if (n->network[0] == '\0' || !n->enabled || n->server_count != 2 ||
		    strcmp(n->network, cfg->network) == 0)
			continue;
		for (k = 0; k < 2; k++) {
			const char *s = cfg->servers[k];
			const char *want = cfg->servers[1 - k];
			const char *has = NULL;

			if (strcmp(n->servers[0], s) == 0)
				has = n->servers[1];
			else if (strcmp(n->servers[1], s) == 0)
				has = n->servers[0];
			if (has != NULL && strcmp(has, want) != 0) {
				snprintf(err, err_size,
				         "\"%s\" already pairs with \"%s\" on network \"%s\" -- a server is in "
				         "at most one failover pair (ADR-0386)",
				         s, has, n->network);
				return 1;
			}
		}
	}
	if (pair_addresses(cfg->servers[0], cfg->servers[1], NULL, 0, &a, &b) != 0) {
		snprintf(err, err_size,
		         "\"%s\" and \"%s\" share no network with an address on both, so they have no "
		         "channel to talk failover over -- attach both to one network",
		         cfg->servers[0], cfg->servers[1]);
		return 1;
	}
	return 0;
}

int dhcp_render_conf(const char *server_name, const struct registry_network_attachment *self,
                     int self_count, char *out, size_t out_size)
{
	static struct dhcpd_conf c;
	const struct registry_network_attachment *atts;
	const char *partner = pair_partner(server_name);
	int natts;
	int i, k;

	memset(&c, 0, sizeof(c));
	if (partner != NULL && dhcp_server_is_registered(partner) &&
	    pair_addresses(server_name, partner, self, self_count, &c.my_addr_be,
	                   &c.peer_addr_be) == 0) {
		c.failover = 1;
		c.primary = strcmp(server_name, partner) < 0;
	}

	/* Every range naming this server, with its pool when it can be
	 * served: a pair's range only with the failover peer resolved. */
	for (i = 0; i < DHCP_MAX_NETWORKS && c.subnet_count < DHCPD_MAX_SUBNETS; i++) {
		const struct dhcp_network *n = &g_networks[i];
		char names[DHCP_MAX_RANGE_SERVERS][DHCP_SERVER_NAME_MAX];
		const struct network_def *nd;
		struct dhcpd_subnet *s;
		int count, mine = 0;

		if (n->network[0] == '\0' || !n->enabled)
			continue;
		count = servers_for_network(n, names, DHCP_MAX_RANGE_SERVERS);
		for (k = 0; k < count; k++)
			if (strcmp(names[k], server_name) == 0)
				mine = 1;
		nd = network_find(n->network);
		if (!mine || nd == NULL)
			continue;
		s = &c.subnets[c.subnet_count++];
		s->net_be = nd->base_be;
		s->prefix_len = nd->prefix_len;
		if (count == 2 && !c.failover)
			continue; /* declared, no pool: never two pools with no failover */
		s->has_pool = 1;
		s->failover = count == 2;
		s->start_be = n->range_start_be;
		s->end_be = n->range_end_be;
		s->router_be = n->router_be;
		s->lease_seconds = n->lease_seconds;
		for (k = 0; k < n->dns_count && k < DHCPD_MAX_DNS; k++)
			s->dns_be[s->dns_count++] = n->dns_be[k];
	}

	/* Its own networks, declared with no pool: dhcpd listens only on an
	 * interface whose subnet is declared, and a relayed request reaches
	 * it on one of these. */
	natts = attachments_of(server_name, self, self_count, &atts);
	for (i = 0; i < natts && c.subnet_count < DHCPD_MAX_SUBNETS; i++) {
		const struct network_def *nd = network_find(atts[i].name);
		int seen = 0;

		if (nd == NULL)
			continue;
		for (k = 0; k < c.subnet_count; k++)
			if (c.subnets[k].net_be == nd->base_be && c.subnets[k].prefix_len == nd->prefix_len)
				seen = 1;
		if (seen)
			continue;
		c.subnets[c.subnet_count].net_be = nd->base_be;
		c.subnets[c.subnet_count].prefix_len = nd->prefix_len;
		c.subnet_count++;
	}

	/* The reservations on a network this server serves a pool on. */
	for (i = 0; i < DHCP_MAX_STATIC && c.host_count < DHCPD_MAX_HOSTS; i++) {
		const struct dhcp_static *st = &g_statics[i];

		if (st->mac[0] == '\0')
			continue;
		for (k = 0; k < c.subnet_count; k++) {
			struct dhcpd_host *h;

			if (!c.subnets[k].has_pool ||
			    !addr_in_net(st->ip_be, c.subnets[k].net_be, c.subnets[k].prefix_len))
				continue;
			h = &c.hosts[c.host_count++];
			snprintf(h->mac, sizeof(h->mac), "%s", st->mac);
			h->ip_be = st->ip_be;
			snprintf(h->name, sizeof(h->name), "%s", st->hostname);
			break;
		}
	}
	return dhcpd_render(&c, out, out_size);
}

int dhcp_seed_lease_file(const char *volume_host_dir)
{
	char path[PATH_MAX];
	int fd;

	if (snprintf(path, sizeof(path), "%s/%s", volume_host_dir, DHCP_LEASE_FILE) >=
	    (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0)
		return errno == EEXIST ? 0 : -1;
	close(fd);
	return 0;
}

/*
 * The file inside the container is the record of what its dhcpd started
 * with: it is staged at create and rewritten only here, and every caller
 * that passes `changed` restarts what it lists. So a sync compares
 * against the file, as the relays' does, and a daemon restart or
 * handover restarts nothing that has not changed.
 */
void dhcp_sync_all(char changed[][DHCP_SERVER_NAME_MAX], int max_changed, int *out_changed_count)
{
	static char conf[DHCP_CONF_RENDER_MAX];
	char names[DHCP_MAX_SERVERS][DHCP_SERVER_NAME_MAX];
	int count;
	int i;

	if (out_changed_count != NULL)
		*out_changed_count = 0;
	count = dhcp_server_list(names, DHCP_MAX_SERVERS);
	for (i = 0; i < count; i++) {
		struct registry_entry *entry = registry_find(names[i]);
		char path[PATH_MAX];

		if (entry == NULL || !entry->running)
			continue;
		if (dhcp_render_conf(names[i], NULL, 0, conf, sizeof(conf)) < 0) {
			logstore_write("cixd", "error",
			               "dhcp: server %s's configuration does not fit; left as it was",
			               names[i]);
			continue;
		}
		/* Host-side path, not the container's own id-mapped view (#269). */
		container_file_host_path(names[i], DHCP_CONF_PATH, path, sizeof(path));
		if (!file_differs(path, conf))
			continue;
		if (persist_atomic_write(path, conf, strlen(conf)) != 0) {
			logstore_write("cixd", "error", "dhcp: could not write %s's config to %s", names[i],
			               path);
			continue;
		}
		if (changed != NULL && out_changed_count != NULL && *out_changed_count < max_changed) {
			snprintf(changed[*out_changed_count], DHCP_SERVER_NAME_MAX, "%s", names[i]);
			(*out_changed_count)++;
		}
	}
}

void dhcp_network_write_json(const struct dhcp_network *cfg, struct json_writer *w)
{
	char buf[16];
	int k;

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
	for (k = 0; k < cfg->server_count; k++)
		jw_str(w, cfg->servers[k]);
	jw_arr_close(w);
	jw_key(w, "relays");
	jw_arr_open(w);
	for (k = 0; k < cfg->relay_count; k++)
		jw_str(w, cfg->relays[k]);
	jw_arr_close(w);
	jw_key(w, "router");
	if (cfg->router_be != 0) {
		ip_str(cfg->router_be, buf, sizeof(buf));
		jw_str(w, buf);
	} else {
		jw_null(w);
	}
	jw_key(w, "dns_servers");
	jw_arr_open(w);
	for (k = 0; k < cfg->dns_count; k++) {
		ip_str(cfg->dns_be[k], buf, sizeof(buf));
		jw_str(w, buf);
	}
	jw_arr_close(w);
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
			dhcp_network_write_json(&g_networks[i], w);
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

/* ---- Leases (ADR-0386 decision 7) ---- */

/* The path a running server's lease file has from the daemon: through
 * its own mount namespace, which is the only view that sees the volume
 * it is on (#357, #698). -1 when the server is not running. */
static int lease_path_of(const char *server, char *out, size_t out_size)
{
	struct registry_entry *e = registry_find(server);

	if (e == NULL || !e->running || e->handle.pid <= 0)
		return -1;
	snprintf(out, out_size, "/proc/%d/root%s", (int)e->handle.pid, DHCP_LEASE_PATH);
	return 0;
}

static void write_lease_json(struct json_writer *w, const struct dhcpd_lease *l)
{
	char ip[16];

	ip_str(l->ip_be, ip, sizeof(ip));
	jw_obj_open(w);
	jw_key(w, "mac");
	jw_str(w, l->mac);
	jw_key(w, "ip");
	jw_str(w, ip);
	jw_key(w, "hostname");
	if (l->hostname[0] != '\0')
		jw_str(w, l->hostname);
	else
		jw_null(w);
	jw_key(w, "expires_at");
	if (l->ends > 0)
		jw_int(w, l->ends);
	else
		jw_null(w); /* "never" */
	jw_obj_close(w);
}

int dhcp_leases_scan(int fd, long long now)
{
	char names[DHCP_MAX_SERVERS][DHCP_SERVER_NAME_MAX];
	struct dhcpd_leases all;
	struct json_writer w;
	int count = dhcp_server_list(names, DHCP_MAX_SERVERS);
	int i, rc;

	memset(&all, 0, sizeof(all));
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	jw_arr_open(&w);
	for (i = 0; i < count; i++) {
		struct dhcpd_leases one;
		char path[PATH_MAX];
		char *text = NULL;
		size_t len = 0;

		if (lease_path_of(names[i], path, sizeof(path)) != 0)
			continue;
		if (persist_read_file(path, &text, &len) != 0 || text == NULL) {
			free(text);
			continue;
		}
		rc = dhcpd_leases_parse(text, len, &one);
		free(text);
		if (rc != 0) {
			dhcpd_leases_free(&one);
			continue;
		}
		jw_obj_open(&w);
		jw_key(&w, "container");
		jw_str(&w, names[i]);
		jw_key(&w, "state");
		if (one.my_state[0] != '\0')
			jw_str(&w, one.my_state);
		else
			jw_null(&w);
		jw_key(&w, "partner_state");
		if (one.partner_state[0] != '\0')
			jw_str(&w, one.partner_state);
		else
			jw_null(&w);
		jw_obj_close(&w);
		rc = dhcpd_leases_merge(&all, &one);
		dhcpd_leases_free(&one);
		if (rc != 0)
			break;
	}
	jw_arr_close(&w);
	jw_key(&w, "leases");
	jw_arr_open(&w);
	for (i = 0; i < all.count; i++)
		if (dhcpd_lease_is_current(&all.v[i], now))
			write_lease_json(&w, &all.v[i]);
	jw_arr_close(&w);
	jw_obj_close(&w);
	dhcpd_leases_free(&all);
	rc = cix_write_all(fd, w.buf, w.len);
	jw_free(&w);
	return rc == 0 ? 0 : -1;
}

/* What each server's lease file was at the last scan that ran. */
static struct {
	char name[DHCP_SERVER_NAME_MAX];
	long long size;
	long long mtime_ns;
} g_lease_stat[DHCP_MAX_SERVERS];

int dhcp_lease_files_moved(void)
{
	char names[DHCP_MAX_SERVERS][DHCP_SERVER_NAME_MAX];
	int count = dhcp_server_list(names, DHCP_MAX_SERVERS);
	int moved = 0;
	int i;

	for (i = 0; i < count && i < DHCP_MAX_SERVERS; i++) {
		char path[PATH_MAX];
		struct stat st;
		long long size = -1, mtime_ns = -1;

		if (lease_path_of(names[i], path, sizeof(path)) == 0 && stat(path, &st) == 0) {
			size = (long long)st.st_size;
			mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
		}
		if (strcmp(g_lease_stat[i].name, names[i]) != 0 || g_lease_stat[i].size != size ||
		    g_lease_stat[i].mtime_ns != mtime_ns)
			moved = 1;
		snprintf(g_lease_stat[i].name, sizeof(g_lease_stat[i].name), "%s", names[i]);
		g_lease_stat[i].size = size;
		g_lease_stat[i].mtime_ns = mtime_ns;
	}
	for (; i < DHCP_MAX_SERVERS; i++) {
		if (g_lease_stat[i].name[0] != '\0')
			moved = 1;
		g_lease_stat[i].name[0] = '\0';
	}
	return moved;
}

/*
 * The last snapshot: each server's failover state, and every lease that
 * was current when it was taken. GET /v1/dhcp/servers and
 * GET /v1/dhcp/leases answer from it, so a change to it is announced
 * here, the one place it is set (ADR-0339).
 */
#define DHCP_SNAPSHOT_LEASES 1024
struct snap_server {
	char name[DHCP_SERVER_NAME_MAX];
	char state[DHCPD_STATE_MAX];
	char partner_state[DHCPD_STATE_MAX];
};
struct snap_lease {
	uint32_t ip_be;
	char mac[DHCP_MAC_MAX];
	char hostname[DHCP_HOSTNAME_MAX];
	long long expires_at; /* 0: never */
};
static struct snap_server g_snap_servers[DHCP_MAX_SERVERS];
static int g_snap_server_count;
static struct snap_lease g_snap_leases[DHCP_SNAPSHOT_LEASES];
static int g_snap_lease_count;

int dhcp_snapshot_apply(const char *json, size_t len)
{
	static struct snap_server servers[DHCP_MAX_SERVERS];
	static struct snap_lease leases[DHCP_SNAPSHOT_LEASES];
	struct json_value *root = json_parse(json, len);
	const struct json_value *arr;
	int nservers = 0, nleases = 0;
	size_t i;

	if (root == NULL)
		return -1;
	memset(servers, 0, sizeof(servers));
	memset(leases, 0, sizeof(leases));
	arr = json_object_get(root, "servers");
	for (i = 0; arr != NULL && arr->type == JSON_ARRAY && i < arr->u.array.count &&
	            nservers < DHCP_MAX_SERVERS;
	     i++) {
		const struct json_value *e = arr->u.array.items[i];
		const char *nm = json_as_string(json_object_get(e, "container"));
		const char *st = json_as_string(json_object_get(e, "state"));
		const char *ps = json_as_string(json_object_get(e, "partner_state"));

		if (nm == NULL)
			continue;
		snprintf(servers[nservers].name, DHCP_SERVER_NAME_MAX, "%s", nm);
		snprintf(servers[nservers].state, DHCPD_STATE_MAX, "%s", st != NULL ? st : "");
		snprintf(servers[nservers].partner_state, DHCPD_STATE_MAX, "%s", ps != NULL ? ps : "");
		nservers++;
	}
	arr = json_object_get(root, "leases");
	for (i = 0; arr != NULL && arr->type == JSON_ARRAY && i < arr->u.array.count &&
	            nleases < DHCP_SNAPSHOT_LEASES;
	     i++) {
		const struct json_value *e = arr->u.array.items[i];
		const char *mac = json_as_string(json_object_get(e, "mac"));
		const char *host = json_as_string(json_object_get(e, "hostname"));
		const struct json_value *jx = json_object_get(e, "expires_at");
		uint32_t a = parse_ip(json_as_string(json_object_get(e, "ip")));

		if (a == 0)
			continue;
		leases[nleases].ip_be = a;
		snprintf(leases[nleases].mac, DHCP_MAC_MAX, "%s", mac != NULL ? mac : "");
		snprintf(leases[nleases].hostname, DHCP_HOSTNAME_MAX, "%s", host != NULL ? host : "");
		leases[nleases].expires_at =
		    jx != NULL && jx->type == JSON_NUMBER ? (long long)json_as_number(jx) : 0;
		nleases++;
	}
	json_free(root);

	if (nservers != g_snap_server_count ||
	    memcmp(servers, g_snap_servers, sizeof(servers[0]) * (size_t)nservers) != 0) {
		memcpy(g_snap_servers, servers, sizeof(g_snap_servers));
		g_snap_server_count = nservers;
		events_announce(EV_getDhcpServers);
	}
	if (nleases != g_snap_lease_count ||
	    memcmp(leases, g_snap_leases, sizeof(leases[0]) * (size_t)nleases) != 0) {
		memcpy(g_snap_leases, leases, sizeof(g_snap_leases));
		g_snap_lease_count = nleases;
		events_announce(EV_getDhcpLeases);
	}
	return 0;
}

void dhcp_leases_write_json(struct json_writer *w, long long now)
{
	char ip[16];
	int i;

	jw_arr_open(w);
	for (i = 0; i < g_snap_lease_count; i++) {
		const struct snap_lease *l = &g_snap_leases[i];

		if (l->expires_at != 0 && l->expires_at <= now)
			continue;
		ip_str(l->ip_be, ip, sizeof(ip));
		jw_obj_open(w);
		jw_key(w, "mac");
		jw_str(w, l->mac);
		jw_key(w, "ip");
		jw_str(w, ip);
		jw_key(w, "hostname");
		if (l->hostname[0] != '\0')
			jw_str(w, l->hostname);
		else
			jw_null(w);
		jw_key(w, "expires_at");
		if (l->expires_at != 0)
			jw_int(w, l->expires_at);
		else
			jw_null(w); /* "never" */
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

/*
 * The names DNS should answer for from DHCP: every named reservation
 * (cixd's own state, since a fixed-address host never appears in a lease
 * file), then every current lease's name from the last snapshot.
 */
int dhcp_lease_names(struct dhcp_lease_name *out, int max)
{
	int i, n = 0;

	for (i = 0; i < DHCP_MAX_STATIC && n < max; i++) {
		if (g_statics[i].mac[0] == '\0' || g_statics[i].hostname[0] == '\0')
			continue;
		out[n].ip_be = g_statics[i].ip_be;
		snprintf(out[n].hostname, sizeof(out[n].hostname), "%s", g_statics[i].hostname);
		n++;
	}
	for (i = 0; i < g_snap_lease_count && n < max; i++) {
		if (g_snap_leases[i].hostname[0] == '\0')
			continue;
		out[n].ip_be = g_snap_leases[i].ip_be;
		snprintf(out[n].hostname, sizeof(out[n].hostname), "%s", g_snap_leases[i].hostname);
		n++;
	}
	return n;
}

void dhcp_servers_write_json(struct json_writer *w)
{
	int i, k;

	jw_arr_open(w);
	for (i = 0; i < DHCP_MAX_SERVERS; i++) {
		struct registry_entry *e;
		const char *partner;

		if (g_servers[i][0] == '\0')
			continue;
		e = registry_find(g_servers[i]);
		partner = pair_partner(g_servers[i]);
		jw_obj_open(w);
		jw_key(w, "container");
		jw_str(w, g_servers[i]);
		jw_key(w, "running");
		jw_bool(w, e != NULL && e->running);
		/* ADR-0386: the server's pair, and its failover state as its
		 * own lease file last said it. */
		jw_key(w, "partner");
		if (partner != NULL)
			jw_str(w, partner);
		else
			jw_null(w);
		jw_key(w, "failover_state");
		for (k = 0; k < g_snap_server_count; k++)
			if (strcmp(g_snap_servers[k].name, g_servers[i]) == 0)
				break;
		if (partner != NULL && k < g_snap_server_count && g_snap_servers[k].state[0] != '\0')
			jw_str(w, g_snap_servers[k].state);
		else
			jw_null(w);
		jw_key(w, "partner_state");
		if (partner != NULL && k < g_snap_server_count &&
		    g_snap_servers[k].partner_state[0] != '\0')
			jw_str(w, g_snap_servers[k].partner_state);
		else
			jw_null(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}
