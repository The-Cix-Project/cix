#ifndef ZSWAP_H
#define ZSWAP_H

#include "json.h"

#include <stddef.h>

/*
 * Issue #51 (ADR-0196): zswap -- the kernel's compressed cache for
 * swap pages. Pages are compressed in RAM before they would otherwise
 * be written to the real swap device, so memory pressure costs CPU
 * instead of disk I/O.
 *
 * Not speculative: 192.168.15.95 went fully unresponsive during a real
 * -j6 gcc bootstrap that drove it into swap thrashing, on a disk this
 * box has already produced two kernel Oopses on in the page-cache /
 * writeback path (ADR-0178). zswap does not fix a bug in that path; it
 * reduces how often that path is entered at all.
 *
 * Every knob here is a real kernel module parameter under
 * /sys/module/zswap/parameters. This module owns the *intent* (what
 * the operator asked for, persisted across reboots) and applies it;
 * what the kernel actually has is read back separately, because a
 * setting and its effect are two different questions -- the kernel
 * silently ignores a compressor it was not built with, and a
 * configuration page that only echoed back its own input would never
 * show that.
 */

#define ZSWAP_COMPRESSOR_MAX 32

struct zswap_config {
	int enabled;
	int max_pool_percent;              /* 1-100; the kernel's own default is 20 */
	char compressor[ZSWAP_COMPRESSOR_MAX];
};

enum zswap_error {
	ZSWAP_OK = 0,
	ZSWAP_ERR_INVALID,
	ZSWAP_ERR_UNSUPPORTED,   /* this kernel has no zswap at all */
	ZSWAP_ERR_APPLY_FAILED,
	ZSWAP_ERR_PERSIST_FAILED
};

/* Loads persisted intent (a missing file leaves the documented
 * defaults) and applies it to the kernel. Never fails startup: a
 * kernel without zswap simply has nothing to apply to, which
 * zswap_write_json() then reports rather than hiding. */
void zswap_init(const char *path);
void zswap_repoint(const char *path);

const struct zswap_config *zswap_get(void);

/* Validates, persists, then applies. A compressor this kernel does not
 * have is refused here rather than written and silently ignored. */
enum zswap_error zswap_set(const struct zswap_config *next);

/* Whether /sys/module/zswap exists at all on this kernel. */
int zswap_supported(void);

/*
 * Writes the configured intent AND what the kernel actually reports,
 * plus the compressors this kernel was built with -- the list that
 * decides whether the compressor knob means anything.
 */
void zswap_write_json(struct json_writer *w);

#endif /* ZSWAP_H */
