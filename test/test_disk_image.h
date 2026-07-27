#ifndef TEST_DISK_IMAGE_H
#define TEST_DISK_IMAGE_H

#include <stddef.h>

/*
 * Low-level disk/partition/ESP/QEMU-boot primitives shared by every
 * Phase 11 boot test (test_boot.c, test_boot_ab.c, ...) -- extracted from
 * test_boot.c once test_boot_ab.c became a second real consumer of the
 * exact same logic, the same "extract when a genuine second consumer
 * appears" moment test_image_fixture.c's own origin already established
 * for this project. None of this decides what a specific test is trying
 * to prove -- that stays in each test's own main().
 *
 * No loop devices, no mount(): this dev LXC exposes no /dev/loop* at all
 * (confirmed empirically during Phase 11 part 1). Partitioning works
 * directly on the disk image file (sfdisk operates on a plain file fine);
 * the ESP is built as a standalone FAT32 file populated with `mtools`
 * (a real userspace FAT implementation, no mount needed); pieces are then
 * written into the disk image at their exact partition byte offsets with
 * a plain seek+write.
 */

int run_subprocess(const char *bin, char *const argv[]);

/* Like run_subprocess(), but captures stdout -- used for `sfdisk -d`. */
int run_subprocess_capture(const char *bin, char *const argv[], char *out, size_t out_size);

/* Like run_subprocess(), but feeds `script` to the child's stdin -- used
 * for `sfdisk`, which reads its partition table from stdin. */
int run_subprocess_stdin(const char *bin, char *const argv[], const char *script);

int ensure_dir(const char *path);
int write_text_file(const char *path, const char *content);

/* Recursively removes path (same nftw()-based pattern test_overlay.c
 * already established) -- every boot test's own mkdtemp() workdir holds
 * a disk image and squashfs build large enough (tens to hundreds of MB)
 * that leaving it behind on every run silently fills the disk. */
void rm_tree(const char *path);

/* Finds the Nth (1-based) "start=<sectors>, size=<sectors>" pair in
 * `sfdisk -d`'s dump output -- one per partition, in partition order. */
int sfdisk_dump_offset(const char *dump, int partition_index, long *out_start_sectors,
                        long *out_size_sectors);

/* Copies the whole content of src_path into dst_path starting at
 * offset_bytes -- used both to drop a built ESP image and a root
 * squashfs image into their real partition offsets inside a disk image. */
int write_at_offset(const char *dst_path, long offset_bytes, const char *src_path);

/* The reverse of write_at_offset() -- copies size_bytes starting at
 * offset_bytes out of src_path into a fresh standalone file at
 * out_path. Used to inspect a partition with a tool that needs a
 * filesystem image starting at byte 0 (e.g. debugfs, which has no
 * offset option the way mtools' @@offset addressing does). */
int extract_partition(const char *src_path, long offset_bytes, long size_bytes,
                       const char *out_path);

/* Thin `mtools` wrappers -- a real userspace FAT implementation operating
 * on esp_img directly, no mount() anywhere. */
int esp_mkfs(const char *esp_img);
int esp_mmd(const char *esp_img, const char *esp_dir_path);
int esp_mcopy_in(const char *esp_img, const char *host_src_path, const char *esp_dest_path);
int esp_mren(const char *esp_img, const char *esp_old_path, const char *esp_new_path);

/* Squashes image_root into out_path via the real mksquashfs -- used by
 * any test that needs to build a bootable squashfs image beyond
 * build/mkbootroot's own control-plane one (e.g. test_installer.c's
 * installer environment). */
int build_squashfs(const char *image_root, const char *out_path);

enum qemu_boot_outcome {
	QEMU_BOOT_SUCCESS, /* success_marker appeared in the captured output */
	QEMU_BOOT_PANIC,   /* panic_marker appeared in the captured output */
	QEMU_BOOT_TIMEOUT, /* neither appeared before timeout_seconds elapsed */
	QEMU_BOOT_EOF,     /* qemu's output ended before either marker appeared */
	QEMU_BOOT_ERROR,   /* harness-level failure (fork/pipe/exec), not a boot outcome */
};

/*
 * Runs one QEMU power-on attempt against disk_img (OVMF firmware +
 * ovmf_vars, software-emulated -- no /dev/kvm in this dev LXC), plus a
 * second disk_img2 if non-NULL (e.g. test_installer.c's target disk,
 * attached alongside its own boot/installer disk) and a virtio-net
 * device if with_nic is set (e.g. test_installer.c's second boot
 * session, proving the static-IP-application path actually has a real
 * interface to apply to) -- streaming captured
 * serial output live to stdout while also copying it into out (truncated
 * at out_size, always NUL-terminated) so the caller can inspect the full
 * text afterward -- not just the marker that stopped the wait. Stops as
 * soon as success_marker or panic_marker (either may be NULL to disable
 * that check -- e.g. an installer run that deliberately ends with init
 * exiting, a harmless, expected "Kernel panic - Attempted to kill init!"
 * that isn't a real failure) appears, the timeout elapses, or qemu's
 * output ends; always cleans up the qemu subprocess (SIGTERM, SIGKILL if
 * it doesn't exit promptly) before returning.
 */
enum qemu_boot_outcome qemu_boot_capture(const char *disk_img, const char *disk_img2, int with_nic,
                                          const char *ovmf_vars, const char *success_marker,
                                          const char *panic_marker, int timeout_seconds, char *out,
                                          size_t out_size);

#endif /* TEST_DISK_IMAGE_H */
