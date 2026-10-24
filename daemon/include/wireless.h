#ifndef WIRELESS_H
#define WIRELESS_H

#include "json.h"

/*
 * The host's wireless regulatory domain (ADR-0354, #355).
 *
 * The kernel holds one domain for the whole host and accepts it only
 * from CAP_NET_ADMIN in the initial user namespace, so a container's
 * hostapd is refused and the daemon sets it instead. This module owns
 * the intent (the country the operator asked for, persisted) and sends
 * it; what the kernel actually has is read back separately, because the
 * kernel applies a country asynchronously and drops one its database
 * lacks.
 */

enum wireless_error {
	WIRELESS_OK = 0,
	WIRELESS_ERR_INVALID,        /* not two upper-case letters */
	WIRELESS_ERR_PERSIST_FAILED
};

/* Loads the persisted country, if any, and sends it to the kernel.
 * Never fails start-up: a refusal is kept for wireless_write_json() to
 * report. Called before the kmod sweep and boot autostart, so a radio
 * and an access point find the domain already in place. */
void wireless_init(const char *path);

/* Validates, persists, then sends. country NULL clears the setting and
 * sends the world domain "00". A kernel refusal is not an error here:
 * it is recorded and reported, and the setting is kept. */
enum wireless_error wireless_set(const char *country);

/* The configured country, the kernel's domain, and each radio's. */
void wireless_write_json(struct json_writer *w);

#endif /* WIRELESS_H */
