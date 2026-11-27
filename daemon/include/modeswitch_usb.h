#ifndef MODESWITCH_USB_H
#define MODESWITCH_USB_H

#include "modeswitch.h"

/* ADR-0398: what one switch's transfers did. */
struct modeswitch_result {
	int ok;          /* every message was sent, or the device left part way */
	int steps_sent;
	int left_early;  /* the device left before the last message: it switched */
	char error[160]; /* why not, when ok is 0 */
};

/*
 * Sends rule r's switch to the device at dev_path (/dev/bus/usb/BBB/DDD).
 * Runs only in a helper child: every transfer has a timeout, and it
 * returns without waiting for the device to re-enumerate. 0, or -1 with
 * res->error set.
 */
int modeswitch_usb_run(const char *dev_path, const struct modeswitch_rule *r,
                       struct modeswitch_result *res);

#endif
