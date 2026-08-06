/*
 * Phase 11 part 4/5 demonstrable test: proves the actual installer *and*
 * its real, distributable .iso packaging both work -- given a raw,
 * blank target disk and the same 5-partition layout an operator would
 * leave behind after a real, interactive fdisk session, kanxeo-install
 * (booted from the real .iso build/mkinstalleriso produces, via QEMU's
 * -cdrom, not a test-only disk-image approximation) partitions its role-
 * detection, formats, writes the real payload, and configures a static
 * IP -- then more, completely separate boots prove the freshly-installed
 * disk actually comes up as a real, running kanxeod *with Secure Boot
 * genuinely enforced* (ADR-0015). "Install" and "boot what was
 * installed" are the same real code paths already proven in parts 1-2,
 * and "the ISO an operator would actually use" is now the same artifact
 * this test boots, not a separate, untested approximation of it (part 3's
 * own test built a private squashfs-based disk instead -- this part
 * replaces that with the real thing).
 *
 * Two disks, five QEMU sessions -- two smoke tests proving the real,
 * unmodified boot/partitioning mechanisms an operator actually drives
 * (GRUB's own menu; fdisk's own interactive UI, see step 4's own comment
 * for why this replaced cfdisk), then the three-session Secure Boot flow:
 *   1. build/mkinstalleriso's real .iso + a blank target disk,
 *      partitioned by kanxeo-install itself via --auto-partition (its
 *      own scripted sfdisk, the exact same layout a real fdisk session
 *      produces -- this session's own job is proving the installer's
 *      role-detection/format/write logic, not re-proving *interactive*
 *      partitioning itself, which is step 4's job). Since the boot
 *      medium is a CD-ROM (a separate ATAPI/SCSI bus), the target disk
 *      is the *only* virtio-blk device
 *      present and is /dev/vda, not /dev/vdb. Booted via direct_kernel
 *      (QEMU's own "-kernel" injection, bypassing firmware's normal
 *      LoadImage-based Secure Boot check -- a test-harness-only
 *      convenience standing in for "Secure Boot off for this one boot"
 *      on real hardware, see ADR-0015) against the *real*, already-User-
 *      Mode ovmf_vars template, so this session's own enroll_signing_key()
 *      (mokutil --import) stages its MOK request into the exact same
 *      vars file session 2 reads.
 *   2. The target disk, Secure Boot genuinely enforced (secure_boot=1),
 *      scripted through shim's real MokManager UI to confirm the
 *      pending enrollment.
 *   3. The target disk again, Secure Boot still enforced, boots for
 *      real -- kanxeod --init-mode --slot=a off the partition
 *      kanxeo-install wrote, now via the fully-trusted shim -> signed
 *      systemd-boot -> signed kernel chain.
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
#include <sys/wait.h>
#include <unistd.h>

#define MKBOOTROOT_BIN "build/mkbootroot"
#define KANXEOD_BIN "build/kanxeod"
#define KANXEOCTL_BIN "build/kanxeoctl"
#define KANXEO_INSTALL_BIN "build/kanxeo-install"
#define MKINSTALLERISO_BIN "build/mkinstalleriso"
#define BZIMAGE_PATH "build/bzImage"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define DEBUGFS_BIN "/usr/sbin/debugfs"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.ms.fd"
#define SIGNING_KEY "image/keys/kanxeo-signing.key"
#define SIGNING_CERT_PEM "image/keys/kanxeo-signing.crt"
#define SIGNING_CERT_DER "image/keys/kanxeo-signing.cer"
#define MOK_PASSWORD "kanxeotest"

#define E2FSCK_BIN "/sbin/e2fsck"

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
/* This project's own qemu-part1.config disables predictable network
 * interface naming, so a virtio-net device always shows up as "eth0"
 * in a fresh QEMU guest (confirmed live, not assumed -- this is the
 * exact --interface= value kanxeo-install.c needs to pass through,
 * Part 0.5). */
#define TEST_IFACE "eth0"

