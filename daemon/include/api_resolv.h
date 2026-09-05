#ifndef API_RESOLV_H
#define API_RESOLV_H

#include <stddef.h>

/*
 * api_resolv -- GET/PUT /v1/system/resolv (ADR-0076): the host's own
 * outbound DNS resolver configuration.
 *
 * resolv.c rewrites the real bind-mounted file directly, so a PUT takes
 * effect immediately with no reboot. See ADR-0249 for the boundary.
 */

void handle_resolv_get(int fd);
void handle_resolv_put(int fd, const char *body, size_t body_len);

#endif /* API_RESOLV_H */
