#ifndef IMAGE_H
#define IMAGE_H

#include "json.h"
#include "pkg.h"

/*
 * ADR-0107/ADR-0108: an image is a name with a real, persisted state
 * document (IMAGES_DIR/<name>/manifest.json) and a set of immutable,
 * content-addressed rootfs trees (IMAGES_DIR/<name>/<version>/rootfs,
 * version = sha256 of the resolved "package@version,..." manifest
 * string). manifest.json holds three things, deliberately kept in one
 * document rather than split across files (One Source of Truth):
 *   - the operator's own declared package intent ({package, mode,
 *     version} entries) -- what SHOULD be here, independent of build
 *     history;
 *   - current_version -- which already-built version is "the" one new
 *     containers resolve to;
 *   - versions[] -- every version this image has ever produced, each
 *     with a real created_at timestamp (ADR-0108; explicit, not
 *     filesystem-mtime-derived, since the copy-forward rebuild
 *     mechanism touches directory metadata in ways that would make
 *     mtime an unreliable proxy here).
 * Read/written fresh from disk on every call, not loaded into a
 * startup-resident table -- this module's own long-standing
 * "re-enumerated fresh" posture, not ADR-0012's competing
 * startup-loaded pattern.
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
	IMAGE_ERR_NO_CURRENT_VERSION, /* image exists but has never had a version built (shouldn't
	                                * happen for anything created via image_create(), which
	                                * always produces an initial one -- surfaced as a real
	                                * error rather than silently treated as "no rootfs") */
	IMAGE_ERR_PERSIST_FAILED
};

/* A package name may have more entries than an image's own manifest
 * could ever need in practice (one entry per distinct package the
 * image declares intent for) -- matches PKG_MAX_DEP_CHAIN's own order
 * of magnitude rather than pkg.c's whole-daemon PKG_MAX_PACKAGES. */
#define IMAGE_MANIFEST_MAX_PACKAGES 128

/* sha256 hex digest (64 chars) + NUL. */
#define IMAGE_VERSION_MAX 65

/* The bound on the recorded history array. ADR-0107's v1 scope pruned
 * nothing, so a rolling image outgrew it: on 192.168.15.95, 2026-10-20,
 * jumpbox's history held 256 versions while newer ones sat on disk
 * unrecorded. ADR-0343 bounds each image to its referenced versions
 * plus its policy's `keep` (at most IMAGE_POLICY_KEEP_MAX), far below
 * this, and a retention pass repairs the history from the disk. A full
 * array still stops growing rather than wrapping. */
#define IMAGE_MAX_VERSION_HISTORY 256

enum image_pkg_mode {
	IMAGE_PKG_PINNED,  /* version is the exact recipe version to install, never auto-advances */
	IMAGE_PKG_ROLLING, /* version is a floor -- resolves to the highest available >= it */
};

struct image_manifest_entry {
	char package[PKG_NAME_MAX];
	enum image_pkg_mode mode;
	char version[PKG_VERSION_MAX];
};

struct image_version_entry {
	char version[IMAGE_VERSION_MAX];
	long created_at; /* Unix epoch seconds, real time(NULL) at creation -- ADR-0108 */
};

/* Call once at daemon startup, before serving any request -- just
 * remembers where images_dir is, same role network_init()'s own
 * state_path parameter plays for a very different (persisted)
 * resource. */
void image_init(const char *images_dir);

/*
 * Validates name (same charset/length rule pkg.c's own recipe/image
 * names already use), IMAGE_ERR_DUPLICATE if manifest.json already
 * exists (the authoritative "this image exists" signal -- ADR-0107/8).
 * Otherwise builds and commits an initial version from the empty
 * package set (sha256 of the empty string) -- and an EMPTY rootfs.
 *
 * Nothing is seeded into it (ADR-0333). This used to call
 * pkg_seed_image_baseline() and describe the result as "immediately
 * usable before any pkg install ever targets it", which stopped being
 * true when ADR-0216 closed the glibc floor: a fresh image has no C
 * library and POST /v1/containers refuses it by name. ADR-0333 removed
 * the rest for the same reason -- an image's content comes only from
 * its packages, and everything the baseline wrote was per-container
 * state that is now created per container.
 */
