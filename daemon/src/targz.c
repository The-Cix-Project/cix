#include "targz.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/*
 * Both halves of the pipeline are exec'd by absolute path, never
 * searched for on $PATH: this daemon runs as PID 1 with no PATH at
 * all, which is what issue #125 already learned the hard way when
 * tar's own `-z` tried to find a bare "gzip".
 */
int targz_run(const char *src_dir, const char *out_path)
{
	int pfd[2];
	int outfd;
	pid_t tar_pid, gz_pid;
	int tar_status = 0, gz_status = 0;
	int rc = TARGZ_EXIT_OK;

	outfd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (outfd < 0)
		return TARGZ_EXIT_SETUP;
	if (pipe(pfd) != 0) {
		close(outfd);
		return TARGZ_EXIT_SETUP;
	}

	tar_pid = fork();
	if (tar_pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		close(outfd);
		return TARGZ_EXIT_SETUP;
	}
	if (tar_pid == 0) {
		/*
		 * The normalizing flags (issue #129) live here, once, rather
		 * than at each call site: without them per-file mtimes ride in
		 * the tar headers and two builds of an identical tree produce
		 * different bytes, which breaks the artifact store's
		 * one-name-one-byte-sequence contract.
		 */
		char *argv[] = { (char *)TARGZ_TAR_BIN,
			         "--sort=name",
			         "--mtime=@0",
			         "--owner=0",
			         "--group=0",
			         "--numeric-owner",
			         "-C",
			         (char *)src_dir,
			         "-cf",
			         "-",
			         ".",
			         NULL };

		if (dup2(pfd[1], STDOUT_FILENO) < 0)
			_exit(TARGZ_EXIT_SETUP);
		close(pfd[0]);
		close(pfd[1]);
		close(outfd);
		execve(TARGZ_TAR_BIN, argv, environ);
		_exit(TARGZ_EXIT_NO_BINARY);
	}

	gz_pid = fork();
	if (gz_pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		close(outfd);
		waitpid(tar_pid, NULL, 0);
		return TARGZ_EXIT_SETUP;
	}
	if (gz_pid == 0) {
		/* "-c" writes to stdout; with no file operand gzip reads
		 * stdin, so it records neither an original filename nor an
		 * mtime -- the property that keeps this output byte-identical
		 * to tar's own --use-compress-program pipeline. */
		char *argv[] = { (char *)TARGZ_GZIP_BIN, "-c", NULL };

		if (dup2(pfd[0], STDIN_FILENO) < 0 || dup2(outfd, STDOUT_FILENO) < 0)
			_exit(TARGZ_EXIT_SETUP);
		close(pfd[0]);
		close(pfd[1]);
		close(outfd);
		execve(TARGZ_GZIP_BIN, argv, environ);
		_exit(TARGZ_EXIT_NO_BINARY);
	}

	/*
	 * Both ends must be closed here before waiting: while this process
	 * still holds the write end open, gzip never sees EOF on its stdin
	 * and neither child ever exits.
	 */
	close(pfd[0]);
	close(pfd[1]);
	close(outfd);

	if (waitpid(tar_pid, &tar_status, 0) != tar_pid)
		rc = TARGZ_EXIT_FAILED;
	if (waitpid(gz_pid, &gz_status, 0) != gz_pid)
		rc = TARGZ_EXIT_FAILED;

	/* A missing binary is reported ahead of a plain failure: it is a
	 * different problem with a different fix, and saying so is the
	 * whole reason these codes are distinct. */
	if (WIFEXITED(tar_status) && WEXITSTATUS(tar_status) == TARGZ_EXIT_NO_BINARY)
		return TARGZ_EXIT_NO_BINARY;
	if (WIFEXITED(gz_status) && WEXITSTATUS(gz_status) == TARGZ_EXIT_NO_BINARY)
		return TARGZ_EXIT_NO_BINARY;
	if (!WIFEXITED(tar_status) || WEXITSTATUS(tar_status) != 0)
		return TARGZ_EXIT_FAILED;
	if (!WIFEXITED(gz_status) || WEXITSTATUS(gz_status) != 0)
		return TARGZ_EXIT_FAILED;
	return rc;
}

int targz_create(const char *src_dir, const char *out_path, int *out_code)
{
	pid_t pid;
	int status = 0;

	if (out_code != NULL)
		*out_code = TARGZ_EXIT_SETUP;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0)
		_exit(targz_run(src_dir, out_path));

	if (waitpid(pid, &status, 0) != pid)
		return -1;
	if (!WIFEXITED(status))
		return -1;
	if (out_code != NULL)
		*out_code = WEXITSTATUS(status);
	return WEXITSTATUS(status) == TARGZ_EXIT_OK ? 0 : -1;
}
