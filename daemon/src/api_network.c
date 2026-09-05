#include "api_network.h"

#include "apiresp.h"
#include "apiroute.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "network.h"
#include "registry.h"
#include "dhcp.h"
#include "device.h"
#include "namecheck.h"
#include "rtnetlink.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * Pure (no fd) resolver, shared by respond_network_error() below and
 * create_container_from_body()'s own networks-array validation (which
 * has no fd yet to respond on directly when reused for boot autostart
 * /crash restart -- see containerdef.h).
 */
int network_error_to_status(enum network_error err, const char **out_msg)
{
	switch (err) {
	case NETWORK_ERR_INVALID_NAME:
		*out_msg = "invalid network name";
		return 400;
	case NETWORK_ERR_INVALID_SUBNET:
		*out_msg = "invalid subnet/prefix_len";
		return 400;
	case NETWORK_ERR_INVALID_ADDRESS:
		*out_msg = "invalid address (must be a real address within this subnet, "
		           "not the network or broadcast address)";
		return 400;
	case NETWORK_ERR_DUPLICATE:
		*out_msg = "a network with this name already exists";
		return 409;
	case NETWORK_ERR_OVERLAP:
		*out_msg = "subnet overlaps an existing network";
		return 400;
	case NETWORK_ERR_FULL:
		*out_msg = "network table full";
		return 500;
	case NETWORK_ERR_NOT_FOUND:
		*out_msg = "no such network";
		return 404;
	case NETWORK_ERR_IN_USE:
		*out_msg = "network is still in use by a container";
		return 409;
	case NETWORK_ERR_IP_OUT_OF_RANGE:
		*out_msg = "ip is not a usable address on this network";
		return 400;
	case NETWORK_ERR_IP_TAKEN:
		/* Not necessarily RUNNING: the address is held by any registry
		 * entry, including one being torn down (#148). Callers that
		 * can identify the holder say so instead of using this. */
		*out_msg = "ip is already assigned to another container";
		return 409;
	case NETWORK_ERR_INTERFACE_NOT_FOUND:
		*out_msg = "unknown or unassignable interface (see GET /v1/devices)";
		return 400;
	case NETWORK_ERR_INTERFACE_ATTACHED:
		*out_msg = "interface is already attached to this network";
		return 409;
	case NETWORK_ERR_INTERFACE_NOT_ATTACHED:
		*out_msg = "interface is not attached to this network";
		return 404;
	case NETWORK_ERR_INTERFACE_FULL:
		*out_msg = "this network's interface table is full";
		return 500;
	case NETWORK_ERR_INTERFACE_NAME_TOO_LONG:
		*out_msg = "ifname.vlan_id would not fit in IFNAMSIZ";
		return 400;
	case NETWORK_ERR_IS_MANAGEMENT:
		*out_msg = "refused: this network carries cixd's own bind address -- "
		           "repoint the management network first (see /v1/system/daemon-config)";
		return 409;
	case NETWORK_ERR_CREATE_FAILED:
	case NETWORK_ERR_DELETE_FAILED:
	default:
		*out_msg = "network operation failed";
		return 500;
	}
}

void respond_network_error(int fd, enum network_error err)
{
	const char *msg;
	int status = network_error_to_status(err, &msg);

	respond_error(fd, status, http_status_text(status), msg);
}

