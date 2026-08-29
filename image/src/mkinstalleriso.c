/*
 * Assembles cix-install's own bootable installer environment and
 * packages it as a real, distributable UEFI-bootable .iso -- the
 * concrete artifact an operator (or test_installer.c's own QEMU -cdrom
 * verification) boots to install Cix onto a real disk.
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
 * cix-install itself still sets up exactly as proven in part 3.
 *
 * Dev-machine-only build tooling (like build/mkbootroot), never shipped
 * or run on a target -- reuses test_image_fixture_build()/_add_lib()/
 * _copy_file() (test/test_image_fixture.c) for staging, the same
 * precedent mkbootroot.c already established (unlike cix-install.c
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

/*
 * grub-mkrescue/sbsign/xorriso/mformat/mcopy are no longer a hardcoded
 * /usr/bin path -- ADR-0064 closes the API-First Mandate gap ADR-0063
 * left open (cixd itself can now assemble an ISO server-side, via
 * POST /v1/system/iso) by pointing these at a real, self-built
 * isotools hostbuild artifact (recipes/package/isotools) instead of
 * whatever happens to be pre-installed on the machine running this
 * tool. g_isotools_root is set once in main() from argv and used by
 * every helper below; a bare manual/dev invocation passes a plain
 * /usr prefix (e.g. "/usr") and gets the exact same host-borrowed
 * behavior this tool always had. */
static char g_isotools_root[512];
static char g_grub_mkrescue_bin[600];
static char g_sbsign_bin[600];
#define SYSTEMD_BOOT_EFI "/usr/lib/systemd/boot/efi/systemd-bootx64.efi"

/* Secure Boot chain for the *target* disk's ESP (cix-install.c's own
 * populate_esp() writes these three under EFI/BOOT/ -- see ADR-0015).
 * Both are pre-signed by Debian directly; no re-signing needed. */
#define SHIM_EFI_SRC "/usr/lib/shim/shimx64.efi.signed"
#define MOKMANAGER_EFI_SRC "/usr/lib/shim/mmx64.efi.signed"

/* The real tools cix-install shells out to, and their full ldd
 * closures (checked directly against this host) -- staged the same way
 * test_image_fixture_add_lib() already stages dnsmasq's own closure
 * (Phase 8), just for a different set of real, unmodified binaries. */
static const char *const g_lib_closure[] = {
	"/lib/x86_64-linux-gnu/libsmartcols.so.1", "/lib/x86_64-linux-gnu/libfdisk.so.1",
	"/lib/x86_64-linux-gnu/libtinfo.so.6",     "/lib/x86_64-linux-gnu/libuuid.so.1",
	"/lib/x86_64-linux-gnu/libblkid.so.1",     "/lib/x86_64-linux-gnu/libreadline.so.8",
	"/lib/x86_64-linux-gnu/libext2fs.so.2",    "/lib/x86_64-linux-gnu/libcom_err.so.2",
	"/lib/x86_64-linux-gnu/libe2p.so.2",
	/* mokutil's own closure (ldd-checked against this host), for
	 * enroll_signing_key()'s "mokutil --import" call in cix-install.c. */
	"/lib/x86_64-linux-gnu/libcrypto.so.3",    "/lib/x86_64-linux-gnu/libefivar.so.1",
	"/lib/x86_64-linux-gnu/libkeyutils.so.1",  "/lib/x86_64-linux-gnu/libcrypt.so.1",
	"/lib/x86_64-linux-gnu/libdl.so.2",
	/* mkfs.btrfs's own closure beyond what's above (ldd-checked):
	 * libuuid/libblkid are already staged, libudev is not. */
	"/lib/x86_64-linux-gnu/libudev.so.1",      NULL,
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
	char *argv_sbsign[] = { g_sbsign_bin, "--key",  (char *)key, "--cert", (char *)cert,
		                 "--output", (char *)dst, (char *)src,  NULL };

	return run_subprocess(g_sbsign_bin, argv_sbsign);
}

