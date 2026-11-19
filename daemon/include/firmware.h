#ifndef CIX_FIRMWARE_H
#define CIX_FIRMWARE_H

#include <stddef.h>

/*
 * ADR-0381: a host keeps its firmware in its own image, and the kernel
 * reads it in place.
 *
 * The kernel's firmware loader searches one configurable directory
 * first: /sys/module/firmware_class/parameters/path, 255 bytes, resolved
 * in pid 1's filesystem view (kernel 7.3.5's main.c:471-485 and
 * kernel_read_file_from_path_initns(), read by probe-fwpath@1-1). cixd
 * points it at one stable path, a symlink naming the firmware image's
 * current version's lib/firmware, so a new version replaces the link and
 * never the parameter.
 *
 * firmware_link_set() and firmware_path_apply() are the only writers of
 * the link and of the parameter. Both are called only in init mode: the
 * parameter is kernel-global, and a test daemon must never repoint the
 * host's loader.
 */

#define FIRMWARE_IMAGE "firmware"
#define FIRMWARE_PARAM_PATH "/sys/module/firmware_class/parameters/path"
/* What the kernel accepts: fw_path_para[256], NUL included. */
#define FIRMWARE_PATH_MAX 255

/*
 * Points link at target, atomically: a symlink made beside it and
 * renamed over it, so a reader sees the old target or the new one and
 * never no link. target NULL or "" points it at empty_dir instead, which
 * is created when absent, so the kernel's search finds nothing rather
 * than a dangling link. Writes nothing when link already names the
 * target. 0, or -1 with err written.
 */
int firmware_link_set(const char *link, const char *target, const char *empty_dir, char *err,
                      size_t err_size);

/* What link names now, into out; "" when it is not a symlink. */
void firmware_link_target(const char *link, char *out, size_t out_size);

/*
 * Writes path to the kernel parameter file param (FIRMWARE_PARAM_PATH;
 * a test passes its own file). Refuses a path the kernel would truncate,
 * and writes nothing when the parameter already reads path. 0, or -1
 * with err written.
 */
int firmware_path_apply(const char *param, const char *path, char *err, size_t err_size);

#endif
