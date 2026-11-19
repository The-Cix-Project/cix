#include "dhcp.h"
#include "containerpath.h"
#include "logstore.h"
#include "dns.h"
#include "persist.h"
#include "registry.h"
#include "network.h"
#include "dhcprelay.h"
#include "linux_compat.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>

static struct dhcp_network g_networks[DHCP_MAX_NETWORKS];
static struct dhcp_static g_statics[DHCP_MAX_STATIC];
static char g_servers[DHCP_MAX_SERVERS][DHCP_SERVER_NAME_MAX];
/* ADR-0383: registered relays, and the service each runs the relay as. */
static char g_relays[DHCP_MAX_RELAYS][DHCP_SERVER_NAME_MAX];
static char g_relay_services[DHCP_MAX_RELAYS][DHCP_SERVICE_NAME_MAX];
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
 * to the shape a DNS label has: letters, digits and hyphens, not
 * starting or ending with one. Refused rather than sanitised -- this
 * string is written into a config file the server parses.
 *
 * LOWER-CASE ONLY, which this comment used to omit and the 400 used to
 * omit too -- both said "letters" while the loop below accepts 'a'-'z'
 * and nothing above it. A DNS label is case-insensitive, so refusing
 * "Printer" is this implementation's choice rather than the standard's;
 * it is kept because the stored string is written verbatim into
 * dnsmasq's config and a single spelling is one less thing to compare.
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

int dhcp_render_conf(const char *server_name, const struct registry_network_attachment *self,
                     int self_count, char *out, size_t out_size)
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
		if (attached_to(server_name, self, self_count, g_networks[i].network)) {
			n = snprintf(out + off, out_size - off, "dhcp-range=set:%s,%s,%s,%ds\n",
			             g_networks[i].network, start, end, g_networks[i].lease_seconds);
		} else {
			/*
			 * ADR-0383: a range this server reaches only through a
			 * relay. dnsmasq's manual: "For networks which receive
			 * DHCP service via a relay agent, dnsmasq cannot determine
			 * the netmask itself, so it should be specified". An
			 * attached range renders exactly as before, so adding
			 * relays never rolls a server whose ranges did not change.
			 */
			const struct network_def *nd = network_find(g_networks[i].network);
			char mask[16];

			if (nd == NULL || nd->prefix_len <= 0 || nd->prefix_len > 32)
				continue;
			ip_str(htonl(0xffffffffu << (32 - nd->prefix_len)), mask, sizeof(mask));
			n = snprintf(out + off, out_size - off, "dhcp-range=set:%s,%s,%s,%s,%ds\n",
			             g_networks[i].network, start, end, mask,
			             g_networks[i].lease_seconds);
		}
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

		if (dhcp_render_conf(names[i], NULL, 0, conf, sizeof(conf)) < 0)
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
		container_file_host_path(names[i], DHCP_CONF_PATH,
		                          path, sizeof(path));
		if (file_differs(path, conf) && persist_atomic_write(path, conf, strlen(conf)) != 0)
			logstore_write("cixd", "error", "dhcp: could not write %s config to %s",
			                names[i], path);
		container_file_host_path(names[i], DHCP_HOSTS_PATH,
		                          path, sizeof(path));
		/* #697: written, and the server told, only when the
		 * reservations changed -- this runs after every container
		 * start, and a reload with nothing new is a reload for
		 * nothing. */
		if (!file_differs(path, hosts))
			continue;
		if (persist_atomic_write(path, hosts, strlen(hosts)) != 0) {
			logstore_write("cixd", "error", "dhcp: could not write %s hosts to %s",
			                names[i], path);
			continue;
		}
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
	jw_key(w, "relays");
	jw_arr_open(w);
	{
		int ri;

		for (ri = 0; ri < cfg->relay_count; ri++)
			jw_str(w, cfg->relays[ri]);
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
		/*
		 * Through the container's own mount namespace, NOT
		 * container_file_host_path() (#357).
		 *
		 * This used to use the host path, under a comment reading "a
		 * READ, but it must look where the writes go (#269)" -- and
		 * that reasoning is right for the two files above, which this
		 * daemon writes into the container's writable layer. It is
		 * wrong here, because dnsmasq writes this one and DHCP_LEASE_PATH
		 * is under /run, which src/mountns.c mounts as a fresh tmpfs at
		 * every container start. The writable layer underneath is
		 * masked by that mount, so the file has no on-disk existence to
		 * open: fopen() failed on every server, every call, and the
		 * endpoint answered 200 with an empty array while a real client
		 * held a real lease.
		 *
		 * create_container_from_body() already refuses a files[] entry
		 * under /run for exactly this reason and says so at length. The
		 * rule was known in one file and lost in this one.
		 *
		 * The distinction that matters is not read versus write, which
		 * is what the old comment reached for: it is WHO writes the
		 * file. Daemon-written files live in the writable layer;
		 * container-written ones live in the container's namespace and
		 * may be on a filesystem with no on-disk backing at all.
		 *
		 * No on-disk fallback, unlike handle_container_file_read():
		 * a lease file exists only while the dnsmasq that owns it
		 * does, and this loop has already skipped every server that is
		 * not running.
		 */
		snprintf(path, sizeof(path), "/proc/%d/root%s", (int)entry->handle.pid,
		          DHCP_LEASE_PATH);
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
