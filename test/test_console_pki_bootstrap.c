/*
 * Phase 20 demonstrable test: proves PKI/pkg genuinely work on a
 * fresh, real install -- not just that they compile, and not just on
 * this rich dev sandbox every other test already runs on. Closes the
 * actual blind spot that let two real bugs (docs/adr/0035) hide this
 * whole time: kanxeod shells out to /usr/bin/openssl (and curl/tar/
 * sha256sum/cp/rm) at runtime, but nothing ever staged them onto the
 * installed root, and pkg_seed_image_baseline() (then still named
 * pkg_seed_image_runtime()) read from the wrong
 * path entirely -- confirmed live, before either fix, that "pki ca
 * bootstrap" on a real console failed outright ("CA genpkey failed").
 *
 * Reuses test_console_shell.c's exact disk-assembly approach (the
 * console-login machinery it proves is what this test scripts a real
 * command over), scripting "pki ca bootstrap" and confirming the real
 * PEM certificate comes back -- not a generic "didn't crash" check.
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

#define BOOT_TIMEOUT_SECONDS 120
#define PANIC_MARKER "Kernel panic"
/* cmd_pki_ca_bootstrap's own fmt_pki_ca formatter prints the real PEM
 * certificate on success -- this exact marker only ever appears if
 * openssl actually ran to completion (binary present, shared libs
 * resolved, its own default config file readable). */
#define SUCCESS_MARKER "-----END CERTIFICATE-----"

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
	char workdir[] = "/tmp/kanxeo_test_console_pki_XXXXXX";
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
	struct qemu_scripted_input console_script[] = {
		{ "kanxeo> ", "pki ca bootstrap\n" },
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
		fprintf(stderr, "pki ca bootstrap over the console did not succeed (outcome=%d)\n",
		        (int)outcome);
		return 1;
	}

	rm_tree(workdir);
	printf("CONSOLE PKI BOOTSTRAP RESULT: PASS\n");
	return 0;
}
