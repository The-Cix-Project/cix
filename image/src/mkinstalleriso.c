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

#include <dirent.h>
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
 * every helper below.
 *
 * It used to be documented that a bare dev invocation could pass a
 * plain "/usr" and get the host-borrowed behaviour this tool always
 * had. That stopped being true the moment the tool began reading
 * Cix-BUILT binaries -- their library closure is not a Debian box's,
 * and a plain /usr carries none of the shim/, sbin/mkfs.fat or
 * bin/mokutil layout this reads. Continuing to assert it produced a
 * dev path that failed on files nobody had noticed were Cix-specific. test/test_installer.c
 * builds a real isotools-root fixture instead: the same layout, filled
 * from the development machine's own tools. A fixture is honest about
 * what it is; "/usr" was pretending to be an artifact. */
static char g_isotools_root[512];
static char g_grub_mkrescue_bin[600];
static char g_sbsign_bin[600];

/* Secure Boot chain for the *target* disk's ESP (cix-install.c's own
 * populate_esp() writes these three under EFI/BOOT/ -- see ADR-0015).
 * Both are pre-signed by Debian directly; no re-signing needed. */
/*
 * shim, MokManager and mokutil come out of the isotools artifact, not
 * off this machine. They used to be read from /usr/lib/shim/ and
 * /usr/bin/mokutil -- absolute paths that exist on a Debian development
 * box and on no Cix control-plane root, which is a large part of why
 * POST /v1/system/iso has never been servable on a real host. They are
 * packages now (shim 16.1-1, mokutil 0.7.2-2) and isotools 2.14-3
 * harvests them, so this tool reads them the same way it already reads
 * grub-mkrescue, xorriso, sbsign and mtools.
 */
#define SHIM_EFI_REL "/shim/shimx64.efi.signed"
#define MOKMANAGER_EFI_REL "/shim/mmx64.efi.signed"
#define MOKUTIL_REL "/bin/mokutil"

/* mkfs.fat: the ESP formatter, which no Cix control-plane root carries
 * and rightly so -- cixd never formats FAT, only the installer does. See
 * the staging block in build_installer_root() for why it comes from the
 * artifact while sfdisk and the mkfs pair come off the host.
 *
 * "/sbin/", not "/bin/", matching where isotools.recipe stages it and
 * where a development machine keeps its own copy (/usr/sbin/mkfs.fat),
 * which is what test_installer's fixture fills this slot from. */
