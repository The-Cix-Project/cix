#ifndef TEST_DISK_IMAGE_H

/*
 * The kernel every boot test boots. Defined once here because seven
 * test files each had their own copy of this literal, and because it
 * lives OUTSIDE build/ deliberately: `make clean` is `rm -rf build/`,
 * and this file is not reproducible by make (#191).
 *
 * If it is missing, test_disk_image_require_kernel() says how to get it
 * back rather than letting six boot tests fail with a bare
 * "No such file or directory" that reads like a code regression.
 */
#define BZIMAGE_PATH "build-inputs/bzImage"

/*
 * 0 if the kernel is present, -1 with an actionable message if not.
 * Call it before doing anything expensive.
 */
int test_disk_image_require_kernel(void);
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
int esp_mcopy_out(const char *esp_img, const char *esp_src_path, const char *host_dst_path);
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
 * One scripted response for qemu_boot_capture()'s interactive prompts
 * (Phase 11 part 5, ADR-0015: mokutil's password prompt, MokManager's
 * confirmation screen) -- event-driven rather than sleep-timed, since
 * boot timing genuinely varies run to run: the first time wait_for
 * appears anywhere in the output captured so far, send is written to
 * qemu's stdin (multiplexed onto the guest's ttyS0 by -serial stdio,
 * the same real mechanism an operator's own keystrokes would use).
 * Consumed strictly in array order, one match each.
 */
struct qemu_scripted_input {
	const char *wait_for;
	const char *send;
};

/*
 * qemu_boot_capture()'s options -- a struct rather than a growing list of
 * positional parameters, since by Phase 11 part 4 (a -cdrom vs. -drive
 * choice for disk_img, on top of the existing second-disk and NIC
 * options) that list had stopped being readable. Zero-initialize
 * (memset or `= {0}`) and set only what a given boot needs -- every
 * field's default (0/NULL) is the inert, part-1-era behavior: a single
 * virtio disk, no second disk, no NIC.
 */
struct qemu_boot_opts {
	const char *disk_img;      /* required */
	int disk_img_is_cdrom;     /* attach disk_img via -cdrom instead of -drive,if=virtio */
	const char *disk_img2;     /* optional second disk, always a virtio block device, or NULL */
	int with_nic;              /* attach a virtio-net device */
	int mem_mib;               /* guest RAM in MiB; 0 means the existing default (512) -- raise
	                             * this for a test whose own guest-side work sits under real
	                             * memory pressure (e.g. unsquashfs-ing a large artifact into a
	                             * tmpfs-backed /var/lib/cix on a disk with no real containers
	                             * partition -- confirmed directly: extracting a real ~600MB
	                             * toolchain squashfs into tmpfs on the 512MB default guest
	                             * exhausted memory and panicked, a real RAM constraint, not a
	                             * bug in the extraction itself). */
	int secure_boot;            /* use the .ms.fd OVMF CODE build (Phase 11 part 5) -- real
	                              * Secure Boot enforcement is actually gated by ovmf_vars's own
	                              * PK-enrollment state (confirmed empirically: plain CODE +
	                              * a User-Mode-enrolled vars file DOES enforce; .secboot CODE +
	                              * a fresh, not-yet-enrolled vars file does NOT) -- this flag's
	                              * CODE-build switch mostly documents intent/matches the user's
	                              * own reported failure conditions exactly; ovmf_vars is what
	                              * actually decides enforcement. */
	const char *direct_kernel;  /* if set, boots via QEMU's own "-kernel" fw_cfg injection
	                              * instead of firmware's normal disk-boot-scan/LoadImage path --
	                              * confirmed empirically to bypass Secure Boot's LoadImage-based
	                              * enforcement entirely, regardless of ovmf_vars's state. Used to
	                              * let the *installer's* own (deliberately unsigned, ADR-0015)
	                              * boot succeed even against a real, already-User-Mode vars file,
	                              * so the same vars file carries Microsoft's real enrolled certs
	                              * AND whatever that boot's own mokutil run stages, with no
	                              * separate vars-file-merging step needed. A pure test-harness
	                              * convenience -- real hardware has no equivalent bypass, and the
	                              * installer's own boot there genuinely does need Secure Boot off. */
	const char *direct_kernel_args; /* kernel command line for direct_kernel; required if set */
	const char *ovmf_vars;     /* required */
	const char *success_marker; /* required */
	const char *panic_marker;   /* NULL to disable panic detection (e.g. an installer run that
	                              * deliberately ends with init exiting -- a harmless, expected
	                              * "Attempted to kill init!" panic, not a failure) */
	int timeout_seconds;        /* required */
	const struct qemu_scripted_input *scripted_input; /* NULL = none */
	int n_scripted_input;
};

/*
 * Runs one QEMU power-on attempt per opts (software-emulated -- no
 * /dev/kvm in this dev LXC), streaming captured serial output live to
 * stdout while also copying it into out (truncated at out_size, always
 * NUL-terminated) so the caller can inspect the full text afterward --
 * not just the marker that stopped the wait. Stops as soon as
 * opts->success_marker or opts->panic_marker appears, the timeout
 * elapses, or qemu's output ends; always cleans up the qemu subprocess
 * (SIGTERM, SIGKILL if it doesn't exit promptly) before returning.
 */
enum qemu_boot_outcome qemu_boot_capture(const struct qemu_boot_opts *opts, char *out, size_t out_size);

#endif /* TEST_DISK_IMAGE_H */
