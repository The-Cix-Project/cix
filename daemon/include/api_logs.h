#ifndef API_LOGS_H
#define API_LOGS_H

#include <stddef.h>

#include "http.h"

/*
 * api_logs -- the two log windows a shell-less installed host has.
 *
 * GET /v1/system/kmsg is the kernel's own ring buffer: dmesg-class
 * diagnostics (mount failures, driver probes, OOM) with no shell to run
 * dmesg in. GET /v1/system/logs is cixd's own consolidated store, with
 * its retention configuration beside it.
 *
 * Distinct on purpose -- one is the kernel talking, the other is this
 * daemon, and conflating them would lose which. logstore.c owns both
 * stores; this is where they meet HTTP.
 */

void handle_kmsg(int fd, const struct http_request *req);
void handle_logs_config_get(int fd);
void handle_logs_config_put(int fd, const char *body, size_t body_len);
void handle_logs_get(int fd, const struct http_request *req);

#endif /* API_LOGS_H */
