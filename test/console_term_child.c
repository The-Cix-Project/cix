/*
 * Exec target for test_console_exec.c's terminal-geometry scenarios
 * (ADR-0242). Reports the three things a program actually asks a
 * terminal before it does anything -- what kind of terminal it is
 * ($TERM), how big it is (TIOCGWINSZ), and what it is CALLED
 * (ttyname) -- so the test can assert on what the exec'd process
 * really saw, rather than on what the daemon believes it sent.
 *
 * Reports once at startup, then again on every SIGWINCH: the kernel
 * raises that on this pty's foreground process group whenever the
 * daemon applies a resize, which is precisely the mechanism a live
 * resize depends on, so a second line arriving is the proof that it
 * worked end to end.
 *
 * Never returns. The daemon SIGKILLs the exec'd process when the
 * console session ends, which is this program's only exit.
 */
#include <fcntl.h>
#include <signal.h>
#include "iohelpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t g_winch;

static void on_winch(int sig)
{
	(void)sig;
	g_winch = 1;
}

/* #278: what the kernel's OOM killer thinks of this process. Reported
 * from in here rather than asserted from outside because inheritance is
 * the whole point -- oom_score_adj survives fork() AND execve(), so the
 * only trustworthy reading is the one taken by a process that actually
 * went through both. */
static const char *report_oom_adj(void)
{
	static char buf[32];
	int fd = open("/proc/self/oom_score_adj", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return "unreadable";
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return "unreadable";
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return buf;
}

/*
 * #290: what this terminal is CALLED, which is a different question
 * from whether it works.
 *
 * The console's pty used to be allocated from the daemon's own devpts
 * and the slave fd handed to a process inside the container, where
 * /dev/pts holds no such entry -- so the fd read and wrote perfectly
 * and ttyname() failed ENODEV. A shell never notices. login(1) does:
 * it resolves its terminal name, and on failure reports to syslog
 * rather than to the terminal it is holding and then sleepexit()s,
 * which presents as a console that connects cleanly, says nothing,
 * and closes five seconds later.
 *
 * Asked from in here for the same reason OOMADJ is: only the process
 * actually sitting on the pty can answer it.
 */
static const char *report_tty(void)
{
	const char *name = ttyname(STDIN_FILENO);

	return name != NULL ? name : "(unnamed)";
}

/*
 * #293: is this process inside the container's own user namespace, or
 * the host's?
 *
 * Read from its own uid_map, which is the kernel's answer rather than
 * an inference: a process in the host's user namespace has the
 * identity map "0 0 4294967295", while one in a container's has that
 * container's subordinate base as the second field. So a non-zero base
 * means the session is genuinely subject to a mapping.
 *
 * Asked from in here because it is a property of the session, not of
 * the container -- the container was always in its namespace; what was
 * wrong was that the console never joined it.
 */
static const char *report_userns(void)
{
	FILE *f = fopen("/proc/self/uid_map", "r");
	long long inside = 0, host = 0, len = 0;
	int mapped = 0;

	if (f != NULL) {
		while (fscanf(f, "%lld %lld %lld", &inside, &host, &len) == 3) {
			if (inside == 0) {
				mapped = (host != 0);
				break;
			}
		}
		fclose(f);
	}
	return mapped ? "mapped" : "host";
}

/*
 * Everything this child has to say, in ONE write.
 *
 * It used to be four printf()s, and each one is a separate write to the
 * pty, a separate relay read, and a separate websocket frame -- four
 * chances for the reader to see a partial answer and give up, which is
 * exactly the intermittent shape of #291. The test matches expected
 * substrings, so one line satisfies every scenario while removing the
 * race rather than retiming it: there is one frame or there is none,
 * and none is unambiguously a failure.
 *
 * write() rather than printf(): stdio on a tty is line-buffered, so a
 * single printf of a single line is already one write, but saying so
 * with write() means a future field carrying an embedded newline
 * cannot quietly reintroduce the split.
 */
static void report(void)
{
	struct winsize wsz;
	const char *term = getenv("TERM");
	char line[512];
	int len;

	memset(&wsz, 0, sizeof(wsz));
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsz) != 0) {
		wsz.ws_col = 0;
		wsz.ws_row = 0;
	}
	len = snprintf(line, sizeof(line),
	               "TERMINFO TERM=%s COLS=%u ROWS=%u OOMADJ=%s TTY=%s USERNS=%s EUID=%d\n",
	               term != NULL ? term : "(unset)", (unsigned)wsz.ws_col, (unsigned)wsz.ws_row,
	               report_oom_adj(), report_tty(), report_userns(), (int)geteuid());
	if (len > 0)
		cix_write_all(STDOUT_FILENO, line, (size_t)len > sizeof(line) - 1 ? sizeof(line) - 1
		                                                                   : (size_t)len);
}

int main(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_winch;
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART, so the pause() below is interrupted rather than
	 * resumed -- being woken IS the event this program exists to
	 * observe. */
	sa.sa_flags = 0;
	sigaction(SIGWINCH, &sa, NULL);

	report();

	for (;;) {
		pause();
		if (g_winch) {
			g_winch = 0;
			report();
		}
	}
}