enum image_error image_create(const char *name);

/*
 * 1 if name is a valid image name with a manifest.json, else 0 -- the
 * one test of "this image exists" the listing, image_write_json_one()
 * and POST /v1/pkg/install's refusal of an unknown image (#500) share.
 */
int image_exists(const char *name);

/* #551: deletes stored trees set aside as rootfs.replaced-* when a reused
 * version was found wrong. Startup only, before any container starts.
 * Returns how many it removed. */
int image_sweep_replaced_trees(void);

/*
 * IMAGE_ERR_NOT_FOUND if name has no manifest.json. IMAGE_ERR_PROTECTED
 * for "base" specifically. IMAGE_ERR_IN_USE if registry_image_in_use()
 * reports a running container still referencing it. IMAGE_ERR_HAS_PACKAGES
 * if pkg_image_has_packages() reports any package still tracked against
 * it -- remove those first, avoiding orphaned pkg.c state referencing a
 * deleted rootfs. Otherwise every tree in it -- each version's rootfs,
 * and any staging leftover -- is set aside through the installed discard
 * function (ADR-0343), and what is left, files only, is removed with the
 * directory. The trees' bytes are freed by a helper child afterwards, so
 * the name is free at once and the space comes back later.
 */
enum image_error image_delete(const char *name);

/*
 * Renames an image directory, and with it every package row recorded
 * against that image (issue #124 -- a rename that left the rows behind
 * would manufacture exactly the orphans #111 was about).
 *
 * Refuses if new_name is invalid or already exists, if old_name does
 * not exist, if either is the protected default image, or if any
 * container is currently using old_name -- a running container's
 * overlay lowerdir points into this directory by path, so renaming it
 * out from under one is not survivable.
 */
enum image_error image_rename(const char *old_name, const char *new_name);

/*
 * The version every image is born with: the hash of its own empty
 * manifest, written by image_create() before anything has been put in
 * it. Callers that need to tell "this image exists" apart from "this
 * image has been filled" compare against this rather than against a
 * hardcoded digest -- the two questions are genuinely different, and
 * conflating them is how a failed build-environment composition once
 * left behind an empty image that every later build accepted as ready
 * (issue #109).
 */
int image_empty_manifest_version(char *out, size_t out_size);

/* Upper bound for image_list_names() -- a real deployment having more
 * than this many distinct images at once is a future problem, the same
 * pragmatic bound every other daemon-owned array in this codebase
 * already uses (PKG_MAX_PACKAGES, REGISTRY_MAX_CONTAINERS, ...). */
#define IMAGE_LIST_MAX 256

/* Every existing image's name (manifest.json presence is the
 * authoritative "exists" signal, same as everywhere else in this
 * module), up to max entries. Returns the count actually written.
 * Shared by image_write_json_list() below and pkg.c's own rolling-
 * rebuild trigger (which needs to enumerate every image's manifest,
 * not just report names over REST) -- one real directory-scan
 * implementation, not two. */
/*
 * ADR-0343: what moves a tree out of an image's way. `path` is a
 * directory inside IMAGES_DIR -- a version's rootfs, or another tree in
 * a version or image directory -- and the function renames it aside and
 * queues its delete off the event loop. 0 when nothing is left at path.
 *
 * main.c installs rootfs_discard() aimed at IMAGES_DIR/.discard, so an
 * image's bytes leave through ADR-0341's queue and its helper children.
 * With none installed nothing can be set aside, and a collection or an
 * image delete fails rather than deleting on the caller's thread.
 */
typedef int (*image_discard_fn)(const char *image, const char *path);
void image_set_discard(image_discard_fn fn);

