#include "exec.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

/*
 * Every /proc/<target_pid>/ns/* fd needed below MUST be opened before
 * any setns() call is made -- not interleaved one at a time. The first
 * setns() (mnt) moves the calling process into the container's own
 * mount namespace immediately, and from that point on "/proc" no
 * longer resolves to the host's procfs at all (it's whatever, if
 * anything, the container itself has mounted there -- often nothing).
 * Opening the *later* fds (uts/net/pid) after that point would then
 * fail outright or, worse, silently resolve a different pid. Confirmed
 * by hitting exactly this failure empirically against a real running
 * container before this fix (ENOENT opening ns/uts right after the
 * ns/mnt setns() had already succeeded).
 */
static int open_ns_fd(pid_t target_pid, const char *ns_name)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int)target_pid, ns_name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		fprintf(stderr, "exec_into_container: open %s: %s\n", path, strerror(errno));
	return fd;
}

/* mnt/uts/net all take effect on the calling process immediately;
 * pid only affects children created *after* the call (setns(2)/
 * pid_namespaces(7)) -- so it's entered last here, right before the
 * fork() that actually needs it, even though its fd (like every
 * other one) was opened up front. No ipc namespace: containers never
 * isolate it in the first place (src/container.c), so there's none
 * to join. */
static int join_namespaces(int mnt_fd, int uts_fd, int net_fd, int pid_fd)
{
	static const struct { int flag; const char *name; } order[] = {
		{ CLONE_NEWNS, "mnt" },
		{ CLONE_NEWUTS, "uts" },
		{ CLONE_NEWNET, "net" },
		{ CLONE_NEWPID, "pid" },
	};
	int fds[4];
	size_t i;

	fds[0] = mnt_fd;
	fds[1] = uts_fd;
	fds[2] = net_fd;
	fds[3] = pid_fd;

	for (i = 0; i < 4; i++) {
		if (setns(fds[i], order[i].flag) != 0) {
			fprintf(stderr, "exec_into_container: setns(%s): %s\n", order[i].name, strerror(errno));
			return -1;
		}
	}
	return 0;
}

