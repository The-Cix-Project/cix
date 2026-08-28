/*
 * Issue #164: creating a compressed archive must not need a shell.
 *
 * This test exists because the bug it guards was invisible everywhere
 * it was ever run. GNU tar spawns `--use-compress-program=` through
 * /bin/sh; every dev sandbox has a /bin/sh, so the old code passed
 * locally forever, while on a real Cix control-plane root -- which
 * deliberately has no shell at all -- every image export and every
 * package-cache save failed. And the failure named the wrong thing:
 * tar reports the failed exec against the compress program, so it read
 * as "/usr/bin/gzip: Cannot exec: No such file or directory" on a host
 * where gzip was present and worked.
 *
 * So a test that merely calls targz_create() here would prove nothing:
 * it would have passed against the broken code too. The only honest
 * check is to reproduce the environment that actually breaks -- a root
 * with tar, gzip and their runtime, and NO shell -- and archive from
 * inside it. That needs root (chroot), which this suite already runs
 * as.
 *
 * Two properties are asserted:
 *   1. It works with no shell present (the regression itself).
 *   2. Its bytes are identical to what tar's own --use-compress-program
 *      pipeline produced, because these archives are published to a
 *      content-addressed store where one name must mean one byte
 *      sequence forever (ADR-0201, issue #129). A fix that quietly
 *      changed the bytes would invalidate every artifact already
 *      published.
 */
#include "targz.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int failures;

#define CHECK(cond, ...)                                                                           \
	do {                                                                                       \
		if (!(cond)) {                                                                      \
			fprintf(stderr, "FAIL: ");                                                 \
			fprintf(stderr, __VA_ARGS__);                                              \
			fprintf(stderr, "\n");                                                     \
			failures++;                                                                \
		}                                                                                  \
	} while (0)

static int write_text(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");

	if (f == NULL)
		return -1;
	fputs(content, f);
	return fclose(f) == 0 ? 0 : -1;
}

static int sha256_of(const char *path, char *out, size_t out_size)
{
	char cmd_path[] = "/usr/bin/sha256sum";
	int pfd[2];
	pid_t pid;
	ssize_t n;
	int status;

	if (pipe(pfd) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		char *argv[] = { cmd_path, (char *)path, NULL };

		dup2(pfd[1], 1);
		close(pfd[0]);
		close(pfd[1]);
		execve(cmd_path, argv, environ);
		_exit(127);
	}
	close(pfd[1]);
	n = read(pfd[0], out, out_size - 1);
	close(pfd[0]);
	waitpid(pid, &status, 0);
	if (n <= 0)
		return -1;
	out[n] = '\0';
	if (n > 64)
		out[64] = '\0'; /* just the digest, not the filename */
	return 0;
}

/*
 * Stages a minimal root that has everything the pipeline needs and
 * nothing else -- crucially no /bin/sh. test_image_fixture_build()
 * supplies ld.so + libc.so.6 (the same two files mkbootroot stages
 * into a real control-plane root), and tar/gzip are copied from this
 * build host alongside their own runtime closure.
 */
