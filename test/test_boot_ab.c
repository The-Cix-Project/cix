/*
 * Phase 11 part 2 demonstrable test: proves the actual A/B rollback
 * mechanism works, not just that it was designed (docs/adr/0014). Slot A
 * boots successfully (kernel, kanxeod, everything) but is started with
 * --simulate-unhealthy-boot, so it deliberately never performs the
 * confirming loader-entry rename -- the real failure class this
 * mechanism exists for (a boot that comes up but is broken), not a
 * corrupted/unmountable root. Slot B is the already-confirmed-good
 * fallback (no counter suffix at all). Each power-on attempt launches a
 * fresh QEMU process against the same persistent disk image (this
 * project's existing poll-and-check test style, not an in-VM reboot
 * loop) so systemd-boot's own Automatic Boot Assessment counter can be
 * observed decrementing between attempts via `mtools`, with no mount
 * needed (same `disk.img@@offset` addressing test_disk_image.c already
 * uses for the ESP).
 *
 * Both slots share the exact same squashfs image -- which slot boots is
 * decided entirely by each loader entry's own init= argv, not by
 * anything different in the partition contents (see
 * image/src/mkbootroot.c / daemon/src/main.c's --slot=/
 * --simulate-unhealthy-boot).
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
#define MDIR_BIN "/usr/bin/mdir"
#define SYSTEMD_BOOT_EFI "/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.fd"

#define DISK_SIZE_BYTES (384 * 1024 * 1024) /* 64MiB ESP + two 160MiB root slots */
#define ESP_SIZE_MIB 64
#define ROOT_SLOT_SIZE_MIB 160
#define SECTOR_SIZE 512

#define BOOT_TIMEOUT_SECONDS 120
#define SUCCESS_MARKER "kanxeod listening on"
#define PANIC_MARKER "Kernel panic"
#define SLOT_A_TRIES 3
#define MAX_ATTEMPTS 5

/* Builds the ESP: systemd-boot, the kernel, a glob default (both slots
 * share the "kanxeo-" name prefix), and two loader entries -- slot A
 * with a tries-remaining counter and --simulate-unhealthy-boot, slot B
 * with no counter (already the confirmed-good fallback). */
