/*
 * Phase 11 part 4/5 demonstrable test: proves the actual installer *and*
 * its real, distributable .iso packaging both work -- given a raw,
 * blank target disk, cix-install
 * (booted from the real .iso build/mkinstalleriso produces, via QEMU's
 * -cdrom, not a test-only disk-image approximation) partitions its role-
 * detection, formats, writes the real payload, and configures a static
 * IP -- then more, completely separate boots prove the freshly-installed
 * disk actually comes up as a real, running cixd *with Secure Boot
 * genuinely enforced* (ADR-0015). "Install" and "boot what was
 * installed" are the same real code paths already proven in parts 1-2,
 * and "the ISO an operator would actually use" is now the same artifact
 * this test boots, not a separate, untested approximation of it (part 3's
 * own test built a private squashfs-based disk instead -- this part
 * replaces that with the real thing).
 *
 * Two disks, four QEMU sessions -- one smoke test proving the real,
 * unmodified boot mechanism an operator actually drives (GRUB's own
 * menu), then the three-session Secure Boot flow. A second smoke test
 * covering an interactive fdisk session sat alongside it until ADR-0214
 * removed that path; see step 4's own comment:
 *   1. build/mkinstalleriso's real .iso + a blank target disk,
 *      partitioned by cix-install itself, which is what it does unless
 *      told otherwise (its own scripted sfdisk -- this session's own job
 *      is proving the installer's role-detection/format/write logic).
 *      This is now the only partitioning path there is. Since the boot
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
 *      real -- cixd --init-mode --slot=a off the partition
 *      cix-install wrote, now via the fully-trusted shim -> signed
 *      systemd-boot -> signed kernel chain.
 *
 * Verification of what the installer actually wrote happens from the
 * host side after session 1 -- the ESP via mtools' disk.img@@offset
 * addressing (no mount needed, same pattern test_boot_ab.c already
 * established for inspecting loader-entry counter state), the config
 * and containers partitions (btrfs since ADR-0207 phase 4) via `btrfs
 * restore` (pure userspace on an extracted partition image -- also no
 * mount, which this sandbox couldn't do anyway: no loop devices), and
 * root A's raw bytes read back and compared against the original
 * squashfs.
 */
#include "test_disk_image.h"
#include "test_image_fixture.h"
/* For PKG_IMAGE_LOADER_REL -- the one definition of "what proves an
 * image can run anything", shared with the daemon rather than restated. */
#include "pkg.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MKBOOTROOT_BIN "build/mkbootroot"
#define CIXD_BIN "build/cixd"
#define CIXCTL_BIN "build/cixctl"
#define CIX_INSTALL_BIN "build/cix-install"
#define CIX_RECOVER_BIN "build/cix-recover"
#define CIX_BOOT_BIN "build/cix-boot.efi"
#define MKINSTALLERISO_BIN "build/mkinstalleriso"
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define BTRFS_BIN "/usr/bin/btrfs"
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS_4M.ms.fd"
#define SIGNING_KEY "image/keys/cix-signing.key"
#define SIGNING_CERT_PEM "image/keys/cix-signing.crt"
#define SIGNING_CERT_DER "image/keys/cix-signing.cer"
/*
 * ADR-0064: mkinstalleriso takes an explicit isotools-root rather than
 * a hardcoded /usr/bin/grub-mkrescue. This test used to pass a plain
 * "/usr", on the reasoning that it reproduced the tool's original
 * host-borrowed behaviour.
 *
 * That reasoning expired without anyone noticing. Once mkinstalleriso
 * started reading Cix-BUILT binaries out of the artifact -- shim,
 * MokManager, mokutil, mkfs.fat -- their library closure stopped being
 * a Debian box's. A development machine has no libcrypt.so.2 at all
 * (it keeps the obsolete DES/NIS ABI and ships .so.1; our libxcrypt
 * drops it, so the soname differs), and no /usr has the shim/ or
 * bin/mokutil layout this reads. "/usr" was pretending to be an
 * artifact, and the pretence failed the first time this test was run
 * afterwards:
 *
 *   /usr/lib/x86_64-linux-gnu/libcrypt.so.2: No such file or directory
 *
 * So build a real one instead. build_isotools_fixture() below fills the
 * artifact's own layout from this machine's tools -- the same
 * directory structure isotools.recipe produces, with Debian's binaries
 * and Debian's measured closure in it. A fixture is honest about being
 * a stand-in; "/usr" was not.
 */
#define ISOTOOLS_FIXTURE_DIRNAME "isotools_fixture"
#define MOK_PASSWORD "cixtest"


#define TARGET_ESP_SIZE_MIB 64
#define TARGET_ROOT_SIZE_MIB 160
#define TARGET_CONFIG_SIZE_MIB 64
/*
 * Issue #104: the installer no longer takes the rest of the disk -- the
 * data partition is bounded and the remainder is deliberately left
 * unallocated for the admin. So the target has to be big enough to have
 * a remainder worth leaving: 448 MiB of system partitions out of 1 GiB
 * leaves ~568 MiB, of which the data partition takes half. The file is
 * sparse, so the extra size costs nothing.
 */
/*
 * 3 GiB, raised from 1 GiB when the default image started carrying a
 * real C library (#189).
 *
 * The system partitions take a fixed 448 MiB (ESP 64 + two 160 MiB root
 * slots + config 64), which left ~576 MiB of btrfs for everything else.
 * That was ample while "base" was an empty manifest; glibc unpacks to
 * ~126 MB, and btrfs allocates metadata in 256 MB chunks, so a volume
 * that size runs out far earlier than the arithmetic suggests. The
 * install itself reported it honestly -- "image base could not produce
 * a new version: the caller's own mutate step failed" -- which is what
 * pointed here.
 *
 * The image is a sparse file, so this costs allocation, not disk.
 */
