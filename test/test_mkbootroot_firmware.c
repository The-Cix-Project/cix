/*
 * Phase 14 part 2 (ADR-0029) test: proves mkbootroot.c's new
 * firmware_dir argv/staging path directly against a synthetic scratch
 * directory -- a real, hundreds-of-MB linux-firmware fetch isn't
 * practical inside an automated test, mirroring test_devices.c's own
 * precedent of a structurally-correct stand-in where the real thing
 * isn't. No QEMU needed -- inspects mkbootroot's own staging directory
 * directly, before it's squashed, rather than the final squashfs
 * image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define STAGE_DIR "/tmp/kanxeo_test_mkbootroot_stage"
#define FW_SRC_DIR "/tmp/kanxeo_test_mkbootroot_fwsrc"
#define OUT_SQUASHFS "/tmp/kanxeo_test_mkbootroot_out.squashfs"
#define MKBOOTROOT_BIN "build/mkbootroot"

static int run_mkbootroot(const char *firmware_dir)
{
	pid_t pid;
	int status;
	char *mkbootroot_argv[] = { (char *)MKBOOTROOT_BIN,   (char *)STAGE_DIR,
		                     (char *)"build/kanxeod",   (char *)"build/kanxeoctl",
		                     (char *)"web",             (char *)OUT_SQUASHFS,
		                     (char *)firmware_dir,      NULL };

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

	system("rm -rf " STAGE_DIR " " FW_SRC_DIR " " OUT_SQUASHFS);

	/* 1. firmware_dir="" (every real call site's own value) is a
	 * complete no-op for firmware specifically -- lib/ itself already
	 * exists regardless (test_image_fixture_build()/_add_lib() already
	 * stage ld.so/libc.so.6/libtinfo.so.6 there for kanxeod's own
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

	printf(ok ? "MKBOOTROOT FIRMWARE RESULT: PASS\n" : "MKBOOTROOT FIRMWARE RESULT: FAIL\n");
	return ok ? 0 : 1;
}
