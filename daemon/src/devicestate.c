#include "devicestate.h"

#include <string.h>

/*
 * ADR-0344. Records form a tree through `parent`; facts sit on the
 * records; claims are what containers hold or name. Everything below is
 * a function of those three, and nothing else.
 */

/* Deeper than any real sysfs chain of records (usb device -> PHY ->
 * interface is three); a parent loop cannot come from sysfs, but a bound
 * means a bad record cannot hang the caller either. */
#define DEVICE_DEPTH_MAX 16

int device_is_grant_unit(const struct discovered_device *d)
{
	return d->dev_path[0] != '\0' || strcmp(d->bus, "net") == 0 || strcmp(d->bus, "phy") == 0;
}

int device_record_index(const struct discovered_device *recs, int n, const char *id)
{
	int i;

	for (i = 0; i < n; i++) {
		if (strcmp(recs[i].id, id) == 0)
			return i;
	}
	return -1;
}

/* Whether `above` is a strict ancestor of `id`, walking parent links. */
static int is_ancestor(const struct discovered_device *recs, int n, const char *above,
                       const char *id)
{
	int idx = device_record_index(recs, n, id);
	int depth;

	for (depth = 0; idx >= 0 && depth < DEVICE_DEPTH_MAX; depth++) {
		const char *parent = recs[idx].parent;

		if (parent[0] == '\0')
			return 0;
		if (strcmp(parent, above) == 0)
			return 1;
		idx = device_record_index(recs, n, parent);
	}
	return 0;
}

int device_ids_related(const struct discovered_device *recs, int n, const char *a, const char *b)
{
	if (strcmp(a, b) == 0)
		return 1;
	return is_ancestor(recs, n, a, b) || is_ancestor(recs, n, b, a);
}

/*
 * A fact on this record alone, in the order a reader most needs to hear
 * it. The OS layout comes first because it is the one a mistake costs
 * the host for; the management interface second for the same reason.
 */
static const char *own_host_reason(const struct discovered_device *d)
{
	if (d->os_layout)
		return d->is_partition ? "os-partition" : "os-disk";
	if (d->management)
		return "management-interface";
	if (d->storage_role)
		return "storage-role";
	if (d->mounted)
		return "mounted";
	if (d->bridge_port)
		return "bridge-port";
	return NULL;
}

/*
 * The facts that reach the records BELOW the one they are on: a disk
 * holding a role or mounted makes each of its partitions the host's too.
 * The OS layout does not reach down, by design (#9): a partition an
 * operator added to the OS disk's reserved space is decided by its own
 * role and mount alone, and the four structural ones carry their own
 * fact. Nor do the network facts -- an interface has no record below it
 * that a grant could reach.
 */
static const char *spreading_host_reason(const struct discovered_device *d)
{
	if (d->storage_role)
		return "storage-role";
	if (d->mounted)
		return "mounted";
	return NULL;
}

static const char *ancestor_host_reason(const struct discovered_device *recs, int n, int idx)
{
	int depth;

	for (depth = 0; idx >= 0 && depth < DEVICE_DEPTH_MAX; depth++) {
		const char *parent = recs[idx].parent;
		const char *r;

		if (parent[0] == '\0')
			return NULL;
		idx = device_record_index(recs, n, parent);
		if (idx < 0)
			return NULL;
		r = spreading_host_reason(&recs[idx]);
		if (r != NULL)
			return r;
	}
	return NULL;
}

/* Any fact on any record below: a USB stick whose partition the host
 * has mounted is the host's, and so is a USB NIC that is a bridge port. */
static const char *descendant_host_reason(const struct discovered_device *recs, int n, int idx)
{
	int i;

	for (i = 0; i < n; i++) {
		const char *r;

		if (i == idx || !is_ancestor(recs, n, recs[idx].id, recs[i].id))
			continue;
		r = own_host_reason(&recs[i]);
		if (r != NULL)
			return r;
	}
	return NULL;
}

/*
 * What the device itself allows, whoever is asking: "host" or
 * "unavailable" with a reason, or neither. The order is the ADR's, and
 * it is what keeps a PCI function with nothing of its own from reading
 * as the host's merely because a disk below it is.
 *
 * Held and reserved sit between the ancestor rule and the unavailable
 * rule in the reported state, so this answers in two halves: with
 * after_claims 0 the record's own and its ancestors' host facts, with
 * after_claims 1 the unavailable rules and its descendants' facts.
 */
