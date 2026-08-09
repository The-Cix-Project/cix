#ifndef IMAGE_H
#define IMAGE_H

#include "json.h"
#include "pkg.h"

/*
 * Images were pure filesystem state -- a directory under images_dir
 * containing its own rootfs/ subdirectory, the exact same convention
 * pkg.c's own image_rootfs_path() already established for `pkg
 * install`'s target images, re-enumerated fresh on every list/get call,
 * same "host state, not daemon-owned state" posture device.c already
 * has for hardware. ADR-0107 changes this: an image can now also carry
 * a real, operator-declared *manifest* -- a persisted, genuinely
 * daemon-owned {package, mode, version} list expressing package intent,
 * distinct from whatever happens to be built right now. Unlike the
 * image directory itself, the manifest is read/written fresh from its
 * own file on every call (IMAGES_DIR/<name>/manifest.json), not loaded
 * into a startup-resident table -- consistent with this module's own
 * existing "re-enumerated fresh" posture rather than introducing a
 * second, competing persistence convention (ADR-0012's startup-loaded
 * atomic-JSON pattern) for what is still fundamentally per-image state.
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
	IMAGE_ERR_DELETE_FAILED,
	IMAGE_ERR_INVALID_PACKAGE,  /* not a valid pkg_name_is_valid() name */
	IMAGE_ERR_INVALID_VERSION,  /* missing, or too long for PKG_VERSION_MAX */
	IMAGE_ERR_MANIFEST_FULL,    /* IMAGE_MANIFEST_MAX_PACKAGES reached, package not already present */
	IMAGE_ERR_PERSIST_FAILED
};

/* A package name may have more entries than an image's own manifest
 * could ever need in practice (one entry per distinct package the
 * image declares intent for) -- matches PKG_MAX_DEP_CHAIN's own order
 * of magnitude rather than pkg.c's whole-daemon PKG_MAX_PACKAGES. */
#define IMAGE_MANIFEST_MAX_PACKAGES 128

enum image_pkg_mode {
	IMAGE_PKG_PINNED,  /* version is the exact recipe version to install, never auto-advances */
	IMAGE_PKG_ROLLING, /* version is a floor -- resolves to the highest available >= it */
};

struct image_manifest_entry {
	char package[PKG_NAME_MAX];
	enum image_pkg_mode mode;
	char version[PKG_VERSION_MAX];
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
 * runtime and baseline FHS layout immediately (pkg_seed_image_baseline(),
 * ADR-0023, ADR-0041) so it's usable right away, before any pkg install
 * ever targets it.
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

/*
 * ADR-0107: an image's manifest is the operator's declared intent --
 * which packages, at which mode/version, should exist in this image --
 * distinct from image_write_json_one()'s own report of what the image
 * currently *is*. Absent manifest.json is a real, valid empty manifest
 * (IMAGE_OK, zero entries), not an error: an image built purely via
 * ad-hoc `pkg install --image=` calls has never had one written.
 * IMAGE_ERR_NOT_FOUND if name's own rootfs doesn't exist.
 */
enum image_error image_manifest_read(const char *name, struct image_manifest_entry *out,
                                      int *out_count, int max_entries);

/* Writes {"manifest": [{"package":..., "mode":"pinned"|"rolling",
 * "version":...}, ...]} -- empty array if name has no manifest.json.
 * IMAGE_ERR_NOT_FOUND if name's own rootfs doesn't exist. */
enum image_error image_manifest_write_json(const char *name, struct json_writer *w);

/*
 * Upserts one entry into name's manifest: package already present ->
 * its mode/version updated in place; otherwise appended.
 * IMAGE_ERR_INVALID_PACKAGE / IMAGE_ERR_INVALID_VERSION on bad input,
 * IMAGE_ERR_MANIFEST_FULL if package is new and the manifest is
 * already at IMAGE_MANIFEST_MAX_PACKAGES, IMAGE_ERR_NOT_FOUND if name
 * itself doesn't exist, IMAGE_ERR_PERSIST_FAILED on a write failure.
 */
enum image_error image_manifest_set(const char *name, const char *package,
                                     enum image_pkg_mode mode, const char *version);

/* Removes package's entry from name's manifest, if present -- a no-op,
 * not an error, if it's already absent. IMAGE_ERR_NOT_FOUND if name
 * itself doesn't exist. */
enum image_error image_manifest_unset(const char *name, const char *package);

#endif /* IMAGE_H */