void handle_network_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *subnet, *address;
	const struct json_value *jprefix;
	int prefix_len;
	struct network_def *net;
	enum network_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	subnet = json_as_string(json_object_get(root, "subnet"));
	jprefix = json_object_get(root, "prefix_len");
	address = json_as_string(json_object_get(root, "address")); /* optional; NULL = no address */

	if (name == NULL || subnet == NULL || jprefix == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/subnet/prefix_len missing");
		return;
	}
	prefix_len = (int)json_as_number(jprefix);

	/* Issue #70: optional auto-allocation window (dotted IPs, in-subnet);
	 * omitted = full default range. */
	{
		const char *alloc_start = json_as_string(json_object_get(root, "alloc_start"));
		const char *alloc_end = json_as_string(json_object_get(root, "alloc_end"));

		nerr = network_create(name, subnet, prefix_len, address, alloc_start, alloc_end, &net);
	}
	json_free(root);

	if (nerr != NETWORK_OK) {
		respond_network_error(fd, nerr);
		return;
	}

	jw_init(&w);
	network_write_json_one(net, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_network_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "networks");
	network_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Issue #26: a network's ports, as a switch panel would show them.
 *
 * The port list comes from the KERNEL -- everything currently enslaved
 * to this network's bridge, read from /sys/class/net/<bridge>/brif --
 * not from what this daemon believes it attached. If the two ever
 * disagree, the kernel is the one that is right, and a port we cannot
 * account for is reported as such rather than dropped: a veth on the
 * bridge that belongs to no container we know of is exactly the kind
 * of thing an operator needs to see, and the only way to see it is to
 * ask the switch what is plugged into it.
 *
 * The registry is the annotation layer on top: which container owns a
 * port, what that interface is called inside it, which IP it holds.
 */
struct net_port {
	char ifname[32];
	const char *kind;                 /* "uplink" / "container" / "unattributed" */
	int vlan_id;                      /* uplinks only; -1 when not applicable */
	char container[REGISTRY_NAME_MAX];
	char container_ifname[16];
	char ip[16];
	int sort_group;                   /* uplinks first, then containers, then the rest */
};

#define NET_PORTS_MAX 256

static int net_port_cmp(const void *a, const void *b)
{
	const struct net_port *pa = a;
	const struct net_port *pb = b;

	if (pa->sort_group != pb->sort_group)
		return pa->sort_group - pb->sort_group;
	if (pa->sort_group == 1) {
		int c = strcmp(pa->container, pb->container);

		if (c != 0)
			return c;
	}
	return strcmp(pa->ifname, pb->ifname);
}

