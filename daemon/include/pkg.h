#ifndef PKG_H
#define PKG_H

#include "container.h"
#include "json.h"

#include <sys/types.h>

/*
 * Phase 10 part 1: a source-based package manager. Our own tooling
 * (this module, the CLI, the REST layer) is hand-rolled C per the
 * mission's TCC mandate -- but each package's own build step is free
 * to use whatever toolchain it actually needs (gcc/make/autotools),
 * confirmed with the user: "package wrappers... written in C" is
 * about our platform code, not about forcing arbitrary upstream C
 * projects through TCC, the same "our code hand-rolled, real
 * software as workloads" split ADR-0007/DNS/PKI already established.
 *
 * Two more decisions, confirmed with the user, then verified against
 * real environment constraints before any code was written:
 *
 * - Builds run inside this project's own container runtime (reuse,
 *   not a second isolation mechanism) -- but the build container gets
 *   NO network access. This project's networking plane has no
 *   outbound NAT/masquerade (a whole new kernel subsystem, netfilter,
 *   never touched here), so fetching happens on the HOST instead
 *   (`curl`, real software, same precedent as `openssl`/`dig`) before
 *   the container ever starts -- a stronger isolation boundary for
 *   untrusted build scripts, and it needs no NAT at all.
 * - Installs are asynchronous. The daemon's reactor is single-
 *   threaded and non-blocking (ADR-0009); a network fetch or a real
 *   compile can take anywhere from seconds to minutes, and blocking a
 *   request handler for that long would freeze every other API
 *   request. Both the fetch subprocess and the build container are
 *   tracked via pidfd+epoll, the same non-blocking shape containers
 *   already get -- POST /v1/pkg/install returns immediately.
 *
 * Recipes are shell scripts (a proven format -- Gentoo ebuilds/Arch
 * PKGBUILDs/CRUX Pkgfiles all work this way) but the daemon NEVER
 * sources or executes one on the host: metadata (pkg_name=/pkg_version=/
 * pkg_source=/pkg_sha256=/pkg_depends=) is read with a strict,
 * non-executing line scanner. The recipe's real shell code
 * (pkg_build()/pkg_install() bodies) is only ever invoked inside the
 * isolated, network-less build container -- the only place running a
 * recipe's actual logic is safe.
 *
 * pkg_source=/pkg_sha256= are each a space-separated list of one or
 * more entries, positionally paired (ADR-0036) -- a real, verified
 * need, not speculative: lldap's own web frontend build fetches
 * several small external CDN assets (CSS/JS/fonts) alongside its main
 * source tarball, and the isolated build container has no network
 * access to fetch them itself. Every recipe before this one has
 * exactly one URL, which parses as a one-element list unchanged.
 * Index 0 is "the" source, extracted into /build/src exactly as
 * every recipe already assumes; index 1+ are plain files, copied
 * verbatim (never extracted) into /build/extra/<basename-of-its-own-
 * URL> for pkg_build()/pkg_install() to reference directly. Any one
 * entry's fetch failure or checksum mismatch fails the whole job --
 * no partial-success state, matching the single-source case's own
 * existing all-or-nothing guarantee.
 */

#define PKG_MAX_PACKAGES 256
#define PKG_NAME_MAX 64
/* Same charset/length rules as a package name (simple_name_is_valid()),
 * for the same reason CONTAINER/NETWORK names share one bound -- an
 * image name is not a distinct kind of identifier, just a directory
 * under images_dir. Not "base"-specific: any name a container's own
 * "image" field could reference is valid here too. */
#define PKG_IMAGE_NAME_MAX 64
#define PKG_DEFAULT_IMAGE "base"
#define PKG_VERSION_MAX 64
#define PKG_URL_MAX 512
#define PKG_SHA256_MAX 65
#define PKG_DEPENDS_MAX 256
#define PKG_ERROR_MAX 256
/* Max total packages in one resolved install chain (the target plus
 * every transitive dependency) -- a real cap, not an unbounded queue. */
