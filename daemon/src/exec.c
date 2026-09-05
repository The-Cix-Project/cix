#include "exec.h"

#include "iohelpers.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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

/*
 * The container's user namespace, when it has one of its own (#293).
 *
 * A session that never enters it runs with HOST credentials inside the
 * container's mount namespace: it reads every on-disk id unmapped, so a
 * container uid 10000 whose files are host 4369840 shows up as 4369840
 * and belongs to nobody the session can be. That is what made a console
 * login report "change directory failed: Permission denied" on the
 * user's own home directory while the identical login over SSH -- which
 * happens inside the container -- worked. The more serious half is that
 * such a session holds real host root against the container's
 * filesystem, so the isolation userns is there to provide was simply
 * absent on this path.
 *
 * Returns 1 and sets *out_fd when the namespace differs and must be
 * joined, 0 with *out_fd == -1 when it is the one this process is
 * already in (setns() onto your own user namespace is EINVAL, which is
 * every non-userns container), and -1 on a real failure. The three
 * cases are kept distinct deliberately: treating an error as "nothing
 * to join" would restore the bug silently, which is how it survived.
 */
static int open_userns_fd(pid_t target_pid, int *out_fd)
{
	struct stat target_st, self_st;
	char path[64];
	int fd;

	*out_fd = -1;
	snprintf(path, sizeof(path), "/proc/%d/ns/user", (int)target_pid);
	if (stat(path, &target_st) != 0) {
		fprintf(stderr, "exec_into_container: stat %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (stat("/proc/self/ns/user", &self_st) != 0) {
		fprintf(stderr, "exec_into_container: stat /proc/self/ns/user: %s\n", strerror(errno));
		return -1;
	}
	if (target_st.st_dev == self_st.st_dev && target_st.st_ino == self_st.st_ino)
		return 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "exec_into_container: open %s: %s\n", path, strerror(errno));
		return -1;
	}
	*out_fd = fd;
	return 1;
}

/*
 * The host uid that the target's own uid 0 maps to, read from the
 * kernel's uid_map rather than plumbed through from the container spec
 * -- the map is what setns() will actually apply, and it is the one
 * source of truth for the answer. A container sharing this process's
 * user namespace maps 0 to 0, so callers get 0 and do nothing.
 */
static long long userns_base_of(pid_t target_pid)
{
	char path[64];
	FILE *f;
	long long inside, host, len;
	long long base = 0;

	snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)target_pid);
	f = fopen(path, "r");
	if (f == NULL)
		return 0;
	while (fscanf(f, "%lld %lld %lld", &inside, &host, &len) == 3) {
		if (inside == 0) {
			base = host;
			break;
		}
	}
	fclose(f);
	return base;
}

/* mnt/uts/net all take effect on the calling process immediately;
 * pid only affects children created *after* the call (setns(2)/
 * pid_namespaces(7)) -- so it's entered last here, right before the
 * fork() that actually needs it, even though its fd (like every
 * other one) was opened up front. No ipc namespace: containers never
 * isolate it in the first place (src/container.c), so there's none
 * to join. */
static int join_namespaces(int user_fd, int mnt_fd, int uts_fd, int net_fd, int pid_fd)
{
	static const struct { int flag; const char *name; } order[] = {
		{ CLONE_NEWUSER, "user" },
		{ CLONE_NEWNS, "mnt" },
		{ CLONE_NEWUTS, "uts" },
		{ CLONE_NEWNET, "net" },
		{ CLONE_NEWPID, "pid" },
	};
	int fds[5];
	size_t i;

	/* user FIRST (#293): the capabilities this process gains in the
	 * target's user namespace are what make the setns(mnt) below
	 * legitimate, which is the order nsenter -U -m uses. Joining mnt
	 * first and user afterwards fails, because a process that has
	 * already entered a foreign mount namespace no longer holds
	 * CAP_SYS_ADMIN over the user namespace owning it. */
	fds[0] = user_fd;
	fds[1] = mnt_fd;
	fds[2] = uts_fd;
	fds[3] = net_fd;
	fds[4] = pid_fd;

	for (i = 0; i < 5; i++) {
		/* Only user_fd is ever absent, and only for a container that
		 * shares this process's own user namespace -- there is nothing
		 * to join there and setns() would refuse it EINVAL. */
		if (fds[i] < 0)
			continue;
		if (setns(fds[i], order[i].flag) != 0) {
			fprintf(stderr, "exec_into_container: setns(%s): %s\n", order[i].name, strerror(errno));
			return -1;
		}
	}
	return 0;
}

