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
 * kanxeo-install.c to copy onto the containers partition at install
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
 * Stages a full package-build toolchain at image_root: this build
 * host's own /usr/{include,lib,lib64,bin,libexec} (wholesale, real
 * `cp -a` -- correct symlink/permission handling a hand-rolled copier
 * would get wrong), the bin/lib/lib64/sbin -> usr/... compatibility
 * symlinks a merged-/usr host needs, targeted extras found by an
 * actual package build failing (not guessed at -- /etc/alternatives,
 * /usr/share/{bison,autoconf,perl}, /usr/local/go), standard /dev
 * nodes, and a writable/sticky /tmp -- everything a real ./configure
 * && make && make install sequence needs, run inside an isolated,
 * network-less build container. Shared by two consumers: kanxeod's
 * own POST /v1/pkg/bootstrap (a live copy from whatever host kanxeod
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
 * "/tmp/kanxeo_test_<name>_XXXXXX" convention test_pki.c/
 * test_installer.c/test_boot_ab.c already each hand-roll on their own)
 * and writes it into out_path. Meant to be passed straight to a test's
 * own kanxeod subprocess as --data-dir=<out_path>, so that subprocess
 * never touches the real /var/lib/kanxeo a live daemon on this same
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

#endif /* TEST_IMAGE_FIXTURE_H */
