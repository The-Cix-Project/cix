/*
 * Phase 11 part 4 demonstrable test: proves the actual installer *and*
 * its real, distributable .iso packaging both work -- given a raw,
 * blank target disk and the same 5-partition layout an operator would
 * leave behind after a real cfdisk session, kanxeo-install (booted from
 * the real .iso build/mkinstalleriso produces, via QEMU's -cdrom, not a
 * test-only disk-image approximation) partitions its role-detection,
 * formats, writes the real payload, and configures a static IP -- then
 * a *second*, completely separate boot proves the freshly-installed
 * disk actually comes up as a real, running kanxeod. "Install" and
 * "boot what was installed" are the same real code paths already proven
 * in parts 1-2, and "the ISO an operator would actually use" is now the
 * same artifact this test boots, not a separate, untested approximation
 * of it (part 3's own test built a private squashfs-based disk instead
 * -- this part replaces that with the real thing).
 *
 * Two disks, two QEMU sessions:
 *   1. build/mkinstalleriso's real .iso (attached via -cdrom) + a blank
 *      target disk, pre-partitioned by this test via sfdisk exactly the
 *      way an operator's own cfdisk session would have left it
 *      (kanxeo-install is invoked with --skip-partition, since cfdisk's
 *      curses UI can't be scripted -- proving the installer's own
 *      role-detection/format/write logic, not cfdisk itself, which is
 *      real, unmodified software this project doesn't need to
 *      re-test). Since the boot medium is now a CD-ROM (a separate
 *      ATAPI/SCSI bus), the target disk is the *only* virtio-blk device
 *      present and is /dev/vda, not /dev/vdb.
 *   2. The target disk alone, boots for real -- kanxeod --init-mode
 *      --slot=a off the partition kanxeo-install just wrote.
 *
 * Verification of what the installer actually wrote happens from the
 * host side after session 1 -- the ESP via mtools' disk.img@@offset
 * addressing (no mount needed, same pattern test_boot_ab.c already
 * established for inspecting loader-entry counter state), the config
 * partition (ext4) via debugfs (also no mount needed), and root A's raw
 * bytes read back and compared against the original squashfs.
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
#define KANXEO_INSTALL_BIN "build/kanxeo-install"
#define MKINSTALLERISO_BIN "build/mkinstalleriso"
#define BZIMAGE_PATH "build/bzImage"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define DEBUGFS_BIN "/usr/sbin/debugfs"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.fd"

#define TARGET_ESP_SIZE_MIB 64
#define TARGET_ROOT_SIZE_MIB 160
#define TARGET_CONFIG_SIZE_MIB 64
#define TARGET_DISK_SIZE_BYTES (512 * 1024 * 1024) /* ESP+root-a+root-b+config, containers gets the rest */
#define SECTOR_SIZE 512

#define INSTALL_TIMEOUT_SECONDS 180
#define BOOT_TIMEOUT_SECONDS 120
#define INSTALL_SUCCESS_MARKER "kanxeo-install: install complete"
#define BOOT_SUCCESS_MARKER "kanxeod listening on"

#define TEST_IP "192.168.50.10"
#define TEST_PREFIX 24
#define TEST_GATEWAY "192.168.50.1"

