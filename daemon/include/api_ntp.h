#ifndef API_NTP_H
#define API_NTP_H

#include <stddef.h>

#include "ntp.h"

/*
 * api_ntp -- REST handlers for the host's NTP configuration and the
 * server containers registered against it.
 *
 * handle_ntp_sync_post() is deliberately NOT here. It calls
 * start_ntp_sync_job(), which arms a timerfd and registers a socket
 * with the daemon's epoll loop, so it belongs with the loop and stays
 * in main.c. That is the boundary this split follows generally: a
 * handler that only reads or writes subsystem state moves out, and a
 * handler that drives the event loop stays with it. Drawing the line
 * anywhere else would mean exporting the loop's internals.
 */
/*
 * The enum-to-status mapping for this subsystem. Exported because
 * handle_ntp_sync_post() stays in main.c (it drives the event loop)
 * and still has to report the same failures the same way -- one
 * mapping rather than two that drift.
 */
void respond_ntp_error(int fd, enum ntp_error err);

void handle_ntp_config_get(int fd);
void handle_ntp_config_put(int fd, const char *body, size_t body_len);
void handle_ntp_status_get(int fd);
void handle_ntp_server_create(int fd, const char *body, size_t body_len);
void handle_ntp_server_list(int fd);
void handle_ntp_server_delete(int fd, const char *name);

#endif /* API_NTP_H */
