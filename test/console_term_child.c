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
static void report_oom_adj(void)
{
	char buf[32];
	int fd = open("/proc/self/oom_score_adj", O_RDONLY);
	ssize_t n;

	if (fd < 0) {
		printf("OOMADJ=unreadable\n");
		return;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		printf("OOMADJ=unreadable\n");
		return;
	}
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	printf("OOMADJ=%s\n", buf);
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
static void report_tty(void)
{
	const char *name = ttyname(STDIN_FILENO);

	printf("TTY=%s\n", name != NULL ? name : "(unnamed)");
}

static void report(void)
{
	struct winsize wsz;
	const char *term = getenv("TERM");

	memset(&wsz, 0, sizeof(wsz));
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsz) != 0) {
		wsz.ws_col = 0;
		wsz.ws_row = 0;
	}
	/* One line, fixed shape, so the test can strstr() for an exact
	 * expected string rather than parsing. */
	printf("TERMINFO TERM=%s COLS=%u ROWS=%u\n", term != NULL ? term : "(unset)",
	       (unsigned)wsz.ws_col, (unsigned)wsz.ws_row);
	report_oom_adj();
	report_tty();
	fflush(stdout);
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
