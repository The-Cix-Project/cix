#ifndef API_KMOD_H
#define API_KMOD_H

#include <stddef.h>

/*
 * api_kmod -- REST handlers for kernel modules (ADR-0159 Phase A):
 * the live modprobe/modinfo surface and the persisted autoload
 * configuration beside it.
 *
 * Two subsystems in one file because they are one operator concern and
 * share a name validator: kmod.c runs the real tools, kmodconfig.c
 * remembers what should be loaded at boot. Neither knows about HTTP.
 * See api_sysctl.h for why the op_ wrappers stay in main.c.
 */
void handle_kmod_post(int fd, const char *name, const char *body, size_t body_len);
void handle_kmod_delete(int fd, const char *name);
void handle_kmod_list(int fd);
void handle_kmod_get(int fd, const char *name);
void handle_kmodconfig_put(int fd, const char *name, const char *body, size_t body_len);
void handle_kmodconfig_delete(int fd, const char *name);
void handle_kmodconfig_list(int fd);
/* ADR-0348: the module-load policy and the last sweep's report, and a sweep
 * queued on request. */
void handle_kmodpolicy_get(int fd);
void handle_kmodpolicy_put(int fd, const char *body, size_t body_len);
void handle_kmodscan_get(int fd);
void handle_kmodscan_post(int fd);

#endif /* API_KMOD_H */