int main(void)
{
	char workdir[] = "/tmp/kanxeo_test_installer_XXXXXX";
	char stage_dir[600], control_plane_squashfs[600], installer_stage[600], installer_iso[600];
	char target_disk_img[600];
	char ovmf_vars[600];
	char sfdisk_dump[16384];
	char captured[131072];
	char kernel_args[256];
	long esp_start_sec, esp_size_sec;
	long root_a_start_sec, root_a_size_sec;
	long config_start_sec, config_size_sec;
	enum qemu_boot_outcome outcome;
	int ok = 1;

	if (mkdtemp(workdir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(stage_dir, sizeof(stage_dir), "%s/cp_stage", workdir);
	snprintf(control_plane_squashfs, sizeof(control_plane_squashfs), "%s/kanxeod-root.squashfs", workdir);
	snprintf(installer_stage, sizeof(installer_stage), "%s/installer_stage", workdir);
	snprintf(installer_iso, sizeof(installer_iso), "%s/installer.iso", workdir);
	snprintf(target_disk_img, sizeof(target_disk_img), "%s/target_disk.img", workdir);
	snprintf(ovmf_vars, sizeof(ovmf_vars), "%s/OVMF_VARS.fd", workdir);

	/* 1. The payload: kanxeod's own control-plane squashfs, unchanged
	 * from parts 1-2. */
	{
		char *mkbootroot_argv[] = { (char *)MKBOOTROOT_BIN, stage_dir, (char *)KANXEOD_BIN,
			                     control_plane_squashfs, NULL };
		if (run_subprocess(MKBOOTROOT_BIN, mkbootroot_argv) != 0)
			return 1;
	}

	/* 2. The real, distributable installer .iso -- the same tool and the
	 * same kind of artifact an operator would actually use, just with
	 * real test values plus --skip-partition standing in for a real
	 * cfdisk session and a real operator's own --disk=/--ip=/... choice
	 * at the GRUB boot-menu edit prompt. */
	snprintf(kernel_args, sizeof(kernel_args),
	         "--disk=/dev/vda --ip=%s --prefix=%d --gateway=%s --skip-partition", TEST_IP,
	         TEST_PREFIX, TEST_GATEWAY);
	{
		char *mkiso_argv[] = { (char *)MKINSTALLERISO_BIN, installer_stage,
			                (char *)KANXEO_INSTALL_BIN,    (char *)BZIMAGE_PATH,
			                control_plane_squashfs,        installer_iso,
			                kernel_args,                   NULL };
		if (run_subprocess(MKINSTALLERISO_BIN, mkiso_argv) != 0)
			return 1;
	}

	/* 3. Target disk: blank, then pre-partitioned via sfdisk exactly
	 * the way a real cfdisk session would have left it -- kanxeo-install
	 * doesn't know or care which one happened. */
	{
		int fd = open(target_disk_img, O_CREAT | O_WRONLY, 0644);

		if (fd < 0 || ftruncate(fd, TARGET_DISK_SIZE_BYTES) != 0) {
			perror(target_disk_img);
			if (fd >= 0)
				close(fd);
			return 1;
		}
		close(fd);
	}
	{
		char script[400];
		char *sfdisk_argv[] = { (char *)SFDISK_BIN, target_disk_img, NULL };

		snprintf(script, sizeof(script),
		         "label: gpt\n"
		         "size=%dMiB, type=uefi, name=\"kanxeo-esp\"\n"
		         "size=%dMiB, type=linux, name=\"kanxeo-root-a\"\n"
		         "size=%dMiB, type=linux, name=\"kanxeo-root-b\"\n"
		         "size=%dMiB, type=linux, name=\"kanxeo-config\"\n"
		         "type=linux, name=\"kanxeo-containers\"\n",
		         TARGET_ESP_SIZE_MIB, TARGET_ROOT_SIZE_MIB, TARGET_ROOT_SIZE_MIB,
		         TARGET_CONFIG_SIZE_MIB);
		if (run_subprocess_stdin(SFDISK_BIN, sfdisk_argv, script) != 0)
			return 1;
	}

	if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, ovmf_vars) != 0)
		return 1;

	/* 4. Session 1: boot the real .iso via -cdrom against the target
	 * disk. No NIC needed -- formatting/writing doesn't touch the
	 * network. A completed install ends with init exiting -- an
	 * expected, harmless "Attempted to kill init!" panic, not a
	 * failure, hence no panic_marker here. */
	{
		struct qemu_boot_opts opts;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = installer_iso;
		opts.disk_img_is_cdrom = 1;
		opts.disk_img2 = target_disk_img;
		opts.ovmf_vars = ovmf_vars;
		opts.success_marker = INSTALL_SUCCESS_MARKER;
		opts.timeout_seconds = INSTALL_TIMEOUT_SECONDS;
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
	}
	if (outcome != QEMU_BOOT_SUCCESS) {
		fprintf(stderr, "install did not report success (outcome=%d)\n", (int)outcome);
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}

	/* 5. Verify from the host side what the installer actually wrote --
	 * not just that it printed "success". */
	{
		char *dump_argv[] = { (char *)SFDISK_BIN, "-d", target_disk_img, NULL };

		if (run_subprocess_capture(SFDISK_BIN, dump_argv, sfdisk_dump, sizeof(sfdisk_dump)) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 1, &esp_start_sec, &esp_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 2, &root_a_start_sec, &root_a_size_sec) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 4, &config_start_sec, &config_size_sec) != 0)
			return 1;
	}

	{
		char drive_arg[700];
		char listing[4096];
		char *mdir_argv[] = { "/usr/bin/mdir", "-i", drive_arg, "::/loader/entries", NULL };

		snprintf(drive_arg, sizeof(drive_arg), "%s@@%ld", target_disk_img, esp_start_sec * SECTOR_SIZE);
		if (run_subprocess_capture("/usr/bin/mdir", mdir_argv, listing, sizeof(listing)) != 0 ||
		    strstr(listing, "KANXEO") == NULL) {
			fprintf(stderr, "installed ESP has no loader entry -- listing:\n%s\n", listing);
			ok = 0;
		} else {
			printf("installed ESP loader/entries:\n%s\n", listing);
		}
	}

	{
		/* Root A's raw bytes should exactly match the original
		 * control-plane squashfs -- the real proof the raw write
		 * landed correctly, not just that *some* data is there. */
		FILE *orig = fopen(control_plane_squashfs, "rb");
		FILE *installed = fopen(target_disk_img, "rb");
		char buf1[65536], buf2[65536];
		size_t n1, n2;
		int mismatch = 0;

		if (orig == NULL || installed == NULL) {
			perror("compare root-a open");
			ok = 0;
		} else {
			if (fseek(installed, root_a_start_sec * SECTOR_SIZE, SEEK_SET) != 0) {
				perror("fseek root-a");
				ok = 0;
			}
			for (;;) {
				n1 = fread(buf1, 1, sizeof(buf1), orig);
				if (n1 == 0)
					break;
				n2 = fread(buf2, 1, n1, installed);
				if (n2 != n1 || memcmp(buf1, buf2, n1) != 0) {
					mismatch = 1;
					break;
				}
			}
			if (mismatch) {
				fprintf(stderr, "root-a partition does not match the original squashfs\n");
				ok = 0;
			} else {
				printf("root-a partition bytes match the original squashfs exactly\n");
			}
		}
		if (orig != NULL)
			fclose(orig);
		if (installed != NULL)
			fclose(installed);
	}

	{
		/* Config partition (ext4) -- debugfs needs a standalone
		 * filesystem image starting at byte 0 (no offset option the
		 * way mtools' @@offset addressing has), so extract it first;
		 * still no mount anywhere in this test harness. */
		char config_extract[600];
		char net_conf[256];

		snprintf(config_extract, sizeof(config_extract), "%s/config_extract.img", workdir);
		if (extract_partition(target_disk_img, config_start_sec * SECTOR_SIZE,
		                       config_size_sec * SECTOR_SIZE, config_extract) != 0) {
			ok = 0;
		} else {
			char *debugfs_argv[] = { (char *)DEBUGFS_BIN, "-R", "cat /net.conf",
				                  config_extract, NULL };

			if (run_subprocess_capture(DEBUGFS_BIN, debugfs_argv, net_conf, sizeof(net_conf)) == 0 &&
			    strstr(net_conf, TEST_IP) != NULL && strstr(net_conf, TEST_GATEWAY) != NULL) {
				printf("config partition net.conf:\n%s\n", net_conf);
			} else {
				fprintf(stderr, "config partition net.conf missing or wrong -- got:\n%s\n", net_conf);
				ok = 0;
			}
		}
	}

	if (!ok) {
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}

	/* 6. Session 2: the actual end-to-end proof -- boot the target disk
	 * alone, for real, with a real NIC attached this time, and confirm
	 * it comes up as a genuinely working kanxeod that actually applied
	 * the static IP configured at install time. Nothing here is
	 * test-built; this is exactly what an operator would see after
	 * rebooting a freshly-installed machine. */
	{
		struct qemu_boot_opts opts;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = target_disk_img;
		opts.with_nic = 1;
		opts.ovmf_vars = ovmf_vars;
		opts.success_marker = BOOT_SUCCESS_MARKER;
		opts.panic_marker = "Kernel panic";
		opts.timeout_seconds = BOOT_TIMEOUT_SECONDS;
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
	}
	if (outcome != QEMU_BOOT_SUCCESS) {
		fprintf(stderr, "installed system did not boot successfully (outcome=%d)\n", (int)outcome);
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}
	if (strstr(captured, "applied static ip") == NULL ||
	    strstr(captured, TEST_IP) == NULL || strstr(captured, TEST_GATEWAY) == NULL) {
		fprintf(stderr, "installed system booted but never applied its static ip config\n");
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}

	rm_tree(workdir); /* only on success -- a failure's artifacts are worth keeping to debug */
	printf("INSTALLER RESULT: PASS\n");
	return 0;
}