#define TARGET_DISK_SIZE_BYTES (3LL * 1024 * 1024 * 1024)
#define SECTOR_SIZE 512

#define INSTALL_TIMEOUT_SECONDS 180
#define BOOT_TIMEOUT_SECONDS 120
#define INSTALL_SUCCESS_MARKER "cix-install: install complete"
#define BOOT_SUCCESS_MARKER "cixd listening on"
/*
 * The first boot of a seeded install does one more thing before it is
 * really ready: it installs a C library into the default image from the
 * seed the ISO carried (#189). That is asynchronous, so "cixd listening"
 * arrives well before the image is runnable -- waiting on this marker
 * instead is what makes the persistence proof below examine a finished
 * box rather than one still working. Printed to the console
 * deliberately: on a first boot the log store is not reachable yet.
 */
#define SEED_READY_MARKER "default image: ready"

/*
 * That first boot now does real work before it is ready -- unpacking a
 * ~50 MB C library into the default image -- so it gets its own budget
 * rather than the ordinary boot timeout. This sandbox has no /dev/kvm
 * (documented), so QEMU is software-emulated and every second of real
 * work costs several here; on real hardware this is far quicker.
 *
 * A failed install does NOT consume this budget: the daemon prints
 * "default image: installing glibc FAILED" and the panic marker below
 * is not the only way this session can end badly -- a failure is
 * visible in the capture rather than looking like a hang.
 */
#define SEED_BOOT_TIMEOUT_SECONDS 1200

#define TEST_IP "192.168.50.10"
#define TEST_PREFIX 24
#define TEST_GATEWAY "192.168.50.1"
/* This project's own qemu-part1.config disables predictable network
 * interface naming, so a virtio-net device always shows up as "eth0"
 * in a fresh QEMU guest (confirmed live, not assumed -- this is the
 * exact --interface= value cix-install.c needs to pass through,
 * Part 0.5). */
#define TEST_IFACE "eth0"

/* A blank disk file of the standard test size -- shared by the real
 * GRUB-path smoke test and the main Secure Boot flow below, each
 * against its own disk file. Unpartitioned: every session in this file
 * now boots with no partition flag at all, i.e. cix-install's own scripted
 * sfdisk, image/src/cix-install.c) rather than this test pre-
 * partitioning host-side -- one source of truth for the partition
 * layout, not two copies of the same sfdisk script kept in sync by
 * hand. */
/*
 * Builds a stand-in isotools artifact from this development machine's
 * own tools, in exactly the layout isotools.recipe produces and
 * mkinstalleriso reads:
 *
 *   bin/     grub-mkrescue, sbsign, xorriso, mtools (+ mcopy/mformat),
 *            mokutil            -- PATH is set to ONLY this directory,
 *                                  so anything grub-mkrescue shells out
 *                                  to by name has to be here
 *   sbin/    mkfs.fat
 *   lib/grub/x86_64-efi/        -- grub's own module tree
 *   share/grub/                 -- grub's own data files, above all
 *                                  unicode.pf2: grub-mkrescue always
 *                                  embeds a font for the media label and
 *                                  reads it from the directory
 *                                  mkinstalleriso points "pkgdatadir" at
 *   shim/    shimx64.efi.signed, mmx64.efi.signed
 *   lib/x86_64-linux-gnu/       -- the closure staged INTO the installer
 *                                  image, measured off THIS machine's
 *                                  mokutil rather than copied from the
 *                                  Cix one, since it is this machine's
 *                                  mokutil that goes in
 *
 * Every file named here must exist: a missing one fails the test now,
 * with the path, rather than producing an ISO whose mokutil dies at
 * load time inside a QEMU boot nobody can see into.
 */
/* Creates every component of `path`, EEXIST tolerated. test_image_fixture
 * exposes no mkdir_p and the fixture needs nested directories. */
static int fixture_mkdir_p(const char *path)
{
	char buf[PATH_MAX];
	char *p;

	if ((size_t)snprintf(buf, sizeof(buf), "%s", path) >= sizeof(buf))
		return -1;
	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
			perror(buf);
			return -1;
		}
		*p = '/';
	}
	if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
		perror(buf);
		return -1;
	}
	return 0;
}

static int copy_into(const char *dir, const char *src)
{
	const char *base = strrchr(src, '/');
	char dst[PATH_MAX];

	base = base != NULL ? base + 1 : src;
	snprintf(dst, sizeof(dst), "%s/%s", dir, base);
	return test_image_fixture_copy_file(src, dst);
}

/*
 * The package seed this ISO carries (#189): the real glibc recipe out
 * of this repo, and the real artifact that recipe approves out of the
 * shared ADR-0209 floor.
 *
 * Never fabricated, for the same reason the floor states in its own
 * fixture: the daemon verifies the artifact against the recipe before
 * installing it, so a made-up artifact would prove the verification
 * works and nothing else. What this test needs to prove is that a
 * freshly installed box can run a container, which needs the real bytes.
 */
#define SEED_LIBC_VERSION TEST_FLOOR_GLIBC_VERSION

