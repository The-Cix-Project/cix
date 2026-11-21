#ifndef ROUTECONFIG_H
#define ROUTECONFIG_H

#include "json.h"

#include <stdint.h>

/*
 * ADR-0387 (#715): the routes an operator added through
 * POST /v1/system/routes, kept, and put back in the kernel at every
 * start and whenever they are missing. Before this, an added route lived
 * only in the kernel: gibsson-1's default route via the cores' VRRP
 * address (192.168.253.254) was gone after every reboot, and the box was
 * unreachable until it was added again at the console.
 *
 * A route whose gateway cannot be reached yet (the cores are containers,
 * and start after this) is "waiting", not failed: routeconfig_apply()
 * tries it again from the main loop until the kernel takes it.
 */

#define ROUTECONFIG_MAX 64

struct route_saved {
	uint32_t dest_be;    /* 0 with prefix 0: the default route */
	int prefix;
	uint32_t gateway_be; /* 0: none (an on-link route) */
	int applied;         /* in memory: the kernel has it, as far as this process knows */
};

/* Loads the saved list; a missing file is an empty list. 0 or -1. */
int routeconfig_init(const char *state_path);

/* Keeps a route (an existing identical one is not added twice).
 * 0, or -1 when the list is full or the file cannot be written. */
int routeconfig_add(uint32_t dest_be, int prefix, uint32_t gateway_be, int applied);

/* Forgets a route; gateway 0 matches any gateway, as the kernel's
 * delete does. 1 when one was kept, 0 when none was. */
int routeconfig_del(uint32_t dest_be, int prefix, uint32_t gateway_be);

/*
 * Puts every kept route the kernel does not have back, and says once in
 * the log for each one that goes in after waiting. Cheap when nothing
 * waits. Returns how many are still waiting; *changed is set when any
 * route went in or dropped out, so the caller announces the read.
 */
int routeconfig_apply(int *changed);

/* "saved": [{"dest","prefix","gateway","state":"applied"|"waiting"}] */
void routeconfig_write_json(struct json_writer *w);

#endif
