#ifndef PKG_H
#define PKG_H

#include "container.h"
#include "json.h"
#include "pipeline.h"

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
 * access to fetch them itself.
 *
 * Full recipe format (every field, the multi-source mechanics above,
 * the pkg_build()/pkg_install() environment contract, and a complete
 * worked example) is documented once, for humans, at
 * docs/guides/writing-recipes.md -- not repeated here.
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

/*
 * The CA trust bundle pkg_seed_image_baseline() stages into every image
 * (ADR-0051), as the image's own filesystem sees it. Named here rather
 * than spelled twice because a second consumer arrived: the nslcd.conf
 * the daemon renders for an ldap_client container points at this exact
 * file for tls_cacertfile (#414), and a path that is written in one
 * place and read in another is the kind of pair that drifts silently --
 * the reader simply finds no file and fails closed.
 */
#define PKG_IMAGE_CA_BUNDLE_PATH "/etc/ssl/certs/cix-ca-bundle.pem"

/*
 * The C library every composed build environment gets, whether or not a
 * recipe names it (#186).
 *
 * Every binary in every package links against libc, and nothing in an
 * environment can exec at all without the loader -- so this is a
 * property of an environment being usable, not of any one recipe. Named
 * once here rather than repeated across sixty recipes, where sixty
 * chances to omit it would each fail only at exec time with a bare
 * ENOENT that names nothing.
 *
 * A recipe may still name it explicitly to pin a version
 * ("glibc@2.44-6"); declared tools are resolved first and
 * buildenv_add_tool() dedups by name, so an explicit pin always wins.
 *
 * Until #186 these files were COPIED into every image off whatever
 * distribution the build host ran (ADR-0209's "glibc floor") -- the
 * last bytes in the system this project did not build.
 */
#define PKG_BASE_LIBC "glibc"

/*
 * The one file that proves an image can run anything: the dynamic
 * loader every binary in it needs before its first instruction.
 *
 * Named once because three separate places ask the same question and
 * must not drift on the answer -- whether a composed build environment
 * still carries the C library it was given, whether a container can be
 * created from an image at all, and whether the default image has been
 * given a runtime yet. Deliberately a path and not a package name: the
 * requirement is a working runtime, and which package provides it is
 * the catalogue's business, not the daemon's.
 */
#define PKG_IMAGE_LOADER_REL "lib64/ld-linux-x86-64.so.2"
#define PKG_VERSION_MAX 64
#define PKG_URL_MAX 512
#define PKG_SHA256_MAX 65
/*
 * Stall guards every fetch this daemon performs carries (#285, #410).
 *
 * A connection can be accepted and then never answered, so a peer that
 * accepts the TCP connection and then says nothing hangs the fetch
 * indefinitely. That is not hypothetical here: ftp.gnu.org did exactly
 * it to this site's egress for several days -- connected on both 443
 * and 80, then answered nothing -- and a package job stuck in
 * `state: building` that neither completed nor failed has been seen on
 * 192.168.15.95.
 *
 * SPEED rather than a blanket max-time, which is the important choice.
 * A hard total-time ceiling is wrong for the transfers that matter most
 * here: a 1.4 GB toolchain over a slow link is legitimate and would be
 * killed by any ceiling short enough to be useful against a silent
 * peer. The low-speed guards abort only when throughput STAYS below
 * the floor, so a transfer that is genuinely progressing is never
 * interrupted no matter how long it takes, while one that has stopped
 * is cut off promptly. It is the precise shape of the failure.
 *
 * 512 bytes/sec sustained for 60s is far under any real link and far
 * over "nothing", so it cannot fire on a slow-but-alive transfer. The
 * connect timeout bounds the handshake separately, since a peer that
 * never completes TLS never reaches the speed check at all.
 *
 * Every curlfetch_opts a call site builds sets connect_timeout to this
 * and, unless it has its own reason not to (kernel_releases_fetch_start
 * uses a flat max_time instead -- a release index is a few KB, so any
 * long duration is pathological rather than merely slow), low_speed_
 * limit/low_speed_time too. A caller that also sets retry_count
 * multiplies these: the guards bound one attempt, not the retry loop.
 */
#define PKG_CURL_CONNECT_TIMEOUT_SECS 20L
#define PKG_CURL_LOW_SPEED_LIMIT 512L
#define PKG_CURL_LOW_SPEED_TIME_SECS 60L
/* Space-separated capability names a recipe may request for its build
 * container (#224) -- room for a handful, not a policy surface. */
#define PKG_BUILD_CAPS_MAX 128

#define PKG_DEPENDS_MAX 256
/* ADR-0176: optional pkg_changelog= -- one short, single-line, free-text
 * summary of what changed in this specific published version (a commit
 * subject line, not a multi-paragraph release note -- extract_line_value()
 * only reads up to the closing quote or a newline, whichever comes
 * first, so a real multi-line changelog isn't representable here by
 * construction, matching the "short" scope this was explicitly asked
 * for). Empty for every recipe published before this field existed --
 * never backfilled, no retroactive edit of ~80 existing recipes for a
 * field that's optional by design. */
#define PKG_CHANGELOG_MAX 512
#define PKG_ERROR_MAX 256
/* ADR-0159 Phase B: a space-joined string of bare CONFIG_* symbol
 * names, room for a genuinely useful number of extra modules per
 * kmod-build call without being unbounded. */
#define PKG_HOSTBUILD_EXTRA_SYMBOLS_MAX 256
/* Max total packages in one resolved install chain (the target plus
 * every transitive dependency) -- a real cap, not an unbounded queue. */
#define PKG_MAX_DEP_CHAIN 32
/* Max pkg_source=/pkg_sha256= entries in one recipe -- lldap's own
 * real need is 9 (one main tarball + 8 CDN assets); generous headroom
 * past that, the same "bounded but roomy" precedent PKG_MAX_DEP_CHAIN
 * already sets, not an unbounded list. */
#define PKG_MAX_SOURCES 16

/* ADR-0157: the hard compile-time ceiling on build/install/hostbuild
 * chains that may exist at once -- sizes g_chains[]/g_build_output_
 * entries[] (pkg.c) and bounds main.c's own per-chain WS conn tracking
 * (g_build_log_ws_conns[]). Declared here, not pkg.c, because main.c
 * needs it too -- one source of truth for both translation units,
 * never redefined independently in pkg.c. This is the array size, not
 * the *operator-configured* limit -- see PKG_BUILD_MAX_JOBS_DEFAULT
 * and pkg_build_get_max_jobs()/pkg_build_set_max_jobs() (Phase 3) for
 * the actual, runtime-adjustable ceiling within [1, this]. Raised from
 * Phase 2's placeholder 2 to the task's own real target of 10. */
#define PKG_MAX_CONCURRENT_JOBS 10

/* ADR-0157 Phase 3: the operator-configured concurrency ceiling
 * (GET/PUT /v1/system/pkg-build-config's own `max_concurrent_jobs`)
 * defaults to this -- the task's own explicit ask ("configurable
 * limit, default 10"), and not coincidentally equal to the compile-
 * time ceiling above: out of the box, every array slot this daemon
 * ever allocates for a build chain is actually usable. */
#define PKG_BUILD_MAX_JOBS_DEFAULT PKG_MAX_CONCURRENT_JOBS

/* Reserved container name prefix for an in-flight build. The real,
 * per-chain container name is "<PKG_BUILD_CONTAINER_NAME>-<chain_idx>"
 * (pkg_build_container_name()) -- ADR-0157 Phase 2 moved this off a
 * single fixed name once concurrent builds became possible, since
 * registry_create() rejects a duplicate container name outright. Each
 * per-chain name still shows up in the normal GET /v1/containers
 * listing, deliberately -- one source of truth for "what's running,"
 * not a second hidden tracking path. */
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

/*
 * Issue #101's rule, now expressed in ADR-0256's vocabulary: WHY a
 * package failed is a field, never prose.
 *
 * "failed" alone cannot be acted on. A source that could not be reached
 * is usually transient and the right response is to try again; a build
 * that did not work is a real defect in a recipe or a toolchain and
 * retrying changes nothing; a missing or unparseable recipe is a
 * catalogue problem and neither. The only thing separating them used to
 * be an error string, so every caller -- `pkg ls`, the dashboard,
 * update-all's own summary -- had to string-match to tell them apart,
 * and none of them did. `enum pkg_failure_kind` fixed that with five
 * names of its own; ADR-0256 replaced those with the platform's one
 * (stage, status) pair, see pipeline.h.
 */

/*
 * ADR-0256: where each of a package's installs stands, one entry per
 * image it is installed into or being built for.
 *
 * The pipeline view needs this per (package, image) because that is the
 * grain builds actually have, while the source catalogue is per package
 * because that is the grain recipes have. Joining them is the whole job
 * of GET /v1/pipeline, and it needs the halves in a shape it can read
 * rather than a rendered JSON list to parse back.
 */
struct pkg_position {
	char image[PKG_IMAGE_NAME_MAX];
	char version[PKG_VERSION_MAX];
	enum pkg_state state;
	enum pipeline_stage stage;   /* meaningful only when status != PIPELINE_OK */
	enum pipeline_status status;
	char error[PKG_ERROR_MAX];
};

