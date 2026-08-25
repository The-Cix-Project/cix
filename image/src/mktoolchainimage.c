/*
 * Phase 20: builds one real, portable, durable build-toolchain artifact
 * -- a single squashfs file containing everything a real package build
 * needs (gcc/make/autoconf/bison/perl/..., and this build host's own
 * Go toolchain if present), meant to be scp'd onto an installed Cix
 * host and imported via `pkg bootstrap --toolchain=PATH`
 * (pkg_bootstrap_from_toolchain(), daemon/src/pkg.c).
 *
 * Replaces relying on cixd's own live host having a real toolchain
 * at `pkg bootstrap` time -- true on a rich dev sandbox, guaranteed
 * false on a real minimal install (confirmed live: a fresh install's
 * own /usr has nothing under it beyond cixd/cixctl and their bare
 * runtime libs). Run manually, occasionally, on a real toolchain-having
 * machine -- the same "explicit one-time action, not part of the fast
 * default `make` loop" posture build/bzImage itself already has.
 *
 * All the actual staging logic (what a toolchain needs, tolerant of
 * what a given build host doesn't have) lives in
 * test_image_fixture_stage_toolchain() (test/test_image_fixture.c) --
 * shared with daemon/src/pkg.c's own pkg_bootstrap_build_image() dev/
 * test fallback, one source of truth for "what a toolchain needs," not
 * two copies drifting apart.
 */
#include "test_image_fixture.h"

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MKSQUASHFS_BIN "/usr/bin/mksquashfs"

static int run_mksquashfs(const char *image_root, const char *out_path)
{
	pid_t pid;
	int status;
	char *argv[] = { (char *)MKSQUASHFS_BIN, (char *)image_root, (char *)out_path,
		          "-noappend", "-comp", "xz", NULL };

	unlink(out_path);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve(MKSQUASHFS_BIN, argv, environ);
		perror("execve mksquashfs");
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "mksquashfs failed (status %d)\n", status);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *staging_dir;
	const char *out_path;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <staging-dir> <out.squashfs>\n", argv[0]);
		return 2;
	}
	staging_dir = argv[1];
	out_path = argv[2];

	if (test_image_fixture_stage_toolchain(staging_dir) != 0)
		return 1;
	if (run_mksquashfs(staging_dir, out_path) != 0)
		return 1;

	printf("wrote %s\n", out_path);
	return 0;
}
