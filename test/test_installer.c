/*
 * Phase 11 part 4/5 demonstrable test: proves the actual installer *and*
 * its real, distributable .iso packaging both work -- given a raw,
 * blank target disk, cix-install
 * (booted from the real .iso build/mkinstalleriso produces, via QEMU's
 * -cdrom, not a test-only disk-image approximation) partitions its role-
 * detection, formats, writes the real payload, and configures a static
 * IP -- then more, completely separate boots prove the freshly-installed
 * disk actually comes up as a real, running cixd with its management
 * network up. "Install" and "boot what was
 * installed" are the same real code paths already proven in parts 1-2,
 * and "the ISO an operator would actually use" is now the same artifact
 * this test boots, not a separate, untested approximation of it (part 3's
 * own test built a private squashfs-based disk instead -- this part
 * replaces that with the real thing).
 *
 * Five QEMU sessions, none with Secure Boot (cix#647):
 *   1. GRUB smoke: the real .iso through its own grub-mkrescue-built
 *      BOOTX64.EFI and menu, the way an operator boots it.
 *   2. The unattended install: the .iso plus a blank target disk,
 *      partitioned by cix-install's own scripted sfdisk. Since the boot
 *      medium is a CD-ROM (a separate ATAPI/SCSI bus), the target disk
 *      is the only virtio-blk device present and is /dev/vda. Booted via
 *      direct_kernel (QEMU's "-kernel"), so the installer's arguments
 *      are this test's.
 *   3. The interactive install a shipped ISO does: no arguments, every
 *      answer typed at a prompt, on its own blank disk.
 *   4. The target disk, booted alone through what cix-install wrote
 *      (shim -> cix-boot.efi -> the kernel), to "default image: ready"
 *      with its management network up from the installed net.conf.
 *   5. The same disk again, an independent boot, for persistence.
 *
 * The Secure Boot sessions -- confirming the MOK enrollment in shim's
 * MokManager, then booting the signed chain with Secure Boot enforced --
 * need an OVMF built with SECURE_BOOT_ENABLE=TRUE and a vars template
 * holding a platform key. Cix's ovmf@202608-1 is neither, so they return
 * with that build (cix#647). The signing pair is a throwaway made per
 * run (make_signing_pair()), so the test needs no release key.
 *
 * Verification of what the installer actually wrote happens from the
 * host side after session 2 -- the ESP via mtools' disk.img@@offset
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
/* Where Cix's util-linux and btrfs-progs install them: btrfs-progs@7.1-12
 * ships usr/sbin/btrfs (GET /v1/pkg/btrfs-progs@cix-hosttools on
 * 192.168.15.95, 2026-10-26), not Debian's usr/bin. */
#define SFDISK_BIN "/usr/sbin/sfdisk"
#define BTRFS_BIN "/usr/sbin/btrfs"
/* Cix's ovmf package, the same firmware test_disk_image.c boots
 * (OVMF_CODE there). Every session here starts from a fresh copy. */
#define OVMF_VARS_TEMPLATE "/usr/share/OVMF/OVMF_VARS.fd"
#define OPENSSL_BIN "/usr/bin/openssl"
/*
 * ADR-0358: mkinstalleriso's tools root is a rootfs with the ISO
 * toolchain's packages installed -- the iso-builder image's on a host.
 * Here it is "/": the composed build container this test runs in holds
 * those packages as declared tools, which is the same answer this test
 * gives mkbootroot for its host-tools root. It used to build a stand-in
 * from a Debian machine's grub, shim and mokutil, which no Cix host has.
 */
#define ISOTOOLS_ROOT "/"


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

	/* #505: the corpus is its own repository and flat. */
	if (test_recipe_path("package", "glibc", SEED_LIBC_VERSION, "cbs", src, sizeof(src)) != 0)
		return -1;
	snprintf(dst, sizeof(dst), "%s/build.cbs", recipe_dir);
	if (test_image_fixture_copy_file(src, dst) != 0)
		return -1;

	snprintf(src, sizeof(src), "build-inputs/floor-artifacts/glibc-%s.cixpkg", SEED_LIBC_VERSION);
	snprintf(dst, sizeof(dst), "%s/glibc-%s.cixpkg", artifact_dir, SEED_LIBC_VERSION);
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
	/* `cbs extract` reads a .cixpkg (ADR-0307) and refuses a
	 * destination that exists, so the tree goes one level down. */
	snprintf(cmd, sizeof(cmd),
	         "cbs extract 'build-inputs/floor-artifacts/glibc-%s.cixpkg' --into '%s/tree' "
	         ">/dev/null 2>&1",
	         SEED_LIBC_VERSION, dir);
	if (system(cmd) != 0)
		return -1;
	snprintf(out_path, out_path_len, "%s/tree/%s", dir, PKG_IMAGE_LOADER_REL);
	return access(out_path, R_OK) == 0 ? 0 : -1;
}

