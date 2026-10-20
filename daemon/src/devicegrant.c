#include "devicegrant.h"

#include "containerdef.h"
#include "daemon_config.h"
#include "daemonpaths.h"
#include "devicemap.h"
#include "json.h"
#include "logstore.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/*
 * ADR-0344. Everything devicestate.c decides from, gathered fresh on
 * every call -- the same "re-enumerate, never cache" posture device.c
 * has always taken toward host hardware, because a device can be
 * plugged, unplugged or granted between any two requests.
 */

static struct device_snapshot g_snap;

/*
 * The interface carrying the management address, by asking the kernel
 * which interface has it (getifaddrs() is netlink-backed, so it answers
 * for the daemon's own namespace -- the sysfs trap in CLAUDE.md does
 * not apply). On an installed host that is the management network's
 * bridge, so its ports carry the fact; a host that put the address on
 * a NIC directly has that NIC carry it.
 */
static void mark_management(struct device_snapshot *s)
{
	const char *addr = daemon_config_management_address();
	struct in_addr want;
	struct ifaddrs *list, *p;
	char ifname[IF_NAMESIZE] = "";
	int i;

	if (addr == NULL || inet_pton(AF_INET, addr, &want) != 1)
		return;
	if (getifaddrs(&list) != 0)
		return;
	for (p = list; p != NULL; p = p->ifa_next) {
		if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET)
			continue;
		if (((const struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr == want.s_addr) {
			snprintf(ifname, sizeof(ifname), "%s", p->ifa_name);
			break;
		}
	}
	freeifaddrs(list);
	if (ifname[0] == '\0')
		return;

	for (i = 0; i < s->n; i++) {
		struct discovered_device *d = &s->recs[i];

		if (strcmp(d->bus, "net") != 0)
			continue;
		if (strcmp(d->id + 4, ifname) == 0 || strcmp(d->master, ifname) == 0)
			d->management = 1;
	}
}

static struct discovered_device *append_record(struct device_snapshot *s, const char *id,
                                               const char *bus, const char *parent,
                                               const char *description, const char *driver)
{
	struct discovered_device *d;

	if (s->n >= DEVICE_ENUM_MAX || device_record_index(s->recs, s->n, id) >= 0)
		return NULL;
	d = &s->recs[s->n++];
	memset(d, 0, sizeof(*d));
	snprintf(d->id, sizeof(d->id), "%s", id);
	snprintf(d->bus, sizeof(d->bus), "%s", bus);
	snprintf(d->parent, sizeof(d->parent), "%s", parent);
	snprintf(d->description, sizeof(d->description), "%s", description);
	snprintf(d->driver, sizeof(d->driver), "%s", driver);
	return d;
}

/*
 * The records running containers took out of the host's view. An
 * interface moved into a netns -- and a radio's PHY with it -- is gone
 * from the host's walk, and its grant record is the only place that
 * still says what it was; this puts it back in the list, as itself,
 * under the device it hangs off.
 */
static void append_moved(struct device_snapshot *s, const struct registry_entry *e)
{
	int j;

	for (j = 0; j < e->interface_count; j++) {
		const struct registry_interface_record *r = &e->interface_records[j];
		struct discovered_device *net;
		char id[DEVICE_ID_MAX];

		if (r->granted[0] == '\0')
			continue;
		if (r->phy[0] != '\0') {
			char phy_desc[64];

			snprintf(phy_desc, sizeof(phy_desc), "wireless PHY %s", r->phy);
			append_record(s, r->granted, "phy", r->anchor, phy_desc, r->driver);
			snprintf(id, sizeof(id), "net:%s", e->interfaces[j]);
			net = append_record(s, id, "net", r->granted, r->description, r->driver);
		} else {
			net = append_record(s, r->granted, "net", r->anchor, r->description, r->driver);
		}
		if (net != NULL) {
			snprintf(net->mac, sizeof(net->mac), "%s", r->mac);
			net->mac_permanent = r->mac_permanent;
		}
	}
}

static void add_claim(struct device_snapshot *s, enum device_claim_kind kind,
                      const char *container, const char *id, int shared, int binding)
{
	struct device_claim *c;

	if (s->claim_count >= DEVICEGRANT_CLAIMS_MAX)
		return;
	c = &s->claims[s->claim_count++];
	memset(c, 0, sizeof(*c));
	c->kind = kind;
	snprintf(c->container, sizeof(c->container), "%s", container);
	snprintf(c->id, sizeof(c->id), "%s", id);
	c->shared = shared;
	c->binding = binding;
}

/*
 * A devices[] reference, resolved now the way a start would resolve it:
 * a named mapping first (ADR-0048), then a raw id or a gpu:N group. A
 * reference that resolves to nothing claims nothing.
 */
static void claim_reference(struct device_snapshot *s, const char *container, const char *ref,
                            int binding)
{
	const struct discovered_device *matches[CONTAINER_MAX_DEVICES];
	int n, k;

	n = devicemap_resolve(ref, CONTAINERS_DIR, matches, CONTAINER_MAX_DEVICES);
	if (n < 0)
		n = device_find_group(ref, CONTAINERS_DIR, matches, CONTAINER_MAX_DEVICES);
	for (k = 0; k < n; k++)
		add_claim(s, DEVICE_CLAIM_RESERVATION, container, matches[k]->id, 0, binding);
}

/* The record an interfaces[] entry is really a grant of: its PHY when
 * it is a radio, itself otherwise. */
static const char *interface_grant_id(const struct device_snapshot *s, const char *ifname,
                                      char *buf, size_t buf_size)
{
	int idx;

	snprintf(buf, buf_size, "net:%s", ifname);
	idx = device_record_index(s->recs, s->n, buf);
	if (idx < 0)
		return NULL;
	if (strncmp(s->recs[idx].parent, "phy:", 4) == 0)
		return s->recs[idx].parent;
	return s->recs[idx].id;
}

/* A stopped definition's claims, read from the body it will be started
 * from -- the one place reserve_devices lives. */
static void claim_definition(struct device_snapshot *s, const struct container_def *def)
{
	struct json_value *root = json_parse(def->body, def->body_len);
	const struct json_value *jreserve, *jdevices, *jinterfaces;
	int binding = 0;
	size_t i;

	if (root == NULL)
		return;
	jreserve = json_object_get(root, "reserve_devices");
	if (jreserve != NULL && jreserve->type == JSON_BOOL)
		binding = jreserve->u.boolean;

	jdevices = json_object_get(root, "devices");
	if (jdevices != NULL && jdevices->type == JSON_ARRAY) {
		for (i = 0; i < jdevices->u.array.count; i++) {
			const struct json_value *item = jdevices->u.array.items[i];
			const char *ref = item->type == JSON_OBJECT
			                      ? json_as_string(json_object_get(item, "id"))
			                      : json_as_string(item);

			if (ref != NULL)
				claim_reference(s, def->name, ref, binding);
		}
	}

	jinterfaces = json_object_get(root, "interfaces");
	if (jinterfaces != NULL && jinterfaces->type == JSON_ARRAY) {
		for (i = 0; i < jinterfaces->u.array.count; i++) {
			const char *ifname = json_as_string(jinterfaces->u.array.items[i]);
			char buf[DEVICE_ID_MAX];
			const char *id;

			if (ifname == NULL)
				continue;
			id = interface_grant_id(s, ifname, buf, sizeof(buf));
			if (id != NULL)
				add_claim(s, DEVICE_CLAIM_RESERVATION, def->name, id, 0, binding);
		}
	}
	json_free(root);
}

const struct device_snapshot *devicegrant_snapshot(void)
{
	static char names[CONTAINERDEF_MAX > REGISTRY_MAX_CONTAINERS ? CONTAINERDEF_MAX
	                                                             : REGISTRY_MAX_CONTAINERS]
	                 [REGISTRY_NAME_MAX];
	static int warned;
	struct device_snapshot *s = &g_snap;
	int count, i, j, a, b;

	s->n = device_enumerate(s->recs, DEVICE_ENUM_MAX, CONTAINERS_DIR);
	s->claim_count = 0;
	mark_management(s);

	/* Running containers: what they hold, and what they wait for. */
	count = registry_list_names(names, REGISTRY_MAX_CONTAINERS);
	for (i = 0; i < count; i++) {
		const struct registry_entry *e = registry_find(names[i]);

		if (e == NULL)
			continue;
		append_moved(s, e);
		for (j = 0; j < e->device_count; j++)
			add_claim(s, DEVICE_CLAIM_GRANT, e->name, e->devices[j].id, e->devices[j].shared, 0);
		for (j = 0; j < e->interface_count; j++) {
			const char *granted = e->interface_records[j].granted;
			char buf[DEVICE_ID_MAX];

			if (granted[0] == '\0') {
				snprintf(buf, sizeof(buf), "net:%s", e->interfaces[j]);
				granted = buf;
			}
			add_claim(s, DEVICE_CLAIM_GRANT, e->name, granted, 0, 0);
		}
		for (j = 0; j < e->pending_device_count; j++)
			claim_reference(s, e->name, e->pending_devices[j], 0);
	}

	/* Stopped definitions: a reservation, binding only with the setting. */
	count = containerdef_list_names(names, CONTAINERDEF_MAX);
	for (i = 0; i < count; i++) {
		const struct container_def *def;

		if (registry_find(names[i]) != NULL)
			continue;
		def = containerdef_find(names[i]);
		if (def != NULL && def->body != NULL)
			claim_definition(s, def);
	}

	/*
	 * The gate the enumerator exists to satisfy. A node in two records
	 * would let it be granted twice under two ids; said once per daemon
	 * run, because a host whose sysfs produces it will produce it on
	 * every request.
	 */
	if (!warned && device_records_share_a_node(s->recs, s->n, &a, &b)) {
		warned = 1;
		logstore_write("cixd", "error",
		               "devices %s and %s own the same /dev node %u:%u -- one of the two "
		               "walks in device.c should have yielded it (ADR-0344)",
		               s->recs[a].id, s->recs[b].id, s->recs[a].major, s->recs[a].minor);
	}
	return s;
}

/* The refusal sentence -- the same reason GET /v1/devices shows. */
static void refusal(const struct device_snapshot *s, int idx, int code,
                    const struct device_verdict *v, int want_shared, char *err, size_t err_size)
{
	const struct discovered_device *d = &s->recs[idx];
	const struct device_claim *c = v->claim >= 0 ? &s->claims[v->claim] : NULL;
	char through[DEVICE_ID_MAX + 16] = "";

	if (code == 400) {
		snprintf(err, err_size, "device %s %s", d->id, device_reason_phrase(v->reason));
		return;
	}
	if (c != NULL && strcmp(c->id, d->id) != 0)
		snprintf(through, sizeof(through), " through %s", c->id);
	if (v->state == DEVICE_STATE_RESERVED)
		snprintf(err, err_size,
		         "device %s is reserved by container \"%s\"%s, which sets reserve_devices",
		         d->id, c != NULL ? c->container : "?", through);
	else if (want_shared)
		snprintf(err, err_size,
		         "device %s is already held by container \"%s\"%s, which did not set "
		         "\"shared\": true",
		         d->id, c != NULL ? c->container : "?", through);
	else
		snprintf(err, err_size,
		         "device %s is already held by container \"%s\"%s -- both grants must set "
		         "\"shared\": true to share it",
		         d->id, c != NULL ? c->container : "?", through);
}

int devicegrant_check(const char *id, int want_shared, const char *requester, char *err,
                      size_t err_size)
{
	const struct device_snapshot *s = devicegrant_snapshot();
	struct device_verdict v;
	int idx = device_record_index(s->recs, s->n, id);
	int code;

	if (idx < 0) {
		snprintf(err, err_size, "no such device %s", id);
		return 404;
	}
	code = device_grant_verdict(s->recs, s->n, idx, s->claims, s->claim_count, want_shared,
	                            requester, &v);
	if (code != 0)
		refusal(s, idx, code, &v, want_shared, err, err_size);
	return code;
}

int devicegrant_check_interface(const char *ifname, const char *requester,
                                struct registry_interface_record *out, char *err,
                                size_t err_size)
{
	const struct device_snapshot *s = devicegrant_snapshot();
	struct device_verdict v;
	char net_id[DEVICE_ID_MAX];
	char buf[DEVICE_ID_MAX];
	const char *grant_id;
	int net_idx, idx, code;

	snprintf(net_id, sizeof(net_id), "net:%s", ifname);
	net_idx = device_record_index(s->recs, s->n, net_id);
	grant_id = interface_grant_id(s, ifname, buf, sizeof(buf));
	idx = grant_id != NULL ? device_record_index(s->recs, s->n, grant_id) : -1;
	if (net_idx < 0 || idx < 0) {
		snprintf(err, err_size, "unknown interface %s (see GET /v1/devices)", ifname);
		return 400;
	}
	code = device_grant_verdict(s->recs, s->n, idx, s->claims, s->claim_count, 0, requester, &v);
	if (code != 0) {
		refusal(s, idx, code, &v, 0, err, err_size);
		return code;
	}

	memset(out, 0, sizeof(*out));
	snprintf(out->granted, sizeof(out->granted), "%s", s->recs[idx].id);
	if (strcmp(s->recs[idx].bus, "phy") == 0)
		snprintf(out->phy, sizeof(out->phy), "%s", s->recs[idx].id + 4);
	snprintf(out->anchor, sizeof(out->anchor), "%s", s->recs[idx].parent);
	snprintf(out->description, sizeof(out->description), "%s", s->recs[net_idx].description);
	snprintf(out->driver, sizeof(out->driver), "%s", s->recs[net_idx].driver);
	snprintf(out->mac, sizeof(out->mac), "%s", s->recs[net_idx].mac);
	out->mac_permanent = s->recs[net_idx].mac_permanent;
	return 0;
}

void devicegrant_write_json_list(struct json_writer *w)
{
	const struct device_snapshot *s = devicegrant_snapshot();

	device_write_json_states(w, s->recs, s->n, s->claims, s->claim_count);
}
