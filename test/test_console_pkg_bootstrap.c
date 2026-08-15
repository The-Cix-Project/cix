/*
 * Phase 20 demonstrable test: proves pkg_bootstrap_from_toolchain()
 * (ADR-0035) genuinely works on a fresh, real install -- a real
 * toolchain squashfs (built by image/src/mktoolchainimage.c, run
 * manually against this dev sandbox's own real gcc/make/etc.) attached
 * on a scratch partition, extracted via a real `unsquashfs -f -d` into
 * the pkgbuild rootfs, then confirmed by checking a real toolchain
 * binary (gcc) actually landed -- not just that the call returned OK.
 *
 * Reached via thincd's own --test-bootstrap-toolchain=, the same
 * precedent --test-update-image=/--test-update-kernel= already
 * established (test_boot_update.c): host-to-guest HTTP is unavailable
 * for a statically-addressed guest under this project's own test
 * harness, so a real device path pointing at a scratch partition is
 * how a "local file the operator already scp'd onto the box" gets
 * proven for real inside a QEMU guest at all.
 *
 * Requires /tmp/toolchain.squashfs to already exist (built once,
 * manually, via `sudo build/mktoolchainimage /tmp/toolchain_stage
 * /tmp/toolchain.squashfs` against a real toolchain-having machine --
 * not reproduced by this test itself, the same "explicit one-time
 * build-time artifact, not part of the fast default suite" posture
 * build/bzImage already has).
 */
#include "test_disk_image.h"
#include "test_image_fixture.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MKBOOTROOT_BIN "build/mkbootroot"
#define THINCD_BIN "build/thincd"
#define THINCCTL_BIN "build/thincctl"
#define BZIMAGE_PATH "build/bzImage"
#define TOOLCHAIN_SQUASHFS_PATH "/tmp/toolchain.squashfs"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define SYSTEMD_BOOT_EFI "/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.fd"

#define ESP_SIZE_MIB 64
#define ROOT_SIZE_MIB 192
#define SECTOR_SIZE 512

/* Real unsquashfs of a real, large (~2GB decompressed) artifact, on a
 * single software-emulated (TCG, no /dev/kvm) vCPU -- confirmed
 * directly to genuinely need this much margin: a run immediately
 * following several other back-to-back QEMU tests in the same session
 * (real CPU/IO contention, not a bug) timed out at 300s despite
 * multiple prior isolated runs completing comfortably within it. */
#define BOOT_TIMEOUT_SECONDS 600
#define PANIC_MARKER "Kernel panic"
#define SUCCESS_MARKER "test-bootstrap-toolchain: status=ok gcc=present"