static int build_seed_fixture(const char *workdir, char *out_root, size_t out_root_len)
{
	char recipe_dir[PATH_MAX], artifact_dir[PATH_MAX];
	char src[PATH_MAX], dst[PATH_MAX];

	snprintf(out_root, out_root_len, "%s/seed", workdir);
	snprintf(recipe_dir, sizeof(recipe_dir), "%s/recipes/glibc/%s", out_root, SEED_LIBC_VERSION);
	snprintf(artifact_dir, sizeof(artifact_dir), "%s/artifacts", out_root);
	if (fixture_mkdir_p(recipe_dir) != 0 || fixture_mkdir_p(artifact_dir) != 0)
		return -1;

	snprintf(src, sizeof(src), "recipes/package/glibc/%s/build.sh", SEED_LIBC_VERSION);
	snprintf(dst, sizeof(dst), "%s/build.sh", recipe_dir);
	if (test_image_fixture_copy_file(src, dst) != 0)
		return -1;

	snprintf(src, sizeof(src), "build-inputs/floor-artifacts/glibc-%s.tar.gz", SEED_LIBC_VERSION);
	snprintf(dst, sizeof(dst), "%s/glibc-%s.tar.gz", artifact_dir, SEED_LIBC_VERSION);
	if (access(src, R_OK) != 0) {
		fprintf(stderr,
		        "the seed needs the real glibc artifact at %s -- fetch the floor artifacts "
		        "first (see ADR-0209); they are never fabricated\n",
		        src);
		return -1;
	}
	if (test_image_fixture_copy_file(src, dst) != 0)
		return -1;
	return 0;
}

/*
 * Pulls the dynamic loader out of the seeded glibc artifact, so the
 * persistence proof compares the installed box against what this ISO
 * actually shipped rather than against the machine running the test.
 */
static int extract_seed_loader(const char *workdir, char *out_path, size_t out_path_len)
{
	char dir[PATH_MAX], cmd[PATH_MAX * 2];

	snprintf(dir, sizeof(dir), "%s/seed_loader", workdir);
	if (fixture_mkdir_p(dir) != 0)
		return -1;
	snprintf(cmd, sizeof(cmd),
	         "tar xzf 'build-inputs/floor-artifacts/glibc-%s.tar.gz' -C '%s' './%s' 2>/dev/null",
	         SEED_LIBC_VERSION, dir, PKG_IMAGE_LOADER_REL);
	if (system(cmd) != 0)
		return -1;
	snprintf(out_path, out_path_len, "%s/%s", dir, PKG_IMAGE_LOADER_REL);
	return access(out_path, R_OK) == 0 ? 0 : -1;
}

static int build_isotools_fixture(const char *workdir, char *out_root, size_t out_root_len)
{
	/* mkinstalleriso sets PATH to <root>/bin alone, so grub-mkrescue's
	 * own children (xorriso, and mformat/mcopy, which it execs by
	 * literal name with no override flag) must resolve there. */
	static const char *const bin_tools[] = {
		"/usr/bin/grub-mkrescue", "/usr/bin/sbsign",  "/usr/bin/sbverify",
		"/usr/bin/xorriso",       "/usr/bin/mtools",  "/usr/bin/mcopy",
		"/usr/bin/mformat",       "/usr/bin/mokutil", NULL,
	};
	/*
	 * THIS machine's mokutil closure, not the Cix artifact's. They
	 * differ, and that difference is the whole reason this fixture
	 * exists: Debian keeps libcrypt's obsolete DES/NIS ABI and ships
	 * libcrypt.so.1, while our libxcrypt drops it and so has a
	 * different soname; Debian's mokutil links libdl where ours does
	 * not, and does not link libssl where ours does. Taken from a real
	 * `ldd /usr/bin/mokutil` on this machine, not guessed.
	 */
	static const char *const artifact_libs[] = {
		"/lib/x86_64-linux-gnu/libcrypto.so.3",   "/lib/x86_64-linux-gnu/libefivar.so.1",
		"/lib/x86_64-linux-gnu/libkeyutils.so.1", "/lib/x86_64-linux-gnu/libcrypt.so.1",
		"/lib/x86_64-linux-gnu/libdl.so.2",       NULL,
	};
	char bin_dir[PATH_MAX], sbin_dir[PATH_MAX], shim_dir[PATH_MAX];
	char lib_dir[PATH_MAX], grub_dir[PATH_MAX], artifact_lib_dir[PATH_MAX];
	int i;

	if ((size_t)snprintf(out_root, out_root_len, "%s/" ISOTOOLS_FIXTURE_DIRNAME, workdir) >=
	    out_root_len)
		return -1;

	snprintf(bin_dir, sizeof(bin_dir), "%s/bin", out_root);
	snprintf(sbin_dir, sizeof(sbin_dir), "%s/sbin", out_root);
	snprintf(shim_dir, sizeof(shim_dir), "%s/shim", out_root);
	snprintf(lib_dir, sizeof(lib_dir), "%s/lib", out_root);
	snprintf(grub_dir, sizeof(grub_dir), "%s/lib/grub/x86_64-efi", out_root);
	snprintf(artifact_lib_dir, sizeof(artifact_lib_dir), "%s/lib/x86_64-linux-gnu", out_root);

	if (fixture_mkdir_p(bin_dir) != 0 ||
	    fixture_mkdir_p(sbin_dir) != 0 ||
	    fixture_mkdir_p(shim_dir) != 0 ||
	    fixture_mkdir_p(lib_dir) != 0 ||
	    fixture_mkdir_p(artifact_lib_dir) != 0)
		return -1;

	for (i = 0; bin_tools[i] != NULL; i++) {
		if (copy_into(bin_dir, bin_tools[i]) != 0)
			return -1;
	}
	if (copy_into(sbin_dir, "/usr/sbin/mkfs.fat") != 0)
		return -1;
	if (copy_into(shim_dir, "/usr/lib/shim/shimx64.efi.signed") != 0 ||
	    copy_into(shim_dir, "/usr/lib/shim/mmx64.efi.signed") != 0)
		return -1;
	for (i = 0; artifact_libs[i] != NULL; i++) {
		if (copy_into(artifact_lib_dir, artifact_libs[i]) != 0)
			return -1;
	}

	/*
	 * grub's module tree, copied wholesale -- grub-mkrescue reads a
	 * great many of these by name and there is no useful subset.
	 * copy_dir_recursive() requires its destination NOT to exist (it is
	 * `cp -a` underneath, which nests rather than merges otherwise), so
	 * only the parent is created here.
	 */
	{
		char grub_parent[PATH_MAX];

		char share_grub[PATH_MAX];

		snprintf(grub_parent, sizeof(grub_parent), "%s/lib/grub", out_root);
		if (fixture_mkdir_p(grub_parent) != 0)
			return -1;
		if (test_image_fixture_copy_dir_recursive("/usr/lib/grub/x86_64-efi", grub_dir) != 0)
			return -1;

		/* grub's data files, which mkinstalleriso resolves through
		 * GRUB's own "pkgdatadir" variable. A real isotools artifact
		 * carries these because grub 2.14-7 generates them; this
		 * fixture stands in for one. */
		snprintf(share_grub, sizeof(share_grub), "%s/share", out_root);
		if (fixture_mkdir_p(share_grub) != 0)
			return -1;
		snprintf(share_grub, sizeof(share_grub), "%s/share/grub", out_root);
		if (test_image_fixture_copy_dir_recursive("/usr/share/grub", share_grub) != 0)
			return -1;
	}
	return 0;
}

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
 * Extracts a btrfs partition image's file tree into dest_dir with
 * `btrfs restore` -- pure userspace, no mount, no loop device (this
 * sandbox has neither). Two properties matter here and shape what the
 * checks below can honestly claim:
 *
 *   - restore reads the last COMMITTED transaction and does not replay
 *     the fsync log tree, so a write made moments before
 *     qemu_boot_capture() SIGTERMs QEMU (btrfs's default commit
 *     interval is 30s) may legitimately be absent. Everything asserted
 *     through this helper is therefore either written under a clean
 *     unmount (the installer session umounts before exiting) or given
 *     a whole subsequent boot session to reach a commit.
 *
 *   - there is no offline WRITE counterpart (debugfs -w has no btrfs
 *     equivalent), which is why the old "inject a marker between
 *     boots" proof is gone -- see the session-4 block for what
 *     replaced it.
 */
