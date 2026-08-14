#include "dual_console.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

/*
 * Tolerant per-device (a real machine/VM, or -- for tests -- a
 * standalone harness, may only have one of the two available) -- -1
 * means "not available," every function below skips it.
 */
static int g_fd0 = -1;
static int g_fd1 = -1;

void dual_console_open(const char *path0, const char *path1)
{
	/* O_NOCTTY: the caller has no use for a controlling terminal of its
	 * own here (it never reads a raw keystroke directly itself) --
	 * these are just raw fds to write status to and, for
	 * run_subprocess_dual_console() below, to relay bytes through. */
	g_fd0 = open(path0, O_RDWR | O_NOCTTY);
	g_fd1 = open(path1, O_RDWR | O_NOCTTY);
	if (g_fd0 < 0 && g_fd1 < 0) {
		/* Neither available at all -- there is nowhere left to report
		 * this to (that's the whole problem), so this is the one
		 * diagnostic in this module that can only ever reach the
		 * kernel's own dmesg/panic log, not either console. */
		perror("dual_console_open: neither console available");
	}
}

/* Writes to whichever of the two consoles are actually open -- a write
 * failure on one (e.g. an unplugged serial cable mid-install) drops
 * just that one for the rest of the program, not the whole install. */
static void dual_write(const char *buf, size_t len)
{
	if (g_fd0 >= 0 && write(g_fd0, buf, len) < 0)
		g_fd0 = -1;
	if (g_fd1 >= 0 && write(g_fd1, buf, len) < 0)
		g_fd1 = -1;
}

void dual_printf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	dual_write(buf, (size_t)((size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1));
}

void dual_perror(const char *s)
{
	dual_printf("%s: %s\n", s, strerror(errno));
}