/*
 * A signing pair made for this run alone: the key, its certificate as
 * PEM for sbsign, and as DER for the MOK enrollment the ISO carries.
 * No session here enforces Secure Boot (cix#647), so nothing checks
 * what signed the chain, and a test must never need the release key --
 * image/keys/cix-signing.key is gitignored and absent from every
 * checkout but the owner's.
 */
static int make_signing_pair(const char *workdir, char *key, char *crt, char *cer, size_t size)
{
	char *req_argv[] = { (char *)OPENSSL_BIN, "req", "-new", "-x509", "-newkey", "rsa:2048",
		             "-nodes", "-sha256", "-days", "1", "-subj", "/CN=cix test_installer",
		             "-keyout", key, "-out", crt, NULL };
	char *der_argv[] = { (char *)OPENSSL_BIN, "x509", "-in", crt, "-outform", "DER", "-out", cer,
		             NULL };

	snprintf(key, size, "%s/signing.key", workdir);
	snprintf(crt, size, "%s/signing.crt", workdir);
	snprintf(cer, size, "%s/signing.cer", workdir);
	if (run_subprocess(OPENSSL_BIN, req_argv) != 0 || run_subprocess(OPENSSL_BIN, der_argv) != 0) {
		fprintf(stderr, "could not make a signing pair with %s\n", OPENSSL_BIN);
		return -1;
	}
	return 0;
}

