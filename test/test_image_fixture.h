#ifndef TEST_IMAGE_FIXTURE_H
#define TEST_IMAGE_FIXTURE_H

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

#endif /* TEST_IMAGE_FIXTURE_H */
