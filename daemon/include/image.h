#ifndef IMAGE_H
#define IMAGE_H

#include "json.h"

/*
 * Images are pure filesystem state -- a directory under images_dir
 * containing its own rootfs/ subdirectory, the exact same convention
 * pkg.c's own image_rootfs_path() already established for `pkg
 * install`'s target images. No separate persisted registry: an image
 * *is* its directory, re-enumerated fresh on every list/get call,
 * same "host state, not daemon-owned state" posture device.c already
 * has for hardware (ADR-0012's atomic-JSON persistence pattern is for
 * genuinely daemon-owned state, which this isn't).
 */

enum image_error {
	IMAGE_OK = 0,
	IMAGE_ERR_INVALID_NAME,
	IMAGE_ERR_DUPLICATE,
	IMAGE_ERR_NOT_FOUND,
	IMAGE_ERR_PROTECTED,     /* "base" itself -- every container's own default, never removable */
	IMAGE_ERR_IN_USE,        /* a running container references it */
	IMAGE_ERR_HAS_PACKAGES,  /* pkg.c still tracks packages against it */
	IMAGE_ERR_CREATE_FAILED,
	IMAGE_ERR_DELETE_FAILED
};

/* Call once at daemon startup, before serving any request -- just
 * remembers where images_dir is, same role network_init()'s own
 * state_path parameter plays for a very different (persisted)
 * resource. */
void image_init(const char *images_dir);

/*
 * Validates name (same charset/length rule pkg.c's own recipe/image
 * names already use), IMAGE_ERR_DUPLICATE if <name>/rootfs already
 * exists. Otherwise creates an empty rootfs directory and seeds its C
 * runtime immediately (pkg_seed_image_runtime(), ADR-0023) so it's
 * usable right away, before any pkg install ever targets it.
 */
enum image_error image_create(const char *name);

/*
 * IMAGE_ERR_NOT_FOUND if <name>/rootfs doesn't exist. IMAGE_ERR_PROTECTED
 * for "base" specifically. IMAGE_ERR_IN_USE if registry_image_in_use()
 * reports a running container still referencing it. IMAGE_ERR_HAS_PACKAGES
 * if pkg_image_has_packages() reports any package still tracked against
 * it -- remove those first, avoiding orphaned pkg.c state referencing a
 * deleted rootfs. Otherwise recursively removes the image's own
 * directory (a hand-rolled nftw()-based walk -- shelling out to `rm -rf`
 * would be inconsistent with this project's own "hand-rolled C for
 * daemon-owned operations" posture; pkg.c's build-tool subprocess use is
 * a distinct, already-justified exception for executing untrusted build
 * scripts, not a precedent for this).
 */
enum image_error image_delete(const char *name);

/* {"images": [...]} entries, each just {"name": "..."} -- deliberately
 * minimal, matching device.c's own "report what's discoverable, not a
 * derived summary" posture (a client wanting per-image package detail
 * already has it via GET /v1/pkg's own "image" field per entry). */
void image_write_json_list(struct json_writer *w);

/* IMAGE_ERR_NOT_FOUND if <name>/rootfs doesn't exist. */
enum image_error image_write_json_one(const char *name, struct json_writer *w);

#endif /* IMAGE_H */
