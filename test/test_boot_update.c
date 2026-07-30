/*
 * Phase 16 part 1 (ADR-0031) demonstrable test: proves POST /v1/system/
 * update's real device-write/loader-entry logic (do_system_update(),
 * daemon/src/main.c) against a real virtio-blk disk inside a real QEMU
 * guest -- the only place ROOT_A_DEVICE/ROOT_B_DEVICE (/dev/vda2,
 * /dev/vda3) actually exist. Reached via kanxeod's own
 * --test-update-image= self-test flag (same --simulate-unhealthy-boot
 * precedent), since host-to-guest HTTP is unavailable for a
 * statically-addressed guest under this project's own test harness
 * (see CLAUDE.md's SLIRP note) -- there is no other way to drive a
 * live POST /v1/system/update into a booted guest from this harness.
 *
 * Layout, deliberately a superset of test_boot_ab.c's own minimal one:
 * ESP + root-a + root-b + a fourth "root-c" scratch partition, test-
 * harness-only (not part of the real 5-partition installer layout --
 * see image/src/kanxeo-install.c's auto_partition()), pre-loaded with
 * a second, genuinely different-content squashfs image_path can point
 * at as a raw device path (write_file_to_device()'s open/read/write
 * loop works identically against a whole block device or a regular
 * file -- there is no meaningful difference to that code between the
 * two). Slot A boots normally, self-updates onto slot B (root-c's
 * content -> root-b's device) during that same boot, then a second,
 * fresh QEMU power-on against the same persistent disk (this project's
 * existing poll-and-check style, not an in-VM reboot) proves the
 * freshly-written slot B actually boots -- the real payoff, not just
 * that bytes landed somewhere.
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
#define KANXEOD_BIN "build/kanxeod"
#define BZIMAGE_PATH "build/bzImage"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define MDIR_BIN "/usr/bin/mdir"
#define SYSTEMD_BOOT_EFI "/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.fd"

#define DISK_SIZE_BYTES (544 * 1024 * 1024) /* 64MiB ESP + three 160MiB slots */
#define ESP_SIZE_MIB 64
#define ROOT_SLOT_SIZE_MIB 160
#define SECTOR_SIZE 512

#define BOOT_TIMEOUT_SECONDS 120
/* The full expected success line, not just a prefix of it -- qemu_boot_
 * capture() stops the instant its marker substring has fully arrived,
 * so a marker that's only a prefix (e.g. "test-update: status=") stops
 * the capture (and kills qemu) before the actual status/slot digits
 * that follow it have arrived, truncating exactly the information this
 * test needs to check. */
#define UPDATE_MARKER "test-update: status=200 slot=b"
#define SUCCESS_MARKER "kanxeod listening on"
#define PANIC_MARKER "Kernel panic"
#define ROOT_UPDATE_TRIES 3