#define PKG_MAX_DEP_CHAIN 32
/* Max pkg_source=/pkg_sha256= entries in one recipe -- lldap's own
 * real need is 9 (one main tarball + 8 CDN assets); generous headroom
 * past that, the same "bounded but roomy" precedent PKG_MAX_DEP_CHAIN
 * already sets, not an unbounded list. */
#define PKG_MAX_SOURCES 16

/* Reserved container name for the single in-flight build (v1
 * serializes installs -- at most one at a time). Shows up in the
 * normal GET /v1/containers listing too, deliberately -- one source
 * of truth for "what's running," not a second hidden tracking path. */
#define PKG_BUILD_CONTAINER_NAME "__pkgbuild"

/*
 * Reserved-by-convention sentinel "image" a hostbuild job's own
 * struct pkg_entry is filed under (pkg_find(name, PKG_HOSTBUILD_IMAGE))
 * -- the same soft-reservation precedent PKG_BUILD_CONTAINER_NAME
 * above already relies on for container names (simple_name_is_valid()
 * has no notion of "reserved," so nothing stops an operator naming a
 * real image "__hostbuild" too; this is a convention, not a structural
 * guarantee). Lets a hostbuild job reuse every existing pkg_entry/
 * state/error/fetch/build code path completely unmodified; the only
 * place that actually branches on it is the one lowerdir-selection
 * line in pkg_fetch_completed() and the one merge-vs-harvest line in
 * pkg_build_completed() (see pkg_hostbuild_start()'s own doc comment
 * below, ADR-0056).
 */
#define PKG_HOSTBUILD_IMAGE "__hostbuild"

enum pkg_state { PKG_STATE_FETCHING, PKG_STATE_BUILDING, PKG_STATE_INSTALLED, PKG_STATE_FAILED };

enum pkg_error {
	PKG_OK = 0,
	PKG_ERR_INVALID_NAME,
	PKG_ERR_NOT_FOUND,
	PKG_ERR_INVALID_RECIPE,
	PKG_ERR_DUPLICATE,
	PKG_ERR_BUSY, /* another install already in flight (v1: one at a time) */
	PKG_ERR_FULL,
	PKG_ERR_SPAWN_FAILED,
	PKG_ERR_PERSIST_FAILED,
	PKG_ERR_INVALID_TOOLCHAIN /* toolchain_path missing, unreadable, or not a regular file */
};

/*
 * pkg_dir is the root of pkg.c's own on-disk layout (recipes/,
 * sources/); installed_state_path is the persisted installed-package
 * registry (only PKG_STATE_INSTALLED entries persist -- an in-flight
 * job lost to a daemon restart is a stated v1 boundary, mirroring
 * containers themselves not surviving a restart). containers_dir and
 * images_dir let pkg.c independently derive the build container's own
 * paths and the two images it owns (images_dir/pkgbuild/rootfs, the
 * sandboxed toolchain; images_dir/base/rootfs, the one canonical
 * image every installed package's files land in -- the thing that
 * makes "the same 100% for host and containers" true: every container
 * built on image "base" gets everything installed, no separate path).
 * artifacts_dir is where a hostbuild job's own harvested output lands
 * (artifacts_dir/<name>/..., ADR-0056) -- a plain host directory, never
 * a container-visible path (a bare bzImage or kanxeod/kanxeoctl/web/
 * has no business inside a normal container image's rootfs).
 */
int pkg_init(const char *pkg_dir, const char *installed_state_path, const char *containers_dir,
              const char *images_dir, const char *artifacts_dir);

