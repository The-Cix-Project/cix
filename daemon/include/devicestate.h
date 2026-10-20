#ifndef DEVICESTATE_H
#define DEVICESTATE_H

#include "device.h"
#include "json.h"

/*
 * ADR-0344: the one answer to "can this device be granted, and if not,
 * why". GET /v1/devices reports it and every grant path asks it, so a
 * refusal and the list can never disagree.
 *
 * PURE. Everything here takes records and claims as data and reads no
 * file, calls no enumerator and touches no global -- the impure half
 * (sysfs, the registry, the stored definitions) is devicegrant.c. That
 * split is what lets test_devicestate prove every rule in SELFTESTS,
 * where a build container's sysfs is not the box's.
 */

enum device_state {
	DEVICE_STATE_FREE,
	DEVICE_STATE_HELD,
	DEVICE_STATE_RESERVED,
	DEVICE_STATE_HOST,
	DEVICE_STATE_UNAVAILABLE,
};

#define DEVICE_CONTAINER_NAME_MAX 64

enum device_claim_kind {
	DEVICE_CLAIM_GRANT,       /* a running container holds it */
	DEVICE_CLAIM_RESERVATION, /* a stopped definition names it, or a running one waits for it */
};

/*
 * A container's relation to one record. `id` is the record the grant
 * resolved to -- for a wireless interface that is its PHY, the unit
 * that moves (ADR-0265) -- never the text an operator wrote.
 */
struct device_claim {
	enum device_claim_kind kind;
	char container[DEVICE_CONTAINER_NAME_MAX];
	char id[DEVICE_ID_MAX];
	int shared;  /* grant: declared "shared" (#356) */
	int binding; /* reservation: stopped, with reserve_devices */
};

struct device_verdict {
	enum device_state state;
	const char *reason; /* a Device.reason word, "" when the state carries none */
	int claim;          /* index of the claim that decided it, or -1 */
};

/*
 * A /dev node, or a network interface or PHY, which moves into a
 * namespace instead. A PCI function whose nodes belong to its children
 * is a record and a parent, and has nothing of its own to grant.
 */
int device_is_grant_unit(const struct discovered_device *d);

/* Index of the record with this id, or -1. */
int device_record_index(const struct discovered_device *recs, int n, const char *id);

/*
 * Whether record a is record b, an ancestor of it or a descendant of it
 * -- the relation holding follows. Ids, not indexes, so a claim naming
 * a record that is no longer enumerated still matches itself.
 */
int device_ids_related(const struct discovered_device *recs, int n, const char *a, const char *b);

/* The state GET /v1/devices reports for recs[idx]. */
void device_state_of(const struct discovered_device *recs, int n, int idx,
                     const struct device_claim *claims, int claim_count,
                     struct device_verdict *out);

/*
 * May <requester> be granted recs[idx]? Returns 0 when it may. Otherwise
 * fills out and returns 400 (host or unavailable: the device itself
 * cannot be granted) or 409 (another container holds or reserves it).
 * The requester's own claims never refuse it, so a definition with
 * reserve_devices can always start onto its own reservation.
 */
int device_grant_verdict(const struct discovered_device *recs, int n, int idx,
                         const struct device_claim *claims, int claim_count, int want_shared,
                         const char *requester, struct device_verdict *out);

/* "free", "held", ... -- the Device.state words. */
const char *device_state_word(enum device_state state);

/* "the OS disk", "a bridge port", ... -- a reason for a refusal sentence. */
const char *device_reason_phrase(const char *reason);

/*
 * The gate that makes one id mean one node: returns 1 and the two
 * indexes when two records own the same /dev node, 0 when none do.
 */
int device_records_share_a_node(const struct discovered_device *recs, int n, int *a, int *b);

/*
 * The gate that makes one id mean one record (ADR-0345): returns 1 and
 * the two indexes when two records carry the same id, 0 when none do.
 * An id names a location, so no present device can produce one; the
 * gate checks the rule rather than enforcing it.
 */
int device_records_share_an_id(const struct discovered_device *recs, int n, int *a, int *b);

/* The whole GET /v1/devices body's array, in record order. */
void device_write_json_states(struct json_writer *w, const struct discovered_device *recs, int n,
                              const struct device_claim *claims, int claim_count);

#endif /* DEVICESTATE_H */
