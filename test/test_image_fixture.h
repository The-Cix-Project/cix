#ifndef TEST_IMAGE_FIXTURE_H
#define TEST_IMAGE_FIXTURE_H

#include <stddef.h>

/*
 * Stages a minimal test image at image_root: child_binary_path (built
 * on the host, e.g. "build/daemon_child") copied to
 * <image_root>/bin/<child_basename>, plus the dynamic linker/libc it
 * needs (dynamically linked per the project's TCC-wide decision --
 * never -static), same content test_overlay.c pioneered for its own
 * lowerdir. Shared by every test that needs a real, working image --
 * child_basename lets each pick its own exec target's name (e.g.
 * "daemon_child" or "net_child") without a second, near-duplicate
 * staging function. Idempotent (safe to call repeatedly against the
 * same image_root). Returns 0, or -1 (with perror on the failing
 * path) otherwise.
 */
int test_image_fixture_build(const char *image_root, const char *child_binary_path,
                              const char *child_basename);

/*
 * Copies one additional shared library from the host into image_root
 * at the SAME absolute path it has on the host (mirroring how ld.so
 * and libc.so.6 are already placed above) -- needed for staging a
 * real third-party binary (e.g. dnsmasq) with more dependencies than
 * the minimal ld.so+libc pair every other exec target in this project
 * needs. Call once per library (the full dependency closure, e.g.
 * from `ldd`). Idempotent; returns 0 or -1 (perror on the failing
 * path).
 */
int test_image_fixture_add_lib(const char *image_root, const char *host_lib_abs_path);

/*
 * Plain byte-copy of one file to another (mode 0755 on the
 * destination), used by the two functions above and available to
 * callers that need to stage a file test_image_fixture_build()/
 * test_image_fixture_add_lib() don't cover directly (e.g.
 * test_overlay.c's own lowerdir, which predates this module and
 * originally carried its own copy). Returns 0, or -1 (with perror on
 * the failing path) otherwise.
 */
int test_image_fixture_copy_file(const char *src_path, const char *dst_path);

/*
 * Copies every regular file directly inside src_dir into dst_dir
 * (flat, not recursive), each via test_image_fixture_copy_file()
 * above. Shared by mkbootroot.c (web/'s three files, an optional
 * amdgpu firmware directory) and mkinstalleriso.c (pkg/recipes/'s
 * .recipe files, staged into the installer's own payload for
 * cix-install.c to copy onto the containers partition at install
 * time) -- one real directory-copy implementation, not two drifting
 * copies. Returns 0, or -1 (with perror on the failing path)
 * otherwise.
 */
int test_image_fixture_copy_dir_files(const char *src_dir, const char *dst_dir);

/*
 * Recursively copies src_dir's own contents into dst_dir (real `cp -a`,
 * correct symlink/permission handling a hand-rolled walker would get
 * wrong -- the same reasoning test_image_fixture_stage_toolchain()'s
 * own internal run_cp_a() already established, exposed here as a
 * public entry point for a second caller). Part 3 of the bare-metal-
 * readiness plan: mkbootroot.c uses this to stage a real kernel module
 * tree (/lib/modules/<version>/, nested by kernel/drivers/...) into the
 * control-plane squashfs -- test_image_fixture_copy_dir_files() above
 * is flat-only and cannot walk that structure. Same contract `cp -a
 * SRC DST` itself has: dst_dir's own parent must already exist, but
 * dst_dir itself must NOT -- `cp -a` creates dst_dir as an exact copy
 * of src_dir's contents only when dst_dir doesn't already exist; if it
 * does, it instead nests a copy of src_dir *inside* dst_dir, which is
 * never what a caller here wants (confirmed against this same
 * project's own test_image_fixture_stage_toolchain(), which already
 * relies on this exact contract via its own "skip if dst already
 * exists" idempotency check). Returns 0, or -1 (with a message on the
 * failing path) otherwise.
 */
int test_image_fixture_copy_dir_recursive(const char *src_dir, const char *dst_dir);