/*
 * Stages a real build toolchain (gcc/make/ld/as/cc1/sh/tar/coreutils
 * and their real headers/libraries) from THIS HOST into the pkgbuild
 * image, by copying whole /usr/{include,lib,lib64,bin,libexec}
 * directories with the real `cp -a` (correct symlink/permission
 * handling; a hand-rolled copier would mishandle the many symlinks a
 * real toolchain install is built from) plus recreating this host's
 * own bin/lib/lib64/sbin -> usr/... compatibility symlinks. Verified
 * empirically (chroot compile+link+`make install DESTDIR=`) before
 * this was written. Idempotent -- skips a subdirectory already
 * staged. Not automatic at daemon startup (startup stays fast); an
 * explicit, PKI-CA-bootstrap-shaped one-time action instead.
 */
enum pkg_error pkg_bootstrap_build_image(void);

/*
 * The correct production path (Phase 20): imports a real, portable
 * toolchain artifact -- built once, elsewhere, by
 * image/src/mktoolchainimage.c on a real toolchain-having machine, then
 * scp'd onto this box -- via a real `unsquashfs -f -d` extraction into
 * the pkgbuild rootfs (userspace, no mount/loop-device needed). Does
 * not depend on this daemon's own live host having anything under its
 * own /usr at all, unlike pkg_bootstrap_build_image()'s live-copy
 * fallback above -- this is what actually works on a real minimal
 * install. toolchain_path must already exist as a readable regular
 * file (PKG_ERR_INVALID_TOOLCHAIN otherwise); the operator is
 * responsible for getting it onto the box first (scp), the same
 * "local path, not an HTTP upload" precedent /system/update's own
 * image_path/kernel_path already established -- a real toolchain
 * artifact is easily hundreds of MB, far past what this daemon's own
 * hand-rolled HTTP server should ever stream.
 */
enum pkg_error pkg_bootstrap_from_toolchain(const char *toolchain_path);

/*
 * Test-only concrete-fact check (daemon/src/main.c's own
 * --test-bootstrap-toolchain= self-test): does the pkgbuild rootfs
 * genuinely have gcc after a bootstrap call, not just "did the call
 * return OK" -- g_pkgbuild_rootfs itself is private to pkg.c. Returns
 * 1 if present, 0 otherwise.
 */
int pkg_toolchain_has_gcc(void);

/*
 * Seeds image's own rootfs with the fixed baseline every container
 * needs but no image ever gets from pkg install alone (ADR-0019,
 * ADR-0023, ADR-0041): the C runtime a dynamically-linked package
 * needs to actually execve() (ld.so, libc.so.6, libtinfo.so.6, ...),
 * from the same fixed host paths pkg_bootstrap_build_image() and this
 * project's own test/install-image tooling already use; standard char
 * device nodes (/dev/{null,zero,full,random,urandom}, same table/
 * pattern test_image_fixture_stage_toolchain() already proves safe);
 * and a plain, empty /run directory. image NULL or "" means
 * PKG_DEFAULT_IMAGE, same convention as every other image parameter in
 * this header. The runtime-lib half is idempotent (skips a file
 * already staged) and tolerant of a missing source file on this
 * particular host (skip, not fatal -- same precedent
 * pkg_bootstrap_build_image() already sets); the dev-node/run half has
 * no such "host source might be missing" excuse, so any real mkdir/
 * mknod failure there is fatal. Either half failing returns
 * PKG_ERR_PERSIST_FAILED. Called automatically by pkg_build_completed()
 * for whatever image a job is merging into (base included, harmlessly
 * redundant there since ADR-0019's install-time seeding already covers
 * the runtime-lib half for base) -- exposed publicly too so an
 * explicit image-creation call can seed a freshly created, still-empty
 * image immediately. This is a fixed, hardcoded baseline set, not an
 * extensible "package declares what it needs" mechanism (ADR-0041).
 */
enum pkg_error pkg_seed_image_baseline(const char *image);

/* Scans pkg_dir/recipes/*.recipe and writes {name,version,depends}
 * for each one that parses -- metadata only, never sourced/executed. */
void pkg_write_json_recipes(struct json_writer *w);