/*
 * "-s" is load-bearing: without it this recovers a faithful copy of the
 * wrong subvolume.
 *
 * `btrfs restore` skips snapshots by default, and an image version is a
 * snapshot the moment a package is installed into one --
 * image_produce_new_version() creates the new version with
 * BTRFS_IOC_SNAP_CREATE_V2 (ADR-0207 phase 1), which is a separate tree
 * root. So a default image with one version (the subvolume
 * image_create() made) restored fine, and the same image after an
 * install came back missing everything the install added, with no error
 * from restore at all -- it did exactly what it was asked.
 *
 * Found when the default image started carrying a real C library (#189):
 * the daemon printed "default image: ready" on the booted machine and
 * this extraction of that same partition then reported no runtime.
 */
static int btrfs_restore_tree(const char *image, const char *dest_dir)
{
	char *argv[] = { (char *)BTRFS_BIN, "restore", "-s", (char *)image, (char *)dest_dir, NULL };

	if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
		perror(dest_dir);
		return -1;
	}
	return run_subprocess(BTRFS_BIN, argv);
}

/* Byte-for-byte comparison, same proof shape as root A's own
 * squashfs comparison in main() below. 0 = identical. */
/*
 * ADR-0210: find the C runtime inside the daemon-created default image.
 *
 * The installer no longer seeds images/base/rootfs -- that flat path
 * predates versioned images (ADR-0107/0108) and container creation
 * stopped reading it. ensure_default_image() now materializes "base"
 * from an empty manifest at boot, so the runtime lives under a VERSION
 * directory. The version is scanned for rather than hardcoded: it is a
 * hash of the manifest, and pinning it here would make this test fail
 * for the wrong reason the day the hashing changes.
 */
static int find_default_image_runtime(const char *containers_tree, char *out, size_t out_size)
{
	char base_dir[700];
	DIR *d;
	struct dirent *e;
	int found = 0;

	snprintf(base_dir, sizeof(base_dir), "%s/rebuildable/images/base", containers_tree);
	d = opendir(base_dir);
	if (d == NULL)
		return -1;
	while (!found && (e = readdir(d)) != NULL) {
		char candidate[900];
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		snprintf(candidate, sizeof(candidate), "%s/%s/rootfs/lib64/ld-linux-x86-64.so.2",
		         base_dir, e->d_name);
		if (stat(candidate, &st) == 0) {
			snprintf(out, out_size, "%s", candidate);
			found = 1;
		}
	}
	closedir(d);
	return found ? 0 : -1;
}

