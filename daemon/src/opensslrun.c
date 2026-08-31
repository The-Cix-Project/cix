#include "opensslrun.h"

/*
 * The one path openssl is run from. It lives here rather than in pki.h
 * because this is now the only translation unit that execve()s it --
 * every caller goes through pki_run_openssl().
 */
#define PKI_OPENSSL_BIN "/usr/bin/openssl"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Forks and execve()s openssl with argv (argv[0] is conventionally
 * PKI_OPENSSL_BIN; the array must be NULL-terminated), redirecting
 * the child's stdout AND stderr into a pipe read back into out (if
 * non-NULL) -- used both for error diagnostics on failure (logged
 * server-side, never echoed raw into an HTTP response) and, for
 * `-noout` query invocations, as the actual result to parse on
 * success. All daemon-owned fds are already CLOEXEC from creation
 * (ADR-0009), so the child only ever inherits the one pipe fd it's
 * meant to.
 */
int pki_run_openssl(char *const argv[], char *out, size_t out_size)
{
	int pipefd[2];
	pid_t pid;
	int status;
	size_t total = 0;
	ssize_t n;

	if (out != NULL && out_size > 0)
		out[0] = '\0';

	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(PKI_OPENSSL_BIN, argv, environ);
		_exit(127);
	}

	close(pipefd[1]);
	if (out != NULL && out_size > 0) {
		while (total + 1 < out_size) {
			n = read(pipefd[0], out + total, out_size - total - 1);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (n == 0)
				break;
			total += (size_t)n;
		}
		out[total] = '\0';
	} else {
		char discard[256];

		while (read(pipefd[0], discard, sizeof(discard)) > 0)
			;
	}
	close(pipefd[0]);

	if (waitpid(pid, &status, 0) != pid)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}
