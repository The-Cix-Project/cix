#ifndef API_STORAGE_H
#define API_STORAGE_H

#include <stddef.h>

#include "diskformat.h"
#include "disk.h"
#include "storageplacement.h"

/*
 * api_storage -- REST handlers for the hardware underneath everything:
 * PCI/USB devices and their named mappings, block devices, the roles a
 * disk can hold, formatting and unmounting, and the partition table.
 *
 * One module because an operator reaches for these as one subject --
 * "what hardware does this box have and what is it doing" -- and they
 * share the protected-layout rule that keeps the OS disk's structural
 * partitions untouchable (#140).
 *
 * handle_disk_format_post() is NOT here: it spawns the format job and
 * registers its pidfd with the event loop, so it stays in main.c with
 * the loop (ADR-0249 rule 2).
 */

/*
 * Whether a placement is the only active one of its kind, so removing
 * or reformatting its disk would leave nothing behind it. Exported
 * because handle_disk_format_post() stays in main.c (it registers the
 * format job's pidfd with the event loop) and applies the same rule.
 */
int is_active_storage_singleton_placement(const char *disk_name);

int disk_holds_volumes(const char *disk_name);

/*
 * Mount a disk that has just been given a role, when it already
 * carries a filesystem (#660). Before this the mount waited for the
 * next boot's remount pass, so the role took and the disk stayed
 * unmounted. Called by every path that assigns a role -- the REST
 * create and a configuration apply -- and the outcome goes to the log
 * store. Not mounting is never an error to the caller: the role is
 * already recorded, and the storage list says whether it is mounted.
 */
void diskrole_mount_assigned(const char *disk_name, const char *containers_dir);

void respond_diskformat_error(int fd, enum diskformat_error err);

void handle_device_list(int fd);
void handle_devicemap_create(int fd, const char *body, size_t body_len);
void handle_devicemap_delete(int fd, const char *name);
void handle_devicemap_list(int fd);
void handle_disk_format_get(int fd, const char *disk_name);
void handle_disk_free_space(int fd, const char *disk_name);
void handle_disk_list(int fd);
void handle_disk_partition_delete(int fd, const char *disk_name, const char *partition_name);
void handle_disk_partition_resize(int fd, const char *disk_name, const char *partition_name,                                           const char *body, size_t body_len);
void handle_disk_partition_table_post(int fd, const char *disk_name, const char *body,                                               size_t body_len);
void handle_disk_partitions_post(int fd, const char *disk_name, const char *body,                                          size_t body_len);
void handle_disk_unmount_post(int fd, const char *disk_name, const char *body, size_t body_len);
void handle_diskrole_create(int fd, const char *body, size_t body_len);
void handle_diskrole_delete(int fd, const char *disk_name);
void handle_diskrole_list(int fd);

#endif /* API_STORAGE_H */