static int host_or_unavailable(const struct discovered_device *recs, int n, int idx,
                               int after_claims, struct device_verdict *out)
{
	const struct discovered_device *d = &recs[idx];
	const char *r;

	if (!after_claims) {
		r = own_host_reason(d);
		if (r == NULL)
			r = ancestor_host_reason(recs, n, idx);
		if (r != NULL) {
			out->state = DEVICE_STATE_HOST;
			out->reason = r;
			return 1;
		}
		/* Before the claims, so a PCI function above a held disk says
		 * what it is -- nothing to grant itself -- and its held_by
		 * still names who holds what is below it. */
		if (!device_is_grant_unit(d)) {
			out->state = DEVICE_STATE_UNAVAILABLE;
			out->reason = "no-node";
			return 1;
		}
		return 0;
	}
	if (d->requires_driver && d->driver[0] == '\0') {
		out->state = DEVICE_STATE_UNAVAILABLE;
		out->reason = "no-driver";
		return 1;
	}
	r = descendant_host_reason(recs, n, idx);
	if (r != NULL) {
		out->state = DEVICE_STATE_HOST;
		out->reason = r;
		return 1;
	}
	return 0;
}

void device_state_of(const struct discovered_device *recs, int n, int idx,
                     const struct device_claim *claims, int claim_count,
                     struct device_verdict *out)
{
	int i;

	out->state = DEVICE_STATE_FREE;
	out->reason = "";
	out->claim = -1;

	if (host_or_unavailable(recs, n, idx, 0, out))
		return;

	for (i = 0; i < claim_count; i++) {
		if (claims[i].kind == DEVICE_CLAIM_GRANT &&
		    device_ids_related(recs, n, claims[i].id, recs[idx].id)) {
			out->state = DEVICE_STATE_HELD;
			out->claim = i;
			return;
		}
	}
	for (i = 0; i < claim_count; i++) {
		if (claims[i].kind == DEVICE_CLAIM_RESERVATION && claims[i].binding &&
		    device_ids_related(recs, n, claims[i].id, recs[idx].id)) {
			out->state = DEVICE_STATE_RESERVED;
			out->claim = i;
			return;
		}
	}
	host_or_unavailable(recs, n, idx, 1, out);
}

int device_grant_verdict(const struct discovered_device *recs, int n, int idx,
                         const struct device_claim *claims, int claim_count, int want_shared,
                         const char *requester, struct device_verdict *out)
{
	int i;

	out->state = DEVICE_STATE_FREE;
	out->reason = "";
	out->claim = -1;

	if (host_or_unavailable(recs, n, idx, 0, out) || host_or_unavailable(recs, n, idx, 1, out))
		return 400;

	for (i = 0; i < claim_count; i++) {
		const struct device_claim *c = &claims[i];

		if (strcmp(c->container, requester) == 0)
			continue;
		if (!device_ids_related(recs, n, c->id, recs[idx].id))
			continue;
		if (c->kind == DEVICE_CLAIM_GRANT) {
			/* Both sides must have asked to share (#356); a record that
			 * is host-wide by design never conflicts at all. */
			if (recs[idx].always_shared || (want_shared && c->shared))
				continue;
			out->state = DEVICE_STATE_HELD;
			out->claim = i;
			return 409;
		}
		if (c->binding) {
			out->state = DEVICE_STATE_RESERVED;
			out->claim = i;
			return 409;
		}
	}
	return 0;
}

const char *device_state_word(enum device_state state)
{
	switch (state) {
	case DEVICE_STATE_FREE:
		return "free";
	case DEVICE_STATE_HELD:
		return "held";
	case DEVICE_STATE_RESERVED:
		return "reserved";
	case DEVICE_STATE_HOST:
		return "host";
	case DEVICE_STATE_UNAVAILABLE:
		return "unavailable";
	}
	return "unavailable";
}

const char *device_reason_phrase(const char *reason)
{
	static const struct {
		const char *word;
		const char *phrase;
	} phrases[] = {
		{ "os-disk", "is the OS disk" },
		{ "os-partition", "is one of the OS disk's own partitions" },
		{ "storage-role", "holds a storage role, or sits on or under one" },
		{ "mounted", "is mounted by the host, or sits on or under something that is" },
		{ "bridge-port", "is a bridge port" },
		{ "management-interface", "carries the management address" },
		{ "no-driver", "has no driver bound" },
		{ "no-node", "has nothing of its own to grant -- grant one of the devices under it" },
	};
	size_t i;

	for (i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
		if (strcmp(phrases[i].word, reason) == 0)
			return phrases[i].phrase;
	}
	return "cannot be granted";
}

int device_records_share_a_node(const struct discovered_device *recs, int n, int *a, int *b)
{
	int i, j;

	for (i = 0; i < n; i++) {
		if (recs[i].dev_path[0] == '\0')
			continue;
		for (j = i + 1; j < n; j++) {
			if (recs[j].dev_path[0] == '\0')
				continue;
			if (recs[i].type == recs[j].type && recs[i].major == recs[j].major &&
			    recs[i].minor == recs[j].minor) {
				*a = i;
				*b = j;
				return 1;
			}
		}
	}
	return 0;
}

