/*
 * Exec target for test_console_exec.c's terminal-geometry scenarios
 * (ADR-0242). Reports the two things a full-screen program actually
 * asks a terminal before it draws anything -- what kind of terminal it
 * is ($TERM) and how big it is (TIOCGWINSZ) -- so the test can assert
 * on what the exec'd process really saw, rather than on what the
 * daemon believes it sent.
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