static int build_esp_image(const char *esp_img, const char *workdir)
{
	char loader_conf_path[600];
	char loader_conf[600];

	if (esp_mkfs(esp_img) != 0)
		return -1;
	if (esp_mmd(esp_img, "::/EFI") != 0)
		return -1;
	if (esp_mmd(esp_img, "::/EFI/BOOT") != 0)
		return -1;
	if (esp_mmd(esp_img, "::/loader") != 0)
		return -1;
	if (esp_mmd(esp_img, "::/loader/entries") != 0)
		return -1;
	if (esp_mcopy_in(esp_img, SYSTEMD_BOOT_EFI, "::/EFI/BOOT/BOOTX64.EFI") != 0)
		return -1;
	if (esp_mcopy_in(esp_img, BZIMAGE_PATH, "::/thinc-bzImage-a") != 0)
		return -1;

	snprintf(loader_conf_path, sizeof(loader_conf_path), "%s/loader.conf", workdir);
	if (write_text_file(loader_conf_path, "default thinc\ntimeout 0\n") != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/loader.conf") != 0)
		return -1;

	/* --test-bootstrap-toolchain=/dev/vda3 -- the scratch partition
	 * holding the real toolchain squashfs, a real raw device path
	 * exactly what an operator's own toolchain_path would be if
	 * pointed at a whole disk rather than a regular file (the daemon
	 * validates by squashfs magic, not stat()+S_ISREG, precisely so
	 * this works -- see pkg_bootstrap_from_toolchain()). */
	snprintf(loader_conf,
	         sizeof(loader_conf),
	         "title thinC\n"
	         "linux /thinc-bzImage-a\n"
	         "options console=ttyS0 root=/dev/vda2 rw init=/bin/thincd -- --init-mode "
	         "--test-bootstrap-toolchain=/dev/vda3\n");
	if (write_text_file(loader_conf_path, loader_conf) != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/entries/thinc.conf") != 0)
		return -1;

	return 0;
}

int main(void)
{
	char workdir[] = "/tmp/thinc_test_console_pkgboot_XXXXXX";
	char stage_dir[600];
	char root_squashfs[600];
	char disk_img[600];
	char esp_img[600];
	char ovmf_vars[600];
	char sfdisk_dump[8192];
	char captured[65536];
	long esp_start_sec, esp_size_sec;
	long root_start_sec, root_size_sec;
	long scratch_start_sec, scratch_size_sec;
	long disk_size_bytes;
	long toolchain_size_bytes;
	struct stat toolchain_st;
	enum qemu_boot_outcome outcome;

	if (stat(TOOLCHAIN_SQUASHFS_PATH, &toolchain_st) != 0) {
		fprintf(stderr,
		        "SKIP: %s not found -- build it first with "
		        "`sudo build/mktoolchainimage /tmp/toolchain_stage %s`\n",
		        TOOLCHAIN_SQUASHFS_PATH, TOOLCHAIN_SQUASHFS_PATH);
		return 1;
	}
	toolchain_size_bytes = (long)toolchain_st.st_size;

	if (mkdtemp(workdir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(stage_dir, sizeof(stage_dir), "%s/stage", workdir);
	snprintf(root_squashfs, sizeof(root_squashfs), "%s/root.squashfs", workdir);
	snprintf(disk_img, sizeof(disk_img), "%s/disk.img", workdir);
	snprintf(esp_img, sizeof(esp_img), "%s/esp.img", workdir);
	snprintf(ovmf_vars, sizeof(ovmf_vars), "%s/OVMF_VARS.fd", workdir);

	{
		char *mkbootroot_argv[] = { (char *)MKBOOTROOT_BIN, stage_dir,
			                     (char *)THINCD_BIN, (char *)THINCCTL_BIN, "web", root_squashfs,
			                     "", "", "", "", NULL };
		if (run_subprocess(MKBOOTROOT_BIN, mkbootroot_argv) != 0)
			return 1;
	}

	/* ESP + root + a scratch partition sized generously past the real
	 * toolchain artifact's own byte size (checked, not assumed -- this
	 * varies by build host). */
	disk_size_bytes =
	    (long)(ESP_SIZE_MIB + ROOT_SIZE_MIB) * 1024 * 1024 + toolchain_size_bytes + 32L * 1024 * 1024;
	{
		int fd = open(disk_img, O_CREAT | O_WRONLY, 0644);

		if (fd < 0 || ftruncate(fd, disk_size_bytes) != 0) {
			perror(disk_img);
			if (fd >= 0)
				close(fd);
			return 1;
		}
		close(fd);
	}
	{
		char script[256];
		char *sfdisk_argv[] = { (char *)SFDISK_BIN, disk_img, NULL };

		snprintf(script, sizeof(script),
		         "label: gpt\n"
		         "size=%dMiB, type=uefi, name=\"ESP\"\n"
		         "size=%dMiB, type=linux, name=\"root\"\n"
		         "type=linux, name=\"toolchain-scratch\"\n",
		         ESP_SIZE_MIB, ROOT_SIZE_MIB);
		if (run_subprocess_stdin(SFDISK_BIN, sfdisk_argv, script) != 0)
			return 1;
	}
	{
		char *sfdisk_dump_argv[] = { (char *)SFDISK_BIN, "-d", disk_img, NULL };

		if (run_subprocess_capture(SFDISK_BIN, sfdisk_dump_argv, sfdisk_dump, sizeof(sfdisk_dump)) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 1, &esp_start_sec, &esp_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 2, &root_start_sec, &root_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 3, &scratch_start_sec, &scratch_size_sec) != 0)
			return 1;
	}

	{
		int fd = open(esp_img, O_CREAT | O_WRONLY, 0644);

		if (fd < 0 || ftruncate(fd, esp_size_sec * SECTOR_SIZE) != 0) {
			perror(esp_img);
			if (fd >= 0)
				close(fd);
			return 1;
		}
		close(fd);
	}
	if (build_esp_image(esp_img, workdir) != 0)
		return 1;
	if (write_at_offset(disk_img, esp_start_sec * SECTOR_SIZE, esp_img) != 0)
		return 1;

	if (write_at_offset(disk_img, root_start_sec * SECTOR_SIZE, root_squashfs) != 0)
		return 1;

	/* The real toolchain artifact, written raw at the scratch
	 * partition's own offset -- unsquashfs reads it directly off
	 * /dev/vda3 inside the guest, no mount/filesystem-on-top needed. */
	if (write_at_offset(disk_img, scratch_start_sec * SECTOR_SIZE, TOOLCHAIN_SQUASHFS_PATH) != 0)
		return 1;

	if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, ovmf_vars) != 0)
		return 1;

	{
		struct qemu_boot_opts opts;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = disk_img;
		opts.ovmf_vars = ovmf_vars;
		/* This throwaway disk has no real containers partition, so
		 * BASE_DIR falls back to tmpfs (boot_init()'s own documented,
		 * non-fatal behavior) -- unsquashfs-ing a real toolchain into
		 * tmpfs consumes guest RAM 1:1 with content size, and tmpfs's
		 * own kernel default caps it at 50% of guest RAM. Confirmed
		 * directly: this artifact's ~607MB xz-compressed squashfs
		 * decompresses to ~2.0GB on disk (`du -sh` against a local
		 * unsquashfs of it) -- comfortably past what even a 2048MB
		 * guest's 1024MB tmpfs cap can hold, hence the generous margin
		 * here. A real install has a real ext4 containers partition
		 * instead (ADR-0018) and doesn't have this constraint at all --
		 * this is a throwaway-test-topology limitation, not a real one. */
		opts.mem_mib = 6144;
		opts.success_marker = SUCCESS_MARKER;
		opts.panic_marker = PANIC_MARKER;
		opts.timeout_seconds = BOOT_TIMEOUT_SECONDS;
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
	}
	if (outcome != QEMU_BOOT_SUCCESS) {
		fprintf(stderr, "toolchain bootstrap did not succeed (outcome=%d)\n", (int)outcome);
		return 1;
	}

	rm_tree(workdir);
	printf("CONSOLE PKG BOOTSTRAP RESULT: PASS\n");
	return 0;
}