static int files_identical(const char *a_path, const char *b_path)
{
	FILE *a = fopen(a_path, "rb");
	FILE *b = fopen(b_path, "rb");
	int same = a != NULL && b != NULL;

	while (same) {
		unsigned char ba[65536], bb[65536];
		size_t na = fread(ba, 1, sizeof(ba), a);
		size_t nb = fread(bb, 1, sizeof(bb), b);

		if (na != nb || memcmp(ba, bb, na) != 0)
			same = 0;
		if (na == 0)
			break;
	}
	if (a != NULL)
		fclose(a);
	if (b != NULL)
		fclose(b);
	return same ? 0 : -1;
}

int main(void)
{
	/* An input, not a build output -- says how to restore it (#191). */
	if (test_disk_image_require_kernel() != 0)
		return 1;

	char workdir[] = "/tmp/cix_test_installer_XXXXXX";
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
	snprintf(control_plane_squashfs, sizeof(control_plane_squashfs), "%s/cixd-root.squashfs", workdir);
	snprintf(installer_stage, sizeof(installer_stage), "%s/installer_stage", workdir);
	snprintf(installer_iso, sizeof(installer_iso), "%s/installer.iso", workdir);
	snprintf(target_disk_img, sizeof(target_disk_img), "%s/target_disk.img", workdir);
	snprintf(ovmf_vars, sizeof(ovmf_vars), "%s/OVMF_VARS.fd", workdir);

	/* 1. The payload: cixd's own control-plane squashfs, unchanged
	 * from parts 1-2 except now also carrying web/ (cixd's own
	 * DEFAULT_WEB_ROOT is a relative path, resolved against PID 1's own
	 * CWD -- never staged before, so the installed system's dashboard
	 * 404'd on every request despite the REST API working fine; found
	 * live, after a real install). */
	{
		char *mkbootroot_argv[] = { (char *)MKBOOTROOT_BIN, stage_dir, (char *)CIXD_BIN,
			                     (char *)CIXCTL_BIN, "web", control_plane_squashfs,
			                     "", /* no real GPU firmware needed for a boot test */
			                     "", /* no kernel modules needed for a boot test */
			                     "", /* no kmod tools needed for a boot test */
			                     "", /* no host-tools image needed for a boot test */
			                     NULL };
		if (run_subprocess(MKBOOTROOT_BIN, mkbootroot_argv) != 0)
			return 1;
	}

	/* 2. The real, distributable installer .iso -- the same tool and the
	 * same kind of artifact an operator would actually use, just with
	 * real test values; partitioning is now the default, standing in for a real
	 * operator's own --disk=/--ip=/... choice at the GRUB boot-menu edit
	 * prompt (the default path itself -- cix-install's own scripted
	 * sfdisk, added as a convenience once typing the fixed fdisk sequence
	 * by hand for every VM/scripted install proved to be pure friction --
	 * gets exercised for real right here, this session's own install). */
	snprintf(kernel_args, sizeof(kernel_args),
	         "--disk=/dev/vda --ip=%s --prefix=%d --gateway=%s --interface=%s "
	         "--enroll-key=always",
	         TEST_IP, TEST_PREFIX, TEST_GATEWAY, TEST_IFACE);
	{
		char isotools_root[PATH_MAX];
		char seed_root[PATH_MAX];
		char *mkiso_argv[] = { (char *)MKINSTALLERISO_BIN, installer_stage,
			                (char *)CIX_INSTALL_BIN,    (char *)CIX_RECOVER_BIN,
			                (char *)CIX_BOOT_BIN,
			                (char *)BZIMAGE_PATH,          control_plane_squashfs,
			                (char *)SIGNING_KEY,           (char *)SIGNING_CERT_PEM,
			                (char *)SIGNING_CERT_DER,      installer_iso,
			                kernel_args,                   isotools_root,
			                seed_root,
			                /*
			                 * #429 follow-on: the kernel module tree and
			                 * the module tools. Both "" here -- this
			                 * fixture has neither, and mkinstalleriso
			                 * treats their absence as "stage nothing"
			                 * rather than as a failure, so the media it
			                 * builds is exactly what it was before plus
			                 * two explicit "none" answers.
			                 *
			                 * Passed rather than omitted because the
			                 * callee checks its argument count exactly.
			                 * A caller one argument short is how every
			                 * POST /v1/system/iso on a real host once
			                 * answered with a fragment of usage text.
			                 */
			                (char *)"",                    (char *)"",
			                NULL };

		if (build_isotools_fixture(workdir, isotools_root, sizeof(isotools_root)) != 0) {
			fprintf(stderr, "could not build the isotools fixture\n");
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
		/* The package seed (#189): the real glibc recipe and the real
		 * artifact it approves, so the installed box has a package
		 * source before it has a network. Both come from this repo and
		 * the shared floor -- never fabricated, same rule the ADR-0209
		 * floor fixture states for itself. */
		if (build_seed_fixture(workdir, seed_root, sizeof(seed_root)) != 0) {
			fprintf(stderr, "could not build the package seed fixture\n");
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
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
	 * cix-install itself writes (already covered by session 1). */
	{
		char grub_smoke_disk[600], grub_smoke_vars[600];
		struct qemu_boot_opts opts;
		/*
		 * Kept, but it usually does not fire any more (#271).
		 *
		 * cix-install now enrols the MOK only when the firmware is
		 * actually ENFORCING Secure Boot, and this boot uses plain
		 * OVMF_VARS, which does not. The entries stay because scripted
		 * input is prompt-driven -- an entry whose prompt never appears
		 * costs nothing -- and because a secure-boot OVMF variant here
		 * would need them again immediately.
		 */
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
		if (outcome != QEMU_BOOT_SUCCESS || strstr(captured, "Cix Install") == NULL ||
		    strstr(captured, "will be executed automatically") == NULL) {
			fprintf(stderr,
			        "real GRUB-path boot did not show a menu/countdown or complete "
			        "(outcome=%d)\n",
			        (int)outcome);
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
	}

	/* An interactive-fdisk smoke test used to sit here, covering
	 * cix-install's default partitioning path when that path WAS an
	 * interactive fdisk session: a full scripted fdisk run under QEMU,
	 * driving a GPT label, five partitions, a type change and five
	 * expert-menu partition names through piped stdin. It earned its
	 * keep -- it was written after that path shipped a real bug
	 * (cfdisk's terminfo database was never staged, so it failed
	 * outright with "Error opening terminal: linux." on a real
	 * install), and switching cfdisk to fdisk was what made the path
	 * scriptable enough to test at all.
	 *
	 * ADR-0214 removed the interactive path, so its test goes with it.
	 * Keeping the feature in order to keep its own test would have been
	 * circular -- the test was never evidence that the feature was
	 * needed, only that it worked. What the path could actually vary,
	 * given find_partition_device()'s fixed five-name read-back, was
	 * partition SIZES; the scripted layout sizes itself from the disk
	 * and leaves the remainder unallocated for the REST partition API,
	 * which is a better answer to the same need. Session 1 below covers
	 * the partitioning path that remains. */
	/* 5. Target disk: blank -- session 1's own default partitioning (kernel_args
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
	 * "Secure Boot off for this one boot" step -- cix-install's own
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
		         "console=ttyS0 root=/dev/sr0 rootfstype=iso9660 ro init=/bin/cix-install "
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

		/*
		 * Issue #104: and the disk is NOT fully consumed. The installer
		 * used to end the data partition at the last usable sector,
		 * which decided how the whole machine's storage was carved up
		 * before its owner had said anything. What is left is the
		 * admin's, and this asserts it is really there rather than
		 * taking the layout's word for it.
		 */
		{
			long long total_sec = TARGET_DISK_SIZE_BYTES / 512;
			long long used_end_sec = containers_start_sec + containers_size_sec;
			long long free_sec = total_sec - used_end_sec;

			/* 34 sectors of secondary GPT sit at the very end; anything
			 * beyond that is genuinely unallocated space. */
			if (free_sec < 34 + 2048) {
				fprintf(stderr,
				        "FAIL: the installer left only %lld sectors unallocated -- a fresh "
				        "install must not claim the whole disk (issue #104)\n",
				        free_sec);
				return 1;
			}
			printf("layout leaves %lld MiB unallocated for the operator\n",
			       (free_sec - 34) / 2048);
		}
	}

	{
		char drive_arg[700];
		char listing[4096];
		char *mdir_argv[] = { "/usr/bin/mdir", "-i", drive_arg, "::/loader/entries", NULL };

		snprintf(drive_arg, sizeof(drive_arg), "%s@@%ld", target_disk_img, esp_start_sec * SECTOR_SIZE);
		if (run_subprocess_capture("/usr/bin/mdir", mdir_argv, listing, sizeof(listing)) != 0 ||
		    strstr(listing, "CIX") == NULL) {
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
		/* Config partition (btrfs --mixed since ADR-0207 phase 4) --
		 * `btrfs restore` works on a standalone filesystem image
		 * starting at byte 0, so extract it first; still no mount
		 * anywhere in this test harness. The installer umounted this
		 * partition cleanly, so its content is fully committed and
		 * restore's committed-transaction view is complete. */
		char config_extract[600];
		char config_tree[600];
		char net_conf_path[700];
		char net_conf[256];

		snprintf(config_extract, sizeof(config_extract), "%s/config_extract.img", workdir);
		snprintf(config_tree, sizeof(config_tree), "%s/config_tree", workdir);
		if (extract_partition(target_disk_img, config_start_sec * SECTOR_SIZE,
		                       config_size_sec * SECTOR_SIZE, config_extract) != 0 ||
		    btrfs_restore_tree(config_extract, config_tree) != 0) {
			ok = 0;
		} else {
			FILE *f;
			size_t n = 0;

			snprintf(net_conf_path, sizeof(net_conf_path), "%s/net.conf", config_tree);
			f = fopen(net_conf_path, "r");
			if (f != NULL) {
				n = fread(net_conf, 1, sizeof(net_conf) - 1, f);
				fclose(f);
			}
			net_conf[n] = '\0';
			if (f != NULL && strstr(net_conf, TEST_IP) != NULL &&
			    strstr(net_conf, TEST_GATEWAY) != NULL) {
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

	/*
	 * 7b. The interactive path -- what a shipped ISO actually does.
	 *
	 * Everything above installs the way an UNATTENDED install does:
	 * every answer handed over on the kernel command line. That is no
	 * longer the default. A shipped ISO passes cix-install nothing and
	 * it asks (#271), so the path every operator takes was the one path
	 * with no coverage at all.
	 *
	 * No second .iso is needed: the install boots via direct_kernel with
	 * arguments this test composes, so "an ISO that passes no installer
	 * arguments" is simply an empty argument string here.
	 *
	 * Its own blank disk, so nothing above is disturbed and the answers
	 * are unambiguous -- the CD-ROM is read-only and cix-install filters
	 * read-only media out of the disk list, so exactly one disk is
	 * offered and "1" is the whole answer.
	 *
	 * Deliberately no --enroll-key here, unlike session 1: this boot does
	 * not enforce Secure Boot, so the default "auto" must SKIP the MOK
	 * password entirely. If that regressed, the scripted answers would
	 * run out at a password prompt and this would time out rather than
	 * pass quietly.
	 */
	{
		char interactive_disk[600], interactive_vars[600], interactive_args[600];
		struct qemu_boot_opts opts;
		struct qemu_scripted_input answers[] = {
			{ "which disk?", "1\n" },
			{ "Type ERASE to confirm", "ERASE\n" },
			{ "Management interface", TEST_IFACE "\n" },
			{ "Management IP address", TEST_IP "\n" },
			{ "Prefix length", "24\n" },
			{ "Default gateway", TEST_GATEWAY "\n" },
		};

		snprintf(interactive_disk, sizeof(interactive_disk), "%s/interactive_disk.img", workdir);
		snprintf(interactive_vars, sizeof(interactive_vars), "%s/interactive_vars.fd", workdir);
		if (create_blank_disk(interactive_disk) != 0) {
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
		if (test_image_fixture_copy_file("/usr/share/OVMF/OVMF_VARS_4M.fd", interactive_vars) !=
		    0) {
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}

		/* No "-- <args>" at all: exactly what a shipped ISO boots. */
		snprintf(interactive_args, sizeof(interactive_args),
		         "console=ttyS0 root=/dev/sr0 rootfstype=iso9660 ro init=/bin/cix-install");

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = installer_iso;
		opts.disk_img_is_cdrom = 1;
		opts.disk_img2 = interactive_disk;
		opts.direct_kernel = BZIMAGE_PATH;
		opts.direct_kernel_args = interactive_args;
		opts.ovmf_vars = interactive_vars;
		opts.success_marker = INSTALL_SUCCESS_MARKER;
		opts.timeout_seconds = INSTALL_TIMEOUT_SECONDS;
		opts.scripted_input = answers;
		opts.n_scripted_input = (int)(sizeof(answers) / sizeof(answers[0]));
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));

		if (outcome != QEMU_BOOT_SUCCESS) {
			fprintf(stderr, "interactive install did not report success (outcome=%d)\n",
			        (int)outcome);
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
		/*
		 * Success alone would not prove the PROMPTS ran -- an installer
		 * that silently defaulted everything would also print the
		 * success marker. These assert the operator was actually asked,
		 * and asked about the right disk.
		 */
		if (strstr(captured, "Disks on this machine:") == NULL) {
			fprintf(stderr, "interactive install never listed the machine's disks\n");
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
		if (strstr(captured, "will be COMPLETELY ERASED") == NULL) {
			fprintf(stderr, "interactive install never asked for erase confirmation\n");
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
		/*
		 * The CD-ROM must never be offered as somewhere to install.
		 *
		 * Matched against the LIST ENTRY form -- ") /dev/sr0" -- and not
		 * a bare "/dev/sr0", which appears in this same capture in the
		 * kernel command line (root=/dev/sr0) and in the kernel's own
		 * messages. The looser check failed the first real run of this
		 * test against an installer that was behaving correctly.
		 */
		if (strstr(captured, ") /dev/sr0") != NULL) {
			fprintf(stderr, "the installer offered read-only media as an install target\n");
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
		/* And with Secure Boot not enforcing, no password may be asked. */
		if (strstr(captured, "Secure Boot is not enforcing") == NULL) {
			fprintf(stderr, "MOK enrollment was not skipped on non-enforcing firmware\n");
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
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
	 * genuinely working cixd that actually bootstrapped its
	 * management network from the static IP/gateway/interface configured at
	 * install time (Part 0.5). Nothing here is test-built; this is
	 * exactly what an operator would see after rebooting a freshly
	 * installed, MOK-confirmed machine -- shim -> the Cix-signed
	 * grubx64.efi (systemd-boot) -> the Cix-signed kernel, all now
	 * trusted, zero further exceptions. */
	{
		struct qemu_boot_opts opts;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = target_disk_img;
		opts.secure_boot = 1;
		opts.with_nic = 1;
		opts.ovmf_vars = ovmf_vars;
		opts.success_marker = SEED_READY_MARKER;
		opts.panic_marker = "Kernel panic";
		opts.timeout_seconds = SEED_BOOT_TIMEOUT_SECONDS;
		outcome = qemu_boot_capture(&opts, captured, sizeof(captured));
	}
	if (outcome != QEMU_BOOT_SUCCESS) {
		fprintf(stderr, "installed system did not boot successfully (outcome=%d)\n", (int)outcome);
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}
	if (strstr(captured, "init-mode: management network") == NULL || strstr(captured, TEST_IP) == NULL ||
	    strstr(captured, TEST_GATEWAY) == NULL || strstr(captured, TEST_IFACE) == NULL) {
		fprintf(stderr, "installed system booted but never bootstrapped its management network\n");
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}

	/* 10. The persistence proof (ADR-0018), btrfs edition. BASE_DIR
	 * (/var/lib/cix) must be the real cix-containers partition
	 * boot_init() mounts, not a fresh tmpfs -- proven by reading the
	 * real on-disk partition, never by believing a boot succeeded.
	 *
	 * The old ext4 form of this proof injected a marker file into the
	 * extracted partition with debugfs -w between two boots; btrfs has
	 * no offline write tool, and `btrfs restore` reads only committed
	 * transactions (a write made moments before qemu_boot_capture()
	 * SIGTERMs QEMU can be legitimately absent -- and, being lost from
	 * the guest page cache, it never appears later either). So every
	 * assertion here relies only on data with a deterministic path to
	 * disk: content the installer wrote under a clean umount, and
	 * session-3 writes old enough (cixd's first-boot init, minutes of
	 * TCG guest time before the success marker) to have crossed
	 * btrfs's 30s commit interval. What this proves is the property
	 * that matters: installer-seeded content survives two real,
	 * independent boots byte-identical (no tmpfs, no reformat, no
	 * corruption), and cixd's own first-boot writes landed on the
	 * real partition (the grouped layout it migrated to is what the
	 * raw disk shows afterwards). */
	{
		char containers_extract[600];
		char containers_tree[600];
		struct stat pst;
		/*
		 * Truth is the loader inside the artifact this ISO seeded, not
		 * the build host's own (#189/ADR-0216).
		 *
		 * This used to point at /usr/lib/x86_64-linux-gnu on the
		 * machine running the test, which made the assertion a
		 * contamination check pointing exactly the wrong way: it
		 * passed only while every image carried a runtime copied off
		 * the build host, and that is the thing that was removed.
		 * Extracted once, from the same artifact the seed carries.
		 */
		char ld_so_src[PATH_MAX];

		if (extract_seed_loader(workdir, ld_so_src, sizeof(ld_so_src)) != 0) {
			fprintf(stderr, "could not extract the seeded artifact's own loader\n");
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
		snprintf(containers_extract, sizeof(containers_extract), "%s/containers_extract.img",
		         workdir);
		snprintf(containers_tree, sizeof(containers_tree), "%s/containers_tree_s3", workdir);

		if (extract_partition(target_disk_img, containers_start_sec * SECTOR_SIZE,
		                       containers_size_sec * SECTOR_SIZE, containers_extract) != 0 ||
		    btrfs_restore_tree(containers_extract, containers_tree) != 0) {
			fprintf(stderr, "could not extract containers partition after session 3\n");
			ok = 0;
		} else {
			/* ADR-0210: the default image is created by the DAEMON at
			 * boot (ensure_default_image()), not written by the
			 * installer, so what is asserted here is that the first
			 * real boot produced a usable "base" -- a version
			 * directory carrying the baseline C runtime. This is the
			 * property that actually matters and the one the old
			 * installer-seeding check never established: it proved
			 * bytes had been written to images/base/rootfs, a path
			 * container creation stopped reading when images became
			 * versioned. */
			char runtime_path[900];

			if (find_default_image_runtime(containers_tree, runtime_path,
			                               sizeof(runtime_path)) != 0) {
				fprintf(stderr,
				        "no default-image runtime on the containers partition after the "
				        "first boot -- ensure_default_image() did not materialize \"base\"\n");
				ok = 0;
			} else if (files_identical(runtime_path, ld_so_src) != 0) {
				fprintf(stderr,
				        "the default image's ld.so does not match the build host source "
				        "after session 3 -- content corrupted on the way to disk\n");
				ok = 0;
			} else {
				printf("first boot materialized the default image with its baseline "
				       "runtime, byte-identical to source\n");
			}
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
		 * cixd's kernel command line unconditionally, on every boot
		 * of this disk -- omitting the NIC here means that address is
		 * never actually assigned to any interface, so cixd's own
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

		snprintf(containers_tree, sizeof(containers_tree), "%s/containers_tree_s4", workdir);
		if (extract_partition(target_disk_img, containers_start_sec * SECTOR_SIZE,
		                       containers_size_sec * SECTOR_SIZE, containers_extract) != 0 ||
		    btrfs_restore_tree(containers_extract, containers_tree) != 0) {
			fprintf(stderr, "could not extract containers partition after session 4\n");
			ok = 0;
		} else {
			char dir_path[700];

			/* Session 3's own first-boot writes, now stably on disk
			 * (they had session 3's whole multi-minute init runtime to
			 * commit; session 4 does not undo them): the grouped
			 * layout's non-empty top-level directories, and the seeded
			 * runtime at its migrated, grouped path -- still
			 * byte-identical to the build host source after TWO real,
			 * independent boots. CONTAINERS_DIR itself is deliberately
			 * not asserted: it is empty (no containers exist in this
			 * test), and `btrfs restore`'s handling of an empty
			 * directory is not a contract this test should depend on. */
			snprintf(dir_path, sizeof(dir_path), "%s/state", containers_tree);
			if (stat(dir_path, &pst) != 0) {
				fprintf(stderr, "state/ missing from the raw containers partition after "
				                "session 4 -- BASE_DIR was not the real partition\n");
				ok = 0;
			}
			snprintf(dir_path, sizeof(dir_path), "%s/rebuildable", containers_tree);
			if (stat(dir_path, &pst) != 0) {
				fprintf(stderr, "rebuildable/ missing from the raw containers partition "
				                "after session 4 -- BASE_DIR was not the real partition\n");
				ok = 0;
			}
			{
				char runtime_path[900];

				if (find_default_image_runtime(containers_tree, runtime_path,
				                               sizeof(runtime_path)) != 0) {
					fprintf(stderr, "the default image is missing after session 4\n");
					ok = 0;
				} else if (files_identical(runtime_path, ld_so_src) != 0) {
					fprintf(stderr,
					        "the default image's ld.so changed across a real, independent "
					        "second boot\n");
					ok = 0;
				} else {
					printf("the default image survived two real, independent boots "
					       "byte-identical -- BASE_DIR genuinely persists across reboots\n");
				}
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