int main(int argc, char **argv)
{
	const char *stage_dir;
	const char *cix_install_bin;
	const char *cix_recover_bin;
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
	char grub_module_dir[600];
	char xorriso_bin[600];
	char isotools_bin_dir[600];
	char isotools_lib_dir[600];
	int i;

	if (argc != 12) {
		fprintf(stderr,
		        "usage: %s <staging-dir> <cix-install-bin> <cix-recover-bin> "
		        "<bzImage> <control-plane-squashfs> <signing-key> <signing-cert.crt> "
		        "<signing-cert.cer> <out.iso> <kernel-args> <isotools-root>\n"
		        "  cix-recover-bin: the break-glass recovery tool (ADR-0146), staged as\n"
		        "  a second GRUB menu entry on the SAME media -- boots straight to a\n"
		        "  console prompt, no kernel-args needed (it takes none).\n"
		        "  signing-key/signing-cert.crt/signing-cert.cer: the Cix Secure Boot\n"
		        "  signing key pair (image/keys/cix-signing.{key,crt,cer} -- .crt is\n"
		        "  PEM, for sbsign; .cer is DER, for mokutil) -- used to sign systemd-boot\n"
		        "  and the kernel for the *target* disk's ESP; the installer media's own\n"
		        "  GRUB boot stays unsigned (see ADR-0015).\n"
		        "  kernel-args: everything after 'init=/bin/cix-install --' on the\n"
		        "  kernel command line, e.g. for the real, shippable ISO:\n"
		        "  \"--disk=/dev/CHANGEME --ip=CHANGEME --prefix=24 --gateway=CHANGEME "
		        "--interface=CHANGEME\"\n"
		        "  (a deliberately-invalid placeholder -- edit it at the GRUB boot menu\n"
		        "  with 'e' before booting; cix-install's own stat() check on --disk=\n"
		        "  fails safely if it's left unedited)\n"
		        "  isotools-root: directory holding <root>/bin/{grub-mkrescue,sbsign,\n"
		        "  xorriso,mcopy,mformat} and <root>/lib/grub/x86_64-efi/ (pkg/recipes/\n"
		        "  isotools.recipe's own hostbuild artifact layout, ADR-0064) -- a plain\n"
		        "  \"/usr\" reproduces this tool's original host-borrowed behavior for\n"
		        "  manual/dev use.\n",
		        argv[0]);
		return 2;
	}
	stage_dir = argv[1];
	cix_install_bin = argv[2];
	cix_recover_bin = argv[3];
	bzimage_path = argv[4];
	control_plane_squashfs = argv[5];
	signing_key = argv[6];
	signing_cert_pem = argv[7];
	signing_cert_der = argv[8];
	out_iso = argv[9];
	kernel_args = argv[10];
	snprintf(g_isotools_root, sizeof(g_isotools_root), "%s", argv[11]);

	snprintf(g_grub_mkrescue_bin, sizeof(g_grub_mkrescue_bin), "%s/bin/grub-mkrescue",
	         g_isotools_root);
	snprintf(g_sbsign_bin, sizeof(g_sbsign_bin), "%s/bin/sbsign", g_isotools_root);
	snprintf(grub_module_dir, sizeof(grub_module_dir), "%s/lib/grub/x86_64-efi", g_isotools_root);
	snprintf(xorriso_bin, sizeof(xorriso_bin), "%s/bin/xorriso", g_isotools_root);
	snprintf(isotools_bin_dir, sizeof(isotools_bin_dir), "%s/bin", g_isotools_root);
	snprintf(isotools_lib_dir, sizeof(isotools_lib_dir), "%s/lib/x86_64-linux-gnu",
	         g_isotools_root);
	/*
	 * grub-mkrescue itself is always invoked by full explicit path
	 * below (never PATH-searched), but it in turn fork/execs "xorriso"
	 * (overridable via --xorriso=, set explicitly below anyway) and
	 * "mformat"/"mcopy" (no override flag exists for either, confirmed
	 * directly against grub-mkrescue's own --help output and ADR-0063's
	 * own research) by literal name via $PATH -- these child processes
	 * inherit this process's own environ, so PATH must include
	 * isotools_bin_dir before grub-mkrescue ever runs.
	 * LD_LIBRARY_PATH covers isotools.recipe's own real, measured
	 * shared-library closure for a deployed host whose own system
	 * paths don't otherwise carry it: libz.so.1 (xorriso) and
	 * libcrypto.so.3 (sbsign/sbverify), plus libc.so.6. It named
	 * liblzma.so.5 and libbz2.so.1.0 until isotools 2.14-2 -- neither
	 * is linked by any of these binaries at any level of the closure;
	 * both were read off the pre-ADR-0199 shared build sandbox that
	 * carried the build host's entire /usr (issue #168).
	 */
	setenv("PATH", isotools_bin_dir, 1);
	setenv("LD_LIBRARY_PATH", isotools_lib_dir, 1);

	if (ensure_dir(stage_dir) != 0)
		return 1;
	if (test_image_fixture_build(stage_dir, cix_install_bin, "cix-install") != 0)
		return 1;
	/*
	 * cix-recover (ADR-0146) is dynamically linked against nothing
	 * beyond plain libc -- the exact same runtime test_image_fixture_build()
	 * just staged for cix-install above (ld-linux-x86-64.so.2, libc.so.6
	 * under lib64/ and lib/x86_64-linux-gnu/ respectively). A second full
	 * _build() call would just re-copy those same two files under a second,
	 * redundant name; a plain file copy into the already-staged bin/ is
	 * the correct, minimal step here.
	 */
	{
		char recover_dst[600];

		snprintf(recover_dst, sizeof(recover_dst), "%s/bin/cix-recover", stage_dir);
		if (test_image_fixture_copy_file(cix_recover_bin, recover_dst) != 0)
			return 1;
	}

	if (ensure_dir_under(stage_dir, "usr") != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "usr/sbin") != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/usr/sbin/fdisk", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/fdisk", dst) != 0)
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
	/* ADR-0207 phase 4: btrfs is the install default for the platform
	 * partitions -- cix-install.c's mkfs_btrfs() calls this. ext4
	 * stays staged through the transition (the tolerant boot mounts
	 * accept either), retiring only once btrfs has run production on
	 * .95 -- the staged retirement the ADR specifies. */
	snprintf(dst, sizeof(dst), "%s/usr/sbin/mkfs.btrfs", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/mkfs.btrfs", dst) != 0)
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

	/* Target-disk payload cix-install itself copies onto the ESP/
	 * root-A it writes -- the Secure Boot chain (shim, MokManager, our
	 * own signed systemd-boot) and the control-plane squashfs. The
	 * kernel lives at /boot/cix-bzImage instead (below) and serves
	 * double duty: GRUB's own boot target here, and the same file
	 * cix-install.c's BZIMAGE_SRC copies onto the target disk -- one
	 * copy, not two.
	 *
	 * cix-grubx64.efi is systemd-boot itself, Cix-signed and
	 * deliberately renamed: shim has a hardcoded second-stage lookup of
	 * "\\grubx64.efi" in its own directory (confirmed via `strings` on
	 * the real Debian-signed shim binary) regardless of what's actually
	 * inside the file -- see ADR-0015. */
	if (ensure_dir_under(stage_dir, "payload") != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/cix-shim.efi", stage_dir);
	if (test_image_fixture_copy_file(SHIM_EFI_SRC, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/cix-mm.efi", stage_dir);
	if (test_image_fixture_copy_file(MOKMANAGER_EFI_SRC, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/cix-grubx64.efi", stage_dir);
	if (sbsign_to(signing_key, signing_cert_pem, SYSTEMD_BOOT_EFI, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/cix-signing.cer", stage_dir);
	if (test_image_fixture_copy_file(signing_cert_der, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/cix-root.squashfs", stage_dir);
	if (test_image_fixture_copy_file(control_plane_squashfs, dst) != 0)
		return 1;

	/*
	 * ADR-0210: a "payload/cix-runtime" C runtime used to be staged
	 * here for cix-install.c to copy into the base image. Both are
	 * gone. The copy wrote to the pre-versioning images/base/rootfs
	 * path, which container creation stopped reading when ADR-0107/0108
	 * made images versioned -- so it was shipping ~3 MB on every
	 * installer ISO to populate a directory nothing consulted. The
	 * daemon's ensure_default_image() now materializes the default
	 * image from an empty manifest with the same baseline every other
	 * image gets. The installer's OWN library closure is unaffected --
	 * that is g_lib_closure[] above, a separate thing.
	 */

	/* Same PID-1 boot shape cixd's own image needs (build/mkbootroot)
	 * -- cix-install.c's own early_mounts() mounts proc/sysfs here
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
	 * signed file is what cix-install.c's BZIMAGE_SRC later copies
	 * onto the target disk's ESP, where the signature does matter. */
	if (ensure_dir_under(stage_dir, "boot") != 0)
		return 1;
	if (ensure_dir_under(stage_dir, "boot/grub") != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/boot/cix-bzImage", stage_dir);
	if (sbsign_to(signing_key, signing_cert_pem, bzimage_path, dst) != 0)
		return 1;

	/*
	 * Second, distinct boot target on the SAME media (ADR-0146):
	 * cix-recover never reformats/reinstalls anything, so it needs no
	 * kernel-args of its own -- everything it needs (which system disk to
	 * touch, what to reset) is either hardcoded (its own header comment
	 * explains why) or gathered interactively at its own console prompt.
	 */
	snprintf(grub_cfg, sizeof(grub_cfg),
	         "set timeout=10\n"
	         "set default=0\n"
	         /* "keep": hand the kernel the EFI console mode GRUB is
	          * already running in, framebuffer info included. The
	          * previous value here, "text", was itself an attempted fix
	          * for GRUB's "no suitable video mode found / Booting in
	          * blind mode" message -- but text mode does not exist under
	          * EFI, so the mode-set still failed AND, crucially, a
	          * failed set means GRUB passes NO video information to the
	          * kernel at all. Proven by direct experiment (QEMU -vga std,
	          * QMP screendump): with "text", the kernel boots with zero
	          * framebuffer -- no sysfb platform device, no simpledrm
	          * bind, no fbcon -- and the VGA console stays permanently
	          * blank showing GRUB's frozen error; a kernel faithfully
	          * built with every video option enabled behaves identically,
	          * because it was never given a framebuffer to drive. So the
	          * install was serial-only on every EFI machine, silently.
	          * "keep" is the mode-set that cannot fail (it sets nothing),
	          * and it forwards the GOP framebuffer the firmware already
	          * configured -- which is exactly what SYSFB_SIMPLEFB +
	          * DRM_SIMPLEDRM (the kernel side of this chain, Phase 18)
	          * were always waiting for. */
	         /* Without a video driver loaded, EVERY gfxpayload value
	          * fails identically ("keep" included -- proven by a second
	          * screendump experiment after "keep" alone changed nothing):
	          * GRUB's menu renders via the EFI text console, which needs
	          * no driver, so the menu working is exactly what made this
	          * invisible. all_video is GRUB's own meta-module that pulls
	          * in the right platform driver (efi_gop here); only with it
	          * loaded does a mode query succeed and framebuffer info
	          * reach the kernel. */
	         "insmod all_video\n"
	         "set gfxpayload=keep\n"
	         "\n"
	         "menuentry \"Cix Install\" {\n"
	         "    linux /boot/cix-bzImage console=tty0 console=ttyS0 root=/dev/sr0 "
	         "rootfstype=iso9660 ro init=/bin/cix-install -- %s\n"
	         "}\n"
	         "\n"
	         "menuentry \"Cix Recovery (reset host-auth admin_groups)\" {\n"
	         "    linux /boot/cix-bzImage console=tty0 console=ttyS0 root=/dev/sr0 "
	         "rootfstype=iso9660 ro init=/bin/cix-recover\n"
	         "}\n",
	         kernel_args);
	snprintf(grub_cfg_path, sizeof(grub_cfg_path), "%s/boot/grub/grub.cfg", stage_dir);
	if (write_text_file(grub_cfg_path, grub_cfg) != 0)
		return 1;

	{
		char directory_flag[600];
		char xorriso_flag[600];
		char *argv_grub[] = { g_grub_mkrescue_bin,
			               directory_flag,
			               xorriso_flag,
			               "-o",
			               (char *)out_iso,
			               (char *)stage_dir,
			               "--",
			               "-volid",
			               "CIX",
			               NULL };

		snprintf(directory_flag, sizeof(directory_flag), "--directory=%s", grub_module_dir);
		snprintf(xorriso_flag, sizeof(xorriso_flag), "--xorriso=%s", xorriso_bin);

		if (run_subprocess(g_grub_mkrescue_bin, argv_grub) != 0)
			return 1;
	}

	printf("wrote %s\n", out_iso);
	return 0;
}
