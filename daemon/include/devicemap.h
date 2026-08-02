#ifndef DEVICEMAP_H
#define DEVICEMAP_H

#include "device.h"
#include "json.h"

/*
 * Persistent, operator-named device mappings (ADR-0048) -- raised
 * directly by the user, Proxmox-style: host hardware (device.c) is
 * deliberately never persisted, re-enumerated fresh from sysfs on
 * every call, since the daemon doesn't own or create it. That's
 * correct for discovery, but leaves nothing an operator can build a
 * stable container spec around -- a USB device's own `id` bakes in
 * its physical port path or serial, so a container referencing a raw
 * id either breaks the moment that specific device is unplugged and
 * replugged elsewhere, or has to be pinned to one exact device
 * forever with no way to say "whichever device of this make/model is
 * plugged in."
 *
 * A mapping is a real, separate, persisted layer on top of discovery:
 * a name an operator chooses (same charset as every other simple
 * resource name -- container/network/package -- via
 * simple_name_is_valid(), reused not reimplemented) bound to either
 * one exact device (`kind == "exact"`, selector is that device's own
 * stable `id` from GET /v1/devices) or a vendor/model pattern
 * (`kind == "vendor_model"`, selector is `"<vendor_id>:<product_id>"`)
 * that resolves against whatever currently matches on every call,
 * never a snapshot -- host hardware can change between calls (a USB
 * device unplugged and replugged elsewhere), so resolution is always
 * done fresh, mirroring device_find()/device_find_group()'s own
 * "simple and correct rather than cached" precedent.
 *
 * A mapping is real and creatable even for hardware that isn't
 * currently plugged in -- GET /v1/devicemaps reports whether each one
 * currently resolves to something, rather than refusing to exist
 * otherwise; an operator predefining "the USB serial adapter" before
 * plugging it in, or temporarily unplugging one, are both legitimate,
 * ordinary states, not errors.
 */

#define DEVICEMAP_NAME_MAX 64
/* One id (96 bytes, device.h's own DEVICE_ENUM_MAX-sized field) for
 * "exact", or "<vendor_id>:<product_id>" (each up to 15 bytes per
 * struct discovered_device's own fields, plus the ':' and NUL) for
 * "vendor_model" -- one bound generously covers both shapes. */
#define DEVICEMAP_SELECTOR_MAX 96
#define DEVICEMAP_MAX 64

enum devicemap_kind {
	DEVICEMAP_EXACT,
	DEVICEMAP_VENDOR_MODEL,
};

enum devicemap_error {
	DEVICEMAP_OK = 0,
	DEVICEMAP_ERR_INVALID_NAME,
	DEVICEMAP_ERR_INVALID_KIND,
	DEVICEMAP_ERR_INVALID_SELECTOR,
	DEVICEMAP_ERR_DUPLICATE,
	DEVICEMAP_ERR_FULL,
	DEVICEMAP_ERR_NOT_FOUND,
	DEVICEMAP_ERR_PERSIST_FAILED
};

/* Loads state_path (the persisted mapping list, if any) at startup. */
int devicemap_init(const char *state_path);

/*
 * Creates a new mapping. kind_str must be exactly "exact" or
 * "vendor_model". For "exact", selector is a device id verbatim (not
 * required to currently resolve to anything -- see this header's own
 * comment on why). For "vendor_model", selector must be
 * "<vendor_id>:<product_id>" (two non-empty parts split by exactly
 * one ':'). PKI/DNS/pkg's own "name must be unique" precedent applies
 * here too: DEVICEMAP_ERR_DUPLICATE if name is already mapped.
 */
enum devicemap_error devicemap_create(const char *name, const char *kind_str,
                                       const char *selector);

enum devicemap_error devicemap_delete(const char *name);

/*
 * Resolves name to 0 or more currently-matching discovered devices,
 * re-enumerating fresh every call (never cached across calls -- see
 * this header's own top comment). Writes up to cap pointers into out,
 * valid until the next devicemap_resolve() call (same per-call-static-
 * cache convention device_find()/device_find_group() themselves
 * already use). Returns the count found (0 if name is a real mapping
 * but nothing currently matches -- e.g. the device is unplugged), or
 * -1 if name does not name any mapping at all (the caller -- main.c's
 * own container-creation devices[] parsing -- falls back to treating
 * it as a raw device.h id in that case, not this one).
 */
int devicemap_resolve(const char *name, const struct discovered_device *out[], int cap);

/*
 * Writes one mapping (name, kind, selector, and -- resolved fresh via
 * devicemap_resolve() -- present: bool plus the real id(s) of whatever
 * currently matches) into w. Returns 1 if name is a real mapping, 0
 * (writes nothing) otherwise.
 */
int devicemap_write_json_one(const char *name, struct json_writer *w);

/* Same per-entry shape as devicemap_write_json_one(), for every
 * mapping, wrapped in a JSON array. */
void devicemap_write_json_list(struct json_writer *w);

#endif /* DEVICEMAP_H */