int device_records_share_an_id(const struct discovered_device *recs, int n, int *a, int *b)
{
	int i, j;

	for (i = 0; i < n; i++) {
		for (j = i + 1; j < n; j++) {
			if (strcmp(recs[i].id, recs[j].id) == 0) {
				*a = i;
				*b = j;
				return 1;
			}
		}
	}
	return 0;
}

static void write_one(struct json_writer *w, const struct discovered_device *recs, int n, int idx,
                      const struct device_claim *claims, int claim_count)
{
	const struct discovered_device *d = &recs[idx];
	struct device_verdict v;
	int i, j;

	device_state_of(recs, n, idx, claims, claim_count, &v);

	jw_obj_open(w);
	jw_key(w, "id");
	jw_str(w, d->id);
	jw_key(w, "bus");
	jw_str(w, d->bus);
	jw_key(w, "parent");
	jw_str(w, d->parent);
	jw_key(w, "vendor_id");
	jw_str(w, d->vendor_id);
	jw_key(w, "product_id");
	jw_str(w, d->product_id);
	jw_key(w, "class");
	jw_str(w, d->class_hex);
	jw_key(w, "description");
	jw_str(w, d->description);
	jw_key(w, "driver");
	jw_str(w, d->driver);
	jw_key(w, "dev_path");
	jw_str(w, d->dev_path);
	jw_key(w, "type");
	jw_str(w, d->type == DEVICE_NODE_BLOCK ? "block" : "char");
	jw_key(w, "major");
	jw_int(w, d->major);
	jw_key(w, "minor");
	jw_int(w, d->minor);
	jw_key(w, "serial");
	jw_str(w, d->serial);
	jw_key(w, "wwid");
	jw_str(w, d->wwid);
	jw_key(w, "mac");
	jw_str(w, d->mac);
	jw_key(w, "mac_permanent");
	jw_bool(w, d->mac_permanent);
	jw_key(w, "state");
	jw_str(w, device_state_word(v.state));
	jw_key(w, "reason");
	jw_str(w, v.reason);

	/*
	 * One entry per container and granted record. A container holding
	 * the same record through two entries (a mapping and its raw id) is
	 * one fact, and a repeat would read as two grants.
	 */
	jw_key(w, "held_by");
	jw_arr_open(w);
	for (i = 0; i < claim_count; i++) {
		int seen = 0;

		if (claims[i].kind != DEVICE_CLAIM_GRANT ||
		    !device_ids_related(recs, n, claims[i].id, d->id))
			continue;
		for (j = 0; j < i && !seen; j++) {
			seen = claims[j].kind == DEVICE_CLAIM_GRANT &&
			       strcmp(claims[j].container, claims[i].container) == 0 &&
			       strcmp(claims[j].id, claims[i].id) == 0;
		}
		if (seen)
			continue;
		jw_obj_open(w);
		jw_key(w, "container");
		jw_str(w, claims[i].container);
		jw_key(w, "granted");
		jw_str(w, claims[i].id);
		jw_key(w, "shared");
		jw_bool(w, claims[i].shared);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	jw_key(w, "reserved_by");
	jw_arr_open(w);
	for (i = 0; i < claim_count; i++) {
		int seen = 0;

		if (claims[i].kind != DEVICE_CLAIM_RESERVATION ||
		    !device_ids_related(recs, n, claims[i].id, d->id))
			continue;
		for (j = 0; j < i && !seen; j++) {
			seen = claims[j].kind == DEVICE_CLAIM_RESERVATION &&
			       strcmp(claims[j].container, claims[i].container) == 0 &&
			       strcmp(claims[j].id, claims[i].id) == 0;
		}
		if (seen)
			continue;
		jw_obj_open(w);
		jw_key(w, "container");
		jw_str(w, claims[i].container);
		jw_key(w, "claimed");
		jw_str(w, claims[i].id);
		jw_key(w, "binding");
		jw_bool(w, claims[i].binding);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	jw_key(w, "interfaces");
	jw_arr_open(w);
	for (i = 0; i < d->interface_count; i++) {
		const struct usb_interface_info *info = &d->interfaces[i];

		jw_obj_open(w);
		jw_key(w, "number");
		jw_int(w, info->number);
		jw_key(w, "class");
		jw_str(w, info->class_hex);
		jw_key(w, "subclass");
		jw_str(w, info->subclass_hex);
		jw_key(w, "protocol");
		jw_str(w, info->protocol_hex);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

void device_write_json_states(struct json_writer *w, const struct discovered_device *recs, int n,
                              const struct device_claim *claims, int claim_count)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < n; i++)
		write_one(w, recs, n, i, claims, claim_count);
	jw_arr_close(w);
}
