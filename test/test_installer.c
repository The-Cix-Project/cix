/*
 * Phase 11 part 3 demonstrable test: proves the actual installer works --
 * given a raw, blank target disk and the same 5-partition layout an
 * operator would leave behind after a real cfdisk session, kanxeo-install
 * partitions its role-detection, formats, writes the real payload, and
 * configures a static IP -- then a *second*, completely separate boot
 * proves the freshly-installed disk actually comes up as a real, running
 * kanxeod. "Install" and "boot what was installed" are the same real
 * code paths already proven in parts 1-2, not a separate, untested
 * assumption.
 *
 * Two disks, two QEMU sessions:
 *   1. Boot disk (ESP + kanxeo-install's own bootable squashfs, carrying
 *      the real payload it installs -- the control-plane squashfs,
 *      kernel, and systemd-boot -- bundled inside it) + a blank target
 *      disk, pre-partitioned by this test via sfdisk exactly the way an
 *      operator's own cfdisk session would have left it (kanxeo-install
 *      is invoked with --skip-partition, since cfdisk's curses UI can't
 *      be scripted -- proving the installer's own role-detection/format/
 *      write logic, not cfdisk itself, which is real, unmodified
 *      software this project doesn't need to re-test).
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
#define BZIMAGE_PATH "build/bzImage"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define DEBUGFS_BIN "/usr/sbin/debugfs"
#define SYSTEMD_BOOT_EFI "/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.fd"

#define BOOT_ESP_SIZE_MIB 64
#define BOOT_DISK_SIZE_BYTES (256 * 1024 * 1024)

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

/* The real tools kanxeo-install shells out to, and their full ldd
 * closures (checked directly against this host) -- staged the same way
 * test_image_fixture_add_lib() already stages dnsmasq's own closure
 * (Phase 8), just for a different set of real, unmodified binaries. */
static const char *const g_lib_closure[] = {
	"/lib/x86_64-linux-gnu/libsmartcols.so.1", "/lib/x86_64-linux-gnu/libfdisk.so.1",
	"/lib/x86_64-linux-gnu/libmount.so.1",     "/lib/x86_64-linux-gnu/libncursesw.so.6",
	"/lib/x86_64-linux-gnu/libtinfo.so.6",     "/lib/x86_64-linux-gnu/libuuid.so.1",
	"/lib/x86_64-linux-gnu/libblkid.so.1",     "/lib/x86_64-linux-gnu/libselinux.so.1",
	"/lib/x86_64-linux-gnu/libpcre2-8.so.0",   "/lib/x86_64-linux-gnu/libreadline.so.8",
	"/lib/x86_64-linux-gnu/libext2fs.so.2",    "/lib/x86_64-linux-gnu/libcom_err.so.2",
	"/lib/x86_64-linux-gnu/libe2p.so.2",       NULL,
};

static int ensure_dir_under(const char *root, const char *rel)
{
	char path[600];

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	return ensure_dir(path);
}

/* Builds kanxeo-install's own bootable environment: the binary itself,
 * every real tool it shells out to (plus their full dependency
 * closures), and the install payload bundled at /payload/... */