static int build_esp_image(const char *esp_img, const char *workdir)
{
	char loader_conf_path[600];
	char entry[512];

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
	if (write_text_file(loader_conf_path, "default kanxeo-*\ntimeout 0\n") != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/loader.conf") != 0)
		return -1;

	/*
	 * sort-key groups both entries as "the same boot item across
	 * versions" per the Boot Loader Specification, so systemd-boot picks
	 * among them by the version field rather than raw filename order --
	 * a glob default alone doesn't express "try the freshly-written slot
	 * first," it just says both are valid candidates (confirmed
	 * empirically: an earlier attempt with no version field landed on
	 * slot B first, not A). Slot A (the fresh write being tried) gets
	 * the higher version.
	 */
	snprintf(entry, sizeof(entry),
	         "title Kanxeo (A)\n"
	         "sort-key kanxeo\n"
	         "version 2\n"
	         "linux /kanxeo-bzImage\n"
	         "options console=ttyS0 root=/dev/vda2 rw init=/bin/kanxeod -- "
	         "--init-mode --slot=a --simulate-unhealthy-boot\n");
	if (write_text_file(loader_conf_path, entry) != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/entries/kanxeo-a-tmp") != 0)
		return -1;

	snprintf(entry, sizeof(entry),
	         "title Kanxeo (B)\n"
	         "sort-key kanxeo\n"
	         "version 1\n"
	         "linux /kanxeo-bzImage\n"
	         "options console=ttyS0 root=/dev/vda3 rw init=/bin/kanxeod -- "
	         "--init-mode --slot=b\n");
	if (write_text_file(loader_conf_path, entry) != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/entries/kanxeo-b.conf") != 0)
		return -1;

	return 0;
}

/* Lists the ESP's loader/entries directory directly against the disk
 * image at its real partition byte offset -- `mtools`' own
 * `drive@@offset` addressing, no mount, no separate esp.img extraction
 * needed. */
static int list_loader_entries(const char *disk_img, long esp_start_sec, char *out, size_t out_size)
{
	char drive_arg[700];
	char *argv[] = { (char *)MDIR_BIN, "-i", drive_arg, "::/loader/entries", NULL };

	snprintf(drive_arg, sizeof(drive_arg), "%s@@%ld", disk_img, esp_start_sec * SECTOR_SIZE);
	return run_subprocess_capture(MDIR_BIN, argv, out, out_size);
}

int main(void)
{
	char workdir[] = "/tmp/kanxeo_test_boot_ab_XXXXXX";
	char stage_dir[600];
	char root_squashfs[600];
	char disk_img[600];
	char esp_img[600];
	char ovmf_vars[600];
	char sfdisk_dump[8192];
	char captured[65536];
	char entries_listing[4096];
	long esp_start_sec, esp_size_sec;
	long a_start_sec, a_size_sec;
	long b_start_sec, b_size_sec;
	int attempt;
	int saw_a = 0, saw_b = 0;
	int b_confirmed_before_a_exhausted = 0;
	int ok = 1;

	if (mkdtemp(workdir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(stage_dir, sizeof(stage_dir), "%s/stage", workdir);
	snprintf(root_squashfs, sizeof(root_squashfs), "%s/root.squashfs", workdir);
	snprintf(disk_img, sizeof(disk_img), "%s/disk.img", workdir);
	snprintf(esp_img, sizeof(esp_img), "%s/esp.img", workdir);
	snprintf(ovmf_vars, sizeof(ovmf_vars), "%s/OVMF_VARS.fd", workdir);

	/* 1. One root squashfs, shared by both slots -- see this file's own
	 * header comment for why. */
	{
		char *mkbootroot_argv[] = { (char *)MKBOOTROOT_BIN, stage_dir,
			                     (char *)KANXEOD_BIN, "web", root_squashfs, NULL };
		if (run_subprocess(MKBOOTROOT_BIN, mkbootroot_argv) != 0)
			return 1;
	}

	/* 2. ESP + root A + root B. */
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
		char script[300];
		char *sfdisk_argv[] = { (char *)SFDISK_BIN, disk_img, NULL };

		/* root-a needs an explicit size -- sfdisk gives a sizeless
		 * partition ALL remaining space unless it's the last one in
		 * the script, which would leave nothing for root-b. */
		snprintf(script, sizeof(script),
		         "label: gpt\n"
		         "size=%dMiB, type=uefi, name=\"ESP\"\n"
		         "size=%dMiB, type=linux, name=\"root-a\"\n"
		         "type=linux, name=\"root-b\"\n",
		         ESP_SIZE_MIB, ROOT_SLOT_SIZE_MIB);
		if (run_subprocess_stdin(SFDISK_BIN, sfdisk_argv, script) != 0)
			return 1;
	}
	{
		char *sfdisk_dump_argv[] = { (char *)SFDISK_BIN, "-d", disk_img, NULL };

		if (run_subprocess_capture(SFDISK_BIN, sfdisk_dump_argv, sfdisk_dump, sizeof(sfdisk_dump)) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 1, &esp_start_sec, &esp_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 2, &a_start_sec, &a_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 3, &b_start_sec, &b_size_sec) != 0)
			return 1;
	}

	/* 3. ESP, then both root slots -- same squashfs written to both. */
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
	/* Give slot A's entry its real tries-left-counted name -- mcopy above
	 * needed a concrete destination path, so it landed under a
	 * placeholder name; rename it to systemd-boot's own Automatic Boot
	 * Assessment convention now. */
	{
		char counted_name[64];

		snprintf(counted_name, sizeof(counted_name), "::/loader/entries/kanxeo-a+%d.conf",
		         SLOT_A_TRIES);
		if (esp_mren(esp_img, "::/loader/entries/kanxeo-a-tmp", counted_name) != 0)
			return 1;
	}
	if (write_at_offset(disk_img, esp_start_sec * SECTOR_SIZE, esp_img) != 0)
		return 1;
	if (write_at_offset(disk_img, a_start_sec * SECTOR_SIZE, root_squashfs) != 0)
		return 1;
	if (write_at_offset(disk_img, b_start_sec * SECTOR_SIZE, root_squashfs) != 0)
		return 1;

	if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, ovmf_vars) != 0)
		return 1;

	/* 4. Repeated power-on attempts against the same persistent disk. */
	for (attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
		enum qemu_boot_outcome outcome;
		int this_is_a, this_is_b;

		printf("\n===== attempt %d =====\n", attempt);
		{
			struct qemu_boot_opts opts;

			memset(&opts, 0, sizeof(opts));
			opts.disk_img = disk_img;
			opts.ovmf_vars = ovmf_vars;
			opts.success_marker = SUCCESS_MARKER;
			opts.panic_marker = PANIC_MARKER;
			opts.timeout_seconds = BOOT_TIMEOUT_SECONDS;
			outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
		}
		if (outcome != QEMU_BOOT_SUCCESS) {
			fprintf(stderr, "attempt %d: did not reach a healthy boot (outcome=%d)\n",
			        attempt, (int)outcome);
			ok = 0;
			break;
		}

		this_is_a = strstr(captured, "init-mode: slot=a") != NULL;
		this_is_b = strstr(captured, "init-mode: slot=b") != NULL;
		if (this_is_a) {
			printf("attempt %d: booted slot A (expected while its tries remain)\n", attempt);
			saw_a = 1;
		} else if (this_is_b) {
			printf("attempt %d: booted slot B (the automatic fallback)\n", attempt);
			saw_b = 1;
		} else {
			fprintf(stderr, "attempt %d: neither slot marker found in captured output\n", attempt);
			ok = 0;
			break;
		}

		if (list_loader_entries(disk_img, esp_start_sec, entries_listing, sizeof(entries_listing)) == 0)
			printf("loader/entries after attempt %d:\n%s\n", attempt, entries_listing);

		if (this_is_b) {
			/* Reached the fallback -- confirm it actually completed
			 * the real confirm-boot handshake (its entry has no
			 * counter to begin with, so "no suffix" was already
			 * true; the real proof is that B reached SUCCESS_MARKER
			 * at all, already checked above). Stop here: the
			 * rollback has been proven. */
			b_confirmed_before_a_exhausted = (attempt <= SLOT_A_TRIES);
			break;
		}
	}

	if (!ok) {
		printf("BOOT AB RESULT: FAIL\n");
		return 1;
	}
	if (!saw_a) {
		fprintf(stderr, "slot A was never observed booting at all\n");
		printf("BOOT AB RESULT: FAIL\n");
		return 1;
	}
	if (!saw_b) {
		fprintf(stderr, "slot B was never reached -- rollback did not happen within %d attempts\n",
		        MAX_ATTEMPTS);
		printf("BOOT AB RESULT: FAIL\n");
		return 1;
	}
	if (b_confirmed_before_a_exhausted) {
		fprintf(stderr, "slot B was reached before slot A's %d tries were exhausted -- "
		                "the counter isn't actually gating the fallback\n", SLOT_A_TRIES);
		printf("BOOT AB RESULT: FAIL\n");
		return 1;
	}

	rm_tree(workdir); /* only on success -- a failure's artifacts are worth keeping to debug */
	printf("BOOT AB RESULT: PASS\n");
	return 0;
}
