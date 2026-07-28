/*
 * Assembles kanxeo-install's own bootable installer environment and
 * packages it as a real, distributable UEFI-bootable .iso -- the
 * concrete artifact an operator (or test_installer.c's own QEMU -cdrom
 * verification) boots to install Kanxeo onto a real disk.
 *
 * The installer media boots via GRUB (grub-mkrescue -- the standard
 * tool every major distro uses for exactly this; hand-tuning xorriso's
 * raw El Torito/GPT-hybrid flags directly proved genuinely fiddly and
 * kept failing during this phase's own empirical spike, the same
 * "real software, not hand-rolled" posture as every other external
 * tool this project shells out to), and the kernel then mounts the
 * ISO9660 media itself directly as root (root=/dev/sr0
 * rootfstype=iso9660 -- no initramfs, no squashfs, the same posture
 * every other root filesystem in this phase already has). This is a
 * one-time, install-media-only bootloader role -- distinct from, and
 * not a reopening of, systemd-boot's own deliberate role on the
 * *installed* target disk (ADR-0014's counted A/B mechanism), which
 * kanxeo-install itself still sets up exactly as proven in part 3.
 *
 * Dev-machine-only build tooling (like build/mkbootroot), never shipped
 * or run on a target -- reuses test_image_fixture_build()/_add_lib()/
 * _copy_file() (test/test_image_fixture.c) for staging, the same
 * precedent mkbootroot.c already established (unlike kanxeo-install.c
 * itself, which deliberately never links test/, since it runs on a
 * real target disk as production code).
 */
#include "test_image_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define GRUB_MKRESCUE_BIN "/usr/bin/grub-mkrescue"
#define SBSIGN_BIN "/usr/bin/sbsign"
#define SYSTEMD_BOOT_EFI "/usr/lib/systemd/boot/efi/systemd-bootx64.efi"

/* Secure Boot chain for the *target* disk's ESP (kanxeo-install.c's own
 * populate_esp() writes these three under EFI/BOOT/ -- see ADR-0015).
 * Both are pre-signed by Debian directly; no re-signing needed. */
#define SHIM_EFI_SRC "/usr/lib/shim/shimx64.efi.signed"
#define MOKMANAGER_EFI_SRC "/usr/lib/shim/mmx64.efi.signed"

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
	"/lib/x86_64-linux-gnu/libe2p.so.2",
	/* mokutil's own closure (ldd-checked against this host), for
	 * enroll_signing_key()'s "mokutil --import" call in kanxeo-install.c. */
	"/lib/x86_64-linux-gnu/libcrypto.so.3",    "/lib/x86_64-linux-gnu/libefivar.so.1",
	"/lib/x86_64-linux-gnu/libkeyutils.so.1",  "/lib/x86_64-linux-gnu/libcrypt.so.1",
	"/lib/x86_64-linux-gnu/libdl.so.2",        NULL,
};

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		perror(path);
		return -1;
	}
	return 0;
}

static int ensure_dir_under(const char *root, const char *rel)
{
	char path[600];

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	return ensure_dir(path);
}

static int write_text_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		perror(path);
		return -1;
	}
	if (fputs(content, f) < 0) {
		perror(path);
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int run_subprocess(const char *bin, char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(bin, argv, environ);
		perror(bin);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s failed (status 0x%x)\n", bin, (unsigned)status);
		return -1;
	}
	return 0;
}

static int sbsign_to(const char *key, const char *cert, const char *src, const char *dst)
{
	char *argv_sbsign[] = { (char *)SBSIGN_BIN, "--key",  (char *)key, "--cert", (char *)cert,
		                 "--output", (char *)dst, (char *)src,  NULL };

	return run_subprocess(SBSIGN_BIN, argv_sbsign);
}

