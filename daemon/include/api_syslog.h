#ifndef API_SYSLOG_H
#define API_SYSLOG_H

#include <stddef.h>

/*
 * api_syslog -- REST handlers for external syslog forward targets
 * (logging epic Part 2, ADR-0127).
 *
 * Registering a running container -- typically syslog-1/syslog-2, a
 * real syslogd -- as a forward target for every container-sourced log
 * line. syslogfwd.c owns the registrations and the forwarding; this is
 * the only place it meets HTTP. See ADR-0249 for the boundary.
 */

void handle_syslog_target_create(int fd, const char *body, size_t body_len);
void handle_syslog_target_delete(int fd, const char *name);
void handle_syslog_target_list(int fd);

#endif /* API_SYSLOG_H */
