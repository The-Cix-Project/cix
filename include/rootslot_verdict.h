#ifndef CIX_ROOTSLOT_VERDICT_H
#define CIX_ROOTSLOT_VERDICT_H

#include <string.h>

/*
 * ADR-0379: is the running control-plane root behind its image?
 *
 * running: the cix-root version the running slot was sealed from, as
 *          cixd recorded it when the roll staged that slot; "" when
 *          unknown (an installer-written slot, one written before
 *          0.2.57-691, or one written by any path but the roll).
 * current: cix-root's current version; "" when the image has none.
 * settled: whether cix-root is settled (pkg_image_settled()): no job in
 *          flight for it and not queued. A converging image is not
 *          behind yet.
 * bad:     the version whose switch or boot failed (decision 3), "" for
 *          none. While it is the current version, the root is not
 *          behind; once cix-root moves past it, the mark no longer
 *          applies.
 */
enum rootslot_verdict {
	ROOTSLOT_CURRENT,   /* the running root was sealed from the current version */
	ROOTSLOT_BEHIND,    /* deploy the current version */
	ROOTSLOT_UNSETTLED, /* cix-root is still converging: decide when it settles */
	ROOTSLOT_FAILED,    /* the current version failed to deploy: wait for cix-root to move */
	ROOTSLOT_NO_IMAGE   /* cix-root has no current version: nothing to deploy */
};

static inline enum rootslot_verdict rootslot_verdict(const char *running, const char *current,
                                                     int settled, const char *bad)
{
	if (current == NULL || current[0] == '\0')
		return ROOTSLOT_NO_IMAGE;
	if (running != NULL && strcmp(running, current) == 0)
		return ROOTSLOT_CURRENT;
	if (!settled)
		return ROOTSLOT_UNSETTLED;
	if (bad != NULL && strcmp(bad, current) == 0)
		return ROOTSLOT_FAILED;
	return ROOTSLOT_BEHIND;
}

#endif
