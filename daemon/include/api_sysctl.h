#ifndef API_SYSCTL_H
#define API_SYSCTL_H

#include <stddef.h>

/*
 * api_sysctl -- the REST handlers for the host sysctl surface
 * (ADR-0160), moved out of main.c.
 *
 * The split this file is one instance of: the operation handlers are
 * the bulk of main.c, and they were stuck there because the two
 * functions every one of them calls -- respond_json() and
 * respond_error() -- were static in that file. With those in apiresp.h
 * a handler group can live beside the subsystem it serves.
 *
 * The op_ wrappers stay in main.c and that is structural rather than
 * unfinished: the generated route table forward-declares them as
 * static and is #included there, so they must be in that translation
 * unit. Each is one line calling into a handler declared here.
 *
 * Note the layering these handlers sit in, which does not change:
 * sysctlconfig.c owns persistence and validation and returns enums; it
 * knows nothing about HTTP. This file is the only place the two meet.
 */
void handle_sysctl_get(int fd, const char *key);
void handle_sysctl_put(int fd, const char *key, const char *body, size_t body_len);
void handle_sysctl_delete(int fd, const char *key);
void handle_sysctl_list(int fd);

#endif /* API_SYSCTL_H */