/*
 * Stages `binary_path`'s REAL shared-library closure into image_root,
 * derived from the binary itself (its ELF DT_NEEDED entries, followed
 * transitively) rather than from a list someone maintained by hand.
 * Each library is resolved by SONAME against `search_dirs` in order --
 * first match wins, the way the loader itself behaves -- and staged at
 * image_root/lib/x86_64-linux-gnu/<soname>.
 *
 * This exists because the hand-maintained lists were wrong twice, in
 * both directions, and could not have been right: ONE list cannot serve
 * two different sets of binaries. The same array named the libraries
 * for Debian's tools on a development machine and for Cix's tools on an
 * installed host, and those genuinely differ -- Debian's mkfs.btrfs
 * needs libudev.so.1 and ours does not; ours links libz.so.1 and
 * Debian's does not; Debian keeps libcrypt's obsolete DES/NIS ABI at
 * soname .so.1 while our libxcrypt drops it and ships .so.2. Every
 * correction for one host broke the other, and the failures were of the
 * worst kind: the ISO built fine and the installer died at exec time,
 * `mkfs.btrfs failed (status 0x7f00)`, inside a QEMU boot.
 *
 * `search_dirs` is NULL-terminated. Pass the host's own library
 * directories for a binary taken off this machine, or the isotools
 * artifact's for one taken out of it -- which is the point: the closure
 * follows the binary, so each set resolves against the tree it came
 * from.
 *
 * libc.so.6 and ld-linux-x86-64.so.2 are deliberately never staged
 * here: test_image_fixture_build() has already put the runtime in
 * place, and re-staging them from a different tree would overwrite a
 * working loader with one the image's other binaries were never linked
 * against.
 *
 * A DT_NEEDED entry that cannot be resolved in `search_dirs` is a hard
 * error naming the library and the binary that wanted it -- the whole
 * value here is finding that at build time instead of at install time.
 * Returns 0, or -1 with a message on stderr.
 */
int test_image_fixture_stage_closure(const char *image_root, const char *binary_path,
                                     const char *const *search_dirs);


/*
 * Stages a full package-build toolchain at image_root: this build
 * host's own /usr/{include,lib,lib64,bin,libexec} (wholesale, real
 * `cp -a` -- correct symlink/permission handling a hand-rolled copier
 * would get wrong), the bin/lib/lib64/sbin -> usr/... compatibility
 * symlinks a merged-/usr host needs, targeted extras found by an
 * actual package build failing (not guessed at -- /etc/alternatives,
 * /usr/share/{bison,autoconf,perl}, /usr/local/go), standard /dev
 * nodes, and a writable/sticky /tmp -- everything a real ./configure
 * && make && make install sequence needs, run inside an isolated,
 * network-less build container. Shared by two consumers: cixd's
 * own POST /v1/pkg/bootstrap (a live copy from whatever host cixd
 * happens to be running on -- fine for dev/test convenience, silently
 * empty on a real minimal install) and image/src/mktoolchainimage.c
 * (builds one real, portable, durable squashfs artifact from this
 * exact same content, meant to be imported via `pkg bootstrap
 * --toolchain=PATH` instead of relying on the live host at all) --
 * one source of truth for "what a toolchain needs," not two copies
 * drifting apart. Idempotent (safe to call repeatedly against the
 * same image_root, and tolerant of anything genuinely absent on this
 * particular build host -- not every host has every extra). Returns
 * 0, or -1 (with perror on the failing path) otherwise.
 */
int test_image_fixture_stage_toolchain(const char *image_root);


/*
 * Creates a fresh, unique directory under /tmp (via mkdtemp(), same
 * "/tmp/cix_test_<name>_XXXXXX" convention test_pki.c/
 * test_installer.c/test_boot_ab.c already each hand-roll on their own)
 * and writes it into out_path. Meant to be passed straight to a test's
 * own cixd subprocess as --data-dir=<out_path>, so that subprocess
 * never touches the real /var/lib/cix a live daemon on this same
 * host might be using -- every daemon-linked test's own reset_state()
 * wipes container/network/DNS/PKI state and image content
 * unconditionally, and this project's test suite wiping a real
 * deployment's state has already happened twice for lack of exactly
 * this isolation. One shared implementation rather than 16 near-
 * duplicate mkdtemp() call sites. Returns 0, or -1 (with perror on the
 * failing path) otherwise.
 */
