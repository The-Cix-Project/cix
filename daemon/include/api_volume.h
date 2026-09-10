#ifndef API_VOLUME_H
#define API_VOLUME_H

#include <stddef.h>

#include "volumebackup.h"

/*
 * api_volume -- REST handlers for persistent volumes (issue #88) and
 * their snapshot schedule (issue #96).
 *
 * The largest single group in main.c at 833 lines: create, list, get,
 * delete, migrate, owner, quota, plus the backup config, policy,
 * on-demand snapshot, listing, delete and restore. volume.c and
 * volumebackup.c own the state and the work; this is where they meet
 * HTTP. See ADR-0249 for the boundary.
 */

void handle_volume_backup_config_get(int fd);
void handle_volume_backup_config_put(int fd, const char *body, size_t body_len);
void handle_volume_backup_delete(int fd, const char *name, const char *stamp);
void handle_volume_backup_now(int fd, const char *name);
void handle_volume_backup_policy_put(int fd, const char *name, const char *body,                                              size_t body_len);
void handle_volume_backups_get(int fd, const char *name);
void handle_volume_create(int fd, const char *body, size_t body_len);
void handle_volume_delete(int fd, const char *name);
void handle_volume_get(int fd, const char *name);
void handle_volume_list(int fd);
void handle_volume_migrate(int fd, const char *name, const char *body, size_t body_len);
void handle_volume_owner_put(int fd, const char *name, const char *body, size_t body_len);
void handle_volume_usage(int fd, const char *name);
void handle_volume_quota_put(int fd, const char *name, const char *body, size_t body_len);
void handle_volume_restore(int fd, const char *name, const char *body, size_t body_len);

/* See the definition for why this is an accessor and not a global. */
const struct volumebackup_hooks *api_volume_backup_hooks(void);

#endif /* API_VOLUME_H */