static void net_port_write_link_state(struct json_writer *w, const char *ifname)
{
	char path[PATH_MAX];
	char buf[32];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
	f = fopen(path, "r");
	if (f == NULL || fgets(buf, sizeof(buf), f) == NULL) {
		if (f != NULL)
			fclose(f);
		/* The interface vanished between listing the bridge and asking
		 * about it -- a container stopping mid-request. Unknown is the
		 * true answer; "down" would be a guess. */
		jw_null(w);
		return;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	jw_str(w, buf);
}

void handle_network_ports_get(int fd, const char *name)
{
	struct network_def *net = network_find(name);
	struct net_port ports[NET_PORTS_MAX];
	int port_count = 0;
	char cnames[REGISTRY_MAX_CONTAINERS][REGISTRY_NAME_MAX];
	int ccount;
	char brif_path[PATH_MAX];
	DIR *d;
	struct dirent *de;
	struct json_writer w;
	int i;
	int j;

	if (net == NULL) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}

	snprintf(brif_path, sizeof(brif_path), "/sys/class/net/%s/brif", name);
	d = opendir(brif_path);
	if (d == NULL) {
		/*
		 * No bridge on this host: a network defined in state but not
		 * realised, or a dev daemon that never had the privilege to
		 * create one. An empty port list with the reason said out loud
		 * beats a 500 that reads as a fault.
		 */
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "network");
		jw_str(&w, name);
		jw_key(&w, "bridge_present");
		jw_bool(&w, 0);
		jw_key(&w, "ports");
		jw_arr_open(&w);
		jw_arr_close(&w);
		jw_obj_close(&w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
		return;
	}

	ccount = registry_list_names(cnames, REGISTRY_MAX_CONTAINERS);
	while ((de = readdir(d)) != NULL && port_count < NET_PORTS_MAX) {
		struct net_port *p;

		if (de->d_name[0] == '.')
			continue;
		p = &ports[port_count];
		memset(p, 0, sizeof(*p));
		snprintf(p->ifname, sizeof(p->ifname), "%s", de->d_name);
		p->vlan_id = -1;
		p->kind = "unattributed";
		p->sort_group = 2;

		/* An uplink: a real host interface this network was told to
		 * bridge onto, including a VLAN sub-interface, which is a port
		 * in its own right while its parent NIC is not one. */
		for (i = 0; i < net->interface_count; i++) {
			if (strcmp(net->interfaces[i].ifname, p->ifname) == 0) {
				p->kind = "uplink";
				p->vlan_id = net->interfaces[i].vlan_id;
				p->sort_group = 0;
				break;
			}
		}
		if (p->sort_group == 0) {
			port_count++;
			continue;
		}
		for (i = 0; i < ccount && p->sort_group != 1; i++) {
			struct registry_entry *e = registry_find(cnames[i]);

			if (e == NULL || !e->running)
				continue;
			for (j = 0; j < e->net_count; j++) {
				char veth[32];

				if (strcmp(e->nets[j].name, name) != 0)
					continue;
				container_veth_host_name(e, j, veth, sizeof(veth));
				if (strcmp(veth, p->ifname) != 0)
					continue;
				p->kind = "container";
				p->sort_group = 1;
				snprintf(p->container, sizeof(p->container), "%s", e->name);
				snprintf(p->container_ifname, sizeof(p->container_ifname), "%s",
				         e->nets[j].ifname[0] != '\0' ? e->nets[j].ifname : "eth0");
				{
					struct in_addr a;

					a.s_addr = e->nets[j].ip_be;
					if (inet_ntop(AF_INET, &a, p->ip, sizeof(p->ip)) == NULL)
						p->ip[0] = '\0';
				}
				break;
			}
		}
		port_count++;
	}
	closedir(d);

	/*
	 * Deterministic order so a rendered panel does not reshuffle
	 * between polls. It is an ORDER, not an identity: ports come and go
	 * with containers, so any number this handed out would move, and
	 * ifname is the thing that actually names a port.
	 */
	qsort(ports, (size_t)port_count, sizeof(ports[0]), net_port_cmp);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "network");
	jw_str(&w, name);
	jw_key(&w, "bridge_present");
	jw_bool(&w, 1);
	jw_key(&w, "ports");
	jw_arr_open(&w);
	for (i = 0; i < port_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "ifname");
		jw_str(&w, ports[i].ifname);
		jw_key(&w, "kind");
		jw_str(&w, ports[i].kind);
		jw_key(&w, "vlan_id");
		if (ports[i].vlan_id < 0)
			jw_null(&w);
		else
			jw_int(&w, ports[i].vlan_id);
		jw_key(&w, "container");
		if (ports[i].container[0] != '\0')
			jw_str(&w, ports[i].container);
		else
			jw_null(&w);
		jw_key(&w, "container_ifname");
		if (ports[i].container_ifname[0] != '\0')
			jw_str(&w, ports[i].container_ifname);
		else
			jw_null(&w);
		jw_key(&w, "ip");
		if (ports[i].ip[0] != '\0')
			jw_str(&w, ports[i].ip);
		else
			jw_null(&w);
		jw_key(&w, "link");
		net_port_write_link_state(&w, ports[i].ifname);
		/*
		 * From the PORT's own side, which is the switch's side: rx is
		 * what arrived at the switch from whatever is plugged in, tx is
		 * what the switch sent to it. That is the inverse of what the
		 * container sees on its own interface, and saying which way
		 * round it is here is the difference between a useful number
		 * and a misleading one.
		 *
		 * Raw counters only; the caller computes rates -- the same
		 * convention every other stats endpoint in this daemon uses.
		 */
		jw_key(&w, "rx_bytes");
		jw_int(&w, read_net_stat(ports[i].ifname, "rx_bytes"));
		jw_key(&w, "tx_bytes");
		jw_int(&w, read_net_stat(ports[i].ifname, "tx_bytes"));
		jw_key(&w, "rx_packets");
		jw_int(&w, read_net_stat(ports[i].ifname, "rx_packets"));
		jw_key(&w, "tx_packets");
		jw_int(&w, read_net_stat(ports[i].ifname, "tx_packets"));
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_network_get_one(int fd, const char *name)
{
	struct network_def *net = network_find(name);
	struct json_writer w;

	if (net == NULL) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}
	jw_init(&w);
	network_write_json_one(net, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * Issue #137: PUT /v1/networks/{name} -- change the auto-allocation
 * window on an existing network.
 *
 * The window was settable only at creation, which put it out of reach
 * on the network that needs it most. A bridged management network
 * cannot be deleted while it is the management network (409, by
 * design), so "delete and recreate with a pool" was never a route that
 * existed, and a real LAN had no way to gain the pool that keeps
 * auto-allocation off live equipment.
 */
void handle_network_update(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *js, *je;
	const char *start = NULL, *end = NULL;
	enum network_error nerr;
	struct network_def *net;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	/*
	 * Absent and explicit-null mean different things and are kept
	 * apart: absent leaves the bound alone, null clears it. Collapsing
	 * them would make it impossible to clear one bound without also
	 * restating the other -- the same distinction POST /pkg/repo-config
	 * and PUT /system/daemon-config already draw for partial updates.
	 */
	js = json_object_get(root, "alloc_start");
	je = json_object_get(root, "alloc_end");
	if (js != NULL)
		start = (js->type == JSON_NULL) ? "" : json_as_string(js);
	if (je != NULL)
		end = (je->type == JSON_NULL) ? "" : json_as_string(je);

	if ((js != NULL && start == NULL) || (je != NULL && end == NULL)) {
		json_free(root);
		respond_error(fd, 400, "Bad Request",
		              "alloc_start and alloc_end must each be a dotted-quad string or null");
		return;
	}

	nerr = network_set_alloc_window(name, start, end);
	json_free(root);

	if (nerr == NETWORK_ERR_NOT_FOUND) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}
	if (nerr == NETWORK_ERR_INVALID_ADDRESS) {
		respond_error(fd, 400, "Bad Request",
		              "alloc_start/alloc_end must be inside this network's subnet, and "
		              "alloc_end must not be below alloc_start");
		return;
	}
	if (nerr != NETWORK_OK) {
		respond_error(fd, 500, "Internal Server Error",
		              "the allocation window was changed but could not be persisted");
		return;
	}

	net = network_find(name);
	if (net == NULL) {
		respond_error(fd, 404, "Not Found", "no such network");
		return;
	}
	jw_init(&w);
	network_write_json_one(net, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}



void handle_network_attach_interface(int fd, const char *net_name, const char *body,
                                             size_t body_len)
{
	struct json_value *root;
	const char *ifname;
	const struct json_value *jvlan;
	int vlan_id;
	struct network_def *net;
	enum network_error nerr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	ifname = json_as_string(json_object_get(root, "ifname"));
	jvlan = json_object_get(root, "vlan_id"); /* optional; absent/0 = untagged */
	vlan_id = jvlan != NULL ? (int)json_as_number(jvlan) : 0;

	if (ifname == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "ifname missing");
		return;
	}
	if (vlan_id < 0 || vlan_id > 4094) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "vlan_id must be in [1,4094], or omitted/0 for untagged");
		return;
	}

	nerr = network_attach_interface(net_name, ifname, vlan_id);
	json_free(root);

	if (nerr != NETWORK_OK) {
		respond_network_error(fd, nerr);
		return;
	}

	net = network_find(net_name);
	jw_init(&w);
	network_write_json_one(net, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_network_detach_interface(int fd, const char *net_name, const char *ifname)
{
	enum network_error nerr = network_detach_interface(net_name, ifname);

	if (nerr != NETWORK_OK) {
		respond_network_error(fd, nerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}