#define MKFS_FAT_REL "/sbin/mkfs.fat"




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
	const char *cix_boot_bin;
	const char *bzimage_path;
	const char *control_plane_squashfs;
	const char *signing_key;
	const char *signing_cert_pem;
	const char *signing_cert_der;
	const char *out_iso;
	const char *kernel_args;
	const char *seed_dir;
	char dst[600];
	char grub_cfg_path[600];
	char grub_cfg[1024];
	char grub_module_dir[600];
	char xorriso_bin[600];
	char isotools_bin_dir[600];
	char isotools_lib_dir[600];
	int i;

	if (argc != 14) {
		fprintf(stderr,
		        "usage: %s <staging-dir> <cix-install-bin> <cix-recover-bin> "
		        "<cix-boot.efi> <bzImage> <control-plane-squashfs> <signing-key> "
		        "<signing-cert.crt> <signing-cert.cer> <out.iso> <kernel-args> "
		        "<isotools-root> <seed-dir>\n"
		        "  cix-recover-bin: the break-glass recovery tool (ADR-0146), staged as\n"
		        "  a second GRUB menu entry on the SAME media -- boots straight to a\n"
		        "  console prompt, no kernel-args needed (it takes none).\n"
		        "  cix-boot.efi: the UEFI boot manager (ADR-0215), signed here and\n"
		        "  written to the TARGET disk's ESP as shim's second stage. Built from\n"
		        "  this repo alongside cix-install, which is why it is an argument\n"
		        "  rather than something read off the build machine -- it used to be\n"
		        "  systemd-boot, at an absolute path no Cix host has.\n"
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
		        "  xorriso,mcopy,mformat,mokutil}, <root>/shim/{shimx64,mmx64}.efi.signed,\n"
		        "  <root>/lib/grub/x86_64-efi/ and <root>/lib/x86_64-linux-gnu/ (isotools\n"
		        "  2.14-3's hostbuild artifact layout, ADR-0064). mokutil, shim and\n"
		        "  mokutil's library closure moved here from absolute host paths: they\n"
		        "  exist on a Debian development box and on no Cix control-plane root,\n"
		        "  so reading them from the host is why this tool could not run on a\n"
		        "  real installed machine. A plain \"/usr\" no longer reproduces the old\n"
		        "  host-borrowed behaviour for those inputs.\n"
		        "  seed-dir: optional (pass \"\" for none) -- a directory holding\n"
		        "  <seed>/recipes/ and <seed>/artifacts/, staged onto the media so a\n"
		        "  freshly installed box has a package source before it has a network\n"
		        "  (#189/#135). Its artifacts are verified against their own recipes by\n"
		        "  the daemon before anything is installed from them, so this is a\n"
		        "  delivery mechanism and not a trust boundary.\n",
		        argv[0]);
		return 2;
	}
	stage_dir = argv[1];
	cix_install_bin = argv[2];
	cix_recover_bin = argv[3];
	cix_boot_bin = argv[4];
	bzimage_path = argv[5];
	control_plane_squashfs = argv[6];
	signing_key = argv[7];
	signing_cert_pem = argv[8];
	signing_cert_der = argv[9];
	out_iso = argv[10];
	kernel_args = argv[11];
	snprintf(g_isotools_root, sizeof(g_isotools_root), "%s", argv[12]);
	seed_dir = argv[13];

	snprintf(g_grub_mkrescue_bin, sizeof(g_grub_mkrescue_bin), "%s/bin/grub-mkrescue",
	         g_isotools_root);
	snprintf(g_sbsign_bin, sizeof(g_sbsign_bin), "%s/bin/sbsign", g_isotools_root);
	snprintf(grub_module_dir, sizeof(grub_module_dir), "%s/lib/grub/x86_64-efi", g_isotools_root);
	/*
	 * grub-mkrescue is pointed at cix-xorriso, not at xorriso itself.
	 * That filter strips the Apple HFS+ metadata arguments
	 * grub-mkrescue adds unconditionally and then runs the real
	 * xorriso, which is handed to it through the environment. See
	 * image/src/cix-xorriso.c for why the arguments cannot simply be
	 * turned off: libisofs needs glibc gconv modules to convert
	 * filenames for HFS+, no Cix image has any, and importing Debian's
	 * to satisfy a feature this platform does not want would be the
	 * wrong fix.
	 *
	 * Found beside this binary rather than passed as another argument:
	 * both ship in the same "cix" hostbuild artifact and are always
	 * co-located, so deriving the path keeps the caller's contract
	 * unchanged.
	 */
	snprintf(xorriso_bin, sizeof(xorriso_bin), "%s/bin/xorriso", g_isotools_root);
	setenv("CIX_REAL_XORRISO", xorriso_bin, 1);
	{
		char self_dir[PATH_MAX];
		const char *slash;

		snprintf(self_dir, sizeof(self_dir), "%s", argv[0]);
		slash = strrchr(self_dir, '/');
		if (slash != NULL) {
			snprintf(xorriso_bin, sizeof(xorriso_bin), "%.*s/cix-xorriso",
			         (int)(slash - self_dir), self_dir);
		} else {
			snprintf(xorriso_bin, sizeof(xorriso_bin), "./cix-xorriso");
		}
		if (access(xorriso_bin, X_OK) != 0) {
			perror(xorriso_bin);
			fprintf(stderr, "cix-xorriso must sit beside mkinstalleriso "
			                "(both come from the same \"cix\" hostbuild)\n");
			return 1;
		}
	}
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

	/*
	 * grub-mkrescue makes itself a temporary directory, and a Cix
	 * control-plane root has no /tmp at all -- so with no TMPDIR set it
	 * fails with
	 *
	 *   grub-mkrescue: error: cannot make temporary directory:
	 *   No such file or directory
	 *
	 * on the very last step, after every input has been staged and
	 * signed. Same class of gap as this project's own container images
	 * having no /tmp (CLAUDE.md), one layer out.
	 *
	 * Pointed NEXT TO the staging directory rather than inside it:
	 * stage_dir is the ISO's content root, so anything created within
	 * it would be published on the media. Created here rather than
	 * assumed, since the whole point is that the usual location does
	 * not exist.
	 */
	{
		char tmp_dir[700];
		const char *slash = strrchr(stage_dir, '/');

		if (slash != NULL) {
			snprintf(tmp_dir, sizeof(tmp_dir), "%.*s/.mkrescue-tmp",
			         (int)(slash - stage_dir), stage_dir);
		} else {
			snprintf(tmp_dir, sizeof(tmp_dir), ".mkrescue-tmp");
		}
		if (ensure_dir(tmp_dir) != 0)
			return 1;
		setenv("TMPDIR", tmp_dir, 1);
	}

	/*
	 * grub-mkrescue reads its own data files -- unicode.pf2 above all --
	 * from a pkgdatadir compiled in at /usr/share/grub, which exists on
	 * the machine that built GRUB and on no Cix control-plane root:
	 *
	 *   grub-mkrescue: error: cannot open `/usr/share/grub/unicode.pf2':
	 *   No such file or directory.
	 *
	 * GRUB's own utility code resolves that directory through a
	 * "pkgdatadir" environment variable before falling back to the
	 * compiled default. Verified directly rather than assumed: a bogus
	 * value makes it complain about that exact path, and a real staged
	 * one builds an ISO. The module directory needs no equivalent --
	 * grub-mkrescue is already given it explicitly with -d.
	 */
	{
		char grub_data_dir[700];

		snprintf(grub_data_dir, sizeof(grub_data_dir), "%s/share/grub", g_isotools_root);
		setenv("pkgdatadir", grub_data_dir, 1);
	}

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
	/*
	 * mkfs.fat comes out of the isotools artifact; sfdisk,
	 * mkfs.ext4 and mkfs.btrfs come off this host. The split is not
	 * arbitrary -- it follows what a Cix control-plane root actually
	 * contains, which is exactly the set mkbootroot.c stages, which is
	 * in turn exactly the set cixd itself shells out to (sfdisk for
	 * diskpart, mkfs.ext4/mkfs.btrfs for diskformat).
	 *
	 * cixd never formats FAT -- only the installer does, for the ESP --
	 * so mkfs.fat has no reason to be in that root and is not in it.
	 * Reading it from /usr/sbin anyway was the same class of bug shim
	 * and mokutil had: an absolute path true on a Debian development
	 * box and on no Cix host, and one of the reasons POST
	 * /v1/system/iso has never been servable on a real machine. It is a
	 * package now (dosfstools 4.2-1) and isotools harvests it.
	 *
	 * It needs no library beyond libc -- asserted in its own recipe
	 * against the real ELF -- so it adds nothing to the closures staged
	 * below.
	 *
	 * fdisk used to be staged here too, for an interactive partitioning
	 * path that no longer exists (ADR-0214).
	 */
	{
		char tool_src[700];

		snprintf(tool_src, sizeof(tool_src), "%s" MKFS_FAT_REL, g_isotools_root);
		snprintf(dst, sizeof(dst), "%s/usr/sbin/mkfs.vfat", stage_dir);
		if (test_image_fixture_copy_file(tool_src, dst) != 0)
			return 1;
	}
	snprintf(dst, sizeof(dst), "%s/usr/sbin/sfdisk", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/sfdisk", dst) != 0)
		return 1;
	/*
	 * Read as mkfs.ext4, not mke2fs. They are the same binary on a
	 * Debian box (mkfs.ext4 is a link to mke2fs, which switches
	 * behaviour on argv[0]), which is why reading either name worked
	 * there and hid the difference. A Cix control-plane root has only
	 * the mkfs.ext4 name, because that is the name
	 * DISKFORMAT_MKFS_EXT4_BIN uses and mkbootroot stages exactly the
	 * _BIN macros -- so mke2fs was a guaranteed miss on a real host.
	 * It is also the name this tool stages it under, so the two ends
	 * now agree.
	 */
	snprintf(dst, sizeof(dst), "%s/usr/sbin/mkfs.ext4", stage_dir);
	if (test_image_fixture_copy_file("/usr/sbin/mkfs.ext4", dst) != 0)
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
	{
		char mokutil_src[700];

		snprintf(mokutil_src, sizeof(mokutil_src), "%s" MOKUTIL_REL, g_isotools_root);
		snprintf(dst, sizeof(dst), "%s/usr/bin/mokutil", stage_dir);
		if (test_image_fixture_copy_file(mokutil_src, dst) != 0)
			return 1;
	}

	/*
	 * Every staged binary's shared-library closure, derived from the
	 * binary itself and resolved against the tree it came from.
	 *
	 * There used to be two hand-maintained arrays here, one per source
	 * tree, and neither could have been right: a single list cannot
	 * describe two different sets of binaries, and these are two
	 * different sets. Debian's mkfs.btrfs needs libudev.so.1 and ours
	 * does not; ours links libz.so.1 and Debian's does not; Debian
	 * keeps libcrypt's obsolete DES/NIS ABI at soname .so.1 while our
	 * libxcrypt drops it and ships .so.2. Every correction made for one
	 * host broke the other, and it broke quietly -- the ISO built, and
	 * the installer died at exec time inside a QEMU boot with
	 * "mkfs.btrfs failed (status 0x7f00)".
	 *
	 * The closure follows the binary now. Host tools resolve against
	 * this machine's library directories, artifact tools against the
	 * artifact's own -- so each set gets exactly what it was linked
	 * against, and a library that cannot be found is a hard error here
	 * rather than a silent failure at install time.
	 */
	{
		static const char *const host_dirs[] = {
			"/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
			"/lib", "/usr/lib", NULL,
		};
		char artifact_lib[700], artifact_lib64[700];
		const char *artifact_dirs[3];
		size_t bi;
		static const char *const host_bins[] = {
			"/usr/sbin/sfdisk", "/usr/sbin/mkfs.ext4", "/usr/sbin/mkfs.btrfs", NULL,
		};
		char staged[PATH_MAX];

		snprintf(artifact_lib, sizeof(artifact_lib), "%s/lib/x86_64-linux-gnu",
		         g_isotools_root);
		snprintf(artifact_lib64, sizeof(artifact_lib64), "%s/lib64", g_isotools_root);
		artifact_dirs[0] = artifact_lib;
		artifact_dirs[1] = artifact_lib64;
		artifact_dirs[2] = NULL;

		for (bi = 0; host_bins[bi] != NULL; bi++) {
			if (test_image_fixture_stage_closure(stage_dir, host_bins[bi], host_dirs) != 0)
				return 1;
		}
		/* Resolved from the artifact, but read from where they were
		 * staged -- same bytes either way, and this keeps the call
		 * uniform. */
		snprintf(staged, sizeof(staged), "%s/usr/bin/mokutil", stage_dir);
		if (test_image_fixture_stage_closure(stage_dir, staged, artifact_dirs) != 0)
			return 1;
		snprintf(staged, sizeof(staged), "%s/usr/sbin/mkfs.vfat", stage_dir);
		if (test_image_fixture_stage_closure(stage_dir, staged, artifact_dirs) != 0)
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
	{
		char shim_src[700];

		snprintf(shim_src, sizeof(shim_src), "%s" SHIM_EFI_REL, g_isotools_root);
		if (test_image_fixture_copy_file(shim_src, dst) != 0)
			return 1;
		snprintf(dst, sizeof(dst), "%s/payload/cix-mm.efi", stage_dir);
		snprintf(shim_src, sizeof(shim_src), "%s" MOKMANAGER_EFI_REL, g_isotools_root);
		if (test_image_fixture_copy_file(shim_src, dst) != 0)
			return 1;
	}
	/*
	 * The boot manager, signed with the Cix key and staged as the
	 * second stage shim will chainload on the target (ADR-0015 --
	 * "grubx64.efi" is shim's own fixed name for that slot, not a
	 * claim about GRUB).
	 *
	 * This used to be systemd-boot, read from an absolute path on
	 * whatever machine ran this tool. It is cix-boot now (ADR-0215),
	 * built from this repo by the same round that builds cix-install
	 * and cix-recover, and passed in the same way -- so it needs no
	 * package, no artifact harvest and no isotools involvement at all.
	 */
	snprintf(dst, sizeof(dst), "%s/payload/cix-grubx64.efi", stage_dir);
	if (sbsign_to(signing_key, signing_cert_pem, cix_boot_bin, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/cix-signing.cer", stage_dir);
	if (test_image_fixture_copy_file(signing_cert_der, dst) != 0)
		return 1;
	snprintf(dst, sizeof(dst), "%s/payload/cix-root.squashfs", stage_dir);
	if (test_image_fixture_copy_file(control_plane_squashfs, dst) != 0)
		return 1;

	/*
	 * The package seed (#189): recipes and artifacts a freshly
	 * installed box needs before it has a network or a configured
	 * package source.
	 *
	 * Deliberately NOT the thing ADR-0210 removed, and the difference
	 * is the whole point. That was a C runtime copied into the base
	 * image directly -- content arriving by mechanism, into a path
	 * nothing read. This stages *packages*: cix-install copies them
	 * into the daemon's own recipe and cache directories, and the
	 * daemon then installs from them through the ordinary pipeline,
	 * verifying each artifact against its own recipe first. What the
	 * image ends up with is a real manifest entry with a version, not
	 * files someone put there.
	 *
	 * Optional: an empty seed-dir stages nothing, and such a box
	 * behaves exactly as it does today -- the default image has no C
	 * library and POST /v1/containers refuses with a message naming
	 * what to install.
	 */
	if (seed_dir != NULL && seed_dir[0] != '\0') {
		char seed_dst[PATH_MAX];

		/*
		 * Deliberately NOT pre-created: this copy is `cp -a src dst`,
		 * and cp's rule is that an existing directory destination means
		 * "copy INTO here" -- which silently produced payload/seed/seed
		 * and an installed box that found no recipes. The destination
		 * must not exist so it becomes the copy rather than its parent.
		 */
		snprintf(seed_dst, sizeof(seed_dst), "%s/payload/seed", stage_dir);
		if (test_image_fixture_copy_dir_recursive(seed_dir, seed_dst) != 0) {
			fprintf(stderr, "staging the package seed from %s failed\n", seed_dir);
			return 1;
		}
		printf("staged the package seed from %s\n", seed_dir);
	}

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