/* ADR-0343: "does anything other than current_version reference this
 * version" -- the caller's one rule, asked once per version a pass looks at. */
typedef int (*image_referenced_fn)(const char *image, const char *version, void *ctx);

struct image_retain_entry {
	struct image_version_entry v;
	int drop; /* 1: the pass collects it */
};

/*
 * ADR-0343: one image's history, reconciled with its disk, and what a
 * pass would collect from it. entries[] is oldest first and holds every
 * version on disk -- uncapped, so an image whose recorded history
 * stopped at IMAGE_MAX_VERSION_HISTORY is seen whole.
 */
struct image_retain_plan {
	char image[PKG_IMAGE_NAME_MAX];
	struct image_retain_entry *entries;
	int count;
	int kept;      /* entries not dropped */
	int dropped;   /* entries dropped */
	int repaired;  /* 1 when the history and the disk disagreed */
};

/*
 * Reads name's history and its version directories, and decides.
 *
 * The recorded history is the one source of a version's age. A version
 * on disk that the history does not name is added, dated by its
 * directory's statx birth time (its mtime where the filesystem has no
 * birth time), and a history entry with no directory is dropped from
 * the plan. Then, newest first: current_version and every version
 * `referenced` names are kept, the next `keep` versions are kept, and
 * the rest are marked drop.
 *
 * Changes nothing on disk. IMAGE_ERR_NOT_FOUND if name is not an image.
 * Free the plan with image_retain_plan_free().
 */
enum image_error image_retain_plan(const char *name, int keep, image_referenced_fn referenced,
                                   void *ctx, struct image_retain_plan *out);
void image_retain_plan_free(struct image_retain_plan *plan);

/*
 * Moves one non-current version out of the image: every directory
 * inside its version directory (its rootfs, and any rootfs.replaced-*)
 * goes to the installed discard function, the files left are removed,
 * then the directory. Never the version directory as a whole: on btrfs
 * SNAP_DESTROY fails on a plain directory, and the fallback would be a
 * file-by-file rmtree on the caller's thread.
 *
 * Leaves the recorded history alone -- image_retain_commit() writes it
 * once per image. IMAGE_ERR_PROTECTED for current_version,
 * IMAGE_ERR_NOT_FOUND if the version directory does not exist,
 * IMAGE_ERR_DELETE_FAILED if anything could not be moved or removed.
 */
enum image_error image_version_set_aside(const char *name, const char *version);

/*
 * Writes the plan's history: every entry not dropped, plus every dropped
 * one whose directory is somehow still there, in the plan's order. At
 * most IMAGE_MAX_VERSION_HISTORY, newest kept. One save, and none when
 * nothing changed.
 */
enum image_error image_retain_commit(const struct image_retain_plan *plan);

/*
 * ADR-0343: images that got a version since the last take. Marked by
 * image_record_version(); taken by the automatic pass once no package
 * job is in flight. Returns how many were written to names, and clears
 * the marks.
 */
int image_retain_take_marked(char names[][PKG_IMAGE_NAME_MAX], int max);

int image_list_names(char names[][PKG_IMAGE_NAME_MAX], int max);

/* {"images": [...]} entries, each just {"name": "..."} -- deliberately
 * minimal, matching device.c's own "report what's discoverable, not a
 * derived summary" posture (a client wanting per-image package detail
 * already has it via GET /v1/pkg's own "image" field per entry). */
void image_write_json_list(struct json_writer *w);

/* IMAGE_ERR_NOT_FOUND if name has no manifest.json. */
enum image_error image_write_json_one(const char *name, struct json_writer *w);

/*
 * ADR-0107: an image's manifest is the operator's declared intent --
 * which packages, at which mode/version, should exist in this image --
 * distinct from image_write_json_one()'s own report of what the image
 * currently *is*. A freshly image_create()'d image has a real, valid
 * empty manifest (zero entries) -- nothing declared yet, not an error.
 * IMAGE_ERR_NOT_FOUND if name doesn't exist.
 */
