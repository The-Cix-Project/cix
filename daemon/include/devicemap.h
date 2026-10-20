#ifndef DEVICEMAP_H
#define DEVICEMAP_H

#include "device.h"
#include "devicematch.h"
#include "json.h"

/*
 * Persistent, operator-named device mappings: ADR-0048, rebuilt by
 * ADR-0345. A device's id says where it is; a mapping says which device
 * it is, so a container spec can name a device once and keep it across
 * a re-plug, a second identical device and a reboot that probes in a
 * different order. Only the mapping persists (name, kind, selector);
 * what it matches is decided on every call, by devicematch.c, against
 * the records the caller passes -- the grant snapshot's, which include
 * an interface a running container holds.
 *
 * A mapping is real and creatable for hardware that is not plugged in:
 * an operator naming a device before plugging it in, or one unplugged
 * for now, is an ordinary state, reported as "absent".
 */

#define DEVICEMAP_NAME_MAX 64
#define DEVICEMAP_SELECTOR_MAX DEVICEMATCH_SELECTOR_MAX
#define DEVICEMAP_MAX 64

/* devicemap_resolve()'s two answers that are not a count. */
#define DEVICEMAP_NOT_A_MAPPING (-1)
#define DEVICEMAP_AMBIGUOUS (-2)

enum devicemap_error {
	DEVICEMAP_OK = 0,
	DEVICEMAP_ERR_INVALID_NAME,
	DEVICEMAP_ERR_INVALID_KIND,
	DEVICEMAP_ERR_INVALID_SELECTOR, /* err says why */
	DEVICEMAP_ERR_DUPLICATE,
	DEVICEMAP_ERR_FULL,
	DEVICEMAP_ERR_NOT_FOUND,
	DEVICEMAP_ERR_PERSIST_FAILED
};

/* Loads state_path (the persisted mapping list, if any) at startup. An
 * entry it cannot read is logged and skipped: one bad mapping must not
 * keep the daemon from starting. */
int devicemap_init(const char *state_path);

/* Says, once the log store is open, how many entries init skipped. */
void devicemap_log_skipped(void);

/* ADR-0141 Phase 2: repoints without reloading -- see network_repoint()'s
 * own doc comment for the shared reasoning. */
void devicemap_repoint(const char *new_state_path);

/*
 * Creates a mapping. kind is a contract kind word; the selector is
 * checked and stored in its one normal form by
 * devicematch_check_selector(), against recs (the records present now).
 * DEVICEMAP_ERR_INVALID_SELECTOR carries the sentence in err.
 */
enum devicemap_error devicemap_create(const char *name, const char *kind, const char *selector,
                                      const struct discovered_device *recs, int n, char *err,
                                      size_t err_size);

enum devicemap_error devicemap_delete(const char *name);

/*
 * What mapping <name> matches among recs. DEVICEMAP_NOT_A_MAPPING when
 * name is no mapping (the caller then treats it as a raw id). Otherwise
 * the count written to out (0 when nothing matches now), or
 * DEVICEMAP_AMBIGUOUS when a kind that names one device matches several,
 * with a sentence in err naming them. out points into recs.
 */
int devicemap_resolve(const char *name, const struct discovered_device *recs, int n,
                      const struct discovered_device *out[], int cap, char *err,
                      size_t err_size);

/* 1 when <name> is a mapping that names an interface (kind mac), which
 * devices[] refuses in favour of interfaces[]. */
int devicemap_names_interface(const char *name);

/* One mapping -- name, kind, selector, state and resolved_ids -- into w.
 * Returns 1 if name is a mapping, 0 (writes nothing) otherwise. */
int devicemap_write_json_one(const char *name, const struct discovered_device *recs, int n,
                             struct json_writer *w);

/* Every mapping, in that shape, as a JSON array. */
void devicemap_write_json_list(struct json_writer *w, const struct discovered_device *recs, int n);

#endif /* DEVICEMAP_H */
