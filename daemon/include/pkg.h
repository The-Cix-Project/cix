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

/* Reserved container name for the single in-flight build (v1
 * serializes installs -- at most one at a time). Shows up in the
 * normal GET /v1/containers listing too, deliberately -- one source
 * of truth for "what's running," not a second hidden tracking path. */
#define PKG_BUILD_CONTAINER_NAME "__pkgbuild"

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
	PKG_ERR_PERSIST_FAILED
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
 */
int pkg_init(const char *pkg_dir, const char *installed_state_path, const char *containers_dir,
              const char *images_dir);

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

/* Scans pkg_dir/recipes/*.recipe and writes {name,version,depends}
 * for each one that parses -- metadata only, never sourced/executed. */
void pkg_write_json_recipes(struct json_writer *w);

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
 */
int pkg_build_completed(const char *container_name, int exit_status, pid_t *out_pid, int *out_pidfd);

/* Metadata for every known package (installed or in-flight): name,
 * image, version, state, error (null unless FAILED), files (manifest,
 * empty until INSTALLED), available_version (null if up to date or
 * not yet installed, else the recipe's current version -- re-read
 * from disk on every call, One Source of Truth). */
void pkg_write_json_list(struct json_writer *w);
/* image NULL or "" means PKG_DEFAULT_IMAGE, matching pkg_install_start(). */
enum pkg_error pkg_get_one(const char *name, const char *image, struct json_writer *w);

/* Unlinks every manifested file from that specific (name, image)
 * entry's own image and forgets the package. PKG_ERR_NOT_FOUND if
 * unknown or not currently installed; PKG_ERR_BUSY if this exact
 * (name, image) is the one currently mid-build. image NULL or ""
 * means PKG_DEFAULT_IMAGE. */
enum pkg_error pkg_delete(const char *name, const char *image);

#endif /* PKG_H */