int exec_into_container(pid_t target_pid, char *const cmd_argv[],
                         int *out_pty_master_fd, pid_t *out_child_pid)
{
	int master_fd, slave_fd;
	char slave_path[64];
	int pipefd[2];
	int mnt_fd, uts_fd, net_fd, pid_fd;
	int mnt_errno, uts_errno, net_errno, pid_errno;
	pid_t intermediate;

	/* Opened here, in the daemon's own (host) mount namespace, before
	 * anything below ever calls setns() -- see join_namespaces()'s own
	 * comment for why opening these any later would be wrong.
	 *
	 * task #764: each open_ns_fd() call's own errno is captured
	 * immediately, before the next open() or any close() below can
	 * clobber it -- this branch used to force a blanket errno=ESRCH
	 * regardless of which of the four actually failed or why.
	 * open_ns_fd() already fprintf(stderr,...)s the real reason, but
	 * that goes nowhere a REST client can ever see (cixd's own
	 * stderr on a real installed box, no host shell access).
	 * Preserving the real first failure's errno here lets the caller
	 * (main.c's try_console_upgrade()) put it in the HTTP response
	 * body instead of a generic "failed to start console session". */
	mnt_fd = open_ns_fd(target_pid, "mnt");
	mnt_errno = errno;
	uts_fd = open_ns_fd(target_pid, "uts");
	uts_errno = errno;
	net_fd = open_ns_fd(target_pid, "net");
	net_errno = errno;
	pid_fd = open_ns_fd(target_pid, "pid");
	pid_errno = errno;
	if (mnt_fd < 0 || uts_fd < 0 || net_fd < 0 || pid_fd < 0) {
		int saved_errno = mnt_fd < 0   ? mnt_errno
		                   : uts_fd < 0 ? uts_errno
		                   : net_fd < 0 ? net_errno
		                                : pid_errno;

		if (mnt_fd >= 0) close(mnt_fd);
		if (uts_fd >= 0) close(uts_fd);
		if (net_fd >= 0) close(net_fd);
		if (pid_fd >= 0) close(pid_fd);
		errno = saved_errno;
		return -1;
	}

	master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master_fd < 0) {
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);
		return -1;
	}
	if (grantpt(master_fd) != 0 || unlockpt(master_fd) != 0) {
		close(master_fd);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);
		return -1;
	}
	if (ptsname_r(master_fd, slave_path, sizeof(slave_path)) != 0) {
		close(master_fd);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);
		return -1;
	}
	/* Deliberately not O_CLOEXEC -- the grandchild needs this fd to
	 * survive across its own execve() below. */
	slave_fd = open(slave_path, O_RDWR);
	if (slave_fd < 0) {
		close(master_fd);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);
		return -1;
	}

	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		close(master_fd);
		close(slave_fd);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);
		return -1;
	}

	intermediate = fork();
	if (intermediate < 0) {
		close(master_fd);
		close(slave_fd);
		close(pipefd[0]);
		close(pipefd[1]);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);
		return -1;
	}

	if (intermediate == 0) {
		pid_t grandchild;

		close(pipefd[0]);
		close(master_fd); /* only the daemon-side relay ever touches the master */

		if (join_namespaces(mnt_fd, uts_fd, net_fd, pid_fd) != 0)
			_exit(127);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);

		grandchild = fork();
		if (grandchild < 0) {
			fprintf(stderr, "exec_into_container: fork (grandchild): %s\n", strerror(errno));
			_exit(127);
		}

		if (grandchild == 0) {
			/* Now a real member of the container's own pid namespace
			 * (pid namespaces are hierarchical -- this process's pid
			 * here is namespace-local, distinct from the outer pid the
			 * intermediate process just handed back to the daemon
			 * below). setsid() + TIOCSCTTY makes the pty slave this
			 * new session's controlling terminal regardless of whether
			 * it was already open before or after the session was
			 * formed -- more robust than relying on open-order alone. */
			setsid();
			ioctl(slave_fd, TIOCSCTTY, 0);
			dup2(slave_fd, STDIN_FILENO);
			dup2(slave_fd, STDOUT_FILENO);
			dup2(slave_fd, STDERR_FILENO);
			if (slave_fd > STDERR_FILENO)
				close(slave_fd);
			chdir("/");
			execve(cmd_argv[0], cmd_argv, environ);
			/* Diagnostic goes nowhere useful here -- stdio was just
			 * redirected onto the pty slave, so this reaches the
			 * console's own client, which is the right audience for
			 * "the command you asked for doesn't exist in here." */
			_exit(127);
		}

		/* Intermediate: hand the grandchild's outer-namespace-visible
		 * pid back to the daemon, then exit immediately -- the daemon
		 * reaps the grandchild itself via pidfd, the same convention
		 * every other child it tracks already uses; this intermediate
		 * process's own brief zombie window is reaped by the daemon's
		 * ordinary child-reaping path right below. */
		if (write(pipefd[1], &grandchild, sizeof(grandchild)) != (ssize_t)sizeof(grandchild))
			fprintf(stderr, "exec_into_container: pipe write: %s\n", strerror(errno));
		close(pipefd[1]);
		_exit(0);
	}

	/* Daemon (original process): never joined any namespace, stays
	 * exactly where it was the whole time -- only the intermediate
	 * child (already forked, its own copies of these fds still open)
	 * needed them. */
	close(pipefd[1]);
	close(slave_fd);
	close(mnt_fd);
	close(uts_fd);
	close(net_fd);
	close(pid_fd);

	{
		pid_t grandchild_pid = -1;
		ssize_t n;

		n = read(pipefd[0], &grandchild_pid, sizeof(grandchild_pid));
		close(pipefd[0]);
		waitpid(intermediate, NULL, 0);

		if (n != (ssize_t)sizeof(grandchild_pid) || grandchild_pid <= 0) {
			close(master_fd);
			errno = ECHILD;
			return -1;
		}

		*out_pty_master_fd = master_fd;
		*out_child_pid = grandchild_pid;
		return 0;
	}
}

/*
 * Issue #62: run argv inside a running container's namespaces with its
 * output on a PIPE rather than a pty, and no controlling terminal.
 *
 * Deliberately not a flag on exec_into_container() above: the two
 * differ in what a caller wants from them, not just in plumbing. A pty
 * exists to carry an interactive session, and it is precisely what
 * makes the console a poor diagnostic tool -- line discipline echoes
 * and edits what passes through it, which is how a piped one-liner
 * came back visibly corrupted during the Part 201 hang investigation
 * and fed a wrong diagnosis. A pipe carries exactly the bytes the
 * command wrote.
 *
 * The namespace entry, the intermediate/grandchild fork, and the
 * pid-handback are identical and shared -- only stdio differs.
 *
 * Returns 0 with *out_read_fd the readable end and *out_child_pid the
 * grandchild's outer-namespace pid, or -1 with errno set.
 */