/*
 * One recipe's own {name,version,depends,content} -- the raw shell
 * script text too, unlike pkg_write_json_recipes()'s list-view
 * metadata-only shape, for the web dashboard's per-package Recipe tab
 * (view, and edit-then-resubmit through the existing pkg_recipe_add()
 * upsert -- editing a recipe is exactly "add with the same name," no
 * separate update path needed). Still never sourced/executed here,
 * same as every other recipe read in this module -- content is only
 * ever actually run inside the isolated build container.
 * PKG_ERR_INVALID_NAME / PKG_ERR_NOT_FOUND (missing or fails to parse).
 */
enum pkg_error pkg_recipe_get(const char *name, struct json_writer *w);

/*
 * Adds a new recipe, or replaces an existing one with the same name
 * (upsert -- the whole point is updating a recipe catalog without a
 * full OS reinstall, ADR-0040). content is written to a staging file
 * under pkg_dir/recipes/ first and validated with the same
 * parse_recipe() every install-time lookup already uses (name must be
 * a valid package name, AND must match content's own pkg_name= field
 * -- the same "filename and pkg_name= agree" invariant
 * resolve_chain()'s own dependency lookups already rely on); only on
 * success is it atomically renamed over name.recipe, so an invalid
 * upload can never clobber a working recipe already there.
 * PKG_ERR_INVALID_NAME / PKG_ERR_INVALID_RECIPE / PKG_ERR_PERSIST_FAILED
 * on failure.
 */
enum pkg_error pkg_recipe_add(const char *name, const char *content);

/*
 * Removes name.recipe. Does not touch anything already installed via
 * that recipe (a build's output is merged into an image at install
 * time -- nothing about a package that's already installed depends on
 * its own recipe file continuing to exist); only affects future
 * `pkg install`/update-all lookups for that name.
 * PKG_ERR_INVALID_NAME / PKG_ERR_NOT_FOUND / PKG_ERR_PERSIST_FAILED.
 */
enum pkg_error pkg_recipe_delete(const char *name);

/*
 * Validates name + its recipe, refuses if any install is already in
 * flight (PKG_ERR_BUSY). If name is already PKG_STATE_INSTALLED, this
 * is PKG_ERR_DUPLICATE unless upgrade is true AND the recipe's current
 * version differs from what's installed (still PKG_ERR_DUPLICATE if
 * upgrade is true but the version already matches -- nothing to do).
 * Otherwise resolves the full dependency chain (pkg_depends, recursive,
 * cycle-checked, already-installed dependencies skipped) via local
 * recipe files only -- no async needed for this part, it's all
 * on-disk. PKG_ERR_INVALID_RECIPE covers a missing/unparseable recipe
 * for name OR any dependency it names, including a circular one.
 * Forks+execve's `curl` to fetch the first package in the resolved
 * chain's source on the host (no container yet -- nothing to isolate
 * for a plain network fetch of a URL the operator's own recipe
 * named) -- which may be a dependency, not name itself, if name
 * needed something installed first. *out_started_name (a caller-
 * owned buffer of size out_started_name_size) is filled with whatever
 * that first package actually is, so the caller can build an accurate
 * response describing what's really happening right now rather than
 * assuming it's always name. On success, *out_pid/*out_pidfd are the
 * running curl subprocess, ready for the caller (main.c, which owns
 * epoll) to track via EPOLLIN on *out_pidfd exactly like a
 * container's own pidfd.
 *
 * image is the target every package in the resolved chain (name and
 * every dependency it pulls in) merges into once built -- NULL or ""
 * means PKG_DEFAULT_IMAGE ("base"). A package name is tracked
 * independently per image: installing "bash" into both "base" and
 * "router" are two separate, independently-upgradable/removable
 * entries, not a collision (a container using one image never sees
 * what another image has installed -- the whole point of having more
 * than one).
 */
enum pkg_error pkg_install_start(const char *name, const char *image, int upgrade,
                                  char *out_started_name, size_t out_started_name_size,
                                  pid_t *out_pid, int *out_pidfd);