static int build_esp_image(const char *esp_img, const char *workdir)
{
	char loader_conf_path[600];
	char entry[600];

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

	/* Slot A's only entry -- a plain, already-confirmed boot (no tries
	 * counter), low version (1). --test-update-image=/dev/vda4 points
	 * at the scratch partition holding the "new" squashfs -- real
	 * raw device path, exactly what an operator's own image_path would
	 * be if pointed at a whole disk rather than a regular file. */
	snprintf(entry, sizeof(entry),
	         "title Kanxeo (A)\n"
	         "sort-key kanxeo\n"
	         "version 1\n"
	         "linux /kanxeo-bzImage\n"
	         "options console=ttyS0 root=/dev/vda2 rw init=/bin/kanxeod -- "
	         "--init-mode --slot=a --bind=127.0.0.1 --test-update-image=/dev/vda4\n");
	if (write_text_file(loader_conf_path, entry) != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/entries/kanxeo-a.conf") != 0)
		return -1;

	return 0;
}

static int list_loader_entries(const char *disk_img, long esp_start_sec, char *out, size_t out_size)
{
	char drive_arg[700];
	char *argv[] = { (char *)MDIR_BIN, "-i", drive_arg, "::/loader/entries", NULL };

	snprintf(drive_arg, sizeof(drive_arg), "%s@@%ld", disk_img, esp_start_sec * SECTOR_SIZE);
	return run_subprocess_capture(MDIR_BIN, argv, out, out_size);
}

/* Builds a second web/ directory with the same three real dashboard
 * files plus one extra marker file -- enough to make the resulting
 * squashfs genuinely different bytes from root A's own image (both
 * built from the identical kanxeod binary otherwise), without needing
 * a second, unrelated binary. */
static int stage_alt_web_dir(const char *dir)
{
	char dst[512];
	FILE *f;

	if (ensure_dir(dir) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/index.html", dir);
	if (test_image_fixture_copy_file("web/index.html", dst) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/app.js", dir);
	if (test_image_fixture_copy_file("web/app.js", dst) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/style.css", dir);
	if (test_image_fixture_copy_file("web/style.css", dst) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/UPDATE_MARKER", dir);
	f = fopen(dst, "w");
	if (f == NULL)
		return -1;
	fputs("phase16-update-test\n", f);
	fclose(f);
	return 0;
}

/* First n bytes of a and b are byte-identical. */
static int files_prefix_equal(const char *a_path, const char *b_path, long n)
{
	FILE *fa, *fb;
	int equal = 1;
	long remaining = n;

	fa = fopen(a_path, "rb");
	fb = fopen(b_path, "rb");
	if (fa == NULL || fb == NULL) {
		if (fa != NULL)
			fclose(fa);
		if (fb != NULL)
			fclose(fb);
		return 0;
	}
	while (remaining > 0) {
		char bufa[65536], bufb[65536];
		size_t chunk = remaining < (long)sizeof(bufa) ? (size_t)remaining : sizeof(bufa);
		size_t na = fread(bufa, 1, chunk, fa);
		size_t nb = fread(bufb, 1, chunk, fb);

		if (na != chunk || nb != chunk || memcmp(bufa, bufb, chunk) != 0) {
			equal = 0;
			break;
		}
		remaining -= (long)chunk;
	}
	fclose(fa);
	fclose(fb);
	return equal;
}

int main(void)
{
	char workdir[] = "/tmp/kanxeo_test_boot_update_XXXXXX";
	char stage_dir[600], stage_dir_new[600], web_new_dir[600];
	char root_squashfs_a[600], root_squashfs_new[600];
	char disk_img[600];
	char esp_img[600];
	char ovmf_vars[600];
	char sfdisk_dump[8192];
	char captured[65536];
	char entries_listing[4096];
	long esp_start_sec, esp_size_sec;
	long a_start_sec, a_size_sec;
	long b_start_sec, b_size_sec;
	long c_start_sec, c_size_sec;
	struct stat new_squashfs_st;
	char extracted_root_b[600];
	int ok = 1;

	if (mkdtemp(workdir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(stage_dir, sizeof(stage_dir), "%s/stage_a", workdir);
	snprintf(stage_dir_new, sizeof(stage_dir_new), "%s/stage_new", workdir);
	snprintf(web_new_dir, sizeof(web_new_dir), "%s/web_new", workdir);
	snprintf(root_squashfs_a, sizeof(root_squashfs_a), "%s/root-a.squashfs", workdir);
	snprintf(root_squashfs_new, sizeof(root_squashfs_new), "%s/root-new.squashfs", workdir);
	snprintf(disk_img, sizeof(disk_img), "%s/disk.img", workdir);
	snprintf(esp_img, sizeof(esp_img), "%s/esp.img", workdir);
	snprintf(ovmf_vars, sizeof(ovmf_vars), "%s/OVMF_VARS.fd", workdir);
	snprintf(extracted_root_b, sizeof(extracted_root_b), "%s/extracted-root-b", workdir);

	/* 1. Two genuinely different-content squashfs images: slot A's own
	 * (built from the real web/ dir) and the "update" payload (built
	 * from web_new/, with one extra marker file). */
	{
		char *argv_a[] = { (char *)MKBOOTROOT_BIN, stage_dir,
			           (char *)KANXEOD_BIN, "web", root_squashfs_a,
			           "", NULL };
		if (run_subprocess(MKBOOTROOT_BIN, argv_a) != 0)
			return 1;
	}
	if (stage_alt_web_dir(web_new_dir) != 0) {
		fprintf(stderr, "FAIL: could not stage alternate web dir\n");
		return 1;
	}
	{
		char *argv_new[] = { (char *)MKBOOTROOT_BIN, stage_dir_new,
			             (char *)KANXEOD_BIN, web_new_dir, root_squashfs_new,
			             "", NULL };
		if (run_subprocess(MKBOOTROOT_BIN, argv_new) != 0)
			return 1;
	}
	if (stat(root_squashfs_new, &new_squashfs_st) != 0) {
		perror(root_squashfs_new);
		return 1;
	}

	/* 2. ESP + root-a + root-b + root-c (scratch, holds the "new" image). */
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
		char script[400];
		char *sfdisk_argv[] = { (char *)SFDISK_BIN, disk_img, NULL };

		snprintf(script, sizeof(script),
		         "label: gpt\n"
		         "size=%dMiB, type=uefi, name=\"ESP\"\n"
		         "size=%dMiB, type=linux, name=\"root-a\"\n"
		         "size=%dMiB, type=linux, name=\"root-b\"\n"
		         "type=linux, name=\"root-c\"\n",
		         ESP_SIZE_MIB, ROOT_SLOT_SIZE_MIB, ROOT_SLOT_SIZE_MIB);
		if (run_subprocess_stdin(SFDISK_BIN, sfdisk_argv, script) != 0)
			return 1;
	}
	{
		char *sfdisk_dump_argv[] = { (char *)SFDISK_BIN, "-d", disk_img, NULL };

		if (run_subprocess_capture(SFDISK_BIN, sfdisk_dump_argv, sfdisk_dump,
		                            sizeof(sfdisk_dump)) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 1, &esp_start_sec, &esp_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 2, &a_start_sec, &a_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 3, &b_start_sec, &b_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 4, &c_start_sec, &c_size_sec) != 0)
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
	if (write_at_offset(disk_img, a_start_sec * SECTOR_SIZE, root_squashfs_a) != 0)
		return 1;
	/* root-b deliberately left untouched (all zero) -- exactly root B's
	 * real state before the very first update ever writes there. */
	if (write_at_offset(disk_img, c_start_sec * SECTOR_SIZE, root_squashfs_new) != 0)
		return 1;

	if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, ovmf_vars) != 0)
		return 1;

	/* 3. Attempt 1: slot A boots, self-updates onto slot B mid-boot via
	 * --test-update-image=/dev/vda4 (do_system_update()'s real device
	 * write + loader-entry write), stops as soon as that's observable. */
	{
		struct qemu_boot_opts opts;
		enum qemu_boot_outcome outcome;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = disk_img;
		opts.ovmf_vars = ovmf_vars;
		opts.success_marker = UPDATE_MARKER;
		opts.panic_marker = PANIC_MARKER;
		opts.timeout_seconds = BOOT_TIMEOUT_SECONDS;
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));

		if (outcome != QEMU_BOOT_SUCCESS) {
			fprintf(stderr, "attempt 1: did not reach the update marker (outcome=%d)\n",
			        (int)outcome);
			ok = 0;
		} else if (strstr(captured, "test-update: status=200 slot=b") == NULL) {
			fprintf(stderr,
			        "attempt 1: expected \"test-update: status=200 slot=b\", captured "
			        "output:\n%s\n",
			        captured);
			ok = 0;
		} else {
			printf("attempt 1: self-update to slot B reported success\n");
		}
	}
	if (!ok) {
		printf("BOOT UPDATE RESULT: FAIL\n");
		return 1;
	}

	/* 4. The fresh loader entry now exists on the ESP, with a real
	 * Automatic Boot Assessment tries-left counter, matching the very
	 * first install's own convention. */
	if (list_loader_entries(disk_img, esp_start_sec, entries_listing, sizeof(entries_listing)) !=
	    0) {
		fprintf(stderr, "FAIL: could not list loader/entries after attempt 1\n");
		ok = 0;
	} else {
		printf("loader/entries after attempt 1:\n%s\n", entries_listing);
		/* mdir may display an 8.3-ineligible long name in either case
		 * depending on locale -- match case-insensitively on the
		 * distinctive "b+3" substring rather than depending on mdir's
		 * own display casing. */
		if (strstr(entries_listing, "B+3") == NULL && strstr(entries_listing, "b+3") == NULL) {
			fprintf(stderr, "FAIL: no fresh kanxeo-b+%d.conf entry found on the ESP\n",
			        ROOT_UPDATE_TRIES);
			ok = 0;
		}
	}

	/* 5. Root B's on-disk bytes are byte-exact with the "new" image --
	 * not just that some write happened, that the RIGHT bytes landed
	 * in the RIGHT place. */
	if (extract_partition(disk_img, b_start_sec * SECTOR_SIZE, new_squashfs_st.st_size,
	                       extracted_root_b) != 0) {
		fprintf(stderr, "FAIL: could not extract root-b partition content\n");
		ok = 0;
	} else if (!files_prefix_equal(extracted_root_b, root_squashfs_new, new_squashfs_st.st_size)) {
		fprintf(stderr, "FAIL: root-b on-disk content does not match the update image\n");
		ok = 0;
	} else {
		printf("root-b on-disk content matches the update image byte-for-byte (%ld bytes)\n",
		       (long)new_squashfs_st.st_size);
	}

	/* 6. Attempt 2: a fresh power-on against the same persistent disk --
	 * the real payoff. The freshly-written slot B entry's version
	 * (a UNIX timestamp) sorts above slot A's own (version 1), so it
	 * should now boot by default, and it must actually work: a real
	 * kernel + kanxeod booting from the image this test itself wrote
	 * moments ago via the daemon's own code, not pre-baked onto the
	 * disk by the test harness. */
	if (ok) {
		struct qemu_boot_opts opts;
		enum qemu_boot_outcome outcome;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = disk_img;
		opts.ovmf_vars = ovmf_vars;
		opts.success_marker = SUCCESS_MARKER;
		opts.panic_marker = PANIC_MARKER;
		opts.timeout_seconds = BOOT_TIMEOUT_SECONDS;
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));

		if (outcome != QEMU_BOOT_SUCCESS) {
			fprintf(stderr, "attempt 2: did not reach a healthy boot (outcome=%d)\n",
			        (int)outcome);
			ok = 0;
		} else if (strstr(captured, "init-mode: slot=b") == NULL) {
			fprintf(stderr,
			        "attempt 2: expected to boot slot B (the freshly-updated slot), "
			        "captured output:\n%s\n",
			        captured);
			ok = 0;
		} else {
			printf("attempt 2: booted slot B successfully -- the update actually works\n");
		}
	}

	if (!ok) {
		printf("BOOT UPDATE RESULT: FAIL\n");
		return 1;
	}

	rm_tree(workdir); /* only on success -- a failure's artifacts are worth keeping to debug */
	printf("BOOT UPDATE RESULT: PASS\n");
	return 0;
}
