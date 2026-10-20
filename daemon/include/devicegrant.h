#ifndef DEVICEGRANT_H
#define DEVICEGRANT_H

#include "devicestate.h"
#include "registry.h"

#include <stddef.h>

/*
 * ADR-0344: the impure half of device state. devicestate.c decides; this
 * module gathers what it decides from -- the host's records, the
 * management interface, the records running containers took out of the
 * host's view, and every container's claims -- and is the ONE gate every
 * grant path calls. Never add a grant check beside devicegrant_check().
 */

/* Grants (a running container's devices and interfaces) plus
 * reservations (stopped definitions, and pending claims). Generous: 256
 * containers each naming a few devices is far above any real host. */
#define DEVICEGRANT_CLAIMS_MAX 1024

struct device_snapshot {
	struct discovered_device recs[DEVICE_ENUM_MAX];
	int n;
	struct device_claim claims[DEVICEGRANT_CLAIMS_MAX];
	int claim_count;
};

/*
 * Builds a fresh snapshot into static storage and returns it -- valid
 * until the next call. Never NULL.
 */
const struct device_snapshot *devicegrant_snapshot(void);

/*
 * May <requester> be granted record <id>? 0 when it may. Otherwise the
 * HTTP status the refusal is -- 404 when no such record exists, 400 when
 * the device itself cannot be granted (host, unavailable), 409 when
 * another container holds or reserves it -- and a sentence in err that
 * names the reason or the container.
 */
int devicegrant_check(const char *id, int want_shared, const char *requester, char *err,
                      size_t err_size);

/*
 * interfaces[] entry <ref> -- a mac mapping or a kernel name (ADR-0345)
 * -- resolved to the host's name for it, written to host: then the same
 * check, of the record a grant of it is really of -- the PHY for a
 * radio, which moves with every interface on it (ADR-0265) -- and, when
 * it may be granted, the record the registry keeps once the interface
 * has left the host's view. A mapping of another kind is a 400, an
 * ambiguous one a 409. The caller sets out->ifname.
 */
int devicegrant_check_interface(const char *ref, const char *requester,
                                struct registry_interface_record *out, char *host,
                                size_t host_size, char *err, size_t err_size);

/* GET /v1/devices' `devices` array. */
void devicegrant_write_json_list(struct json_writer *w);

#endif /* DEVICEGRANT_H */
