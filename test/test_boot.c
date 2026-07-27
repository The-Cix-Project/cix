/*
 * Phase 11 part 1 demonstrable test: proves the actual boot chain works --
 * kernel -> systemd-boot -> kanxeod --init-mode -- not just that each piece
 * builds. Assembles a throwaway 2-partition disk (ESP + one squashfs root
 * slot; the real 5-partition A/B layout is part 3's installer output, not
 * this test's job), boots it in QEMU (software-emulated -- no /dev/kvm in
 * this dev LXC), and scrapes the serial console for the exact same
 * "kanxeod listening on ..." line every other test in this suite already
 * treats as daemon-ready (daemon/src/main.c) -- proof this is the real
 * startup sequence running for real, not a special-cased boot-mode stub.
 *
 * Disk/partition/ESP/QEMU-boot primitives live in test_disk_image.c,
 * shared with test_boot_ab.c (part 2) -- see that file's own header
 * comment for why (no loop devices in this dev LXC, etc.).
 */
#include "test_disk_image.h"
#include "test_image_fixture.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MKBOOTROOT_BIN "build/mkbootroot"
#define KANXEOD_BIN "build/kanxeod"
#define BZIMAGE_PATH "build/bzImage"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define SYSTEMD_BOOT_EFI "/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.fd"

#define DISK_SIZE_BYTES (256 * 1024 * 1024) /* 64MB ESP + ~192MB root -- root.squashfs measures well under 1MB */
#define ESP_SIZE_MIB 64
#define SECTOR_SIZE 512

#define BOOT_TIMEOUT_SECONDS 120
#define SUCCESS_MARKER "kanxeod listening on"
#define PANIC_MARKER "Kernel panic"

/* Builds the ESP as a standalone FAT32 file via mtools -- systemd-boot
 * itself, the kernel, and a loader entry pointing init at
 * kanxeod --init-mode on the raw root partition. */
static int build_esp_image(const char *esp_img, const char *workdir)
{
	char loader_conf_path[600];
	char loader_conf[512];

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
	if (esp_mcopy_in(esp_img, BZIMAGE_PATH, "::/kanxeo-bzImage") != 0)
		return -1;

	snprintf(loader_conf_path, sizeof(loader_conf_path), "%s/loader.conf", workdir);
	if (write_text_file(loader_conf_path, "default kanxeo\ntimeout 0\n") != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/loader.conf") != 0)
		return -1;

	snprintf(loader_conf, sizeof(loader_conf),
	         "title Kanxeo\n"
	         "linux /kanxeo-bzImage\n"
	         "options console=ttyS0 root=/dev/vda2 rw init=/bin/kanxeod -- --init-mode\n");
	if (write_text_file(loader_conf_path, loader_conf) != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/entries/kanxeo.conf") != 0)
		return -1;

	return 0;
}

int main(void)
{
	char workdir[] = "/tmp/kanxeo_test_boot_XXXXXX";
	char stage_dir[600];
	char root_squashfs[600];
	char disk_img[600];
	char esp_img[600];
	char ovmf_vars[600];
	char sfdisk_dump[8192];
	char captured[65536];
	long esp_start_sec, esp_size_sec;
	long root_start_sec, root_size_sec;
	enum qemu_boot_outcome outcome;

	if (mkdtemp(workdir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(stage_dir, sizeof(stage_dir), "%s/stage", workdir);
	snprintf(root_squashfs, sizeof(root_squashfs), "%s/root.squashfs", workdir);
	snprintf(disk_img, sizeof(disk_img), "%s/disk.img", workdir);
	snprintf(esp_img, sizeof(esp_img), "%s/esp.img", workdir);
	snprintf(ovmf_vars, sizeof(ovmf_vars), "%s/OVMF_VARS.fd", workdir);

	/* 1. Build the minimal control-plane root (build/mkbootroot already
	 * reuses test_image_fixture_build() for the kanxeod+ld.so+libc
	 * staging -- not reimplemented here). */
	{
		char *mkbootroot_argv[] = { (char *)MKBOOTROOT_BIN, stage_dir,
			                     (char *)KANXEOD_BIN, root_squashfs, NULL };
		if (run_subprocess(MKBOOTROOT_BIN, mkbootroot_argv) != 0)
			return 1;
	}

	/* 2. Throwaway raw disk: GPT, one ESP + one root partition. Part 1's
	 * own scope boundary -- the real 5-partition A/B layout is part 3's
	 * installer output, not this test's job. sfdisk operates on the
	 * plain file directly, no loop device needed. */
	{
		int fd = open(disk_img, O_CREAT | O_WRONLY, 0644);

		if (fd < 0 || ftruncate(fd, DISK_SIZE_BYTES) != 0) {
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
		         "type=linux, name=\"root\"\n",
		         ESP_SIZE_MIB);
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
	}

	/* 3. ESP, built standalone via mtools, then dropped into the disk
	 * image at its real partition offset. */
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

	/* 4. Root partition: the whole squashfs image, written raw at its
	 * partition offset -- a single atomic block copy, exactly how
	 * ADR-0014 describes every future A/B slot write working too. */
	if (write_at_offset(disk_img, root_start_sec * SECTOR_SIZE, root_squashfs) != 0)
		return 1;

	/* 5. Boot it. Software-emulated (no /dev/kvm in this dev LXC) --
	 * correct but slow, acceptable for a boot-correctness smoke test. */
	if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, ovmf_vars) != 0)
		return 1;

	outcome = qemu_boot_capture(disk_img, NULL, 0, ovmf_vars, SUCCESS_MARKER, PANIC_MARKER,
	                             BOOT_TIMEOUT_SECONDS, captured, sizeof(captured));
	if (outcome != QEMU_BOOT_SUCCESS) {
		fprintf(stderr, "boot did not reach a healthy state (outcome=%d)\n", (int)outcome);
		return 1;
	}

	rm_tree(workdir); /* only on success -- a failure's artifacts are worth keeping to debug */
	printf("BOOT RESULT: PASS\n");
	return 0;
}