/* A blank, unpartitioned disk of the standard size: every install here
 * partitions with cix-install's own scripted sfdisk, the one source of
 * the layout. */
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

	/*
	 * Under $TMPDIR when it is set. This test writes three disk images
	 * and extracts partitions of gigabytes, and a build container's /tmp
	 * is a tmpfs that counts against the shared build memory budget:
	 * on 192.168.15.95, 2026-10-26, the memory cgroup cix-pkgbuild
	 * OOM-killed `btrfs restore` as it extracted the containers
	 * partition (probe-cix-instest@1-3).
	 */
	char workdir[PATH_MAX];
	const char *tmpdir = getenv("TMPDIR");

	snprintf(workdir, sizeof(workdir), "%s/cix_test_installer_XXXXXX",
	         tmpdir != NULL && tmpdir[0] != '\0' ? tmpdir : "/tmp");
	char signing_key[PATH_MAX], signing_crt[PATH_MAX], signing_cer[PATH_MAX];
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
			                     /*
			                      * The host-tools root. "" used to mean "not needed for a boot
			                      * test"; mkbootroot refuses it now, because a root with none of
			                      * the programs the control plane executes cannot install a
			                      * package or assemble its own successor (cix#350, and ADR-0307
			                      * clause 6 before it for cbs alone). "/" is the truthful
			                      * answer: ADR-0078 defines this argument as an image's
			                      * installed rootfs, and the composed build container this test
			                      * runs in IS one -- see test_boot.c, where the same change
			                      * carries the full reasoning.
			                      */
			                     "/",
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
	/*
	 * No --enroll-key: the default, "auto", enrols the MOK only on
	 * firmware enforcing Secure Boot, and none here does (cix#647).
	 * Forcing it would leave a MokNew request that shim acts on whatever
	 * the firmware enforces, putting MokManager in front of session 4.
	 */
	snprintf(kernel_args, sizeof(kernel_args),
	         "--disk=/dev/vda --ip=%s --prefix=%d --gateway=%s --interface=%s", TEST_IP,
	         TEST_PREFIX, TEST_GATEWAY, TEST_IFACE);
	if (make_signing_pair(workdir, signing_key, signing_crt, signing_cer, PATH_MAX) != 0) {
		printf("INSTALLER RESULT: FAIL\n");
		return 1;
	}
	{
		char seed_root[PATH_MAX];
		char *mkiso_argv[] = { (char *)MKINSTALLERISO_BIN, installer_stage,
			                (char *)CIX_INSTALL_BIN,    (char *)CIX_RECOVER_BIN,
			                (char *)CIX_BOOT_BIN,
			                (char *)BZIMAGE_PATH,          control_plane_squashfs,
			                signing_key,                   signing_crt,
			                signing_cer,                   installer_iso,
			                kernel_args,                   (char *)ISOTOOLS_ROOT,
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
			                /*
			                 * The GPT disk GUID (#430). cixd derives
			                 * this from the release it is building;
			                 * a fixture has no release, so it passes
			                 * a fixed one shaped the way the contract
			                 * requires -- 8-4-4-4-12 lowercase hex
			                 * whose fourth group ends in a byte low
			                 * enough to add a partition number to.
			                 * The repeated nibbles mirror the probe
			                 * GUID that measured the mechanism on
			                 * 192.168.15.95, so a raw dump of this
			                 * fixture's media reads the same way the
			                 * measurements in #430 do.
			                 *
			                 * mkinstalleriso writes the ISO9660
			                 * partition's PARTUUID into the USB boot
			                 * entries and then re-reads the finished
			                 * image to check it is really there, so
			                 * this value is also what that gate
			                 * compares against.
			                 */
			                (char *)"11111111-2222-3333-4400-555555555555",
			                NULL };

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
	 * direct_kernel boot session 2 below uses to pass its own arguments.
	 * Its own fresh vars copy; the installer media itself is never
	 * signed (ADR-0015). This is the ONLY place this test suite exercises
	 * the real GRUB menu/config path at all -- it's what would have
	 * caught a genuine `timeout=0` bug (the menu booted instantly, with
	 * no visible window to press 'e' and edit --disk=/--ip=/...) that
	 * shipped and only surfaced during a real operator's own install.
	 * Asserts the menu text actually renders and a countdown genuinely
	 * runs (not an instant, uninterruptible auto-boot) before the
	 * default entry boots and completes -- a throwaway disk, since this
	 * is only proving the boot *mechanism*, not re-verifying what
	 * cix-install itself writes (already covered by session 2). */
	{
		char grub_smoke_disk[600], grub_smoke_vars[600];
		struct qemu_boot_opts opts;

		snprintf(grub_smoke_disk, sizeof(grub_smoke_disk), "%s/grub_smoke_disk.img", workdir);
		snprintf(grub_smoke_vars, sizeof(grub_smoke_vars), "%s/grub_smoke_vars.fd", workdir);
		if (create_blank_disk(grub_smoke_disk) != 0)
			return 1;
		if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, grub_smoke_vars) != 0)
			return 1;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = installer_iso;
		opts.disk_img_is_cdrom = 1;
		opts.disk_img2 = grub_smoke_disk;
		opts.ovmf_vars = grub_smoke_vars;
		opts.success_marker = INSTALL_SUCCESS_MARKER;
		opts.timeout_seconds = INSTALL_TIMEOUT_SECONDS;
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
	/* 5. Target disk: blank -- session 2's own default partitioning (kernel_args
	 * above) partitions it, same as a real install would. */
	if (create_blank_disk(target_disk_img) != 0)
		return 1;

	if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, ovmf_vars) != 0)
		return 1;

	/* 6. Session 2: boot the real .iso via -cdrom against the target
	 * disk. No NIC needed -- formatting/writing doesn't touch the
	 * network. A completed install ends with init exiting -- an
	 * expected, harmless "Attempted to kill init!" panic, not a
	 * failure, hence no panic_marker here.
	 *
	 * direct_kernel (QEMU's "-kernel" fw_cfg injection) boots the
	 * installer with this test's arguments; the GRUB path above already
	 * proved the media boots on its own. The vars file is session 4's
	 * and 5's too, so the boot entries the installer writes are the ones
	 * those sessions boot from. */
	{
		struct qemu_boot_opts opts;
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
	 * No --enroll-key here either: this boot does not enforce Secure
	 * Boot, so the default "auto" must SKIP the MOK password entirely. If
	 * that regressed, the scripted answers would run out at a password
	 * prompt and this would time out rather than pass quietly.
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
		if (test_image_fixture_copy_file(OVMF_VARS_TEMPLATE, interactive_vars) != 0) {
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

	/* 8. Session 4: the end-to-end proof -- boot the target disk alone,
	 * with a NIC attached this time, and confirm it comes up as a working
	 * cixd that bootstrapped its management network from the static
	 * IP/gateway/interface configured at install time (Part 0.5). Nothing
	 * here is test-built: it is the chain cix-install wrote, shim ->
	 * cix-boot.efi (installed as grubx64.efi) -> the kernel, which is
	 * what an operator's machine boots after the install. Secure Boot is
	 * not enforced (cix#647), so nothing checks the signatures on it. */
	{
		struct qemu_boot_opts opts;

		memset(&opts, 0, sizeof(opts));
		opts.disk_img = target_disk_img;
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
	/*
	 * What cixd's init mode prints once the address is up (main.c,
	 * "init-mode: management address %s on network %s%s"), and the
	 * kernel's line for the interface joining that network's bridge.
	 * The line ends in " (no default route)" when net.conf names no
	 * gateway, so its absence is the gateway's evidence. This used to
	 * look for "init-mode: management network", which cixd no longer
	 * prints, and for TEST_GATEWAY anywhere, which "192.168.50.10"
	 * already contains.
	 */
	{
		const char *addr_line =
		        strstr(captured, "init-mode: management address " TEST_IP " on network ");
		const char *eol = addr_line != NULL ? strchr(addr_line, '\n') : NULL;
		const char *no_route = addr_line != NULL ? strstr(addr_line, "(no default route)") : NULL;

		if (addr_line == NULL || (no_route != NULL && (eol == NULL || no_route < eol)) ||
		    strstr(captured, "(" TEST_IFACE ") entered forwarding state") == NULL) {
			fprintf(stderr,
			        "installed system booted but never bootstrapped its management network\n");
			printf("INSTALLER RESULT: FAIL\n");
			return 1;
		}
	}

	/* 9. The persistence proof (ADR-0018), btrfs edition. BASE_DIR
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
	 * session-4 writes old enough (cixd's first-boot init, minutes of
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
			fprintf(stderr, "could not extract containers partition after session 4\n");
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
				        "after session 4 -- content corrupted on the way to disk\n");
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

		/*
		 * Session 5: the same target disk, a completely independent
		 * QEMU process/boot, with the same ovmf_vars.
		 *
		 * The NIC is kept because a realistic boot of this disk has
		 * one: net.conf names an address that lives on it. It is no
		 * longer REQUIRED, and the reason it used to be is worth
		 * keeping, because it described a real defect that was sitting
		 * here in a comment while a live box was lost to it.
		 *
		 * It read: "the ESP's own loader entry bakes in --bind=<the
		 * configured static IP> on cixd's kernel command line
		 * unconditionally, on every boot of this disk -- omitting the
		 * NIC here means that address is never actually assigned to
		 * any interface, so cixd's own bind() fails and PID 1 exits,
		 * panicking the kernel (found directly by first omitting it
		 * here)."
		 *
		 * Both halves have since been fixed, in the wrong order. #131
		 * replaced the panic with a deliberate park, so the symptom
		 * stopped looking like a kernel bug while remaining fatal. Then
		 * ADR-0284 made a bind failure non-fatal outright (cixd always
		 * binds loopback, so it keeps its console) and #443 removed the
		 * --bind= from the entry altogether, so net.conf is the one
		 * place the address lives.
		 */
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
			fprintf(stderr, "could not extract containers partition after session 5\n");
			ok = 0;
		} else {
			char dir_path[700];

			/* Session 4's own first-boot writes, now stably on disk
			 * (they had session 4's whole multi-minute init runtime to
			 * commit; session 5 does not undo them): rebuildable/ on the
			 * containers partition, state/ on the config partition, and
			 * the seeded runtime at its grouped path -- still
			 * byte-identical to the source after TWO real, independent
			 * boots. state/ is read from cix-config because an installed
			 * host keeps it there (STATE_DIR is CONFIG_DIR/state, #250);
			 * this used to look for it on the containers partition.
			 * CONTAINERS_DIR itself is deliberately not asserted: it is
			 * empty (no containers exist in this test), and `btrfs
			 * restore`'s handling of an empty directory is not a
			 * contract this test should depend on. */
			{
				char config_extract[600], config_tree[600];

				snprintf(config_extract, sizeof(config_extract), "%s/config_extract_s5.img",
				         workdir);
				snprintf(config_tree, sizeof(config_tree), "%s/config_tree_s5", workdir);
				snprintf(dir_path, sizeof(dir_path), "%s/state", config_tree);
				if (extract_partition(target_disk_img, config_start_sec * SECTOR_SIZE,
				                       config_size_sec * SECTOR_SIZE, config_extract) != 0 ||
				    btrfs_restore_tree(config_extract, config_tree) != 0 ||
				    stat(dir_path, &pst) != 0) {
					fprintf(stderr, "state/ missing from the raw config partition after "
					                "session 5 -- STATE_DIR was not the real partition\n");
					ok = 0;
				}
			}
			snprintf(dir_path, sizeof(dir_path), "%s/rebuildable", containers_tree);
			if (stat(dir_path, &pst) != 0) {
				fprintf(stderr, "rebuildable/ missing from the raw containers partition "
				                "after session 5 -- BASE_DIR was not the real partition\n");
				ok = 0;
			}
			{
				char runtime_path[900];

				if (find_default_image_runtime(containers_tree, runtime_path,
				                               sizeof(runtime_path)) != 0) {
					fprintf(stderr, "the default image is missing after session 5\n");
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