/* A blank disk file of the standard test size -- shared by the real
 * GRUB-path smoke test and the main Secure Boot flow below, each
 * against its own disk file. Unpartitioned: every session in this file
 * now boots with --auto-partition (kanxeo-install's own scripted
 * sfdisk, image/src/kanxeo-install.c) rather than this test pre-
 * partitioning host-side -- one source of truth for the partition
 * layout, not two copies of the same sfdisk script kept in sync by
 * hand. */
static int create_blank_disk(const char *path)
{
	int fd = open(path, O_CREAT | O_WRONLY, 0644);

	if (fd < 0 || ftruncate(fd, TARGET_DISK_SIZE_BYTES) != 0) {
		perror(path);
		if (fd >= 0)
			close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/*
 * A partition extracted right after qemu_boot_capture() kills the QEMU
 * process (SIGTERM once the success marker appears, per that function's
 * own doc comment) still has a pending ext4 journal from that abrupt
 * stop -- confirmed directly ("EXT4-fs: recovery complete" on the next
 * real mount). Writing into the extracted image with debugfs (which
 * touches on-disk blocks/inodes directly, bypassing the journal
 * entirely) before that pending journal gets replayed is unsafe: the
 * next real mount's own recovery can silently revert exactly the
 * blocks debugfs just wrote, since the journal still records the
 * pre-write state. Forcing replay with e2fsck -fy first (0 = clean,
 * 1 = errors corrected -- exactly the expected "replayed a pending
 * journal" case here, not real corruption; only >= 4 is a genuine,
 * uncorrected problem) leaves the image in its true final state, safe
 * for debugfs to modify directly.
 */
static int run_e2fsck_fy(const char *path)
{
	pid_t pid;
	int status;
	char *argv[] = { (char *)E2FSCK_BIN, "-fy", (char *)path, NULL };

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(E2FSCK_BIN, argv, environ);
		perror(E2FSCK_BIN);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) >= 4) {
		fprintf(stderr, "%s -fy %s failed (status 0x%x)\n", E2FSCK_BIN, path, (unsigned)status);
		return -1;
	}
	return 0;
}

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
	long containers_start_sec, containers_size_sec;
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
	 * from parts 1-2 except now also carrying web/ (kanxeod's own
	 * DEFAULT_WEB_ROOT is a relative path, resolved against PID 1's own
	 * CWD -- never staged before, so the installed system's dashboard
	 * 404'd on every request despite the REST API working fine; found
	 * live, after a real install). */
	{
		char *mkbootroot_argv[] = { (char *)MKBOOTROOT_BIN, stage_dir, (char *)KANXEOD_BIN,
			                     (char *)KANXEOCTL_BIN, "web", control_plane_squashfs,
			                     "", /* no real GPU firmware needed for a boot test */
			                     "", /* no kernel modules needed for a boot test */
			                     "", /* no kmod tools needed for a boot test */
			                     NULL };
		if (run_subprocess(MKBOOTROOT_BIN, mkbootroot_argv) != 0)
			return 1;
	}

	/* 2. The real, distributable installer .iso -- the same tool and the
	 * same kind of artifact an operator would actually use, just with
	 * real test values plus --auto-partition standing in for a real
	 * operator's own --disk=/--ip=/... choice at the GRUB boot-menu edit
	 * prompt (--auto-partition itself -- kanxeo-install's own scripted
	 * sfdisk, added as a convenience once typing the fixed fdisk sequence
	 * by hand for every VM/scripted install proved to be pure friction --
	 * gets exercised for real right here, this session's own install). */
	snprintf(kernel_args, sizeof(kernel_args),
	         "--disk=/dev/vda --ip=%s --prefix=%d --gateway=%s --interface=%s --auto-partition", TEST_IP,
	         TEST_PREFIX, TEST_GATEWAY, TEST_IFACE);
	{
		char *mkiso_argv[] = { (char *)MKINSTALLERISO_BIN, installer_stage,
			                (char *)KANXEO_INSTALL_BIN,    (char *)BZIMAGE_PATH,
			                control_plane_squashfs,        (char *)SIGNING_KEY,
			                (char *)SIGNING_CERT_PEM,      (char *)SIGNING_CERT_DER,
			                installer_iso,                 kernel_args,
			                NULL };
		if (run_subprocess(MKINSTALLERISO_BIN, mkiso_argv) != 0)
			return 1;
	}

	/* 3. GRUB-path smoke test: boot the actual .iso the way a human
	 * operator really experiences it -- through its own grub-mkrescue-
	 * built BOOTX64.EFI and grub.cfg via a normal -cdrom attach, not the
	 * direct_kernel bypass session 1 below uses to solve a different,
	 * Secure-Boot-specific problem. Secure Boot off (a throwaway, never-
	 * enrolled vars copy), matching ADR-0015: the installer media itself
	 * is never signed. This is the ONLY place this test suite exercises
	 * the real GRUB menu/config path at all -- it's what would have
	 * caught a genuine `timeout=0` bug (the menu booted instantly, with
	 * no visible window to press 'e' and edit --disk=/--ip=/...) that
	 * shipped and only surfaced during a real operator's own install.
	 * Asserts the menu text actually renders and a countdown genuinely
	 * runs (not an instant, uninterruptible auto-boot) before the
	 * default entry boots and completes -- a throwaway disk, since this
	 * is only proving the boot *mechanism*, not re-verifying what
	 * kanxeo-install itself writes (already covered by session 1). */
	{
		char grub_smoke_disk[600], grub_smoke_vars[600];
		struct qemu_boot_opts opts;
		struct qemu_scripted_input mok_password[] = {
			{ "input password: ", MOK_PASSWORD "\n" },
			{ "input password again: ", MOK_PASSWORD "\n" },
		};

		snprintf(grub_smoke_disk, sizeof(grub_smoke_disk), "%s/grub_smoke_disk.img", workdir);
		snprintf(grub_smoke_vars, sizeof(grub_smoke_vars), "%s/grub_smoke_vars.fd", workdir);
		if (create_blank_disk(grub_smoke_disk) != 0)
			return 1;
		if (test_image_fixture_copy_file("/usr/share/OVMF/OVMF_VARS_4M.fd", grub_smoke_vars) != 0)
			return 1;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = installer_iso;
		opts.disk_img_is_cdrom = 1;
		opts.disk_img2 = grub_smoke_disk;
		opts.ovmf_vars = grub_smoke_vars;
		opts.success_marker = INSTALL_SUCCESS_MARKER;
		opts.timeout_seconds = INSTALL_TIMEOUT_SECONDS;
		opts.scripted_input = mok_password;
		opts.n_scripted_input = 2;
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
		if (outcome != QEMU_BOOT_SUCCESS || strstr(captured, "Kanxeo Install") == NULL ||
		    strstr(captured, "will be executed automatically") == NULL) {
			fprintf(stderr,
			        "real GRUB-path boot did not show a menu/countdown or complete "
			        "(outcome=%d)\n",
			        (int)outcome);
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
	}

	/* 4. fdisk-interactive smoke test: kanxeo-install's real partitioning
	 * path -- without --skip-partition, it shells out to a real,
	 * interactive fdisk (switched from cfdisk: fdisk's own ncurses-free,
	 * command-letter/line-based UI can actually be scripted via plain
	 * piped stdin, the same technique sfdisk's own script already uses;
	 * cfdisk's full-screen curses UI genuinely couldn't be, which is
	 * exactly why this path had zero coverage and shipped with a real
	 * bug -- cfdisk's terminfo database was never staged, so it failed
	 * outright with "Error opening terminal: linux." on a real install).
	 * The exact command sequence below was verified directly against a
	 * real fdisk first (not guessed): create a GPT label, five
	 * partitions sized to match this project's own layout, set
	 * partition 1's type to EFI System, then (fdisk's expert submenu)
	 * name all five to the exact GPT names kanxeo-install itself reads
	 * back -- confirmed byte-for-byte via `sfdisk -d` against the
	 * existing sfdisk-scripted layout used elsewhere in this project.
	 * Boots via direct_kernel with kernel_args built fresh here (neither
	 * --skip-partition nor --auto-partition, unlike every other session
	 * in this file, so it falls through to the default: interactive
	 * fdisk) against the same already-built installer_iso -- direct_kernel's own
	 * command line is supplied per-boot, independent of whatever's baked
	 * into the ISO's own grub.cfg, so no separate ISO build is needed. */
	{
		char fdisk_smoke_disk[600], fdisk_smoke_vars[600], fdisk_kernel_args[300];
		struct qemu_boot_opts opts;
		struct qemu_scripted_input fdisk_script[] = {
			{ "Command (m for help): ", "g\n" },
			{ "Command (m for help): ", "n\n" },
			{ "Partition number (", "1\n" },
			{ "First sector (", "\n" },
			{ "size{K,M,G,T,P} (", "+64M\n" },
			{ "Command (m for help): ", "n\n" },
			{ "Partition number (", "2\n" },
			{ "First sector (", "\n" },
			{ "size{K,M,G,T,P} (", "+160M\n" },
			{ "Command (m for help): ", "n\n" },
			{ "Partition number (", "3\n" },
			{ "First sector (", "\n" },
			{ "size{K,M,G,T,P} (", "+160M\n" },
			{ "Command (m for help): ", "n\n" },
			{ "Partition number (", "4\n" },
			{ "First sector (", "\n" },
			{ "size{K,M,G,T,P} (", "+64M\n" },
			{ "Command (m for help): ", "n\n" },
			{ "Partition number (", "5\n" },
			{ "First sector (", "\n" },
			{ "size{K,M,G,T,P} (", "\n" },
			{ "Command (m for help): ", "t\n" },
			{ "Partition number (", "1\n" },
			{ "Partition type or alias", "1\n" },
			{ "Command (m for help): ", "x\n" },
			{ "Expert command (m for help): ", "n\n" },
			{ "Partition number (", "1\n" },
			{ "New name: ", "kanxeo-esp\n" },
			{ "Expert command (m for help): ", "n\n" },
			{ "Partition number (", "2\n" },
			{ "New name: ", "kanxeo-root-a\n" },
			{ "Expert command (m for help): ", "n\n" },
			{ "Partition number (", "3\n" },
			{ "New name: ", "kanxeo-root-b\n" },
			{ "Expert command (m for help): ", "n\n" },
			{ "Partition number (", "4\n" },
			{ "New name: ", "kanxeo-config\n" },
			{ "Expert command (m for help): ", "n\n" },
			{ "Partition number (", "5\n" },
			{ "New name: ", "kanxeo-containers\n" },
			{ "Expert command (m for help): ", "r\n" },
			{ "Command (m for help): ", "w\n" },
			{ "input password: ", MOK_PASSWORD "\n" },
			{ "input password again: ", MOK_PASSWORD "\n" },
		};

		snprintf(fdisk_smoke_disk, sizeof(fdisk_smoke_disk), "%s/fdisk_smoke_disk.img", workdir);
		snprintf(fdisk_smoke_vars, sizeof(fdisk_smoke_vars), "%s/fdisk_smoke_vars.fd", workdir);
		snprintf(fdisk_kernel_args, sizeof(fdisk_kernel_args),
		         "console=ttyS0 root=/dev/sr0 rootfstype=iso9660 ro init=/bin/kanxeo-install -- "
		         "--disk=/dev/vda --ip=%s --prefix=%d --gateway=%s --interface=%s",
		         TEST_IP, TEST_PREFIX, TEST_GATEWAY, TEST_IFACE);
		{
			int fd = open(fdisk_smoke_disk, O_CREAT | O_WRONLY, 0644);

			if (fd < 0 || ftruncate(fd, TARGET_DISK_SIZE_BYTES) != 0) {
				perror(fdisk_smoke_disk);
				if (fd >= 0)
					close(fd);
				return 1;
			}
			close(fd);
		}
		if (test_image_fixture_copy_file("/usr/share/OVMF/OVMF_VARS_4M.fd", fdisk_smoke_vars) != 0)
			return 1;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = installer_iso;
		opts.disk_img_is_cdrom = 1;
		opts.disk_img2 = fdisk_smoke_disk;
		opts.direct_kernel = BZIMAGE_PATH;
		opts.direct_kernel_args = fdisk_kernel_args;
		opts.ovmf_vars = fdisk_smoke_vars;
		opts.success_marker = INSTALL_SUCCESS_MARKER;
		opts.timeout_seconds = INSTALL_TIMEOUT_SECONDS;
		opts.scripted_input = fdisk_script;
		opts.n_scripted_input = sizeof(fdisk_script) / sizeof(fdisk_script[0]);
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
		if (outcome != QEMU_BOOT_SUCCESS) {
			fprintf(stderr, "real interactive fdisk partitioning did not complete (outcome=%d)\n",
			        (int)outcome);
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
	}

	/* 5. Target disk: blank -- session 1's own --auto-partition (kernel_args
	 * above) partitions it, same as a real install would. */
	if (create_blank_disk(target_disk_img) != 0)
		return 1;

	if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, ovmf_vars) != 0)
		return 1;

	/* 6. Session 1: boot the real .iso via -cdrom against the target
	 * disk. No NIC needed -- formatting/writing doesn't touch the
	 * network. A completed install ends with init exiting -- an
	 * expected, harmless "Attempted to kill init!" panic, not a
	 * failure, hence no panic_marker here.
	 *
	 * This installer boot itself stays unsigned/Secure-Boot-off on real
	 * hardware (ADR-0015) -- ovmf_vars here is nonetheless the real
	 * User-Mode template (Microsoft's own certs already enrolled), same
	 * as MOK-confirm/final-boot below, so the pending MOK request
	 * enroll_signing_key() stages lands in the SAME vars file shim will
	 * later check -- no separate vars-file merge step needed. Booting via
	 * direct_kernel (QEMU's own "-kernel" fw_cfg injection, confirmed
	 * empirically to bypass firmware's normal LoadImage-based Secure Boot
	 * check entirely) is what makes this safe: a pure test-harness
	 * convenience standing in for what, on real hardware, is a genuine
	 * "Secure Boot off for this one boot" step -- kanxeo-install's own
	 * code runs identically either way.
	 *
	 * enroll_signing_key()'s "mokutil --import" prompts twice for a
	 * password (exact wording confirmed against the real mokutil binary)
	 * that MOK-confirm below needs again. */
	{
		struct qemu_boot_opts opts;
		struct qemu_scripted_input mok_password[] = {
			{ "input password: ", MOK_PASSWORD "\n" },
			{ "input password again: ", MOK_PASSWORD "\n" },
		};
		char direct_args[512];

		snprintf(direct_args, sizeof(direct_args),
		         "console=ttyS0 root=/dev/sr0 rootfstype=iso9660 ro init=/bin/kanxeo-install "
		         "-- %s",
		         kernel_args);

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = installer_iso;
		opts.disk_img_is_cdrom = 1;
		opts.disk_img2 = target_disk_img;
		opts.direct_kernel = BZIMAGE_PATH;
		opts.direct_kernel_args = direct_args;
		opts.ovmf_vars = ovmf_vars;
		opts.success_marker = INSTALL_SUCCESS_MARKER;
		opts.timeout_seconds = INSTALL_TIMEOUT_SECONDS;
		opts.scripted_input = mok_password;
		opts.n_scripted_input = 2;
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
	}
	if (outcome != QEMU_BOOT_SUCCESS) {
		fprintf(stderr, "install did not report success (outcome=%d)\n", (int)outcome);
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}

	/* 7. Verify from the host side what the installer actually wrote --
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
		if (sfdisk_dump_offset(sfdisk_dump, 5, &containers_start_sec, &containers_size_sec) != 0)
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

	/* 8. Session 2: MOK-confirm boot -- the target disk, for real, with
	 * Secure Boot actually enforced (secure_boot=1, same ovmf_vars as
	 * session 1, so the real Microsoft certs baked into the .ms.fd
	 * template and the pending MOK request enroll_signing_key() staged
	 * during session 1 are both present in the SAME vars file). shim
	 * (Microsoft-signed) loads cleanly; finding the pending request, it
	 * shows its own MokManager UI instead of proceeding straight to
	 * grubx64.efi (not yet trusted at this point -- confirmed separately
	 * that boot fails safely, "Security Violation", if this step is
	 * skipped). The exact menu text/navigation below was discovered by
	 * driving a real run interactively (piped scripted input, observing
	 * the raw captured output) rather than assumed: "Press any key" (a
	 * 10s countdown otherwise falls through to the same safe failure) ->
	 * "Perform MOK management" (Continue boot / Enroll MOK / ...,
	 * "Enroll MOK" one down-arrow away) -> "View key 0 / Continue" (one
	 * down-arrow to Continue) -> "Enroll the key(s)? No / Yes" (one
	 * down-arrow to Yes) -> "Password:" (the same password
	 * enroll_signing_key() used) -> back at "Perform MOK management",
	 * now offering "Reboot" as the top entry.
	 *
	 * MokManager's own reset after confirming reboots the machine; with
	 * QEMU's -no-reboot flag that means the QEMU process itself exits --
	 * there is no serial marker for "about to reset", so QEMU_BOOT_EOF
	 * (not QEMU_BOOT_SUCCESS) is this session's own expected, successful
	 * outcome, the same "this termination is expected, not a failure"
	 * posture session 1's own harmless install-complete panic already
	 * has. */
	{
		struct qemu_boot_opts opts;
		struct qemu_scripted_input mok_confirm[] = {
			{ "Press any key to perform MOK management", " " },
			{ "Enroll MOK", "\x1b[B\r" },
			{ "View key 0", "\x1b[B\r" },
			{ "Enroll the key(s)?", "\x1b[B\r" },
			{ "Password:", MOK_PASSWORD "\r" },
			{ "Reboot", "\r" },
		};

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = target_disk_img;
		opts.secure_boot = 1;
		opts.ovmf_vars = ovmf_vars;
		opts.timeout_seconds = 90;
		opts.scripted_input = mok_confirm;
		opts.n_scripted_input = 6;
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
	}
	if (outcome != QEMU_BOOT_EOF) {
		fprintf(stderr, "MOK-confirm boot did not end as expected (outcome=%d)\n", (int)outcome);
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}

	/* 9. Session 3: the actual end-to-end proof -- boot the target disk
	 * alone, for real, with Secure Boot still enforced (secure_boot=1)
	 * and a real NIC attached this time, and confirm it comes up as a
	 * genuinely working kanxeod that actually bootstrapped its "mgmt"
	 * network from the static IP/gateway/interface configured at
	 * install time (Part 0.5). Nothing here is test-built; this is
	 * exactly what an operator would see after rebooting a freshly
	 * installed, MOK-confirmed machine -- shim -> the Kanxeo-signed
	 * grubx64.efi (systemd-boot) -> the Kanxeo-signed kernel, all now
	 * trusted, zero further exceptions. */
	{
		struct qemu_boot_opts opts;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = target_disk_img;
		opts.secure_boot = 1;
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
	if (strstr(captured, "init-mode: mgmt network") == NULL || strstr(captured, TEST_IP) == NULL ||
	    strstr(captured, TEST_GATEWAY) == NULL || strstr(captured, TEST_IFACE) == NULL) {
		fprintf(stderr, "installed system booted but never bootstrapped its mgmt network\n");
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}

	/* 10. The actual persistence proof (ADR-0018): BASE_DIR
	 * (/var/lib/kanxeo) must be the real kanxeo-containers partition
	 * boot_init() now mounts, not a fresh tmpfs -- session 3's boot
	 * above already exercised ensure_dir()'s own directory creation
	 * (images/, containers/, pki/, pkg/) against whatever BASE_DIR
	 * resolved to; extracting the real on-disk partition and finding
	 * them there (not just believing the boot succeeded) is the actual
	 * proof, exactly as root-A's own byte-for-byte comparison above
	 * proves the raw write landed rather than trusting "install
	 * complete". A marker file written directly into the extracted
	 * partition (debugfs -w, no mount needed) BEFORE a second,
	 * completely independent boot, still present with the exact same
	 * content AFTER it, is the stronger proof that matters here: real
	 * data genuinely survives a real reboot, not just that kanxeod
	 * created some directories once. */
	{
		char containers_extract[600];
		char marker_src[600];
		char listing[4096];
		char marker_readback[256];
		const char *marker_content = "kanxeo-persistence-test-marker\n";

		snprintf(containers_extract, sizeof(containers_extract), "%s/containers_extract.img",
		         workdir);
		snprintf(marker_src, sizeof(marker_src), "%s/marker.txt", workdir);

		if (extract_partition(target_disk_img, containers_start_sec * SECTOR_SIZE,
		                       containers_size_sec * SECTOR_SIZE, containers_extract) != 0) {
			fprintf(stderr, "could not extract containers partition after session 3\n");
			ok = 0;
		} else if (run_e2fsck_fy(containers_extract) != 0) {
			ok = 0;
		} else {
			char *ls_argv[] = { (char *)DEBUGFS_BIN, "-R", "ls -l /", containers_extract, NULL };

			if (run_subprocess_capture(DEBUGFS_BIN, ls_argv, listing, sizeof(listing)) != 0 ||
			    strstr(listing, "images") == NULL || strstr(listing, "containers") == NULL ||
			    strstr(listing, "pki") == NULL || strstr(listing, "pkg") == NULL) {
				fprintf(stderr,
				        "containers partition missing kanxeod's own directories after a "
				        "real boot -- BASE_DIR was not actually the real partition. "
				        "listing:\n%s\n",
				        listing);
				ok = 0;
			} else {
				printf("containers partition after session 3 (real kanxeod state, not "
				       "tmpfs):\n%s\n",
				       listing);
			}
		}

		/* The "base" image's own C runtime (kanxeo-install.c's own
		 * containers-partition block, staged from mkinstalleriso.c's
		 * KANXEO_RUNTIME_DIR_SRC payload) must already be present right
		 * here, before session 4 and before any pkg install ever runs
		 * -- proving the installer itself wrote it, not something a
		 * later boot happened to create. Without this, nothing
		 * dynamically linked a package installs could ever execve()
		 * successfully (confirmed directly this session). */
		if (ok) {
			char *ls_lib64_argv[] = { (char *)DEBUGFS_BIN, "-R", "ls -l images/base/rootfs/lib64",
				                   containers_extract, NULL };
			char *ls_lib_argv[] = { (char *)DEBUGFS_BIN, "-R",
				                 "ls -l images/base/rootfs/lib/x86_64-linux-gnu",
				                 containers_extract, NULL };
			char lib64_listing[2048], lib_listing[2048];

			if (run_subprocess_capture(DEBUGFS_BIN, ls_lib64_argv, lib64_listing,
			                            sizeof(lib64_listing)) != 0 ||
			    strstr(lib64_listing, "ld-linux-x86-64.so.2") == NULL) {
				fprintf(stderr,
				        "containers partition missing the seeded ld.so after install -- "
				        "listing:\n%s\n",
				        lib64_listing);
				ok = 0;
			} else if (run_subprocess_capture(DEBUGFS_BIN, ls_lib_argv, lib_listing,
			                                   sizeof(lib_listing)) != 0 ||
			           strstr(lib_listing, "libc.so.6") == NULL ||
			           strstr(lib_listing, "libtinfo.so.6") == NULL) {
				fprintf(stderr,
				        "containers partition missing seeded libc.so.6/libtinfo.so.6 after "
				        "install -- listing:\n%s\n",
				        lib_listing);
				ok = 0;
			} else {
				printf("base image runtime (ld.so/libc.so.6/libtinfo.so.6) seeded at install "
				       "time:\n%s%s\n",
				       lib64_listing, lib_listing);
			}
		}

		if (ok && write_text_file(marker_src, marker_content) != 0) {
			fprintf(stderr, "could not write local marker file\n");
			ok = 0;
		}
		if (ok) {
			char *write_argv[] = { (char *)DEBUGFS_BIN, "-w", "-R", NULL, containers_extract,
				                NULL };
			char write_cmd[700];

			snprintf(write_cmd, sizeof(write_cmd), "write %s images/PERSISTENCE_MARKER",
			         marker_src);
			write_argv[3] = write_cmd;
			if (run_subprocess(DEBUGFS_BIN, write_argv) != 0) {
				fprintf(stderr, "could not inject marker file into containers partition\n");
				ok = 0;
			}
		}
		if (ok && write_at_offset(target_disk_img, containers_start_sec * SECTOR_SIZE,
		                           containers_extract) != 0) {
			fprintf(stderr, "could not write modified containers partition back to disk\n");
			ok = 0;
		}

		if (!ok) {
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}

		/* Session 4: the same target disk, a completely independent
		 * QEMU process/boot, Secure Boot still enforced (the same
		 * ovmf_vars, already MOK-confirmed by session 2). A NIC is
		 * still required even though this check has nothing to do with
		 * networking: the ESP's own loader entry (written by session
		 * 1's install) bakes in --bind=<the configured static IP> on
		 * kanxeod's kernel command line unconditionally, on every boot
		 * of this disk -- omitting the NIC here means that address is
		 * never actually assigned to any interface, so kanxeod's own
		 * bind() fails and PID 1 exits, panicking the kernel (found
		 * directly by first omitting it here). */
		{
			struct qemu_boot_opts opts;

			memset(&opts, 0, sizeof(opts));
			opts.disk_img = target_disk_img;
			opts.secure_boot = 1;
			opts.with_nic = 1;
			opts.ovmf_vars = ovmf_vars;
			opts.success_marker = BOOT_SUCCESS_MARKER;
			opts.panic_marker = "Kernel panic";
			opts.timeout_seconds = BOOT_TIMEOUT_SECONDS;
			outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
		}
		if (outcome != QEMU_BOOT_SUCCESS) {
			fprintf(stderr, "second, independent boot of the same disk did not succeed "
			                "(outcome=%d)\n",
			        (int)outcome);
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}

		if (extract_partition(target_disk_img, containers_start_sec * SECTOR_SIZE,
		                       containers_size_sec * SECTOR_SIZE, containers_extract) != 0) {
			fprintf(stderr, "could not extract containers partition after session 4\n");
			ok = 0;
		} else {
			char *cat_argv[] = { (char *)DEBUGFS_BIN, "-R", "cat images/PERSISTENCE_MARKER",
				              containers_extract, NULL };

			if (run_subprocess_capture(DEBUGFS_BIN, cat_argv, marker_readback,
			                            sizeof(marker_readback)) != 0 ||
			    strcmp(marker_readback, marker_content) != 0) {
				fprintf(stderr,
				        "marker file did not survive a real, independent second boot -- "
				        "got %s (%zu bytes), want %s (%zu bytes)\n",
				        marker_readback, strlen(marker_readback), marker_content,
				        strlen(marker_content));
				ok = 0;
			} else {
				printf("marker file survived a real, independent second boot unchanged -- "
				       "BASE_DIR genuinely persists across reboots\n");
			}
		}
	}

	if (!ok) {
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}

	rm_tree(workdir); /* only on success -- a failure's artifacts are worth keeping to debug */
	printf("INSTALLER RESULT: PASS\n");
	return 0;
}