static int build_installer_image(const char *stage_dir, const char *control_plane_squashfs,
                                  const char *out_squashfs)
{
	int i;
	char dst[600];

	if (ensure_dir(stage_dir) != 0)
		return -1;
	if (test_image_fixture_build(stage_dir, KANXEO_INSTALL_BIN, "kanxeo-install") != 0)
		return -1;

	if (ensure_dir_under(stage_dir, "usr") != 0)
		return -1;
	if (ensure_dir_under(stage_dir, "usr/sbin") != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/usr/sbin/cfdisk", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/cfdisk", dst) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/usr/sbin/sfdisk", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/sfdisk", dst) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/usr/sbin/mkfs.vfat", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/mkfs.fat", dst) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/usr/sbin/mkfs.ext4", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/mke2fs", dst) != 0)
		return -1;

	for (i = 0; g_lib_closure[i] != NULL; i++) {
		if (test_image_fixture_add_lib(stage_dir, g_lib_closure[i]) != 0)
			return -1;
	}

	if (ensure_dir_under(stage_dir, "payload") != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/payload/systemd-bootx64.efi", stage_dir);
	if (test_image_fixture_copy_file(SYSTEMD_BOOT_EFI, dst) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/payload/kanxeo-bzImage", stage_dir);
	if (test_image_fixture_copy_file(BZIMAGE_PATH, dst) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/payload/kanxeo-root.squashfs", stage_dir);
	if (test_image_fixture_copy_file(control_plane_squashfs, dst) != 0)
		return -1;

	/* Same PID-1 boot shape kanxeod's own image needs (build/mkbootroot)
	 * -- early_mounts() (image/src/kanxeo-install.c) mounts proc/sysfs
	 * here before anything else runs; devtmpfs auto-populates /dev on
	 * its own once the mountpoint exists (CONFIG_DEVTMPFS_MOUNT). */
	if (ensure_dir_under(stage_dir, "proc") != 0)
		return -1;
	if (ensure_dir_under(stage_dir, "sys") != 0)
		return -1;
	if (ensure_dir_under(stage_dir, "dev") != 0)
		return -1;

	if (ensure_dir_under(stage_dir, "mnt") != 0)
		return -1;
	if (ensure_dir_under(stage_dir, "mnt/esp") != 0)
		return -1;
	if (ensure_dir_under(stage_dir, "mnt/config") != 0)
		return -1;
	if (ensure_dir_under(stage_dir, "mnt/containers") != 0)
		return -1;

	return build_squashfs(stage_dir, out_squashfs);
}

/* The boot disk's own ESP -- systemd-boot, kernel, one loader entry
 * whose init= drives kanxeo-install against the target disk
 * (--skip-partition: this test pre-partitions the target itself via
 * sfdisk, standing in for what a real cfdisk session would have left --
 * cfdisk's own curses UI can't be scripted, and it's real, unmodified
 * software this project doesn't need to re-verify). */
static int build_boot_esp(const char *esp_img, const char *workdir)
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
	if (write_text_file(loader_conf_path, "default kanxeo\ntimeout 0\n") != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/loader.conf") != 0)
		return -1;

	snprintf(entry, sizeof(entry),
	         "title Kanxeo Install\n"
	         "linux /kanxeo-bzImage\n"
	         "options console=ttyS0 root=/dev/vda2 rw init=/bin/kanxeo-install -- "
	         "--disk=/dev/vdb --ip=%s --prefix=%d --gateway=%s --skip-partition\n",
	         TEST_IP, TEST_PREFIX, TEST_GATEWAY);
	if (write_text_file(loader_conf_path, entry) != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/entries/kanxeo.conf") != 0)
		return -1;

	return 0;
}

/* The target disk's ESP for the *second*, real boot -- a plain, one-entry
 * loader.conf/entries setup, identical in shape to test_boot.c's part 1
 * disk, just booting off whatever kanxeo-install actually wrote instead
 * of a test-built one. Not used until after verifying session 1's
 * output -- see main()'s own second scenario. */

int main(void)
{
	char workdir[] = "/tmp/kanxeo_test_installer_XXXXXX";
	char stage_dir[600], control_plane_squashfs[600], installer_stage[600], installer_squashfs[600];
	char boot_disk_img[600], boot_esp_img[600];
	char target_disk_img[600];
	char ovmf_vars[600];
	char sfdisk_dump[16384];
	char captured[131072];
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
	snprintf(installer_squashfs, sizeof(installer_squashfs), "%s/installer.squashfs", workdir);
	snprintf(boot_disk_img, sizeof(boot_disk_img), "%s/boot_disk.img", workdir);
	snprintf(boot_esp_img, sizeof(boot_esp_img), "%s/boot_esp.img", workdir);
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

	/* 2. kanxeo-install's own bootable environment, carrying that
	 * payload bundled inside it. */
	if (build_installer_image(installer_stage, control_plane_squashfs, installer_squashfs) != 0)
		return 1;

	/* 3. Boot disk: ESP + the installer squashfs as its one root
	 * partition -- same 2-partition shape test_boot.c already proved. */
	{
		int fd = open(boot_disk_img, O_CREAT | O_WRONLY, 0644);

		if (fd < 0 || ftruncate(fd, BOOT_DISK_SIZE_BYTES) != 0) {
			perror(boot_disk_img);
			if (fd >= 0)
				close(fd);
			return 1;
		}
		close(fd);
	}
	{
		char script[256];
		char *sfdisk_argv[] = { (char *)SFDISK_BIN, boot_disk_img, NULL };
		long boot_esp_start, boot_esp_size, boot_root_start, boot_root_size;
		char *dump_argv[] = { (char *)SFDISK_BIN, "-d", boot_disk_img, NULL };

		snprintf(script, sizeof(script),
		         "label: gpt\n"
		         "size=%dMiB, type=uefi, name=\"ESP\"\n"
		         "type=linux, name=\"root\"\n",
		         BOOT_ESP_SIZE_MIB);
		if (run_subprocess_stdin(SFDISK_BIN, sfdisk_argv, script) != 0)
			return 1;
		if (run_subprocess_capture(SFDISK_BIN, dump_argv, sfdisk_dump, sizeof(sfdisk_dump)) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 1, &boot_esp_start, &boot_esp_size) != 0)
			return 1;
		if (sfdisk_dump_offset(sfdisk_dump, 2, &boot_root_start, &boot_root_size) != 0)
			return 1;

		{
			int fd = open(boot_esp_img, O_CREAT | O_WRONLY, 0644);

			if (fd < 0 || ftruncate(fd, boot_esp_size * SECTOR_SIZE) != 0) {
				perror(boot_esp_img);
				if (fd >= 0)
					close(fd);
				return 1;
			}
			close(fd);
		}
		if (build_boot_esp(boot_esp_img, workdir) != 0)
			return 1;
		if (write_at_offset(boot_disk_img, boot_esp_start * SECTOR_SIZE, boot_esp_img) != 0)
			return 1;
		if (write_at_offset(boot_disk_img, boot_root_start * SECTOR_SIZE, installer_squashfs) != 0)
			return 1;
	}

	/* 4. Target disk: blank, then pre-partitioned via sfdisk exactly
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

	/* 5. Session 1: boot the installer against both disks. No NIC needed
	 * -- formatting/writing doesn't touch the network. */
	outcome = qemu_boot_capture(boot_disk_img, target_disk_img, 0, ovmf_vars, INSTALL_SUCCESS_MARKER,
	                             NULL /* a completed install ends with init exiting -- an expected,
	                                   * harmless "Attempted to kill init!" panic, not a failure */,
	                             INSTALL_TIMEOUT_SECONDS, captured, sizeof(captured));
	if (outcome != QEMU_BOOT_SUCCESS) {
		fprintf(stderr, "install did not report success (outcome=%d)\n", (int)outcome);
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}

	/* 6. Verify from the host side what the installer actually wrote --
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

	/* 7. Session 2: the actual end-to-end proof -- boot the target disk
	 * alone, for real, with a real NIC attached this time, and confirm
	 * it comes up as a genuinely working kanxeod that actually applied
	 * the static IP configured at install time. Nothing here is
	 * test-built; this is exactly what an operator would see after
	 * rebooting a freshly-installed machine. */
	outcome = qemu_boot_capture(target_disk_img, NULL, 1, ovmf_vars, BOOT_SUCCESS_MARKER,
	                             "Kernel panic", BOOT_TIMEOUT_SECONDS, captured, sizeof(captured));
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