/* Returns how many were written. */
int pkg_positions(const char *name, struct pkg_position *out, int max);

/*
 * Issue #213: stop an in-flight build for (name, image).
 *
 * PKG_ERR_NOT_FOUND when no such entry exists; PKG_ERR_NOT_BUILDING when
 * the entry has no build in flight (installed, or already failed) --
 * refused rather than half-acted on, so a cancel racing a build that
 * has just finished cannot mark a completed install as cancelled.
 *
 * Does NOT record the failure, and does NOT kill anything. It marks the
 * entry and writes its build container's name to out_container (empty
 * when the entry is still FETCHING and no container exists yet); the
 * CALLER calls registry_remove() on it. pkg.c has never linked against
 * registry.h -- main.c alone owns every registry_* call -- and
 * pkg_resume() already hands work back the same way.
 *
 * The exit is then observed by pkg_build_completed() exactly as any
 * other build death, which records the outcome through the single
 * pkg_fail() path. Recording it here as well would mean two writers for
 * one outcome, racing over which description survives.
 */
enum pkg_error pkg_cancel(const char *name, const char *image,
                          char *out_container, size_t out_container_size);

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
	/*
	 * The caller named a build image and the recipe declares a
	 * different one (#182). Distinct from INVALID_RECIPE because the
	 * recipe is fine -- reporting "no such recipe, or it failed to
	 * parse" for a correct recipe sends the reader somewhere wrong,
	 * which is the failure this project keeps having to unpick.
	 */
	PKG_ERR_WRONG_BUILD_IMAGE,
	/*
	 * No build image was given and the recipe declares none. Its own
	 * code because it was reported as PKG_ERR_INVALID_NAME, which is
	 * a lie about a perfectly valid name: `POST /pkg/hostbuild
	 * {"name":"isotools"}` answered "invalid package name" while the
	 * real reason -- that isotools declares no pkg_build_image= and
	 * so needs one passed -- sat only in the daemon log. The reader is
	 * then debugging the wrong field. Same failure this enum's own
	 * WRONG_BUILD_IMAGE comment above exists to prevent.
	 */
	PKG_ERR_NO_BUILD_IMAGE,
	PKG_ERR_INVALID_TOOLCHAIN, /* toolchain_path missing, unreadable, or not a regular file */
	/*
	 * Issue #213: pkg_cancel() found the entry, but it has no build in
	 * flight. Its own code rather than reusing PKG_ERR_BUSY, which
	 * means the opposite ("something IS running"), or NOT_FOUND, which
	 * would say the package does not exist when it plainly does.
	 */
	PKG_ERR_NOT_BUILDING,
	/*
	 * #317: a hostbuild's BUILD IMAGE does not exist. The package and
	 * its recipe are both fine, and reporting PKG_ERR_NOT_FOUND said
	 * "no such package" about a package that is present and current --
	 * which sends the reader to check the recipe, the version and the
	 * catalogue, none of which is wrong. Measured on 192.168.15.95:
	 * `POST /pkg/hostbuild {"name":"kernel","version":"6.18.40-24"}`
	 * answered 404 "no such package" while the real cause was a missing
	 * `kernel-builder`. Same failure the two build-image codes above
	 * already exist to prevent, one step further along.
	 */
	PKG_ERR_NO_SUCH_BUILD_IMAGE,
	/*
	 * #318: a DEPENDENCY could not be resolved. The package asked for
	 * is fine, and reporting PKG_ERR_INVALID_RECIPE said "no such
	 * recipe, or it failed to parse" about a recipe that parses --
	 * sending the reader to inspect the wrong file. resolve_chain()
	 * already builds a precise message naming what it could not
	 * resolve; that message was being discarded at the return. It now
	 * reaches the log store, and this code makes the API answer point
	 * at the dependency rather than at the package.
	 */
	PKG_ERR_DEP_UNRESOLVABLE,
	PKG_ERR_TARGET_IMAGE_NOT_FOUND /* pkg_image_recipe_apply_start(): the recipe itself parsed
	                                 * fine, but the image it names doesn't exist yet -- distinct
	                                 * from PKG_ERR_INVALID_RECIPE (a genuine parse failure) and
	                                 * from PKG_ERR_NOT_FOUND (the recipe itself missing), which
	                                 * this used to collapse into, misleadingly reporting a real
	                                 * "create the image first" situation as if the recipe's own
	                                 * content were malformed. */,
	PKG_ERR_NOT_INSTALLED /* the package row exists but is not INSTALLED -- a failed or
	                       * in-flight build. Distinct from PKG_ERR_NOT_FOUND (no such
	                       * package at all) for the same reason
	                       * PKG_ERR_TARGET_IMAGE_NOT_FOUND is: collapsing the two reports
	                       * "no such thing" for something the operator can plainly see. */
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
 * a container-visible path (a bare bzImage or cixd/cixctl/web/
 * has no business inside a normal container image's rootfs).
 */
int pkg_init(const char *pkg_dir, const char *installed_state_path, const char *containers_dir,
              const char *images_dir, const char *artifacts_dir);

/*
 * ADR-0141 Phase 4: repoints every REBUILDABLE_DIR-derived path this
 * module caches (and image_recipe_init()'s own, via image_recipe_
 * repoint()) without touching in-memory package state or any
 * in-flight job -- see network_repoint()'s own doc comment (daemon/
 * src/network.c) for the shared reasoning. containers_dir is
 * deliberately not a parameter -- it never moves as part of this
 * migration.
 */
/*
 * Called periodically: reports any in-flight build that has produced no
 * output for a long time, with what every process in its build
 * container is blocked on. Reports only -- never kills.
 */
void pkg_check_build_stalls(void);

/* Seconds between pkg_check_build_stalls() runs -- derived from the
 * configured stall threshold so the two cannot drift apart. */
long pkg_build_stall_check_interval(void);

void pkg_repoint(const char *pkg_dir, const char *installed_state_path, const char *images_dir,
                  const char *artifacts_dir);

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
 * Real sha256 of a real on-disk file, computed in-process against the
 * already-linked libcrypto (EVP_sha256(), #352 -- no forked sha256sum
 * any more) -- exported so main.c's own bootstrap-fetch mechanism
 * (ADR-0065: fetching the toolchain artifact itself over HTTP, not
 * just importing an already-local one) can verify a freshly curl'd
 * artifact the same way pkg_fetch_completed() already verifies every
 * recipe source, one real implementation, not a second copy of it.
 * out_size must be at least 65 (64 hex chars + NUL); returns 0 with
 * out null-terminated on success, -1 on any failure (open, read,
 * digest).
 */
int pkg_run_capture_sha256(const char *path, char *out, size_t out_size);

/*
 * Test-only concrete-fact check (daemon/src/main.c's own
 * --test-bootstrap-toolchain= self-test): does the pkgbuild rootfs
 * genuinely have gcc after a bootstrap call, not just "did the call
 * return OK" -- g_pkgbuild_rootfs itself is private to pkg.c. Returns
 * 1 if present, 0 otherwise.
 */
int pkg_toolchain_has_gcc(void);

/*
 * Issue #40: folds a pre-existing flat <images_dir>/pkgbuild/rootfs
 * into the build-sandbox image, once, and sets the old directory aside
 * rather than deleting it. Must be called AFTER image_init(), since it
 * goes through the ordinary image versioning path; a no-op on any box
 * that has already migrated or never had a flat sandbox.
 */
void pkg_migrate_build_sandbox(void);

/*
 * Seeds a target rootfs directory with the fixed baseline every
 * container needs but no image ever gets from pkg install alone
 * (ADR-0019, ADR-0023, ADR-0041): the C runtime a dynamically-linked
 * package needs to actually execve() (ld.so, libc.so.6, libtinfo.so.6,
 * ...), from the same fixed host paths pkg_bootstrap_build_image() and
 * this project's own test/install-image tooling already use; standard
 * char device nodes (/dev/{null,zero,full,random,urandom}, same
 * table/pattern test_image_fixture_stage_toolchain() already proves
 * safe); and a plain, empty /run directory. Takes the target rootfs
 * *path* directly (ADR-0107/0108: an image version's own rootfs is
 * IMAGES_DIR/<image>/<version>/rootfs, not a flat per-image path --
 * image.h's image_version_rootfs_path() is the one place that path is
 * ever computed, so this function no longer derives it itself from an
 * image name). The runtime-lib half is idempotent (skips a file
 * already staged) and tolerant of a missing source file on this
 * particular host (skip, not fatal -- same precedent
 * pkg_bootstrap_build_image() already sets); the dev-node/run half has
 * no such "host source might be missing" excuse, so any real mkdir/
 * mknod failure there is fatal. Either half failing returns
 * PKG_ERR_PERSIST_FAILED. Called automatically by pkg_build_completed()
 * for whatever new image version a job is merging into -- exposed
 * publicly too so image_create() can seed a freshly created version's
 * still-empty rootfs immediately. This is a fixed, hardcoded baseline
 * set, not an extensible "package declares what it needs" mechanism
 * (ADR-0041).
 */
enum pkg_error pkg_seed_image_baseline(const char *rootfs_path);

/*
 * Natural-sort/dpkg-style version comparison (ADR-0107): walks both
 * strings left to right, alternating between runs of digits (compared
 * numerically) and runs of non-digits (compared byte-wise). Handles
 * every pkg_version= actually in use across this project's own
 * recipes, including a leading non-digit prefix ("v1.4.0") and a
 * non-numeric trailing component ("1.5.8.pl02") -- a naive
 * per-component atoi() split silently mis-orders both. No semver-
 * specific concepts (pre-release precedence, build metadata). Returns
 * <0/0/>0 like strcmp(): a < b, a == b, a > b.
 */
int pkg_version_compare(const char *a, const char *b);

/*
 * ADR-0255: the discovery kind package `name` declares (pkg_upstream=),
 * or "" when it declares none and is therefore pinned. Returns 0 when a
 * recipe was found and parsed, -1 otherwise -- and -1 is not an error
 * condition for a caller, it simply means the package does not roll.
 */
int pkg_recipe_upstream(const char *name, char *out, size_t out_size);

/* Scans pkg_dir/recipes/<name>/<version>/build.sh (ADR-0107's
 * version-keyed layout) and writes one {name,version,depends} object
 * per (name,version) pair that parses -- metadata only, never
 * sourced/executed. Multiple entries may share the same name at
 * different versions. */
void pkg_write_json_recipes(struct json_writer *w);

/*
 * One recipe version's own {name,version,depends,content} -- the raw
 * shell script text too, unlike pkg_write_json_recipes()'s list-view
 * metadata-only shape, for the web dashboard's per-package Recipe tab.
 * version NULL or "" resolves to the highest available version for
 * name (pkg_version_compare()-ordered), matching every other
 * "no explicit version given" caller in this module. Still never
 * sourced/executed here, same as every other recipe read in this
 * module -- content is only ever actually run inside the isolated
 * build container.
 * PKG_ERR_INVALID_NAME / PKG_ERR_NOT_FOUND (missing or fails to parse).
 */
/*
 * Queues an already-built, installed artifact for publication to the
 * configured artifact cache, exactly as a fresh build would (issue
 * #171). Returns PKG_OK if queued, PKG_ERR_NOT_FOUND if no installed
 * package of that name exists, PKG_ERR_INVALID_RECIPE if publishing is
 * not configured.
 *
 * Publishing used to be reachable only as a side effect of building,
 * so an artifact that failed to push, or was built before push was
 * configured, or predates a rule change, could never be published
 * again without rebuilding it -- which for `cix` or `kernel` is the
 * most expensive thing this platform does.
 */
enum pkg_error pkg_artifact_publish(const char *name);
void pkg_artifact_cache_path(const char *name, const char *version, char *out, size_t out_size);
int pkg_artifact_cache_has(const char *name, const char *version);

/*
 * Stages the package seed an installer ISO carries into dest_dir, as
 * <dest>/recipes and <dest>/artifacts (#135).
 *
 * Breaks the fresh-install bootstrap cycle: no recipes without a forge,
 * no forge without DNS, no DNS without a recipe. Both halves of the
 * delivery already existed (mkinstalleriso stages the seed, cix-install
 * consumes it) and nothing produced one.
 *
 * Fails rather than staging a partial seed: media that claims to carry
 * one and cannot bring up DNS is worse than media that carries none,
 * because the failure only shows on the installed box.
 */
enum pkg_error pkg_seed_stage(const char *dest_dir, char *err, size_t err_size);

/*
 * The image rebuilds this host has queued but not started (#236).
 *
 * In memory and deliberately not persisted: a rebuild is re-derivable
 * from the manifests at any time, so a restart drops pending work
 * rather than resuming a stale intention.
 */
/*
 * Re-derives the rolling-rebuild queue from the images that are
 * actually behind a rolling package (#373). Called once at startup: the
 * queue is in-memory, so a restart between a publish and the drain lost
 * it entirely. Derived rather than persisted -- an image that is behind
 * is discoverable, so this is recoverable state, and deriving it also
 * repairs a queue lost to a crash.
 */
/*
 * Closes every still-open run as failed, because the daemon is stopping
 * (#375). Called once on the shutdown path. ADR-0272 compliant: the
 * daemon stopping is a real outcome, written once and never amended --
 * not a half-record. Does not cover a panic or power cut, where nothing
 * can be written at the time.
 */
void pkg_runs_close_open_at_shutdown(void);

void pkg_rebuild_queue_rederive(void);

void pkg_rebuild_queue_write_json(struct json_writer *w);

/*
 * ADR-0272: the pipeline run store -- what has HAPPENED to an atom, as
 * opposed to where it stands now (which GET /v1/pipeline still derives
 * at read time and stores nothing for).
 *
 * pkg_runs_write_json() renders the whole object for
 * GET /v1/pipeline/runs: newest first, optionally narrowed to one
 * package and/or one image, capped at `limit` (<= 0 means the store's
 * own ceiling). Each run reports both the build log it wrote and
 * whether that log still exists -- build logs are pruned at
 * PKG_BUILD_LOG_KEEP and runs outlive them by design, so naming a
 * pruned file is the normal case and is said rather than hidden.
 *
 * Retention is a count and is settable at runtime through
 * /v1/system/pipeline-config. pkg_run_retention_set() returns -1 and
 * changes nothing for a value outside 1..2000.
 */
void pkg_runs_write_json(struct json_writer *w, const char *name, const char *image, int limit);
int pkg_run_retention_get(void);
int pkg_run_retention_set(int keep);

/*
 * ADR-0273: the three gates, and the approvals that let one held change
 * through.
 *
 * A gate holds automation at a point where a change escapes its own
 * blast radius -- an artifact reaching the shared cache ("publish"), an
 * image rolling to a version nobody asked for ("roll"), a boot slot
 * being written ("deploy"). All three default OFF, and a host with them
 * off behaves exactly as it did before: the hold is one test at the
 * head of each drain and one refusal in the update handler.
 *
 * A held item is reported as PIPELINE_BLOCKED with blocked_on
 * {kind:"approval", name:<gate>}. There is deliberately no sixth
 * pipeline_status -- "blocked" already means "waiting on something
 * outside this stage, or a person".
 *
 * pkg_target_is_queued() answers "is this actually held" from the live
 * queues and nothing stored, which is what lets the pending list have
 * no persistent state. "deploy" always answers 1 while its gate is on:
 * an update is one synchronous request with no queue to be in.
 */
#define PKG_APPROVE_OK 0
#define PKG_APPROVE_UNKNOWN_GATE (-1)
#define PKG_APPROVE_NOT_HELD (-2)
#define PKG_APPROVE_ALREADY (-3)
#define PKG_APPROVE_FULL (-4)

int pkg_gate_enabled(const char *gate);
int pkg_gate_set(const char *gate, int on);
int pkg_target_is_queued(const char *gate, const char *target);
int pkg_approval_grant(const char *gate, const char *target, const char *who);
int pkg_approval_take_deploy(const char *target);
void pkg_approvals_write_json(struct json_writer *w);
enum pkg_error pkg_artifact_publish_resolve(const char *name, char *out_version,
                                             size_t out_version_size, int *out_is_hostbuild);

enum pkg_error pkg_recipe_get(const char *name, const char *version, struct json_writer *w);

/*
 * Adds a new recipe version at pkg_dir/recipes/<name>/<version>/build.sh,
 * where <name>/<version> both come from content's own pkg_name=/
 * pkg_version= fields (the same "filename and pkg_name= agree"
 * invariant this always enforced, now extended to the version
 * directory too). content is written to a staging file first and
 * validated with the same parse_recipe() every install-time lookup
 * already uses; only on success is it atomically renamed into place.
 *
 * Recipe versions are immutable once added (ADR-0107) -- unlike the
 * old flat-file upsert-by-name behavior, adding a (name,version) pair
 * that already exists is PKG_ERR_DUPLICATE, not a silent overwrite.
 * Fixing a mistake in an already-published version means publishing a
 * new version.
 * PKG_ERR_INVALID_NAME / PKG_ERR_INVALID_RECIPE / PKG_ERR_DUPLICATE /
 * PKG_ERR_PERSIST_FAILED on failure.
 */
enum pkg_error pkg_recipe_add(const char *name, const char *content, int *out_was_approval);

/*
 * Removes recipe version(s) for name. version NULL or "" removes every
 * version of name (the whole pkg_dir/recipes/<name>/ directory);
 * a specific version removes only that one version's subdirectory,
 * leaving any other published versions of name intact. Does not touch
 * anything already installed via that recipe (a build's output is
 * merged into an image at install time -- nothing about a package
 * that's already installed depends on its own recipe file continuing
 * to exist); only affects future `pkg install`/update-all lookups.
 * PKG_ERR_INVALID_NAME / PKG_ERR_NOT_FOUND / PKG_ERR_PERSIST_FAILED.
 */
enum pkg_error pkg_recipe_delete(const char *name, const char *version);

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
 *
 * version (ADR-0107) is NULL or "" for every pre-existing, non-
 * manifest-aware caller (direct CLI/REST install, tests) -- resolves
 * to the highest available version for name, matching the "rolling"
 * default posture. A non-empty version pins name itself to exactly
 * that published recipe version (used by manifest-driven image
 * builds); every one of name's own dependencies still always resolves
 * to ITS OWN highest available version regardless -- pinning only
 * ever applies to the single top-level package actually being
 * installed, never transitively to its build-time prerequisites.
 * PKG_ERR_INVALID_RECIPE if version is given but no such (name,version)
 * recipe exists.
 *
 * out_chain_idx (ADR-0157 Phase 2): only meaningful on PKG_OK, the
 * index into pkg.c's own small g_chains[] table this job was assigned
 * -- the caller must thread it through register_pkg_fetch_pidfd() so
 * the corresponding fetch-exit event routes back to the right chain
 * (see that function's own doc comment, daemon/src/main.c).
 *
 * keep_on_failure (ADR-0175/issue #35): 0 for every pre-existing
 * caller's unchanged behavior. When true, a build-container failure
 * (any nonzero exit_status reaching pkg_build_completed()) leaves the
 * exited build container registered and its overlay mounted instead
 * of tearing it down -- readable afterward via the already-existing
 * GET /v1/containers/{name}/files?path=... (ADR-0055, which already
 * supports an exited-but-registered container's upperdir) for pulling
 * out whatever the failed build actually produced (a core dump, a
 * partially-built object tree, a crashing binary) for real,
 * off-daemon debugging. The container's own name is reported back as
 * this entry's kept_build_container (write_pkg_json()) once failed.
 * Never applies to a *successful* build (nothing to preserve) or to a
 * dependency mid-chain (only the chain's own final job -- the one the
 * caller actually asked to install/hostbuild -- honors this flag; an
 * unrelated prerequisite failing is a normal, uninteresting failure).
 * Cleanup is the existing, ordinary container-delete path: DELETE
 * /v1/containers/{name} (or POST .../stop) on the preserved name
 * removes it exactly like any other exited container, no new
 * mechanism needed (see pkg_build_completed()'s own comment).
 */
/*
 * Gives the default image a C library, once, if it has none and one can
 * be installed without building anything (#189).
 *
 * A freshly installed host materializes "base" at first boot with the
 * ordinary image baseline, which -- since ADR-0216 closed the glibc
 * floor -- no longer includes a runtime borrowed from the build host.
 * Correct, but it leaves the one image a fresh box actually has unable
 * to run a container until an operator installs a libc into it.
 *
 * This installs one through the ordinary pipeline, so the image ends up
 * with a real manifest entry, a version and an upgrade path -- never a
 * copy, which is the distinction ADR-0216 exists to hold.
 *
 * Deliberately refuses unless it would be a cache hit: a recipe alone is
 * not enough, because building glibc on a box that has no toolchain yet
 * is a long failure, not a fix. Returns PKG_OK with the child's pid and
 * pidfd for the caller to register (asynchronous, like every other
 * install), or PKG_ERR_NOT_FOUND when there is nothing to do -- already
 * has a runtime, no recipe, or no cached artifact -- which is the
 * ordinary case on every boot after the first.
 */
enum pkg_error pkg_seed_default_image_libc(pid_t *out_pid, int *out_pidfd, int *out_chain_idx);

/*
 * 1 if that install has finished and FAILED, with its own error text
 * copied into out_error. Lets a first boot say why the default image is
 * still unusable instead of appearing to hang: the install is
 * asynchronous, so without this the only observable difference between
 * "still working" and "gave up" is that nothing ever happens.
 */
int pkg_default_image_libc_failed(char *out_error, size_t out_error_size);

enum pkg_error pkg_install_start(const char *name, const char *image, const char *version,
                                  int upgrade, int keep_on_failure, char *out_started_name,
                                  size_t out_started_name_size, pid_t *out_pid, int *out_pidfd,
                                  int *out_chain_idx);

/*
 * A second mode of the same fetch/build pipeline pkg_install_start()
 * drives, for building a standalone HOST artifact (a kernel bzImage, a
 * fresh cixd/cixctl/web/ control-plane) rather than installing
 * into a container image's rootfs (ADR-0056). Reuses fetch/verify/
 * stage/build completely unmodified -- the build container gets the
 * exact same offline, no-network isolation every ordinary install
 * already gets. Two real differences from pkg_install_start():
 *
 *   - The build container's own lowerdir is build_image's rootfs
 *     (an ordinary image, built up via completely normal `pkg install
 *     --image=<build_image>` calls beforehand -- e.g. installing
 *     "tcc"/"make" into a "cix-builder" image), never the shared
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
 * PKG_STATE_INSTALLED and either upgrade is false, or the recipe's
 * current pkg_version= matches what's already installed (ADR-0094 --
 * same semantics as pkg_install_start()'s own upgrade parameter: a
 * genuinely unchanged version is still a no-op duplicate even with
 * upgrade=1, since nothing would actually differ; bump pkg_version=
 * to force a real rebuild).
 *
 * version (ADR-0107): same meaning as pkg_install_start()'s own --
 * NULL or "" resolves to name's highest available recipe version;
 * a non-empty value hostbuilds that exact published version instead
 * (e.g. rebuilding an older cix.recipe release on demand).
 *
 * extra_config_symbols (ADR-0159 Phase B): NULL or "" for every caller
 * except POST /v1/system/kmod-build -- a pre-validated, space-joined
 * string of bare CONFIG_* symbol names. Written by cixd itself, as
 * "<SYMBOL>=m" lines, to /build/extra/kmod-extra.config (#412 --
 * previously passed through as the build container's own
 * CIX_KMOD_EXTRA_SYMBOLS environment variable for the recipe to
 * re-derive the same file from; cixd already owns /build/extra,
 * ADR-0036, so it writes the file directly now). Recipe-agnostic at
 * this layer (pkg.c has no notion of "the kernel recipe" specifically)
 * -- only kernel.recipe's own pkg_build() actually reads that file;
 * any other recipe simply never looks for it. PKG_ERR_INVALID_NAME if it doesn't fit
 * PKG_HOSTBUILD_EXTRA_SYMBOLS_MAX.
 *
 * out_chain_idx (ADR-0157 Phase 2): same contract as pkg_install_
 * start()'s own out_chain_idx -- only meaningful on PKG_OK.
 *
 * keep_on_failure (ADR-0175/issue #35): same contract as
 * pkg_install_start()'s own keep_on_failure -- see its doc comment.
 * The primary motivating use case for this flag existing at all: a
 * hostbuild is exactly the shape a kernel/toolchain-bootstrap failure
 * (e.g. issue #32's own gcc stage2 segfault) takes, and this is the
 * only way to get the crashing binary/object files off the box for
 * real inspection afterward.
 */
enum pkg_error pkg_hostbuild_start(const char *name, const char *build_image, const char *version,
                                    int upgrade, const char *extra_config_symbols,
                                    int keep_on_failure, pid_t *out_pid, int *out_pidfd,
                                    int *out_chain_idx);

/*
 * ADR-0177/issue #46: resumes a build container a prior FAILED attempt
 * left preserved (keep_on_failure above) instead of paying for a full
 * fetch+extract+build restart from scratch -- the real, repeated cost
 * this exists to eliminate (see the ADR's own motivating case: a multi-
 * hour gcc bootstrap, restarted from zero after every single one-line
 * recipe fixup). No fetch subprocess is involved at all (unlike pkg_
 * install_start()/pkg_hostbuild_start() above) -- this call is
 * synchronous and, on PKG_OK, immediately fills *spec_out exactly like
 * pkg_fetch_completed() does, ready for the caller to registry_create()
 * the (reused-name) build container right away.
 *
 * (name, image) must resolve to a real entry currently PKG_STATE_FAILED
 * with a non-empty kept_build_container (PKG_ERR_NOT_FOUND otherwise --
 * nothing to resume; GET the entry first to check). version: same
 * ADR-0107 meaning as every other entry point here -- NULL/"" resolves
 * to the highest available recipe version, letting a caller resume
 * under a newly-published, fixed recipe version without that fix ever
 * needing to reuse the failed attempt's own exact version string.
 * extra_config_symbols: same ADR-0159 Phase B meaning/validation as
 * pkg_hostbuild_start()'s own parameter (NULL/"" for every caller except
 * a kmod-build resume). keep_on_failure: same ADR-0175 meaning, applies
 * to this resumed attempt's own outcome, independent of whether the
 * original attempt asked for it.
 *
 * PKG_ERR_BUSY if the original chain slot (parsed back out of
 * kept_build_container itself via pkg_build_container_chain_index() --
 * never a freshly allocated one, see this function's own definition for
 * why) has since been reclaimed by a different, unrelated job.
 *
 * Deliberately does not touch the registry -- pkg.c has no dependency
 * on registry.h and this preserves that; the caller is responsible for
 * a registry_remove() on the exact same container name (derivable via
 * pkg_build_container_name(*out_chain_idx, ...)) before its own
 * registry_create() call, mirroring handle_pkg_fetch_event()'s existing
 * sequence with one extra registry_remove() first since this name is a
 * reuse, not a fresh one.
 */
enum pkg_error pkg_resume_build(const char *name, const char *image, const char *version,
                                 const char *extra_config_symbols, int keep_on_failure,
                                 struct container_spec *spec_out, int *out_chain_idx,
                                 int *out_stdio_write_fd);

/*
 * Called once the tracked fetch subprocess's pidfd fires (caller has
 * already waitpid()'d it and passes the raw exit status). On a clean
 * exit, verifies the download against the recipe's pkg_sha256 (via
 * the real `sha256sum`, never hand-rolled crypto), extracts it, and
 * fills *spec_out (pointers into pkg.c's own static storage, valid
 * until the next pkg_* call -- safe given v1's one-job-at-a-time
 * serialization) ready for the caller to registry_create() the build
 * container itself, including a pipe wired up to capture its real
 * stdout/stderr (struct container_spec's capture_output field) --
 * pkg_build_completed() reads it back once the container exits, so a
 * build failure's actual diagnostic output reaches the log store
 * even though this project has no SSH/console access to a real
 * remote box's own inherited stdout (ADR-0034). *out_stdio_write_fd
 * is set to the pipe's write end; only meaningful when this function
 * returns 1 (same conditional validity as *spec_out itself) -- the
 * caller must close it right after registry_create() (the child
 * already inherited its own copy via clone3, so closing the parent's
 * copy is safe and necessary hygiene). Returns 1 if the caller should
 * registry_create() the build container, 0 if the job already ended
 * (FAILED -- bad exit status or checksum mismatch) and there is
 * nothing to build.
 *
 * chain_idx (ADR-0157 Phase 2): identifies which chain this fetch
 * completion belongs to -- the caller's own conn already knows this
 * (register_pkg_fetch_pidfd()'s own chain_idx parameter, threaded
 * through struct conn), so it's passed straight in rather than
 * re-derived. Every "the" build this function's own comments still
 * refer to below is that specific chain's build, not a singular
 * daemon-wide one.
 */
int pkg_fetch_completed(int chain_idx, int exit_status, struct container_spec *spec_out,
                         int *out_stdio_write_fd, pid_t *out_compose_pid, int *out_compose_pidfd);

/*
 * Continues an install whose build environment was being composed in a
 * forked child (#238), once that child has exited. Same return contract
 * as pkg_fetch_completed(): 1 to start the build described in spec_out,
 * 0 if the install is over, 2 if another composition child was forked
 * and out_compose_pid/out_compose_pidfd name it.
 */
int pkg_buildenv_completed(int chain_idx, int exit_status, struct container_spec *spec_out,
                            int *out_stdio_write_fd, pid_t *out_compose_pid,
                            int *out_compose_pidfd);

/* Called if registry_create() itself fails for the build container
 * pkg_fetch_completed() just prepared -- transitions the in-flight
 * job to FAILED (there is no container to wait for in this case). */
void pkg_build_spawn_failed(int chain_idx);

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
 * *means* (ADR-0057's own ARTIFACTS_DIR/<name>/cixd-root.squashfs
 * assembly for name=="cix" specifically is main.c's business, not
 * this module's).
 *
 * out_chain_idx (ADR-0157 Phase 2): only meaningful when this
 * function returns 1 -- the same chain the just-completed build
 * belonged to (a dependency chain always advances within its own
 * chain slot, never migrates), for the caller to thread into
 * register_pkg_fetch_pidfd() for the newly-started next fetch.
 *
 * out_kept (ADR-0175/issue #35): set to 1 exactly when this
 * completion was a failure (exit_status != 0) of the chain's own
 * final job with keep_on_failure requested (pkg_install_start()'s/
 * pkg_hostbuild_start()'s own parameter) -- 0 in every other case
 * (success, a mid-chain dependency failure, or keep_on_failure not
 * requested). The caller (main.c's handle_container_event()) must
 * skip its own registry_remove() of container_name when this is 1,
 * leaving the exited build container's registry entry and overlay
 * mount in place instead of tearing them down. No special "kept"
 * registry state is needed for this: the container is left as an
 * entirely ordinary exited, registered container, so an operator can
 * remove it later through the completely ordinary DELETE
 * /v1/containers/{name} path (a plain registry_remove(), no call back
 * into this function at all -- pkg.c's own chain state for this job
 * is already fully settled by the time *out_kept was set) exactly
 * like any other exited container. ADR-0055's GET .../files endpoint
 * already reads an exited-but-registered container's upperdir, which
 * is the actual point of preserving it at all.
 */
int pkg_build_completed(const char *container_name, int exit_status, pid_t *out_pid, int *out_pidfd,
                         int *out_chain_idx, char *out_hostbuild_done_name, int *out_kept);

/*
 * ADR-0157 Phase 2: computes this chain's own real, distinct build
 * container name ("__pkgbuild-<chain_idx>") -- exported so main.c's
 * own registry_create() call (handle_pkg_fetch_event()) can pass the
 * same name pkg_fetch_completed() already put in spec_out->ns.
 * hostname/cg.name. out must be at least PKG_NAME_MAX bytes.
 */
void pkg_build_container_name(int chain_idx, char *out, size_t out_size);

/*
 * ADR-0157 Phase 2: the inverse of pkg_build_container_name() -- given
 * a real container name (e.g. from a registry_remove()/handle_stop()
 * event), returns which chain it belongs to, or -1 if container_name
 * doesn't match the "<PKG_BUILD_CONTAINER_NAME>-<chain_idx>" shape at
 * all (the overwhelming majority of real container names, since a
 * build container is never the only kind that can be stopped).
 */
int pkg_build_container_chain_index(const char *container_name);

/*
 * ADR-0157 Phase 2: resolves a caller-supplied name+image pair (e.g.
 * GET /v1/pkg/build/log's own ?name=&image= query params) to the
 * concrete chain_idx currently building that target, now that a fixed
 * single build no longer exists to attach to implicitly. image may be
 * NULL/"" (defaults exactly like every other image-optional entry
 * point in this file). Returns -1 if no chain is currently building
 * that target.
 */
int pkg_chain_index_for_target(const char *name, const char *image);

/*
 * ADR-0157 Phase 2: fills out_indices (room for at least
 * PKG_MAX_CONCURRENT_JOBS ints) with every currently-busy chain's
 * index, returning how many. Used by main.c's own GET /v1/pkg/build/
 * log handler to fall back to "the one build in progress" when a
 * caller supplies no ?name= -- unambiguous exactly when this returns
 * 1, matching the old single-build behavior.
 */
/*
 * Names the chains currently holding a job slot ("name@image, ..."),
 * comma-separated, and returns how many there are (#246). Empty string
 * when none. Lets an operator see what is occupying the concurrency
 * budget instead of inferring it from a refusal.
 */
int pkg_active_chain_names(char *out, size_t out_size);

int pkg_active_chain_indices(int *out_indices);

/*
 * ADR-0107: publishing a new recipe version (pkg_recipe_add()) queues a
 * rebuild for every image whose own manifest tracks that package as
 * "rolling" with a floor at or below the new version -- but the single-
 * job-in-flight constraint (PKG_ERR_BUSY) every install/hostbuild
 * already shares means that rebuild can't start immediately if a job is
 * already running. This is pkg.c's own drain step for that queue,
 * called by main.c at every point a job's completion might free the
 * single in-flight slot (pkg_build_completed()/pkg_fetch_completed()
 * returning "nothing more to chain", and pkg_build_spawn_failed()) --
 * mirrors pkg_build_completed()'s own out-param/return shape exactly,
 * so the caller reacts identically ("nonzero: register *out_pid/
 * *out_pidfd via register_pkg_fetch_pidfd(), same as any other started
 * job"). Internally walks the queue front-to-back: an image whose
 * manifest is already fully satisfied (every pinned entry installed at
 * its exact version, every rolling entry installed at its own current
 * highest available version) is dropped with no job started, and the
 * next queued image is tried instead -- so one call can both start a
 * real job AND drain any number of already-satisfied entries ahead of
 * it. Returns 0 if a job is already in flight or the queue is (now)
 * empty.
 *
 * out_chain_idx (ADR-0157 Phase 2): same contract as pkg_install_
 * start()'s own out_chain_idx -- only meaningful when this function
 * returns nonzero.
 */
/* Images waiting for a rolling rebuild. Lets the event loop check
 * whether there is work before calling the expensive drain (#236). */
int pkg_rebuild_queue_depth(void);

/*
 * ADR-0270: enqueue an image for convergence from outside pkg.c, which
 * is what a deployment applied against an unrealized image needs.
 * Idempotent -- an image already queued is left alone.
 */
void pkg_rebuild_queue_add(const char *image);

/*
 * An image no longer exists: drop anything the package layer still
 * holds for it (#382). Today that is its place in the rebuild queue
 * and any approval granted against it -- a grant outlives its target
 * otherwise, and GET /v1/pipeline/approvals goes on reporting a
 * deleted image as approved forever.
 */
void pkg_image_forgotten(const char *image);

int pkg_try_start_queued_rebuild(pid_t *out_pid, int *out_pidfd, int *out_chain_idx);

/*
 * The build-output capture pipe's read end (see pkg_fetch_completed()'s
 * doc comment), for the caller to register with its own epoll loop
 * right after a successful registry_create() -- ADR-0087: draining
 * this only once, after the container has already exited, deadlocks
 * any build whose combined stdout+stderr exceeds the pipe's 64KB
 * kernel buffer, since nothing would read from it while the build is
 * still running. Returns -1 if no build is in flight for chain_idx or
 * the pipe2() call itself failed (capture is a diagnostic nicety,
 * never a reason to fail the build).
 */
int pkg_build_output_fd(int chain_idx);

/*
 * Called whenever pkg_build_output_fd()'s fd reports EPOLLIN --
 * drains everything currently available (the fd is non-blocking) into
 * pkg.c's own bounded, sliding-window capture buffer, the same one
 * pkg_build_completed() logs from on a build failure. If new_data is
 * non-NULL, also copies up to new_data_cap of the raw bytes read this
 * call into it (*new_data_len set to how many) -- for a caller that
 * wants to relay newly-arrived output live (see
 * try_pkg_build_log_upgrade(), daemon/src/main.c) without waiting for
 * a failure. Pass NULL/0/NULL when only draining matters (e.g.
 * pkg_build_completed()'s own internal call). Returns 1 if the pipe
 * reached EOF or a real read error (every write end has been closed
 * -- the caller should now tear down its own epoll registration and
 * call pkg_build_output_close()), 0 if there may still be more to
 * come later.
 */
int pkg_build_output_readable(int chain_idx, char *new_data, int new_data_cap, int *new_data_len);

/*
 * Copies the current sliding-window tail (up to out_cap bytes) into
 * out, for a live-log client attaching mid-build to see everything
 * captured so far before streaming further chunks. Returns the number
 * of bytes copied.
 */
int pkg_build_output_snapshot(int chain_idx, char *out, int out_cap);

/* Closes pkg_build_output_fd()'s fd and clears it -- called by the
 * caller once pkg_build_output_readable() returns 1, or directly by
 * pkg_build_spawn_failed()'s own caller path when registry_create()
 * never actually spawned a container to produce any output at all
 * (no epoll registration ever existed to drive the EOF path in that
 * case). Safe to call when already closed (no-op). */
void pkg_build_output_close(int chain_idx);

/*
 * Issue #57: every build's COMPLETE output, teed to a file as it
 * streams. The in-memory capture is a ~4KB tail and `pkg build-log` is
 * live-only, so a finished build's full output used to survive nowhere
 * -- which cost two multi-hour round trips on gcc: once when a verbose
 * configure swamped the tail and hid the real error, once when the
 * workaround silenced the live stream and a healthy 71-minute build was
 * misread as hung and killed. Both were the same missing primitive.
 */
struct pkg_build_log_entry {
	char file[256];
	long long size_bytes;
	long long modified_at;
};

int pkg_build_log_list(struct pkg_build_log_entry *out, int max);
/* Returns bytes written into buf (tail-first if the log is larger than
 * cap -- the end is where the failure is), or -1 for no such log.
 * out_total, when non-NULL, receives the log's real full size. */
long long pkg_build_log_read(const char *file, char *buf, long long cap, long long *out_total);

/* Metadata for every known package (installed or in-flight): name,
 * image, version, state, error (null unless FAILED), files (manifest,
 * empty until INSTALLED), available_version (null if up to date or
 * not yet installed, else the recipe's current version -- re-read
 * from disk on every call, One Source of Truth). */
void pkg_write_json_list(struct json_writer *w);

/* The same packages as CONFIGURATION: name, image, version, state, and
 * nothing transient. The full form above is 97% files[] manifests by
 * volume, which is the right answer for GET /pkg and the wrong one for
 * a document meant to be read and diffed (ADR-0206). */
void pkg_write_json_config(struct json_writer *w);

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
/*
 * GET /pkg/drift (issue #217): how many packages are installed, how
 * many are behind the recipe on disk, and which ones. Shares
 * pkg_entry_drift() with available_version and update-all, so there is
 * one comparison rule rather than three.
 */
void pkg_write_drift_json(struct json_writer *w);

/*
 * Issues #281 and #289: {"packages":[...], "checked":N, "incomplete":N}
 * -- every INSTALLED package whose own recorded files are not present
 * in its image's current rootfs, or whose installed headers make
 * unconditional includes that do not resolve in that image. Empty
 * `packages` is the healthy answer.
 *
 * On demand only. It stats every file of every installed package, which
 * is why it is its own endpoint rather than a field on GET /v1/pkg --
 * that one is polled every two seconds and is already this daemon's
 * largest source of event-loop stalls.
 *
 * Measured on 192.168.15.95 with 202 installed packages: 2.66 s, and
 * stallwatch attributes the boot's worst loop pass to this endpoint by
 * name. Below the 5 s stall threshold, above the 750 ms slow-pass one.
 * Stated rather than estimated, because "on demand" without a number
 * is how a polled path acquires one by accident later.
 */
void pkg_write_verify_json(struct json_writer *w);

int pkg_find_update_candidate(char *out_name, size_t out_name_size, char *out_image,
                               size_t out_image_size);
/* image NULL or "" means PKG_DEFAULT_IMAGE, matching pkg_install_start(). */
/*
 * Issue #144: whether this chain's in-flight job is a cache/artifact
 * hit -- i.e. its content is already on disk and there is nothing to
 * build. main.c uses it to skip creating a build container entirely.
 */
int pkg_chain_is_cache_hit(int chain_idx);

enum pkg_error pkg_get_one(const char *name, const char *image, struct json_writer *w);

/*
 * Issue #129: where a hostbuild package's harvested artifact lives, so
 * it can be exported. out_dir is ADR-0056's artifacts_dir/<name>/.
 * PKG_ERR_NOT_FOUND if there is no such hostbuild package,
 * PKG_ERR_NOT_INSTALLED if one exists but has no artifact to export.
 */
enum pkg_error pkg_hostbuild_artifact_info(const char *name, char *out_version,
                                           size_t out_version_size, char *out_dir,
                                           size_t out_dir_size);

/* Forgets a package entry in either of the two terminal states:
 * PKG_STATE_INSTALLED (unlinks every manifested file from that
 * specific (name, image) entry's own image first, producing a new
 * immutable image version -- ADR-0107/0108) or PKG_STATE_FAILED (a
 * permanently-failed fetch/build attempt that never actually merged
 * anything into any image -- cleared directly, no image version
 * produced). PKG_ERR_NOT_FOUND if unknown or still FETCHING/BUILDING;
 * PKG_ERR_BUSY if this exact (name, image) is the one currently
 * mid-build. image NULL or "" means PKG_DEFAULT_IMAGE. */
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

/* Issue #124: repoint every package row recorded against old_image to
 * new_image, persisting the result. Returns the number of rows moved,
 * or -1 if the state could not be saved. Called by image_rename(). */
int pkg_rename_image(const char *old_image, const char *new_image);

/* ---- pkg/ redesign Part 2 (ADR-0121): configurable repo + pkg sync ---- */

#define PKGREPO_URL_MAX 512
#define PKGREPO_KIND_MAX 16 /* "gitea" / "github" / "gitlab" */
#define PKGREPO_REF_MAX 128
#define PKGREPO_TOKEN_MAX 256

/* Loads any persisted repo config (or leaves the defaults: kind
 * "gitea", ref "master", no URL/token/interval configured) -- same
 * "missing file is not an error, just first-ever startup" tolerance
 * every other *_init() in this codebase already has. */
int pkg_repo_init(const char *config_path);

/* ADR-0141 Phase 4: path-only repoint -- see pkg_repoint()'s own doc comment. */
void pkg_repo_repoint(const char *new_config_path);

/* {"repo_url","repo_kind","ref","auth_token_set"} -- the token itself
 * is never echoed back (auth_token_set
 * is a bool), the one piece of secret-shaped state this daemon
 * persists that's genuinely sensitive over REST. */
void pkg_repo_write_json_config(struct json_writer *w);

/*
 * Any NULL pointer parameter leaves that field unchanged (a partial
 * PUT); passing "" for auth_token clears it explicitly (distinct from
 * NULL, which leaves whatever's already configured). repo_kind (if
 * given) must be exactly "gitea"/"github"/"gitlab" -- PKG_ERR_INVALID_
 * NAME otherwise, reusing the existing error for "not a valid
 * identifier of the expected shape" rather than adding a new one just
 * for this.
 *
 * ADR-0257: sync_interval_seconds is gone. WHEN a sync runs is a
 * schedule (`GET /v1/schedules`, action "pkg.sync"); this resource says
 * only WHERE recipes come from. Two places to look for "why did it not
 * sync" is the thing that ADR removed.
 */
enum pkg_error pkg_repo_set_config(const char *repo_url, const char *repo_kind, const char *ref,
                                    const char *auth_token);

/* Read once at init from an older config file, for the one-time
 * migration that turns it into a schedule. 0 when there was none. */
int pkg_repo_legacy_sync_interval_seconds(void);
void pkg_repo_clear_legacy_sync_interval(void);
int pkg_repo_is_configured(void);

/*
 * Starts an async fetch of the configured repo's own archive (forge-
 * specific URL/auth, see pkg.c's build_sync_fetch_request()) --
 * PKG_ERR_BUSY if a sync is already running, PKG_ERR_NOT_FOUND if no
 * repo is configured. out_pid/out_pidfd are registered with epoll by
 * the caller exactly like every other async pkg.c job (pkg_install_
 * start(), pkg_bootstrap_from_toolchain(), ...).
 */
/*
 * Issue #59: arms a one-shot re-fetch of a single name@version for the
 * next sync, so a recipe under active development can be corrected in
 * place instead of burning a new version number per iteration (the
 * catalogue really did collect five dead kernel pins and four dead gcc
 * pins from one investigation). Immutability stays the default for
 * everything else. Returns -1 if version is missing.
 */
int pkg_sync_set_refetch(const char *name, const char *version);

enum pkg_error pkg_sync_start(pid_t *out_pid, int *out_pidfd);

/*
 * Called once the curl child from pkg_sync_start() exits. A non-zero
 * exit_status is a fetch failure, recorded and nothing else happens.
 * On success: extracts the archive, walks recipes/package/<name>/
 * <version>/build.sh within it, and pkg_recipe_add()s every one -- an
 * already-published (name,version) comes back PKG_ERR_DUPLICATE and is
 * silently skipped (merge semantics: sync only ever adds, never
 * deletes or overwrites, so a locally-added-only recipe is always
 * safe). Also walks recipes/image/<name>/<version>/build.sh (ADR-0149)
 * and recipes/deployment/<name>/<version>/container.json (ADR-0151),
 * each picking the highest version per name and add()ing it via its
 * own image_recipe_add()/container_recipe_add() -- both always
 * overwrite (no version-keying at the daemon layer, ADR-0123/ADR-0151),
 * so neither ever comes back PKG_ERR_DUPLICATE the way a package
 * recipe can. Records combined added/skipped counts and any error for
 * pkg_sync_write_json_status().
 */
/*
 * ADR-0278: a sync is now three steps rather than one, so the heavy one
 * can run in a helper process instead of on the event loop (#367).
 *
 * pkg_sync_fetch_done() records the fetch's outcome and returns 1 when
 * there is an archive to unpack, 0 when the sync is already over.
 *
 * pkg_sync_extract() is the forkable half -- filesystem effects only,
 * so it is identical run in a child or inline in the parent. Returns 0,
 * -1 (could not create the extraction directory) or -2 (extraction
 * failed).
 *
 * pkg_sync_completed() takes that return and does the rest: merging
 * every recipe found, which touches in-memory state and must run on the
 * loop.
 */
int pkg_sync_fetch_done(int exit_status);
/*
 * Where a sync deposits the release-signing public keys it finds in the
 * repository's docs/keys/ (ADR-0279). Set once at startup; unset means
 * a sync adopts nothing and no artifact signature can be verified.
 */
void pkg_trusted_keys_init(const char *dir);

int pkg_sync_extract(void);
/*
 * The merge -- every recipe in the extracted tree, added to the
 * catalogue. Forkable like pkg_sync_extract() and for the same reason,
 * with one caveat that is the whole of ADR-0278's rule: it queues
 * rolling rebuilds into an in-memory queue a child cannot hand back, so
 * pkg_sync_completed() rebuilds that queue with
 * pkg_rebuild_queue_rederive() instead. Tallies travel through a file.
 */
int pkg_sync_merge(void);
void pkg_sync_completed(int exit_status);

/* {"state":"never"|"running"|"success"|"failed","last_attempt":
 * <epoch or null>,"added":N,"skipped":N,"error":<string or null>}. */
void pkg_sync_write_json_status(struct json_writer *w);

int pkg_sync_in_progress(void);

/* ---- pkg/ redesign Part 3 (ADR-0122): local build-artifact cache + LRU eviction ---- */

/* 2 GiB -- a real, always-enforced cap (no "unlimited" mode), the same
 * "bounded, not silently unbounded" posture logstore's own max_bytes
 * cap already established for this project. */
#define PKG_CACHE_DEFAULT_MAX_BYTES (2LL * 1024 * 1024 * 1024)

/* Loads any persisted cache config (default: PKG_CACHE_DEFAULT_MAX_BYTES,
 * same "missing file = first-ever startup, not an error" tolerance
 * every other *_init() here already has). cache_dir is where built-
 * package artifact tarballs are stored, one file per (name,version). */
int pkg_cache_init(const char *cache_dir, const char *config_path);

/* ADR-0141 Phase 4: path-only repoint -- see pkg_repoint()'s own doc comment. */
void pkg_cache_repoint(const char *new_cache_dir, const char *new_config_path);

long long pkg_cache_get_max_bytes(void);

/* max_bytes must be > 0 -- there is no "unlimited" mode. */
enum pkg_error pkg_cache_set_max_bytes(long long max_bytes);

/* {"max_bytes":N,"current_bytes":N,"entry_count":N}. */
void pkg_cache_write_json_status(struct json_writer *w);

/* Removes every cached artifact -- an explicit operator reset; nothing
 * else in this module ever calls this itself. */
void pkg_cache_clear(void);

/* ---- ADR-0157 Phase 3: operator-configured concurrent-build limit ---- */

/* Loads any persisted build-concurrency config (default:
 * PKG_BUILD_MAX_JOBS_DEFAULT, same "missing file = first-ever startup,
 * not an error" tolerance every other *_init() here already has). */
int pkg_build_config_init(const char *config_path);

/* ADR-0141 Phase 4: path-only repoint -- see pkg_repoint()'s own doc comment. */
void pkg_build_config_repoint(const char *new_config_path);

int pkg_build_get_max_jobs(void);

/* max_jobs must be in [1, PKG_MAX_CONCURRENT_JOBS] -- the compile-time
 * ceiling above it is a real array bound, not just a soft suggestion.
 * Lowering it below the count of chains already in flight does not
 * disrupt them -- they run to completion in their already-allocated
 * slots; the new, lower ceiling only takes effect for the *next*
 * chain-allocation decision (chain_alloc()). */
enum pkg_error pkg_build_set_max_jobs(int max_jobs);

/* ---- ADR-0165: real cgroup resource ceilings on the pkgbuild sandbox ---- */

/* Bytes; 0 means unlimited. Applied to every __pkgbuild-N container's
 * own cgroup (struct cgroup_limits.memory_max) -- the exact same
 * mechanism every regular container's own run --memory-max= already
 * uses, not a second one. */
long long pkg_build_get_memory_max(void);
enum pkg_error pkg_build_set_memory_max(long long memory_max);

/* Raw cgroup v2 cpu.max syntax ("QUOTA PERIOD" in microseconds, e.g.
 * "100000 100000" for one full CPU's worth); NULL means unlimited.
 * Same pass-through convention as run --cpu-max=, applied via the
 * identical struct cgroup_limits.cpu_max field. The returned pointer
 * is only valid until the next pkg_build_set_cpu_max()/
 * pkg_build_config_init() call -- copy it if it needs to outlive that. */
const char *pkg_build_get_cpu_max(void);
enum pkg_error pkg_build_set_cpu_max(const char *cpu_max);

/* ---- pkg/ redesign Part 3b (ADR-0122): plain-HTTP precompiled-artifact server config ---- */

#define PKGARTIFACT_URL_MAX 512
#define PKGARTIFACT_TOKEN_MAX 256

/* Loads any persisted artifact-server config (default: unconfigured --
 * every install simply builds from source, same as before this part
 * existed). */
int pkg_artifact_init(const char *config_path);

/* ADR-0141 Phase 4: path-only repoint -- see pkg_repoint()'s own doc comment. */
void pkg_artifact_repoint(const char *new_config_path);

/* {"base_url","auth_token_set"} -- the token itself is never echoed
 * back, same posture pkg_repo_write_json_config() already has. */
void pkg_artifact_write_json_config(struct json_writer *w);

/*
 * NULL leaves that field unchanged (a partial PUT, same contract
 * pkg_repo_set_config() already has); "" for auth_token explicitly
 * clears it. Deliberately no repo_kind/ref here -- this is a plain
 * HTTP location, not a git forge (see pkg.c's own module comment for
 * why binaries and git don't mix).
 */
/* NULL leaves a field unchanged (partial PUT); "" for auth_token
 * clears it. push_enabled is a pointer for the same reason -- NULL
 * means "not mentioned in this request", not "false". */
enum pkg_error pkg_artifact_set_config(const char *base_url, const char *auth_token,
                                       const int *push_enabled);

/*
 * Issue #129: publishing a freshly built package to the configured
 * artifact server, so the next host pulls it instead of rebuilding it.
 * pkg.c queues the work itself (only ever for a genuine fresh build);
 * main.c owns the event loop, so it drives the child:
 * pkg_artifact_push_try_start() returns 1 with a pid/pidfd to register,
 * and pkg_artifact_push_completed() is called when that pidfd fires.
 */
int pkg_artifact_push_try_start(pid_t *out_pid, int *out_pidfd, char *out_desc, size_t desc_size);
void pkg_artifact_push_completed(int exit_status);

/*
 * ADR-0220: the PUT an arbitrary artifact filename needs, for a
 * caller that is not the package push queue above.
 *
 * The installer ISO is the one case. It is deliberately NOT routed
 * through that queue: the queue's whole job is publishing freshly
 * built PACKAGES, keyed by name@version, and an ISO has neither -- no
 * recipe stands behind it and nothing resolves it by version, which
 * is exactly why the cache counts installers separately from packages.
 * Bending the queue to carry one would make its "what to publish"
 * logic mean two different things.
 *
 * Exposed here rather than reimplemented in main.c so the base URL's
 * trailing-slash handling and the bearer header exist in one place.
 * Returns PKG_ERR_NOT_FOUND when no cache is configured. Whether
 * pushing is ENABLED is asked separately, because the two are
 * different operator situations deserving different answers -- an
 * unconfigured cache is a setup step not yet done, a disabled push is
 * a deliberate choice already made.
 */
enum pkg_error pkg_artifact_push_request(const char *remote_name, char *out_url, size_t url_size,
                                          char *out_auth_header, size_t hdr_size);
int pkg_artifact_push_is_enabled(void);

/*
 * ---- ADR-0221 / issue #112: build-environment reclamation ----
 *
 * Composed environments are reclaimed by LAST USE rather than by
 * reachability over the recipe catalogue. Deleting one that turns out
 * to be wanted costs a recomposition, not a failure -- see the ADR for
 * why that reframing settles the whole design.
 */

/* {"buildenvs":[{name,last_used_seconds_ago,in_use,size_note}]}. */
void pkg_buildenv_write_json(struct json_writer *w);

enum pkg_error pkg_buildenv_delete(const char *name);

/*
 * Reclaims every environment idle beyond the retention window, never
 * touching one an in-flight build holds. Returns how many were removed.
 * Safe to call at any time: being wrong costs a recomposition.
 */
int pkg_buildenv_reclaim(void);

/*
 * ---- pkg/ redesign Part 4 (ADR-0123): image recipes ----
 *
 * An image recipe is the image-layer analog of a package recipe: a
 * plain text file declaring an image's intended package set --
 *   image_packages="name:mode:version name2:mode2:version2 ..."
 * -- git-syncable text, stored one file per image name (no version-
 * keying of its own; the image's own existing content-addressed
 * versioning, ADR-0108, already tracks distinct resolved states).
 *
 * Applying a recipe (pkg_image_recipe_apply_start()) is synchronous
 * bulk-declare: image_manifest_set() for every entry, exactly what N
 * manual PUT /v1/images/{name}/manifest calls would do. Packages still
 * need real pkg_install() calls afterward to actually build, same as
 * any other manifest edit.
 *
 * ADR-0209 removed the second path this used to have. A recipe that
 * was entirely "pinned" and carried an image_artifact_sha256 could be
 * applied by fetching one whole-rootfs tarball and extracting it as
 * the image's new version, skipping every per-package build. That
 * mechanism shipped one contaminated capture to every host (#168), and
 * it was a second representation of something a package set plus a
 * manifest already describes completely. Package artifacts are the
 * only published binaries now.
 */

/* name-keyed flat file (<pkg_dir>/image-recipes/<name>.recipe) --
 * called once at startup alongside pkg_init(), no separate init
 * entry point needed since it shares pkg_init()'s own pkg_dir. */
void image_recipe_init(const char *pkg_dir);

/* ADR-0141 Phase 4: path-only repoint, never resets the last-apply-
 * status fields the way image_recipe_init() does -- see pkg_repoint()'s
 * own doc comment. */
void image_recipe_repoint(const char *pkg_dir);

/* Parses content before ever writing it -- an unparseable recipe is
 * rejected outright (PKG_ERR_INVALID_RECIPE), never stored half-valid. */
enum pkg_error image_recipe_add(const char *name, const char *content);

/* Raw file content, for display/edit. Caller frees *out_content. */
enum pkg_error image_recipe_get(const char *name, char **out_content, size_t *out_len);

enum pkg_error image_recipe_rm(const char *name);

/* {"recipes":[{"name":...}, ...]}. */
void image_recipe_write_json_list(struct json_writer *w);

/*
 * Applies image's own stored recipe (PKG_ERR_NOT_FOUND if none).
 * Fully synchronous: it has completed (or failed) by the time it
 * returns, so there is no job to register and no status to poll.
 *
 * Never returns PKG_ERR_BUSY. It used to, and ADR-0270 removed that
 * check as vestigial -- see the note at the call site. Declaring a
 * manifest needs no job slot, and the manual POST .../manifest path has
 * never taken one.
 */
enum pkg_error pkg_image_recipe_apply_start(const char *image);

/*
 * Container recipes (ADR-0151): a git-syncable, reproducible template
 * for a real POST /v1/containers body -- everything an image recipe
 * deliberately doesn't cover (cmd/files/network/restart-policy/
 * sysctls). name-keyed flat file (<pkg_dir>/container-recipes/
 * <name>.recipe), content stored and applied as-is (must already be
 * the exact JSON create_container_from_body() would accept, including
 * its own "name" field matching the recipe's own name). See
 * daemon/src/pkg.c's own doc comment above container_recipe_add() for
 * the full {{SECRET:KEY}} substitution mechanism.
 */
void container_recipe_init(const char *pkg_dir);
void container_recipe_repoint(const char *pkg_dir);
enum pkg_error container_recipe_add(const char *name, const char *content);
enum pkg_error container_recipe_get(const char *name, char **out_content, size_t *out_len);
enum pkg_error container_recipe_rm(const char *name);
void container_recipe_write_json_list(struct json_writer *w);

/*
 * Name-only enumerations for the declared-vs-installed reconciliation
 * (issue #97). Each returns how many names it wrote.
 *
 * They exist because a client cannot compute this reliably: joining
 * "what recipes exist" against "what is installed" across four
 * endpoints is a join two clients would each implement and then drift
 * on, the way disk role and partition label did before #90 folded that
 * join server-side.
 */
int image_recipe_list_names(char names[][PKG_IMAGE_NAME_MAX], int max);
int container_recipe_list_names(char names[][PKG_IMAGE_NAME_MAX], int max);
int pkg_recipe_list_names(char names[][PKG_IMAGE_NAME_MAX], int max);

/*
 * Every recipe version that exists for one package (ADR-0107's
 * version-keyed layout: the directory names under recipes/<name>/ ARE
 * the versions). Returns how many were written, unordered.
 *
 * ADR-0255's source catalogue needs all of them, not just the highest:
 * the question it asks is "does a recipe exist for the release the
 * policy resolved to", and the answer can be yes while a *different*
 * line has a higher version. With 6.18.40-24 and 7.2.3-2 both present,
 * a longterm policy resolving to 6.18.46 is missing a recipe even
 * though the highest recipe on disk is numerically greater.
 */
int pkg_recipe_list_versions(const char *name, char versions[][PKG_VERSION_MAX], int max);

/*
 * The highest recipe version for `name` (pkg_version_compare()-ordered),
 * or -1 if the package has no parseable recipe. The daemon has resolved
 * this internally since ADR-0107; ADR-0255's catalogue is the first
 * caller outside this module that needs the same answer, and reaching
 * for a second scan of the same directory would be a parallel
 * implementation of a cached one.
 */
int pkg_recipe_latest_version(const char *name, char *out, size_t out_size);
/* Installed package names, de-duplicated -- the same package in three
 * images is one piece of software, not three. */
/*
 * Admission check for a new job: is one already running for this exact
 * target (hostbuild=0, name+image), or is ANY hostbuild already running
 * (hostbuild=1, name/image ignored)?  #362 and #384.
 *
 * Called from the REST handlers, NOT from pkg_install_start() -- the
 * rolling drain reads a failed start as "this image is caught up", so a
 * refusal there would silently drop a converging image from the queue.
 */
int pkg_job_in_flight_for(const char *name, const char *image, int hostbuild);

int pkg_installed_list_names(char names[][PKG_IMAGE_NAME_MAX], int max);

/*
 * Substitutes every {{SECRET:KEY}} token in the named recipe's own
 * content from secrets (an already-parsed JSON object, NULL for none)
 * and returns the result -- caller frees. NULL + *out_err set on any
 * failure (recipe not found). This is the one step between a stored
 * recipe and a real create_container_from_body() call; main.c's own
 * apply handler does exactly: container_recipe_render() -> pass the
 * result straight to handle_create()'s own body-handling path.
 */
char *container_recipe_render(const char *name, const struct json_value *secrets,
                               enum pkg_error *out_err);

#endif /* PKG_H */