int exec_into_container_piped(pid_t target_pid, char *const cmd_argv[], int *out_read_fd,
                              pid_t *out_child_pid)
{
	int outpipe[2];
	int pidpipe[2];
	int mnt_fd, uts_fd, net_fd, pid_fd;
	int saved_errno;
	pid_t intermediate, grandchild;

	mnt_fd = open_ns_fd(target_pid, "mnt");
	saved_errno = errno;
	uts_fd = open_ns_fd(target_pid, "uts");
	if (mnt_fd >= 0)
		saved_errno = errno;
	net_fd = open_ns_fd(target_pid, "net");
	pid_fd = open_ns_fd(target_pid, "pid");
	if (mnt_fd < 0 || uts_fd < 0 || net_fd < 0 || pid_fd < 0) {
		if (mnt_fd >= 0) close(mnt_fd);
		if (uts_fd >= 0) close(uts_fd);
		if (net_fd >= 0) close(net_fd);
		if (pid_fd >= 0) close(pid_fd);
		errno = saved_errno;
		return -1;
	}

	/* The output pipe's write end must survive execve(), so no
	 * CLOEXEC on it -- the read end and the pid-handback pipe do get
	 * it, since nothing exec'd should inherit either. */
	if (pipe(outpipe) != 0 || pipe2(pidpipe, O_CLOEXEC) != 0) {
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);
		return -1;
	}

	intermediate = fork();
	if (intermediate < 0) {
		close(outpipe[0]); close(outpipe[1]);
		close(pidpipe[0]); close(pidpipe[1]);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);
		return -1;
	}
	if (intermediate == 0) {
		close(outpipe[0]);
		close(pidpipe[0]);
		if (join_namespaces(mnt_fd, uts_fd, net_fd, pid_fd) != 0)
			_exit(127);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);

		grandchild = fork();
		if (grandchild < 0)
			_exit(127);
		if (grandchild == 0) {
			/*
			 * No setsid(), no TIOCSCTTY, no controlling terminal: this
			 * is a command, not a session. stdin is /dev/null so
			 * anything that tries to read gets EOF immediately rather
			 * than blocking forever on a terminal that does not exist.
			 */
			int devnull = open("/dev/null", O_RDONLY);

			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				if (devnull > STDERR_FILENO)
					close(devnull);
			}
			dup2(outpipe[1], STDOUT_FILENO);
			dup2(outpipe[1], STDERR_FILENO);
			if (outpipe[1] > STDERR_FILENO)
				close(outpipe[1]);
			chdir("/");
			execve(cmd_argv[0], cmd_argv, environ);
			/* Reaches the caller through the pipe, which is exactly
			 * the right audience for "that command is not in here". */
			fprintf(stderr, "exec: %s: %s\n", cmd_argv[0], strerror(errno));
			_exit(127);
		}
		/*
		 * Unlike the console path above, the intermediate does NOT
		 * exit immediately here -- it waits for the grandchild and
		 * exits with its status.
		 *
		 * That is not a style difference, it is the only way the
		 * daemon can learn the exit status at all. The grandchild is
		 * the intermediate's child, not the daemon's, so the daemon
		 * cannot waitid() on it: a pidfd would still report its exit,
		 * but reaping requires parentage, and a job waiting on a
		 * status it can never collect simply hangs (which is exactly
		 * what happened before this).
		 *
		 * Signals are folded into the 128+n shell convention, which is
		 * what the API reports anyway, so nothing is lost squeezing
		 * the result through an exit code.
		 */
		close(pidpipe[1]);
		close(outpipe[1]); /* the grandchild holds the only writer now */
		{
			int st = 0;

			if (waitpid(grandchild, &st, 0) != grandchild)
				_exit(127);
			if (WIFEXITED(st))
				_exit(WEXITSTATUS(st));
			if (WIFSIGNALED(st))
				_exit(128 + WTERMSIG(st));
			_exit(127);
		}
	}

	close(outpipe[1]);
	close(pidpipe[0]);
	close(pidpipe[1]);
	close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);

	*out_read_fd = outpipe[0];
	/* The INTERMEDIATE's pid: a real child of this daemon, so its
	 * pidfd is both watchable and reapable. */
	*out_child_pid = intermediate;
	return 0;
}