enum image_error image_manifest_read(const char *name, struct image_manifest_entry *out,
                                      int *out_count, int max_entries);

/* Writes {"manifest": [{"package":..., "mode":"pinned"|"rolling",
 * "version":...}, ...]}. IMAGE_ERR_NOT_FOUND if name doesn't exist. */
enum image_error image_manifest_write_json(const char *name, struct json_writer *w);

/*
 * #398: what a specific image VERSION holds, snapshotted beside that
 * version's rootfs when it was produced.
 *
 * NOT the declared manifest above, which is a different document. That
 * one is live operator intent and moves whenever an operator changes
 * what the image should contain; worse, a `rolling` entry's version
 * there is a FLOOR rather than a fact, so it cannot say which version
 * of a package anything actually holds. What is recorded instead is
 * the INSTALLED SET -- the same sorted name@version pairs
 * image_produce_new_version() hashes to derive the version (ADR-0108)
 * -- so the snapshot is a copy of the version's own identity.
 *
 * A container records the version it runs from and keeps running it,
 * which is why this exists at all: "what is in this container" cannot
 * be answered from anything that moves.
 *
 * pkg_version_snapshot_write() records it (in pkg.c, next to the
 * installed set it reads); this returns it as a JSON array of
 * {package, version}. There is no mode: mode is a property of the
 * declaration, not of a version. A version produced before snapshots
 * existed returns IMAGE_ERR_NOT_FOUND, deliberately -- falling back to
 * the live manifest would hand back a wrong answer with nothing
 * marking it as wrong.
 */
void image_version_manifest_path(const char *name, const char *version, char *out, size_t out_size);
enum image_error image_manifest_version_write_json(const char *name, const char *version,
                                                    struct json_writer *w);

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

/*
 * name's own currently-current version (the hash new containers
 * resolve to at creation time) -- IMAGE_ERR_NOT_FOUND if name doesn't
 * exist, IMAGE_ERR_NO_CURRENT_VERSION if it exists but has (somehow)
 * never had a version recorded (not reachable via image_create(),
 * which always produces one; surfaced rather than assumed impossible).
 */
enum image_error image_current_version(const char *name, char *out, size_t out_size);

/* Pure path construction, no I/O, always succeeds --
 * IMAGES_DIR/<name>/<version>/rootfs. */
void image_version_rootfs_path(const char *name, const char *version, char *out, size_t out_size);

/* Writes {"versions": [{"version":..., "created_at":...}, ...]},
 * newest first. IMAGE_ERR_NOT_FOUND if name doesn't exist. */
enum image_error image_version_history_write_json(const char *name, struct json_writer *w);

/*
 * Records version as name's new current_version (ADR-0108) -- called
 * once a caller (pkg.c) has already built a real, complete rootfs tree
 * on disk at image_version_rootfs_path(name, version). If version is
 * already present in history (a rebuild that reproduced already-seen
 * content, or an install/delete cycle that lands back on an earlier
 * state), only current_version is repointed -- no duplicate history
 * entry, no duplicate created_at. IMAGE_ERR_NOT_FOUND if name doesn't
 * exist, IMAGE_ERR_PERSIST_FAILED on a write failure.
 */
enum image_error image_record_version(const char *name, const char *version);

/*
 * Hashes s (a canonical, caller-constructed "package@version,..."
 * manifest string, sorted by package name -- ADR-0108) via
 * pkg_run_capture_sha256() -- the linked libcrypto's own EVP_sha256()
 * since #352, never a hand-rolled digest -- the one shared primitive
 * both image_create()'s own
 * initial empty-manifest version and pkg.c's own post-install
 * version-recording use, so there is exactly one hashing code path
 * for "what identifies an image version." Returns 0 on success.
 */
int image_hash_manifest_string(const char *s, char *out_hash, size_t out_hash_size);

#endif /* IMAGE_H */