static int stage_shell_less_root(const char *root, const char *probe_bin)
{
	char path[PATH_MAX];
	static const char *const libs[] = {
		"/lib/x86_64-linux-gnu/libacl.so.1",     "/lib/x86_64-linux-gnu/libselinux.so.1",
		"/lib/x86_64-linux-gnu/libpcre2-8.so.0", NULL,
	};
	int i;

	/* Places the probe binary plus ld.so/libc under root/bin. */
	if (test_image_fixture_build(root, probe_bin, "targz_probe") != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/usr", root);
	if (mkdir(path, 0755) != 0 && errno != EEXIST)
		return -1;
	snprintf(path, sizeof(path), "%s/usr/bin", root);
	if (mkdir(path, 0755) != 0 && errno != EEXIST)
		return -1;

	snprintf(path, sizeof(path), "%s%s", root, TARGZ_TAR_BIN);
	if (test_image_fixture_copy_file(TARGZ_TAR_BIN, path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s%s", root, TARGZ_GZIP_BIN);
	if (test_image_fixture_copy_file(TARGZ_GZIP_BIN, path) != 0)
		return -1;
	/* tar's own closure beyond libc (confirmed by ldd on this build
	 * host); gzip needs only libc, already staged above. */
	for (i = 0; libs[i] != NULL; i++) {
		if (test_image_fixture_add_lib(root, libs[i]) != 0)
			return -1;
	}
	return 0;
}

int main(void)
{
	char root[] = "/tmp/cix_test_targz_XXXXXX";
	char path[PATH_MAX];
	char probe_src[PATH_MAX];
	char sha_new[128], sha_old[128];
	struct stat st;
	pid_t pid;
	int status = 0;

	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	/* Content to archive: a couple of files, one nested, so the
	 * archive is not trivially empty. */
	snprintf(probe_src, sizeof(probe_src), "%s/src", root);
	if (mkdir(probe_src, 0755) != 0)
		return 1;
	snprintf(path, sizeof(path), "%s/a.txt", probe_src);
	if (write_text(path, "alpha\n") != 0)
		return 1;
	snprintf(path, sizeof(path), "%s/sub", probe_src);
	if (mkdir(path, 0755) != 0)
		return 1;
	snprintf(path, sizeof(path), "%s/sub/b.txt", probe_src);
	if (write_text(path, "beta\n") != 0)
		return 1;

	/* 1. Plain, same-namespace run: the pipeline works and produces a
	 * real archive. */
	snprintf(path, sizeof(path), "%s/new.tar.gz", root);
	{
		int code = -1;

		CHECK(targz_create(probe_src, path, &code) == 0, "targz_create failed (code %d)", code);
		CHECK(stat(path, &st) == 0 && st.st_size > 0, "no archive produced");
	}

	/* 2. Byte-for-byte identical to tar's own --use-compress-program
	 * output -- the property the artifact store depends on. */
	{
		char old_path[PATH_MAX];
		pid_t tp;
		int tstatus = 0;

		snprintf(old_path, sizeof(old_path), "%s/old.tar.gz", root);
		tp = fork();
		if (tp == 0) {
			char *argv[] = { (char *)TARGZ_TAR_BIN,
				         "--use-compress-program=" TARGZ_GZIP_BIN,
				         "--sort=name",
				         "--mtime=@0",
				         "--owner=0",
				         "--group=0",
				         "--numeric-owner",
				         "-C",
				         probe_src,
				         "-cf",
				         old_path,
				         ".",
				         NULL };

			execve(TARGZ_TAR_BIN, argv, environ);
			_exit(127);
		}
		waitpid(tp, &tstatus, 0);
		if (WIFEXITED(tstatus) && WEXITSTATUS(tstatus) == 0 &&
		    sha256_of(path, sha_new, sizeof(sha_new)) == 0 &&
		    sha256_of(old_path, sha_old, sizeof(sha_old)) == 0) {
			CHECK(strcmp(sha_new, sha_old) == 0,
			      "archive bytes changed -- every already-published artifact would be "
			      "invalidated (new %s, old %s)",
			      sha_new, sha_old);
		} else {
			fprintf(stderr, "FAIL: could not produce the comparison archive\n");
			failures++;
		}
	}

	/*
	 * 3. The regression itself: the same call, inside a root with NO
	 * /bin/sh. This is the only part of this test that would have
	 * failed against the old --use-compress-program implementation.
	 */
	{
		char chroot_root[PATH_MAX];

		snprintf(chroot_root, sizeof(chroot_root), "%s/root", root);
		if (mkdir(chroot_root, 0755) != 0) {
			perror("mkdir chroot root");
			return 1;
		}
		if (stage_shell_less_root(chroot_root, "build/targz_probe") != 0) {
			fprintf(stderr, "could not stage the shell-less root\n");
			return 1;
		}
		snprintf(path, sizeof(path), "%s/bin/sh", chroot_root);
		CHECK(stat(path, &st) != 0,
		      "this test is meaningless if the staged root has a shell -- it must not");
		snprintf(path, sizeof(path), "%s/probe_src", chroot_root);
		if (mkdir(path, 0755) != 0)
			return 1;
		snprintf(path, sizeof(path), "%s/probe_src/f.txt", chroot_root);
		if (write_text(path, "hello\n") != 0)
			return 1;

		pid = fork();
		if (pid < 0)
			return 1;
		if (pid == 0) {
			char *argv[] = { (char *)"/bin/targz_probe", (char *)"/probe_src",
				         (char *)"/out.tar.gz", NULL };

			if (chroot(chroot_root) != 0 || chdir("/") != 0)
				_exit(90);
			execve("/bin/targz_probe", argv, environ);
			_exit(91);
		}
		waitpid(pid, &status, 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		      "archiving inside a shell-less root failed (status 0x%x) -- this is exactly "
		      "the real-host failure issue #164 described",
		      (unsigned)status);
		snprintf(path, sizeof(path), "%s/out.tar.gz", chroot_root);
		CHECK(stat(path, &st) == 0 && st.st_size > 0,
		      "no archive was produced inside the shell-less root");
	}

	if (failures == 0) {
		char cmd[PATH_MAX + 32];

		snprintf(cmd, sizeof(cmd), "%s", root);
		/* best-effort cleanup; a failure's artifacts are worth keeping */
		if (fork() == 0) {
			char *argv[] = { (char *)"/bin/rm", (char *)"-rf", cmd, NULL };

			execve("/bin/rm", argv, environ);
			_exit(127);
		}
		wait(NULL);
	}

	printf("TARGZ RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
