/*
 * test_mkinstalleriso_args -- the argument count cixd passes to
 * mkinstalleriso is the one mkinstalleriso accepts (#480).
 *
 * That count went wrong twice. Both times POST /v1/system/iso answered
 * with a fragment of mkinstalleriso's usage text, and nothing caught it
 * because test_installer, the only test that ran mkinstalleriso, boots
 * QEMU and runs in no gate. Both ends now take the list from
 * include/mkinstalleriso_args.h, and this runs the real binary against
 * that list, with no VM:
 *
 *   - one argument short and one too many are refused with exit 2 and
 *     a usage line naming every argument in the list, in order;
 *   - exactly MKISO_ARGC is accepted past the count check. Every
 *     argument is a path inside a fresh temporary directory and none
 *     exists, so it then fails on the first one it validates -- since
 *     #430 that is gpt-disk-guid, which a path is not, and before it
 *     was the first missing input. Either way the assertion is the
 *     same and deliberately loose: anything but usage, and not
 *     success. What this test exists to catch is a count mismatch, so
 *     it must not also pin WHICH complaint comes first.
 *
 * Runs build/mkinstalleriso from the repository root, as test_installer
 * does; `make selftest` builds it as a SELFTEST_HELPER.
 */
#include "mkinstalleriso_args.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MKINSTALLERISO_BIN "build/mkinstalleriso"

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	return remove(path);
}

/*
 * Runs mkinstalleriso with `nargs` arguments after argv[0], each a path
 * under `dir` that does not exist. Captures stdout and stderr together.
 * Returns 0 with *status set, or -1 if it could not be run at all.
 */
static int run_mkinstalleriso(const char *dir, int nargs, char *out, size_t out_size,
                              int *status)
{
	char paths[MKISO_ARGC + 1][256];
	char *argv[MKISO_ARGC + 2];
	int fds[2];
	size_t used = 0;
	pid_t pid;
	int i;

	if (nargs > MKISO_ARGC)
		return -1;
	argv[0] = MKINSTALLERISO_BIN;
	for (i = 1; i <= nargs; i++) {
		snprintf(paths[i], sizeof(paths[i]), "%s/arg%d", dir, i);
		argv[i] = paths[i];
	}
	argv[nargs + 1] = NULL;

	if (pipe(fds) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[0]);
		close(fds[1]);
		execv(argv[0], argv);
		fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	close(fds[1]);
	for (;;) {
		ssize_t n;

		if (used + 1 >= out_size) {
			char sink[512];

			n = read(fds[0], sink, sizeof(sink));
		} else {
			n = read(fds[0], out + used, out_size - 1 - used);
			if (n > 0)
				used += (size_t)n;
		}
		if (n == 0 || (n < 0 && errno != EINTR))
			break;
	}
	out[used] = '\0';
	close(fds[0]);
	while (waitpid(pid, status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return 0;
}

static int refused_with_usage(const char *dir, int nargs, const char *what)
{
	char out[16384];
	int status = 0;

	if (run_mkinstalleriso(dir, nargs, out, sizeof(out), &status) != 0) {
		fprintf(stderr, "FAIL: could not run %s (%s)\n", MKINSTALLERISO_BIN, what);
		return 0;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 2) {
		fprintf(stderr, "FAIL: %s: expected exit 2, got status 0x%x\n%s\n", what, status, out);
		return 0;
	}
	if (strncmp(out, "usage: " MKINSTALLERISO_BIN MKISO_USAGE_ARGS "\n",
	            strlen("usage: " MKINSTALLERISO_BIN MKISO_USAGE_ARGS "\n")) != 0) {
		fprintf(stderr, "FAIL: %s: the usage line does not name the argument list:\n%.400s\n",
		        what, out);
		return 0;
	}
	return 1;
}

int main(void)
{
	char dir[] = "/tmp/test_mkinstalleriso_args_XXXXXX";
	char out[16384];
	int status = 0;
	int ok = 1;

	if (access(MKINSTALLERISO_BIN, X_OK) != 0) {
		fprintf(stderr, "FAIL: %s: %s (make build/mkinstalleriso)\n", MKINSTALLERISO_BIN,
		        strerror(errno));
		return 1;
	}
	if (mkdtemp(dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp: %s\n", strerror(errno));
		return 1;
	}

	if (!refused_with_usage(dir, MKISO_ARGC - 2, "one argument short"))
		ok = 0;
	if (!refused_with_usage(dir, MKISO_ARGC, "one argument too many"))
		ok = 0;

	if (run_mkinstalleriso(dir, MKISO_ARGC - 1, out, sizeof(out), &status) != 0) {
		fprintf(stderr, "FAIL: could not run %s with the daemon's count\n",
		        MKINSTALLERISO_BIN);
		ok = 0;
	} else if (strstr(out, "usage:") != NULL ||
	           (WIFEXITED(status) && WEXITSTATUS(status) == 2)) {
		fprintf(stderr, "FAIL: the daemon's argument count (%d) was refused as usage:\n%.400s\n",
		        MKISO_ARGC, out);
		ok = 0;
	} else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		fprintf(stderr, "FAIL: mkinstalleriso reported success with every input missing\n");
		ok = 0;
	}

	nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);

	if (!ok)
		return 1;
	printf("test_mkinstalleriso_args: PASS (%d arguments, both wrong counts refused)\n",
	       MKISO_ARGC - 1);
	return 0;
}
