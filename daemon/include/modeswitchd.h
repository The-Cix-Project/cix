#ifndef MODESWITCHD_H
#define MODESWITCHD_H

/*
 * ADR-0398: the daemon's half of the mode switcher -- the rules a device
 * is decided by, the facts gathered from the device records (ADR-0344),
 * the per-port record of what a switch did, and the `modeswitch` key of
 * every device record. The verdict and the plan are modeswitch.c's; the
 * transfers are modeswitch_usb.c's, in a helper child main.c starts.
 *
 * A port's record lives in this process's memory only. A handover waits
 * while a switch is in flight (modeswitch_in_flight()), and what is left
 * after it -- that a device switched, or that one failed -- is the log's
 * to keep: after a handover a switched device simply reads as its target
 * id, and a failed one as switchable again.
 */

#include "devicestate.h"
#include "json.h"
#include "modeswitch.h"

#include <stddef.h>

/* One switch, from the request that asked to the helper that sends it. */
struct modeswitch_job {
	char id[DEVICE_ID_MAX];      /* the record switched */
	char dev_path[64];           /* its usbfs node */
	struct modeswitch_rule rule;
};

/* Switching runs only on an installed host (init mode): a test daemon
 * never sends a message to the build host's devices. */
void modeswitch_set_active(int active);

/* The `modeswitch` key of device record idx: null when no rule names the
 * device, else what was done or would be done, and why. */
void modeswitch_write_device(struct json_writer *w, const struct discovered_device *recs, int n,
                             int idx, const struct device_verdict *v);

/*
 * POST /v1/devices/{id}/modeswitch: whether record id may be switched
 * now, from a fresh device snapshot. 0 with job filled when it may;
 * otherwise the HTTP status (404 no such record, 409 refused, already
 * switching or not active, 422 no rule) and a sentence in why.
 */
int modeswitch_prepare(const char *id, struct modeswitch_job *job, char *why, size_t why_size);

/* The helper was started for job: its port now reads "switching". */
void modeswitch_started(const struct modeswitch_job *job);

/* The helper ended: ok and error are modeswitch_usb_run()'s. */
void modeswitch_finished(const struct modeswitch_job *job, int ok, const char *error);

/* A USB device was added: a switching port that now holds a target id
 * has switched. */
void modeswitch_usb_added(void);

/* Once a second from the loop: a switch whose device has not come back
 * as a target by its deadline has failed. */
void modeswitch_tick(void);

/* A switch is between its request and its outcome. */
int modeswitch_in_flight(void);

#endif
