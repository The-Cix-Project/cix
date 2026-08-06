/*
 * Phase 19 demonstrable test: proves kanxeod --init-mode's new console-
 * login machinery actually works end to end -- not just that it compiles.
 * Reuses test_boot.c's exact disk-assembly approach (same throwaway
 * ESP + one squashfs root slot), then scripts a real interactive session
 * over the serial console via qemu_boot_capture()'s existing
 * scripted_input mechanism (already proven for MOK-enrollment/fdisk
 * interaction in test_installer.c): wait for the console shell's own
 * prompt, exit it, wait for the *respawned* instance's own fresh prompt
 * (real proof the pidfd-reap + timerfd-respawn path works, not just that
 * spawn_console_shell() ran once), then confirm the respawned instance
 * actually answers a real API call.
 *
 * Deliberately scoped to the serial path (ttyS0) only -- the one
 * genuinely end-to-end testable in this sandbox. Video-console (tty0)
 * *input* needs a real keyboard (PS/2 or USB HID, see the kernel config
 * changes this same phase added) and this harness has no QMP/monitor
 * channel to synthesize a keypress even in principle (qemu_boot_capture()
 * runs with -monitor none, -display none) -- a human actually typing at
 * Proxmox's console viewer is necessarily the user's own final check,
 * the same boundary GPU passthrough/Secure Boot/the framebuffer output
 * itself already have.
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
#define KANXEOCTL_BIN "build/kanxeoctl"
#define BZIMAGE_PATH "build/bzImage"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define SYSTEMD_BOOT_EFI "/usr/lib/systemd/boot/efi/systemd-bootx64.efi"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.fd"

#define DISK_SIZE_BYTES (256 * 1024 * 1024)
#define ESP_SIZE_MIB 64
#define SECTOR_SIZE 512

/* Generous over test_boot.c's own 120s: the respawn round-trip alone
 * costs a real, deliberate 2s wait (arm_console_respawn_timer()'s own
 * fixed delay), on top of normal software-emulated boot time. */
#define BOOT_TIMEOUT_SECONDS 150
#define PANIC_MARKER "Kernel panic"
/* Confirmed via a real full boot log grep: the bare word "ok" never
 * otherwise appears -- safe as a success marker precisely because the
 * script below never triggers "health" on the *first* shell instance,
 * only the respawned one, so its first-ever appearance in the captured
 * buffer can only mean the respawned instance answered a real API call. */
#define SUCCESS_MARKER "ok"

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
	if (esp_mcopy_in(esp_img, BZIMAGE_PATH, "::/kanxeo-bzImage-a") != 0)
		return -1;

	snprintf(loader_conf_path, sizeof(loader_conf_path), "%s/loader.conf", workdir);
	if (write_text_file(loader_conf_path, "default kanxeo\ntimeout 0\n") != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/loader.conf") != 0)
		return -1;

	snprintf(loader_conf, sizeof(loader_conf),
	         "title Kanxeo\n"
	         "linux /kanxeo-bzImage-a\n"
	         "options console=ttyS0 root=/dev/vda2 rw init=/bin/kanxeod -- --init-mode\n");
	if (write_text_file(loader_conf_path, loader_conf) != 0)
		return -1;
	if (esp_mcopy_in(esp_img, loader_conf_path, "::/loader/entries/kanxeo.conf") != 0)
		return -1;

	return 0;
}

int main(void)
{
	char workdir[] = "/tmp/kanxeo_test_console_shell_XXXXXX";
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
	/* Deliberately skips "health" on the *first* shell instance -- exits
	 * it immediately, waits for the respawned instance's own genuinely
	 * new prompt (match_search_from advances past the first "kanxeo> "
	 * match, so this can only match a second, later occurrence -- real
	 * proof handle_console_shell_event()/arm_console_respawn_timer()
	 * actually respawned it), then proves that respawned instance is a
	 * fully working shell by calling health for real. */
	struct qemu_scripted_input console_script[] = {
		{ "kanxeo> ", "exit\n" },
		{ "kanxeo> ", "health\n" },
	};

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
			                     (char *)KANXEOD_BIN, (char *)KANXEOCTL_BIN, "web", root_squashfs,
			                     "", "", "", NULL };
		if (run_subprocess(MKBOOTROOT_BIN, mkbootroot_argv) != 0)
			return 1;
	}

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

	if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, ovmf_vars) != 0)
		return 1;

	{
		struct qemu_boot_opts opts;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = disk_img;
		opts.ovmf_vars = ovmf_vars;
		opts.success_marker = SUCCESS_MARKER;
		opts.panic_marker = PANIC_MARKER;
		opts.timeout_seconds = BOOT_TIMEOUT_SECONDS;
		opts.scripted_input = console_script;
		opts.n_scripted_input = sizeof(console_script) / sizeof(console_script[0]);
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
	}
	if (outcome != QEMU_BOOT_SUCCESS) {
		fprintf(stderr, "console shell round-trip did not complete (outcome=%d)\n", (int)outcome);
		return 1;
	}

	rm_tree(workdir);
	printf("CONSOLE SHELL RESULT: PASS\n");
	return 0;
}