/*
 * A second mode of the same fetch/build pipeline pkg_install_start()
 * drives, for building a standalone HOST artifact (a kernel bzImage, a
 * fresh kanxeod/kanxeoctl/web/ control-plane) rather than installing
 * into a container image's rootfs (ADR-0056). Reuses fetch/verify/
 * stage/build completely unmodified -- the build container gets the
 * exact same offline, no-network isolation every ordinary install
 * already gets. Two real differences from pkg_install_start():
 *
 *   - The build container's own lowerdir is build_image's rootfs
 *     (an ordinary image, built up via completely normal `pkg install
 *     --image=<build_image>` calls beforehand -- e.g. installing
 *     "tcc"/"make" into a "kanxeo-builder" image), never the shared
 *     g_pkgbuild_rootfs toolchain sandbox every ordinary install uses.
 *     build_image must already exist (PKG_ERR_NOT_FOUND if its rootfs
 *     doesn't).
 *   - On success the build's $PKG_DESTDIR contents are copied
 *     (recursively, verbatim, no manifest) to a plain host directory
 *     under BASE_DIR/artifacts/<name>/ instead of being merged into
 *     any image -- readable back via GET /v1/pkg/hostbuild/<name>'s
 *     own artifact_path field, then handed to /system/update as an
 *     ordinary local path (no new deploy mechanism -- that endpoint
 *     already accepts any local image_path/kernel_path).
 *
 * name's own recipe must have an EMPTY pkg_depends -- dependency
 * resolution targets "merge into an image," a concept with no meaning
 * for a one-shot artifact harvest; every prerequisite the build needs
 * must already be baked into build_image's own rootfs, which is
 * exactly why that image gets built up via the ordinary install path
 * first (PKG_ERR_INVALID_RECIPE if pkg_depends is non-empty). Same
 * PKG_ERR_BUSY serialization as every other install -- a hostbuild
 * job occupies the one v1 in-flight slot exactly like an ordinary one.
 * PKG_ERR_DUPLICATE if name is already a hostbuild entry in
 * PKG_STATE_INSTALLED (re-run with a bumped pkg_version= to rebuild;
 * there is no separate "upgrade" flag here, a hostbuild has no
 * container depending on its own continued installed-ness the way a
 * package does).
 */
enum pkg_error pkg_hostbuild_start(const char *name, const char *build_image, pid_t *out_pid,
                                    int *out_pidfd);

/*
 * Called once the tracked fetch subprocess's pidfd fires (caller has
 * already waitpid()'d it and passes the raw exit status). On a clean
 * exit, verifies the download against the recipe's pkg_sha256 (via
 * the real `sha256sum`, never hand-rolled crypto), extracts it, and
 * fills *spec_out (pointers into pkg.c's own static storage, valid
 * until the next pkg_* call -- safe given v1's one-job-at-a-time
 * serialization) ready for the caller to registry_create() the build
 * container itself. Returns 1 if the caller should do that, 0 if the
 * job already ended (FAILED -- bad exit status or checksum mismatch)
 * and there is nothing to build.
 */
int pkg_fetch_completed(int exit_status, struct container_spec *spec_out);

/* Called if registry_create() itself fails for the build container
 * pkg_fetch_completed() just prepared -- transitions the in-flight
 * job to FAILED (there is no container to wait for in this case). */
void pkg_build_spawn_failed(void);