int dual_console_wait_for_key(void)
{
	for (;;) {
		struct pollfd pfds[2];
		nfds_t nfds = 0;
		int fd0_idx = -1, fd1_idx = -1;
		int prc;
		char c;
		ssize_t n;

		if (g_fd0 < 0 && g_fd1 < 0)
			return -1;

		if (g_fd0 >= 0) {
			pfds[nfds].fd = g_fd0;
			pfds[nfds].events = POLLIN;
			fd0_idx = (int)nfds;
			nfds++;
		}
		if (g_fd1 >= 0) {
			pfds[nfds].fd = g_fd1;
			pfds[nfds].events = POLLIN;
			fd1_idx = (int)nfds;
			nfds++;
		}

		prc = poll(pfds, nfds, -1);
		if (prc < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (fd0_idx >= 0 && (pfds[fd0_idx].revents & POLLIN)) {
			n = read(g_fd0, &c, 1);
			if (n > 0)
				return 0;
			if (!(n < 0 && (errno == EINTR || errno == EAGAIN)))
				g_fd0 = -1;
		}
		if (fd1_idx >= 0 && (pfds[fd1_idx].revents & POLLIN)) {
			n = read(g_fd1, &c, 1);
			if (n > 0)
				return 0;
			if (!(n < 0 && (errno == EINTR || errno == EAGAIN)))
				g_fd1 = -1;
		}
		if (fd0_idx >= 0 && (pfds[fd0_idx].revents & (POLLHUP | POLLERR)))
			g_fd0 = -1;
		if (fd1_idx >= 0 && (pfds[fd1_idx].revents & (POLLHUP | POLLERR)))
			g_fd1 = -1;
	}
}

int run_subprocess_dual_console(const char *bin, char *const argv[])
{
	int master;
	char slave_path[64];
	pid_t pid;
	int status;
	struct termios fd0_saved, fd1_saved;
	int fd0_raw = 0, fd1_raw = 0;

	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0) {
		dual_perror("posix_openpt");
		return -1;
	}
	if (grantpt(master) != 0 || unlockpt(master) != 0) {
		dual_perror("grantpt/unlockpt");
		close(master);
		return -1;
	}
	if (ptsname_r(master, slave_path, sizeof(slave_path)) != 0) {
		dual_perror("ptsname_r");
		close(master);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		dual_perror("fork");
		close(master);
		return -1;
	}
	if (pid == 0) {
		int slave;

		/* The parent must never open the slave itself -- doing so
		 * here too would hold a reference that delays this PTY ever
		 * reporting POLLHUP on the master after this child exits,
		 * turning the parent's drain-then-exit loop into a hang. */
		close(master);
		setsid();
		slave = open(slave_path, O_RDWR);
		if (slave < 0) {
			dual_perror(slave_path);
			_exit(127);
		}
		dup2(slave, STDIN_FILENO);
		dup2(slave, STDOUT_FILENO);
		dup2(slave, STDERR_FILENO);
		if (slave > STDERR_FILENO)
			close(slave);
		execve(bin, argv, environ);
		dual_perror(bin);
		_exit(127);
	}

	/* Raw/cbreak on the real consoles only, only for the duration of
	 * this one call -- bin's own controlling terminal is the PTY slave,
	 * whose line discipline (echo, canonical mode, a password prompt's
	 * own tcsetattr()) is entirely bin's own business and untouched
	 * here. Without this, a real console's own local echo would double
	 * up with the slave's echo of the same keystrokes relayed back out. */
	if (g_fd0 >= 0 && tcgetattr(g_fd0, &fd0_saved) == 0) {
		struct termios raw = fd0_saved;

		cfmakeraw(&raw);
		if (tcsetattr(g_fd0, TCSANOW, &raw) == 0)
			fd0_raw = 1;
	}
	if (g_fd1 >= 0 && tcgetattr(g_fd1, &fd1_saved) == 0) {
		struct termios raw = fd1_saved;

		cfmakeraw(&raw);
		if (tcsetattr(g_fd1, TCSANOW, &raw) == 0)
			fd1_raw = 1;
	}

	for (;;) {
		struct pollfd pfds[3];
		nfds_t nfds = 0;
		int master_idx = -1, fd0_idx = -1, fd1_idx = -1;
		char buf[4096];
		ssize_t n;
		int prc;

		pfds[nfds].fd = master;
		pfds[nfds].events = POLLIN;
		master_idx = (int)nfds;
		nfds++;
		if (g_fd0 >= 0) {
			pfds[nfds].fd = g_fd0;
			pfds[nfds].events = POLLIN;
			fd0_idx = (int)nfds;
			nfds++;
		}
		if (g_fd1 >= 0) {
			pfds[nfds].fd = g_fd1;
			pfds[nfds].events = POLLIN;
			fd1_idx = (int)nfds;
			nfds++;
		}

		prc = poll(pfds, nfds, -1);
		if (prc < 0) {
			if (errno == EINTR)
				continue;
			dual_perror("poll");
			break;
		}

		/* PTY master output -> both consoles. A 0/error read here (not
		 * EINTR/EAGAIN) is the one authoritative "child is done" signal
		 * -- checked and drained before ever looking at POLLHUP/
		 * POLLERR, so a child's final output right before it exits is
		 * never lost to a race against reaping it. */
		if (pfds[master_idx].revents & POLLIN) {
			n = read(master, buf, sizeof(buf));
			if (n > 0) {
				dual_write(buf, (size_t)n);
				continue;
			}
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			break;
		}
		if (pfds[master_idx].revents & (POLLHUP | POLLERR))
			break;

		/* Either console -> PTY master, relaying operator input from
		 * whichever one actually has data. No arbitration needed
		 * beyond that: a real install has one operator at a time. */
		if (fd0_idx >= 0 && (pfds[fd0_idx].revents & POLLIN)) {
			n = read(g_fd0, buf, sizeof(buf));
			if (n > 0) {
				if (write(master, buf, (size_t)n) < 0 && errno != EINTR) {
					dual_perror("write to pty master");
					break;
				}
			} else if (n < 0 && errno != EINTR && errno != EAGAIN) {
				g_fd0 = -1;
			}
		}
		if (fd1_idx >= 0 && (pfds[fd1_idx].revents & POLLIN)) {
			n = read(g_fd1, buf, sizeof(buf));
			if (n > 0) {
				if (write(master, buf, (size_t)n) < 0 && errno != EINTR) {
					dual_perror("write to pty master");
					break;
				}
			} else if (n < 0 && errno != EINTR && errno != EAGAIN) {
				g_fd1 = -1;
			}
		}
	}

	if (fd0_raw)
		tcsetattr(g_fd0, TCSANOW, &fd0_saved);
	if (fd1_raw)
		tcsetattr(g_fd1, TCSANOW, &fd1_saved);
	close(master);

	if (waitpid(pid, &status, 0) != pid) {
		dual_perror("waitpid");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		dual_printf("%s failed (status 0x%x)\n", bin, (unsigned)status);
		return -1;
	}
	return 0;
}
