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

#endif /* TEST_IMAGE_FIXTURE_H */