/*
 * Called from main.c's container-exit path unconditionally, after
 * registry_mark_exited() -- no-ops (returns 0) unless container_name
 * is PKG_BUILD_CONTAINER_NAME, mirroring exactly how
 * dns_record_forget_owner()/pki_cert_forget_owner() are already
 * called unconditionally on every container delete. On a clean exit,
 * merges <build container upperdir>/build/pkg-dest/ into the base
 * image (plain host file I/O -- the container has already exited, so
 * there is no running mount namespace left to reach into via
 * /proc/<pid>/root/, unlike DNS/PKI's live-container writes); for an
 * upgrade of an already-installed package, the OLD manifest's files
 * are unlinked first, so a version that renamed/dropped files doesn't
 * leave the old ones behind. Records every copied file's path into
 * the package's manifest.
 *
 * If this completion advances the dependency chain (there's a next
 * queued package to install), forks+execve's `curl` for it exactly
 * like pkg_install_start() does for the first one, and returns 1 with
 * *out_pid/*out_pidfd filled in -- the caller must
 * register_pkg_fetch_pidfd() them, the same reaction it already has
 * to pkg_fetch_completed() returning 1 to start a build. Returns 0 if
 * there's nothing more to chain (queue exhausted, this container
 * wasn't the tracked job, or the build failed).
 *
 * out_hostbuild_done_name (a caller-owned buffer of at least
 * PKG_NAME_MAX bytes) is written with the completed package's own
 * name if -- and only if -- this specific completion was a hostbuild
 * job (ADR-0056) that just reached PKG_STATE_INSTALLED; left as an
 * empty string otherwise (ordinary install, mid-chain, or a failure).
 * This is pkg.c's only hostbuild-completion signal to the rest of the
 * daemon -- deliberately just a name, not a dispatch decision: pkg.c
 * itself stays completely agnostic to what any particular package
 * *means* (ADR-0057's own ARTIFACTS_DIR/<name>/kanxeod-root.squashfs
 * assembly for name=="kanxeo" specifically is main.c's business, not
 * this module's).
 */
int pkg_build_completed(const char *container_name, int exit_status, pid_t *out_pid, int *out_pidfd,
                         char *out_hostbuild_done_name);

/* Metadata for every known package (installed or in-flight): name,
 * image, version, state, error (null unless FAILED), files (manifest,
 * empty until INSTALLED), available_version (null if up to date or
 * not yet installed, else the recipe's current version -- re-read
 * from disk on every call, One Source of Truth). */
void pkg_write_json_list(struct json_writer *w);

/*
 * Finds the first PKG_STATE_INSTALLED entry whose recipe's own current
 * pkg_version= differs from what's installed -- the exact same
 * available_version drift check pkg_write_json_list()'s own per-entry
 * write_pkg_json() already performs, extracted here so both share one
 * comparison. Returns 1 with out_name/out_image filled in if one was
 * found, 0 if every installed package is already up to date. Does NOT
 * itself start an upgrade -- the caller (handle_pkg_update_all())
 * passes the result straight to pkg_install_start(..., upgrade=1, ...),
 * the same v1 single-job-in-flight constraint every other install path
 * already has (PKG_ERR_BUSY if one is already running).
 */
int pkg_find_update_candidate(char *out_name, size_t out_name_size, char *out_image,
                               size_t out_image_size);
/* image NULL or "" means PKG_DEFAULT_IMAGE, matching pkg_install_start(). */
enum pkg_error pkg_get_one(const char *name, const char *image, struct json_writer *w);

/* Unlinks every manifested file from that specific (name, image)
 * entry's own image and forgets the package. PKG_ERR_NOT_FOUND if
 * unknown or not currently installed; PKG_ERR_BUSY if this exact
 * (name, image) is the one currently mid-build. image NULL or ""
 * means PKG_DEFAULT_IMAGE. */
enum pkg_error pkg_delete(const char *name, const char *image);

/*
 * True if any package (installed or in-flight) is currently tracked
 * against image -- used by image_delete() (daemon/src/image.c) to
 * refuse removing an image with packages still tracked against it,
 * avoiding orphaned pkg.c state referencing a deleted rootfs. image
 * NULL or "" means PKG_DEFAULT_IMAGE, matching every other image
 * parameter in this header.
 */
int pkg_image_has_packages(const char *image);

#endif /* PKG_H */
