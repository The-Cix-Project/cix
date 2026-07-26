#ifndef TEST_IMAGE_FIXTURE_H
#define TEST_IMAGE_FIXTURE_H

/*
 * Stages a minimal test image at image_root: child_binary_path (built
 * on the host, e.g. "build/daemon_child") copied to <image_root>/bin/
 * daemon_child, plus the dynamic linker/libc it needs (dynamically
 * linked per the project's TCC-wide decision -- never -static), same
 * content test_overlay.c pioneered for its own lowerdir. Shared by
 * test_daemon.c and test_cli.c so image staging exists in one place.
 * Idempotent (safe to call repeatedly against the same image_root).
 * Returns 0, or -1 (with perror on the failing path) otherwise.
 */
int test_image_fixture_build(const char *image_root, const char *child_binary_path);

#endif /* TEST_IMAGE_FIXTURE_H */