int test_data_dir_create(char *out_path, size_t out_size);

/*
 * Recursively removes a directory created by test_data_dir_create()
 * above (shells out to `rm -rf`, same precedent test_pki.c's own
 * cleanup already uses -- this is disposable test scratch space, not
 * project-managed state, so the "never shell out" rule governing the
 * daemon's own C code does not apply here).
 */
void test_data_dir_cleanup(const char *path);

/*
 * ADR-0107/0108: an image no longer has one fixed rootfs path -- each
 * install/upgrade/delete produces a new immutable
 * <image_dir>/<version>/rootfs directory, with manifest.json's own
 * "current_version" field naming whichever one is current. Every
 * daemon-linked test that inspects an image's rootfs directly on disk
 * (rather than only through the REST API) needs this same lookup, so
 * it lives here rather than as a near-duplicate helper in each test
 * file (test_pkg.c/test_images.c). A plain text scan for the
 * "current_version":"..." field, not a real JSON parse -- this file
 * is linked into image/src/mkbootroot.c and image/src/mktoolchainimage.c
 * too (see the Makefile), neither of which links daemon/src/json.c,
 * so pulling in a real JSON parser here would ripple into their own
 * build rules for a lookup this simple. image_dir is an image's own
 * directory (e.g. "<data_dir>/images/base"), NOT a version's rootfs.
 * Returns 0, or -1 if image_dir has no manifest.json or no
 * current_version field.
 */
int test_image_fixture_read_current_version(const char *image_dir, char *out_version,
                                             size_t out_size);

/*
 * Writes a manifest.json directly into image_dir naming version as
 * both the sole version-history entry and current_version -- the same
 * document shape image.c's own image_create() produces, hand-written
 * here so a test can stage a real image's rootfs directly on disk
 * (test_image_fixture_build(), bypassing a real `pkg install` for
 * speed) while still satisfying create_container_from_body()'s own
 * image_current_version() lookup (ADR-0107/0108) -- without this, a
 * directly-staged image has no manifest.json at all and POST
 * /v1/containers 400s with "image rootfs does not exist." version can
 * be any distinct literal (these fixtures never go through pkg.c's
 * real ADR-0108 manifest-hash computation -- image.c only ever reads
 * this field back as an opaque string, never re-derives or validates
 * it). Callers build their own target rootfs path as
 * "<image_dir>/<version>/rootfs" before calling
 * test_image_fixture_build() against it, same shape
 * image_version_rootfs_path() (daemon/src/image.c) itself produces.
 * Returns 0, or -1 (with perror on the failing path) otherwise.
 */
int test_image_fixture_write_manifest(const char *image_dir, const char *version);

/*
 * The glibc version the test floor seeds, in ONE place.
 *
 * It was written out by hand in three: the floor table itself, the
 * installer test's seed check, and the rolling-restart test's install
 * poll. Moving the floor off libc-dev (#187) changed the table and left
 * the other two asserting a version that is no longer installed -- a
 * drift that reports as a package "never reaching installed" rather
 * than as a stale literal, which is a long way from the cause.
 */
#define TEST_FLOOR_GLIBC_VERSION "2.44-12"

/*
 * ADR-0209: seeds a test daemon's own package cache with REAL,
 * recipe-built package artifacts, and copies in the real recipes that
 * approve them.
 *
 * This is how a test gets a working build environment now that there
 * is no fungible sandbox and no fallback. It is deliberately the same
 * mechanism a fresh host uses, not a test-specific one: a package
 * whose tarball is already in the local cache installs as a cache hit,
 * which needs no build environment at all -- so the chicken-and-egg
 * ("you need bash to build bash") is broken by an artifact, exactly as
 * it is in production, rather than by anything invented for tests.
 *
 * What it does NOT do is fabricate a toolchain. An earlier draft of
 * this tarred up the build host's own bash and called it a package;
 * that is a binary blob wearing a package's name, and it would have
 * quietly put host content back into the one place we are trying to
 * make deterministic. These bytes are the real artifacts, built by a
 * Cix host from the real recipes, and each is checked here against the
 * pkg_artifact_sha256 committed in its own recipe before use -- if a
 * file has been tampered with or a recipe re-pinned, the test fails
 * rather than proceeding on unverified bytes.
 *
 * artifacts_dir is a directory holding <name>-<version>.tar.gz files
 * (build-inputs/floor-artifacts in this repo). data_dir is the test's own
 * --data-dir. Returns 0, or -1 with a diagnostic naming what was
 * missing or failed verification.
 */