int main(int argc, char **argv)
{
	const char *stage_dir;
	const char *kanxeo_install_bin;
	const char *bzimage_path;
	const char *control_plane_squashfs;
	const char *signing_key;
	const char *signing_cert_pem;
	const char *signing_cert_der;
	const char *out_iso;
	const char *kernel_args;
	char dst[600];
	char grub_cfg_path[600];
	char grub_cfg[1024];
	int i;

	if (argc != 10) {
		fprintf(stderr,
		        "usage: %s <staging-dir> <kanxeo-install-bin> <bzImage> "
		        "<control-plane-squashfs> <signing-key> <signing-cert.crt> "
		        "<signing-cert.cer> <out.iso> <kernel-args>\n"
		        "  signing-key/signing-cert.crt/signing-cert.cer: the Kanxeo Secure Boot\n"
		        "  signing key pair (image/keys/kanxeo-signing.{key,crt,cer} -- .crt is\n"
		        "  PEM, for sbsign; .cer is DER, for mokutil) -- used to sign systemd-boot\n"
		        "  and the kernel for the *target* disk's ESP; the installer media's own\n"
		        "  GRUB boot stays unsigned (see ADR-0015).\n"
		        "  kernel-args: everything after 'init=/bin/kanxeo-install --' on the\n"
		        "  kernel command line, e.g. for the real, shippable ISO:\n"
		        "  \"--disk=/dev/CHANGEME --ip=CHANGEME --prefix=24 --gateway=CHANGEME\"\n"
		        "  (a deliberately-invalid placeholder -- edit it at the GRUB boot menu\n"
		        "  with 'e' before booting; kanxeo-install's own stat() check on --disk=\n"
		        "  fails safely if it's left unedited)\n",
		        argv[0]);
		return 2;
	}
	stage_dir = argv[1];
	kanxeo_install_bin = argv[2];
	bzimage_path = argv[3];
	control_plane_squashfs = argv[4];
	signing_key = argv[5];
	signing_cert_pem = argv[6];
	signing_cert_der = argv[7];
	out_iso = argv[8];
	kernel_args = argv[9];

	if (ensure_dir(stage_dir) != 0)
		return 1;
	if (test_image_fixture_build(stage_dir, kanxeo_install_bin, "kanxeo-install") != 0)
		return 1;

	if (ensure_dir_under(stage_dir, "usr") != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "usr/sbin") != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/usr/sbin/cfdisk", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/cfdisk", dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/usr/sbin/sfdisk", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/sfdisk", dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/usr/sbin/mkfs.vfat", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/mkfs.fat", dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/usr/sbin/mkfs.ext4", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/mke2fs", dst) != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "usr/bin") != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/usr/bin/mokutil", stage_dir);
	if (test_image_fixture_copy_file("/usr/bin/mokutil", dst) != 0)
		return 1;

	for (i = 0; g_lib_closure[i] != NULL; i++) {
		if (test_image_fixture_add_lib(stage_dir, g_lib_closure[i]) != 0)
			return 1;
	}

	/* Target-disk payload kanxeo-install itself copies onto the ESP/
	 * root-A it writes -- the Secure Boot chain (shim, MokManager, our
	 * own signed systemd-boot) and the control-plane squashfs. The
	 * kernel lives at /boot/kanxeo-bzImage instead (below) and serves
	 * double duty: GRUB's own boot target here, and the same file
	 * kanxeo-install.c's BZIMAGE_SRC copies onto the target disk -- one
	 * copy, not two.
	 *
	 * kanxeo-grubx64.efi is systemd-boot itself, Kanxeo-signed and
	 * deliberately renamed: shim has a hardcoded second-stage lookup of
	 * "\\grubx64.efi" in its own directory (confirmed via `strings` on
	 * the real Debian-signed shim binary) regardless of what's actually
	 * inside the file -- see ADR-0015. */
	if (ensure_dir_under(stage_dir, "payload") != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/kanxeo-shim.efi", stage_dir);
	if (test_image_fixture_copy_file(SHIM_EFI_SRC, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/kanxeo-mm.efi", stage_dir);
	if (test_image_fixture_copy_file(MOKMANAGER_EFI_SRC, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/kanxeo-grubx64.efi", stage_dir);
	if (sbsign_to(signing_key, signing_cert_pem, SYSTEMD_BOOT_EFI, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/kanxeo-signing.cer", stage_dir);
	if (test_image_fixture_copy_file(signing_cert_der, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/kanxeo-root.squashfs", stage_dir);
	if (test_image_fixture_copy_file(control_plane_squashfs, dst) != 0)
		return 1;

	/* Same PID-1 boot shape kanxeod's own image needs (build/mkbootroot)
	 * -- kanxeo-install.c's own early_mounts() mounts proc/sysfs here
	 * before anything else runs; devtmpfs auto-populates /dev on its
	 * own once the mountpoint exists (CONFIG_DEVTMPFS_MOUNT). */
	if (ensure_dir_under(stage_dir, "proc") != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "sys") != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "dev") != 0)
		return 1;

	if (ensure_dir_under(stage_dir, "mnt") != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "mnt/esp") != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "mnt/config") != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "mnt/containers") != 0)
		return 1;

	/* GRUB's own boot target -- the ISO9660-mounted-as-root kernel. Signed
	 * here (not a plain copy): a valid signature doesn't affect the
	 * installer's own unsigned/unenforced GRUB boot, and this exact same
	 * signed file is what kanxeo-install.c's BZIMAGE_SRC later copies
	 * onto the target disk's ESP, where the signature does matter. */
	if (ensure_dir_under(stage_dir, "boot") != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "boot/grub") != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/boot/kanxeo-bzImage", stage_dir);
	if (sbsign_to(signing_key, signing_cert_pem, bzimage_path, dst) != 0)
		return 1;

	snprintf(grub_cfg, sizeof(grub_cfg),
	         "set timeout=10\n"
	         "set default=0\n"
	         "\n"
	         "menuentry \"Kanxeo Install\" {\n"
	         "    linux /boot/kanxeo-bzImage console=tty0 console=ttyS0 root=/dev/sr0 "
	         "rootfstype=iso9660 ro init=/bin/kanxeo-install -- %s\n"
	         "}\n",
	         kernel_args);
	snprintf(grub_cfg_path, sizeof(grub_cfg_path), "%s/boot/grub/grub.cfg", stage_dir);
	if (write_text_file(grub_cfg_path, grub_cfg) != 0)
		return 1;

	{
		char *argv_grub[] = { (char *)GRUB_MKRESCUE_BIN, "-o", (char *)out_iso, (char *)stage_dir,
			               "--", "-volid", "KANXEO", NULL };

		if (run_subprocess(GRUB_MKRESCUE_BIN, argv_grub) != 0)
			return 1;
	}

	printf("wrote %s\n", out_iso);
	return 0;
}