int exec_resize_pty(int pty_master_fd, unsigned short cols, unsigned short rows)
{
	struct winsize wsz;

	if (cols == 0 || rows == 0) {
		errno = EINVAL;
		return -1;
	}
	memset(&wsz, 0, sizeof(wsz));
	wsz.ws_col = cols;
	wsz.ws_row = rows;
	/* ws_xpixel/ws_ypixel stay 0: they describe a pixel geometry only
	 * a real graphical terminal knows, nothing here has it, and no
	 * terminal-handling program requires it. */
	return ioctl(pty_master_fd, TIOCSWINSZ, &wsz) == 0 ? 0 : -1;
}

int exec_into_container(pid_t target_pid, char *const cmd_argv[],
                         const struct exec_term *term,
                         int *out_pty_master_fd, pid_t *out_child_pid)
{
	int master_fd, slave_fd;
	char slave_path[64];
	int pipefd[2];
	int mnt_fd, uts_fd, net_fd, pid_fd;
	int mnt_errno, uts_errno, net_errno, pid_errno;
	int user_fd = -1;
	int user_errno = 0;
	pid_t intermediate;
	unsigned short term_cols = EXEC_TERM_COLS_DEFAULT;
	unsigned short term_rows = EXEC_TERM_ROWS_DEFAULT;
	const char *term_name = EXEC_TERM_NAME_DEFAULT;

	if (term != NULL) {
		if (term->cols != 0)
			term_cols = term->cols;
		if (term->rows != 0)
			term_rows = term->rows;
		if (term->term != NULL && term->term[0] != '\0')
			term_name = term->term;
	}

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
	/* Up front with the rest, for the same reason (#293): once the
	 * mount namespace is joined, /proc no longer resolves to the
	 * host's procfs and this path cannot be opened at all. */
	if (open_userns_fd(target_pid, &user_fd) < 0)
		user_errno = errno;
	if (mnt_fd < 0 || uts_fd < 0 || net_fd < 0 || pid_fd < 0 || user_errno != 0) {
		int saved_errno = mnt_fd < 0   ? mnt_errno
		                   : uts_fd < 0 ? uts_errno
		                   : net_fd < 0 ? net_errno
		                   : pid_fd < 0 ? pid_errno
		                                : user_errno;

		if (mnt_fd >= 0) close(mnt_fd);
		if (uts_fd >= 0) close(uts_fd);
		if (net_fd >= 0) close(net_fd);
		if (pid_fd >= 0) close(pid_fd);
		if (user_fd >= 0) close(user_fd);
		errno = saved_errno;
		return -1;
	}

	/*
	 * The pty is allocated from the CONTAINER's devpts, not this
	 * daemon's (#290).
	 *
	 * posix_openpt() opens the host's /dev/ptmx, so the slave lands in
	 * the host's devpts instance. Handing that slave's fd to a process
	 * inside a container gives it a terminal that reads and writes
	 * perfectly and CANNOT BE NAMED: /proc/self/fd/0 says /dev/pts/0,
	 * the container's own /dev/pts holds no such entry, and ttyname()
	 * fails ENODEV. bash never notices. login(1) resolves its terminal
	 * name in init_tty(), reports the failure to syslog rather than to
	 * the terminal it is holding, and sleepexit()s -- measured on
	 * 192.168.15.95 as a console that upgrades cleanly, sends zero
	 * bytes, and closes after exactly 5.0 seconds. Every program that
	 * names its own tty is affected: agetty, who, w, wall, script, and
	 * anything writing utmp.
	 *
	 * Opening the container's own /dev/pts/ptmx fixes it at the
	 * source. The kernel resolves which devpts instance a ptmx open
	 * belongs to from the PATH used (path_pts(), the /dev/pts sibling
	 * of the ptmx being opened), so a slave allocated through the
	 * container's ptmx belongs to the container's instance and is
	 * named there. Measured on the jump box, which mounts its own:
	 * "devpts rw,mode=620,ptmxmode=666" -- ptmxmode is what makes this
	 * open permitted at all.
	 *
	 * Reached via /proc/<pid>/root rather than by entering the mount
	 * namespace first: the daemon must keep the master fd and must not
	 * setns() itself, and this is the same host-side route into a
	 * container's tree that container_file_host_path() (#269) already
	 * established for exactly that reason.
	 *
	 * grantpt() is deliberately NOT called. Its job is to fix up the
	 * slave's ownership and mode, which devpts already did at
	 * allocation from its own mount options -- and glibc's
	 * implementation reasons about /dev/pts in THIS process's mount
	 * namespace, which is not the instance the slave lives in. There
	 * is nothing correct for it to do here.
	 */
	{
		char ptmx_path[64];
		int ptn = -1;

		snprintf(ptmx_path, sizeof(ptmx_path), "/proc/%ld/root/dev/pts/ptmx", (long)target_pid);
		master_fd = open(ptmx_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
		if (master_fd < 0) {
			fprintf(stderr, "exec_into_container: open %s: %s\n", ptmx_path, strerror(errno));
			close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd); if (user_fd >= 0) close(user_fd);
			return -1;
		}
		if (unlockpt(master_fd) != 0 || ioctl(master_fd, TIOCGPTN, &ptn) != 0 || ptn < 0) {
			fprintf(stderr, "exec_into_container: unlockpt/TIOCGPTN: %s\n", strerror(errno));
			close(master_fd);
			close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd); if (user_fd >= 0) close(user_fd);
			return -1;
		}
		/* A path in the CONTAINER's mount namespace, which is the only
		 * namespace it is valid in -- opened by the intermediate below,
		 * after it has joined. */
		snprintf(slave_path, sizeof(slave_path), "/dev/pts/%d", ptn);

		/*
		 * Hand the slave to the container's own root before anything
		 * joins the user namespace (#293).
		 *
		 * devpts created it for whoever opened ptmx -- this daemon, so
		 * host uid 0 -- at the mode src/mountns.c mounts it with, 0620.
		 * The intermediate below now enters the container's user
		 * namespace, where host 0 is not mapped at all, and container
		 * root cannot DAC_OVERRIDE an inode whose owner is unmapped.
		 * Without this the open() a few lines down fails EACCES on
		 * every userns container, which is every new container under
		 * ADR-0207 -- the console would go from showing wrong ids to
		 * having no terminal at all.
		 *
		 * Done host-side, because this is the last moment a process
		 * with host credentials holds this path. login(1) re-chowns the
		 * tty to whoever logs in, from inside the namespace, so this
		 * ownership is a starting point rather than a final answer.
		 */
		{
			long long base = userns_base_of(target_pid);
			char slave_host_path[80];

			snprintf(slave_host_path, sizeof(slave_host_path), "/proc/%ld/root/dev/pts/%d",
			         (long)target_pid, ptn);
			if (base != 0 && chown(slave_host_path, (uid_t)base, (gid_t)base) != 0) {
				fprintf(stderr, "exec_into_container: chown %s to %lld: %s\n", slave_host_path,
				        base, strerror(errno));
				close(master_fd);
				close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd); if (user_fd >= 0) close(user_fd);
				return -1;
			}
		}
	}
	/*
	 * Size the pty BEFORE the fork, not after the 101 is sent: ncurses
	 * reads the window size once during setupterm() at program start,
	 * so a size arriving even slightly later is a size the program has
	 * already missed. There is no race to lose here if it is set now,
	 * because the child does not exist yet.
	 *
	 * A failure is reported and not fatal. It cannot realistically
	 * happen on a pty master this function created three lines ago,
	 * and if it somehow does, the result is the unsized terminal that
	 * was this endpoint's behaviour for its whole history -- a plain
	 * shell session still works perfectly well on one, so refusing the
	 * whole console over it would turn a cosmetic failure into a
	 * regression for every non-full-screen caller.
	 */
	if (exec_resize_pty(master_fd, term_cols, term_rows) != 0)
		fprintf(stderr, "exec_into_container: TIOCSWINSZ %ux%u: %s\n",
		        (unsigned)term_cols, (unsigned)term_rows, strerror(errno));

	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		close(master_fd);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd); if (user_fd >= 0) close(user_fd);
		return -1;
	}

	intermediate = fork();
	if (intermediate < 0) {
		close(master_fd);
		close(pipefd[0]);
		close(pipefd[1]);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd); if (user_fd >= 0) close(user_fd);
		return -1;
	}

	if (intermediate == 0) {
		pid_t grandchild;

		close(pipefd[0]);
		close(master_fd); /* only the daemon-side relay ever touches the master */

		if (join_namespaces(user_fd, mnt_fd, uts_fd, net_fd, pid_fd) != 0)
			_exit(127);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd); if (user_fd >= 0) close(user_fd);

		/*
		 * The slave is opened HERE, after the mount namespace has been
		 * joined, because /dev/pts/<n> exists only in the container's
		 * own devpts (#290). Deliberately not O_CLOEXEC -- the
		 * grandchild needs this fd to survive its own execve().
		 */
		slave_fd = open(slave_path, O_RDWR);
		if (slave_fd < 0) {
			fprintf(stderr, "exec_into_container: open %s in container: %s\n", slave_path,
			        strerror(errno));
			_exit(127);
		}

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
			/* #278: an exec session runs workload code and must be
			 * an ordinary OOM candidate, not inherit the control
			 * plane's exemption. */
			cix_oom_unprotect_self();
			setsid();
			ioctl(slave_fd, TIOCSCTTY, 0);
			dup2(slave_fd, STDIN_FILENO);
			dup2(slave_fd, STDOUT_FILENO);
			dup2(slave_fd, STDERR_FILENO);
			if (slave_fd > STDERR_FILENO)
				close(slave_fd);
			chdir("/");
			/*
			 * $TERM names the terminfo entry describing what this
			 * pty can do. Without this, the exec'd process inherited
			 * whatever cixd itself has -- measured on a real host as
			 * "linux", the kernel console's type, since cixd runs as
			 * pid 1 from the bootloader. That is a valid terminfo
			 * entry, so nothing failed; every session simply claimed
			 * to be a Linux virtual console no matter what was really
			 * attached, mis-describing the colour and key-sequence
			 * capabilities of anything that is not one.
			 *
			 * setenv() after fork() is safe here specifically
			 * because cixd is single-threaded (no pthread_create
			 * anywhere in the daemon), so no other thread can have
			 * held malloc's lock at the moment of the fork.
			 *
			 * Deliberately only on this pty path. The piped variant
			 * below gives its command no terminal at all, and
			 * claiming otherwise via $TERM would invite a program to
			 * emit cursor escapes into what is meant to be a clean
			 * byte stream -- the exact corruption that path exists
			 * to avoid.
			 */
			setenv("TERM", term_name, 1);
			execve(cmd_argv[0], cmd_argv, environ);
			/*
			 * Issue #108: report the failure back to the daemon, not
			 * just to the pty.
			 *
			 * This used to _exit(127) and nothing else, on the stated
			 * reasoning that stdio now points at the pty slave so the
			 * console's own client is the right audience. It is not
			 * reachable in practice: the daemon has not sent its 101
			 * yet, so there IS no client attached to read it, and
			 * anything written here lands in a pty nobody is holding.
			 * The observable result was a console that accepted the
			 * upgrade and then produced nothing at all, forever --
			 * indistinguishable from a working session with nothing
			 * to say.
			 *
			 * pipefd is O_CLOEXEC, so a SUCCESSFUL execve closes it
			 * and the daemon's read below sees a clean EOF. Only a
			 * failure ever puts bytes here, which is what makes the
			 * absence of bytes a trustworthy success signal.
			 */
			{
				int exec_errno = errno;

				(void)!write(pipefd[1], &exec_errno, sizeof(exec_errno));
			}
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
	close(mnt_fd);
	close(uts_fd);
	close(net_fd);
	close(pid_fd);
	if (user_fd >= 0) close(user_fd);

	{
		pid_t grandchild_pid = -1;
		ssize_t n;

		n = read(pipefd[0], &grandchild_pid, sizeof(grandchild_pid));
		waitpid(intermediate, NULL, 0);

		if (n != (ssize_t)sizeof(grandchild_pid) || grandchild_pid <= 0) {
			close(pipefd[0]);
			close(master_fd);
			errno = ECHILD;
			return -1;
		}

		/*
		 * Issue #108: a second read, and it is the whole point of this
		 * change. The pid above is written BEFORE the grandchild has
		 * tried to execve() anything, so a valid pid proved only that
		 * fork() worked -- this function returned success for a
		 * command that does not exist in the container, the caller
		 * sent its 101, and the client then waited forever on a pty
		 * whose process was already dead. Really hit on a container
		 * whose image has no shell at all.
		 *
		 * The write end is O_CLOEXEC in the grandchild, so a
		 * successful execve() closes it and this read sees EOF
		 * immediately once the intermediate has exited. It therefore
		 * cannot block waiting on a healthy long-lived session.
		 */
		{
			int exec_errno = 0;
			ssize_t en = read(pipefd[0], &exec_errno, sizeof(exec_errno));

			close(pipefd[0]);
			if (en == (ssize_t)sizeof(exec_errno) && exec_errno != 0) {
				waitpid(grandchild_pid, NULL, 0);
				close(master_fd);
				errno = exec_errno;
				return -1;
			}
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
	int user_fd = -1;
	int user_errno = 0;
	int saved_errno;
	pid_t intermediate, grandchild;

	mnt_fd = open_ns_fd(target_pid, "mnt");
	saved_errno = errno;
	uts_fd = open_ns_fd(target_pid, "uts");
	if (mnt_fd >= 0)
		saved_errno = errno;
	net_fd = open_ns_fd(target_pid, "net");
	pid_fd = open_ns_fd(target_pid, "pid");
	/* #293: same namespace, same reasons, same up-front open as above. */
	if (open_userns_fd(target_pid, &user_fd) < 0)
		user_errno = errno;
	if (mnt_fd < 0 || uts_fd < 0 || net_fd < 0 || pid_fd < 0 || user_errno != 0) {
		if (mnt_fd >= 0) close(mnt_fd);
		if (uts_fd >= 0) close(uts_fd);
		if (net_fd >= 0) close(net_fd);
		if (pid_fd >= 0) close(pid_fd);
		if (user_fd >= 0) close(user_fd);
		if (user_errno != 0)
			saved_errno = user_errno;
		errno = saved_errno;
		return -1;
	}

	/* The output pipe's write end must survive execve(), so no
	 * CLOEXEC on it -- the read end and the pid-handback pipe do get
	 * it, since nothing exec'd should inherit either. */
	if (pipe(outpipe) != 0 || pipe2(pidpipe, O_CLOEXEC) != 0) {
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd); if (user_fd >= 0) close(user_fd);
		return -1;
	}

	intermediate = fork();
	if (intermediate < 0) {
		close(outpipe[0]); close(outpipe[1]);
		close(pidpipe[0]); close(pidpipe[1]);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd); if (user_fd >= 0) close(user_fd);
		return -1;
	}
	if (intermediate == 0) {
		close(outpipe[0]);
		close(pidpipe[0]);
		if (join_namespaces(user_fd, mnt_fd, uts_fd, net_fd, pid_fd) != 0)
			_exit(127);
		close(mnt_fd); close(uts_fd); close(net_fd); close(pid_fd);
		if (user_fd >= 0) close(user_fd);

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
			int devnull;

			/* #278: workload code, so an ordinary OOM candidate --
			 * same reasoning as the pty path above. */
			cix_oom_unprotect_self();
			devnull = open("/dev/null", O_RDONLY);

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
	if (user_fd >= 0) close(user_fd);

	*out_read_fd = outpipe[0];
	/* The INTERMEDIATE's pid: a real child of this daemon, so its
	 * pidfd is both watchable and reapable. */
	*out_child_pid = intermediate;
	return 0;
}
