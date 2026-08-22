/*
 * Phase 14 part 2 (ADR-0029) test: proves mkbootroot.c's new
 * firmware_dir argv/staging path directly against a synthetic scratch
 * directory -- a real, hundreds-of-MB linux-firmware fetch isn't
 * practical inside an automated test, mirroring test_devices.c's own
 * precedent of a structurally-correct stand-in where the real thing
 * isn't. No QEMU needed -- inspects mkbootroot's own staging directory
 * directly, before it's squashed, rather than the final squashfs
 * image.
 *
 * Part 3 (bare-metal-readiness plan, ADR-0060 family) extends this
 * same file with the analogous modules_dir/kmod_bin_dir staging path --
 * same synthetic-stand-in reasoning (a real kernel module tree needs a
 * real kernel build, not practical inside an automated test either).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define STAGE_DIR "/tmp/thinc_test_mkbootroot_stage"
#define FW_SRC_DIR "/tmp/thinc_test_mkbootroot_fwsrc"
#define MODULES_SRC_DIR "/tmp/thinc_test_mkbootroot_modsrc"
#define KMOD_BIN_SRC_DIR "/tmp/thinc_test_mkbootroot_kmodbinsrc"
#define OUT_SQUASHFS "/tmp/thinc_test_mkbootroot_out.squashfs"
#define MKBOOTROOT_BIN "build/mkbootroot"

static int run_mkbootroot2(const char *firmware_dir, const char *modules_dir,
                            const char *kmod_bin_dir)
{
	pid_t pid;
	int status;
	char *mkbootroot_argv[] = { (char *)MKBOOTROOT_BIN,   (char *)STAGE_DIR,
		                     (char *)"build/thincd",   (char *)"build/thincctl",
		                     (char *)"web",             (char *)OUT_SQUASHFS,
		                     (char *)firmware_dir,      (char *)modules_dir,
		                     (char *)kmod_bin_dir,      (char *)"",
		                     NULL };

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(MKBOOTROOT_BIN, mkbootroot_argv, environ);
		perror("execve mkbootroot");
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int run_mkbootroot(const char *firmware_dir)
{
	return run_mkbootroot2(firmware_dir, "", "");
}

static int file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int dir_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void write_fake_blob(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");

	if (f != NULL) {
		fputs(content, f);
		fclose(f);
	}
}

int main(void)
{
	int ok = 1;
	char path[512];

	system("rm -rf " STAGE_DIR " " FW_SRC_DIR " " MODULES_SRC_DIR " " KMOD_BIN_SRC_DIR " "
	       OUT_SQUASHFS);

	/* 1. firmware_dir="" (every real call site's own value) is a
	 * complete no-op for firmware specifically -- lib/ itself already
	 * exists regardless (test_image_fixture_build()/_add_lib() already
	 * stage ld.so/libc.so.6/libtinfo.so.6 there for thincd's own
	 * needs, nothing to do with firmware), so lib/firmware -- only ever
	 * created by this new staging path -- is the real marker. */
	if (run_mkbootroot("") != 0) {
		fprintf(stderr, "FAIL: mkbootroot with firmware_dir=\"\" failed\n");
		ok = 0;
	}
	snprintf(path, sizeof(path), "%s/lib/firmware", STAGE_DIR);
	if (dir_exists(path)) {
		fprintf(stderr, "FAIL: firmware_dir=\"\" created a lib/firmware dir anyway -- "
		                "should be a no-op for firmware staging specifically\n");
		ok = 0;
	}
	system("rm -rf " STAGE_DIR " " OUT_SQUASHFS);

	/* 2. a real (synthetic) firmware_dir stages its files into
	 * lib/firmware/amdgpu, verbatim, via the same copy_dir_files()
	 * web/ already proves. */
	mkdir(FW_SRC_DIR, 0755);
	snprintf(path, sizeof(path), "%s/vega10_smc.bin", FW_SRC_DIR);
	write_fake_blob(path, "fake firmware blob one");
	snprintf(path, sizeof(path), "%s/navi10_vcn.bin", FW_SRC_DIR);
	write_fake_blob(path, "fake firmware blob two");

	if (run_mkbootroot(FW_SRC_DIR) != 0) {
		fprintf(stderr, "FAIL: mkbootroot with a real firmware_dir failed\n");
		ok = 0;
	}
	snprintf(path, sizeof(path), "%s/lib/firmware/amdgpu/vega10_smc.bin", STAGE_DIR);
	if (!file_exists(path)) {
		fprintf(stderr, "FAIL: vega10_smc.bin not staged to lib/firmware/amdgpu\n");
		ok = 0;
	}
	snprintf(path, sizeof(path), "%s/lib/firmware/amdgpu/navi10_vcn.bin", STAGE_DIR);
	if (!file_exists(path)) {
		fprintf(stderr, "FAIL: navi10_vcn.bin not staged to lib/firmware/amdgpu\n");
		ok = 0;
	}
	system("rm -rf " STAGE_DIR " " OUT_SQUASHFS);

	/* 3. an explicitly-requested but unreadable firmware_dir fails
	 * loudly (nonzero exit), not silently -- unlike
	 * pkg_seed_image_baseline()'s own tolerant-if-missing-runtime-lib precedent,
	 * this was explicitly asked for. */
	if (run_mkbootroot("/nonexistent/firmware/dir") == 0) {
		fprintf(stderr, "FAIL: mkbootroot with an unreadable firmware_dir should fail, "
		                "not silently succeed\n");
		ok = 0;
	}
	system("rm -rf " STAGE_DIR " " FW_SRC_DIR " " OUT_SQUASHFS);

	/* 4. modules_dir/kmod_bin_dir="" (every existing call site's own
	 * value) is a complete no-op -- lib/modules and the kmod tool names
	 * under usr/bin are only ever created by this new staging path. */
	if (run_mkbootroot2("", "", "") != 0) {
		fprintf(stderr, "FAIL: mkbootroot with modules_dir/kmod_bin_dir=\"\" failed\n");
		ok = 0;
	}
	snprintf(path, sizeof(path), "%s/lib/modules", STAGE_DIR);
	if (dir_exists(path)) {
		fprintf(stderr, "FAIL: modules_dir=\"\" created a lib/modules dir anyway\n");
		ok = 0;
	}
	snprintf(path, sizeof(path), "%s/usr/bin/modprobe", STAGE_DIR);
	if (file_exists(path)) {
		fprintf(stderr, "FAIL: kmod_bin_dir=\"\" staged a modprobe anyway\n");
		ok = 0;
	}
	system("rm -rf " STAGE_DIR " " OUT_SQUASHFS);

	/* 5. a real (synthetic) modules_dir/kmod_bin_dir pair stages the
	 * module tree recursively (nested by kernel/drivers/..., proving
	 * test_image_fixture_copy_dir_recursive() actually walks it, unlike
	 * the flat copy_dir_files() firmware_dir above uses) and the kmod
	 * tool names into usr/bin (merged alongside the thincd/thincctl
	 * already staged there, proving the flat copy correctly dereferences
	 * "modprobe -> kmod"-style symlinks into a real, independently
	 * readable file rather than skipping them). */
	{
		char modules_dst_dir[512];

		snprintf(modules_dst_dir, sizeof(modules_dst_dir), "%s/6.18.40/kernel/drivers/net",
		         MODULES_SRC_DIR);
		system("mkdir -p " MODULES_SRC_DIR "/6.18.40/kernel/drivers/net");
		snprintf(path, sizeof(path), "%s/e1000e.ko", modules_dst_dir);
		write_fake_blob(path, "fake module blob");
		snprintf(path, sizeof(path), "%s/6.18.40/modules.dep", MODULES_SRC_DIR);
		write_fake_blob(path, "kernel/drivers/net/e1000e.ko:\n");

		mkdir(KMOD_BIN_SRC_DIR, 0755);
		snprintf(path, sizeof(path), "%s/kmod", KMOD_BIN_SRC_DIR);
		write_fake_blob(path, "fake kmod binary");
		snprintf(path, sizeof(path), "%s/modprobe", KMOD_BIN_SRC_DIR);
		symlink("kmod", path);
	}

	if (run_mkbootroot2("", MODULES_SRC_DIR, KMOD_BIN_SRC_DIR) != 0) {
		fprintf(stderr, "FAIL: mkbootroot with a real modules_dir/kmod_bin_dir failed\n");
		ok = 0;
	}
	snprintf(path, sizeof(path), "%s/lib/modules/6.18.40/kernel/drivers/net/e1000e.ko", STAGE_DIR);
	if (!file_exists(path)) {
		fprintf(stderr, "FAIL: e1000e.ko not staged (recursively) to lib/modules/6.18.40/"
		                "kernel/drivers/net\n");
		ok = 0;
	}
	snprintf(path, sizeof(path), "%s/lib/modules/6.18.40/modules.dep", STAGE_DIR);
	if (!file_exists(path)) {
		fprintf(stderr, "FAIL: modules.dep not staged to lib/modules/6.18.40\n");
		ok = 0;
	}
	snprintf(path, sizeof(path), "%s/usr/bin/modprobe", STAGE_DIR);
	if (!file_exists(path)) {
		fprintf(stderr, "FAIL: modprobe not staged (dereferenced) to usr/bin\n");
		ok = 0;
	}

	/*
	 * Every binary the daemon has a compiled-in absolute path for must
	 * actually be in the control-plane image, or the feature that shells
	 * out to it cannot work on an installed host at all.
	 *
	 * sfdisk is why this check exists. It was missing from the moment
	 * partition management shipped (ADR-0158), so the very first real
	 * POST /disks/{name}/partition-table on 192.168.15.95 could only
	 * ever fail: the code was correct, the binary was not there. Nothing
	 * caught it because this build sandbox has a rich /usr where
	 * everything resolves, and thinc-install.c drives the same sfdisk
	 * from the ISO, where it is also present. Only the installed host is
	 * missing it, and only at runtime.
	 *
	 * Kept in sync by hand with the *_BIN defines across daemon/include
	 * and daemon/src (grep for '_BIN "'). modprobe/modinfo are
	 * deliberately absent: they come from the operator's own
	 * kmod_bin_dir argument rather than this fixed table, and are
	 * asserted directly above.
	 */
	{
		static const char *const shelled_bins[] = {
			"usr/bin/openssl",    /* PKI_OPENSSL_BIN, WS_OPENSSL_BIN */
			"usr/bin/curl",       /* PKG_CURL_BIN */
			"usr/bin/tar",        /* PKG_TAR_BIN */
			"usr/bin/unsquashfs", /* PKG_UNSQUASHFS_BIN */
			"bin/rm",             /* PKG_RM_BIN -- /bin, not /usr/bin */
			"usr/sbin/mkfs.ext4", /* DISKFORMAT_MKFS_EXT4_BIN */
			"usr/sbin/sfdisk",    /* DISKPART_SFDISK_BIN */
			"usr/sbin/resize2fs", /* DISKPART_RESIZE2FS_BIN */
			"usr/sbin/e2fsck",    /* DISKPART_E2FSCK_BIN */
		};
		size_t bi;

		for (bi = 0; bi < sizeof(shelled_bins) / sizeof(shelled_bins[0]); bi++) {
			snprintf(path, sizeof(path), "%s/%s", STAGE_DIR, shelled_bins[bi]);
			if (!file_exists(path)) {
				fprintf(stderr,
				        "FAIL: %s not staged -- the daemon shells out to it by absolute "
				        "path, so the feature using it cannot work on a real installed "
				        "host\n",
				        shelled_bins[bi]);
				ok = 0;
			}
		}
	}

	system("rm -rf " STAGE_DIR " " FW_SRC_DIR " " MODULES_SRC_DIR " " KMOD_BIN_SRC_DIR " "
	       OUT_SQUASHFS);

	printf(ok ? "MKBOOTROOT FIRMWARE RESULT: PASS\n" : "MKBOOTROOT FIRMWARE RESULT: FAIL\n");
	return ok ? 0 : 1;
}
