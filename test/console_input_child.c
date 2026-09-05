/*
 * Exec target for test_console_exec.c's INPUT scenario (#296).
 *
 * Every other console fixture only ever SPEAKS: it reports what it saw
 * at startup and the test reads that back. Nothing in the suite ever
 * wrote a byte INTO a console, which is exactly how #294 shipped -- a
 * console that drew its prompt and then ignored every keystroke, while
 * the whole console test file passed. The daemon's session struct was
 * malloc()ed and never zeroed, so the new pty output buffer's length
 * came up as garbage, the direct write was skipped and terminal input
 * was copied to a junk offset. Nothing that only reads could have
 * caught it.
 *
 * So this one listens. It echoes each line it receives back with a
 * marker the test can match, which makes the assertion end to end: the
 * bytes left the test as a websocket frame, crossed the daemon's relay,
 * went through the pty master into the slave, were read by a real
 * exec'd process, and came back the other way. A single marker arriving
 * exercises the entire input direction.
 *
 * Reads with read() rather than fgets() deliberately: stdin here is a
 * pty slave, and a short read is ordinary rather than an error -- a
 * line can arrive in as many pieces as the writer chose to send it in,
 * so lines are assembled here instead of assumed.
 *
 * Never returns. The daemon SIGKILLs the exec'd process when the
 * console session ends, which is this program's only exit -- the same
 * contract console_term_child has, and for the same reason: returning
 * would let the session end for a reason the test could not tell apart
 * from a failure.
 */
#include "iohelpers.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LINE_MAX_LEN 512

static void report(const char *prefix, const char *line)
{
	char out[LINE_MAX_LEN + 64];
	int n = snprintf(out, sizeof(out), "%s%s\n", prefix, line);

	if (n > 0)
		cix_write_all(STDOUT_FILENO, out, (size_t)n);
}

int main(void)
{
	char line[LINE_MAX_LEN];
	size_t len = 0;

	/*
	 * Announced before anything is read, so the test can wait for a
	 * process that is genuinely at its read() before writing. Without
	 * it the test would race its own exec: bytes written into the pty
	 * before the child reaches read() are not lost (the tty buffers
	 * them), but a failure to receive them would be indistinguishable
	 * from the relay never delivering them, which is the bug this
	 * exists to detect.
	 */
	report("INPUT-READY", "");

	for (;;) {
		char buf[256];
		ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
		ssize_t i;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			/*
			 * Park rather than exit. A read error here would end
			 * the process, and the daemon would report that as the
			 * session ending -- which reads as "the console closed"
			 * rather than "input broke", hiding the very failure
			 * this fixture is for.
			 */
			for (;;)
				pause();
		}
		if (n == 0) {
			for (;;)
				pause();
		}

		for (i = 0; i < n; i++) {
			char c = buf[i];

			/*
			 * A terminal sends carriage return for Enter, not
			 * newline, and the pty's own line discipline may or may
			 * not have translated it by the time it arrives. Both
			 * are treated as end-of-line so the test does not have
			 * to depend on which.
			 */
			if (c == '\n' || c == '\r') {
				line[len] = '\0';
				if (len > 0)
					report("INPUT-ECHO:", line);
				len = 0;
				continue;
			}
			if (len < sizeof(line) - 1)
				line[len++] = c;
		}
	}
}