int test_image_fixture_seed_floor_packages(const char *data_dir, const char *artifacts_dir);

/*
 * Removes the floor tarballs that test_image_fixture_seed_floor_packages()
 * put in this daemon's cache, once they have served their purpose.
 *
 * The floor exists to bootstrap a build environment; it is not part of
 * whatever a test is actually examining. Leaving ~90 MB of it behind
 * silently changes the subject for any test that reasons about cache
 * contents or size -- test_pkg_cache sets a small cap and checks LRU
 * eviction, and the floor evicted the very entries under test. Call
 * this after the floor packages are installed.
 */
int test_image_fixture_clear_floor_cache(const char *data_dir);

/*
 * How long a test may wait for a container to finish stopping or being
 * deleted, in 100ms polls. ONE definition, because eight sites across
 * seven test files were each carrying their own and they disagreed.
 *
 * ADR-0180 made stop and delete asynchronous: the response records the
 * intent, and the registry entry settles when the reactor reaps the
 * child. Usually that is a single loop turn, and every wait built on
 * this returns the instant it settles, so a healthy run costs nothing.
 *
 * THE BOUND IS 20 SECONDS BECAUSE THE PLATFORM PROMISES 15.
 * A container asked to stop within milliseconds of being created can
 * take the full SIGKILL grace: a signal sent to a pid-namespace init
 * that has not yet installed a handler is discarded rather than queued
 * (ADR-0260). How long a stop takes is therefore a property of the
 * container's AGE, not of the code under test -- which is exactly what
 * makes it look random when a test guesses lower.
 *
 * Most of these sites said 50 (five seconds), several with a comment
 * asserting that longer "is a regression". That contradicted both the
 * documented grace and test_cli.c's own neighbouring wait, which had
 * already been corrected to 20 s for precisely this reason and carried
 * the explanation. Two of the five-second ones then failed a release
 * build (#309), one after another, each looking like a fresh bug.
 *
 * This is not a timeout enlarged to quiet a race. It is a bound that
 * asserted something the design deliberately does not promise,
 * corrected to what the design actually guarantees -- and put in one
 * place so the next test cannot quietly guess again.
 */
/*
 * mkdir -p, exported because it was already the fixture's own and a
 * fifth file-local copy (four test files carry a "mkdir_p1") is the
 * duplication "No Parallel Implementations" forbids. First needed
 * outside this file by test_system_update.c, which has to create the
 * bootroot directory the assembly endpoint reports (#481).
 */
int test_mkdir_p(const char *path);

/*
 * #505: where the recipe corpus lives, now that it is a repository of
 * its own (git.home.arpa/itdlabs/cix-recipes) rather than a directory
 * in this one.
 *
 * Defaults to a sibling checkout, which is the layout a developer
 * gets by cloning both repos next to each other, and is overridable
 * with CIX_RECIPES_DIR for anything that puts them elsewhere. It is a
 * function rather than a #define so that a wrong path fails once,
 * here, with a message naming the environment variable -- rather than
 * as three separate "could not open" failures in three tests.
 *
 * Recipes are FLAT: "<root>/package/<name>@<version>.<ext>". Use
 * test_recipe_path() to build one rather than composing the name by
 * hand, so the day the separator changes there is one place to fix.
 */
const char *test_recipes_root(void);
int test_recipe_path(const char *kind, const char *name, const char *version,
                     const char *ext, char *out, size_t out_size);

#define TEST_SETTLE_ATTEMPTS 200
#define TEST_SETTLE_INTERVAL_US (100 * 1000)

#endif /* TEST_IMAGE_FIXTURE_H */
