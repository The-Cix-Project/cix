#ifndef KSM_H
#define KSM_H

#include "json.h"

#include <stddef.h>

/*
 * Issue #50: KSM -- kernel samepage merging. The kernel scans anonymous
 * memory and collapses identical pages onto one physical copy.
 *
 * Not speculative here. Containers on this platform routinely run close
 * to their own memory.max ceiling -- a gcc bootstrap hit its full 4 GiB
 * on 192.168.15.95 -- and they are overwhelmingly built from the SAME
 * image, so identical pages across containers are the ordinary case
 * rather than a lucky one. That is precisely the shape KSM is good at.
 *
 * Two halves, and neither works alone:
 *
 *   This module is the host half -- whether the scanner runs at all,
 *   and how hard. Every knob is a real file under /sys/kernel/mm/ksm.
 *
 *   The container half is the opt-in. KSM merges nothing that has not
 *   volunteered: a process must mark its memory mergeable, which
 *   container.c does with prctl(PR_SET_MEMORY_MERGE) for a container
 *   that asks. Turning the scanner on while nothing has opted in costs
 *   CPU and saves nothing, which is why the default here is off.
 *
 * Same split as zswap (ADR-0196) between what was ASKED FOR and what
 * the kernel actually has: intent is persisted and applied, and the
 * readback is separate, because a page that only echoed its own input
 * could never show a kernel that ignored it.
 */

struct ksm_config {
	int enabled;          /* writes /sys/kernel/mm/ksm/run: 1 run, 0 stop */
	int pages_to_scan;    /* pages per scan cycle; kernel default 100 */
	int sleep_millisecs;  /* pause between cycles; kernel default 20 */
};

enum ksm_error {
	KSM_OK = 0,
	KSM_ERR_INVALID,
	KSM_ERR_UNSUPPORTED,   /* this kernel has no KSM at all */
	KSM_ERR_APPLY_FAILED,
	KSM_ERR_PERSIST_FAILED
};

/* Loads persisted intent (a missing file leaves the documented
 * defaults) and applies it. Never fails startup: a kernel without KSM
 * has nothing to apply to, which ksm_write_json() reports rather than
 * hides. */
void ksm_init(const char *path);
void ksm_repoint(const char *path);

const struct ksm_config *ksm_get(void);

/* Validates, persists, then applies. */
enum ksm_error ksm_set(const struct ksm_config *next);

/* Whether /sys/kernel/mm/ksm exists on this kernel. */
int ksm_supported(void);

/*
 * Writes the configured intent, what the kernel actually reports, and
 * the savings. The savings are the only honest measure of whether this
 * is worth its CPU: pages_sharing is how many pages were eliminated,
 * and a deployment where that stays near zero is paying to scan for
 * nothing.
 */
void ksm_write_json(struct json_writer *w);

#endif /* KSM_H */
